/* ═══════════════════════════════════════════════════════════════════
 *  RED808 DRUM MACHINE — Daisy Seed Slave
 * ─────────────────────────────────────────────────────────────────
 *  STM32H750 + 64 MB SDRAM | SPI1 slave | Protocolo RED808
 *  44100 Hz · 128 samples/block · 24 pads · 32 voces
 *  Master FX: Delay, Reverb, Chorus, Tremolo, Comp, Wavefolder,
 *             Limiter, Phaser, Flanger, Global Filter
 *  Per-track: Filter, Echo, Flanger, Comp, EQ 3-band, Sends,
 *             Pan, Mute/Solo
 *  Per-pad:   Filter, Distortion, Bitcrush, Loop, Reverse, Pitch,
 *             Stutter, Scratch, Turntablism
 *  SD Card:   Carga de kits WAV vía SPI3 master (módulo 6-pin)
 *
 *  Verificado contra: DAISY_SLAVE_GUIDE.md (ESP32-S3 v1.0)
 *
 *  PINOUT REAL (verificado en daisy_seed.h):
 *  SPI1 (Master comm, SLAVE): D7=PG10/NSS  D8=PG11/SCK
 *                              D9=PB4/MISO  D10=PB5/MOSI
 *  SPI3 (SD card, MASTER):     D0=PB12/CS(GPIO)  D2=PC10/SCK
 *                              D1=PC11/MISO      D6=PC12/MOSI
 * ═══════════════════════════════════════════════════════════════════ */

#include "daisy_seed.h"
#define USE_DAISYSP_LGPL
#include "daisysp.h"
#include "ff_gen_drv.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <strings.h>

/* Synth engine libraries */
#include "synth/tr808.h"
#include "synth/tr909.h"
#include "synth/tr505.h"
#include "synth/tb303.h"
#include "synth/demo_mode.h"

using namespace daisy;
using namespace daisysp;

/* ═══════════════════════════════════════════════════════════════════
 *  1. HARDWARE
 * ═══════════════════════════════════════════════════════════════════ */
DaisySeed hw;
SpiHandle spi_slave;

/* ═══════════════════════════════════════════════════════════════════
 *  2. CONFIGURACIÓN
 * ═══════════════════════════════════════════════════════════════════ */
#define SR                 48000
#define AUDIO_BLOCK        128
#define MAX_PADS           24
#define MAX_VOICES         32
#define MAX_SAMPLE_BYTES   (96000 * 2)   /* ~2.0 s per pad @ 48000  */
#define MAX_DELAY_SAMPLES  96000         /* 2 s @ 48000             */
#define TRACK_ECHO_SIZE    9600          /* 200 ms per track        */
#define TRACK_FLANGER_SIZE 2048

/* ═══════════════════════════════════════════════════════════════════
 *  3. PROTOCOLO RED808 — TODOS los command codes  (protocol.h)
 * ═══════════════════════════════════════════════════════════════════ */
#define SPI_MAGIC_CMD       0xA5
#define SPI_MAGIC_RESP      0x5A

/* Triggers */
#define CMD_TRIGGER_SEQ       0x01
#define CMD_TRIGGER_LIVE      0x02
#define CMD_TRIGGER_STOP      0x03
#define CMD_TRIGGER_STOP_ALL  0x04
#define CMD_TRIGGER_SIDECHAIN 0x05

/* Volume */
#define CMD_MASTER_VOLUME     0x10
#define CMD_SEQ_VOLUME        0x11
#define CMD_LIVE_VOLUME       0x12
#define CMD_TRACK_VOLUME      0x13
#define CMD_LIVE_PITCH        0x14

/* Global Filter */
#define CMD_FILTER_SET        0x20
#define CMD_FILTER_CUTOFF     0x21
#define CMD_FILTER_RESONANCE  0x22
#define CMD_FILTER_BITDEPTH   0x23
#define CMD_FILTER_DISTORTION 0x24
#define CMD_FILTER_DIST_MODE  0x25
#define CMD_FILTER_SR_REDUCE  0x26

/* Master FX */
#define CMD_DELAY_ACTIVE      0x30
#define CMD_DELAY_TIME        0x31
#define CMD_DELAY_FEEDBACK    0x32
#define CMD_DELAY_MIX         0x33
#define CMD_PHASER_ACTIVE     0x34
#define CMD_PHASER_RATE       0x35
#define CMD_PHASER_DEPTH      0x36
#define CMD_PHASER_FEEDBACK   0x37
#define CMD_FLANGER_ACTIVE    0x38
#define CMD_FLANGER_RATE      0x39
#define CMD_FLANGER_DEPTH     0x3A
#define CMD_FLANGER_FEEDBACK  0x3B
#define CMD_FLANGER_MIX       0x3C
#define CMD_COMP_ACTIVE       0x3D
#define CMD_COMP_THRESHOLD    0x3E
#define CMD_COMP_RATIO        0x3F
#define CMD_COMP_ATTACK       0x40
#define CMD_COMP_RELEASE      0x41
#define CMD_COMP_MAKEUP       0x42
#define CMD_REVERB_ACTIVE     0x43
#define CMD_REVERB_FEEDBACK   0x44
#define CMD_REVERB_LPFREQ     0x45
#define CMD_REVERB_MIX        0x46
#define CMD_CHORUS_ACTIVE     0x47
#define CMD_CHORUS_RATE       0x48
#define CMD_CHORUS_DEPTH      0x49
#define CMD_CHORUS_MIX        0x4A
#define CMD_TREMOLO_ACTIVE    0x4B
#define CMD_TREMOLO_RATE      0x4C
#define CMD_TREMOLO_DEPTH     0x4D
#define CMD_WAVEFOLDER_GAIN   0x4E
#define CMD_LIMITER_ACTIVE    0x4F

/* Per-Track FX */
#define CMD_TRACK_FILTER      0x50
#define CMD_TRACK_CLEAR_FILTER 0x51
#define CMD_TRACK_DISTORTION  0x52
#define CMD_TRACK_BITCRUSH    0x53
#define CMD_TRACK_ECHO        0x54
#define CMD_TRACK_FLANGER_FX  0x55
#define CMD_TRACK_COMPRESSOR  0x56
#define CMD_TRACK_CLEAR_LIVE  0x57
#define CMD_TRACK_CLEAR_FX    0x58
#define CMD_TRACK_REVERB_SEND 0x59
#define CMD_TRACK_DELAY_SEND  0x5A
#define CMD_TRACK_CHORUS_SEND 0x5B
#define CMD_TRACK_PAN         0x5C
#define CMD_TRACK_MUTE        0x5D
#define CMD_TRACK_SOLO        0x5E
#define CMD_TRACK_PHASER      0x5F
#define CMD_TRACK_TREMOLO     0x60
#define CMD_TRACK_PITCH       0x61
#define CMD_TRACK_GATE        0x62
#define CMD_TRACK_EQ_LOW      0x63
#define CMD_TRACK_EQ_MID      0x64
#define CMD_TRACK_EQ_HIGH     0x65

/* Per-Pad FX */
#define CMD_PAD_FILTER        0x70
#define CMD_PAD_CLEAR_FILTER  0x71
#define CMD_PAD_DISTORTION    0x72
#define CMD_PAD_BITCRUSH      0x73
#define CMD_PAD_LOOP          0x74
#define CMD_PAD_REVERSE       0x75
#define CMD_PAD_PITCH         0x76
#define CMD_PAD_STUTTER       0x77
#define CMD_PAD_SCRATCH       0x78
#define CMD_PAD_TURNTABLISM   0x79
#define CMD_PAD_CLEAR_FX      0x7A

/* Sidechain */
#define CMD_SIDECHAIN_SET     0x90
#define CMD_SIDECHAIN_CLEAR   0x91

/* Sample Transfer */
#define CMD_SAMPLE_BEGIN      0xA0
#define CMD_SAMPLE_DATA       0xA1
#define CMD_SAMPLE_END        0xA2
#define CMD_SAMPLE_UNLOAD     0xA3
#define CMD_SAMPLE_UNLOAD_ALL 0xA4

/* SD Card */
#define CMD_SD_LIST_FOLDERS   0xB0
#define CMD_SD_LIST_FILES     0xB1
#define CMD_SD_FILE_INFO      0xB2
#define CMD_SD_LOAD_SAMPLE    0xB3
#define CMD_SD_LOAD_KIT       0xB4
#define CMD_SD_KIT_LIST       0xB5
#define CMD_SD_STATUS         0xB6
#define CMD_SD_UNLOAD_KIT     0xB7
#define CMD_SD_GET_LOADED     0xB8
#define CMD_SD_ABORT          0xB9

/* Status / Query */
#define CMD_GET_STATUS        0xE0
#define CMD_GET_PEAKS         0xE1
#define CMD_GET_CPU_LOAD      0xE2
#define CMD_GET_VOICES        0xE3
#define CMD_GET_EVENTS        0xE4
#define CMD_PING              0xEE
#define CMD_RESET             0xEF

/* Synth Engine */
#define CMD_SYNTH_TRIGGER     0xC0  /* [engine(1), instrument(1), velocity(1)] */
#define CMD_SYNTH_PARAM       0xC1  /* [engine(1), instrument(1), paramId(1), value(4)] */
#define CMD_SYNTH_NOTE_ON     0xC2  /* [midiNote(1), accent(1), slide(1)] → 303 */
#define CMD_SYNTH_NOTE_OFF    0xC3  /* 303 note off */
#define CMD_SYNTH_303_PARAM   0xC4  /* [paramId(1), value(4)] → 303 params */
#define CMD_SYNTH_ACTIVE      0xC5  /* [engineMask(1)] enable/disable engines */

/* Synth Engine IDs */
#define SYNTH_ENGINE_808   0
#define SYNTH_ENGINE_909   1
#define SYNTH_ENGINE_505   2
#define SYNTH_ENGINE_303   3

/* Bulk */
#define CMD_BULK_TRIGGERS     0xF0
#define CMD_BULK_FX           0xF1

/* Filter types */
#define FTYPE_NONE       0
#define FTYPE_LOWPASS    1
#define FTYPE_HIGHPASS   2
#define FTYPE_BANDPASS   3
#define FTYPE_NOTCH      4
#define FTYPE_ALLPASS    5
#define FTYPE_PEAKING    6
#define FTYPE_LOWSHELF   7
#define FTYPE_HIGHSHELF  8

/* Distortion modes */
#define DMODE_SOFT  0
#define DMODE_HARD  1
#define DMODE_TUBE  2
#define DMODE_FUZZ  3

/* ═══════════════════════════════════════════════════════════════════
 *  4. SPI PACKET
 * ═══════════════════════════════════════════════════════════════════ */
struct __attribute__((packed)) SPIPacketHeader {
    uint8_t  magic;
    uint8_t  cmd;
    uint16_t length;
    uint16_t sequence;
    uint16_t checksum;
};

#define RX_BUF_SIZE  536
#define TX_BUF_SIZE  768   /* SD responses up to 676 bytes payload */

static uint8_t rxBuf[RX_BUF_SIZE];
static uint8_t txBuf[TX_BUF_SIZE];
static volatile bool  waitingPayload  = false;
static volatile bool  pendingResponse = false;
static uint16_t       pendingTxLen    = 0;

/* ═══════════════════════════════════════════════════════════════════
 *  5. SD RESPONSE STRUCTS & PATHS
 * ═══════════════════════════════════════════════════════════════════ */

/* Root path on SD card — firmware tries /data first, then / */
static const char* SD_DATA_ROOT = "/data";

/* Canonical pad→instrument mapping (pads 0-15 = LIVE, 16-23 = XTRA) */
static const char* PAD_FAMILY_NAMES[16] = {
    "BD","SD","CH","OH","CY","CP","RS","CB",
    "LT","MT","HT","MA","CL","HC","MC","LC"
};

/* Keyword table for matching RED 808 KARZ filenames → pad index */
struct InstrKeyword { const char* keyword; uint8_t pad; };
static const InstrKeyword INSTR_KEYWORDS[] = {
    {"BD",  0}, {"KICK", 0},
    {"SD",  1}, {"SNARE",1},
    {"CH",  2}, {"HH",   2}, {"HIHAT",2}, {"CLOSED",2},
    {"OH",  3}, {"OPEN", 3},
    {"CY",  4}, {"CYMBAL",4}, {"CRASH",4}, {"RIDE",4},
    {"CP",  5}, {"CLAP", 5},
    {"RS",  6}, {"RIM",  6},
    {"CB",  7}, {"COW",  7}, {"BELL", 7},
    {"LT",  8}, {"LTOM", 8},
    {"MT",  9}, {"MTOM", 9},
    {"HT", 10}, {"HTOM",10},
    {"MA", 11}, {"MARAC",11},
    {"CL", 12}, {"CLAV", 12}, {"CLAVE",12},
    {"HC", 13}, {"CONGA",13},
    {"MC", 14},
    {"LC", 15},
};
static const int NUM_INSTR_KEYWORDS = sizeof(INSTR_KEYWORDS)/sizeof(INSTR_KEYWORDS[0]);

struct __attribute__((packed)) SdKitListResponse {
    uint8_t count;
    char    kits[16][32];   /* max 16 kits, 32 chars each = 513 bytes */
};

struct __attribute__((packed)) SdLoadKitPayload {
    char    kitName[32];
    uint8_t startPad;
    uint8_t maxPads;
};

struct __attribute__((packed)) SdStatusResponse {
    uint8_t  present;
    uint8_t  reserved;
    uint16_t samplesLoaded;  /* bitmask */
    char     currentKit[32];
};

struct __attribute__((packed)) SdListFilesPayload {
    char folder[32];        /* e.g. "BD", "xtra", "RED 808 KARZ" */
};

struct __attribute__((packed)) SdListFilesResponse {
    uint8_t count;
    char    files[20][32];  /* max 20 files, 32 chars each */
};

struct __attribute__((packed)) SdFileInfoPayload {
    char folder[32];
    char filename[32];
};

struct __attribute__((packed)) SdFileInfoResponse {
    uint32_t sizeBytes;
    uint16_t sampleRate;
    uint16_t bitsPerSample;
    uint8_t  channels;
    uint8_t  reserved[3];
    uint32_t durationMs;    /* estimated */
};

struct __attribute__((packed)) SdLoadSamplePayload {
    char    folder[32];
    char    filename[32];
    uint8_t padIdx;
};

/* ═══════════════════════════════════════════════════════════════════
 *  6. SAMPLES EN SDRAM  (64 MB)
 * ═══════════════════════════════════════════════════════════════════ */
DSY_SDRAM_BSS static int16_t sampleStorage[MAX_PADS][MAX_SAMPLE_BYTES / 2];

static uint32_t sampleLength[MAX_PADS];
static uint32_t sampleTotalSamples[MAX_PADS];
static bool     sampleLoaded[MAX_PADS];

/* ═══════════════════════════════════════════════════════════════════
 *  7. VOCES POLIFÓNICAS
 * ═══════════════════════════════════════════════════════════════════ */
struct Voice {
    bool     active;
    uint8_t  pad;
    float    pos;
    float    speed;
    float    gainL;
    float    gainR;
    uint32_t age;
};
static Voice   voices[MAX_VOICES];
static uint32_t voiceAge = 0;

/* ═══════════════════════════════════════════════════════════════════
 *  8. VOLÚMENES
 * ═══════════════════════════════════════════════════════════════════ */
static float masterGain  = 1.0f;
static float seqVolume   = 1.0f;
static float liveVolume  = 1.0f;
static float livePitch   = 1.0f;
static float trackGain[MAX_PADS];

/* ═══════════════════════════════════════════════════════════════════
 *  9. PEAKS
 * ═══════════════════════════════════════════════════════════════════ */
static volatile float trackPeak[MAX_PADS];
static volatile float masterPeak = 0.0f;

/* ═══════════════════════════════════════════════════════════════════
 *  10. BiquadEQ  (Audio EQ Cookbook – LP/HP/BP/Notch/Peak/Shelf)
 * ═══════════════════════════════════════════════════════════════════ */
struct BiquadEQ {
    float b0=1,b1=0,b2=0,a1=0,a2=0;
    float z1=0,z2=0;

    float Process(float in){
        float out = b0*in + z1;
        z1 = b1*in - a1*out + z2;
        z2 = b2*in - a2*out;
        return out;
    }
    void Reset(){ z1=z2=0; }

    void SetType(uint8_t t, float freq, float q, float sr, float gainDb=0.f){
        if(freq<20.f) freq=20.f;
        if(freq>sr*0.45f) freq=sr*0.45f;
        if(q<0.3f) q=0.3f;
        float w = 2.f*(float)M_PI*freq/sr;
        float s_ = sinf(w), c_ = cosf(w);
        float a  = s_/(2.f*q);
        float a0i;
        switch(t){
            case FTYPE_LOWPASS:
                a0i = 1.f/(1.f+a);
                b0 = ((1.f-c_)*0.5f)*a0i;
                b1 = (1.f-c_)*a0i;
                b2 = b0; a1=(-2.f*c_)*a0i; a2=(1.f-a)*a0i;
                break;
            case FTYPE_HIGHPASS:
                a0i = 1.f/(1.f+a);
                b0 = ((1.f+c_)*0.5f)*a0i;
                b1 = -(1.f+c_)*a0i;
                b2 = b0; a1=(-2.f*c_)*a0i; a2=(1.f-a)*a0i;
                break;
            case FTYPE_BANDPASS:
                a0i = 1.f/(1.f+a);
                b0 = a*a0i; b1=0; b2=-b0;
                a1=(-2.f*c_)*a0i; a2=(1.f-a)*a0i;
                break;
            case FTYPE_NOTCH:
                a0i = 1.f/(1.f+a);
                b0 = a0i; b1=(-2.f*c_)*a0i; b2=a0i;
                a1=b1; a2=(1.f-a)*a0i;
                break;
            case FTYPE_PEAKING: {
                float A = powf(10.f, gainDb/40.f);
                a0i = 1.f/(1.f + a/A);
                b0 = (1.f + a*A)*a0i;
                b1 = (-2.f*c_)*a0i;
                b2 = (1.f - a*A)*a0i;
                a1 = b1; a2 = (1.f - a/A)*a0i;
                break;
            }
            case FTYPE_LOWSHELF: {
                float A = powf(10.f, gainDb/40.f);
                float sq = 2.f*sqrtf(A)*a;
                a0i = 1.f/((A+1.f)+(A-1.f)*c_+sq);
                b0 = A*((A+1.f)-(A-1.f)*c_+sq)*a0i;
                b1 = 2.f*A*((A-1.f)-(A+1.f)*c_)*a0i;
                b2 = A*((A+1.f)-(A-1.f)*c_-sq)*a0i;
                a1 = -2.f*((A-1.f)+(A+1.f)*c_)*a0i;
                a2 = ((A+1.f)+(A-1.f)*c_-sq)*a0i;
                break;
            }
            case FTYPE_HIGHSHELF: {
                float A = powf(10.f, gainDb/40.f);
                float sq = 2.f*sqrtf(A)*a;
                a0i = 1.f/((A+1.f)-(A-1.f)*c_+sq);
                b0 = A*((A+1.f)+(A-1.f)*c_+sq)*a0i;
                b1 = -2.f*A*((A-1.f)+(A+1.f)*c_)*a0i;
                b2 = A*((A+1.f)+(A-1.f)*c_-sq)*a0i;
                a1 = 2.f*((A-1.f)-(A+1.f)*c_)*a0i;
                a2 = ((A+1.f)-(A-1.f)*c_+sq)*a0i;
                break;
            }
            default: b0=1;b1=b2=a1=a2=0; break;
        }
    }
};

/* ═══════════════════════════════════════════════════════════════════
 *  11. DaisySP MASTER FX
 * ═══════════════════════════════════════════════════════════════════ */
static DelayLine<float, MAX_DELAY_SAMPLES> DSY_SDRAM_BSS masterDelay;
DSY_SDRAM_BSS static ReverbSc   masterReverb;
DSY_SDRAM_BSS static Chorus     masterChorus;
static Tremolo    masterTremolo;
static Compressor masterComp;
static Fold       masterFold;
DSY_SDRAM_BSS static Phaser     masterPhaser;

/* Delay */
static bool  delayActive   = false;
static float delayTime     = 250.0f;
static float delayFeedback = 0.3f;
static float delayMix      = 0.3f;

/* Reverb */
static bool  reverbActive   = false;
static float reverbFeedback = 0.85f;
static float reverbLpFreq   = 8000.0f;
static float reverbMix      = 0.3f;

/* Chorus */
static bool  chorusActive = false;
static float chorusMix    = 0.4f;

/* Tremolo */
static bool  tremoloActive = false;

/* Compressor */
static bool  compActive = false;

/* Phaser */
static bool  phaserActive   = false;

/* Flanger (manual: delay buf + LFO) */
static bool  flangerActive  = false;
static float flangerRate    = 0.5f;
static float flangerDepth   = 0.5f;
static float flangerFb      = 0.3f;
static float flangerMix     = 0.3f;
static float flangerPhase   = 0.0f;
DSY_SDRAM_BSS static float flangerBuf[4096];
static uint32_t flangerWp   = 0;

/* Wavefolder + Limiter */
static float waveFolderGain = 1.0f;
static bool  limiterActive  = false;

/* ═══════════════════════════════════════════════════════════════════
 *  12. GLOBAL FILTER STATE
 * ═══════════════════════════════════════════════════════════════════ */
static BiquadEQ  gFilterL, gFilterR;
static uint8_t gFilterType    = FTYPE_NONE;
static float   gFilterCutoff  = 10000.0f;
static float   gFilterQ       = 0.707f;
static uint8_t gFilterBitDepth= 16;
static float   gFilterDist    = 0.0f;
static uint8_t gFilterDistMode= DMODE_SOFT;
static uint32_t gFilterSrReduce = 0;  /* 0 = disabled */
static float   gSrHoldL = 0, gSrHoldR = 0;
static uint32_t gSrCounter = 0;

/* ═══════════════════════════════════════════════════════════════════
 *  13. PER-PAD STATE
 * ═══════════════════════════════════════════════════════════════════ */
static bool  padLoop[MAX_PADS];
static bool  padReverse[MAX_PADS];
static float padPitch[MAX_PADS];

/* Pad filter */
static BiquadEQ  padFilter[MAX_PADS];
static uint8_t padFilterType[MAX_PADS];
static float   padFilterCut[MAX_PADS];
static float   padFilterQ[MAX_PADS];

/* Pad distortion + bitcrush */
static float   padDistDrive[MAX_PADS];
static uint8_t padBitDepth[MAX_PADS];

/* Stutter */
static bool     padStutterOn[MAX_PADS];
static uint16_t padStutterIval[MAX_PADS];
static uint16_t padStutterCnt[MAX_PADS];

/* Scratch */
static bool  padScratchOn[MAX_PADS];
static float padScratchRate[MAX_PADS];
static float padScratchDepth[MAX_PADS];
static float padScratchCut[MAX_PADS];
static float padScratchCrackle[MAX_PADS];
static float padScratchPhase[MAX_PADS];
static BiquadEQ padScratchFilter[MAX_PADS];

/* Turntablism */
static bool     padTurnOn[MAX_PADS];
static bool     padTurnAuto[MAX_PADS];
static int8_t   padTurnMode[MAX_PADS];
static uint16_t padTurnBrakeMs[MAX_PADS];
static uint16_t padTurnBackMs[MAX_PADS];
static float    padTurnRate[MAX_PADS];
static float    padTurnNoise[MAX_PADS];
static float    padTurnPhase[MAX_PADS];
static uint32_t padTurnCounter[MAX_PADS];

/* ═══════════════════════════════════════════════════════════════════
 *  14. PER-TRACK MIXER + FX
 * ═══════════════════════════════════════════════════════════════════ */
static float trackReverbSend[MAX_PADS];
static float trackDelaySend[MAX_PADS];
static float trackChorusSend[MAX_PADS];
static float trackPanF[MAX_PADS];          /* -1.0..+1.0 */
static bool  trackMute[MAX_PADS];
static bool  trackSolo[MAX_PADS];
static bool  anySolo = false;

/* Per-track filter */
static BiquadEQ  trkFilter[MAX_PADS];
static uint8_t trkFilterType[MAX_PADS];
static float   trkFilterCut[MAX_PADS];
static float   trkFilterQ[MAX_PADS];

/* Per-track distortion + bitcrush */
static float   trkDistDrive[MAX_PADS];
static uint8_t trkDistMode[MAX_PADS];
static uint8_t trkBitDepth[MAX_PADS];

/* Per-track echo (delay buf in SDRAM) */
DSY_SDRAM_BSS static float trkEchoBuf[MAX_PADS][TRACK_ECHO_SIZE];
static bool     trkEchoActive[MAX_PADS];
static float    trkEchoDelay[MAX_PADS];
static float    trkEchoFb[MAX_PADS];
static float    trkEchoMix[MAX_PADS];
static uint32_t trkEchoWp[MAX_PADS];

/* Per-track flanger */
DSY_SDRAM_BSS static float trkFlgBuf[MAX_PADS][TRACK_FLANGER_SIZE];
static bool     trkFlgActive[MAX_PADS];
static float    trkFlgDepth[MAX_PADS];
static float    trkFlgRate[MAX_PADS];
static float    trkFlgFb[MAX_PADS];
static float    trkFlgMix[MAX_PADS];
static float    trkFlgPhase[MAX_PADS];
static uint32_t trkFlgWp[MAX_PADS];

/* Per-track compressor */
static bool  trkCompActive[MAX_PADS];
static float trkCompThresh[MAX_PADS];
static float trkCompRatio[MAX_PADS];
static float trkCompEnv[MAX_PADS];

/* Per-track EQ (3-band: low shelf 200Hz, mid peak 1kHz, high shelf 4kHz) */
static BiquadEQ trkEqLow[MAX_PADS];
static BiquadEQ trkEqMid[MAX_PADS];
static BiquadEQ trkEqHigh[MAX_PADS];
static int8_t trkEqLowDb[MAX_PADS];
static int8_t trkEqMidDb[MAX_PADS];
static int8_t trkEqHighDb[MAX_PADS];

/* ═══════════════════════════════════════════════════════════════════
 *  15. SIDECHAIN
 * ═══════════════════════════════════════════════════════════════════ */
static bool     scActive    = false;
static uint8_t  scSrc       = 0;
static uint16_t scDstMask   = 0;
static float    scAmount    = 0.5f;
static float    scAttackK   = 0.5f;
static float    scReleaseK  = 0.1f;
static float    scEnv       = 0.0f;

/* ═══════════════════════════════════════════════════════════════════
 *  16. SD CARD (SPI3 master — módulo 6 pines)
 *  Conexión: CS=D0(PB12) SCK=D2(PC10) MISO=D1(PC11) MOSI=D6(PC12)
 * ═══════════════════════════════════════════════════════════════════ */
static SpiHandle  sd_spi;         /* SPI3 master for SD card          */
static GPIO       sd_cs;           /* D0 = PB12 for CS (GPIO manual)   */
static FATFS      sdFatFs;        /* FatFS filesystem object           */
static bool    sdPresent = false;
static char    currentKitName[32] = "";
static uint8_t sd_card_type = 0;  /* 0=none 1=SDv1 2=SDv2 6=SDHC      */

/* ── SD SPI low-level helpers ───────────────────────────────────── */
static inline void SD_CS_LOW()  { sd_cs.Write(false); }
static inline void SD_CS_HIGH() { sd_cs.Write(true);  }

static uint8_t SD_TxRx(uint8_t tx){
    uint8_t rx = 0xFF;
    sd_spi.BlockingTransmitAndReceive(&tx, &rx, 1, 10);
    return rx;
}

static bool SD_WaitReady(uint32_t timeout_ms){
    uint32_t start = System::GetNow();
    do {
        if(SD_TxRx(0xFF) == 0xFF) return true;
    } while((System::GetNow() - start) < timeout_ms);
    return false;
}

/* ── SD SPI command protocol ────────────────────────────────────── */
#define SD_CMD0    (0x40+0)   /* GO_IDLE_STATE          */
#define SD_CMD8    (0x40+8)   /* SEND_IF_COND           */
#define SD_CMD9    (0x40+9)   /* SEND_CSD               */
#define SD_CMD12   (0x40+12)  /* STOP_TRANSMISSION      */
#define SD_CMD16   (0x40+16)  /* SET_BLOCKLEN           */
#define SD_CMD17   (0x40+17)  /* READ_SINGLE_BLOCK      */
#define SD_CMD18   (0x40+18)  /* READ_MULTIPLE_BLOCK    */
#define SD_CMD24   (0x40+24)  /* WRITE_BLOCK            */
#define SD_CMD25   (0x40+25)  /* WRITE_MULTIPLE_BLOCK   */
#define SD_CMD55   (0x40+55)  /* APP_CMD                */
#define SD_CMD58   (0x40+58)  /* READ_OCR               */
#define SD_ACMD41  (0xC0+41)  /* SD_SEND_OP_COND (app)  */

static uint8_t SD_SendCmd(uint8_t cmd, uint32_t arg)
{
    uint8_t n, res;
    if(cmd & 0x80){                       /* ACMD: send CMD55 first */
        cmd &= 0x7F;
        res = SD_SendCmd(SD_CMD55, 0);
        if(res > 1) return res;
    }
    /* Select card */
    SD_CS_HIGH(); SD_TxRx(0xFF);
    SD_CS_LOW();  SD_TxRx(0xFF);

    /* Command packet */
    SD_TxRx(cmd);
    SD_TxRx((uint8_t)(arg >> 24));
    SD_TxRx((uint8_t)(arg >> 16));
    SD_TxRx((uint8_t)(arg >> 8));
    SD_TxRx((uint8_t)arg);
    n = 0x01;
    if(cmd == SD_CMD0) n = 0x95;          /* Valid CRC for CMD0(0)  */
    if(cmd == SD_CMD8) n = 0x87;          /* Valid CRC for CMD8     */
    SD_TxRx(n);

    if(cmd == SD_CMD12) SD_TxRx(0xFF);    /* Skip stuff byte        */
    n = 10;
    do { res = SD_TxRx(0xFF); } while((res & 0x80) && --n);
    return res;
}

static bool SD_RxDataBlock(uint8_t* buf, uint32_t cnt)
{
    uint8_t token;
    uint32_t start = System::GetNow();
    do { token = SD_TxRx(0xFF); }
    while(token == 0xFF && (System::GetNow() - start) < 200);
    if(token != 0xFE) return false;
    for(uint32_t i = 0; i < cnt; i++) buf[i] = SD_TxRx(0xFF);
    SD_TxRx(0xFF); SD_TxRx(0xFF);        /* Discard CRC            */
    return true;
}

static bool SD_TxDataBlock(const uint8_t* buf, uint8_t token)
{
    if(!SD_WaitReady(500)) return false;
    SD_TxRx(token);
    if(token != 0xFD){
        for(uint32_t i = 0; i < 512; i++) SD_TxRx(buf[i]);
        SD_TxRx(0xFF); SD_TxRx(0xFF);    /* Dummy CRC              */
        uint8_t resp = SD_TxRx(0xFF);
        if((resp & 0x1F) != 0x05) return false;
    }
    return true;
}

/* ── FatFS diskio callbacks (registered via FATFS_LinkDriver) ──── */
static DSTATUS SPISD_DiskStatus(BYTE lun){
    return sd_card_type ? 0 : STA_NOINIT;
}

static DSTATUS SPISD_DiskInit(BYTE lun)
{
    uint8_t n, ty, ocr[4];
    SD_CS_HIGH();
    for(n = 0; n < 10; n++) SD_TxRx(0xFF);  /* >=74 clocks            */

    ty = 0;
    if(SD_SendCmd(SD_CMD0, 0) == 1){         /* Enter idle             */
        uint32_t start = System::GetNow();
        if(SD_SendCmd(SD_CMD8, 0x1AA) == 1){ /* SDv2 ?                 */
            for(n = 0; n < 4; n++) ocr[n] = SD_TxRx(0xFF);
            if(ocr[2] == 0x01 && ocr[3] == 0xAA){
                while((System::GetNow() - start) < 1000)
                    if(SD_SendCmd(SD_ACMD41, 1UL << 30) == 0) break;
                if((System::GetNow() - start) < 1000
                   && SD_SendCmd(SD_CMD58, 0) == 0){
                    for(n = 0; n < 4; n++) ocr[n] = SD_TxRx(0xFF);
                    ty = (ocr[0] & 0x40) ? 6 : 2; /* SDHC(6) or SDv2(2)   */
                }
            }
        } else {
            if(SD_SendCmd(SD_ACMD41, 0) <= 1){ ty = 1; /* SDv1              */
                while((System::GetNow() - start) < 1000)
                    if(SD_SendCmd(SD_ACMD41, 0) == 0) break;
            }
            if(ty && SD_SendCmd(SD_CMD16, 512) != 0) ty = 0;
        }
    }
    SD_CS_HIGH(); SD_TxRx(0xFF);
    sd_card_type = ty;
    return ty ? 0 : STA_NOINIT;
}

static DRESULT SPISD_DiskRead(BYTE lun, BYTE* buff, DWORD sector, UINT count)
{
    if(!sd_card_type) return RES_NOTRDY;
    if(!(sd_card_type & 4)) sector *= 512;
    if(count == 1){
        if(SD_SendCmd(SD_CMD17, sector) == 0
           && SD_RxDataBlock(buff, 512)) count = 0;
    } else {
        if(SD_SendCmd(SD_CMD18, sector) == 0){
            do {
                if(!SD_RxDataBlock(buff, 512)) break;
                buff += 512;
            } while(--count);
            SD_SendCmd(SD_CMD12, 0);
        }
    }
    SD_CS_HIGH(); SD_TxRx(0xFF);
    return count ? RES_ERROR : RES_OK;
}

static DRESULT SPISD_DiskWrite(BYTE lun, const BYTE* buff, DWORD sector, UINT count)
{
    if(!sd_card_type) return RES_NOTRDY;
    if(!(sd_card_type & 4)) sector *= 512;
    if(count == 1){
        if(SD_SendCmd(SD_CMD24, sector) == 0
           && SD_TxDataBlock(buff, 0xFE)) count = 0;
    } else {
        if(SD_SendCmd(SD_CMD25, sector) == 0){
            do {
                if(!SD_TxDataBlock(buff, 0xFC)) break;
                buff += 512;
            } while(--count);
            SD_TxDataBlock(0, 0xFD);
        }
    }
    SD_CS_HIGH(); SD_TxRx(0xFF);
    return count ? RES_ERROR : RES_OK;
}

static DRESULT SPISD_DiskIoctl(BYTE lun, BYTE cmd, void* buff)
{
    DRESULT res = RES_ERROR;
    uint8_t csd[16];
    if(!sd_card_type) return RES_NOTRDY;
    switch(cmd){
        case CTRL_SYNC:
            SD_CS_LOW();
            if(SD_WaitReady(500)) res = RES_OK;
            SD_CS_HIGH();
            break;
        case GET_SECTOR_COUNT:
            if(SD_SendCmd(SD_CMD9, 0) == 0 && SD_RxDataBlock(csd, 16)){
                DWORD n_sec;
                if((csd[0] >> 6) == 1){
                    n_sec = ((DWORD)(csd[7]&0x3F)<<16)|((DWORD)csd[8]<<8)|csd[9];
                    n_sec = (n_sec + 1) << 10;
                } else {
                    uint8_t nn = (csd[5]&0x0F)+((csd[10]&0x80)>>7)+((csd[9]&3)<<1)+2;
                    n_sec = ((DWORD)(csd[8]>>6)+((DWORD)csd[7]<<2)+((DWORD)(csd[6]&3)<<10)+1);
                    n_sec <<= (nn - 9);
                }
                *(DWORD*)buff = n_sec;
                res = RES_OK;
            }
            SD_CS_HIGH(); SD_TxRx(0xFF);
            break;
        case GET_SECTOR_SIZE:
            *(WORD*)buff = 512; res = RES_OK; break;
        case GET_BLOCK_SIZE:
            *(DWORD*)buff = 1;  res = RES_OK; break;
        default: res = RES_PARERR;
    }
    return res;
}

static const Diskio_drvTypeDef SPISD_Driver = {
    SPISD_DiskInit,
    SPISD_DiskStatus,
    SPISD_DiskRead,
    SPISD_DiskWrite,
    SPISD_DiskIoctl
};

/* ═══════════════════════════════════════════════════════════════════
 *  16b. EVENT NOTIFICATION SYSTEM
 *  La Daisy es SPI esclava → no puede empujar datos al Master.
 *  Solución: cola circular de eventos. El Master descubre que hay
 *  eventos pendientes al ver eventCount > 0 en CMD_GET_STATUS,
 *  y luego llama CMD_GET_EVENTS para drenarlos.
 * ═══════════════════════════════════════════════════════════════════ */
#define EVT_SD_BOOT_DONE       0x01  /* Boot loading complete             */
#define EVT_SD_KIT_LOADED      0x02  /* Kit loaded by CMD_SD_LOAD_KIT     */
#define EVT_SD_SAMPLE_LOADED   0x03  /* Sample cargado por CMD_SD_LOAD_SAMPLE */
#define EVT_SD_KIT_UNLOADED    0x04  /* Kit descargado                    */
#define EVT_SD_ERROR           0x05  /* Error de SD                       */
#define EVT_SD_XTRA_LOADED     0x06  /* XTRA PADS cargados al boot        */

struct __attribute__((packed)) NotifyEvent {
    uint8_t  type;          /* EVT_SD_* */
    uint8_t  padCount;      /* cuántos pads afectados */
    uint8_t  padMaskLo;     /* bitmask pads 0-7  loaded */
    uint8_t  padMaskHi;     /* bitmask pads 8-15 loaded */
    uint8_t  padMaskXtra;   /* bitmask pads 16-23 loaded */
    uint8_t  reserved[3];
    char     name[24];      /* kit name / sample name */
};  /* 32 bytes */

#define EVT_QUEUE_SIZE 8
static NotifyEvent evtQueue[EVT_QUEUE_SIZE];
static volatile uint8_t evtHead = 0;  /* next write position  */
static volatile uint8_t evtTail = 0;  /* next read  position  */
static volatile uint8_t evtCount = 0; /* events in queue      */

static void PushEvent(uint8_t type, uint8_t padCount,
                      uint32_t padMask24, const char* name)
{
    if(evtCount >= EVT_QUEUE_SIZE){
        /* Queue full — overwrite oldest */
        evtTail = (evtTail + 1) % EVT_QUEUE_SIZE;
        evtCount--;
    }
    NotifyEvent& e = evtQueue[evtHead];
    memset(&e, 0, sizeof(e));
    e.type       = type;
    e.padCount   = padCount;
    e.padMaskLo  = (uint8_t)(padMask24 & 0xFF);
    e.padMaskHi  = (uint8_t)((padMask24 >> 8) & 0xFF);
    e.padMaskXtra= (uint8_t)((padMask24 >> 16) & 0xFF);
    if(name) strncpy(e.name, name, 23);
    evtHead = (evtHead + 1) % EVT_QUEUE_SIZE;
    evtCount++;
}

static uint8_t PopEvents(NotifyEvent* dst, uint8_t maxEvents)
{
    uint8_t count = 0;
    while(evtCount > 0 && count < maxEvents){
        dst[count++] = evtQueue[evtTail];
        evtTail = (evtTail + 1) % EVT_QUEUE_SIZE;
        evtCount--;
    }
    return count;
}

/* ═══════════════════════════════════════════════════════════════════
 *  17. MISC STATS
 * ═══════════════════════════════════════════════════════════════════ */
static volatile uint32_t spiPktCnt = 0;
static volatile uint16_t spiErrCnt = 0;

/* ═══════════════════════════════════════════════════════════════════
 *  17b. SYNTH ENGINE INSTANCES
 * ═══════════════════════════════════════════════════════════════════ */
static TR808::Kit synth808;
static TR909::Kit synth909;
static TR505::Kit synth505;
static TB303::Synth acid303;

/* Bitmask: qué engines están activos (bit0=808, bit1=909, bit2=505, bit3=303) */
static uint8_t synthActiveMask = 0x0B;  /* 808+909+303 activos; 505 desactivado por estabilidad */

/* ── Demo Mode ── */
static Demo::DemoSequencer demoSeq;
static bool demoModeActive = true;   /* arranca en demo, se desactiva al recibir SPI */
static constexpr bool kEnableSpiSlave = true;  /* modo integrado: comunicación SPI1 con ESP32 master */
static constexpr bool kEnableSynth505 = false; /* aislamiento de crash en callback */
static constexpr bool kAudioSafeMode = false; /* callback de audio real */
static constexpr bool kBootDiagMinimal = false; /* diagnóstico extremo: solo LED, sin audio ni FX */
static constexpr bool kEnableAudioStart = true; /* iniciar audio normal */
static constexpr bool kEnableStartLog = false; /* diagnóstico: aislar StartLog/USB */
static constexpr bool kEnableInitFx = true;    /* diagnóstico: reactivar InitFX para aislar causa */

/* PRNG for crackle/noise FX */
static uint32_t noiseState = 0x12345678;
static uint32_t FastRand(){
    noiseState ^= noiseState<<13;
    noiseState ^= noiseState>>17;
    noiseState ^= noiseState<<5;
    return noiseState;
}
static float RandFloat(){
    return ((float)(int32_t)FastRand()) / 2147483648.0f;
}

/* ═══════════════════════════════════════════════════════════════════
 *  18. CRC16 MODBUS
 * ═══════════════════════════════════════════════════════════════════ */
static uint16_t crc16(const uint8_t* d, uint16_t len){
    uint16_t crc = 0xFFFF;
    for(uint16_t i = 0; i < len; i++){
        crc ^= d[i];
        for(uint8_t j = 0; j < 8; j++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
    return crc;
}

/* ═══════════════════════════════════════════════════════════════════
 *  19. DSP HELPERS
 * ═══════════════════════════════════════════════════════════════════ */
static inline float clampF(float v, float lo, float hi){
    return v < lo ? lo : (v > hi ? hi : v);
}

static float MySoftClip(float x){
    if(x >  1.5f) return  1.0f;
    if(x < -1.5f) return -1.0f;
    return x - (x*x*x)/6.75f;
}

static float ApplyDist(float s, float drive, uint8_t mode){
    if(drive < 0.01f) return s;
    float d = 1.0f + drive * 15.0f;
    s *= d;
    switch(mode){
        case DMODE_SOFT: s = MySoftClip(s); break;
        case DMODE_HARD: s = clampF(s,-1.f,1.f); break;
        case DMODE_TUBE: s = tanhf(s); break;
        case DMODE_FUZZ:
            while(s >  1.f || s < -1.f){
                if(s >  1.f) s =  2.f - s;
                if(s < -1.f) s = -2.f - s;
            }
            break;
    }
    return s / d * (1.f + drive * 0.5f);
}

static float BitCrush(float s, uint8_t bits){
    if(bits >= 16) return s;
    float levels = (float)(1 << bits);
    return roundf(s * levels) / levels;
}

/* ═══════════════════════════════════════════════════════════════════
 *  20. TRIGGER
 * ═══════════════════════════════════════════════════════════════════ */
static void TriggerPad(uint8_t pad, uint8_t velocity,
                       uint8_t trkVol = 100, int8_t pan = 0,
                       uint32_t maxSamples = 0)
{
    if(pad >= MAX_PADS || !sampleLoaded[pad]) return;

    /* Find free slot or steal oldest */
    int slot = -1;
    for(int i = 0; i < MAX_VOICES; i++)
        if(!voices[i].active){ slot = i; break; }

    if(slot < 0){
        /* Voice stealing: prefer same pad, then oldest */
        uint32_t oldest = UINT32_MAX; int best = 0;
        for(int i = 0; i < MAX_VOICES; i++){
            if(voices[i].pad == pad){ best = i; break; }
            if(voices[i].age < oldest){ oldest = voices[i].age; best = i; }
        }
        slot = best;
    }

    uint32_t len = sampleLength[pad];
    if(maxSamples > 0 && maxSamples < len) len = maxSamples;

    float gain = (velocity / 127.0f) * (trkVol / 100.0f) * trackGain[pad];
    float panF = trackPanF[pad] + (pan / 100.0f);
    panF = clampF(panF, -1.0f, 1.0f);
    float gL = gain * (1.0f - clampF(panF, 0.f, 1.f));
    float gR = gain * (1.0f + clampF(panF, -1.f, 0.f));

    voices[slot].active = true;
    voices[slot].pad    = pad;
    voices[slot].pos    = padReverse[pad] ? (float)(sampleLength[pad] - 1) : 0.0f;
    voices[slot].speed  = padPitch[pad];
    voices[slot].gainL  = gL;
    voices[slot].gainR  = gR;
    voices[slot].age    = voiceAge++;
}

static uint8_t ActiveVoices(){
    uint8_t c = 0;
    for(int i = 0; i < MAX_VOICES; i++) if(voices[i].active) c++;
    return c;
}

/* ═══════════════════════════════════════════════════════════════════
 *  21. AUDIO CALLBACK
 * ═══════════════════════════════════════════════════════════════════ */
void AudioCallback(AudioHandle::InputBuffer  /*in*/,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    for(size_t i = 0; i < size; i++) out[0][i] = out[1][i] = 0.0f;

    if(kAudioSafeMode)
        return;

    float mixPeak = 0.0f;

    for(size_t i = 0; i < size; i++){
        float busL = 0, busR = 0;
        float reverbBusL = 0, delayBusL = 0, chorusBusL = 0;
        float sideSrc = 0;

        /* ── Render voices ── */
        for(int v = 0; v < MAX_VOICES; v++){
            Voice& vx = voices[v];
            if(!vx.active) continue;
            uint8_t p = vx.pad;

            /* Position / bounds */
            uint32_t idx = (uint32_t)fabsf(vx.pos);
            if(padReverse[p]){
                if(vx.pos < 0.0f){
                    if(padLoop[p]) vx.pos = (float)(sampleLength[p] - 1);
                    else { vx.active = false; continue; }
                }
            } else {
                if(idx >= sampleLength[p]){
                    if(padLoop[p]){ vx.pos = 0.0f; idx = 0; }
                    else { vx.active = false; continue; }
                }
            }
            idx = (uint32_t)fabsf(vx.pos);
            if(idx >= sampleLength[p]){ vx.active = false; continue; }

            /* Interpolation */
            float frac = fabsf(vx.pos) - idx;
            float s0   = sampleStorage[p][idx] / 32768.0f;
            float s1   = (idx + 1 < sampleLength[p])
                         ? sampleStorage[p][idx + 1] / 32768.0f : 0.0f;
            float s    = s0 + frac * (s1 - s0);

            /* ── Stutter ── */
            if(padStutterOn[p]){
                padStutterCnt[p]++;
                if(padStutterCnt[p] >= padStutterIval[p]){
                    padStutterCnt[p] = 0;
                    if(vx.pos > 100.f) vx.pos -= 100.f; else vx.pos = 0.f;
                }
            }

            /* ── Advance position ── */
            float adv = vx.speed;
            if(padScratchOn[p]){
                float tri = padScratchPhase[p] < 0.5f
                    ? padScratchPhase[p]*2.f : 2.f - padScratchPhase[p]*2.f;
                adv *= 1.f + (tri - 0.5f) * padScratchDepth[p];
                if(adv < 0.25f) adv = 0.25f;
                padScratchPhase[p] += padScratchRate[p] / (float)SR;
                if(padScratchPhase[p] >= 1.f) padScratchPhase[p] -= 1.f;
            }
            if(padTurnOn[p]){
                int8_t mode = padTurnMode[p];
                if(padTurnAuto[p]){
                    mode = (padTurnPhase[p] < 0.5f) ? 0 : 1;
                    padTurnPhase[p] += padTurnRate[p] / (float)SR;
                    if(padTurnPhase[p] >= 1.f) padTurnPhase[p] -= 1.f;
                }
                if(mode == 1){
                    float bsmp = padTurnBrakeMs[p] * (float)SR / 1000.f;
                    float envF = 1.f - clampF((float)padTurnCounter[p]/bsmp, 0.f, 1.f);
                    adv *= envF;
                    if(adv < 0.01f) adv = 0.01f;
                    padTurnCounter[p]++;
                } else if(mode == 2){
                    float bsmp = padTurnBackMs[p] * (float)SR / 1000.f;
                    if((padTurnCounter[p] % 3) == 0 && vx.pos > 0.f) vx.pos -= 1.f;
                    adv *= 0.7f;
                    padTurnCounter[p]++;
                    if(padTurnCounter[p] > (uint32_t)bsmp) padTurnCounter[p] = 0;
                } else {
                    padTurnCounter[p] = 0;
                }
            }
            vx.pos += padReverse[p] ? -adv : adv;

            /* ── Pad filter ── */
            if(padFilterType[p]){
                s = padFilter[p].Process(s);
            }

            /* ── Pad distortion + crush ── */
            s = ApplyDist(s, padDistDrive[p], DMODE_SOFT);
            s = BitCrush(s, padBitDepth[p]);

            /* ── Scratch FX ── */
            if(padScratchOn[p]){
                padScratchFilter[p].SetType(FTYPE_LOWPASS, padScratchCut[p], 0.707f, (float)SR);
                s = padScratchFilter[p].Process(s);
                if(padScratchCrackle[p] > 0.01f && (FastRand() & 0xFF) < (uint32_t)(padScratchCrackle[p]*64.f))
                    s += RandFloat() * 0.05f;
            }
            if(padTurnOn[p] && padTurnNoise[p] > 0.01f)
                s += RandFloat() * padTurnNoise[p] * 0.1f;

            /* ── Per-track filter ── */
            if(trkFilterType[p]){
                s = trkFilter[p].Process(s);
            }

            /* ── Per-track dist + crush ── */
            s = ApplyDist(s, trkDistDrive[p], trkDistMode[p]);
            s = BitCrush(s, trkBitDepth[p]);

            /* ── Per-track EQ (3-band) ── */
            if(trkEqLowDb[p])  s = trkEqLow[p].Process(s);
            if(trkEqMidDb[p])  s = trkEqMid[p].Process(s);
            if(trkEqHighDb[p]) s = trkEqHigh[p].Process(s);

            /* ── Per-track echo ── */
            if(trkEchoActive[p]){
                uint32_t d = (uint32_t)trkEchoDelay[p];
                if(d==0) d=1; if(d>=TRACK_ECHO_SIZE) d=TRACK_ECHO_SIZE-1;
                uint32_t rp = (trkEchoWp[p] + TRACK_ECHO_SIZE - d) % TRACK_ECHO_SIZE;
                float delayed = trkEchoBuf[p][rp];
                trkEchoBuf[p][trkEchoWp[p]] = clampF(s + delayed*trkEchoFb[p], -1.f, 1.f);
                s = s*(1.f - trkEchoMix[p]) + delayed*trkEchoMix[p];
                trkEchoWp[p] = (trkEchoWp[p] + 1) % TRACK_ECHO_SIZE;
            }

            /* ── Per-track flanger ── */
            if(trkFlgActive[p]){
                trkFlgBuf[p][trkFlgWp[p]] = s;
                float tri = trkFlgPhase[p] < 0.5f ? trkFlgPhase[p]*2.f : 2.f - trkFlgPhase[p]*2.f;
                uint32_t tap = 2 + (uint32_t)(tri * trkFlgDepth[p] * (float)TRACK_FLANGER_SIZE * 0.25f);
                if(tap >= TRACK_FLANGER_SIZE) tap = TRACK_FLANGER_SIZE - 1;
                uint32_t rp = (trkFlgWp[p] + TRACK_FLANGER_SIZE - tap) % TRACK_FLANGER_SIZE;
                float del = trkFlgBuf[p][rp];
                trkFlgBuf[p][trkFlgWp[p]] = clampF(s + del*trkFlgFb[p], -1.f, 1.f);
                s = s*(1.f - trkFlgMix[p]) + del*trkFlgMix[p];
                trkFlgWp[p] = (trkFlgWp[p] + 1) % TRACK_FLANGER_SIZE;
                trkFlgPhase[p] += trkFlgRate[p] / (float)SR;
                if(trkFlgPhase[p] >= 1.f) trkFlgPhase[p] -= 1.f;
            }

            /* ── Per-track compressor ── */
            if(trkCompActive[p]){
                float absS = fabsf(s);
                if(absS > trkCompEnv[p]) trkCompEnv[p] += (absS - trkCompEnv[p]) * 0.25f;
                else                     trkCompEnv[p] -= (trkCompEnv[p] - absS) * 0.03f;
                if(trkCompEnv[p] > trkCompThresh[p] && trkCompEnv[p] > 0.001f){
                    float g = trkCompThresh[p] / trkCompEnv[p];
                    g = powf(g, 1.f - 1.f/trkCompRatio[p]);
                    if(g < 0.125f) g = 0.125f;
                    s *= g;
                }
            }

            /* ── Sidechain ── */
            float absS = fabsf(s);
            if(scActive && p == scSrc) sideSrc = fmaxf(sideSrc, absS);
            if(scActive && p != scSrc && (scDstMask & (1u << p))){
                float duck = scAmount * scEnv;
                if(duck > 0.88f) duck = 0.88f;
                s *= (1.f - duck);
            }

            /* ── Mute / Solo ── */
            bool muted = trackMute[p];
            if(anySolo && !trackSolo[p]) muted = true;
            if(muted) s = 0;

            /* ── Apply voice gain → mix ── */
            float outL = s * vx.gainL;
            float outR = s * vx.gainR;

            /* ── Pan ── */
            float panL = (1.0f - trackPanF[p]) * 0.5f;
            float panR = (1.0f + trackPanF[p]) * 0.5f;
            busL += outL * panL;
            busR += outR * panR;

            /* ── Send buses ── */
            float mono = (outL + outR) * 0.5f;
            reverbBusL += mono * trackReverbSend[p];
            delayBusL  += mono * trackDelaySend[p];
            chorusBusL += mono * trackChorusSend[p];

            /* ── Track peak ── */
            float pk = fmaxf(fabsf(outL), fabsf(outR));
            if(pk > trackPeak[p]) trackPeak[p] = pk;
        }

        /* ── Sidechain envelope ── */
        if(scActive){
            if(sideSrc > scEnv) scEnv += (sideSrc - scEnv) * scAttackK;
            else                scEnv -= (scEnv - sideSrc) * scReleaseK;
        }

        /* ── DEMO MODE: tick del secuenciador ── */
        float demoFadeGain = 1.0f;
        if (demoModeActive)
            demoFadeGain = demoSeq.ProcessSample();

        /* ── SYNTH ENGINES (síntesis matemática) ── */
        float synthMix = 0.0f;
        if (synthActiveMask & (1 << SYNTH_ENGINE_808))
            synthMix += synth808.Process();
        if (synthActiveMask & (1 << SYNTH_ENGINE_909))
            synthMix += synth909.Process();
        if (kEnableSynth505 && (synthActiveMask & (1 << SYNTH_ENGINE_505)))
            synthMix += synth505.Process();
        if (synthActiveMask & (1 << SYNTH_ENGINE_303))
            synthMix += acid303.Process();

        /* Aplicar fade del demo mode */
        if (demoModeActive)
            synthMix *= demoFadeGain;

        /* Añadir synths al bus (mono → ambos canales) */
        busL += synthMix;
        busR += synthMix;

        /* ── MASTER FX CHAIN ── */
        float L = busL * masterGain;
        float R = busR * masterGain;

        /* ── Global filter ── */
        if(gFilterType != FTYPE_NONE){
            L = gFilterL.Process(L);
            R = gFilterR.Process(R);
        }

        /* ── Global bitcrush + distortion ── */
        L = BitCrush(L, gFilterBitDepth);
        R = BitCrush(R, gFilterBitDepth);
        L = ApplyDist(L, gFilterDist, gFilterDistMode);
        R = ApplyDist(R, gFilterDist, gFilterDistMode);

        /* ── Global SR reduce ── */
        if(gFilterSrReduce > 0 && gFilterSrReduce < (uint32_t)SR){
            uint32_t step = (uint32_t)SR / gFilterSrReduce;
            if(step < 1) step = 1;
            gSrCounter++;
            if(gSrCounter >= step){
                gSrCounter = 0;
                gSrHoldL = L; gSrHoldR = R;
            } else {
                L = gSrHoldL; R = gSrHoldR;
            }
        }

        /* ── Delay (with send bus input) ── */
        if(delayActive){
            float wet = masterDelay.Read();
            masterDelay.Write(L + delayBusL + wet * delayFeedback);
            L = L * (1.0f - delayMix) + wet * delayMix;
            R = R * (1.0f - delayMix) + wet * delayMix;
        }

        /* ── Compressor ── */
        if(compActive){
            L = masterComp.Process(L);
            R = masterComp.Process(R);
        }

        /* ── Wavefolder ── */
        if(waveFolderGain > 1.01f){
            masterFold.SetIncrement(waveFolderGain);
            L = masterFold.Process(L);
            R = masterFold.Process(R);
        }

        /* ── Phaser ── */
        if(phaserActive){
            L = masterPhaser.Process(L);
            /* Apply partially to R for stereo width */
            R = R * 0.7f + L * 0.3f;
        }

        /* ── Flanger (manual) ── */
        if(flangerActive){
            flangerBuf[flangerWp] = L;
            float tri = flangerPhase < 0.5f ? flangerPhase*2.f : 2.f - flangerPhase*2.f;
            uint32_t tap = 4 + (uint32_t)(tri * flangerDepth * 200.f);
            if(tap >= 4096) tap = 4095;
            uint32_t rp = (flangerWp + 4096 - tap) % 4096;
            float del = flangerBuf[rp];
            flangerBuf[flangerWp] = clampF(L + del*flangerFb, -1.f, 1.f);
            L = L*(1.f - flangerMix) + del*flangerMix;
            R = R*(1.f - flangerMix) + del*flangerMix;
            flangerWp = (flangerWp + 1) % 4096;
            flangerPhase += flangerRate / (float)SR;
            if(flangerPhase >= 1.f) flangerPhase -= 1.f;
        }

        /* ── Tremolo ── */
        if(tremoloActive){
            float t = masterTremolo.Process(1.0f);
            L *= t; R *= t;
        }

        /* ── Chorus (with send bus input) ── */
        if(chorusActive){
            float wet = masterChorus.Process(L + chorusBusL);
            L = L * (1.0f - chorusMix) + wet * chorusMix;
            R = R * (1.0f - chorusMix) + wet * chorusMix;
        }

        /* ── Reverb (with send bus input) ── */
        float revL = 0, revR = 0;
        if(reverbActive){
            masterReverb.Process(L + reverbBusL, R + reverbBusL,
                                &revL, &revR);
            L = L * (1.0f - reverbMix) + revL * reverbMix;
            R = R * (1.0f - reverbMix) + revR * reverbMix;
        }

        /* ── Limiter / Soft clip ── */
        if(limiterActive){
            L = clampF(L, -1.0f, 1.0f);
            R = clampF(R, -1.0f, 1.0f);
        } else {
            L = tanhf(L);
            R = tanhf(R);
        }

        out[0][i] = L;
        out[1][i] = R;

        float pk = fmaxf(fabsf(L), fabsf(R));
        if(pk > mixPeak) mixPeak = pk;
    }
    masterPeak = mixPeak;
}

/* ═══════════════════════════════════════════════════════════════════
 *  22. BUILD RESPONSE
 * ═══════════════════════════════════════════════════════════════════ */
static void BuildResponse(uint8_t cmd, uint16_t seq,
                          const uint8_t* payload, uint16_t payloadLen)
{
    SPIPacketHeader* r = (SPIPacketHeader*)txBuf;
    r->magic    = SPI_MAGIC_RESP;
    r->cmd      = cmd;
    r->length   = payloadLen;
    r->sequence = seq;
    r->checksum = payloadLen ? crc16(payload, payloadLen) : 0;
    if(payloadLen && payload) memcpy(txBuf + 8, payload, payloadLen);
    pendingTxLen    = 8 + payloadLen;
    pendingResponse = true;
    /* NUNCA transmitir desde ISR — se hace en main loop */
}

/* Forward declaration — definida more adelante en sección SD */
static bool LoadWavToPad(const char* filepath, uint8_t padIdx);

/* ═══════════════════════════════════════════════════════════════════
 *  23. PROCESS COMMAND  (ALL RED808 commands)
 * ═══════════════════════════════════════════════════════════════════ */
static void ProcessCommand()
{
    SPIPacketHeader* hdr = (SPIPacketHeader*)rxBuf;
    uint8_t* p = rxBuf + 8;
    uint16_t len = hdr->length;

    /* CRC check (skip for PING) */
    if(hdr->cmd != CMD_PING && len > 0){
        uint16_t calc = crc16(p, len);
        if(calc != hdr->checksum){ spiErrCnt++; return; }
    }
    spiPktCnt++;

    switch(hdr->cmd){

    /* ════════════════════════════════════════════
     *  PING
     * ════════════════════════════════════════════ */
    case CMD_PING: {
        uint32_t echo = 0, uptime = hw.system.GetNow();
        if(len >= 4) memcpy(&echo, p, 4);
        uint8_t pong[8];
        memcpy(pong,     &echo,   4);
        memcpy(pong + 4, &uptime, 4);
        BuildResponse(CMD_PING, hdr->sequence, pong, 8);
        return;
    }

    /* ════════════════════════════════════════════
     *  TRIGGERS
     * ════════════════════════════════════════════ */
    case CMD_TRIGGER_LIVE:
        if(len >= 2) TriggerPad(p[0], p[1]);
        break;

    case CMD_TRIGGER_SEQ:
        if(len >= 8){
            uint32_t maxS = 0; memcpy(&maxS, p + 4, 4);
            TriggerPad(p[0], p[1], p[2], (int8_t)p[3], maxS);
        }
        break;

    case CMD_TRIGGER_STOP:
        if(len >= 1)
            for(int v = 0; v < MAX_VOICES; v++)
                if(voices[v].active && voices[v].pad == p[0])
                    voices[v].active = false;
        break;

    case CMD_TRIGGER_STOP_ALL:
        for(int v = 0; v < MAX_VOICES; v++) voices[v].active = false;
        break;

    case CMD_TRIGGER_SIDECHAIN:
        if(len >= 3) scEnv = clampF(p[2] / 255.0f, 0.f, 1.f);
        break;

    /* ════════════════════════════════════════════
     *  VOLUME
     * ════════════════════════════════════════════ */
    case CMD_MASTER_VOLUME:
        if(len >= 1) masterGain = p[0] / 100.0f;
        break;
    case CMD_SEQ_VOLUME:
        if(len >= 1) seqVolume = p[0] / 100.0f;
        break;
    case CMD_LIVE_VOLUME:
        if(len >= 1) liveVolume = p[0] / 100.0f;
        break;
    case CMD_TRACK_VOLUME:
        if(len >= 2 && p[0] < MAX_PADS) trackGain[p[0]] = p[1] / 100.0f;
        break;
    case CMD_LIVE_PITCH:
        if(len >= 4){
            float pitch; memcpy(&pitch, p, 4);
            livePitch = clampF(pitch, 0.25f, 4.0f);
        }
        break;

    /* ════════════════════════════════════════════
     *  GLOBAL FILTER (0x20-0x26)
     * ════════════════════════════════════════════ */
    case CMD_FILTER_SET:
        if(len >= 20){
            gFilterType = p[0];
            memcpy(&gFilterCutoff, p + 2, 4);
            memcpy(&gFilterQ,      p + 6, 4);
            gFilterBitDepth = p[10];
            gFilterDistMode = p[11];
            memcpy(&gFilterDist,    p + 12, 4);
            memcpy(&gFilterSrReduce,p + 16, 4);
            gFilterCutoff = clampF(gFilterCutoff, 20.f, 20000.f);
            gFilterQ      = clampF(gFilterQ, 0.3f, 10.f);
            gFilterL.SetType(gFilterType, gFilterCutoff, gFilterQ, (float)SR);
            gFilterR.SetType(gFilterType, gFilterCutoff, gFilterQ, (float)SR);
        }
        break;
    case CMD_FILTER_CUTOFF:
        if(len >= 4){
            memcpy(&gFilterCutoff, p, 4);
            gFilterCutoff = clampF(gFilterCutoff, 20.f, 20000.f);
            if(gFilterType) {
                gFilterL.SetType(gFilterType, gFilterCutoff, gFilterQ, (float)SR);
                gFilterR.SetType(gFilterType, gFilterCutoff, gFilterQ, (float)SR);
            }
        }
        break;
    case CMD_FILTER_RESONANCE:
        if(len >= 4){
            memcpy(&gFilterQ, p, 4);
            gFilterQ = clampF(gFilterQ, 0.3f, 10.f);
            if(gFilterType) {
                gFilterL.SetType(gFilterType, gFilterCutoff, gFilterQ, (float)SR);
                gFilterR.SetType(gFilterType, gFilterCutoff, gFilterQ, (float)SR);
            }
        }
        break;
    case CMD_FILTER_BITDEPTH:
        if(len >= 1) gFilterBitDepth = (p[0] < 4) ? 4 : (p[0] > 16 ? 16 : p[0]);
        break;
    case CMD_FILTER_DISTORTION:
        if(len >= 4) memcpy(&gFilterDist, p, 4);
        break;
    case CMD_FILTER_DIST_MODE:
        if(len >= 1) gFilterDistMode = p[0];
        break;
    case CMD_FILTER_SR_REDUCE:
        if(len >= 4) memcpy(&gFilterSrReduce, p, 4);
        break;

    /* ════════════════════════════════════════════
     *  DELAY (0x30-0x33)
     * ════════════════════════════════════════════ */
    case CMD_DELAY_ACTIVE:
        if(len >= 1) delayActive = (p[0] != 0);
        break;
    case CMD_DELAY_TIME:
        if(len >= 2){
            uint16_t ms = 0; memcpy(&ms, p, 2);
            delayTime = (float)ms;
            masterDelay.SetDelay(delayTime / 1000.0f * (float)SR);
        }
        break;
    case CMD_DELAY_FEEDBACK:
        if(len >= 1) delayFeedback = p[0] / 100.0f;
        break;
    case CMD_DELAY_MIX:
        if(len >= 1) delayMix = p[0] / 100.0f;
        break;

    /* ════════════════════════════════════════════
     *  PHASER (0x34-0x37)
     * ════════════════════════════════════════════ */
    case CMD_PHASER_ACTIVE:
        if(len >= 1) phaserActive = (p[0] != 0);
        break;
    case CMD_PHASER_RATE:
        if(len >= 1) masterPhaser.SetFreq(p[0] / 10.0f);
        break;
    case CMD_PHASER_DEPTH:
        if(len >= 1) masterPhaser.SetLfoDepth(p[0] / 100.0f);
        break;
    case CMD_PHASER_FEEDBACK:
        if(len >= 1) masterPhaser.SetFeedback(p[0] / 100.0f);
        break;

    /* ════════════════════════════════════════════
     *  FLANGER (0x38-0x3C)
     * ════════════════════════════════════════════ */
    case CMD_FLANGER_ACTIVE:
        if(len >= 1) flangerActive = (p[0] != 0);
        break;
    case CMD_FLANGER_RATE:
        if(len >= 1) flangerRate = clampF(p[0] * 0.1f, 0.1f, 20.f);
        break;
    case CMD_FLANGER_DEPTH:
        if(len >= 1) flangerDepth = p[0] / 100.0f;
        break;
    case CMD_FLANGER_FEEDBACK:
        if(len >= 1) flangerFb = p[0] / 100.0f;
        break;
    case CMD_FLANGER_MIX:
        if(len >= 1) flangerMix = p[0] / 100.0f;
        break;

    /* ════════════════════════════════════════════
     *  COMPRESSOR (0x3D-0x42)
     * ════════════════════════════════════════════ */
    case CMD_COMP_ACTIVE:
        if(len >= 1) compActive = (p[0] != 0);
        break;
    case CMD_COMP_THRESHOLD:
        if(len >= 1) masterComp.SetThreshold(-((float)p[0]));
        break;
    case CMD_COMP_RATIO:
        if(len >= 1) masterComp.SetRatio((float)p[0]);
        break;
    case CMD_COMP_ATTACK:
        if(len >= 1) masterComp.SetAttack((float)p[0] / 1000.0f);
        break;
    case CMD_COMP_RELEASE:
        if(len >= 1) masterComp.SetRelease((float)p[0] / 1000.0f);
        break;
    case CMD_COMP_MAKEUP:
        if(len >= 1) masterComp.SetMakeup((float)p[0] / 10.0f);
        break;

    /* ════════════════════════════════════════════
     *  REVERB (0x43-0x46)
     * ════════════════════════════════════════════ */
    case CMD_REVERB_ACTIVE:
        if(len >= 1) reverbActive = (p[0] != 0);
        break;
    case CMD_REVERB_FEEDBACK:
        if(len >= 1){
            reverbFeedback = p[0] / 100.0f;
            masterReverb.SetFeedback(reverbFeedback);
        }
        break;
    case CMD_REVERB_LPFREQ:
        if(len >= 2){
            uint16_t f = 0; memcpy(&f, p, 2);
            reverbLpFreq = (float)f;
            masterReverb.SetLpFreq(reverbLpFreq);
        }
        break;
    case CMD_REVERB_MIX:
        if(len >= 1) reverbMix = p[0] / 100.0f;
        break;

    /* ════════════════════════════════════════════
     *  CHORUS (0x47-0x4A)
     * ════════════════════════════════════════════ */
    case CMD_CHORUS_ACTIVE:
        if(len >= 1) chorusActive = (p[0] != 0);
        break;
    case CMD_CHORUS_RATE:
        if(len >= 1) masterChorus.SetLfoFreq(p[0] / 10.0f);
        break;
    case CMD_CHORUS_DEPTH:
        if(len >= 1) masterChorus.SetLfoDepth(p[0] / 100.0f);
        break;
    case CMD_CHORUS_MIX:
        if(len >= 1) chorusMix = p[0] / 100.0f;
        break;

    /* ════════════════════════════════════════════
     *  TREMOLO (0x4B-0x4D)
     * ════════════════════════════════════════════ */
    case CMD_TREMOLO_ACTIVE:
        if(len >= 1) tremoloActive = (p[0] != 0);
        break;
    case CMD_TREMOLO_RATE:
        if(len >= 1) masterTremolo.SetFreq(p[0] / 10.0f);
        break;
    case CMD_TREMOLO_DEPTH:
        if(len >= 1) masterTremolo.SetDepth(p[0] / 100.0f);
        break;

    /* ════════════════════════════════════════════
     *  WAVEFOLDER + LIMITER (0x4E-0x4F)
     * ════════════════════════════════════════════ */
    case CMD_WAVEFOLDER_GAIN:
        if(len >= 1) waveFolderGain = p[0] / 10.0f;
        break;
    case CMD_LIMITER_ACTIVE:
        if(len >= 1) limiterActive = (p[0] != 0);
        break;

    /* ════════════════════════════════════════════
     *  PER-TRACK FX (0x50-0x65)
     * ════════════════════════════════════════════ */
    case CMD_TRACK_FILTER:
        if(len >= 20){
            uint8_t t = p[0]; if(t >= MAX_PADS) break;
            trkFilterType[t] = p[1];
            float cut, res;
            memcpy(&cut, p + 4, 4);
            memcpy(&res, p + 8, 4);
            trkFilterCut[t] = clampF(cut, 20.f, 20000.f);
            trkFilterQ[t]   = clampF(res, 0.3f, 10.f);
            trkFilter[t].SetType(p[1], trkFilterCut[t], trkFilterQ[t], (float)SR);
        }
        break;
    case CMD_TRACK_CLEAR_FILTER:
        if(len >= 1 && p[0] < MAX_PADS){
            trkFilterType[p[0]] = 0;
            trkFilter[p[0]].Reset();
        }
        break;
    case CMD_TRACK_DISTORTION:
        if(len >= 5 && p[0] < MAX_PADS){
            float d; memcpy(&d, p + 1, 4);
            trkDistDrive[p[0]] = clampF(d, 0.f, 1.f);
        } else if(len >= 2 && p[0] < MAX_PADS){
            trkDistDrive[p[0]] = p[1] / 255.0f;
        }
        break;
    case CMD_TRACK_BITCRUSH:
        if(len >= 2 && p[0] < MAX_PADS)
            trkBitDepth[p[0]] = (p[1] < 4) ? 4 : (p[1] > 16 ? 16 : p[1]);
        break;
    case CMD_TRACK_ECHO:
        if(len >= 16 && p[0] < MAX_PADS){
            uint8_t t = p[0];
            trkEchoActive[t] = (p[1] != 0);
            float timeMs, fb, mix;
            memcpy(&timeMs, p + 4, 4);
            memcpy(&fb,     p + 8, 4);
            memcpy(&mix,    p + 12, 4);
            trkEchoDelay[t] = clampF(timeMs * (float)SR / 1000.f, 1.f, (float)(TRACK_ECHO_SIZE-1));
            trkEchoFb[t]    = clampF(fb, 0.f, 0.95f);
            trkEchoMix[t]   = clampF(mix, 0.f, 1.f);
        }
        break;
    case CMD_TRACK_FLANGER_FX:
        if(len >= 16 && p[0] < MAX_PADS){
            uint8_t t = p[0];
            trkFlgActive[t] = (p[1] != 0);
            float depth, rate, fb, mix;
            memcpy(&depth, p + 4, 4);
            memcpy(&rate,  p + 8, 4);
            memcpy(&fb,    p + 12, 4);
            trkFlgDepth[t] = clampF(depth, 0.f, 1.f);
            trkFlgRate[t]  = clampF(rate, 0.1f, 20.f);
            trkFlgFb[t]    = clampF(fb, 0.f, 0.95f);
            trkFlgMix[t]   = clampF(mix, 0.f, 1.f);
        }
        break;
    case CMD_TRACK_COMPRESSOR:
        if(len >= 12 && p[0] < MAX_PADS){
            uint8_t t = p[0];
            trkCompActive[t] = (p[1] != 0);
            float thresh, ratio;
            memcpy(&thresh, p + 4, 4);
            memcpy(&ratio,  p + 8, 4);
            trkCompThresh[t] = clampF(thresh, 0.01f, 1.f);
            trkCompRatio[t]  = clampF(ratio, 1.f, 20.f);
        }
        break;
    case CMD_TRACK_CLEAR_LIVE:
        if(len >= 1 && p[0] < MAX_PADS){
            uint8_t t = p[0];
            trkEchoActive[t] = false;
            trkFlgActive[t]  = false;
            trkCompActive[t] = false;
            memset(trkEchoBuf[t], 0, sizeof(trkEchoBuf[t]));
            memset(trkFlgBuf[t],  0, sizeof(trkFlgBuf[t]));
        }
        break;
    case CMD_TRACK_CLEAR_FX:
        if(len >= 1 && p[0] < MAX_PADS){
            uint8_t t = p[0];
            trkFilterType[t] = 0;  trkFilter[t].Reset();
            trkDistDrive[t]  = 0;  trkDistMode[t] = 0;
            trkBitDepth[t]   = 16;
            trkEchoActive[t] = false; trkEchoWp[t] = 0;
            trkFlgActive[t]  = false; trkFlgWp[t] = 0;
            trkCompActive[t] = false; trkCompEnv[t] = 0;
            trackReverbSend[t] = 0; trackDelaySend[t] = 0;
            trackChorusSend[t] = 0;
            trackPanF[t] = 0; trackMute[t] = false; trackSolo[t] = false;
            trkEqLowDb[t] = 0; trkEqMidDb[t] = 0; trkEqHighDb[t] = 0;
            memset(trkEchoBuf[t], 0, sizeof(trkEchoBuf[t]));
            memset(trkFlgBuf[t],  0, sizeof(trkFlgBuf[t]));
        }
        break;

    /* ── Track Sends / Pan / Mute / Solo ── */
    case CMD_TRACK_REVERB_SEND:
        if(len >= 2 && p[0] < MAX_PADS)
            trackReverbSend[p[0]] = p[1] / 100.0f;
        break;
    case CMD_TRACK_DELAY_SEND:
        if(len >= 2 && p[0] < MAX_PADS)
            trackDelaySend[p[0]] = p[1] / 100.0f;
        break;
    case CMD_TRACK_CHORUS_SEND:
        if(len >= 2 && p[0] < MAX_PADS)
            trackChorusSend[p[0]] = p[1] / 100.0f;
        break;
    case CMD_TRACK_PAN:
        if(len >= 2 && p[0] < MAX_PADS)
            trackPanF[p[0]] = (int8_t)p[1] / 100.0f;
        break;
    case CMD_TRACK_MUTE:
        if(len >= 2 && p[0] < MAX_PADS)
            trackMute[p[0]] = (p[1] != 0);
        break;
    case CMD_TRACK_SOLO:
        if(len >= 2 && p[0] < MAX_PADS){
            trackSolo[p[0]] = (p[1] != 0);
            anySolo = false;
            for(int i = 0; i < MAX_PADS; i++)
                if(trackSolo[i]){ anySolo = true; break; }
        }
        break;

    /* ── Track EQ 3-band ── */
    case CMD_TRACK_EQ_LOW:
        if(len >= 2 && p[0] < MAX_PADS){
            trkEqLowDb[p[0]] = (int8_t)p[1];
            trkEqLow[p[0]].SetType(FTYPE_LOWSHELF, 200.f, 0.707f, (float)SR,
                                    (float)(int8_t)p[1]);
        }
        break;
    case CMD_TRACK_EQ_MID:
        if(len >= 2 && p[0] < MAX_PADS){
            trkEqMidDb[p[0]] = (int8_t)p[1];
            trkEqMid[p[0]].SetType(FTYPE_PEAKING, 1000.f, 1.0f, (float)SR,
                                   (float)(int8_t)p[1]);
        }
        break;
    case CMD_TRACK_EQ_HIGH:
        if(len >= 2 && p[0] < MAX_PADS){
            trkEqHighDb[p[0]] = (int8_t)p[1];
            trkEqHigh[p[0]].SetType(FTYPE_HIGHSHELF, 4000.f, 0.707f, (float)SR,
                                    (float)(int8_t)p[1]);
        }
        break;

    /* ── Track Phaser / Tremolo / Pitch / Gate (stubs — state stored) ── */
    case CMD_TRACK_PHASER:
    case CMD_TRACK_TREMOLO:
    case CMD_TRACK_PITCH:
    case CMD_TRACK_GATE:
        /* TODO: implement per-track phaser/tremolo/pitch/gate DSP */
        break;

    /* ════════════════════════════════════════════
     *  PER-PAD FX (0x70-0x7A)
     * ════════════════════════════════════════════ */
    case CMD_PAD_FILTER:
        if(len >= 12 && p[0] < MAX_PADS){
            uint8_t pad = p[0];
            padFilterType[pad] = p[1];
            float cut, res;
            memcpy(&cut, p + 4, 4);
            memcpy(&res, p + 8, 4);
            padFilterCut[pad] = clampF(cut, 20.f, 20000.f);
            padFilterQ[pad]   = clampF(res, 0.3f, 10.f);
            padFilter[pad].SetType(p[1], padFilterCut[pad], padFilterQ[pad], (float)SR);
        }
        break;
    case CMD_PAD_CLEAR_FILTER:
        if(len >= 1 && p[0] < MAX_PADS){
            padFilterType[p[0]] = 0;
            padFilter[p[0]].Reset();
        }
        break;
    case CMD_PAD_DISTORTION:
        if(len >= 5 && p[0] < MAX_PADS){
            float d; memcpy(&d, p + 1, 4);
            padDistDrive[p[0]] = clampF(d, 0.f, 1.f);
        } else if(len >= 2 && p[0] < MAX_PADS){
            padDistDrive[p[0]] = p[1] / 255.0f;
        }
        break;
    case CMD_PAD_BITCRUSH:
        if(len >= 2 && p[0] < MAX_PADS)
            padBitDepth[p[0]] = (p[1] < 4) ? 4 : (p[1] > 16 ? 16 : p[1]);
        break;
    case CMD_PAD_LOOP:
        if(len >= 2 && p[0] < MAX_PADS)
            padLoop[p[0]] = (p[1] != 0);
        break;
    case CMD_PAD_REVERSE:
        if(len >= 2 && p[0] < MAX_PADS)
            padReverse[p[0]] = (p[1] != 0);
        break;
    case CMD_PAD_PITCH:
        if(len >= 3 && p[0] < MAX_PADS){
            int16_t cents = 0; memcpy(&cents, p + 1, 2);
            padPitch[p[0]] = powf(2.0f, cents / 1200.0f);
        }
        break;
    case CMD_PAD_STUTTER:
        if(len >= 4 && p[0] < MAX_PADS){
            padStutterOn[p[0]] = (p[1] != 0);
            uint16_t ival; memcpy(&ival, p + 2, 2);
            padStutterIval[p[0]] = (ival < 20) ? 20 : (ival > 2000 ? 2000 : ival);
        }
        break;
    case CMD_PAD_SCRATCH:
        if(len >= 20 && p[0] < MAX_PADS){
            uint8_t pad = p[0];
            padScratchOn[pad] = (p[1] != 0);
            float rate, depth, cut, crackle;
            memcpy(&rate,    p + 4,  4);
            memcpy(&depth,   p + 8,  4);
            memcpy(&cut,     p + 12, 4);
            memcpy(&crackle, p + 16, 4);
            padScratchRate[pad]    = clampF(rate, 0.5f, 20.f);
            padScratchDepth[pad]   = clampF(depth, 0.f, 1.f);
            padScratchCut[pad]     = clampF(cut, 200.f, 16000.f);
            padScratchCrackle[pad] = clampF(crackle, 0.f, 1.f);
        }
        break;
    case CMD_PAD_TURNTABLISM:
        if(len >= 16 && p[0] < MAX_PADS){
            uint8_t pad = p[0];
            padTurnOn[pad]   = (p[1] != 0);
            padTurnAuto[pad] = (p[2] != 0);
            padTurnMode[pad] = (int8_t)p[3];
            uint16_t brk, bck;
            memcpy(&brk, p + 4, 2);
            memcpy(&bck, p + 6, 2);
            padTurnBrakeMs[pad] = (brk < 20) ? 20 : (brk > 2000 ? 2000 : brk);
            padTurnBackMs[pad]  = (bck < 20) ? 20 : (bck > 2000 ? 2000 : bck);
            float tRate, noise;
            memcpy(&tRate, p + 8,  4);
            memcpy(&noise, p + 12, 4);
            padTurnRate[pad]  = clampF(tRate, 0.2f, 30.f);
            padTurnNoise[pad] = clampF(noise, 0.f, 1.f);
        }
        break;
    case CMD_PAD_CLEAR_FX:
        if(len >= 1 && p[0] < MAX_PADS){
            uint8_t pad = p[0];
            padFilterType[pad] = 0; padFilter[pad].Reset();
            padDistDrive[pad]  = 0; padBitDepth[pad] = 16;
            padLoop[pad] = false; padReverse[pad] = false; padPitch[pad] = 1.0f;
            padStutterOn[pad] = false; padScratchOn[pad] = false; padTurnOn[pad] = false;
        }
        break;

    /* ════════════════════════════════════════════
     *  SIDECHAIN (0x90-0x91)
     * ════════════════════════════════════════════ */
    case CMD_SIDECHAIN_SET:
        if(len >= 20){
            scActive = true;
            scSrc = p[0];
            memcpy(&scDstMask, p + 2, 2);
            memcpy(&scAmount,    p + 4, 4);
            memcpy(&scAttackK,   p + 8, 4);
            memcpy(&scReleaseK,  p + 12, 4);
            /* p+16: knee (ignored for now) */
        }
        break;
    case CMD_SIDECHAIN_CLEAR:
        scActive = false; scEnv = 0;
        break;

    /* ════════════════════════════════════════════
     *  SAMPLE TRANSFER (0xA0-0xA4)
     * ════════════════════════════════════════════ */
    case CMD_SAMPLE_BEGIN:
        if(len >= 12){
            uint8_t pad = p[0];
            if(pad < MAX_PADS){
                uint32_t ts = 0; memcpy(&ts, p + 8, 4);
                sampleTotalSamples[pad] = ts;
                sampleLength[pad] = 0;
                sampleLoaded[pad] = false;
            }
        }
        break;

    case CMD_SAMPLE_DATA:
        if(len >= 8){
            uint8_t pad = p[0];
            uint16_t chunkSize = 0; uint32_t offset = 0;
            memcpy(&chunkSize, p + 2, 2);
            memcpy(&offset,    p + 4, 4);
            uint32_t startSample = offset / 2;
            uint16_t numSamples  = chunkSize / 2;
            if(pad < MAX_PADS && startSample + numSamples <= MAX_SAMPLE_BYTES / 2)
                memcpy(&sampleStorage[pad][startSample], p + 8, chunkSize);
        }
        break;

    case CMD_SAMPLE_END:
        if(len >= 1){
            uint8_t pad = p[0];
            if(pad < MAX_PADS){
                sampleLength[pad] = sampleTotalSamples[pad];
                sampleLoaded[pad] = true;
            }
        }
        break;

    case CMD_SAMPLE_UNLOAD:
        if(len >= 1 && p[0] < MAX_PADS){
            sampleLoaded[p[0]] = false;
            sampleLength[p[0]] = 0;
        }
        break;

    case CMD_SAMPLE_UNLOAD_ALL:
        for(int i = 0; i < MAX_PADS; i++){
            sampleLoaded[i] = false; sampleLength[i] = 0;
        }
        for(int v = 0; v < MAX_VOICES; v++) voices[v].active = false;
        break;

    /* ════════════════════════════════════════════
     *  SD CARD (0xB0-0xB9)
     * ════════════════════════════════════════════ */
    case CMD_SD_KIT_LIST: {
        SdKitListResponse resp;
        memset(&resp, 0, sizeof(resp));
        DIR dir; FILINFO fno;
        /* List kit folders inside /data (any directory counts as a kit) */
        char root[16];
        snprintf(root, sizeof(root), "%s", SD_DATA_ROOT);
        if(sdPresent && f_opendir(&dir, root) == FR_OK){
            while(f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0){
                if(!(fno.fattrib & AM_DIR)) continue;
                /* Skip single-instrument family folders (2-char names) and xtra */
                size_t nlen = strlen(fno.fname);
                bool isFamily = (nlen <= 2);
                bool isXtra   = (strcasecmp(fno.fname, "xtra") == 0);
                if(!isFamily && !isXtra && resp.count < 16){
                    strncpy(resp.kits[resp.count], fno.fname, 31);
                    resp.count++;
                }
            }
            f_closedir(&dir);
        }
        BuildResponse(CMD_SD_KIT_LIST, hdr->sequence,
                      (uint8_t*)&resp, 1 + resp.count * 32);
        return;
    }

    case CMD_SD_LOAD_KIT: {
        if(len >= sizeof(SdLoadKitPayload)){
            SdLoadKitPayload lk;
            memcpy(&lk, p, sizeof(lk));
            char path[96];
            snprintf(path, sizeof(path), "%s/%s", SD_DATA_ROOT, lk.kitName);
            DIR dir; FILINFO fno;
            uint8_t padIdx = lk.startPad;
            uint8_t maxIdx = lk.startPad + lk.maxPads;
            if(maxIdx > MAX_PADS) maxIdx = MAX_PADS;
            if(sdPresent && f_opendir(&dir, path) == FR_OK){
                while(f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0
                      && padIdx < maxIdx){
                    if(fno.fattrib & AM_DIR) continue;
                    size_t flen = strlen(fno.fname);
                    if(flen < 4) continue;
                    const char* ext = fno.fname + flen - 4;
                    if(ext[0] != '.' || (ext[1]!='w' && ext[1]!='W')) continue;
                    char fpath[160];
                    snprintf(fpath, sizeof(fpath), "%s/%s", path, fno.fname);
                    if(LoadWavToPad(fpath, padIdx)) padIdx++;
                }
                f_closedir(&dir);
                strncpy(currentKitName, lk.kitName, 31);
                hw.PrintLine("SD: Kit '%s' loaded pads %d-%d",
                               lk.kitName, lk.startPad, padIdx-1);
                /* Notify Master */
                uint32_t mask = 0;
                for(int i = lk.startPad; i < padIdx; i++)
                    if(sampleLoaded[i]) mask |= (1u << i);
                PushEvent(EVT_SD_KIT_LOADED, padIdx - lk.startPad,
                          mask, lk.kitName);
            }
        }
        break;
    }

    case CMD_SD_STATUS: {
        SdStatusResponse resp;
        memset(&resp, 0, sizeof(resp));
        resp.present = sdPresent ? 1 : 0;
        for(int i = 0; i < MAX_PADS && i < 16; i++)
            if(sampleLoaded[i]) resp.samplesLoaded |= (1 << i);
        strncpy(resp.currentKit, currentKitName, 31);
        BuildResponse(CMD_SD_STATUS, hdr->sequence,
                      (uint8_t*)&resp, sizeof(resp));
        return;
    }

    case CMD_SD_UNLOAD_KIT:
        for(int i = 0; i < MAX_PADS; i++){
            sampleLoaded[i] = false; sampleLength[i] = 0;
        }
        for(int v = 0; v < MAX_VOICES; v++) voices[v].active = false;
        PushEvent(EVT_SD_KIT_UNLOADED, 0, 0, currentKitName);
        currentKitName[0] = 0;
        break;

    case CMD_SD_GET_LOADED: {
        uint8_t resp[4] = {};
        for(int i = 0; i < MAX_PADS && i < 24; i++)
            if(sampleLoaded[i]) resp[i/8] |= (1 << (i%8));
        BuildResponse(CMD_SD_GET_LOADED, hdr->sequence, resp, 4);
        return;
    }

    case CMD_SD_LIST_FOLDERS: {
        /* List all subdirectories inside /data */
        SdKitListResponse resp;   /* reuse: count + names[16][32] */
        memset(&resp, 0, sizeof(resp));
        DIR dir; FILINFO fno;
        if(sdPresent && f_opendir(&dir, SD_DATA_ROOT) == FR_OK){
            while(f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0){
                if((fno.fattrib & AM_DIR) && resp.count < 16){
                    strncpy(resp.kits[resp.count], fno.fname, 31);
                    resp.count++;
                }
            }
            f_closedir(&dir);
        }
        BuildResponse(CMD_SD_LIST_FOLDERS, hdr->sequence,
                      (uint8_t*)&resp, 1 + resp.count * 32);
        return;
    }

    case CMD_SD_LIST_FILES: {
        /* List .wav files in a given subfolder of /data */
        SdListFilesResponse resp;
        memset(&resp, 0, sizeof(resp));
        if(len >= sizeof(SdListFilesPayload)){
            SdListFilesPayload pl;
            memcpy(&pl, p, sizeof(pl));
            pl.folder[31] = 0;
            char path[96];
            snprintf(path, sizeof(path), "%s/%s", SD_DATA_ROOT, pl.folder);
            DIR dir; FILINFO fno;
            if(sdPresent && f_opendir(&dir, path) == FR_OK){
                while(f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0
                      && resp.count < 20){
                    if(fno.fattrib & AM_DIR) continue;
                    size_t flen = strlen(fno.fname);
                    if(flen < 4) continue;
                    const char* ext = fno.fname + flen - 4;
                    if(ext[0]=='.' && (ext[1]=='w'||ext[1]=='W')){
                        strncpy(resp.files[resp.count], fno.fname, 31);
                        resp.count++;
                    }
                }
                f_closedir(&dir);
            }
        }
        BuildResponse(CMD_SD_LIST_FILES, hdr->sequence,
                      (uint8_t*)&resp, 1 + resp.count * 32);
        return;
    }

    case CMD_SD_FILE_INFO: {
        SdFileInfoResponse resp;
        memset(&resp, 0, sizeof(resp));
        if(len >= sizeof(SdFileInfoPayload)){
            SdFileInfoPayload pl;
            memcpy(&pl, p, sizeof(pl));
            pl.folder[31] = 0; pl.filename[31] = 0;
            char path[160];
            snprintf(path, sizeof(path), "%s/%s/%s",
                     SD_DATA_ROOT, pl.folder, pl.filename);
            FIL fil;
            if(sdPresent && f_open(&fil, path, FA_READ) == FR_OK){
                resp.sizeBytes = f_size(&fil);
                uint8_t wh[44]; UINT br;
                if(f_read(&fil, wh, 44, &br)==FR_OK && br>=44
                   && memcmp(wh,"RIFF",4)==0){
                    resp.channels       = wh[22];
                    resp.sampleRate     = wh[24]|(wh[25]<<8);
                    resp.bitsPerSample  = wh[34]|(wh[35]<<8);
                    uint32_t dataBytes  = resp.sizeBytes > 44 ? resp.sizeBytes-44 : 0;
                    uint32_t bytesPerSec= wh[28]|(wh[29]<<8)|(wh[30]<<16)|(wh[31]<<24);
                    if(bytesPerSec > 0)
                        resp.durationMs = (uint32_t)((uint64_t)dataBytes*1000/bytesPerSec);
                }
                f_close(&fil);
            }
        }
        BuildResponse(CMD_SD_FILE_INFO, hdr->sequence,
                      (uint8_t*)&resp, sizeof(resp));
        return;
    }

    case CMD_SD_LOAD_SAMPLE: {
        /* Load a specific .wav file into a specific pad */
        if(len >= sizeof(SdLoadSamplePayload)){
            SdLoadSamplePayload pl;
            memcpy(&pl, p, sizeof(pl));
            pl.folder[31] = 0; pl.filename[31] = 0;
            char path[160];
            snprintf(path, sizeof(path), "%s/%s/%s",
                     SD_DATA_ROOT, pl.folder, pl.filename);
            if(pl.padIdx < MAX_PADS){
                bool ok = LoadWavToPad(path, pl.padIdx);
                hw.PrintLine("SD: Load '%s' → pad %d: %s",
                               pl.filename, pl.padIdx, ok?"OK":"FAIL");
                if(ok){
                    PushEvent(EVT_SD_SAMPLE_LOADED, 1,
                              1u << pl.padIdx, pl.filename);
                } else {
                    PushEvent(EVT_SD_ERROR, 0,
                              1u << pl.padIdx, pl.filename);
                }
            }
        }
        break;
    }

    case CMD_SD_ABORT:
        /* Abort any ongoing SD transfer (currently unused) */
        break;

    /* ════════════════════════════════════════════
     *  STATUS / QUERY (0xE0-0xE3)
     * ════════════════════════════════════════════ */
    case CMD_GET_PEAKS: {
        float buf[17];
        for(int i = 0; i < 16; i++){
            buf[i] = trackPeak[i];
            trackPeak[i] = 0.0f;
        }
        buf[16] = masterPeak;
        BuildResponse(CMD_GET_PEAKS, hdr->sequence, (uint8_t*)buf, 68);
        return;
    }

    case CMD_GET_STATUS: {
        /* Expanded: 20 bytes base + 1 byte eventCount + 32 bytes currentKit */
        uint8_t resp[54]; memset(resp, 0, sizeof(resp));
        resp[0] = ActiveVoices();
        resp[1] = 0; /* CPU % — TODO: implement with DWT */
        /* resp[2-3]: loaded bitmask pads 0-15 */
        for(int i = 0; i < 8; i++)
            if(sampleLoaded[i]) resp[2] |= (1 << i);
        for(int i = 8; i < 16; i++)
            if(sampleLoaded[i]) resp[3] |= (1 << (i-8));
        /* resp[4-7]: uptime ms */
        uint32_t up = hw.system.GetNow();
        memcpy(resp + 4, &up, 4);
        /* resp[8]: SD present */
        resp[8] = sdPresent ? 1 : 0;
        /* resp[9]: loaded bitmask pads 16-23 (XTRA) */
        for(int i = 16; i < 24; i++)
            if(sampleLoaded[i]) resp[9] |= (1 << (i-16));
        /* resp[10]: pending event count → Master sabe si debe llamar CMD_GET_EVENTS */
        resp[10] = evtCount;
        /* resp[11-13]: reserved */
        /* resp[14-45]: currentKitName (32 chars) */
        strncpy((char*)(resp + 14), currentKitName, 31);
        /* resp[46-53]: total loaded sample count + total sample bytes (info) */
        uint8_t totalLoaded = 0;
        uint32_t totalBytes = 0;
        for(int i = 0; i < MAX_PADS; i++){
            if(sampleLoaded[i]){
                totalLoaded++;
                totalBytes += sampleLength[i] * 2;
            }
        }
        resp[46] = totalLoaded;
        memcpy(resp + 47, &totalBytes, 4);
        /* resp[51]: MAX_PADS */
        resp[51] = MAX_PADS;
        /* resp[52-53]: reserved */
        BuildResponse(CMD_GET_STATUS, hdr->sequence, resp, 54);
        return;
    }

    case CMD_GET_CPU_LOAD: {
        uint8_t pct = 0; /* TODO: real measurement */
        BuildResponse(CMD_GET_CPU_LOAD, hdr->sequence, &pct, 1);
        return;
    }

    case CMD_GET_VOICES: {
        uint8_t cnt = ActiveVoices();
        BuildResponse(CMD_GET_VOICES, hdr->sequence, &cnt, 1);
        return;
    }

    case CMD_GET_EVENTS: {
        /* Drain pending events → Master receives up to 4 events per call.
         * Response: [count(1)] + [NotifyEvent(32)] * count
         * El Master llama repetidamente hasta que count == 0. */
        NotifyEvent evts[4];
        uint8_t n = PopEvents(evts, 4);
        uint8_t buf[1 + 4 * 32];
        buf[0] = n;
        if(n > 0) memcpy(buf + 1, evts, n * sizeof(NotifyEvent));
        BuildResponse(CMD_GET_EVENTS, hdr->sequence, buf, 1 + n * 32);
        return;
    }

    /* ════════════════════════════════════════════
     *  RESET
     * ════════════════════════════════════════════ */
    case CMD_RESET:
        for(int v = 0; v < MAX_VOICES; v++) voices[v].active = false;
        for(int i = 0; i < MAX_PADS; i++){
            sampleLoaded[i] = false; sampleLength[i] = 0;
            trackGain[i]    = 1.0f;  trackPeak[i] = 0;
            padLoop[i] = false; padReverse[i] = false; padPitch[i] = 1.0f;
            padFilterType[i] = 0; padDistDrive[i] = 0; padBitDepth[i] = 16;
            padStutterOn[i] = false; padScratchOn[i] = false; padTurnOn[i] = false;
            trkFilterType[i] = 0; trkDistDrive[i] = 0; trkBitDepth[i] = 16;
            trkEchoActive[i] = false; trkFlgActive[i] = false; trkCompActive[i] = false;
            trackReverbSend[i] = 0; trackDelaySend[i] = 0; trackChorusSend[i] = 0;
            trackPanF[i] = 0; trackMute[i] = false; trackSolo[i] = false;
            trkEqLowDb[i] = 0; trkEqMidDb[i] = 0; trkEqHighDb[i] = 0;
        }
        masterGain = 1.0f; seqVolume = 1.0f; liveVolume = 1.0f; livePitch = 1.0f;
        delayActive = false; reverbActive = false; chorusActive = false;
        tremoloActive = false; compActive = false; phaserActive = false;
        flangerActive = false; waveFolderGain = 1.0f; limiterActive = false;
        gFilterType = FTYPE_NONE; gFilterBitDepth = 16; gFilterDist = 0;
        scActive = false; scEnv = 0;
        anySolo = false;
        masterPeak = 0;
        spiPktCnt = 0; spiErrCnt = 0;
        /* Reset synth engines */
        synth808.Init((float)SR);
        synth909.Init((float)SR);
        synth505.Init((float)SR);
        acid303.Init((float)SR);
        synthActiveMask = 0x0B;
        break;

    /* ════════════════════════════════════════════
     *  BULK (0xF0-0xF1)
     * ════════════════════════════════════════════ */
    /* ════════════════════════════════════════════
     *  SYNTH ENGINES (0xC0-0xC5)
     * ════════════════════════════════════════════ */
    case CMD_SYNTH_TRIGGER:
        if(len >= 3){
            uint8_t engine = p[0];
            uint8_t instrument = p[1];
            float velocity = p[2] / 127.0f;
            switch(engine){
                case SYNTH_ENGINE_808: synth808.Trigger(instrument, velocity); break;
                case SYNTH_ENGINE_909: synth909.Trigger(instrument, velocity); break;
                case SYNTH_ENGINE_505: synth505.Trigger(instrument, velocity); break;
            }
        }
        break;

    case CMD_SYNTH_PARAM:
        if(len >= 7){
            uint8_t engine = p[0];
            uint8_t instrument = p[1];
            uint8_t paramId = p[2];
            float val; memcpy(&val, p + 3, 4);
            /* paramId: 0=decay, 1=pitch, 2=tone, 3=volume, 4=snappy */
            switch(engine){
                case SYNTH_ENGINE_808:
                    switch(instrument){
                        case TR808::INST_KICK:
                            if(paramId==0) synth808.kick.SetDecay(val);
                            if(paramId==1) synth808.kick.SetPitch(val);
                            if(paramId==2) synth808.kick.saturation = clampF(val,0.f,1.f);
                            if(paramId==3) synth808.kick.volume = clampF(val,0.f,1.f);
                            break;
                        case TR808::INST_SNARE:
                            if(paramId==0) synth808.snare.SetDecay(val);
                            if(paramId==2) synth808.snare.SetTone(val);
                            if(paramId==3) synth808.snare.volume = clampF(val,0.f,1.f);
                            if(paramId==4) synth808.snare.SetSnappy(val);
                            break;
                        case TR808::INST_CLAP:
                            if(paramId==0) synth808.clap.SetDecay(val);
                            if(paramId==3) synth808.clap.volume = clampF(val,0.f,1.f);
                            break;
                        case TR808::INST_HIHAT_C:
                            if(paramId==0) synth808.hihatC.SetDecay(val);
                            if(paramId==3) synth808.hihatC.volume = clampF(val,0.f,1.f);
                            break;
                        case TR808::INST_HIHAT_O:
                            if(paramId==0) synth808.hihatO.SetDecay(val);
                            if(paramId==3) synth808.hihatO.volume = clampF(val,0.f,1.f);
                            break;
                        case TR808::INST_COWBELL:
                            if(paramId==0) synth808.cowbell.SetDecay(val);
                            if(paramId==3) synth808.cowbell.volume = clampF(val,0.f,1.f);
                            break;
                        case TR808::INST_CYMBAL:
                            if(paramId==0) synth808.cymbal.SetDecay(val);
                            if(paramId==3) synth808.cymbal.volume = clampF(val,0.f,1.f);
                            break;
                        default:
                            /* Toms, congas: paramId 0=decay, 3=volume */
                            break;
                    }
                    break;
                case SYNTH_ENGINE_909:
                    switch(instrument){
                        case TR909::INST_KICK:
                            if(paramId==0) synth909.kick.SetDecay(val);
                            if(paramId==1) synth909.kick.SetPitch(val);
                            if(paramId==3) synth909.kick.volume = clampF(val,0.f,1.f);
                            break;
                        case TR909::INST_SNARE:
                            if(paramId==0) synth909.snare.SetDecay(val);
                            if(paramId==2) synth909.snare.SetTone(val);
                            if(paramId==3) synth909.snare.volume = clampF(val,0.f,1.f);
                            if(paramId==4) synth909.snare.SetSnappy(val);
                            break;
                        default: break;
                    }
                    break;
                case SYNTH_ENGINE_505:
                    /* 505 param handling similar */
                    break;
            }
        }
        break;

    case CMD_SYNTH_NOTE_ON:
        if(len >= 3){
            uint8_t note = p[0];
            bool accent = (p[1] != 0);
            bool slide  = (p[2] != 0);
            acid303.NoteOn(note, accent, slide);
        }
        break;

    case CMD_SYNTH_NOTE_OFF:
        acid303.NoteOff();
        break;

    case CMD_SYNTH_303_PARAM:
        if(len >= 5){
            uint8_t paramId = p[0];
            float val; memcpy(&val, p + 1, 4);
            switch(paramId){
                case 0: acid303.SetCutoff(val);    break;
                case 1: acid303.SetResonance(val);  break;
                case 2: acid303.SetEnvMod(val);     break;
                case 3: acid303.SetDecay(val);      break;
                case 4: acid303.SetAccent(val);     break;
                case 5: acid303.SetSlide(val);      break;
                case 6: acid303.SetWaveform(val < 0.5f ? TB303::WAVE_SAW : TB303::WAVE_SQUARE); break;
                case 7: acid303.volume = clampF(val, 0.f, 1.f); break;
            }
        }
        break;

    case CMD_SYNTH_ACTIVE:
        if(len >= 1) synthActiveMask = p[0];
        break;

    case CMD_BULK_TRIGGERS:
        if(len >= 2){
            uint8_t count = p[0];
            for(uint8_t i = 0; i < count && (1 + i*2 + 1) < len; i++)
                TriggerPad(p[1 + i*2], p[1 + i*2 + 1]);
        }
        break;

    case CMD_BULK_FX:
        if(len >= 1){
            uint8_t cnt = p[0]; uint16_t off = 1;
            for(uint8_t j = 0; j < cnt; j++){
                if(off + 2 > len) break;
                uint8_t subCmd = p[off]; uint8_t subLen = p[off+1]; off += 2;
                if(off + subLen > len) break;
                /* Build temp header + process */
                SPIPacketHeader tmpHdr;
                tmpHdr.magic = SPI_MAGIC_CMD;
                tmpHdr.cmd = subCmd;
                tmpHdr.length = subLen;
                tmpHdr.sequence = hdr->sequence;
                tmpHdr.checksum = crc16(p + off, subLen);
                /* Temporarily swap rxBuf content for recursive processing */
                uint8_t savedCmd = hdr->cmd;
                hdr->cmd = subCmd;
                hdr->length = subLen;
                /* Move sub-payload to p+8 position (it's already at p+off) */
                if(off != 8) memmove(rxBuf + 8, p + off, subLen);
                hdr->cmd = savedCmd;
                hdr->length = len;
                /* Simple inline: just call the sub-handler directly */
                off += subLen;
            }
        }
        break;

    default: break;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  24. SPI DMA CALLBACK
 * ═══════════════════════════════════════════════════════════════════ */
static void SpiRxCallback(void* context, SpiHandle::Result result)
{
    if(result != SpiHandle::Result::OK){
        spiErrCnt++;
        spi_slave.DmaReceive(rxBuf, 8, nullptr, SpiRxCallback, nullptr);
        return;
    }
    SPIPacketHeader* hdr = (SPIPacketHeader*)rxBuf;

    if(!waitingPayload){
        if(hdr->magic != SPI_MAGIC_CMD){
            spiErrCnt++;
            spi_slave.DmaReceive(rxBuf, 8, nullptr, SpiRxCallback, nullptr);
            return;
        }
        if(hdr->length > 0 && hdr->length <= (RX_BUF_SIZE - 8)){
            waitingPayload = true;
            spi_slave.DmaReceive(rxBuf + 8, hdr->length, nullptr,
                                 SpiRxCallback, nullptr);
            return;
        }
    }
    waitingPayload = false;
    /* Desactivar demo mode al recibir primer comando SPI real */
    if (demoModeActive) {
        demoModeActive = false;
        acid303.NoteOff();
    }
    ProcessCommand();
    if(!pendingResponse)
        spi_slave.DmaReceive(rxBuf, 8, nullptr, SpiRxCallback, nullptr);
}

/* ═══════════════════════════════════════════════════════════════════
 *  25. SD CARD INIT (SPI3 master) + AUTO-LOAD
 * ═══════════════════════════════════════════════════════════════════ */
static bool InitSD()
{
    /* ── CS pin (D0 = PB12) as GPIO output, start HIGH ── */
    sd_cs.Init(hw.GetPin(0), GPIO::Mode::OUTPUT, GPIO::Pull::NOPULL);
    SD_CS_HIGH();

    /* ── SPI3 master — slow for card init (<=400 kHz) ── */
    SpiHandle::Config sc;
    sc.periph         = SpiHandle::Config::Peripheral::SPI_3;
    sc.mode           = SpiHandle::Config::Mode::MASTER;
    sc.direction      = SpiHandle::Config::Direction::TWO_LINES;
    sc.datasize       = 8;
    sc.clock_polarity = SpiHandle::Config::ClockPolarity::LOW;
    sc.clock_phase    = SpiHandle::Config::ClockPhase::ONE_EDGE;
    sc.nss            = SpiHandle::Config::NSS::SOFT;
    sc.baud_prescaler = SpiHandle::Config::BaudPrescaler::PS_256;  /* ~400 kHz */
    sc.pin_config.sclk = hw.GetPin(2);   /* D2 = PC10 */
    sc.pin_config.miso = hw.GetPin(1);   /* D1 = PC11 */
    sc.pin_config.mosi = hw.GetPin(6);   /* D6 = PC12 */
    sc.pin_config.nss  = Pin();            /* CS manual via GPIO */
    sd_spi.Init(sc);

    /* ── Register SPI SD driver with FatFS ── */
    char sdPath[4];
    FATFS_LinkDriver(&SPISD_Driver, sdPath);

    /* ── Mount filesystem ── */
    FRESULT fr = f_mount(&sdFatFs, sdPath, 1);
    if(fr == FR_OK){
        sdPresent = true;
        /* ── Switch to fast SPI for data transfer ── */
        sc.baud_prescaler = SpiHandle::Config::BaudPrescaler::PS_8;   /* ~12 MHz */
        sd_spi.Init(sc);
        return true;
    }
    sdPresent = false;
    return false;
}

/* Try to load the first .wav from a directory directly into pad slot */
static bool LoadWavToPad(const char* filepath, uint8_t padIdx)
{
    if(padIdx >= MAX_PADS) return false;
    FIL fil;
    if(f_open(&fil, filepath, FA_READ) != FR_OK) return false;

    /* Simple WAV header parse: find "data" chunk */
    uint8_t hdr[44];
    UINT br;
    if(f_read(&fil, hdr, 44, &br) != FR_OK || br < 44){
        f_close(&fil); return false;
    }
    if(memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr+8, "WAVE", 4) != 0){
        f_close(&fil); return false;
    }

    uint16_t ch  = hdr[22] | (hdr[23]<<8);
    uint32_t sr  = hdr[24]|(hdr[25]<<8)|(hdr[26]<<16)|(hdr[27]<<24);
    uint16_t bps = hdr[34] | (hdr[35]<<8);
    (void)sr;

    /* Skip to data chunk */
    uint32_t pos = 12;
    f_lseek(&fil, 12);
    uint32_t dataSize = 0;
    while(pos < f_size(&fil) - 8){
        uint8_t ck[8];
        if(f_read(&fil, ck, 8, &br) != FR_OK || br < 8) break;
        uint32_t ckSz = ck[4]|(ck[5]<<8)|(ck[6]<<16)|(ck[7]<<24);
        if(memcmp(ck, "data", 4) == 0){
            dataSize = ckSz;
            break;
        }
        f_lseek(&fil, f_tell(&fil) + ckSz);
        pos += 8 + ckSz;
    }
    if(dataSize == 0){ f_close(&fil); return false; }

    uint32_t bytesPerFrame = (bps/8) * ch;
    if(bytesPerFrame == 0){ f_close(&fil); return false; }
    uint32_t totalFrames = dataSize / bytesPerFrame;
    if(totalFrames > MAX_SAMPLE_BYTES / 2) totalFrames = MAX_SAMPLE_BYTES / 2;

    /* Read and convert to mono 16-bit */
    if(bps == 16 && ch == 1){
        /* Optimal: direct read */
        f_read(&fil, sampleStorage[padIdx], totalFrames * 2, &br);
        sampleLength[padIdx] = br / 2;
    } else {
        /* Convert: read in chunks */
        uint8_t buf[512];
        uint32_t frames = 0;
        while(frames < totalFrames){
            uint32_t want = (totalFrames - frames) * bytesPerFrame;
            if(want > sizeof(buf)) want = sizeof(buf);
            if(f_read(&fil, buf, want, &br) != FR_OK || br == 0) break;
            uint32_t got = br / bytesPerFrame;
            for(uint32_t i = 0; i < got && frames < totalFrames; i++){
                const uint8_t* s = buf + i * bytesPerFrame;
                int32_t sample = 0;
                if(bps == 16){
                    sample = (int16_t)(s[0]|(s[1]<<8));
                    if(ch == 2) sample = (sample + (int16_t)(s[2]|(s[3]<<8))) / 2;
                } else if(bps == 24){
                    sample = (int32_t)(((uint32_t)s[0]<<8)|((uint32_t)s[1]<<16)|((uint32_t)s[2]<<24));
                    sample >>= 16;
                    if(ch == 2){
                        int32_t s2 = (int32_t)(((uint32_t)s[3]<<8)|((uint32_t)s[4]<<16)|((uint32_t)s[5]<<24));
                        sample = (sample + (s2>>16)) / 2;
                    }
                } else if(bps == 8){
                    sample = ((int32_t)s[0] - 128) * 256;
                    if(ch == 2) sample = (sample + ((int32_t)s[1]-128)*256) / 2;
                }
                sampleStorage[padIdx][frames++] =
                    (int16_t)(sample < -32768 ? -32768 : (sample > 32767 ? 32767 : sample));
            }
        }
        sampleLength[padIdx] = frames;
    }

    sampleLoaded[padIdx] = (sampleLength[padIdx] > 0);
    f_close(&fil);
    return sampleLoaded[padIdx];
}

/* ── Helper: case-insensitive substring match ─────────────────── */
static bool containsCI(const char* haystack, const char* needle)
{
    for(const char* h = haystack; *h; h++){
        const char* hp = h;
        const char* np = needle;
        while(*np && *hp && (toupper((uint8_t)*hp) == toupper((uint8_t)*np))){
            hp++; np++;
        }
        if(!*np) return true;
    }
    return false;
}

/* ── Helper: guess pad index from a filename using keyword table ── */
static int GuessPadFromFilename(const char* fname)
{
    /* Try each keyword — longer keywords checked implicitly because
       the table is ordered from most-specific to least-specific */
    for(int k = 0; k < NUM_INSTR_KEYWORDS; k++){
        if(containsCI(fname, INSTR_KEYWORDS[k].keyword))
            return INSTR_KEYWORDS[k].pad;
    }
    return -1;
}

/* ── Helper: check if .wav extension ─────────────────────────── */
static bool isWavFile(const char* fname)
{
    size_t len = strlen(fname);
    if(len < 4) return false;
    const char* ext = fname + len - 4;
    return (ext[0] == '.') && (ext[1]=='w'||ext[1]=='W')
        && (ext[2]=='a'||ext[2]=='A') && (ext[3]=='v'||ext[3]=='V');
}

/* Auto-load default kit from SD at boot */
static void AutoLoadFromSD()
{
    if(!sdPresent) return;

    /* ── PHASE 1: Load LIVE PADS 0-15 from default kit ─────────── */
    /* Try "RED 808 KARZ" first, then any folder in /data          */
    static const char* defaultKitNames[] = {
        "RED 808 KARZ", nullptr
    };

    bool liveLoaded = false;

    for(int k = 0; defaultKitNames[k]; k++){
        char kitPath[96];
        snprintf(kitPath, sizeof(kitPath), "%s/%s", SD_DATA_ROOT, defaultKitNames[k]);
        DIR dir;
        if(f_opendir(&dir, kitPath) != FR_OK) continue;

        /* Pass 1: smart-map by instrument keyword */
        bool padUsed[16] = {};
        FILINFO fno;
        uint8_t loaded = 0;
        while(f_readdir(&dir, &fno) == FR_OK && fno.fname[0]){
            if(fno.fattrib & AM_DIR) continue;
            if(!isWavFile(fno.fname)) continue;
            int pad = GuessPadFromFilename(fno.fname);
            if(pad >= 0 && pad < 16 && !padUsed[pad]){
                char fpath[192];
                snprintf(fpath, sizeof(fpath), "%s/%s", kitPath, fno.fname);
                if(LoadWavToPad(fpath, (uint8_t)pad)){
                    padUsed[pad] = true;
                    loaded++;
                }
            }
        }
        f_closedir(&dir);

        /* Pass 2: any remaining .wav files → first free pad slot */
        if(f_opendir(&dir, kitPath) == FR_OK){
            while(f_readdir(&dir, &fno) == FR_OK && fno.fname[0]){
                if(fno.fattrib & AM_DIR) continue;
                if(!isWavFile(fno.fname)) continue;
                /* Already loaded this file by keyword? Check by trying to
                   find an empty slot for overflow/duplicate instruments */
                int pad = GuessPadFromFilename(fno.fname);
                if(pad >= 0 && pad < 16 && padUsed[pad]){
                    /* This instrument already has a sample — find free slot */
                    int free = -1;
                    for(int s = 0; s < 16; s++){
                        if(!padUsed[s]){ free = s; break; }
                    }
                    if(free >= 0){
                        char fpath[192];
                        snprintf(fpath, sizeof(fpath), "%s/%s", kitPath, fno.fname);
                        if(LoadWavToPad(fpath, (uint8_t)free)){
                            padUsed[free] = true;
                            loaded++;
                        }
                    }
                } else if(pad < 0){
                    /* Unknown instrument — put in first free slot */
                    int free = -1;
                    for(int s = 0; s < 16; s++){
                        if(!padUsed[s]){ free = s; break; }
                    }
                    if(free >= 0){
                        char fpath[192];
                        snprintf(fpath, sizeof(fpath), "%s/%s", kitPath, fno.fname);
                        if(LoadWavToPad(fpath, (uint8_t)free)){
                            padUsed[free] = true;
                            loaded++;
                        }
                    }
                }
            }
            f_closedir(&dir);
        }

        if(loaded > 0){
            strncpy(currentKitName, defaultKitNames[k], 31);
            hw.PrintLine("SD: Loaded %d LIVE PADS from '%s'",
                           loaded, defaultKitNames[k]);
            /* Build pad mask for event */
            uint32_t bootMask = 0;
            for(int i = 0; i < 16; i++)
                if(sampleLoaded[i]) bootMask |= (1u << i);
            PushEvent(EVT_SD_BOOT_DONE, loaded, bootMask,
                      defaultKitNames[k]);
            liveLoaded = true;
            break;
        }
    }

    /* Fallback if default kit not found: try first directory with WAVs */
    if(!liveLoaded){
        DIR root; FILINFO fno;
        if(f_opendir(&root, SD_DATA_ROOT) == FR_OK){
            while(f_readdir(&root, &fno) == FR_OK && fno.fname[0]){
                if(!(fno.fattrib & AM_DIR)) continue;
                if(strlen(fno.fname) <= 2) continue;  /* skip family folders */
                if(strcasecmp(fno.fname, "xtra") == 0) continue;
                char kitPath[96];
                snprintf(kitPath, sizeof(kitPath), "%s/%s", SD_DATA_ROOT, fno.fname);
                DIR kdir; FILINFO kfno;
                uint8_t padIdx = 0;
                if(f_opendir(&kdir, kitPath) == FR_OK){
                    while(f_readdir(&kdir, &kfno) == FR_OK && kfno.fname[0]
                          && padIdx < 16){
                        if(kfno.fattrib & AM_DIR) continue;
                        if(!isWavFile(kfno.fname)) continue;
                        char fpath[192];
                        snprintf(fpath, sizeof(fpath), "%s/%s", kitPath, kfno.fname);
                        if(LoadWavToPad(fpath, padIdx)) padIdx++;
                    }
                    f_closedir(&kdir);
                }
                if(padIdx > 0){
                    strncpy(currentKitName, fno.fname, 31);
                    hw.PrintLine("SD: Fallback loaded %d LIVE PADS from '%s'",
                                   padIdx, fno.fname);
                    uint32_t fbMask = 0;
                    for(int i = 0; i < padIdx; i++)
                        if(sampleLoaded[i]) fbMask |= (1u << i);
                    PushEvent(EVT_SD_BOOT_DONE, padIdx, fbMask, fno.fname);
                    liveLoaded = true;
                    break;
                }
            }
            f_closedir(&root);
        }
    }

    /* ── PHASE 2: Load XTRA PADS 16-23 from /data/xtra ─────────── */
    {
        char xtraPath[48];
        snprintf(xtraPath, sizeof(xtraPath), "%s/xtra", SD_DATA_ROOT);
        DIR dir; FILINFO fno;
        uint8_t xtraIdx = 16;  /* pads 16-23 */
        if(f_opendir(&dir, xtraPath) == FR_OK){
            while(f_readdir(&dir, &fno) == FR_OK && fno.fname[0]
                  && xtraIdx < MAX_PADS){
                if(fno.fattrib & AM_DIR) continue;
                if(!isWavFile(fno.fname)) continue;
                char fpath[160];
                snprintf(fpath, sizeof(fpath), "%s/%s", xtraPath, fno.fname);
                if(LoadWavToPad(fpath, xtraIdx)) xtraIdx++;
            }
            f_closedir(&dir);
            if(xtraIdx > 16){
                hw.PrintLine("SD: Loaded %d XTRA PADS from /data/xtra",
                               xtraIdx - 16);
                uint32_t xtraMask = 0;
                for(int i = 16; i < xtraIdx; i++)
                    if(sampleLoaded[i]) xtraMask |= (1u << i);
                PushEvent(EVT_SD_XTRA_LOADED, xtraIdx - 16,
                          xtraMask, "xtra");
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  26. INIT HELPERS
 * ═══════════════════════════════════════════════════════════════════ */
static void InitArrays()
{
    for(int i = 0; i < MAX_PADS; i++){
        sampleLoaded[i] = false;
        sampleLength[i] = 0;
        sampleTotalSamples[i] = 0;
        trackGain[i]  = 1.0f;
        trackPeak[i]  = 0.0f;
        padLoop[i]    = false;
        padReverse[i] = false;
        padPitch[i]   = 1.0f;
        padFilterType[i] = 0;
        padFilterCut[i]  = 10000.f;
        padFilterQ[i]    = 0.707f;
        padDistDrive[i]  = 0;
        padBitDepth[i]   = 16;
        padStutterOn[i]  = false;
        padScratchOn[i]  = false;
        padTurnOn[i]     = false;
        trackReverbSend[i] = 0;
        trackDelaySend[i]  = 0;
        trackChorusSend[i] = 0;
        trackPanF[i]  = 0;
        trackMute[i]  = false;
        trackSolo[i]  = false;
        trkFilterType[i] = 0;
        trkFilterCut[i]  = 10000.f;
        trkFilterQ[i]    = 0.707f;
        trkDistDrive[i]  = 0;
        trkDistMode[i]   = 0;
        trkBitDepth[i]   = 16;
        trkEchoActive[i] = false;
        trkEchoWp[i] = 0;
        trkFlgActive[i]  = false;
        trkFlgWp[i] = 0;
        trkCompActive[i] = false;
        trkCompThresh[i] = 0.6f;
        trkCompRatio[i]  = 4.0f;
        trkCompEnv[i]    = 0;
        trkEqLowDb[i]  = 0;
        trkEqMidDb[i]  = 0;
        trkEqHighDb[i] = 0;
    }
    for(int i = 0; i < MAX_VOICES; i++) voices[i].active = false;
}

static void InitFX()
{
    float sr = (float)SR;

    masterDelay.Init();
    masterDelay.SetDelay(sr * 0.25f);

    masterReverb.Init(sr);
    masterReverb.SetFeedback(0.6f);
    masterReverb.SetLpFreq(8000.0f);

    masterChorus.Init(sr);
    masterChorus.SetLfoFreq(0.3f);
    masterChorus.SetLfoDepth(0.4f);
    masterChorus.SetDelay(0.75f);

    masterTremolo.Init(sr);
    masterTremolo.SetFreq(4.0f);
    masterTremolo.SetDepth(0.5f);
    masterTremolo.SetWaveform(Oscillator::WAVE_SIN);

    masterComp.Init(sr);
    masterComp.SetThreshold(-20.0f);
    masterComp.SetRatio(4.0f);
    masterComp.SetAttack(0.01f);
    masterComp.SetRelease(0.1f);
    masterComp.SetMakeup(1.0f);
    masterComp.AutoMakeup(true);

    masterFold.Init();
    masterFold.SetIncrement(1.0f);

    masterPhaser.Init(sr);
    masterPhaser.SetFreq(0.5f);
    masterPhaser.SetLfoDepth(0.4f);
    masterPhaser.SetFeedback(0.5f);

    memset(flangerBuf, 0, sizeof(flangerBuf));
    for(int i = 0; i < MAX_PADS; i++){
        memset(trkEchoBuf[i], 0, sizeof(trkEchoBuf[i]));
        memset(trkFlgBuf[i],  0, sizeof(trkFlgBuf[i]));
    }

    /* ── Synth Engines Init ── */
    synth808.Init(sr);
    synth909.Init(sr);
    synth505.Init(sr);
    acid303.Init(sr);

    /* ── Demo Mode Init ── */
    demoSeq.Init(sr, &synth808, &synth909, &acid303);
}

/* ═══════════════════════════════════════════════════════════════════
 *  27. MAIN
 * ═══════════════════════════════════════════════════════════════════ */
int main()
{
    /* ── Hardware init ── */
    hw.Init();

    if(kBootDiagMinimal)
    {
        bool led = false;
        while(1)
        {
            led = !led;
            hw.SetLed(led);
            for(volatile uint32_t d = 0; d < 900000; ++d)
                __asm__("nop");
        }
    }

    hw.SetAudioBlockSize(AUDIO_BLOCK);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);

    auto Log = [&](const char* fmt, auto... args)
    {
        if(kEnableStartLog)
            hw.PrintLine(fmt, args...);
    };

    /* USB serial debug */
    if(kEnableStartLog)
        hw.StartLog(true);
    Log("══════════════════════════════════════════");
    Log("  RED808 DrumMachine — Daisy Seed Slave");
    Log("  %d pads · %d voices · %d Hz · %d block",
        MAX_PADS, MAX_VOICES, SR, AUDIO_BLOCK);
    Log("  Synth: TR808 · TR909 · TR505 · TB303");
    Log("  DEMO MODE: auto-play 3 min");
    Log("══════════════════════════════════════════");

    /* ── Init state ── */
    InitArrays();
    if(kEnableInitFx)
        InitFX();

    /* ── SD Card DESHABILITADA temporalmente ── */
    // hw.PrintLine("Montando SD card (SPI3: D0=CS D2=SCK D1=MISO D6=MOSI)...");
    // bool sdOk = InitSD();
    // hw.PrintLine(sdOk ? "SD OK (SPI mode)" : "SD no encontrada");
    // if(sdOk) AutoLoadFromSD();
    bool sdOk = false;
    sdPresent = false;
    Log("SD card: DESHABILITADA (sin hardware)");

    /* ── Conteo de samples cargados ── */
    uint8_t loadedCount = 0;
    for(int i = 0; i < MAX_PADS; i++) if(sampleLoaded[i]) loadedCount++;
    Log("Samples cargados: %d / %d", loadedCount, MAX_PADS);

    if(kEnableSpiSlave)
    {
        /* ── SPI1 Slave (comunicación con ESP32-S3 Master) ── */
        Log("Iniciando SPI1 slave...");
        SpiHandle::Config spi_config;
        spi_config.periph         = SpiHandle::Config::Peripheral::SPI_1;
        spi_config.mode           = SpiHandle::Config::Mode::SLAVE;
        spi_config.direction      = SpiHandle::Config::Direction::TWO_LINES;
        spi_config.datasize       = 8;
        spi_config.clock_polarity = SpiHandle::Config::ClockPolarity::LOW;
        spi_config.clock_phase    = SpiHandle::Config::ClockPhase::ONE_EDGE;
        spi_config.nss            = SpiHandle::Config::NSS::HARD_INPUT;
        spi_config.baud_prescaler = SpiHandle::Config::BaudPrescaler::PS_128;
        spi_config.pin_config.sclk = hw.GetPin(8);     /* D8  = PG11 (SPI1_SCK)  */
        spi_config.pin_config.miso = hw.GetPin(9);     /* D9  = PB4  (SPI1_MISO) */
        spi_config.pin_config.mosi = hw.GetPin(10);    /* D10 = PB5  (SPI1_MOSI) */
        spi_config.pin_config.nss  = hw.GetPin(7);     /* D7  = PG10 (SPI1_NSS)  */
        spi_slave.Init(spi_config);

        spi_slave.DmaReceive(rxBuf, 8, nullptr, SpiRxCallback, nullptr);
        Log("SPI1 listo (D7=NSS D8=SCK D9=MISO D10=MOSI)");
    }
    else
    {
        Log("SPI1: DESHABILITADO (modo standalone demo)");
    }

    /* ── Start Audio ── */
    if(kEnableAudioStart)
    {
        Log("Iniciando audio @ %d Hz, %d samples/block", SR, AUDIO_BLOCK);
        hw.StartAudio(AudioCallback);
    }
    else
    {
        Log("Audio: DESHABILITADO (diagnostico StartAudio)");
    }

    /* LED = ready */
    hw.SetLed(true);
    Log(">>> RED808 DRUM MACHINE READY <<<");

    /* ── Main loop ── */
    uint32_t lastBlink = 0;
    bool ledState = true;

    while(1){
        /* ── SPI response (NUNCA desde ISR) ── */
        if(kEnableSpiSlave && pendingResponse){
            pendingResponse = false;
            spi_slave.DmaTransmit(txBuf, pendingTxLen, nullptr, nullptr, nullptr);
            System::Delay(1);
            spi_slave.DmaReceive(rxBuf, 8, nullptr, SpiRxCallback, nullptr);
        }

        /* ── Heartbeat LED ── */
        uint32_t now = hw.system.GetNow();
        if(now - lastBlink > 500){
            lastBlink = now;
            ledState = !ledState;
            hw.SetLed(ledState);
        }
    }
}
