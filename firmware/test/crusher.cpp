// Crusher check: the crusher's (FxCrusher.h) rate, bits and dive, on a sine. Built and run by
// unit.sh crusher.
//  - rate: the reducer holds each sample as long as its knob says: 15 samples in the middle, 100
//    at the top (21.6 kHz down to 480 Hz, every 18% of the knob halving it); at 0 a 1 kHz sine
//    comes through nearly as it was
//  - bits: at the top, 2 bits: a full-scale sine comes out on a handful of steps; at 0 (16
//    bits) on as many as the reducer passes
//  - dive: a press drops the rate 10x over 0.1 s, and it's back within 0.3 s of the press
//  - the dive through a sleep: a tap shorter than its attack, asleep before the attack is
//    done, and pressed again: the dive picks up where it would have been, the attack finished
//    and decayed since, not decayed from where the attack was cut off (#167)
#include <cmath>
#include <cstdio>
#include <set>
#include "check.h"
// the crusher's dive, to compare it
#define private public
#include "FxChain.h"
#undef private

using namespace chompi;

static const float kSr = 48000.f;

static void Setup(Crusher& c, float rate, float bits)
{
    c.Init(kSr);
    c.SetParam(Crusher::RATE, rate);
    c.SetParam(Crusher::BITS, bits);
    c.SetParam(Crusher::TONE, 1.f); // the lowpass open
    c.SetParam(Crusher::XOR, 0.f);
    c.SnapParams();
}

/** Runs n samples of a 997 Hz sine through it from sample `from`; how long the output holds
 *  a value on average, in samples (from one step to the next: the reducer's BLEP spreads a step
 *  over two samples), and how many values it takes. Held is within 1e-4: the level guard and
 *  the gate's crossfade move a held value by float rounding */
static float Hold(Crusher& c, size_t& from, size_t n, size_t* values = nullptr)
{
    float last = 0.f;
    size_t changes = 0;
    bool changing = false;
    std::set<float> seen;
    for (size_t i = from; i < from + n; i++)
    {
        float l = .7f * sinf(2.f * float(M_PI) * 997.f * i / kSr), r = l;
        c.Process(&l, &r);
        const bool change = i > from && fabsf(l - last) > 1e-4f;
        changes += change && !changing;
        changing = change;
        last = l;
        seen.insert(roundf(l * 1000.f));
    }
    from += n;
    if (values)
        *values = seen.size();
    return changes ? static_cast<float>(n) / changes : static_cast<float>(n);
}

/** The rate knob: the hold once it's on and faded in */
static float RateHold(float knob)
{
    static Crusher c;
    Setup(c, knob, 0.f);
    c.SetOn(true);
    size_t t = 0;
    Hold(c, t, 4800); // the punch-in's fade and the dive of the press, over
    Hold(c, t, 48000);
    return Hold(c, t, 24000);
}

/** The rate knob at 0: how far below the input what it changes is, in dB */
static float BottomError()
{
    static Crusher c;
    Setup(c, 0.f, 0.f);
    c.SetOn(true);
    double in_sum = 0., err_sum = 0.;
    for (size_t i = 0; i < 96000; i++)
    {
        const float in = .7f * sinf(2.f * float(M_PI) * 997.f * i / kSr);
        float l = in, r = in;
        c.Process(&l, &r);
        if (i >= 48000)
        {
            in_sum += in * in;
            // against the input a sample late: the reducer's BLEP
            const float prev = .7f * sinf(2.f * float(M_PI) * 997.f * (i - 1) / kSr);
            err_sum += (l - prev) * (l - prev);
        }
    }
    return static_cast<float>(10. * log10(err_sum / in_sum));
}

static void TestRate()
{
    const float bottom = BottomError(), middle = RateHold(.5f), top = RateHold(1.f);
    printf("      rate: %.1f dB off the input at 0, a sample held %.1f, %.1f samples at 0.5, 1\n",
           bottom, middle, top);
    Check(bottom < -12.f, "rate: at 0, 21.6 kHz: a 1 kHz sine comes through nearly as it was");
    Check(middle > 13.f && middle < 17.f, "rate: in the middle, about 3.2 kHz: held about 15");
    Check(top > 90.f && top < 110.f, "rate: at the top, 480 Hz: held about 100");
}

static void TestBits()
{
    static Crusher c;
    size_t fine = 0, coarse = 0;
    Setup(c, 0.f, 0.f);
    c.SetOn(true);
    size_t t = 0;
    Hold(c, t, 48000);
    Hold(c, t, 24000, &fine);
    Setup(c, 0.f, 1.f);
    c.SetOn(true);
    t = 0;
    Hold(c, t, 48000);
    Hold(c, t, 24000, &coarse);
    printf("      bits: %zu values at 16 bits, %zu at 2\n", fine, coarse);
    Check(fine > 1000, "bits: at 0, 16 bits: the sine on many values");
    Check(coarse <= 5, "bits: at the top, 2 bits: on a handful of steps");
}

static void TestDive()
{
    static Crusher c;
    Setup(c, .25f, 0.f); // about 8 kHz: held about 6
    c.SetOn(true);
    size_t t = 0;
    Hold(c, t, 48000); // the press's dive over
    const float before = Hold(c, t, 2400);
    c.SetOn(false);
    Hold(c, t, 4800);
    c.SetOn(true); // a press: the dive (its first ms the punch-in's crossfade)
    Hold(c, t, 4320);
    const float deep = Hold(c, t, 960); // 90-110 ms after the press
    Hold(c, t, 9120);
    const float back = Hold(c, t, 2400); // 300-350 ms
    printf("      dive: held %.1f before, %.1f at 0.1 s, %.1f at 0.3 s\n", before, deep, back);
    Check(deep > before * 8.f && deep < before * 12.f, "dive: 0.1 s on, the rate 10x lower");
    Check(back < before * 1.2f, "dive: and back to the knob's 0.3 s after the press");
}

/** A tap of 10ms, asleep 80ms into the dive's 100ms attack, and pressed again after `asleep`
 *  samples: the dive as an envelope that never slept has it */
static void TestDiveWake(uint32_t asleep)
{
    static Crusher c;
    Setup(c, .5f, 0.f);
    PressEnvelope ref;
    ref.Reset();
    const float attack = 1.f / (.1f * kSr), decay = Decay60dBCoeff(.4f, kSr);
    bool was_on = false;
    auto step = [&](bool on) {
        c.SetOn(on);
        if (on && !was_on)
            ref.Press();
        was_on = on;
        float l = .3f, r = .3f;
        c.Process(&l, &r);
        ref.Process(attack, decay);
    };
    for (int i = 0; i < 480; i++)
        step(true);
    size_t fading = 0;
    while (!c.Idle())
    {
        step(false);
        fading++;
    }
    for (uint32_t i = 0; i < asleep; i++)
        step(false);
    step(true);
    printf("      asleep after %zu samples, %u more: the dive %.5f, never asleep %.5f\n",
           480 + fading, asleep, c.dive_.value, ref.value);
    char what[120];
    snprintf(what, sizeof(what), "the dive through a sleep of %u samples, mid-attack: as if awake",
             asleep);
    Check(fabsf(c.dive_.value - ref.value) < 1e-3f, what);
}

int main()
{
    TestRate();
    TestBits();
    TestDive();
    TestDiveWake(100);
    TestDiveWake(2400);
    TestDiveWake(24000);
    return Finish();
}
