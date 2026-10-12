// scenes.cpp: checks the FX scene file format (FxScenes.h) and, through the engine, the
// recall's fast slew (FxChain::FastSlew). Exits 0 when everything passes. Run by unit.sh scenes.
#include <cmath>
#include <cstdio>
#include "check.h"
#include <cstring>
#include "FxScenes.h"
#include "passthroughEngine.h"

using namespace chompi;


static float defaults[kNumFx][kNumFxParams];

static bool SameScenes(const FxScene* a, const FxScene* b)
{
    for (size_t s = 0; s < kNumScenes; s++)
    {
        if (a[s].used != b[s].used)
            return false;
        if (!a[s].used)
            continue;
        if (a[s].latched != b[s].latched)
            return false;
        if (memcmp(a[s].params, b[s].params, sizeof(a[s].params)) != 0)
            return false;
    }
    return true;
}

// the engine, as harness.cpp builds it
static const float kSr = 48000.f;
static const size_t kBlock = 24;
static int16_t loop_mem[kLoopMemSize];
static const size_t kDelayFrames = 480000;
static float delay_mem[kDelayFrames * 2];
static const size_t kFreezerFrames = 240000;
static float freezer_mem_l[kFreezerFrames];
static float freezer_mem_r[kFreezerFrames];
static const size_t kTapeStopFrames = 1u << 19;
static float tapestop_mem_l[kTapeStopFrames];
static float tapestop_mem_r[kTapeStopFrames];
static daisysp::Reverb reverb;
static MidiClock midi_clock;
static PassthroughEngine engine;

/** A 220 Hz sine through the filter, its cutoff jumping from a deep lowpass to flat, with a
 *  recall's fast slew or a knob's. Gives the largest step between two output samples after
 *  the jump, and when the output's level (RMS over one period of the sine) is 90% of the way
 *  to where it ends up */
static void RecallJump(bool fast, float* max_step, float* settle_ms)
{
    engine.Init(kSr, loop_mem, &midi_clock, delay_mem, kDelayFrames, &reverb,
                freezer_mem_l, freezer_mem_r, kFreezerFrames,
                tapestop_mem_l, tapestop_mem_r, kTapeStopFrames);
    engine.SetMainGain(1.f);
    engine.SetInputGain(false, 1.f);
    engine.SetMix(0.f);
    engine.SetCompParam(MasterComp::kThreshold, 0.f);
    engine.SetFxParam(FX_FILTER, 0, .05f);
    engine.SetFxParam(FX_FILTER, 1, 0.f);
    engine.SetFxParam(FX_FILTER, 2, 0.f);
    engine.SetFxOn(FX_FILTER, true);

    float zero[kBlock] = {}, inl[kBlock], inr[kBlock], o[4][kBlock];
    const float* in[4] = {zero, zero, inl, inr};
    float* out[4] = {o[0], o[1], o[2], o[3]};
    double phase = 0.;
    const size_t before = static_cast<size_t>(.5f * kSr / kBlock);
    const size_t after = static_cast<size_t>(.3f * kSr / kBlock);
    const size_t period = static_cast<size_t>(kSr / 220.f + .5f);

    // the output, the last period before the jump first
    static float rec[20000];
    static float prev[256];
    size_t n = 0;
    float last = 0.f;
    *max_step = 0.f;
    for (size_t b = 0; b < before + after; b++)
    {
        if (b == before)
        {
            for (size_t i = 0; i < period; i++)
                rec[n++] = prev[(b * kBlock + i) % period];
            if (fast)
                engine.FastFxSlew();
            engine.SetFxParam(FX_FILTER, 0, .5f);
        }
        for (size_t i = 0; i < kBlock; i++)
        {
            inl[i] = inr[i] = static_cast<float>(sin(phase)) * .3f;
            phase += 2. * M_PI * 220. / kSr;
        }
        engine.Process(in, out, kBlock);
        for (size_t i = 0; i < kBlock; i++)
        {
            if (b < before)
                prev[(b * kBlock + i) % period] = o[2][i];
            else
            {
                *max_step = fmaxf(*max_step, fabsf(o[2][i] - last));
                if (n < 20000)
                    rec[n++] = o[2][i];
            }
            last = o[2][i];
        }
    }

    // rms(k): the period ending k samples after the jump
    auto rms = [&](size_t k) {
        double sum = 0.;
        for (size_t i = k; i < k + period; i++)
            sum += rec[i] * rec[i];
        return static_cast<float>(sqrt(sum / period));
    };
    const size_t len = n - period;
    const float start = rms(0), end = rms(len - 1);
    size_t settled = 0;
    for (size_t k = 0; k < len; k++)
        if (fabsf(rms(k) - end) > .1f * fabsf(end - start))
            settled = k + 1;
    *settle_ms = settled / (kSr / 1000.f);
}

int main()
{
    for (size_t fx = 0; fx < kNumFx; fx++)
        for (size_t p = 0; p < kNumFxParams; p++)
            defaults[fx][p] = .25f + .01f * (fx * kNumFxParams + p);

    // scenes with awkward values: grid points, the ends, and a knob's float drift
    FxScene scenes[kNumScenes] = {};
    scenes[0].used = true;
    scenes[0].latched = 0x2a5;
    scenes[2].used = true;
    scenes[2].latched = (1u << kNumFx) - 1;
    for (size_t fx = 0; fx < kNumFx; fx++)
        for (size_t p = 0; p < kNumFxParams; p++)
        {
            scenes[0].params[fx][p] = (fx * kNumFxParams + p) / static_cast<float>(kNumFx * kNumFxParams - 1);
            float drift = 0.f;
            for (size_t i = 0; i < fx * 7 + p; i++)
                drift += .01f;
            scenes[2].params[fx][p] = fminf(drift, 1.f);
        }
    scenes[0].params[FX_RESONATOR][0] = .436295f + 17 * .0156585f; // a coarse pitch point
    scenes[0].params[FX_SHIFTER][0] = 19.f / 24.f;
    scenes[2].params[FX_FILTER][0] = 0.f;
    scenes[2].params[FX_FILTER][1] = 1.f;

    static char text[kSceneFileMax];
    const size_t len = FormatScenes(scenes, text, sizeof(text));
    Check(len > 0 && len < 2048, "formats into under 2KB");

    // 1. through the text and back: every value within 1e-6, the grid points on the grid
    FxScene read[kNumScenes];
    Check(ParseScenes(text, defaults, read), "reads its own file");
    float worst = 0.f;
    bool same_latches = true;
    for (size_t s = 0; s < kNumScenes; s++)
    {
        if (read[s].used != scenes[s].used || (scenes[s].used && read[s].latched != scenes[s].latched))
            same_latches = false;
        if (!scenes[s].used)
            continue;
        for (size_t fx = 0; fx < kNumFx; fx++)
            for (size_t p = 0; p < kNumFxParams; p++)
                worst = fmaxf(worst, fabsf(read[s].params[fx][p] - scenes[s].params[fx][p]));
    }
    Check(same_latches, "slots and latches come back");
    printf("      worst parameter error %.2g\n", worst);
    Check(worst <= 1e-6f, "parameters within 1e-6");
    // CoarseStep counts a value within 1% of the spacing as on a grid point
    Check(fabsf(read[0].params[FX_RESONATOR][0] - scenes[0].params[FX_RESONATOR][0]) < .0156585f * .01f,
          "a resonator pitch grid point stays on it");

    // 2. a second round trip is exact, bit for bit
    static char text2[kSceneFileMax];
    FormatScenes(read, text2, sizeof(text2));
    FxScene read2[kNumScenes];
    ParseScenes(text2, defaults, read2);
    Check(strcmp(text, text2) == 0 && SameScenes(read, read2), "the second round trip is exact");

    // 3. by name: unknown effects skipped, missing ones on their defaults, order free,
    //    short lines keep the rest at defaults, CRLF and junk lines tolerated
    const char* hand =
        "FRIZZ scenes 1\r\n"
        "scene 4\r\n"
        "reverb 1 100000 200000 300000 400000\r\n"
        "teleporter 1 1 1 1 1\r\n"
        "freezer 0 500000\r\n"
        "\r\n"
        "this line means nothing\r\n"
        "scene 9\r\n"
        "delay 1 0 0 0 0\r\n";
    FxScene h[kNumScenes];
    Check(ParseScenes(hand, defaults, h), "reads a hand-written file");
    Check(!h[0].used && !h[1].used && !h[2].used && h[3].used, "only scene 4 is used");
    Check(h[3].latched == (1u << FX_REVERB), "reverb latched, the unknown effect and scene 9 skipped");
    Check(fabsf(h[3].params[FX_REVERB][7] - LevelKnob(.4f)) < 1e-6f,
          "reverb level read, onto page 2's knob 4 in dB (layout 5)");
    Check(h[3].params[FX_REVERB][4] == defaults[FX_REVERB][4]
              && h[3].params[FX_REVERB][3] == defaults[FX_REVERB][3],
          "a line of four, as v0.11 wrote them: page 2 on its defaults, the ducking now on "
          "page 1's knob 4 too");

    // a file from #35, before page 2's shared knobs (no layout line): its page 2 was the
    // shifter's mix alone, kept; the zeros it wrote for the rest load as defaults, or every
    // effect's Level would be off
    {
        const char* old =
            "FRIZZ scenes 1\n"
            "scene 1\n"
            "shifter 1 791667 0 0 0 250000 0 0 0\n"
            "folder 1 500000 0 1000000 0 0 0 0 0\n";
        FxScene o[kNumScenes];
        Check(ParseScenes(old, defaults, o) && o[0].params[FX_SHIFTER][4] == .25f
                  && o[0].params[FX_SHIFTER][7] == defaults[FX_SHIFTER][7]
                  && o[0].params[FX_FOLDER][4] == defaults[FX_FOLDER][4]
                  && o[0].params[FX_FOLDER][7] == defaults[FX_FOLDER][7]
                  && o[0].params[FX_FOLDER][0] == .5f,
              "a file from before the shared knobs: the shifter's mix kept, the rest of page 2 on defaults");
        // and a file with them reads page 2 as written
        const char* now =
            "FRIZZ scenes 1\n"
            "layout 2\n"
            "scene 1\n"
            "folder 1 500000 0 1000000 0 0 0 0 0\n";
        Check(ParseScenes(now, defaults, o) && o[0].params[FX_FOLDER][7] == 0.f,
              "layout 2: page 2 as written, a Level of 0 too");
    }

    // page 2 (#35): v0.11 reads the first four values of a line and skips the rest, so a
    // newer file gives it page 1 as saved
    {
        const char* line = " 1 100000 200000 300000 400000 500000 600000 700000 800000\nnext";
        const char* p = line;
        size_t wlen;
        scenefile::Word(p, wlen); // the latch
        float four[4] = {};
        scenefile::ReadValues(p, four, 4);
        scenefile::NextLine(p);
        Check(fabsf(four[3] - .4f) < 1e-6f && strcmp(p, "next") == 0,
              "a line of eight read as v0.11 does: page 1's four, then the next line");
    }
    // the largest file: 4 scenes, every value 1, every effect latched
    {
        FxScene full[kNumScenes];
        for (size_t s = 0; s < kNumScenes; s++)
        {
            full[s].used = true;
            full[s].latched = (1u << kNumFx) - 1;
            for (size_t fx = 0; fx < kNumFx; fx++)
                for (size_t p = 0; p < kNumFxParams; p++)
                    full[s].params[fx][p] = 1.f;
        }
        static char big[kSceneFileMax];
        const size_t big_len = FormatScenes(full, big, sizeof(big));
        printf("      the largest file: %zu of %zu bytes\n", big_len, kSceneFileMax);
        FxScene back[kNumScenes];
        Check(big_len > 0 && ParseScenes(big, defaults, back) && SameScenes(full, back),
              "the largest file, both pages, fits and reads back");
    }
    Check(fabsf(h[3].params[FX_FREEZER][0] - .5f) < 1e-6f
              && h[3].params[FX_FREEZER][1] == defaults[FX_FREEZER][1],
          "a short line keeps the rest at defaults");
    Check(h[3].params[FX_SHIFTER][2] == defaults[FX_SHIFTER][2], "a missing effect gets its defaults");
    Check(!(h[3].latched >> FX_CHAOS & 1) && h[3].params[FX_CHAOS][0] == defaults[FX_CHAOS][0]
              && h[3].params[FX_CHAOS][2] == defaults[FX_CHAOS][2],
          "a file from before the chaos key: it loads off, on its defaults");
    Check(!(h[3].latched >> FX_PHASER & 1) && h[3].params[FX_PHASER][0] == defaults[FX_PHASER][0]
              && h[3].params[FX_PHASER][5] == defaults[FX_PHASER][5],
          "a file from before the phaser: it loads off, on its defaults");
    {
        const char* chaos = "FRIZZ scenes 1\nlayout 3\nscene 2\nchaos 1 300000 600000 750000 1000000\n";
        FxScene c[kNumScenes];
        Check(ParseScenes(chaos, defaults, c) && (c[1].latched >> FX_CHAOS & 1)
                  && fabsf(c[1].params[FX_CHAOS][1] - .6f) < 1e-6f
                  && fabsf(c[1].params[FX_CHAOS][2] - .75f) < 1e-6f,
              "the chaos key's line: latched, its knobs");
    }
    {
        const char* phaser = "FRIZZ scenes 1\nlayout 4\nscene 3\nphaser 1 750000 500000 1000000 0 1000000 250000\n";
        FxScene c[kNumScenes];
        Check(ParseScenes(phaser, defaults, c) && (c[2].latched == 1u << FX_PHASER)
                  && fabsf(c[2].params[FX_PHASER][0] - .75f) < 1e-6f
                  && fabsf(c[2].params[FX_PHASER][5] - .25f) < 1e-6f
                  && c[2].params[FX_PHASER][7] == defaults[FX_PHASER][7],
              "the phaser's line: latched, its knobs, the rest on its defaults");
    }

    // layout 4 (#117): stereo moved from page 1's knob 4 to page 2's knob 2 and the effect's
    // own the other way. A file from before loads with the two swapped, the knobs a line lacks
    // on the defaults of their places now, and the tape stop and the sends as they were
    {
        const char* v3 = "FRIZZ scenes 1\nlayout 3\nscene 1\n"
                         "shifter 1 500000 0 0 300000 1000000 100000 500000 750000\n"
                         "tapestop 1 600000 600000 0 900000 1000000 200000 500000 750000\n"
                         "delay 1 250000 400000 500000 300000 0 500000 500000 0\n"
                         "scene 2\n"
                         "phaser 1 250000 400000 0 300000\n";
        FxScene m[kNumScenes];
        Check(ParseScenes(v3, defaults, m) && m[0].params[FX_SHIFTER][5] == .3f
                  && m[0].params[FX_SHIFTER][3] == .1f && m[0].params[FX_SHIFTER][4] == 1.f,
              "layout 3: stereo to page 2's knob 2, the effect's own to page 1's knob 4");
        Check(m[0].params[FX_TAPESTOP][3] == .9f && m[0].params[FX_TAPESTOP][5] == .2f
                  && m[0].params[FX_DELAY][7] == LevelKnob(.3f) && m[0].params[FX_DELAY][3] == 0.f
                  && m[0].params[FX_DELAY][5] == .5f,
              "layout 3: the tape stop and the delay keep their knobs (the delay's level and "
              "ducking swapped by layout 5)");
        Check(m[1].params[FX_PHASER][5] == .3f
                  && m[1].params[FX_PHASER][3] == defaults[FX_PHASER][3]
                  && m[1].params[FX_PHASER][7] == defaults[FX_PHASER][7],
              "layout 3, page 2 left out: its own on its default, on page 1's knob 4");

        const char* v2 = "FRIZZ scenes 1\nlayout 2\nscene 1\n"
                         "shifter 1 500000 0 0 300000 1000000 0 500000 750000\n";
        const char* v1 = "FRIZZ scenes 1\nscene 1\n"
                         "shifter 1 500000 0 0 300000\n"
                         "filter 1 500000 500000 0 600000\n";
        Check(ParseScenes(v2, defaults, m) && m[0].params[FX_SHIFTER][5] == .3f
                  && m[0].params[FX_SHIFTER][3] == defaults[FX_SHIFTER][3],
              "layout 2: stereo to page 2's knob 2, the new own knob on its default");
        Check(ParseScenes(v1, defaults, m) && m[0].params[FX_SHIFTER][5] == .3f
                  && m[0].params[FX_SHIFTER][3] == defaults[FX_SHIFTER][3]
                  && m[0].params[FX_FILTER][3] == .6f
                  && m[0].params[FX_FILTER][5] == defaults[FX_FILTER][5],
              "v0.11: stereo to page 2's knob 2, the filter's LFO division stays on knob 4");
    }

    // layout 5 (#172, #173): the sends' level, a gain on page 1's knob 4 until then, on
    // page 2's knob 4 in dB (mute, then -36dB to +12dB, 0dB at 3/4), their ducking on page 1's
    // knob 4; a gain under -36dB mutes. The freezer's roll keeps its step: five both times
    {
        const char* v4 = "FRIZZ scenes 1\nlayout 4\nscene 1\n"
                         "delay 1 250000 400000 500000 500000 0 500000 500000 300000\n"
                         "reverb 1 600000 600000 600000 1000000 0 200000 500000 0\n"
                         "freezer 1 1000000 0 750000 0 1000000 0 500000 750000\n"
                         "scene 2\n"
                         "delay 0 250000 400000 500000 10000\n"
                         "reverb 0 600000 600000 600000 0\n";
        FxScene m[kNumScenes];
        Check(ParseScenes(v4, defaults, m), "layout 4: read");
        const float half = m[0].params[FX_DELAY][7];
        printf("      a delay's level of .5 from layout 4: Level %.5f, %.2f dB\n", half,
               20.f * log10f(FxOutput::LevelGain(half)));
        Check(fabsf(FxOutput::LevelGain(half) - .5f) < 1e-5f && m[0].params[FX_DELAY][3] == .3f
                  && m[0].params[FX_DELAY][5] == .5f,
              "layout 4: the delay's level .5 on page 2's knob 4 at -6dB, its ducking .3 on "
              "page 1's knob 4");
        Check(m[0].params[FX_REVERB][7] == kP2Level && m[0].params[FX_REVERB][3] == 0.f
                  && fabsf(m[0].params[FX_REVERB][5] - .2f) < 1e-6f,
              "layout 4: the reverb's level 1 to 0dB exactly, its ducking off");
        Check(m[1].params[FX_DELAY][7] == 0.f && m[1].params[FX_REVERB][7] == 0.f
                  && m[1].params[FX_DELAY][3] == defaults[FX_DELAY][3],
              "layout 4: a level under -36dB and a level of 0 mute; page 2 left out on its "
              "defaults");
        Check(m[0].params[FX_FREEZER][2] == .75f
                  && StepIndex(m[0].params[FX_FREEZER][2], Freezer::kNumRolls) == 3,
              "layout 4: the freezer's roll keeps its knob, its 4th step (1/8 of the length)");
        Check(m[0].params[FX_SHIFTER][7] == defaults[FX_SHIFTER][7]
                  && m[0].params[FX_DELAY][0] == .25f,
              "layout 4: the rest as it was");
        // and the defaults survive the way back and forth for a line that lacks them
        float d[kNumFxParams];
        memcpy(d, kFxParams[FX_REVERB].defaults, sizeof(d));
        SendsFrom5(FX_REVERB, d);
        SendsTo5(FX_REVERB, d);
        Check(memcmp(d, kFxParams[FX_REVERB].defaults, sizeof(d)) == 0,
              "layout 5's upgrade: the defaults back exactly from their place before");
    }

    // 4. not a scene file, and an empty one
    FxScene bad[kNumScenes];
    bad[1].used = true;
    Check(!ParseScenes("hello\nscene 1\n", defaults, bad) && !bad[1].used, "rejects a foreign file");
    Check(!ParseScenes("", defaults, bad), "rejects an empty file");
    Check(!ParseScenes("FRIZZ scenes 10\nscene 1\n", defaults, bad) && !bad[0].used,
          "rejects another version whose header starts the same");
    // a layout from a newer FRIZZ, or not a number: unreadable (to the .bak at the next
    // save), not read as an older one and overwritten (#168)
    {
        char newer[64];
        snprintf(newer, sizeof(newer), "FRIZZ scenes 1\nscene 1\nlayout %ld\nscene 2\n",
                 scenefile::kLayoutNow + 1);
        Check(!ParseScenes(newer, defaults, bad) && !bad[0].used && !bad[1].used,
              "rejects a layout newer than this FRIZZ's, every scene unused");
        Check(!ParseScenes("FRIZZ scenes 1\nlayout x\nscene 1\n", defaults, bad) && !bad[0].used
                  && !ParseScenes("FRIZZ scenes 1\nlayout 4x\nscene 1\n", defaults, bad)
                  && !ParseScenes("FRIZZ scenes 1\nlayout\nscene 1\n", defaults, bad),
              "rejects a layout that isn't a number");
        char now[64];
        snprintf(now, sizeof(now), "FRIZZ scenes 1\nlayout %ld\nscene 1\n", scenefile::kLayoutNow);
        Check(ParseScenes(now, defaults, bad) && bad[0].used, "reads its own layout");
    }
    Check(ParseScenes("FRIZZ scenes 1\r\nscene 1\r\n", defaults, bad) && bad[0].used,
          "reads a file saved with Windows line ends");

    // 5. cut off mid-line (a power cut without the .tmp): what's there is read, nothing crashes
    char cut[kSceneFileMax];
    strncpy(cut, text, len / 2);
    cut[len / 2] = '\0';
    FxScene c[kNumScenes];
    Check(ParseScenes(cut, defaults, c) && c[0].used, "reads a truncated file");

    // 6. a buffer too small: refused rather than cut short
    char tiny[100];
    Check(FormatScenes(scenes, tiny, sizeof(tiny)) == 0, "refuses a buffer too small");

    // 7. no scenes: just the header, which reads back as all empty
    FxScene none[kNumScenes] = {};
    char empty[64];
    FormatScenes(none, empty, sizeof(empty));
    FxScene e[kNumScenes];
    Check(ParseScenes(empty, defaults, e) && !e[0].used && !e[3].used, "an empty set round-trips");

    // 8. the fast slew, through the engine: a recall's jump lands quickly, without a click,
    //    and the knobs slew normally afterwards
    Check(FxSlew::coeff == kFxParamCoeff, "the knobs slew normally");
    float step[2], settle_ms[2];
    for (int fast = 0; fast < 2; fast++)
        RecallJump(fast, &step[fast], &settle_ms[fast]);
    printf("      filter jump: 90%% after %.1f ms (knob slew %.1f ms), largest step between "
           "samples %.3f (knob slew %.3f)\n", settle_ms[1], settle_ms[0], step[1], step[0]);
    Check(settle_ms[1] < 30.f && settle_ms[1] < settle_ms[0] * .75f, "a recall lands within 30 ms, sooner than a knob");
    Check(step[1] < .1f, "without a click");
    Check(FxSlew::coeff == kFxParamCoeff, "then the knobs slew normally again");

    return Finish();
}
