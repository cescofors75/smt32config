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

I2S_HandleTypeDef hi2s2;
DMA_HandleTypeDef hdma_spi2_tx;
SD_HandleTypeDef hsd;

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

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_I2S2_Init(void);
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
static void ProcessSequencerStep(void);
static void RenderDemoBuffer(uint16_t *dst, uint32_t words);

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)); }

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
  g_voices[slot].gain_q15 = gain_q15;
  g_voices[slot].pan = pan;
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

  if (kick)  TriggerVoice(0, 29491, 0);
  if (snare) TriggerVoice(1, 26214, 0);
  if (ch)    TriggerVoice(2, (step16 == 0U || step16 == 8U) ? 17500 : 15800, (bar & 1U) ? -12 : -5);
  if (oh)    TriggerVoice(3, 19661, (phrase >= 4U) ? 24 : 12);
  if (clap)  TriggerVoice(4, 22937, 0);

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

    for (uint32_t v = 0; v < MAX_VOICES; v++)
    {
      if (!g_voices[v].active) continue;
      InstrumentSample *sm = &g_samples[g_voices[v].inst];

      if (g_voices[v].pos >= sm->length)
      {
        g_voices[v].active = 0;
        continue;
      }

      int32_t s = sm->data[g_voices[v].pos++];
      s = (s * g_voices[v].gain_q15) >> 15;

      int32_t pan = g_voices[v].pan;
      int32_t gL = 128 - pan;
      int32_t gR = 128 + pan;
      mixL += (s * gL) >> 7;
      mixR += (s * gR) >> 7;
    }

    g_lpL += (mixL - g_lpL) >> 3;
    g_lpR += (mixR - g_lpR) >> 3;

    int32_t dl = g_delayL[g_delayIdx];
    int32_t dr = g_delayR[g_delayIdx];

    int32_t delayMix = g_fxReverbBoost ? 3 : 2;
    int32_t outL = g_lpL + ((dl * delayMix) >> 2);
    int32_t outR = g_lpR + ((dr * delayMix) >> 2);

    int32_t fb = g_fxReverbBoost ? 3 : 2;
    g_delayL[g_delayIdx] = ClipS16((g_lpL >> 1) + ((dr * fb) >> 3));
    g_delayR[g_delayIdx] = ClipS16((g_lpR >> 1) + ((dl * fb) >> 3));

    if (g_fxFlangerOn)
    {
      uint32_t tri = (g_flangerPhase < 256U) ? g_flangerPhase : (511U - g_flangerPhase);
      uint32_t tap = g_fxSparkleOn ? (18U + ((tri * 170U) >> 8)) : (24U + ((tri * 120U) >> 8));
      uint32_t idxF = (g_delayIdx + DELAY_SAMPLES - tap) % DELAY_SAMPLES;
      outL += (int32_t)g_delayL[idxF] >> 2;
      outR += (int32_t)g_delayR[idxF] >> 2;
      g_flangerPhase += g_fxSparkleOn ? 5U : 3U;
      if (g_flangerPhase >= 512U) g_flangerPhase = 0;
    }

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
  MX_SDIO_SD_Init();
  MX_I2S2_Init();
  Debug_LED_Blink(2);

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
