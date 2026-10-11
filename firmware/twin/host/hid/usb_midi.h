// Host stand-in for libDaisy's MidiUsbTransport: MIDI in and out over the twin's USB
// (board.h), as raw MIDI bytes, the USB packets' framing left out
#pragma once
#include "daisy_core.h"
#include "board.h"

namespace daisy
{
class MidiUsbTransport
{
public:
    typedef void (*MidiRxParseCallback)(uint8_t* data, size_t size, void* context);

    struct Config
    {
        enum Periph
        {
            INTERNAL = 0,
            EXTERNAL
        };
        Periph periph;
        uint8_t tx_retry_count;
        Config() : periph(INTERNAL), tx_retry_count(3) {}
    };

    void Init(Config) {}
    void Reset() {}
    void StartRx(MidiRxParseCallback cb, void* context)
    {
        active_ = true;
        twin::UsbListen(cb, context);
    }
    bool RxActive() { return active_; }
    void FlushRx() {}
    /** A host has it configured, and the last transfer is done (no transfer waits here) */
    bool Ready() const { return twin::UsbHost(); }
    bool Tx(uint8_t* data, size_t size)
    {
        twin::UsbTx(data, size);
        return true;
    }

private:
    bool active_ = false;
};
} // namespace daisy
