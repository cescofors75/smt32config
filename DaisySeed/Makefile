# ══════════════════════════════════════════════════════════════════
#  DrumMachine — Daisy Seed Edition
#  16-Pad Sampler + FX + SPI Slave + Synth Engines (808/909/505/303)
# ══════════════════════════════════════════════════════════════════

TARGET = DrumMachine

CPP_SOURCES = main.cpp

# Include path for synth headers
C_INCLUDES += -I.

OPT = -O2

# ── Ajusta estas rutas a tu instalación ──
LIBDAISY_DIR = ./libdaisy
DAISYSP_DIR  = ./DaisySP

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
