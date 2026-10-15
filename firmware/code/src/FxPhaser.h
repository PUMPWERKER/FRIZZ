/** @file FxPhaser.h
 *  @brief The phaser (#68): FRIZZ's own, a chain of first-order allpass stages per channel;
 *  and its second mode, a frequency shifter (#306) after Mutable Instruments' Warps.
 */
#pragma once
#include "FxCommon.h"
#include "FxFilter.h"
#include "FxFlanger.h"
#include "FxTempoLfo.h"
#include "TempoClock.h"

namespace chompi
{

// The frequency shifter's allpass pair (Phaser::Chain): Warps' AllPassFilter's coefficients
// (which its Init takes as -pole), fitted for 48kHz to a 90 degree difference within 1.25
// degrees from 25Hz to 19kHz; a seventh pair would make it .65 for half a point of the block
static const size_t kShiftStages = 6;
static const float kShiftI[kShiftStages] = {-.998399628f, -.983978526f, -.942594583f, -.795442138f,
                                            -.414807544f, .276425238f};
static const float kShiftQ[kShiftStages] = {-.993727251f, -.966852645f, -.894634862f, -.642969373f,
                                            -.103465642f, .728583257f};

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
 *  LFOs are where they'd be when it comes on again, its stages starting from silence.
 *
 *  Its second mode (SetMode, #306), a frequency shifter: every frequency in the sound moved by
 *  the same number of Hz, so harmonics no longer line up, as in Warps' easter egg
 *  (pichenettes/eurorack, warps/dsp/quadrature_transform.h, MIT; THIRD_PARTY.md). Two chains
 *  of first-order allpasses (Warps' AllPassFilter) turn the input into a pair 90 degrees
 *  apart, kShiftStages each, their coefficients (kShiftI, kShiftQ) fitted for 48kHz to within
 *  1.25 degrees from 25Hz to 19kHz (Warps' 17 are for 96kHz): an unwanted sideband 39dB down.
 *  A quadrature carrier multiplies them, and their difference or sum is the upper or lower
 *  sideband alone. The same eight knobs, read the other way: 0 the shift (the left half free,
 *  kShiftLowHz to 10000 times that on an exponential curve; the right half one cycle per
 *  tempo division, the phaser's), 1 feedback (the shifted sound shifted again, through a soft
 *  limit and a lowpass, as Warps' does: a few Hz make a barber pole), 2 depth (the shifted
 *  sound's share, 0 dry), 3 direction (down at 0, both sidebands in the middle, up at 1, in
 *  between leaning one way); page 2: 5 stereo (the right channel's direction turned the other
 *  way, through both sidebands at the middle). The carrier is a phasor turned a sample at a
 *  time and set again every kRetune samples from the LFOs, so it's locked to the clock when
 *  synced. One mode runs at a time: a switch dips the effect out over kDipSamples, switches
 *  and starts the new mode from silence, and dips it back in; asleep it switches at once. */
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
        // the frequency shifter's names for two of them (SetMode)
        SHIFT = RATE,
        DIRECTION = CENTRE,
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

    // the frequency shifter (SetMode)
    static constexpr float kShiftLowHz = .1f;       // the shift's free half: .1Hz ...
    static constexpr float kShiftDecades = 4.f;     // ... to 1kHz
    static constexpr float kMaxShiftFeedback = .9f;
    static constexpr float kShiftFeedbackLp = .35f; // the feedback's lowpass, ~3.3kHz
    static const uint32_t kDipSamples = 240;        // a mode switch's dip out and in, 5ms each
    static const uint32_t kResync = 32;             // retunes between setting the carrier, 256 samples

    FX_ONCE void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        gate_.Init();
        lfo_.Init(sample_rate);
        lfo_.SetReverse(false);
        free_phase_ = 0.f;
        since_ = 0;
        asleep_ = true;
        want_ = shifting_ = dipping_ = dip_down_ = false;
        dip_ = 1.f;
        co_ = 1.f;
        so_ = cr_ = sr_ = turn_ = 0.f;
        resync_ = kResync;
        coeff_[0] = coeff_[1] = step_[0] = step_[1] = 0.f;
        for (size_t i = 0; i < kNumFxParams; i++)
            SetParam(i, 0.f);
        Snap();
        Clear();
    }

    /** The tempo clock, as the filter's (FxFilter.h): the pulses' spacing in samples once a
     *  block, then each pulse in the block with the clock's position */
    void SetPulseSamples(float samples) { lfo_.SetPulseSamples(samples); }
    void ClockPulse(uint32_t pos, bool reverse = false)
    {
        // asleep, the samples counted so far move the LFOs now: the pulse sets where the
        // tempo LFO is, and a Wake that moved it by all of them would do it at the tempo it
        // wakes at, the whole sleep off if the tempo changed meanwhile
        if (asleep_)
        {
            Advance(since_);
            since_ = 0;
        }
        lfo_.PulseAfter(pos, reverse, since_);
    }

    /** The mode: the phaser, or the frequency shifter (#306). From MainLoop or the morph: the
     *  audio switches with a dip, or at once while asleep (file comment) */
    inline void SetMode(bool shifter) { want_ = shifter; }
    /** The mode the audio runs now, for the tests */
    inline bool Shifting() const { return shifting_; }

    /** FxBase::SleepsThrough: what Process does asleep, once for the run */
    FX_PER_RUN bool SleepsThrough(const float*, const float*, size_t n)
    {
        if (!gate_.AsleepForRun())
            return false;
        since_ += n;
        asleep_ = true;
        return true;
    }

    void Process(float* l, float* r)
    {
        float gate = gate_.Process();
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
        // a mode switch: out of line, as the other mode
        if (__builtin_expect(dipping_, 0))
            gate *= Dip();
        if (__builtin_expect(shifting_, 0))
        {
            Shift(l, r, gate);
            return;
        }

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
            rate_ = val;
            SetRate();
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
    /** The shift's free half: Hz for its knob 0..1 (the rate knob's lower half as 0..1) */
    static float ShiftHz(float x) { return kShiftLowHz * expf(kShiftDecades * kLn10 * x); }

private:
    static constexpr float kLn2 = .69314718f, kLn10 = 2.3025851f;
    static constexpr float kTwoPi = 6.2831853f;

    /** The rate knob (rate_) as the mode reads it: synced, or the free LFO's or carrier's
     *  advance a sample */
    __attribute__((noinline)) FRIZZ_AFTER_RUN_LAST void SetRate()
    {
        synced_ = SplitUpper(rate_);
        resync_ = kResync; // the carrier where the LFO is, at the next retune
        if (synced_)
            div_pulses_ = kLfoDivisionPulses[SplitStep(rate_, kNumDivisions)];
        else
            free_inc_ = (shifting_ ? ShiftHz(SplitLower(rate_))
                                   : CurveMap(SplitLower(rate_), kFlangerRateX, kFlangerRateHz, 5))
                        / sample_rate_;
    }

    /** The knobs and LFOs on n samples further, and the coefficients' glide to where they're
     *  going. Out of line, so the per-sample code stays small (#51) */
    __attribute__((noinline)) void Retune(bool snap)
    {
        const float n = static_cast<float>(since_);
        Advance(since_);
        since_ = 0;
        // the knobs slew here, once per kRetune samples, at the rate they would per sample
        const float coeff = FxSlewOver(n, snap);
        const float depth = depth_.Process(coeff);
        const float centre = centre_.Process(coeff);
        // the frequency shifter's, or a mode switch's start: out of line, so the phaser's code
        // before FxChain::Run keeps its size (FRIZZ_AFTER_RUN_LAST)
        if (__builtin_expect(shifting_ | (want_ != shifting_), 0) && RetuneShift(coeff, depth, centre))
            return;
        feedback_ = kMaxFeedback * feedback_knob_.Process(coeff);
        wet_ = fminf(depth / kFullWetDepth, 1.f);
        // half the input and half the chain, so the notches go all the way down; the feedback's
        // peaks turned down a little
        gain_ = .5f * (1.f - .5f * feedback_);

        float phase;
        if (synced_)
            phase = lfo_.Cycle(div_pulses_);
        else
            phase = free_phase_;
        for (size_t c = 0; c < 2; c++)
        {
            // triangle: the centre on the beat, up to the top a quarter cycle later
            float p = phase + .25f - (c ? lag_ : 0.f);
            p -= floorf(p);
            const float tri = LfoTriangle(p);
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

    /** Both LFOs on by n samples, the synced one towards the next pulse but never past it
     *  (FxTempoLfo.h) */
    void Advance(uint32_t n)
    {
        const float f = static_cast<float>(n);
        free_phase_ += f * free_inc_;
        free_phase_ -= floorf(free_phase_);
        lfo_.Advance(n);
    }

    /** The frequency shifter's Retune: the knobs, and the carrier set again where the LFO is,
     *  its turn a sample at the free rate or the clock's; false in the phaser's mode, where it
     *  only starts a switch. The shifter's code (this, Shift, Dip,
     *  SetRate, Clear) is linked after FxChain::Run and the other effects' code there
     *  (FRIZZ_AFTER_RUN_LAST), so it moves neither */
    __attribute__((noinline)) FRIZZ_AFTER_RUN_LAST bool RetuneShift(float coeff, float depth, float direction)
    {
        // a mode switch starts with its dip (Dip); the phaser's Retune goes on until it's done
        if (want_ != shifting_ && !dipping_)
            dipping_ = dip_down_ = true;
        if (!shifting_)
            return false;
        feedback_ = kMaxShiftFeedback * feedback_knob_.Process(coeff);
        wet_ = depth;
        gain_ = 1.f - .5f * feedback_; // the repeats add up
        dir_[0] = 2.f * direction - 1.f;
        dir_[1] = dir_[0] * (1.f - 4.f * lag_); // stereo (2 lag_) 1: the other way
        float phase, turn;
        if (synced_)
        {
            phase = lfo_.Cycle(div_pulses_);
            turn = lfo_.PulseInc() / static_cast<float>(div_pulses_);
            if (lfo_.Reverse())
                turn = -turn;
        }
        else
        {
            phase = free_phase_;
            turn = free_inc_;
        }
        // the carrier set again where the LFO is every kResync retunes, turned a sample at a
        // time in between (a few 1e-7 off by then): sinf and cosf every 8 samples cost about
        // half a point of the block
        if (++resync_ >= kResync)
        {
            resync_ = 0;
            co_ = cosf(kTwoPi * phase);
            so_ = sinf(kTwoPi * phase);
        }
        if (turn != turn_)
        {
            turn_ = turn;
            cr_ = cosf(kTwoPi * turn);
            sr_ = sinf(kTwoPi * turn);
        }
        return true;
    }

    /** A sample of the frequency shifter, both channels (file comment), gate: the key's fade */
    __attribute__((noinline)) FRIZZ_AFTER_RUN_LAST void Shift(float* l, float* r, float gate)
    {
        // both inputs read first: a store to the state may be one to l or r, as far as the
        // compiler knows, so a channel's read after the other's stores would keep the two
        // channels from overlapping
        const float dry[2] = {*l, *r};
        float wet[2];
        for (size_t c = 0; c < 2; c++)
        {
            const float x = dry[c] + feedback_ * fed_[c];
            float* const st = pair_[c]; // the input a sample ago, then each stage's output
            const float i = Chain(kShiftI, x, st[0], st + 1);
            const float q = Chain(kShiftQ, x, st[0], st + 1 + kShiftStages);
            st[0] = x;
            const float y = i * co_ + dir_[c] * q * so_;
            fed_[c] += kShiftFeedbackLp * (SoftLimit(y) - fed_[c]);
            wet[c] = dry[c] + wet_ * (gain_ * y - dry[c]);
        }
        *l = dry[0] + gate * (wet[0] - dry[0]);
        *r = dry[1] + gate * (wet[1] - dry[1]);
        const float co = co_ * cr_ - so_ * sr_;
        so_ = so_ * cr_ + co_ * sr_;
        co_ = co;
    }
    /** x through a chain of Warps' first-order allpasses on coefficients a; prev: the input a
     *  sample ago, which is the first stage's; st: each stage's output a sample ago, the
     *  next one's input then */
    static inline float Chain(const float* a, float x, float prev, float* st)
    {
        for (size_t s = 0; s < kShiftStages; s++)
        {
            const float y = a[s] * (x - st[s]) + prev;
            prev = st[s];
            st[s] = y;
            x = y;
        }
        return x;
    }
    /** A soft limit as stmlib's SoftLimit, without its division: x - 4/27 x^3, smoothly up to
     *  1 at 1.5 and flat from there */
    static inline float SoftLimit(float x)
    {
        x = fclamp(x, -1.5f, 1.5f);
        return x - (4.f / 27.f) * x * x * x;
    }

    /** A sample of a mode switch's dip, 1 to 0 and back: at the bottom the mode switches,
     *  starting from silence. Wanted back meanwhile, it comes up as it was */
    __attribute__((noinline)) FRIZZ_AFTER_RUN_LAST float Dip()
    {
        const float step = 1.f / static_cast<float>(kDipSamples);
        if (dip_down_)
        {
            dip_ -= step;
            if (dip_ <= 0.f)
            {
                // to the mode wanted, from silence, as from asleep; then back up
                if (want_ != shifting_)
                    Wake();
                dipping_ = true;
                dip_down_ = false;
                dip_ = 0.f;
            }
        }
        else
        {
            dip_ += step;
            if (dip_ >= 1.f)
            {
                dip_ = 1.f;
                dipping_ = false;
            }
        }
        return dip_;
    }
    /** Coming on from asleep: from silence, the knobs where they went meanwhile, in the mode
     *  wanted */
    __attribute__((noinline)) void Wake()
    {
        asleep_ = false;
        dipping_ = dip_down_ = false;
        dip_ = 1.f;
        if (want_ != shifting_)
        {
            shifting_ = want_;
            SetRate();
        }
        resync_ = kResync; // the carrier where the LFO is
        Clear();
        Snap();
        Retune(true);
    }
    __attribute__((noinline, optimize("Os"))) FRIZZ_AFTER_RUN_LAST void Clear()
    {
        for (size_t c = 0; c < 2; c++)
        {
            last_[c] = fed_[c] = 0.f;
            for (size_t s = 0; s < kStages; s++)
                state_[c][s] = 0.f;
            for (size_t s = 0; s < 1 + 2 * kShiftStages; s++)
                pair_[c][s] = 0.f;
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
    TempoLfo lfo_;              // synced: the tempo clock
    uint32_t since_;            // samples since the LFOs last moved
    bool asleep_;
    float rate_ = 0.f;          // the rate knob, read by the mode (SetRate)

    // the frequency shifter
    volatile bool want_;        // the mode set (SetMode)
    bool shifting_;             // and the one the audio runs
    bool dipping_, dip_down_;   // a switch's dip under way, and going down
    float dip_;                 // its gain, 1 to 0 and back
    float pair_[2][1 + 2 * kShiftStages]; // the allpass pair's (Chain), per channel
    float fed_[2];              // the shifted sound a sample ago, limited and lowpassed
    float dir_[2];              // per channel: -1 down, 0 both sidebands, 1 up
    float co_, so_;             // the carrier, cos and sin
    float cr_, sr_, turn_;      // its turn a sample, and that in cycles
    uint32_t resync_;           // retunes since the carrier was set (RetuneShift)
};


} // namespace chompi
