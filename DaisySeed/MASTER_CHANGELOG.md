# 📋 CAMBIOS SLAVE (Daisy Seed) — Notificación al equipo Master

**Fecha:** 25 febrero 2026  
**Firmware:** DaisySeed/main.cpp  
**Afecta a:** Protocolo RED808, comandos SD, CMD_GET_STATUS

---

## 1. Rutas SD cambiadas

La estructura de la SD card ha cambiado:

| Antes | Ahora |
|-------|-------|
| `/RED808/` | **`/data/`** |
| `/RED808/<kitName>/` | **`/data/<kitName>/`** |
| (no existía) | **`/data/BD/`, `/data/SD/`, ... (familias)** |
| (no existía) | **`/data/xtra/` (XTRA PADS)** |

### Estructura de carpetas en la SD

```
/data/
  ├── RED 808 KARZ/          ← Kit por defecto (LIVE PADS 0-15)
  │   ├── 808 BD 3-1.wav       Mapeo automático por nombre: BD→pad0, SD→1, etc.
  │   ├── 808 SD 1-5.wav
  │   ├── 808 HH.wav
  │   └── ... (16 wavs)
  │
  ├── BD/                     ← Familias de instrumentos (por pad)
  │   ├── BD0000.WAV            25 variantes de bass drum
  │   └── BD7575.WAV
  ├── SD/                     ← 25 variantes snare
  ├── CH/  OH/  CY/  CP/     ← Más familias
  ├── RS/  CB/  LT/  MT/
  ├── HT/  MA/  CL/  HC/
  ├── MC/  LC/
  │
  └── xtra/                   ← XTRA PADS (pads 16-23)
      ├── Alesis-Fusion-Bass-C3.wav
      ├── dre-yeah.wav
      └── ...
```

---

## 2. `CMD_GET_STATUS` (0xE0) — RESPUESTA AMPLIADA

**Antes: 20 bytes → Ahora: 54 bytes**

El Master DEBE actualizar el parser de la respuesta.

| Byte(s) | Contenido | Cambio |
|---------|-----------|--------|
| 0 | Voces activas | (sin cambio) |
| 1 | CPU % | (sin cambio) |
| 2-3 | Bitmask pads 0-15 cargados | (sin cambio) |
| 4-7 | Uptime ms | (sin cambio) |
| **8** | **SD present (0/1)** | 🆕 |
| **9** | **Bitmask pads 16-23 (XTRA)** | 🆕 |
| **10** | **evtCount — eventos pendientes** | 🆕 |
| 11-13 | Reservado | 🆕 |
| **14-45** | **currentKitName (32 chars, null-term)** | 🆕 |
| **46** | **Total pads loaded** | 🆕 |
| **47-50** | **Total bytes usados en SDRAM (uint32)** | 🆕 |
| **51** | **MAX_PADS (=24)** | 🆕 |
| 52-53 | Reservado | 🆕 |

### Acción requerida Master

```c
// Antes:
uint8_t statusResp[20];
// Ahora:
uint8_t statusResp[54];

// Nuevos campos disponibles:
bool sdPresent       = statusResp[8];
uint8_t xtraMask     = statusResp[9];
uint8_t pendingEvents = statusResp[10];
char kitName[32];
memcpy(kitName, &statusResp[14], 32);
uint8_t totalLoaded  = statusResp[46];
uint32_t totalBytes;
memcpy(&totalBytes, &statusResp[47], 4);
uint8_t maxPads      = statusResp[51];
```

---

## 3. NUEVO COMANDO: `CMD_GET_EVENTS` (0xE4)

**¿Cuándo llamarlo?** Cuando `statusResp[10] > 0` (evtCount en GET_STATUS).

### Request
Header estándar RED808, sin payload.

### Response
```
[count (1 byte)] + [NotifyEvent (32 bytes)] × count
```

Máximo **4 eventos por llamada**. Si hay más, llamar de nuevo hasta `count == 0`.

### Estructura `NotifyEvent` (32 bytes)

| Offset | Tipo | Campo | Descripción |
|--------|------|-------|-------------|
| 0 | uint8 | type | Tipo de evento (ver tabla) |
| 1 | uint8 | padCount | Pads afectados |
| 2 | uint8 | padMaskLo | Bitmask pads 0-7 |
| 3 | uint8 | padMaskHi | Bitmask pads 8-15 |
| 4 | uint8 | padMaskXtra | Bitmask pads 16-23 |
| 5-7 | — | reserved | — |
| 8-31 | char[24] | name | Kit/sample name (null-term) |

### Tipos de evento

| Valor | Nombre | Cuándo se emite |
|-------|--------|-----------------|
| 0x01 | `EVT_SD_BOOT_DONE` | Boot: LIVE PADS cargados desde SD |
| 0x02 | `EVT_SD_KIT_LOADED` | Un kit cargado por CMD_SD_LOAD_KIT |
| 0x03 | `EVT_SD_SAMPLE_LOADED` | Sample individual cargado por CMD_SD_LOAD_SAMPLE |
| 0x04 | `EVT_SD_KIT_UNLOADED` | Kit descargado por CMD_SD_UNLOAD_KIT |
| 0x05 | `EVT_SD_ERROR` | Error al cargar un sample |
| 0x06 | `EVT_SD_XTRA_LOADED` | Boot: XTRA PADS cargados desde /data/xtra |

### Ejemplo de uso Master

```c
void checkDaisyEvents() {
    // 1. Status polling normal
    sendCommand(CMD_GET_STATUS);
    uint8_t resp[54];
    readResponse(resp, 54);
    
    uint8_t evtCount = resp[10];
    
    // 2. Si hay eventos, drenarlos
    while(evtCount > 0) {
        sendCommand(CMD_GET_EVENTS);
        uint8_t evtResp[1 + 4*32];
        readResponse(evtResp, sizeof(evtResp));
        
        uint8_t n = evtResp[0];
        for(int i = 0; i < n; i++) {
            NotifyEvent* e = (NotifyEvent*)&evtResp[1 + i*32];
            switch(e->type) {
                case 0x01: // EVT_SD_BOOT_DONE
                    printf("Boot OK: %d pads, kit='%s'\n", 
                           e->padCount, e->name);
                    updateUI_LivePads(e->padMaskLo, e->padMaskHi);
                    break;
                case 0x06: // EVT_SD_XTRA_LOADED
                    printf("XTRA: %d pads loaded\n", e->padCount);
                    updateUI_XtraPads(e->padMaskXtra);
                    break;
                case 0x02: // EVT_SD_KIT_LOADED
                    printf("Kit loaded: '%s'\n", e->name);
                    refreshPadDisplay();
                    break;
                case 0x03: // EVT_SD_SAMPLE_LOADED
                    printf("Sample loaded: '%s'\n", e->name);
                    break;
                case 0x05: // EVT_SD_ERROR
                    printf("SD ERROR loading '%s'\n", e->name);
                    showError();
                    break;
            }
        }
        evtCount -= n;
    }
}
```

---

## 4. COMANDOS SD — AHORA TOTALMENTE IMPLEMENTADOS

### CMD_SD_LIST_FOLDERS (0xB0)
Lista todas las subcarpetas de `/data/`.

| | Detalle |
|--|---------|
| **Payload Master** | (vacío) |
| **Respuesta** | `[count(1)] + [name(32)] × count` (máx 16) |

### CMD_SD_LIST_FILES (0xB1)
Lista archivos `.wav` dentro de una carpeta.

| | Detalle |
|--|---------|
| **Payload Master** | `folder[32]` — ej. `"BD"`, `"xtra"`, `"RED 808 KARZ"` |
| **Respuesta** | `[count(1)] + [filename(32)] × count` (máx 20) |

### CMD_SD_FILE_INFO (0xB2)
Info detallada de un archivo WAV.

| | Detalle |
|--|---------|
| **Payload Master** | `folder[32] + filename[32]` |
| **Respuesta** | 16 bytes: |

```
struct SdFileInfoResponse {
    uint32_t sizeBytes;       // Tamaño archivo
    uint16_t sampleRate;      // ej. 44100
    uint16_t bitsPerSample;   // 8, 16, 24
    uint8_t  channels;        // 1=mono, 2=stereo
    uint8_t  reserved[3];
    uint32_t durationMs;      // Duración estimada
};
```

### CMD_SD_LOAD_SAMPLE (0xB3) 🆕
Carga un WAV específico en un pad concreto.

| | Detalle |
|--|---------|
| **Payload Master** | `folder[32] + filename[32] + padIdx(1)` = 65 bytes |
| **Respuesta** | Sin respuesta directa. Se emite `EVT_SD_SAMPLE_LOADED` o `EVT_SD_ERROR` |

**Ejemplo:** Cargar BD2525.WAV en el pad 0:
```c
SdLoadSamplePayload pl;
strcpy(pl.folder, "BD");
strcpy(pl.filename, "BD2525.WAV");
pl.padIdx = 0;
sendCommand(CMD_SD_LOAD_SAMPLE, &pl, sizeof(pl));
// → Evento EVT_SD_SAMPLE_LOADED cuando termine
```

### CMD_SD_LOAD_KIT (0xB4) — RUTA ACTUALIZADA

| | Detalle |
|--|---------|
| **Payload Master** | `kitName[32] + startPad(1) + maxPads(1)` = 34 bytes |
| **Respuesta** | Sin respuesta directa. Se emite `EVT_SD_KIT_LOADED` |

**Antes:** buscaba en `/RED808/<kitName>`  
**Ahora:** busca en **`/data/<kitName>`**

### CMD_SD_KIT_LIST (0xB5) — FILTRADO INTELIGENTE

**Antes:** listaba subdirs de `/RED808/`  
**Ahora:** lista subdirs de `/data/` **excluyendo** familias (2 chars) y `xtra`.  
Solo devuelve carpetas que son **kits completos** (ej. `RED 808 KARZ`).

---

## 5. Flujo recomendado para Master

```
┌─ BOOT ──────────────────────────────────────────────┐
│ 1. Pollear CMD_GET_STATUS hasta resp[10] > 0        │
│ 2. CMD_GET_EVENTS → recibir EVT_SD_BOOT_DONE        │
│    + EVT_SD_XTRA_LOADED                              │
│ 3. UI ya sabe qué pads están loaded y qué kit       │
└─────────────────────────────────────────────────────┘

┌─ CAMBIAR KIT COMPLETO ──────────────────────────────┐
│ 1. CMD_SD_KIT_LIST → obtener lista de kits          │
│ 2. CMD_SD_LOAD_KIT("RED 808 KARZ", 0, 16)          │
│ 3. Pollear GET_STATUS → resp[10] > 0                │
│ 4. CMD_GET_EVENTS → EVT_SD_KIT_LOADED               │
└─────────────────────────────────────────────────────┘

┌─ CAMBIAR SAMPLE INDIVIDUAL ─────────────────────────┐
│ 1. CMD_SD_LIST_FILES("BD") → lista de .wav          │
│ 2. (opcional) CMD_SD_FILE_INFO("BD","BD2525.WAV")   │
│ 3. CMD_SD_LOAD_SAMPLE("BD", "BD2525.WAV", 0)       │
│ 4. Pollear → EVT_SD_SAMPLE_LOADED                   │
└─────────────────────────────────────────────────────┘

┌─ BROWSING XTRA SAMPLES ────────────────────────────┐
│ 1. CMD_SD_LIST_FILES("xtra") → lista de .wav       │
│ 2. CMD_SD_LOAD_SAMPLE("xtra", "dre-yeah.wav", 17)  │
└─────────────────────────────────────────────────────┘
```

---

## 6. Resumen de cambios críticos

| Qué cambió | Impacto Master |
|-------------|---------------|
| GET_STATUS ahora es **54 bytes** (era 20) | **Actualizar parser** |
| Nuevo CMD_GET_EVENTS (0xE4) | **Implementar handler** |
| Ruta `/RED808/` → `/data/` | Actualizar cualquier referencia hardcoded |
| CMD_SD_KIT_LIST filtra familias | UI mostrará solo kits reales |
| CMD_SD_LIST_FILES, FILE_INFO, LOAD_SAMPLE implementados | **Disponibles para usar** |
| LOAD_KIT y LOAD_SAMPLE emiten eventos | No esperar respuesta SPI, usar eventos |

---
---

# 📋 CAMBIOS SLAVE v2 — Synth Engines + Demo Mode

**Fecha:** 26 febrero 2026  
**Firmware:** DaisySeed/main.cpp  
**Archivos nuevos:** `synth/tr808.h`, `synth/tr909.h`, `synth/tr505.h`, `synth/tb303.h`, `synth/demo_mode.h`  
**Afecta a:** Protocolo RED808 — Nuevos comandos 0xC0–0xC6, comportamiento al boot

---

## 1. RESUMEN EJECUTIVO

Se han añadido **7 motores de síntesis matemática** al firmware Slave:

| Motor | Descripción | Instrumentos |
|-------|-------------|-------------|
| **TR-808** | Roland TR-808 analógica | 16: Kick, Snare, Clap, HiHat C/O, 3 Toms, 3 Congas, Claves, Maracas, RimShot, Cowbell, Cymbal |
| **TR-909** | Roland TR-909 (más agresiva) | 11: Kick, Snare, Clap, HiHat C/O, 3 Toms, Ride, Crash, RimShot |
| **TR-505** | Roland TR-505 lo-fi digital | 11: Kick, Snare, Clap, HiHat C/O, 3 Toms, Cowbell, Cymbal, RimShot |
| **TB-303** | Bass synth acid (monofónico) | Oscilador SAW/SQUARE + filtro ladder 24dB/oct + accent + slide |
| **WTOSC** | Wavetable polysynth | 8 ondas con morph, AD por voz, LPF y LFO interno |
| **SH-101** | Mono lead/bass | Osc principal + sub, filtro ladder, doble envolvente y LFO |
| **FM2Op** | FM 2 operadores | Algoritmos FM/aditivo/ring con envelopes separados |

Todo se genera **en tiempo real por CPU** — no usa samples ni SDRAM. El audio de los synths se mezcla directamente al bus master **después** del sidechain y **antes** de la cadena master FX.

Además, se ha añadido un **DEMO MODE** que se reproduce automáticamente al arrancar si no llega ningún comando SPI.

---

## 2. NUEVOS COMANDOS SPI (0xC0–0xC6)

### 2.1 `CMD_SYNTH_TRIGGER` (0xC0)

Dispara un instrumento de percusión en un motor específico.

| Byte | Campo | Tipo | Rango | Descripción |
|------|-------|------|-------|-------------|
| 0 | engine | uint8 | 0–2 | 0=808, 1=909, 2=505 |
| 1 | instrument | uint8 | 0–15 | ID del instrumento (ver tablas abajo) |
| 2 | velocity | uint8 | 0–127 | Se normaliza a 0.0–1.0 internamente |

**Payload total: 3 bytes**

#### Tabla de instrumentos TR-808 (engine=0)

| ID | Instrumento | ID | Instrumento |
|----|------------|-----|------------|
| 0 | Kick | 8 | Low Conga |
| 1 | Snare | 9 | Mid Conga |
| 2 | Clap | 10 | Hi Conga |
| 3 | HiHat Closed | 11 | Claves |
| 4 | HiHat Open | 12 | Maracas |
| 5 | Low Tom | 13 | RimShot |
| 6 | Mid Tom | 14 | Cowbell |
| 7 | Hi Tom | 15 | Cymbal |

#### Tabla de instrumentos TR-909 (engine=1)

| ID | Instrumento | ID | Instrumento |
|----|------------|-----|------------|
| 0 | Kick | 6 | Hi Tom |
| 1 | Snare | 7 | Ride |
| 2 | Clap | 8 | Crash |
| 3 | HiHat Closed | 9 | Cowbell |
| 4 | HiHat Open | 10 | RimShot |
| 5 | Low Tom | | |

#### Tabla de instrumentos TR-505 (engine=2)

| ID | Instrumento | ID | Instrumento |
|----|------------|-----|------------|
| 0 | Kick | 6 | Hi Tom |
| 1 | Snare | 7 | Cowbell |
| 2 | Clap | 8 | Cymbal |
| 3 | HiHat Closed | 9 | RimShot |
| 4 | HiHat Open | 10 | (reservado) |
| 5 | Low Tom | | |

**Ejemplo Master (C):**
```c
// Disparar kick 808 a velocidad máxima
uint8_t payload[] = { 0x00, 0x00, 0x7F };
sendCommand(CMD_SYNTH_TRIGGER, payload, 3);

// Disparar snare 909 a velocidad media
uint8_t payload2[] = { 0x01, 0x01, 0x64 };
sendCommand(CMD_SYNTH_TRIGGER, payload2, 3);
```

---

### 2.2 `CMD_SYNTH_PARAM` (0xC1)

Modifica un parámetro de un instrumento de percusión.

| Byte | Campo | Tipo | Descripción |
|------|-------|------|-------------|
| 0 | engine | uint8 | 0=808, 1=909, 2=505 |
| 1 | instrument | uint8 | ID del instrumento |
| 2 | paramId | uint8 | ID del parámetro (ver tabla) |
| 3–6 | value | float32 | Valor (little-endian IEEE 754) |

**Payload total: 7 bytes**

#### Tabla de parámetros

| paramId | Nombre | Rango | Aplica a |
|---------|--------|-------|----------|
| 0 | decay | 0.0–1.0 | Todos los instrumentos |
| 1 | pitch | Hz (ej. 30–200) | Kick, Toms |
| 2 | tone / saturation | 0.0–1.0 | Snare (tone), Kick (saturation) |
| 3 | volume | 0.0–1.0 | Todos los instrumentos |
| 4 | snappy | 0.0–1.0 | Snare |

**Ejemplo:**
```c
// Subir decay del kick 808 a 0.8
float val = 0.8f;
uint8_t payload[7] = { 0x00, 0x00, 0x00 };
memcpy(payload + 3, &val, 4);
sendCommand(CMD_SYNTH_PARAM, payload, 7);
```

---

### 2.3 `CMD_SYNTH_NOTE_ON` (0xC2) — Solo TB-303

Inicia una nota en el sintetizador acid bass.

| Byte | Campo | Tipo | Descripción |
|------|-------|------|-------------|
| 0 | midiNote | uint8 | 0–127 (ej. 36=C2, 48=C3) |
| 1 | accent | uint8 | 0=normal, 1=accent |
| 2 | slide | uint8 | 0=normal, 1=slide (portamento) |

**Payload total: 3 bytes**

**Ejemplo:**
```c
// C2 con accent, sin slide
uint8_t payload[] = { 36, 1, 0 };
sendCommand(CMD_SYNTH_NOTE_ON, payload, 3);

// Eb2 con slide desde la nota anterior
uint8_t payload2[] = { 39, 0, 1 };
sendCommand(CMD_SYNTH_NOTE_ON, payload2, 3);
```

---

### 2.4 `CMD_SYNTH_NOTE_OFF` (0xC3) — Solo TB-303

Corta la nota actual del 303.

**Payload: 0 bytes** (solo header)

```c
sendCommand(CMD_SYNTH_NOTE_OFF, NULL, 0);
```

---

### 2.5 `CMD_SYNTH_303_PARAM` (0xC4) — Solo TB-303

Modifica parámetros del sintetizador acid.

| Byte | Campo | Tipo | Descripción |
|------|-------|------|-------------|
| 0 | paramId | uint8 | ID del parámetro (ver tabla) |
| 1–4 | value | float32 | Valor (little-endian IEEE 754) |

**Payload total: 5 bytes**

#### Tabla de parámetros 303

| paramId | Nombre | Rango típico | Descripción |
|---------|--------|-------------|-------------|
| 0 | cutoff | 60.0–8000.0 | Frecuencia de corte del filtro (Hz) |
| 1 | resonance | 0.0–1.0 | Resonancia (feedback del filtro) |
| 2 | envMod | 0.0–1.0 | Cantidad de modulación del envelope al filtro |
| 3 | decay | 0.05–2.0 | Tiempo de decay del envelope (segundos) |
| 4 | accent | 0.0–1.0 | Intensidad del accent |
| 5 | slideTime | 0.01–0.2 | Tiempo de portamento (segundos) |
| 6 | waveform | 0.0 / 1.0 | < 0.5 = SAW, ≥ 0.5 = SQUARE |
| 7 | volume | 0.0–1.0 | Volumen de salida |

**Ejemplo:**
```c
// Subir cutoff a 2000 Hz
float cutoff = 2000.0f;
uint8_t payload[5] = { 0x00 };
memcpy(payload + 1, &cutoff, 4);
sendCommand(CMD_SYNTH_303_PARAM, payload, 5);

// Cambiar a onda cuadrada
float square = 1.0f;
uint8_t payload2[5] = { 0x06 };
memcpy(payload2 + 1, &square, 4);
sendCommand(CMD_SYNTH_303_PARAM, payload2, 5);
```

---

### 2.6 `CMD_SYNTH_ACTIVE` (0xC5)

Activa/desactiva motores de síntesis individualmente.

| Byte | Campo | Tipo | Descripción |
|------|-------|------|-------------|
| 0 | engineMask | uint8 | Bitmask de engines activos |

**Payload total: 1 byte**

| Bit | Motor | Activar | Desactivar |
|-----|-------|---------|------------|
| 0 | TR-808 | `mask \| 0x01` | `mask & ~0x01` |
| 1 | TR-909 | `mask \| 0x02` | `mask & ~0x02` |
| 2 | TR-505 | `mask \| 0x04` | `mask & ~0x04` |
| 3 | TB-303 | `mask \| 0x08` | `mask & ~0x08` |
| 4 | WTOSC | `mask \| 0x10` | `mask & ~0x10` |
| 5 | SH-101 | `mask \| 0x20` | `mask & ~0x20` |
| 6 | FM2Op | `mask \| 0x40` | `mask & ~0x40` |

**Valor por defecto: `0x7B`** (808+909+303+WTOSC+SH101+FM2Op; 505 desactivado)

**Ejemplos:**
```c
// Solo 808 + 303
uint8_t mask = 0x09;  // bit0 + bit3
sendCommand(CMD_SYNTH_ACTIVE, &mask, 1);

// Todos apagados
uint8_t off = 0x00;
sendCommand(CMD_SYNTH_ACTIVE, &off, 1);

// Todos encendidos (default)
uint8_t all = 0x7F;
sendCommand(CMD_SYNTH_ACTIVE, &all, 1);
```

---

### 2.7 `CMD_SYNTH_PRESET` (0xC6)

Aplica uno de los **4 presets de fábrica** de un motor de síntesis.

| Byte | Campo | Tipo | Descripción |
|------|-------|------|-------------|
| 0 | engine | uint8 | 0=808, 1=909, 2=505, 3=303, 4=WTOSC, 5=SH-101, 6=FM2Op |
| 1 | preset | uint8 | 0–3 |

**Payload total: 2 bytes**

#### Bancos disponibles

| Engine | Preset 0 | Preset 1 | Preset 2 | Preset 3 |
|--------|----------|----------|----------|----------|
| 808 | Classic808 | HipHop | Techno | Latin |
| 909 | Classic909 | Techno | HousePound | Industrial |
| 505 | Classic505 | NewWave | Electro | LoFiHipHop |
| 303 | Classic Acid | Resonant Squelch | Sub Bass | Soft Lead |
| WTOSC | Classic Pad | Glass Pluck | Organ Motion | PWM Bass |
| SH-101 | Bass Punch | Acid Lead | PWM Keys | Drone Pad |
| FM2Op | FM Bass | EPiano | Bell | Growl Lead |

**Ejemplos:**
```c
// Cargar preset Techno del 909
uint8_t techno909[] = { 0x01, 0x01 };
sendCommand(CMD_SYNTH_PRESET, techno909, 2);

// Cargar preset Bell del FM2Op
uint8_t bellFm[] = { 0x06, 0x02 };
sendCommand(CMD_SYNTH_PRESET, bellFm, 2);
```

---

## 3. DEMO MODE — Comportamiento al boot

### ¿Qué es?
Al arrancar, si la Daisy no recibe ningún comando SPI, reproduce automáticamente una secuencia de audio de **3 minutos** usando los motores de síntesis. Esto permite verificar que el audio funciona sin necesidad de hardware externo.

### Comportamiento
- **Se activa automáticamente** al boot (`demoModeActive = true`)
- **Se desactiva al instante** cuando llega el **primer comando SPI válido** del ESP32-S3
- Una vez desactivado, **no se reactiva** hasta el próximo reset/power cycle
- El demo **no interfiere** con los samples cargados — solo usa los engines de síntesis

### Guión de la demo (3 minutos)

| Tiempo | Sección | BPM | Descripción |
|--------|---------|-----|-------------|
| 0:00 | Intro | 90 | Kick 808 solo |
| 0:15 | +Snare | 90 | Snare 808 en beats 2 y 4 |
| 0:25 | +HiHats | 90 | Hi-hats 808 con swing 56% |
| 0:40 | +Bass | 90 | Línea acid 303 (filtro cerrado, 200 Hz) |
| 1:00 | Sweep | 90 | Cutoff 303 sube: 200→3000 Hz en 30s |
| 1:30 | **MORPH** | 90→145 | BPM sube, swing→0%, kick 808→909, cutoff→4000 |
| 2:10 | Detroit | 145 | 909 completo: kick+snare+clap+hihats rápidos |
| 2:50 | Fade | 145 | Fade out lineal 10 segundos |
| 3:00 | Reset | — | Silencio, la demo reinicia desde 0:00 |

### Acción requerida Master

**Ninguna obligatoria.** El demo se apaga solo al recibir cualquier comando SPI.

Si queréis **control explícito**, podéis:
```c
// Enviar un PING al boot para desactivar el demo inmediatamente
sendCommand(CMD_PING, NULL, 0);
```

O simplemente enviar cualquier otro comando (CMD_GET_STATUS, etc.) — cualquier paquete SPI válido desactiva el demo.

### Si no queréis demo al boot

En una futura versión podemos añadir un `CMD_DEMO_MODE` (0xC6) para activarlo/desactivarlo por comando. De momento el comportamiento es: **siempre arranca en demo, primer SPI lo mata**.

---

## 4. IMPACTO EN CMD_RESET (0xEF)

Al recibir `CMD_RESET`, ahora también se resetean los synth engines:
- Se reinicializan las 4 instancias (808, 909, 505, 303)
- `synthActiveMask` vuelve a `0x0F`
- **El demo mode NO se reactiva** con un reset — solo al power cycle

---

## 5. AUDIO ROUTING

```
┌─────────────────────────────────────────────────────────────────┐
│  AUDIO CALLBACK (por sample, 48 kHz)                            │
│                                                                  │
│  ┌──────────┐    ┌─────────────┐    ┌────────────────────────┐  │
│  │ 32 Voices│───>│ Per-Track FX│───>│ busL / busR            │  │
│  │ (samples)│    │ Filter,Echo │    │                        │  │
│  └──────────┘    │ Comp,EQ,Pan │    │  += synthMix ◄─────┐  │  │
│                  └─────────────┘    │                     │  │  │
│                                     └────────┬───────────┘  │  │
│                                              │              │  │
│  ┌──────────────────────────────┐            ▼              │  │
│  │ SYNTH ENGINES               │    ┌──────────────┐       │  │
│  │ synth808.Process()  ──┐     │    │ MASTER FX    │       │  │
│  │ synth909.Process()  ──┤     │    │ × masterGain │       │  │
│  │ synth505.Process()  ──┼─sum─┼───>│ Filter,Dist  │       │  │
│  │ acid303.Process()   ──┘     │    │ Delay,Reverb │       │  │
│  └──────────────────────────────┘    │ Chorus,Comp  │       │  │
│                                      │ Limiter      │       │  │
│  ┌──────────────────────────────┐    └──────┬───────┘       │  │
│  │ DEMO MODE                   │            │               │  │
│  │ demoSeq.ProcessSample()     │            ▼               │  │
│  │ → triggers synth engines    │    ┌──────────────┐        │  │
│  │ → devuelve fadeGain         │    │ DAC Output   │        │  │
│  │ → synthMix *= fadeGain      │    │ (Audio Jack) │        │  │
│  └──────────────────────────────┘    └──────────────┘        │  │
└──────────────────────────────────────────────────────────────────┘
```

Los synths se mezclan **DESPUÉS** del sidechain envelope y **ANTES** del master FX chain. Esto significa:
- Master Volume (`CMD_MASTER_VOLUME`) afecta al volumen de los synths
- Global Filter, Delay, Reverb, etc. se aplican también a los synths
- El sidechain **no** afecta a los synths (intencionado)

---

## 6. EJEMPLO COMPLETO — Secuencia acid desde el Master

```c
void playAcidDemo() {
    // 1. Activar solo 808 + 303
    uint8_t mask = 0x09;
    sendCommand(CMD_SYNTH_ACTIVE, &mask, 1);
    
    // 2. Configurar 303: SAW, cutoff bajo, resonancia alta
    setParam303(0, 300.0f);    // cutoff = 300 Hz
    setParam303(1, 0.8f);      // resonance = 0.8
    setParam303(2, 0.6f);      // envMod = 0.6
    setParam303(3, 0.15f);     // decay = 150ms
    
    // 3. Loop de secuencia (desde el secuenciador del ESP32)
    while(playing) {
        // Kick en beat 1
        uint8_t kick[] = { 0x00, 0x00, 0x7F };
        sendCommand(CMD_SYNTH_TRIGGER, kick, 3);
        
        // Nota 303
        uint8_t note[] = { currentNote, hasAccent, hasSlide };
        sendCommand(CMD_SYNTH_NOTE_ON, note, 3);
        
        waitForNextStep();
        
        // Note off si no hay slide
        if(!nextHasSlide)
            sendCommand(CMD_SYNTH_NOTE_OFF, NULL, 0);
    }
}

// Helper para 303 params
void setParam303(uint8_t paramId, float value) {
    uint8_t payload[5];
    payload[0] = paramId;
    memcpy(payload + 1, &value, 4);
    sendCommand(CMD_SYNTH_303_PARAM, payload, 5);
}
```

---

## 7. MAPA COMPLETO DE COMANDOS (actualizado)

| Rango | Grupo | Comandos |
|-------|-------|----------|
| 0x01–0x05 | Triggers | SEQ, LIVE, STOP, STOP_ALL, SIDECHAIN |
| 0x10–0x14 | Volume/Pitch | MASTER, SEQ, LIVE, TRACK, LIVE_PITCH |
| 0x20–0x26 | Global Filter | SET, CUTOFF, RES, BITDEPTH, DIST, MODE, SR |
| 0x30–0x4F | Master FX | Delay, Phaser, Flanger, Comp, Reverb, Chorus, Tremolo, Wavefolder, Limiter |
| 0x50–0x65 | Per-Track FX | Filter, Dist, Bitcrush, Echo, Flanger, Comp, Sends, Pan, Mute, Solo, EQ |
| 0x70–0x7A | Per-Pad FX | Filter, Dist, Bitcrush, Loop, Rev, Pitch, Stutter, Scratch, Turntablism |
| 0x90–0x91 | Sidechain | SET, CLEAR |
| 0xA0–0xA4 | Sample Transfer | BEGIN, DATA, END, UNLOAD, UNLOAD_ALL |
| 0xB0–0xB9 | SD Card | LIST_FOLDERS, LIST_FILES, FILE_INFO, LOAD_SAMPLE, LOAD_KIT, KIT_LIST, STATUS, UNLOAD, GET_LOADED, ABORT |
| **0xC0–0xC5** | **🆕 Synth Engines** | **TRIGGER, PARAM, NOTE_ON, NOTE_OFF, 303_PARAM, ACTIVE** |
| 0xE0–0xEF | Status/Query | STATUS, PEAKS, CPU, VOICES, EVENTS, PING, RESET |
| 0xF0–0xF1 | Bulk | TRIGGERS, FX |

---

## 8. RESUMEN DE ACCIONES REQUERIDAS PARA EL MASTER

| Prioridad | Qué hacer | Detalle |
|-----------|-----------|---------|
| ⚠️ Informativo | Demo mode al boot | Se apaga solo al primer SPI. Enviad `CMD_PING` si queréis desactivar inmediatamente |
| 🔧 Opcional | Implementar `CMD_SYNTH_TRIGGER` (0xC0) | Para UI de drum pads sintéticos |
| 🔧 Opcional | Implementar `CMD_SYNTH_NOTE_ON/OFF` (0xC2/0xC3) | Para sequencer 303 en el Master |
| 🔧 Opcional | Implementar `CMD_SYNTH_303_PARAM` (0xC4) | Para knobs de cutoff, resonance, etc. |
| 🔧 Opcional | Implementar `CMD_SYNTH_PARAM` (0xC1) | Para editar decay/pitch/volume por instrumento |
| 🔧 Opcional | Implementar `CMD_SYNTH_ACTIVE` (0xC5) | Para activar/desactivar engines desde la UI |

**Ningún cambio es breaking** — el protocolo existente sigue funcionando exactamente igual. Los comandos synth son 100% adicionales.
