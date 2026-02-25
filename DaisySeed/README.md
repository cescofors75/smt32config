# RED808 DrumMachine — Daisy Seed Slave

Drum machine de **24 pads** con efectos completos.  
SPI3 slave — protocolo RED808 compatible con **ESP32-S3** master.

## Hardware

| Componente | Descripción |
|---|---|
| **Daisy Seed** | Electro-Smith (STM32H750 + 64MB SDRAM + codec integrado) |
| **Micro SD** | Socket SDMMC 4-bit (3.3V, NO módulo SPI) |
| **ESP32-S3 N16R8** | Master SPI + Web UI + Sequencer |

## Conexiones

### SPI3: ESP32-S3 → Daisy Seed

| Señal | ESP32-S3 GPIO | Daisy Pin | STM32H750 |
|-------|---------------|-----------|------------|
| SCK   | GPIO 12       | **D10**   | PC10 (SPI3_SCK) |
| MOSI  | GPIO 11       | **D9**    | PC11 (SPI3_MOSI/RX) |
| MISO  | GPIO 13       | **D8**    | PC12 (SPI3_MISO/TX) |
| CS    | GPIO 10       | **D7**    | PA15 (SPI3_NSS) |
| GND   | GND           | GND       | — |

> **SPI Mode 0**, MSB first, 8-bit, Hardware NSS.  
> Bring-up @ 2 MHz → estable @ 20 MHz.

### SD Card (SDMMC 4-bit)

| SD Pin | Daisy Pin |
|--------|-----------|
| CLK    | D18       |
| CMD    | D19       |
| DAT0   | D20       |
| DAT1   | D21       |
| DAT2   | D22       |
| DAT3   | D23       |
| VCC    | 3V3       |
| GND    | GND       |

### Audio Output
Codec integrado en la Daisy Seed → salida por **jack de 3.5mm**.

## Especificaciones

| Parámetro | Valor |
|-----------|-------|
| Sample rate | **44100 Hz** |
| Block size | **128 samples** |
| Max pads | **24** (16 seq + 8 XTRA) |
| Max voces | **32** polyphonic |
| DSP | Float 32-bit |
| SDRAM para samples | 24 × 96000 int16 ≈ 4.4 MB |
| Max duración sample | ~2.17 s per pad (SPI) |
| SD card loading | ~0.25 s para 16 samples |

## Protocolo RED808

Paquete SPI: **8 bytes header + payload**

```
[0xA5] [CMD] [LEN_L] [LEN_H] [SEQ_L] [SEQ_H] [CRC_L] [CRC_H] [PAYLOAD...]
```

- Magic: `0xA5` (CMD) / `0x5A` (RESP)
- CRC16-Modbus sobre payload
- Respuesta NUNCA desde ISR — flag `pendingResponse` + main loop

### Comandos implementados

| Grupo | CMDs | Estado |
|-------|------|--------|
| Triggers | 0x01-0x05 | ✅ |
| Volume | 0x10-0x14 | ✅ |
| Global Filter | 0x20-0x26 | ✅ |
| Delay | 0x30-0x33 | ✅ |
| Phaser | 0x34-0x37 | ✅ |
| Flanger | 0x38-0x3C | ✅ |
| Compressor | 0x3D-0x42 | ✅ |
| Reverb | 0x43-0x46 | ✅ |
| Chorus | 0x47-0x4A | ✅ |
| Tremolo | 0x4B-0x4D | ✅ |
| Wavefolder/Limiter | 0x4E-0x4F | ✅ |
| Track FX | 0x50-0x58 | ✅ |
| Track Sends/Pan/Mute | 0x59-0x65 | ✅ |
| Pad FX | 0x70-0x7A | ✅ |
| Sidechain | 0x90-0x91 | ✅ |
| Sample Transfer | 0xA0-0xA4 | ✅ |
| SD Card | 0xB0-0xB9 | ✅ (core) |
| Status/Peaks/Ping | 0xE0-0xEF | ✅ |
| Bulk | 0xF0-0xF1 | ✅ |

## Setup del toolchain

```bash
# 1. Instalar arm-none-eabi-gcc
#    https://developer.arm.com/downloads/-/gnu-rm

# 2. Clonar libDaisy y DaisySP
cd DaisySeed/
git clone https://github.com/electro-smith/libDaisy.git libdaisy
git clone https://github.com/electro-smith/DaisySP.git DaisySP

# 3. Compilar libDaisy
cd libdaisy && make -j4 && cd ..

# 4. Compilar DaisySP
cd DaisySP && make -j4 && cd ..

# 5. Compilar el firmware
make -j4
```

## Flash

### DFU (USB)
1. Conecta la Daisy por USB
2. Mantén **BOOT** y pulsa **RESET** → modo DFU
3. `make program-dfu`

### ST-Link
```bash
make program
```

## Estructura SD Card (microSD FAT32, ≤32 GB)

```
/data/
  ├── RED 808 KARZ/          ← Kit por defecto (LIVE PADS 0-15 al arrancar)
  │   ├── 808 BD 3-1.wav       Mapeo automático BD→pad0, SD→1, HH→2, etc.
  │   ├── 808 SD 1-5.wav       Duplicados (ej. 2x SD, 3x HC) van a pads libres.
  │   ├── 808 HH.wav
  │   ├── 808 OH 1.wav
  │   ├── 808 CY 3-1.wav
  │   ├── 808 CP.wav
  │   ├── 808 RS.wav
  │   ├── 808 COW.wav
  │   └── ... (16 wavs)
  │
  ├── BD/                     ← Familias de instrumentos (selección desde Master)
  │   ├── BD0000.WAV            Master envía CMD_SD_LIST_FILES("BD") para listar
  │   ├── BD2525.WAV            Master envía CMD_SD_LOAD_SAMPLE("BD","BD2525.WAV",0)
  │   └── ... (25 variantes)
  ├── SD/                     ← 25 variantes de snare
  ├── CH/                     ← 1 closed hihat
  ├── OH/                     ← 5 variantes open hihat
  ├── CY/                     ← 25 variantes cymbal
  ├── CP/  RS/  CB/           ← 1 variante cada uno
  ├── LT/  MT/  HT/           ← 5 variantes toms
  ├── MA/  CL/                ← 1 variante cada uno
  ├── HC/  MC/  LC/           ← 5 variantes congas
  │
  └── xtra/                   ← XTRA PADS (pads 16-23, cargados al arrancar)
      ├── Alesis-Fusion-Bass-C3.wav
      ├── dre-yeah.wav
      ├── fast114bpm.wav
      └── ragefx.wav
```

### Flujo de carga

1. **Boot**: `AutoLoadFromSD()` carga `RED 808 KARZ/` en LIVE PADS 0-15 (mapeo inteligente por nombre de instrumento) y `xtra/` en XTRA PADS 16-23.
2. **Master cambia kit**: `CMD_SD_KIT_LIST` (0xB5) lista kits → `CMD_SD_LOAD_KIT` (0xB4) carga uno.
3. **Master cambia sample individual**: `CMD_SD_LIST_FILES` (0xB1) lista .wav de una familia → `CMD_SD_LOAD_SAMPLE` (0xB3) carga uno en un pad concreto.
4. **Master consulta info**: `CMD_SD_LIST_FOLDERS` (0xB0) lista todas las carpetas, `CMD_SD_FILE_INFO` (0xB2) devuelve tamaño/sr/bps/duración.

**Formatos WAV soportados:** 8/16/24-bit, mono o estéreo, cualquier sample rate (se almacena tal cual).

## Módulos DaisySP utilizados

| Efecto | Módulo |
|--------|--------|
| Delay | `DelayLine<float, 88200>` |
| Reverb | `ReverbSc` |
| Chorus | `Chorus` |
| Tremolo | `Tremolo` |
| Compressor | `Compressor` |
| Wavefolder | `Fold` |
| Phaser | `Phaser` |
| Filters | `Biquad` custom (LP/HP/BP/Notch/Peak/Shelf) |

## Debug USB

Monitor serie (115200 baud) muestra:
- Carga de samples al arrancar
- Estado SD card
- SPI3 ready
- "RED808 DRUM MACHINE READY"

## Criterio de éxito

1. **PING OK** → ESP32 muestra `STM32 connected! RTT: ~300us`
2. **Samples cargados** → 16/16 via SPI o SD
3. **Audio** → Triggers suenan a 44100 Hz estéreo
4. **FX** → Delay (0x30), Reverb (0x43), Comp (0x3D) audibles
5. **SD** → Kit list + load desde web UI
