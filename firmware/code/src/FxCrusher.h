/** @file FxCrusher.h
 *  @brief The crusher: sample-rate and bit reduction, tone, XOR and a press dive.
 *
 *  The XOR and the dive are ported from Bastl Instruments' Kastle 2 FX Wizard
 *  (github.com/bastl-instruments/kastle2, code/src/apps/FxWizard/), rewritten from Kastle's
 *  44kHz fixed point to 48kHz float.
 *  Copyright (c) 2024 Marek Mach, Vaclav Mach (Bastl Instruments), MIT License: see
 *  LICENSE-kastle2.
 */
#pragma once
#include "FxCommon.h"

namespace chompi
{

/** TEMPO's sample-rate reducer, plus bit-depth reduction, a tone control, and two extras
 *  from Kastle's crusher: XOR, which flips fixed bits of every 16-bit sample for a digital
 *  buzz, and a dive on every key press, the rate dropping up to 10x over 0.1s and
 *  recovering over 0.4s. Fully wet while on (page 2's Mix blends in the dry signal). The
 *  XOR's flips and the coarsest steps are a fixed size whatever the level, so on a quiet
 *  signal they come out louder than it went in: page 2's Level (FxOutput.h) is for that.
 *  Params: 0 rate, 1 bits, 2 tone, 3 stereo (the right channel's rate up to an octave lower);
 *  page 2's own, 5 XOR (0 = off). */
class Crusher : public FxBase
{
public:
    enum Param
    {
        RATE,
        BITS,
        TONE,
        STEREO,
        XOR = 5,
    };

    FX_ONCE void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;

        srr_l_.Init();
        srr_r_.Init();
        xor_dc_l_.Init(sample_rate);
        xor_dc_r_.Init(sample_rate);
        dive_.Reset();
        dive_attack_inc_ = 1.f / (.1f * sample_rate);
        dive_decay_coeff_ = Decay60dBCoeff(.4f, sample_rate);

        lp_l_ = lp_r_ = 0.f;
        gate_.Init();

        for (size_t i = 0; i < kNumFxParams; i++)
            SetParam(i, 0.f);
        SnapParams();
    }

    void Process(float* l, float* r)
    {
        const float gate = gate_.Process();

        // off and faded out: only what a punch-in starts from goes on: the reducer (so its
        // grid runs on unbroken) and the XOR's DC blockers (so its offset doesn't thump in);
        // the knobs where they're going
        if (gate_.Asleep())
        {
            SnapParams();
            srr_l_.SetFreq(rate_.value);
            srr_r_.SetFreq(rate_.value * stereo_);
            srr_l_.Process(*l);
            srr_r_.Process(*r);
            XorOffset(xor_dc_l_, *l);
            XorOffset(xor_dc_r_, *r);
            asleep_samples_++;
            return;
        }
        if (asleep_samples_ > 0)
        {
            // back: the dive where it would have decayed to, the tone's lowpass settled on
            // the input
            dive_.value *= powf(dive_decay_coeff_, static_cast<float>(asleep_samples_));
            asleep_samples_ = 0;
            lp_l_ = *l;
            lp_r_ = *r;
        }
        const float rate_knob = rate_.Process();
        const float tone_coeff = tone_coeff_.Process();

        if (gate_.TakePress())
            dive_.Press();
        const float dive = dive_.Process(dive_attack_inc_, dive_decay_coeff_);

        // Kastle: the rate divided by the dive envelope times 10, at least 1
        const float rate = rate_knob / fmaxf(1.f, dive * 10.f);
        srr_l_.SetFreq(rate);
        srr_r_.SetFreq(rate * stereo_);

        // XOR before the reducer, as on Kastle. On its own XOR turns silence into a constant
        // offset, so what it adds is DC-blocked: the buzz stays, the thump on punch-in doesn't
        const float xl = *l + XorOffset(xor_dc_l_, *l);
        const float xr = *r + XorOffset(xor_dc_r_, *r);
        float wl = srr_l_.Process(xl);
        float wr = srr_r_.Process(xr);

        // bit-depth reduction: round to the nearest step
        // one value from the UI, so it can't be read half-written
        const float step = step_;
        const float inv_step = 1.f / step;
        wl = floorf(wl * inv_step + .5f) * step;
        wr = floorf(wr * inv_step + .5f) * step;

        // one-pole lowpass to tame the aliasing
        lp_l_ += tone_coeff * (wl - lp_l_);
        lp_r_ += tone_coeff * (wr - lp_r_);

        *l += gate * (lp_l_ - *l);
        *r += gate * (lp_r_ - *r);
    }

    /** The slewed parameters jump to their targets, at Init */
    void SnapParams()
    {
        rate_.Snap();
        tone_coeff_.Snap();
    }

    void SetParam(size_t param, float val) override
    {
        switch (param)
        {
        case RATE:
            // TEMPO's range (SampleEngine::setSampleReducer), 21.6kHz down to 480Hz, but
            // exponential: TEMPO's is linear in Hz, which leaves the crunch in the last
            // quarter of the knob. Here every 18% of the knob halves the rate.
            rate_.target = .45f * powf(kRateBottom / .45f, val);
            break;

        case BITS:
        {
            // 16 bits down to 2, continuous so the knob sweeps smoothly
            const float bits = 16.f - val * 14.f;
            step_ = powf(2.f, 1.f - bits);
            break;
        }

        case TONE:
        {
            // lowpass from 200Hz to 20kHz, fully open at the top
            tone_coeff_.target = ToneCoeff(val, sample_rate_);
            break;
        }

        case STEREO:
            stereo_ = 1.f - .5f * val;
            break;
        case XOR:
        {
            // Kastle: 0 / 1000 / 2000 / 4000 over the top 30% of its Amount knob, here over
            // the whole knob
            static const float xs[] = {0.f, .233f, .667f, 1.f};
            static const float ys[] = {0.f, 1000.f, 2000.f, 4000.f};
            xor_ = static_cast<int16_t>(CurveMap(val, xs, ys, 4));
            break;
        }

        default:
            break;
        }
    }

private:
    static constexpr float kRateBottom = .01f; // 480Hz, as a fraction of 48kHz

    /** Kastle's XOR on the sample as 16-bit */
    inline float Xor(float x) const
    {
        const int16_t i = static_cast<int16_t>(fclamp(x, -1.f, 1.f) * 32767.f);
        return static_cast<float>(static_cast<int16_t>(i ^ xor_)) * (1.f / 32767.f);
    }
    /** What the XOR adds to x, DC-blocked; with it off, the blocker runs on 0 */
    inline float XorOffset(daisysp::DcBlock& dc, float x) const
    {
        return dc.Process(xor_ > 0 ? Xor(x) - x : 0.f);
    }

    float sample_rate_;
    daisysp::SampleRateReducer srr_l_, srr_r_;
    float lp_l_, lp_r_;
    Smoothed rate_;
    Smoothed tone_coeff_;
    float step_; // quantizer step, 2^(1 - bits)
    int16_t xor_ = 0;
    float stereo_ = 1.f; // the right channel's rate over the left's
    daisysp::DcBlock xor_dc_l_, xor_dc_r_;
    PressEnvelope dive_;
    uint32_t asleep_samples_ = 0; // how long it hasn't run (FxGate::Asleep)
    float dive_attack_inc_, dive_decay_coeff_;
};

} // namespace chompi
