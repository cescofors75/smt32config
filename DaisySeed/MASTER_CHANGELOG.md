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
