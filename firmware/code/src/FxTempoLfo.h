/** @file FxTempoLfo.h
 *  @brief An LFO locked to the tempo clock (TempoClock.h): the filter's (FxFilter.h) and the
 *  phaser's while synced (FxPhaser.h).
 */
#pragma once
#include <math.h>
#include <stdint.h>
#include "daisysp.h"
#include "TempoClock.h"

namespace chompi
{

/** Where the clock is as the LFO follows it: the last pulse and the progress towards the next.
 *  It moves smoothly between pulses at the tempo, but waits at the next pulse rather than run
 *  past it, so a late MIDI clock tick doesn't make the phase jump back. In reverse the
 *  position counts down, and the next pulse is the one below */
class TempoLfo
{
public:
    /** At 120 BPM until told; the direction stays as it is */
    void Init(float sample_rate)
    {
        pulse_inc_ = 120.f * kPulsesPerBeat / (60.f * sample_rate);
        pulses_ = 0;
        frac_ = 0.f;
    }

    /** Once per block: the time between two clock pulses in samples (TempoClock::
     *  PulseSamples). The pulses' real rate, not the FX's tempo, which stops at 50 BPM: on a
     *  slowed loop the LFO would otherwise run ahead and wait at every pulse */
    void SetPulseSamples(float samples) { pulse_inc_ = samples > 0.f ? 1.f / samples : 0.f; }
    /** A clock pulse, the clock's position (TempoClock::Pulse); frac: the way already gone
     *  from it towards the next */
    void Pulse(uint32_t pos, bool reverse, float frac)
    {
        pulses_ = pos;
        frac_ = frac;
        reverse_ = reverse;
    }
    inline void SetReverse(bool reverse) { reverse_ = reverse; }
    /** The LFO's advance a sample, in pulses */
    inline float PulseInc() const { return pulse_inc_; }

    /** One sample on */
    inline void Step()
    {
        const float inc = pulse_inc_;
        frac_ += reverse_ ? -inc : inc;
        if (frac_ > .999f)
            frac_ = .999f;
        else if (frac_ < -.999f)
            frac_ = -.999f;
    }
    /** n samples on */
    inline void Advance(uint32_t n)
    {
        const float f = static_cast<float>(n);
        frac_ += (reverse_ ? -f : f) * pulse_inc_;
        frac_ = daisysp::fclamp(frac_, -.999f, .999f);
    }

    /** Where the LFO is in a cycle of div pulses, 0..1 from a cycle's first pulse */
    inline float Cycle(uint32_t div) const
    {
        float pos = static_cast<float>(pulses_ % div) + frac_;
        if (pos < 0.f)
            pos += static_cast<float>(div);
        return pos / static_cast<float>(div);
    }

private:
    float pulse_inc_;      // the LFO's advance a sample, in pulses
    uint32_t pulses_;      // the clock's position
    float frac_;           // progress towards the next pulse, negative in reverse
    bool reverse_ = false;
};

/** A triangle LFO at phase 0..1: -1 at 0 and 1, +1 halfway */
inline float LfoTriangle(float phase) { return 1.f - 4.f * fabsf(phase - .5f); }

} // namespace chompi
