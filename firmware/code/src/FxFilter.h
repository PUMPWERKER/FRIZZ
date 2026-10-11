/** @file FxFilter.h
 *  @brief The DJ filter with a tempo-synced LFO.
 */
#pragma once
#include "FxCommon.h"
#include "DJFilter.h"
#include "TempoClock.h"

namespace chompi
{

// The filter LFO's cycle in 12 PPQN pulses: 1/16, 1/8, 1/4, 1/2, 1 bar, 2 bars, 4 bars.
// Each divides kPulsesPerCycle, so the LFO stays on the beat grid whichever is picked.
static const uint32_t kLfoDivisionPulses[] = {3, 6, 12, 24, 48, 96, 192};
static_assert(kPulsesPerCycle % 192 == 0, "the longest LFO cycle must divide the clock's");

/** The DJ filter shared by TAPE, TEMPO and WAVE (DJFilter.h, WAVE's copy): lowpass below the
 *  centre of the cutoff knob, highpass above, flat in the middle. Plus a triangle LFO on the
 *  cutoff, like WAVE's filter LFO but locked to the tempo clock (TempoClock.h).
 *  Params: 0 cutoff, 1 resonance, 2 LFO depth, 3 stereo (the right LFO up to half a cycle
 *  behind, Tonverk's LAG; only heard with the LFO); page 2's own, 5 LFO division
 *  (kNumLfoDivisions steps). */
class Filter : public FxBase
{
public:
    enum Param
    {
        CUTOFF,
        RESONANCE,
        LFO_DEPTH,
        STEREO,
        LFO_DIVISION = 5,
    };

    static const size_t kNumLfoDivisions = sizeof(kLfoDivisionPulses) / sizeof(kLfoDivisionPulses[0]);

    FX_ONCE void Init(float sample_rate)
    {
        filter_.Init(sample_rate);
        filter_.SetSlew(1.f); // the cutoff is slewed here, so the LFO isn't smoothed away
        gate_.Init();
        pulse_inc_ = 120.f * kPulsesPerBeat / (60.f * sample_rate); // 120 BPM until told
        lfo_pulses_ = 0;
        lfo_frac_ = 0.f;
        lfo_div_pulses_ = 48;

        for (size_t i = 0; i < kNumFxParams; i++)
            SetParam(i, 0.f);
        SnapParams();
    }

    /** Once per block: the time between two clock pulses in samples (TempoClock::
     *  PulseSamples), plus one call per clock pulse in this block, with the clock's position
     *  (TempoClock::Pulse). The pulses' real rate, not the FX's tempo, which stops at 50 BPM:
     *  on a slowed loop the LFO would otherwise run ahead and wait at every pulse */
    void SetPulseSamples(float samples) { pulse_inc_ = samples > 0.f ? 1.f / samples : 0.f; }
    /** reverse: the position counts down, so the LFO eases down towards the pulse before */
    void ClockPulse(uint32_t pos, bool reverse = false)
    {
        lfo_pulses_ = pos;
        lfo_frac_ = 0.f;
        lfo_reverse_ = reverse;
    }

    void Process(float* l, float* r)
    {
        const float gate = gate_.Process();
        const float cutoff = cutoff_.Process();
        const float depth = depth_.Process();
        // the resonance slews like the other knobs, so turning it doesn't zipper
        if (res_.Settle())
            filter_.SetRes(res_.value);

        // move smoothly between pulses at the tempo, but wait at the next pulse rather than
        // run past it, so a late MIDI clock tick doesn't make the phase jump back. In
        // reverse, the next pulse is the one below
        const float inc = pulse_inc_;
        lfo_frac_ += lfo_reverse_ ? -inc : inc;
        if (lfo_frac_ > .999f)
            lfo_frac_ = .999f;
        else if (lfo_frac_ < -.999f)
            lfo_frac_ = -.999f;

        // off and faded out: what's heard is the input, but the filter runs on, so a
        // punch-in finds it where it would be; only its cutoff, LFO and all, is worked out
        // once every kSleepRetune samples rather than every one
        if (gate_.Asleep())
        {
            if (++sleep_samples_ >= kSleepRetune)
            {
                sleep_samples_ = 0;
                SetControls(cutoff, depth);
                filter_.Retune();
            }
            float fl, fr;
            filter_.Run(*l, *r, &fl, &fr);
            return;
        }
        sleep_samples_ = 0;

        SetControls(cutoff, depth);

        float fl, fr;
        filter_.Process(*l, *r, &fl, &fr);

        *l += gate * (fl - *l);
        *r += gate * (fr - *r);
    }

    /** Both channels' cutoffs: the right one's LFO lags with the stereo knob */
    inline void SetControls(float cutoff, float depth)
    {
        filter_.SetControl(Control(cutoff, depth));
        if (lag_ > 0.f && depth > 0.f)
            filter_.SetControlR(Control(cutoff, depth, lag_));
        else
            filter_.SetControlR(-1.f);
    }

    /** The cutoff with the LFO on it, 0..1, the LFO lag cycles behind */
    inline float Control(float cutoff, float depth, float lag = 0.f) const
    {
        float pos = static_cast<float>(lfo_pulses_ % lfo_div_pulses_) + lfo_frac_;
        if (pos < 0.f)
            pos += static_cast<float>(lfo_div_pulses_);
        float phase = pos / static_cast<float>(lfo_div_pulses_) + .25f - lag;
        if (phase >= 1.f)
            phase -= 1.f;
        else if (phase < 0.f)
            phase += 1.f;
        // triangle: 0 on the beat, up to +1 (towards highpass) a quarter cycle later
        const float tri = 1.f - 4.f * fabsf(phase - .5f);

        // full depth sweeps +/-.5, the whole knob range from the centre
        return fclamp(cutoff + depth * .5f * tri, 0.f, 1.f);
    }

    /** The slewed parameters jump to their targets, at Init */
    void SnapParams()
    {
        cutoff_.Snap();
        depth_.Snap();
        res_.Snap();
        filter_.SetRes(res_.value);
    }

    void SetParam(size_t param, float val) override
    {
        switch (param)
        {
        case CUTOFF:
            cutoff_.target = val;
            break;
        case RESONANCE:
            // WAVE's master resonance: the full range, limited just below 1
            res_.target = fclamp(val, 0.f, .99f);
            break;
        case LFO_DEPTH:
            depth_.target = val;
            break;
        case STEREO:
            lag_ = .5f * val;
            break;
        case LFO_DIVISION:
            lfo_div_pulses_ = kLfoDivisionPulses[StepIndex(val, kNumLfoDivisions)];
            break;
        default:
            break;
        }
    }

private:
    DjFilter filter_;
    Smoothed cutoff_;
    Smoothed depth_;
    Smoothed res_;
    float pulse_inc_; // the LFO's advance a sample, in pulses
    uint32_t lfo_pulses_;     // the clock's position
    float lfo_frac_;          // progress towards the next pulse, negative in reverse
    bool lfo_reverse_ = false;
    static const uint32_t kSleepRetune = 8;
    uint32_t sleep_samples_ = 0; // asleep: samples since the cutoff was last worked out
    uint32_t lfo_div_pulses_; // pulses per LFO cycle
    float lag_ = 0.f;         // the right LFO's lag, in cycles
};

} // namespace chompi
