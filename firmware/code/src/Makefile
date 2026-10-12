# Project Name
TARGET = FRIZZ

# make BENCH=1: FRIZZ-bench.bin, which measures the audio callback's load (Bench.h), built
# apart in build-bench/ so it never mixes with the normal build's objects
ifeq ($(BENCH),1)
TARGET = FRIZZ-bench
# cpu.txt names the source it was built from by a checksum of it, not by the time, so a
# rebuild of the same source gives the same FRIZZ-bench.bin
BENCH_SOURCE := $(shell cat $(sort $(wildcard *.h *.cpp)) | cksum | cut -d' ' -f1)
C_DEFS = -DFRIZZ_BENCH=1 -DFRIZZ_BENCH_SOURCE='"$(BENCH_SOURCE)"'
override BUILD_DIR = build-bench$(if $(filter 1,$(ITCM)),-itcm)
endif

# make ITCM=1: the per-sample code runs from ITCM (FrizzHot.h), an experiment (#51), built
# apart in build-itcm/ (build-bench-itcm/ with BENCH=1): make doesn't rebuild an object for
# other C_DEFS, so in build/ a stale one of either kind would pass as fresh (#171)
ifeq ($(ITCM),1)
C_DEFS += -DFRIZZ_ITCM=1
ifneq ($(BENCH),1)
override BUILD_DIR = build-itcm
endif
endif

OPT=-O3

APP_TYPE=BOOT_SRAM
# make program-boot installs CHOMPI's own bootloader, not libDaisy's generic one
BOOT_BIN = ../../bin/CHOMPI_Bootloader_V6_2_0.bin
LDSCRIPT = chompi_sram.lds

# Sources
CPP_SOURCES = chompi_main.cpp \
	encoder.cpp

# FatFs: the FX scenes and master settings on the card (SceneStore.h)
USE_FATFS = 1

# Libs
LIBDAISY_DIR = ../libs/libDaisy
DAISYSP_DIR = ../libs/DaisySP

# libDaisy's USB CDC class, for its transfer's state (MidiClock::UsbReady)
C_INCLUDES = -I$(LIBDAISY_DIR)/Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc

# Core location, and generic Makefile
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# A change to the memory layout, or to a prebuilt library it links (a libDaisy fix rebuilt
# into libdaisy.a, THIRD_PARTY.md), relinks (#161)
$(BUILD_DIR)/$(TARGET).elf: $(LDSCRIPT) $(LIBDAISY_DIR)/build/libdaisy.a $(DAISYSP_DIR)/build/libdaisysp.a
