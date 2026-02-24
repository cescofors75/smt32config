# STM32 SPI Audio Slave — Guía Completa

## Arquitectura General: ESP32-S3 (Master) ↔ STM32 (Slave)

```
┌─────────────────────────────────────┐     SPI @ 40MHz      ┌──────────────────────────────────┐
│         ESP32-S3 (MASTER)           │◄════════════════════►│         STM32 (SLAVE)             │
│                                     │  MOSI/MISO/SCK/CS    │                                  │
│  Core 0: WiFi, Web, Sequencer,      │                      │  Dedicado 100% a Audio DSP:      │
│          MIDI, UDP, LED             │  + GPIO IRQ ────────►│  - I2S DAC output (44.1kHz/16b)  │
│                                     │  (STM32 → ESP32)     │  - 10 voces polifónicas          │
│  Core 1: SPI Master + Sample TX     │                      │  - Filtros Biquad por canal      │
│          (antes era AudioEngine)     │  + SYNC pin ────────►│  - Master FX chain completa      │
│                                     │  (ESP32 → STM32)     │  - Sidechain PRO                 │
│  LittleFS: samples .wav/.raw        │                      │  - Per-track Echo/Flanger/Comp   │
│  PSRAM 8MB: sample buffers          │                      │  - VU meters por track + master  │
│  WiFi AP/STA: interfaz web          │                      │  - SRAM/Flash para sample cache  │
└─────────────────────────────────────┘                      └──────────────────────────────────┘
```

---

## 1. ¿Por qué STM32 para Audio?

| Aspecto | ESP32-S3 (actual) | STM32 (propuesto) |
|---------|-------------------|-------------------|
| **CPU** | Xtensa LX7 dual 240MHz | Cortex-M4F/M7 hasta 480MHz |
| **FPU** | Básica | Hardware FPU (single/double) |
| **DSP** | Sin instrucciones DSP | SIMD, MAC, saturación nativa |
| **I2S** | Compartido con WiFi IRQs | Dedicado, DMA sin interrupciones |
| **Latencia** | ~2.9ms (con jitter WiFi) | <1ms determinista |
| **Filtros** | Biquad en float (lento) | Biquad en CMSIS-DSP (optimizado) |
| **Voces** | 10 con ~60% CPU | 16+ con <30% CPU |

### STM32 Recomendados

| Chip | Clock | SRAM | Flash | FPU | Precio aprox. |
|------|-------|------|-------|-----|---------------|
| **STM32F411CE** | 100MHz M4F | 128KB | 512KB | SP | ~3€ (BlackPill) |
| **STM32F446RE** | 180MHz M4F | 128KB | 512KB | SP | ~8€ (Nucleo) |
| **STM32H743** | 480MHz M7 | 1MB | 2MB | DP | ~15€ |
| **STM32F407VG** | 168MHz M4F | 192KB | 1MB | SP | ~5€ (BlackPill) |

**Recomendación**: **STM32F411CE (BlackPill)** para empezar — barato, FPU, suficiente SRAM para 10 voces con FX.

---

## 2. Conexión Hardware SPI

### Pinout ESP32-S3 ↔ STM32

```
ESP32-S3 (Master)          STM32 (Slave)
═══════════════           ═══════════════
GPIO 12 (MOSI) ────────► PA7  (SPI1_MOSI)
GPIO 13 (MISO) ◄──────── PA6  (SPI1_MISO)
GPIO 14 (SCK)  ────────► PA5  (SPI1_SCK)
GPIO 15 (CS)   ────────► PA4  (SPI1_NSS)
GPIO 16 (SYNC) ────────► PB0  (EXTI - IRQ ready)
GPIO 17 (IRQ)  ◄──────── PB1  (STM32 interrupt → ESP32)
GND            ════════   GND
3.3V           ════════   3.3V
```

### Señales de Control

| Pin | Dirección | Función |
|-----|-----------|---------|
| **SYNC** | ESP32 → STM32 | Pulso HIGH cuando hay comando listo |
| **IRQ** | STM32 → ESP32 | STM32 solicita datos (sample data, status ready) |

### SPI Config

```
Mode:        SPI Mode 0 (CPOL=0, CPHA=0)
Speed:       40 MHz (máximo práctico para cables cortos)
Bit Order:   MSB First
Frame:       8-bit
DMA:         Habilitado en ambos lados
```

---

## 3. Protocolo SPI — Formato de Paquetes

### 3.1 Estructura del Paquete (Header fijo 8 bytes)

```c
// Paquete SPI: Header (8 bytes) + Payload (variable)
typedef struct __attribute__((packed)) {
    uint8_t  magic;      // 0xA5 = comando, 0x5A = respuesta, 0xDA = sample data
    uint8_t  cmd;        // Código de comando (ver tabla)
    uint16_t length;     // Longitud del payload en bytes
    uint16_t sequence;   // Número de secuencia (para verificación)
    uint16_t checksum;   // CRC16 del payload
} SPIPacketHeader;

#define SPI_MAGIC_CMD      0xA5
#define SPI_MAGIC_RESP     0x5A
#define SPI_MAGIC_SAMPLE   0xDA
#define SPI_MAGIC_BULK     0xBB  // Bulk multi-command
```

### 3.2 Tabla de Comandos

```c
// ═══════════════════════════════════════════════════════
// COMANDOS DE TRIGGER (0x01 - 0x0F)
// ═══════════════════════════════════════════════════════
#define CMD_TRIGGER_SEQ       0x01  // Trigger desde secuenciador
#define CMD_TRIGGER_LIVE      0x02  // Trigger desde pad live
#define CMD_TRIGGER_STOP      0x03  // Parar sample específico
#define CMD_TRIGGER_STOP_ALL  0x04  // Parar todos los samples
#define CMD_TRIGGER_SIDECHAIN 0x05  // Trigger sidechain

// ═══════════════════════════════════════════════════════
// COMANDOS DE VOLUMEN (0x10 - 0x1F)
// ═══════════════════════════════════════════════════════
#define CMD_MASTER_VOLUME     0x10  // Volumen master (0-100)
#define CMD_SEQ_VOLUME        0x11  // Volumen secuenciador (0-150)
#define CMD_LIVE_VOLUME       0x12  // Volumen live pads (0-180)
#define CMD_TRACK_VOLUME      0x13  // Volumen por track (0-150)

// ═══════════════════════════════════════════════════════
// COMANDOS DE FILTRO GLOBAL (0x20 - 0x2F)
// ═══════════════════════════════════════════════════════
#define CMD_FILTER_TYPE       0x20  // Tipo de filtro global
#define CMD_FILTER_CUTOFF     0x21  // Frecuencia de corte
#define CMD_FILTER_RESONANCE  0x22  // Resonancia (Q)
#define CMD_FILTER_BITDEPTH   0x23  // Bit crush
#define CMD_FILTER_DISTORTION 0x24  // Distorsión global
#define CMD_FILTER_DIST_MODE  0x25  // Modo distorsión (soft/hard/tube/fuzz)
#define CMD_FILTER_SR_REDUCE  0x26  // Sample rate reduction

// ═══════════════════════════════════════════════════════
// COMANDOS DE EFECTOS MASTER (0x30 - 0x4F)
// ═══════════════════════════════════════════════════════
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

// ═══════════════════════════════════════════════════════
// COMANDOS PER-TRACK FX (0x50 - 0x6F)
// ═══════════════════════════════════════════════════════
#define CMD_TRACK_FILTER      0x50  // Filtro per-track
#define CMD_TRACK_CLEAR_FX    0x51  // Limpiar FX de track
#define CMD_TRACK_DISTORTION  0x52  // Distorsión per-track
#define CMD_TRACK_BITCRUSH    0x53  // Bitcrush per-track
#define CMD_TRACK_ECHO        0x54  // Echo per-track
#define CMD_TRACK_FLANGER_FX  0x55  // Flanger per-track
#define CMD_TRACK_COMPRESSOR  0x56  // Compresor per-track
#define CMD_TRACK_CLEAR_LIVE  0x57  // Limpiar live FX de track

// ═══════════════════════════════════════════════════════
// COMANDOS PER-PAD FX (0x70 - 0x8F)
// ═══════════════════════════════════════════════════════
#define CMD_PAD_FILTER        0x70  // Filtro per-pad
#define CMD_PAD_CLEAR_FX      0x71  // Limpiar FX de pad
#define CMD_PAD_DISTORTION    0x72  // Distorsión per-pad
#define CMD_PAD_BITCRUSH      0x73  // Bitcrush per-pad
#define CMD_PAD_LOOP          0x74  // Loop continuo pad
#define CMD_PAD_REVERSE       0x75  // Reverse sample
#define CMD_PAD_PITCH         0x76  // Pitch shift per-pad
#define CMD_PAD_STUTTER       0x77  // Stutter effect
#define CMD_PAD_SCRATCH       0x78  // Scratch vinyl
#define CMD_PAD_TURNTABLISM   0x79  // Turntablism DJ

// ═══════════════════════════════════════════════════════
// COMANDOS DE SIDECHAIN (0x90 - 0x9F)
// ═══════════════════════════════════════════════════════
#define CMD_SIDECHAIN_SET     0x90  // Configurar sidechain
#define CMD_SIDECHAIN_CLEAR   0x91  // Desactivar sidechain

// ═══════════════════════════════════════════════════════
// COMANDOS DE SAMPLE DATA (0xA0 - 0xAF)
// ═══════════════════════════════════════════════════════
#define CMD_SAMPLE_BEGIN      0xA0  // Iniciar transferencia de sample
#define CMD_SAMPLE_DATA       0xA1  // Bloque de datos del sample
#define CMD_SAMPLE_END        0xA2  // Finalizar transferencia
#define CMD_SAMPLE_UNLOAD     0xA3  // Descargar sample de memoria
#define CMD_SAMPLE_UNLOAD_ALL 0xA4  // Descargar todos

// ═══════════════════════════════════════════════════════
// COMANDOS DE STATUS / QUERY (0xE0 - 0xEF)
// ═══════════════════════════════════════════════════════
#define CMD_GET_STATUS        0xE0  // Pedir estado general
#define CMD_GET_PEAKS         0xE1  // Pedir VU meters (16 tracks + master)
#define CMD_GET_CPU_LOAD      0xE2  // Pedir carga CPU
#define CMD_GET_VOICES        0xE3  // Pedir voces activas
#define CMD_PING              0xEE  // Ping/Pong (verificar conexión)
#define CMD_RESET             0xEF  // Reset DSP completo

// ═══════════════════════════════════════════════════════
// BULK COMMANDS (0xF0 - 0xFF) — Múltiples comandos en 1 SPI transaction
// ═══════════════════════════════════════════════════════
#define CMD_BULK_TRIGGERS     0xF0  // Múltiples triggers en un paquete
#define CMD_BULK_FX           0xF1  // Múltiples cambios FX
```

---

## 4. Payloads Detallados — Triggers

### 4.1 CMD_TRIGGER_SEQ (0x01) — Trigger Secuenciador

Enviado por el callback `onStepTrigger()` cuando el sequencer dispara un step.

```c
// Payload: 6 bytes
typedef struct __attribute__((packed)) {
    uint8_t  padIndex;      // 0-15 (track del secuenciador)
    uint8_t  velocity;      // 1-127 (MIDI velocity)
    uint8_t  trackVolume;   // 0-150 (volumen del track)
    uint8_t  reserved;      // Alineación
    uint32_t maxSamples;    // 0 = full sample, >0 = cortar después de N samples (note length)
} TriggerSeqPayload;
```

**Ejemplo SPI completo (14 bytes):**
```
A5 01 00 06 00 01 XX XX | 00 7F 64 00 00 00 00 00
│  │  │     │     │       │  │   │  │  └─ maxSamples = 0 (full)
│  │  │     │     │       │  │   │  └── reserved
│  │  │     │     │       │  │   └── trackVolume = 100
│  │  │     │     │       │  └── velocity = 127
│  │  │     │     │       └── padIndex = 0 (BD/Kick)
│  │  │     │     └── checksum
│  │  │     └── sequence = 1
│  │  └── length = 6
│  └── cmd = TRIGGER_SEQ
└── magic = 0xA5
```

### 4.2 CMD_TRIGGER_LIVE (0x02) — Trigger Live Pad

Enviado cuando el usuario toca un pad en la web o MIDI.

```c
// Payload: 2 bytes
typedef struct __attribute__((packed)) {
    uint8_t  padIndex;      // 0-23 (16 seq + 8 XTRA)
    uint8_t  velocity;      // 1-127
} TriggerLivePayload;
```

### 4.3 CMD_BULK_TRIGGERS (0xF0) — Múltiples Triggers (Optimización)

Cuando el sequencer dispara múltiples tracks en el mismo step (ej: kick + hi-hat + clap):

```c
// Payload: 2 + N*6 bytes
typedef struct __attribute__((packed)) {
    uint8_t  count;         // Número de triggers (1-16)
    uint8_t  reserved;
    TriggerSeqPayload triggers[];  // Array variable de triggers
} BulkTriggersPayload;
```

**Ejemplo: Step con BD + CH + CP (3 triggers simultáneos):**
```
A5 F0 00 14 00 05 XX XX |  ← Header (8 bytes)
03 00                    |  ← count=3
00 7F 64 00 00 00 00 00  |  ← BD: pad=0, vel=127, vol=100, full
02 60 64 00 00 00 00 00  |  ← CH: pad=2, vel=96, vol=100, full
05 50 64 00 00 00 00 00  |  ← CP: pad=5, vel=80, vol=100, full
```

---

## 5. Payloads Detallados — Efectos y Filtros

### 5.1 CMD_FILTER_TYPE (0x20) + Parámetros

```c
// Payload global filter: 16 bytes
typedef struct __attribute__((packed)) {
    uint8_t  filterType;    // enum FilterType (0-14)
    uint8_t  distMode;      // enum DistortionMode (0-3)
    uint8_t  bitDepth;      // 1-16
    uint8_t  reserved;
    float    cutoff;        // Hz (20.0 - 20000.0)
    float    resonance;     // Q (0.1 - 30.0)
    float    distortion;    // 0.0 - 100.0
} GlobalFilterPayload;
```

### 5.2 CMD_TRACK_FILTER (0x50) — Filtro Per-Track

```c
// Payload: 17 bytes
typedef struct __attribute__((packed)) {
    uint8_t  track;         // 0-15
    uint8_t  filterType;    // enum FilterType
    uint8_t  reserved[2];
    float    cutoff;        // Hz
    float    resonance;     // Q
    float    gain;          // dB (para peaking/shelf)
} TrackFilterPayload;
```

### 5.3 CMD_TRACK_ECHO (0x54)

```c
// Payload: 16 bytes
typedef struct __attribute__((packed)) {
    uint8_t  track;         // 0-15
    uint8_t  active;        // 0/1
    uint8_t  reserved[2];
    float    time;          // ms (10-200)
    float    feedback;      // 0.0-90.0 (% → 0.0-0.9 interno)
    float    mix;           // 0.0-100.0 (% → 0.0-1.0 interno)
} TrackEchoPayload;
```

### 5.4 CMD_DELAY_ACTIVE (0x30) + Parámetros Master

```c
// Enviar como comandos individuales o como BULK:
// CMD_DELAY_ACTIVE:  payload = { uint8_t active }           (1 byte)
// CMD_DELAY_TIME:    payload = { float time_ms }            (4 bytes)
// CMD_DELAY_FEEDBACK: payload = { float feedback }          (4 bytes)
// CMD_DELAY_MIX:     payload = { float mix }                (4 bytes)

// O más eficiente con CMD_BULK_FX (0xF1):
typedef struct __attribute__((packed)) {
    uint8_t  count;         // Número de sub-comandos
    struct {
        uint8_t cmd;        // Sub-comando (0x30-0x42)
        uint8_t len;        // Longitud valor
        uint8_t data[];     // Valor (1-4 bytes)
    } entries[];
} BulkFXPayload;
```

### 5.5 CMD_SIDECHAIN_SET (0x90)

```c
// Payload: 20 bytes
typedef struct __attribute__((packed)) {
    uint8_t  active;        // 0/1
    uint8_t  sourceTrack;   // 0-15
    uint16_t destMask;      // Bitmask de tracks destino
    float    amount;        // 0.0-1.0
    float    attackMs;      // 0.1-80.0 ms
    float    releaseMs;     // 10.0-1200.0 ms
    float    knee;          // 0.0-1.0
} SidechainPayload;
```

### 5.6 CMD_PAD_SCRATCH (0x78)

```c
// Payload: 20 bytes
typedef struct __attribute__((packed)) {
    uint8_t  padIndex;      // 0-23
    uint8_t  active;        // 0/1
    uint8_t  reserved[2];
    float    rate;          // Hz (0.5-20.0)
    float    depth;         // 0.0-1.0
    float    filterCutoff;  // Hz (500-12000)
    float    crackle;       // 0.0-1.0
} ScratchPayload;
```

### 5.7 CMD_PAD_TURNTABLISM (0x79)

```c
// Payload: 24 bytes
typedef struct __attribute__((packed)) {
    uint8_t  padIndex;       // 0-23
    uint8_t  active;         // 0/1
    uint8_t  autoMode;       // 0/1
    int8_t   mode;           // -1=auto, 0-3=manual
    uint16_t brakeMs;        // ms
    uint16_t backspinMs;     // ms
    float    transformRate;  // Hz
    float    vinylNoise;     // 0.0-1.0
    // Total: 16 bytes
} TurntablismPayload;
```

---

## 6. Transferencia de Samples por SPI

Los samples se cargan en la PSRAM del ESP32-S3 desde LittleFS y se transfieren al STM32 via SPI.

### 6.1 Protocolo de Transferencia

```
┌───────────┐                           ┌──────────┐
│  ESP32-S3  │                           │  STM32   │
│  (Master)  │                           │  (Slave) │
│            │                           │          │
│  1. CMD_SAMPLE_BEGIN ──────────────►   │          │
│     { pad=0, totalBytes=88200,         │  Alloc   │
│       sampleRate=44100, bits=16 }      │  buffer  │
│            │                           │          │
│  2. CMD_SAMPLE_DATA ──────────────►    │          │
│     { pad=0, offset=0,                 │  Store   │
│       data[512 bytes] }                │  chunk   │
│            │                           │          │
│  3. CMD_SAMPLE_DATA ──────────────►    │          │
│     { pad=0, offset=512, ... }         │  Store   │
│            │         ...               │  chunk   │
│            │                           │          │
│  N. CMD_SAMPLE_END ──────────────►     │          │
│     { pad=0, totalSamples=44100 }      │  Verify  │
│            │                           │  ✓ ACK   │
│            │  ◄────────────────  IRQ   │          │
└───────────┘                           └──────────┘
```

### 6.2 Payloads de Sample Transfer

```c
// CMD_SAMPLE_BEGIN (0xA0)
typedef struct __attribute__((packed)) {
    uint8_t  padIndex;       // 0-23
    uint8_t  bitsPerSample;  // 16
    uint16_t sampleRate;     // 44100 típicamente
    uint32_t totalBytes;     // Tamaño total de datos PCM
    uint32_t totalSamples;   // Número total de muestras int16_t
} SampleBeginPayload;        // 12 bytes

// CMD_SAMPLE_DATA (0xA1)
typedef struct __attribute__((packed)) {
    uint8_t  padIndex;       // 0-23
    uint8_t  reserved;
    uint16_t chunkSize;      // Bytes en este chunk (max 512)
    uint32_t offset;         // Offset en bytes desde el inicio
    int16_t  data[256];      // Hasta 256 muestras (512 bytes)
} SampleDataPayload;         // 8 + 512 = 520 bytes max

// CMD_SAMPLE_END (0xA2)
typedef struct __attribute__((packed)) {
    uint8_t  padIndex;       // 0-23
    uint8_t  status;         // 0=OK, 1=error
    uint16_t reserved;
    uint32_t checksum;       // CRC32 de todos los datos
} SampleEndPayload;          // 8 bytes
```

### 6.3 Gestión de Memoria en STM32

```c
// STM32F411 (128KB SRAM):
// - 16 tracks × ~2-4 KB por sample corto (808 drums) = ~64KB
// - Para samples largos: usar flash externa SPI (W25Q128) o limitar a ~4KB/sample
//
// STM32H743 (1MB SRAM):
// - 24 pads × ~40KB por sample = ~960KB (cabe todo en SRAM)

#define MAX_STM32_SAMPLES    24
#define MAX_SAMPLE_BYTES     8192   // 4096 muestras × 2 bytes (STM32F411)
// #define MAX_SAMPLE_BYTES  81920  // 40960 muestras (STM32H743)

int16_t* sampleBuffers[MAX_STM32_SAMPLES];
uint32_t sampleLengths[MAX_STM32_SAMPLES];
```

---

## 7. Respuestas del STM32 → ESP32-S3

### 7.1 CMD_GET_PEAKS (0xE1) — VU Meters

```c
// Respuesta: 68 bytes
typedef struct __attribute__((packed)) {
    float trackPeaks[16];   // 0.0-1.0 por track (64 bytes)
    float masterPeak;       // 0.0-1.0 master (4 bytes)
} PeaksResponse;
```

El ESP32-S3 pide peaks cada ~50ms (20Hz) y los reenvía por WebSocket a la web.

### 7.2 CMD_GET_STATUS (0xE0)

```c
// Respuesta: 16 bytes
typedef struct __attribute__((packed)) {
    uint8_t  activeVoices;    // 0-10
    uint8_t  cpuLoadPercent;  // 0-100
    uint16_t freeSRAM;        // KB libres
    uint32_t samplesLoaded;   // Bitmask de pads con sample
    uint32_t uptime;          // Segundos desde boot
    uint16_t spiErrors;       // Contador de errores SPI
    uint16_t bufferUnderruns; // I2S underruns
} StatusResponse;
```

### 7.3 CMD_PING (0xEE)

```c
// Payload: 4 bytes
typedef struct __attribute__((packed)) {
    uint32_t timestamp;    // millis() del ESP32
} PingPayload;

// Respuesta: 8 bytes
typedef struct __attribute__((packed)) {
    uint32_t echoTimestamp; // El mismo timestamp devuelto
    uint32_t stm32Uptime;  // millis() del STM32
} PongResponse;
```

---

## 8. Mapeo JSON WebSocket → Comando SPI

El ESP32 recibe JSON via WebSocket y los traduce a comandos SPI binarios:

### 8.1 Triggers

```json
// WebSocket entrante (del navegador):
{"type": "trigger", "pad": 0, "velocity": 127}

// → ESP32 traduce a SPI:
// CMD_TRIGGER_LIVE (0x02), payload: {padIndex=0, velocity=127}
```

```json
// Secuenciador interno (callback onStepTrigger):
// track=0, velocity=100, trackVolume=80, noteLenSamples=5512

// → SPI: CMD_TRIGGER_SEQ (0x01)
// payload: {padIndex=0, velocity=100, trackVolume=80, maxSamples=5512}
```

### 8.2 Filtros y FX

```json
// WebSocket: Filtro global
{"type": "filter", "filterType": 1, "cutoff": 2000, "resonance": 3.5}
// → SPI: CMD_FILTER_TYPE (0x20)
// payload: GlobalFilterPayload{type=1, cutoff=2000.0, resonance=3.5, ...}
```

```json
// WebSocket: Delay master
{"type": "delay", "active": true, "time": 250, "feedback": 30, "mix": 40}
// → SPI: CMD_BULK_FX (0xF1) con 4 sub-comandos:
//   CMD_DELAY_ACTIVE(1), CMD_DELAY_TIME(250.0),
//   CMD_DELAY_FEEDBACK(0.3), CMD_DELAY_MIX(0.4)
```

```json
// WebSocket: Per-track echo
{"type": "trackEcho", "track": 3, "active": true, "time": 100, "feedback": 40, "mix": 50}
// → SPI: CMD_TRACK_ECHO (0x54)
// payload: TrackEchoPayload{track=3, active=1, time=100.0, feedback=40.0, mix=50.0}
```

```json
// WebSocket: Sidechain
{"type": "sidechain", "active": true, "source": 0, "destinations": [1,2,3,5],
 "amount": 0.8, "attack": 6, "release": 160, "knee": 0.4}
// → SPI: CMD_SIDECHAIN_SET (0x90)
// payload: destMask = 0b0000000000101110 = 0x002E
```

```json
// WebSocket: Scratch
{"type": "scratch", "pad": 5, "active": true, "rate": 8.0, "depth": 0.9,
 "filterCutoff": 3000, "crackle": 0.3}
// → SPI: CMD_PAD_SCRATCH (0x78)
```

```json
// WebSocket: Volumen
{"type": "masterVolume", "value": 85}
// → SPI: CMD_MASTER_VOLUME (0x10), payload: {85}

{"type": "trackVolume", "track": 5, "value": 120}
// → SPI: CMD_TRACK_VOLUME (0x13), payload: {track=5, volume=120}
```

### 8.3 VU Meters (STM32 → ESP32 → WebSocket)

```json
// ESP32 envía cada 50ms al navegador:
{
  "type": "peaks",
  "tracks": [0.85, 0.0, 0.45, 0.0, 0.0, 0.12, 0.0, 0.0,
             0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
  "master": 0.72
}
```

---

## 9. Código ESP32-S3 — SPI Master

### 9.1 SPIMaster.h (Nuevo archivo, reemplaza AudioEngine)

```cpp
#ifndef SPI_MASTER_H
#define SPI_MASTER_H

#include <Arduino.h>
#include <SPI.h>

// SPI Pins
#define SPI_MOSI   12
#define SPI_MISO   13
#define SPI_SCK    14
#define SPI_CS     15
#define SPI_SYNC   16  // ESP32 → STM32: "comando listo"
#define SPI_IRQ    17  // STM32 → ESP32: "necesito datos / status listo"

// SPI Speed
#define SPI_CLOCK  40000000  // 40 MHz

// Packet constants
#define SPI_MAGIC_CMD    0xA5
#define SPI_MAGIC_RESP   0x5A
#define SPI_MAGIC_SAMPLE 0xDA
#define SPI_MAX_PAYLOAD  528  // 8 header + 520 sample data

// Packet header
typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint8_t  cmd;
    uint16_t length;
    uint16_t sequence;
    uint16_t checksum;
} SPIPacketHeader;

class SPIMaster {
public:
    SPIMaster();
    
    bool begin();
    
    // ══════ Triggers ══════
    void triggerSequencer(uint8_t pad, uint8_t velocity, uint8_t trackVolume, uint32_t maxSamples);
    void triggerLive(uint8_t pad, uint8_t velocity);
    void triggerBulk(uint8_t count, uint8_t* pads, uint8_t* velocities, 
                     uint8_t* volumes, uint32_t* maxSamples);
    void stopSample(uint8_t pad);
    void stopAll();
    
    // ══════ Volumen ══════
    void setMasterVolume(uint8_t vol);
    void setSequencerVolume(uint8_t vol);
    void setLiveVolume(uint8_t vol);
    void setTrackVolume(uint8_t track, uint8_t vol);
    
    // ══════ Filtros Globales ══════
    void setGlobalFilter(uint8_t type, float cutoff, float resonance,
                         float distortion, uint8_t distMode, uint8_t bitDepth);
    
    // ══════ Master FX ══════
    void setDelay(bool active, float timeMs, float feedback, float mix);
    void setPhaser(bool active, float rate, float depth, float feedback);
    void setFlanger(bool active, float rate, float depth, float feedback, float mix);
    void setCompressor(bool active, float threshold, float ratio, float attackMs, 
                       float releaseMs, float makeupDb);
    
    // ══════ Per-Track FX ══════
    void setTrackFilter(uint8_t track, uint8_t type, float cutoff, float resonance, float gain);
    void clearTrackFilter(uint8_t track);
    void setTrackEcho(uint8_t track, bool active, float time, float feedback, float mix);
    void setTrackFlanger(uint8_t track, bool active, float rate, float depth, float feedback);
    void setTrackCompressor(uint8_t track, bool active, float threshold, float ratio);
    void clearTrackLiveFX(uint8_t track);
    
    // ══════ Per-Pad FX ══════
    void setPadFilter(uint8_t pad, uint8_t type, float cutoff, float resonance, float gain);
    void clearPadFilter(uint8_t pad);
    void setPadLoop(uint8_t pad, bool enabled);
    void setPadReverse(uint8_t pad, bool reverse);
    void setPadPitch(uint8_t pad, float pitch);
    void setPadStutter(uint8_t pad, bool active, uint16_t intervalMs);
    void setPadScratch(uint8_t pad, bool active, float rate, float depth, float cutoff, float crackle);
    void setPadTurntablism(uint8_t pad, bool active, bool autoMode, int8_t mode,
                           uint16_t brakeMs, uint16_t backspinMs, float transformRate, float vinylNoise);
    
    // ══════ Sidechain ══════
    void setSidechain(bool active, uint8_t source, uint16_t destMask,
                      float amount, float attackMs, float releaseMs, float knee);
    void clearSidechain();
    
    // ══════ Sample Transfer ══════
    bool transferSample(uint8_t padIndex, int16_t* buffer, uint32_t numSamples);
    void unloadSample(uint8_t padIndex);
    void unloadAllSamples();
    
    // ══════ Status / Queries ══════
    bool getStatus(uint8_t& voices, uint8_t& cpu, uint16_t& freeSram, uint16_t& spiErrors);
    bool getPeaks(float* trackPeaks, float& masterPeak);
    bool ping(uint32_t& roundtripUs);
    void resetDSP();
    
    // Estado interno
    uint32_t getSPIErrors() { return spiErrorCount; }
    bool isConnected() { return stm32Connected; }

private:
    SPIClass* spi;
    uint16_t seqNumber;
    uint32_t spiErrorCount;
    bool stm32Connected;
    
    // Buffer para SPI transactions
    uint8_t txBuffer[SPI_MAX_PAYLOAD];
    uint8_t rxBuffer[SPI_MAX_PAYLOAD];
    
    // Envío genérico
    bool sendCommand(uint8_t cmd, const void* payload, uint16_t payloadLen);
    bool sendAndReceive(uint8_t cmd, const void* payload, uint16_t payloadLen,
                        void* response, uint16_t responseLen);
    
    // CRC16
    uint16_t crc16(const uint8_t* data, uint16_t len);
    
    // CS control
    void csLow()  { digitalWrite(SPI_CS, LOW); }
    void csHigh() { digitalWrite(SPI_CS, HIGH); }
    void syncPulse(); // Pulso en SYNC pin
};

#endif // SPI_MASTER_H
```

### 9.2 Ejemplo de implementación (extracto SPIMaster.cpp)

```cpp
#include "SPIMaster.h"

SPIMaster::SPIMaster() : seqNumber(0), spiErrorCount(0), stm32Connected(false) {
    spi = nullptr;
}

bool SPIMaster::begin() {
    // Configurar pines
    pinMode(SPI_CS, OUTPUT);
    pinMode(SPI_SYNC, OUTPUT);
    pinMode(SPI_IRQ, INPUT_PULLUP);
    
    digitalWrite(SPI_CS, HIGH);
    digitalWrite(SPI_SYNC, LOW);
    
    // Iniciar SPI en HSPI (no interfiere con PSRAM que usa SPI0)
    spi = new SPIClass(HSPI);
    spi->begin(SPI_SCK, SPI_MISO, SPI_MOSI, SPI_CS);
    
    // Verificar conexión con STM32
    uint32_t rtt;
    for (int i = 0; i < 5; i++) {
        if (ping(rtt)) {
            stm32Connected = true;
            Serial.printf("[SPI] STM32 connected! RTT: %d us\n", rtt);
            return true;
        }
        delay(100);
    }
    
    Serial.println("[SPI] WARNING: STM32 not responding!");
    return false;
}

bool SPIMaster::sendCommand(uint8_t cmd, const void* payload, uint16_t payloadLen) {
    SPIPacketHeader header;
    header.magic = SPI_MAGIC_CMD;
    header.cmd = cmd;
    header.length = payloadLen;
    header.sequence = seqNumber++;
    header.checksum = payload ? crc16((const uint8_t*)payload, payloadLen) : 0;
    
    csLow();
    spi->beginTransaction(SPISettings(SPI_CLOCK, MSBFIRST, SPI_MODE0));
    
    // Enviar header
    spi->transferBytes((uint8_t*)&header, nullptr, sizeof(SPIPacketHeader));
    
    // Enviar payload si existe
    if (payload && payloadLen > 0) {
        spi->transferBytes((const uint8_t*)payload, nullptr, payloadLen);
    }
    
    spi->endTransaction();
    csHigh();
    
    // Pulso SYNC para notificar al STM32
    syncPulse();
    
    return true;
}

void SPIMaster::triggerSequencer(uint8_t pad, uint8_t velocity, 
                                  uint8_t trackVolume, uint32_t maxSamples) {
    struct __attribute__((packed)) {
        uint8_t  padIndex;
        uint8_t  velocity;
        uint8_t  trackVolume;
        uint8_t  reserved;
        uint32_t maxSamples;
    } payload = {pad, velocity, trackVolume, 0, maxSamples};
    
    sendCommand(CMD_TRIGGER_SEQ, &payload, sizeof(payload));
}

void SPIMaster::triggerLive(uint8_t pad, uint8_t velocity) {
    struct __attribute__((packed)) {
        uint8_t padIndex;
        uint8_t velocity;
    } payload = {pad, velocity};
    
    sendCommand(CMD_TRIGGER_LIVE, &payload, sizeof(payload));
}

bool SPIMaster::getPeaks(float* trackPeaks, float& masterPeak) {
    struct PeaksResponse {
        float tracks[16];
        float master;
    } resp;
    
    if (sendAndReceive(CMD_GET_PEAKS, nullptr, 0, &resp, sizeof(resp))) {
        memcpy(trackPeaks, resp.tracks, 16 * sizeof(float));
        masterPeak = resp.master;
        return true;
    }
    return false;
}

bool SPIMaster::transferSample(uint8_t padIndex, int16_t* buffer, uint32_t numSamples) {
    uint32_t totalBytes = numSamples * sizeof(int16_t);
    
    // 1. BEGIN
    struct __attribute__((packed)) {
        uint8_t  padIndex;
        uint8_t  bitsPerSample;
        uint16_t sampleRate;
        uint32_t totalBytes;
        uint32_t totalSamples;
    } beginPayload = {padIndex, 16, 44100, totalBytes, numSamples};
    
    sendCommand(CMD_SAMPLE_BEGIN, &beginPayload, sizeof(beginPayload));
    delayMicroseconds(100);  // Dar tiempo al STM32 para allocar
    
    // 2. DATA chunks (512 bytes = 256 muestras por chunk)
    const uint16_t CHUNK_SAMPLES = 256;
    const uint16_t CHUNK_BYTES = CHUNK_SAMPLES * sizeof(int16_t);
    uint32_t offset = 0;
    
    while (offset < totalBytes) {
        uint16_t chunkSize = min((uint32_t)CHUNK_BYTES, totalBytes - offset);
        
        struct __attribute__((packed)) {
            uint8_t  padIndex;
            uint8_t  reserved;
            uint16_t chunkSize;
            uint32_t offset;
        } dataHeader = {padIndex, 0, chunkSize, offset};
        
        // Construir paquete completo: header + data
        uint8_t pkt[8 + 520];
        memcpy(pkt, &dataHeader, 8);
        memcpy(pkt + 8, ((uint8_t*)buffer) + offset, chunkSize);
        
        sendCommand(CMD_SAMPLE_DATA, pkt, 8 + chunkSize);
        
        offset += chunkSize;
        delayMicroseconds(50);  // Throttle para no saturar STM32
    }
    
    // 3. END
    struct __attribute__((packed)) {
        uint8_t  padIndex;
        uint8_t  status;
        uint16_t reserved;
        uint32_t checksum;
    } endPayload = {padIndex, 0, 0, crc16((uint8_t*)buffer, totalBytes)};
    
    sendCommand(CMD_SAMPLE_END, &endPayload, sizeof(endPayload));
    
    Serial.printf("[SPI] Sample %d transferred: %d samples (%d bytes)\n",
                  padIndex, numSamples, totalBytes);
    return true;
}

void SPIMaster::syncPulse() {
    digitalWrite(SPI_SYNC, HIGH);
    delayMicroseconds(2);
    digitalWrite(SPI_SYNC, LOW);
}

uint16_t SPIMaster::crc16(const uint8_t* data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xA001;
            else crc >>= 1;
        }
    }
    return crc;
}
```

---

## 10. Código STM32 — SPI Slave + Audio DSP

### 10.1 Estructura del Proyecto STM32 (STM32CubeIDE / PlatformIO)

```
STM32_AudioSlave/
├── Core/
│   ├── Inc/
│   │   ├── main.h
│   │   ├── spi_slave.h        // Protocolo SPI
│   │   ├── audio_engine.h     // DSP: voces, mixing, I2S
│   │   ├── fx_engine.h        // Filtros, delay, phaser, etc.
│   │   ├── sample_manager.h   // Gestión de samples en SRAM
│   │   └── protocol.h         // Definiciones de comandos
│   └── Src/
│       ├── main.c
│       ├── spi_slave.c
│       ├── audio_engine.c
│       ├── fx_engine.c
│       └── sample_manager.c
├── Drivers/
│   └── CMSIS/DSP/             // ARM CMSIS-DSP library
└── STM32F411.ioc              // CubeMX config
```

### 10.2 main.c del STM32 (esquema)

```c
#include "main.h"
#include "spi_slave.h"
#include "audio_engine.h"
#include "sample_manager.h"

// Doble buffer DMA para I2S (ping-pong)
#define AUDIO_BUFFER_SIZE 256  // 256 muestras estéreo = 512 int16_t
int16_t audioBuf[2][AUDIO_BUFFER_SIZE * 2];  // Ping-pong estéreo
volatile uint8_t activeBuffer = 0;

// SPI RX buffer (DMA)
#define SPI_BUF_SIZE 540
uint8_t spiRxBuf[SPI_BUF_SIZE];
volatile bool spiPacketReady = false;

int main(void) {
    HAL_Init();
    SystemClock_Config();  // 100MHz (F411) / 480MHz (H743)
    
    // Inicializar periféricos
    MX_GPIO_Init();
    MX_DMA_Init();
    MX_SPI1_Init();     // SPI Slave mode
    MX_I2S2_Init();     // I2S Master TX @ 44100Hz 16-bit
    MX_TIM2_Init();     // Timer para timing/debug
    
    // Inicializar módulos
    AudioEngine_Init();
    SampleManager_Init();
    SPI_Slave_Init();
    
    // Arrancar I2S con DMA (ping-pong automático)
    HAL_I2S_Transmit_DMA(&hi2s2, (uint16_t*)audioBuf, AUDIO_BUFFER_SIZE * 4);
    
    // Arrancar SPI DMA recepción
    HAL_SPI_Receive_DMA(&hspi1, spiRxBuf, SPI_BUF_SIZE);
    
    while (1) {
        // Procesar comandos SPI recibidos
        if (spiPacketReady) {
            SPI_ProcessPacket(spiRxBuf);
            spiPacketReady = false;
            // Re-armar DMA para siguiente paquete
            HAL_SPI_Receive_DMA(&hspi1, spiRxBuf, SPI_BUF_SIZE);
        }
        
        // Otras tareas de housekeeping (cada 100ms)
        static uint32_t lastHousekeep = 0;
        if (HAL_GetTick() - lastHousekeep > 100) {
            AudioEngine_UpdatePeaks();
            lastHousekeep = HAL_GetTick();
        }
    }
}

// Callback I2S DMA: primera mitad del buffer transferida
void HAL_I2S_TxHalfCpltCallback(I2S_HandleTypeDef *hi2s) {
    AudioEngine_FillBuffer(audioBuf[0], AUDIO_BUFFER_SIZE);
}

// Callback I2S DMA: segunda mitad del buffer transferida
void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *hi2s) {
    AudioEngine_FillBuffer(audioBuf[1], AUDIO_BUFFER_SIZE);
}

// Callback SPI DMA: paquete recibido
void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi) {
    spiPacketReady = true;
}

// Callback GPIO EXTI: SYNC pin del ESP32
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
    if (GPIO_Pin == SYNC_PIN) {
        // ESP32 indica que hay un comando SPI listo
        // (el DMA ya lo está recibiendo)
    }
}
```

### 10.3 spi_slave.c — Procesador de Comandos

```c
#include "spi_slave.h"
#include "audio_engine.h"
#include "sample_manager.h"
#include "protocol.h"

void SPI_ProcessPacket(uint8_t* packet) {
    SPIPacketHeader* hdr = (SPIPacketHeader*)packet;
    
    // Verificar magic byte
    if (hdr->magic != SPI_MAGIC_CMD && hdr->magic != SPI_MAGIC_SAMPLE) return;
    
    uint8_t* payload = packet + sizeof(SPIPacketHeader);
    
    // Verificar CRC si hay payload
    if (hdr->length > 0) {
        uint16_t calcCrc = CRC16(payload, hdr->length);
        if (calcCrc != hdr->checksum) {
            spiErrorCount++;
            return;
        }
    }
    
    switch (hdr->cmd) {
        // ═══════ TRIGGERS ═══════
        case CMD_TRIGGER_SEQ: {
            TriggerSeqPayload* t = (TriggerSeqPayload*)payload;
            AudioEngine_TriggerSeq(t->padIndex, t->velocity, 
                                    t->trackVolume, t->maxSamples);
            break;
        }
        
        case CMD_TRIGGER_LIVE: {
            TriggerLivePayload* t = (TriggerLivePayload*)payload;
            AudioEngine_TriggerLive(t->padIndex, t->velocity);
            break;
        }
        
        case CMD_TRIGGER_STOP:
            AudioEngine_StopSample(payload[0]);
            break;
            
        case CMD_TRIGGER_STOP_ALL:
            AudioEngine_StopAll();
            break;
        
        case CMD_BULK_TRIGGERS: {
            uint8_t count = payload[0];
            TriggerSeqPayload* triggers = (TriggerSeqPayload*)(payload + 2);
            for (int i = 0; i < count && i < 16; i++) {
                AudioEngine_TriggerSeq(triggers[i].padIndex, triggers[i].velocity,
                                        triggers[i].trackVolume, triggers[i].maxSamples);
            }
            break;
        }
        
        // ═══════ VOLUMEN ═══════
        case CMD_MASTER_VOLUME:
            AudioEngine_SetMasterVolume(payload[0]);
            break;
            
        case CMD_SEQ_VOLUME:
            AudioEngine_SetSeqVolume(payload[0]);
            break;
        
        case CMD_TRACK_VOLUME: {
            uint8_t track = payload[0];
            uint8_t vol = payload[1];
            AudioEngine_SetTrackVolume(track, vol);
            break;
        }
        
        // ═══════ FILTROS GLOBALES ═══════
        case CMD_FILTER_TYPE: {
            GlobalFilterPayload* f = (GlobalFilterPayload*)payload;
            AudioEngine_SetGlobalFilter(f->filterType, f->cutoff, f->resonance,
                                         f->distortion, f->distMode, f->bitDepth);
            break;
        }
        
        // ═══════ MASTER FX ═══════
        case CMD_DELAY_ACTIVE:
            AudioEngine_SetDelayActive(payload[0]);
            break;
        case CMD_DELAY_TIME:
            AudioEngine_SetDelayTime(*(float*)payload);
            break;
        case CMD_DELAY_FEEDBACK:
            AudioEngine_SetDelayFeedback(*(float*)payload);
            break;
        case CMD_DELAY_MIX:
            AudioEngine_SetDelayMix(*(float*)payload);
            break;
            
        // ═══════ PER-TRACK FX ═══════
        case CMD_TRACK_FILTER: {
            TrackFilterPayload* f = (TrackFilterPayload*)payload;
            AudioEngine_SetTrackFilter(f->track, f->filterType, 
                                        f->cutoff, f->resonance, f->gain);
            break;
        }
        
        case CMD_TRACK_ECHO: {
            TrackEchoPayload* e = (TrackEchoPayload*)payload;
            AudioEngine_SetTrackEcho(e->track, e->active, e->time, 
                                      e->feedback, e->mix);
            break;
        }
        
        // ═══════ SIDECHAIN ═══════
        case CMD_SIDECHAIN_SET: {
            SidechainPayload* s = (SidechainPayload*)payload;
            AudioEngine_SetSidechain(s->active, s->sourceTrack, s->destMask,
                                      s->amount, s->attackMs, s->releaseMs, s->knee);
            break;
        }
        
        // ═══════ SAMPLE TRANSFER ═══════
        case CMD_SAMPLE_BEGIN: {
            SampleBeginPayload* sb = (SampleBeginPayload*)payload;
            SampleManager_BeginTransfer(sb->padIndex, sb->totalSamples, 
                                         sb->sampleRate, sb->bitsPerSample);
            break;
        }
        
        case CMD_SAMPLE_DATA: {
            SampleDataPayload* sd = (SampleDataPayload*)payload;
            SampleManager_WriteChunk(sd->padIndex, sd->offset, 
                                      sd->data, sd->chunkSize);
            break;
        }
        
        case CMD_SAMPLE_END: {
            SampleEndPayload* se = (SampleEndPayload*)payload;
            SampleManager_EndTransfer(se->padIndex, se->checksum);
            break;
        }
        
        // ═══════ STATUS ═══════
        case CMD_GET_PEAKS: {
            // Preparar respuesta y enviar via SPI TX
            PeaksResponse resp;
            AudioEngine_GetPeaks(resp.trackPeaks, &resp.masterPeak);
            SPI_SendResponse(CMD_GET_PEAKS, &resp, sizeof(resp));
            break;
        }
        
        case CMD_GET_STATUS: {
            StatusResponse resp;
            resp.activeVoices = AudioEngine_GetActiveVoices();
            resp.cpuLoadPercent = AudioEngine_GetCPULoad();
            resp.freeSRAM = SampleManager_GetFreeSRAM();
            resp.spiErrors = spiErrorCount;
            resp.bufferUnderruns = AudioEngine_GetUnderruns();
            SPI_SendResponse(CMD_GET_STATUS, &resp, sizeof(resp));
            break;
        }
        
        case CMD_PING: {
            uint32_t echoTs = *(uint32_t*)payload;
            struct { uint32_t echo; uint32_t uptime; } pong = {echoTs, HAL_GetTick()};
            SPI_SendResponse(CMD_PING, &pong, sizeof(pong));
            break;
        }
        
        case CMD_RESET:
            AudioEngine_Reset();
            SampleManager_UnloadAll();
            break;
    }
}
```

### 10.4 audio_engine.c — DSP Core (esquema CMSIS-DSP)

```c
#include "audio_engine.h"
#include "arm_math.h"   // CMSIS-DSP

#define MAX_VOICES      10
#define SAMPLE_RATE     44100
#define MAX_TRACKS      16
#define MAX_PADS        24

// ══════ Voices ══════
typedef struct {
    int16_t* buffer;
    uint32_t position;
    uint32_t length;
    uint32_t maxLength;
    uint8_t  active;
    uint8_t  velocity;
    uint8_t  volume;
    float    pitchShift;
    uint8_t  padIndex;
    uint8_t  isLivePad;
    uint8_t  loop;
    uint32_t loopStart, loopEnd;
    float    scratchPos;
    // Per-voice filter state (CMSIS biquad)
    arm_biquad_casd_df1_inst_f32 biquadInst;
    float biquadState[4];
} Voice;

static Voice voices[MAX_VOICES];
static uint8_t masterVolume = 100;
static uint8_t seqVolume = 10;

// ══════ Mix accumulators ══════
static float mixAccL[256];  // Left channel accumulator
static float mixAccR[256];  // Right channel accumulator

// ══════ Master FX ══════
static float* delayBuffer;  // Allocado en init (~32K floats)
static DelayParams delay;
static PhaserParams phaser;
static FlangerParams flanger;
static CompressorParams compressor;

// ══════ Per-track FX ══════
static arm_biquad_casd_df1_inst_f32 trackBiquad[MAX_TRACKS];
static float trackBiquadState[MAX_TRACKS][4];
static float trackBiquadCoeffs[MAX_TRACKS][5];

// ══════ DSP Functions ══════
void AudioEngine_FillBuffer(int16_t* buffer, uint32_t samples) {
    // Limpiar acumuladores
    arm_fill_f32(0.0f, mixAccL, samples);
    arm_fill_f32(0.0f, mixAccR, samples);
    
    // Mezclar voces activas
    for (int v = 0; v < MAX_VOICES; v++) {
        if (!voices[v].active) continue;
        Voice* voice = &voices[v];
        
        for (uint32_t i = 0; i < samples; i++) {
            if (voice->position >= voice->length) {
                if (voice->loop) {
                    voice->position = voice->loopStart;
                } else {
                    voice->active = 0;
                    break;
                }
            }
            
            // Leer sample y escalar
            float sample = (float)voice->buffer[voice->position] / 32768.0f;
            sample *= (float)voice->velocity / 127.0f;
            sample *= (float)voice->volume / 100.0f;
            
            // Acumular (mono → estéreo)
            mixAccL[i] += sample;
            mixAccR[i] += sample;
            
            voice->position++;
        }
    }
    
    // Aplicar volumen master y FX chain
    float volScale = (float)masterVolume / 100.0f;
    arm_scale_f32(mixAccL, volScale, mixAccL, samples);
    arm_scale_f32(mixAccR, volScale, mixAccR, samples);
    
    // Soft clip (Torvalds limit_value pattern)
    for (uint32_t i = 0; i < samples; i++) {
        mixAccL[i] = mixAccL[i] / (1.0f + fabsf(mixAccL[i]));
        mixAccR[i] = mixAccR[i] / (1.0f + fabsf(mixAccR[i]));
    }
    
    // Master FX chain: Phaser → Flanger → Delay → Compressor
    if (phaser.active)     FX_ProcessPhaser(mixAccL, mixAccR, samples);
    if (flanger.active)    FX_ProcessFlanger(mixAccL, mixAccR, samples);
    if (delay.active)      FX_ProcessDelay(mixAccL, mixAccR, samples);
    if (compressor.active) FX_ProcessCompressor(mixAccL, mixAccR, samples);
    
    // Convertir float → int16_t estéreo intercalado
    for (uint32_t i = 0; i < samples; i++) {
        float l = mixAccL[i] * 32767.0f;
        float r = mixAccR[i] * 32767.0f;
        // CMSIS saturación
        buffer[i * 2]     = (int16_t)__SSAT((int32_t)l, 16);
        buffer[i * 2 + 1] = (int16_t)__SSAT((int32_t)r, 16);
    }
}
```

---

## 11. Distribución de Cores ESP32-S3 (Nueva)

### Antes (con AudioEngine local):
```
Core 0: WiFi + WebServer + Sequencer + UDP + MIDI + LED       ← SATURADO
Core 1: AudioEngine.process() loop infinito I2S               ← PESADO
```

### Después (con STM32 SPI Slave):
```
Core 0: WiFi + WebServer + UDP + MIDI + LED                   ← LIGERO
Core 1: Sequencer.update() + SPI Master (triggers/FX/peaks)   ← RÁPIDO
```

### Cambios en main.cpp:

```cpp
// ANTES:
void audioTask(void *pvParameters) {
    while (true) {
        audioEngine.process();  // ← ELIMINADO
    }
}

// DESPUÉS:
void spiAudioTask(void *pvParameters) {
    Serial.println("[Task] SPI Audio Task en Core 1 (Prioridad: 24)");
    uint32_t lastPeakQuery = 0;
    
    while (true) {
        // 1. Actualizar secuenciador (genera triggers → SPI)
        sequencer.update();
        
        // 2. Consultar VU meters cada 50ms
        if (millis() - lastPeakQuery > 50) {
            float peaks[16];
            float masterPeak;
            if (spiMaster.getPeaks(peaks, masterPeak)) {
                webInterface.broadcastPeaks(peaks, masterPeak);
            }
            lastPeakQuery = millis();
        }
        
        // 3. Yield mínimo (secuenciador necesita timing preciso)
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void systemTask(void *pvParameters) {
    Serial.println("[Task] System Task en Core 0 (Prioridad: 5)");
    while (true) {
        webInterface.update();
        webInterface.handleUdp();
        midiController.update();
        
        // LED fade-out...
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// Callback del secuenciador → SPI trigger
void onStepTrigger(int track, uint8_t velocity, uint8_t trackVolume, uint32_t noteLenSamples) {
    spiMaster.triggerSequencer(track, velocity, trackVolume, noteLenSamples);
}
```

---

## 12. Timing y Latencia

### Análisis de latencia SPI

```
SPI @ 40 MHz:
  - 1 byte = 200 ns
  - Header (8 bytes) = 1.6 µs
  - Trigger payload (8 bytes) = 1.6 µs
  - Total trigger SPI: ~3.2 µs + overhead CS/SYNC ≈ 5 µs

Comparación:
  - Trigger SPI:     ~5 µs  (prácticamente instantáneo)
  - Step interval @120BPM: 125,000 µs (125ms)
  - Budget por step:  0.004% del intervalo

Sample transfer (ejemplo: sample de 44100 muestras = 88200 bytes):
  - 88200 / 512 = 173 chunks
  - 173 × 520 bytes = 89,960 bytes via SPI
  - 89,960 / (40,000,000 / 8) = ~18 ms total
  - Es decir: ~18ms para transferir 1 segundo de audio
```

### I2S en STM32 (DMA ping-pong)

```
Buffer size: 256 muestras × 2 canales = 512 int16_t
Latencia: 256 / 44100 = 5.8 ms (doble buffer)
Comparación ESP32: ~2.9ms (buffer más pequeño pero con jitter WiFi)
STM32: 5.8ms consistente, CERO jitter (no hay WiFi)
```

---

## 13. Checklist de Implementación

### Fase 1 — Hardware
- [ ] Obtener STM32F411CE BlackPill (~3€)
- [ ] Obtener DAC I2S (PCM5102A o MAX98357A)
- [ ] Cablear: SPI (4 cables) + SYNC + IRQ + I2S (3 cables al DAC)
- [ ] Alimentación 3.3V compartida o independiente

### Fase 2 — Firmware STM32
- [ ] Crear proyecto STM32CubeIDE (o PlatformIO + stm32)
- [ ] Configurar SPI1 Slave + DMA
- [ ] Configurar I2S2 Master TX + DMA (ping-pong)
- [ ] Portar AudioEngine (voices, mixing, soft clip)
- [ ] Portar filtros biquad (usar CMSIS-DSP `arm_biquad_cascade_df1_f32`)
- [ ] Portar Master FX (delay, phaser, flanger, compressor)
- [ ] Portar Per-track FX (echo, flanger, compressor)
- [ ] Portar Sidechain PRO
- [ ] Implementar SampleManager en SRAM
- [ ] Implementar procesador de comandos SPI
- [ ] Test con tono de prueba (sine wave) antes de samples reales

### Fase 3 — Firmware ESP32-S3
- [ ] Crear SPIMaster.h / SPIMaster.cpp
- [ ] Eliminar AudioEngine.h / AudioEngine.cpp
- [ ] Actualizar main.cpp (nuevo task layout)
- [ ] Modificar SampleManager: después de cargar sample → transferir por SPI
- [ ] Modificar WebInterface: JSON commands → SPIMaster calls
- [ ] Mover sequencer.update() a Core 1 (con SPI task)
- [ ] Test de ping/pong SPI
- [ ] Test de trigger secuenciador
- [ ] Test de VU meters

### Fase 4 — Integración
- [ ] Verificar todos los FX vía WebSocket → SPI → STM32
- [ ] Medir latencia real SPI (con osciloscopio)
- [ ] Stress test: 16 tracks simultáneos a 300 BPM
- [ ] Verificar ausencia de glitches audio
- [ ] Comparar calidad audio vs. versión ESP32-only

---

## 14. Gestión de Errores

### SPI Errors

```c
// En ESP32 (Master):
if (!spiMaster.sendCommand(CMD_TRIGGER_SEQ, &payload, sizeof(payload))) {
    // Reintentar 1 vez
    delayMicroseconds(100);
    spiMaster.sendCommand(CMD_TRIGGER_SEQ, &payload, sizeof(payload));
}

// En STM32 (Slave):
if (calcCrc != hdr->checksum) {
    spiErrorCount++;
    // Solicitar retransmisión (IRQ → ESP32)
    HAL_GPIO_WritePin(IRQ_GPIO_Port, IRQ_Pin, GPIO_PIN_SET);
    return;
}
```

### Watchdog

```c
// STM32: Si no recibe ping en 10 segundos → reset audio (no crash)
static uint32_t lastPingTime = 0;

void SPI_UpdateWatchdog(void) {
    if (HAL_GetTick() - lastPingTime > 10000) {
        AudioEngine_StopAll();  // Silencio seguro
        // Seguir escuchando SPI
    }
}
```

---

## 15. Diagrama de Flujo Completo

```
  USUARIO                ESP32-S3                    STM32
    │                       │                          │
    │  Toca pad web ───────►│                          │
    │                       │  triggerLive(pad,vel)     │
    │                       │  ──── SPI CMD 0x02 ────►│
    │                       │                          │  AudioEngine_TriggerLive()
    │                       │                          │  → Voice allocation
    │                       │                          │  → Start playback
    │                       │                          │  → I2S DMA → DAC → Audio!
    │                       │                          │
    │                       │  [cada 50ms]             │
    │                       │  ──── SPI CMD 0xE1 ────►│
    │                       │  ◄─── peaks response ───│  → VU meter data
    │  ◄── WebSocket JSON ──│                          │
    │  (peaks UI update)    │                          │
    │                       │                          │
    │                       │  [sequencer tick]        │
    │                       │  onStepTrigger()         │
    │                       │  ──── SPI CMD 0x01 ────►│
    │                       │  (pad, vel, vol, len)    │  AudioEngine_TriggerSeq()
    │                       │                          │  → Voice with note length
    │                       │                          │  → Per-track FX processing
    │                       │                          │
    │  Cambia FX web ──────►│                          │
    │  {"type":"delay"...}  │                          │
    │                       │  setDelay(active,t,fb,m) │
    │                       │  ──── SPI CMD 0x30-33 ──►│
    │                       │                          │  AudioEngine_SetDelay()
    │                       │                          │  → Master FX active
```

---

## 16. Presupuesto de Hardware

| Componente | Precio | Notas |
|------------|--------|-------|
| STM32F411CE BlackPill | ~3€ | WeAct Studio, USB-C |
| PCM5102A I2S DAC | ~3€ | Ya lo tienes para el ESP32, reusar o duplicar |
| Cables Dupont (8 pcs) | ~1€ | SPI (4) + SYNC + IRQ + I2S (shared o nuevo) |
| **Total** | **~7€** | Si ya tienes el DAC: ~3€ |

---

## Resumen Ejecutivo

**Lo que ganas:**
1. **Audio sin jitter** — STM32 dedicado 100% a DSP, sin WiFi que interrumpa
2. **Más potencia DSP** — Cortex-M4F con FPU + CMSIS-DSP optimizado
3. **ESP32-S3 liberado** — Ambos cores para WiFi/Web/MIDI/Sequencer sin estrés
4. **Escalabilidad** — Fácil añadir más FX o voces en el STM32
5. **Latencia predecible** — <1ms determinista vs ~3ms con jitter

**Lo que pierdes:**
1. Complejidad — 2 firmwares en vez de 1
2. Hardware extra — ~3-7€ más
3. Debug — Necesitas depurar protocolo SPI entre dos MCUs

**Veredicto**: Para una drum machine seria, la separación ESP32(control) + STM32(audio) es la arquitectura profesional estándar. Los 3€ extra valen la pena.
