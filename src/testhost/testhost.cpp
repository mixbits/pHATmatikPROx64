// Minimal command-line VST2 host used to validate the bridge.
// Builds for x86 (loads the original plug-in directly) and x64 (loads the bridge), so the two
// can be compared.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <tlhelp32.h>
#include <gdiplus.h>
#include <mmsystem.h>
#include <ole2.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <set>
#include <atomic>
#include <algorithm>
#include "../common/vst2.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "advapi32.lib")

static bool g_standalone = false; // started as the windowed program rather than as a test

static double g_sampleRate = 44100.0;
static double g_tempo = 120.0;
static int g_blockSize = 512;
static std::atomic<long long> g_samplePos{0};
static std::atomic<int> g_cbCount[64];
static bool g_verboseCb = false;
static DWORD g_mainThread = 0;
static AEffect* g_effect = nullptr;
static std::atomic<int> g_automateCount{0};
static std::atomic<DWORD> g_lastAutomateThread{0};
static std::atomic<int> g_midiOutCount{0};
static HWND g_hostWnd = nullptr;
static DWORD g_hostStyle = 0;
static int g_extraHeight = 0; // strip below the editor used as a drag source
static std::atomic<int> g_sizeWindowCalls{0};
static int g_sizeW = 0, g_sizeH = 0;
static bool g_refuseSizeWindow = false;
static uint64_t g_midiOutHash = 1469598103934665603ull;
static std::atomic<int> g_lastAutomateIndex{-1};
static std::atomic<int> g_beginEdits{0}, g_endEdits{0}, g_editOffMain{0};
static std::atomic<float> g_lastAutomateValue{0.0f};
static uint64_t fnv(const void* data, size_t n, uint64_t h);

static void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static intptr_t __cdecl hostCallback(AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt) {
    static thread_local VstTimeInfo ti;
    if (opcode >= 0 && opcode < 64) {
        int n = ++g_cbCount[opcode];
        if (g_verboseCb && (n <= 3))
            logf("  [cb] op=%d index=%d value=%lld opt=%g thread=%s effect=%s\n", opcode, index, (long long)value, opt,
                 GetCurrentThreadId() == g_mainThread ? "main" : "other", effect ? "set" : "null");
    }
    switch (opcode) {
    case audioMasterAutomate:
        ++g_automateCount;
        g_lastAutomateThread = GetCurrentThreadId();
        g_lastAutomateIndex = index;
        g_lastAutomateValue = opt;
        return 0;
    case audioMasterVersion: return 2400;
    case audioMasterCurrentId: return 0;
    case audioMasterIdle: return 0;
    case audioMasterWantMidi_deprecated: return 1;
    case audioMasterGetTime: {
        memset(&ti, 0, sizeof(ti));
        ti.samplePos = (double)g_samplePos.load();
        ti.sampleRate = g_sampleRate;
        ti.tempo = g_tempo;
        ti.ppqPos = ti.samplePos / g_sampleRate * g_tempo / 60.0;
        ti.barStartPos = floor(ti.ppqPos / 4.0) * 4.0;
        ti.timeSigNumerator = 4;
        ti.timeSigDenominator = 4;
        ti.flags = kVstTransportPlaying | kVstPpqPosValid | kVstTempoValid | kVstBarsValid | kVstTimeSigValid;
        return (intptr_t)&ti;
    }
    case audioMasterProcessEvents: {
        VstEvents* ev = (VstEvents*)ptr;
        if (ev) {
            g_midiOutCount += ev->numEvents;
            for (int i = 0; i < ev->numEvents; i++) {
                const VstEvent* x = ev->events[i];
                g_midiOutHash = fnv(&x->type, 4, g_midiOutHash);
                g_midiOutHash = fnv(&x->deltaFrames, 4, g_midiOutHash);
                if (x->type == kVstMidiType) {
                    g_midiOutHash = fnv(((const VstMidiEvent*)x)->midiData, 4, g_midiOutHash);
                } else if (x->type == kVstSysExType) {
                    const VstMidiSysexEvent* sx = (const VstMidiSysexEvent*)x;
                    g_midiOutHash = fnv(&sx->dumpBytes, 4, g_midiOutHash);
                    g_midiOutHash = fnv(sx->sysexDump, (size_t)sx->dumpBytes, g_midiOutHash);
                }
            }
        }
        return 1;
    }
    case audioMasterIOChanged: return 1;
    case audioMasterSizeWindow: {
        ++g_sizeWindowCalls;
        g_sizeW = index;
        g_sizeH = (int)value;
        if (g_refuseSizeWindow || !g_hostWnd) return 0;
        RECT r{0, 0, index, (int)value + g_extraHeight};
        AdjustWindowRect(&r, g_hostStyle, FALSE);
        SetWindowPos(g_hostWnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        return 1;
    }
    case audioMasterGetSampleRate: return (intptr_t)g_sampleRate;
    case audioMasterGetBlockSize: return g_blockSize;
    case audioMasterGetInputLatency: return 0;
    case audioMasterGetOutputLatency: return 0;
    case audioMasterGetCurrentProcessLevel: return GetCurrentThreadId() == g_mainThread ? 1 : 2;
    case audioMasterGetAutomationState: return 1;
    case audioMasterGetVendorString:
        strcpy((char*)ptr, "BridgeTest");
        return 1;
    case audioMasterGetProductString:
        strcpy((char*)ptr, "TestHost");
        return 1;
    case audioMasterGetVendorVersion: return 1000;
    case audioMasterCanDo: {
        const char* s = (const char*)ptr;
        if (g_verboseCb) logf("  [cb] canDo(\"%s\")\n", s ? s : "(null)");
        if (!s) return 0;
        if (!strcmp(s, "sendVstEvents") || !strcmp(s, "sendVstMidiEvent") || !strcmp(s, "sendVstTimeInfo") ||
            !strcmp(s, "receiveVstEvents") || !strcmp(s, "receiveVstMidiEvent") || !strcmp(s, "sizeWindow"))
            return 1;
        return 0;
    }
    case audioMasterGetLanguage: return 1;
    case audioMasterGetDirectory: return 0;
    case audioMasterUpdateDisplay: return 1;
    case audioMasterBeginEdit:
        ++g_beginEdits;
        if (GetCurrentThreadId() != g_mainThread) ++g_editOffMain;
        return 1;
    case audioMasterEndEdit:
        ++g_endEdits;
        if (GetCurrentThreadId() != g_mainThread) ++g_editOffMain;
        return 1;
    default: return 0;
    }
}

static uint64_t fnv(const void* data, size_t n, uint64_t h) {
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// ---- window inspection -------------------------------------------------------------------

static std::set<DWORD> processTree() {
    std::set<DWORD> pids;
    pids.insert(GetCurrentProcessId());
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    bool grew = true;
    while (grew) {
        grew = false;
        PROCESSENTRY32W pe{sizeof(pe)};
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pids.count(pe.th32ParentProcessID) && !pids.count(pe.th32ProcessID)) {
                    // conhost and the like are not interesting but harmless.
                    pids.insert(pe.th32ProcessID);
                    grew = true;
                }
            } while (Process32NextW(snap, &pe));
        }
    }
    CloseHandle(snap);
    return pids;
}

struct DumpCtx {
    std::set<DWORD> pids;
    std::vector<HWND> tops;
};

static BOOL CALLBACK enumChild(HWND h, LPARAM) {
    char cls[128] = "", txt[400] = "";
    GetClassNameA(h, cls, sizeof(cls));
    SendMessageTimeoutA(h, WM_GETTEXT, sizeof(txt), (LPARAM)txt, SMTO_ABORTIFHUNG, 300, nullptr);
    RECT r{};
    GetWindowRect(h, &r);
    if (txt[0] || !strcmp(cls, "Button") || !strcmp(cls, "Edit"))
        logf("      child %p class=%s id=%d vis=%d text=\"%s\"\n", (void*)h, cls, GetDlgCtrlID(h), IsWindowVisible(h), txt);
    return TRUE;
}

static BOOL CALLBACK enumTop(HWND h, LPARAM lp) {
    DumpCtx* c = (DumpCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (c->pids.count(pid) && IsWindowVisible(h)) c->tops.push_back(h);
    return TRUE;
}

static void dumpWindows(const char* why) {
    DumpCtx c;
    c.pids = processTree();
    EnumWindows(enumTop, (LPARAM)&c);
    logf("  [windows:%s] %d visible top-level window(s) in process tree\n", why, (int)c.tops.size());
    for (HWND h : c.tops) {
        char cls[128] = "", txt[400] = "";
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        GetClassNameA(h, cls, sizeof(cls));
        SendMessageTimeoutA(h, WM_GETTEXT, sizeof(txt), (LPARAM)txt, SMTO_ABORTIFHUNG, 300, nullptr);
        RECT r{};
        GetWindowRect(h, &r);
        logf("    top %p pid=%lu%s class=%s title=\"%s\" rect=%ld,%ld %ldx%ld\n", (void*)h, pid,
             pid == GetCurrentProcessId() ? "(self)" : "(child process)", cls, txt, r.left, r.top, r.right - r.left, r.bottom - r.top);
        EnumChildWindows(h, enumChild, 0);
    }
}

static int pngEncoder(CLSID* clsid) {
    UINT n = 0, sz = 0;
    Gdiplus::GetImageEncodersSize(&n, &sz);
    if (!sz) return -1;
    std::vector<char> buf(sz);
    Gdiplus::ImageCodecInfo* info = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(n, sz, info);
    for (UINT i = 0; i < n; i++)
        if (!wcscmp(info[i].MimeType, L"image/png")) {
            *clsid = info[i].Clsid;
            return (int)i;
        }
    return -1;
}

// Saves the client area of a window. With fromScreen the pixels are read back from the screen (the
// window must be on top), which is what the user sees; otherwise the window is asked to render.
// Returns the number of distinct colours found, as a crude "something was drawn" measure.
static int captureWindow(HWND h, const wchar_t* path, bool fromScreen) {
    RECT rc;
    GetClientRect(h, &rc);
    int w = rc.right, hgt = rc.bottom;
    if (w <= 0 || hgt <= 0) return -1;
    HDC wdc = GetDC(h);
    HDC mdc = CreateCompatibleDC(wdc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -hgt;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mdc, bmp);
    if (fromScreen) {
        POINT o{0, 0};
        ClientToScreen(h, &o);
        HDC sdc = GetDC(nullptr);
        BitBlt(mdc, 0, 0, w, hgt, sdc, o.x, o.y, SRCCOPY);
        ReleaseDC(nullptr, sdc);
    } else {
        PrintWindow(h, mdc, 1 | 2); // PW_CLIENTONLY | PW_RENDERFULLCONTENT
    }
    GdiFlush();
    uint32_t* px = (uint32_t*)bits;
    size_t total = (size_t)w * hgt;
    std::set<uint32_t> colours;
    for (size_t i = 0; i < total; i += 7) colours.insert(px[i] & 0xFFFFFF);
    if (path) {
        CLSID png;
        if (pngEncoder(&png) >= 0) {
            Gdiplus::Bitmap b(bmp, nullptr);
            b.Save(path, &png, nullptr);
        }
    }
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(h, wdc);
    return (int)colours.size();
}

// Presses a button, by caption, on any visible dialog in the process tree. Used only to dismiss
// the plug-in's own start-up prompts during unattended tests.
struct ClickCtx {
    std::set<DWORD> pids;
    const char* caption;
    bool clicked;
};
static BOOL CALLBACK clickChild(HWND h, LPARAM lp) {
    ClickCtx* c = (ClickCtx*)lp;
    char cls[64] = "", txt[200] = "";
    GetClassNameA(h, cls, sizeof(cls));
    if (_stricmp(cls, "Button")) return TRUE;
    SendMessageTimeoutA(h, WM_GETTEXT, sizeof(txt), (LPARAM)txt, SMTO_ABORTIFHUNG, 300, nullptr);
    std::string t(txt);
    t.erase(std::remove(t.begin(), t.end(), '&'), t.end());
    if (!_stricmp(t.c_str(), c->caption) && IsWindowVisible(h) && IsWindowEnabled(h)) {
        PostMessageA(h, BM_CLICK, 0, 0);
        c->clicked = true;
        return FALSE;
    }
    return TRUE;
}
static BOOL CALLBACK clickTop(HWND h, LPARAM lp) {
    ClickCtx* c = (ClickCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (c->pids.count(pid) && IsWindowVisible(h)) EnumChildWindows(h, clickChild, lp);
    return !c->clicked;
}
static bool clickButton(const char* caption) {
    ClickCtx c{processTree(), caption, false};
    EnumWindows(clickTop, (LPARAM)&c);
    return c.clicked;
}

// ---- options -------------------------------------------------------------------------------

struct Options {
    std::wstring plugin;
    int blocks = 200;
    int editorSeconds = 0;
    std::wstring shot;
    std::wstring wav;
    int timeout = 60;
    int dumpAfter = 0;
    std::string click;
    bool listParams = false;
    bool audioThread = false;
    bool dpiAware = false;
    bool topmost = false;
    bool accumulate = false;
    bool automate = false;
    bool sysex = false;
    int clickX = -1, clickY = -1, clickParam = -1;
    bool killHelper = false;
    int dragX = -1, dragY = -1;
    std::wstring loadFile;
    std::wstring dropFile;
    int dropX = 320, dropY = 130;
    int realClickX = -1, realClickY = -1;
    bool noSizeWindow = false;
    bool interactive = false; // keep the editor open until the user closes the window
    bool liveAudio = false;   // play the plug-in's output on the default sound device
    std::wstring saveChunk, loadChunk;
    int loadX = -1, loadY = -1;
    int postBlocks = 0;
    bool sweep = false;        // every parameter at several values, with audio
    bool programs = false;     // walk through the programs
    int editorCycles = 0;      // open and close the editor this many times
    bool dropTarget = false;   // the host window accepts drops, like a DAW's arrange page
    std::wstring dropSave;     // prefix for copies of the files dropped on the host
    bool abandon = false;
    int instances = 0;
};

static Options g_opt;
static std::atomic<bool> g_done{false};

static DWORD WINAPI watchdog(LPVOID) {
    DWORD start = GetTickCount();
    bool dumped = false;
    DWORD lastClick = 0;
    while (!g_done) {
        Sleep(200);
        DWORD el = (GetTickCount() - start) / 1000;
        if (g_opt.dumpAfter && !dumped && el >= (DWORD)g_opt.dumpAfter) {
            dumped = true;
            dumpWindows("watchdog");
        }
        if (!g_opt.click.empty() && GetTickCount() - lastClick > 1000) {
            // Comma-separated list of captions; the first one found is pressed.
            size_t pos = 0;
            while (pos <= g_opt.click.size()) {
                size_t comma = g_opt.click.find(',', pos);
                if (comma == std::string::npos) comma = g_opt.click.size();
                std::string cap = g_opt.click.substr(pos, comma - pos);
                pos = comma + 1;
                if (cap.empty() || !clickButton(cap.c_str())) continue;
                dumpWindows("before-click");
                logf("  [watchdog] pressed \"%s\"\n", cap.c_str());
                lastClick = GetTickCount();
                break;
            }
        }
        if (el >= (DWORD)g_opt.timeout) {
            logf("TIMEOUT after %d s - the host thread is blocked\n", g_opt.timeout);
            dumpWindows("timeout");
            fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 99);
        }
    }
    return 0;
}

// ---- audio -----------------------------------------------------------------------------------

struct EventBlock {
    VstEvents hdr;
    VstMidiEvent ev[2];
};

static void sendSysex(AEffect* e, int seed) {
    static thread_local struct {
        VstEvents hdr;
        VstMidiSysexEvent sx;
        VstMidiEvent note;
        char dump[40];
    } b;
    memset(&b, 0, sizeof(b));
    for (int i = 0; i < 40; i++) b.dump[i] = (char)((seed * 7 + i * 3) & 0x7F);
    b.dump[0] = (char)0xF0;
    b.dump[39] = (char)0xF7;
    b.sx.type = kVstSysExType;
    b.sx.byteSize = sizeof(VstMidiSysexEvent);
    b.sx.deltaFrames = 11;
    b.sx.dumpBytes = 40;
    b.sx.sysexDump = b.dump;
    b.note.type = kVstMidiType;
    b.note.byteSize = sizeof(VstMidiEvent);
    b.note.deltaFrames = 13;
    b.note.midiData[0] = (char)0xB0;
    b.note.midiData[1] = 7;
    b.note.midiData[2] = (char)(seed & 0x7F);
    b.hdr.numEvents = 2;
    b.hdr.events[0] = (VstEvent*)&b.sx;
    b.hdr.events[1] = (VstEvent*)&b.note;
    e->dispatcher(e, effProcessEvents, 0, 0, &b.hdr, 0);
}

static void sendNote(AEffect* e, int note, int vel, int delta) {
    static thread_local EventBlock eb;
    memset(&eb, 0, sizeof(eb));
    eb.hdr.numEvents = 1;
    eb.hdr.events[0] = (VstEvent*)&eb.ev[0];
    eb.ev[0].type = kVstMidiType;
    eb.ev[0].byteSize = sizeof(VstMidiEvent);
    eb.ev[0].deltaFrames = delta;
    eb.ev[0].midiData[0] = (char)(vel ? 0x90 : 0x80);
    eb.ev[0].midiData[1] = (char)note;
    eb.ev[0].midiData[2] = (char)vel;
    e->dispatcher(e, effProcessEvents, 0, 0, &eb.hdr, 0);
}

struct Render {
    uint64_t hash = 1469598103934665603ull;
    double peak = 0;
    long long nonzero = 0;
    std::vector<float> interleaved;
};

static int g_noteSpan = 24;
static int g_noteBase = 36;
static long long g_noteClock = 0;
static void renderBlocks(AEffect* e, int blocks, Render& r, bool keep) {
    int nin = e->numInputs, nout = e->numOutputs;
    std::vector<std::vector<float>> in(nin, std::vector<float>(g_blockSize)), out(nout, std::vector<float>(g_blockSize));
    std::vector<float*> inp(nin + 1), outp(nout + 1);
    for (int c = 0; c < nin; c++) inp[c] = in[c].data();
    for (int c = 0; c < nout; c++) outp[c] = out[c].data();
    for (int b = 0; b < blocks; b++) {
        // A deterministic input signal, for effects.
        for (int c = 0; c < nin; c++)
            for (int i = 0; i < g_blockSize; i++) in[c][i] = 0.25f * (float)sin(0.05 * (double)(g_samplePos + i) * (c + 1));
        // Notes follow a clock that runs across calls, so a test made of many short renders
        // still walks through the whole note range.
        long long nb = g_noteClock++;
        if (nb % 40 == 2) sendNote(e, g_noteBase + (int)((nb / 40) % g_noteSpan), 100, 7);
        if (nb % 40 == 30) sendNote(e, g_noteBase + (int)((nb / 40) % g_noteSpan), 0, 3);
        if (g_opt.sysex && b % 40 == 17) sendSysex(e, b);
        if (g_opt.automate && e->numParams > 1 && b % 25 == 10) {
            // Parameter changes made from the processing thread, as host automation does.
            e->setParameter(e, 1, (float)(b % 7) / 7.0f);
            float back = e->getParameter(e, 1);
            r.hash = fnv(&back, sizeof(back), r.hash);
        }
        // Accumulating mode starts from a known non-zero buffer so that the addition is visible.
        for (int c = 0; c < nout; c++) std::fill(out[c].begin(), out[c].end(), g_opt.accumulate ? 0.125f : 0.0f);
        if (g_opt.accumulate)
            e->process(e, inp.data(), outp.data(), g_blockSize);
        else if (e->flags & effFlagsCanReplacing)
            e->processReplacing(e, inp.data(), outp.data(), g_blockSize);
        else
            e->process(e, inp.data(), outp.data(), g_blockSize);
        for (int c = 0; c < nout; c++) {
            r.hash = fnv(out[c].data(), g_blockSize * sizeof(float), r.hash);
            for (int i = 0; i < g_blockSize; i++) {
                double a = fabs(out[c][i]);
                if (a > r.peak) r.peak = a;
                if (out[c][i] != 0.0f) r.nonzero++;
            }
        }
        if (keep && nout >= 1)
            for (int i = 0; i < g_blockSize; i++) {
                r.interleaved.push_back(out[0][i]);
                r.interleaved.push_back(out[nout > 1 ? 1 : 0][i]);
            }
        g_samplePos += g_blockSize;
    }
}

static void writeWav(const wchar_t* path, const std::vector<float>& s) {
    FILE* f = _wfopen(path, L"wb");
    if (!f) return;
    uint32_t dataBytes = (uint32_t)(s.size() * 4), u32;
    uint16_t u16;
    fwrite("RIFF", 1, 4, f);
    u32 = 36 + dataBytes; fwrite(&u32, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    u32 = 16; fwrite(&u32, 4, 1, f);
    u16 = 3; fwrite(&u16, 2, 1, f); // IEEE float
    u16 = 2; fwrite(&u16, 2, 1, f);
    u32 = (uint32_t)g_sampleRate; fwrite(&u32, 4, 1, f);
    u32 = (uint32_t)g_sampleRate * 8; fwrite(&u32, 4, 1, f);
    u16 = 8; fwrite(&u16, 2, 1, f);
    u16 = 32; fwrite(&u16, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&dataBytes, 4, 1, f);
    fwrite(s.data(), 4, s.size(), f);
    fclose(f);
}

static std::atomic<bool> g_audioRun{false};
static std::atomic<long long> g_audioBlocks{0};
// Plays outputs 1 and 2 of the plug-in on the default sound device, in real time.
static void liveAudioLoop(AEffect* e) {
    const int kBuffers = 4, kFrames = g_blockSize;
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = (DWORD)g_sampleRate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * 4;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HWAVEOUT wo = nullptr;
    if (waveOutOpen(&wo, WAVE_MAPPER, &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        logf("  live audio: no sound device could be opened; processing without sound\n");
        wo = nullptr;
    }
    std::vector<std::vector<short>> pcm(kBuffers, std::vector<short>(kFrames * 2));
    std::vector<WAVEHDR> hdr(kBuffers);
    for (int i = 0; i < kBuffers; i++) {
        memset(&hdr[i], 0, sizeof(WAVEHDR));
        hdr[i].lpData = (LPSTR)pcm[i].data();
        hdr[i].dwBufferLength = kFrames * 4;
        hdr[i].dwFlags = 0;
        if (wo) {
            waveOutPrepareHeader(wo, &hdr[i], sizeof(WAVEHDR));
            hdr[i].dwFlags |= WHDR_DONE;
        }
    }
    int nin = e->numInputs, nout = e->numOutputs;
    std::vector<std::vector<float>> in(nin, std::vector<float>(kFrames, 0.0f)), out(nout, std::vector<float>(kFrames));
    std::vector<float*> inp(nin + 1), outp(nout + 1);
    for (int c = 0; c < nin; c++) inp[c] = in[c].data();
    for (int c = 0; c < nout; c++) outp[c] = out[c].data();
    double peak = 0;
    long long blocks = 0, clipped = 0;
    int next = 0;
    while (g_audioRun) {
        if (wo && !(hdr[next].dwFlags & WHDR_DONE)) {
            WaitForSingleObject(ev, 50);
            continue;
        }
        for (int c = 0; c < nout; c++) std::fill(out[c].begin(), out[c].end(), 0.0f);
        e->processReplacing(e, inp.data(), outp.data(), kFrames);
        g_samplePos += kFrames;
        blocks++;
        const float* l = nout > 0 ? out[0].data() : nullptr;
        const float* r = nout > 1 ? out[1].data() : l;
        for (int i = 0; i < kFrames; i++) {
            float a = l ? l[i] : 0.0f, b = r ? r[i] : 0.0f;
            if (fabs(a) > peak) peak = fabs(a);
            if (fabs(b) > peak) peak = fabs(b);
            if (a > 1.0f || a < -1.0f || b > 1.0f || b < -1.0f) clipped++;
            a = a > 1.0f ? 1.0f : (a < -1.0f ? -1.0f : a);
            b = b > 1.0f ? 1.0f : (b < -1.0f ? -1.0f : b);
            pcm[next][i * 2] = (short)(a * 32767.0f);
            pcm[next][i * 2 + 1] = (short)(b * 32767.0f);
        }
        if (wo) {
            hdr[next].dwFlags &= ~WHDR_DONE;
            waveOutWrite(wo, &hdr[next], sizeof(WAVEHDR));
        } else {
            Sleep((DWORD)(1000.0 * kFrames / g_sampleRate));
        }
        next = (next + 1) % kBuffers;
    }
    if (wo) {
        waveOutReset(wo);
        for (int i = 0; i < kBuffers; i++) waveOutUnprepareHeader(wo, &hdr[i], sizeof(WAVEHDR));
        waveOutClose(wo);
    }
    CloseHandle(ev);
    g_audioBlocks += blocks;
    logf("  live audio: %lld blocks played (%.1f s), loudest sample %.4f%s\n", blocks, blocks * kFrames / g_sampleRate, peak,
         clipped ? ", some samples clipped" : "");
}

static DWORD WINAPI audioThreadProc(LPVOID p) {
    AEffect* e = (AEffect*)p;
    if (g_opt.liveAudio) {
        liveAudioLoop(e);
        return 0;
    }
    Render r;
    while (g_audioRun) {
        renderBlocks(e, 4, r, false);
        g_audioBlocks += 4;
        Sleep(1);
    }
    logf("  audio thread: %lld blocks, peak %.6f, nonzero samples %lld\n", g_audioBlocks.load(), r.peak, r.nonzero);
    return 0;
}

struct DragJob {
    HWND child;
    int x, y;
};
static DWORD WINAPI dragThread(LPVOID p) {
    DragJob* j = (DragJob*)p;
    POINT pt{j->x, j->y}, saved;
    ClientToScreen(j->child, &pt);
    GetCursorPos(&saved);
    INPUT down{}, up{};
    down.type = up.type = INPUT_MOUSE;
    down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SetCursorPos(pt.x, pt.y);
    Sleep(40);
    SendInput(1, &down, sizeof(INPUT));
    for (int i = 1; i <= 6; i++) {
        Sleep(40);
        SetCursorPos(pt.x, pt.y - i * 3);
    }
    Sleep(60);
    SendInput(1, &up, sizeof(INPUT));
    Sleep(20);
    SetCursorPos(saved.x, saved.y);
    return 0;
}

static DWORD WINAPI realClickThread(LPVOID p) {
    DragJob* j = (DragJob*)p;
    POINT pt{j->x, j->y}, saved;
    ClientToScreen(j->child, &pt);
    GetCursorPos(&saved);
    INPUT down{}, up{};
    down.type = up.type = INPUT_MOUSE;
    down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SetCursorPos(pt.x, pt.y);
    Sleep(60);
    SendInput(1, &down, sizeof(INPUT));
    Sleep(60);
    SendInput(1, &up, sizeof(INPUT));
    Sleep(60);
    SetCursorPos(saved.x, saved.y);
    return 0;
}

// The plug-in's "open file" dialog: a visible dialog in the process tree that has an edit box.
struct FileDlgCtx {
    std::set<DWORD> pids;
    HWND dlg;
    HWND edit;
};
static BOOL CALLBACK fileDlgChild(HWND h, LPARAM lp) {
    FileDlgCtx* c = (FileDlgCtx*)lp;
    char cls[64] = "";
    GetClassNameA(h, cls, sizeof(cls));
    if (!_stricmp(cls, "Edit") && IsWindowVisible(h)) {
        c->edit = h;
        return FALSE;
    }
    return TRUE;
}
static BOOL CALLBACK fileDlgTop(HWND h, LPARAM lp) {
    FileDlgCtx* c = (FileDlgCtx*)lp;
    DWORD pid = 0;
    char cls[64] = "";
    GetWindowThreadProcessId(h, &pid);
    GetClassNameA(h, cls, sizeof(cls));
    if (!c->pids.count(pid) || !IsWindowVisible(h) || strcmp(cls, "#32770")) return TRUE;
    c->edit = nullptr;
    EnumChildWindows(h, fileDlgChild, lp);
    if (c->edit) {
        c->dlg = h;
        return FALSE;
    }
    return TRUE;
}
static bool findFileDialog(HWND* dlg, HWND* edit) {
    FileDlgCtx c{processTree(), nullptr, nullptr};
    EnumWindows(fileDlgTop, (LPARAM)&c);
    if (dlg) *dlg = c.dlg;
    if (edit) *edit = c.edit;
    return c.dlg != nullptr;
}

struct DropSource : IDropSource {
    LONG rc = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&rc); }
    ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&rc); }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL esc, DWORD keys) override {
        if (esc) return DRAGDROP_S_CANCEL;
        if (!(keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

static bool g_dropStarted = false;
// Runs a real OLE drag, like dragging a file out of a host's browser. Returns when it is dropped.
static void startFileDrag() {
    g_dropStarted = true;
    IShellItem* item = nullptr;
    IDataObject* data = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(g_opt.dropFile.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (SUCCEEDED(hr)) hr = item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
    if (FAILED(hr) || !data) {
        logf("dropfile: could not create a data object for the file (%08lX)\n", (unsigned long)hr);
        return;
    }
    DropSource src;
    DWORD effect = 0, t0 = GetTickCount();
    hr = DoDragDrop(data, &src, DROPEFFECT_COPY | DROPEFFECT_LINK | DROPEFFECT_MOVE, &effect);
    logf("dropfile: drag ended after %lu ms: %s, drop effect=%lu (%s)\n", GetTickCount() - t0,
         hr == DRAGDROP_S_DROP ? "dropped" : hr == DRAGDROP_S_CANCEL ? "cancelled" : "error", effect,
         effect ? "accepted by the plug-in" : "NOT accepted");
    data->Release();
    item->Release();
}

struct DropJob {
    HWND host, child;
    int fromX, fromY, toX, toY;
};
static DWORD WINAPI dropMouseThread(LPVOID p) {
    DropJob* j = (DropJob*)p;
    POINT from{j->fromX, j->fromY}, to{j->toX, j->toY}, saved;
    ClientToScreen(j->host, &from);
    ClientToScreen(j->child ? j->child : j->host, &to);
    GetCursorPos(&saved);
    INPUT down{}, up{};
    down.type = up.type = INPUT_MOUSE;
    down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SetCursorPos(from.x, from.y);
    Sleep(80);
    SendInput(1, &down, sizeof(INPUT));
    Sleep(250);
    for (int i = 1; i <= 12; i++) {
        SetCursorPos(from.x + (to.x - from.x) * i / 12, from.y + (to.y - from.y) * i / 12);
        Sleep(40);
    }
    SetCursorPos(to.x + 2, to.y + 1);
    Sleep(80);
    SetCursorPos(to.x, to.y);
    Sleep(350);
    SendInput(1, &up, sizeof(INPUT));
    Sleep(150);
    SetCursorPos(saved.x, saved.y);
    return 0;
}

struct Action {
    DWORD at;          // milliseconds after the editor opened
    int type;          // 0 click, 1 double click, 2 screen capture, 3 mouse wheel, 4 title bar capture,
                       // 5 drag, 6 drag with Alt held, 7 click with Ctrl held, 8 answer a file dialog,
                       // 9 press a dialog button, 10 list the windows
    int x, y, n;
    int x2, y2;
    std::wstring path;
    bool done;
};
static std::vector<Action> g_actions;

// "1500:click:18,13;2600:dbl:665,55;4000:shot:C:\\dir\\file.png"
static void parseActions(const std::wstring& spec) {
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t end = spec.find(L';', pos);
        if (end == std::wstring::npos) end = spec.size();
        std::wstring item = spec.substr(pos, end - pos);
        pos = end + 1;
        size_t c1 = item.find(L':'), c2 = item.find(L':', c1 + 1);
        if (c1 == std::wstring::npos || c2 == std::wstring::npos) continue;
        Action a{};
        a.at = (DWORD)_wtoi(item.substr(0, c1).c_str());
        std::wstring type = item.substr(c1 + 1, c2 - c1 - 1), arg = item.substr(c2 + 1);
        if (type == L"shot" || type == L"frame") {
            a.type = type == L"frame" ? 4 : 2;
            a.path = arg;
        } else if (type == L"filedlg" || type == L"button" || type == L"dump") {
            a.type = type == L"filedlg" ? 8 : type == L"button" ? 9 : 10;
            a.path = arg;
        } else if (type == L"drag" || type == L"altdrag") {
            a.type = type == L"altdrag" ? 6 : 5;
            swscanf(arg.c_str(), L"%d,%d,%d,%d", &a.x, &a.y, &a.x2, &a.y2);
        } else if (type == L"ctrlclick") {
            a.type = 7;
            swscanf(arg.c_str(), L"%d,%d", &a.x, &a.y);
        } else if (type == L"wheel") {
            a.type = 3;
            swscanf(arg.c_str(), L"%d,%d,%d", &a.x, &a.y, &a.n);
        } else {
            a.type = type == L"dbl" ? 1 : 0;
            swscanf(arg.c_str(), L"%d,%d", &a.x, &a.y);
        }
        g_actions.push_back(a);
    }
}

struct ClickJob {
    HWND child;
    int x, y, count;
    int wheel; // notches to turn instead of clicking; negative scrolls down
    int key = 0;      // virtual key held down meanwhile (VK_MENU, VK_CONTROL), or 0
    bool drag = false; // press at x,y, move to x2,y2, release
    int x2 = 0, y2 = 0;
};

static void sendKey(int vk, bool down) {
    INPUT k{};
    k.type = INPUT_KEYBOARD;
    k.ki.wVk = (WORD)vk;
    k.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &k, sizeof(INPUT));
}
static DWORD WINAPI clicksThread(LPVOID p) {
    ClickJob* j = (ClickJob*)p;
    POINT pt{j->x, j->y}, saved;
    ClientToScreen(j->child, &pt);
    GetCursorPos(&saved);
    INPUT down{}, up{};
    down.type = up.type = INPUT_MOUSE;
    down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    // Arrive, settle, then press: the plug-in's lists track the pointer and act on the row it
    // last saw the pointer over, so a click that follows a jump too quickly lands on the old row.
    SetCursorPos(pt.x - 6, pt.y - 3);
    Sleep(120);
    SetCursorPos(pt.x - 2, pt.y - 1);
    Sleep(120);
    SetCursorPos(pt.x, pt.y);
    Sleep(300);
    if (j->key) {
        sendKey(j->key, true);
        Sleep(120);
    }
    if (j->drag) {
        POINT to{j->x2, j->y2};
        ClientToScreen(j->child, &to);
        SendInput(1, &down, sizeof(INPUT));
        Sleep(200);
        for (int i = 1; i <= 14; i++) {
            SetCursorPos(pt.x + (to.x - pt.x) * i / 14, pt.y + (to.y - pt.y) * i / 14);
            Sleep(45);
        }
        SetCursorPos(to.x + 2, to.y + 1);
        Sleep(90);
        SetCursorPos(to.x, to.y);
        Sleep(400);
        SendInput(1, &up, sizeof(INPUT));
        Sleep(150);
    }
    for (int i = 0; i < j->count; i++) {
        SendInput(1, &down, sizeof(INPUT));
        Sleep(60);
        SendInput(1, &up, sizeof(INPUT));
        Sleep(90);
    }
    if (j->key) {
        Sleep(80);
        sendKey(j->key, false);
    }
    for (int i = 0; i < abs(j->wheel); i++) {
        INPUT wheel{};
        wheel.type = INPUT_MOUSE;
        wheel.mi.dwFlags = MOUSEEVENTF_WHEEL;
        wheel.mi.mouseData = (DWORD)(j->wheel < 0 ? -WHEEL_DELTA : WHEEL_DELTA);
        SendInput(1, &wheel, sizeof(INPUT));
        Sleep(80);
    }
    Sleep(250);
    SetCursorPos(saved.x, saved.y);
    delete j;
    return 0;
}

// Answers a system file dialog opened by the plug-in: waits for it, types the path, presses
// Save (or Open), confirms an overwrite. Runs on its own thread because the dialog is modal on
// the host's UI thread when the plug-in is loaded directly.
static DWORD WINAPI fileDlgThread(LPVOID p) {
    std::wstring* path = (std::wstring*)p;
    HWND dlg = nullptr, edit = nullptr;
    DWORD t0 = GetTickCount();
    while (!findFileDialog(&dlg, &edit)) {
        if (GetTickCount() - t0 > 9000) {
            logf("filedlg: NO file dialog appeared\n");
            dumpWindows("filedlg");
            delete path;
            return 0;
        }
        Sleep(100);
    }
    Sleep(700); // let it finish opening
    bool entered = false;
    if (findFileDialog(&dlg, &edit)) {
        char title[200] = "";
        DWORD dpid = 0;
        GetWindowTextA(dlg, title, sizeof(title));
        GetWindowThreadProcessId(dlg, &dpid);
        // The dialog may still be filling in its own default name; keep at it until the box
        // really holds our path, and only then press the button. A wrong name here would save
        // the file somewhere unintended.
        for (int attempt = 0; attempt < 12 && !entered; attempt++) {
            SendMessageTimeoutW(edit, WM_SETTEXT, 0, (LPARAM)path->c_str(), SMTO_ABORTIFHUNG, 2000, nullptr);
            Sleep(350);
            wchar_t now[1200] = L"";
            SendMessageTimeoutW(edit, WM_GETTEXT, 1200, (LPARAM)now, SMTO_ABORTIFHUNG, 2000, nullptr);
            entered = *path == now;
        }
        logf("filedlg: \"%s\" dialog found in %s; path %s\n", title, dpid == GetCurrentProcessId() ? "this process" : "the helper process",
             entered ? "entered and confirmed" : "could NOT be entered");
        if (entered) {
            if (!clickButton("Save")) clickButton("Open");
            if (!path->empty() && path->back() == L'\\') {
                // A folder was given: the first press only made the dialog go there. Press again
                // to save under the name the dialog proposes. (pHATmatik's "Save as pHATfile"
                // ignores a typed name anyway and always uses the loop's name in that folder.)
                Sleep(1200);
                if (findFileDialog(&dlg, &edit)) {
                    wchar_t name[600] = L"";
                    SendMessageTimeoutW(edit, WM_GETTEXT, 600, (LPARAM)name, SMTO_ABORTIFHUNG, 2000, nullptr);
                    logf("filedlg: now in the folder; saving as \"%ls\"\n", name);
                    clickButton("Save");
                }
            }
        } else {
            clickButton("Cancel");
        }
    }
    t0 = GetTickCount();
    while (findFileDialog(nullptr, nullptr)) {
        if (GetTickCount() - t0 > 9000) {
            // Never answers an overwrite question: tests delete their target files beforehand.
            logf("filedlg: the dialog did NOT close\n");
            dumpWindows("filedlg");
            clickButton("No");
            clickButton("Cancel");
            delete path;
            return 0;
        }
        Sleep(250);
    }
    logf("filedlg: dialog closed\n");
    delete path;
    return 0;
}

static std::vector<char> readFile(const std::wstring& path) {
    std::vector<char> data;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
        fclose(f);
    }
    return data;
}

static std::atomic<int> g_dropsReceived{0};
// Accepts whatever is dragged onto the host window and reports it, the way a DAW's arrange
// page accepts MIDI and audio dragged out of the plug-in.
struct HostDropTarget : IDropTarget {
    LONG rc = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&rc); }
    ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&rc); }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* d, DWORD keys, POINTL, DWORD* effect) override {
        // Everything a host can learn about the drag before the drop: the formats listed, the
        // answers to the questions a host typically asks, and the effects the source allows.
        std::string formats;
        IEnumFORMATETC* en = nullptr;
        HRESULT enumResult = d ? d->EnumFormatEtc(DATADIR_GET, &en) : E_POINTER;
        if (SUCCEEDED(enumResult) && en) {
            FORMATETC fe;
            ULONG fetched = 0;
            while (en->Next(1, &fe, &fetched) == S_OK && fetched == 1) {
                char name[100] = "";
                if (fe.cfFormat == CF_HDROP) strcpy(name, "CF_HDROP");
                else if (!GetClipboardFormatNameA(fe.cfFormat, name, sizeof(name))) sprintf(name, "#%d", (int)fe.cfFormat);
                char item[160];
                sprintf(item, "%s(tymed %lu) ", name, fe.tymed);
                formats += item;
                if (fe.ptd) CoTaskMemFree(fe.ptd);
            }
            en->Release();
        }
        std::string queries;
        if (d) {
            struct { const char* label; CLIPFORMAT cf; } ask[] = {
                {"CF_HDROP", CF_HDROP},
                {"FileNameW", (CLIPFORMAT)RegisterClipboardFormatA("FileNameW")},
                {"FileName", (CLIPFORMAT)RegisterClipboardFormatA("FileName")},
                {"Shell IDList Array", (CLIPFORMAT)RegisterClipboardFormatA("Shell IDList Array")},
                {"FileGroupDescriptorW", (CLIPFORMAT)RegisterClipboardFormatA("FileGroupDescriptorW")},
                {"CF_TEXT", CF_TEXT},
            };
            for (auto& a : ask) {
                FORMATETC fe{a.cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
                char item[80];
                sprintf(item, "%s=%s ", a.label, d->QueryGetData(&fe) == S_OK ? "yes" : "no");
                queries += item;
            }
        }
        logf("droptarget: a drag entered the host window on the %s thread; formats offered: %s\n",
             GetCurrentThreadId() == g_mainThread ? "main" : "WRONG", formats.c_str());
        logf("droptarget:   format list %s; asked directly: %s; effects allowed by the source: 0x%lX; keys 0x%lX\n",
             SUCCEEDED(enumResult) ? "available" : "NOT available", queries.c_str(), *effect, keys);
        *effect = DROPEFFECT_COPY;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        *effect = DROPEFFECT_COPY;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        logf("droptarget: the drag left the host window without a drop\n");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* d, DWORD, POINTL, DWORD* effect) override {
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM sm{};
        if (d && SUCCEEDED(d->GetData(&fe, &sm))) {
            HDROP h = (HDROP)sm.hGlobal;
            UINT n = DragQueryFileW(h, 0xFFFFFFFF, nullptr, 0);
            logf("droptarget: dropped on the host window: %u file(s)\n", n);
            for (UINT i = 0; i < n; i++) {
                wchar_t path[1024] = L"";
                DragQueryFileW(h, i, path, 1024);
                std::vector<char> data = readFile(path);
                const wchar_t* base = wcsrchr(path, L'\\');
                const wchar_t* ext = wcsrchr(path, L'.');
                logf("droptarget:   file %u: \"%ls\" %d bytes hash=%016llX starts with \"%.4s\"\n", i, base ? base + 1 : path, (int)data.size(),
                     (unsigned long long)(data.empty() ? 0 : fnv(data.data(), data.size(), 1469598103934665603ull)), data.size() >= 4 ? data.data() : "");
                if (data.empty())
                    logf("droptarget:     nothing readable at \"%ls\" (file %s)\n", path, GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES ? "does not exist" : "exists but is empty");
                if (!g_opt.dropSave.empty()) {
                    wchar_t dest[1200];
                    swprintf(dest, 1200, L"%ls_%u%ls", g_opt.dropSave.c_str(), i, ext ? ext : L"");
                    CopyFileW(path, dest, FALSE);
                }
            }
            ReleaseStgMedium(&sm);
        } else {
            logf("droptarget: something was dropped, but it offered no files\n");
        }
        ++g_dropsReceived;
        *effect = DROPEFFECT_COPY;
        return S_OK;
    }
};

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_LBUTTONDOWN && !g_opt.dropFile.empty() && !g_dropStarted) {
        startFileDrag();
        return 0;
    }
    if (m == WM_CLOSE) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static std::string str(AEffect* e, int op, int index = 0) {
    char buf[1024];
    memset(buf, 0, sizeof(buf));
    e->dispatcher(e, op, index, 0, buf, 0);
    buf[255] = 0;
    return buf;
}

// Ends every bridge helper process started by this process. Returns how many were ended.
static int killHelpers() {
    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof(pe)};
    if (snap != INVALID_HANDLE_VALUE && Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ParentProcessID == GetCurrentProcessId() && !_wcsicmp(pe.szExeFile, L"pHATmatikBridgeHost32.exe")) {
                if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID)) {
                    TerminateProcess(h, 7);
                    CloseHandle(h);
                    n++;
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    return n;
}

struct InstanceJob {
    AEffect* e;
    Render r;
    int blocks;
};
static DWORD WINAPI instanceThread(LPVOID p) {
    // Each instance is rendered on its own thread, all at the same time.
    InstanceJob* j = (InstanceJob*)p;
    AEffect* e = j->e;
    int nin = e->numInputs, nout = e->numOutputs, bs = 256;
    std::vector<std::vector<float>> in(nin, std::vector<float>(bs)), out(nout, std::vector<float>(bs));
    std::vector<float*> inp(nin + 1), outp(nout + 1);
    for (int c = 0; c < nin; c++) inp[c] = in[c].data();
    for (int c = 0; c < nout; c++) outp[c] = out[c].data();
    for (int b = 0; b < j->blocks; b++) {
        for (int c = 0; c < nin; c++)
            for (int i = 0; i < bs; i++) in[c][i] = 0.25f * (float)sin(0.05 * (double)(b * bs + i) * (c + 1));
        if (b % 40 == 2) sendNote(e, 60, 100, 7);
        if (b % 40 == 30) sendNote(e, 60, 0, 3);
        e->processReplacing(e, inp.data(), outp.data(), bs);
        for (int c = 0; c < nout; c++) {
            j->r.hash = fnv(out[c].data(), bs * sizeof(float), j->r.hash);
            for (int i = 0; i < bs; i++)
                if (fabs(out[c][i]) > j->r.peak) j->r.peak = fabs(out[c][i]);
        }
    }
    return 0;
}

static int runInstances(VstPluginMainProc entry) {
    std::vector<InstanceJob> jobs(g_opt.instances);
    for (auto& j : jobs) {
        j.e = entry(hostCallback);
        if (!j.e) {
            logf("instance failed to load\n");
            return 3;
        }
        j.blocks = g_opt.blocks;
        j.e->dispatcher(j.e, effOpen, 0, 0, nullptr, 0);
        j.e->dispatcher(j.e, effSetSampleRate, 0, 0, nullptr, (float)g_sampleRate);
        j.e->dispatcher(j.e, effSetBlockSize, 0, 256, nullptr, 0);
        if (!g_opt.loadChunk.empty()) {
            std::vector<char> state = readFile(g_opt.loadChunk);
            if (!state.empty()) j.e->dispatcher(j.e, effSetChunk, 0, (intptr_t)state.size(), state.data(), 0);
        }
        j.e->dispatcher(j.e, effMainsChanged, 0, 1, nullptr, 0);
    }
    std::vector<HANDLE> threads;
    for (auto& j : jobs) threads.push_back(CreateThread(nullptr, 0, instanceThread, &j, 0, nullptr));
    WaitForMultipleObjects((DWORD)threads.size(), threads.data(), TRUE, INFINITE);
    bool same = true;
    for (auto& j : jobs) {
        if (j.r.hash != jobs[0].r.hash) same = false;
        j.e->dispatcher(j.e, effMainsChanged, 0, 0, nullptr, 0);
        j.e->dispatcher(j.e, effClose, 0, 0, nullptr, 0);
    }
    logf("%d instances rendered %d blocks each at the same time: peak=%.6f hash=%016llX, %s\n", g_opt.instances, g_opt.blocks, jobs[0].r.peak,
         (unsigned long long)jobs[0].r.hash, same ? "all instances identical" : "INSTANCES DIFFER");
    return same ? 0 : 4;
}

static int run() {
    HMODULE mod = LoadLibraryW(g_opt.plugin.c_str());
    if (!mod) {
        logf("LoadLibrary failed: %lu\n", GetLastError());
        return 2;
    }
    VstPluginMainProc entry = (VstPluginMainProc)GetProcAddress(mod, "VSTPluginMain");
    if (!entry) entry = (VstPluginMainProc)GetProcAddress(mod, "main");
    if (!entry) {
        logf("no VST entry point\n");
        return 2;
    }
    if (g_opt.instances > 0) return runInstances(entry);
    DWORD t0 = GetTickCount();
    AEffect* e = entry(hostCallback);
    if (!e || e->magic != kEffectMagic) {
        logf("plug-in main returned %p - not a VST effect\n", (void*)e);
        if (g_standalone)
            MessageBoxW(nullptr, L"pHATmatik PRO could not be loaded.\n\nThe reason is written to pHATmatikBridge.log in your temporary folder (%TEMP%).",
                        L"pHATmatik PRO 64-bit", MB_OK | MB_ICONWARNING);
        return 3;
    }
    g_effect = e;
    logf("loaded in %lu ms: uniqueID=%08X ('%c%c%c%c') version=%d numParams=%d numPrograms=%d in=%d out=%d flags=0x%X initialDelay=%d\n",
         GetTickCount() - t0, e->uniqueID, (e->uniqueID >> 24) & 0xFF, (e->uniqueID >> 16) & 0xFF, (e->uniqueID >> 8) & 0xFF,
         e->uniqueID & 0xFF, e->version, e->numParams, e->numPrograms, e->numInputs, e->numOutputs, e->flags, e->initialDelay);

    t0 = GetTickCount();
    e->dispatcher(e, effOpen, 0, 0, nullptr, 0);
    logf("effOpen returned after %lu ms\n", GetTickCount() - t0);
    e->dispatcher(e, effSetSampleRate, 0, 0, nullptr, (float)g_sampleRate);
    e->dispatcher(e, effSetBlockSize, 0, g_blockSize, nullptr, 0);

    logf("name=\"%s\" vendor=\"%s\" product=\"%s\" vendorVersion=%lld vstVersion=%lld category=%lld\n",
         str(e, effGetEffectName).c_str(), str(e, effGetVendorString).c_str(), str(e, effGetProductString).c_str(),
         (long long)e->dispatcher(e, effGetVendorVersion, 0, 0, nullptr, 0), (long long)e->dispatcher(e, effGetVstVersion, 0, 0, nullptr, 0),
         (long long)e->dispatcher(e, effGetPlugCategory, 0, 0, nullptr, 0));
    for (const char* cd : {"receiveVstEvents", "receiveVstMidiEvent", "sendVstEvents", "sendVstMidiEvent", "receiveVstTimeInfo", "bypass"})
        logf("canDo(%s)=%lld ", cd, (long long)e->dispatcher(e, effCanDo, 0, 0, (void*)cd, 0));
    logf("\nprogram=%lld \"%s\"\n", (long long)e->dispatcher(e, effGetProgram, 0, 0, nullptr, 0), str(e, effGetProgramName).c_str());

    // Parameters: a hash over everything, plus an optional listing.
    uint64_t ph = 1469598103934665603ull;
    for (int i = 0; i < e->numParams; i++) {
        std::string n = str(e, effGetParamName, i), d = str(e, effGetParamDisplay, i), l = str(e, effGetParamLabel, i);
        float v = e->getParameter(e, i);
        ph = fnv(n.data(), n.size(), ph);
        ph = fnv(d.data(), d.size(), ph);
        ph = fnv(l.data(), l.size(), ph);
        ph = fnv(&v, sizeof(v), ph);
        if (g_opt.listParams || i < 6) logf("  param %3d %-24s = %.6f  \"%s\" %s\n", i, n.c_str(), v, d.c_str(), l.c_str());
    }
    logf("parameter table hash=%016llX\n", (unsigned long long)ph);

    // Parameter write/read round trip, restored afterwards. Not in a session the user drives.
    if (e->numParams > 0 && !g_opt.interactive) {
        float old = e->getParameter(e, 0);
        e->setParameter(e, 0, 0.25f);
        float got = e->getParameter(e, 0);
        std::string disp = str(e, effGetParamDisplay, 0);
        e->setParameter(e, 0, old);
        logf("setParameter(0, 0.25) -> getParameter=%.6f display=\"%s\"; restored to %.6f -> %.6f\n", got, disp.c_str(), old, e->getParameter(e, 0));
    }

    if (!g_opt.loadChunk.empty()) {
        // Restore a state saved by another run, as a host does when it reopens a project.
        std::vector<char> data;
        if (FILE* f = _wfopen(g_opt.loadChunk.c_str(), L"rb")) {
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
            fclose(f);
        }
        DWORD c0 = GetTickCount();
        intptr_t sr = data.empty() ? -1 : e->dispatcher(e, effSetChunk, 0, (intptr_t)data.size(), data.data(), 0);
        logf("loadchunk: %d bytes hash=%016llX, setChunk returned %lld\n", (int)data.size(),
             (unsigned long long)(data.empty() ? 0 : fnv(data.data(), data.size(), 1469598103934665603ull)), (long long)sr);
        (void)c0;
    }
    e->dispatcher(e, effMainsChanged, 0, 1, nullptr, 0);
    e->dispatcher(e, effStartProcess, 0, 0, nullptr, 0);

    Render r;
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    renderBlocks(e, g_opt.blocks, r, !g_opt.wav.empty() && g_opt.postBlocks == 0);
    QueryPerformanceCounter(&b);
    double us = (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)f.QuadPart;
    logf("render: %d blocks of %d, %.1f us/block, peak=%.6f nonzero=%lld hash=%016llX\n", g_opt.blocks, g_blockSize, us / g_opt.blocks, r.peak, r.nonzero,
         (unsigned long long)r.hash);
    if (!g_opt.wav.empty() && g_opt.postBlocks == 0) writeWav(g_opt.wav.c_str(), r.interleaved);

    if (g_opt.sweep) {
        // Every parameter at several values while notes play. The values read back, their
        // display strings and all the audio go into the hashes.
        Render sr;
        uint64_t vh = 1469598103934665603ull;
        for (int i = 0; i < e->numParams; i++) {
            float old = e->getParameter(e, i);
            for (float v : {0.0f, 0.3f, 0.7f, 1.0f}) {
                e->setParameter(e, i, v);
                float back = e->getParameter(e, i);
                std::string d = str(e, effGetParamDisplay, i);
                vh = fnv(&back, sizeof(back), vh);
                vh = fnv(d.data(), d.size(), vh);
                renderBlocks(e, 8, sr, false);
            }
            e->setParameter(e, i, old);
        }
        logf("sweep: %d parameters x 4 values: peak=%.6f nonzero=%lld audio hash=%016llX value hash=%016llX\n", e->numParams, sr.peak, sr.nonzero,
             (unsigned long long)sr.hash, (unsigned long long)vh);
    }
    if (g_opt.programs) {
        uint64_t ph = 1469598103934665603ull;
        intptr_t cur = e->dispatcher(e, effGetProgram, 0, 0, nullptr, 0);
        for (int p = 0; p < e->numPrograms; p++) {
            e->dispatcher(e, effSetProgram, 0, p, nullptr, 0);
            intptr_t got = e->dispatcher(e, effGetProgram, 0, 0, nullptr, 0);
            std::string n1 = str(e, effGetProgramName), n2 = str(e, effGetProgramNameIndexed, p);
            void* data = nullptr;
            intptr_t size = e->dispatcher(e, effGetChunk, 1, 0, &data, 0);
            ph = fnv(&got, sizeof(int32_t), ph);
            ph = fnv(n1.data(), n1.size(), ph);
            ph = fnv(n2.data(), n2.size(), ph);
            if (size > 0 && data) ph = fnv(data, (size_t)size, ph);
        }
        e->dispatcher(e, effSetProgram, 0, e->numPrograms > 1 ? 1 : 0, nullptr, 0);
        e->dispatcher(e, effSetProgramName, 0, 0, (void*)"Bridge test", 0);
        std::string renamed = str(e, effGetProgramName);
        e->dispatcher(e, effSetProgram, 0, cur, nullptr, 0);
        Render pr;
        renderBlocks(e, 80, pr, false);
        logf("programs: %d programs walked, hash=%016llX; renamed program reads back \"%s\"; back on program %lld: 80 blocks peak=%.6f hash=%016llX\n",
             e->numPrograms, (unsigned long long)ph, renamed.c_str(), (long long)e->dispatcher(e, effGetProgram, 0, 0, nullptr, 0), pr.peak,
             (unsigned long long)pr.hash);
    }
    if (g_opt.editorCycles > 0 && (e->flags & effFlagsHasEditor)) {
        // Open and close the editor over and over, as a user flipping between plug-in windows does.
        WNDCLASSW cw{};
        cw.lpfnWndProc = DefWindowProcW;
        cw.hInstance = GetModuleHandleW(nullptr);
        cw.lpszClassName = L"BridgeTestCycleWnd";
        RegisterClassW(&cw);
        HWND cwnd = CreateWindowW(cw.lpszClassName, L"pHATmatik PRO editor cycles", WS_OVERLAPPED | WS_CAPTION | WS_CLIPCHILDREN, 80, 80, 680, 520, nullptr, nullptr,
                                  cw.hInstance, nullptr);
        ShowWindow(cwnd, SW_SHOWNOACTIVATE);
        int opened = 0, closed = 0;
        DWORD c0 = GetTickCount(), slowest = 0;
        for (int i = 0; i < g_opt.editorCycles; i++) {
            DWORD o0 = GetTickCount();
            e->dispatcher(e, effEditOpen, 0, 0, cwnd, 0);
            if (GetWindow(cwnd, GW_CHILD)) opened++;
            for (int k = 0; k < 6; k++) {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
                e->dispatcher(e, effEditIdle, 0, 0, nullptr, 0);
                Sleep(10);
            }
            e->dispatcher(e, effEditClose, 0, 0, nullptr, 0);
            if (!GetWindow(cwnd, GW_CHILD)) closed++;
            DWORD d = GetTickCount() - o0;
            if (d > slowest) slowest = d;
        }
        Render cr;
        renderBlocks(e, 80, cr, false);
        logf("editor cycles: %d of %d opened, %d of %d closed cleanly; then 80 blocks peak=%.6f hash=%016llX\n", opened, g_opt.editorCycles, closed,
             g_opt.editorCycles, cr.peak, (unsigned long long)cr.hash);
        logf("  (cycle timing: %lu ms in total, slowest %lu ms)\n", GetTickCount() - c0, slowest);
        DestroyWindow(cwnd);
    }
    if (g_opt.killHelper) {
        // The helper process dies while the plug-in is in use. Nothing may hang or crash, and
        // the output must go silent.
        int killed = killHelpers();
        Render after;
        DWORD k0 = GetTickCount();
        renderBlocks(e, 50, after, false);
        float v = e->numParams ? e->getParameter(e, 0) : 0.0f;
        void* data = nullptr;
        intptr_t n = e->dispatcher(e, effGetChunk, 0, 0, &data, 0);
        std::string name = str(e, effGetEffectName);
        logf("killed %d helper(s): 50 more blocks peak=%.6f, getParameter=%.3f, getChunk=%lld, name=\"%s\", all within %lu ms\n", killed, after.peak, v,
             (long long)n, name.c_str(), GetTickCount() - k0);
        e->dispatcher(e, effMainsChanged, 0, 0, nullptr, 0);
        e->dispatcher(e, effClose, 0, 0, nullptr, 0);
        logf("effClose after helper death done\n");
        return 0;
    }
    if (g_opt.abandon) {
        // The host goes away without closing the plug-in, as when it crashes.
        logf("abandoning the plug-in without effClose\n");
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 0);
    }

    // Chunk round trip.
    if (g_opt.interactive) {
        // left alone: the user's session starts from whatever the plug-in starts with
    } else if (e->flags & effFlagsProgramChunks) {
        for (int isProgram = 0; isProgram <= 1; isProgram++) {
            void* data = nullptr;
            intptr_t n = e->dispatcher(e, effGetChunk, isProgram, 0, &data, 0);
            uint64_t h1 = (n > 0 && data) ? fnv(data, (size_t)n, 1469598103934665603ull) : 0;
            std::vector<char> copy;
            if (n > 0 && data) copy.assign((char*)data, (char*)data + n);
            logf("getChunk(%s): %lld bytes hash=%016llX\n", isProgram ? "program" : "bank", (long long)n, (unsigned long long)h1);
            if (!copy.empty()) {
                intptr_t sr = e->dispatcher(e, effSetChunk, isProgram, (intptr_t)copy.size(), copy.data(), 0);
                void* data2 = nullptr;
                intptr_t n2 = e->dispatcher(e, effGetChunk, isProgram, 0, &data2, 0);
                uint64_t h2 = (n2 > 0 && data2) ? fnv(data2, (size_t)n2, 1469598103934665603ull) : 0;
                logf("  setChunk returned %lld; getChunk again: %lld bytes hash=%016llX %s\n", (long long)sr, (long long)n2, (unsigned long long)h2,
                     (n2 == n && h2 == h1) ? "(identical)" : "(differs)");
            }
        }
    } else {
        logf("plug-in does not use chunks\n");
    }

    // Editor.
    if (g_opt.interactive) {
        g_opt.editorSeconds = 24 * 3600;
        g_opt.audioThread = true;
    }
    if (g_opt.editorSeconds > 0 && (e->flags & effFlagsHasEditor)) {
        ERect* rect = nullptr;
        e->dispatcher(e, effEditGetRect, 0, 0, &rect, 0);
        int w = rect ? rect->right - rect->left : 400, h = rect ? rect->bottom - rect->top : 300;
        logf("editor rect before open: %s %dx%d\n", rect ? "ok" : "null", w, h);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"BridgeTestHostWnd";
        wc.hbrBackground = (HBRUSH)GetStockObject(DKGRAY_BRUSH);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        // The icon embedded by testhost.rc: the large one is used for the taskbar button and
        // Alt+Tab, the small one for the title bar. Null (the Windows default) if there is none.
        wc.hIcon = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED);
        wc.hIconSm = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
        RegisterClassExW(&wc);
        logf("window icon: %s\n", wc.hIcon && wc.hIconSm ? "set from the embedded icon" : "none embedded");
        g_extraHeight = (g_opt.dropFile.empty() && !g_opt.dropTarget) ? 0 : 40;
        g_refuseSizeWindow = g_opt.noSizeWindow;
        RECT wr{0, 0, w, h + g_extraHeight};
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
        g_hostStyle = style;
        AdjustWindowRect(&wr, style, FALSE);
        HWND wnd = CreateWindowExW(g_opt.topmost ? WS_EX_TOPMOST : 0, wc.lpszClassName, sizeof(void*) == 8 ? L"pHATmatik PRO 64-bit" : L"pHATmatik PRO 32-bit", style, g_opt.interactive && !g_opt.topmost ? CW_USEDEFAULT : 60, g_opt.interactive && !g_opt.topmost ? CW_USEDEFAULT : 60, wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, wc.hInstance, nullptr);
        g_hostWnd = wnd;
        static HostDropTarget hostDropTarget;
        if (g_opt.dropTarget) {
            HRESULT hr = RegisterDragDrop(wnd, &hostDropTarget);
            logf("droptarget: host window registered as a drop target: %s\n", SUCCEEDED(hr) ? "ok" : "FAILED");
        }
        ShowWindow(wnd, (g_opt.topmost || g_opt.interactive) ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
        if (g_opt.interactive) SetForegroundWindow(wnd);
        UpdateWindow(wnd);
        t0 = GetTickCount();
        intptr_t er = e->dispatcher(e, effEditOpen, 0, 0, wnd, 0);
        logf("effEditOpen returned %lld after %lu ms\n", (long long)er, GetTickCount() - t0);
        rect = nullptr;
        e->dispatcher(e, effEditGetRect, 0, 0, &rect, 0);
        if (rect) {
            w = rect->right - rect->left;
            h = rect->bottom - rect->top;
            logf("editor rect after open: %dx%d\n", w, h);
            RECT wr2{0, 0, w, h + g_extraHeight};
            AdjustWindowRect(&wr2, style, FALSE);
            SetWindowPos(wnd, nullptr, 0, 0, wr2.right - wr2.left, wr2.bottom - wr2.top, SWP_NOMOVE | SWP_NOZORDER);
        }
        HANDLE at = nullptr;
        if (g_opt.audioThread) {
            g_audioRun = true;
            at = CreateThread(nullptr, 0, audioThreadProc, e, 0, nullptr);
        }
        DWORD start = GetTickCount(), lastIdle = 0;
        bool shot = false, early = false;
        int clickStage = 0;
        int loadStage = 0;
        int fdStage = 0;
        DWORD fdTick = 0;
        std::wstring fdPath;
        int dropStage = 0, rcStage = 0;
        int editorH = h;
        DWORD loadTick = 0;
        int idles = 0;
        DWORD maxIdleMs = 0;
        for (;;) {
            MSG msg;
            bool quit = false;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) quit = true;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (quit) {
                logf("editor: window closed by the user after %lu s\n", (GetTickCount() - start) / 1000);
                break;
            }
            DWORD now = GetTickCount();
            if (now - lastIdle >= 30) {
                lastIdle = now;
                DWORD i0 = GetTickCount();
                e->dispatcher(e, effEditIdle, 0, 0, nullptr, 0);
                DWORD d = GetTickCount() - i0;
                if (d > maxIdleMs) maxIdleMs = d;
                idles++;
            }
            DWORD el = now - start;
            for (Action& a : g_actions) {
                if (a.done || el < a.at) continue;
                a.done = true;
                if (a.type == 4) {
                    // The top of the whole window as it is on screen, title bar and icon included.
                    RECT wr{};
                    GetWindowRect(wnd, &wr);
                    int fw = wr.right - wr.left, fh = 70;
                    HDC sdc = GetDC(nullptr);
                    HDC mdc = CreateCompatibleDC(sdc);
                    HBITMAP bmp = CreateCompatibleBitmap(sdc, fw, fh);
                    HGDIOBJ old = SelectObject(mdc, bmp);
                    BitBlt(mdc, 0, 0, fw, fh, sdc, wr.left, wr.top, SRCCOPY);
                    SelectObject(mdc, old);
                    CLSID png;
                    if (pngEncoder(&png) >= 0) {
                        Gdiplus::Bitmap b(bmp, nullptr);
                        b.Save(a.path.c_str(), &png, nullptr);
                    }
                    DeleteObject(bmp);
                    DeleteDC(mdc);
                    ReleaseDC(nullptr, sdc);
                    wchar_t title[200] = L"";
                    GetWindowTextW(wnd, title, 200);
                    logf("action %lu ms: title bar captured; title=\"%ls\"; class icon %s, small icon %s\n", a.at, title,
                         GetClassLongPtrW(wnd, GCLP_HICON) ? "set" : "missing", GetClassLongPtrW(wnd, GCLP_HICONSM) ? "set" : "missing");
                } else if (a.type == 8) {
                    // Schedule this just before the click that opens the dialog: with the plug-in
                    // loaded directly, its dialog blocks this loop until it is closed.
                    logf("action %lu ms: watching for a file dialog\n", a.at);
                    CloseHandle(CreateThread(nullptr, 0, fileDlgThread, new std::wstring(a.path), 0, nullptr));
                } else if (a.type == 9) {
                    std::string cap;
                    for (wchar_t ch : a.path) cap.push_back((char)ch);
                    logf("action %lu ms: button \"%s\" %s\n", a.at, cap.c_str(), clickButton(cap.c_str()) ? "pressed" : "NOT FOUND");
                } else if (a.type == 10) {
                    dumpWindows("action");
                } else if (a.type == 5 || a.type == 6) {
                    logf("action %lu ms: %sdrag from %d,%d to %d,%d\n", a.at, a.type == 6 ? "Alt+" : "", a.x, a.y, a.x2, a.y2);
                    ClickJob* j = new ClickJob{GetWindow(wnd, GW_CHILD), a.x, a.y, 0, 0};
                    j->key = a.type == 6 ? VK_MENU : 0;
                    j->drag = true;
                    j->x2 = a.x2;
                    j->y2 = a.y2;
                    CloseHandle(CreateThread(nullptr, 0, clicksThread, j, 0, nullptr));
                } else if (a.type == 7) {
                    logf("action %lu ms: Ctrl+click at %d,%d\n", a.at, a.x, a.y);
                    ClickJob* j = new ClickJob{GetWindow(wnd, GW_CHILD), a.x, a.y, 1, 0};
                    j->key = VK_CONTROL;
                    CloseHandle(CreateThread(nullptr, 0, clicksThread, j, 0, nullptr));
                } else if (a.type == 2) {
                    int colours = captureWindow(wnd, a.path.c_str(), true);
                    RECT hc{};
                    GetClientRect(wnd, &hc);
                    logf("action %lu ms: screen capture, client %ldx%ld, %d colours\n", a.at, hc.right, hc.bottom, colours);
                } else {
                    logf("action %lu ms: %s at %d,%d\n", a.at, a.type == 3 ? "wheel" : a.type ? "double click" : "click", a.x, a.y);
                    CloseHandle(CreateThread(nullptr, 0, clicksThread, new ClickJob{GetWindow(wnd, GW_CHILD), a.x, a.y, a.type == 3 ? 0 : (a.type ? 2 : 1), a.type == 3 ? a.n : 0}, 0, nullptr));
                }
            }
            if (fdStage) {
                // Answer a system file dialog the plug-in has opened: type the path, press Save.
                HWND dlg = nullptr, edit = nullptr;
                bool present = findFileDialog(&dlg, &edit);
                if (fdStage == 1) {
                    if (present) {
                        char title[200] = "";
                        DWORD dpid = 0;
                        GetWindowTextA(dlg, title, sizeof(title));
                        GetWindowThreadProcessId(dlg, &dpid);
                        SendMessageTimeoutW(edit, WM_SETTEXT, 0, (LPARAM)fdPath.c_str(), SMTO_ABORTIFHUNG, 2000, nullptr);
                        logf("filedlg: \"%s\" dialog found in %s; path entered\n", title, dpid == GetCurrentProcessId() ? "this process" : "the helper process");
                        fdStage = 2;
                        fdTick = now;
                    } else if (now - fdTick > 7000) {
                        logf("filedlg: NO file dialog appeared\n");
                        dumpWindows("filedlg");
                        fdStage = 0;
                    }
                } else if (fdStage == 2 && now - fdTick > 500) {
                    if (!clickButton("Save")) clickButton("Open");
                    fdStage = 3;
                    fdTick = now;
                } else if (fdStage == 3 && now - fdTick > 400) {
                    if (!present) {
                        logf("filedlg: dialog closed %lu ms after pressing its button\n", now - fdTick);
                        fdStage = 0;
                    } else if (now - fdTick > 7000) {
                        logf("filedlg: the dialog did NOT close\n");
                        dumpWindows("filedlg");
                        fdStage = 0;
                    } else {
                        clickButton("Yes"); // "replace the existing file?"
                    }
                }
            }
            if (g_opt.realClickX >= 0) {
                // A real click on one of the plug-in's controls, then a look at the window sizes.
                static DragJob rcJob;
                if (rcStage == 0 && el >= 1500) {
                    rcJob = {GetWindow(wnd, GW_CHILD), g_opt.realClickX, g_opt.realClickY};
                    CloseHandle(CreateThread(nullptr, 0, realClickThread, &rcJob, 0, nullptr));
                    rcStage = 1;
                } else if (rcStage == 1 && el >= 2800) {
                    RECT hc{}, cc{};
                    HWND child = GetWindow(wnd, GW_CHILD);
                    GetClientRect(wnd, &hc);
                    if (child) GetClientRect(child, &cc);
                    ERect* er = nullptr;
                    e->dispatcher(e, effEditGetRect, 0, 0, &er, 0);
                    logf("realclick %d,%d: sizeWindow callbacks=%d (asked for %dx%d, host %s); host client %ldx%ld; plug-in window %ldx%ld; effEditGetRect %dx%d\n",
                         g_opt.realClickX, g_opt.realClickY, g_sizeWindowCalls.load(), g_sizeW, g_sizeH, g_refuseSizeWindow ? "refuses" : "resizes",
                         hc.right, hc.bottom, cc.right, cc.bottom, er ? er->right - er->left : -1, er ? er->bottom - er->top : -1);
                    rcStage = 2;
                }
            }
            if (!g_opt.dropFile.empty()) {
                static DropJob dj;
                if (dropStage == 0 && el >= (DWORD)(g_opt.realClickX >= 0 ? 3200 : 1500)) {
                    RECT hc{};
                    GetClientRect(wnd, &hc);
                    dj = {wnd, GetWindow(wnd, GW_CHILD), 60, hc.bottom - g_extraHeight / 2, g_opt.dropX, g_opt.dropY};
                    CloseHandle(CreateThread(nullptr, 0, dropMouseThread, &dj, 0, nullptr));
                    dropStage = 1;
                }
            }
            if (!g_opt.loadFile.empty() && g_opt.loadX >= 0) {
                // Load an audio file the way a user does: press the plug-in's open button, type
                // the path into its file dialog, press Open.
                static DragJob clickJob;
                HWND dlg = nullptr, edit = nullptr;
                if (loadStage == 0 && el >= 1500) {
                    clickJob = {GetWindow(wnd, GW_CHILD), g_opt.loadX, g_opt.loadY};
                    CloseHandle(CreateThread(nullptr, 0, realClickThread, &clickJob, 0, nullptr));
                    loadStage = 1;
                    loadTick = now;
                } else if (loadStage == 1 && now - loadTick > 800) {
                    if (findFileDialog(&dlg, &edit)) {
                        char title[200] = "";
                        DWORD dpid = 0;
                        GetWindowTextA(dlg, title, sizeof(title));
                        GetWindowThreadProcessId(dlg, &dpid);
                        SendMessageTimeoutW(edit, WM_SETTEXT, 0, (LPARAM)g_opt.loadFile.c_str(), SMTO_ABORTIFHUNG, 2000, nullptr);
                        logf("loadfile: file dialog \"%s\" found in %s; path entered\n", title,
                             dpid == GetCurrentProcessId() ? "this process" : "the helper process");
                        loadStage = 2;
                        loadTick = now;
                    } else if (now - loadTick > 6000) {
                        logf("loadfile: NO file dialog appeared after clicking %d,%d\n", g_opt.loadX, g_opt.loadY);
                        dumpWindows("loadfile");
                        loadStage = 9;
                    }
                } else if (loadStage == 2 && now - loadTick > 400) {
                    clickButton("Open");
                    loadStage = 3;
                    loadTick = now;
                } else if (loadStage == 3 && now - loadTick > 500) {
                    if (!findFileDialog(nullptr, nullptr)) {
                        logf("loadfile: file dialog closed %lu ms after pressing Open\n", now - loadTick);
                        loadStage = 4;
                    } else if (now - loadTick > 8000) {
                        logf("loadfile: the file dialog did NOT close\n");
                        dumpWindows("loadfile");
                        loadStage = 9;
                    }
                }
            }
            if (g_opt.dragX >= 0) {
                static DragJob job;
                auto report = [&](const char* when) {
                    int idx = g_lastAutomateIndex.load();
                    logf("dragtest %s: beginEdit=%d automate=%d endEdit=%d (edit callbacks off the main thread: %d, last automate on %s thread); "
                         "last automate index=%d value=%.4f; getParameter(%d)=%.4f\n",
                         when, g_beginEdits.load(), g_automateCount.load(), g_endEdits.load(), g_editOffMain.load(),
                         g_lastAutomateThread == 0 ? "no" : (g_lastAutomateThread == g_mainThread ? "the main" : "ANOTHER"), idx,
                         g_lastAutomateValue.load(), idx, idx >= 0 ? e->getParameter(e, idx) : 0.0f);
                };
                if (clickStage == 0 && el >= 1800) {
                    report("before");
                    job = {GetWindow(wnd, GW_CHILD), g_opt.dragX, g_opt.dragY};
                    CloseHandle(CreateThread(nullptr, 0, dragThread, &job, 0, nullptr));
                    clickStage = 1;
                } else if (clickStage == 1 && el >= 2800) {
                    report("after");
                    clickStage = 2;
                }
            }
            if (g_opt.clickX >= 0) {
                // A synthetic mouse click on a control of the plug-in's editor, then a second one
                // to put the control back.
                HWND child = GetWindow(wnd, GW_CHILD);
                LPARAM pos = MAKELPARAM(g_opt.clickX, g_opt.clickY);
                auto click = [&]() {
                    PostMessageW(child, WM_LBUTTONDOWN, MK_LBUTTON, pos);
                    PostMessageW(child, WM_LBUTTONUP, 0, pos);
                };
                auto report = [&](const char* when) {
                    logf("clicktest %s: automate callbacks=%d last index=%d value=%.3f on %s thread; getParameter(%d)=%.3f\n", when,
                         g_automateCount.load(), g_lastAutomateIndex.load(), g_lastAutomateValue.load(),
                         g_lastAutomateThread == 0 ? "no" : (g_lastAutomateThread == g_mainThread ? "the main" : "ANOTHER"), g_opt.clickParam,
                         g_opt.clickParam >= 0 ? e->getParameter(e, g_opt.clickParam) : 0.0f);
                };
                if (clickStage == 0 && el >= 1800 && child) { report("before"); click(); clickStage = 1; }
                else if (clickStage == 1 && el >= 2400) { report("after first click"); click(); clickStage = 2; }
                else if (clickStage == 2 && el >= 3000) { report("after second click"); clickStage = 3; }
            }
            if (!early && el >= 1500) {
                // What is on the screen shortly after opening, with no help from the test.
                early = true;
                std::wstring ep = g_opt.shot.empty() ? L"" : g_opt.shot + L".early.png";
                logf("editor: 1.5 s after open, distinct colours on screen=%d\n", captureWindow(wnd, ep.empty() ? nullptr : ep.c_str(), true));
            }
            if (!g_opt.interactive && !shot && el >= (DWORD)(g_opt.editorSeconds * 1000 - 700)) {
                shot = true;
                HWND child = GetWindow(wnd, GW_CHILD);
                DWORD cpid = 0;
                char ccls[128] = "";
                if (child) {
                    GetWindowThreadProcessId(child, &cpid);
                    GetClassNameA(child, ccls, sizeof(ccls));
                }
                logf("editor: host window iconic=%d visible=%d foreground=%d\n", IsIconic(wnd), IsWindowVisible(wnd), GetForegroundWindow() == wnd);
                std::wstring pw = g_opt.shot.empty() ? L"" : g_opt.shot + L".printwindow.png";
                int onScreen = captureWindow(wnd, g_opt.shot.empty() ? nullptr : g_opt.shot.c_str(), true);
                int rendered = captureWindow(wnd, pw.empty() ? nullptr : pw.c_str(), false);
                logf("editor: child window %s (class=%s, pid=%lu, %s); distinct colours on screen=%d, via PrintWindow=%d\n", child ? "present" : "ABSENT",
                     ccls, cpid, cpid == GetCurrentProcessId() ? "this process" : "other process", onScreen, rendered);
                dumpWindows("editor");
            }
            if (el >= (DWORD)(g_opt.editorSeconds * 1000)) break;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
        logf("editor loop: %d effEditIdle calls, slowest %lu ms; automate callbacks=%d (last on %s thread)\n", idles, maxIdleMs, g_automateCount.load(),
             g_lastAutomateThread == 0 ? "no" : (g_lastAutomateThread == g_mainThread ? "main" : "another"));
        if (at) {
            g_audioRun = false;
            WaitForSingleObject(at, 5000);
            CloseHandle(at);
        }
        e->dispatcher(e, effEditClose, 0, 0, nullptr, 0);
        logf("effEditClose done; child window now %s\n", GetWindow(wnd, GW_CHILD) ? "STILL PRESENT" : "gone");
        if (!g_opt.saveChunk.empty() && (e->flags & effFlagsProgramChunks)) {
            void* data = nullptr;
            intptr_t n = e->dispatcher(e, effGetChunk, 0, 0, &data, 0);
            if (n > 0 && data) {
                if (FILE* f = _wfopen(g_opt.saveChunk.c_str(), L"wb")) {
                    fwrite(data, 1, (size_t)n, f);
                    fclose(f);
                }
            }
            logf("savechunk: %lld bytes hash=%016llX\n", (long long)n,
                 (unsigned long long)((n > 0 && data) ? fnv(data, (size_t)n, 1469598103934665603ull) : 0));
        }
        if (g_opt.dropTarget) {
            RevokeDragDrop(wnd);
            logf("droptarget: %d drop(s) received by the host\n", g_dropsReceived.load());
        }
        DestroyWindow(wnd);
    }

    if (g_opt.postBlocks > 0) {
        Render pr;
        g_samplePos = 0;
        g_noteClock = 0;
        if (g_noteSpan == 24 && g_noteBase == 36) g_noteSpan = 48; // default: wide enough to reach the slices
        renderBlocks(e, g_opt.postBlocks, pr, !g_opt.wav.empty());
        logf("post-load render: %d blocks of %d, peak=%.6f nonzero=%lld hash=%016llX\n", g_opt.postBlocks, g_blockSize, pr.peak, pr.nonzero,
             (unsigned long long)pr.hash);
        if (!g_opt.wav.empty()) writeWav(g_opt.wav.c_str(), pr.interleaved);
        if (e->flags & effFlagsProgramChunks) {
            void* data = nullptr;
            intptr_t n = e->dispatcher(e, effGetChunk, 0, 0, &data, 0);
            logf("post-load chunk: %lld bytes hash=%016llX\n", (long long)n,
                 (unsigned long long)((n > 0 && data) ? fnv(data, (size_t)n, 1469598103934665603ull) : 0));
        }
    }
    e->dispatcher(e, effStopProcess, 0, 0, nullptr, 0);
    e->dispatcher(e, effMainsChanged, 0, 0, nullptr, 0);
    logf("callbacks seen:");
    for (int i = 0; i < 64; i++)
        if (g_cbCount[i]) logf(" op%d x%d", i, g_cbCount[i].load());
    logf("; midi out events=%d hash=%016llX\n", g_midiOutCount.load(), (unsigned long long)g_midiOutHash);
    e->dispatcher(e, effClose, 0, 0, nullptr, 0);
    logf("effClose done\n");
    FreeLibrary(mod);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--blocks") g_opt.blocks = _wtoi(next().c_str());
        else if (a == L"--editor") g_opt.editorSeconds = _wtoi(next().c_str());
        else if (a == L"--shot") g_opt.shot = next();
        else if (a == L"--wav") g_opt.wav = next();
        else if (a == L"--timeout") g_opt.timeout = _wtoi(next().c_str());
        else if (a == L"--dump") g_opt.dumpAfter = _wtoi(next().c_str());
        else if (a == L"--click") {
            for (wchar_t ch : next()) g_opt.click.push_back((char)ch);
        } else if (a == L"--params") g_opt.listParams = true;
        else if (a == L"--audiothread") g_opt.audioThread = true;
        else if (a == L"--dpiaware") g_opt.dpiAware = true;
        else if (a == L"--topmost") g_opt.topmost = true;
        else if (a == L"--killhelper") g_opt.killHelper = true;
        else if (a == L"--loadfile") g_opt.loadFile = next();
        else if (a == L"--dropfile") g_opt.dropFile = next();
        else if (a == L"--dropat") swscanf(next().c_str(), L"%d,%d", &g_opt.dropX, &g_opt.dropY);
        else if (a == L"--realclick") swscanf(next().c_str(), L"%d,%d", &g_opt.realClickX, &g_opt.realClickY);
        else if (a == L"--nosizewindow") g_opt.noSizeWindow = true;
        else if (a == L"--interactive") g_opt.interactive = true;
        else if (a == L"--actions") parseActions(next());
        else if (a == L"--sweep") g_opt.sweep = true;
        else if (a == L"--programs") g_opt.programs = true;
        else if (a == L"--editorcycles") g_opt.editorCycles = _wtoi(next().c_str());
        else if (a == L"--droptarget") g_opt.dropTarget = true;
        else if (a == L"--dropsave") g_opt.dropSave = next();
        else if (a == L"--samplerate") g_sampleRate = _wtof(next().c_str());
        else if (a == L"--tempo") g_tempo = _wtof(next().c_str());
        else if (a == L"--notespan") g_noteSpan = _wtoi(next().c_str());
        else if (a == L"--notebase") g_noteBase = _wtoi(next().c_str());
        else if (a == L"--audio") g_opt.liveAudio = true;
        else if (a == L"--savechunk") g_opt.saveChunk = next();
        else if (a == L"--loadchunk") g_opt.loadChunk = next();
        else if (a == L"--loadbutton") swscanf(next().c_str(), L"%d,%d", &g_opt.loadX, &g_opt.loadY);
        else if (a == L"--postblocks") g_opt.postBlocks = _wtoi(next().c_str());
        else if (a == L"--dragtest") swscanf(next().c_str(), L"%d,%d", &g_opt.dragX, &g_opt.dragY);
        else if (a == L"--abandon") g_opt.abandon = true;
        else if (a == L"--instances") g_opt.instances = _wtoi(next().c_str());
        else if (a == L"--accumulate") g_opt.accumulate = true;
        else if (a == L"--automate") g_opt.automate = true;
        else if (a == L"--sysex") g_opt.sysex = true;
        else if (a == L"--clicktest") swscanf(next().c_str(), L"%d,%d,%d", &g_opt.clickX, &g_opt.clickY, &g_opt.clickParam);
        else if (a == L"--verbose") g_verboseCb = true;
        else if (a == L"--blocksize") g_blockSize = _wtoi(next().c_str());
        else g_opt.plugin = a;
    }
#ifdef STANDALONE_APP
    // Built as a windowed program ("pHATmatik PRO 64-bit.exe"): started without a plug-in path it
    // opens the installed plug-in through the bridge, with sound, until its window is closed.
    if (g_opt.plugin.empty()) {
        wchar_t dir[1024] = L"";
        DWORD size = sizeof(dir);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\pHATmatik PRO Bridge", L"Vst64Dir", RRF_RT_REG_SZ, nullptr, dir, &size) != ERROR_SUCCESS) {
            size = sizeof(dir);
            dir[0] = 0;
            RegGetValueW(HKEY_CURRENT_USER, L"SOFTWARE\\pHATmatik PRO Bridge", L"Vst64Dir", RRF_RT_REG_SZ, nullptr, dir, &size);
        }
        if (!dir[0]) {
            MessageBoxW(nullptr, L"pHATmatik PRO and its 64-bit bridge are not installed on this computer.\n\nRun pHATmatik_PRO_v1_52_x64_Setup.exe first.",
                        L"pHATmatik PRO 64-bit", MB_OK | MB_ICONWARNING);
            return 1;
        }
        g_opt.plugin = std::wstring(dir) + L"\\iZotope pHATmatik PRO.dll";
        g_opt.interactive = true;
        g_opt.liveAudio = true;
        g_opt.blocks = 0;
        g_opt.timeout = 30 * 24 * 3600;
        g_standalone = true;
    }
#endif
    if (g_opt.plugin.empty()) {
        logf("usage: testhost <plugin.dll> [--blocks N] [--blocksize N] [--editor SEC] [--shot file.png] [--wav file.wav]\n"
             "                [--timeout SEC] [--dump SEC] [--click CAPTION] [--params] [--audiothread] [--verbose]\n");
        return 1;
    }
    g_mainThread = GetCurrentThreadId();
    if (g_opt.dpiAware) {
        typedef BOOL(WINAPI * SetCtx)(HANDLE);
        if (SetCtx f = (SetCtx)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")) f((HANDLE)(intptr_t)-4);
    }
    logf("testhost (%d-bit) loading %ls\n", (int)sizeof(void*) * 8, g_opt.plugin.c_str());
    OleInitialize(nullptr);
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gtok = 0;
    Gdiplus::GdiplusStartup(&gtok, &gsi, nullptr);
    CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr);
    int rc = run();
    g_done = true;
    Gdiplus::GdiplusShutdown(gtok);
    logf("exit code %d\n", rc);
    return rc;
}
