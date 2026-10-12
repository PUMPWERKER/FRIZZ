// Shifter splice check: the splice search (Shifter::SpliceSearch, FxShifter.h) against the
// search as it was before #137 made it cheaper, kept here as it was: on noise, tones, their
// mixes and near-silence, at every position in the ring (so with and without the reads
// wrapping it), shifting up and down as far as the swoop and the stereo offset take it, both
// must pick the same lag for every one of kTrials searches; and so must the search spread
// over two blocks as the shifter runs it, with a block written to the ring in between.
// Then the shifter itself, at the ends of its shift, grain, swoop and stereo: while a tap
// waits for its splice, the other carries the sound alone, and mustn't run into the end of
// its delay (#137). Built and run by unit.sh splice.
#include <cmath>
#include <cstdio>
#include <random>
// the shifter's taps and windows, to watch them
#define private public
#include "FxShifter.h"
#undef private
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
    int searched = 0, differ = 0, split_differ = 0, split_searched = 0, wrapped = 0;
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

        // as the shifter runs it: half its groups, a block written after last, the rest;
        // where every read is behind the write head (going up at the swoop's top with the
        // shortest grain the nearest candidates read ahead of it, stale samples a ring ago,
        // before #137 too, and the block written between is among them)
        if (nominal - 120.f * ratio + chompi::Shifter::SpliceSearch::kMinLag < 0.f)
            continue;
        split_searched++;
        chompi::Shifter::SpliceSearch split;
        split.Begin(buf, last, nominal, other, ratio);
        split.Run(chompi::Shifter::kFirstGroups);
        for (size_t i = 1; i <= 24; i++)
            buf[(last + i) & kBufMask] = 2.f * uni(rng) - 1.f;
        split.Run(chompi::Shifter::SpliceSearch::kGroups);
        if (split.best.lag != old_lag && split_differ++ < 10)
            printf("split differs: kind %d last %zu ratio %g: %d, was %d\n", kind, last, ratio,
                   split.best.lag, old_lag);
        // whether a candidate's reads wrap the ring (the search's masked path): its lags from
        // where the 16th point reads at lag 0, and the 1st
        const size_t near = last - static_cast<size_t>(nominal - 120.f * ratio);
        const size_t far = last - static_cast<size_t>(nominal);
        using S = chompi::Shifter::SpliceSearch;
        wrapped += ((far - S::kMaxLag) & kBufMask) > ((near - S::kMinLag) & kBufMask);
    }
    printf("%d searches, %d with reads that wrap the ring\n", searched, wrapped);
    Check(searched > kTrials * 9 / 10, "most of the configurations are searched");
    Check(wrapped > searched / 10 && wrapped < searched * 9 / 10,
          "both with and without reads that wrap the ring");
    char what[96];
    snprintf(what, sizeof(what), "the same lag as before #137 in every search (%d differ)",
             differ);
    Check(differ == 0, what);
    snprintf(what, sizeof(what),
             "the same over two blocks, a block written between (%d of %d differ)",
             split_differ, split_searched);
    Check(split_differ == 0, what);

    // the shifter at its ends: shift, grain, swoop (fired by the key) and stereo; a 220Hz saw
    // for 4s. A tap that sounds (weight over 1%) and sits at the end of its delay (1 or the
    // longest) has run into it. Counted from the end of the taps' first lives, which start
    // from Init's delays, not from a splice (going up, the first one can run into delay 1,
    // before #137 too)
    struct Setting
    {
        const char* name;
        float shift, grain, swoop, stereo;
        int from;
    } settings[] = {
        {"+12, grain 10ms", 1.f, 0.f, 0.f, 0.f, 4800},
        {"-12, grain 10ms", 0.f, 0.f, 0.f, 0.f, 4800},
        {"+12, grain 50ms", 1.f, 1.f, 0.f, 0.f, 48000},
        {"-12, grain 50ms", 0.f, 1.f, 0.f, 0.f, 48000},
        {"+7, grain 30ms", .5f + 7.f / 24, .5f, 0.f, 0.f, 48000},
        {"+12, 10ms, swoop, stereo", 1.f, 0.f, 1.f, 1.f, 4800},
        {"+12, 10ms, swoop", 1.f, 0.f, 1.f, 0.f, 4800},
        {"-12, 10ms, swoop, stereo", 0.f, 0.f, 1.f, 1.f, 4800},
        {"+12, 50ms, swoop, stereo", 1.f, 1.f, 1.f, 1.f, 48000},
    };
    static chompi::Shifter shifter;
    for (const Setting& set : settings)
    {
        shifter.Init(48000.f);
        shifter.SetParam(chompi::Shifter::SHIFT, set.shift);
        shifter.SetParam(chompi::Shifter::GRAIN, set.grain);
        shifter.SetParam(chompi::Shifter::SWOOP, set.swoop);
        shifter.SetParam(chompi::Shifter::STEREO, set.stereo);
        shifter.SetOn(true);
        int waits = 0, waiting_stuck = 0, stuck = 0;
        double ph = 0;
        for (int i = 0; i < 48000 * 4; i++)
        {
            ph += 220.0 / 48000.0;
            float v = 0;
            for (int h = 1; h <= 8; h++)
                v += sinf(2 * M_PI * ph * h) / h;
            float l = v * .3f, r = l;
            shifter.Process(&l, &r);
            for (size_t c = 0; c < 2; c++)
            {
                const float max_delay = shifter.window_frames_ + chompi::Shifter::kDelayPast;
                const float w = shifter.window_[c];
                const float weight[2] = {1.f - fabsf(2.f * w - 1.f), fabsf(2.f * w - 1.f)};
                waits += shifter.held_[c] == 1;
                for (size_t t = 0; t < 2; t++)
                {
                    const float d = shifter.delay_[c][t];
                    const bool at_end =
                        i >= set.from && weight[t] > .01f && (d <= 1.f || d >= max_delay);
                    stuck += at_end;
                    waiting_stuck += at_end && shifter.waiting_[c];
                }
            }
        }
        char line[160];
        snprintf(line, sizeof(line),
                 "%s: %d waits, a sounding tap at the end of its delay %d samples, %d of them "
                 "while the other waits",
                 set.name, waits, stuck, waiting_stuck);
        Check(waits > 0 && stuck == 0, line);
    }
    return Finish();
}
