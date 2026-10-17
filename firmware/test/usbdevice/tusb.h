// tusb.h for usbdevice.cpp: TinyUSB as UsbDevice.cpp and UsbMidiTransport.h use it, a computer's
// side in memory: the bus, the audio FIFOs, the drive's answers. The constants are TinyUSB's
// (platform/daisy/libs/tinyusb/src: usbd.h, msc.h, msc_device.h, audio.h)
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

#define TUD_AUDIO_EP_SIZE(_is_highspeed, _maxFrequency, _nBytesPerSample, _nChannels)                   \
    (((((_maxFrequency) + ((_is_highspeed) ? 7999 : 999)) / ((_is_highspeed) ? 8000 : 1000)) + 1)       \
     * (_nBytesPerSample) * (_nChannels))
#include "tusb_config.h"

// ---- the device ----
struct tud_configure_dwc2_t
{
    bool vbus_sensing;
};
#define CFG_TUD_CONFIGURE_DWC2_DEFAULT {true}
#define TUD_CFGID_DWC2 100
enum
{
    TUSB_ROLE_DEVICE = 1
};
enum
{
    TUSB_SPEED_FULL = 0
};
struct tusb_rhport_init_t
{
    int role;
    int speed;
};
struct tusb_control_request_t
{
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
};
inline uint8_t tu_u16_low(uint16_t v) { return static_cast<uint8_t>(v); }
#define TU_U16_HIGH(v) (static_cast<uint8_t>((v) >> 8))

// ---- the drive ----
enum
{
    SCSI_SENSE_NOT_READY = 0x02,
    SCSI_SENSE_ILLEGAL_REQUEST = 0x05,
    SCSI_SENSE_UNIT_ATTENTION = 0x06,
};
enum
{
    TUD_MSC_RET_BUSY = 0,
    TUD_MSC_RET_ERROR = -1,
    TUD_MSC_RET_ASYNC = -2,
};

// ---- audio ----
enum
{
    AUDIO10_CS_REQ_SET_CUR = 0x01,
    AUDIO10_CS_REQ_GET_CUR = 0x81,
};
enum
{
    AUDIO10_EP_CTRL_SAMPLING_FREQ = 0x01,
};
enum
{
    AUDIO_FEEDBACK_METHOD_FIFO_COUNT = 4,
};
struct audio_feedback_params_t
{
    uint8_t method;
    uint32_t sample_freq;
    struct
    {
        uint16_t fifo_threshold;
    } fifo_count;
};
/** The IN FIFO: what FRIZZ wrote, up to depth bytes */
struct tu_fifo_t
{
    std::vector<uint8_t> data;
    size_t depth;
};

namespace tusbstub
{
extern bool connected;           // on the bus: tud_connect, not tud_disconnect since
extern int connects, disconnects;
extern bool configured;          // the computer's: tud_ready, tud_mounted
extern int tasks;                // tud_task's passes
extern bool inited;              // tusb_init
extern std::deque<uint8_t> rx_fifo; // the OUT FIFO: what the computer played, not yet read
extern tu_fifo_t tx_fifo;        // the IN FIFO
extern bool tx_ff;               // tud_audio_get_ep_in_ff gives it (false: null, unconfigured)
extern std::vector<uint8_t> control; // the last control transfer's data
struct Sense
{
    uint8_t key, asc, ascq;
};
extern Sense sense;              // the last tud_msc_set_sense
extern std::vector<int32_t> io_done; // each tud_msc_async_io_done's result
} // namespace tusbstub

void tud_disconnect();
void tud_connect();
void tud_task();
bool tud_ready();
bool tud_mounted();
bool tud_configure(uint8_t rhport, uint32_t cfg_id, const void* cfg);
bool tusb_init(uint8_t rhport, const tusb_rhport_init_t* init);
void tusb_int_handler(uint8_t rhport, bool in_isr);

uint16_t tud_audio_available();
uint16_t tud_audio_n_available(uint8_t func_id);
uint16_t tud_audio_read(void* buffer, uint16_t bufsize);
tu_fifo_t* tud_audio_get_ep_in_ff();
uint16_t tu_fifo_count(const tu_fifo_t* f);
uint16_t tu_fifo_remaining(const tu_fifo_t* f);
uint16_t tud_audio_write(const void* data, uint16_t len);
bool tud_audio_buffer_and_schedule_control_xfer(uint8_t rhport, const tusb_control_request_t* p_request,
                                                void* data, uint16_t len);

void tud_msc_async_io_done(int32_t bytes_io, bool in_isr);
bool tud_msc_set_sense(uint8_t lun, uint8_t sense_key, uint8_t add_sense_code, uint8_t add_sense_qualifier);

uint32_t tud_midi_available();
uint32_t tud_midi_stream_read(void* buffer, uint32_t bufsize);
bool tud_midi_mounted();
uint32_t tud_midi_stream_write(uint8_t cable, const uint8_t* buffer, uint32_t bufsize);
