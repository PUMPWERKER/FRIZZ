// Host stand-in for FRIZZ's MidiClock: no clock unless a test sets one, so TempoClock runs on
// its internal 120 BPM. tempo.cpp sets the fields to fake a running clock.
#pragma once
#include "daisy.h"
namespace chompi
{
static const uint32_t kTicksPerBeat = 24;
static const uint32_t kBeatsPerBar = 4;
static const uint32_t kTicksPerBar = kTicksPerBeat * kBeatsPerBar;
enum class ClockFactor : uint8_t
{
    HALF,
    ONE,
    DOUBLE,
};
class MidiClock
{
public:
    inline bool HasClock() const { return has_clock; }
    inline uint32_t GetTicks() const { return ticks; }
    inline uint32_t GetLastTickTime() const { return last_tick_time; }
    inline uint32_t GetLastTick() const { return last_tick_time; }
    inline ClockFactor Factor() const { return factor; }
    inline float GetTickPeriod() const { return tick_period; }
    inline float GetBpm() const { return bpm; }
    inline uint32_t GetLocks() const { return locks; }

    bool has_clock = false;
    uint32_t ticks = 0;
    uint32_t last_tick_time = 0;
    ClockFactor factor = ClockFactor::ONE;
    float tick_period = 0.f;
    float bpm = 0.f;
    uint32_t locks = 0;
};
}
