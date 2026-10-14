# Project Name
TARGET = FRIZZ

# make BENCH=1: FRIZZ-bench.bin, which measures the audio callback's load (Bench.h), built
# apart in build-bench/ so it never mixes with the normal build's objects
ifeq ($(BENCH),1)
TARGET = FRIZZ-bench
# cpu.txt names the source it was built from by a checksum of it, not by the time, so a
# rebuild of the same source gives the same FRIZZ-bench.bin
BENCH_SOURCE := $(shell cat $(sort $(wildcard *.h *.c *.cpp)) | cksum | cut -d' ' -f1)
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

# make USB_SAFETY=1: a development build for a new USB stack, with UsbDevice.h's Watch() (an
# enumeration that never finishes, or no MIDI for 60 s, restarts into the launcher), built
# apart in build-usbsafety/ (build-bench-usbsafety/ with BENCH=1). Never a player's build
ifeq ($(USB_SAFETY),1)
C_DEFS += -DFRIZZ_USB_SAFETY=1
override BUILD_DIR := $(or $(BUILD_DIR),build)-usbsafety
endif

OPT=-O3

APP_TYPE=BOOT_SRAM
# make program-boot installs CHOMPI's own bootloader, not libDaisy's generic one
BOOT_BIN = ../../bin/CHOMPI_Bootloader_V6_2_0.bin
LDSCRIPT = chompi_sram.lds

# TinyUSB (../libs/tinyusb/VERSION): FRIZZ's USB (UsbDevice.h), its classes built for size
TINYUSB_DIR = ../libs/tinyusb/src
TINYUSB_SOURCES = $(TINYUSB_DIR)/tusb.c \
	$(TINYUSB_DIR)/common/tusb_fifo.c \
	$(TINYUSB_DIR)/device/usbd.c \
	$(TINYUSB_DIR)/class/audio/audio_device.c \
	$(TINYUSB_DIR)/class/midi/midi_device.c \
	$(TINYUSB_DIR)/class/msc/msc_device.c \
	$(TINYUSB_DIR)/portable/synopsys/dwc2/dcd_dwc2.c \
	$(TINYUSB_DIR)/portable/synopsys/dwc2/dwc2_common.c

# Sources
C_SOURCES = $(TINYUSB_SOURCES) usb_descriptors.c
CPP_SOURCES = chompi_main.cpp \
	encoder.cpp \
	UsbDevice.cpp

# FatFs: the FX scenes and master settings on the card (SceneStore.h)
USE_FATFS = 1

# Libs
LIBDAISY_DIR = ../libs/libDaisy
DAISYSP_DIR = ../libs/DaisySP

C_INCLUDES = -I. -I$(TINYUSB_DIR)

# Core location, and generic Makefile
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# A change to the memory layout, or to a prebuilt library it links (a libDaisy fix rebuilt
# into libdaisy.a, THIRD_PARTY.md), relinks (#161)
$(BUILD_DIR)/$(TARGET).elf: $(LDSCRIPT) $(LIBDAISY_DIR)/build/libdaisy.a $(DAISYSP_DIR)/build/libdaisysp.a

# TinyUSB at -Os, as lost-tty/CHOMPI builds it: its code isn't the audio callback's
$(addprefix $(BUILD_DIR)/,$(notdir $(TINYUSB_SOURCES:.c=.o))): OPT = -Os
