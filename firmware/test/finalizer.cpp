// finalizer.cpp: checks the finalizer's sound (Finalizer.h, #345). Run by unit.sh finalizer.
//  - neutral: every knob on its default is an exact bypass, also while a meter runs the bands,
//    and again once a knob turned away is turned back and the module has gone to sleep
//  - the three bands sum flat: with the bands running and nothing changed, a sine at any
//    frequency comes out within 0.1dB (Linkwitz-Riley 4th order sums to an allpass)
//  - each band's compressor works on a tone in its band only, by its curve: a steady tone
//    reduced as the character's ratio and knee say, the other bands untouched
//  - Width at full left is mono: both outputs the same
//  - Space, right of Width's centre (#455): a mono source gets wider (the correlation down, the
//    meter's at .6 at the right), its mono sum stays what it was (mid's and high's sample by
//    sample, the low band's as loud at every frequency), nothing of the mid's and high's Space
//    below the low split, each channel's level up by about 1dB at most and the two about as
//    loud, the low band's too; at or left of the centre its allpasses don't run, and turned
//    back it sleeps again, exact
//  - the drive's level compensation: a sine at its reference level comes out within 1dB, every
//    type and amount
//  - the limiter: pushed hard by Loud, no peak above its ceiling of -1dB, also between the
//    samples (a 2x-oversampled check), and its delay exactly 1ms (Latency)
//  - the meters against ffmpeg's (a reference of its own, LR4 bands from acrossover): each
//    band's correlation (aphasemeter), each band's peak and the input's (astats), the
//    limiter's true peak (ebur128, 4x). Without ffmpeg these print SKIP
// Exits 0 when everything passes.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include "check.h"
#define private public // Space's state (space_run_)
#include "Finalizer.h"
#undef private

using namespace chompi;

static const float kSr = 48000.f;
static const size_t kBlock = 24;

static float Db(float x) { return 20.f * log10f(fmaxf(fabsf(x), 1e-12f)); }

static void CheckF(bool ok, const char* fmt, double a, double b = 0., double c = 0.)
{
    char buf[256];
    snprintf(buf, sizeof(buf), fmt, a, b, c);
    Check(ok, buf);
}

/** A finalizer with these parameters, the rest on their defaults */
struct Setup
{
    Finalizer f;
    explicit Setup(std::initializer_list<std::pair<int, float>> params = {})
    {
        f.Init(kSr);
        for (auto& p : params)
            f.SetParam(p.first, p.second);
    }
    /** Runs l and r through it in blocks of kBlock, in place */
    void Run(std::vector<float>& l, std::vector<float>& r)
    {
        for (size_t i = 0; i + kBlock <= l.size(); i += kBlock)
            f.Process(&l[i], &r[i], kBlock);
    }
};

static uint32_t seed = 1;
static float Noise()
{
    seed = seed * 1664525u + 1013904223u;
    return static_cast<float>(seed >> 8) / 16777216.f - .5f;
}

static std::vector<float> Sine(size_t n, float hz, float amp, float phase = 0.f)
{
    std::vector<float> v(n);
    for (size_t i = 0; i < n; i++)
        v[i] = amp * sinf(2.f * static_cast<float>(M_PI) * hz * i / kSr + phase);
    return v;
}

static float Rms(const std::vector<float>& v, size_t from, size_t to)
{
    double s = 0.;
    for (size_t i = from; i < to; i++)
        s += static_cast<double>(v[i]) * v[i];
    return static_cast<float>(sqrt(s / (to - from)));
}

// ---------------------------------------------------------------------------------------------

static void TestNeutral()
{
    const size_t n = 48000;
    std::vector<float> l(n), r(n);
    for (size_t i = 0; i < n; i++)
    {
        l[i] = 3.f * Noise();
        r[i] = 3.f * Noise();
    }
    for (int meter = 0; meter <= Finalizer::kNoMeter; meter++)
    {
        Setup s;
        s.f.SetMeter(static_cast<uint8_t>(meter));
        std::vector<float> ol = l, orr = r;
        s.Run(ol, orr);
        CheckF(ol == l && orr == r, "neutral: an exact bypass, the meters of module %g showing", meter);
    }
    {
        Setup s;
        std::vector<float> ol = l, orr = r;
        s.Run(ol, orr);
        Check(s.f.Asleep(), "neutral: asleep");
    }
    // every knob turned away, then back: asleep again, and exact
    for (size_t p = 0; p < kNumFinParams; p++)
    {
        const FxParams& m = kFinalizerKnobs[p / kNumFxKnobs];
        if (!(m.knobs >> (p % kNumFxKnobs) & 1))
            continue;
        Setup s;
        const float def = m.defaults[p % kNumFxKnobs];
        s.f.SetParam(p, def < .5f ? def + .4f : def - .3f);
        std::vector<float> ol = l, orr = r;
        s.Run(ol, orr);
        s.f.SetParam(p, def);
        ol = l;
        orr = r;
        // the knob settles, a reduction releases (Depth's, up to 4s), the module fades out and
        // sleeps
        for (int k = 0; k < 5; k++)
        {
            ol = l;
            orr = r;
            s.Run(ol, orr);
        }
        CheckF(ol == l && orr == r && s.f.Asleep(), "neutral: parameter %g turned and back, exact and asleep again", p);
    }
}

static void TestFlat()
{
    const float freqs[] = {30.f, 100.f, 250.f, 300.f, 400.f, 1000.f, 2500.f, 3000.f, 4000.f, 10000.f, 18000.f};
    float worst = 0.f;
    for (float hz : freqs)
    {
        // the bands awake (Depth a hair up), the tone far below where they'd compress
        Setup s({{Finalizer::DEPTH, .001f}});
        const size_t n = 48000;
        std::vector<float> l = Sine(n, hz, .1f), r = Sine(n, hz, .1f, 1.f);
        const std::vector<float> in = l;
        s.Run(l, r);
        worst = fmaxf(worst, fabsf(Db(Rms(l, n / 2, n)) - Db(Rms(in, n / 2, n))));
    }
    CheckF(worst < .1f, "bands: the three sum flat, 30Hz-18kHz within %.3f dB (< 0.1)", worst);
}

static void TestBandComp()
{
    // Depth full, Standard & slow (ratio 3, a knee of 6dB, no offset of its own): a tone of
    // -12dB is 18dB over the threshold of -30dB, so 12dB down
    const float freqs[] = {100.f, 1000.f, 8000.f};
    const float expect = (1.f / 3.f - 1.f) * 18.f;
    for (size_t band = 0; band < Finalizer::kBands; band++)
    {
        Setup s({{Finalizer::DEPTH, 1.f}});
        const size_t n = 96000;
        std::vector<float> l = Sine(n, freqs[band], .25f), r = l;
        const std::vector<float> in = l;
        s.Run(l, r);
        // the high band's peak, sampled at 6 a cycle, reads up to 1.25dB low
        const float tol = band == 2 ? 1.5f : .5f;
        const float gr = s.f.BandReduction(band);
        bool others = true;
        for (size_t b = 0; b < Finalizer::kBands; b++)
            others &= b == band || s.f.BandReduction(b) > -.1f;
        CheckF(fabsf(gr - expect) < tol && others,
               "comp: a tone at %gHz, 12dB down in its band only (%.2f dB, the others untouched)", freqs[band], gr);
        const float out = Db(Rms(l, n / 2, n)) - Db(Rms(in, n / 2, n));
        CheckF(fabsf(out - expect) < tol + .2f, "comp: and the tone at %gHz comes out %.2f dB", freqs[band], out);
    }
    // below the knee nothing
    Setup s({{Finalizer::DEPTH, .2f}});
    std::vector<float> l = Sine(48000, 1000.f, .1f), r = l;
    s.Run(l, r);
    CheckF(s.f.BandReduction(1) == 0.f, "comp: a tone 14dB below the threshold, untouched (%.3f dB)", s.f.BandReduction(1));
}

static void TestWidth()
{
    Setup s({{Finalizer::WIDTH_LOW, 0.f}, {Finalizer::WIDTH_LOW + 1, 0.f}, {Finalizer::WIDTH_LOW + 2, 0.f}});
    const size_t n = 24000;
    std::vector<float> l(n), r(n);
    for (size_t i = 0; i < n; i++)
    {
        l[i] = Noise();
        r[i] = Noise();
    }
    s.Run(l, r);
    bool mono = true;
    for (size_t i = n / 2; i < n; i++)
        mono &= l[i] == r[i];
    Check(mono, "width: all three at the left, mono (both outputs the same)");
}

/** Space (#455): mono pinkish noise through the bands at widths wl, wm, wh (the bands awake,
 *  Depth a hair up), the last second's L and R */
static void SpaceRun(float wl, float wm, float wh, const std::vector<float>& in, std::vector<float>& l,
                     std::vector<float>& r, Setup* keep = nullptr)
{
    Setup s({{Finalizer::DEPTH, .001f}, {Finalizer::WIDTH_LOW, wl}, {Finalizer::WIDTH_LOW + 1, wm},
             {Finalizer::WIDTH_LOW + 2, wh}});
    l = in;
    r = in;
    s.Run(l, r);
    if (keep)
        keep->f = s.f;
}

static void TestSpace()
{
    // its own noise, so the checks after it get theirs as before
    const uint32_t seed_before = seed;
    const size_t n = 3 * 48000, from = 48000;
    std::vector<float> in(n);
    float lp = 0.f;
    for (size_t i = 0; i < n; i++)
    {
        const float w = Noise();
        lp += .06f * (w - lp);
        in[i] = .3f * (2.f * lp + .3f * w);
    }
    seed = seed_before;
    std::vector<float> cl, cr, l, r;
    Setup centre, full;
    SpaceRun(.5f, .5f, .5f, in, cl, cr, &centre);
    Check(!centre.f.space_run_, "space: Width at the centre, its allpasses asleep");
    {
        Setup left;
        SpaceRun(.2f, .3f, .4f, in, l, r, &left);
        Check(!left.f.space_run_, "space: Width left of the centre, its allpasses asleep");
    }
    {
        // mid's and high's Space only into the side: the mono sum is the centre's, sample by sample
        SpaceRun(.5f, 1.f, 1.f, in, l, r);
        double fold = 0., err = 0.;
        for (size_t i = from; i < n; i++)
        {
            const double m = .5 * (static_cast<double>(cl[i]) + cr[i]);
            const double d = .5 * (static_cast<double>(l[i]) + r[i]) - m;
            fold += m * m;
            err += d * d;
        }
        CheckF(10. * log10(err / fold) < -100., "space: mid and high at the right, the mono sum the centre's (the difference %.1f dB, < -100)",
               10. * log10(err / fold));
    }
    SpaceRun(1.f, 1.f, 1.f, in, l, r, &full);
    Check(full.f.space_run_, "space: Width at the right, its allpasses running");
    double sl = 0., sr = 0., slr = 0., c2 = 0.;
    for (size_t i = from; i < n; i++)
    {
        sl += static_cast<double>(l[i]) * l[i];
        sr += static_cast<double>(r[i]) * r[i];
        slr += static_cast<double>(l[i]) * r[i];
        c2 += static_cast<double>(cl[i]) * cl[i];
    }
    const double corr = slr / sqrt(sl * sr);
    CheckF(corr < .7, "space: a mono source with every band at the right is wider (correlation %.3f, < .7)", corr);
    const double dl = 10. * log10(sl / c2), dr = 10. * log10(sr / c2), dp = 10. * log10((sl + sr) / (2. * c2));
    // together at most 1 + k^2 (.97dB at k .5), less as the bands overlap at the splits; the
    // two channels about as loud: nothing leans
    CheckF(dl < 1.5 && dr < 1.5 && dp > .5 && dp < 1. && fabs(dl - dr) < 1.,
           "space: each channel up by %.2f and %.2f dB, together %.2f dB (.5 to 1, apart < 1)", dl, dr, dp);
    // the low band's Space, a quarter turn: both channels as loud, and the mono sum as loud
    // as the centre's (its phase turned)
    for (float hz : {40.f, 60.f, 100.f, 150.f})
    {
        std::vector<float> x = Sine(48000, hz, .3f), xl, xr, yl, yr;
        SpaceRun(.5f, .5f, .5f, x, yl, yr);
        SpaceRun(1.f, .5f, .5f, x, xl, xr);
        double a = 0., b = 0., f = 0., f0 = 0.;
        for (size_t i = 24000; i < 48000; i++)
        {
            a += static_cast<double>(xl[i]) * xl[i];
            b += static_cast<double>(xr[i]) * xr[i];
            f += .25 * (static_cast<double>(xl[i]) + xr[i]) * (static_cast<double>(xl[i]) + xr[i]);
            f0 += .25 * (static_cast<double>(yl[i]) + yr[i]) * (static_cast<double>(yl[i]) + yr[i]);
        }
        CheckF(fabs(10. * log10(a / b)) < 1. && fabs(10. * log10(f / f0)) < .1,
               "space: the low band at the right, %gHz: the channels %.2f dB apart (< 1), the mono sum %.3f dB (< .1)",
               hz, 10. * log10(a / b), 10. * log10(f / f0));
    }
    for (size_t b = 0; b < Finalizer::kBands; b++)
        CheckF(fabsf(full.f.BandCorrelation(b) - .6f) < .05f && Finalizer::CorrelationLed(full.f.BandCorrelation(b)) == 1,
               "space: band %g's correlation meter on a mono source at the right %.3f (.6, yellow)", b,
               full.f.BandCorrelation(b));
    {
        // halfway right: Space at a quarter
        SpaceRun(.75f, .75f, .75f, in, l, r);
        double a = 0., b = 0., ab = 0.;
        for (size_t i = from; i < n; i++)
        {
            a += static_cast<double>(l[i]) * l[i];
            b += static_cast<double>(r[i]) * r[i];
            ab += static_cast<double>(l[i]) * r[i];
        }
        CheckF(ab / sqrt(a * b) < .95 && ab / sqrt(a * b) > corr,
               "space: halfway right, wider than at the centre, narrower than at the right (correlation %.3f)",
               ab / sqrt(a * b));
    }
    {
        // mid's and high's Space leaves the low band mono: a mono 80Hz tone
        std::vector<float> x = Sine(n, 80.f, .3f);
        SpaceRun(.5f, 1.f, 1.f, x, l, r);
        double s2 = 0., x2 = 0.;
        for (size_t i = from; i < n; i++)
        {
            const double s = .5 * (static_cast<double>(l[i]) - r[i]);
            s2 += s * s;
            x2 += static_cast<double>(x[i]) * x[i];
        }
        CheckF(10. * log10(s2 / x2) < -45., "space: mid and high at the right, an 80Hz tone's side %.1f dB (< -45)",
               10. * log10(s2 / x2));
    }
    // each band's Width turned right and back: Space asleep again, the bands too, exact
    for (size_t b = 0; b < Finalizer::kBands; b++)
    {
        Setup s({{Finalizer::WIDTH_LOW + b, .9f}});
        std::vector<float> ol = in, orr = in;
        s.Run(ol, orr);
        s.f.SetParam(Finalizer::WIDTH_LOW + b, .5f);
        for (int k = 0; k < 3; k++)
        {
            ol = in;
            orr = in;
            s.Run(ol, orr);
        }
        CheckF(ol == in && orr == in && s.f.Asleep() && !s.f.space_run_,
               "space: band %g's Width right and back to the centre, exact and asleep again", b);
    }
}

static void TestMute()
{
    // a band muted adds nothing: the other two muted and it alone add up to all three
    const size_t n = 24000;
    std::vector<float> l(n), r(n);
    for (size_t i = 0; i < n; i++)
    {
        l[i] = Noise();
        r[i] = Noise();
    }
    std::vector<float> out[4][2];
    for (int m = 0; m < 4; m++)
    {
        // m 0-2: band m muted; 3: the other two, band 0 alone
        Setup s({{Finalizer::DEPTH, .001f}}); // the bands running, as with a mute
        if (m < 3)
            s.f.SetMute(m, true);
        else
        {
            s.f.SetMute(1, true);
            s.f.SetMute(2, true);
        }
        out[m][0] = l;
        out[m][1] = r;
        s.Run(out[m][0], out[m][1]);
    }
    Setup all({{Finalizer::DEPTH, .001f}});
    std::vector<float> al = l, ar = r;
    all.Run(al, ar);
    float worst = 0.f;
    for (size_t i = n / 2; i < n; i++)
        worst = fmaxf(worst, fabsf(out[0][0][i] + out[3][0][i] - al[i]));
    CheckF(Db(worst) - Db(Rms(al, n / 2, n)) < -80.f, "mute: the low band muted adds nothing, %.1f dB of what plays", Db(worst) - Db(Rms(al, n / 2, n)));
    // all three muted: silence
    Setup s;
    for (int b = 0; b < 3; b++)
        s.f.SetMute(b, true);
    std::vector<float> ml = l, mr = r;
    s.Run(ml, mr);
    CheckF(Rms(ml, n / 2, n) == 0.f && Rms(mr, n / 2, n) == 0.f, "mute: all three muted, silence%s", 0);
    // and a bass deep in the low band, 25Hz, muted there: 80dB down (the crossover's slope)
    Setup b;
    b.f.SetMute(0, true);
    std::vector<float> bl = Sine(48000, 25.f, .5f), br = bl;
    const std::vector<float> in = bl;
    b.Run(bl, br);
    const float db = Db(Rms(bl, 24000, 48000)) - Db(Rms(in, 24000, 48000));
    CheckF(db < -80.f, "mute: the low band muted, 25Hz comes out %.1f dB", db);
    // unmuted again: asleep, exact
    for (int k = 0; k < 3; k++)
        b.f.SetMute(k, false);
    for (int k = 0; k < 2; k++)
    {
        bl = in;
        br = in;
        b.Run(bl, br);
    }
    Check(bl == in && b.f.Asleep(), "mute: unmuted, asleep again and exact");
}

static void TestDrive()
{
    for (int type = 0; type < kFinTypes; type++)
        for (float drive : {.1f, .5f, 1.f})
        {
            Setup s({{Finalizer::DRIVE, drive}, {Finalizer::TYPE, type / 2.f}});
            const size_t n = 48000;
            std::vector<float> l = Sine(n, 200.f, .5f), r = l;
            const std::vector<float> in = l;
            s.Run(l, r);
            const float db = Db(Rms(l, n / 2, n)) - Db(Rms(in, n / 2, n));
            char what[128];
            snprintf(what, sizeof(what), "drive: type %d at %.1f, a sine of .5 comes out %.2f dB (within 1)", type, drive, db);
            Check(fabsf(db) < 1.f, what);
        }
}

/** v at twice the rate, a windowed sinc of 64 taps: its largest magnitude */
static float TruePeak2x(const std::vector<float>& v, size_t from)
{
    const int taps = 32;
    float peak = 0.f;
    for (size_t i = from + taps; i + taps < v.size(); i++)
    {
        double mid = 0.;
        for (int k = -taps + 1; k <= taps; k++)
        {
            const double x = k - .5;
            const double w = .5 + .5 * cos(M_PI * x / taps);
            mid += v[i + k] * sin(M_PI * x) / (M_PI * x) * w;
        }
        peak = fmaxf(peak, fmaxf(fabsf(v[i]), fabsf(static_cast<float>(mid))));
    }
    return peak;
}

/** Something to limit, as music has it: noise rolling off from 9kHz (four one-poles), a bass,
 *  a tone at 11kHz and bursts, peaking about .8. The limiter's detector estimates the peaks
 *  between the samples flat to 20kHz (Between): full-band white noise limited by 15dB, with
 *  as much above 20kHz as below, can overshoot by up to .4dB. Music has little up there */
static void Program(std::vector<float>& l, std::vector<float>& r)
{
    float a[4] = {}, b[4] = {};
    const float c = .7f;
    for (size_t i = 0; i < l.size(); i++)
    {
        const float burst = (i % 9600) < 960 ? 1.f : .2f;
        const float bass = .3f * sinf(2.f * static_cast<float>(M_PI) * 55.f * i / kSr);
        const float hi = .2f * sinf(2.f * static_cast<float>(M_PI) * 11025.3f * i / kSr);
        float x = Noise(), y = Noise();
        for (int k = 0; k < 4; k++)
        {
            a[k] += c * (x - a[k]);
            b[k] += c * (y - b[k]);
            x = a[k];
            y = b[k];
        }
        l[i] = burst * (.8f * x + bass + hi);
        r[i] = burst * (.8f * y + bass - hi);
    }
}

static std::vector<float> lim_out_l, lim_out_r;

static void TestLimiter()
{
    Setup s({{Finalizer::LOUD, 1.f}});
    const size_t n = 96000;
    std::vector<float> l(n), r(n);
    Program(l, r);
    s.Run(l, r);
    const float peak = fmaxf(TruePeak2x(l, 9600), TruePeak2x(r, 9600));
    CheckF(peak <= powf(10.f, -1.f / 20.f), "limiter: Loud full, its 2x-oversampled peak %.2f dB (<= -1)", Db(peak));
    lim_out_l = l;
    lim_out_r = r;
    CheckF(s.f.Latency() == 48, "limiter: Loud up, a delay of %g samples (1ms)", s.f.Latency());

    // Loud a hair up and nothing to limit: the input 1ms later, about as loud
    Setup q({{Finalizer::LOUD, .001f}});
    std::vector<float> a = Sine(n, 220.f, .1f), b = a;
    const std::vector<float> in = a;
    q.Run(a, b);
    float worst = 0.f;
    for (size_t i = n / 2; i < n; i++)
        worst = fmaxf(worst, fabsf(a[i] - in[i - 48] * powf(10.f, .001f * 18.f / 20.f)));
    CheckF(worst < 1e-4f, "limiter: nothing over the ceiling, the input exactly 48 samples later (%.1e off)", worst);
    Setup z;
    CheckF(z.f.Latency() == 0, "limiter: Loud at 0, no delay (%g)", z.f.Latency());
}

// ---- against ffmpeg -------------------------------------------------------------------------

static std::string tmp_dir;

static bool HaveFfmpeg() { return system("ffmpeg -version > /dev/null 2>&1") == 0; }

static void WriteWav(const std::string& path, const std::vector<float>& l, const std::vector<float>& r)
{
    FILE* f = fopen(path.c_str(), "wb");
    const uint32_t frames = static_cast<uint32_t>(l.size()), bytes = frames * 8;
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f);
    u32(36 + bytes);
    fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(3); // float
    u16(2);
    u32(48000);
    u32(48000 * 8);
    u16(8);
    u16(32);
    fwrite("data", 1, 4, f);
    u32(bytes);
    for (size_t i = 0; i < l.size(); i++)
    {
        fwrite(&l[i], 4, 1, f);
        fwrite(&r[i], 4, 1, f);
    }
    fclose(f);
}

static std::string Run(const std::string& cmd)
{
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p)
        return out;
    char buf[512];
    while (fgets(buf, sizeof(buf), p))
        out += buf;
    pclose(p);
    return out;
}

/** The values of key, one per line "key=value", in ffmpeg's ametadata file: their mean from
 *  the first'th on, or the last */
static double MetaValue(const std::string& file, const char* key, bool last, size_t first = 0)
{
    FILE* f = fopen(file.c_str(), "r");
    if (!f)
        return NAN;
    char line[256];
    double sum = 0., v = NAN;
    size_t n = 0, i = 0;
    const size_t klen = strlen(key);
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, key, klen) == 0 && line[klen] == '=')
        {
            v = atof(line + klen + 1);
            if (i++ >= first)
            {
                sum += v;
                n++;
            }
        }
    fclose(f);
    return last ? v : (n ? sum / n : NAN);
}

/** ffmpeg's per band: acrossover's three LR4 bands each through filter, its ametadata of key
 *  into tmp_dir/NAME-b.txt */
static bool PerBand(const std::string& wav, const char* filter, const char* key, const char* name)
{
    std::string graph = "[0:a]acrossover=split=300 3000:order=4th[b0][b1][b2]";
    std::string maps;
    for (int b = 0; b < 3; b++)
    {
        char part[512];
        snprintf(part, sizeof(part), ";[b%d]%s,ametadata=mode=print:key=%s:file=%s/%s-%d.txt[o%d]", b, filter,
                 key, tmp_dir.c_str(), name, b, b);
        graph += part;
        maps += " -map '[o" + std::to_string(b) + "]' -f null -";
    }
    return system(("ffmpeg -v error -y -i " + wav + " -filter_complex '" + graph + "'" + maps).c_str()) == 0;
}

static void TestMetersFfmpeg()
{
    if (!HaveFfmpeg())
    {
        printf("SKIP: local only (ffmpeg): the meters against ffmpeg\n");
        return;
    }
    char dir[] = "/tmp/finalizer.XXXXXX";
    if (!mkdtemp(dir))
    {
        Check(false, "ffmpeg: a scratch folder");
        return;
    }
    tmp_dir = dir;

    // correlation: four signals, each band against aphasemeter's mean phase. Ours weighs the
    // samples by their energy (sum(M^2 - S^2) / sum(M^2 + S^2), Pearson's for channels about
    // as loud), aphasemeter averages each sample's 2LR / (L^2 + R^2): the same for mono,
    // unrelated and out of phase, within 0.1 and the noise of 2s; for a normal stereo signal
    // (Pearson's .6) aphasemeter reads lower, so there the LED's colour has to agree
    const char* names[] = {"mono", "wide", "anti", "normal"};
    for (int sig = 0; sig < 4; sig++)
    {
        const size_t n = 96000;
        std::vector<float> l(n), r(n);
        for (size_t i = 0; i < n; i++)
        {
            const float a = .3f * Noise(), b = .3f * Noise();
            l[i] = a;
            r[i] = sig == 0 ? a : sig == 1 ? b : sig == 2 ? -a : .6f * a + .8f * b;
        }
        Setup s;
        s.f.SetMeter(Finalizer::WIDTH);
        std::vector<float> ol = l, orr = r;
        s.Run(ol, orr);
        const std::string wav = tmp_dir + "/corr.wav";
        WriteWav(wav, l, r);
        if (!PerBand(wav, "aphasemeter=video=0", "lavfi.aphasemeter.phase", names[sig]))
        {
            Check(false, "ffmpeg: aphasemeter ran");
            continue;
        }
        for (int b = 0; b < 3; b++)
        {
            const double ref = MetaValue(tmp_dir + "/" + names[sig] + "-" + std::to_string(b) + ".txt",
                                         "lavfi.aphasemeter.phase", false, 10);
            const float ours = s.f.BandCorrelation(b);
            const int ours_led = Finalizer::CorrelationLed(ours);
            const int ref_led = Finalizer::CorrelationLed(static_cast<float>(ref));
            char what[160];
            snprintf(what, sizeof(what), "meters: correlation, %s, band %d: %.3f, aphasemeter %.3f, LED %d and %d",
                     names[sig], b, ours, ref, ours_led, ref_led);
            Check((sig == 3 || fabs(ours - ref) < .1) && ours_led == ref_led && ours_led == (sig == 0 ? 0 : sig == 1 ? 2 : sig == 2 ? 3 : 1), what);
        }
    }

    // each band's peak (after the EQ, here 0dB) and the input's after a trim of +6dB, against
    // astats: within 0.5dB, as ffmpeg's LR4 is a biquad cascade, ours state-variable filters,
    // and the bands' phases differ (ours has the low band through the high split's allpass)
    {
        const size_t n = 96000;
        std::vector<float> l(n), r(n);
        for (size_t i = 0; i < n; i++)
        {
            const float t = static_cast<float>(i) / kSr;
            const float x = .4f * sinf(2.f * static_cast<float>(M_PI) * 80.f * t)
                            + .2f * sinf(2.f * static_cast<float>(M_PI) * 900.f * t)
                            + .1f * sinf(2.f * static_cast<float>(M_PI) * 7000.f * t);
            l[i] = x;
            r[i] = .8f * x;
        }
        Setup s;
        s.f.SetMeter(Finalizer::EQ);
        std::vector<float> ol = l, orr = r;
        s.Run(ol, orr);
        float ours[3];
        for (int b = 0; b < 3; b++)
            ours[b] = s.f.TakeBandPeak(b);
        const std::string wav = tmp_dir + "/bands.wav";
        WriteWav(wav, l, r);
        if (PerBand(wav, "astats=metadata=1:reset=0", "lavfi.astats.Overall.Peak_level", "peak"))
            for (int b = 0; b < 3; b++)
            {
                const double ref = MetaValue(tmp_dir + "/peak-" + std::to_string(b) + ".txt",
                                             "lavfi.astats.Overall.Peak_level", true);
                char what[160];
                snprintf(what, sizeof(what), "meters: band %d's peak %.2f dB, astats %.2f dB", b, Db(ours[b]), ref);
                Check(fabs(Db(ours[b]) - ref) < .5, what);
            }
        else
            Check(false, "ffmpeg: astats ran");

        Setup t({{Finalizer::INPUT, .75f}});
        t.f.SetMeter(Finalizer::LEVEL);
        ol = l;
        orr = r;
        t.Run(ol, orr);
        const float in = t.f.TakeInputPeak();
        const std::string out = tmp_dir + "/input.wav";
        WriteWav(out, ol, orr);
        system(("ffmpeg -v error -y -i " + out + " -af astats=metadata=1:reset=0,ametadata=mode=print:key=lavfi.astats.Overall.Peak_level:file="
                + tmp_dir + "/in.txt -f null -").c_str());
        const double ref = MetaValue(tmp_dir + "/in.txt", "lavfi.astats.Overall.Peak_level", true);
        char what[160];
        snprintf(what, sizeof(what), "meters: the input's peak after +6dB of trim %.2f dB, astats %.2f dB", Db(in), ref);
        Check(fabs(Db(in) - ref) < .05, what);
    }

    // the limiter's output: ebur128's true peak (4x) at most -1dBTP, within its 0.1dB
    {
        const std::string wav = tmp_dir + "/lim.wav";
        std::vector<float> l(lim_out_l.begin() + 9600, lim_out_l.end());
        std::vector<float> r(lim_out_r.begin() + 9600, lim_out_r.end());
        WriteWav(wav, l, r);
        const std::string out = Run("ffmpeg -nostats -hide_banner -i " + wav + " -af ebur128=peak=true -f null - 2>&1");
        const size_t at = out.rfind("Peak:");
        const double tp = at == std::string::npos ? NAN : atof(out.c_str() + at + 5);
        char what[160];
        snprintf(what, sizeof(what), "meters: the limiter's true peak (ebur128, 4x) %.2f dBTP (<= -1)", tp);
        Check(tp <= -1., what);
    }

    system(("rm -rf '" + tmp_dir + "'").c_str());
}

int main()
{
    TestNeutral();
    TestFlat();
    TestBandComp();
    TestWidth();
    TestSpace();
    TestMute();
    TestDrive();
    TestLimiter();
    TestMetersFfmpeg();
    return Finish();
}
