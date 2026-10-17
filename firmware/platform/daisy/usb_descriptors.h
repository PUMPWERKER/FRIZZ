#pragma once
// The USB device (usb_descriptors.c): its two layouts, one of them chosen as it connects
// (UsbDevice.h's Start and SetAudio).
#include <stdbool.h>
#include "tusb_config.h"

// with USB audio on: the sound card, then MIDI, then the card as a drive
enum
{
    ITF_AUDIO_CONTROL = 0,
    ITF_AUDIO_STREAMING_RX, // the computer -> the device
    ITF_AUDIO_STREAMING_TX, // the device -> the computer
    ITF_AUDIO_MIDI,
    ITF_AUDIO_MIDI_STREAMING,
    ITF_AUDIO_MSC,
    ITF_AUDIO_TOTAL
};
// with it off: MIDI and the drive
enum
{
    ITF_MIDI = 0,
    ITF_MIDI_STREAMING,
    ITF_MIDI_MSC,
    ITF_MIDI_TOTAL
};

// the audio function's terminals, each direction an input terminal and an output terminal
#define UAC_ENTITY_RX_INPUT_TERMINAL  0x01 // USB streaming, from the computer
#define UAC_ENTITY_RX_OUTPUT_TERMINAL 0x02 // to the firmware's input
#define UAC_ENTITY_TX_INPUT_TERMINAL  0x11 // the firmware's output to the computer
#define UAC_ENTITY_TX_OUTPUT_TERMINAL 0x12 // USB streaming, to the computer

#ifdef __cplusplus
extern "C" {
#endif
/** Which layout the descriptors give: with the audio function or without. Set while
 *  disconnected, before the computer asks */
void usb_descriptors_audio(bool audio);
#ifdef __cplusplus
}
#endif
