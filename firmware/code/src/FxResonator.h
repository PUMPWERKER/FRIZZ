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
 *  Params: 0 pitch (22-880Hz), 1 feedback, 2 tone (the loop's lowpass), 3 stereo (the right
 *  channel up to 12 semitones higher); page 2: 5 env mod, the feedback rising with the
 *  input's level (the SP-404MK2 Resonator's ENV MOD), 7 the return's level (as an insert's
 *  Level, FxOutput.h). Page 2's Band filters what goes into the loop (FxChain.h). */
class Resonator : public FxBase
{
public:
    enum Param
    {
        PITCH,
        FEEDBACK,
        TONE,
        STEREO,
        ENV_MOD = 5,
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
        gate_.Init();

        for (size_t i = 0; i < kNumFxParams; i++)
            SetParam(i, 0.f);
        SetParam(LEVEL, FxOutput::kLevelDefault); // 0dB
        env_ = 0.f;
        env_att_ = TimeCoeff(.002f, sample_rate);
        env_rel_ = TimeCoeff(.1f, sample_rate);
        SnapParams();
    }

    /** After the freezer: adds the loop's return, scaling the input down as Kastle does */
    void Feed(float* l, float* r)
    {
        const float gate = gate_.Process();
        // off and faded out: no loop, so nothing to read. Tap still fills the ring, so a
        // punch-in rings from the recent sound as ever, with the knobs where they're going
        if (gate_.Asleep())
        {
            SnapParams();
            ret_[0] = ret_[1] = 0.f;
            return;
        }
        const float feedback = feedback_.Process();
        lp_coeff_.Process();

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
            ret_[c] = level * fb * ring_.Read(c, delay - 1.f);
            *io[c] = *io[c] * (1.f - .5f * fb) + ret_[c];
        }
    }

    /** After the flanger: into the loop through the clipper and filters */
    void Tap(float l, float r)
    {
        const float in[2] = {l, r};
        for (size_t c = 0; c < 2; c++)
        {
            lp_[c] += lp_coeff_.value * (SoftClip(in[c]) - lp_[c]);
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
            pitch_hz_ = kLowestHz * powf(kHighestHz / kLowestHz, val);
            break;
        case FEEDBACK:
            feedback_.target = val * kMaxFeedback;
            break;
        case TONE:
        {
            // the loop's lowpass, 1kHz to Kastle's 15kHz
            const float freq = 1000.f * powf(15.f, val);
            lp_coeff_.target = OnePoleCoeff(freq, sample_rate_);
            break;
        }
        case ENV_MOD:
            env_mod_ = val;
            break;
        case LEVEL:
            level_.target = FxOutput::LevelGain(val);
            break;
        case STEREO:
            stereo_ratio_ = powf(2.f, val);
            break;
        default:
            break;
        }
        delay_[0].target = sample_rate_ / pitch_hz_;
        delay_[1].target = sample_rate_ / (pitch_hz_ * stereo_ratio_);
    }

private:
    static const size_t kBufSize = 4096; // > 48kHz / 22Hz
    static constexpr float kLowestHz = 22.f;   // Kastle's 2000 samples at 44kHz
    static constexpr float kHighestHz = 880.f; // Kastle stops at 440Hz
    static constexpr float kMaxFeedback = .98f;

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
    Smoothed delay_[2]; // frames, per channel
    float pitch_hz_ = kLowestHz;
    float stereo_ratio_ = 1.f; // 2 ^ the stereo knob: the right channel's pitch over the left's
};

} // namespace chompi
