/** @file FxOutput.h
 *  @brief Page 2's knobs every effect shares, on the same knobs for each, so they're found
 *  without looking (FxParams.h): knob 1 Mix, knob 3 Band, knob 4 Level. FxChain.h wraps each
 *  insert in one; the sends use only its Band, on what goes into them.
 *
 *  - Mix: the dry signal against the effect's, 0 dry to 1 (the default) fully the effect's.
 *  - Band: which part of the spectrum the effect works on. In the middle (the default) all of
 *    it; turned left, only the lows, below a crossover that falls from 20kHz to 40Hz; turned
 *    right, only the highs, above one that rises from 20Hz to 8kHz. The rest passes dry. Two
 *    one-pole lowpasses: the effect gets the difference of the two, the rest is the signal
 *    minus that, so with the effect neutral the two add up to the signal exactly.
 *  - Level: the effect's output, not the dry signal Mix blends in: mute at 0, then -36dB to
 *    +12dB, 0dB at 3/4 (the default).
 *
 *  Nothing here adjusts itself: what comes out is what the knobs say. The key's fade (the
 *  effect's FxGate) applies to Mix and Level as to the effect, so an effect that's off is
 *  untouched by them, and at their defaults the effect runs exactly as without them.
 */
#pragma once
#include "FxCommon.h"

namespace chompi
{

class FxOutput
{
public:
    // their parameters, on page 2 (knobs 1, 3 and 4)
    static const size_t kMix = kNumFxKnobs, kBand = kNumFxKnobs + 2, kLevel = kNumFxKnobs + 3;
    // the knob values that leave the effect as it is
    static constexpr float kMixDefault = 1.f, kBandDefault = .5f, kLevelDefault = .75f;

    void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        mix_.Reset(kMixDefault);
        level_.Reset(1.f);
        lo_.Reset(0.f);
        hi_.Reset(1.f);
        for (size_t c = 0; c < 2; c++)
            lp_lo_[c] = lp_hi_[c] = 0.f;
        awake_ = band_ = false;
        neutral_ = true;
        moving_ = false;
    }

    /** One of page 2's shared knobs, 0..1; false if param isn't one */
    __attribute__((noinline, optimize("Os"))) bool SetParam(size_t param, float val)
    {
        switch (param)
        {
        case kMix:
            mix_.target = val;
            moving_ = true;
            return true;
        case kLevel:
            level_.target = LevelGain(val);
            moving_ = true;
            return true;
        case kBand:
            if (val < kBandDefault)
            {
                // the lows: everything below the upper crossover
                lo_.target = 0.f;
                hi_.target = OnePoleCoeff(40.f * powf(500.f, 2.f * val), sample_rate_);
            }
            else if (val > kBandDefault)
            {
                // the highs: everything above the lower crossover
                lo_.target = OnePoleCoeff(20.f * powf(400.f, 2.f * (val - kBandDefault)), sample_rate_);
                hi_.target = 1.f;
            }
            else
            {
                lo_.target = 0.f;
                hi_.target = 1.f;
            }
            moving_ = true;
            return true;
        default:
            return false;
        }
    }

    /** One sample of fx (an insert, with Process(l, r), Quiet() and Fade(): FxBase's, or the
     *  tape stop's own) in place, with Mix, Band and Level around it */
    template <class Fx>
    inline void Process(Fx& fx, float* l, float* r)
    {
        const bool split = Begin(fx.Quiet(), l, r);
        fx.Process(l, r);
        End(split, l, r, fx.Fade());
    }

    /** Process in two halves, for FxChain.h, which calls them only while Busy. Before the
     *  effect (idle: its Quiet()): the knobs' slew, and the band it gets split off, unless at
     *  their defaults or with the effect off and faded out it runs alone, bit for bit as
     *  without them (then false) */
    inline bool Begin(bool idle, float* l, float* r)
    {
        const bool plain = Plain(idle);
        if (!plain)
            Split(l, r);
        return !plain;
    }

    /** After the effect: what Begin split off joined back (split: what Begin returned), with
     *  fade the effect's Fade(). Returns Busy() */
    inline bool End(bool split, float* l, float* r, float fade)
    {
        if (split)
            Join(l, r, fade);
        return Busy();
    }

    /** Off their defaults or still slewing: at rest at the defaults, the effect runs alone
     *  and FxChain.h leaves this out (SetParam makes it busy again) */
    inline bool Busy() const { return moving_ || !neutral_; }
    /** A knob turned and not yet settled */
    inline bool Moving() const { return moving_; }
    /** The effect comes on again after it was quiet: the crossovers start afresh from the
     *  next sample, not from what they held when it went quiet. A send's Band (FxChain::
     *  SendBand) isn't run while the effect is idle, so only this tells it */
    inline void Rest() { awake_ = false; }

    /** The knobs' slewed values jump to their targets (tests) */
    void Snap()
    {
        mix_.Snap();
        level_.Snap();
        lo_.Snap();
        hi_.Snap();
        moving_ = true; // Slew works out what follows
    }

    /** A send's input: its Band alone. In place, l and r. Returns Banding() */
    inline bool Band(float* l, float* r)
    {
        if (moving_)
            Slew();
        if (!band_)
        {
            awake_ = false;
            return Banding();
        }
        float* const io[2] = {l, r};
        for (size_t c = 0; c < 2; c++)
        {
            if (!awake_)
                Wake(c, *io[c]); // the crossovers start from here once the knob turns
            *io[c] = Crossover(c, *io[c]);
        }
        awake_ = true;
        return Banding();
    }

    /** Band off its default or still slewing: at rest in the middle, Band leaves the signal
     *  as it is and FxChain.h leaves it out */
    inline bool Banding() const { return moving_ || band_; }

    /** Level's knob 0..1 as a gain: 0 mutes, then -36dB to +12dB, exactly 1 at its default */
    static float LevelGain(float val)
    {
        if (val <= 0.f)
            return 0.f;
        if (val == kLevelDefault)
            return 1.f;
        return pow10f(48.f * (val - kLevelDefault) * .05f);
    }

private:
    /** Before the effect: whether it runs alone; the knobs' slew. At rest a compare each */
    __attribute__((always_inline)) inline bool Plain(bool idle)
    {
        // the knobs slew only after a turn (moving_), so at rest this is a flag and a compare
        if (moving_)
            Slew();
        const bool plain = neutral_ || idle;
        if (plain)
            awake_ = false;
        return plain;
    }

    /** After a turn: the knobs one step on, and whether they rest at their defaults. The
     *  flag is cleared first, so a turn meanwhile sets it again */
    __attribute__((noinline)) void Slew()
    {
        moving_ = false;
        bool moving = mix_.Settle();
        moving |= level_.Settle();
        moving |= lo_.Settle();
        moving |= hi_.Settle();
        band_ = lo_.value != 0.f || hi_.value != 1.f;
        neutral_ = mix_.value == 1.f && level_.value == 1.f && !band_;
        if (moving)
            moving_ = true;
    }

    /** Before the effect: the signal kept, and the band the effect gets in its place */
    FRIZZ_HOT inline void Split(float* l, float* r)
    {
        float* const io[2] = {l, r};
        for (size_t c = 0; c < 2; c++)
        {
            // the crossovers start from here: only the effect's share fades in, so whatever
            // they held from before can't be heard
            if (!awake_)
                Wake(c, *io[c]);
            x_[c] = *io[c];
        }
        awake_ = true;
        if (!band_)
            return; // the effect gets it all: the signal as it is (Join takes x_ for the band)
        for (size_t c = 0; c < 2; c++)
        {
            b_[c] = Crossover(c, x_[c]);
            *io[c] = b_[c];
        }
    }

    /** Channel c's x through the crossovers: the band between them, the upper one's
     *  lowpass less the lower one's */
    FRIZZ_HOT inline float Crossover(size_t c, float x)
    {
        lp_hi_[c] += hi_.value * (x - lp_hi_[c]);
        lp_lo_[c] += lo_.value * (x - lp_lo_[c]);
        return lp_hi_[c] - lp_lo_[c];
    }
    /** They start from x, as if it had always been there: the band passes it whole */
    inline void Wake(size_t c, float x)
    {
        lp_hi_[c] = x;
        lp_lo_[c] = 0.f;
    }

    /** After it: the signal, with the band crossfaded by Mix into the effect's output at its
     *  Level. Level is the effect's alone, as the SP-404's: Mix at 0 is the signal untouched.
     *  Level fades in and out with the key (fade) as the effect does */
    FRIZZ_HOT inline void Join(float* l, float* r, float fade)
    {
        // the fade only nears 1 (fonepole): its last 1e-4 (-80dB) taken as 1, so Level 0 mutes
        if (fade > 1.f - 1e-4f)
            fade = 1.f;
        const float gain = 1.f + fade * (level_.value - 1.f);
        const float mix = mix_.value;
        const float* const b = band_ ? b_ : x_; // Split's band, or without one all of it
        float* const io[2] = {l, r};
        for (size_t c = 0; c < 2; c++)
            *io[c] = x_[c] + mix * (gain * *io[c] - b[c]);
    }

    float sample_rate_;
    Smoothed mix_, level_;
    Smoothed lo_, hi_;           // the crossovers' one-pole coefficients: 0 is no lows cut,
                                 // 1 no highs cut
    float lp_lo_[2], lp_hi_[2];  // their lowpasses
    bool awake_;                 // whether the crossovers ran last sample
    bool band_;                  // whether they're on now
    bool neutral_ = true;        // Mix, Band and Level all at their defaults
    volatile bool moving_ = false; // a knob turned and not yet settled (SetParam, Slew)
    float x_[2], b_[2];          // this sample's input, and the band the effect got
};

} // namespace chompi
