/** @file FxPhaser.h
 *  @brief The phaser (#68): FRIZZ's own, a chain of first-order allpass stages per channel.
 */
#pragma once
#include "FxCommon.h"
#include "FxFilter.h"
#include "FxFlanger.h"
#include "TempoClock.h"

namespace chompi
{

/** A classic phaser: kStages first-order allpasses in a row, all on one break frequency,
 *  added to the input, with feedback from the end of the chain back into it. Six stages put
 *  three notches in the spectrum, where each stage turns the phase by 30, 90 and 150 degrees
 *  (180, 540 and 900 in all); a triangle LFO sweeps them up and down in octaves around the
 *  centre. The LFO runs free in Hz or locked to the tempo clock (TempoClock.h), as the
 *  filter's, by which half of the rate knob it's on (FxCommon.h's split knob).
 *  DaisySP's Phaser isn't used: its "poles" are identical engines in parallel, not a chain,
 *  on 77KB of delay line a channel (#68).
 *
 *  Params: 0 rate (the left half free, .02-50Hz on the flanger's curve; the right half the
 *  filter LFO's divisions, 1/16 at the centre to 4 bars, kNumLfoDivisions steps), 1 feedback,
 *  2 depth (the sweep's width; it also blends the notches in, all of them from kFullWetDepth,
 *  so at 0 the phaser is dry and silent at its defaults), 3 centre: 100Hz to 6.4kHz in
 *  octaves, 800Hz in the middle; page 2: 5 stereo (the right LFO up to half a cycle behind,
 *  as the filter's).
 *
 *  The break frequency is worked out every kRetune samples and the allpasses' coefficient
 *  glides to it in between, so a sample costs 6 stages' 2 multiplies a channel and the
 *  blend. Off and faded out (FxGate::Asleep) it does nothing but count the samples, so the
 *  LFOs are where they'd be when it comes on again, its stages starting from silence. */
class Phaser : public FxBase
{
public:
    enum Param
    {
        RATE,
        FEEDBACK,
        DEPTH,
        CENTRE,
        STEREO = 5,
    };

    static const size_t kStages = 6;
    static const size_t kNumDivisions = Filter::kNumLfoDivisions;
    static constexpr float kMaxFeedback = .85f;
    static constexpr float kFullWetDepth = .2f;
    static constexpr float kMaxOctaves = 2.5f; // full depth sweeps this far either way
    static constexpr float kCentreLowHz = 100.f;
    static constexpr float kCentreOctaves = 6.f; // the centre knob's range above it
    static constexpr float kMinHz = 20.f, kMaxHz = 16000.f;
    static const uint32_t kRetune = 8;

    FX_ONCE void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        gate_.Init();
        pulse_inc_ = 120.f * kPulsesPerBeat / (60.f * sample_rate); // 120 BPM until told
        lfo_pulses_ = 0;
        lfo_frac_ = 0.f;
        lfo_reverse_ = false;
        free_phase_ = 0.f;
        since_ = 0;
        asleep_ = true;
        coeff_[0] = coeff_[1] = step_[0] = step_[1] = 0.f;
        for (size_t i = 0; i < kNumFxParams; i++)
            SetParam(i, 0.f);
        Snap();
        Clear();
    }

    /** The tempo clock, as the filter's (FxFilter.h): the pulses' spacing in samples once a
     *  block, then each pulse in the block with the clock's position */
    void SetPulseSamples(float samples) { pulse_inc_ = samples > 0.f ? 1.f / samples : 0.f; }
    void ClockPulse(uint32_t pos, bool reverse = false)
    {
        lfo_pulses_ = pos;
        lfo_reverse_ = reverse;
        // on the pulse, less the samples counted since the LFOs last moved: they went by
        // before it, and the next Retune adds them
        lfo_frac_ = (reverse ? 1.f : -1.f) * static_cast<float>(since_) * pulse_inc_;
    }

    void Process(float* l, float* r)
    {
        const float gate = gate_.Process();
        if (gate_.Asleep())
        {
            since_++;
            asleep_ = true;
            return;
        }
        if (asleep_)
            Wake();
        else if (++since_ >= kRetune)
            Retune(false);

        float* const io[2] = {l, r};
        for (size_t c = 0; c < 2; c++)
        {
            const float a = coeff_[c] += step_[c];
            const float dry = *io[c];
            float x = dry + feedback_ * last_[c];
            for (size_t s = 0; s < kStages; s++)
            {
                const float y = a * x + state_[c][s];
                state_[c][s] = x - a * y;
                x = y;
            }
            last_[c] = x;
            const float out = dry + wet_ * (gain_ * (dry + x) - dry);
            *io[c] = dry + gate * (out - dry);
        }
    }

    void SetParam(size_t param, float val) override
    {
        switch (param)
        {
        case RATE:
            synced_ = SplitUpper(val);
            if (synced_)
                div_pulses_ = kLfoDivisionPulses[SplitStep(val, kNumDivisions)];
            else
                free_inc_ = CurveMap(SplitLower(val), kFlangerRateX, kFlangerRateHz, 5) / sample_rate_;
            break;
        case FEEDBACK:
            feedback_knob_.target = val;
            break;
        case DEPTH:
            depth_.target = val;
            break;
        case STEREO:
            lag_ = .5f * val;
            break;
        case CENTRE:
            centre_.target = val;
            break;
        default:
            break;
        }
    }

    /** The break frequency in Hz for a centre knob, depth and LFO (-1..1): for the tests */
    static float BreakHz(float centre, float depth, float lfo)
    {
        const float hz = kCentreLowHz * expf(kLn2 * (kCentreOctaves * centre + kMaxOctaves * depth * lfo));
        return fclamp(hz, kMinHz, kMaxHz);
    }
    /** A first-order allpass's coefficient for its 90 degree point at hz, with a Pade
     *  tangent (under .3% off up to kMaxHz) */
    static float Coeff(float hz, float sample_rate)
    {
        const float x = PI_F * hz / sample_rate, x2 = x * x;
        const float t = x * (15.f - x2) / (15.f - 6.f * x2);
        return (t - 1.f) / (t + 1.f);
    }
    /** The coefficient the left channel's stages run on now, for the tests */
    inline float CoeffNow(size_t c) const { return coeff_[c]; }
    /** Whether the LFO follows the tempo clock, and its cycle in pulses then, for the tests */
    inline bool Synced() const { return synced_; }
    inline uint32_t DivisionPulses() const { return div_pulses_; }
    inline float FreeHz() const { return free_inc_ * sample_rate_; }

private:
    static constexpr float kLn2 = .69314718f;

    /** The knobs and LFOs on n samples further, and the coefficients' glide to where they're
     *  going. Out of line, so the per-sample code stays small (#51) */
    __attribute__((noinline)) void Retune(bool snap)
    {
        const float n = static_cast<float>(since_);
        Advance(since_);
        since_ = 0;
        // the knobs slew here, once per kRetune samples, at the rate they would per sample
        const float coeff = snap ? 1.f : fminf(FxSlew::coeff * n, 1.f);
        const float depth = depth_.Process(coeff);
        const float centre = centre_.Process(coeff);
        feedback_ = kMaxFeedback * feedback_knob_.Process(coeff);
        wet_ = fminf(depth / kFullWetDepth, 1.f);
        // half the input and half the chain, so the notches go all the way down; the feedback's
        // peaks turned down a little
        gain_ = .5f * (1.f - .5f * feedback_);

        float phase;
        if (synced_)
        {
            float pos = static_cast<float>(lfo_pulses_ % div_pulses_) + lfo_frac_;
            if (pos < 0.f)
                pos += static_cast<float>(div_pulses_);
            phase = pos / static_cast<float>(div_pulses_);
        }
        else
            phase = free_phase_;
        for (size_t c = 0; c < 2; c++)
        {
            // triangle: the centre on the beat, up to the top a quarter cycle later
            float p = phase + .25f - (c ? lag_ : 0.f);
            p -= floorf(p);
            const float tri = 1.f - 4.f * fabsf(p - .5f);
            const float target = Coeff(BreakHz(centre, depth, tri), sample_rate_);
            if (snap)
            {
                coeff_[c] = target;
                step_[c] = 0.f;
            }
            else
                step_[c] = (target - coeff_[c]) * (1.f / kRetune);
        }
    }

    /** Both LFOs on by n samples, the synced one towards the next pulse but never past it,
     *  so a late MIDI clock tick doesn't make it jump back (FxFilter.h) */
    void Advance(uint32_t n)
    {
        const float f = static_cast<float>(n);
        free_phase_ += f * free_inc_;
        free_phase_ -= floorf(free_phase_);
        lfo_frac_ += (lfo_reverse_ ? -f : f) * pulse_inc_;
        lfo_frac_ = fclamp(lfo_frac_, -.999f, .999f);
    }

    /** Coming on from asleep: from silence, the knobs where they went meanwhile */
    __attribute__((noinline)) void Wake()
    {
        asleep_ = false;
        Clear();
        Snap();
        Retune(true);
    }
    void Clear()
    {
        for (size_t c = 0; c < 2; c++)
        {
            last_[c] = 0.f;
            for (size_t s = 0; s < kStages; s++)
                state_[c][s] = 0.f;
        }
    }
    void Snap()
    {
        depth_.Snap();
        centre_.Snap();
        feedback_knob_.Snap();
    }

    float sample_rate_;
    float state_[2][kStages];
    float last_[2];             // the chain's output a sample ago, fed back
    float coeff_[2], step_[2];  // the stages' coefficient, and its glide a sample
    float feedback_ = 0.f, wet_ = 0.f, gain_ = .5f;
    Smoothed depth_, centre_, feedback_knob_;
    float lag_ = 0.f;           // the right LFO's lag, in cycles
    bool synced_ = false;
    uint32_t div_pulses_ = 48;  // synced: pulses per LFO cycle
    float free_inc_ = 0.f;      // free: the LFO's advance a sample, in cycles
    float free_phase_;
    float pulse_inc_;           // the clock's advance a sample, in pulses
    uint32_t lfo_pulses_;       // the clock's position
    float lfo_frac_;            // progress towards the next pulse, negative in reverse
    bool lfo_reverse_;
    uint32_t since_;            // samples since the LFOs last moved
    bool asleep_;
};

} // namespace chompi
