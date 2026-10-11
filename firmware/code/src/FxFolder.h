/** @file FxFolder.h
 *  @brief The folder: a wavefolder, sine to triangle, antialiased.
 */
#pragma once
#include "FxCommon.h"

namespace chompi
{

/** A wavefolder in the Buchla / Serge manner: the signal is driven past the fold points and
 *  mirrored back, each fold adding harmonics. The fold is a sine (smooth, the classic
 *  Buchla shape) crossfaded into a triangle (sharp, brighter), with the symmetry adding a
 *  bias so the folds go uneven and bring in even harmonics.
 *
 *  DaisySP's Wavefolder is the triangle alone, unfiltered; here the fold is antialiased
 *  with first-order ADAA (the fold's antiderivative, differenced), DC-blocked, and followed
 *  by a lowpass. The sine fold barely aliases anyway; on the triangle the ADAA takes off
 *  5-13dB, which at high drive on bright material still leaves some grit. The sine's and the
 *  triangle's antiderivatives are kept apart and blended with the current shape, so moving
 *  the shape knob doesn't put its change into the difference. The fold is scaled to a slope
 *  of 1 at 0 (2/pi for the sine, 1 for the triangle, blended with the shape), so at 1x a
 *  quiet signal comes out at its own level whatever the shape; driven harder, the output stays near
 *  full scale whatever goes in, and page 2's Level (FxOutput.h) sets how loud that is. Nothing
 *  follows the input's level. Fully wet while on (page 2's Mix blends in the dry signal).
 *  Params: 0 drive, 1 shape (sine to triangle), 2 tone, 3 stereo (the right channel driven up
 *  to 2x harder); page 2's own, 5 symmetry. */
class Folder : public FxBase
{
public:
    enum Param
    {
        DRIVE,
        SHAPE,
        TONE,
        STEREO,
        SYMMETRY = 5,
    };

    FX_ONCE void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        asleep_ = true;
        for (size_t c = 0; c < 2; c++)
        {
            u1_[c] = 0.f;
            Antiderivatives(0.f, &f1_sine_[c], &f1_tri_[c]);
            lp_[c] = 0.f;
            dc_[c].Init(sample_rate);
        }
        gate_.Init();

        for (size_t i = 0; i < kNumFxParams; i++)
            SetParam(i, 0.f);
        SnapParams();
    }

    void Process(float* l, float* r)
    {
        const float gate = gate_.Process();
        // off and faded out: nothing to do, the knobs where they're going. Back on, the fold
        // starts from this sample (its first one plain, no step to average over); the
        // fade-in covers the tone's start
        if (gate_.Asleep())
        {
            SnapParams();
            asleep_ = true;
            return;
        }
        const float drive_l = drive_.Process();
        const float drive_r = drive_l * stereo_.Process();
        const float shape = shape_.Process();
        const float bias = bias_.Process();
        const float tone_coeff = tone_coeff_.Process();
        float* const io[2] = {l, r};
        // the sine's slope at 0 is pi/2, the triangle's 1: both brought to 1
        const float unity = kSineUnity + shape * (1.f - kSineUnity);
        if (asleep_)
        {
            for (size_t c = 0; c < 2; c++)
            {
                u1_[c] = *io[c] * (c ? drive_r : drive_l) + bias;
                Antiderivatives(u1_[c], &f1_sine_[c], &f1_tri_[c]);
            }
            asleep_ = false;
        }

        for (size_t c = 0; c < 2; c++)
        {
            const float u = *io[c] * (c ? drive_r : drive_l) + bias;
            float f_sine, f_tri;
            Antiderivatives(u, &f_sine, &f_tri);
            // ADAA: the fold averaged over the step from the last sample, which takes some
            // of the aliasing out of the triangle's corners; the plain fold at the midpoint
            // when the step is too small to divide by. Both antiderivatives at this sample's
            // shape, so a moving shape adds nothing to the difference
            const float du = u - u1_[c];
            float y;
            if (fabsf(du) > kAdaaMinStep)
            {
                const float d_sine = (f_sine - f1_sine_[c]) / du;
                const float d_tri = (f_tri - f1_tri_[c]) / du;
                y = d_sine + shape * (d_tri - d_sine);
            }
            else
                y = Fold(.5f * (u + u1_[c]), shape);
            u1_[c] = u;
            f1_sine_[c] = f_sine;
            f1_tri_[c] = f_tri;

            // the bias's DC out, then the tone
            lp_[c] += tone_coeff * (dc_[c].Process(y) - lp_[c]);
            *io[c] += gate * (unity * lp_[c] - *io[c]);
        }
    }

    /** The slewed parameters jump to their targets, at Init */
    void SnapParams()
    {
        drive_.Snap();
        stereo_.Snap();
        shape_.Snap();
        bias_.Snap();
        tone_coeff_.Snap();
    }

    void SetParam(size_t param, float val) override
    {
        switch (param)
        {
        case DRIVE:
            // 1x (the sine slightly saturating, the triangle clean) to 32x, many folds
            drive_.target = powf(kMaxDrive, val);
            break;
        case SHAPE:
            shape_.target = val;
            break;
        case STEREO:
            stereo_.target = 1.f + val;
            break;
        case SYMMETRY:
            // up to a quarter of the fold's period: the sine becomes a cosine, all even
            bias_.target = val;
            break;
        case TONE:
            // lowpass from 200Hz to 20kHz, fully open at the top, as the crusher's
            tone_coeff_.target = ToneCoeff(val, sample_rate_);
            break;
        default:
            break;
        }
    }

private:
    static constexpr float kMaxDrive = 32.f;
    static constexpr float kAdaaMinStep = 1e-3f;
    static constexpr float kSineUnity = 2.f / PI_F;

    /** The fold: period 4, 0 at 0, 1 at 1, back through 0 at 2 to -1 at 3, so below 1 it's
     *  close to the input, past it mirrored back. A sine crossfaded into a triangle. */
    static inline float Fold(float u, float shape)
    {
        const float sine = sinf(HALFPI_F * u);
        const float v = Wrap(u);
        const float tri = v < 2.f ? v - 1.f : 3.f - v;
        return sine + shape * (tri - sine);
    }

    /** The sine fold's and the triangle fold's antiderivatives, for the ADAA */
    static inline void Antiderivatives(float u, float* sine, float* tri)
    {
        *sine = -(2.f / PI_F) * cosf(HALFPI_F * u);
        const float v = Wrap(u);
        *tri = v < 2.f ? .5f * v * v - v : 3.f * v - .5f * v * v - 4.f;
    }

    /** u + 1 wrapped into 0..4, where the triangle starts its period at -1 */
    static inline float Wrap(float u)
    {
        const float v = u + 1.f;
        return v - 4.f * floorf(.25f * v);
    }

    float sample_rate_;
    bool asleep_ = true;
    float u1_[2];                 // the last sample's fold input
    float f1_sine_[2], f1_tri_[2]; // and its antiderivatives
    float lp_[2];
    daisysp::DcBlock dc_[2];
    Smoothed drive_, shape_, bias_, tone_coeff_;
    Smoothed stereo_; // the right channel's drive over the left's
};

} // namespace chompi
