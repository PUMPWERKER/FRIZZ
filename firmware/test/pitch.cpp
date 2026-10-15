// Shifter pitch check: runs a 220Hz tone through FxShifter.h at every interval from -12 to
// +12 and measures the output's pitch (the strongest frequency over 1s, by a Goertzel scan to
// 0.05Hz) and how much its level wobbles (1000-sample windows). Built and run by
// unit.sh pitch; fails if any interval is off by more than kMaxCents or wobbles by more than
// kMaxWobbleDb, on a sine (the hardest case for a delay-line shifter) or a harmonic tone;
// and that a 10kHz sine a fifth up keeps its level within kMaxHighLossDb (the taps' Hermite
// reads, #305).
#include <cmath>
#include <cstdio>
#include <vector>
#include "FxShifter.h"
#include "check.h"

static const double kMaxCents = 5.0;
static const double kMaxWobbleDb = 2.0;
static const double kMaxHighLossDb = .6;

static chompi::Shifter shifter;

// power at f over 1s from at, by Goertzel
static double Power(const std::vector<float>& x, size_t at, double f)
{
    const double coeff = 2 * cos(2 * M_PI * f / 48000.0);
    double s1 = 0, s2 = 0;
    for (int i = 0; i < 48000; i++)
    {
        const double s0 = x[at + i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

// the strongest frequency between 50Hz and 1kHz: 1Hz steps, then 0.05Hz steps around the peak
static double Pitch(const std::vector<float>& x, size_t at)
{
    double best = 0, best_f = 0;
    for (double f = 50; f < 1000; f += 1)
    {
        const double p = Power(x, at, f);
        if (p > best)
        {
            best = p;
            best_f = f;
        }
    }
    const double coarse = best_f;
    for (double f = coarse - 1; f <= coarse + 1; f += .05)
    {
        const double p = Power(x, at, f);
        if (p > best)
        {
            best = p;
            best_f = f;
        }
    }
    return best_f;
}

int main()
{
    for (int harmonics : {1, 8})
    {
        printf("220Hz %s:\n", harmonics == 1 ? "sine" : "saw, 8 harmonics");
        for (int st = -12; st <= 12; st++)
        {
            shifter.Init(48000.f);
            shifter.SetParam(chompi::Shifter::SHIFT, (st + 12) / 24.f);
            shifter.SetOn(true);

            std::vector<float> out;
            double ph = 0;
            for (int i = 0; i < 48000 * 3; i++)
            {
                ph += 220.0 / 48000.0;
                float v = 0;
                for (int h = 1; h <= harmonics; h++)
                    v += sinf(2 * M_PI * ph * h) / h;
                float l = v * .3f, r = l;
                shifter.Process(&l, &r);
                out.push_back(l);
            }

            // skip the first second: the fade-in and the first splices
            const double want = 220 * pow(2, st / 12.0);
            const double cents = 1200 * log2(Pitch(out, 48000) / want);
            double lo = 1e9, hi = 0;
            for (size_t w = 48000; w + 1000 < out.size(); w += 1000)
            {
                double s = 0;
                for (int i = 0; i < 1000; i++)
                    s += out[w + i] * out[w + i];
                lo = fmin(lo, s);
                hi = fmax(hi, s);
            }
            const double wobble = 10 * log10(hi / lo);
            char what[64];
            snprintf(what, sizeof(what), "%+3d st: %+5.1f cents, wobble %3.1f dB", st, cents, wobble);
            Check(fabs(cents) <= kMaxCents && wobble <= kMaxWobbleDb, what);
        }
    }

    // the highs (#305): the taps read between samples, Hermite, which keeps a 10kHz sine a
    // fifth up within kMaxHighLossDb of its level, where linear reads lost 1.3dB
    shifter.Init(48000.f);
    shifter.SetParam(chompi::Shifter::SHIFT, (7 + 12) / 24.f);
    shifter.SetOn(true);
    double in = 0, out = 0;
    for (int i = 0; i < 48000 * 3; i++)
    {
        float l = .1f * sinf(2 * M_PI * fmod(10000.0 / 48000.0 * i, 1.0)), r = l;
        const float x = l;
        shifter.Process(&l, &r);
        if (i >= 96000)
            in += x * x, out += l * l;
    }
    const double loss = 10 * log10(out / in);
    char what[64];
    snprintf(what, sizeof(what), "10kHz a fifth up: %+.2f dB", loss);
    Check(loss >= -kMaxHighLossDb, what);
    return Finish();
}
