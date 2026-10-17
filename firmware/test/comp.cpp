// comp.cpp: checks the master section around the finalizer (Finalizer.h, #345; its sound is
// finalizer.cpp's): the stereo-linked band compressor, the gain not following a bass note's
// waveform at its fastest; the engine's safety limiter keeping the outputs within 1.0 with
// everything up, and the finalizer's limiter keeping it from working (no red on key 15) with
// Loud and VOLUME full on a hot input; and the master settings' file format
// (MasterSettings.h): the finalizer's knobs and presets round-trip, a file from before (the
// master compressor's) loads with the finalizer neutral and the rest intact, one from a 1.5
// beta with its drive up loads with the drive dropped and every other knob where it was. Exits 0 when
// everything passes. Run by unit.sh comp.
#include <cmath>
#include <cstdio>
#include <cstring>
#include "check.h"
#include "MasterSettings.h"
#include "passthroughEngine.h"

using namespace chompi;

static const float kSr = 48000.f;
static const size_t kBlock = 24;

/** A finalizer with these parameters, run on n samples of l and r in place, in blocks */
static void Run(Finalizer& f, float* l, float* r, size_t n)
{
    for (size_t i = 0; i + kBlock <= n; i += kBlock)
        f.Process(l + i, r + i, kBlock);
}

static void TestLinked()
{
    // a loud left and a quiet right in the mid band, Depth full: the same gain on both
    Finalizer f;
    f.Init(kSr);
    f.SetParam(Finalizer::DEPTH, 1.f);
    const size_t n = 48000;
    static float l[n], r[n];
    for (size_t i = 0; i < n; i++)
    {
        const float s = sinf(2.f * static_cast<float>(M_PI) * 1000.f * i / kSr);
        l[i] = .5f * s;
        r[i] = .005f * s;
    }
    Run(f, l, r, n);
    float pl = 0.f, pr = 0.f;
    for (size_t i = n / 2; i < n; i++)
    {
        pl = fmaxf(pl, fabsf(l[i]));
        pr = fmaxf(pr, fabsf(r[i]));
    }
    printf("      left %.4f, right %.6f, reduced %.2f dB\n", pl, pr, f.BandReduction(1));
    Check(f.BandReduction(1) < -6.f && fabsf(pl / .5f - pr / .005f) < 2e-3f,
          "stereo linked: the quiet side gets the loud side's gain");
}

/** The gain's ripple, dB, on a steady sine of freq Hz at 0dB, the low band at its fastest
 *  (Beat FX) and Depth full: max minus min reduction over a second, once settled */
static float Ripple(float freq)
{
    Finalizer f;
    f.Init(kSr);
    f.SetParam(Finalizer::DEPTH, 1.f);
    f.SetParam(Finalizer::CHARACTER_LOW, 1.f);
    float lo = 0.f, hi = -100.f;
    float l[kBlock], r[kBlock];
    for (size_t b = 0; b < 96000 / kBlock; b++)
    {
        for (size_t i = 0; i < kBlock; i++)
            l[i] = r[i] = sinf(2.f * static_cast<float>(M_PI) * freq * (b * kBlock + i) / kSr);
        f.Process(l, r, kBlock);
        if (b >= 48000 / kBlock)
        {
            lo = fminf(lo, f.BandReduction(0));
            hi = fmaxf(hi, f.BandReduction(0));
        }
    }
    return hi - lo;
}

static void TestRipple()
{
    const float r50 = Ripple(50.f), r100 = Ripple(100.f);
    printf("      gain ripple, Beat FX on the low band, a 0dB sine: %.2fdB at 50Hz, %.2fdB at 100Hz\n",
           r50, r100);
    Check(r50 < 1.f && r100 < 1.f, "fast: the gain doesn't follow a bass note's waveform");
}

static int16_t loop_mem[kLoopMemSize];
static const size_t kDelayFrames = 480000;
static float delay_mem[kDelayFrames * 2];
static const size_t kFreezerFrames = 240000;
static float freezer_mem_l[kFreezerFrames], freezer_mem_r[kFreezerFrames];
static const size_t kTapeStopFrames = 1u << 19;
static float tapestop_mem_l[kTapeStopFrames], tapestop_mem_r[kTapeStopFrames];
static daisysp::Reverb reverb;
static MidiClock midi_clock;
static PassthroughEngine engine;

/** The engine at VOLUME and the input gain full, the finalizer's parameters as given; x(n)
 *  on AUX for n seconds: the outputs' peak, and the safety limiter's deepest gain after the
 *  first second */
static float Engine(std::initializer_list<std::pair<int, float>> fin, float (*x)(size_t), float& lim)
{
    engine.Init(kSr, loop_mem, &midi_clock, delay_mem, kDelayFrames, &reverb,
                freezer_mem_l, freezer_mem_r, kFreezerFrames,
                tapestop_mem_l, tapestop_mem_r, kTapeStopFrames);
    engine.SetMainGain(1.f);
    engine.SetInputGain(false, 1.f);
    for (auto& p : fin)
        engine.SetFinParam(p.first, p.second);
    float zero[kBlock] = {}, inl[kBlock], inr[kBlock];
    float o[4][kBlock];
    const float* in[4] = {zero, zero, inl, inr};
    float* out[4] = {o[0], o[1], o[2], o[3]};
    float peak = 0.f;
    lim = 1.f;
    for (size_t b = 0; b < 3 * 48000 / kBlock; b++)
    {
        for (size_t i = 0; i < kBlock; i++)
            inl[i] = inr[i] = x(b * kBlock + i);
        engine.Process(in, out, kBlock);
        for (size_t c = 0; c < 4; c++)
            for (size_t i = 0; i < kBlock; i++)
                peak = fmaxf(peak, fabsf(o[c][i]));
        const float g = engine.TakeLimiterGain();
        if (b >= 48000 / kBlock)
            lim = fminf(lim, g);
    }
    return peak;
}

// a full-scale square that starts from silence, so its first milliseconds come through
// before any reduction does
static float Square(size_t n) { return n < 12000 ? 0.f : ((n / 50) % 2 ? 1.f : -1.f); }
// a hot input: a bass and a tone near full scale, with bursts
static float Hot(size_t n)
{
    const float t = static_cast<float>(n) / kSr;
    return ((n % 9600) < 960 ? 1.f : .5f)
           * (.5f * sinf(2.f * static_cast<float>(M_PI) * 55.f * t) + .45f * sinf(2.f * static_cast<float>(M_PI) * 2300.f * t));
}

static void TestCeiling()
{
    // everything up: input, volume, the finalizer's input and EQ boosts (no Loud, whose
    // limiter would hold the level back)
    float lim;
    const float peak = Engine({{Finalizer::INPUT, 1.f}, {Finalizer::EQ_LOW, 1.f}, {Finalizer::EQ_LOW + 2, 1.f}},
                              Square, lim);
    printf("      peak out, everything up: %.7f\n", peak);
    // the soft clip's top (daisysp::SoftClip at 3) rounds to a float's step over 1.0
    Check(peak <= 1.f + 1e-6f && peak > .5f, "safety limiter: the outputs stay within 1.0");

    // Loud full, VOLUME full, a hot input: the finalizer's limiter keeps the safety limiter
    // under the 1dB where key 15 starts turning red (NormalPage's kLimStartDb)
    Engine({{Finalizer::LOUD, 1.f}}, Hot, lim);
    const float db = -20.f * log10f(lim);
    printf("      the safety limiter behind Loud full: %.2f dB at most\n", db);
    Check(db < 1.f, "Loud full, VOLUME full, a hot input: no red on key 15");
    // without the finalizer's limiter the same input does turn it red: the check sees red
    Engine({}, Hot, lim);
    Check(-20.f * log10f(lim) > 1.f, "and without Loud, the same input does");
}

static void TestFile()
{
    MasterSettings a, b;
    a.Reset();
    a.fin[0] = .3f;
    a.fin[3] = 1.f;
    a.fin[5] = .123457f;
    a.fin[11] = .4f;
    a.fin[14] = .625f;
    char buf[kMasterFileMax];
    size_t len = FormatMaster(a, buf, sizeof(buf));
    printf("%s", buf);
    bool same = ParseMaster(buf, b) && len > 0;
    for (size_t p = 0; p < kNumFinParams; p++)
        same = same && fabsf(a.fin[p] - b.fin[p]) < 1e-6f;
    Check(same, "file: the finalizer round-trips");
    Check(!strstr(buf, "finalizer_preset"), "file: no preset stored, none written");

    // presets: the ones stored, and only those
    a.fin_presets[1][4] = .75f;
    a.fin_presets[1][12] = 0.f;
    a.fin_presets[3][3] = 1.f;
    a.fin_presets_used = 0x0a;
    FormatMaster(a, buf, sizeof(buf));
    same = ParseMaster(buf, b) && b.fin_presets_used == 0x0a;
    for (size_t i = 0; i < kFinPresets; i++)
        for (size_t p = 0; p < kNumFinParams; p++)
            same = same && (!(a.fin_presets_used >> i & 1) || fabsf(a.fin_presets[i][p] - b.fin_presets[i][p]) < 1e-6f);
    Check(same && strstr(buf, "finalizer_preset2 ") && strstr(buf, "finalizer_preset4 ")
              && !strstr(buf, "finalizer_preset1") && !strstr(buf, "finalizer_preset3"),
          "file: the presets stored round-trip, the others aren't written");
    Check(ParseMaster("FRIZZ master 1\nfinalizer_preset5 1 1 1\nfinalizer_preset0 1\n", b)
              && b.fin_presets_used == 0,
          "file: a preset past 4, or 0, is skipped");

    Check(!ParseMaster("FRIZZ scenes 1\n", b) && b.fin[0] == kFinalizerKnobs[0].defaults[0],
          "file: another file isn't read, the defaults stay");
    Check(!ParseMaster("FRIZZ master 10\n", b), "file: another version isn't read");
    Check(ParseMaster("FRIZZ master 1\r\nvolume 3\r\nfinalizer 300000 0 0 2000000\r\n", b) &&
              b.fin[0] == .3f && b.fin[3] == 1.f && b.fin[4] == kFinalizerKnobs[1].defaults[0],
          "file: unknown lines skipped, values clamped, missing ones on their defaults");

    // a 1.5 beta's file, its drive up on Level's knobs 2 and 3, the finalizer's line and a
    // preset's: those two dropped (their defaults, off), every other knob where it was, and
    // the next write puts 0 there, the format as it was, so a beta reads it with the drive off
    Check(ParseMaster("FRIZZ master 1\nfinalizer 600000 750000 500000 250000 700000 450000 550000 "
                      "200000 400000 1000000 700000 300000 600000 800000\n"
                      "finalizer_preset3 400000 1000000 1000000 500000 500000 500000 500000 400000 "
                      "400000 400000 0 500000 500000 500000\nmono 1\n",
                      b),
          "file: a beta's, its drive up, is read");
    const float beta[kNumFinParams] = {.6f, 0.f, 0.f, .25f, .7f, .45f, .55f, 0.f,
                                       .2f, .4f, 1.f, .7f, .3f, .6f, .8f, 0.f};
    const float beta_preset[kNumFinParams] = {.4f, 0.f, 0.f, .5f, .5f, .5f, .5f, 0.f,
                                              .4f, .4f, .4f, 0.f, .5f, .5f, .5f, 0.f};
    same = b.mono && b.fin_presets_used == 0x04;
    for (size_t p = 0; p < kNumFinParams; p++)
        same = same && fabsf(b.fin[p] - beta[p]) < 1e-6f && fabsf(b.fin_presets[2][p] - beta_preset[p]) < 1e-6f;
    Check(same, "file: a beta's, its drive and type dropped (off), every other knob where it was, its preset's too");
    FormatMaster(b, buf, sizeof(buf));
    Check(strstr(buf, "\nfinalizer 600000 0 0 250000 700000 450000 550000 200000 400000 1000000 700000 300000 600000 800000\n")
              && strstr(buf, "\nfinalizer_preset3 400000 0 0 500000 "),
          "file: written again in the same format, 0 on Level's knobs 2 and 3");

    // a file from before the finalizer, the master compressor's: its lines skipped, the
    // finalizer neutral, every other setting read, and the old lines not written again
    Check(ParseMaster("FRIZZ master 1\ncompressor2 300000 500000 500000 500000 1000000 0 0 250000\n"
                      "compressor 300000 750000 200000 600000\nmono 1\nmidi_channel 5\ngain_aux 60\n",
                      b),
          "file: one from before the finalizer is read");
    same = b.mono && b.midi_channel == 5 && b.gain_aux == 60 && b.fin_presets_used == 0;
    for (size_t p = 0; p < kNumFinParams; p++)
        same = same && b.fin[p] == kFinalizerKnobs[p / kNumFxKnobs].defaults[p % kNumFxKnobs];
    Check(same, "file: its settings intact, the finalizer neutral (its defaults)");
    FormatMaster(b, buf, sizeof(buf));
    Check(!strstr(buf, "compressor") && strstr(buf, "\nfinalizer "), "file: written with the finalizer, not the old lines");
    Check(ParseMaster("FRIZZ master 1\n", b) && b.fin[11] == 0.f, "file: an empty one, every default");
    Check(FormatMaster(a, buf, 20) == 0, "file: one that doesn't fit isn't written");

    // a file from when there was a randomizer: its line is skipped and not written again
    Check(ParseMaster("FRIZZ master 1\nrandomizer 125000 500000 1000000 0\nmono 1\n", b) && b.mono,
          "file: one with the randomizer's line, the rest read");
    FormatMaster(b, buf, sizeof(buf));
    Check(!strstr(buf, "randomizer"), "file: the randomizer's line isn't written again");

    // the mono input switch, on a line of its own
    a.Reset();
    a.mono = true;
    FormatMaster(a, buf, sizeof(buf));
    Check(ParseMaster(buf, b) && b.mono, "file: mono round-trips");
    a.mono = false;
    FormatMaster(a, buf, sizeof(buf));
    Check(ParseMaster(buf, b) && !b.mono, "file: stereo round-trips");

    // MIDI's channel and transport following (MidiControl.h)
    a.Reset();
    a.midi_channel = 3;
    a.midi_transport = true;
    FormatMaster(a, buf, sizeof(buf));
    Check(ParseMaster(buf, b) && b.midi_channel == 3 && b.midi_transport,
          "file: the MIDI settings round-trip");
    a.midi_channel = 0;
    FormatMaster(a, buf, sizeof(buf));
    Check(ParseMaster(buf, b) && b.midi_channel == 0, "file: every channel (0) round-trips");
    Check(ParseMaster("FRIZZ master 1\nmidi_channel 17\n", b) && b.midi_channel == 16
              && !b.midi_transport,
          "file: one from before MIDI, or a channel past 16: channel 16, no transport");

    // the settings page's clock factor and LED brightness (SettingsPage.h)
    a.Reset();
    a.clock_factor = 200;
    a.led_brightness = 50;
    FormatMaster(a, buf, sizeof(buf));
    Check(ParseMaster(buf, b) && b.clock_factor == 200 && b.led_brightness == 50,
          "file: the clock factor and the LED brightness round-trip");
    Check(ParseMaster("FRIZZ master 1\nclock_factor 300\nled_brightness 10\n", b)
              && b.clock_factor == 100 && b.led_brightness == 100,
          "file: one from before them, or values they can't take: x1, full");

    // the clock source: 0 Auto, 1 TRS, 2 USB, 3 internal
    a.Reset();
    Check(a.clock_source == 0, "file: the clock source Auto by default");
    a.clock_source = 3;
    FormatMaster(a, buf, sizeof(buf));
    Check(ParseMaster(buf, b) && b.clock_source == 3, "file: the clock source round-trips");
    Check(ParseMaster("FRIZZ master 1\nclock_source 4\n", b) && b.clock_source == 0,
          "file: one from before it, or a source past 3: Auto");

    // MIDI out: 0 off, 1 the jack, 2 the jack and USB
    a.Reset();
    Check(a.midi_out == 0, "file: MIDI out off by default");
    a.midi_out = 2;
    FormatMaster(a, buf, sizeof(buf));
    Check(ParseMaster(buf, b) && b.midi_out == 2, "file: MIDI out round-trips");
    Check(ParseMaster("FRIZZ master 1\nmidi_out 3\n", b) && b.midi_out == 0,
          "file: one from before it, or a value past 2: off");

    // every setting at its longest, every preset stored, still fits the file's buffer
    a.Reset();
    for (size_t p = 0; p < kNumFinParams; p++)
    {
        a.fin[p] = 1.f;
        for (size_t i = 0; i < kFinPresets; i++)
            a.fin_presets[i][p] = .999999f;
    }
    a.fin_presets_used = 0x0f;
    a.mono = a.midi_transport = true;
    a.midi_channel = 16;
    a.clock_factor = 200;
    a.clock_source = 3;
    a.midi_out = 2;
    char big[kMasterFileMax];
    len = FormatMaster(a, big, sizeof(big));
    printf("      the longest file: %zu bytes of %zu\n", len, kMasterFileMax);
    Check(len > 0, "file: every setting at its longest, four presets, fits");
}

int main()
{
    TestLinked();
    TestRipple();
    TestCeiling();
    TestFile();
    return Finish();
}
