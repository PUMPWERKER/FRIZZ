// Phaser check: the phaser's (FxPhaser.h) allpass chain, feedback, LFOs and sleep. Built and
// run by unit.sh phaser.
//  - notches: with the LFO held on the centre, the six stages notch the sound where each turns
//    the phase by 30, 90 and 150 degrees, the middle one on the centre knob's frequency (800 Hz
//    in the middle, 100 Hz at 0, 6.4 kHz at 1, within the Pade tangent's error); between them it
//    passes at unity
//  - depth 0 is dry, whatever the other knobs say: the phaser is silent at its defaults
//  - feedback at the top of its knob, the LFO sweeping fast and wide: noise in stays bounded,
//    and it rings down to nothing once the noise stops
//  - stereo: at 0 both channels on one coefficient; at 1 the right LFO half a cycle behind,
//    so the right sweeps down as far as the left sweeps up
//  - sleep: off and faded out, the output is the input, bit for bit, and it comes on from
//    silence without a click
//  - the rate knob: its left half free, .02 to 50 Hz on the flanger's curve, the right half
//    the filter LFO's divisions, 1/16 at the centre to 4 bars at the top; and the free LFO
//    really runs at its rate, the synced one with the clock's pulses
// The knob's turns (fine, stepped and SHIFT across both halves) are controls.cpp's.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "check.h"
#include "FxChain.h"

using namespace chompi;
// FxCommon.h brings in DaisySP's Phaser too, which isn't used
using Ph = chompi::Phaser;

static const float kSr = 48000.f;

/** A phaser on, faded in, its knobs set and the LFO held still (synced, no pulses): the
 *  sweep sits where the clock's position 0 puts it, on the centre */
static void Setup(Ph& p, float feedback, float depth, float stereo, float centre)
{
    p.Init(kSr);
    p.SetParam(Ph::RATE, 1.f); // synced
    p.SetParam(Ph::FEEDBACK, feedback);
    p.SetParam(Ph::DEPTH, depth);
    p.SetParam(Ph::STEREO, stereo);
    p.SetParam(Ph::CENTRE, centre);
    p.SetPulseSamples(0.f); // the LFO stands still
    p.ClockPulse(0);
    p.SetOn(true);
}

/** The gain at hz, once settled: the output's RMS over the input's, both channels */
static float Gain(Ph& p, float hz)
{
    double in_sum = 0., out_sum = 0.;
    for (size_t i = 0; i < 48000; i++)
    {
        const float in = .25f * sinf(2.f * float(M_PI) * hz * i / kSr);
        float l = in, r = in;
        p.Process(&l, &r);
        if (i >= 24000)
        {
            in_sum += in * in;
            out_sum += l * l;
        }
    }
    return sqrtf(out_sum / in_sum);
}

static float Db(float g) { return 20.f * log10f(fmaxf(g, 1e-9f)); }

/** The frequency a first-order allpass with coefficient a turns by deg degrees (bilinear) */
static float PhaseHz(float a, float deg)
{
    const float t90 = (1.f + a) / (1.f - a); // tan(pi f90 / fs)
    return kSr / float(M_PI) * atanf(t90 * tanf(deg / 2.f * float(M_PI) / 180.f));
}

static void Notches()
{
    static Ph p;
    char what[160];
    for (float centre : {.5f, 0.f, 1.f})
    {
        Setup(p, 0.f, 1.f, 0.f, centre);
        float l = 0.f, r = 0.f;
        p.Process(&l, &r); // wakes it, the coefficient set
        const float a = p.CoeffNow(0);
        const float want = 100.f * powf(2.f, 6.f * centre);
        const float f90 = PhaseHz(a, 90.f);
        printf("      centre %.2f: the stages' 90 degrees at %.1f Hz (%.0f Hz wanted)\n", centre, f90,
               want);
        snprintf(what, sizeof what, "notches: the centre knob at %.1f puts the stages' 90 degrees on %.0f Hz",
                 centre, want);
        Check(fabsf(f90 / want - 1.f) < .005f, what);

        float notch[3];
        int k = 0;
        for (float deg : {30.f, 90.f, 150.f})
        {
            Setup(p, 0.f, 1.f, 0.f, centre);
            notch[k++] = Gain(p, PhaseHz(a, deg));
        }
        printf("      notches at %.0f, %.0f, %.0f Hz: %.1f, %.1f, %.1f dB\n", PhaseHz(a, 30.f),
               PhaseHz(a, 90.f), PhaseHz(a, 150.f), Db(notch[0]), Db(notch[1]), Db(notch[2]));
        snprintf(what, sizeof what, "notches: centre %.1f: three, at 30, 90 and 150 degrees a stage, each below -30 dB",
                 centre);
        Check(Db(notch[0]) < -30.f && Db(notch[1]) < -30.f && Db(notch[2]) < -30.f, what);
        // the peaks between the notches (180 and 360 degrees: 60 and 120 a stage), and well
        // below the first: unity
        float peak[2];
        k = 0;
        for (float deg : {60.f, 120.f})
        {
            Setup(p, 0.f, 1.f, 0.f, centre);
            peak[k++] = Gain(p, PhaseHz(a, deg));
        }
        printf("      peaks between them: %.2f, %.2f dB\n", Db(peak[0]), Db(peak[1]));
        snprintf(what, sizeof what, "notches: centre %.1f: unity between them", centre);
        Check(fabsf(Db(peak[0])) < .5f && fabsf(Db(peak[1])) < .5f, what);
    }
}

static void DryAtDepth0()
{
    static Ph p;
    Setup(p, 1.f, 0.f, 1.f, .3f);
    p.SetParam(Ph::RATE, .4f);
    float worst = 0.f;
    for (size_t i = 0; i < 48000; i++)
    {
        const float in = .5f * sinf(2.f * float(M_PI) * 440.f * i / kSr);
        float l = in, r = -in;
        p.Process(&l, &r);
        if (i > 4800)
            worst = fmaxf(worst, fmaxf(fabsf(l - in), fabsf(r + in)));
    }
    printf("      depth 0, feedback and stereo up: off by %g at most\n", worst);
    Check(worst == 0.f, "depth: at 0 the output is the input, whatever feedback and stereo say");
}

static void Feedback()
{
    static Ph p;
    p.Init(kSr);
    p.SetParam(Ph::RATE, .45f); // free, fast
    p.SetParam(Ph::FEEDBACK, 1.f);
    p.SetParam(Ph::DEPTH, 1.f);
    p.SetParam(Ph::STEREO, 1.f);
    p.SetParam(Ph::CENTRE, .5f);
    p.SetOn(true);
    srand(1);
    float peak = 0.f, tail = 0.f;
    bool finite = true;
    for (size_t i = 0; i < 48000 * 6; i++)
    {
        const bool noise = i < 48000 * 5;
        float l = noise ? .5f * (2.f * rand() / RAND_MAX - 1.f) : 0.f;
        float r = noise ? .5f * (2.f * rand() / RAND_MAX - 1.f) : 0.f;
        p.Process(&l, &r);
        finite &= std::isfinite(l) && std::isfinite(r);
        peak = fmaxf(peak, fmaxf(fabsf(l), fabsf(r)));
        if (i >= 48000 * 5 + 24000)
            tail = fmaxf(tail, fmaxf(fabsf(l), fabsf(r)));
    }
    printf("      feedback at the top on noise at .5: peak %.2f, after 0.5 s of silence %g\n", peak,
           tail);
    Check(finite && peak < 4.f, "feedback: at the top of the knob, sweeping, the output stays bounded");
    Check(tail < 1e-4f, "feedback: and rings down once the input stops");

    // its resonance: louder between the notches the more it's turned up
    Setup(p, 0.f, 1.f, 0.f, .5f);
    const float a = p.CoeffNow(0);
    float l = 0.f, r = 0.f;
    p.Process(&l, &r);
    const float hz = PhaseHz(p.CoeffNow(0), 60.f);
    const float flat = Gain(p, hz);
    Setup(p, 1.f, 1.f, 0.f, .5f);
    const float res = Gain(p, hz);
    printf("      at the first peak (%.0f Hz): %.1f dB without feedback, %.1f dB at the top\n", hz,
           Db(flat), Db(res));
    Check(Db(res) > Db(flat) + 3.f, "feedback: it resonates between the notches");
    (void)a;
}

static void Stereo()
{
    static Ph p;
    // 1 bar (48 pulses): the 5th of 7 divisions
    const float bar = SplitStepValue(4.f, Ph::kNumDivisions);
    for (float stereo : {0.f, 1.f})
    {
        Setup(p, 0.f, 1.f, stereo, .5f);
        p.SetParam(Ph::RATE, bar);
        p.ClockPulse(12); // a quarter of the way: the left LFO at its top
        for (size_t i = 0; i < 64; i++)
        {
            float l = 0.f, r = 0.f;
            p.Process(&l, &r);
        }
        const float up = Ph::Coeff(Ph::BreakHz(.5f, 1.f, 1.f), kSr);
        const float down = Ph::Coeff(Ph::BreakHz(.5f, 1.f, -1.f), kSr);
        printf("      stereo %.0f: coefficients %.4f, %.4f (top %.4f, bottom %.4f)\n", stereo,
               p.CoeffNow(0), p.CoeffNow(1), up, down);
        if (stereo == 0.f)
            Check(fabsf(p.CoeffNow(0) - up) < 1e-4f && p.CoeffNow(0) == p.CoeffNow(1),
                  "stereo: at 0 both channels sweep together");
        else
            Check(fabsf(p.CoeffNow(0) - up) < 1e-4f && fabsf(p.CoeffNow(1) - down) < 1e-4f,
                  "stereo: at 1 the right LFO is half a cycle behind: down as far as the left is up");
    }
    // 2.5 octaves either way of 800 Hz at full depth
    printf("      full depth around 800 Hz: %.0f to %.0f Hz\n", Ph::BreakHz(.5f, 1.f, -1.f),
           Ph::BreakHz(.5f, 1.f, 1.f));
    Check(fabsf(Ph::BreakHz(.5f, 1.f, 1.f) - 800.f * powf(2.f, 2.5f)) < 1.f
              && fabsf(Ph::BreakHz(.5f, 1.f, -1.f) - 800.f / powf(2.f, 2.5f)) < .1f,
          "depth: at the top, the sweep goes 2.5 octaves either way of the centre");
}

static void Sleep()
{
    static Ph p;
    Setup(p, 1.f, 1.f, 1.f, .5f);
    p.SetParam(Ph::RATE, .4f);
    for (size_t i = 0; i < 4800; i++)
    {
        float l = .3f * sinf(i * .05f), r = l;
        p.Process(&l, &r);
    }
    p.SetOn(false);
    bool exact = true;
    for (size_t i = 0; i < 48000; i++)
    {
        const float in = .3f * sinf(i * .05f);
        float l = in, r = .5f * in;
        p.Process(&l, &r);
        if (i >= 9600)
            exact &= l == in && r == .5f * in;
    }
    Check(p.Idle() && exact, "sleep: off and faded out, the output is the input, bit for bit");

    // on again: the stages start from silence and the key fades them in, so what it changes
    // starts at nothing and grows without a step
    p.SetOn(true);
    float first = 0.f, step = 0.f, last = 0.f;
    for (size_t i = 0; i < 480; i++)
    {
        const float in = .3f * sinf((48000 + i) * .05f);
        float l = in, r = in;
        p.Process(&l, &r);
        if (i == 0)
            first = fabsf(l - in);
        step = fmaxf(step, fabsf((l - in) - last));
        last = l - in;
    }
    printf("      on again: off the input by %.5f on the first sample, by %.4f more a sample at most\n",
           first, step);
    Check(first < .002f && step < .02f, "sleep: it comes on again without a click, fading in from the input");
}

static void Rate()
{
    static Ph p;
    p.Init(kSr);
    char what[160];
    // the free half: the flanger's curve over 0 to just below the centre
    const struct
    {
        float knob, hz;
    } kFree[] = {{0.f, .02f}, {.15f, .1f}, {.25f, .7f}, {.3f, 1.f}, {.375f, 5.f}};
    bool free_ok = true;
    for (const auto& f : kFree)
    {
        p.SetParam(Ph::RATE, f.knob);
        free_ok &= !p.Synced() && fabsf(p.FreeHz() / f.hz - 1.f) < .001f;
    }
    p.SetParam(Ph::RATE, .49f);
    printf("      free: just below the centre %.1f Hz\n", p.FreeHz());
    free_ok &= !p.Synced() && p.FreeHz() > 40.f && p.FreeHz() < 50.f;
    Check(free_ok, "rate: the left half is free, .02 Hz at 0 to 50 Hz at the centre, as the flanger's");

    // the synced half: the filter LFO's divisions, 1/16 at the centre to 4 bars at the top
    static const uint32_t kWant[] = {3, 6, 12, 24, 48, 96, 192};
    bool synced_ok = true;
    for (size_t k = 0; k < Ph::kNumDivisions; k++)
    {
        p.SetParam(Ph::RATE, SplitStepValue(static_cast<float>(k), Ph::kNumDivisions));
        synced_ok &= p.Synced() && p.DivisionPulses() == kWant[k];
    }
    p.SetParam(Ph::RATE, .5f);
    synced_ok &= p.Synced() && p.DivisionPulses() == 3;
    Check(synced_ok, "rate: the right half is synced: 1/16 on the centre, then 1/8 ... 4 bars at the top");

    // the free LFO runs at its rate: 1 Hz, so the coefficient comes back to where it was each
    // second, and is elsewhere a quarter second on
    p.Init(kSr);
    p.SetParam(Ph::RATE, .3f);
    p.SetParam(Ph::DEPTH, 1.f);
    p.SetParam(Ph::CENTRE, .5f);
    p.SetOn(true);
    float c0 = 0.f, c_half = 0.f, c1 = 0.f;
    for (size_t i = 0; i <= 96000; i++)
    {
        float l = 0.f, r = 0.f;
        p.Process(&l, &r);
        if (i == 48000)
            c0 = p.CoeffNow(0);
        if (i == 48000 + 12000)
            c_half = p.CoeffNow(0);
        if (i == 96000)
            c1 = p.CoeffNow(0);
    }
    printf("      free at 1 Hz: coefficient %.4f, a quarter second on %.4f, a second on %.4f\n", c0,
           c_half, c1);
    Check(fabsf(c0 - c1) < .002f && fabsf(c0 - c_half) > .1f, "rate: the free LFO runs a cycle a second at 1 Hz");

    // synced, 1/4 (12 pulses): pulses 3 apart put it a quarter cycle on each, the top a
    // quarter after the beat, the centre at the half, the bottom at three quarters
    for (uint32_t pos : {0u, 3u, 6u, 9u})
    {
        p.Init(kSr);
        p.SetParam(Ph::RATE, SplitStepValue(2.f, Ph::kNumDivisions));
        p.SetParam(Ph::DEPTH, 1.f);
        p.SetParam(Ph::CENTRE, .5f);
        p.SetPulseSamples(0.f);
        p.ClockPulse(pos);
        p.SetOn(true);
        for (size_t i = 0; i < 16; i++)
        {
            float l = 0.f, r = 0.f;
            p.Process(&l, &r);
        }
        const float tri = pos == 3 ? 1.f : (pos == 9 ? -1.f : 0.f);
        const float want = Ph::Coeff(Ph::BreakHz(.5f, 1.f, tri), kSr);
        snprintf(what, sizeof what, "rate: synced to 1/4, pulse %u of 12 puts the sweep at %+.0f", pos, tri);
        Check(fabsf(p.CoeffNow(0) - want) < 1e-4f, what);
    }

    // between pulses it moves on at the clock's pace: 1/16 at 120 BPM (2000 samples a pulse), a
    // pulse in, a third of the way round
    p.Init(kSr);
    p.SetParam(Ph::RATE, .5f);
    p.SetParam(Ph::DEPTH, 1.f);
    p.SetParam(Ph::CENTRE, .5f);
    p.SetPulseSamples(2000.f);
    p.ClockPulse(0);
    p.SetOn(true);
    for (size_t i = 0; i < 1000; i++)
    {
        float l = 0.f, r = 0.f;
        p.Process(&l, &r);
    }
    // half a pulse in: 1/6 of the cycle past the centre, on the way up: 2/3 of the way to the top
    const float want = Ph::Coeff(Ph::BreakHz(.5f, 1.f, 4.f / 6.f), kSr);
    printf("      synced 1/16, half a pulse on: %.4f (%.4f wanted)\n", p.CoeffNow(0), want);
    Check(fabsf(p.CoeffNow(0) - want) < .005f, "rate: synced, it glides on between the clock's pulses");
}

int main()
{
    Notches();
    DryAtDepth0();
    Feedback();
    Stereo();
    Sleep();
    Rate();
    return Finish();
}
