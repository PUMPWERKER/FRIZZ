#pragma once
// FRIZZ's USB interfaces. From lost-tty/CHOMPI's TAPE (MIT, usb_descriptors.h), without MSC.
// usb-audio-poc (#196), an experiment.
#include "tusb_config.h"

#if FRIZZ_USB_AUDIO
enum
{
    ITF_NUM_AUDIO_CONTROL = 0,
    ITF_NUM_AUDIO_STREAMING_RX, // host -> FRIZZ
    ITF_NUM_AUDIO_STREAMING_TX, // FRIZZ -> host
    ITF_NUM_MIDI,
    ITF_NUM_MIDI_STREAMING,
    ITF_NUM_TOTAL
};
#else
enum
{
    ITF_NUM_MIDI = 0,
    ITF_NUM_MIDI_STREAMING,
    ITF_NUM_TOTAL
};
#endif

// the audio entities' IDs, shared by the descriptors and the audio control requests
#define UAC_ENTITY_CLOCK              0x04 // UAC2 only
#define UAC_ENTITY_RX_INPUT_TERMINAL  0x01 // host -> FRIZZ
#define UAC_ENTITY_RX_OUTPUT_TERMINAL 0x03
#define UAC_ENTITY_TX_INPUT_TERMINAL  0x11 // FRIZZ -> host
#define UAC_ENTITY_TX_OUTPUT_TERMINAL 0x13
