/** @file FxCommon.h
 *  @brief What the punch-in effects (Fx*.h) share.
 */
#pragma once
#include "daisy.h"
#include "daisysp.h"
#include "FrizzHot.h"

using namespace daisysp;


namespace chompi
{

// What runs once (at boot), not per sample: built for size. Code space is the tightest
// memory (docs/CAPACITY.md), and -O3 unrolls the effects' buffer clears and settings into
// kilobytes
#define FX_ONCE __attribute__((noinline, optimize("Os")))

// The FX knobs 1-4, and the parameters per effect: two pages of them, page 1's on parameters
// 0-3, page 2's on 4-7 (FxControls.h)
static const size_t kNumFxKnobs = 4;
static const size_t kNumFxPages = 2;
static const size_t kNumFxParams = kNumFxKnobs * kNumFxPages;

// fonepole coefficients at 48kHz. A coefficient c has a time constant of 1 / (48000 c) and
// settles (to under 1%) in about 5 of them.
// The punch-in fade: a time constant of ~5ms, settled in ~25ms
static const float kFxGateCoeff = .004f;
// Below this (-120dB) a fading-out key counts as silent (FxGate::Asleep)
static const float kFxGateSleep = 1e-6f;
// The knobs' slew: a time constant of ~21ms, settled in ~100ms
static const float kFxParamCoeff = .001f;
// The slew for a moment after a scene recall (FxChain::FastSlew), the punch-in fade's, so a
// scene change is a cut rather than a sweep
static const float kFxRecallCoeff = kFxGateCoeff;

/** The slew every Smoothed uses: kFxParamCoeff, or kFxRecallCoeff right after a scene recall.
 *  A template only so the header can define the static */
template <class Unused = void>
struct FxSlewT
{
    static float coeff;
};
template <class Unused>
float FxSlewT<Unused>::coeff = kFxParamCoeff;
using FxSlew = FxSlewT<>;

/** A setting with a target, set from the UI, and a live value that follows it */
struct Smoothed
{
    float value, target;

    /** Both at v, no slew */
    void Reset(float v) { value = target = v; }
    /** The live value jumps to the target */
    void Snap() { value = target; }
    /** Once per sample, at the knobs' slew (FxSlew) */
    float Process() { return Process(FxSlew::coeff); }
    /** Once per sample, at a slew of its own */
    float Process(float coeff)
    {
        fonepole(value, target, coeff);
        return value;
    }
    /** For a value that's costly to apply: slews while it isn't at the target, and lands on
     *  it once within 1e-5, or once a step no longer moves it: near 1 the slew's step falls
     *  below a float's resolution and it would stall short of the target for good, a knob
     *  turned back to its default never at rest again (#51). True if it moved, so it needs
     *  applying */
    bool Settle(float coeff = FxSlew::coeff)
    {
        if (value == target)
            return false;
        const float before = value;
        Process(coeff);
        if (value == before || fabsf(value - target) < 1e-5f)
            Snap();
        return true;
    }
};

/** An effect's key: on while held or latched, faded in and out over ~5ms so punching in
 *  doesn't click, and the press itself for the effects that react to it */
class FxGate
{
public:
    void Init()
    {
        value_ = target_ = 0.f;
        on_ = false;
        pressed_ = false;
    }

    /** The key, from FxChain::Block. Returns true on a press (off to on) */
    bool SetOn(bool on)
    {
        const bool press = on && !on_;
        on_ = on;
        if (press)
            pressed_ = true;
        target_ = on ? 1.f : 0.f;
        return press;
    }

    /** Once per sample, before the gate is used: the fade, 0..1 */
    float Process()
    {
        fonepole(value_, target_, kFxGateCoeff);
        return value_;
    }

    inline bool IsOn() const { return on_; }
    /** The fade, as the last Process left it */
    inline float Value() const { return value_; }

    /** After Process: off and faded out, so the effect's output is its input and it can skip
     *  the work that only shapes what's heard. The fade's last 120dB snap to 0, so the output
     *  is exactly the input */
    inline bool Asleep()
    {
        if (on_ || value_ > kFxGateSleep)
            return false;
        value_ = 0.f;
        return true;
    }
    /** Off and faded out (under -120dB) */
    inline bool Silent() const { return !on_ && value_ <= kFxGateSleep; }

    /** True once after each press, for the audio callback */
    bool TakePress()
    {
        if (!pressed_)
            return false;
        pressed_ = false;
        return true;
    }

private:
    float value_, target_;
    bool on_; // SetOn runs in the audio callback, as everything else here
    bool pressed_;
};

/** What every punch-in effect has: a key and kNumFxParams parameters, each 0..1. Process is
 *  each effect's own, called by FxChain in its place in the chain. While off and faded out
 *  (FxGate::Asleep), the costly ones skip what only shapes the sound but keep what a
 *  punch-in starts from (a delay line's input, a filter's state), so engaging one never
 *  starts from stale state. */
class FxBase
{
public:
    virtual void SetOn(bool on) { gate_.SetOn(on); }
    virtual void SetParam(size_t param, float val) = 0;

    /** Off and faded out: its output is its input, and its meter isn't shown */
    inline bool Idle() const { return gate_.Silent(); }
    /** The key's fade now, 0..1, and whether it's off and faded out: for page 2's Mix and
     *  Level (FxOutput.h), which an effect sounding on after its key (the tape stop) hides */
    inline float Fade() const { return gate_.Value(); }
    inline bool Quiet() const { return Idle(); }

protected:
    FxGate gate_;
};

/** A send's tail: once its key is off and faded out and its return has stayed under -120dB
 *  for longer than anything in it could come back, it has nothing left to add and can skip
 *  its work until the key comes on */
struct TailWatch
{
    uint32_t quiet = 0; // samples the return has been silent, with the key off

    /** Before the work: whether to skip it. silent_in: the key is off and faded out */
    inline bool Sleeping(bool silent_in, uint32_t hold)
    {
        if (!silent_in)
            quiet = 0;
        return quiet >= hold;
    }
    /** After the work, with its return */
    inline void Track(bool silent_in, float l, float r)
    {
        if (silent_in && fabsf(l) < kFxGateSleep && fabsf(r) < kFxGateSleep)
            quiet++;
        else
            quiet = 0;
    }
};

/** A send's ducking (FxDelay.h, FxReverb.h): its return turned down while something goes in,
 *  by up to amount: 5ms down, 300ms back up, fully down from about -12dBFS in */
struct Ducker
{
    float amount = 0.f;
    float env = 0.f;

    void Init()
    {
        amount = 0.f;
        env = 0.f;
    }
    /** The return's gain for this sample's input */
    inline float Gain(float in_l, float in_r)
    {
        const float in = fmaxf(fabsf(in_l), fabsf(in_r));
        env += (in > env ? kAttack : kRelease) * (in - env);
        return 1.f - amount * fminf(env * 4.f, 1.f);
    }

    static constexpr float kAttack = 1.f - 0.995842f;  // ~5ms at 48kHz
    static constexpr float kRelease = 1.f - 0.999931f; // ~300ms
};

/** The effects' random numbers: a xorshift32, seeded per effect so every run is the same */
struct Rng
{
    uint32_t state = 1;

    inline void Seed(uint32_t seed) { state = seed ? seed : 1; }
    inline uint32_t Next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    /** 0..1, never 1 */
    inline float Uniform() { return static_cast<float>(Next() >> 8) * (1.f / 16777216.f); }
};

/** A stepped parameter's step, 0..steps - 1, from its knob value 0..1 (clamped, since the
 *  step indexes a table) */
inline size_t StepIndex(float val, size_t steps)
{
    val = val < 0.f ? 0.f : (val > 1.f ? 1.f : val);
    return static_cast<size_t>(val * static_cast<float>(steps - 1) + .5f);
}

/** A one-pole follower's coefficient for a time constant of seconds */
inline float TimeCoeff(float seconds, float sample_rate)
{
    return 1.f - expf(-1.f / (seconds * sample_rate));
}

/** A per-sample decay that falls by 60dB in seconds */
inline float Decay60dBCoeff(float seconds, float sample_rate)
{
    return expf(-6.9078f / (seconds * sample_rate));
}

/** Kastle's curve_map: linear between (xs[i], ys[i]) points, clamped at both ends */
inline float CurveMap(float x, const float* xs, const float* ys, size_t n)
{
    if (x <= xs[0])
        return ys[0];
    for (size_t i = 1; i < n; i++)
    {
        if (x <= xs[i])
            return ys[i - 1] + (ys[i] - ys[i - 1]) * (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
    }
    return ys[n - 1];
}

/** Linear-interpolated read, delay in frames behind the last write, from a power-of-2 ring */
inline float ReadFrac(const float* buf, size_t mask, size_t write_pos, float delay)
{
    const size_t whole = static_cast<size_t>(delay);
    const float frac = delay - static_cast<float>(whole);
    const float a = buf[(write_pos - whole) & mask];
    const float b = buf[(write_pos - whole - 1) & mask];
    return a + (b - a) * frac;
}

/** A one-pole lowpass's coefficient (fonepole) for a cutoff of freq Hz */
inline float OnePoleCoeff(float freq, float sample_rate)
{
    return 1.f - expf(-TWOPI_F * freq / sample_rate);
}

/** The folder's and crusher's tone knob: a lowpass from 200Hz to 20kHz, fully open at the top */
inline float ToneCoeff(float val, float sample_rate)
{
    return val >= 1.f ? 1.f : OnePoleCoeff(200.f * powf(100.f, val), sample_rate);
}

/** The envelope a press starts (the crusher's dive, the shifter's swoop, the slicer's steps):
 *  a linear attack up to 1, then an exponential decay */
struct PressEnvelope
{
    float value = 0.f;
    bool attacking = false;

    void Reset()
    {
        value = 0.f;
        attacking = false;
    }
    inline void Press() { attacking = true; }
    /** Once per sample */
    float Process(float attack_inc, float decay_coeff)
    {
        if (attacking)
        {
            value += attack_inc;
            if (value >= 1.f)
            {
                value = 1.f;
                attacking = false;
            }
        }
        else
            value *= decay_coeff;
        return value;
    }
    /** n samples of Process at once, for an effect that slept through them */
    void Skip(uint32_t n, float attack_inc, float decay_coeff)
    {
        if (attacking)
        {
            // the samples the attack still needs to reach 1 (its steps add up a little
            // short or over, so a hair under a whole number counts as it)
            const float k = ceilf((1.f - value) / attack_inc - 1e-3f);
            if (static_cast<float>(n) < k)
            {
                value += static_cast<float>(n) * attack_inc;
                return;
            }
            n -= static_cast<uint32_t>(k);
            value = 1.f;
            attacking = false;
        }
        value *= powf(decay_coeff, static_cast<float>(n));
    }
};

/** A stereo delay line of N frames (a power of 2): write one frame a sample, read behind it */
template <size_t N>
struct StereoRing
{
    static_assert((N & (N - 1)) == 0, "a power of 2");
    static const size_t kMask = N - 1;

    void Clear()
    {
        for (size_t c = 0; c < 2; c++)
            for (size_t i = 0; i < N; i++)
                buf[c][i] = 0.f;
        pos = 0;
    }
    /** delay frames behind the last frame written, interpolated */
    inline float Read(size_t c, float delay) const { return ReadFrac(buf[c], kMask, pos - 1, delay); }
    /** This sample's frame, one channel at a time; Advance once both are written */
    inline void Write(size_t c, float x) { buf[c][pos] = x; }
    inline void Advance() { pos = (pos + 1) & kMask; }
    /** Both channels of this sample's frame, and Advance */
    inline void WriteFrame(float l, float r)
    {
        buf[0][pos] = l;
        buf[1][pos] = r;
        Advance();
    }
    /** The index of the last frame written, for reads of their own (& kMask) */
    inline size_t Last() const { return pos - 1; }

    float buf[2][N];
    size_t pos;
};


} // namespace chompi
