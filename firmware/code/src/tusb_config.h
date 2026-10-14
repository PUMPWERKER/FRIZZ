#pragma once

// TinyUSB on FRIZZ's USB port: the OTG_HS core's full speed PHY, rhport 1. From lost-tty/CHOMPI's
// TAPE (MIT, github.com/lost-tty/CHOMPI, firmware/chompi-tape/code/src/tusb_config.h), with
// FRIZZ's channels: 2 each way, no MSC. usb-audio-poc (#196), an experiment.
//
// make USB_AUDIO=0 builds it with MIDI alone (TinyUSB in ST's stack's place, nothing more), the
// step before audio. make UAC1=1 builds the same device with USB Audio Class 1 descriptors (usb_descriptors.c),
// to compare the two on hosts: UAC1's feedback is 10.14 everywhere, UAC2's at full speed is
// 10.14 here (CFG_TUD_AUDIO20_FS_FEEDBACK_10_14, for macOS), which Windows may not take.

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_MCU               OPT_MCU_STM32H7
#define CFG_TUSB_OS                OPT_OS_NONE
#define CFG_TUSB_DEBUG             0

#define CFG_TUD_ENABLED            1
#define CFG_TUD_MAX_SPEED          OPT_MODE_FULL_SPEED
#define FRIZZ_USB_RHPORT           1

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN         __attribute__((aligned(4)))

#define CFG_TUD_ENDPOINT0_SIZE     64

#ifndef FRIZZ_USB_AUDIO
#define FRIZZ_USB_AUDIO            1
#endif
// the card as a USB drive, lent on the settings page (CardLend.h); make USB_MSC=0 leaves it out
#ifndef FRIZZ_USB_MSC
#define FRIZZ_USB_MSC              1
#endif
// UsbDevice.h's Watch: no MIDI from a host for 60 s restarts into the launcher (test builds)
#ifndef FRIZZ_USB_SAFETY
#define FRIZZ_USB_SAFETY           1
#endif
#ifndef FRIZZ_UAC1
#define FRIZZ_UAC1                 0
#endif

#define CFG_TUD_AUDIO              FRIZZ_USB_AUDIO
#define CFG_TUD_MIDI               1
#define CFG_TUD_CDC                0
#define CFG_TUD_MSC                FRIZZ_USB_MSC
#define CFG_TUD_HID                0
#define CFG_TUD_VENDOR             0

#define FRIZZ_USB_SAMPLE_RATE      48000
#define FRIZZ_USB_BYTES_PER_SAMPLE 3 // 24 bit packed
#define FRIZZ_USB_BITS_PER_SAMPLE  24
#define FRIZZ_USB_CHANNELS_RX      2 // host -> FRIZZ: in AUX's place while the host plays
#define FRIZZ_USB_CHANNELS_TX      2 // FRIZZ -> host: the line out (master L/R)

#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX \
    TUD_AUDIO_EP_SIZE(false, FRIZZ_USB_SAMPLE_RATE, FRIZZ_USB_BYTES_PER_SAMPLE, FRIZZ_USB_CHANNELS_RX)
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX \
    TUD_AUDIO_EP_SIZE(false, FRIZZ_USB_SAMPLE_RATE, FRIZZ_USB_BYTES_PER_SAMPLE, FRIZZ_USB_CHANNELS_TX)

// feedback and flow control hold the FIFOs half full: 4 packets, ~2 ms each way
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ (4 * CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX)
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ  (4 * CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX)

#define CFG_TUD_AUDIO_ENABLE_EP_OUT       1
#define CFG_TUD_AUDIO_ENABLE_EP_IN        1
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP  1
#define CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL  1
// UAC2's feedback at full speed as the spec's 3 byte 10.14 (macOS), not TinyUSB's 4 byte 16.16
// (Windows): lost-tty's change in audio_device.c. UAC1's is 10.14 anyway
#define CFG_TUD_AUDIO20_FS_FEEDBACK_10_14 1

#define CFG_TUD_MIDI_RX_BUFSIZE    64
#define CFG_TUD_MIDI_TX_BUFSIZE    64

#define CFG_TUD_MSC_EP_BUFSIZE     4096 // 8 sectors a transfer

#ifdef __cplusplus
}
#endif
