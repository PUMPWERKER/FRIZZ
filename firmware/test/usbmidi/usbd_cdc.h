// usbd_cdc.h for usbmidi.cpp: what libDaisy's usb_midi.cpp takes from ST's USB CDC class (the
// mode it switches the device to), without the rest of ST's USB stack
#pragma once
#include <cstdint>
#define USBD_MODE_CDC 0
#define USBD_MODE_MIDI 1
extern uint8_t usbd_mode;
