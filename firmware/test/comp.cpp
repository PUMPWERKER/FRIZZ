// comp.cpp: checks the master compressor (MasterComp.h): off is an exact bypass, its static
// curve with no makeup of its own, the makeup knob, its attack and release, no gain ripple
// on bass at its fastest, the linked stereo and the mix; the
// engine's safety limiter keeping the outputs within 1.0; and the master settings' file
// format (MasterSettings.h). Exits 0 when everything passes. Run by unit.sh comp.
#include <cmath>
#include <cstdio>
#include <cstring>
#include "check.h"
#include "MasterSettings.h"
#include "passthroughEngine.h"

using namespace chompi;

static const float kSr = 48000.f;

static float Db(float x) { return 20.f * log10f(fabsf(x)); }

/** A compressor with its knobs set, settled; speed sets the attack and the release alike */
static void Setup(MasterComp& c, float amount, float ratio, float speed, float mix,
                  float makeup = 0.f)
{
    c.Init(kSr);
    c.SetParam(MasterComp::kThreshold, amount);
    c.SetParam(MasterComp::kRatio, ratio);
    c.SetParam(MasterComp::kAttack, speed);
    c.SetParam(MasterComp::kRelease, speed);
    c.SetParam(MasterComp::kMix, mix);
    c.SetParam(MasterComp::kMakeup, makeup);
    float l = 0.f, r = 0.f;
    for (int i = 0; i < 48000; i++)
    {
        l = r = 0.f;
        c.Process(&l, &r);
    }
}

/** The output level, dB, of a steady level in dB, once settled */
static float Steady(MasterComp& c, float in_db)
{
    const float in = powf(10.f, in_db / 20.f);
    float l = 0.f, r = 0.f;
    for (int i = 0; i < 96000; i++)
    {
        l = r = in;
        c.Process(&l, &r);
    }
    return Db(l);
}

static void TestBypass()
{
    MasterComp c;
    Setup(c, 0.f, .5f, .5f, 1.f);
    bool same = true;
    uint32_t n = 1;
    for (int i = 0; i < 48000; i++)
    {
        n = n * 1664525u + 1013904223u;
        const float x = (static_cast<float>(n >> 8) / 16777216.f - .5f) * 8.f;
        float l = x, r = -x * .5f;
        c.Process(&l, &r);
        same = same && l == x && r == -x * .5f;
    }
    Check(same, "threshold 0, makeup 0: an exact bypass");

    // threshold 0 with makeup: that gain alone, nothing compressed
    Setup(c, 0.f, .5f, .5f, 1.f, .25f);
    const float out = Steady(c, 0.f);
    Check(fabsf(out - 6.f) < .05f, "threshold 0, makeup +6dB: 6dB louder, nothing else");

    Setup(c, 1.f, 1.f, .5f, 0.f);
    same = true;
    for (int i = 0; i < 4800; i++)
    {
        float l = 2.f, r = -2.f;
        c.Process(&l, &r);
        same = same && l == 2.f && r == -2.f;
    }
    Check(same, "mix 0: the dry signal, exactly");
}

static void TestCurve()
{
    MasterComp c;
    // threshold -30dB; 4:1 reduces 0dB by 22.5dB, and nothing gives any of it back
    Setup(c, 1.f, .5f, .5f, 1.f);
    float out = Steady(c, 0.f);
    printf("      0dB in, threshold -30dB, 4:1: %.2fdB out\n", out);
    Check(fabsf(out - -22.5f) < .2f, "threshold -30dB, 4:1: 0dB comes out at -22.5dB");
    out = Steady(c, -40.f);
    Check(fabsf(out - -40.f) < .05f, "below the threshold: untouched, no makeup of its own");
    out = Steady(c, -30.f);
    // in the knee's middle: a quarter of the knee times the slope
    Check(fabsf(out - (-30.f - .75f * 6.f / 8.f)) < .2f, "at the threshold: the soft knee");

    Setup(c, 1.f, 1.f, .5f, 1.f);
    out = Steady(c, 0.f);
    Check(fabsf(out - -28.5f) < .2f, "20:1: 0dB comes out at -28.5dB");
    const float louder = Steady(c, 6.f);
    Check(louder - out < .5f, "20:1: 6dB louder in is under 0.5dB louder out");

    Setup(c, .5f, .5f, .5f, 1.f);
    out = Steady(c, 0.f);
    Check(fabsf(out - -15.f * .75f) < .2f, "threshold .5: -15dB");

    // the makeup: 0 to +24dB, on everything, set by hand
    Setup(c, 1.f, .5f, .5f, 1.f, .5f);
    out = Steady(c, 0.f);
    Check(fabsf(out - (-22.5f + 12.f)) < .2f, "makeup .5: +12dB on the compressed signal");
    out = Steady(c, -40.f);
    Check(fabsf(out - (-40.f + 12.f)) < .2f, "makeup .5: +12dB below the threshold too");
    Setup(c, 1.f, .5f, .5f, 1.f, 1.f);
    out = Steady(c, -40.f);
    Check(fabsf(out - (-40.f + 24.f)) < .2f, "makeup 1: +24dB");
}

/** The reduction, dB, on a steady sine of freq Hz at 0dB, with the sidechain highpass at sc */
static float ReductionAt(float freq, float sc)
{
    MasterComp c;
    Setup(c, 1.f, .5f, 0.f, 1.f);
    c.SetParam(MasterComp::kSidechain, sc);
    float deepest = 0.f;
    for (int i = 0; i < 96000; i++)
    {
        float l = sinf(2.f * float(M_PI) * freq * i / kSr), r = l;
        c.Process(&l, &r);
        if (i > 72000)
            deepest = fminf(deepest, c.GetReduction());
    }
    return deepest;
}

static void TestSidechain()
{
    const float bass = ReductionAt(50.f, 0.f), bass_hp = ReductionAt(50.f, 1.f);
    const float mid = ReductionAt(2000.f, 0.f), mid_hp = ReductionAt(2000.f, 1.f);
    printf("      sidechain highpass: 50Hz %.1f -> %.1fdB, 2kHz %.1f -> %.1fdB\n", bass, bass_hp, mid,
           mid_hp);
    Check(bass_hp > bass + 10.f, "sidechain: at the top, a bass note is compressed far less");
    Check(fabsf(mid_hp - mid) < 1.f, "sidechain: the mids are compressed as before");
    MasterComp c;
    Setup(c, 0.f, .5f, .5f, 1.f);
    c.SetParam(MasterComp::kSidechain, 1.f);
    bool same = true;
    for (int i = 0; i < 4800; i++)
    {
        float l = .7f, r = -.3f;
        c.Process(&l, &r);
        same = same && l == .7f && r == -.3f;
    }
    Check(same, "sidechain: with the threshold off, still an exact bypass");
}

/** ms until the reduction has recovered to 3dB after a loud burst stops */
static float Recovery(float speed)
{
    MasterComp c;
    Setup(c, 1.f, .5f, speed, 1.f);
    Steady(c, 0.f);
    for (int i = 0; i < 96000; i++)
    {
        float l = .01f, r = .01f;
        c.Process(&l, &r);
        if (c.GetReduction() > -3.f)
            return i / 48.f;
    }
    return 2000.f;
}

/** ms until the reduction reaches 10dB after a loud signal starts */
static float Grab(float speed)
{
    MasterComp c;
    Setup(c, 1.f, .5f, speed, 1.f);
    Steady(c, -60.f);
    for (int i = 0; i < 96000; i++)
    {
        float l = 1.f, r = 1.f;
        c.Process(&l, &r);
        if (c.GetReduction() < -10.f)
            return i / 48.f;
    }
    return 2000.f;
}

static void TestSpeed()
{
    const float r0 = Recovery(0.f), r5 = Recovery(.5f), r1 = Recovery(1.f);
    const float a0 = Grab(0.f), a5 = Grab(.5f), a1 = Grab(1.f);
    printf("      to 3dB after a burst: %.0f / %.0f / %.0fms; to 10dB on a step: %.2f / %.2f / %.2fms\n",
           r0, r5, r1, a0, a5, a1);
    Check(r0 < r5 && r5 < r1 && r1 < 2000.f, "release: slows from fast to slow");
    Check(a0 < a5 && a5 < a1, "attack: slows from fast to slow");
    Check(a0 < 1.f && a1 < 30.f, "attack: under 1ms fast, under 30ms slow");
}

static void TestLinked()
{
    MasterComp c;
    Setup(c, 1.f, .5f, .5f, 1.f);
    float l = 0.f, r = 0.f;
    for (int i = 0; i < 48000; i++)
    {
        l = 1.f;
        r = .01f;
        c.Process(&l, &r);
    }
    Check(fabsf(l / 1.f - r / .01f) < 1e-4f, "stereo linked: the quiet side gets the loud side's gain");
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

static void TestCeiling()
{
    // everything up: input, volume, the compressor's makeup, and a full-scale square that
    // starts from silence, so its first milliseconds come through before the reduction does
    engine.Init(kSr, loop_mem, &midi_clock, delay_mem, kDelayFrames, &reverb,
                freezer_mem_l, freezer_mem_r, kFreezerFrames,
                tapestop_mem_l, tapestop_mem_r, kTapeStopFrames);
    engine.SetMainGain(1.f);
    engine.SetInputGain(false, 1.f);
    engine.SetCompParam(MasterComp::kThreshold, 1.f);
    engine.SetCompParam(MasterComp::kRatio, 0.f);
    engine.SetCompParam(MasterComp::kAttack, 1.f);
    engine.SetCompParam(MasterComp::kRelease, 1.f);
    engine.SetCompParam(MasterComp::kMix, 1.f);
    engine.SetCompParam(MasterComp::kMakeup, 1.f);
    const size_t kBlock = 24;
    float zero[kBlock] = {}, inl[kBlock], inr[kBlock];
    float o[4][kBlock];
    const float* in[4] = {zero, zero, inl, inr};
    float* out[4] = {o[0], o[1], o[2], o[3]};
    float peak = 0.f;
    for (size_t b = 0; b < 48000 / kBlock; b++)
    {
        for (size_t i = 0; i < kBlock; i++)
        {
            const size_t n = b * kBlock + i;
            const float x = n < 12000 ? 0.f : ((n / 50) % 2 ? 1.f : -1.f);
            inl[i] = inr[i] = x;
        }
        engine.Process(in, out, kBlock);
        for (size_t c = 0; c < 4; c++)
            for (size_t i = 0; i < kBlock; i++)
                peak = fmaxf(peak, fabsf(o[c][i]));
    }
    printf("      peak out, everything up: %.7f\n", peak);
    // the soft clip's top (daisysp::SoftClip at 3) rounds to a float's step over 1.0
    Check(peak <= 1.f + 1e-6f && peak > .5f, "safety limiter: the outputs stay within 1.0");
}

static void TestFile()
{
    MasterSettings a, b;
    a.Reset();
    a.comp[0] = .3f;
    a.comp[1] = .75f;
    a.comp[2] = 0.f;
    a.comp[3] = .123457f;
    a.comp[4] = .4f;
    a.comp[7] = .625f;
    char buf[kMasterFileMax];
    const size_t len = FormatMaster(a, buf, sizeof(buf));
    printf("%s", buf);
    bool same = ParseMaster(buf, b) && len > 0;
    for (size_t p = 0; p < kNumFxKnobs; p++)
        same = same && fabsf(a.comp[p] - b.comp[p]) < 1e-6f;
    Check(same, "file: round-trips");

    // the coarse ratio's points come back on the grid
    a.comp[1] = .25f;
    FormatMaster(a, buf, sizeof(buf));
    ParseMaster(buf, b);
    Check(b.comp[1] == .25f, "file: a grid point comes back exactly");

    Check(!ParseMaster("FRIZZ scenes 1\n", b) && b.comp[3] == kCompParams.defaults[3],
          "file: another file isn't read, the defaults stay");
    Check(!ParseMaster("FRIZZ master 10\n", b), "file: another version isn't read");
    Check(ParseMaster("FRIZZ master 1\r\nvolume 3\r\ncompressor2 500000 2000000\r\n", b) &&
              b.comp[0] == .5f && b.comp[1] == 1.f && b.comp[2] == kCompParams.defaults[2],
          "file: unknown lines skipped, values clamped, missing ones on their defaults");

    // one from before the compressor's page 2: its speed is the attack and the release, its
    // mix moves to page 2, and the makeup starts at 0dB (it was automatic)
    Check(ParseMaster("FRIZZ master 1\ncompressor 300000 750000 200000 600000\n", b) &&
              b.comp[0] == .3f && b.comp[1] == .75f && b.comp[2] == .2f && b.comp[3] == .2f &&
              b.comp[4] == .6f && b.comp[7] == 0.f,
          "file: the old compressor line, speed to attack and release, makeup 0dB");
    Check(ParseMaster("FRIZZ master 1\ncompressor2 100000 0 0 0 1000000 0 0 500000\n"
                      "compressor 300000 750000 200000 600000\n", b) &&
              b.comp[0] == .1f && b.comp[7] == .5f,
          "file: the new line wins over an old one");
    FormatMaster(b, buf, sizeof(buf));
    Check(strstr(buf, "compressor2 ") && !strstr(buf, "\ncompressor "),
          "file: written as the new line only");
    Check(ParseMaster("FRIZZ master 1\n", b) && b.comp[0] == kCompParams.defaults[0],
          "file: an empty one, every default");
    Check(FormatMaster(a, buf, 20) == 0, "file: one that doesn't fit isn't written");

    // a file from when there was a randomizer: its line is skipped and not written again
    Check(ParseMaster("FRIZZ master 1\ncompressor 500000 500000 500000 500000\n"
                      "randomizer 125000 500000 1000000 0\nmono 1\n", b) &&
              b.comp[0] == .5f && b.mono,
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
    Check(ParseMaster("FRIZZ master 1\ncompressor 500000 500000 500000 500000\n", b) && !b.mono,
          "file: one from before the mono switch, stereo");

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
    // every setting at its longest still fits the file's buffer
    a.Reset();
    for (size_t p = 0; p < kNumFxKnobs; p++)
        a.comp[p] = 1.f;
    a.mono = a.midi_transport = true;
    a.midi_channel = 16;
    a.clock_factor = 200;
    a.clock_source = 3;
    a.midi_out = 2;
    char big[kMasterFileMax];
    Check(FormatMaster(a, big, sizeof(big)) > 0, "file: every setting at its longest fits");
}

/** The gain's ripple, dB, on a steady sine of freq Hz at 0dB: max minus min reduction
 *  over a second, once settled */
static float Ripple(float freq, float speed)
{
    MasterComp c;
    Setup(c, 1.f, 1.f, speed, 1.f);
    float lo = 0.f, hi = -100.f;
    for (int i = 0; i < 96000; i++)
    {
        float l = sinf(2.f * static_cast<float>(M_PI) * freq * i / kSr), r = l;
        c.Process(&l, &r);
        if (i >= 48000)
        {
            lo = fminf(lo, c.GetReduction());
            hi = fmaxf(hi, c.GetReduction());
        }
    }
    return hi - lo;
}

static void TestRipple()
{
    const float r50 = Ripple(50.f, 0.f), r100 = Ripple(100.f, 0.f);
    printf("      gain ripple at 20:1, fast, on a 0dB sine: %.2fdB at 50Hz, %.2fdB at 100Hz\n",
           r50, r100);
    Check(r50 < 1.f && r100 < 1.f, "fast: the gain doesn't follow a bass note's waveform");
}

int main()
{
    TestBypass();
    TestCurve();
    TestSpeed();
    TestSidechain();
    TestRipple();
    TestLinked();
    TestCeiling();
    TestFile();
    return Finish();
}
