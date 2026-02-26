# ══════════════════════════════════════════════════════════════════
#  DrumMachine — Daisy Seed Edition
#  16-Pad Sampler + FX + SPI Slave + Synth Engines (808/909/505/303)
# ══════════════════════════════════════════════════════════════════

TARGET = DrumMachine

CPP_SOURCES = main.cpp

# Include path for synth headers + DaisySP-LGPL
C_INCLUDES += -I.
C_INCLUDES += -I./DaisySP/DaisySP-LGPL/Source

OPT = -O2

# ── Boot desde SRAM (requiere Daisy Bootloader) ──
# El STM32H750 solo tiene 128K flash interna, insuficiente.
# BOOT_SRAM: bootloader copia app de QSPI→SRAM (480K disponibles)
APP_TYPE = BOOT_SRAM

# ── Ajusta estas rutas a tu instalación ──
LIBDAISY_DIR = ./libdaisy
DAISYSP_DIR  = ./DaisySP

# ── Linkear DaisySP-LGPL (ReverbSc, Compressor, Fold, etc.) ──
LIBS += -ldaisysp-lgpl
LIBDIR += -L./DaisySP/DaisySP-LGPL/build

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
