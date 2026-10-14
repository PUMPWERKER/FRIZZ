// tusb.h for usbtransport.cpp: TinyUSB's MIDI class as UsbMidiTransport.h uses it, a host's
// side in memory: what it sent waits to be read, what FRIZZ writes is kept
#pragma once
#include <cstdint>
#include <vector>
namespace tusbstub
{
extern std::vector<uint8_t> from_host; // bytes the host sent, not yet read
extern std::vector<uint8_t> to_host;   // bytes FRIZZ wrote
extern bool mounted;
extern uint32_t room; // what one write takes at most, as TinyUSB's FIFO
} // namespace tusbstub
uint32_t tud_midi_available();
uint32_t tud_midi_stream_read(void* buffer, uint32_t bufsize);
bool tud_midi_mounted();
uint32_t tud_midi_stream_write(uint8_t cable, const uint8_t* buffer, uint32_t bufsize);
