/** @file FxFlanger.h
 *  @brief The flanger.
 *
 *  Ported from Bastl Instruments' Kastle 2 FX Wizard (github.com/bastl-instruments/kastle2,
 *  code/src/apps/FxWizard/), rewritten from Kastle's 44kHz fixed point to 48kHz float.
 *  Copyright (c) 2024 Marek Mach, Vaclav Mach (Bastl Instruments), MIT License: see
 *  LICENSE-kastle2.
 */
#pragma once
#include "FxCommon.h"

namespace chompi
{

// Flanger maps: LFO rate in Hz by the rate knob, the right LFO's detune by the stereo knob
static const float kFlangerRateX[] = {0.f, .3f, .6f, .75f, 1.f};
static const float kFlangerRateHz[] = {.02f, .1f, 1.f, 5.f, 50.f};
static const float kFlangerDetuneX[] = {0.f, .1f, 1.f};
static const float kFlangerDetuneHz[] = {0.f, .5f, .2f};

/** Kastle's flanger. A delay of about 12ms swept either way by a triangle LFO, mixed
 *  with the input. Like Kastle's Amount, one knob sets both the sweep depth and the mix, so
 *  the top of it is pure vibrato. Pressing the key restarts the sweep (Kastle's trigger).
 *  Feedback recirculates the swept delay, a classic flanger's resonance; Kastle's Feedback
 *  is instead a short comb around every mode, at most 8% for the flanger.
 *  Params: 0 rate, 1 feedback, 2 amount, 3 stereo; page 2's own, 5 polarity: the feedback
 *  positive (metallic, as before) or negative (hollow), two steps. */
class Flanger : public FxBase
{
public:
    enum Param
    {
        RATE,
        FEEDBACK,
        AMOUNT,
        STEREO,
        POLARITY = 5,
    };

    FX_ONCE void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        ring_.Clear();
        for (size_t c = 0; c < 2; c++)
            phase_[c] = 0.f;
        gate_.Init();

        for (size_t i = 0; i < kNumFxParams; i++)
            SetParam(i, 0.f);
        SnapParams();
    }

    void Process(float* l, float* r)
    {
        const float gate = gate_.Process();

        if (gate_.TakePress())
            phase_[0] = phase_[1] = 0.f;

        // off: only the buffer goes on (a press restarts the LFOs anyway), the knobs where
        // they're going
        if (gate_.Asleep())
        {
            SnapParams();
            ring_.WriteFrame(SoftClip(*l), SoftClip(*r));
            return;
        }
        const float depth = depth_.Process();
        const float mix = mix_.Process();
        const float feedback = feedback_.Process();
        const float stereo_mix = stereo_mix_.Process();

        // triangle LFOs, -1..1, starting at 0 going up
        float lfo[2];
        for (size_t c = 0; c < 2; c++)
        {
            phase_[c] += inc_[c];
            if (phase_[c] >= 1.f)
                phase_[c] -= 1.f;
            lfo[c] = phase_[c] < .25f   ? 4.f * phase_[c]
                     : phase_[c] < .75f ? 2.f - 4.f * phase_[c]
                                        : 4.f * phase_[c] - 4.f;
        }
        // Kastle: the right LFO is the left one until stereo is turned up. Re-synced once the
        // glide back has all but arrived, where the right one's share is too small to jump
        if (stereo_mix > kStereoSync)
            phase_[1] = phase_[0];
        lfo[1] = stereo_mix * lfo[0] + (1.f - stereo_mix) * lfo[1];

        float* const io[2] = {l, r};
        for (size_t c = 0; c < 2; c++)
        {
            const float delay = fclamp(kCentreFrames * (1.f + lfo[c] * depth), 1.f, kBufSize - 2.f);
            const float wet = ring_.Read(c, delay);
            ring_.Write(c, SoftClip(*io[c] + wet * feedback));

            const float out = *io[c] + mix * (wet - *io[c]);
            *io[c] += gate * (out - *io[c]);
        }
        ring_.Advance();
    }

    /** The slewed parameters jump to their targets, at Init */
    void SnapParams()
    {
        depth_.Snap();
        mix_.Snap();
        feedback_.Snap();
        stereo_mix_.Snap();
    }

    void SetParam(size_t param, float val) override
    {
        switch (param)
        {
        case RATE:
            rate_ = CurveMap(val, kFlangerRateX, kFlangerRateHz, 5);
            break;
        case AMOUNT:
        {
            // Kastle: depth 0 / .5 / 1 at 0 / .2 / 1, and the mix 0 to 1
            static const float xs[] = {0.f, .2f, 1.f};
            static const float ys[] = {0.f, .5f, 1.f};
            depth_.target = CurveMap(val, xs, ys, 3);
            mix_.target = val;
            break;
        }
        case FEEDBACK:
            feedback_val_ = val;
            feedback_.target = polarity_ * val * .85f;
            break;
        case POLARITY:
            polarity_ = StepIndex(val, 2) ? -1.f : 1.f;
            feedback_.target = polarity_ * feedback_val_ * .85f;
            break;
        case STEREO:
            stereo_ = val;
            stereo_mix_.target = fclamp(1.f - val / .2f, 0.f, 1.f);
            break;
        default:
            break;
        }
        inc_[0] = rate_ / sample_rate_;
        inc_[1] = (rate_ + CurveMap(stereo_, kFlangerDetuneX, kFlangerDetuneHz, 3)) / sample_rate_;
    }

private:
    static const size_t kBufSize = 2048; // 2 x the deepest sweep, 1114 frames
    static constexpr float kCentreFrames = 557.f; // Kastle's 511 at 44kHz, 11.6ms

    float sample_rate_;
    StereoRing<kBufSize> ring_;
    float phase_[2];
    float inc_[2] = {0.f, 0.f};
    float rate_ = 0.f, stereo_ = 0.f;
    float feedback_val_ = 0.f, polarity_ = 1.f;
    Smoothed depth_;
    Smoothed mix_;
    Smoothed feedback_;
    static constexpr float kStereoSync = .9999f;
    Smoothed stereo_mix_;
};

} // namespace chompi
