// FRIZZ's USB device: a USB Audio Class 1 sound card (48 kHz / 24 bit, 2 channels each way,
// asynchronous, the computer's stream held by a feedback endpoint), USB MIDI and the card as a
// drive (MSC). With USB audio off (SHIFT + the upper F# on the settings page, UsbDevice.h) only
// MIDI and the drive: the layout is chosen before FRIZZ connects, so each comes up as a device
// of its own (bcdDevice).
//
// The audio function is TinyUSB's examples' UAC1 configurations put together: uac2_headset's
// (both directions, each an input and an output terminal, no feature unit) with
// uac2_speaker_fb's asynchronous OUT endpoint and its feedback endpoint (bRefresh 0, which that
// example has tested on Windows, macOS and Linux). MIDI as midi_test's, the drive as cdc_msc's.
// The way these sit together is lost-tty/CHOMPI's TAPE (MIT, usb_descriptors.c).
//
// The names stay what ST's stack gave (libDaisy's usbd_desc.c): maker "Chompi Club", product
// "FrizZ", the serial in ST's format, the Daisy's VID:PID 0483:5740. The tools find FRIZZ by
// them (tools/chompi.py, midi_send.py: "chompi" in the card's names, the USB id; reaper.py's
// hw:FrizZ).

#include "tusb.h"
#include "usb_descriptors.h"

#define USB_VID 0x0483
#define USB_PID 0x5740
// ST's stack had 0x0200: each layout its own, as hosts cache a device's interfaces by it
#define USB_BCD_AUDIO 0x0311
#define USB_BCD_MIDI  0x0301

#define EPNUM_AUDIO_RX 0x01
#define EPNUM_AUDIO_FB 0x81
#define EPNUM_AUDIO_TX 0x82
#define EPNUM_MIDI_OUT 0x03
#define EPNUM_MIDI_IN  0x83
#define EPNUM_MSC_OUT  0x04
#define EPNUM_MSC_IN   0x84

enum
{
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_AUDIO,
    STRID_MSC,
    STRID_COUNT
};

static bool with_audio = true;

void usb_descriptors_audio(bool audio) { with_audio = audio; }

static tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    // UAC1 and MIDI: the interfaces say what they are, each function by its audio control
    // interface (UAC1 has no interface association)
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = USB_BCD_AUDIO,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void)
{
    desc_device.bcdDevice = with_audio ? USB_BCD_AUDIO : USB_BCD_MIDI;
    return (uint8_t const *)&desc_device;
}

// ---- with USB audio ---------------------------------------------------------------------

#define AC_ENTITIES_LEN (2 * TUD_AUDIO10_DESC_INPUT_TERM_LEN + 2 * TUD_AUDIO10_DESC_OUTPUT_TERM_LEN)

#define AUDIO_DESC_LEN                                                                            \
    (TUD_AUDIO10_DESC_STD_AC_LEN + TUD_AUDIO10_DESC_CS_AC_LEN(2) + AC_ENTITIES_LEN               \
     /* the computer -> FRIZZ, alt 0 and alt 1 */                                                 \
     + 2 * TUD_AUDIO10_DESC_STD_AS_LEN + TUD_AUDIO10_DESC_CS_AS_INT_LEN                          \
     + TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(1) + TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN                \
     + TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN + TUD_AUDIO10_DESC_STD_AS_ISO_SYNC_EP_LEN               \
     /* FRIZZ -> the computer, alt 0 and alt 1 */                                                 \
     + 2 * TUD_AUDIO10_DESC_STD_AS_LEN + TUD_AUDIO10_DESC_CS_AS_INT_LEN                          \
     + TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(1) + TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN                \
     + TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN)

#define CONFIG_AUDIO_LEN (TUD_CONFIG_DESC_LEN + AUDIO_DESC_LEN + TUD_MIDI_DESC_LEN + TUD_MSC_DESC_LEN)

#define STEREO (AUDIO10_CHANNEL_CONFIG_LEFT_FRONT | AUDIO10_CHANNEL_CONFIG_RIGHT_FRONT)
#define TERM_TYPE_LINE_CONNECTOR 0x0603 // an external line connector: FRIZZ's own audio
#define ISO_ASYNC (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_ASYNCHRONOUS)

static uint8_t const desc_config_audio[] = {
    // bus powered, 100 mA: what a tablet's port gives (ST's stack asked for 160 mA)
    TUD_CONFIG_DESCRIPTOR(1, ITF_AUDIO_TOTAL, 0, CONFIG_AUDIO_LEN, 0x00, 100),

    TUD_AUDIO10_DESC_STD_AC(ITF_AUDIO_CONTROL, 0x00, STRID_AUDIO),
    TUD_AUDIO10_DESC_CS_AC(0x0100, AC_ENTITIES_LEN, ITF_AUDIO_STREAMING_RX, ITF_AUDIO_STREAMING_TX),
    // the computer -> FRIZZ
    TUD_AUDIO10_DESC_INPUT_TERM(UAC_ENTITY_RX_INPUT_TERMINAL, AUDIO_TERM_TYPE_USB_STREAMING, 0x00,
                                FRIZZ_USB_CHANNELS_RX, STEREO, 0x00, 0x00),
    TUD_AUDIO10_DESC_OUTPUT_TERM(UAC_ENTITY_RX_OUTPUT_TERMINAL, TERM_TYPE_LINE_CONNECTOR, 0x00,
                                 UAC_ENTITY_RX_INPUT_TERMINAL, 0x00),
    // FRIZZ -> the computer
    TUD_AUDIO10_DESC_INPUT_TERM(UAC_ENTITY_TX_INPUT_TERMINAL, TERM_TYPE_LINE_CONNECTOR, 0x00,
                                FRIZZ_USB_CHANNELS_TX, STEREO, 0x00, 0x00),
    TUD_AUDIO10_DESC_OUTPUT_TERM(UAC_ENTITY_TX_OUTPUT_TERMINAL, AUDIO_TERM_TYPE_USB_STREAMING, 0x00,
                                 UAC_ENTITY_TX_INPUT_TERMINAL, 0x00),

    // the computer -> FRIZZ: alt 0 without bandwidth, alt 1 streaming, asynchronous with feedback
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_AUDIO_STREAMING_RX, 0x00, 0x00, 0x00),
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_AUDIO_STREAMING_RX, 0x01, 0x02, 0x00),
    TUD_AUDIO10_DESC_CS_AS_INT(UAC_ENTITY_RX_INPUT_TERMINAL, 0x01, AUDIO10_DATA_FORMAT_TYPE_I_PCM),
    TUD_AUDIO10_DESC_TYPE_I_FORMAT(FRIZZ_USB_CHANNELS_RX, FRIZZ_USB_BYTES_PER_SAMPLE,
                                   FRIZZ_USB_BITS_PER_SAMPLE, FRIZZ_USB_SAMPLE_RATE),
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_RX, ISO_ASYNC, CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX, 0x01,
                                   EPNUM_AUDIO_FB),
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ,
                                  AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0x0000),
    TUD_AUDIO10_DESC_STD_AS_ISO_SYNC_EP(EPNUM_AUDIO_FB, 0),

    // FRIZZ -> the computer: alt 0 without bandwidth, alt 1 streaming, asynchronous
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_AUDIO_STREAMING_TX, 0x00, 0x00, 0x00),
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_AUDIO_STREAMING_TX, 0x01, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_INT(UAC_ENTITY_TX_OUTPUT_TERMINAL, 0x01, AUDIO10_DATA_FORMAT_TYPE_I_PCM),
    TUD_AUDIO10_DESC_TYPE_I_FORMAT(FRIZZ_USB_CHANNELS_TX, FRIZZ_USB_BYTES_PER_SAMPLE,
                                   FRIZZ_USB_BITS_PER_SAMPLE, FRIZZ_USB_SAMPLE_RATE),
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_TX, ISO_ASYNC, CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ,
                                  AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0x0000),

    TUD_MIDI_DESCRIPTOR(ITF_AUDIO_MIDI, 0x00, EPNUM_MIDI_OUT, EPNUM_MIDI_IN, 64),
    TUD_MSC_DESCRIPTOR(ITF_AUDIO_MSC, STRID_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

TU_VERIFY_STATIC(sizeof(desc_config_audio) == CONFIG_AUDIO_LEN, "the audio layout's length");

// ---- without ----------------------------------------------------------------------------

#define CONFIG_MIDI_LEN (TUD_CONFIG_DESC_LEN + TUD_MIDI_DESC_LEN + TUD_MSC_DESC_LEN)

static uint8_t const desc_config_midi[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_MIDI_TOTAL, 0, CONFIG_MIDI_LEN, 0x00, 100),
    TUD_MIDI_DESCRIPTOR(ITF_MIDI, 0x00, EPNUM_MIDI_OUT, EPNUM_MIDI_IN, 64),
    TUD_MSC_DESCRIPTOR(ITF_MIDI_MSC, STRID_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

TU_VERIFY_STATIC(sizeof(desc_config_midi) == CONFIG_MIDI_LEN, "the MIDI layout's length");

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return with_audio ? desc_config_audio : desc_config_midi;
}

// ---- strings ----------------------------------------------------------------------------

static char const *const string_desc[] = {
    [STRID_LANGID]       = NULL,
    [STRID_MANUFACTURER] = "Chompi Club",
    [STRID_PRODUCT]      = "FrizZ",
    [STRID_SERIAL]       = NULL, // from the chip's unique ID
    [STRID_AUDIO]        = "FrizZ",
    [STRID_MSC]          = "FrizZ card",
};

TU_VERIFY_STATIC(sizeof(string_desc) / sizeof(string_desc[0]) == STRID_COUNT, "a string per ID");

static uint16_t desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    size_t chr_count;

    if(index == STRID_LANGID)
    {
        desc_str[1] = 0x0409;
        chr_count   = 1;
    }
    else if(index == STRID_SERIAL)
    {
        // ST's format (libDaisy's usbd_desc.c, Get_SerialNum), so the computer knows FRIZZ as it
        // did: 8 hex digits of uid0 + uid2, then the top 4 of uid1
        uint32_t const *uid = (uint32_t const *)UID_BASE;
        uint32_t const  hi  = uid[0] + uid[2];
        chr_count           = 0;
        for(int n = 7; n >= 0; n--)
            desc_str[1 + chr_count++] = "0123456789ABCDEF"[(hi >> (4 * n)) & 0xF];
        for(int n = 7; n >= 4; n--)
            desc_str[1 + chr_count++] = "0123456789ABCDEF"[(uid[1] >> (4 * n)) & 0xF];
    }
    else
    {
        if(index >= STRID_COUNT || string_desc[index] == NULL)
            return NULL;

        char const *str = string_desc[index];
        for(chr_count = 0; str[chr_count] && chr_count < 32; chr_count++)
            desc_str[1 + chr_count] = str[chr_count];
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc_str;
}
