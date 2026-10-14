/** @file UsbMidiTransport.h
 *  @brief libDaisy's MidiHandler transport on TinyUSB's MIDI class: usb-audio-poc (#196), an
 *  experiment. From lost-tty/CHOMPI's TAPE (MIT, UsbDevice.cpp's MidiTinyUsbTransport, commit
 *  a56545b), apart from UsbDevice.cpp so the host checks it (test/usbtransport.cpp, against a
 *  stand-in of TinyUSB's MIDI functions).
 *
 *  Tx queues from MainLoop (or any context, with interrupts off), Process() in TinyUSB's task
 *  (UsbDevice.cpp, TIM7) reads what came in into MidiHandler's parser, with the audio callback
 *  held off (it empties MidiHandler's queue and preempts the task), and sends the queue.
 *  RxActive() is false until StartRx: MidiHandler::Listen() starts the receive only then (true
 *  from the start, as lost-tty's, left FRIZZ deaf to USB MIDI: no IDENT, no restart).
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include "tusb.h"
#include "util/scopedirqblocker.h"

namespace chompi
{
class MidiTinyUsbTransport
{
public:
    typedef void (*MidiRxParseCallback)(uint8_t* data, size_t size, void* context);

    struct Config
    {
    };

    void Init(Config) {}
    /** Drops what's queued, at the task's next pass */
    void Reset() { drop_ = true; }
    void StartRx(MidiRxParseCallback callback, void* context)
    {
        rx_ctx_ = context;
        rx_cb_ = callback;
        Active() = this;
    }
    /** Whether StartRx was called: MidiHandler::Listen() calls it only while this is false */
    bool RxActive() const { return rx_cb_ != nullptr; }
    void FlushRx() {}

    /** All of it queued, or nothing (false): the queue's full */
    bool Tx(uint8_t* buffer, size_t size)
    {
        daisy::ScopedIrqBlocker block;
        const bool fits = (head_ - tail_) + size <= kTxSize;
        if (fits)
            for (size_t i = 0; i < size; i++)
                tx_[head_++ % kTxSize] = buffer[i];
        Active() = this;
        return fits;
    }
    /** The queue's free bytes */
    size_t Room() const { return kTxSize - (head_ - tail_); }

    /** Whether any MIDI came in over USB since power-on (UsbDevice.cpp's Watch) */
    static bool& Heard()
    {
        static bool heard = false;
        return heard;
    }

    /** From TinyUSB's task: the one transport in use */
    static void Process()
    {
        if (Active())
            Active()->Poll();
    }

    // an answer to a query is 45 bytes at most (MidiControl::Reply)
    static const size_t kTxSize = 256;

private:
    static MidiTinyUsbTransport*& Active()
    {
        static MidiTinyUsbTransport* active = nullptr;
        return active;
    }

    void Poll()
    {
        uint8_t buf[64];
        while (tud_midi_available())
        {
            const uint32_t n = tud_midi_stream_read(buf, sizeof(buf));
            if (n == 0)
                break;
            // the parser fills MidiHandler's queue, which the audio callback empties: it waits
            if (rx_cb_)
            {
                Heard() = true; // and parsed: a transport not listening doesn't count
                daisy::ScopedIrqBlocker block;
                rx_cb_(buf, n, rx_ctx_);
            }
        }

        // reset, or nobody listening: drop the queue
        if (drop_ || !tud_midi_mounted())
        {
            drop_ = false;
            tail_ = head_;
            return;
        }

        while (tail_ != head_)
        {
            const size_t pos = tail_ % kTxSize;
            const size_t count = head_ - tail_;
            const size_t chunk = count < kTxSize - pos ? count : kTxSize - pos;
            const uint32_t sent = tud_midi_stream_write(0, &tx_[pos], static_cast<uint32_t>(chunk));
            tail_ += sent;
            if (sent < chunk)
                break; // the rest next time
        }
    }

    MidiRxParseCallback rx_cb_ = nullptr;
    void* rx_ctx_ = nullptr;
    uint8_t tx_[kTxSize];
    volatile size_t head_ = 0, tail_ = 0; // only Poll() moves the tail
    volatile bool drop_ = false;
};
} // namespace chompi
