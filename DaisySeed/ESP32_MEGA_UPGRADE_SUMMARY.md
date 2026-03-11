# RED808 — Mega Upgrade: Resumen de Integración para ESP32-S3

**Fecha:** Junio 2025  
**Protocolo:** SPI1 Slave (Mode 0, 2 MHz), paquete 8-byte header + payload, CRC16 MODBUS  
**Compatibilidad:** 100% backward-compatible con comandos existentes

---

## Índice
1. [Nuevos Motores de Síntesis](#1-nuevos-motores-de-síntesis)
2. [Nuevos Tipos de Filtro Master](#2-nuevos-tipos-de-filtro-master)
3. [Nuevos Efectos Master](#3-nuevos-efectos-master)
4. [Choke Groups](#4-choke-groups)
5. [Song Mode](#5-song-mode)
6. [LFO Expandido por Track](#6-lfo-expandido-por-track)
7. [Cambios en Comandos Existentes](#7-cambios-en-comandos-existentes)
8. [Tabla de Nuevos Comandos SPI](#8-tabla-de-nuevos-comandos-spi)
9. [Detalle de Payloads](#9-detalle-de-payloads)
10. [Enumeraciones de Referencia](#10-enumeraciones-de-referencia)

---

## 1. Nuevos Motores de Síntesis

Se añaden 2 nuevos engines (total: 9):

| ID | Nombre | Descripción | Librería DaisySP |
|----|--------|-------------|-----------------|
| 7  | `SYNTH_ENGINE_PHYS` | Modelado físico: ModalVoice (metales/membranas) + StringVoice (cuerdas/resonadores) | `ModalVoice`, `StringVoice` |
| 8  | `SYNTH_ENGINE_NOISE` | Texturas/ruido: generador de partículas percusivas | `Particle` |

### 1.1 Trigger

**CMD_SYNTH_TRIGGER (0xC0):** `[engine(1), instrument(1), velocity(1)]`

- **PHYS (engine=7):**
  - `instrument 0` → ModalVoice (SetFreq 100Hz base, Trig)
  - `instrument 1` → StringVoice (SetFreq 100Hz base, Trig)
  - velocity → gain (vel/127.0)

- **NOISE (engine=8):**
  - Cualquier instrument → Particle (SetFreq 300Hz, Trig)
  - velocity → gain (vel/127.0)

### 1.2 Parámetros

**CMD_SYNTH_PARAM (0xC1):** `[engine(1), instrument(1), paramId(1), value_float(4)]`

**PHYS (engine=7) — 10 parámetros:**

| paramId | Modal (inst 0/2/4/6/8) | String (inst 1/3/5/7/9) | Rango |
|---------|------------------------|------------------------|-------|
| 0 | SetFreq | SetFreq | 20-5000 Hz |
| 1 | SetStructure | SetStructure | 0.0-1.0 |
| 2 | SetBrightness | SetBrightness | 0.0-1.0 |
| 3 | SetDamping | SetDamping | 0.0-1.0 |
| 4 | gain | gain | 0.0-2.0 |
| 5 | SetFreq | SetFreq | (duplicado par→modal, impar→string) |
| 6 | SetStructure | SetStructure | |
| 7 | SetBrightness | SetBrightness | |
| 8 | SetDamping | SetDamping | |
| 9 | gain | gain | |

Nota: paramId 0-4 con `instrument par` → ModalVoice, `instrument impar` → StringVoice.

**NOISE (engine=8) — 7 parámetros:**

| paramId | Parámetro | Rango |
|---------|-----------|-------|
| 0 | SetFreq | 20-10000 Hz |
| 1 | SetRes (resonancia) | 0.0-1.0 |
| 2 | SetRandomFreq | 0.0-1.0 |
| 3 | SetDensity | 0.0-1.0 |
| 4 | gain | 0.0-2.0 |
| 5 | SetSpread | 0.0-1.0 |
| 6 | noisePartGain (global) | 0.0-2.0 |

### 1.3 Activación

**CMD_SYNTH_ACTIVE (0xC5):** Ahora acepta 1 o 2 bytes.

- **1 byte (compatible):** `[mask(1)]` — bits 0-6 (engines 0-6)
- **2 bytes (nuevo):** `[maskLo(1), maskHi(1)]` → uint16_t, bits 0-8 (engines 0-8)
  - Bit 7 → SYNTH_ENGINE_PHYS
  - Bit 8 → SYNTH_ENGINE_NOISE

Máscara completa activando todos: `0xFF, 0x01` (= 0x01FF)

---

## 2. Nuevos Tipos de Filtro Master

**CMD_FILTER_SET (0x20):** `[filterType(1)]` — se amplían los tipos:

| ID | Tipo | Descripción |
|----|------|-------------|
| 0 | NONE | Sin filtro |
| 1-9 | (existentes) | Lowpass, Highpass, Bandpass, Notch, Allpass, Peaking, LowShelf, HighShelf, Resonant |
| **10** | **FTYPE_LADDER** | Moog Ladder 24dB/oct (DaisySP `Ladder`) |
| **11** | **FTYPE_SVF_LP** | State Variable Filter LP con drive |
| **12** | **FTYPE_SVF_HP** | State Variable Filter HP |
| **13** | **FTYPE_SVF_BP** | State Variable Filter BP |
| **14** | **FTYPE_COMB** | Comb filter resonador (delay feedback) |

Los comandos existentes `CMD_FILTER_CUTOFF (0x21)` y `CMD_FILTER_RESONANCE (0x22)` funcionan automáticamente con los nuevos tipos. La Daisy ruta internamente a Ladder/SVF/Comb según `gFilterType`.

---

## 3. Nuevos Efectos Master

### 3.1 Auto-Wah (0xA5-0xA7)

Envelope follower que modula un filtro bandpass dinámicamente.

| CMD | Hex | Payload | Descripción |
|-----|-----|---------|-------------|
| CMD_AUTOWAH_ACTIVE | 0xA5 | `[active(1)]` | 0=off, 1=on |
| CMD_AUTOWAH_LEVEL | 0xA6 | `[level(1)]` | Sensibilidad 0-127, mapeado a 0.0-1.0 |
| CMD_AUTOWAH_MIX | 0xA7 | `[mix(1)]` | Wet/dry 0-100, mapeado a 0.0-1.0 |

Para activar en el grafo: `CMD_MASTER_FX_ROUTE (0x27)` con `fxId=10` (MASTER_FX_ROUTE_AUTOWAH), `connected=1`.

### 3.2 Stereo Width (0xA8)

Procesamiento mid-side para controlar amplitud estéreo.

| CMD | Hex | Payload | Descripción |
|-----|-----|---------|-------------|
| CMD_STEREO_WIDTH | 0xA8 | `[width(1)]` | 0-200: 0=mono, 100=normal, 200=extra-wide |

Mapeado interno: `stereoWidth = width_byte / 100.0f` (0.0 a 2.0).

### 3.3 Tape Stop (0xA9)

Efecto de parada/arranque de cinta (ralentización/aceleración progresiva).

| CMD | Hex | Payload | Descripción |
|-----|-----|---------|-------------|
| CMD_TAPE_STOP | 0xA9 | `[mode(1)]` | 0=off, 1=stop (rampa descendente), 2=start (rampa ascendente) |

- **mode 1:** El pitch desciende desde 1.0 a 0.0 en ~0.4 segundos
- **mode 2:** El pitch sube desde el ratio actual a 1.0 en ~0.6 segundos
- Automático: se desactiva al llegar al extremo

### 3.4 Beat Repeat (0xAA)

Captura y repite un slice del audio sincronizado al tempo.

| CMD | Hex | Payload | Descripción |
|-----|-----|---------|-------------|
| CMD_BEAT_REPEAT | 0xAA | `[division(1)]` | 0=off, 1=1/1, 2=1/2, 4=1/4, 8=1/8, 16=1/16, 32=1/32 |

Cálculo interno: `sliceLen = (60/BPM × sampleRate × 4) / division` samples.  
Buffer circular de 96000 samples estéreo en SDRAM.

### 3.5 Delay Stereo/Ping-Pong (0xAB)

| CMD | Hex | Payload | Descripción |
|-----|-----|---------|-------------|
| CMD_DELAY_STEREO | 0xAB | `[mode(1)]` | 0=mono (default), 1=ping-pong |

En modo ping-pong, L y R se cruzan con el feedback creando rebote estéreo.

### 3.6 Chorus Stereo (0xAC)

| CMD | Hex | Payload | Descripción |
|-----|-----|---------|-------------|
| CMD_CHORUS_STEREO | 0xAC | `[mode(1)]` | 0=mono, 1=stereo (L invertido) |

En modo stereo, el canal izquierdo se invierte para crear imagen stereo amplia.

### 3.7 Early Reflections (0xAD-0xAE)

Simulación de primeras reflexiones de sala (6 taps asimétricos L/R).

| CMD | Hex | Payload | Descripción |
|-----|-----|---------|-------------|
| CMD_EARLY_REF_ACTIVE | 0xAD | `[active(1)]` | 0=off, 1=on |
| CMD_EARLY_REF_MIX | 0xAE | `[mix(1)]` | 0-100, mapeado a 0.0-1.0 |

Para activar en el grafo: `CMD_MASTER_FX_ROUTE (0x27)` con `fxId=11` (MASTER_FX_ROUTE_EARLY_REF), `connected=1`.

Tiempos de tap (fijos, optimizados para percusión):
- L: 7, 13, 19, 29, 41, 53 ms
- R: 11, 17, 23, 37, 47, 59 ms

---

## 4. Choke Groups

Permite que un pad silencie automáticamente a otros pads del mismo grupo (ej: Open HH silencia Closed HH).

| CMD | Hex | Payload | Descripción |
|-----|-----|---------|-------------|
| CMD_CHOKE_GROUP | 0xAF | `[pad(1), group(1)]` | pad=0-15, group=0 (none) o 1-8 |

**Comportamiento:** Al disparar un pad con grupo X, todos los demás pads del mismo grupo X se silencian inmediatamente (todas sus voces activas se desactivan).

---

## 5. Song Mode

Cadena de patterns con repeticiones para reproducción secuencial automática.

### 5.1 Upload (0xF2)

| CMD | Hex | Payload | Descripción |
|-----|-----|---------|-------------|
| CMD_SONG_UPLOAD | 0xF2 | `[count(1), {pattern(1), repeats(1)} × count]` | Max 32 entries |

- `count`: Número de entradas (1-32)
- `pattern`: ID del pattern (0-7, se aplica & 7)
- `repeats`: Número de repeticiones (1-255, 0 se interpreta como 1)

### 5.2 Control (0xF3)

| CMD | Hex | Payload | Descripción |
|-----|-----|---------|-------------|
| CMD_SONG_CONTROL | 0xF3 | `[action(1)]` | 0=stop, 1=play, 2=reset position |

- **action=1 (play):** Inicia song mode, carga el primer pattern del chain, setea `currentStep = -1`
- **action=0 (stop):** Detiene song mode (el sequencer sigue en el pattern actual)
- **action=2 (reset):** Resetea posición a idx=0, repeat=0

### 5.3 Get Position (0xF4)

| CMD | Hex | Payload (TX) | Descripción |
|-----|-----|--------------|-------------|
| CMD_SONG_GET_POS | 0xF4 | → `[songIdx(1), pattern(1), repeatCnt(1), 0x00(1)]` | Respuesta 4 bytes |

**Avance automático:** Cuando termina un ciclo del pattern (step vuelve a 0), si song mode está activo:
1. Incrementa `repeatCnt`
2. Si `repeatCnt >= repeats` de la entrada actual → avanza al siguiente entry
3. Si se supera la última entrada → song mode se detiene automáticamente

---

## 6. LFO Expandido por Track

### 6.1 Configuración (0x67)

| CMD | Hex | Payload | Descripción |
|-----|-----|---------|-------------|
| CMD_TRACK_LFO_CONFIG | 0x67 | `[track(1), wave(1), target(1), rateHi(1), rateLo(1), depthHi(1), depthLo(1)]` | 7 bytes |

- `track`: 0-15 (pad/track)
- `wave`: 0=Sine, 1=Triangle, 2=Sample&Hold
- `target`: Ver tabla de targets abajo
- `rate`: uint16_t (Hi << 8 | Lo), valor en centésimas de Hz → `rate_float = rate16 / 100.0f`
- `depth`: uint16_t (Hi << 8 | Lo), valor en milésimas → `depth_float = depth16 / 1000.0f`

### 6.2 Targets LFO

| ID | Target | Efecto |
|----|--------|--------|
| 0 | LFO_TGT_GAIN | Modula volumen del track (±80%) |
| 1 | LFO_TGT_PAN | Modula paneo del track (±90%) |
| 2 | LFO_TGT_FILTER | Modula cutoff del filtro per-track (±90%) |
| **3** | **LFO_TGT_PITCH** | Modula velocidad de reproducción (±50%, rango 0.25x-4x) |
| **4** | **LFO_TGT_ECHO_TIME** | Modula tiempo de echo per-track (±40%) |
| **5** | **LFO_TGT_DIST_DRIVE** | Modula drive de distorsión per-track (±80%) |
| **6** | **LFO_TGT_CRUSH** | Modula bit-depth del bitcrusher per-track (±6 bits) |
| **7** | **LFO_TGT_SEND_REV** | Modula nivel de send a reverb (±50%) |
| **8** | **LFO_TGT_SEND_DEL** | Modula nivel de send a delay (±50%) |

---

## 7. Cambios en Comandos Existentes

### 7.1 CMD_SYNTH_ACTIVE (0xC5) — Expansión a 2 bytes

**Antes:** `[mask(1)]` — solo bits 0-6  
**Ahora:** `[maskLo(1)]` o `[maskLo(1), maskHi(1)]`  
- Si `len >= 2`: maskHi contiene bits 8+ (bit 0 de maskHi = engine 8)
- Si `len == 1`: compatible con firmware anterior

### 7.2 CMD_FILTER_SET (0x20) — Nuevos tipos

Ahora acepta tipos 10-14 (Ladder, SVF LP/HP/BP, Comb). El payload sigue siendo `[type(1)]`.

### 7.3 CMD_RESET (0xEF) — Reinicio completo

Ahora resetea también: autowah, stereo width, tape stop, beat repeat, delay mode, chorus mode, early reflections, choke groups, song mode, physModal, physString, noisePart. La máscara de synth se resetea a `0x01FF` (9 engines).

### 7.4 CMD_MASTER_FX_ROUTE (0x27) — Nuevos fxId

| fxId | Efecto |
|------|--------|
| 0-9 | (existentes sin cambio) |
| **10** | MASTER_FX_ROUTE_AUTOWAH |
| **11** | MASTER_FX_ROUTE_EARLY_REF |

---

## 8. Tabla de Nuevos Comandos SPI

| Hex | Nombre | Payload (bytes) | Dirección |
|-----|--------|-----------------|-----------|
| 0x67 | CMD_TRACK_LFO_CONFIG | 7 | ESP32 → Daisy |
| 0xA5 | CMD_AUTOWAH_ACTIVE | 1 | ESP32 → Daisy |
| 0xA6 | CMD_AUTOWAH_LEVEL | 1 | ESP32 → Daisy |
| 0xA7 | CMD_AUTOWAH_MIX | 1 | ESP32 → Daisy |
| 0xA8 | CMD_STEREO_WIDTH | 1 | ESP32 → Daisy |
| 0xA9 | CMD_TAPE_STOP | 1 | ESP32 → Daisy |
| 0xAA | CMD_BEAT_REPEAT | 1 | ESP32 → Daisy |
| 0xAB | CMD_DELAY_STEREO | 1 | ESP32 → Daisy |
| 0xAC | CMD_CHORUS_STEREO | 1 | ESP32 → Daisy |
| 0xAD | CMD_EARLY_REF_ACTIVE | 1 | ESP32 → Daisy |
| 0xAE | CMD_EARLY_REF_MIX | 1 | ESP32 → Daisy |
| 0xAF | CMD_CHOKE_GROUP | 2 | ESP32 → Daisy |
| 0xF2 | CMD_SONG_UPLOAD | 1 + 2×count | ESP32 → Daisy |
| 0xF3 | CMD_SONG_CONTROL | 1 | ESP32 → Daisy |
| 0xF4 | CMD_SONG_GET_POS | 0 → resp 4 | Daisy → ESP32 |

---

## 9. Detalle de Payloads

### CMD_AUTOWAH_ACTIVE (0xA5)
```
Byte 0: active (uint8_t) — 0=off, 1=on
```

### CMD_AUTOWAH_LEVEL (0xA6)
```
Byte 0: level (uint8_t) — 0-127 → sensibilidad 0.0-1.0
```

### CMD_AUTOWAH_MIX (0xA7)
```
Byte 0: mix (uint8_t) — 0-100 → wet/dry 0.0-1.0
```

### CMD_STEREO_WIDTH (0xA8)
```
Byte 0: width (uint8_t) — 0-200
  0   = mono completo (solo mid)
  100 = normal (sin cambio)
  200 = extra-wide (side amplificado ×2)
```

### CMD_TAPE_STOP (0xA9)
```
Byte 0: mode (uint8_t)
  0 = off (desactivar)
  1 = stop (rampa de desaceleración ~0.4s)
  2 = start (rampa de aceleración ~0.6s)
```

### CMD_BEAT_REPEAT (0xAA)
```
Byte 0: division (uint8_t)
  0  = off
  1  = 1/1 nota (4 beats completos)
  2  = 1/2
  4  = 1/4
  8  = 1/8
  16 = 1/16
  32 = 1/32
```

### CMD_DELAY_STEREO (0xAB)
```
Byte 0: mode (uint8_t) — 0=mono, 1=ping-pong
```

### CMD_CHORUS_STEREO (0xAC)
```
Byte 0: mode (uint8_t) — 0=mono, 1=stereo
```

### CMD_EARLY_REF_ACTIVE (0xAD)
```
Byte 0: active (uint8_t) — 0=off, 1=on
```

### CMD_EARLY_REF_MIX (0xAE)
```
Byte 0: mix (uint8_t) — 0-100 → 0.0-1.0
```

### CMD_CHOKE_GROUP (0xAF)
```
Byte 0: pad (uint8_t) — 0-15
Byte 1: group (uint8_t) — 0=none, 1-8=grupo
```

### CMD_SONG_UPLOAD (0xF2)
```
Byte 0: count (uint8_t) — 1-32
Bytes 1+: {pattern(1), repeats(1)} × count
  pattern: 0-7
  repeats: 1-255 (0 se interpreta como 1)
```

### CMD_SONG_CONTROL (0xF3)
```
Byte 0: action (uint8_t)
  0 = stop
  1 = play (carga chain e inicia)
  2 = reset (vuelve al inicio del chain)
```

### CMD_SONG_GET_POS (0xF4)
```
Respuesta (4 bytes):
  Byte 0: songIdx (uint8_t) — posición en chain
  Byte 1: currentPattern (uint8_t) — pattern actual
  Byte 2: songRepeatCnt (uint8_t) — repetición actual
  Byte 3: 0x00 (reservado)
```

### CMD_TRACK_LFO_CONFIG (0x67)
```
Byte 0: track (uint8_t) — 0-15
Byte 1: wave (uint8_t) — 0=sine, 1=triangle, 2=sample&hold
Byte 2: target (uint8_t) — 0-8 (ver tabla targets)
Byte 3: rateHi (uint8_t) — rate >> 8
Byte 4: rateLo (uint8_t) — rate & 0xFF
Byte 5: depthHi (uint8_t) — depth >> 8
Byte 6: depthLo (uint8_t) — depth & 0xFF

rate16 = (rateHi << 8) | rateLo → rate_Hz = rate16 / 100.0
depth16 = (depthHi << 8) | depthLo → depth = depth16 / 1000.0
```

### CMD_SYNTH_TRIGGER (0xC0) — engines nuevos
```
Engine 7 (PHYS):
  Byte 0: 7
  Byte 1: instrument — 0=ModalVoice, 1=StringVoice
  Byte 2: velocity — 0-127

Engine 8 (NOISE):
  Byte 0: 8
  Byte 1: instrument (ignorado, siempre Particle)
  Byte 2: velocity — 0-127
```

### CMD_SYNTH_PARAM (0xC1) — engines nuevos  
```
Engine 7 (PHYS):
  Byte 0: 7
  Byte 1: instrument — par=ModalVoice, impar=StringVoice
  Byte 2: paramId — 0-9 (ver tabla §1.2)
  Bytes 3-6: value (float32 little-endian)

Engine 8 (NOISE):
  Byte 0: 8
  Byte 1: instrument (ignorado)
  Byte 2: paramId — 0-6 (ver tabla §1.2)
  Bytes 3-6: value (float32 little-endian)
```

### CMD_SYNTH_ACTIVE (0xC5) — expandido
```
Formato 1 byte (compatible):
  Byte 0: maskLo (uint8_t) — bits 0-6

Formato 2 bytes (nuevo):
  Byte 0: maskLo (uint8_t) — bits 0-7
  Byte 1: maskHi (uint8_t) — bit 0 = engine 8 (NOISE)

Ejemplo activar todo: [0xFF, 0x01] = 0x01FF
```

---

## 10. Enumeraciones de Referencia

### Synth Engine IDs
```c
#define SYNTH_ENGINE_808   0
#define SYNTH_ENGINE_909   1
#define SYNTH_ENGINE_505   2
#define SYNTH_ENGINE_303   3
#define SYNTH_ENGINE_WTOSC 4
#define SYNTH_ENGINE_SH101 5
#define SYNTH_ENGINE_FM2OP 6
#define SYNTH_ENGINE_PHYS  7   // NUEVO
#define SYNTH_ENGINE_NOISE 8   // NUEVO
#define SYNTH_ENGINE_COUNT 9
```

### Master Filter Types
```c
#define FTYPE_NONE       0
#define FTYPE_LOWPASS    1
#define FTYPE_HIGHPASS   2
#define FTYPE_BANDPASS   3
#define FTYPE_NOTCH      4
#define FTYPE_ALLPASS    5
#define FTYPE_PEAKING    6
#define FTYPE_LOWSHELF   7
#define FTYPE_HIGHSHELF  8
#define FTYPE_RESONANT   9    // 4-pole LP (2 biquads cascaded)
#define FTYPE_LADDER    10    // NUEVO — Moog Ladder 24dB/oct
#define FTYPE_SVF_LP    11    // NUEVO — State Variable LP
#define FTYPE_SVF_HP    12    // NUEVO — State Variable HP
#define FTYPE_SVF_BP    13    // NUEVO — State Variable BP
#define FTYPE_COMB      14    // NUEVO — Comb resonator
```

### Master FX Route IDs
```c
enum MasterFxRouteId : uint8_t {
    MASTER_FX_ROUTE_FILTER    = 0,
    MASTER_FX_ROUTE_DELAY     = 1,
    MASTER_FX_ROUTE_PHASER    = 2,
    MASTER_FX_ROUTE_FLANGER   = 3,
    MASTER_FX_ROUTE_COMP      = 4,
    MASTER_FX_ROUTE_REVERB    = 5,
    MASTER_FX_ROUTE_CHORUS    = 6,
    MASTER_FX_ROUTE_TREMOLO   = 7,
    MASTER_FX_ROUTE_WAVEFOLDER= 8,
    MASTER_FX_ROUTE_LIMITER   = 9,
    MASTER_FX_ROUTE_AUTOWAH   = 10,  // NUEVO
    MASTER_FX_ROUTE_EARLY_REF = 11,  // NUEVO
};
```

### LFO Waves
```c
LFO_WAVE_SIN = 0
LFO_WAVE_TRI = 1
LFO_WAVE_SH  = 2   // Sample & Hold
```

### LFO Targets (expandidos)
```c
enum TrackLfoTargetEx : uint8_t {
    LFO_TGT_GAIN      = 0,   // existing
    LFO_TGT_PAN       = 1,   // existing
    LFO_TGT_FILTER    = 2,   // existing
    LFO_TGT_PITCH     = 3,   // NUEVO
    LFO_TGT_ECHO_TIME = 4,   // NUEVO
    LFO_TGT_DIST_DRIVE= 5,   // NUEVO
    LFO_TGT_CRUSH     = 6,   // NUEVO
    LFO_TGT_SEND_REV  = 7,   // NUEVO
    LFO_TGT_SEND_DEL  = 8,   // NUEVO
};
```

---

## Resumen Rápido de Integración

### Mínimo para probar los nuevos FX:
1. Enviar `CMD_MASTER_FX_ROUTE` con `fxId=10, connected=1` para activar autowah
2. Enviar `CMD_AUTOWAH_ACTIVE` con `1`
3. Enviar `CMD_AUTOWAH_LEVEL` con `80`
4. Enviar `CMD_AUTOWAH_MIX` con `50`

### Para activar Physical Modeling synth:
1. `CMD_SYNTH_ACTIVE` → `[0xFF, 0x01]` (activa todos los 9 engines)
2. `CMD_SYNTH_TRIGGER` → `[7, 0, 100]` (ModalVoice, velocity 100)
3. `CMD_SYNTH_PARAM` → `[7, 0, 0, <float 440.0>]` (frecuencia 440Hz)

### Para configurar Song Mode:
1. `CMD_SONG_UPLOAD` → `[3, 0,4, 1,2, 2,4]` (3 entries: pattern0×4, pattern1×2, pattern2×4)
2. `CMD_SONG_CONTROL` → `[1]` (play)
3. Monitorizar con `CMD_SONG_GET_POS` periódicamente

### Para configurar Choke Groups (HH típico):
1. `CMD_CHOKE_GROUP` → `[6, 1]` (pad 6 = Closed HH → grupo 1)
2. `CMD_CHOKE_GROUP` → `[10, 1]` (pad 10 = Open HH → grupo 1)
3. Al disparar pad 6, pad 10 se silencia automáticamente y viceversa
