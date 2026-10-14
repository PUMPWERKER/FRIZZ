/** @file UsbDevice.h
 *  @brief FRIZZ's USB port on TinyUSB: audio both ways and MIDI. usb-audio-poc (#196), an
 *  experiment, not merged.
 *
 *  From lost-tty/CHOMPI's TAPE (MIT, github.com/lost-tty/CHOMPI, firmware/chompi-tape/code/src/
 *  UsbDevice.h/.cpp, commit a56545b), which turned TAPE into a USB audio interface; FRIZZ keeps
 *  its names and VID:PID (usb_descriptors.c), has 2 channels each way and no MSC.
 *
 *  - Audio, 48 kHz / 24 bit (tusb_config.h): the host plays into FRIZZ in AUX's place
 *    (Receive, chompi_main.cpp), FRIZZ sends its line out to the host (Send). The host follows
 *    FRIZZ's clock both ways: an OUT FIFO held half full by the feedback endpoint (FIFO count),
 *    IN packets of n-1, n or n+1 frames by the IN FIFO's fill. CRS trims the USB clock (HSI48)
 *    to the host's start of frame.
 *  - MIDI (MidiTinyUsbTransport): libDaisy's MidiHandler on TinyUSB. What comes in is read and
 *    parsed in TinyUSB's task, what goes out queued from MainLoop and sent from the task.
 *  - TinyUSB's task runs from TIM7 at 4 kHz, priority 2: below the USB interrupt and the
 *    audio callback (0, which never preempt each other), above MainLoop. Receive() and Send()
 *    run in the audio callback, against TinyUSB's FIFOs, which the USB interrupt fills and
 *    empties at the same priority.
 *
 *  ST's USB stack (libDaisy's UsbHandle, MidiUsbTransport, the CDC class) is gone from the
 *  image: libDaisy's system.cpp no longer has the OTG_HS interrupt handlers (THIRD_PARTY.md).
 *  Only on the device: the virtual CHOMPI keeps its own USB (twin/host/hid/usb_midi.h).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include "tusb_config.h"

namespace chompi
{
namespace usb
{
static constexpr size_t kRxChannels = FRIZZ_USB_CHANNELS_RX;
static constexpr size_t kTxChannels = FRIZZ_USB_CHANNELS_TX;
static constexpr size_t kMaxBlock = 48; // Receive() holds one

/** Once, after the USB lines are back from the charger (chompi_main.cpp): the host meets
 *  FRIZZ from the task's first pass */
void Init();

/** A host has FRIZZ configured and isn't suspending it: a cable to a computer that knows it */
bool Host();

#if FRIZZ_USB_AUDIO
/** The host plays to FRIZZ */
bool RxStreaming();

/** The host's audio for this block, kRxChannels; fades in when the stream starts, out when it
 *  stops or runs dry. From the audio callback */
const float* const* Receive(size_t size);

/** kTxChannels to the host, null ones as silence; dropped while it doesn't record. From the
 *  audio callback */
void Send(const float* const* to_host, size_t size);

/** Counts for the PoC's measurements, since boot */
struct Stats
{
    uint32_t rx_blocks;  // blocks taken from the host
    uint32_t rx_dry;     // the host's stream ran dry: faded out, waited to prime again
    uint32_t rx_starts;  // the host started playing
    uint32_t tx_blocks;  // blocks sent
    uint32_t tx_full;    // blocks dropped, the IN FIFO full
    uint16_t rx_fill;    // the OUT FIFO's bytes at the last block
    uint16_t tx_fill;    // the IN FIFO's
};
const Stats& GetStats();
#endif

/** A function TinyUSB's task calls every pass, before it sends MIDI: MIDI out's USB bytes
 *  (MidiOut::FlushUsb), so a MainLoop held up by the card doesn't hold them up (#265) */
void SetTaskHook(void (*hook)());

/** From MainLoop: an enumeration that never finishes (the host sends frames for 10 s but
 *  never configures FRIZZ) restarts into the launcher, whose USB the tools can reach. A PoC's
 *  way back without the power switch */
void Watch(uint32_t now_ms);
} // namespace usb

/** libDaisy's MidiHandler transport on TinyUSB: Tx queues from MainLoop (or any context),
 *  Process() in the task sends and receives */
class MidiTinyUsbTransport
{
public:
    typedef void (*MidiRxParseCallback)(uint8_t* data, size_t size, void* context);

    struct Config
    {
    };

    void Init(Config) {}
    void Reset();
    void StartRx(MidiRxParseCallback callback, void* context);
    bool RxActive() { return true; }
    void FlushRx() {}
    /** All of it queued, or nothing (false): the queue's full, or no host */
    bool Tx(uint8_t* buffer, size_t size);
    /** The queue's free bytes */
    size_t Room() const;

    static void Process();
};
} // namespace chompi
