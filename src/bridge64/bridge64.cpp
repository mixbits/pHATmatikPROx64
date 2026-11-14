// 64-bit VST2 plug-in that forwards everything to the original 32-bit plug-in, which runs
// unmodified in a 32-bit helper process (one helper per plug-in instance).
#define _CRT_RAND_S
#include <stdlib.h>
#include "../common/ipc.h"
#include <atomic>
#include <deque>
#include <map>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")

using namespace ipc;

static HINSTANCE g_inst = nullptr;
static Log g_log;
// The installer looks for this text to tell the bridge from the original plug-in, which has the
// same file name.
extern "C" __declspec(dllexport) const char pHATmatikBridgeMarker[] = "pHATmatikBridge64-marker-v1";
static const wchar_t* kMsgClass = L"PhatBridge64Msg";
static const UINT WM_BRIDGE_CALLBACKS = WM_APP + 1;
static const UINT WM_BRIDGE_DESTROY = WM_APP + 2;
static const wchar_t* kRegKey = L"SOFTWARE\\pHATmatik PRO Bridge";
static const wchar_t* kPluginFile = L"iZotope pHATmatik PRO.dll";
static const wchar_t* kHostFile = L"pHATmatikBridgeHost32.exe";

struct Instance;

// The helper callback this thread is currently executing, so that calls the host makes from
// inside its callback handler can be marked as belonging to it.
struct ActiveCallback {
    Instance* inst;
    uint32_t id;
};
static thread_local ActiveCallback t_cb{nullptr, 0};

struct Instance {
    AEffect fx{};
    AudioMasterCallback master = nullptr;
    std::atomic<bool> dead{false};
    std::atomic<bool> closing{false};

    HANDLE hMap = nullptr, pipeOut = INVALID_HANDLE_VALUE, pipeIn = INVALID_HANDLE_VALUE;
    HANDLE evReq = nullptr, evRep = nullptr, evReady = nullptr, hProcess = nullptr, hJob = nullptr;
    Shared* shm = nullptr;
    DWORD helperPid = 0;

    CRITICAL_SECTION mainLock, writeLock, qLock, audioLock;
    std::deque<Message> cbQueue;         // calls from the helper waiting to be executed
    std::map<uint32_t, Message> replies; // replies to our calls
    int waitDepth = 0;                   // a host thread is inside mainCall
    uint32_t nextId = 0;
    HANDLE evWaiter = nullptr, evWorker = nullptr, evQuit = nullptr;
    HANDLE readerThread = nullptr, workerThread = nullptr;
    HWND msgWnd = nullptr;

    std::atomic<DWORD> audioTids[8];
    std::atomic<DWORD> lastAudioTid{0};
    std::atomic<DWORD> audioBusyTid{0};
    std::atomic<DWORD> mainOwnerTid{0}; // thread currently inside mainCall
    unsigned audioTidNext = 0;

    // Storage for data the host receives by pointer.
    std::vector<uint8_t> chunk, spkIn, spkOut, evStoreAudio;
    ERect rect{};

    Instance() {
        InitializeCriticalSection(&mainLock);
        InitializeCriticalSection(&writeLock);
        InitializeCriticalSection(&qLock);
        InitializeCriticalSection(&audioLock);
        for (auto& t : audioTids) t = 0;
    }
    ~Instance() {
        DeleteCriticalSection(&mainLock);
        DeleteCriticalSection(&writeLock);
        DeleteCriticalSection(&qLock);
        DeleteCriticalSection(&audioLock);
    }

    bool isAudioThread(DWORD tid) const {
        if (lastAudioTid.load(std::memory_order_relaxed) == tid) return true;
        for (auto& t : audioTids)
            if (t.load(std::memory_order_relaxed) == tid) return true;
        return false;
    }
    void noteAudioThread(DWORD tid) {
        if (lastAudioTid.load(std::memory_order_relaxed) == tid) return;
        if (!isAudioThread(tid)) audioTids[audioTidNext++ & 7] = tid;
        lastAudioTid = tid;
    }
};

static void markDead(Instance* in, const char* why) {
    if (!in->dead.exchange(true) && !in->closing) g_log.error("the 32-bit helper is gone (%s); the plug-in is now silent", why);
    if (in->evWaiter) SetEvent(in->evWaiter);
}

static bool sendMsg(Instance* in, Message& m) {
    EnterCriticalSection(&in->writeLock);
    bool ok = writeMessage(in->pipeOut, m);
    LeaveCriticalSection(&in->writeLock);
    if (!ok) markDead(in, "pipe write failed");
    return ok;
}

static void applyInfo(Instance* in, const EffectInfo& i) {
    AEffect& fx = in->fx;
    fx.numPrograms = i.numPrograms;
    fx.numParams = i.numParams;
    fx.numInputs = i.numInputs;
    fx.numOutputs = i.numOutputs;
    // Double precision is not bridged; hosts use the 32-bit float path.
    fx.flags = i.flags & ~effFlagsCanDoubleReplacing;
    fx.initialDelay = i.initialDelay;
    fx.uniqueID = i.uniqueID;
    fx.version = i.version;
    fx.realQualities = i.realQualities;
    fx.offQualities = i.offQualities;
    fx.ioRatio = i.ioRatio;
}

// ---- calls from the helper (the plug-in calling its host) ----------------------------------

static void executeCallback(Instance* in, Message& m) {
    AEffect* eff = (m.h.flags & F_EFFECT_NULL) ? nullptr : &in->fx;
    int32_t opcode = m.h.opcode;
    OpDesc d = masterDesc(opcode);
    Message r;
    r.h.kind = K_REPLY;
    r.h.id = m.h.id;
    r.h.op = M_AUDIOMASTER;
    ActiveCallback saved = t_cb;
    t_cb = {in, m.h.id};
    intptr_t res = 0;
    auto call = [&](void* ptr) { return in->master(eff, opcode, m.h.index, (intptr_t)m.h.value, ptr, m.h.opt); };
    switch (d.kind) {
    case PK_NONE:
        res = call(nullptr);
        break;
    case PK_STR_IN:
        if (m.h.flags & F_PTR_NULL) res = call(nullptr);
        else {
            m.d1.push_back(0);
            res = call(m.d1.data());
        }
        break;
    case PK_STR_OUT:
        if (m.h.flags & F_PTR_NULL) res = call(nullptr);
        else {
            char buf[kStringMax];
            memset(buf, 0, sizeof(buf));
            res = call(buf);
            buf[kStringMax - 1] = 0;
            r.d1.assign(buf, buf + strlen(buf) + 1);
            r.h.flags |= F_HAS_D1;
        }
        break;
    case PK_SPECIAL:
        if (opcode == audioMasterGetTime) {
            const void* ti = (const void*)call(nullptr);
            if (ti) {
                r.d1.assign((const uint8_t*)ti, (const uint8_t*)ti + kSizeVstTimeInfo);
                r.h.flags |= F_HAS_D1;
                res = 1;
            }
        } else if (opcode == audioMasterProcessEvents) {
            std::vector<uint8_t> store;
            VstEvents* ev = unflattenEvents(m.d1.data(), m.d1.size(), store);
            res = call(ev);
        } else if (opcode == audioMasterIOChanged) {
            if (m.d1.size() >= sizeof(EffectInfo)) {
                EffectInfo info;
                memcpy(&info, m.d1.data(), sizeof(info));
                applyInfo(in, info);
            }
            res = call(nullptr);
        }
        break;
    default: break;
    }
    t_cb = saved;
    r.h.result = (int64_t)res;
    sendMsg(in, r);
}

static void drainCallbacks(Instance* in) {
    for (;;) {
        Message m;
        bool have = false;
        EnterCriticalSection(&in->qLock);
        if (!in->cbQueue.empty()) {
            m = std::move(in->cbQueue.front());
            in->cbQueue.pop_front();
            have = true;
        }
        LeaveCriticalSection(&in->qLock);
        if (!have) return;
        executeCallback(in, m);
    }
}

static DWORD WINAPI readerThread(LPVOID p) {
    Instance* in = (Instance*)p;
    for (;;) {
        Message m;
        if (!readMessage(in->pipeIn, m)) {
            markDead(in, "pipe closed");
            return 0;
        }
        if (m.h.kind == K_REPLY) {
            EnterCriticalSection(&in->qLock);
            uint32_t id = m.h.id;
            in->replies[id] = std::move(m);
            LeaveCriticalSection(&in->qLock);
            SetEvent(in->evWaiter);
        } else {
            // Run it on the host thread that is waiting for the helper if there is one; otherwise
            // on the host's UI thread while the editor is open; otherwise on our worker thread.
            m.queuedTick = GetTickCount();
            EnterCriticalSection(&in->qLock);
            in->cbQueue.push_back(std::move(m));
            bool waiter = in->waitDepth > 0;
            HWND wnd = in->msgWnd;
            LeaveCriticalSection(&in->qLock);
            if (waiter) SetEvent(in->evWaiter);
            else if (wnd) PostMessageW(wnd, WM_BRIDGE_CALLBACKS, 0, 0);
            else SetEvent(in->evWorker);
        }
    }
}

static DWORD WINAPI workerThread(LPVOID p) {
    Instance* in = (Instance*)p;
    HANDLE hs[2] = {in->evQuit, in->evWorker};
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, hs, FALSE, 100);
        if (w == WAIT_OBJECT_0) return 0;
        if (w == WAIT_OBJECT_0 + 1) {
            drainCallbacks(in);
            continue;
        }
        // Safety net: a callback nobody has picked up for a while (the thread it was meant for is
        // blocked elsewhere) is executed here so that the plug-in's UI thread cannot stall.
        for (;;) {
            Message m;
            bool have = false;
            EnterCriticalSection(&in->qLock);
            if (!in->cbQueue.empty() && GetTickCount() - in->cbQueue.front().queuedTick > 250) {
                m = std::move(in->cbQueue.front());
                in->cbQueue.pop_front();
                have = true;
            }
            LeaveCriticalSection(&in->qLock);
            if (!have) break;
            executeCallback(in, m);
        }
    }
}

static LRESULT CALLBACK msgWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_BRIDGE_CALLBACKS) {
        Instance* in = (Instance*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (in) drainCallbacks(in);
        return 0;
    }
    if (msg == WM_BRIDGE_DESTROY) {
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

static void createMsgWindow(Instance* in) {
    if (in->msgWnd) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = msgWndProc;
    wc.hInstance = g_inst;
    wc.lpszClassName = kMsgClass;
    RegisterClassW(&wc); // fails harmlessly when already registered
    HWND h = CreateWindowExW(0, kMsgClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_inst, nullptr);
    if (!h) return;
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)in);
    EnterCriticalSection(&in->qLock);
    in->msgWnd = h;
    LeaveCriticalSection(&in->qLock);
}

static void destroyMsgWindow(Instance* in) {
    EnterCriticalSection(&in->qLock);
    HWND h = in->msgWnd;
    in->msgWnd = nullptr;
    bool pending = !in->cbQueue.empty();
    LeaveCriticalSection(&in->qLock);
    if (!h) return;
    SetWindowLongPtrW(h, GWLP_USERDATA, 0);
    if (GetWindowThreadProcessId(h, nullptr) == GetCurrentThreadId()) DestroyWindow(h);
    else PostMessageW(h, WM_BRIDGE_DESTROY, 0, 0);
    if (pending) SetEvent(in->evWorker);
}

// ---- calls to the helper -------------------------------------------------------------------

static bool takeReply(Instance* in, uint32_t id, Message& reply) {
    bool got = false;
    EnterCriticalSection(&in->qLock);
    auto it = in->replies.find(id);
    if (it != in->replies.end()) {
        reply = std::move(it->second);
        in->replies.erase(it);
        got = true;
    }
    LeaveCriticalSection(&in->qLock);
    return got;
}

// Sends a call on the main channel and waits for the reply, executing the plug-in's host
// callbacks on this thread in the meantime.
static bool mainCall(Instance* in, Message& call, Message& reply) {
    if (in->dead) return false;
    EnterCriticalSection(&in->mainLock);
    DWORD prevOwner = in->mainOwnerTid.exchange(GetCurrentThreadId());
    EnterCriticalSection(&in->qLock);
    in->waitDepth++;
    LeaveCriticalSection(&in->qLock);
    // Anything the plug-in asked before this call is answered first.
    drainCallbacks(in);

    call.h.kind = K_CALL;
    uint32_t id = call.h.id = ++in->nextId;
    call.h.parent = t_cb.inst == in ? t_cb.id : 0;
    // Lets a dialog the plug-in opens during this call come to the front.
    AllowSetForegroundWindow(in->helperPid);
    bool ok = sendMsg(in, call);
    DWORD start = GetTickCount();
    while (ok) {
        if (takeReply(in, id, reply)) break;
        drainCallbacks(in);
        if (in->dead) {
            ok = takeReply(in, id, reply);
            break;
        }
        HANDLE hs[2] = {in->evWaiter, in->hProcess};
        // Delivers sent window messages and incoming drag-and-drop calls while it waits.
        DWORD w = waitPumping(2, hs, 50);
        if (w == WAIT_OBJECT_0 + 1) {
            // The helper exited. Its last reply may still be on its way through the reader thread.
            WaitForSingleObject(in->readerThread, 500);
            ok = takeReply(in, id, reply);
            markDead(in, "process exited");
            break;
        } else if (w == WAIT_TIMEOUT && GetTickCount() - start > 300) {
            // A long call usually means the plug-in is showing a dialog. Keep the host's windows
            // painted meanwhile, as they would be if the dialog ran inside the host.
            MSG msg;
            for (int i = 0; i < 8 && PeekMessageW(&msg, nullptr, WM_PAINT, WM_PAINT, PM_REMOVE); i++) DispatchMessageW(&msg);
        }
    }
    EnterCriticalSection(&in->qLock);
    in->waitDepth--;
    bool leftover = in->waitDepth == 0 && !in->cbQueue.empty();
    LeaveCriticalSection(&in->qLock);
    if (leftover) drainCallbacks(in);
    in->mainOwnerTid = prevOwner;
    LeaveCriticalSection(&in->mainLock);
    return ok;
}

// ---- audio channel -------------------------------------------------------------------------

static void serviceAudioCallback(Instance* in) {
    Shared* s = in->shm;
    int32_t opcode = s->cbOpcode;
    OpDesc d = masterDesc(opcode);
    intptr_t res = 0;
    auto call = [&](void* ptr) { return in->master(&in->fx, opcode, s->cbIndex, (intptr_t)s->cbValue, ptr, s->cbOpt); };
    switch (d.kind) {
    case PK_NONE:
        res = call(nullptr);
        break;
    case PK_STR_IN:
        if (s->cbFlags & F_PTR_NULL) res = call(nullptr);
        else {
            s->cbData[kCbDataBytes - 1] = 0;
            res = call(s->cbData);
        }
        s->cbLen = 0;
        break;
    case PK_STR_OUT:
        if (s->cbFlags & F_PTR_NULL) res = call(nullptr);
        else {
            char buf[kStringMax];
            memset(buf, 0, sizeof(buf));
            res = call(buf);
            buf[kStringMax - 1] = 0;
            size_t n = strlen(buf) + 1;
            memcpy(s->cbData, buf, n);
            s->cbLen = (uint32_t)n;
        }
        break;
    case PK_SPECIAL:
        if (opcode == audioMasterGetTime) {
            const void* ti = (const void*)call(nullptr);
            s->cbLen = 0;
            if (ti) {
                memcpy(s->cbData, ti, kSizeVstTimeInfo);
                s->cbLen = kSizeVstTimeInfo;
                res = 1;
            }
        } else if (opcode == audioMasterIOChanged) {
            if (s->cbLen >= sizeof(EffectInfo)) {
                EffectInfo info;
                memcpy(&info, s->cbData, sizeof(info));
                applyInfo(in, info);
            }
            res = call(nullptr);
        }
        break;
    default: break;
    }
    s->cbResult = (int64_t)res;
}

// Runs one request on the helper's audio thread. The audio lock is held by the caller.
static bool audioRoundTrip(Instance* in) {
    Shared* s = in->shm;
    DWORD tid = GetCurrentThreadId();
    in->audioBusyTid = tid;
    s->evOutBytes = 0;
    s->state = S_REQUEST;
    SetEvent(in->evReq);
    HANDLE hs[2] = {in->evRep, in->hProcess};
    bool ok = false;
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0) {
            markDead(in, "process exited during an audio call");
            break;
        }
        if (s->state == S_CALLBACK) {
            serviceAudioCallback(in);
            s->state = S_CB_REPLY;
            SetEvent(in->evReq);
            continue;
        }
        if (s->state == S_DONE) {
            s->state = S_IDLE;
            ok = true;
            break;
        }
    }
    // Events the plug-in sent to the host during this request are handed over now, still inside
    // the host's call, which is when the host expects them.
    if (ok && s->evOutBytes) {
        uint32_t evBytes = s->evOutBytes > (uint32_t)kEventBytes ? (uint32_t)kEventBytes : s->evOutBytes;
        VstEvents* ev = unflattenEvents(s->evOut, evBytes, in->evStoreAudio);
        if (ev->numEvents) in->master(&in->fx, audioMasterProcessEvents, 0, 0, ev, 0);
    }
    in->audioBusyTid = 0;
    return ok;
}

// True when this thread should use the audio channel: it has run process(), and it is not
// already inside a call to the helper (a host calling back into us from one of its callbacks).
static bool useAudioChannel(Instance* in) {
    DWORD tid = GetCurrentThreadId();
    return in->isAudioThread(tid) && in->audioBusyTid.load(std::memory_order_relaxed) != tid &&
           in->mainOwnerTid.load(std::memory_order_relaxed) != tid;
}

// Fetches from the host what the plug-in is known to ask for on every block.
static void prefetch(Instance* in, int offsetFrames) {
    Shared* s = in->shm;
    s->haveTime = 0;
    if (s->wantsTime) {
        int32_t filter = s->timeFilter;
        const VstTimeInfo* ti = (const VstTimeInfo*)in->master(&in->fx, audioMasterGetTime, 0, filter, nullptr, 0);
        s->timeFilterUsed = filter;
        if (ti) {
            VstTimeInfo t = *ti;
            if (offsetFrames) {
                // Second and later parts of a host block that was split.
                if ((t.flags & kVstPpqPosValid) && (t.flags & kVstTempoValid) && t.sampleRate > 0)
                    t.ppqPos += (double)offsetFrames / t.sampleRate * t.tempo / 60.0;
                t.samplePos += offsetFrames;
            }
            memcpy(s->time, &t, kSizeVstTimeInfo);
            s->haveTime = 1;
        } else {
            s->haveTime = 2;
        }
    }
    uint32_t askOut = s->pinAskOut, askIn = s->pinAskIn, ansOut = 0, ansIn = 0;
    for (int i = 0; askOut >> i; i++)
        if ((askOut >> i) & 1)
            if (in->master(&in->fx, audioMasterPinConnected_deprecated, i, 1, nullptr, 0)) ansOut |= 1u << i;
    for (int i = 0; askIn >> i; i++)
        if ((askIn >> i) & 1)
            if (in->master(&in->fx, audioMasterPinConnected_deprecated, i, 0, nullptr, 0)) ansIn |= 1u << i;
    s->pinAnswerOut = ansOut;
    s->pinAnswerIn = ansIn;
    s->pinValidOut = askOut;
    s->pinValidIn = askIn;
}

static void doProcess(AEffect* e, float** inputs, float** outputs, int32_t frames, bool accumulate) {
    Instance* in = (Instance*)e->object;
    if (!in || frames <= 0) return;
    int nin = in->fx.numInputs < kMaxChannels ? in->fx.numInputs : kMaxChannels;
    int nout = in->fx.numOutputs < kMaxChannels ? in->fx.numOutputs : kMaxChannels;
    auto silence = [&](int from) {
        if (accumulate || !outputs) return;
        for (int c = 0; c < nout; c++)
            if (outputs[c]) memset(outputs[c] + from, 0, (size_t)(frames - from) * sizeof(float));
    };
    if (in->dead) return silence(0);
    in->noteAudioThread(GetCurrentThreadId());
    EnterCriticalSection(&in->audioLock);
    Shared* s = in->shm;
    int done = 0;
    while (done < frames) {
        int n = frames - done < kMaxFrames ? frames - done : kMaxFrames;
        size_t bytes = (size_t)n * sizeof(float);
        for (int c = 0; c < nin; c++) {
            if (inputs && inputs[c]) memcpy(s->audioIn[c], inputs[c] + done, bytes);
            else memset(s->audioIn[c], 0, bytes);
        }
        // The plug-in sees the host's output buffers as it would natively: an accumulating
        // process() adds to them, and outputs it chooses not to write keep their content.
        for (int c = 0; c < nout; c++) {
            if (outputs && outputs[c]) memcpy(s->audioOut[c], outputs[c] + done, bytes);
            else memset(s->audioOut[c], 0, bytes);
        }
        prefetch(in, done);
        s->op = A_PROCESS;
        s->frames = n;
        s->index = accumulate ? 1 : 0;
        if (!audioRoundTrip(in)) {
            silence(done);
            break;
        }
        for (int c = 0; c < nout; c++)
            if (outputs && outputs[c]) memcpy(outputs[c] + done, s->audioOut[c], bytes);
        done += n;
    }
    LeaveCriticalSection(&in->audioLock);
}

static void __cdecl bridgeProcessReplacing(AEffect* e, float** inputs, float** outputs, int32_t frames) {
    doProcess(e, inputs, outputs, frames, false);
}

static void __cdecl bridgeProcess(AEffect* e, float** inputs, float** outputs, int32_t frames) {
    doProcess(e, inputs, outputs, frames, true);
}

static void __cdecl bridgeSetParameter(AEffect* e, int32_t index, float value) {
    Instance* in = (Instance*)e->object;
    if (!in || in->dead) return;
    if (useAudioChannel(in)) {
        EnterCriticalSection(&in->audioLock);
        in->shm->op = A_SETPARAM;
        in->shm->index = index;
        in->shm->fvalue = value;
        audioRoundTrip(in);
        LeaveCriticalSection(&in->audioLock);
        return;
    }
    Message c, r;
    c.h.op = M_SETPARAM;
    c.h.index = index;
    c.h.opt = value;
    mainCall(in, c, r);
}

static float __cdecl bridgeGetParameter(AEffect* e, int32_t index) {
    Instance* in = (Instance*)e->object;
    if (!in || in->dead) return 0.0f;
    if (useAudioChannel(in)) {
        EnterCriticalSection(&in->audioLock);
        in->shm->op = A_GETPARAM;
        in->shm->index = index;
        in->shm->fvalue = 0.0f;
        float v = audioRoundTrip(in) ? in->shm->fvalue : 0.0f;
        LeaveCriticalSection(&in->audioLock);
        return v;
    }
    Message c, r;
    c.h.op = M_GETPARAM;
    c.h.index = index;
    return mainCall(in, c, r) ? r.h.opt : 0.0f;
}

// ---- lifetime ------------------------------------------------------------------------------

static void destroyInstance(Instance* in, bool graceful) {
    in->closing = true;
    destroyMsgWindow(in);
    if (in->hProcess) {
        // After effClose the helper unloads the plug-in and exits by itself.
        if (!graceful || WaitForSingleObject(in->hProcess, 5000) != WAIT_OBJECT_0) TerminateProcess(in->hProcess, 1);
        WaitForSingleObject(in->hProcess, 2000);
    }
    in->dead = true;
    if (in->evQuit) SetEvent(in->evQuit);
    if (in->pipeOut != INVALID_HANDLE_VALUE) CloseHandle(in->pipeOut);
    if (in->readerThread) {
        // The read fails once the helper's end of the pipe is closed.
        if (WaitForSingleObject(in->readerThread, 3000) != WAIT_OBJECT_0) {
            CancelSynchronousIo(in->readerThread);
            WaitForSingleObject(in->readerThread, 2000);
        }
        CloseHandle(in->readerThread);
    }
    if (in->workerThread) {
        WaitForSingleObject(in->workerThread, 5000);
        CloseHandle(in->workerThread);
    }
    if (in->pipeIn != INVALID_HANDLE_VALUE) CloseHandle(in->pipeIn);
    if (in->shm) UnmapViewOfFile(in->shm);
    for (HANDLE h : {in->hMap, in->evReq, in->evRep, in->evReady, in->hProcess, in->hJob, in->evWaiter, in->evWorker, in->evQuit})
        if (h) CloseHandle(h);
    delete in;
}

static intptr_t __cdecl bridgeDispatcher(AEffect* e, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt) {
    Instance* in = (Instance*)e->object;
    if (!in) return 0;

    if (opcode == effClose) {
        intptr_t res = 0;
        if (!in->dead) {
            EnterCriticalSection(&in->audioLock); // no audio request is in flight past this point
            Message c, r;
            c.h.op = M_DISPATCH;
            c.h.opcode = effClose;
            if (mainCall(in, c, r)) res = (intptr_t)r.h.result;
            LeaveCriticalSection(&in->audioLock);
        }
        e->object = nullptr;
        destroyInstance(in, true);
        return res;
    }
    if (in->dead) return 0;

    OpDesc d = dispatchDesc(opcode);
    if (d.kind == PK_SKIP) return 0;

    if (opcode == effProcessEvents && useAudioChannel(in)) {
        EnterCriticalSection(&in->audioLock);
        Shared* s = in->shm;
        s->evInBytes = (uint32_t)flattenEvents((const VstEvents*)ptr, s->evIn, kEventBytes);
        s->op = A_EVENTS;
        s->result = 0;
        intptr_t res = audioRoundTrip(in) ? (intptr_t)s->result : 0;
        LeaveCriticalSection(&in->audioLock);
        return res;
    }

    Message c, r;
    c.h.op = M_DISPATCH;
    c.h.opcode = opcode;
    c.h.index = index;
    c.h.value = (int64_t)value;
    c.h.opt = opt;
    const uint8_t* bytes = (const uint8_t*)ptr;
    switch (d.kind) {
    case PK_STR_IN:
        if (!ptr) c.h.flags |= F_PTR_NULL;
        else c.d1.assign(bytes, bytes + strnlen((const char*)ptr, 65535));
        break;
    case PK_STR_OUT:
    case PK_BLOB_OUT:
        if (!ptr) c.h.flags |= F_PTR_NULL;
        break;
    case PK_BLOB_IN:
    case PK_BLOB_INOUT:
        if (!ptr) c.h.flags |= F_PTR_NULL;
        else c.d1.assign(bytes, bytes + d.size);
        break;
    case PK_SPECIAL:
        switch (opcode) {
        case effEditOpen:
            c.h.value = (int64_t)(uintptr_t)ptr; // parent window handle
            createMsgWindow(in);
            break;
        case effSetChunk:
            c.h.value = 0;
            if (ptr && value > 0) c.d1.assign(bytes, bytes + (size_t)value);
            break;
        case effProcessEvents:
            c.d1.resize(flatEventsSize((const VstEvents*)ptr));
            if (!c.d1.empty()) flattenEvents((const VstEvents*)ptr, c.d1.data(), c.d1.size());
            break;
        case effSetSpeakerArrangement: {
            auto arrangement = [](const void* p, std::vector<uint8_t>& out) {
                int32_t channels;
                memcpy(&channels, (const uint8_t*)p + 4, 4);
                if (channels < 8) channels = 8;
                if (channels > 64) channels = 64;
                size_t n = kSizeVstSpeakerArrangementHeader + (size_t)channels * kSizeVstSpeakerProperties;
                out.assign((const uint8_t*)p, (const uint8_t*)p + n);
            };
            c.h.value = 0;
            if (value) arrangement((const void*)value, c.d2);
            else c.h.flags |= F_VALUE_NULL;
            if (ptr) arrangement(ptr, c.d1);
            else c.h.flags |= F_PTR_NULL;
            break;
        }
        case effGetSpeakerArrangement:
            c.h.value = 0;
            break;
        default: break;
        }
        break;
    default: break;
    }

    if (!mainCall(in, c, r)) {
        if (opcode == effEditClose) destroyMsgWindow(in);
        return 0;
    }

    switch (d.kind) {
    case PK_STR_OUT:
        // Written exactly as the plug-in wrote it, including its length.
        if (ptr && (r.h.flags & F_HAS_D1) && !r.d1.empty()) memcpy(ptr, r.d1.data(), r.d1.size());
        break;
    case PK_BLOB_OUT:
    case PK_BLOB_INOUT:
        if (ptr && (r.h.flags & F_HAS_D1) && r.d1.size() >= d.size) memcpy(ptr, r.d1.data(), d.size);
        break;
    case PK_SPECIAL:
        switch (opcode) {
        case effEditGetRect:
            if (ptr && (r.h.flags & F_HAS_D1) && r.d1.size() == sizeof(ERect)) {
                memcpy(&in->rect, r.d1.data(), sizeof(ERect));
                *(ERect**)ptr = &in->rect;
            }
            break;
        case effEditClose:
            destroyMsgWindow(in);
            break;
        case effGetChunk:
            in->chunk = std::move(r.d1);
            if (ptr) *(void**)ptr = in->chunk.empty() ? nullptr : in->chunk.data();
            break;
        case effGetSpeakerArrangement:
            if (value && (r.h.flags & F_HAS_D2)) {
                in->spkIn = std::move(r.d2);
                *(void**)value = in->spkIn.data();
            }
            if (ptr && (r.h.flags & F_HAS_D1)) {
                in->spkOut = std::move(r.d1);
                *(void**)ptr = in->spkOut.data();
            }
            break;
        default: break;
        }
        break;
    default: break;
    }
    return (intptr_t)r.h.result;
}

// ---- start-up ------------------------------------------------------------------------------

static bool fileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring envVar(const wchar_t* name) {
    wchar_t buf[2048];
    DWORD n = GetEnvironmentVariableW(name, buf, 2048);
    return (n && n < 2048) ? std::wstring(buf, n) : std::wstring();
}

static std::wstring regString(HKEY root, const wchar_t* key, const wchar_t* name, REGSAM view = 0) {
    HKEY h;
    if (RegOpenKeyExW(root, key, 0, KEY_QUERY_VALUE | view, &h) != ERROR_SUCCESS) return L"";
    wchar_t buf[2048];
    DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
    std::wstring out;
    if (RegQueryValueExW(h, name, nullptr, &type, (BYTE*)buf, &size) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        buf[size / sizeof(wchar_t)] = 0;
        out = buf;
    }
    RegCloseKey(h);
    return out;
}

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

struct Config {
    std::wstring host32, plugin32;
    bool debug = false;
};

// Where the helper and the original plug-in are. In order: a "<bridge name>.ini" next to this
// DLL, the registry values written by the installer, then the usual 32-bit VST2 folders.
static bool loadConfig(Config& cfg, std::string& why) {
    wchar_t self[2048];
    GetModuleFileNameW(g_inst, self, 2048);
    std::wstring selfPath = self;
    std::wstring dir = selfPath.substr(0, selfPath.find_last_of(L'\\'));
    std::wstring ini = selfPath.substr(0, selfPath.find_last_of(L'.')) + L".ini";
    auto absolute = [&](std::wstring p) {
        if (p.empty()) return p;
        bool rooted = (p.size() > 1 && p[1] == L':') || p[0] == L'\\';
        return rooted ? p : dir + L"\\" + p;
    };
    if (fileExists(ini)) {
        wchar_t buf[2048];
        GetPrivateProfileStringW(L"Bridge", L"Host32", L"", buf, 2048, ini.c_str());
        cfg.host32 = absolute(buf);
        GetPrivateProfileStringW(L"Bridge", L"Plugin32", L"", buf, 2048, ini.c_str());
        cfg.plugin32 = absolute(buf);
        cfg.debug = GetPrivateProfileIntW(L"Bridge", L"Debug", 0, ini.c_str()) != 0;
    }
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        if (cfg.host32.empty()) cfg.host32 = regString(root, kRegKey, L"Host32");
        if (cfg.plugin32.empty()) cfg.plugin32 = regString(root, kRegKey, L"Plugin32");
        if (!cfg.debug) cfg.debug = regString(root, kRegKey, L"Debug") == L"1";
    }
    if (!envVar(L"PHATBRIDGE_DEBUG").empty()) cfg.debug = true;
    if (cfg.host32.empty()) {
        for (const std::wstring& p : {dir + L"\\" + kHostFile, dir + L"\\pHATmatik PRO Bridge\\" + kHostFile})
            if (fileExists(p)) {
                cfg.host32 = p;
                break;
            }
    }
    if (cfg.plugin32.empty()) {
        std::wstring pf86 = envVar(L"ProgramFiles(x86)"), cf86 = envVar(L"CommonProgramFiles(x86)");
        std::vector<std::wstring> folders;
        std::wstring reg = regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\VST", L"VSTPluginsPath", KEY_WOW64_32KEY);
        if (!reg.empty()) folders.push_back(reg);
        if (!pf86.empty()) {
            folders.push_back(pf86 + L"\\Steinberg\\VstPlugins");
            folders.push_back(pf86 + L"\\VstPlugins");
            folders.push_back(pf86 + L"\\Steinberg\\VstPlugins\\iZotope");
        }
        if (!cf86.empty()) folders.push_back(cf86 + L"\\VST2");
        for (const std::wstring& f : folders)
            if (fileExists(f + L"\\" + kPluginFile)) {
                cfg.plugin32 = f + L"\\" + kPluginFile;
                break;
            }
    }
    if (cfg.host32.empty() || !fileExists(cfg.host32)) {
        why = "the 32-bit helper (" + narrow(kHostFile) + ") was not found: '" + narrow(cfg.host32) + "'";
        return false;
    }
    if (cfg.plugin32.empty() || !fileExists(cfg.plugin32)) {
        why = "the original 32-bit plug-in (" + narrow(kPluginFile) + ") was not found: '" + narrow(cfg.plugin32) + "'";
        return false;
    }
    return true;
}

static uint32_t hostDpiAwareness() {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    typedef HANDLE(WINAPI * GetCtx)();
    typedef int(WINAPI * GetAw)(HANDLE);
    typedef BOOL(WINAPI * AreEq)(HANDLE, HANDLE);
    GetCtx getCtx = u ? (GetCtx)GetProcAddress(u, "GetThreadDpiAwarenessContext") : nullptr;
    GetAw getAw = u ? (GetAw)GetProcAddress(u, "GetAwarenessFromDpiAwarenessContext") : nullptr;
    AreEq areEq = u ? (AreEq)GetProcAddress(u, "AreDpiAwarenessContextsEqual") : nullptr;
    if (!getCtx || !getAw) return IsProcessDPIAware() ? 1 : 0;
    HANDLE ctx = getCtx();
    int a = getAw(ctx);
    if (a <= 0) return 0;
    if (a == 1) return 1;
    if (areEq && areEq(ctx, (HANDLE)(intptr_t)-4)) return 3;
    return 2;
}

static bool launch(Instance* in, const Config& cfg, std::string& why) {
    unsigned int r1 = 0, r2 = 0;
    rand_s(&r1);
    rand_s(&r2);
    wchar_t session[96];
    swprintf(session, 96, L"PhatBridge-%08lx-%08lx-%08x%08x", GetCurrentProcessId(), GetTickCount(), r1, r2);
    std::wstring name = session;
    auto failed = [&](const char* what) {
        char buf[200];
        _snprintf(buf, sizeof(buf), "%s failed with error %lu", what, GetLastError());
        buf[sizeof(buf) - 1] = 0;
        why = buf;
        return false;
    };

    in->hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)sizeof(Shared), (L"Local\\" + name + L"-shm").c_str());
    if (!in->hMap) return failed("CreateFileMapping");
    in->shm = (Shared*)MapViewOfFile(in->hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
    if (!in->shm) return failed("MapViewOfFile");
    Shared* s = in->shm;
    s->magic = kMagic;
    s->version = kProtocolVersion;
    s->bridgePid = GetCurrentProcessId();
    s->dpiAwareness = hostDpiAwareness();
    s->debug = g_log.verbose ? 1 : 0;
    wcsncpy(s->pluginPath, cfg.plugin32.c_str(), 1023);
    wcsncpy(s->logPath, g_log.path, 1023);

    in->evReq = CreateEventW(nullptr, FALSE, FALSE, (L"Local\\" + name + L"-req").c_str());
    in->evRep = CreateEventW(nullptr, FALSE, FALSE, (L"Local\\" + name + L"-rep").c_str());
    in->evReady = CreateEventW(nullptr, FALSE, FALSE, (L"Local\\" + name + L"-ready").c_str());
    in->evWaiter = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    in->evWorker = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    in->evQuit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!in->evReq || !in->evRep || !in->evReady || !in->evWaiter || !in->evWorker || !in->evQuit) return failed("CreateEvent");

    DWORD mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
    in->pipeOut = CreateNamedPipeW((L"\\\\.\\pipe\\" + name + L"-c2h").c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE, mode, 1, 1 << 16, 1 << 16, 0, nullptr);
    in->pipeIn = CreateNamedPipeW((L"\\\\.\\pipe\\" + name + L"-h2c").c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE, mode, 1, 1 << 16, 1 << 16, 0, nullptr);
    if (in->pipeOut == INVALID_HANDLE_VALUE || in->pipeIn == INVALID_HANDLE_VALUE) return failed("CreateNamedPipe");

    // The helper must never outlive the host: it is killed when the job handle closes, which
    // happens when the instance is destroyed or the host process ends for any reason.
    in->hJob = CreateJobObjectW(nullptr, nullptr);
    if (in->hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(in->hJob, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }

    std::wstring cmd = L"\"" + cfg.host32 + L"\" " + name;
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(cfg.host32.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi))
        return failed("starting the 32-bit helper");
    in->hProcess = pi.hProcess;
    in->helperPid = pi.dwProcessId;
    if (in->hJob && !AssignProcessToJobObject(in->hJob, pi.hProcess)) g_log.trace("AssignProcessToJobObject failed (%lu)", GetLastError());
    SetPriorityClass(pi.hProcess, GetPriorityClass(GetCurrentProcess()));
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    HANDLE hs[2] = {in->evReady, in->hProcess};
    DWORD w = WaitForMultipleObjects(2, hs, FALSE, 20000);
    if (w != WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(in->hProcess, &code);
        char buf[200];
        _snprintf(buf, sizeof(buf), w == WAIT_TIMEOUT ? "the 32-bit helper did not start within 20 s" : "the 32-bit helper exited at start-up with code %lu", code);
        why = buf;
        return false;
    }
    // The helper has already opened both pipes; these return at once.
    ConnectNamedPipe(in->pipeOut, nullptr);
    ConnectNamedPipe(in->pipeIn, nullptr);

    in->readerThread = CreateThread(nullptr, 0, readerThread, in, 0, nullptr);
    in->workerThread = CreateThread(nullptr, 0, workerThread, in, 0, nullptr);
    return in->readerThread && in->workerThread;
}

static void initLog() {
    if (g_log.path[0]) return;
    g_log.who = "bridge64";
    wchar_t tmp[900];
    DWORD n = GetTempPathW(900, tmp);
    if (n && n < 900) swprintf(g_log.path, 1024, L"%lspHATmatikBridge.log", tmp);
}

extern "C" AEffect* __cdecl VSTPluginMain(AudioMasterCallback master) {
    if (!master) return nullptr;
    initLog();
    Config cfg;
    std::string why;
    if (!loadConfig(cfg, why)) {
        g_log.error("cannot load: %s", why.c_str());
        return nullptr;
    }
    if (cfg.debug) g_log.verbose = true;
    g_log.trace("loading; helper '%s', plug-in '%s'", narrow(cfg.host32).c_str(), narrow(cfg.plugin32).c_str());

    Instance* in = new Instance();
    in->master = master;
    in->fx.magic = kEffectMagic;
    in->fx.object = in;
    in->fx.dispatcher = bridgeDispatcher;
    in->fx.process = bridgeProcess;
    in->fx.setParameter = bridgeSetParameter;
    in->fx.getParameter = bridgeGetParameter;
    in->fx.processReplacing = bridgeProcessReplacing;
    in->fx.processDoubleReplacing = nullptr;
    in->fx.ioRatio = 1.0f;

    if (!launch(in, cfg, why)) {
        g_log.error("cannot load: %s", why.c_str());
        destroyInstance(in, false);
        return nullptr;
    }
    // The plug-in is created here. If it shows a dialog (its authorization wizard does), this
    // returns when the dialog is closed.
    Message c, r;
    c.h.op = M_LOAD;
    if (!mainCall(in, c, r) || r.h.result != 1 || r.d1.size() < sizeof(EffectInfo)) {
        std::string reason(r.d1.begin(), r.d1.end());
        g_log.error("the 32-bit plug-in could not be loaded: %s", reason.empty() ? "the helper stopped" : reason.c_str());
        destroyInstance(in, true);
        return nullptr;
    }
    EffectInfo info;
    memcpy(&info, r.d1.data(), sizeof(info));
    applyInfo(in, info);
    g_log.trace("loaded: uniqueID %08X params %d programs %d in %d out %d flags 0x%X", info.uniqueID, info.numParams, info.numPrograms,
                info.numInputs, info.numOutputs, in->fx.flags);
    return &in->fx;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_inst = inst;
    } else if (reason == DLL_PROCESS_DETACH && !reserved) {
        UnregisterClassW(kMsgClass, inst);
    }
    return TRUE;
}
