#pragma once

// TinyUSB on the USB port (UsbDevice.h): the OTG_HS core on its full speed PHY, rhport 1, as
// a USB Audio Class 1 sound card (48 kHz / 24 bit, 2 channels each way), USB MIDI and the card
// as a drive (MSC). After TinyUSB's examples uac2_speaker_fb and uac2_headset (their UAC1
// configurations), midi_test and cdc_msc; the shape of lost-tty/CHOMPI's TAPE (MIT, its
// tusb_config.h).

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_MCU               OPT_MCU_STM32H7
#define CFG_TUSB_OS                OPT_OS_NONE
// a debug level above 0 prints from the USB interrupt and costs the streams their timing
#define CFG_TUSB_DEBUG             0

#define CFG_TUD_ENABLED            1
#define CFG_TUD_MAX_SPEED          OPT_MODE_FULL_SPEED
#define PLATFORM_USB_RHPORT        1

// no DMA (the core's FIFOs are filled by the CPU), so no section and the usual alignment
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN         __attribute__((aligned(4)))

#define CFG_TUD_ENDPOINT0_SIZE     64

#define CFG_TUD_AUDIO              1 // its interfaces only while USB audio is on (usb_descriptors.c)
#define CFG_TUD_MIDI               1
#define CFG_TUD_MSC                1
#define CFG_TUD_CDC                0
#define CFG_TUD_HID                0
#define CFG_TUD_VENDOR             0

// ---- audio ----
#define PLATFORM_USB_SAMPLE_RATE      48000
#define PLATFORM_USB_BYTES_PER_SAMPLE 3 // 24 bit, packed
#define PLATFORM_USB_BITS_PER_SAMPLE  24
#define PLATFORM_USB_CHANNELS_RX      2 // the computer -> the device: an input (FRIZZ's source USB)
#define PLATFORM_USB_CHANNELS_TX      2 // the device -> the computer (FRIZZ's master before VOLUME)

#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX \
    TUD_AUDIO_EP_SIZE(false, PLATFORM_USB_SAMPLE_RATE, PLATFORM_USB_BYTES_PER_SAMPLE, \
                      PLATFORM_USB_CHANNELS_RX)
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX \
    TUD_AUDIO_EP_SIZE(false, PLATFORM_USB_SAMPLE_RATE, PLATFORM_USB_BYTES_PER_SAMPLE, \
                      PLATFORM_USB_CHANNELS_TX)

// 4 packets each way, held about half full: the feedback (FIFO count, which needs 4 at least,
// as uac2_speaker_fb says) and the IN flow control keep them there, ~2 ms each way
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ (4 * CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX)
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ  (4 * CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX)

#define CFG_TUD_AUDIO_ENABLE_EP_OUT       1
#define CFG_TUD_AUDIO_ENABLE_EP_IN        1
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP  1 // UAC1's 3 byte 10.14, on every host
#define CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL  1 // IN packets of 47, 48 or 49 frames by the FIFO's fill

// ---- MIDI ----
#define CFG_TUD_MIDI_RX_BUFSIZE    64
#define CFG_TUD_MIDI_TX_BUFSIZE    64

// ---- the card as a drive ----
#define CFG_TUD_MSC_EP_BUFSIZE     4096 // 8 sectors a transfer

#ifdef __cplusplus
}
#endif
