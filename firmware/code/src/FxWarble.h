/** @file FxWarble.h
 *  @brief Wow & flutter.
 *
 *  Ported from CHOMPI TAPE's Warble.h (its SHIFT menu's warble knob), with flutter, tone and
 *  stereo added.
 */
#pragma once
#include "FxCommon.h"

namespace chompi
{

// TAPE's wow: a new delay target of kWowMinFrames up to kWowMinFrames + kWowSpanFrames, chosen
// at random rate * 30 + .1 times a second, followed at a random slew of up to kWowMaxCoeff
static const float kWowMinFrames = 100.f;
static const float kWowSpanFrames = 880.f;
static const float kWowMaxCoeff = .0001f;
// Flutter: two wobbles, a capstan's and a pinch roller's, up to kFlutterMaxFrames, .21ms
// together. That's 1.4% of pitch at the top; real tape's .1-.5% is the knob's lower half
static const float kFlutterMaxFrames = 10.f;
static const float kFlutterHz[2] = {7.3f, 11.7f};
static const float kFlutterWeights[2] = {1.f / 1.4f, .4f / 1.4f};

/** TAPE's warble: a short delay whose length wanders to random targets at random speeds, mixed
 *  with the input, so the pitch drifts like a worn tape's. Knob 1 is TAPE's knob: it sets how
 *  often the delay wanders and the mix together. Flutter adds a fast, shallow wobble; it
 *  brings the wet signal in over the first quarter of its knob, so it works on its own.
 *  Params: 0 wow, 1 flutter, 2 tone, 3 stereo; page 2's own, 5 age: worn tape's dropouts,
 *  the level dipping at random, more often, deeper and longer as it turns up (Cassette
 *  Sim's AGE on the SP-404MK2). */
class Warble : public FxBase
{
public:
    enum Param
    {
        WOW,
        FLUTTER,
        TONE,
        STEREO,
        AGE = 5,
    };

    FX_ONCE void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        ring_.Clear();
        gate_.Init();
        rand_ = 1;
        for (size_t c = 0; c < 2; c++)
        {
            // halfway along TAPE's range, still
            walk_[c].pos = walk_[c].target = kWowMinFrames + .5f * kWowSpanFrames;
            walk_[c].coeff = 0.f;
            lp_[c] = 0.f;
            phase_[c] = 0.f;
            inc_[c] = kFlutterHz[c] / sample_rate_;
        }

        for (size_t i = 0; i < kNumFxParams; i++)
            SetParam(i, 0.f);
        SetParam(TONE, 1.f);
        SnapParams();
    }

    FRIZZ_HOT void Process(float* l, float* r)
    {
        const float gate = gate_.Process();

        // TAPE: now and then a new target and slew. None while the wow is off, so the flutter
        // alone is just the flutter. The right channel's walk is heard with the stereo knob.
        for (size_t c = 0; c < 2; c++)
        {
            Walk& w = walk_[c];
            if (wow_ && RandUnit() < chance_)
            {
                w.target = kWowMinFrames + RandUnit() * kWowSpanFrames;
                w.coeff = RandUnit() * kWowMaxCoeff;
            }
            fonepole(w.pos, w.target, w.coeff);
        }

        // flutter: the right channel's wobbles up to a quarter cycle behind with the stereo knob
        for (size_t i = 0; i < 2; i++)
        {
            phase_[i] += inc_[i];
            if (phase_[i] >= 1.f)
                phase_[i] -= 1.f;
        }
        // off: the walk, the flutter's phases and the buffer go on, so a punch-in starts where
        // it would have; the tone filter follows the dry signal, close to what it would hear
        if (gate_.Asleep())
        {
            SnapParams(); // the knobs where they're going
            ring_.WriteFrame(*l, *r);
            lp_[0] = *l;
            lp_[1] = *r;
            return;
        }
        const float mix = mix_.Process();
        const float depth = depth_.Process();
        const float tone = tone_.Process();
        const float stereo = stereo_.Process();

        float flutter[2];
        for (size_t c = 0; c < 2; c++)
        {
            const float offset = c == 0 ? 0.f : .25f * stereo;
            flutter[c] = kFlutterWeights[0] * Sine(phase_[0] - offset)
                         + kFlutterWeights[1] * Sine(phase_[1] - offset);
        }

        const float wow[2] = {walk_[0].pos, walk_[0].pos + stereo * (walk_[1].pos - walk_[0].pos)};
        // age: a dropout now and then, held for a while, the level gliding down and back
        float drop = 1.f;
        if (age_ > 0.f || drop_ < 1.f)
        {
            if (drop_left_ > 0)
                drop_left_--;
            else
            {
                drop_target_ = 1.f;
                if (age_ > 0.f && RandUnit() < age_ * kDropsPerSample)
                {
                    const float depth = age_ * (.3f + .7f * RandUnit());
                    drop_target_ = 1.f - depth;
                    drop_left_ = static_cast<uint32_t>(sample_rate_ * (.02f + .2f * age_ * RandUnit()));
                }
            }
            fonepole(drop_, drop_target_, kDropCoeff);
            if (drop_ > 1.f - 1e-5f && drop_target_ == 1.f)
                drop_ = 1.f;
            drop = drop_;
        }
        float* const io[2] = {l, r};
        for (size_t c = 0; c < 2; c++)
        {
            ring_.Write(c, *io[c]);
            const float wet = ring_.Read(c, wow[c] + depth * flutter[c]);
            fonepole(lp_[c], wet, tone);

            const float out = *io[c] + mix * (lp_[c] * drop - *io[c]);
            *io[c] += gate * (out - *io[c]);
        }
        ring_.Advance();
    }

    /** The slewed parameters jump to their targets, at Init */
    void SnapParams()
    {
        mix_.Snap();
        depth_.Snap();
        tone_.Snap();
        stereo_.Snap();
    }

    void SetParam(size_t param, float val) override
    {
        switch (param)
        {
        case WOW:
            wow_val_ = val;
            wow_ = val > 0.f;
            // TAPE: SetFreq, a chance per sample
            chance_ = (val * 30.f + .1f) / sample_rate_;
            break;
        case FLUTTER:
            flutter_val_ = val;
            depth_.target = kFlutterMaxFrames * val * val;
            break;
        case TONE:
            tone_.target = ToneCoeff(val, sample_rate_);
            break;
        case STEREO:
            stereo_.target = val;
            break;
        case AGE:
            age_ = val;
            break;
        default:
            break;
        }
        // the wet signal comes in with the wow, the flutter or the age, whichever is up most
        mix_.target = fmaxf(fmaxf(wow_val_, fminf(4.f * flutter_val_, 1.f)), fminf(4.f * age_, 1.f));
    }

private:
    static const size_t kBufSize = 2048; // TAPE's longest delay, 980 frames, plus the flutter
    static constexpr float kDropsPerSample = 3.f / 48000.f; // at full age, 3 a second
    static constexpr float kDropCoeff = .005f;               // ~4ms into and out of one

    struct Walk
    {
        float pos, target, coeff;
    };

    /** TAPE's vinyl_rand, 0..2^31 - 1 */
    uint32_t Rand()
    {
        rand_ = (1103515245u * rand_ + 12345u) & 0x7fffffffu;
        return rand_;
    }
    /** The same as 0..1 */
    float RandUnit() { return static_cast<float>(Rand()) * 4.656612873077392578125e-10f; } // 1 / 2^31

    /** A parabolic sine, close enough for a wobble, of a phase in cycles (-1..1) */
    static float Sine(float phase)
    {
        if (phase < 0.f)
            phase += 1.f;
        const float x = 2.f * phase - 1.f; // -1..1
        return -4.f * x * (1.f - fabsf(x));
    }

    float sample_rate_;
    StereoRing<kBufSize> ring_;
    uint32_t rand_;
    Walk walk_[2];
    float lp_[2];
    float phase_[2];
    float inc_[2] = {0.f, 0.f};
    float chance_ = 0.f;
    bool wow_ = false;
    float wow_val_ = 0.f, flutter_val_ = 0.f;
    float age_ = 0.f;
    float drop_ = 1.f, drop_target_ = 1.f; // the dropout's level, gliding
    uint32_t drop_left_ = 0;               // samples the dropout still holds
    Smoothed mix_;
    Smoothed depth_;
    Smoothed tone_;
    Smoothed stereo_;
};

} // namespace chompi
