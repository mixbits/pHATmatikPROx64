// Protocol shared by the 64-bit bridge DLL and the 32-bit helper process.
//
// Two channels connect them:
//   * the main channel - two one-way named pipes carrying framed messages. Either side may
//     issue a CALL and waits for the REPLY with the same id; while waiting it serves the CALLs
//     the other side issues in order to answer it.
//   * the audio channel - one shared memory block and two events. Strict ping-pong between the
//     host's audio thread and the helper's audio thread; no allocation, no pipe.
#pragma once
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include "vst2.h"

namespace ipc {

const uint32_t kMagic = 0x42544850; // 'PHTB'
const uint32_t kProtocolVersion = 3;
const int kMaxChannels = 32;   // per direction
const int kMaxFrames = 16384;  // per audio request; larger host blocks are split
const int kEventBytes = 96 * 1024;
const int kCbDataBytes = 1024;
const int kStringMax = 1024;   // buffer handed to the plug-in for "out" strings
const uint32_t kMaxPayload = 512u * 1024u * 1024u;

// ---- audio channel -------------------------------------------------------------------------

enum AudioOp : int32_t { A_NONE = 0, A_PROCESS, A_EVENTS, A_SETPARAM, A_GETPARAM };
enum AudioState : int32_t { S_IDLE = 0, S_REQUEST, S_CALLBACK, S_CB_REPLY, S_DONE };

#pragma pack(push, 8)
struct EffectInfo {
    int32_t numPrograms, numParams, numInputs, numOutputs;
    int32_t flags, initialDelay, uniqueID, version;
    int32_t realQualities, offQualities;
    float ioRatio;
    int32_t pad;
};

struct Shared {
    uint32_t magic;
    uint32_t version;
    uint32_t bridgePid;
    uint32_t dpiAwareness; // 0 unaware, 1 system, 2 per-monitor, 3 per-monitor v2
    uint32_t debug;
    uint32_t pad0;
    wchar_t pluginPath[1024];
    wchar_t logPath[1024];

    // Request, written by the bridge.
    int32_t state;
    int32_t op;
    int32_t frames;
    int32_t index;
    float fvalue;
    int32_t pad1;
    int64_t result;

    // Host answers fetched ahead of a process call, so the plug-in's per-block questions do not
    // each cost a round trip. The helper records what the plug-in asks for; the bridge supplies it.
    int32_t wantsTime;   // helper: the plug-in calls audioMasterGetTime while processing
    int32_t timeFilter;  // helper: union of the flags it asked for
    int32_t haveTime;    // bridge: 0 not fetched, 1 time[] valid, 2 the host returned null
    int32_t timeFilterUsed; // bridge: the flags it asked the host for
    uint8_t time[kSizeVstTimeInfo];
    uint32_t pinAskIn, pinAskOut;       // helper: pins the plug-in asks about
    uint32_t pinValidIn, pinValidOut;   // bridge: which answers below are valid
    uint32_t pinAnswerIn, pinAnswerOut; // bridge: host's answers (bit set = non-zero)

    uint32_t evInBytes;  // bridge: flattened events for A_EVENTS
    uint32_t evOutBytes; // helper: events the plug-in sent while processing

    // A host callback made by the plug-in on the audio thread that could not be answered locally.
    int32_t cbOpcode;
    int32_t cbIndex;
    int64_t cbValue;
    float cbOpt;
    uint32_t cbFlags;
    int64_t cbResult;
    uint32_t cbLen;
    uint32_t pad3;
    uint8_t cbData[kCbDataBytes];

    uint8_t evIn[kEventBytes];
    uint8_t evOut[kEventBytes];
    float audioIn[kMaxChannels][kMaxFrames];
    float audioOut[kMaxChannels][kMaxFrames];
};

// ---- main channel --------------------------------------------------------------------------

enum MsgKind : uint32_t { K_CALL = 1, K_REPLY = 2 };
enum MsgOp : int32_t { M_LOAD = 1, M_DISPATCH, M_SETPARAM, M_GETPARAM, M_AUDIOMASTER };
enum MsgFlags : uint32_t {
    F_PTR_NULL = 1,     // the caller's ptr argument was null
    F_VALUE_NULL = 2,   // the caller's value argument (when it is a pointer) was null
    F_UNTOUCHED = 4,    // reply: the callee did not write to the out buffer
    F_HAS_D1 = 8,       // reply: d1 carries data for the caller's ptr
    F_HAS_D2 = 16,
    F_EFFECT_NULL = 32, // audioMaster call: the plug-in passed a null effect pointer
};

struct MsgHeader {
    uint32_t kind;
    uint32_t id;
    uint32_t parent; // CALL: id of the peer's call this one helps to answer, or 0
    int32_t op;
    int32_t opcode;
    int32_t index;
    int64_t value;
    float opt;
    uint32_t flags;
    int64_t result;
    uint32_t len1;
    uint32_t len2;
};
#pragma pack(pop)

static_assert(sizeof(MsgHeader) == 56, "MsgHeader must match on x86 and x64");
static_assert(sizeof(EffectInfo) == 48, "EffectInfo must match on x86 and x64");
static_assert(offsetof(Shared, state) == 4120, "Shared layout must match on x86 and x64");
static_assert(offsetof(Shared, time) == 4168, "Shared layout must match on x86 and x64");
static_assert(offsetof(Shared, cbOpcode) == 4288, "Shared layout must match on x86 and x64");
static_assert(offsetof(Shared, cbData) == 4328, "Shared layout must match on x86 and x64");
static_assert(offsetof(Shared, audioIn) == 4328 + kCbDataBytes + 2 * kEventBytes, "Shared layout must match on x86 and x64");

struct Message {
    MsgHeader h{};
    std::vector<uint8_t> d1, d2;
    uint32_t queuedTick = 0;
};

inline bool readAll(HANDLE h, void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    while (n) {
        DWORD got = 0;
        DWORD want = n > (1u << 20) ? (1u << 20) : (DWORD)n;
        if (!ReadFile(h, p, want, &got, nullptr) || got == 0) return false;
        p += got;
        n -= got;
    }
    return true;
}

inline bool writeAll(HANDLE h, const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    while (n) {
        DWORD put = 0;
        DWORD want = n > (1u << 20) ? (1u << 20) : (DWORD)n;
        if (!WriteFile(h, p, want, &put, nullptr)) return false;
        p += put;
        n -= put;
    }
    return true;
}

inline bool readMessage(HANDLE h, Message& m) {
    if (!readAll(h, &m.h, sizeof(m.h))) return false;
    if (m.h.len1 > kMaxPayload || m.h.len2 > kMaxPayload) return false;
    m.d1.resize(m.h.len1);
    m.d2.resize(m.h.len2);
    if (m.h.len1 && !readAll(h, m.d1.data(), m.h.len1)) return false;
    if (m.h.len2 && !readAll(h, m.d2.data(), m.h.len2)) return false;
    return true;
}

// The caller serialises writers.
inline bool writeMessage(HANDLE h, Message& m) {
    m.h.len1 = (uint32_t)m.d1.size();
    m.h.len2 = (uint32_t)m.d2.size();
    if (m.h.len1 == 0 && m.h.len2 == 0) return writeAll(h, &m.h, sizeof(m.h));
    // One write for small messages keeps the header and payload together.
    if (m.h.len1 + m.h.len2 <= 4096) {
        uint8_t tmp[sizeof(MsgHeader) + 4096];
        memcpy(tmp, &m.h, sizeof(m.h));
        if (m.h.len1) memcpy(tmp + sizeof(m.h), m.d1.data(), m.h.len1);
        if (m.h.len2) memcpy(tmp + sizeof(m.h) + m.h.len1, m.d2.data(), m.h.len2);
        return writeAll(h, tmp, sizeof(m.h) + m.h.len1 + m.h.len2);
    }
    if (!writeAll(h, &m.h, sizeof(m.h))) return false;
    if (m.h.len1 && !writeAll(h, m.d1.data(), m.h.len1)) return false;
    if (m.h.len2 && !writeAll(h, m.d2.data(), m.h.len2)) return false;
    return true;
}

// ---- waiting for the other side --------------------------------------------------------------

// Waits for one of the handles on a thread that may own windows. Returns WAIT_OBJECT_0 + index,
// WAIT_TIMEOUT, or WAIT_OBJECT_0 + count after it has only delivered messages.
//
// While it waits it delivers what the other process's UI thread may need from this thread:
//   * window messages sent to this thread's windows (the plug-in's editor is a child of a window
//     in the host process, so the two UI threads send each other messages);
//   * on a COM single-threaded apartment, incoming COM calls. OLE drag and drop between the two
//     processes arrives this way: when MIDI is dragged out of the plug-in, the helper's drag
//     loop calls the host's drop target, and that call must be answered even though the host is
//     at that moment waiting for the helper. Without this the two wait for each other forever.
inline DWORD waitPumping(DWORD count, const HANDLE* handles, DWORD timeoutMs) {
    APTTYPE type;
    APTTYPEQUALIFIER qualifier;
    if (SUCCEEDED(CoGetApartmentType(&type, &qualifier)) && (type == APTTYPE_STA || type == APTTYPE_MAINSTA)) {
        DWORD index = 0;
        HRESULT hr = CoWaitForMultipleHandles(0, timeoutMs, count, (LPHANDLE)handles, &index);
        if (hr == S_OK) return WAIT_OBJECT_0 + index;
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        if (hr == RPC_S_CALLPENDING) return WAIT_TIMEOUT;
        // Anything else: fall back to the plain wait below.
    }
    DWORD w = MsgWaitForMultipleObjects(count, handles, FALSE, timeoutMs, QS_SENDMESSAGE);
    if (w == WAIT_OBJECT_0 + count) {
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
    return w;
}

// ---- how each opcode's pointer argument crosses the bridge ---------------------------------

enum PtrKind : uint8_t {
    PK_NONE,       // no pointer; forwarded with ptr = null
    PK_SKIP,       // not forwarded; answered with 0
    PK_STR_IN,     // zero-terminated string read by the callee
    PK_STR_OUT,    // string buffer written by the callee
    PK_BLOB_IN,    // fixed-size structure read by the callee
    PK_BLOB_OUT,   // fixed-size structure written by the callee
    PK_BLOB_INOUT, // fixed-size structure read and written by the callee
    PK_SPECIAL,    // handled case by case
};

struct OpDesc {
    PtrKind kind;
    uint16_t size;
};

inline OpDesc dispatchDesc(int32_t op) {
    switch (op) {
    case effOpen: case effSetProgram: case effGetProgram: case effGetVu_deprecated:
    case effSetSampleRate: case effSetBlockSize: case effMainsChanged: case effEditIdle:
    case effIdentify_deprecated: case effCanBeAutomated: case effGetNumProgramCategories_deprecated:
    case effCopyProgram_deprecated: case effConnectInput_deprecated: case effConnectOutput_deprecated:
    case effGetPlugCategory: case effGetCurrentPosition_deprecated:
    case effSetBlockSizeAndSampleRate_deprecated: case effSetBypass: case effGetVendorVersion:
    case effGetTailSize: case effIdle_deprecated: case effSetViewPosition_deprecated:
    case effKeysRequired_deprecated: case effGetVstVersion: case effEditKeyDown: case effEditKeyUp:
    case effSetEditKnobMode: case effHasMidiProgramsChanged: case effBeginSetProgram:
    case effEndSetProgram: case effStartProcess: case effStopProcess:
    case effSetTotalSampleToProcess: case effSetPanLaw: case effSetProcessPrecision:
    case effGetNumMidiInputChannels: case effGetNumMidiOutputChannels:
        return {PK_NONE, 0};
    case effSetProgramName: case effString2Parameter: case effCanDo:
        return {PK_STR_IN, 0};
    case effGetProgramName: case effGetParamLabel: case effGetParamDisplay: case effGetParamName:
    case effGetProgramNameIndexed: case effGetEffectName: case effGetErrorText_deprecated:
    case effGetVendorString: case effGetProductString: case effShellGetNextPlugin:
        return {PK_STR_OUT, 0};
    case effGetInputProperties: case effGetOutputProperties:
        return {PK_BLOB_OUT, kSizeVstPinProperties};
    case effGetParameterProperties:
        return {PK_BLOB_OUT, kSizeVstParameterProperties};
    case effGetMidiProgramName: case effGetCurrentMidiProgram:
        return {PK_BLOB_INOUT, kSizeMidiProgramName};
    case effGetMidiProgramCategory:
        return {PK_BLOB_INOUT, kSizeMidiProgramCategory};
    case effGetMidiKeyName:
        return {PK_BLOB_INOUT, kSizeMidiKeyName};
    case effBeginLoadBank: case effBeginLoadProgram:
        return {PK_BLOB_IN, kSizeVstPatchChunkInfo};
    case effClose: case effEditGetRect: case effEditOpen: case effEditClose: case effGetChunk:
    case effSetChunk: case effProcessEvents: case effSetSpeakerArrangement: case effGetSpeakerArrangement:
        return {PK_SPECIAL, 0};
    default:
        // Mac-only editor calls, offline processing, vendor-specific calls and anything unknown
        // carry pointers whose meaning is not known here.
        return {PK_SKIP, 0};
    }
}

inline OpDesc masterDesc(int32_t op) {
    switch (op) {
    case audioMasterAutomate: case audioMasterVersion: case audioMasterCurrentId: case audioMasterIdle:
    case audioMasterPinConnected_deprecated: case audioMasterWantMidi_deprecated:
    case audioMasterTempoAt_deprecated: case audioMasterGetNumAutomatableParameters_deprecated:
    case audioMasterGetParameterQuantization_deprecated: case audioMasterNeedIdle_deprecated:
    case audioMasterSizeWindow: case audioMasterGetSampleRate: case audioMasterGetBlockSize:
    case audioMasterGetInputLatency: case audioMasterGetOutputLatency:
    case audioMasterWillReplaceOrAccumulate_deprecated: case audioMasterGetCurrentProcessLevel:
    case audioMasterGetAutomationState: case audioMasterSetOutputSampleRate_deprecated:
    case audioMasterGetVendorVersion: case audioMasterGetLanguage: case audioMasterUpdateDisplay:
    case audioMasterBeginEdit: case audioMasterEndEdit:
        return {PK_NONE, 0};
    case audioMasterCanDo:
        return {PK_STR_IN, 0};
    case audioMasterGetVendorString: case audioMasterGetProductString:
        return {PK_STR_OUT, 0};
    case audioMasterGetTime: case audioMasterProcessEvents: case audioMasterIOChanged:
        return {PK_SPECIAL, 0};
    default:
        return {PK_SKIP, 0};
    }
}

inline void fillInfo(EffectInfo& i, const AEffect* e) {
    i.numPrograms = e->numPrograms; i.numParams = e->numParams;
    i.numInputs = e->numInputs; i.numOutputs = e->numOutputs;
    i.flags = e->flags; i.initialDelay = e->initialDelay;
    i.uniqueID = e->uniqueID; i.version = e->version;
    i.realQualities = e->realQualities; i.offQualities = e->offQualities;
    i.ioRatio = e->ioRatio; i.pad = 0;
}

// ---- events --------------------------------------------------------------------------------
// Flattened form: for each event a uint32 record length followed by the record.
//   MIDI and other fixed events: the 32-byte VstEvent as is.
//   System exclusive: type, byteSize(unused), deltaFrames, flags, dumpBytes, then the dump.

inline size_t flatEventSize(const VstEvent* e) {
    if (e->type == kVstSysExType) {
        const VstMidiSysexEvent* s = (const VstMidiSysexEvent*)e;
        int32_t n = s->dumpBytes > 0 && s->sysexDump ? s->dumpBytes : 0;
        return 4 + 20 + (size_t)n;
    }
    return 4 + 32;
}

// Writes as many events as fit; returns bytes used.
inline size_t flattenEvents(const VstEvents* ev, uint8_t* dst, size_t cap) {
    size_t used = 0;
    if (!ev) return 0;
    for (int32_t i = 0; i < ev->numEvents; i++) {
        const VstEvent* e = ev->events[i];
        if (!e) continue;
        size_t need = flatEventSize(e);
        if (used + need > cap) break;
        uint32_t rec = (uint32_t)(need - 4);
        memcpy(dst + used, &rec, 4);
        if (e->type == kVstSysExType) {
            const VstMidiSysexEvent* s = (const VstMidiSysexEvent*)e;
            int32_t hdr[5] = {s->type, 0, s->deltaFrames, s->flags, (int32_t)(rec - 20)};
            memcpy(dst + used + 4, hdr, 20);
            if (rec > 20) memcpy(dst + used + 24, s->sysexDump, rec - 20);
        } else {
            memcpy(dst + used + 4, e, 32);
        }
        used += need;
    }
    return used;
}

inline size_t flatEventsSize(const VstEvents* ev) {
    size_t n = 0;
    if (!ev) return 0;
    for (int32_t i = 0; i < ev->numEvents; i++)
        if (ev->events[i]) n += flatEventSize(ev->events[i]);
    return n;
}

// Rebuilds a native VstEvents block inside 'storage', which must outlive the plug-in's use of it.
inline VstEvents* unflattenEvents(const uint8_t* src, size_t n, std::vector<uint8_t>& storage) {
    size_t count = 0, body = 0, pos = 0;
    while (pos + 4 <= n) {
        uint32_t rec;
        memcpy(&rec, src + pos, 4);
        if (rec < 20 || pos + 4 + rec > n) break;
        int32_t type;
        memcpy(&type, src + pos + 4, 4);
        if (type == kVstSysExType)
            body += sizeof(VstMidiSysexEvent) + (rec - 20);
        else
            body += 32;
        body = (body + 7) & ~(size_t)7;
        count++;
        pos += 4 + rec;
    }
    size_t head = offsetof(VstEvents, events) + (count < 2 ? 2 : count) * sizeof(VstEvent*);
    head = (head + 7) & ~(size_t)7;
    if (storage.size() < head + body + 8) storage.resize(head + body + 8);
    VstEvents* out = (VstEvents*)storage.data();
    out->numEvents = (int32_t)count;
    out->reserved = 0;
    out->events[0] = out->events[1] = nullptr;
    uint8_t* p = storage.data() + head;
    pos = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t rec;
        memcpy(&rec, src + pos, 4);
        int32_t type;
        memcpy(&type, src + pos + 4, 4);
        out->events[i] = (VstEvent*)p;
        size_t sz;
        if (type == kVstSysExType) {
            int32_t hdr[5];
            memcpy(hdr, src + pos + 4, 20);
            VstMidiSysexEvent* s = (VstMidiSysexEvent*)p;
            memset(s, 0, sizeof(*s));
            s->type = kVstSysExType;
            s->byteSize = (int32_t)sizeof(VstMidiSysexEvent);
            s->deltaFrames = hdr[2];
            s->flags = hdr[3];
            s->dumpBytes = (int32_t)(rec - 20);
            s->sysexDump = (char*)(p + sizeof(VstMidiSysexEvent));
            memcpy(s->sysexDump, src + pos + 24, rec - 20);
            sz = sizeof(VstMidiSysexEvent) + (rec - 20);
        } else {
            memset(p, 0, 32);
            memcpy(p, src + pos + 4, rec < 32 ? rec : 32);
            sz = 32;
        }
        p += (sz + 7) & ~(size_t)7;
        pos += 4 + rec;
    }
    return out;
}

// ---- logging -------------------------------------------------------------------------------

struct Log {
    wchar_t path[1024] = L"";
    bool verbose = false;
    const char* who = "?";
    void write(bool always, const char* fmt, va_list ap) {
        if (!always && !verbose) return;
        if (!path[0]) return;
        char line[1500];
        SYSTEMTIME st;
        GetLocalTime(&st);
        int n = _snprintf(line, 200, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s pid %lu tid %lu] ", st.wYear, st.wMonth, st.wDay, st.wHour,
                          st.wMinute, st.wSecond, st.wMilliseconds, who, GetCurrentProcessId(), GetCurrentThreadId());
        int m = _vsnprintf(line + n, sizeof(line) - n - 3, fmt, ap);
        if (m < 0) m = (int)sizeof(line) - n - 3;
        n += m;
        line[n++] = '\r';
        line[n++] = '\n';
        HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        DWORD put;
        WriteFile(f, line, (DWORD)n, &put, nullptr);
        CloseHandle(f);
    }
    void error(const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        write(true, fmt, ap);
        va_end(ap);
    }
    void trace(const char* fmt, ...) {
        if (!verbose) return;
        va_list ap;
        va_start(ap, fmt);
        write(false, fmt, ap);
        va_end(ap);
    }
};

} // namespace ipc
