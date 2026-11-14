// Minimal VST 2.4 binary interface, written from the documented ABI.
// Only what the bridge needs. Layouts are identical to the SDK's for both x86 and x64.
#pragma once
#include <cstdint>

#pragma pack(push, 8)

struct AEffect;

typedef intptr_t (__cdecl *AudioMasterCallback)(AEffect* effect, int32_t opcode, int32_t index,
                                                intptr_t value, void* ptr, float opt);
typedef intptr_t (__cdecl *AEffectDispatcherProc)(AEffect* effect, int32_t opcode, int32_t index,
                                                  intptr_t value, void* ptr, float opt);
typedef void (__cdecl *AEffectProcessProc)(AEffect* effect, float** inputs, float** outputs, int32_t sampleFrames);
typedef void (__cdecl *AEffectProcessDoubleProc)(AEffect* effect, double** inputs, double** outputs, int32_t sampleFrames);
typedef void (__cdecl *AEffectSetParameterProc)(AEffect* effect, int32_t index, float parameter);
typedef float (__cdecl *AEffectGetParameterProc)(AEffect* effect, int32_t index);
typedef AEffect* (__cdecl *VstPluginMainProc)(AudioMasterCallback audioMaster);

const int32_t kEffectMagic = 0x56737450; // 'VstP'

struct AEffect {
    int32_t magic;
    AEffectDispatcherProc dispatcher;
    AEffectProcessProc process; // deprecated accumulating process
    AEffectSetParameterProc setParameter;
    AEffectGetParameterProc getParameter;
    int32_t numPrograms;
    int32_t numParams;
    int32_t numInputs;
    int32_t numOutputs;
    int32_t flags;
    intptr_t resvd1;
    intptr_t resvd2;
    int32_t initialDelay;
    int32_t realQualities;
    int32_t offQualities;
    float ioRatio;
    void* object;
    void* user;
    int32_t uniqueID;
    int32_t version;
    AEffectProcessProc processReplacing;
    AEffectProcessDoubleProc processDoubleReplacing;
    char future[56];
};

enum {
    effFlagsHasEditor = 1 << 0,
    effFlagsCanReplacing = 1 << 4,
    effFlagsProgramChunks = 1 << 5,
    effFlagsIsSynth = 1 << 8,
    effFlagsNoSoundInStop = 1 << 9,
    effFlagsCanDoubleReplacing = 1 << 12,
};

// Host -> plug-in opcodes.
enum {
    effOpen = 0, effClose, effSetProgram, effGetProgram, effSetProgramName, effGetProgramName,
    effGetParamLabel, effGetParamDisplay, effGetParamName, effGetVu_deprecated,
    effSetSampleRate, effSetBlockSize, effMainsChanged, effEditGetRect, effEditOpen, effEditClose,
    effEditDraw_deprecated, effEditMouse_deprecated, effEditKey_deprecated, effEditIdle,
    effEditTop_deprecated, effEditSleep_deprecated, effIdentify_deprecated, effGetChunk, effSetChunk,
    effProcessEvents, effCanBeAutomated, effString2Parameter, effGetNumProgramCategories_deprecated,
    effGetProgramNameIndexed, effCopyProgram_deprecated, effConnectInput_deprecated,
    effConnectOutput_deprecated, effGetInputProperties, effGetOutputProperties, effGetPlugCategory,
    effGetCurrentPosition_deprecated, effGetDestinationBuffer_deprecated, effOfflineNotify,
    effOfflinePrepare, effOfflineRun, effProcessVarIo, effSetSpeakerArrangement,
    effSetBlockSizeAndSampleRate_deprecated, effSetBypass, effGetEffectName, effGetErrorText_deprecated,
    effGetVendorString, effGetProductString, effGetVendorVersion, effVendorSpecific, effCanDo,
    effGetTailSize, effIdle_deprecated, effGetIcon_deprecated, effSetViewPosition_deprecated,
    effGetParameterProperties, effKeysRequired_deprecated, effGetVstVersion, effEditKeyDown,
    effEditKeyUp, effSetEditKnobMode, effGetMidiProgramName, effGetCurrentMidiProgram,
    effGetMidiProgramCategory, effHasMidiProgramsChanged, effGetMidiKeyName, effBeginSetProgram,
    effEndSetProgram, effGetSpeakerArrangement, effShellGetNextPlugin, effStartProcess,
    effStopProcess, effSetTotalSampleToProcess, effSetPanLaw, effBeginLoadBank, effBeginLoadProgram,
    effSetProcessPrecision, effGetNumMidiInputChannels, effGetNumMidiOutputChannels,
    effNumOpcodes
};

// Plug-in -> host opcodes.
enum {
    audioMasterAutomate = 0, audioMasterVersion, audioMasterCurrentId, audioMasterIdle,
    audioMasterPinConnected_deprecated, audioMasterUnused5, audioMasterWantMidi_deprecated,
    audioMasterGetTime, audioMasterProcessEvents, audioMasterSetTime_deprecated,
    audioMasterTempoAt_deprecated, audioMasterGetNumAutomatableParameters_deprecated,
    audioMasterGetParameterQuantization_deprecated, audioMasterIOChanged,
    audioMasterNeedIdle_deprecated, audioMasterSizeWindow, audioMasterGetSampleRate,
    audioMasterGetBlockSize, audioMasterGetInputLatency, audioMasterGetOutputLatency,
    audioMasterGetPreviousPlug_deprecated, audioMasterGetNextPlug_deprecated,
    audioMasterWillReplaceOrAccumulate_deprecated, audioMasterGetCurrentProcessLevel,
    audioMasterGetAutomationState, audioMasterOfflineStart, audioMasterOfflineRead,
    audioMasterOfflineWrite, audioMasterOfflineGetCurrentPass, audioMasterOfflineGetCurrentMetaPass,
    audioMasterSetOutputSampleRate_deprecated, audioMasterGetOutputSpeakerArrangement_deprecated,
    audioMasterGetVendorString, audioMasterGetProductString, audioMasterGetVendorVersion,
    audioMasterVendorSpecific, audioMasterSetIcon_deprecated, audioMasterCanDo,
    audioMasterGetLanguage, audioMasterOpenWindow_deprecated, audioMasterCloseWindow_deprecated,
    audioMasterGetDirectory, audioMasterUpdateDisplay, audioMasterBeginEdit, audioMasterEndEdit,
    audioMasterOpenFileSelector, audioMasterCloseFileSelector, audioMasterEditFile_deprecated,
    audioMasterGetChunkFile_deprecated, audioMasterGetInputSpeakerArrangement_deprecated,
    audioMasterNumOpcodes
};

struct ERect {
    int16_t top, left, bottom, right;
};

// 88 bytes, no pointers: identical on x86 and x64.
struct VstTimeInfo {
    double samplePos;
    double sampleRate;
    double nanoSeconds;
    double ppqPos;
    double tempo;
    double barStartPos;
    double cycleStartPos;
    double cycleEndPos;
    int32_t timeSigNumerator;
    int32_t timeSigDenominator;
    int32_t smpteOffset;
    int32_t smpteFrameRate;
    int32_t samplesToNextClock;
    int32_t flags;
};

enum {
    kVstTransportChanged = 1, kVstTransportPlaying = 1 << 1, kVstTransportCycleActive = 1 << 2,
    kVstNanosValid = 1 << 8, kVstPpqPosValid = 1 << 9, kVstTempoValid = 1 << 10,
    kVstBarsValid = 1 << 11, kVstCyclePosValid = 1 << 12, kVstTimeSigValid = 1 << 13,
    kVstSmpteValid = 1 << 14, kVstClockValid = 1 << 15,
};

enum { kVstMidiType = 1, kVstSysExType = 6 };

// 32 bytes on both architectures.
struct VstEvent {
    int32_t type;
    int32_t byteSize;
    int32_t deltaFrames;
    int32_t flags;
    char data[16];
};

struct VstMidiEvent {
    int32_t type;
    int32_t byteSize;
    int32_t deltaFrames;
    int32_t flags;
    int32_t noteLength;
    int32_t noteOffset;
    char midiData[4];
    char detune;
    char noteOffVelocity;
    char reserved1;
    char reserved2;
};

// Pointer-sized members: layout differs between x86 and x64.
struct VstMidiSysexEvent {
    int32_t type;
    int32_t byteSize;
    int32_t deltaFrames;
    int32_t flags;
    int32_t dumpBytes;
    intptr_t resvd1;
    char* sysexDump;
    intptr_t resvd2;
};

struct VstEvents {
    int32_t numEvents;
    intptr_t reserved;
    VstEvent* events[2]; // variable length
};

#pragma pack(pop)

// Sizes of pointer-free structures that cross the bridge as raw bytes.
enum {
    kSizeVstTimeInfo = 88,
    kSizeVstPinProperties = 128,
    kSizeVstParameterProperties = 152,
    kSizeMidiProgramName = 80,
    kSizeMidiProgramCategory = 76,
    kSizeMidiKeyName = 80,
    kSizeVstPatchChunkInfo = 64,
    kSizeVstSpeakerProperties = 112,
    kSizeVstSpeakerArrangementHeader = 8,
};

static_assert(sizeof(VstTimeInfo) == kSizeVstTimeInfo, "VstTimeInfo layout");
static_assert(sizeof(VstMidiEvent) == 32, "VstMidiEvent layout");
static_assert(sizeof(VstEvent) == 32, "VstEvent layout");
