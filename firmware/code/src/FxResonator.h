/** @file FxResonator.h
 *  @brief The resonator, a comb feedback loop around the inserts.
 *
 *  Ported from Bastl Instruments' Kastle 2 FX Wizard (github.com/bastl-instruments/kastle2,
 *  code/src/apps/FxWizard/), rewritten from Kastle's 44kHz fixed point to 48kHz float.
 *  Copyright (c) 2024 Marek Mach, Vaclav Mach (Bastl Instruments), MIT License: see
 *  LICENSE-kastle2.
 */
#pragma once
#include "FxCommon.h"
#include "FxOutput.h"

namespace chompi
{

/** The feedback comb Kastle runs around every FX Wizard mode, as its own key. While
 *  on, the loop wraps the inserts between Feed and Tap (FxChain.h), so the ringing goes
 *  through every one of them that's on, on every trip. In the loop, as on Kastle:
 *  a soft clipper, a lowpass and a 50Hz highpass, which keep it bounded whatever is inside.
 *  On its own it's a comb on the dry signal. Kastle's comb is 100-2000 samples at 44kHz and
 *  at most about 40% feedback; this one is tunable and goes up to 98%.
 *  Params: 0 pitch (22-880Hz), 1 feedback, 2 tone (the loop's lowpass), 3 env mod, the
 *  feedback rising with the input's level (the SP-404MK2 Resonator's ENV MOD); page 2: 5
 *  stereo (the right channel up to 12 semitones higher), 7 the return's level (as an insert's
 *  Level, FxOutput.h). Page 2's Band filters what goes into the loop (FxChain.h). */
class Resonator : public FxBase
{
public:
    enum Param
    {
        PITCH,
        FEEDBACK,
        TONE,
        ENV_MOD,
        STEREO = 5,
        LEVEL = 7,
    };

    FX_ONCE void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        ring_.Clear();
        for (size_t c = 0; c < 2; c++)
        {
            lp_[c] = 0.f;
            hp_[c].Init(sample_rate);
            ret_[c] = 0.f;
        }
        for (size_t i = 0; i < kFxMaxBlock; i++)
            lp_at_[i] = 0.f;
        gate_.Init();

        for (size_t i = 0; i < kNumFxParams; i++)
            SetParam(i, 0.f);
        SetParam(LEVEL, FxOutput::kLevelDefault); // 0dB
        env_ = 0.f;
        env_att_ = TimeCoeff(.002f, sample_rate);
        env_rel_ = TimeCoeff(.1f, sample_rate);
        SnapParams();
    }

    /** After the freezer: adds the loop's return, scaling the input down as Kastle does.
     *  The chain runs a block's Feeds before its Taps (FxChain::Run): i is the sample's place
     *  in the block, so Feed reads the ring as if the block's i Taps before it had written it.
     *  They hadn't, but the loop is longer than a block (kHighestHz), so it reads only frames
     *  earlier blocks wrote */
    void Feed(float* l, float* r, size_t i = 0)
    {
        const float gate = gate_.Process();
        // off and faded out: no loop, so nothing to read. Tap still fills the ring, so a
        // punch-in rings from the recent sound as ever, with the knobs where they're going;
        // the env mod's follower from silence, not from the level it last had (#167)
        if (gate_.Asleep())
        {
            SnapParams();
            lp_at_[i] = lp_coeff_.value;
            ret_[0] = ret_[1] = 0.f;
            env_ = 0.f;
            return;
        }
        const float feedback = feedback_.Process();
        lp_at_[i] = lp_coeff_.Process();

        float fb = gate * feedback;
        float* const io[2] = {l, r};
        // env mod: the input's level pushes the feedback up, at most to its top
        if (env_mod_ > 0.f)
        {
            const float in = fmaxf(fabsf(*l), fabsf(*r));
            env_ += (in > env_ ? env_att_ : env_rel_) * (in - env_);
            fb = fminf(fb + gate * env_mod_ * 2.f * env_, kMaxFeedback);
        }
        else
            env_ = 0.f;
        const float level = level_.Process();
        for (size_t c = 0; c < 2; c++)
        {
            const float delay = delay_[c].Process();
            ret_[c] = level * fb * ring_.Read(c, delay - 1.f, i);
            *io[c] = *io[c] * (1.f - .5f * fb) + ret_[c];
        }
    }

    /** In place of a run's n Feeds while it's asleep for all of them (FxGate::AsleepForRun):
     *  what each would do, at once */
    FX_PER_RUN bool SleepsThrough(size_t n)
    {
        if (!gate_.AsleepForRun())
            return false;
        SnapParams();
        for (size_t i = 0; i < n; i++)
            lp_at_[i] = lp_coeff_.value;
        ret_[0] = ret_[1] = 0.f;
        env_ = 0.f;
        return true;
    }

    /** After the phaser: into the loop through the clipper and filters, at the lowpass Feed i
     *  left */
    void Tap(float l, float r, size_t i = 0)
    {
        const float in[2] = {l, r};
        for (size_t c = 0; c < 2; c++)
        {
            lp_[c] += lp_at_[i] * (SoftClip(in[c]) - lp_[c]);
            ring_.Write(c, hp_[c].Process(lp_[c]));
        }
        ring_.Advance();
    }

    /** What the loop added in the last Feed, for the key LED */
    inline float Return() const { return ret_[0] + ret_[1]; }

    /** The slewed parameters jump to their targets, at Init */
    void SnapParams()
    {
        for (size_t c = 0; c < 2; c++)
            delay_[c].Snap();
        feedback_.Snap();
        level_.Snap();
        lp_coeff_.Snap();
    }

    void SetParam(size_t param, float val) override
    {
        switch (param)
        {
        case PITCH:
            pitch_ = fclamp(val, 0.f, 1.f);
            Retune();
            break;
        case FEEDBACK:
            feedback_.target = val * kMaxFeedback;
            break;
        case TONE:
        {
            // the loop's lowpass, 1kHz to Kastle's 15kHz
            const float freq = 1000.f * powf(15.f, val);
            lp_coeff_.target = OnePoleCoeff(freq, sample_rate_);
            tone_ = uint16_t(fclamp(val, 0.f, 1.f) * kKnobUnits);
            Retune();
            break;
        }
        case ENV_MOD:
            env_mod_ = val;
            break;
        case LEVEL:
            level_.target = FxOutput::LevelGain(val);
            break;
        case STEREO:
            stereo_ = uint16_t(fclamp(val, 0.f, 1.f) * kKnobUnits);
            Retune();
            break;
        default:
            break;
        }
    }

    /** The loop's length in frames, per channel: a period of its note, less the lag the loop
     *  adds, and never as short as a block (Feed). The lag keeps it on the note (#303): the
     *  ringing goes once through the loop's lowpass every trip, which makes it flat the more
     *  the darker the tone and the higher the note, 67 cents at 440Hz with the tone at the
     *  bottom. Low notes ring on their harmonics, which the 50Hz highpass hardly moves; high
     *  notes with the tone open on their fundamental, which it pulls sharp a little: hence the
     *  table's few negative frames. For size: it runs on a knob's turn */
    __attribute__((noinline, optimize("Os"))) void Retune()
    {
        // fitted by measuring the ringing's pitch as test/resonator.cpp does, in kLagUnit
        // frames: a row per eighth of the tone knob from the bottom, a column per eighth of
        // the pitch knob, two past its top for the right channel's stereo
        static const int8_t kLag[kLagTones][kLagPitches] = {
        {92, 77, 60, 41, 17, -1, 56, 90, 109, 119, 127},
        {77, 64, 51, 38, 28, -1, 25, 57, 77, 91, 101},
        {62, 52, 42, 33, 22, 7, 5, 32, 49, 63, 75},
        {48, 41, 34, 26, 18, 9, -1, 15, 28, 38, 50},
        {35, 30, 25, 20, 15, 10, -1, 3, 15, 20, 30},
        {23, 20, 17, 14, 11, 7, 3, -2, 6, 12, 18},
        {11, 10, 9, 8, 7, 5, 3, -2, 0, 6, 11},
        {0, 1, 2, 2, 3, 2, 1, -1, -2, 1, 6},
        {-11, -8, -6, -3, -2, -1, -1, -2, -3, -1, 2},
        };
        // the tone's row and how far it is to the next, in eighths of the knob
        const uint32_t y = tone_ * kLagSteps;
        const size_t j = y >> 16;
        const float fy = (y & 0xffff) * (1.f / 65536.f);
        for (size_t c = 0; c < 2; c++)
        {
            // the note on the pitch knob's scale, the right channel's up to an octave higher
            // with the stereo, past the knob's top; the lag bilinearly from the table
            const float pitch = c ? pitch_ + stereo_ * (kStereoOnPitch / kKnobUnits) : pitch_;
            const float x = pitch * kLagSteps;
            const size_t i = size_t(x);
            const float fx = x - i;
            const int8_t* lo = &kLag[j][i];
            const int8_t* hi = &kLag[j + 1][i];
            const float a = lo[0] + fx * (lo[1] - lo[0]);
            const float b = hi[0] + fx * (hi[1] - hi[0]);
            const float lag = (a + fy * (b - a)) * kLagUnit;
            const float hz = kLowestHz * powf(kHighestHz / kLowestHz, pitch);
            delay_[c].target = fmaxf(sample_rate_ / hz - lag, kFxMaxBlock + 1.f);
        }
    }

private:
    static const size_t kBufSize = 4096; // > 48kHz / 22Hz
    static constexpr float kLowestHz = 22.f;   // Kastle's 2000 samples at 44kHz
    static constexpr float kHighestHz = 880.f; // Kastle stops at 440Hz
    static constexpr float kMaxStereoRatio = 2.f; // the right channel up to an octave higher
    static constexpr float kMaxFeedback = .98f;
    // the stereo knob's octave (kMaxStereoRatio) on the pitch knob's scale, 22-880Hz:
    // 1 / log2(40)
    static constexpr float kStereoOnPitch = .187902f;
    // Retune's table: a row or column per kLagSteps of a knob, the pitch's up to 1.25 (the
    // right channel's stereo takes the note up to 1 + kStereoOnPitch); its unit in frames
    static const size_t kLagSteps = 8, kLagPitches = 11, kLagTones = 9;
    static constexpr float kLagUnit = 1.f / 16.f;
    static_assert((1.f + kStereoOnPitch) * kLagSteps < kLagPitches - 1, "the table's pitches");
    static constexpr float kKnobUnits = 65535.f; // a knob's 0-1 in stereo_ and tone_
    // A block's Feeds read before its Taps write (Feed): the loop, at its highest pitch and
    // stereo, longer than a block, a frame to spare (25 frames at 48kHz, 1920Hz); Retune
    // keeps it so with the lag taken off
    static_assert(kHighestHz * kMaxStereoRatio < 48000.f / (kFxMaxBlock + 1),
                  "the resonator's loop longer than a block");

    /** The loop's highpass: a one-pole at 50Hz, Kastle's */
    struct Highpass
    {
        void Init(float sample_rate)
        {
            coeff = OnePoleCoeff(50.f, sample_rate);
            lp = 0.f;
        }
        float Process(float x)
        {
            lp += coeff * (x - lp);
            return x - lp;
        }
        float coeff, lp;
    };

    float sample_rate_;
    StereoRing<kBufSize> ring_;
    float lp_[2];
    Highpass hp_[2];
    float ret_[2];
    Smoothed feedback_;
    Smoothed level_;    // the return's gain
    float env_mod_ = 0.f;
    float env_ = 0.f, env_att_ = 0.f, env_rel_ = 0.f; // the input's level, for the env mod
    Smoothed lp_coeff_; // set in Feed, used in Tap
    float lp_at_[kFxMaxBlock]; // lp_coeff_ at each of a block's Feeds, for its Taps
    Smoothed delay_[2]; // frames, per channel
    // the knobs, for Retune: the pitch (kLowestHz * (kHighestHz / kLowestHz) ^ pitch_), the
    // stereo (the right channel's pitch over the left's, 2 ^ the knob) and the tone in
    // kKnobUnits, as small as that so the members before them keep their places and so
    // FxChain::Run's code its bytes
    float pitch_ = 0.f;
    uint16_t stereo_ = 0, tone_ = 0;
};

} // namespace chompi
