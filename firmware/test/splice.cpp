// Shifter splice check: the splice search (Shifter::SpliceLag, FxShifter.h) against the search
// as it was before #137 made it cheaper, kept here as it was: on noise, tones, their mixes and
// near-silence, at every position in the ring (so with and without the reads wrapping it),
// shifting up and down as far as the swoop and the stereo offset take it, both must pick the
// same lag for every one of kTrials searches. Built and run by unit.sh splice.
#include <cmath>
#include <cstdio>
#include <random>
#include "FxShifter.h"
#include "check.h"

static const size_t kBufSize = 4096;
static const size_t kBufMask = kBufSize - 1;
static const int kTrials = 60000;

// the search before #137, Shifter::Splice's body returning the lag
static int OldLag(const float* b, size_t last, float nominal, float other, float ratio)
{
    const int kSearch = 360, kCoarseStep = 6, kFineRange = 5;
    const size_t kCorrPoints = 16, kCorrStep = 8;
    float ref[kCorrPoints];
    size_t start[kCorrPoints];
    for (size_t k = 0; k < kCorrPoints; k++)
    {
        const float ahead = static_cast<float>(k * kCorrStep) * ratio;
        ref[k] = b[(last - static_cast<size_t>(fmaxf(other - ahead, 1.f))) & kBufMask];
        start[k] = last - static_cast<size_t>(nominal - ahead);
    }
    auto score = [&](int lag) {
        float dot = 0.f, energy = 0.f;
        for (size_t k = 0; k < kCorrPoints; k++)
        {
            const float x = b[(start[k] - static_cast<size_t>(lag)) & kBufMask];
            dot += x * ref[k];
            energy += x * x;
        }
        return dot / sqrtf(energy + 1e-9f);
    };
    int best = 0;
    float best_score = -1e30f;
    for (int lag = -kSearch; lag <= kSearch; lag += kCoarseStep)
    {
        const float v = score(lag);
        if (v > best_score)
        {
            best_score = v;
            best = lag;
        }
    }
    const int coarse = best;
    for (int lag = coarse - kFineRange; lag <= coarse + kFineRange; lag++)
    {
        const float v = score(lag);
        if (v > best_score)
        {
            best_score = v;
            best = lag;
        }
    }
    return best;
}

static float buf[kBufSize];

int main()
{
    std::mt19937 rng(137);
    std::uniform_real_distribution<float> uni(0.f, 1.f);
    int searched = 0, differ = 0, wrapped = 0;
    for (int t = 0; t < kTrials; t++)
    {
        // the ring's contents: 0 noise, 1 a tone, 2 two tones and noise, 3 near-silence,
        // 4 silence with a click
        const int kind = t % 5;
        const float f1 = 40.f + 2000.f * uni(rng) * uni(rng), f2 = 60.f + 4000.f * uni(rng);
        const float level = kind == 3 ? 1e-6f * uni(rng) : uni(rng);
        for (size_t i = 0; i < kBufSize; i++)
        {
            const float n = 2.f * uni(rng) - 1.f;
            float x = 0.f;
            if (kind == 0)
                x = n;
            else if (kind == 1)
                x = sinf(6.2831853f * f1 * i / 48000.f);
            else if (kind == 2 || kind == 3)
                x = .5f * sinf(6.2831853f * f1 * i / 48000.f)
                    + .3f * sinf(6.2831853f * f2 * i / 48000.f) + .2f * n;
            else
                x = (i % 977 == 0) ? n : 0.f;
            buf[i] = level * x;
        }

        // the speed: up to 12 semitones, the swoop's 24 and the stereo offset's 1 on top, up
        // or down; the window 10ms to 50ms of delay
        const float semis = (2.f * uni(rng) - 1.f) * 37.f;
        const float ratio = powf(2.f, semis / 12.f);
        const bool up = ratio > 1.f;
        const float window = 480.f + 1920.f * uni(rng);
        const float nominal = (up ? window : 0.f) + 130.f + 360.f;
        // the far reads stay ahead of the write head (on the CHOMPI a negative one would
        // saturate, on the host it wraps)
        if (nominal - 120.f * ratio < 1.f)
            continue;
        const float other = 1.f + (window + 130.f + 725.f) * uni(rng);
        const size_t last = rng() & kBufMask;

        searched++;
        const int old_lag = OldLag(buf, last, nominal, other, ratio);
        const int new_lag = chompi::Shifter::SpliceLag(buf, last, nominal, other, ratio);
        if (old_lag != new_lag && differ++ < 10)
            printf("differs: kind %d last %zu ratio %g nominal %g other %g: %d, was %d\n", kind,
                   last, ratio, nominal, other, new_lag, old_lag);
        // whether a candidate's reads wrap the ring (SpliceLag's masked path): lags -365 to
        // +378 from where the 16th point reads at lag 0, and the 1st
        const size_t near = last - static_cast<size_t>(nominal - 120.f * ratio);
        const size_t far = last - static_cast<size_t>(nominal);
        wrapped += ((far - 378) & kBufMask) > ((near + 365) & kBufMask);
    }
    printf("%d searches, %d with reads that wrap the ring\n", searched, wrapped);
    Check(searched > kTrials * 9 / 10, "most of the configurations are searched");
    Check(wrapped > searched / 10 && wrapped < searched * 9 / 10,
          "both with and without reads that wrap the ring");
    char what[96];
    snprintf(what, sizeof(what), "the same lag as before #137 in every search (%d differ)",
             differ);
    Check(differ == 0, what);
    return Finish();
}
