/** @file MasterComp.h
 *  @brief The master compressor: always on, after the punch-in FX and before the output gain
 *  (passthroughEngine.h), so VOLUME doesn't change how hard it works. Its knobs are edited like
 *  an FX's from its own key (FxControls.h, kCompParams in FxParams.h) and kept on the card
 *  (MasterSettings.h), not in the scenes.
 *
 *  One stereo-linked detector, the louder channel's peak, so the image doesn't shift, held for
 *  kHoldMs so a fast attack doesn't follow a bass note's waveform (crackle). Above the
 *  threshold, the gain falls by the ratio, with a soft knee. The reduction is smoothed in dB,
 *  so the release recovers evenly however deep it went. The reference is a signal of 1.0 at
 *  this point, about a hot line input at the default input gain. A plain compressor: the
 *  makeup is a knob, nothing turns anything up by itself.
 *
 *  Knobs, each 0..1, on two pages as an FX's (page 2's Mix and Makeup on the knobs an FX has
 *  its Mix and Level on, FxOutput.h):
 *   0 threshold: 0dB down to -30dB. 0 is off: with the makeup at 0 too, an exact bypass
 *   1 ratio: 1.5:1 to 20:1, 4:1 in the middle
 *   2 attack: 1-30ms (time constant)
 *   3 release: 40-600ms (time constant)
 *   4 mix: dry to fully compressed, for parallel compression
 *   6 sidechain highpass: what the detector hears, off (0) or from 20Hz up to 500Hz, so the
 *     bass doesn't pump the rest (page 2's knob 3, where an FX has its Band)
 *   7 makeup: 0dB to +24dB
 *
 *  The safety limiter after the output gain is the old master compressor at its lowest
 *  setting (limiter.h); this is in front of it.
 */
#pragma once
#include <math.h>
#include "daisysp.h"
#include "FxCommon.h"

namespace chompi
{

class MasterComp
{
public:
    static const size_t kThreshold = 0, kRatio = 1, kAttack = 2, kRelease = 3, kMix = 4,
                        kSidechain = 6, kMakeup = 7;

    void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        reduction_ = 0.f;
        gain_db_ = 0.f;
        gain_ = 1.f;
        held_ = 0.f;
        hold_left_ = 0;
        sc_coeff_ = 0.f;
        sc_lp_[0] = sc_lp_[1] = 0.f;
        hold_samples_ = static_cast<uint32_t>(kHoldMs * .001f * sample_rate);
        for (size_t p = 0; p < kNumFxParams; p++)
            knobs_[p].Reset(0.f);
        knobs_[kMix].Reset(1.f);
        update_pending_ = false;
        since_update_ = 0;
        Update();
    }

    /** From the UI, 0..1 */
    inline void SetParam(size_t param, float val)
    {
        knobs_[param].target = val;
        turned_ = true;
    }

    /** One stereo sample, in place */
    void Process(float* l, float* r)
    {
        // the knobs slew; what follows from them (several powf and expf) is worked out while
        // one moves, once per kUpdateSamples, and once more where they land
        // only after a turn (turned_, cleared first so a turn meanwhile sets it again)
        bool moving = false;
        if (turned_)
        {
            turned_ = false;
            for (size_t p = 0; p < kNumFxParams; p++)
            {
                update_pending_ |= knobs_[p].Settle(kFxParamCoeff);
                moving |= knobs_[p].value != knobs_[p].target;
            }
            if (moving)
                turned_ = true;
        }
        if (update_pending_ && (!moving || ++since_update_ >= kUpdateSamples))
        {
            Update();
            update_pending_ = false;
            since_update_ = 0;
        }

        // off: a bypass, once a reduction left from before has released, so turning it off
        // under a hot signal doesn't click; with makeup it's that gain alone
        const bool off = knobs_[kThreshold].value == 0.f;
        if (off && makeup_db_ == 0.f && reduction_ > -kOffDb)
        {
            reduction_ = 0.f;
            held_ = 0.f;
            hold_left_ = 0;
            return;
        }

        // the louder channel's peak, held for hold_samples_ before it may fall; through the
        // sidechain's highpass where it's on (a one-pole lowpass taken off)
        float dl = *l, dr = *r;
        if (sc_coeff_ > 0.f)
        {
            sc_lp_[0] += sc_coeff_ * (dl - sc_lp_[0]);
            sc_lp_[1] += sc_coeff_ * (dr - sc_lp_[1]);
            dl -= sc_lp_[0];
            dr -= sc_lp_[1];
        }
        else
        {
            sc_lp_[0] = dl;
            sc_lp_[1] = dr;
        }
        const float now = fmaxf(fabsf(dl), fabsf(dr));
        if (now >= held_)
        {
            held_ = now;
            hold_left_ = hold_samples_;
        }
        else if (hold_left_ > 0)
            hold_left_--;
        else
            held_ = now;
        const float peak = held_;

        // the reduction that peak calls for, <= 0, and then the attack (more) or release (less)
        // towards it
        float target = 0.f;
        if (!off && peak > knee_start_)
        {
            const float over = 6.0206f * daisysp::fastlog2f(peak) - thresh_db_;
            const float slope = 1.f / ratio_ - 1.f;
            target = over < kKneeDb * .5f
                         ? slope * (over + kKneeDb * .5f) * (over + kKneeDb * .5f) / (2.f * kKneeDb)
                         : slope * over;
        }
        reduction_ += (target < reduction_ ? attack_ : release_) * (target - reduction_);
        // released all but 1e-6 dB (a gain 140 dB from 1): released, so the gain below stops
        // moving, rather than creeping through ever tinier values for seconds
        if (target == 0.f && reduction_ > -kRestDb)
            reduction_ = 0.f;
        // the gain only when its dB moved: at rest (no reduction, the knobs still) it doesn't,
        // and the expf is most of what the compressor costs
        const float gain_db = reduction_ + makeup_db_;
        if (gain_db != gain_db_)
        {
            gain_db_ = gain_db;
            gain_ = daisysp::pow10f(gain_db * .05f);
        }
        const float gain = gain_;

        const float mix = knobs_[kMix].value;
        *l += (*l * gain - *l) * mix;
        *r += (*r * gain - *r) * mix;
    }

    /** n stereo samples, in place: the compressor's code in the I-cache for all of them (#136) */
    void Process(float* l, float* r, size_t n)
    {
        for (size_t i = 0; i < n; i++)
        {
            float sl = l[i], sr = r[i];
            Process(&sl, &sr);
            l[i] = sl;
            r[i] = sr;
        }
    }

    /** The gain reduction now, in dB (<= 0), for the key's LED */
    inline float GetReduction() const { return reduction_; }

private:
    static constexpr float kKneeDb = 6.f;
    static constexpr float kOffDb = .01f; // a reduction this small is gone
    static constexpr float kRestDb = 1e-6f; // and this small, released
    static constexpr float kMaxThreshDb = 30.f;
    static constexpr float kMaxMakeupDb = 24.f;
    static constexpr float kHoldMs = 10.f; // a half-cycle of 50Hz
    static constexpr uint32_t kUpdateSamples = 24; // an audio block: 0.5ms, far below the slew

    /** The threshold, ratio, makeup and envelope times from the knobs */
    void Update()
    {
        static const float kRatioX[] = {0.f, .25f, .5f, .75f, 1.f};
        static const float kRatioY[] = {1.5f, 2.f, 4.f, 8.f, 20.f};
        thresh_db_ = -kMaxThreshDb * knobs_[kThreshold].value;
        ratio_ = CurveMap(knobs_[kRatio].value, kRatioX, kRatioY, 5);
        makeup_db_ = kMaxMakeupDb * knobs_[kMakeup].value;
        const float sc = knobs_[kSidechain].value;
        sc_coeff_ = sc > 0.f ? OnePoleCoeff(20.f * powf(25.f, sc), sample_rate_) : 0.f;
        // below where the knee starts nothing is reduced: no log needed
        knee_start_ = daisysp::pow10f((thresh_db_ - kKneeDb * .5f) * .05f);

        const float attack_ms = powf(30.f, knobs_[kAttack].value);
        const float release_ms = 40.f * powf(15.f, knobs_[kRelease].value);
        attack_ = 1.f - expf(-1000.f / (attack_ms * sample_rate_));
        release_ = 1.f - expf(-1000.f / (release_ms * sample_rate_));
    }

    float sample_rate_;
    bool update_pending_ = false; // a knob moved since the last Update
    volatile bool turned_ = false; // a knob turned and not yet settled
    uint32_t since_update_ = 0;
    Smoothed knobs_[kNumFxParams];
    float reduction_;  // dB, <= 0, smoothed
    float held_;       // the detector's held peak
    uint32_t hold_left_, hold_samples_;
    float thresh_db_, ratio_, makeup_db_, knee_start_;
    float attack_, release_;
    float sc_coeff_;  // the sidechain highpass's lowpass coefficient, 0: off
    float sc_lp_[2];
    float gain_db_ = 0.f, gain_ = 1.f; // the last gain worked out, and its dB
};

} // namespace chompi
