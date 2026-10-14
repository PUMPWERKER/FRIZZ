// FRIZZ's USB device: one audio function (48 kHz / 24 bit, 2 channels each way, asynchronous
// with a feedback endpoint) and one MIDI function. From lost-tty/CHOMPI's TAPE (MIT,
// github.com/lost-tty/CHOMPI, firmware/chompi-tape/code/src/usb_descriptors.c), with FRIZZ's
// names, a UAC1 variant and the card as a USB drive (MSC, CardLend.h). usb-audio-poc (#196), an experiment.
//
// The names stay libDaisy's (usbd_desc.c): maker "Chompi Club", product "FrizZ", the serial
// in ST's format, the Daisy's VID:PID 0483:5740. The tools find FRIZZ by them (tools/chompi.py,
// midi_send.py: "chompi" in the card's names, the USB id; reaper.py's hw:FrizZ).

#include "tusb.h"
#include "usb_descriptors.h"

#define USB_VID 0x0483
#define USB_PID 0x5740
// ST's stack had 0x0200; each layout of interfaces its own, so hosts that cache by it re-read them
#if !FRIZZ_USB_AUDIO
#define USB_BCD_DEVICE (0x0300 + FRIZZ_USB_MSC)
#elif FRIZZ_UAC1
#define USB_BCD_DEVICE (0x0310 + FRIZZ_USB_MSC)
#else
#define USB_BCD_DEVICE (0x0320 + FRIZZ_USB_MSC)
#endif

#define EPNUM_AUDIO_RX 0x01
#define EPNUM_AUDIO_FB 0x81
#define EPNUM_AUDIO_TX 0x82
#define EPNUM_MIDI_OUT 0x03
#define EPNUM_MIDI_IN  0x83
#define EPNUM_MSC_OUT  0x04
#define EPNUM_MSC_IN   0x84

#if FRIZZ_USB_MSC
#define MSC_DESC_LEN TUD_MSC_DESC_LEN
#define MSC_DESC TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, STRID_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
#else
#define MSC_DESC_LEN 0
#define MSC_DESC
#endif

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

#define TERM_TYPE_LINE_CONNECTOR 0x0603

static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
#if FRIZZ_USB_AUDIO && !FRIZZ_UAC1
    // UAC2's function and MIDI each behind an IAD
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
#else
    // UAC1 and MIDI: the interfaces say, grouped by their audio control headers
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
#endif
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = USB_BCD_DEVICE,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

#if FRIZZ_USB_AUDIO
#define RX_EP_SIZE CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX
#define TX_EP_SIZE CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX
#define ISO_ASYNC_DATA \
    (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_ASYNCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_DATA)
#endif

#if FRIZZ_USB_AUDIO && !FRIZZ_UAC1
// ---- UAC2 ---------------------------------------------------------------------------------

#define AC_ENTITIES_LEN \
    (TUD_AUDIO20_DESC_CLK_SRC_LEN + 2 * TUD_AUDIO20_DESC_INPUT_TERM_LEN + 2 * TUD_AUDIO20_DESC_OUTPUT_TERM_LEN)

#define AUDIO_DESC_LEN                                                                            \
    (TUD_AUDIO20_DESC_IAD_LEN + TUD_AUDIO20_DESC_STD_AC_LEN + TUD_AUDIO20_DESC_CS_AC_LEN + AC_ENTITIES_LEN \
     /* RX alt 0, alt 1 */                                                                        \
     + TUD_AUDIO20_DESC_STD_AS_LEN + TUD_AUDIO20_DESC_STD_AS_LEN + TUD_AUDIO20_DESC_CS_AS_INT_LEN \
     + TUD_AUDIO20_DESC_TYPE_I_FORMAT_LEN + TUD_AUDIO20_DESC_STD_AS_ISO_EP_LEN                   \
     + TUD_AUDIO20_DESC_CS_AS_ISO_EP_LEN + TUD_AUDIO20_DESC_STD_AS_ISO_FB_EP_LEN                 \
     /* TX alt 0, alt 1 */                                                                        \
     + TUD_AUDIO20_DESC_STD_AS_LEN + TUD_AUDIO20_DESC_STD_AS_LEN + TUD_AUDIO20_DESC_CS_AS_INT_LEN \
     + TUD_AUDIO20_DESC_TYPE_I_FORMAT_LEN + TUD_AUDIO20_DESC_STD_AS_ISO_EP_LEN                   \
     + TUD_AUDIO20_DESC_CS_AS_ISO_EP_LEN)

#define MIDI_IAD_LEN 8
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + AUDIO_DESC_LEN + MIDI_IAD_LEN + TUD_MIDI_DESC_LEN + MSC_DESC_LEN)

#define STEREO (AUDIO20_CHANNEL_CONFIG_FRONT_LEFT | AUDIO20_CHANNEL_CONFIG_FRONT_RIGHT)

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    TUD_AUDIO20_DESC_IAD(ITF_NUM_AUDIO_CONTROL, 3, 0x00),
    TUD_AUDIO20_DESC_STD_AC(ITF_NUM_AUDIO_CONTROL, 0x00, STRID_AUDIO),
    TUD_AUDIO20_DESC_CS_AC(0x0200, AUDIO20_FUNC_IO_BOX, AC_ENTITIES_LEN, 0x00),
    TUD_AUDIO20_DESC_CLK_SRC(UAC_ENTITY_CLOCK, AUDIO20_CLOCK_SOURCE_ATT_INT_FIX_CLK,
                             (AUDIO20_CTRL_R << AUDIO20_CLOCK_SOURCE_CTRL_CLK_FRQ_POS)
                                 | (AUDIO20_CTRL_R << AUDIO20_CLOCK_SOURCE_CTRL_CLK_VAL_POS),
                             0x00, 0x00),
    // host -> FRIZZ
    TUD_AUDIO20_DESC_INPUT_TERM(UAC_ENTITY_RX_INPUT_TERMINAL, AUDIO_TERM_TYPE_USB_STREAMING, 0x00,
                                UAC_ENTITY_CLOCK, FRIZZ_USB_CHANNELS_RX, STEREO, 0x00, 0x0000, 0x00),
    TUD_AUDIO20_DESC_OUTPUT_TERM(UAC_ENTITY_RX_OUTPUT_TERMINAL, TERM_TYPE_LINE_CONNECTOR, 0x00,
                                 UAC_ENTITY_RX_INPUT_TERMINAL, UAC_ENTITY_CLOCK, 0x0000, 0x00),
    // FRIZZ -> host
    TUD_AUDIO20_DESC_INPUT_TERM(UAC_ENTITY_TX_INPUT_TERMINAL, TERM_TYPE_LINE_CONNECTOR, 0x00,
                                UAC_ENTITY_CLOCK, FRIZZ_USB_CHANNELS_TX, STEREO, 0x00, 0x0000, 0x00),
    TUD_AUDIO20_DESC_OUTPUT_TERM(UAC_ENTITY_TX_OUTPUT_TERMINAL, AUDIO_TERM_TYPE_USB_STREAMING, 0x00,
                                 UAC_ENTITY_TX_INPUT_TERMINAL, UAC_ENTITY_CLOCK, 0x0000, 0x00),

    // host -> FRIZZ streaming, alt 0 is zero bandwidth
    TUD_AUDIO20_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_RX, 0x00, 0x00, 0x00),
    TUD_AUDIO20_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_RX, 0x01, 0x02, 0x00),
    TUD_AUDIO20_DESC_CS_AS_INT(UAC_ENTITY_RX_INPUT_TERMINAL, AUDIO20_CTRL_NONE, AUDIO20_FORMAT_TYPE_I,
                               AUDIO20_DATA_FORMAT_TYPE_I_PCM, FRIZZ_USB_CHANNELS_RX, STEREO, 0x00),
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(FRIZZ_USB_BYTES_PER_SAMPLE, FRIZZ_USB_BITS_PER_SAMPLE),
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_RX, ISO_ASYNC_DATA, RX_EP_SIZE, 0x01),
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO20_CTRL_NONE,
                                  AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0x0000),
    TUD_AUDIO20_DESC_STD_AS_ISO_FB_EP(EPNUM_AUDIO_FB, 3, 0x01), // full speed: 3 byte 10.14

    // FRIZZ -> host streaming, alt 0 is zero bandwidth
    TUD_AUDIO20_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_TX, 0x00, 0x00, 0x00),
    TUD_AUDIO20_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_TX, 0x01, 0x01, 0x00),
    TUD_AUDIO20_DESC_CS_AS_INT(UAC_ENTITY_TX_OUTPUT_TERMINAL, AUDIO20_CTRL_NONE, AUDIO20_FORMAT_TYPE_I,
                               AUDIO20_DATA_FORMAT_TYPE_I_PCM, FRIZZ_USB_CHANNELS_TX, STEREO, 0x00),
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(FRIZZ_USB_BYTES_PER_SAMPLE, FRIZZ_USB_BITS_PER_SAMPLE),
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_TX, ISO_ASYNC_DATA, TX_EP_SIZE, 0x01),
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO20_CTRL_NONE,
                                  AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0x0000),

    // MIDI behind its own IAD, so Windows' composite driver keeps its interfaces together
    MIDI_IAD_LEN, TUSB_DESC_INTERFACE_ASSOCIATION, ITF_NUM_MIDI, 2, TUSB_CLASS_AUDIO,
    AUDIO_FUNCTION_SUBCLASS_UNDEFINED, AUDIO_FUNC_PROTOCOL_CODE_UNDEF, 0x00,
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 0x00, EPNUM_MIDI_OUT, EPNUM_MIDI_IN, 64),
    MSC_DESC
};

#elif FRIZZ_USB_AUDIO
// ---- UAC1 ---------------------------------------------------------------------------------
// One rate, so no sampling frequency control: hosts send no requests for it

#define AC_ENTITIES_LEN (2 * TUD_AUDIO10_DESC_INPUT_TERM_LEN + 2 * TUD_AUDIO10_DESC_OUTPUT_TERM_LEN)

#define AUDIO_DESC_LEN                                                                            \
    (TUD_AUDIO10_DESC_STD_AC_LEN + TUD_AUDIO10_DESC_CS_AC_LEN(2) + AC_ENTITIES_LEN               \
     /* RX alt 0, alt 1 */                                                                        \
     + 2 * TUD_AUDIO10_DESC_STD_AS_LEN + TUD_AUDIO10_DESC_CS_AS_INT_LEN                          \
     + TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(1) + TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN                \
     + TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN + TUD_AUDIO10_DESC_STD_AS_ISO_SYNC_EP_LEN               \
     /* TX alt 0, alt 1 */                                                                        \
     + 2 * TUD_AUDIO10_DESC_STD_AS_LEN + TUD_AUDIO10_DESC_CS_AS_INT_LEN                          \
     + TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(1) + TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN                \
     + TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN)

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + AUDIO_DESC_LEN + TUD_MIDI_DESC_LEN + MSC_DESC_LEN)

#define STEREO (AUDIO10_CHANNEL_CONFIG_LEFT_FRONT | AUDIO10_CHANNEL_CONFIG_RIGHT_FRONT)

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    TUD_AUDIO10_DESC_STD_AC(ITF_NUM_AUDIO_CONTROL, 0x00, STRID_AUDIO),
    TUD_AUDIO10_DESC_CS_AC(0x0100, AC_ENTITIES_LEN, ITF_NUM_AUDIO_STREAMING_RX, ITF_NUM_AUDIO_STREAMING_TX),
    // host -> FRIZZ
    TUD_AUDIO10_DESC_INPUT_TERM(UAC_ENTITY_RX_INPUT_TERMINAL, AUDIO_TERM_TYPE_USB_STREAMING, 0x00,
                                FRIZZ_USB_CHANNELS_RX, STEREO, 0x00, 0x00),
    TUD_AUDIO10_DESC_OUTPUT_TERM(UAC_ENTITY_RX_OUTPUT_TERMINAL, TERM_TYPE_LINE_CONNECTOR, 0x00,
                                 UAC_ENTITY_RX_INPUT_TERMINAL, 0x00),
    // FRIZZ -> host
    TUD_AUDIO10_DESC_INPUT_TERM(UAC_ENTITY_TX_INPUT_TERMINAL, TERM_TYPE_LINE_CONNECTOR, 0x00,
                                FRIZZ_USB_CHANNELS_TX, STEREO, 0x00, 0x00),
    TUD_AUDIO10_DESC_OUTPUT_TERM(UAC_ENTITY_TX_OUTPUT_TERMINAL, AUDIO_TERM_TYPE_USB_STREAMING, 0x00,
                                 UAC_ENTITY_TX_INPUT_TERMINAL, 0x00),

    // host -> FRIZZ streaming, alt 0 is zero bandwidth
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_RX, 0x00, 0x00, 0x00),
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_RX, 0x01, 0x02, 0x00),
    TUD_AUDIO10_DESC_CS_AS_INT(UAC_ENTITY_RX_INPUT_TERMINAL, 0x00, AUDIO10_DATA_FORMAT_TYPE_I_PCM),
    TUD_AUDIO10_DESC_TYPE_I_FORMAT(FRIZZ_USB_CHANNELS_RX, FRIZZ_USB_BYTES_PER_SAMPLE,
                                   FRIZZ_USB_BITS_PER_SAMPLE, FRIZZ_USB_SAMPLE_RATE),
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_RX,
                                   (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_ASYNCHRONOUS),
                                   RX_EP_SIZE, 0x01, EPNUM_AUDIO_FB),
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(AUDIO10_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK,
                                  AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0x0000),
    // TinyUSB's UAC1 example has bRefresh 0 here
    TUD_AUDIO10_DESC_STD_AS_ISO_SYNC_EP(EPNUM_AUDIO_FB, 0),

    // FRIZZ -> host streaming, alt 0 is zero bandwidth
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_TX, 0x00, 0x00, 0x00),
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_TX, 0x01, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_INT(UAC_ENTITY_TX_OUTPUT_TERMINAL, 0x00, AUDIO10_DATA_FORMAT_TYPE_I_PCM),
    TUD_AUDIO10_DESC_TYPE_I_FORMAT(FRIZZ_USB_CHANNELS_TX, FRIZZ_USB_BYTES_PER_SAMPLE,
                                   FRIZZ_USB_BITS_PER_SAMPLE, FRIZZ_USB_SAMPLE_RATE),
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_TX,
                                   (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_ASYNCHRONOUS),
                                   TX_EP_SIZE, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(AUDIO10_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK,
                                  AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0x0000),

    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 0x00, EPNUM_MIDI_OUT, EPNUM_MIDI_IN, 64),
    MSC_DESC
};

#else
// ---- MIDI alone ---------------------------------------------------------------------------

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MIDI_DESC_LEN + MSC_DESC_LEN)

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 0x00, EPNUM_MIDI_OUT, EPNUM_MIDI_IN, 64),
    MSC_DESC
};
#endif

TU_VERIFY_STATIC(sizeof(desc_configuration) == CONFIG_TOTAL_LEN, "Incorrect size");

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

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
        // ST's format (usbd_desc.c's Get_SerialNum), so the host sees the same device: 8 hex
        // digits of uid0 + uid2, the top 4 of uid1
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
