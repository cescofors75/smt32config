/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : TR-808 demo sequencer (SD folders -> I2S2 PCM5102)
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"
#include <string.h>
#include <math.h>

I2S_HandleTypeDef hi2s2;
DMA_HandleTypeDef hdma_spi2_tx;
SD_HandleTypeDef hsd;
SPI_HandleTypeDef hspi1;

#define AUDIO_TX_WORDS       4096U
#define SD_READ_BYTES        4096U

#define DEBUG_LED_PORT       GPIOE
#define DEBUG_LED_PIN        GPIO_PIN_0
#define DEBUG_LED2_PORT      GPIOA
#define DEBUG_LED2_PIN       GPIO_PIN_6
#define DEBUG_LED3_PORT      GPIOD
#define DEBUG_LED3_PIN       GPIO_PIN_13

#define NUM_INSTRUMENTS      5U
#define MAX_SAMPLE_FRAMES    5000U
#define MAX_VOICES           10U

#define DEMO_SAMPLE_RATE     44100U
#define DEMO_BPM             92U
#define TOTAL_STEPS          64U
#define ARR_SECTIONS         8U
#define ARR_STEPS            (TOTAL_STEPS * ARR_SECTIONS)
#define DELAY_SAMPLES        6000U

#define SPI_MAGIC_CMD        0xA5U
#define SPI_MAGIC_RESP       0x5AU
#define SPI_MAGIC_SAMPLE     0xDAU
#define SPI_MAGIC_BULK       0xBBU

#define CMD_TRIGGER_SEQ      0x01U
#define CMD_TRIGGER_LIVE     0x02U
#define CMD_TRIGGER_STOP     0x03U
#define CMD_TRIGGER_STOP_ALL 0x04U
#define CMD_TRIGGER_SIDECHAIN 0x05U

#define CMD_MASTER_VOLUME    0x10U
#define CMD_SEQ_VOLUME       0x11U
#define CMD_LIVE_VOLUME      0x12U
#define CMD_TRACK_VOLUME     0x13U

#define CMD_FILTER_TYPE       0x20U
#define CMD_FILTER_CUTOFF     0x21U
#define CMD_FILTER_RESONANCE  0x22U
#define CMD_FILTER_BITDEPTH   0x23U
#define CMD_FILTER_DISTORTION 0x24U
#define CMD_FILTER_DIST_MODE  0x25U
#define CMD_FILTER_SR_REDUCE  0x26U

#define CMD_DELAY_ACTIVE     0x30U
#define CMD_DELAY_TIME       0x31U
#define CMD_DELAY_FEEDBACK   0x32U
#define CMD_DELAY_MIX        0x33U

#define CMD_PHASER_ACTIVE    0x34U
#define CMD_PHASER_RATE      0x35U
#define CMD_PHASER_DEPTH     0x36U
#define CMD_PHASER_FEEDBACK  0x37U

#define CMD_FLANGER_ACTIVE   0x38U
#define CMD_FLANGER_RATE     0x39U
#define CMD_FLANGER_DEPTH    0x3AU
#define CMD_FLANGER_FEEDBACK 0x3BU
#define CMD_FLANGER_MIX      0x3CU

#define CMD_COMP_ACTIVE      0x3DU
#define CMD_COMP_THRESHOLD   0x3EU
#define CMD_COMP_RATIO       0x3FU
#define CMD_COMP_ATTACK      0x40U
#define CMD_COMP_RELEASE     0x41U
#define CMD_COMP_MAKEUP      0x42U

#define CMD_TRACK_FILTER     0x50U
#define CMD_TRACK_CLEAR_FX   0x51U
#define CMD_TRACK_DISTORTION 0x52U
#define CMD_TRACK_BITCRUSH   0x53U
#define CMD_TRACK_ECHO       0x54U
#define CMD_TRACK_FLANGER_FX 0x55U
#define CMD_TRACK_COMPRESSOR 0x56U
#define CMD_TRACK_CLEAR_LIVE 0x57U

#define CMD_SIDECHAIN_SET    0x90U
#define CMD_SIDECHAIN_CLEAR  0x91U

#define CMD_SAMPLE_BEGIN      0xA0U
#define CMD_SAMPLE_DATA       0xA1U
#define CMD_SAMPLE_END        0xA2U
#define CMD_SAMPLE_UNLOAD     0xA3U
#define CMD_SAMPLE_UNLOAD_ALL 0xA4U

#define CMD_PAD_FILTER       0x70U
#define CMD_PAD_CLEAR_FX     0x71U
#define CMD_PAD_DISTORTION   0x72U
#define CMD_PAD_BITCRUSH     0x73U
#define CMD_PAD_LOOP         0x74U
#define CMD_PAD_REVERSE      0x75U
#define CMD_PAD_PITCH        0x76U
#define CMD_PAD_STUTTER      0x77U
#define CMD_PAD_SCRATCH      0x78U
#define CMD_PAD_TURNTABLISM  0x79U

#define CMD_BULK_TRIGGERS    0xF0U
#define CMD_BULK_FX          0xF1U

#define CMD_GET_STATUS       0xE0U
#define CMD_GET_PEAKS        0xE1U
#define CMD_GET_CPU_LOAD     0xE2U
#define CMD_GET_VOICES       0xE3U
#define CMD_PING             0xEEU
#define CMD_RESET            0xEFU

#define SPI_TRIG_Q_LEN       16U
#define SPI_MAX_PAYLOAD      600U
#define SPI_TX_Q_LEN         768U
#define TRACK_FX_BUF_SAMPLES 1024U
#define TRACK_PEAK_COUNT     16U

static uint8_t  sdReadBuf[SD_READ_BYTES];
static uint16_t i2sTxBufA[AUDIO_TX_WORDS];
static uint16_t i2sTxBufB[AUDIO_TX_WORDS];

typedef struct {
  uint32_t part_lba;
  uint32_t fat_lba;
  uint32_t data_lba;
  uint32_t root_cluster;
  uint32_t sectors_per_fat;
  uint8_t sectors_per_cluster;
} Fat32Info;

typedef struct {
  Fat32Info fs;
  uint32_t first_cluster;
  uint32_t current_cluster;
  uint32_t file_size;
  uint32_t pos;
  uint32_t sector_in_cluster;
  uint16_t sector_offset;
  uint8_t sector_buf[512];
  uint8_t sector_valid;
} FileCtx;

typedef struct {
  uint16_t channels;
  uint32_t sample_rate;
  uint16_t bits_per_sample;
  uint32_t data_size;
} WavInfo;

typedef struct {
  int16_t data[MAX_SAMPLE_FRAMES];
  uint32_t length;
  uint8_t loaded;
} InstrumentSample;

typedef struct {
  uint8_t active;
  uint8_t inst;
  uint32_t pos;
  uint16_t frac_q12;
  uint16_t step_q12;
  int16_t gain_q15;
  int8_t pan;
} Voice;

static InstrumentSample g_samples[NUM_INSTRUMENTS];
static Voice g_voices[MAX_VOICES];
static int16_t g_delayL[DELAY_SAMPLES];
static int16_t g_delayR[DELAY_SAMPLES];
static uint32_t g_delayIdx = 0;
static int32_t g_lpL = 0;
static int32_t g_lpR = 0;
static uint32_t g_step = 0;
static uint32_t g_songStep = 0;
static uint32_t g_samplesPerStep = 0;
static uint32_t g_samplesToNextStep = 1;
static uint16_t g_flangerPhase = 0;
static uint8_t g_fxFlangerOn = 0;
static uint8_t g_fxReverbBoost = 0;
static uint8_t g_fxSparkleOn = 0;

typedef struct {
  uint8_t pad;
  uint8_t vel;
} SpiTrigger;

typedef struct __attribute__((packed)) {
  uint8_t magic;
  uint8_t cmd;
  uint16_t length;
  uint16_t sequence;
  uint16_t checksum;
} SpiPacketHeader;

static volatile uint8_t g_spiRxByte = 0;
static volatile uint8_t g_spiTxByte = 0;

static volatile uint8_t g_spiHdrBuf[sizeof(SpiPacketHeader)];
static volatile uint16_t g_spiHdrIdx = 0;
static volatile uint8_t g_spiPayloadBuf[SPI_MAX_PAYLOAD];
static volatile uint16_t g_spiPayloadIdx = 0;
static volatile uint16_t g_spiPayloadLen = 0;
static volatile uint8_t g_spiCmd = 0;
static volatile uint16_t g_spiSeq = 0;
static volatile uint16_t g_spiChk = 0;
static volatile uint8_t g_spiState = 0;

static volatile SpiTrigger g_spiTrigQ[SPI_TRIG_Q_LEN];
static volatile uint8_t g_spiTrigHead = 0;
static volatile uint8_t g_spiTrigTail = 0;

static volatile uint8_t g_spiTxQ[SPI_TX_Q_LEN];
static volatile uint16_t g_spiTxHead = 0;
static volatile uint16_t g_spiTxTail = 0;

static volatile uint8_t g_masterVolume = 100;
static volatile uint8_t g_seqVolume = 100;
static volatile uint8_t g_liveVolume = 100;
static volatile uint8_t g_trackVolume[NUM_INSTRUMENTS] = {100,100,100,100,100};
static volatile uint8_t g_globalFilterType = 0;
static volatile uint8_t g_globalFilterCutQ8 = 200;
static volatile uint8_t g_globalFilterResQ8 = 32;
static volatile uint8_t g_globalBitDepth = 16;
static volatile uint8_t g_globalDistQ8 = 0;
static volatile uint8_t g_globalDistMode = 0;
static volatile uint8_t g_globalSrReduce = 1;
static int32_t g_globalFilterStateL = 0;
static int32_t g_globalFilterStateR = 0;
static uint8_t g_globalSrPhase = 0;
static int32_t g_globalSrHoldL = 0;
static int32_t g_globalSrHoldR = 0;
static volatile uint8_t g_delayActive = 1;
static volatile uint8_t g_delayMixQ8 = 128;
static volatile uint8_t g_delayFbQ8 = 96;
static volatile uint8_t g_flangerEnabled = 1;
static volatile uint8_t g_flangerDepth = 120;
static volatile uint8_t g_flangerMixQ8 = 64;
static volatile uint8_t g_phaserEnabled = 0;
static volatile uint8_t g_phaserDepthQ8 = 96;
static volatile uint8_t g_phaserFeedbackQ8 = 48;
static volatile uint8_t g_phaserRateStep = 2;
static volatile int16_t g_phaserLast = 0;
static volatile uint16_t g_phaserPhase = 0;

static volatile uint8_t g_masterCompEnabled = 0;
static volatile uint16_t g_masterCompThresholdQ15 = 20000;
static volatile uint8_t g_masterCompRatioQ8 = 64;
static volatile uint8_t g_masterCompAttackK = 64;
static volatile uint8_t g_masterCompReleaseK = 8;
static volatile uint16_t g_masterCompEnvQ15 = 0;
static volatile uint8_t g_masterCompMakeupQ8 = 255;

static volatile uint8_t g_cpuLoadPercent = 14;
static volatile uint16_t g_masterPeakQ15 = 0;
static volatile uint16_t g_trackPeakQ15[TRACK_PEAK_COUNT] = {0};
static volatile uint16_t g_spiErrorCount = 0;
static volatile uint32_t g_samplesLoadedMask = 0;

static volatile uint16_t g_instPitchQ12[NUM_INSTRUMENTS] = {4096,4096,4096,4096,4096};
static volatile uint8_t g_padLoopEnabled[NUM_INSTRUMENTS] = {0};
static volatile uint8_t g_padStutterEnabled[NUM_INSTRUMENTS] = {0};
static volatile uint16_t g_padStutterInterval[NUM_INSTRUMENTS] = {220,220,220,220,220};
static volatile uint16_t g_padStutterCount[NUM_INSTRUMENTS] = {0};
static volatile uint8_t g_trackFilterType[NUM_INSTRUMENTS] = {0};
static volatile uint8_t g_trackFilterCutQ8[NUM_INSTRUMENTS] = {200,200,200,200,200};
static volatile uint8_t g_trackFilterResQ8[NUM_INSTRUMENTS] = {32,32,32,32,32};
static int32_t g_trackFilterState[NUM_INSTRUMENTS] = {0};
static volatile uint8_t g_padFilterType[NUM_INSTRUMENTS] = {0};
static volatile uint8_t g_padFilterCutQ8[NUM_INSTRUMENTS] = {200,200,200,200,200};
static volatile uint8_t g_padFilterResQ8[NUM_INSTRUMENTS] = {32,32,32,32,32};
static int32_t g_padFilterState[NUM_INSTRUMENTS] = {0};
static volatile uint8_t g_trackDistQ8[NUM_INSTRUMENTS] = {0};
static volatile uint8_t g_trackBitDepth[NUM_INSTRUMENTS] = {16,16,16,16,16};
static volatile uint8_t g_padDistQ8[NUM_INSTRUMENTS] = {0};
static volatile uint8_t g_padBitDepth[NUM_INSTRUMENTS] = {16,16,16,16,16};
static volatile uint8_t g_padScratchActive[NUM_INSTRUMENTS] = {0};
static volatile uint16_t g_padScratchRateQ8[NUM_INSTRUMENTS] = {0};
static volatile uint8_t g_padScratchDepthQ8[NUM_INSTRUMENTS] = {0};
static volatile uint8_t g_padScratchCutQ8[NUM_INSTRUMENTS] = {128,128,128,128,128};
static volatile uint8_t g_padScratchCrackleQ8[NUM_INSTRUMENTS] = {0};
static uint16_t g_padScratchPhase[NUM_INSTRUMENTS] = {0};
static int32_t g_padScratchState[NUM_INSTRUMENTS] = {0};

static volatile uint8_t g_padTurnActive[NUM_INSTRUMENTS] = {0};
static volatile uint8_t g_padTurnAuto[NUM_INSTRUMENTS] = {0};
static volatile int8_t g_padTurnMode[NUM_INSTRUMENTS] = {0};
static volatile uint16_t g_padTurnBrakeMs[NUM_INSTRUMENTS] = {150,150,150,150,150};
static volatile uint16_t g_padTurnBackspinMs[NUM_INSTRUMENTS] = {120,120,120,120,120};
static volatile uint16_t g_padTurnRateQ8[NUM_INSTRUMENTS] = {0};
static volatile uint8_t g_padTurnNoiseQ8[NUM_INSTRUMENTS] = {0};
static uint16_t g_padTurnPhase[NUM_INSTRUMENTS] = {0};
static uint16_t g_padTurnCounter[NUM_INSTRUMENTS] = {0};

static volatile uint8_t g_abSeqTrimQ8 = 242U;
static volatile uint8_t g_abLiveTrimQ8 = 236U;
static volatile uint8_t g_abMasterTrimQ8 = 232U;
static uint32_t g_noiseState = 0x12345678U;

typedef struct {
  uint8_t active;
  uint8_t inst;
  uint32_t totalBytes;
  uint32_t receivedBytes;
} SampleUploadState;

static volatile SampleUploadState g_sampleUpload = {0};

typedef struct {
  uint8_t active;
  uint16_t delaySamples;
  uint8_t feedbackQ8;
  uint8_t mixQ8;
  uint16_t writePos;
} TrackEchoState;

typedef struct {
  uint8_t active;
  uint8_t depthQ8;
  uint8_t feedbackQ8;
  uint8_t mixQ8;
  uint16_t writePos;
  uint16_t phase;
} TrackFlangerState;

typedef struct {
  uint8_t active;
  uint16_t thresholdQ15;
  uint8_t ratioQ8;
  uint16_t envQ15;
} TrackCompState;

typedef struct {
  uint8_t active;
  uint8_t sourceTrack;
  uint16_t destinationMask;
  uint8_t amountQ8;
  uint8_t attackK;
  uint8_t releaseK;
  uint16_t envQ15;
} SidechainState;

static TrackEchoState g_trackEcho[NUM_INSTRUMENTS];
static TrackFlangerState g_trackFlanger[NUM_INSTRUMENTS];
static TrackCompState g_trackComp[NUM_INSTRUMENTS];
static SidechainState g_sidechain = {0};
static int16_t g_trackEchoBuf[NUM_INSTRUMENTS][TRACK_FX_BUF_SAMPLES];
static int16_t g_trackFlangerBuf[NUM_INSTRUMENTS][TRACK_FX_BUF_SAMPLES];

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_I2S2_Init(void);
static void MX_SPI1_Init(void);
static void MX_SDIO_SD_Init(void);

static void Debug_LED_Init(void);
static void Debug_LED_Blink(int times);

static int SD_ReadSectors(uint32_t lba, uint8_t *buf, uint32_t count);
static int FAT32_Init(Fat32Info *fs);
static uint32_t FAT32_ClusterToLba(const Fat32Info *fs, uint32_t cluster);
static int FAT32_NextCluster(const Fat32Info *fs, uint32_t cluster, uint32_t *next_cluster);
static int FAT32_FindRootDirCluster(const Fat32Info *fs, const char *dirName, uint32_t *cluster_out);
static int FAT32_OpenWavInDirByIndex(const Fat32Info *fs, uint32_t dir_cluster, uint32_t wav_index, FileCtx *file);

static int File_Read(FileCtx *file, uint8_t *dst, uint32_t bytes, uint32_t *read_bytes);
static int File_Skip(FileCtx *file, uint32_t bytes);
static int WAV_ReadHeader(FileCtx *file, WavInfo *wav);
static int LoadInstrumentFromFolder(const char *folderName, InstrumentSample *out);

static int16_t ClipS16(int32_t x);
static void TriggerVoice(uint8_t inst, int16_t gain_q15, int8_t pan);
static void ProcessSpiTriggers(void);
static uint16_t SpiCrc16(const uint8_t *data, uint16_t len);
static void SpiTxEnqueue(const uint8_t *data, uint16_t len);
static uint8_t SpiTxPopByte(void);
static void SpiEnqueueResponse(uint8_t cmd, uint16_t sequence, const uint8_t *payload, uint16_t len);
static void SpiQueueTrigger(uint8_t pad, uint8_t vel);
static void SpiHandleCommand(uint8_t cmd, const uint8_t *payload, uint16_t len, uint16_t seq);
static void SpiParseIncomingByte(uint8_t byte_in);
static void ProcessSequencerStep(void);
static void RenderDemoBuffer(uint16_t *dst, uint32_t words);

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)); }

static int32_t ApplySoftDist(int32_t sample, uint8_t amountQ8)
{
  if (amountQ8 == 0U) return sample;
  int32_t driveQ8 = 256 + ((int32_t)amountQ8 * 3);
  int32_t x = (sample * driveQ8) >> 8;
  int32_t absx = (x < 0) ? -x : x;
  int32_t y = (x * 32767) / (32767 + absx);
  return y;
}

static int32_t ApplyBitCrush(int32_t sample, uint8_t bits)
{
  if (bits >= 16U) return sample;
  if (bits < 4U) bits = 4U;
  int32_t shift = 16 - bits;
  int32_t s = ClipS16(sample);
  s = (s >> shift) << shift;
  return s;
}

static uint8_t CutoffHzToQ8(float cutoffHz)
{
  if (cutoffHz < 20.0f) cutoffHz = 20.0f;
  if (cutoffHz > 20000.0f) cutoffHz = 20000.0f;
  float norm = (cutoffHz - 20.0f) / (20000.0f - 20.0f);
  uint32_t q = (uint32_t)(norm * 255.0f);
  if (q > 255U) q = 255U;
  return (uint8_t)q;
}

static uint8_t ResonanceToQ8(float resonance)
{
  if (resonance < 0.1f) resonance = 0.1f;
  if (resonance > 30.0f) resonance = 30.0f;
  float norm = (resonance - 0.1f) / (30.0f - 0.1f);
  uint32_t q = (uint32_t)(norm * 255.0f);
  if (q > 255U) q = 255U;
  return (uint8_t)q;
}

static int32_t ApplyOnePoleFilter(int32_t sample, uint8_t type, uint8_t cutQ8, uint8_t resQ8, int32_t *state)
{
  if (type == 0U) return sample;
  int32_t alpha = 8 + ((int32_t)cutQ8 * 120);
  int32_t lp = *state + (((sample - *state) * alpha) >> 15);
  *state = lp;
  int32_t out = (type == 2U) ? (sample - lp) : lp;
  int32_t makeUp = 256 + ((int32_t)resQ8 >> 2);
  out = (out * makeUp) >> 8;
  return ClipS16(out);
}

static int32_t ApplyGlobalDistMode(int32_t sample)
{
  if (g_globalDistQ8 == 0U) return sample;
  switch (g_globalDistMode)
  {
    case 1U:
    {
      int32_t drive = 256 + ((int32_t)g_globalDistQ8 * 4);
      int32_t x = (sample * drive) >> 8;
      if (x > 22000) x = 22000;
      if (x < -22000) x = -22000;
      return x;
    }
    case 2U:
      return (sample * (256 + (int32_t)g_globalDistQ8)) >> 8;
    case 3U:
    {
      int32_t x = ApplySoftDist(sample, g_globalDistQ8);
      int32_t a = (x < 0) ? -x : x;
      if (a < 600) x = 0;
      return x;
    }
    default:
      return ApplySoftDist(sample, g_globalDistQ8);
  }
}

static uint32_t FastRandU32(void)
{
  g_noiseState = (g_noiseState * 1664525U) + 1013904223U;
  return g_noiseState;
}

static void Debug_LED_Init(void)
{
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  GPIO_InitTypeDef gi = {0};
  gi.Mode = GPIO_MODE_OUTPUT_PP;
  gi.Pull = GPIO_NOPULL;
  gi.Speed = GPIO_SPEED_FREQ_LOW;

  gi.Pin = DEBUG_LED_PIN;
  HAL_GPIO_Init(DEBUG_LED_PORT, &gi);
  gi.Pin = DEBUG_LED2_PIN;
  HAL_GPIO_Init(DEBUG_LED2_PORT, &gi);
  gi.Pin = DEBUG_LED3_PIN;
  HAL_GPIO_Init(DEBUG_LED3_PORT, &gi);
}

static void Debug_LED_Blink(int times)
{
  for (int i = 0; i < times; i++)
  {
    HAL_GPIO_WritePin(DEBUG_LED_PORT, DEBUG_LED_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(DEBUG_LED2_PORT, DEBUG_LED2_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(DEBUG_LED3_PORT, DEBUG_LED3_PIN, GPIO_PIN_SET);
    HAL_Delay(120);
    HAL_GPIO_WritePin(DEBUG_LED_PORT, DEBUG_LED_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(DEBUG_LED2_PORT, DEBUG_LED2_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(DEBUG_LED3_PORT, DEBUG_LED3_PIN, GPIO_PIN_RESET);
    HAL_Delay(120);
  }
}

static int SD_ReadSectors(uint32_t lba, uint8_t *buf, uint32_t count)
{
  if (HAL_SD_ReadBlocks(&hsd, buf, lba, count, HAL_MAX_DELAY) != HAL_OK) return -1;
  while (HAL_SD_GetCardState(&hsd) != HAL_SD_CARD_TRANSFER) {}
  return 0;
}

static int FAT32_Init(Fat32Info *fs)
{
  uint8_t sec[512];
  uint32_t part_lba = 0;

  if (SD_ReadSectors(0, sec, 1) != 0) return -1;
  if (sec[510] != 0x55 || sec[511] != 0xAA) return -2;

  if (!(sec[0] == 0xEB || sec[0] == 0xE9))
  {
    part_lba = le32(&sec[446 + 8]);
    if (part_lba == 0) return -3;
    if (SD_ReadSectors(part_lba, sec, 1) != 0) return -4;
  }

  if (le16(&sec[11]) != 512) return -5;

  uint16_t reserved = le16(&sec[14]);
  uint8_t fats = sec[16];
  uint32_t sectors_per_fat = le32(&sec[36]);
  uint32_t root_cluster = le32(&sec[44]);

  fs->part_lba = part_lba;
  fs->sectors_per_cluster = sec[13];
  fs->sectors_per_fat = sectors_per_fat;
  fs->fat_lba = part_lba + reserved;
  fs->data_lba = part_lba + reserved + (fats * sectors_per_fat);
  fs->root_cluster = root_cluster;
  return 0;
}

static uint32_t FAT32_ClusterToLba(const Fat32Info *fs, uint32_t cluster)
{
  return fs->data_lba + (cluster - 2U) * fs->sectors_per_cluster;
}

static int FAT32_NextCluster(const Fat32Info *fs, uint32_t cluster, uint32_t *next_cluster)
{
  uint8_t sec[512];
  uint32_t fat_offset = cluster * 4U;
  uint32_t fat_sector = fs->fat_lba + (fat_offset / 512U);
  uint32_t ent_offset = fat_offset % 512U;

  if (SD_ReadSectors(fat_sector, sec, 1) != 0) return -1;
  *next_cluster = le32(&sec[ent_offset]) & 0x0FFFFFFFU;
  return 0;
}

static uint32_t EntryCluster(const uint8_t *entry)
{
  uint32_t cl_hi = le16(&entry[20]);
  uint32_t cl_lo = le16(&entry[26]);
  return (cl_hi << 16) | cl_lo;
}

static int IsWavEntry(const uint8_t *entry)
{
  uint8_t e0 = entry[8], e1 = entry[9], e2 = entry[10];
  if (e0 >= 'a' && e0 <= 'z') e0 -= 32;
  if (e1 >= 'a' && e1 <= 'z') e1 -= 32;
  if (e2 >= 'a' && e2 <= 'z') e2 -= 32;
  return (e0 == 'W' && e1 == 'A' && e2 == 'V');
}

static int FAT32_FindRootDirCluster(const Fat32Info *fs, const char *dirName, uint32_t *cluster_out)
{
  uint8_t sec[512];
  uint32_t cluster = fs->root_cluster;
  char name83[8] = {' ',' ',' ',' ',' ',' ',' ',' '};
  for (int i = 0; i < 8 && dirName[i] != '\0'; i++)
  {
    char c = dirName[i];
    if (c >= 'a' && c <= 'z') c -= 32;
    name83[i] = c;
  }

  while (cluster >= 2U && cluster < 0x0FFFFFF8U)
  {
    for (uint32_t s = 0; s < fs->sectors_per_cluster; s++)
    {
      uint32_t lba = FAT32_ClusterToLba(fs, cluster) + s;
      if (SD_ReadSectors(lba, sec, 1) != 0) return -1;

      for (int off = 0; off < 512; off += 32)
      {
        uint8_t *entry = &sec[off];
        uint8_t first = entry[0];
        if (first == 0x00) return -2;
        if (first == 0xE5) continue;
        if (entry[11] == 0x0F) continue;
        if ((entry[11] & 0x10) == 0) continue;
        if (entry[0] == '.') continue;

        if (memcmp(entry, name83, 8) == 0)
        {
          uint32_t cl = EntryCluster(entry);
          if (cl < 2) return -3;
          *cluster_out = cl;
          return 0;
        }
      }
    }

    uint32_t next;
    if (FAT32_NextCluster(fs, cluster, &next) != 0) return -4;
    cluster = next;
  }
  return -5;
}

static int FAT32_OpenWavInDirByIndex(const Fat32Info *fs, uint32_t dir_cluster, uint32_t wav_index, FileCtx *file)
{
  uint8_t sec[512];
  uint32_t cluster = dir_cluster;
  uint32_t found = 0;

  while (cluster >= 2U && cluster < 0x0FFFFFF8U)
  {
    for (uint32_t s = 0; s < fs->sectors_per_cluster; s++)
    {
      uint32_t lba = FAT32_ClusterToLba(fs, cluster) + s;
      if (SD_ReadSectors(lba, sec, 1) != 0) return -1;

      for (int off = 0; off < 512; off += 32)
      {
        uint8_t *entry = &sec[off];
        uint8_t first = entry[0];
        if (first == 0x00) return -2;
        if (first == 0xE5) continue;
        if (entry[11] == 0x0F) continue;

        uint8_t attr = entry[11];
        if (attr & 0x08) continue;
        if (attr & 0x10) continue;
        if (!IsWavEntry(entry)) continue;

        if (found == wav_index)
        {
          memset(file, 0, sizeof(*file));
          file->fs = *fs;
          file->first_cluster = EntryCluster(entry);
          file->current_cluster = file->first_cluster;
          file->file_size = le32(&entry[28]);
          return 0;
        }
        found++;
      }
    }

    uint32_t next;
    if (FAT32_NextCluster(fs, cluster, &next) != 0) return -3;
    cluster = next;
  }
  return -4;
}

static int File_AdvanceSector(FileCtx *file)
{
  file->sector_in_cluster++;
  if (file->sector_in_cluster >= file->fs.sectors_per_cluster)
  {
    uint32_t next;
    if (FAT32_NextCluster(&file->fs, file->current_cluster, &next) != 0) return -1;
    if (next >= 0x0FFFFFF8U) return -2;
    file->current_cluster = next;
    file->sector_in_cluster = 0;
  }
  file->sector_valid = 0;
  file->sector_offset = 0;
  return 0;
}

static int File_Read(FileCtx *file, uint8_t *dst, uint32_t bytes, uint32_t *read_bytes)
{
  *read_bytes = 0;
  while (bytes > 0 && file->pos < file->file_size)
  {
    if (!file->sector_valid)
    {
      uint32_t lba = FAT32_ClusterToLba(&file->fs, file->current_cluster) + file->sector_in_cluster;
      if (SD_ReadSectors(lba, file->sector_buf, 1) != 0) return -1;
      file->sector_valid = 1;
      file->sector_offset = 0;
    }

    uint32_t remain_sector = 512U - file->sector_offset;
    uint32_t remain_file = file->file_size - file->pos;
    uint32_t n = bytes;
    if (n > remain_sector) n = remain_sector;
    if (n > remain_file) n = remain_file;

    memcpy(dst, &file->sector_buf[file->sector_offset], n);
    dst += n;
    bytes -= n;
    file->pos += n;
    file->sector_offset += (uint16_t)n;
    *read_bytes += n;

    if (file->sector_offset >= 512U)
    {
      if (File_AdvanceSector(file) != 0 && file->pos < file->file_size) return -2;
    }
  }
  return 0;
}

static int File_Skip(FileCtx *file, uint32_t bytes)
{
  uint8_t tmp[64];
  while (bytes > 0)
  {
    uint32_t chunk = (bytes > sizeof(tmp)) ? sizeof(tmp) : bytes;
    uint32_t got = 0;
    if (File_Read(file, tmp, chunk, &got) != 0) return -1;
    if (got == 0) break;
    bytes -= got;
  }
  return 0;
}

static int WAV_ReadHeader(FileCtx *file, WavInfo *wav)
{
  uint8_t hdr[12];
  uint32_t got = 0;
  if (File_Read(file, hdr, sizeof(hdr), &got) != 0 || got != sizeof(hdr)) return -1;
  if (memcmp(&hdr[0], "RIFF", 4) != 0 || memcmp(&hdr[8], "WAVE", 4) != 0) return -2;

  memset(wav, 0, sizeof(*wav));
  while (1)
  {
    uint8_t ch[8];
    if (File_Read(file, ch, sizeof(ch), &got) != 0 || got != sizeof(ch)) return -3;
    uint32_t chunk_size = le32(&ch[4]);

    if (memcmp(&ch[0], "fmt ", 4) == 0)
    {
      uint8_t fmt[40];
      uint32_t take = (chunk_size > sizeof(fmt)) ? sizeof(fmt) : chunk_size;
      if (File_Read(file, fmt, take, &got) != 0 || got != take) return -4;
      if (chunk_size > take) if (File_Skip(file, chunk_size - take) != 0) return -5;

      uint16_t audio_fmt = le16(&fmt[0]);
      wav->channels = le16(&fmt[2]);
      wav->sample_rate = le32(&fmt[4]);
      wav->bits_per_sample = le16(&fmt[14]);
      if (audio_fmt != 1) return -6;
    }
    else if (memcmp(&ch[0], "data", 4) == 0)
    {
      wav->data_size = chunk_size;
      break;
    }
    else
    {
      if (File_Skip(file, chunk_size) != 0) return -7;
    }
  }

  if (wav->bits_per_sample != 16) return -8;
  if (!(wav->channels == 1 || wav->channels == 2)) return -9;
  return 0;
}

static int16_t ClipS16(int32_t x)
{
  if (x > 32767) return 32767;
  if (x < -32768) return -32768;
  return (int16_t)x;
}

static int LoadInstrumentFromFolder(const char *folderName, InstrumentSample *out)
{
  Fat32Info fs;
  if (FAT32_Init(&fs) != 0) return -1;

  uint32_t dir_cluster = 0;
  if (FAT32_FindRootDirCluster(&fs, folderName, &dir_cluster) != 0) return -2;

  FileCtx file;
  if (FAT32_OpenWavInDirByIndex(&fs, dir_cluster, 0, &file) != 0) return -3;

  WavInfo wav;
  if (WAV_ReadHeader(&file, &wav) != 0) return -4;

  uint32_t frames = 0;
  while (frames < MAX_SAMPLE_FRAMES)
  {
    uint32_t remain_frames = MAX_SAMPLE_FRAMES - frames;
    uint32_t want_bytes = (wav.channels == 2) ? (remain_frames * 4U) : (remain_frames * 2U);
    if (want_bytes > SD_READ_BYTES) want_bytes = SD_READ_BYTES;

    uint32_t got = 0;
    if (File_Read(&file, sdReadBuf, want_bytes, &got) != 0 || got == 0) break;

    if (wav.channels == 2)
    {
      uint32_t stereo_frames = got / 4U;
      int16_t *src = (int16_t*)sdReadBuf;
      for (uint32_t i = 0; i < stereo_frames && frames < MAX_SAMPLE_FRAMES; i++)
      {
        int32_t m = ((int32_t)src[i * 2] + (int32_t)src[i * 2 + 1]) / 2;
        out->data[frames++] = ClipS16(m);
      }
    }
    else
    {
      uint32_t mono_frames = got / 2U;
      int16_t *src = (int16_t*)sdReadBuf;
      for (uint32_t i = 0; i < mono_frames && frames < MAX_SAMPLE_FRAMES; i++)
        out->data[frames++] = src[i];
    }
  }

  out->length = frames;
  out->loaded = (frames > 0) ? 1U : 0U;
  if (out->loaded)
  {
    for (uint32_t i = 0; i < NUM_INSTRUMENTS; i++)
    {
      if (out == &g_samples[i])
      {
        g_samplesLoadedMask |= (1U << i);
        break;
      }
    }
  }
  return out->loaded ? 0 : -5;
}

static void TriggerVoice(uint8_t inst, int16_t gain_q15, int8_t pan)
{
  if (inst >= NUM_INSTRUMENTS || !g_samples[inst].loaded) return;

  int slot = -1;
  for (int i = 0; i < (int)MAX_VOICES; i++)
  {
    if (!g_voices[i].active) { slot = i; break; }
  }
  if (slot < 0) slot = 0;

  g_voices[slot].active = 1;
  g_voices[slot].inst = inst;
  g_voices[slot].pos = 0;
  g_voices[slot].frac_q12 = 0;
  g_voices[slot].step_q12 = g_instPitchQ12[inst];
  if (g_voices[slot].step_q12 < 512U) g_voices[slot].step_q12 = 512U;
  g_voices[slot].gain_q15 = gain_q15;
  g_voices[slot].pan = pan;
}

static void ProcessSpiTriggers(void)
{
  while (1)
  {
    uint8_t pad;
    uint8_t vel;

    __disable_irq();
    if (g_spiTrigTail == g_spiTrigHead)
    {
      __enable_irq();
      break;
    }

    pad = g_spiTrigQ[g_spiTrigTail].pad;
    vel = g_spiTrigQ[g_spiTrigTail].vel;
    g_spiTrigTail = (uint8_t)((g_spiTrigTail + 1U) % SPI_TRIG_Q_LEN);
    __enable_irq();

    uint8_t inst = (uint8_t)(pad % NUM_INSTRUMENTS);
    int32_t gain = 12000 + ((int32_t)vel * 140);
    gain = (gain * g_liveVolume) / 100;
    gain = (gain * g_abLiveTrimQ8) >> 8;
    if (gain > 32000) gain = 32000;
    int8_t pan = 0;

    if (inst == 2) pan = -10;
    else if (inst == 3) pan = 12;

    TriggerVoice(inst, (int16_t)gain, pan);
  }
}

static uint16_t SpiCrc16(const uint8_t *data, uint16_t len)
{
  uint16_t crc = 0xFFFF;
  for (uint16_t i = 0; i < len; i++)
  {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++)
    {
      if (crc & 1U) crc = (uint16_t)((crc >> 1) ^ 0xA001U);
      else crc >>= 1;
    }
  }
  return crc;
}

static void SpiTxEnqueue(const uint8_t *data, uint16_t len)
{
  __disable_irq();
  for (uint16_t i = 0; i < len; i++)
  {
    uint16_t next = (uint16_t)((g_spiTxHead + 1U) % SPI_TX_Q_LEN);
    if (next == g_spiTxTail) break;
    g_spiTxQ[g_spiTxHead] = data[i];
    g_spiTxHead = next;
  }
  __enable_irq();
}

static uint8_t SpiTxPopByte(void)
{
  uint8_t out = 0;
  if (g_spiTxTail != g_spiTxHead)
  {
    out = g_spiTxQ[g_spiTxTail];
    g_spiTxTail = (uint16_t)((g_spiTxTail + 1U) % SPI_TX_Q_LEN);
  }
  return out;
}

static void SpiQueueTrigger(uint8_t pad, uint8_t vel)
{
  uint8_t next = (uint8_t)((g_spiTrigHead + 1U) % SPI_TRIG_Q_LEN);
  if (next == g_spiTrigTail) return;
  g_spiTrigQ[g_spiTrigHead].pad = pad;
  g_spiTrigQ[g_spiTrigHead].vel = vel;
  g_spiTrigHead = next;
}

static void SpiEnqueueResponse(uint8_t cmd, uint16_t sequence, const uint8_t *payload, uint16_t len)
{
  uint8_t hdr[8];
  uint16_t crc = SpiCrc16(payload, len);

  hdr[0] = SPI_MAGIC_RESP;
  hdr[1] = cmd;
  hdr[2] = (uint8_t)(len & 0xFFU);
  hdr[3] = (uint8_t)((len >> 8) & 0xFFU);
  hdr[4] = (uint8_t)(sequence & 0xFFU);
  hdr[5] = (uint8_t)((sequence >> 8) & 0xFFU);
  hdr[6] = (uint8_t)(crc & 0xFFU);
  hdr[7] = (uint8_t)((crc >> 8) & 0xFFU);

  SpiTxEnqueue(hdr, sizeof(hdr));
  if (len > 0U) SpiTxEnqueue(payload, len);
}

static uint8_t ActiveVoicesCount(void)
{
  uint8_t cnt = 0;
  for (uint32_t i = 0; i < MAX_VOICES; i++) if (g_voices[i].active) cnt++;
  return cnt;
}

static void StopInstrumentVoices(uint8_t inst)
{
  for (uint32_t i = 0; i < MAX_VOICES; i++)
  {
    if (g_voices[i].active && g_voices[i].inst == inst) g_voices[i].active = 0;
  }
}

static void StopAllVoices(void)
{
  for (uint32_t i = 0; i < MAX_VOICES; i++) g_voices[i].active = 0;
}

static void SpiHandleCommand(uint8_t cmd, const uint8_t *payload, uint16_t len, uint16_t seq)
{
  switch (cmd)
  {
    case CMD_TRIGGER_SEQ:
      if (len >= 2U)
      {
        uint8_t pad = payload[0];
        uint8_t vel = payload[1];
        if (len >= 3U)
        {
          uint16_t tv = payload[2];
          vel = (uint8_t)((vel * tv) / 100U);
        }
        SpiQueueTrigger(pad, vel);
      }
      break;

    case CMD_TRIGGER_LIVE:
      if (len >= 2U) SpiQueueTrigger(payload[0], payload[1]);
      break;

    case CMD_TRIGGER_STOP_ALL:
      StopAllVoices();
      break;

    case CMD_TRIGGER_STOP:
      if (len >= 1U) StopInstrumentVoices((uint8_t)(payload[0] % NUM_INSTRUMENTS));
      break;

    case CMD_MASTER_VOLUME:
      if (len >= 1U) g_masterVolume = payload[0];
      break;

    case CMD_SEQ_VOLUME:
      if (len >= 1U) g_seqVolume = payload[0];
      break;

    case CMD_LIVE_VOLUME:
      if (len >= 1U) g_liveVolume = payload[0];
      break;

    case CMD_TRACK_VOLUME:
      if (len >= 2U)
      {
        uint8_t tr = payload[0] % NUM_INSTRUMENTS;
        g_trackVolume[tr] = payload[1];
      }
      break;

    case CMD_FILTER_TYPE:
      if (len >= 16U)
      {
        g_globalFilterType = payload[0];
        g_globalDistMode = payload[1] & 0x03U;
        g_globalBitDepth = payload[2];
        if (g_globalBitDepth < 4U) g_globalBitDepth = 4U;
        if (g_globalBitDepth > 16U) g_globalBitDepth = 16U;
        float cutoff, resonance, distortion;
        memcpy(&cutoff, &payload[4], sizeof(float));
        memcpy(&resonance, &payload[8], sizeof(float));
        memcpy(&distortion, &payload[12], sizeof(float));
        g_globalFilterCutQ8 = CutoffHzToQ8(cutoff);
        g_globalFilterResQ8 = ResonanceToQ8(resonance);
        if (distortion < 0.0f) distortion = 0.0f;
        if (distortion > 100.0f) distortion = 100.0f;
        g_globalDistQ8 = (uint8_t)((distortion * 255.0f) / 100.0f);
      }
      else if (len >= 1U)
      {
        g_globalFilterType = payload[0];
      }
      break;

    case CMD_FILTER_CUTOFF:
      if (len >= 4U)
      {
        float cutoff;
        memcpy(&cutoff, payload, sizeof(float));
        g_globalFilterCutQ8 = CutoffHzToQ8(cutoff);
      }
      break;

    case CMD_FILTER_RESONANCE:
      if (len >= 4U)
      {
        float resonance;
        memcpy(&resonance, payload, sizeof(float));
        g_globalFilterResQ8 = ResonanceToQ8(resonance);
      }
      break;

    case CMD_FILTER_BITDEPTH:
      if (len >= 1U)
      {
        uint8_t bits = payload[0];
        if (bits < 4U) bits = 4U;
        if (bits > 16U) bits = 16U;
        g_globalBitDepth = bits;
      }
      break;

    case CMD_FILTER_DISTORTION:
      if (len >= 4U)
      {
        float distortion;
        memcpy(&distortion, payload, sizeof(float));
        if (distortion < 0.0f) distortion = 0.0f;
        if (distortion > 100.0f) distortion = 100.0f;
        g_globalDistQ8 = (uint8_t)((distortion * 255.0f) / 100.0f);
      }
      else if (len >= 1U)
      {
        g_globalDistQ8 = payload[0];
      }
      break;

    case CMD_FILTER_DIST_MODE:
      if (len >= 1U) g_globalDistMode = payload[0] & 0x03U;
      break;

    case CMD_FILTER_SR_REDUCE:
      if (len >= 4U)
      {
        float sr;
        memcpy(&sr, payload, sizeof(float));
        if (sr < 1.0f) sr = 1.0f;
        if (sr > 16.0f) sr = 16.0f;
        g_globalSrReduce = (uint8_t)sr;
      }
      else if (len >= 1U)
      {
        uint8_t d = payload[0];
        if (d < 1U) d = 1U;
        if (d > 16U) d = 16U;
        g_globalSrReduce = d;
      }
      break;

    case CMD_TRACK_FILTER:
      if (len >= 17U)
      {
        uint8_t track = payload[0] % NUM_INSTRUMENTS;
        g_trackFilterType[track] = payload[1];
        float cutoff, resonance;
        memcpy(&cutoff, &payload[4], sizeof(float));
        memcpy(&resonance, &payload[8], sizeof(float));
        g_trackFilterCutQ8[track] = CutoffHzToQ8(cutoff);
        g_trackFilterResQ8[track] = ResonanceToQ8(resonance);
      }
      break;

    case CMD_TRACK_CLEAR_FX:
      if (len >= 1U)
      {
        uint8_t track = payload[0] % NUM_INSTRUMENTS;
        g_trackFilterType[track] = 0U;
        g_trackDistQ8[track] = 0U;
        g_trackBitDepth[track] = 16U;
        g_trackEcho[track].active = 0U;
        g_trackFlanger[track].active = 0U;
        g_trackComp[track].active = 0U;
      }
      break;

    case CMD_DELAY_ACTIVE:
      if (len >= 1U) g_delayActive = payload[0] ? 1U : 0U;
      break;

    case CMD_DELAY_FEEDBACK:
      if (len >= 4U)
      {
        float fb;
        memcpy(&fb, payload, sizeof(float));
        if (fb < 0.0f) fb = 0.0f;
        if (fb > 0.95f) fb = 0.95f;
        g_delayFbQ8 = (uint8_t)(fb * 255.0f);
      }
      break;

    case CMD_DELAY_TIME:
      if (len >= 4U)
      {
        float t;
        memcpy(&t, payload, sizeof(float));
        if (t < 20.0f) t = 20.0f;
        if (t > 700.0f) t = 700.0f;
      }
      break;

    case CMD_DELAY_MIX:
      if (len >= 4U)
      {
        float mx;
        memcpy(&mx, payload, sizeof(float));
        if (mx < 0.0f) mx = 0.0f;
        if (mx > 1.0f) mx = 1.0f;
        g_delayMixQ8 = (uint8_t)(mx * 255.0f);
      }
      break;

    case CMD_FLANGER_ACTIVE:
      if (len >= 1U) g_flangerEnabled = payload[0] ? 1U : 0U;
      break;

    case CMD_FLANGER_DEPTH:
      if (len >= 4U)
      {
        float dp;
        memcpy(&dp, payload, sizeof(float));
        if (dp < 0.0f) dp = 0.0f;
        if (dp > 1.0f) dp = 1.0f;
        g_flangerDepth = (uint8_t)(dp * 255.0f);
      }
      break;

    case CMD_FLANGER_RATE:
      if (len >= 4U)
      {
        float rt;
        memcpy(&rt, payload, sizeof(float));
        if (rt < 0.05f) rt = 0.05f;
        if (rt > 6.0f) rt = 6.0f;
      }
      break;

    case CMD_FLANGER_FEEDBACK:
      if (len >= 4U)
      {
        float fb;
        memcpy(&fb, payload, sizeof(float));
        if (fb < 0.0f) fb = 0.0f;
        if (fb > 0.95f) fb = 0.95f;
      }
      break;

    case CMD_FLANGER_MIX:
      if (len >= 4U)
      {
        float mx;
        memcpy(&mx, payload, sizeof(float));
        if (mx < 0.0f) mx = 0.0f;
        if (mx > 1.0f) mx = 1.0f;
        g_flangerMixQ8 = (uint8_t)(mx * 255.0f);
      }
      break;

    case CMD_PHASER_ACTIVE:
      if (len >= 1U) g_phaserEnabled = payload[0] ? 1U : 0U;
      break;

    case CMD_PHASER_DEPTH:
      if (len >= 4U)
      {
        float dp;
        memcpy(&dp, payload, sizeof(float));
        if (dp < 0.0f) dp = 0.0f;
        if (dp > 1.0f) dp = 1.0f;
        g_phaserDepthQ8 = (uint8_t)(dp * 255.0f);
      }
      break;

    case CMD_PHASER_RATE:
      if (len >= 4U)
      {
        float rt;
        memcpy(&rt, payload, sizeof(float));
        if (rt < 0.05f) rt = 0.05f;
        if (rt > 8.0f) rt = 8.0f;
        uint32_t st = (uint32_t)(rt * 1.5f);
        if (st < 1U) st = 1U;
        if (st > 12U) st = 12U;
        g_phaserRateStep = (uint8_t)st;
      }
      break;

    case CMD_PHASER_FEEDBACK:
      if (len >= 4U)
      {
        float fb;
        memcpy(&fb, payload, sizeof(float));
        if (fb < 0.0f) fb = 0.0f;
        if (fb > 0.95f) fb = 0.95f;
        g_phaserFeedbackQ8 = (uint8_t)(fb * 255.0f);
      }
      break;

    case CMD_COMP_ACTIVE:
      if (len >= 1U) g_masterCompEnabled = payload[0] ? 1U : 0U;
      break;

    case CMD_COMP_THRESHOLD:
      if (len >= 4U)
      {
        float db;
        memcpy(&db, payload, sizeof(float));
        if (db < -60.0f) db = -60.0f;
        if (db > 0.0f) db = 0.0f;
        float lin = powf(10.0f, db / 20.0f);
        uint32_t q = (uint32_t)(lin * 32767.0f);
        if (q > 32767U) q = 32767U;
        g_masterCompThresholdQ15 = (uint16_t)q;
      }
      break;

    case CMD_COMP_RATIO:
      if (len >= 4U)
      {
        float r;
        memcpy(&r, payload, sizeof(float));
        if (r < 1.0f) r = 1.0f;
        if (r > 20.0f) r = 20.0f;
        g_masterCompRatioQ8 = (uint8_t)(r * 12.0f);
      }
      break;

    case CMD_COMP_MAKEUP:
      if (len >= 4U)
      {
        float db;
        memcpy(&db, payload, sizeof(float));
        if (db < 0.0f) db = 0.0f;
        if (db > 24.0f) db = 24.0f;
        float lin = powf(10.0f, db / 20.0f);
        uint32_t q = (uint32_t)(lin * 128.0f);
        if (q > 255U) q = 255U;
        g_masterCompMakeupQ8 = (uint8_t)q;
      }
      break;

    case CMD_COMP_ATTACK:
      if (len >= 4U)
      {
        float attackMs;
        memcpy(&attackMs, payload, sizeof(float));
        if (attackMs < 0.1f) attackMs = 0.1f;
        if (attackMs > 200.0f) attackMs = 200.0f;
        uint32_t atk = (uint32_t)(1000.0f / attackMs);
        if (atk < 1U) atk = 1U;
        if (atk > 255U) atk = 255U;
        g_masterCompAttackK = (uint8_t)atk;
      }
      break;

    case CMD_COMP_RELEASE:
      if (len >= 4U)
      {
        float releaseMs;
        memcpy(&releaseMs, payload, sizeof(float));
        if (releaseMs < 5.0f) releaseMs = 5.0f;
        if (releaseMs > 2000.0f) releaseMs = 2000.0f;
        uint32_t rel = (uint32_t)(1000.0f / releaseMs);
        if (rel < 1U) rel = 1U;
        if (rel > 255U) rel = 255U;
        g_masterCompReleaseK = (uint8_t)rel;
      }
      break;

    case CMD_TRACK_ECHO:
      if (len >= 16U)
      {
        uint8_t track = payload[0] % NUM_INSTRUMENTS;
        uint8_t active = payload[1] ? 1U : 0U;
        float timeMs, feedback, mix;
        memcpy(&timeMs, &payload[4], sizeof(float));
        memcpy(&feedback, &payload[8], sizeof(float));
        memcpy(&mix, &payload[12], sizeof(float));

        if (timeMs < 5.0f) timeMs = 5.0f;
        if (timeMs > 200.0f) timeMs = 200.0f;
        if (feedback < 0.0f) feedback = 0.0f;
        if (feedback > 100.0f) feedback = 100.0f;
        if (mix < 0.0f) mix = 0.0f;
        if (mix > 100.0f) mix = 100.0f;

        g_trackEcho[track].active = active;
        g_trackEcho[track].delaySamples = (uint16_t)((timeMs * DEMO_SAMPLE_RATE) / 1000.0f);
        if (g_trackEcho[track].delaySamples >= TRACK_FX_BUF_SAMPLES) g_trackEcho[track].delaySamples = TRACK_FX_BUF_SAMPLES - 1U;
        g_trackEcho[track].feedbackQ8 = (uint8_t)((feedback * 255.0f) / 100.0f);
        g_trackEcho[track].mixQ8 = (uint8_t)((mix * 255.0f) / 100.0f);
      }
      break;

    case CMD_TRACK_FLANGER_FX:
      if (len >= 16U)
      {
        uint8_t track = payload[0] % NUM_INSTRUMENTS;
        uint8_t active = payload[1] ? 1U : 0U;
        float rate, depth, feedback;
        memcpy(&rate, &payload[4], sizeof(float));
        memcpy(&depth, &payload[8], sizeof(float));
        memcpy(&feedback, &payload[12], sizeof(float));

        if (rate < 0.05f) rate = 0.05f;
        if (rate > 8.0f) rate = 8.0f;
        if (depth < 0.0f) depth = 0.0f;
        if (depth > 100.0f) depth = 100.0f;
        if (feedback < 0.0f) feedback = 0.0f;
        if (feedback > 100.0f) feedback = 100.0f;

        g_trackFlanger[track].active = active;
        g_trackFlanger[track].depthQ8 = (uint8_t)((depth * 255.0f) / 100.0f);
        g_trackFlanger[track].feedbackQ8 = (uint8_t)((feedback * 255.0f) / 100.0f);
        g_trackFlanger[track].mixQ8 = (uint8_t)(96U + (g_trackFlanger[track].depthQ8 / 2U));
        if (g_trackFlanger[track].mixQ8 > 220U) g_trackFlanger[track].mixQ8 = 220U;
      }
      break;

    case CMD_TRACK_COMPRESSOR:
      if (len >= 12U)
      {
        uint8_t track = payload[0] % NUM_INSTRUMENTS;
        uint8_t active = payload[1] ? 1U : 0U;
        float thresholdDb, ratio;
        memcpy(&thresholdDb, &payload[4], sizeof(float));
        memcpy(&ratio, &payload[8], sizeof(float));

        if (thresholdDb < -60.0f) thresholdDb = -60.0f;
        if (thresholdDb > 0.0f) thresholdDb = 0.0f;
        if (ratio < 1.0f) ratio = 1.0f;
        if (ratio > 20.0f) ratio = 20.0f;

        float thLin = powf(10.0f, thresholdDb / 20.0f);
        uint32_t thQ15 = (uint32_t)(thLin * 32767.0f);
        if (thQ15 > 32767U) thQ15 = 32767U;

        g_trackComp[track].active = active;
        g_trackComp[track].thresholdQ15 = (uint16_t)thQ15;
        g_trackComp[track].ratioQ8 = (uint8_t)(ratio * 16.0f);
        if (g_trackComp[track].ratioQ8 < 16U) g_trackComp[track].ratioQ8 = 16U;
      }
      break;

    case CMD_TRACK_CLEAR_LIVE:
      if (len >= 1U)
      {
        uint8_t track = payload[0] % NUM_INSTRUMENTS;
        g_trackEcho[track].active = 0U;
        g_trackFlanger[track].active = 0U;
        g_trackComp[track].active = 0U;
      }
      break;

    case CMD_SIDECHAIN_SET:
      if (len >= 20U)
      {
        uint8_t active = payload[0] ? 1U : 0U;
        uint8_t sourceTrack = payload[1] % NUM_INSTRUMENTS;
        uint16_t destMask = (uint16_t)(payload[2] | (payload[3] << 8));
        float amount, attackMs, releaseMs, knee;
        memcpy(&amount, &payload[4], sizeof(float));
        memcpy(&attackMs, &payload[8], sizeof(float));
        memcpy(&releaseMs, &payload[12], sizeof(float));
        memcpy(&knee, &payload[16], sizeof(float));

        if (amount < 0.0f) amount = 0.0f;
        if (amount > 1.0f) amount = 1.0f;
        if (attackMs < 0.2f) attackMs = 0.2f;
        if (attackMs > 80.0f) attackMs = 80.0f;
        if (releaseMs < 10.0f) releaseMs = 10.0f;
        if (releaseMs > 1200.0f) releaseMs = 1200.0f;
        (void)knee;

        g_sidechain.active = active;
        g_sidechain.sourceTrack = sourceTrack;
        g_sidechain.destinationMask = destMask;
        g_sidechain.amountQ8 = (uint8_t)(amount * 255.0f);

        uint32_t atk = (uint32_t)(1000.0f / attackMs);
        uint32_t rel = (uint32_t)(1000.0f / releaseMs);
        if (atk < 1U) atk = 1U;
        if (atk > 255U) atk = 255U;
        if (rel < 1U) rel = 1U;
        if (rel > 255U) rel = 255U;
        g_sidechain.attackK = (uint8_t)atk;
        g_sidechain.releaseK = (uint8_t)rel;
      }
      break;

    case CMD_SIDECHAIN_CLEAR:
      g_sidechain.active = 0U;
      g_sidechain.envQ15 = 0U;
      break;

    case CMD_TRIGGER_SIDECHAIN:
      if (g_sidechain.active && len >= 2U)
      {
        uint8_t src = payload[0] % NUM_INSTRUMENTS;
        if (src == g_sidechain.sourceTrack)
        {
          uint8_t vel = payload[1];
          uint16_t trg = (uint16_t)(vel * 258U);
          if (trg > g_sidechain.envQ15) g_sidechain.envQ15 = trg;
        }
      }
      break;

    case CMD_SAMPLE_BEGIN:
      if (len >= 12U)
      {
        uint8_t pad = payload[0] % NUM_INSTRUMENTS;
        uint32_t totalBytes = (uint32_t)(payload[4] | (payload[5] << 8) | (payload[6] << 16) | (payload[7] << 24));
        uint32_t maxBytes = MAX_SAMPLE_FRAMES * 2U;
        if (totalBytes > maxBytes) totalBytes = maxBytes;

        g_sampleUpload.active = 1U;
        g_sampleUpload.inst = pad;
        g_sampleUpload.totalBytes = totalBytes;
        g_sampleUpload.receivedBytes = 0U;
        g_samples[pad].length = 0U;
        g_samples[pad].loaded = 0U;
      }
      break;

    case CMD_SAMPLE_DATA:
      if (g_sampleUpload.active && len >= 8U)
      {
        uint8_t pad = payload[0] % NUM_INSTRUMENTS;
        uint16_t chunkSize = (uint16_t)(payload[2] | (payload[3] << 8));
        uint32_t offset = (uint32_t)(payload[4] | (payload[5] << 8) | (payload[6] << 16) | (payload[7] << 24));
        if (pad == g_sampleUpload.inst && (8U + chunkSize) <= len)
        {
          if (offset < g_sampleUpload.totalBytes)
          {
            uint32_t room = g_sampleUpload.totalBytes - offset;
            if (chunkSize > room) chunkSize = (uint16_t)room;
            memcpy(((uint8_t*)g_samples[pad].data) + offset, &payload[8], chunkSize);
            uint32_t end = offset + chunkSize;
            if (end > g_sampleUpload.receivedBytes) g_sampleUpload.receivedBytes = end;
          }
        }
      }
      break;

    case CMD_SAMPLE_END:
      if (g_sampleUpload.active)
      {
        uint8_t pad = g_sampleUpload.inst;
        g_samples[pad].length = g_sampleUpload.receivedBytes / 2U;
        g_samples[pad].loaded = (g_samples[pad].length > 0U) ? 1U : 0U;
        if (g_samples[pad].loaded) g_samplesLoadedMask |= (1U << pad);
        else g_samplesLoadedMask &= ~(1U << pad);
        g_sampleUpload.active = 0U;
      }
      break;

    case CMD_SAMPLE_UNLOAD:
      if (len >= 1U)
      {
        uint8_t pad = payload[0] % NUM_INSTRUMENTS;
        g_samples[pad].length = 0U;
        g_samples[pad].loaded = 0U;
        g_samplesLoadedMask &= ~(1U << pad);
      }
      break;

    case CMD_SAMPLE_UNLOAD_ALL:
      for (uint32_t k = 0; k < NUM_INSTRUMENTS; k++)
      {
        g_samples[k].length = 0U;
        g_samples[k].loaded = 0U;
      }
      g_samplesLoadedMask = 0U;
      break;

    case CMD_PAD_REVERSE:
      if (len >= 2U && payload[1])
      {
        uint8_t pad = payload[0] % NUM_INSTRUMENTS;
        uint32_t n = g_samples[pad].length;
        for (uint32_t i = 0; i < (n / 2U); i++)
        {
          int16_t t = g_samples[pad].data[i];
          g_samples[pad].data[i] = g_samples[pad].data[n - 1U - i];
          g_samples[pad].data[n - 1U - i] = t;
        }
      }
      break;

    case CMD_PAD_PITCH:
      if (len >= 5U)
      {
        uint8_t pad = payload[0] % NUM_INSTRUMENTS;
        float pitch;
        memcpy(&pitch, &payload[1], sizeof(float));
        if (pitch < 0.25f) pitch = 0.25f;
        if (pitch > 4.0f) pitch = 4.0f;
        uint32_t q12 = (uint32_t)(pitch * 4096.0f);
        if (q12 < 512U) q12 = 512U;
        if (q12 > 16384U) q12 = 16384U;
        g_instPitchQ12[pad] = (uint16_t)q12;
      }
      break;

    case CMD_PAD_LOOP:
      if (len >= 2U)
      {
        uint8_t pad = payload[0] % NUM_INSTRUMENTS;
        g_padLoopEnabled[pad] = payload[1] ? 1U : 0U;
      }
      break;

    case CMD_PAD_STUTTER:
      if (len >= 4U)
      {
        uint8_t pad = payload[0] % NUM_INSTRUMENTS;
        g_padStutterEnabled[pad] = payload[1] ? 1U : 0U;
        g_padStutterInterval[pad] = (uint16_t)(payload[2] | (payload[3] << 8));
        if (g_padStutterInterval[pad] < 20U) g_padStutterInterval[pad] = 20U;
        if (g_padStutterInterval[pad] > 2000U) g_padStutterInterval[pad] = 2000U;
      }
      break;

    case CMD_PAD_FILTER:
      if (len >= 17U)
      {
        uint8_t pad = payload[0] % NUM_INSTRUMENTS;
        g_padFilterType[pad] = payload[1];
        float cutoff, resonance;
        memcpy(&cutoff, &payload[4], sizeof(float));
        memcpy(&resonance, &payload[8], sizeof(float));
        g_padFilterCutQ8[pad] = CutoffHzToQ8(cutoff);
        g_padFilterResQ8[pad] = ResonanceToQ8(resonance);
      }
      break;

    case CMD_PAD_CLEAR_FX:
      if (len >= 1U)
      {
        uint8_t pad = payload[0] % NUM_INSTRUMENTS;
        g_padFilterType[pad] = 0U;
        g_padDistQ8[pad] = 0U;
        g_padBitDepth[pad] = 16U;
        g_padStutterEnabled[pad] = 0U;
      }
      break;

    case CMD_PAD_SCRATCH:
      if (len >= 20U)
      {
        uint8_t pad = payload[0] % NUM_INSTRUMENTS;
        g_padScratchActive[pad] = payload[1] ? 1U : 0U;

        float rate, depth, cutoff, crackle;
        memcpy(&rate, &payload[4], sizeof(float));
        memcpy(&depth, &payload[8], sizeof(float));
        memcpy(&cutoff, &payload[12], sizeof(float));
        memcpy(&crackle, &payload[16], sizeof(float));

        if (rate < 0.5f) rate = 0.5f;
        if (rate > 20.0f) rate = 20.0f;
        if (depth < 0.0f) depth = 0.0f;
        if (depth > 1.0f) depth = 1.0f;
        if (crackle < 0.0f) crackle = 0.0f;
        if (crackle > 1.0f) crackle = 1.0f;

        g_padScratchRateQ8[pad] = (uint16_t)(rate * 256.0f);
        g_padScratchDepthQ8[pad] = (uint8_t)(depth * 255.0f);
        g_padScratchCutQ8[pad] = CutoffHzToQ8(cutoff);
        g_padScratchCrackleQ8[pad] = (uint8_t)(crackle * 255.0f);
      }
      break;

    case CMD_PAD_TURNTABLISM:
      if (len >= 16U)
      {
        uint8_t pad = payload[0] % NUM_INSTRUMENTS;
        g_padTurnActive[pad] = payload[1] ? 1U : 0U;
        g_padTurnAuto[pad] = payload[2] ? 1U : 0U;
        g_padTurnMode[pad] = (int8_t)payload[3];
        g_padTurnBrakeMs[pad] = (uint16_t)(payload[4] | (payload[5] << 8));
        g_padTurnBackspinMs[pad] = (uint16_t)(payload[6] | (payload[7] << 8));
        if (g_padTurnBrakeMs[pad] < 20U) g_padTurnBrakeMs[pad] = 20U;
        if (g_padTurnBackspinMs[pad] < 20U) g_padTurnBackspinMs[pad] = 20U;

        float transformRate, vinylNoise;
        memcpy(&transformRate, &payload[8], sizeof(float));
        memcpy(&vinylNoise, &payload[12], sizeof(float));
        if (transformRate < 0.2f) transformRate = 0.2f;
        if (transformRate > 30.0f) transformRate = 30.0f;
        if (vinylNoise < 0.0f) vinylNoise = 0.0f;
        if (vinylNoise > 1.0f) vinylNoise = 1.0f;

        g_padTurnRateQ8[pad] = (uint16_t)(transformRate * 256.0f);
        g_padTurnNoiseQ8[pad] = (uint8_t)(vinylNoise * 255.0f);
      }
      break;

    case CMD_TRACK_DISTORTION:
      if (len >= 2U)
      {
        uint8_t track = payload[0] % NUM_INSTRUMENTS;
        uint8_t amount = payload[1];
        if (len >= 5U)
        {
          float d;
          memcpy(&d, &payload[1], sizeof(float));
          if (d < 0.0f) d = 0.0f;
          if (d > 1.0f) d = 1.0f;
          amount = (uint8_t)(d * 255.0f);
        }
        g_trackDistQ8[track] = amount;
      }
      break;

    case CMD_TRACK_BITCRUSH:
      if (len >= 2U)
      {
        uint8_t track = payload[0] % NUM_INSTRUMENTS;
        uint8_t bits = payload[1];
        if (bits < 4U) bits = 4U;
        if (bits > 16U) bits = 16U;
        g_trackBitDepth[track] = bits;
      }
      break;

    case CMD_PAD_DISTORTION:
      if (len >= 2U)
      {
        uint8_t pad = payload[0] % NUM_INSTRUMENTS;
        uint8_t amount = payload[1];
        if (len >= 5U)
        {
          float d;
          memcpy(&d, &payload[1], sizeof(float));
          if (d < 0.0f) d = 0.0f;
          if (d > 1.0f) d = 1.0f;
          amount = (uint8_t)(d * 255.0f);
        }
        g_padDistQ8[pad] = amount;
      }
      break;

    case CMD_PAD_BITCRUSH:
      if (len >= 2U)
      {
        uint8_t pad = payload[0] % NUM_INSTRUMENTS;
        uint8_t bits = payload[1];
        if (bits < 4U) bits = 4U;
        if (bits > 16U) bits = 16U;
        g_padBitDepth[pad] = bits;
      }
      break;

    case CMD_GET_STATUS:
    {
      uint8_t resp[16] = {0};
      uint16_t freeKb = 0;
      uint32_t uptime = HAL_GetTick() / 1000U;
      uint16_t spiErr = g_spiErrorCount;

      resp[0] = ActiveVoicesCount();
      resp[1] = g_cpuLoadPercent;
      resp[2] = (uint8_t)(freeKb & 0xFFU);
      resp[3] = (uint8_t)((freeKb >> 8) & 0xFFU);
      resp[4] = (uint8_t)(g_samplesLoadedMask & 0xFFU);
      resp[5] = (uint8_t)((g_samplesLoadedMask >> 8) & 0xFFU);
      resp[6] = (uint8_t)((g_samplesLoadedMask >> 16) & 0xFFU);
      resp[7] = (uint8_t)((g_samplesLoadedMask >> 24) & 0xFFU);
      resp[8] = (uint8_t)(uptime & 0xFFU);
      resp[9] = (uint8_t)((uptime >> 8) & 0xFFU);
      resp[10] = (uint8_t)((uptime >> 16) & 0xFFU);
      resp[11] = (uint8_t)((uptime >> 24) & 0xFFU);
      resp[12] = (uint8_t)(spiErr & 0xFFU);
      resp[13] = (uint8_t)((spiErr >> 8) & 0xFFU);
      SpiEnqueueResponse(cmd, seq, resp, sizeof(resp));
      break;
    }

    case CMD_GET_PEAKS:
    {
      float peaks[17];
      for (int i = 0; i < 16; i++) peaks[i] = ((float)g_trackPeakQ15[i]) / 32767.0f;
      peaks[16] = ((float)g_masterPeakQ15) / 32767.0f;
      SpiEnqueueResponse(cmd, seq, (uint8_t*)peaks, sizeof(peaks));
      break;
    }

    case CMD_GET_CPU_LOAD:
      SpiEnqueueResponse(cmd, seq, (uint8_t*)&g_cpuLoadPercent, 1U);
      break;

    case CMD_GET_VOICES:
    {
      uint8_t v = ActiveVoicesCount();
      SpiEnqueueResponse(cmd, seq, &v, 1U);
      break;
    }

    case CMD_PING:
      if (len >= 4U)
      {
        uint8_t pong[8];
        memcpy(pong, payload, 4);
        uint32_t up = HAL_GetTick();
        pong[4] = (uint8_t)(up & 0xFFU);
        pong[5] = (uint8_t)((up >> 8) & 0xFFU);
        pong[6] = (uint8_t)((up >> 16) & 0xFFU);
        pong[7] = (uint8_t)((up >> 24) & 0xFFU);
        SpiEnqueueResponse(cmd, seq, pong, sizeof(pong));
      }
      break;

    case CMD_RESET:
      StopAllVoices();
      g_globalFilterType = 0U;
      g_globalBitDepth = 16U;
      g_globalDistQ8 = 0U;
      g_globalDistMode = 0U;
      g_globalSrReduce = 1U;
      g_globalFilterStateL = 0;
      g_globalFilterStateR = 0;
      g_globalSrPhase = 0U;
      g_globalSrHoldL = 0;
      g_globalSrHoldR = 0;
      g_phaserFeedbackQ8 = 48U;
      g_phaserRateStep = 2U;
      g_phaserLast = 0;
      g_phaserPhase = 0;
      g_masterCompAttackK = 64U;
      g_masterCompReleaseK = 8U;
      for (uint32_t t = 0; t < TRACK_PEAK_COUNT; t++) g_trackPeakQ15[t] = 0U;
      g_masterPeakQ15 = 0U;
      break;

    case CMD_BULK_TRIGGERS:
      if (len >= 2U)
      {
        uint8_t count = payload[0];
        uint16_t off = 2U;
        for (uint8_t i = 0; i < count; i++)
        {
          if ((off + 8U) > len) break;
          uint8_t pad = payload[off + 0U];
          uint8_t vel = payload[off + 1U];
          uint8_t trkVol = payload[off + 2U];
          vel = (uint8_t)((vel * trkVol) / 100U);
          SpiQueueTrigger(pad, vel);
          off += 8U;
        }
      }
      break;

    case CMD_BULK_FX:
      if (len >= 1U)
      {
        uint8_t count = payload[0];
        uint16_t off = 1U;
        for (uint8_t i = 0; i < count; i++)
        {
          if ((off + 2U) > len) break;
          uint8_t subCmd = payload[off + 0U];
          uint8_t subLen = payload[off + 1U];
          off += 2U;
          if ((off + subLen) > len) break;
          SpiHandleCommand(subCmd, &payload[off], subLen, seq);
          off += subLen;
        }
      }
      break;

    default:
      break;
  }
}

static void SpiParseIncomingByte(uint8_t byte_in)
{
  if (g_spiState == 0U)
  {
    if (byte_in == SPI_MAGIC_CMD || byte_in == SPI_MAGIC_SAMPLE || byte_in == SPI_MAGIC_BULK)
    {
      g_spiHdrIdx = 0;
      g_spiHdrBuf[g_spiHdrIdx++] = byte_in;
      g_spiState = 1U;
    }
    return;
  }

  if (g_spiState == 1U)
  {
    g_spiHdrBuf[g_spiHdrIdx++] = byte_in;
    if (g_spiHdrIdx >= sizeof(SpiPacketHeader))
    {
      g_spiCmd = g_spiHdrBuf[1];
      g_spiPayloadLen = (uint16_t)(g_spiHdrBuf[2] | (g_spiHdrBuf[3] << 8));
      g_spiSeq = (uint16_t)(g_spiHdrBuf[4] | (g_spiHdrBuf[5] << 8));
      g_spiChk = (uint16_t)(g_spiHdrBuf[6] | (g_spiHdrBuf[7] << 8));

      if (g_spiPayloadLen > SPI_MAX_PAYLOAD)
      {
        g_spiErrorCount++;
        g_spiState = 0U;
        return;
      }

      g_spiPayloadIdx = 0;
      g_spiState = (g_spiPayloadLen == 0U) ? 3U : 2U;
    }
    return;
  }

  if (g_spiState == 2U)
  {
    g_spiPayloadBuf[g_spiPayloadIdx++] = byte_in;
    if (g_spiPayloadIdx >= g_spiPayloadLen) g_spiState = 3U;
    return;
  }

  if (g_spiState == 3U)
  {
    uint16_t crc = SpiCrc16((const uint8_t*)g_spiPayloadBuf, g_spiPayloadLen);
    if (crc == g_spiChk)
      SpiHandleCommand(g_spiCmd, (const uint8_t*)g_spiPayloadBuf, g_spiPayloadLen, g_spiSeq);
    else
      g_spiErrorCount++;
    g_spiState = 0U;
  }
}

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
  if (hspi->Instance == SPI1)
  {
    SpiParseIncomingByte(g_spiRxByte);
    g_spiTxByte = SpiTxPopByte();
    (void)HAL_SPI_TransmitReceive_IT(&hspi1, (uint8_t*)&g_spiTxByte, (uint8_t*)&g_spiRxByte, 1);
  }
}

void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
  if (hspi->Instance == SPI1)
  {
    SpiParseIncomingByte(g_spiRxByte);
    g_spiTxByte = SpiTxPopByte();
    (void)HAL_SPI_TransmitReceive_IT(&hspi1, (uint8_t*)&g_spiTxByte, (uint8_t*)&g_spiRxByte, 1);
  }
}

static void ProcessSequencerStep(void)
{
  uint32_t s = g_step % TOTAL_STEPS;
  uint32_t phrase = (g_songStep / TOTAL_STEPS) % ARR_SECTIONS;
  uint32_t step16 = s % 16U;
  uint32_t bar = s / 16U;

  g_fxFlangerOn = (phrase == 2U || phrase == 3U || phrase == 6U) ? 1U : 0U;
  g_fxReverbBoost = (phrase >= 4U || (phrase == 1U && bar == 3U)) ? 1U : 0U;
  g_fxSparkleOn = (phrase >= 3U) ? 1U : 0U;

  uint8_t kick = 0;
  uint8_t snare = 0;
  uint8_t ch = 0;
  uint8_t oh = 0;
  uint8_t clap = 0;

  if (step16 == 0U) kick = 1;
  if (step16 == 8U && phrase >= 1U) kick = 1;
  if (step16 == 11U || step16 == 14U) kick = 1;
  if ((phrase & 1U) && step16 == 15U) kick = 1;
  if (phrase >= 2U && (s == 30U || s == 62U)) kick = 1;
  if ((phrase == 1U || phrase == 5U) && bar == 0U && (step16 == 0U || step16 == 8U)) kick = 0;

  if (step16 == 4U || step16 == 12U) snare = 1;
  if (phrase >= 3U && (s == 20U || s == 52U)) snare = 1;
  if (phrase >= 4U && (step16 == 15U) && (bar & 1U)) snare = 1;

  if ((s % 2U) == 0U) ch = 1;
  if ((step16 == 2U || step16 == 10U) && phrase < 2U) ch = 0;
  if (phrase >= 1U && (s % 8U) == 6U) ch = 1;
  if (phrase >= 5U && (step16 == 3U || step16 == 7U || step16 == 11U)) ch = 1;

  if (step16 == 14U || (s == 31U) || (s == 63U)) oh = 1;
  if (phrase >= 5U && step16 == 10U) oh = 1;
  if (phrase >= 6U && step16 == 6U) oh = 1;

  if (phrase >= 2U && ((s % 32U) == 12U || (s % 32U) == 28U)) clap = 1;
  if (phrase >= 5U && (step16 == 12U || step16 == 13U)) clap = 1;

  int32_t seqGain = g_seqVolume;
  if (seqGain < 10) seqGain = 10;
  if (seqGain > 200) seqGain = 200;
  seqGain = (seqGain * g_abSeqTrimQ8) >> 8;

  if (kick)  TriggerVoice(0, (int16_t)((29491 * seqGain * g_trackVolume[0]) / 10000), 0);
  if (snare) TriggerVoice(1, (int16_t)((26214 * seqGain * g_trackVolume[1]) / 10000), 0);
  if (ch)    TriggerVoice(2, (int16_t)((((step16 == 0U || step16 == 8U) ? 17500 : 15800) * seqGain * g_trackVolume[2]) / 10000), (bar & 1U) ? -12 : -5);
  if (oh)    TriggerVoice(3, (int16_t)((19661 * seqGain * g_trackVolume[3]) / 10000), (phrase >= 4U) ? 24 : 12);
  if (clap)  TriggerVoice(4, (int16_t)((22937 * seqGain * g_trackVolume[4]) / 10000), 0);

  if (g_fxSparkleOn && (step16 == 15U))
  {
    TriggerVoice(2, 13500, -36);
    TriggerVoice(2, 13500, 36);
  }

  if ((phrase >= 4U) && (s == 62U || s == 63U))
  {
    TriggerVoice(2, 14500, -25);
    TriggerVoice(2, 14500, 25);
    TriggerVoice(3, 17000, -40);
    TriggerVoice(3, 17000, 40);
    TriggerVoice(4, 22000, 0);
  }

  if ((phrase == 6U || phrase == 7U) && (s % 16U) == 15U)
  {
    TriggerVoice(0, 25000, 0);
    TriggerVoice(1, 22000, -12);
    TriggerVoice(1, 22000, 12);
  }

  g_step = (g_step + 1U) % TOTAL_STEPS;
  g_songStep = (g_songStep + 1U) % ARR_STEPS;
}

static void RenderDemoBuffer(uint16_t *dst, uint32_t words)
{
  g_masterPeakQ15 = (uint16_t)((g_masterPeakQ15 * 240U) >> 8);
  for (uint32_t t = 0; t < TRACK_PEAK_COUNT; t++)
    g_trackPeakQ15[t] = (uint16_t)((g_trackPeakQ15[t] * 240U) >> 8);
  uint32_t frames = words / 2U;
  for (uint32_t i = 0; i < frames; i++)
  {
    if (g_samplesToNextStep == 0)
    {
      ProcessSequencerStep();
      uint32_t swing = g_fxSparkleOn ? (g_samplesPerStep / 8U) : (g_samplesPerStep / 12U);
      if ((g_step & 1U) != 0U)
        g_samplesToNextStep = g_samplesPerStep + swing;
      else
        g_samplesToNextStep = g_samplesPerStep - swing;
      if (g_samplesToNextStep < 8U) g_samplesToNextStep = 8U;
    }
    g_samplesToNextStep--;

    int32_t mixL = 0;
    int32_t mixR = 0;
    uint16_t srcPeak = 0;

    for (uint32_t v = 0; v < MAX_VOICES; v++)
    {
      if (!g_voices[v].active) continue;
      InstrumentSample *sm = &g_samples[g_voices[v].inst];

      if (g_voices[v].pos >= sm->length)
      {
        if (g_padLoopEnabled[g_voices[v].inst])
          g_voices[v].pos = 0;
        else
          g_voices[v].active = 0;
        continue;
      }

      int32_t s = sm->data[g_voices[v].pos];

      if (g_padStutterEnabled[g_voices[v].inst])
      {
        g_padStutterCount[g_voices[v].inst]++;
        if (g_padStutterCount[g_voices[v].inst] >= g_padStutterInterval[g_voices[v].inst])
        {
          g_padStutterCount[g_voices[v].inst] = 0;
          if (g_voices[v].pos > 100U) g_voices[v].pos -= 100U;
          else g_voices[v].pos = 0;
        }
      }

      uint32_t adv = g_voices[v].step_q12 + g_voices[v].frac_q12;
      uint8_t inst = g_voices[v].inst;

      if (g_padScratchActive[inst])
      {
        uint16_t ph = g_padScratchPhase[inst];
        uint16_t tri = (ph < 256U) ? ph : (511U - ph);
        int16_t lfo = (int16_t)tri - 128;
        int32_t modQ8 = 256 + ((lfo * (int32_t)g_padScratchDepthQ8[inst]) >> 8);
        if (modQ8 < 64) modQ8 = 64;
        adv = (uint32_t)(((uint64_t)adv * (uint32_t)modQ8) >> 8);
        uint16_t step = (uint16_t)(1U + (g_padScratchRateQ8[inst] >> 8));
        g_padScratchPhase[inst] = (uint16_t)((ph + step) & 0x01FFU);
      }

      if (g_padTurnActive[inst])
      {
        int8_t mode = g_padTurnMode[inst];
        if (g_padTurnAuto[inst])
        {
          uint16_t ph = g_padTurnPhase[inst];
          mode = (ph & 0x100U) ? 1 : 0;
          uint16_t step = (uint16_t)(1U + (g_padTurnRateQ8[inst] >> 9));
          g_padTurnPhase[inst] = (uint16_t)((ph + step) & 0x01FFU);
        }

        if (mode == 1)
        {
          uint32_t brakeSamples = ((uint32_t)g_padTurnBrakeMs[inst] * DEMO_SAMPLE_RATE) / 1000U;
          if (brakeSamples < 32U) brakeSamples = 32U;
          uint32_t c = g_padTurnCounter[inst]++;
          uint32_t envQ8 = (c >= brakeSamples) ? 0U : (255U - ((c * 255U) / brakeSamples));
          adv = (uint32_t)(((uint64_t)adv * envQ8) >> 8);
          if (adv < 16U) adv = 16U;
        }
        else if (mode == 2)
        {
          uint32_t backspinSamples = ((uint32_t)g_padTurnBackspinMs[inst] * DEMO_SAMPLE_RATE) / 1000U;
          if (backspinSamples < 32U) backspinSamples = 32U;
          uint32_t c = g_padTurnCounter[inst]++;
          if ((c % 3U) == 0U && g_voices[v].pos > 0U) g_voices[v].pos--;
          if (c >= backspinSamples) g_padTurnCounter[inst] = 0U;
          adv = (uint32_t)(((uint64_t)adv * 180U) >> 8);
        }
        else
        {
          g_padTurnCounter[inst] = 0U;
        }
      }

      g_voices[v].pos += (adv >> 12);
      g_voices[v].frac_q12 = (uint16_t)(adv & 0x0FFFU);

      s = (s * g_voices[v].gain_q15) >> 15;

      uint8_t track = g_voices[v].inst % NUM_INSTRUMENTS;

      if (g_trackFilterType[track] != 0U)
      {
        s = ApplyOnePoleFilter(s, g_trackFilterType[track], g_trackFilterCutQ8[track], g_trackFilterResQ8[track], &g_trackFilterState[track]);
      }

      if (g_padFilterType[track] != 0U)
      {
        s = ApplyOnePoleFilter(s, g_padFilterType[track], g_padFilterCutQ8[track], g_padFilterResQ8[track], &g_padFilterState[track]);
      }

      if (g_trackEcho[track].active)
      {
        uint16_t d = g_trackEcho[track].delaySamples;
        if (d == 0U) d = 1U;
        uint16_t wp = g_trackEcho[track].writePos;
        uint16_t rp = (uint16_t)((wp + TRACK_FX_BUF_SAMPLES - d) % TRACK_FX_BUF_SAMPLES);
        int32_t delayed = g_trackEchoBuf[track][rp];
        int32_t writeVal = s + ((delayed * g_trackEcho[track].feedbackQ8) >> 8);
        g_trackEchoBuf[track][wp] = ClipS16(writeVal);
        s = ((s * (256 - g_trackEcho[track].mixQ8)) + (delayed * g_trackEcho[track].mixQ8)) >> 8;
        g_trackEcho[track].writePos = (uint16_t)((wp + 1U) % TRACK_FX_BUF_SAMPLES);
      }

      if (g_trackFlanger[track].active)
      {
        uint16_t wp = g_trackFlanger[track].writePos;
        g_trackFlangerBuf[track][wp] = ClipS16(s);
        uint16_t tri = (g_trackFlanger[track].phase < 256U) ? g_trackFlanger[track].phase : (511U - g_trackFlanger[track].phase);
        uint16_t tap = (uint16_t)(2U + ((tri * g_trackFlanger[track].depthQ8) >> 8));
        uint16_t rp = (uint16_t)((wp + TRACK_FX_BUF_SAMPLES - tap) % TRACK_FX_BUF_SAMPLES);
        int32_t delayed = g_trackFlangerBuf[track][rp];
        int32_t writeVal = s + ((delayed * g_trackFlanger[track].feedbackQ8) >> 8);
        g_trackFlangerBuf[track][wp] = ClipS16(writeVal);
        s = ((s * (256 - g_trackFlanger[track].mixQ8)) + ((s + delayed) * g_trackFlanger[track].mixQ8)) >> 8;
        g_trackFlanger[track].writePos = (uint16_t)((wp + 1U) % TRACK_FX_BUF_SAMPLES);
        g_trackFlanger[track].phase = (uint16_t)((g_trackFlanger[track].phase + 3U) & 0x01FFU);
      }

      if (g_trackComp[track].active)
      {
        uint16_t absS = (uint16_t)((s < 0) ? -s : s);
        uint16_t env = g_trackComp[track].envQ15;
        if (absS > env) env = (uint16_t)(env + ((absS - env) >> 2));
        else env = (uint16_t)(env - ((env - absS) >> 5));
        g_trackComp[track].envQ15 = env;

        if (env > g_trackComp[track].thresholdQ15 && env > 0U)
        {
          int32_t num = (int32_t)g_trackComp[track].thresholdQ15 * 32767;
          int32_t gainQ15 = num / env;
          if (gainQ15 < 4096) gainQ15 = 4096;
          s = (s * gainQ15) >> 15;
        }
      }

      {
        uint16_t distMix = (uint16_t)g_trackDistQ8[track] + (uint16_t)g_padDistQ8[track];
        if (distMix > 255U) distMix = 255U;
        s = ApplySoftDist(s, (uint8_t)distMix);

        uint8_t crushBits = g_trackBitDepth[track];
        if (g_padBitDepth[track] < crushBits) crushBits = g_padBitDepth[track];
        s = ApplyBitCrush(s, crushBits);
      }

      if (g_padScratchActive[track])
      {
        s = ApplyOnePoleFilter(s, 1U, g_padScratchCutQ8[track], 32U, &g_padScratchState[track]);
        uint32_t r = FastRandU32();
        uint16_t density = g_padScratchCrackleQ8[track] >> 2;
        if ((r & 0xFFU) < density)
        {
          int32_t click = (int32_t)((int16_t)(r >> 16)) >> 3;
          s += click;
        }
      }

      if (g_padTurnActive[track] && g_padTurnNoiseQ8[track] > 0U)
      {
        int32_t n = (int16_t)(FastRandU32() >> 16);
        s += (n * g_padTurnNoiseQ8[track]) >> 11;
      }

      if (g_sidechain.active)
      {
        if (track == g_sidechain.sourceTrack)
        {
          uint16_t absS = (uint16_t)((s < 0) ? -s : s);
          if (absS > srcPeak) srcPeak = absS;
        }
        else
        {
          if (g_sidechain.destinationMask & (1U << track))
          {
            uint32_t duck = ((uint32_t)g_sidechain.amountQ8 * g_sidechain.envQ15) >> 15;
            if (duck > 224U) duck = 224U;
            s = (s * (int32_t)(256U - duck)) >> 8;
          }
        }
      }

      {
        uint16_t absS = (uint16_t)((s < 0) ? -s : s);
        if (track < TRACK_PEAK_COUNT && absS > g_trackPeakQ15[track]) g_trackPeakQ15[track] = absS;
      }

      int32_t pan = g_voices[v].pan;
      int32_t gL = 128 - pan;
      int32_t gR = 128 + pan;
      mixL += (s * gL) >> 7;
      mixR += (s * gR) >> 7;
    }

    if (g_sidechain.active)
    {
      uint16_t env = g_sidechain.envQ15;
      if (srcPeak > env)
      {
        uint32_t delta = srcPeak - env;
        env = (uint16_t)(env + ((delta * g_sidechain.attackK) >> 8));
      }
      else
      {
        uint32_t delta = env - srcPeak;
        env = (uint16_t)(env - ((delta * g_sidechain.releaseK) >> 8));
      }
      g_sidechain.envQ15 = env;
    }

    g_lpL += (mixL - g_lpL) >> 3;
    g_lpR += (mixR - g_lpR) >> 3;

    int32_t dl = g_delayL[g_delayIdx];
    int32_t dr = g_delayR[g_delayIdx];

    int32_t delayMixQ8 = g_delayActive ? g_delayMixQ8 : 0;
    int32_t outL = g_lpL + ((dl * delayMixQ8) >> 8);
    int32_t outR = g_lpR + ((dr * delayMixQ8) >> 8);

    int32_t fbQ8 = g_delayActive ? g_delayFbQ8 : 0;
    g_delayL[g_delayIdx] = ClipS16((g_lpL >> 1) + ((dr * fbQ8) >> 9));
    g_delayR[g_delayIdx] = ClipS16((g_lpR >> 1) + ((dl * fbQ8) >> 9));

    if (g_fxFlangerOn && g_flangerEnabled)
    {
      uint32_t tri = (g_flangerPhase < 256U) ? g_flangerPhase : (511U - g_flangerPhase);
      uint32_t depth = g_flangerDepth;
      uint32_t tap = 12U + ((tri * depth) >> 8);
      uint32_t idxF = (g_delayIdx + DELAY_SAMPLES - tap) % DELAY_SAMPLES;
      outL += ((int32_t)g_delayL[idxF] * g_flangerMixQ8) >> 9;
      outR += ((int32_t)g_delayR[idxF] * g_flangerMixQ8) >> 9;
      g_flangerPhase += g_fxSparkleOn ? 5U : 3U;
      if (g_flangerPhase >= 512U) g_flangerPhase = 0;
    }

    if (g_phaserEnabled)
    {
      uint16_t tri = (g_phaserPhase < 256U) ? g_phaserPhase : (511U - g_phaserPhase);
      int32_t aQ8 = 64 + ((tri * g_phaserDepthQ8) >> 8);
      int32_t fbIn = ((int32_t)g_phaserLast * g_phaserFeedbackQ8) >> 8;
      int32_t apL = (outL + fbIn) + ((aQ8 * (int32_t)g_phaserLast) >> 8);
      outL = g_phaserLast - ((aQ8 * apL) >> 8);
      g_phaserLast = (int16_t)ClipS16(apL);
      outR = outR + (outL >> 3);
      g_phaserPhase = (uint16_t)((g_phaserPhase + g_phaserRateStep) & 0x01FFU);
    }

    int32_t mv = g_masterVolume;
    if (mv < 0) mv = 0;
    if (mv > 180) mv = 180;
    outL = (outL * mv) / 100;
    outR = (outR * mv) / 100;
    outL = (outL * g_abMasterTrimQ8) >> 8;
    outR = (outR * g_abMasterTrimQ8) >> 8;

    if (g_globalSrReduce > 1U)
    {
      if (g_globalSrPhase == 0U)
      {
        g_globalSrHoldL = outL;
        g_globalSrHoldR = outR;
      }
      else
      {
        outL = g_globalSrHoldL;
        outR = g_globalSrHoldR;
      }
      g_globalSrPhase++;
      if (g_globalSrPhase >= g_globalSrReduce) g_globalSrPhase = 0U;
    }

    outL = ApplyBitCrush(outL, g_globalBitDepth);
    outR = ApplyBitCrush(outR, g_globalBitDepth);
    outL = ApplyGlobalDistMode(outL);
    outR = ApplyGlobalDistMode(outR);

    outL = ApplyOnePoleFilter(outL, g_globalFilterType, g_globalFilterCutQ8, g_globalFilterResQ8, &g_globalFilterStateL);
    outR = ApplyOnePoleFilter(outR, g_globalFilterType, g_globalFilterCutQ8, g_globalFilterResQ8, &g_globalFilterStateR);

    if (g_masterCompEnabled)
    {
      uint16_t absMix = (uint16_t)(((outL < 0 ? -outL : outL) > (outR < 0 ? -outR : outR)) ? (outL < 0 ? -outL : outL) : (outR < 0 ? -outR : outR));
      uint16_t env = g_masterCompEnvQ15;
      if (absMix > env)
      {
        uint32_t d = absMix - env;
        env = (uint16_t)(env + ((d * g_masterCompAttackK) >> 8));
      }
      else
      {
        uint32_t d = env - absMix;
        env = (uint16_t)(env - ((d * g_masterCompReleaseK) >> 8));
      }
      g_masterCompEnvQ15 = env;

      if (env > g_masterCompThresholdQ15 && env > 0U)
      {
        int32_t gainQ15 = ((int32_t)g_masterCompThresholdQ15 * 32767) / env;
        gainQ15 = (gainQ15 * (int32_t)g_masterCompRatioQ8) >> 8;
        if (gainQ15 > 32767) gainQ15 = 32767;
        if (gainQ15 < 4096) gainQ15 = 4096;
        outL = (outL * gainQ15) >> 15;
        outR = (outR * gainQ15) >> 15;
      }

      outL = (outL * g_masterCompMakeupQ8) >> 8;
      outR = (outR * g_masterCompMakeupQ8) >> 8;
    }

    int32_t absL = (outL < 0) ? -outL : outL;
    int32_t absR = (outR < 0) ? -outR : outR;
    int32_t pk = (absL > absR) ? absL : absR;
    if (pk > 32767) pk = 32767;
    if (pk > g_masterPeakQ15) g_masterPeakQ15 = (uint16_t)pk;

    g_delayIdx++;
    if (g_delayIdx >= DELAY_SAMPLES) g_delayIdx = 0;

    dst[i * 2] = (uint16_t)ClipS16(outL);
    dst[i * 2 + 1] = (uint16_t)ClipS16(outR);
  }
}

int main(void)
{
  HAL_Init();
  Debug_LED_Init();

  Debug_LED_Blink(3);
  SystemClock_Config();
  Debug_LED_Blink(5);

  MX_GPIO_Init();
  MX_DMA_Init();
  MX_SPI1_Init();
  MX_SDIO_SD_Init();
  MX_I2S2_Init();
  Debug_LED_Blink(2);

  g_spiTxByte = 0;
  g_spiRxByte = 0;
  if (HAL_SPI_TransmitReceive_IT(&hspi1, (uint8_t*)&g_spiTxByte, (uint8_t*)&g_spiRxByte, 1) != HAL_OK) Error_Handler();

  memset(g_samples, 0, sizeof(g_samples));
  if (LoadInstrumentFromFolder("BD", &g_samples[0]) != 0) Error_Handler();
  if (LoadInstrumentFromFolder("SD", &g_samples[1]) != 0) Error_Handler();
  if (LoadInstrumentFromFolder("CH", &g_samples[2]) != 0) Error_Handler();
  if (LoadInstrumentFromFolder("OH", &g_samples[3]) != 0) Error_Handler();
  if (LoadInstrumentFromFolder("CP", &g_samples[4]) != 0) Error_Handler();

  g_samplesPerStep = (DEMO_SAMPLE_RATE * 60U) / (DEMO_BPM * 4U);
  g_samplesToNextStep = 1;
  g_step = 0;
  g_songStep = 0;

  HAL_GPIO_WritePin(DEBUG_LED2_PORT, DEBUG_LED2_PIN, GPIO_PIN_SET);

  uint16_t *tx_bufs[2] = { i2sTxBufA, i2sTxBufB };
  uint32_t tx_words = AUDIO_TX_WORDS;
  uint32_t buf_index = 0;

  RenderDemoBuffer(tx_bufs[buf_index], tx_words);
  if (HAL_I2S_Transmit_DMA(&hi2s2, tx_bufs[buf_index], tx_words) != HAL_OK) Error_Handler();
  buf_index ^= 1U;

  while (1)
  {
    ProcessSpiTriggers();
    RenderDemoBuffer(tx_bufs[buf_index], tx_words);

    while (HAL_I2S_GetState(&hi2s2) == HAL_I2S_STATE_BUSY_TX)
    {
      HAL_GPIO_TogglePin(DEBUG_LED_PORT, DEBUG_LED_PIN);
    }

    if (HAL_I2S_Transmit_DMA(&hi2s2, tx_bufs[buf_index], tx_words) == HAL_OK)
      buf_index ^= 1U;
  }
}

void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) Error_Handler();

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK) Error_Handler();

  PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_I2S;
  PeriphClkInitStruct.PLLI2S.PLLI2SN = 192;
  PeriphClkInitStruct.PLLI2S.PLLI2SR = 2;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK) Error_Handler();
}

static void MX_I2S2_Init(void)
{
  hi2s2.Instance = SPI2;
  hi2s2.Init.Mode = I2S_MODE_MASTER_TX;
  hi2s2.Init.Standard = I2S_STANDARD_PHILIPS;
  hi2s2.Init.DataFormat = I2S_DATAFORMAT_16B;
  hi2s2.Init.MCLKOutput = I2S_MCLKOUTPUT_DISABLE;
  hi2s2.Init.AudioFreq = I2S_AUDIOFREQ_44K;
  hi2s2.Init.CPOL = I2S_CPOL_LOW;
  hi2s2.Init.ClockSource = I2S_CLOCK_PLL;
  hi2s2.Init.FullDuplexMode = I2S_FULLDUPLEXMODE_DISABLE;
  if (HAL_I2S_Init(&hi2s2) != HAL_OK) Error_Handler();
}

static void MX_SPI1_Init(void)
{
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_SLAVE;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_HARD_INPUT;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLED;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLED;
  hspi1.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK) Error_Handler();
}

static void MX_DMA_Init(void)
{
  __HAL_RCC_DMA1_CLK_ENABLE();
  HAL_NVIC_SetPriority(DMA1_Stream4_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream4_IRQn);
}

static void MX_SDIO_SD_Init(void)
{
  hsd.Instance = SDIO;
  hsd.Init.ClockEdge = SDIO_CLOCK_EDGE_RISING;
  hsd.Init.ClockBypass = SDIO_CLOCK_BYPASS_DISABLE;
  hsd.Init.ClockPowerSave = SDIO_CLOCK_POWER_SAVE_DISABLE;
  hsd.Init.BusWide = SDIO_BUS_WIDE_1B;
  hsd.Init.HardwareFlowControl = SDIO_HARDWARE_FLOW_CONTROL_DISABLE;
  hsd.Init.ClockDiv = 2;
  if (HAL_SD_Init(&hsd) != HAL_OK) Error_Handler();
  if (HAL_SD_ConfigWideBusOperation(&hsd, SDIO_BUS_WIDE_4B) != HAL_OK) Error_Handler();
}

static void MX_GPIO_Init(void)
{
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
}

void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
    HAL_GPIO_TogglePin(DEBUG_LED3_PORT, DEBUG_LED3_PIN);
    HAL_Delay(80);
  }
}
