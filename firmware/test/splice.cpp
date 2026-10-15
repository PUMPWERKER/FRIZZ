// Shifter splice check: the splice search (Shifter::SpliceSearch, FxShifter.h) against the
// search as it was before #137 made it cheaper, kept here as it was: on noise, tones, their
// mixes and near-silence, at every position in the ring (so with and without the reads
// wrapping it), shifting up and down as far as the swoop and the stereo offset take it, both
// must pick the same lag for every one of kTrials searches; and so must the search spread
// over two blocks as the shifter runs it, with a block written to the ring in between.
// Then the shifter itself, at the ends of its shift, grain, swoop and stereo, from power-on,
// after the shift or the grain was turned while it was off, turned from the centre while it
// plays, and the grain turned at once while it plays: no sounding tap may run into the end of its delay
// (#137, #140, #167). And back at stereo 0 while it plays (#226): the right channel relocks
// onto the left's taps without a jump, within one grain (30 ms at the default, #249).
// Built and run by unit.sh splice.
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

// a channel's window: one for both channels before #167
static float Window(float w, size_t) { return w; }
static float Window(const float (&w)[2], size_t c) { return w[c]; }

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
    // for 4s from power-on. A tap that sounds (weight over 1%) and sits at the end of its
    // delay (1 or the longest) has run into it: never, not even in the taps' first lives,
    // which start from Init's delays (going up, the first one ran into delay 1 before #140)
    struct Setting
    {
        const char* name;
        float shift, grain, swoop, stereo;
    } settings[] = {
        {"+12, grain 10ms", 1.f, 0.f, 0.f, 0.f},
        {"-12, grain 10ms", 0.f, 0.f, 0.f, 0.f},
        {"+12, grain 50ms", 1.f, 1.f, 0.f, 0.f},
        {"-12, grain 50ms", 0.f, 1.f, 0.f, 0.f},
        {"+7, grain 30ms", .5f + 7.f / 24, .5f, 0.f, 0.f},
        {"+12, 10ms, swoop, stereo", 1.f, 0.f, 1.f, 1.f},
        {"+12, 10ms, swoop", 1.f, 0.f, 1.f, 0.f},
        {"-12, 10ms, swoop, stereo", 0.f, 0.f, 1.f, 1.f},
        {"+12, 50ms, swoop, stereo", 1.f, 1.f, 1.f, 1.f},
    };
    static chompi::Shifter shifter;
    double ph = 0;
    int waits = 0, waiting_stuck = 0, stuck = 0;
    auto play = [&](int samples) {
        for (int i = 0; i < samples; i++)
        {
            ph += 220.0 / 48000.0;
            float v = 0;
            for (int h = 1; h <= 8; h++)
                v += sinf(2 * M_PI * ph * h) / h;
            float l = v * .3f, r = l;
            shifter.Process(&l, &r);
            for (size_t c = 0; c < 2; c++)
            {
                const float max_delay =
                    Window(shifter.window_frames_, c) + chompi::Shifter::kDelayPast;
                const float w = shifter.window_[c];
                const float weight[2] = {1.f - fabsf(2.f * w - 1.f), fabsf(2.f * w - 1.f)};
                waits += shifter.held_[c] == 1;
                for (size_t t = 0; t < 2; t++)
                {
                    const float d = shifter.delay_[c][t];
                    const bool at_end = !shifter.still_[c] && weight[t] > .01f
                                        && (d <= 1.f || d >= max_delay);
                    stuck += at_end;
                    waiting_stuck += at_end && shifter.waiting_[c];
                }
            }
        }
    };
    for (const Setting& set : settings)
    {
        shifter.Init(48000.f);
        shifter.SetParam(chompi::Shifter::SHIFT, set.shift);
        shifter.SetParam(chompi::Shifter::GRAIN, set.grain);
        shifter.SetParam(chompi::Shifter::SWOOP, set.swoop);
        shifter.SetParam(chompi::Shifter::STEREO, set.stereo);
        shifter.SetOn(true);
        waits = waiting_stuck = stuck = 0;
        play(48000 * 4);
        char line[160];
        snprintf(line, sizeof(line),
                 "%s: %d waits, a sounding tap at the end of its delay %d samples, %d of them "
                 "while the other waits",
                 set.name, waits, stuck, waiting_stuck);
        Check(waits > 0 && stuck == 0, line);
    }

    // punched in, out, the shift turned while it's off, and in again: where the taps were left
    // for the other direction, they start afresh on waking; otherwise they go on where they
    // were (#140)
    struct Turn
    {
        const char* name;
        float from, to;
        bool afresh;
    } turns[] = {
        {"-12, off, +12", 0.f, 1.f, true},
        {"+12, off, -12", 1.f, 0.f, true},
        {"+12, off, +7", 1.f, .5f + 7.f / 24, false},
        {"-12, off, -12", 0.f, 0.f, false},
    };
    // and the grain turned while it's off (#167): a long grain's taps, a short one on waking
    struct GrainTurn
    {
        const char* name;
        float shift, from, to;
    } grain_turns[] = {
        {"+12 grain 50ms, off, 10ms", 1.f, 1.f, 0.f},
        {"-12 grain 50ms, off, 10ms", 0.f, 1.f, 0.f},
        {"+12 grain 10ms, off, 50ms", 1.f, 0.f, 1.f},
        {"-12 grain 10ms, off, 50ms", 0.f, 0.f, 1.f},
    };
    for (const GrainTurn& g : grain_turns)
        for (int at : {0, 211, 777})
        {
            shifter.Init(48000.f);
            shifter.SetParam(chompi::Shifter::SHIFT, g.shift);
            shifter.SetParam(chompi::Shifter::GRAIN, g.from);
            shifter.SetOn(true);
            play(48000 + at);
            shifter.SetOn(false);
            play(24000);
            shifter.SetParam(chompi::Shifter::GRAIN, g.to);
            play(100);
            shifter.SetOn(true);
            waits = waiting_stuck = stuck = 0;
            play(48000);
            if (at != 0 && stuck == 0)
                continue;
            char line[160];
            snprintf(line, sizeof(line), "%s, %d samples later: a sounding tap at the end of its "
                     "delay %d samples", g.name, at, stuck);
            Check(stuck == 0, line);
        }
    for (const Turn& turn : turns)
    {
        shifter.Init(48000.f);
        shifter.SetParam(chompi::Shifter::SHIFT, turn.from);
        shifter.SetOn(true);
        play(48000);
        shifter.SetOn(false);
        play(24000);
        const bool asleep = shifter.still_[0] && shifter.still_[1];
        float before[2][2], window[2];
        for (size_t c = 0; c < 2; c++)
        {
            window[c] = shifter.window_[c];
            for (size_t t = 0; t < 2; t++)
                before[c][t] = shifter.delay_[c][t];
        }
        shifter.SetParam(chompi::Shifter::SHIFT, turn.to);
        shifter.SetOn(true);
        waits = waiting_stuck = stuck = 0;
        play(1);
        // going on: each delay moved by 1 - ratio, the window by a sample's worth
        bool went_on = true;
        for (size_t c = 0; c < 2; c++)
        {
            went_on &= shifter.window_[c] > window[c];
            for (size_t t = 0; t < 2; t++)
                went_on &= fabsf(shifter.delay_[c][t]
                                 - (before[c][t] + 1.f - shifter.ratio_[c])) < 1e-3f;
        }
        play(48000 * 2);
        char line[160];
        snprintf(line, sizeof(line),
                 "%s: the taps %s, a sounding tap at the end of its delay %d samples",
                 turn.name, went_on ? "go on" : "start afresh", stuck);
        Check(asleep && went_on == !turn.afresh && stuck == 0, line);
    }
    // punched in at the centre (its own speed: the taps stand still), then turned while it
    // plays: up and down, as a player turns knob 1 (#140)
    for (float to : {1.f, 0.f, .5f + 7.f / 24})
    {
        shifter.Init(48000.f);
        shifter.SetParam(chompi::Shifter::SHIFT, .5f);
        shifter.SetOn(true);
        play(24000);
        shifter.SetParam(chompi::Shifter::SHIFT, to);
        waits = waiting_stuck = stuck = 0;
        play(48000 * 2);
        char line[160];
        snprintf(line, sizeof(line),
                 "at the centre, then %+d while it plays: a sounding tap at the end of its delay "
                 "%d samples",
                 static_cast<int>(lroundf(to * 24.f)) - 12, stuck);
        Check(waits > 0 && stuck == 0, line);
    }
    // the grain turned at once while it plays (a scene recall, MIDI, a fast hand): the taps
    // were placed for the window they had, so the new one comes in at the splices as far as
    // they fit it. Before #167 it came in at once: going up into a longer window, a tap ran
    // into delay 1 (+12 with 10 to 50ms: ~1000 samples), going down into a shorter one, into
    // the end of it, and played unshifted for up to ~20ms, a click
    struct Grain
    {
        const char* name;
        float shift, from, to, swoop;
    } grains[] = {
        {"+12, grain 10 to 50ms", 1.f, 0.f, 1.f, 0.f},
        {"-12, grain 50 to 10ms", 0.f, 1.f, 0.f, 0.f},
        {"+12, grain 50 to 10ms", 1.f, 1.f, 0.f, 0.f},
        {"-12, grain 10 to 50ms", 0.f, 0.f, 1.f, 0.f},
        {"+3, grain 10 to 50ms", .5f + 3.f / 24, 0.f, 1.f, 0.f},
        {"-3, grain 50 to 10ms", .5f - 3.f / 24, 1.f, 0.f, 0.f},
        {"+12, swoop, grain 10 to 50ms", 1.f, 0.f, 1.f, 1.f},
        {"-12, swoop, grain 50 to 10ms", 0.f, 1.f, 0.f, 1.f},
    };
    for (const Grain& g : grains)
        for (int at : {0, 97, 333, 1001})
        {
            shifter.Init(48000.f);
            shifter.SetParam(chompi::Shifter::SHIFT, g.shift);
            shifter.SetParam(chompi::Shifter::GRAIN, g.from);
            shifter.SetParam(chompi::Shifter::SWOOP, g.swoop);
            shifter.SetOn(true);
            play(24000 + at);
            // back and forth a few times, then to the end
            waits = waiting_stuck = stuck = 0;
            for (int i = 0; i < 3; i++)
            {
                shifter.SetParam(chompi::Shifter::GRAIN, g.to);
                play(1500 + at);
                shifter.SetParam(chompi::Shifter::GRAIN, g.from);
                play(700);
            }
            shifter.SetParam(chompi::Shifter::GRAIN, g.to);
            play(48000);
            const float want = g.to < .5f ? 480.f : 2400.f;
            const bool there = Window(shifter.window_frames_, 0) == want
                               && Window(shifter.window_frames_, 1) == want;
            if (at != 0 && stuck == 0 && there)
                continue; // one line per setting, unless one fails
            char line[160];
            snprintf(line, sizeof(line),
                     "%s, %d samples later: a sounding tap at the end of its delay %d samples, "
                     "%s the grain a second on",
                     g.name, at, stuck, there ? "at" : "NOT at");
            Check(stuck == 0 && there, line);
        }

    // back at stereo 0 while it plays (#226): a stereo offset set the channels' taps apart, and
    // the right channel fades over a life from its own taps to the left's, then takes them. A
    // 311Hz sine, the same into both, feedback 0. The right channel's steps sample to sample
    // through the fade stay within the left's (a jump would stand out: moving the right's taps
    // to the left's at once jumps by |L - R|, up to what the fade's first tenth shows, where
    // the right still plays its own), both channels splice through
    // it, and once it's over L and R are the same to the bit. At every grain, up and down,
    // with the swoop rising through it, and turned apart again halfway: back to its own taps
    // without a jump, and on to the left's once at 0 again
    struct Relock
    {
        const char* name;
        float shift, grain, swoop;
        bool reverse;
    } relocks[] = {
        {"+7, grain 30ms", .5f + 7.f / 24, .5f, 0.f, false},
        {"-12, grain 10ms", 0.f, 0.f, 0.f, false},
        {"+12, grain 50ms", 1.f, 1.f, 0.f, false},
        {"-5, grain 50ms", .5f - 5.f / 24, 1.f, 0.f, false},
        {"+7, the swoop rising", .5f + 7.f / 24, .5f, 1.f, false},
        {"+7, apart again halfway", .5f + 7.f / 24, .5f, 0.f, true},
    };
    for (const Relock& rl : relocks)
    {
        double sph = 0;
        float l = 0.f, r = 0.f, prev_l = 0.f, prev_r = 0.f;
        int splices[2] = {0, 0};
        auto step = [&] {
            sph += 311.0 / 48000.0;
            prev_l = l;
            prev_r = r;
            l = r = .3f * static_cast<float>(sin(2 * M_PI * sph));
            shifter.Process(&l, &r);
            for (size_t c = 0; c < 2; c++)
                splices[c] += shifter.held_[c] == 1;
        };
        shifter.Init(48000.f);
        shifter.SetParam(chompi::Shifter::SHIFT, rl.shift);
        shifter.SetParam(chompi::Shifter::GRAIN, rl.grain);
        shifter.SetParam(chompi::Shifter::SWOOP, rl.swoop);
        shifter.SetParam(chompi::Shifter::STEREO, .25f);
        shifter.SetOn(true);
        for (int i = 0; i < (rl.swoop > 0.f ? 4800 : 48000); i++)
            step();
        const bool apart = shifter.window_[0] != shifter.window_[1]
                           || shifter.delay_[0][0] != shifter.delay_[1][0];
        shifter.SetParam(chompi::Shifter::STEREO, 0.f);
        float jump = 0.f, step_l = 0.f, step_r = 0.f;
        int fade = 0, back = 0;
        splices[0] = splices[1] = 0;
        bool reversed = !rl.reverse;
        while ((shifter.fade_ > 0.f || shifter.apart_) && fade < 48000)
        {
            if (!reversed && shifter.fade_ >= .5f)
            {
                // apart again: back to its own taps, then at 0 once it's there
                shifter.SetParam(chompi::Shifter::STEREO, .25f);
                while (fade < 48000 && (step(), shifter.fade_ > 0.f))
                {
                    step_l = fmaxf(step_l, fabsf(l - prev_l));
                    step_r = fmaxf(step_r, fabsf(r - prev_r));
                    fade++;
                    back++;
                }
                for (int i = 0; i < 4800; i++)
                    step();
                shifter.SetParam(chompi::Shifter::STEREO, 0.f);
                reversed = true;
            }
            step();
            step_l = fmaxf(step_l, fabsf(l - prev_l));
            step_r = fmaxf(step_r, fabsf(r - prev_r));
            if (!back && shifter.fade_ < .1f)
                jump = fmaxf(jump, fabsf(l - r));
            fade++;
        }
        bool same = !shifter.apart_ && shifter.fade_ == 0.f;
        for (int i = 0; i < 48000; i++)
        {
            step();
            same &= l == r;
        }
        // the fade is one grain long (#249's "within about 30 ms" at the default grain), two
        // when turned apart again halfway
        const int grain = static_cast<int>(48.f * (10.f + 40.f * rl.grain) + .5f);
        const bool in_time = fade <= (rl.reverse ? 2 : 1) * grain;
        char line[240];
        snprintf(line, sizeof(line),
                 "%s: back at stereo 0, the right channel on the left's taps after %d samples (%.1f ms)%s, "
                 "its steps up to %.4f (the left's %.4f; at once: %.4f), %d and %d splices; L == R "
                 "after: %s",
                 rl.name, fade, fade / 48.f, rl.reverse ? (back ? " (back to its own first)" : " (NOT back)") : "",
                 step_r, step_l, jump, splices[0], splices[1], same ? "yes" : "NO");
        Check(apart && same && in_time && step_r <= 1.1f * step_l && jump > 3.f * step_l
                  && splices[0] > 0 && splices[1] > 0 && (!rl.reverse || back > 0),
              line);
    }
    return Finish();
}
