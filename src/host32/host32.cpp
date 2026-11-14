// 32-bit helper process. Loads the original 32-bit VST2 plug-in and runs it on behalf of the
// 64-bit bridge DLL. Started by the bridge; not meant to be run by hand.
//
// Threads:
//   main    - owns the plug-in instance and its windows; runs the message loop; serves main
//             channel calls (delivered as a posted window message, so that they are also served
//             while the plug-in runs a modal dialog or menu, as they would be in a native host).
//   reader  - reads the pipe and hands messages to whoever waits for them.
//   audio   - serves the audio channel.
#include "../common/ipc.h"
#include <ole2.h>
#include <atomic>
#include <deque>
#include <map>
#include <set>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")

using namespace ipc;

static const UINT WM_RPC = WM_APP + 1;

static Shared* g_shm = nullptr;
static HANDLE g_pipeIn = INVALID_HANDLE_VALUE, g_pipeOut = INVALID_HANDLE_VALUE;
static HANDLE g_evReq = nullptr, g_evRep = nullptr, g_evIn = nullptr;
static CRITICAL_SECTION g_writeLock, g_qLock;
static std::deque<Message> g_inQueue;

struct Pending {
    HANDLE ev;
    Message reply;
};
static std::map<uint32_t, Pending*> g_pending;
static std::atomic<uint32_t> g_nextId{0};

static DWORD g_mainTid = 0, g_audioTid = 0;
static HWND g_msgWnd = nullptr;
static AEffect* g_fx = nullptr;
static HMODULE g_module = nullptr;
static uint32_t g_curCall = 0;              // main thread: id of the bridge call being served
static volatile LONG g_mainBusySince = 0;   // tick at which the main thread started serving, or 0
static char g_pluginDir[MAX_PATH * 2] = "";
static Log g_log;

static void die(const char* why) {
    g_log.trace("exiting: %s", why);
    TerminateProcess(GetCurrentProcess(), 0);
}

static void send(Message& m) {
    EnterCriticalSection(&g_writeLock);
    bool ok = writeMessage(g_pipeOut, m);
    LeaveCriticalSection(&g_writeLock);
    if (!ok) die("pipe write failed");
}

static void handleCall(Message& m);

static bool takeCall(uint32_t parent, bool any, Message& out) {
    bool found = false;
    EnterCriticalSection(&g_qLock);
    for (auto it = g_inQueue.begin(); it != g_inQueue.end(); ++it) {
        if (any || it->h.parent == parent) {
            out = std::move(*it);
            g_inQueue.erase(it);
            found = true;
            break;
        }
    }
    LeaveCriticalSection(&g_qLock);
    return found;
}

static bool queueEmpty() {
    EnterCriticalSection(&g_qLock);
    bool e = g_inQueue.empty();
    LeaveCriticalSection(&g_qLock);
    return e;
}

// ---- calls to the host (through the bridge) ------------------------------------------------

// Sends a call and waits for its reply. On the main thread, calls the bridge makes in order to
// answer this one are served meanwhile; unrelated calls stay queued until the plug-in is back in
// a message loop, so the plug-in is never re-entered with something it did not ask for.
static bool roundTrip(Message& call, Message& reply, bool onMain) {
    static thread_local HANDLE t_ev = nullptr;
    if (!t_ev) t_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Pending p;
    p.ev = t_ev;
    ResetEvent(p.ev);
    EnterCriticalSection(&g_qLock);
    g_pending[call.h.id] = &p;
    LeaveCriticalSection(&g_qLock);
    send(call);
    if (!onMain) {
        WaitForSingleObject(p.ev, INFINITE);
    } else {
        for (;;) {
            Message m;
            while (takeCall(call.h.id, false, m)) handleCall(m);
            HANDLE hs[2] = {p.ev, g_evIn};
            // Delivers sent window messages and incoming drag-and-drop calls while it waits.
            if (waitPumping(2, hs, 50) == WAIT_OBJECT_0) break;
        }
    }
    EnterCriticalSection(&g_qLock);
    g_pending.erase(call.h.id);
    reply = std::move(p.reply);
    LeaveCriticalSection(&g_qLock);
    if (onMain && !queueEmpty()) {
        SetEvent(g_evIn);
        PostMessageW(g_msgWnd, WM_RPC, 0, 0);
    }
    return true;
}

static intptr_t mainCallback(AEffect* e, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt, OpDesc d, bool onMain) {
    static thread_local VstTimeInfo t_time;
    Message c, r;
    c.h.kind = K_CALL;
    c.h.id = ++g_nextId;
    c.h.parent = onMain ? g_curCall : 0;
    c.h.op = M_AUDIOMASTER;
    c.h.opcode = opcode;
    c.h.index = index;
    c.h.value = (int64_t)value;
    c.h.opt = opt;
    if (!e) c.h.flags |= F_EFFECT_NULL;
    switch (d.kind) {
    case PK_STR_IN:
        if (!ptr) c.h.flags |= F_PTR_NULL;
        else {
            const char* s = (const char*)ptr;
            c.d1.assign(s, s + strnlen(s, 4095));
        }
        break;
    case PK_STR_OUT:
        if (!ptr) c.h.flags |= F_PTR_NULL;
        break;
    case PK_SPECIAL:
        if (opcode == audioMasterProcessEvents) {
            const VstEvents* ev = (const VstEvents*)ptr;
            c.d1.resize(flatEventsSize(ev));
            if (!c.d1.empty()) flattenEvents(ev, c.d1.data(), c.d1.size());
        } else if (opcode == audioMasterIOChanged && g_fx) {
            EffectInfo info;
            fillInfo(info, g_fx);
            c.d1.assign((uint8_t*)&info, (uint8_t*)&info + sizeof(info));
        }
        break;
    default: break;
    }
    roundTrip(c, r, onMain);
    if (d.kind == PK_STR_OUT && ptr && (r.h.flags & F_HAS_D1) && !r.d1.empty()) {
        // Host strings are at most 64 characters in VST 2.4.
        size_t n = r.d1.size() > 64 ? 64 : r.d1.size();
        memcpy(ptr, r.d1.data(), n);
        ((char*)ptr)[n - 1] = 0;
    }
    if (opcode == audioMasterGetTime) {
        if ((r.h.flags & F_HAS_D1) && r.d1.size() == kSizeVstTimeInfo) {
            memcpy(&t_time, r.d1.data(), kSizeVstTimeInfo);
            return (intptr_t)&t_time;
        }
        return 0;
    }
    return (intptr_t)r.h.result;
}

// Callbacks the plug-in makes from inside an audio request.
static intptr_t audioCallback(int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt, OpDesc d) {
    static VstTimeInfo s_time; // audio thread only
    Shared* s = g_shm;
    switch (opcode) {
    case audioMasterGetTime:
        if (s->haveTime && ((int32_t)value & ~s->timeFilterUsed) == 0) {
            if (s->haveTime != 1) return 0;
            memcpy(&s_time, s->time, kSizeVstTimeInfo);
            return (intptr_t)&s_time;
        }
        s->timeFilter |= (int32_t)value;
        s->wantsTime = 1;
        break; // ask the host this once
    case audioMasterPinConnected_deprecated:
        if ((uint32_t)index < 32) {
            uint32_t bit = 1u << index;
            if (value) {
                if (s->pinValidOut & bit) return (s->pinAnswerOut & bit) ? 1 : 0;
                s->pinAskOut |= bit;
            } else {
                if (s->pinValidIn & bit) return (s->pinAnswerIn & bit) ? 1 : 0;
                s->pinAskIn |= bit;
            }
        }
        break;
    case audioMasterProcessEvents: {
        // Collected here; the bridge hands them to the host before its process call returns.
        size_t used = flattenEvents((const VstEvents*)ptr, s->evOut + s->evOutBytes, kEventBytes - s->evOutBytes);
        s->evOutBytes += (uint32_t)used;
        return 1;
    }
    default: break;
    }

    s->cbOpcode = opcode;
    s->cbIndex = index;
    s->cbValue = (int64_t)value;
    s->cbOpt = opt;
    s->cbFlags = 0;
    s->cbLen = 0;
    s->cbResult = 0;
    if (d.kind == PK_STR_IN) {
        if (!ptr) s->cbFlags |= F_PTR_NULL;
        else {
            size_t n = strnlen((const char*)ptr, kCbDataBytes - 1);
            memcpy(s->cbData, ptr, n);
            s->cbData[n] = 0;
            s->cbLen = (uint32_t)n + 1;
        }
    } else if (d.kind == PK_STR_OUT) {
        if (!ptr) s->cbFlags |= F_PTR_NULL;
    } else if (opcode == audioMasterIOChanged && g_fx) {
        EffectInfo info;
        fillInfo(info, g_fx);
        memcpy(s->cbData, &info, sizeof(info));
        s->cbLen = sizeof(info);
    }
    s->state = S_CALLBACK;
    SetEvent(g_evRep);
    WaitForSingleObject(g_evReq, INFINITE);
    if (d.kind == PK_STR_OUT && ptr && s->cbLen) {
        size_t n = s->cbLen > 64 ? 64 : s->cbLen;
        memcpy(ptr, s->cbData, n);
        ((char*)ptr)[n - 1] = 0;
    }
    if (opcode == audioMasterGetTime) {
        if (s->cbLen != kSizeVstTimeInfo) return 0;
        memcpy(&s_time, s->cbData, kSizeVstTimeInfo);
        return (intptr_t)&s_time;
    }
    return (intptr_t)s->cbResult;
}

static intptr_t __cdecl hostCallback(AEffect* e, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt) {
    switch (opcode) {
    case audioMasterGetDirectory:
        // The plug-in's own folder, not the folder of the 64-bit bridge DLL.
        return (intptr_t)g_pluginDir;
    case audioMasterCanDo:
        // File selector structures are not marshalled; the plug-in falls back to its own dialogs.
        if (ptr && (!strcmp((const char*)ptr, "openFileSelector") || !strcmp((const char*)ptr, "closeFileSelector"))) return 0;
        break;
    default: break;
    }
    OpDesc d = masterDesc(opcode);
    if (d.kind == PK_SKIP) return 0;
    DWORD tid = GetCurrentThreadId();
    if (tid == g_audioTid) return audioCallback(opcode, index, value, ptr, opt, d);
    return mainCallback(e, opcode, index, value, ptr, opt, d, tid == g_mainTid);
}

// ---- calls from the host -------------------------------------------------------------------

static void doLoad(Message& r, bool& quit) {
    auto fail = [&](const char* fmt, DWORD code) {
        char buf[300];
        _snprintf(buf, sizeof(buf), fmt, code);
        buf[sizeof(buf) - 1] = 0;
        g_log.error("load failed: %s", buf);
        r.h.result = 0;
        r.d1.assign(buf, buf + strlen(buf));
        quit = true;
    };
    std::wstring path = g_shm->pluginPath;
    std::wstring dir = path.substr(0, path.find_last_of(L"\\/"));
    WideCharToMultiByte(CP_ACP, 0, dir.c_str(), -1, g_pluginDir, sizeof(g_pluginDir) - 1, nullptr, nullptr);
    SetDllDirectoryW(dir.c_str());
    g_module = LoadLibraryW(path.c_str());
    if (!g_module) return fail("LoadLibrary failed with error %lu", GetLastError());
    VstPluginMainProc entry = (VstPluginMainProc)GetProcAddress(g_module, "VSTPluginMain");
    if (!entry) entry = (VstPluginMainProc)GetProcAddress(g_module, "main");
    if (!entry) return fail("the DLL has no VST entry point (error %lu)", GetLastError());
    AEffect* e = entry(hostCallback);
    if (!e || e->magic != kEffectMagic) return fail("the plug-in's entry point did not return an effect (%lu)", 0);
    g_fx = e;
    EffectInfo info;
    fillInfo(info, e);
    r.d1.assign((uint8_t*)&info, (uint8_t*)&info + sizeof(info));
    r.h.result = 1;
    g_log.trace("plug-in loaded: uniqueID %08X params %d programs %d in %d out %d flags 0x%X", e->uniqueID, e->numParams, e->numPrograms,
                e->numInputs, e->numOutputs, e->flags);
}

static const uint8_t kSentinel[4] = {0x01, 0x7F, 0x02, 0x7E};

static size_t speakerArrangementSize(const void* p) {
    int32_t channels;
    memcpy(&channels, (const uint8_t*)p + 4, 4);
    if (channels < 8) channels = 8;
    if (channels > 64) channels = 64;
    return kSizeVstSpeakerArrangementHeader + (size_t)channels * kSizeVstSpeakerProperties;
}

static void doDispatch(Message& m, Message& r, bool& quit) {
    if (!g_fx) return;
    AEffect* fx = g_fx;
    int32_t op = m.h.opcode;
    OpDesc d = dispatchDesc(op);
    auto disp = [&](intptr_t value, void* ptr) -> int64_t {
        return (int64_t)fx->dispatcher(fx, op, m.h.index, value, ptr, m.h.opt);
    };
    bool nullPtr = (m.h.flags & F_PTR_NULL) != 0;
    switch (d.kind) {
    case PK_NONE:
        r.h.result = disp((intptr_t)m.h.value, nullptr);
        break;
    case PK_STR_IN:
        if (nullPtr) r.h.result = disp((intptr_t)m.h.value, nullptr);
        else {
            m.d1.push_back(0);
            r.h.result = disp((intptr_t)m.h.value, m.d1.data());
        }
        break;
    case PK_STR_OUT:
        if (nullPtr) r.h.result = disp((intptr_t)m.h.value, nullptr);
        else {
            char buf[kStringMax];
            memset(buf, 0, sizeof(buf));
            memcpy(buf, kSentinel, sizeof(kSentinel));
            r.h.result = disp((intptr_t)m.h.value, buf);
            if (!memcmp(buf, kSentinel, sizeof(kSentinel))) r.h.flags |= F_UNTOUCHED;
            else {
                size_t n = strnlen(buf, kStringMax - 1);
                r.d1.assign(buf, buf + n);
                r.d1.push_back(0);
                r.h.flags |= F_HAS_D1;
            }
        }
        break;
    case PK_BLOB_IN:
    case PK_BLOB_OUT:
    case PK_BLOB_INOUT:
        if (nullPtr) r.h.result = disp((intptr_t)m.h.value, nullptr);
        else {
            std::vector<uint8_t> buf(d.size, 0);
            if (d.kind != PK_BLOB_OUT) memcpy(buf.data(), m.d1.data(), m.d1.size() < d.size ? m.d1.size() : d.size);
            r.h.result = disp((intptr_t)m.h.value, buf.data());
            if (d.kind != PK_BLOB_IN) {
                r.d1 = std::move(buf);
                r.h.flags |= F_HAS_D1;
            }
        }
        break;
    case PK_SPECIAL:
        switch (op) {
        case effClose:
            g_fx = nullptr; // the audio thread stops calling into the plug-in
            r.h.result = disp(0, nullptr);
            quit = true;
            break;
        case effEditGetRect: {
            ERect* rc = nullptr;
            r.h.result = disp(0, &rc);
            if (rc) {
                r.d1.assign((uint8_t*)rc, (uint8_t*)rc + sizeof(ERect));
                r.h.flags |= F_HAS_D1;
            }
            break;
        }
        case effEditOpen:
            // Window handles are 32-bit values valid in every process of the session.
            r.h.result = disp(0, (void*)(uintptr_t)(uint32_t)m.h.value);
            break;
        case effEditClose:
            r.h.result = disp(0, nullptr);
            break;
        case effGetChunk: {
            void* p = nullptr;
            int64_t n = disp(0, &p);
            r.h.result = n;
            if (n > 0 && p) {
                r.d1.assign((uint8_t*)p, (uint8_t*)p + n);
                r.h.flags |= F_HAS_D1;
            }
            break;
        }
        case effSetChunk:
            r.h.result = disp((intptr_t)m.d1.size(), m.d1.empty() ? nullptr : m.d1.data());
            break;
        case effProcessEvents: {
            static std::vector<uint8_t> store[4];
            static unsigned next = 0;
            VstEvents* ev = unflattenEvents(m.d1.data(), m.d1.size(), store[next++ & 3]);
            r.h.result = disp(0, ev);
            break;
        }
        case effSetSpeakerArrangement:
            r.h.result = disp((m.h.flags & F_VALUE_NULL) || m.d2.empty() ? 0 : (intptr_t)m.d2.data(), nullPtr || m.d1.empty() ? nullptr : m.d1.data());
            break;
        case effGetSpeakerArrangement: {
            void* in = nullptr;
            void* out = nullptr;
            r.h.result = disp((intptr_t)&in, &out);
            if (in) {
                r.d2.assign((uint8_t*)in, (uint8_t*)in + speakerArrangementSize(in));
                r.h.flags |= F_HAS_D2;
            }
            if (out) {
                r.d1.assign((uint8_t*)out, (uint8_t*)out + speakerArrangementSize(out));
                r.h.flags |= F_HAS_D1;
            }
            break;
        }
        default: break;
        }
        break;
    default: break;
    }
}

static void handleCall(Message& m) {
    uint32_t savedCall = g_curCall;
    LONG savedBusy = g_mainBusySince;
    g_curCall = m.h.id;
    if (!savedBusy) InterlockedExchange(&g_mainBusySince, (LONG)(GetTickCount() | 1));
    Message r;
    r.h.kind = K_REPLY;
    r.h.id = m.h.id;
    r.h.op = m.h.op;
    bool quit = false;
    switch (m.h.op) {
    case M_LOAD: doLoad(r, quit); break;
    case M_DISPATCH: doDispatch(m, r, quit); break;
    case M_SETPARAM:
        if (g_fx && g_fx->setParameter) g_fx->setParameter(g_fx, m.h.index, m.h.opt);
        break;
    case M_GETPARAM:
        r.h.opt = (g_fx && g_fx->getParameter) ? g_fx->getParameter(g_fx, m.h.index) : 0.0f;
        break;
    default: break;
    }
    g_curCall = savedCall;
    InterlockedExchange(&g_mainBusySince, savedBusy);
    send(r);
    if (quit) PostQuitMessage(0);
}

static LRESULT CALLBACK msgWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_RPC) {
        Message m;
        while (takeCall(0, true, m)) handleCall(m);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

// ---- threads -------------------------------------------------------------------------------

static DWORD WINAPI readerThread(LPVOID) {
    for (;;) {
        Message m;
        if (!readMessage(g_pipeIn, m)) die("the bridge closed the pipe");
        if (m.h.kind == K_REPLY) {
            EnterCriticalSection(&g_qLock);
            auto it = g_pending.find(m.h.id);
            if (it != g_pending.end()) {
                it->second->reply = std::move(m);
                SetEvent(it->second->ev);
            }
            LeaveCriticalSection(&g_qLock);
        } else {
            EnterCriticalSection(&g_qLock);
            g_inQueue.push_back(std::move(m));
            LeaveCriticalSection(&g_qLock);
            SetEvent(g_evIn);
            PostMessageW(g_msgWnd, WM_RPC, 0, 0);
        }
    }
}

static DWORD WINAPI audioThread(LPVOID) {
    Shared* s = g_shm;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    if (HMODULE avrt = LoadLibraryW(L"avrt.dll")) {
        typedef HANDLE(WINAPI * AvSet)(LPCWSTR, LPDWORD);
        if (AvSet f = (AvSet)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW")) {
            DWORD task = 0;
            f(L"Pro Audio", &task);
        }
    }
    static float* in[kMaxChannels + 1];
    static float* out[kMaxChannels + 1];
    for (int c = 0; c < kMaxChannels; c++) {
        in[c] = s->audioIn[c];
        out[c] = s->audioOut[c];
    }
    static std::vector<uint8_t> evStore[4];
    unsigned evNext = 0;
    for (;;) {
        WaitForSingleObject(g_evReq, INFINITE);
        if (s->state != S_REQUEST) continue;
        AEffect* fx = g_fx;
        switch (s->op) {
        case A_PROCESS: {
            int n = s->frames;
            if (n < 0) n = 0;
            if (n > kMaxFrames) n = kMaxFrames;
            if (!fx) {
                for (int c = 0; c < kMaxChannels; c++) memset(out[c], 0, n * sizeof(float));
            } else if (s->index && fx->process) {
                fx->process(fx, in, out, n); // accumulating
            } else if ((fx->flags & effFlagsCanReplacing) && fx->processReplacing) {
                fx->processReplacing(fx, in, out, n);
            } else if (fx->process) {
                for (int c = 0; c < kMaxChannels; c++) memset(out[c], 0, n * sizeof(float));
                fx->process(fx, in, out, n);
            }
            break;
        }
        case A_EVENTS: {
            uint32_t bytes = s->evInBytes > (uint32_t)kEventBytes ? (uint32_t)kEventBytes : s->evInBytes;
            VstEvents* ev = unflattenEvents(s->evIn, bytes, evStore[evNext++ & 3]);
            s->result = fx ? (int64_t)fx->dispatcher(fx, effProcessEvents, 0, 0, ev, 0) : 0;
            break;
        }
        case A_SETPARAM:
            if (fx && fx->setParameter) fx->setParameter(fx, s->index, s->fvalue);
            break;
        case A_GETPARAM:
            s->fvalue = (fx && fx->getParameter) ? fx->getParameter(fx, s->index) : 0.0f;
            break;
        default: break;
        }
        s->state = S_DONE;
        SetEvent(g_evRep);
    }
}

static DWORD WINAPI parentWatch(LPVOID p) {
    WaitForSingleObject((HANDLE)p, INFINITE);
    die("the host process ended");
    return 0;
}

// A dialog the plug-in opens while the host is waiting on it (its authorization wizard, for
// example) can come up behind the host's window. Raise such dialogs once.
static std::set<HWND> g_raised;
static BOOL CALLBACK raiseProc(HWND h, LPARAM) {
    if (!IsWindowVisible(h) || g_raised.count(h)) return TRUE;
    wchar_t cls[32] = L"";
    GetClassNameW(h, cls, 32);
    if (wcscmp(cls, L"#32770")) return TRUE;
    g_raised.insert(h);
    SetForegroundWindow(h);
    BringWindowToTop(h);
    return TRUE;
}
static DWORD WINAPI raiseThread(LPVOID) {
    for (;;) {
        Sleep(200);
        LONG since = g_mainBusySince;
        if (since && GetTickCount() - (DWORD)since > 300) EnumThreadWindows(g_mainTid, raiseProc, 0);
    }
}

static void applyDpiAwareness(uint32_t a) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    typedef BOOL(WINAPI * SetCtx)(HANDLE);
    if (SetCtx f = u ? (SetCtx)GetProcAddress(u, "SetProcessDpiAwarenessContext") : nullptr) {
        // DPI_AWARENESS_CONTEXT values: -1 unaware, -2 system, -3 per monitor, -4 per monitor v2.
        static const intptr_t ctx[4] = {-1, -2, -3, -4};
        if (f((HANDLE)ctx[a > 3 ? 3 : a])) return;
        if (a >= 3 && f((HANDLE)(intptr_t)-3)) return;
    }
    if (a >= 1) SetProcessDPIAware();
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmd, int) {
    std::wstring session = cmd ? cmd : L"";
    while (!session.empty() && session.back() == L' ') session.pop_back();
    while (!session.empty() && session.front() == L' ') session.erase(session.begin());
    if (session.empty() || session.find(L"PhatBridge-") != 0) {
        MessageBoxW(nullptr,
                    L"This program is part of the pHATmatik PRO 64-bit bridge.\n\n"
                    L"It is started automatically when a 64-bit host loads the plug-in and does nothing on its own.",
                    L"pHATmatik PRO bridge", MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS);
    InitializeCriticalSection(&g_writeLock);
    InitializeCriticalSection(&g_qLock);

    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, (L"Local\\" + session + L"-shm").c_str());
    if (!map) return 2;
    g_shm = (Shared*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
    if (!g_shm || g_shm->magic != kMagic || g_shm->version != kProtocolVersion) return 3;
    g_log.who = "host32";
    g_log.verbose = g_shm->debug != 0;
    wcsncpy(g_log.path, g_shm->logPath, 1023);
    g_log.trace("started for session %ls", session.c_str());

    applyDpiAwareness(g_shm->dpiAwareness);

    g_pipeIn = CreateFileW((L"\\\\.\\pipe\\" + session + L"-c2h").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    g_pipeOut = CreateFileW((L"\\\\.\\pipe\\" + session + L"-h2c").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    g_evReq = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, (L"Local\\" + session + L"-req").c_str());
    g_evRep = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, (L"Local\\" + session + L"-rep").c_str());
    HANDLE evReady = OpenEventW(EVENT_MODIFY_STATE, FALSE, (L"Local\\" + session + L"-ready").c_str());
    if (g_pipeIn == INVALID_HANDLE_VALUE || g_pipeOut == INVALID_HANDLE_VALUE || !g_evReq || !g_evRep || !evReady) {
        g_log.error("could not open the bridge's pipes or events (error %lu)", GetLastError());
        return 4;
    }
    g_evIn = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, g_shm->bridgePid)) CreateThread(nullptr, 0, parentWatch, parent, 0, nullptr);

    g_mainTid = GetCurrentThreadId();
    OleInitialize(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = msgWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"PhatBridgeHost32Msg";
    RegisterClassW(&wc);
    g_msgWnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    if (!g_msgWnd) return 5;

    CreateThread(nullptr, 0, audioThread, nullptr, 0, &g_audioTid);
    CreateThread(nullptr, 0, readerThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, raiseThread, nullptr, 0, nullptr);
    SetEvent(evReady);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_log.trace("message loop ended");
    // Unload the plug-in as a host would, then leave without running anything else.
    if (g_module) FreeLibrary(g_module);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
