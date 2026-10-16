/** @file Finalizer.h
 *  @brief The finalizer (#345): always there, after the punch-in FX and before the output
 *  gain (passthroughEngine.h), so VOLUME doesn't change how hard it works. It replaced the
 *  master compressor in 1.5. Its knobs are edited like an FX's from its own key, four modules
 *  of four knobs (FxControls.h, kFinalizerKnobs in FxParams.h), kept on the card
 *  (MasterSettings.h), not in the scenes. After Endorphin.es' Golden Master and OTO's BOUM.
 *
 *  The chain, in a fixed order: Input -> EQ -> Comp -> Width -> Drive -> Limiter. The middle
 *  three share a split into three bands at kSplitLow and kSplitHigh, Linkwitz-Riley 4th order
 *  (two state-variable filters in a row each way), the low band through the high split's
 *  allpass, so the bands sum back to an allpass, flat. The split works on mid and side
 *  (M = (L + R) / 2, S = (L - R) / 2): EQ and Comp scale a band's M and S alike, Width its S
 *  alone, and the louder channel's peak, which each band's compressor hears so the image
 *  doesn't shift, is |M| + |S|.
 *  - Input: a clean trim, -12 to +12dB, 0 at the centre.
 *  - EQ: each band's level, -20 to +8dB, 0 at the centre.
 *  - Comp: each band's character (kCharacters: attack, release, ratio, knee, a threshold of its
 *    own, parallel or not; the low band's times kLowSlower slower) and Depth, the three
 *    thresholds down together, 0 off. The gain computers run once a block (control rate), a
 *    gain glides linearly over the block to the next; per sample they would cost too much.
 *    Parallel mixes the heavily compressed band half and half with the dry, keeping its
 *    transients. No makeup: nothing turns anything up by itself.
 *  - Width: each band's side, 0 (mono) at the left, as it is at the centre, up to kMaxWidth
 *    (the low band kMaxLowWidth) at the right.
 *  - Drive: 0 to +18dB into a saturation of the type's (Saturate), run at twice the rate
 *    (polyphase IIR halfbands, as hiir's) so its harmonics don't fold back, then a gentle
 *    hi-cut. A master's warmth, not an amp's: Boost a soft symmetric curve, low odd harmonics
 *    (the third); Tube an asymmetric one whose second harmonic grows with the drive and
 *    stays above its third; Fuzz a harder knee, the densest, odd and even. Each type's knee
 *    sits at its own level (kTypeGain), so that even the top is a lot but not a square wave.
 *    Level-compensated by a gain per drive and type: a sine of kDriveRef comes out as loud
 *    as it went in, other levels near it. Changing the type while it drives dips the drive
 *    out, holds it there for kTypeHoldMs while the new type's DC settles in the blocker after
 *    it, and fades it back in, so the switch doesn't thump.
 *  - Limiter, Loud: 0 to +18dB into a limiter with a lookahead of kLookahead (1ms), its
 *    ceiling kCeiling (-1dB of the reference, with a margin of .3dB for the peaks between
 *    samples, which its detector estimates at twice the rate), its release automatic: longer the
 *    longer it limits. The gain it needs is held over the lookahead and smoothed by two box
 *    filters (as Signalsmith's limiter), so it's down before the peak comes out.
 *  The reference is a signal of 1.0 here, about a hot line input at the default input gain;
 *  at VOLUME full the safety limiter (limiter.h) starts a little below the ceiling.
 *
 *  Neutral, a module sleeps: every knob on its default leaves the samples untouched, an exact
 *  bypass costing a test a block. What changes the phase or the latency fades in and out over
 *  kFadeMs, the bands, the drive and the limiter's delay alike, so nothing clicks: the bands
 *  are an allpass, the drive's halfbands delay a little, and while Loud is up the output is
 *  kLookahead later (Latency, which MIDI clock out follows, MidiOut.h). The limiter's delay
 *  fills before it fades in. The bands run on, the output untouched, for kBandsHoldMs after
 *  fading out, and while a meter needs them (SetMeter), so a knob nudged off and back doesn't
 *  restart them. Bypassed (SetBypass), every module fades out the same way. A band muted
 *  (SetMute, #347) fades out over kMuteMs, as a kill on a DJ mixer.
 *
 *  The meters, for the knob LEDs and key 15 (NormalPage.h), read from MainLoop: the input's
 *  peak, the bands' peaks after the EQ, their gain reductions and correlation (+1 mono, 0
 *  unrelated, -1 out of phase), the limiter's gain reduction.
 */
#pragma once
#include <math.h>
#include <stdint.h>
#include "daisysp.h"
#include "FrizzHot.h"
#include "FxCommon.h"
#include "FxParams.h"

// once a block or a turn, not per sample: small
#define FIN_ONCE __attribute__((noinline, optimize("Os")))

namespace chompi
{

class Finalizer
{
public:
    // parameter m * kNumFxKnobs + k: module m's knob k (kFinalizerKnobs)
    enum Param : uint8_t
    {
        INPUT = 0,
        DRIVE = 1,
        TYPE = 2,
        LOUD = 3,
        EQ_LOW = 4, // 4-6: low, mid, high
        CHARACTER_LOW = 8, // 8-10
        DEPTH = 11,
        WIDTH_LOW = 12, // 12-14
    };
    enum Module : uint8_t
    {
        LEVEL = 0,
        EQ = 1,
        COMP = 2,
        WIDTH = 3,
        kNoMeter = 4,
    };
    enum Type : uint8_t
    {
        BOOST = 0,
        TUBE = 1,
        FUZZ = 2,
    };
    static const size_t kBands = 3;
    static const uint32_t kLookahead = 48; // 1ms
    static constexpr float kCeiling = .8610f; // -1dB, less .3dB for the peaks between samples

    FIN_ONCE void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        for (size_t p = 0; p < kNumFinParams; p++)
            knobs_[p].Reset(kFinalizerKnobs[p / kNumFxKnobs].defaults[p % kNumFxKnobs]);
        Svf::Coeffs(split_[0], kSplitLow, sample_rate);
        Svf::Coeffs(split_[1], kSplitHigh, sample_rate);
        hicut_ = OnePoleCoeff(kHiCutHz, sample_rate);
        dc_ = OnePoleCoeff(kDcHz, sample_rate);
        turned_ = update_pending_ = bypass_ = false;
        since_update_ = 0;
        meter_ = kNoMeter;
        trim_from_ = band_from_ = drive_from_ = lim_from_ = 0.f;
        type_ = gain_type_ = comp_type_ = BOOST;
        type_hold_ = 0;
        comp_gain_ = -1.f; // worked out on the first block
        bands_run_ = bands_apply_ = drive_run_ = false;
        release_lim_ = 0.f;
        fade_step_ = 1000.f / (kFadeMs * sample_rate);
        mute_step_ = 1000.f / (kMuteMs * sample_rate);
        Update();
        Clear();
        awake_ = true;
    }

    /** From the UI, 0..1: parameter p (Param). A stepped one (the type, the characters) at once */
    inline void SetParam(size_t p, float val)
    {
        if (p >= kNumFinParams)
            return;
        knobs_[p].target = val;
        if (p == TYPE || (p >= CHARACTER_LOW && p < DEPTH))
            knobs_[p].value = val;
        turned_ = true;
        awake_ = true;
    }
    /** The whole finalizer bypassed (true) or not: its modules fade out, or back in */
    inline void SetBypass(bool bypass)
    {
        bypass_ = bypass;
        awake_ = true;
    }
    inline bool Bypassed() const { return bypass_; }
    /** Band b muted (true) or not, faded over kMuteMs (#347) */
    inline void SetMute(size_t b, bool mute)
    {
        if (b < kBands)
            mute_[b] = mute;
        awake_ = true;
    }
    inline bool Muted(size_t b) const { return mute_[b]; }
    /** The module whose meters the knob LEDs show (Module), kNoMeter for none: the bands run
     *  for EQ's and Width's, the output untouched */
    inline void SetMeter(uint8_t module)
    {
        meter_ = module;
        awake_ = true;
    }

    /** n stereo samples, in place. Asleep, the samples stay as they are */
    inline void Process(float* l, float* r, size_t n)
    {
        if (awake_)
            ProcessAwake(l, r, n);
        else if (meter_ == LEVEL)
            InputPeak(l, r, n);
    }

    /** The output's delay now, in samples: kLookahead while Loud is up (and not bypassed), as
     *  the limiter's delay fades in or is in, else 0. For MIDI clock out (MidiOut.h) */
    inline uint32_t Latency() const { return lim_want_ ? kLookahead : 0; }

    // ---- the meters, from MainLoop ----
    /** The input's peak after the trim, since the last call (full scale 1) */
    inline float TakeInputPeak()
    {
        const float p = in_peak_;
        in_peak_ = 0.f;
        return p;
    }
    /** Band b's peak after the EQ, since the last call */
    inline float TakeBandPeak(size_t b)
    {
        const float p = band_peak_[b];
        band_peak_[b] = 0.f;
        return p;
    }
    /** Band b's gain reduction now, dB (<= 0) */
    inline float BandReduction(size_t b) const { return gr_db_[b]; }
    /** Band b's correlation after Width, smoothed: +1 mono, 0 unrelated, -1 out of phase */
    inline float BandCorrelation(size_t b) const { return corr_[b]; }
    /** A correlation's LED (NormalPage.h): 0 green, mono or nearly (above .9); 1 yellow, a
     *  normal width (above .3); 2 pink, very wide, the channels nearly unrelated (above -.3);
     *  3 red, out of phase: it would cancel in mono. Unrelated channels read 0 give or take
     *  .15 in the low band, which has few cycles in the meter's time: pink, not red */
    static inline int CorrelationLed(float corr)
    {
        return corr > .9f ? 0 : corr > .3f ? 1 : corr > -.3f ? 2 : 3;
    }
    /** The limiter's deepest gain reduction since the last call, dB (<= 0) */
    inline float TakeLimiterReduction()
    {
        const float g = lim_min_;
        lim_min_ = 1.f;
        return g < 1.f ? 6.0206f * daisysp::fastlog2f(g) : 0.f;
    }
    /** The deepest gain reduction now, a band's or the limiter's, dB (<= 0), for key 15 */
    inline float GetReduction() const
    {
        float r = lim_gr_db_;
        for (size_t b = 0; b < kBands; b++)
            r = fminf(r, gr_db_[b]);
        return r;
    }
    /** Every module asleep: the samples untouched */
    inline bool Asleep() const { return !awake_; }

private:
    static constexpr float kSplitLow = 300.f, kSplitHigh = 3000.f;
    static constexpr float kInputDb = 12.f;
    static constexpr float kEqCutDb = 20.f, kEqBoostDb = 8.f;
    static constexpr float kDepthDb = 30.f;
    static constexpr float kDriveDb = 18.f;
    static constexpr float kLoudDb = 18.f;
    static constexpr float kMaxWidth = 2.f, kMaxLowWidth = 1.5f;
    static constexpr float kLowSlower = 1.5f;
    static constexpr float kFadeMs = 10.f;
    static constexpr float kMuteMs = 5.f;
    static constexpr float kBandsHoldMs = 500.f;
    static constexpr float kHiCutHz = 10000.f, kDcHz = 10.f;
    static constexpr float kDriveRef = .5f; // the sine the drive's compensation is set for
    static constexpr float kTypeHoldMs = 50.f; // 3 time constants of the DC blocker (kDcHz)
    static constexpr float kRestDb = .01f; // a gain reduction this small is released
    static constexpr float kCorrMs = 150.f; // the correlation meter's time constant
    static const uint32_t kUpdateSamples = 24;
    static const uint32_t kBox = 21; // each of the limiter's two box filters
    static const uint32_t kQueue = 64; // the limiter's running minimum, > kLookahead + 1

    /** A compressor character: times in ms, ratio, knee and its own threshold in dB, and
     *  whether it's mixed half and half with the dry (Parallel) */
    struct Character
    {
        float attack, release, ratio, knee, offset;
        bool parallel;
    };
    static constexpr Character kCharacters[kFinCharacters] = {
        {3.f, 100.f, 1.5f, 6.f, 0.f, false},     // light & fast
        {3.f, 150.f, 3.f, 6.f, 0.f, false},      // standard & fast
        {30.f, 300.f, 3.f, 6.f, 0.f, false},     // standard & slow
        {1.f, 150.f, 10.f, 2.f, -10.f, true},    // parallel
        {10.f, 800.f, 4.f, 10.f, -4.f, false},   // keep loud
        {1.f, 220.f, 20.f, 1.f, -10.f, false},   // beat FX: pumps
    };
    // the detector's hold on a band's peak, so a fast attack doesn't follow a waveform: half
    // a cycle of the band's lowest
    static constexpr float kHoldMs[kBands] = {10.f, 2.f, .2f};

    /** A topology-preserving state-variable filter's state (Zavalishin), and its coefficients
     *  for a Butterworth Q (k = sqrt 2): two in a row are a Linkwitz-Riley 4th order */
    struct Svf
    {
        float ic1, ic2;
        struct C
        {
            float a1, a2, a3;
        };
        static constexpr float kK = 1.41421356f;
        FIN_ONCE static void Coeffs(C& c, float freq, float sample_rate)
        {
            // tan(x) by its series, to x^7: x is under .2 here (3kHz at 48kHz), where that's
            // within 1e-8, and newlib's tanf would bring 4KB of argument reduction along
            const float x = PI_F * freq / sample_rate, x2 = x * x;
            const float g = x * (1.f + x2 * (1.f / 3.f + x2 * (2.f / 15.f + x2 * (17.f / 315.f))));
            c.a1 = 1.f / (1.f + g * (g + kK));
            c.a2 = g * c.a1;
            c.a3 = g * c.a2;
        }
        /** One sample in: the lowpass and bandpass out */
        __attribute__((always_inline)) inline void Tick(const C& c, float v0, float& lp, float& bp)
        {
            const float v3 = v0 - ic2;
            const float v1 = c.a1 * ic1 + c.a2 * v3;
            const float v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
            ic1 = 2.f * v1 - ic1;
            ic2 = 2.f * v2 - ic2;
            lp = v2;
            bp = v1;
        }
    };

    /** One channel's three bands */
    struct Split
    {
        Svf f[7];
        __attribute__((always_inline)) inline void Process(const Svf::C* c, float x, float& lo,
                                                           float& mid, float& hi)
        {
            float lp, bp, lp2, bp2;
            f[0].Tick(c[0], x, lp, bp);
            const float hp = x - Svf::kK * bp - lp;
            f[1].Tick(c[0], lp, lo, bp2); // the low split's lowpass
            f[2].Tick(c[0], hp, lp2, bp2);
            const float rest = hp - Svf::kK * bp2 - lp2; // and its highpass
            f[3].Tick(c[1], rest, lp, bp);
            const float hp3 = rest - Svf::kK * bp - lp;
            f[4].Tick(c[1], lp, mid, bp2);
            f[5].Tick(c[1], hp3, lp2, bp2);
            hi = hp3 - Svf::kK * bp2 - lp2;
            f[6].Tick(c[1], lo, lp, bp); // the low band through the high split's allpass
            lo = lo - 2.f * Svf::kK * bp;
        }
    };

    /** A first-order allpass of the halfbands (hiir's), at the low rate: y = a (x - y1) + x1 */
    struct Allpass
    {
        float x1, y1;
        __attribute__((always_inline)) inline float Process(float a, float x)
        {
            const float y = a * (x - y1) + x1;
            x1 = x;
            y1 = y;
            return y;
        }
    };
    // the halfbands' coefficients (hiir's PolyphaseIir2Designer, 4 coefficients, a transition
    // of .083: flat to 16kHz, 65dB down from 32kHz at twice the rate); even ones on the first
    // path, odd ones on the second
    static constexpr float kHalfband[4] = {.089922015f, .311525233f, .578049670f, .851016616f};
    // Between's taps, from the middle out
    static const uint32_t kMidTaps = 16, kHist = 16;
    static constexpr float kMid[kMidTaps / 2] = {.63203657f, -.19875598f, .10587670f, -.06283757f,
                                                 .03762753f, -.02156795f, .01123512f, -.00484577f};
    // the gain down before its peak: the two boxes span 2 kBox - 1 samples, and Between's
    // peak comes kMidTaps / 2 samples late
    static_assert(2 * kBox - 1 + kMidTaps / 2 <= kLookahead + 1, "the gain down before its peak (Limit)");
    struct Drive2x
    {
        Allpass up[4], down[4];
        float odd; // the last pair's second sample, for the downsampler's second path
        float dc, lp;
    };

    /** Everything the samples go through, at rest */
    FIN_ONCE void Clear()
    {
        for (size_t c = 0; c < 2; c++)
        {
            splits_[c] = Split();
            drive2x_[c] = Drive2x();
        }
        for (size_t b = 0; b < kBands; b++)
        {
            gr_db_[b] = 0.f;
            held_[b] = 0.f;
            hold_left_[b] = 0;
            pk_[b] = em_[b] = es_[b] = 0.f;
            gm_[b] = gs_[b] = 1.f;
            corr_[b] = 1.f;
            cm_[b] = cs_[b] = 0.f;
            mute_[b] = false;
            mute_gain_[b] = 1.f;
            band_peak_[b] = 0.f;
        }
        trim_ = 1.f;
        band_mix_ = drive_mix_ = lim_mix_ = 0.f;
        drive_gain_ = 1.f;
        loud_gain_ = 1.f;
        bands_hold_ = 0;
        lim_run_ = lim_want_ = false;
        lim_gr_db_ = 0.f;
        lim_min_ = 1.f;
        in_peak_ = 0.f;
        ClearLimiter();
    }

    FIN_ONCE void ClearLimiter()
    {
        for (uint32_t i = 0; i < kLookahead; i++)
            delay_[0][i] = delay_[1][i] = 0.f;
        for (uint32_t i = 0; i < kBox; i++)
            box_[0][i] = box_[1][i] = 1.f;
        sum_[0] = sum_[1] = static_cast<float>(kBox);
        box_pos_ = 0;
        delay_pos_ = 0;
        q_head_ = q_tail_ = 0;
        now_ = 0;
        env_ = 1.f;
        for (size_t i = 0; i < 2 * kHist; i++)
            hist_[0][i] = hist_[1][i] = 0.f;
        hist_pos_ = 0;
        fill_ = kLookahead;
        avg_gr_ = 0.f;
    }

    /** What the knobs say, worked out once a block while one moves, once more where it lands */
    FIN_ONCE void Update()
    {
        const float in = knobs_[INPUT].value;
        trim_target_ = in == .5f ? 1.f : daisysp::pow10f((in - .5f) * 2.f * kInputDb * .05f);
        eq_neutral_ = width_neutral_ = true;
        for (size_t b = 0; b < kBands; b++)
        {
            const float e = knobs_[EQ_LOW + b].value;
            eq_db_[b] = e < .5f ? (e - .5f) * 2.f * kEqCutDb : (e - .5f) * 2.f * kEqBoostDb;
            eq_[b] = e == .5f ? 1.f : daisysp::pow10f(eq_db_[b] * .05f);
            eq_neutral_ &= e == .5f;
            const float w = knobs_[WIDTH_LOW + b].value;
            const float max = b == 0 ? kMaxLowWidth : kMaxWidth;
            width_[b] = w <= .5f ? 2.f * w : 1.f + (w - .5f) * 2.f * (max - 1.f);
            width_neutral_ &= w == .5f;
            character_[b] = static_cast<uint8_t>(fminf(knobs_[CHARACTER_LOW + b].value, 1.f)
                                                     * (kFinCharacters - 1) + .5f);
        }
        depth_ = knobs_[DEPTH].value;
        taken_[0] = knobs_[TYPE].value;
        for (size_t b = 0; b < kBands; b++)
            taken_[1 + b] = knobs_[CHARACTER_LOW + b].value;
        const float drive = knobs_[DRIVE].value;
        drive_on_ = drive > 0.f;
        drive_db_gain_ = daisysp::pow10f(drive * kDriveDb * .05f);
        type_want_ = static_cast<uint8_t>(fminf(knobs_[TYPE].value, 1.f) * (kFinTypes - 1) + .5f);
        const float loud = knobs_[LOUD].value;
        loud_on_ = loud > 0.f;
        loud_target_ = daisysp::pow10f(loud * kLoudDb * .05f);
        times_n_ = 0; // the comp's coefficients again (Times)
    }

    /** The saturation of a type, at twice the rate. Polynomials clamped where their slope
     *  reaches 0, so the knee is smooth (its harmonics fall off fast and don't fold back):
     *  - Boost: x - x^3 / (3 L^2), L 1.5: below the knee the third harmonic alone
     *  - Tube: the same, L 2.5, plus .2 of its square: the square's second harmonic grows
     *    with the level as the cubic's third does, so it stays on top as the drive rises
     *  - Fuzz: x - x^5 / (5 L^4), L 1.25: flatter, then a harder knee, more and higher
     *    harmonics as it clips, plus .1 of its square for some even ones
     *  The square's DC goes in the blocker after it. Asymmetric, Tube and Fuzz take a little
     *  DC with them that follows the level, as a tube stage's bias shifts */
    static __attribute__((always_inline)) inline float Saturate(uint8_t type, float x)
    {
        if (type == BOOST)
        {
            x = fminf(fmaxf(x, -1.5f), 1.5f);
            return x - x * x * x * (1.f / 6.75f);
        }
        if (type == TUBE)
        {
            x = fminf(fmaxf(x, -2.5f), 2.5f);
            const float s = x - x * x * x * (1.f / 18.75f);
            return s + .2f * s * s;
        }
        x = fminf(fmaxf(x, -1.25f), 1.25f);
        const float x2 = x * x;
        const float s = x - x2 * x2 * x * (1.f / 12.2070312f);
        return s + .1f * s * s;
    }
    /** Each type's gain before its curve: the reference sine meets Boost's knee just past the
     *  top (+18.7dB), Tube's past it too (+20.9dB, its second harmonic 9dB above the third at
     *  the top), Fuzz's at +14dB, so Fuzz drives hardest */
    static constexpr float kTypeGain[kFinTypes] = {.35f, .45f, .5f};

    /** The drive's fixed compensation: what brings a sine of kDriveRef, driven by gain into
     *  type's saturation, back to its level (its RMS, the DC taken off) */
    FIN_ONCE static float DriveCompensation(float gain, uint8_t type)
    {
        static const int kPoints = 32;
        float sum = 0.f, sq = 0.f;
        for (int i = 0; i < kPoints; i++)
        {
            const float y = Saturate(type, gain * kDriveRef * sinf(TWOPI_F * (i + .5f) / kPoints));
            sum += y;
            sq += y * y;
        }
        const float mean = sum / kPoints;
        const float rms = sqrtf(fmaxf(sq / kPoints - mean * mean, 1e-12f));
        return kDriveRef * .70710678f / rms;
    }

    /** The comp's attack and release per block of n, a band's time scaled by its slowness */
    FIN_ONCE void Times(size_t n)
    {
        times_n_ = n;
        for (size_t b = 0; b < kBands; b++)
        {
            const Character& c = kCharacters[character_[b]];
            const float k = (b == 0 ? kLowSlower : 1.f) * .001f * sample_rate_;
            attack_[b] = 1.f - expf(-static_cast<float>(n) / (c.attack * k));
            release_[b] = 1.f - expf(-static_cast<float>(n) / (c.release * k));
        }
        corr_coeff_ = 1.f - expf(-static_cast<float>(n) / (kCorrMs * .001f * sample_rate_));
    }

    /** f towards target by step, landing on it */
    static inline float Toward(float f, float target, float step)
    {
        return f < target ? fminf(f + step, target) : fmaxf(f - step, target);
    }

    /** Once a block: the knobs, which modules run, the bands' gain computers */
    FIN_ONCE void ControlRate(size_t n)
    {
        // the knobs slew; what follows from them is worked out once a block while one moves,
        // and once more where they land (as the master compressor did)
        bool moving = false;
        if (turned_)
        {
            turned_ = false;
            for (size_t p = 0; p < kNumFinParams; p++)
            {
                update_pending_ |= knobs_[p].Settle(kFxParamCoeff * n);
                moving |= knobs_[p].value != knobs_[p].target;
            }
            // a stepped knob (the type, the characters) lands at once and never slews: worked
            // out where it isn't what Update took last
            update_pending_ |= knobs_[TYPE].value != taken_[0];
            for (size_t b = 0; b < kBands; b++)
                update_pending_ |= knobs_[CHARACTER_LOW + b].value != taken_[1 + b];
            if (moving)
                turned_ = true;
        }
        if (update_pending_ && (!moving || (since_update_ += n) >= kUpdateSamples))
        {
            Update();
            update_pending_ = false;
            since_update_ = 0;
        }
        if (times_n_ != n)
            Times(n);
        const float step = fade_step_ * n;

        // the bands: their gain computers on last block's peaks, then the gains this block
        // glides to
        bool reducing = false, muting = false;
        const bool comp = depth_ > 0.f && !bypass_;
        for (size_t b = 0; b < kBands; b++)
        {
            float pk = pk_[b];
            pk_[b] = 0.f;
            if (pk >= held_[b])
            {
                held_[b] = pk;
                hold_left_[b] = static_cast<int32_t>(kHoldMs[b] * .001f * sample_rate_);
            }
            else if (hold_left_[b] > 0)
                hold_left_[b] -= static_cast<int32_t>(n);
            else
                held_[b] = pk;
            const Character& c = kCharacters[character_[b]];
            float target = 0.f;
            if (comp && held_[b] > 0.f)
            {
                const float over = 6.0206f * daisysp::fastlog2f(held_[b]) + eq_db_[b]
                                   - (c.offset - depth_ * kDepthDb);
                const float slope = 1.f / c.ratio - 1.f;
                const float half = c.knee * .5f;
                if (over > half)
                    target = slope * over;
                else if (over > -half)
                    target = slope * (over + half) * (over + half) / (2.f * c.knee);
            }
            float gr = gr_db_[b];
            gr += (target < gr ? attack_[b] : release_[b]) * (target - gr);
            if (target == 0.f && gr > -kRestDb)
                gr = 0.f;
            gr_db_[b] = gr;
            reducing |= gr != 0.f;
            float g = gr == 0.f ? 1.f : daisysp::pow10f(gr * .05f);
            if (c.parallel)
                g = .5f * (1.f + g);
            mute_gain_[b] = Toward(mute_gain_[b], mute_[b] && !bypass_ ? 0.f : 1.f, mute_step_ * n);
            muting |= mute_gain_[b] != 1.f || (mute_[b] && !bypass_);
            gm_target_[b] = eq_[b] * g * mute_gain_[b];
            gs_target_[b] = gm_target_[b] * width_[b];
            const float peak = held_[b] * eq_[b];
            if (peak > band_peak_[b])
                band_peak_[b] = peak;
            // the correlation after Width: (M^2 - S^2) / (M^2 + S^2), each smoothed and scaled
            // by its gain: 2 LR / (L^2 + R^2), Pearson's for channels about as loud
            cm_[b] += corr_coeff_ * (em_[b] * gm_target_[b] * gm_target_[b] - cm_[b]);
            cs_[b] += corr_coeff_ * (es_[b] * gs_target_[b] * gs_target_[b] - cs_[b]);
            em_[b] = es_[b] = 0.f;
            if (cm_[b] + cs_[b] > 1e-12f)
                corr_[b] = (cm_[b] - cs_[b]) / (cm_[b] + cs_[b]);
        }
        const bool bands = !bypass_ && (!eq_neutral_ || !width_neutral_ || depth_ > 0.f || reducing || muting);
        band_from_ = band_mix_;
        band_mix_ = Toward(band_mix_, bands ? 1.f : 0.f, step);
        if (bands || band_mix_ > 0.f)
            bands_hold_ = static_cast<int32_t>(kBandsHoldMs * .001f * sample_rate_);
        else if (bands_hold_ > 0)
            bands_hold_ -= static_cast<int32_t>(n);
        bands_apply_ = band_from_ > 0.f || band_mix_ > 0.f;
        bands_run_ = bands_apply_ || bands_hold_ > 0 || meter_ == EQ || meter_ == WIDTH;
        if (!bands_run_)
            for (size_t b = 0; b < kBands; b++)
                gm_[b] = gs_[b] = 1.f;

        // the trim: there at once, gliding over the block
        trim_from_ = trim_;
        trim_ = bypass_ ? 1.f : trim_target_;

        // the drive. A new type while it's heard dips it out first; the type changes at the
        // bottom, where it then runs unheard for kTypeHoldMs while the blocker takes the new
        // type's DC, and fades back in
        const bool drive = drive_on_ && !bypass_;
        drive_from_ = drive_mix_;
        if (drive && !drive_run_)
            for (size_t c = 0; c < 2; c++)
                drive2x_[c] = Drive2x();
        if (type_want_ != type_ && drive_mix_ == 0.f)
        {
            type_ = type_want_;
            if (drive_run_)
                type_hold_ = static_cast<int32_t>(kTypeHoldMs * .001f * sample_rate_);
        }
        else if (type_hold_ > 0)
            type_hold_ -= static_cast<int32_t>(n);
        const bool heard = drive && type_want_ == type_ && type_hold_ <= 0;
        drive_mix_ = Toward(drive_mix_, heard ? 1.f : 0.f, step);
        drive_run_ = drive_from_ > 0.f || drive_mix_ > 0.f || (drive && type_hold_ > 0);
        if (!drive_run_)
            type_hold_ = 0;
        drive_target_ = drive_db_gain_ * kTypeGain[type_];
        if (drive_target_ != comp_gain_ || type_ != comp_type_)
        {
            // only when the drive or the type changed: 32 sines
            comp_gain_ = drive_target_;
            comp_type_ = type_;
            drive_comp_ = DriveCompensation(drive_target_, type_);
        }
        if (type_ != gain_type_)
        {
            // unheard: the gain jumps with the type
            drive_gain_ = drive_target_;
            gain_type_ = type_;
        }

        // the limiter: on waking its delay fills first, then fades in
        lim_want_ = loud_on_ && !bypass_;
        if (lim_want_ && !lim_run_)
        {
            ClearLimiter();
            lim_run_ = true;
        }
        lim_from_ = lim_mix_;
        if (lim_run_ && fill_ == 0)
            lim_mix_ = Toward(lim_mix_, lim_want_ ? 1.f : 0.f, step);
        if (lim_run_ && !lim_want_ && lim_mix_ == 0.f && lim_from_ == 0.f)
        {
            lim_run_ = false;
            lim_gr_db_ = 0.f;
        }
        // the release: from 30ms, longer the deeper it has limited lately, up to 400ms
        const float rel_ms = fminf(30.f + 60.f * avg_gr_, 400.f);
        release_lim_ = 1000.f / (rel_ms * sample_rate_);

        awake_ = turned_ || update_pending_ || trim_ != 1.f || trim_from_ != 1.f || bands_run_
                 || drive_run_ || lim_run_;
    }

    /** Everything awake, a block: out of line, linked after FxChain::Run (FrizzHot.h), at -O2,
     *  as -O3's unrolling made it twice the size for little */
    FRIZZ_HOT FRIZZ_AFTER_RUN __attribute__((noinline, optimize("O2"))) void ProcessAwake(float* l, float* r, size_t n)
    {
        ControlRate(n);
        if (trim_ != 1.f || trim_from_ != 1.f)
            Trim(l, r, n);
        else if (meter_ == LEVEL)
            InputPeak(l, r, n);
        if (bands_run_)
            Bands(l, r, n);
        if (drive_run_)
            Drive(l, r, n);
        if (lim_run_)
            Limit(l, r, n);
    }

    /** The input's peak, for its meter, out of line */
    FRIZZ_AFTER_RUN __attribute__((noinline)) void InputPeak(const float* l, const float* r, size_t n)
    {
        float peak = in_peak_;
        for (size_t i = 0; i < n; i++)
            peak = fmaxf(peak, fmaxf(fabsf(l[i]), fabsf(r[i])));
        in_peak_ = peak;
    }

    __attribute__((always_inline)) inline void Trim(float* l, float* r, size_t n)
    {
        float g = trim_from_;
        const float dg = (trim_ - g) / n;
        float peak = in_peak_;
        for (size_t i = 0; i < n; i++)
        {
            g += dg;
            l[i] *= g;
            r[i] *= g;
            peak = fmaxf(peak, fmaxf(fabsf(l[i]), fabsf(r[i])));
        }
        in_peak_ = peak;
    }

    __attribute__((always_inline)) inline void Bands(float* l, float* r, size_t n)
    {
        const float inv = 1.f / n;
        float gm[kBands], gs[kBands], dgm[kBands], dgs[kBands], pk[kBands], em[kBands], es[kBands];
        for (size_t b = 0; b < kBands; b++)
        {
            gm[b] = gm_[b];
            gs[b] = gs_[b];
            dgm[b] = (gm_target_[b] - gm[b]) * inv;
            dgs[b] = (gs_target_[b] - gs[b]) * inv;
            pk[b] = pk_[b];
            em[b] = em_[b];
            es[b] = es_[b];
        }
        float mix = band_from_;
        const float dmix = (band_mix_ - mix) * inv;
        const bool apply = bands_apply_;
        for (size_t i = 0; i < n; i++)
        {
            const float m = .5f * (l[i] + r[i]), s = .5f * (l[i] - r[i]);
            float mb[kBands], sb[kBands];
            splits_[0].Process(split_, m, mb[0], mb[1], mb[2]);
            splits_[1].Process(split_, s, sb[0], sb[1], sb[2]);
            float ym = 0.f, ys = 0.f;
            for (size_t b = 0; b < kBands; b++)
            {
                pk[b] = fmaxf(pk[b], fabsf(mb[b]) + fabsf(sb[b]));
                em[b] += mb[b] * mb[b];
                es[b] += sb[b] * sb[b];
                gm[b] += dgm[b];
                gs[b] += dgs[b];
                ym += gm[b] * mb[b];
                ys += gs[b] * sb[b];
            }
            if (apply)
            {
                mix += dmix;
                ym = m + mix * (ym - m);
                ys = s + mix * (ys - s);
                l[i] = ym + ys;
                r[i] = ym - ys;
            }
        }
        for (size_t b = 0; b < kBands; b++)
        {
            gm_[b] = gm_target_[b];
            gs_[b] = gs_target_[b];
            pk_[b] = pk[b];
            em_[b] = em[b];
            es_[b] = es[b];
        }
    }

    __attribute__((always_inline)) inline float Drive1(Drive2x& d, float x, float gain, float comp)
    {
        x *= gain;
        // up: each path's output one sample of the pair
        const float u0 = d.up[2].Process(kHalfband[2], d.up[0].Process(kHalfband[0], x));
        const float u1 = d.up[3].Process(kHalfband[3], d.up[1].Process(kHalfband[1], x));
        const float v0 = Saturate(type_, u0), v1 = Saturate(type_, u1);
        // down: the first path takes the pair's first, the second the last pair's second
        const float a = d.down[2].Process(kHalfband[2], d.down[0].Process(kHalfband[0], v0));
        const float b = d.down[3].Process(kHalfband[3], d.down[1].Process(kHalfband[1], d.odd));
        d.odd = v1;
        float y = .5f * (a + b) * comp;
        d.dc += dc_ * (y - d.dc);
        y -= d.dc;
        d.lp += hicut_ * (y - d.lp);
        return d.lp;
    }

    __attribute__((always_inline)) inline void Drive(float* l, float* r, size_t n)
    {
        const float inv = 1.f / n;
        const float dg = (drive_target_ - drive_gain_) * inv;
        const float dmix = (drive_mix_ - drive_from_) * inv;
        const float comp = drive_comp_;
        // a channel at a time: the drive's code once
        for (size_t c = 0; c < 2; c++)
        {
            float* x = c ? r : l;
            float g = drive_gain_, mix = drive_from_;
            for (size_t i = 0; i < n; i++)
            {
                g += dg;
                mix += dmix;
                x[i] += mix * (Drive1(drive2x_[c], x[i], g, comp) - x[i]);
            }
        }
        drive_gain_ = drive_target_;
    }

    /** A channel's last kMidTaps samples, x the newest: the one between the middle two at
     *  twice the rate, a half-sample interpolator (least squares, flat within -0.21/+0.07dB
     *  to .42 of the rate, 20kHz) */
    __attribute__((always_inline)) inline float Between(size_t c, float x)
    {
        const uint32_t p = hist_pos_;
        float* h = hist_[c];
        h[p] = h[p + kHist] = x;
        // the newest at p + kHist, the oldest of the taps kMidTaps - 1 before it
        const float* t = h + p + kHist - (kMidTaps - 1);
        float y = 0.f;
        for (uint32_t k = 0; k < kMidTaps / 2; k++)
            y += kMid[k] * (t[kMidTaps / 2 - 1 - k] + t[kMidTaps / 2 + k]);
        return y;
    }

    __attribute__((always_inline)) inline void Limit(float* l, float* r, size_t n)
    {
        const float inv = 1.f / n;
        float g = loud_gain_;
        const float dg = (loud_target_ - g) * inv;
        float mix = lim_from_;
        const float dmix = (lim_mix_ - mix) * inv;
        // the box filters' sums again from their rings, so rounding doesn't add up
        for (size_t k = 0; k < 2; k++)
        {
            float s = 0.f;
            for (uint32_t i = 0; i < kBox; i++)
                s += box_[k][i];
            sum_[k] = s;
        }
        float env = env_, lim_min = lim_min_, gr_sum = 0.f;
        const float release = release_lim_;
        for (size_t i = 0; i < n; i++)
        {
            g += dg;
            const float xl = l[i] * g, xr = r[i] * g;
            // the peak, the sample's and the one between two, kMidTaps / 2 samples back
            hist_pos_ = (hist_pos_ + 1) & (kHist - 1);
            float p = fmaxf(fabsf(xl), fabsf(xr));
            p = fmaxf(p, fmaxf(fabsf(Between(0, xl)), fabsf(Between(1, xr))));
            const float need = p > kCeiling ? kCeiling / p : 1.f;
            // its running minimum over the lookahead and a sample: a queue of rising values
            while (q_tail_ != q_head_ && q_val_[(q_tail_ - 1) % kQueue] >= need)
                q_tail_--;
            q_val_[q_tail_ % kQueue] = need;
            q_at_[q_tail_ % kQueue] = now_;
            q_tail_++;
            if (now_ - q_at_[q_head_ % kQueue] > kLookahead)
                q_head_++;
            const float held = q_val_[q_head_ % kQueue];
            now_++;
            // two box filters
            const uint32_t at = box_pos_;
            sum_[0] += held - box_[0][at];
            box_[0][at] = held;
            const float b1 = sum_[0] * (1.f / kBox);
            sum_[1] += b1 - box_[1][at];
            box_[1][at] = b1;
            box_pos_ = at + 1 == kBox ? 0 : at + 1;
            const float smooth = sum_[1] * (1.f / kBox);
            // and the release
            env = fminf(smooth, env + release * (1.f - env));
            lim_min = fminf(lim_min, env);
            gr_sum += 1.f - env;
            // the delay
            const uint32_t d = delay_pos_;
            const float dl = delay_[0][d], dr = delay_[1][d];
            delay_[0][d] = xl;
            delay_[1][d] = xr;
            delay_pos_ = d + 1 == kLookahead ? 0 : d + 1;
            if (fill_ > 0)
            {
                fill_--;
                continue;
            }
            mix += dmix;
            l[i] += mix * (dl * env - l[i]);
            r[i] += mix * (dr * env - r[i]);
        }
        loud_gain_ = loud_target_;
        env_ = env;
        lim_min_ = lim_min;
        lim_gr_db_ = env < 1.f ? 6.0206f * daisysp::fastlog2f(env) : 0.f;
        // how deep it limited lately, dB, over about a second (a time constant of 50000 samples), for the release
        avg_gr_ += .00002f * n * (gr_sum * inv * 8.7f - avg_gr_);
    }

    float sample_rate_;
    Smoothed knobs_[kNumFinParams];
    volatile bool turned_; // a knob turned and not yet settled
    volatile bool awake_;   // something to do: Process runs ProcessAwake
    volatile bool bypass_;
    volatile uint8_t meter_;
    bool update_pending_;
    float taken_[1 + kBands]; // the stepped knobs as Update took them: the type, the characters
    uint32_t since_update_;
    size_t times_n_; // the block the comp's coefficients are for (Times)
    float fade_step_;

    // what the knobs say (Update)
    float trim_target_;
    float eq_db_[kBands], eq_[kBands], width_[kBands];
    uint8_t character_[kBands];
    bool eq_neutral_, width_neutral_;
    float depth_;
    bool drive_on_, loud_on_;
    float drive_db_gain_, loud_target_;
    uint8_t type_want_;

    // the input
    float trim_, trim_from_;
    volatile float in_peak_;

    // the bands
    Svf::C split_[2];
    Split splits_[2]; // M's and S's
    float attack_[kBands], release_[kBands];
    float pk_[kBands], held_[kBands];
    int32_t hold_left_[kBands];
    float gm_[kBands], gs_[kBands], gm_target_[kBands], gs_target_[kBands];
    float em_[kBands], es_[kBands];
    float cm_[kBands], cs_[kBands]; // their smoothed energies, for the correlation
    float corr_coeff_;
    volatile float gr_db_[kBands];
    volatile float corr_[kBands];
    volatile float band_peak_[kBands];
    float band_mix_, band_from_;
    volatile bool mute_[kBands];
    float mute_gain_[kBands]; // 1 to 0 as a band is muted
    float mute_step_;
    int32_t bands_hold_;
    bool bands_run_, bands_apply_;

    // the drive
    float drive_target_, drive_comp_;
    float comp_gain_; // what drive_comp_ was worked out for
    uint8_t comp_type_;
    uint8_t type_, gain_type_; // the type it plays, and the one drive_gain_ is for
    int32_t type_hold_; // a new type's hold at the bottom, samples left
    Drive2x drive2x_[2];
    float hicut_, dc_;
    float drive_gain_;
    float drive_mix_, drive_from_;
    bool drive_run_;

    // the limiter
    float loud_gain_;
    float delay_[2][kLookahead];
    float hist_[2][2 * kHist]; // the last kHist samples, twice over (Between)
    uint32_t hist_pos_;
    float q_val_[kQueue];
    uint32_t q_at_[kQueue];
    uint32_t q_head_, q_tail_, now_;
    float box_[2][kBox];
    float sum_[2];
    uint32_t box_pos_, delay_pos_;
    uint32_t fill_;
    float env_, release_lim_, avg_gr_;
    float lim_mix_, lim_from_;
    bool lim_run_;
    volatile bool lim_want_;
    volatile float lim_gr_db_;
    volatile float lim_min_;
};

constexpr Finalizer::Character Finalizer::kCharacters[kFinCharacters];
constexpr float Finalizer::kHoldMs[Finalizer::kBands];
constexpr float Finalizer::kHalfband[4];
constexpr float Finalizer::kTypeGain[kFinTypes];
constexpr float Finalizer::kMid[Finalizer::kMidTaps / 2];

} // namespace chompi
