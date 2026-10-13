# Project Name
TARGET = CHOMPI

DEBUG=0
# OPT=-O0
OPT=-O3

APP_TYPE=BOOT_SRAM
LDSCRIPT = chompi_sram.lds

LDSCRIPT=chompi_sram.lds

# FreeRTOS coreJSON
C_SOURCES = ../libs/coreJSON/source/core_json.c
C_INCLUDES +=  -I../libs/coreJSON/source/include

# TinyUSB
TINYUSB_DIR = ../libs/tinyusb/src
TINYUSB_SOURCES = $(TINYUSB_DIR)/tusb.c \
	$(TINYUSB_DIR)/common/tusb_fifo.c \
	$(TINYUSB_DIR)/device/usbd.c \
	$(TINYUSB_DIR)/class/audio/audio_device.c \
	$(TINYUSB_DIR)/class/midi/midi_device.c \
	$(TINYUSB_DIR)/portable/synopsys/dwc2/dcd_dwc2.c \
	$(TINYUSB_DIR)/portable/synopsys/dwc2/dwc2_common.c
C_SOURCES += $(TINYUSB_SOURCES) usb_descriptors.c
C_INCLUDES += -I. -I$(TINYUSB_DIR)

# Sources
CPP_SOURCES = chompi_main.cpp \
	encoder.cpp \
	FileStreamingManager.cpp \
	Streamer.cpp \
	Voice.cpp \
	UsbDevice.cpp

USE_FATFS = 1

# SD transfers that end run the SD timer at once, see __wrap_HAL_SD_IRQHandler
LDFLAGS = -Wl,--wrap=HAL_SD_IRQHandler

# Libs
LIBDAISY_DIR = ../libs/libDaisy
DAISYSP_DIR = ../libs/DaisySP

# Core location, and generic Makefile
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# for size
$(addprefix $(BUILD_DIR)/,$(notdir $(TINYUSB_SOURCES:.c=.o))): OPT = -Os
