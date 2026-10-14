/** @file FxFilter.h
 *  @brief The DJ filter with a tempo-synced LFO.
 */
#pragma once
#include "FxCommon.h"
#include "DJFilter.h"
#include "FxTempoLfo.h"
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
 *  Params: 0 cutoff, 1 resonance, 2 LFO depth, 3 LFO division (kNumLfoDivisions
 *  steps); page 2: 5 stereo (the right LFO up to half a cycle behind, Tonverk's LAG; only
 *  heard with the LFO).
 *
 *  The knobs, the LFO and the filter's coefficients move every kRetune samples, as the
 *  phaser's (FxPhaser.h); a sample costs the two channels' filter and the blend. Off and faded
 *  out (FxGate::Asleep), the filter runs on unheard, so a punch-in finds it where it would
 *  be. */
class Filter : public FxBase
{
public:
    enum Param
    {
        CUTOFF,
        RESONANCE,
        LFO_DEPTH,
        LFO_DIVISION,
        STEREO = 5,
    };

    static const size_t kNumLfoDivisions = sizeof(kLfoDivisionPulses) / sizeof(kLfoDivisionPulses[0]);

    FX_ONCE void Init(float sample_rate)
    {
        filter_.Init();
        gate_.Init();
        lfo_.Init(sample_rate);
        lfo_div_pulses_ = 48;

        for (size_t i = 0; i < kNumFxParams; i++)
            SetParam(i, 0.f);
        SnapParams();
    }

    /** The tempo clock (FxTempoLfo.h): the pulses' spacing in samples once a block, then
     *  each pulse in the block with the clock's position. reverse: the position counts down,
     *  so the LFO eases down towards the pulse before */
    void SetPulseSamples(float samples) { lfo_.SetPulseSamples(samples); }
    void ClockPulse(uint32_t pos, bool reverse = false) { lfo_.PulseAfter(pos, reverse, since_); }

    void Process(float* l, float* r)
    {
        const float gate = gate_.Process();
        if (++since_ >= DjFilter::kRetune)
            Retune(false);

        float fl, fr;
        filter_.Run(*l, *r, &fl, &fr);
        if (gate_.Asleep())
            return;

        *l += gate * (fl - *l);
        *r += gate * (fr - *r);
    }

    /** The cutoff with the LFO on it, 0..1, the LFO lag cycles behind */
    inline float Control(float cutoff, float depth, float lag = 0.f) const
    {
        float phase = lfo_.Cycle(lfo_div_pulses_) + .25f - lag;
        if (phase >= 1.f)
            phase -= 1.f;
        else if (phase < 0.f)
            phase += 1.f;
        // triangle: 0 on the beat, up to +1 (towards highpass) a quarter cycle later
        const float tri = LfoTriangle(phase);

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
        Retune(true);
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
    /** The knobs and the LFO on by the samples since the last time, and the filter's
     *  coefficients: both channels' cutoffs, the right one's LFO lagging with the stereo
     *  knob. Out of line, so the per-sample code stays small (#51) */
    __attribute__((noinline)) void Retune(bool snap)
    {
        const float n = static_cast<float>(since_);
        lfo_.Advance(since_);
        since_ = 0;
        // the knobs slew here, at the rate they would per sample; the resonance too, so
        // turning it doesn't zipper
        const float coeff = FxSlewOver(n, snap);
        const float cutoff = cutoff_.Process(coeff);
        const float depth = depth_.Process(coeff);
        if (res_.Settle(coeff))
            filter_.SetRes(res_.value);
        const bool lag = lag_ > 0.f && depth > 0.f;
        filter_.Retune(Control(cutoff, depth), lag ? Control(cutoff, depth, lag_) : -1.f, snap);
    }

    DjFilter filter_;
    Smoothed cutoff_;
    Smoothed depth_;
    Smoothed res_;
    TempoLfo lfo_;
    uint32_t since_ = 0; // samples since the knobs and the LFO last moved
    uint32_t lfo_div_pulses_; // pulses per LFO cycle
    float lag_ = 0.f;         // the right LFO's lag, in cycles
};

} // namespace chompi
