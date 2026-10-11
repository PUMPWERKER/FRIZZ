/** @file FxSlicer.h
 *  @brief The slicer, a rhythmic gate.
 *
 *  Ported from Bastl Instruments' Kastle 2 FX Wizard (github.com/bastl-instruments/kastle2,
 *  code/src/apps/FxWizard/), rewritten from Kastle's 44kHz fixed point to 48kHz float.
 *  Copyright (c) 2024 Marek Mach, Vaclav Mach (Bastl Instruments), MIT License: see
 *  LICENSE-kastle2.
 */
#pragma once
#include "FxCommon.h"
#include "TempoClock.h"

namespace chompi
{

// Kastle's slicer patterns, sparse to dense, 8 16th-note steps
static const uint8_t kSlicerPatterns[] = {
    0b10000000,
    0b10001000,
    0b00100010,
    0b10000100,
    0b10010010,
    0b10101010,
    0b10101100,
    0b11111111,
};

/** Kastle's slicer. A gate on 16th-note steps: each step of the pattern that's on
 *  retriggers an envelope (10ms attack, then a decay) that the signal is multiplied by.
 *  Steps are counted from the clock pulses, so an 8-step pattern spans half a bar.
 *  Pressing the key also triggers it, so the signal doesn't drop out until the next step.
 *  Params: 0 pattern (kNumPatterns steps), 1 decay, 2 chance, 3 stereo (kNumPatterns steps).
 *  Chance flips every step of the pattern, on both channels, at random. Stereo plays the
 *  pattern that many patterns up on the left and down on the right. Page 2's own, 5 shuffle:
 *  the even steps late, up to two thirds of a 16th (a triplet swing), timed in samples from
 *  the pulses' spacing (the clock has only 3 pulses a 16th). */
class Slicer : public FxBase
{
public:
    enum Param
    {
        PATTERN,
        DECAY,
        CHANCE,
        STEREO,
        SHUFFLE = 5,
    };

    static const size_t kNumPatterns = sizeof(kSlicerPatterns);

    FX_ONCE void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        attack_inc_ = 1.f / (.01f * sample_rate);
        gate_.Init();
        env_[0].Reset();
        env_[1].Reset();
        step_ = false;
        skipped_ = 0;
        rng_.Seed(0x2545F491u);

        for (size_t i = 0; i < kNumFxParams; i++)
            SetParam(i, 0.f);
    }

    /** Once per block: the time between two clock pulses in samples, for the shuffle */
    void SetPulseSamples(float samples) { pulse_samples_ = samples; }
    /** One call per clock pulse, with the clock's position (TempoClock::Pulse) */
    void ClockPulse(uint32_t pos)
    {
        if (pos % kPulsesPer16th == 0)
        {
            // the odd steps on the 16th, the even ones (2, 4 ...) shuffle_ of one late
            const uint32_t step = pos / kPulsesPer16th % kNumSteps;
            if ((step & 1) && shuffle_ > 0.f)
            {
                late_step_ = step;
                late_ = 1 + static_cast<uint32_t>(shuffle_ * kPulsesPer16th * pulse_samples_);
            }
            else
            {
                step_ = true;
                step_idx_ = step;
            }
        }
    }

    void Process(float* l, float* r)
    {
        const float gate = gate_.Process();
        // off and faded out, it passes its input and only keeps count: the steps and their
        // chances still come, so the pattern goes on as ever, and the envelopes catch up on
        // what they slept through when a step presses them or the key wakes them
        const bool asleep = gate_.Asleep();
        if (!asleep)
            CatchUp();

        if (gate_.TakePress())
        {
            env_[0].Press();
            env_[1].Press();
        }

        if (late_ > 0 && --late_ == 0)
        {
            step_ = true;
            step_idx_ = late_step_;
        }
        if (step_)
        {
            step_ = false;
            CatchUp();
            const uint32_t step = step_idx_;
            const bool flip = rng_.Uniform() < chance_;

            const size_t pattern[2] = {
                (pattern_ + stereo_) % kNumPatterns,
                (pattern_ + kNumPatterns - stereo_) % kNumPatterns,
            };
            for (size_t c = 0; c < 2; c++)
            {
                // bit 7 is the first step, so 0b10001000 is quarter notes (Kastle reads the
                // bits from the other end)
                const bool hit = (kSlicerPatterns[pattern[c]] >> (kNumSteps - 1 - step)) & 1;
                if (hit != flip)
                    env_[c].Press();
            }
        }

        if (asleep)
        {
            skipped_++;
            return;
        }

        float* const io[2] = {l, r};
        for (size_t c = 0; c < 2; c++)
        {
            const float env = env_[c].Process(attack_inc_, decay_coeff_);
            *io[c] += gate * (*io[c] * env - *io[c]);
        }
    }

    void SetParam(size_t param, float val) override
    {
        switch (param)
        {
        case PATTERN:
            pattern_ = StepIndex(val, kNumPatterns);
            break;
        case DECAY:
        {
            // Kastle: 1s down to 10ms; here short to long, the time to fall by 60dB
            const float time = .01f * powf(100.f, val);
            decay_coeff_ = Decay60dBCoeff(time, sample_rate_);
            break;
        }
        case CHANCE:
            chance_ = val * .9f; // Kastle's maximum
            break;
        case STEREO:
            stereo_ = StepIndex(val, kNumPatterns);
            break;
        case SHUFFLE:
            shuffle_ = val * (2.f / 3.f);
            break;
        default:
            break;
        }
    }

private:
    static const uint32_t kNumSteps = 8;

    /** The envelopes over the samples it slept through, at once */
    inline void CatchUp()
    {
        if (skipped_ == 0)
            return;
        env_[0].Skip(skipped_, attack_inc_, decay_coeff_);
        env_[1].Skip(skipped_, attack_inc_, decay_coeff_);
        skipped_ = 0;
    }

    float sample_rate_;
    float attack_inc_;
    PressEnvelope env_[2];
    bool step_; // ClockPulse and Process both run in the audio callback
    Rng rng_;
    size_t pattern_ = 0;
    size_t stereo_ = 0;
    float chance_ = 0.f;
    float shuffle_ = 0.f;        // how late the even steps come, in 16ths
    float pulse_samples_ = 1000.f; // the clock pulses' spacing (SetPulseSamples)
    uint32_t late_ = 0;          // samples until a late step, 0: none waiting
    uint32_t late_step_ = 0, step_idx_ = 0;
    float decay_coeff_ = 0.f;
    uint32_t skipped_ = 0; // samples the envelopes slept through (CatchUp)
};

} // namespace chompi
