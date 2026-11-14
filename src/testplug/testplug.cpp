// A small deterministic VST2 plug-in used only to validate the bridge. It exercises what the
// real plug-in cannot be made to show without a licence: audio in and out, MIDI in and out,
// host time, parameters, chunks and callbacks on the audio thread.
//
// Built for x86. Running it directly in the 32-bit test host and through the bridge in the
// 64-bit test host must give bit-identical output, because the same x86 code does the arithmetic.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include "../common/vst2.h"

namespace {

const int kParams = 4;
const char* kParamNames[kParams] = {"Gain", "Detune", "Clicks", "Echo"};

struct Plug {
    AEffect fx{};
    AudioMasterCallback master = nullptr;
    float params[kParams] = {0.5f, 0.25f, 1.0f, 1.0f};
    double sampleRate = 44100.0;
    double phase = 0.0;
    double freq = 0.0;
    float level = 0.0f;
    int pendingNote = -1, pendingDelta = 0, pendingVel = 0;
    char chunk[64];
    char programName[32] = "Init";
    double lastBar = -1.0;
    long long processed = 0;
};

void setParam(AEffect* e, int32_t i, float v) {
    Plug* p = (Plug*)e->object;
    if (i >= 0 && i < kParams) p->params[i] = v;
}

float getParam(AEffect* e, int32_t i) {
    Plug* p = (Plug*)e->object;
    return (i >= 0 && i < kParams) ? p->params[i] : 0.0f;
}

void processReplacing(AEffect* e, float** in, float** out, int32_t n) {
    Plug* p = (Plug*)e->object;
    // Host time: put a click wherever a bar starts inside this block.
    int clickAt = -1;
    VstTimeInfo* ti = (VstTimeInfo*)p->master(e, audioMasterGetTime, 0, kVstPpqPosValid | kVstTempoValid | kVstBarsValid, nullptr, 0);
    double tempoScale = 1.0;
    if (ti && (ti->flags & kVstPpqPosValid) && (ti->flags & kVstTempoValid) && ti->sampleRate > 0) {
        double ppqPerSample = ti->tempo / 60.0 / ti->sampleRate;
        double nextBar = std::ceil(ti->ppqPos / 4.0) * 4.0;
        double samples = (nextBar - ti->ppqPos) / ppqPerSample;
        if (samples >= 0 && samples < n && nextBar != p->lastBar) {
            clickAt = (int)samples;
            p->lastBar = nextBar;
        }
        tempoScale = ti->tempo / 120.0;
    }
    // Asked on every block, like the real plug-in does.
    bool out1Connected = p->master(e, audioMasterPinConnected_deprecated, 1, 1, nullptr, 0) == 0;
    intptr_t level = p->master(e, audioMasterGetCurrentProcessLevel, 0, 0, nullptr, 0);
    float gain = p->params[0] * 2.0f;
    for (int i = 0; i < n; i++) {
        if (p->pendingNote >= 0 && i == p->pendingDelta) {
            if (p->pendingVel > 0) {
                p->freq = 440.0 * std::pow(2.0, (p->pendingNote - 69 + p->params[1]) / 12.0) * tempoScale;
                p->level = p->pendingVel / 127.0f;
            } else {
                p->level = 0.0f;
            }
            p->pendingNote = -1;
        }
        float s = (float)std::sin(p->phase) * p->level * 0.5f;
        p->phase += 6.283185307179586 * p->freq / p->sampleRate;
        if (p->phase > 6.283185307179586) p->phase -= 6.283185307179586;
        float click = (i == clickAt && p->params[2] > 0.5f) ? 0.9f : 0.0f;
        float l = in[0][i] * gain + s + click;
        float r = in[1][i] * gain - s + click + (level == 2 ? 0.001f : 0.0f);
        out[0][i] = l;
        if (out1Connected) out[1][i] = r;
    }
    p->processed += n;
}

void processAccumulating(AEffect* e, float** in, float** out, int32_t n) {
    static float tl[65536], tr[65536];
    float* tmp[2] = {tl, tr};
    if (n > 65536) n = 65536;
    memcpy(tr, out[1], n * sizeof(float));
    processReplacing(e, in, tmp, n);
    for (int i = 0; i < n; i++) {
        out[0][i] += tl[i];
        out[1][i] += tr[i];
    }
}

intptr_t dispatcher(AEffect* e, int32_t op, int32_t index, intptr_t value, void* ptr, float opt) {
    Plug* p = (Plug*)e->object;
    switch (op) {
    case effOpen: return 0;
    case effClose:
        delete p;
        return 0;
    case effSetSampleRate:
        p->sampleRate = opt;
        return 0;
    case effGetProgramName:
        strcpy((char*)ptr, p->programName);
        return 0;
    case effSetProgramName:
        strncpy(p->programName, (const char*)ptr, 31);
        return 0;
    case effGetParamName:
        if (index >= 0 && index < kParams) strcpy((char*)ptr, kParamNames[index]);
        return 0;
    case effGetParamDisplay:
        if (index >= 0 && index < kParams) sprintf((char*)ptr, "%.3f", p->params[index]);
        return 0;
    case effGetParamLabel:
        strcpy((char*)ptr, index == 0 ? "x" : "");
        return 0;
    case effGetEffectName:
        strcpy((char*)ptr, "BridgeTestPlug");
        return 1;
    case effGetVendorString:
        strcpy((char*)ptr, "Bridge validation");
        return 1;
    case effGetProductString:
        strcpy((char*)ptr, "BridgeTestPlug");
        return 1;
    case effGetVendorVersion: return 1234;
    case effGetVstVersion: return 2400;
    case effGetPlugCategory: return 2;
    case effCanDo:
        if (!strcmp((const char*)ptr, "receiveVstMidiEvent") || !strcmp((const char*)ptr, "sendVstMidiEvent") ||
            !strcmp((const char*)ptr, "receiveVstEvents") || !strcmp((const char*)ptr, "sendVstEvents"))
            return 1;
        return -1;
    case effGetChunk: {
        memset(p->chunk, 0, sizeof(p->chunk));
        memcpy(p->chunk, "TPLG", 4);
        memcpy(p->chunk + 4, p->params, sizeof(p->params));
        memcpy(p->chunk + 4 + sizeof(p->params), p->programName, 32);
        *(void**)ptr = p->chunk;
        return 4 + sizeof(p->params) + 32;
    }
    case effSetChunk:
        if (value >= (intptr_t)(4 + sizeof(p->params) + 32) && !memcmp(ptr, "TPLG", 4)) {
            memcpy(p->params, (char*)ptr + 4, sizeof(p->params));
            memcpy(p->programName, (char*)ptr + 4 + sizeof(p->params), 32);
            return 1;
        }
        return 0;
    case effGetOutputProperties:
    case effGetInputProperties:
        if (index < 2) {
            memset(ptr, 0, kSizeVstPinProperties);
            sprintf((char*)ptr, "%s %d", op == effGetInputProperties ? "In" : "Out", index + 1);
            ((int32_t*)ptr)[16] = 1 | 2; // active, stereo
            return 1;
        }
        return 0;
    case effProcessEvents: {
        VstEvents* ev = (VstEvents*)ptr;
        struct {
            VstEvents hdr;
            VstEvent* more[14];
        } outEv{};
        static VstMidiEvent echoes[16];
        static char sysexCopy[256];
        static VstMidiSysexEvent sysexOut;
        int nOut = 0;
        for (int i = 0; i < ev->numEvents && nOut < 15; i++) {
            if (ev->events[i]->type == kVstMidiType) {
                VstMidiEvent* m = (VstMidiEvent*)ev->events[i];
                int status = m->midiData[0] & 0xF0;
                if (status == 0x90 || status == 0x80) {
                    p->pendingNote = m->midiData[1];
                    p->pendingVel = status == 0x90 ? m->midiData[2] : 0;
                    p->pendingDelta = m->deltaFrames;
                }
                // Echo every event back a fifth higher.
                echoes[nOut] = *m;
                echoes[nOut].midiData[1] = (char)((m->midiData[1] + 7) & 0x7F);
                outEv.hdr.events[nOut] = (VstEvent*)&echoes[nOut];
                nOut++;
            } else if (ev->events[i]->type == kVstSysExType) {
                // Echo system exclusive data back reversed.
                VstMidiSysexEvent* sx = (VstMidiSysexEvent*)ev->events[i];
                int len = sx->dumpBytes > 256 ? 256 : sx->dumpBytes;
                for (int k = 0; k < len; k++) sysexCopy[k] = sx->sysexDump[len - 1 - k];
                memset(&sysexOut, 0, sizeof(sysexOut));
                sysexOut.type = kVstSysExType;
                sysexOut.byteSize = sizeof(sysexOut);
                sysexOut.deltaFrames = sx->deltaFrames;
                sysexOut.dumpBytes = len;
                sysexOut.sysexDump = sysexCopy;
                outEv.hdr.events[nOut++] = (VstEvent*)&sysexOut;
            }
        }
        if (nOut && p->params[3] > 0.5f) {
            outEv.hdr.numEvents = nOut;
            p->master(e, audioMasterProcessEvents, 0, 0, &outEv.hdr, 0);
        }
        return 1;
    }
    default: return 0;
    }
}

} // namespace

extern "C" __declspec(dllexport) AEffect* __cdecl VSTPluginMain(AudioMasterCallback master) {
    if (!master || master(nullptr, audioMasterVersion, 0, 0, nullptr, 0) == 0) return nullptr;
    Plug* p = new Plug();
    p->master = master;
    p->fx.magic = kEffectMagic;
    p->fx.dispatcher = dispatcher;
    p->fx.process = processAccumulating;
    p->fx.setParameter = setParam;
    p->fx.getParameter = getParam;
    p->fx.processReplacing = processReplacing;
    p->fx.numPrograms = 1;
    p->fx.numParams = kParams;
    p->fx.numInputs = 2;
    p->fx.numOutputs = 2;
    p->fx.flags = effFlagsCanReplacing | effFlagsProgramChunks;
    p->fx.uniqueID = 0x42545031; // 'BTP1'
    p->fx.version = 1;
    p->fx.object = p;
    p->fx.ioRatio = 1.0f;
    return &p->fx;
}
