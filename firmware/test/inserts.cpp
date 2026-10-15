// inserts.cpp: checks the effects that had none of their own, the folder (FxFolder.h), the
// slicer (FxSlicer.h) and the filter (FxFilter.h): the folder's bypass when off, its level at
// 1x (down to 30Hz), that nothing matches it to the input and that Symmetry turned on silence
// doesn't thump; the slicer's patterns on the clock's 16ths, its chance and its stereo; the filter's centre (the input, bit for bit, at any resonance), its
// -3dB points and resonance peak where WAVE's were (#304), and its LFO sweeping a low sine
// across the centre without a step; and page 2's Mix on the shifter (FxOutput.h). Exits 0 when
// everything passes. Run by unit.sh inserts.
#include <cmath>
#include <cstdio>
#include "check.h"
#include "FxChain.h"

using namespace chompi;

static const float kSr = 48000.f;

// ======== the folder ========

/** The folder at a drive, on a sine of amp at hz: the output's level over the input's in dB,
 *  over the last half of a second */
static float FolderOver(float drive, float amp, float shape = 0.f, float tone = 1.f, float hz = 220.f)
{
    Folder f;
    f.Init(kSr);
    f.SetParam(Folder::DRIVE, drive);
    f.SetParam(Folder::SHAPE, shape);
    f.SetParam(Folder::TONE, tone);
    f.SnapParams();
    f.SetOn(true);
    double in_sum = 0., out_sum = 0.;
    for (size_t i = 0; i < 48000; i++)
    {
        float l = amp * sinf(2.f * float(M_PI) * hz * i / kSr), r = l;
        const float in = l;
        f.Process(&l, &r);
        if (i >= 24000)
        {
            in_sum += 2. * in * in;
            out_sum += l * l + r * r;
        }
    }
    return static_cast<float>(10. * log10(out_sum / in_sum));
}

static void TestFolder()
{
    // off: what goes in comes out, bit for bit (its key's fade at 0)
    Folder f;
    f.Init(kSr);
    f.SetParam(Folder::DRIVE, 1.f);
    f.SnapParams();
    bool same = true;
    for (size_t i = 0; i < 48000; i++)
    {
        const float in = .5f * sinf(.01f * i);
        float l = in, r = -in;
        f.Process(&l, &r);
        same = same && l == in && r == -in;
    }
    Check(same, "folder: off, the input passes untouched, bit for bit");

    // the tone open (the panel's default; the effect's Init leaves it at its darkest)
    const float unity = FolderOver(0.f, .03f), driven = FolderOver(1.f, .03f);
    printf("  folder: %+.1fdB at 1x, %+.1fdB at full drive, both on a sine at -30dBFS\n", unity,
           driven);
    Check(fabsf(unity) < 1.f, "folder: at 1x a quiet signal comes out at its own level");
    // the bass too: its DC blocker at 15Hz (daisysp::DcBlock's 76Hz took 5dB off 50Hz)
    const float bass = FolderOver(0.f, .03f, 0.f, 1.f, 50.f), deep = FolderOver(0.f, .03f, 0.f, 1.f, 30.f);
    printf("  folder: at 1x %+.1fdB at 50Hz, %+.1fdB at 30Hz\n", bass, deep);
    Check(fabsf(bass) < .5f && fabsf(deep) < 1.5f, "folder: at 1x the bass comes out at its own level, 50Hz within 0.5dB");
    const float tri = FolderOver(0.f, .03f, 1.f);
    printf("  folder: the triangle at 1x %+.1fdB\n", tri);
    Check(fabsf(tri) < 1.f, "folder: so does the triangle: the shape doesn't change the level at 1x");
    Check(driven > 15.f, "folder: driven, a quiet signal comes out far louder: nothing matches "
                         "it to the input (page 2's Level is for that)");
    // Symmetry turned all the way at a recall's slew on silence: the bias's DC is taken off
    // before the DC blocker, which let it through as a thump (0.2 at 76Hz)
    {
        Folder g;
        g.Init(kSr);
        g.SetParam(Folder::TONE, 1.f);
        g.SnapParams();
        g.SetOn(true);
        float peak = 0.f;
        for (size_t i = 0; i < 24000; i++)
        {
            if (i == 4800)
            {
                FxSlew::coeff = kFxRecallCoeff;
                g.SetParam(Folder::SYMMETRY, 1.f);
            }
            float l = 0.f, r = 0.f;
            g.Process(&l, &r);
            peak = fmaxf(peak, fabsf(l));
        }
        FxSlew::coeff = kFxParamCoeff;
        printf("  folder: Symmetry 0 to 1 on silence, peak %.4f\n", peak);
        Check(peak < .01f, "folder: Symmetry turned on silence doesn't thump");
    }
    // #37: the tone is a tone control: darker is quieter, at the same drive
    const float open = FolderOver(.6f, .3f, 0.f, 1.f), dark = FolderOver(.6f, .3f, 0.f, .2f);
    printf("  folder: drive .6 on -10dBFS, tone open %+.1fdB, at .2 %+.1fdB\n", open, dark);
    Check(dark < open - 3.f, "folder: a darker tone is quieter (#37)");
}

// ======== the slicer ========

/** The slicer on a steady input of 1 for bars at 120 BPM (a pulse every 2000 samples): the
 *  envelope hits on each channel, counted by its rising past .5 */
static void SlicerHits(float pattern, float chance, float stereo, int bars, int* hits_l, int* hits_r)
{
    Slicer s;
    s.Init(kSr);
    s.SetParam(Slicer::PATTERN, pattern);
    s.SetParam(Slicer::DECAY, 0.f); // the shortest: every hit stands alone
    s.SetParam(Slicer::CHANCE, chance);
    s.SetParam(Slicer::STEREO, stereo);
    s.SetOn(true);
    const uint32_t kSamplesPerPulse = 2000;
    float last[2] = {0.f, 0.f};
    *hits_l = *hits_r = 0;
    uint32_t pos = 0;
    // a 16th first, for the key's own press to fall away
    const uint32_t samples = (bars * 16 + 1) * kPulsesPer16th * kSamplesPerPulse;
    for (uint32_t i = 0; i < samples; i++)
    {
        if (i % kSamplesPerPulse == 0)
            s.ClockPulse(pos++);
        float l = 1.f, r = 1.f;
        s.Process(&l, &r);
        if (i >= kPulsesPer16th * kSamplesPerPulse)
        {
            *hits_l += l > .5f && last[0] <= .5f;
            *hits_r += r > .5f && last[1] <= .5f;
        }
        last[0] = l;
        last[1] = r;
    }
}

static void TestSlicer()
{
    const size_t n = Slicer::kNumPatterns;
    bool all = true;
    for (size_t p = 0; p < n; p++)
    {
        int l, r;
        SlicerHits((p + .5f) / n, 0.f, 0.f, 2, &l, &r);
        int want = 0;
        for (int b = 0; b < 8; b++)
            want += (kSlicerPatterns[p] >> b) & 1;
        want *= 4; // 8 steps are half a bar: 2 bars play the pattern 4 times
        if (l != want || r != want)
        {
            printf("  pattern %zu: %d / %d hits, want %d\n", p, l, r, want);
            all = false;
        }
    }
    Check(all, "slicer: every pattern hits on its steps of the clock's 16ths, both channels alike");

    int l, r, cl, cr;
    SlicerHits(1.f, 0.f, 0.f, 4, &l, &r);
    SlicerHits(1.f, 1.f, 0.f, 4, &cl, &cr);
    printf("  slicer: every 16th: %d hits in 4 bars, with chance at full %d\n", l, cl);
    Check(l == 64 && cl < 32 && cl > 0 && cl == cr,
          "slicer: chance flips steps at random, on both channels alike");

    int sl, sr;
    SlicerHits(.5f / n, 0.f, (2.5f) / n, 2, &sl, &sr);
    printf("  slicer: the sparsest pattern with stereo 2: %d hits left, %d right\n", sl, sr);
    Check(sl != sr, "slicer: stereo plays different patterns left and right");
}

// ======== the filter ========

/** The filter at a cutoff and resonance, LFO off, on a sine of hz: its gain in dB over the
 *  last half of 0.3s */
static float FilterGain(float cutoff, float res, float hz)
{
    static Filter f;
    f.Init(kSr);
    f.SetParam(Filter::CUTOFF, cutoff);
    f.SetParam(Filter::RESONANCE, res);
    f.SnapParams();
    f.SetOn(true);
    for (size_t i = 0; i < 4800; i++) // the key's fade in
    {
        float l = 0.f, r = 0.f;
        f.Process(&l, &r);
    }
    double in_sum = 0., out_sum = 0.;
    const size_t n = 14400;
    for (size_t i = 0; i < n; i++)
    {
        const float x = sinf(2.f * static_cast<float>(M_PI) * hz * static_cast<float>(i) / kSr);
        float l = x, r = x;
        f.Process(&l, &r);
        if (i >= n / 2)
        {
            in_sum += x * x;
            out_sum += l * l;
        }
    }
    return 10.f * log10f(static_cast<float>(out_sum / in_sum));
}

/** Where the gain crosses -3dB between lo and hi Hz (the passband at lo for a lowpass, at hi
 *  for a highpass), by bisection in octaves */
static float FilterMinus3(float cutoff, float res, float lo, float hi)
{
    const bool lp = cutoff < .5f;
    for (int k = 0; k < 24; k++)
    {
        const float mid = sqrtf(lo * hi);
        if ((FilterGain(cutoff, res, mid) > -3.f) == lp)
            lo = mid;
        else
            hi = mid;
    }
    return sqrtf(lo * hi);
}

static void TestFilter()
{
    // the centre: the input itself, whatever the resonance
    bool dry = true;
    for (float res : {0.f, .5f, 1.f})
    {
        static Filter f;
        f.Init(kSr);
        f.SetParam(Filter::CUTOFF, .5f);
        f.SetParam(Filter::RESONANCE, res);
        f.SnapParams();
        f.SetOn(true);
        for (size_t i = 0; i < 24000; i++)
        {
            const float x = .5f * sinf(static_cast<float>(i) * .37f) + .3f * sinf(static_cast<float>(i) * .011f);
            float l = x, r = -x;
            f.Process(&l, &r);
            dry = dry && l == x && r == -x;
        }
    }
    Check(dry, "filter: at the centre it's the input, bit for bit, at any resonance");

    // WAVE's -3dB points at the default resonance (50%), measured on its filter (#304)
    struct Point
    {
        float cutoff, lo, hi, wave_hz;
    };
    const Point points[] = {{.1f, 20.f, 1000.f, 88.f},    {.3f, 200.f, 20000.f, 2368.f},
                            {.45f, 1000.f, 20000.f, 10470.f}, {.65f, 10.f, 2000.f, 80.f},
                            {.8f, 50.f, 10000.f, 915.f},  {1.f, 200.f, 20000.f, 4763.f}};
    float worst = 0.f;
    for (const Point& p : points)
    {
        const float hz = FilterMinus3(p.cutoff, .5f, p.lo, p.hi);
        const float st = 12.f * log2f(hz / p.wave_hz);
        printf("      filter: cutoff %.2f, -3dB at %.0fHz (WAVE's %.0fHz, %+.1f semitones)\n", p.cutoff, hz,
               p.wave_hz, st);
        worst = fmaxf(worst, fabsf(st));
    }
    Check(worst < 1.f, "filter: at the default resonance the cutoff is within a semitone of WAVE's");

    // the resonance at the top: a peak of ~20dB at the cutoff, as WAVE's 19-21dB
    float peak = -100.f;
    for (float hz = 300.f; hz < 900.f; hz *= 1.005f)
        peak = fmaxf(peak, FilterGain(.2f, 1.f, hz));
    printf("      filter: resonance at the top, a %.1fdB peak\n", peak);
    Check(peak > 18.f && peak < 22.f, "filter: the resonance peaks at ~20dB at the top");

    // the LFO sweeping a 50Hz sine across the centre, lowpass to highpass and back, at full
    // depth, 1/16 and the default resonance: no step beyond the sine's own (.0033 a sample)
    // and the sweep's, where the one filter changes sides (clicks.cpp has the cutoff jumping
    // across)
    {
        static Filter f;
        f.Init(kSr);
        f.SetParam(Filter::CUTOFF, .5f);
        f.SetParam(Filter::RESONANCE, .5f);
        f.SetParam(Filter::LFO_DEPTH, 1.f);
        f.SetParam(Filter::LFO_DIVISION, 0.f);
        f.SnapParams();
        f.SetPulseSamples(2000.f);
        f.SetOn(true);
        float last = 0.f, step = 0.f;
        for (size_t i = 0; i < 96000; i++)
        {
            if (i % 2000 == 0)
                f.ClockPulse(static_cast<uint32_t>(i / 2000));
            float l = .5f * sinf(2.f * static_cast<float>(M_PI) * 50.f * static_cast<float>(i) / kSr), r = l;
            f.Process(&l, &r);
            if (i > 9600)
                step = fmaxf(step, fabsf(l - last));
            last = l;
        }
        printf("      filter: a 50Hz sine swept across the centre, largest step %.4f\n", step);
        Check(step < .015f, "filter: the LFO sweeps a low sine across the centre without a step");
    }
}

// ======== page 2's Mix on the shifter (FxOutput.h) ========

static void TestShifterMix()
{
    // three shifters a fifth up on the same input, the mix at 1, .5 and 0: what each adds to
    // the input is in that proportion
    static Shifter sh[3];
    static FxOutput out[3];
    const float mixes[3] = {1.f, .5f, 0.f};
    for (size_t k = 0; k < 3; k++)
    {
        sh[k].Init(kSr);
        out[k].Init(kSr);
        sh[k].SetParam(Shifter::SHIFT, .5f + 7.f / 24.f);
        out[k].SetParam(FxOutput::kMix, mixes[k]);
        out[k].Snap();
        sh[k].SnapParams();
        sh[k].SetOn(true);
    }
    float worst_half = 0.f, worst_dry = 0.f, wet = 0.f;
    for (size_t i = 0; i < 24000; i++)
    {
        const float in = .5f * sinf(2.f * float(M_PI) * 220.f * i / kSr);
        float l[3], r[3];
        for (size_t k = 0; k < 3; k++)
        {
            l[k] = r[k] = in;
            out[k].Process(sh[k], &l[k], &r[k]);
        }
        if (i < 4800)
            continue;
        wet = fmaxf(wet, fabsf(l[0] - in));
        worst_half = fmaxf(worst_half, fabsf((l[1] - in) - .5f * (l[0] - in)));
        worst_dry = fmaxf(worst_dry, fabsf(l[2] - in));
    }
    printf("  shifter mix: full adds up to %.3f, half off by %.2g, none by %.2g\n", wet, worst_half,
           worst_dry);
    Check(wet > .1f && worst_half < 1e-5f, "shifter mix: at .5, half of what fully shifted adds");
    Check(worst_dry < 1e-6f, "shifter mix: at 0, the input passes");
}

int main()
{
    TestShifterMix();
    TestFolder();
    TestSlicer();
    TestFilter();
    return Finish();
}
