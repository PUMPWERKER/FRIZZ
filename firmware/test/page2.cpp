// page2.cpp: checks each effect's own knobs on page 2 (#40) and the stereo that took page 1's
// knob 4 on the folder, crusher and filter: the freezer's gate, the shifter's grain, the
// folder's, crusher's and filter's stereo and their moved knobs, the flanger's polarity, the
// resonator's env mod and return level, the slicer's shuffle, wow & flutter's age, the tape
// stop's depth and darken, the delay's freeze, damping and ducking, the reverb's freeze,
// pre-delay and ducking; and the scene file's layout 3 (FxScenes.h). Exits 0 when everything
// passes. Run by unit.sh page2.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "check.h"
#include "FxParams.h"
#include "FxScenes.h"

using namespace chompi;

static const float kSr = 48000.f;

static float Sine(size_t i, float freq = 220.f, float amp = .3f)
{
    return amp * sinf(2.f * float(M_PI) * freq * static_cast<float>(i) / kSr);
}

/** RMS of a and b over n, and of their difference */
struct Diff
{
    double a = 0., b = 0., d = 0.;
    size_t n = 0;
    void Add(float x, float y)
    {
        a += x * x;
        b += y * y;
        d += (x - y) * (x - y);
        n++;
    }
    float Rel() const { return static_cast<float>(sqrt(d / (a + 1e-20))); }
};

// ======== the freezer's gate ========

static const size_t kFreezerFrames = 240000;
static float fz_l[kFreezerFrames], fz_r[kFreezerFrames];

static void TestFreezerGate()
{
    // a 1/16 loop at 120 BPM (6000 frames), the gate at the top: an eighth of each repeat
    static chompi::Freezer f;
    f.Init(kSr, fz_l, fz_r, kFreezerFrames);
    f.SetTempo(120.f);
    f.SetParam(chompi::Freezer::LENGTH, 0.f);
    f.SetParam(chompi::Freezer::GATE, 1.f);
    f.SetOn(true);
    f.ClockPulse(0);
    size_t silent = 0, loud = 0;
    for (size_t i = 0; i < 48000; i++)
    {
        float l = Sine(i), r = l;
        f.Process(&l, &r);
        // silent: the key's fade gets within 1e-4 of 1, so the input's share below that
        if (i > 12000 + 6000 && i < 48000)
            (fabsf(l) < 1e-3f ? silent : loud)++;
    }
    printf("  freezer gate at the top: %zu silent, %zu heard\n", silent, loud);
    Check(silent > 4 * loud && loud > 0, "freezer: the gate leaves the start of each repeat, the rest silent");
}

// ======== the shifter's grain ========

static void TestShifterGrain()
{
    static chompi::Shifter a, b;
    for (chompi::Shifter* s : {&a, &b})
    {
        s->Init(kSr);
        s->SetParam(chompi::Shifter::SHIFT, .5f + 7.f / 24.f);
        s->SnapParams();
        s->SetOn(true);
    }
    b.SetParam(chompi::Shifter::GRAIN, 0.f); // 10ms
    Diff d;
    for (size_t i = 0; i < 48000; i++)
    {
        float l1 = Sine(i), r1 = l1, l2 = l1, r2 = l1;
        a.Process(&l1, &r1);
        b.Process(&l2, &r2);
        if (i > 4800)
            d.Add(l1, l2);
    }
    Check(d.Rel() > .05f && std::isfinite(d.Rel()), "shifter: the grain changes the sound");
}

// ======== stereo on page 1's knob 4, the old knob 4 on page 2's knob 2 ========

template <class Fx>
static float StereoSpread(Fx& fx, float driven_amp)
{
    Diff d;
    for (size_t i = 0; i < 48000; i++)
    {
        float l = Sine(i, 220.f, driven_amp), r = l;
        fx.Process(&l, &r);
        if (i > 4800)
            d.Add(l, r);
    }
    return d.Rel();
}

static void TestStereo()
{
    static chompi::Folder fo;
    fo.Init(kSr);
    fo.SetParam(chompi::Folder::DRIVE, .5f);
    fo.SetParam(chompi::Folder::TONE, 1.f);
    fo.SnapParams();
    fo.SetOn(true);
    const float mono = StereoSpread(fo, .3f);
    fo.SetParam(chompi::Folder::STEREO, 1.f);
    fo.SnapParams();
    const float wide = StereoSpread(fo, .3f);
    Check(mono == 0.f && wide > .1f, "folder: knob 4 is stereo: off both sides alike, on they differ");

    static chompi::Crusher cr;
    cr.Init(kSr);
    cr.SetParam(chompi::Crusher::RATE, .6f);
    cr.SetParam(chompi::Crusher::TONE, 1.f);
    cr.SnapParams();
    cr.SetOn(true);
    const float cmono = StereoSpread(cr, .3f);
    cr.SetParam(chompi::Crusher::STEREO, 1.f);
    const float cwide = StereoSpread(cr, .3f);
    Check(cmono == 0.f && cwide > .05f, "crusher: knob 4 is stereo, the right rate lower");

    static chompi::Filter fi;
    fi.Init(kSr);
    fi.SetParam(chompi::Filter::CUTOFF, .3f);
    fi.SetParam(chompi::Filter::LFO_DEPTH, 1.f);
    fi.SetParam(chompi::Filter::LFO_DIVISION, 0.f);
    fi.SnapParams();
    fi.SetPulseSamples(2000.f);
    fi.SetOn(true);
    Diff fm, fw;
    for (int pass = 0; pass < 2; pass++)
    {
        fi.SetParam(chompi::Filter::STEREO, pass ? 1.f : 0.f);
        for (size_t i = 0; i < 48000; i++)
        {
            if (i % 2000 == 0)
                fi.ClockPulse(static_cast<uint32_t>(i / 2000));
            float l = Sine(i, 220.f, .3f) + Sine(i, 3000.f, .1f), r = l;
            fi.Process(&l, &r);
            if (i > 9600)
                (pass ? fw : fm).Add(l, r);
        }
    }
    Check(fm.Rel() == 0.f && fw.Rel() > .05f, "filter: knob 4 is stereo, the right LFO lagging");

    // the old knob 4s are page 2's knob 2: symmetry, XOR, LFO division
    Check(kFxParams[FX_FOLDER].defaults[chompi::Folder::SYMMETRY] == 0.f && chompi::Folder::SYMMETRY == 5
              && chompi::Crusher::XOR == 5 && chompi::Filter::LFO_DIVISION == 5
              && kFxParams[FX_FILTER].steps[5] == chompi::Filter::kNumLfoDivisions,
          "folder, crusher, filter: symmetry, XOR and the LFO division on page 2's knob 2");
}

// ======== the flanger's polarity ========

static void TestFlangerPolarity()
{
    static chompi::Flanger a, b;
    for (chompi::Flanger* f : {&a, &b})
    {
        f->Init(kSr);
        f->SetParam(chompi::Flanger::AMOUNT, .5f);
        f->SetParam(chompi::Flanger::FEEDBACK, .8f);
        f->SnapParams();
        f->SetOn(true);
    }
    b.SetParam(chompi::Flanger::POLARITY, 1.f);
    b.SnapParams();
    Diff d;
    for (size_t i = 0; i < 48000; i++)
    {
        float l1 = Sine(i), r1 = l1, l2 = l1, r2 = l1;
        a.Process(&l1, &r1);
        b.Process(&l2, &r2);
        if (i > 4800)
            d.Add(l1, l2);
    }
    Check(d.Rel() > .05f, "flanger: negative polarity sounds different");
}

// ======== the resonator's env mod and level ========

static float ResonatorRing(float env_mod, float level)
{
    static chompi::Resonator res;
    res.Init(kSr);
    res.SetParam(chompi::Resonator::FEEDBACK, .3f);
    res.SetParam(chompi::Resonator::ENV_MOD, env_mod);
    res.SetParam(chompi::Resonator::LEVEL, level);
    res.SnapParams();
    res.SetOn(true);
    double sum = 0.;
    for (size_t i = 0; i < 48000; i++)
    {
        float l = i < 24000 ? Sine(i) : 0.f, r = l;
        res.Feed(&l, &r);
        res.Tap(l, r);
        if (i >= 24000 && i < 26400)
            sum += l * l; // the ring after the input stops
        if (i > 4800 && i < 24000)
            sum += res.Return() * res.Return();
    }
    return static_cast<float>(sqrt(sum));
}

static void TestResonator()
{
    const float plain = ResonatorRing(0.f, .75f), env = ResonatorRing(1.f, .75f),
                quiet = ResonatorRing(0.f, .5f), off = ResonatorRing(0.f, 0.f);
    printf("  resonator: ring %.2f, env mod %.2f, level -12dB %.2f, level off %.2f\n", plain, env,
           quiet, off);
    Check(env > plain * 1.2f, "resonator: env mod rings more on a loud input");
    Check(quiet < plain * .5f && off < plain * .01f, "resonator: its level turns the return down, off at 0");
}

// ======== the slicer's shuffle ========

/** The sample of each envelope hit on the left channel over 2 bars at 120 BPM */
static std::vector<size_t> SlicerHits(float shuffle)
{
    static chompi::Slicer s;
    s.Init(kSr);
    s.SetParam(chompi::Slicer::PATTERN, 1.f);
    s.SetParam(chompi::Slicer::DECAY, 0.f);
    s.SetParam(chompi::Slicer::SHUFFLE, shuffle);
    s.SetPulseSamples(2000.f);
    s.SetOn(true);
    std::vector<size_t> hits;
    float last = 0.f;
    bool rising = false;
    for (size_t i = 0; i < 192000; i++)
    {
        if (i % 2000 == 0)
            s.ClockPulse(static_cast<uint32_t>(i / 2000));
        float l = 1.f, r = 1.f;
        s.Process(&l, &r);
        if (l > last + 1e-4f && !rising && i > 2000)
            hits.push_back(i);
        rising = l > last + 1e-4f;
        last = l;
    }
    return hits;
}

static void TestSlicerShuffle()
{
    const std::vector<size_t> straight = SlicerHits(0.f), swung = SlicerHits(1.f);
    bool odd_same = straight.size() == swung.size() && straight.size() > 8, even_late = odd_same;
    for (size_t k = 0; odd_same && k < straight.size(); k++)
    {
        const long d = static_cast<long>(swung[k]) - static_cast<long>(straight[k]);
        // a hit on step n of the 16ths: even ones (2, 4 ...) late by up to 2/3 of 6000
        const size_t step = (straight[k] + 100) / 6000;
        if (step & 1)
            even_late = even_late && d > 3500 && d < 4100;
        else
            odd_same = odd_same && labs(d) < 2;
    }
    Check(odd_same && even_late, "slicer: shuffle at the top: the even steps 2/3 of a 16th late, the rest on time");
}

// ======== wow & flutter's age ========

static void TestWarbleAge()
{
    static chompi::Warble w;
    w.Init(kSr);
    w.SetParam(chompi::Warble::AGE, 1.f);
    w.SnapParams();
    w.SetOn(true);
    float lowest = 1.f;
    for (size_t i = 0; i < 4 * 48000; i++)
    {
        float l = .5f, r = .5f;
        w.Process(&l, &r);
        if (i > 4800)
            lowest = fminf(lowest, l / .5f);
    }
    printf("  wow & flutter, age at the top: the level dips to %.2f\n", lowest);
    Check(lowest < .7f && lowest >= 0.f, "wow & flutter: age dips the level at random");
}

// ======== the tape stop's depth and darken ========

static const size_t kTapeFrames = 1u << 19;
static float tape_l[kTapeFrames], tape_r[kTapeFrames];

static float StoppedLevel(float depth, float darken, float freq)
{
    static chompi::TapeStop t;
    t.Init(kSr, tape_l, tape_r, kTapeFrames);
    t.SetTempo(120.f);
    t.SetParam(chompi::TapeStop::STOP, 0.f); // 1/16
    t.SetParam(chompi::TapeStop::DEPTH, depth);
    t.SetParam(chompi::TapeStop::DARKEN, darken);
    for (size_t i = 0; i < 4800; i++)
    {
        float l = Sine(i, freq), r = l;
        t.Process(&l, &r);
    }
    t.SetOn(true);
    double sum = 0.;
    for (size_t i = 4800; i < 4800 + 24000; i++)
    {
        float l = Sine(i, freq), r = l;
        t.Process(&l, &r);
        if (i > 4800 + 12000)
            sum += l * l;
    }
    return static_cast<float>(sqrt(sum / 12000.));
}

static void TestTapeStop()
{
    const float full = StoppedLevel(1.f, 0.f, 220.f), half = StoppedLevel(0.f, 0.f, 220.f);
    const float bright = StoppedLevel(0.f, 0.f, 4000.f), dark = StoppedLevel(0.f, 1.f, 4000.f);
    printf("  tape stop held: depth 1 %.3f, depth 0 %.3f; at 4kHz %.3f, darkened %.3f\n", full, half,
           bright, dark);
    Check(full < 1e-4f && half > .1f, "tape stop: depth 1 stops it, depth 0 only slows it");
    Check(dark < bright * .7f, "tape stop: darken takes the highs off as it slows");
}

// ======== the delay ========

static const size_t kDelayFrames = 480000;
static float delay_mem[kDelayFrames * 2];

/** The delay's return: a burst of sine, then silence; its RMS from from to to (samples) */
static float DelayTail(float freeze, float damping, float ducking, size_t from, size_t to,
                       float freq = 220.f, bool keep_input = false)
{
    static chompi::DelaySend d;
    d.Init(48000.f, delay_mem, kDelayFrames);
    d.SetTempo(120.f);
    d.SetParam(chompi::DelaySend::DIVISION, .25f);
    d.SetParam(chompi::DelaySend::FEEDBACK, .5f);
    d.SetParam(chompi::DelaySend::RANDOM, .5f);
    d.SetParam(chompi::DelaySend::LEVEL, 1.f);
    d.SetParam(chompi::DelaySend::DAMPING, damping);
    d.SetParam(chompi::DelaySend::DUCKING, ducking);
    d.SetOn(true);
    double sum = 0.;
    for (size_t i = 0; i < to; i++)
    {
        if (i == 48000)
            d.SetParam(chompi::DelaySend::FREEZE, freeze);
        const float in = i < 24000 || keep_input ? Sine(i, freq) : 0.f;
        float l = 0.f, r = 0.f;
        d.Process(in, in, &l, &r);
        if (i >= from)
            sum += l * l;
    }
    return static_cast<float>(sqrt(sum / static_cast<double>(to - from)));
}

static void TestDelay()
{
    const float fades = DelayTail(0.f, .5f, 0.f, 240000, 288000);
    const float held = DelayTail(1.f, .5f, 0.f, 240000, 288000);
    printf("  delay 4 s on: plain %.4f, frozen %.4f\n", fades, held);
    Check(held > 10.f * fades && held > .01f, "delay: freeze holds the echoes");
    const float early = DelayTail(1.f, .5f, 0.f, 96000, 144000), late = DelayTail(1.f, .5f, 0.f, 432000, 480000);
    printf("  delay frozen at 2 s %.4f, at 9 s %.4f\n", early, late);
    Check(late > early * .9f && late < early * 1.1f, "delay: frozen, the level holds, neither falling nor growing");
    const float open = DelayTail(0.f, .5f, 0.f, 48000, 96000, 4000.f);
    const float damped = DelayTail(0.f, 1.f, 0.f, 48000, 96000, 4000.f);
    const float thin = DelayTail(0.f, 0.f, 0.f, 48000, 96000, 60.f);
    const float thick = DelayTail(0.f, .5f, 0.f, 48000, 96000, 60.f);
    Check(damped < open * .7f && thin < thick * .7f, "delay: damping right darkens the repeats, left thins them");
    const float free = DelayTail(0.f, .5f, 0.f, 24000, 48000, 220.f, true);
    const float ducked = DelayTail(0.f, .5f, 1.f, 24000, 48000, 220.f, true);
    Check(ducked < free * .3f, "delay: ducking turns the echoes down under the input");
}

// ======== the reverb ========

static daisysp::Reverb reverb;

static float ReverbRun(float freeze, float pre, float ducking, size_t from, size_t to,
                       bool keep_input = false, size_t* onset = nullptr)
{
    static chompi::ReverbSend v;
    v.Init(kSr, &reverb);
    v.SetParam(chompi::ReverbSend::DECAY, .5f);
    v.SetParam(chompi::ReverbSend::LEVEL, 1.f);
    v.SetParam(chompi::ReverbSend::PRE_DELAY, pre);
    v.SetParam(chompi::ReverbSend::DUCKING, ducking);
    v.SetOn(true);
    double sum = 0.;
    if (onset)
        *onset = 0;
    for (size_t i = 0; i < to; i++)
    {
        if (i == 24000)
            v.SetParam(chompi::ReverbSend::FREEZE, freeze);
        const float in = (i >= 4800 && i < 24000) || keep_input ? Sine(i) : 0.f;
        float l = 0.f, r = 0.f;
        v.Process(in, in, &l, &r);
        if (onset && !*onset && fabsf(l) > 1e-3f)
            *onset = i;
        if (i >= from)
            sum += l * l;
    }
    return static_cast<float>(sqrt(sum / static_cast<double>(to - from)));
}

static void TestReverb()
{
    const float fades = ReverbRun(0.f, 0.f, 0.f, 200000, 240000);
    const float held = ReverbRun(1.f, 0.f, 0.f, 200000, 240000);
    printf("  reverb 4 s on: plain %.5f, frozen %.5f\n", fades, held);
    Check(held > 20.f * fades && held > 1e-3f, "reverb: freeze holds the room");
    size_t plain_on = 0, pre_on = 0;
    ReverbRun(0.f, 0.f, 0.f, 0, 30000, false, &plain_on);
    ReverbRun(0.f, 1.f, 0.f, 0, 30000, false, &pre_on);
    printf("  reverb onset: %zu, with 250ms pre-delay %zu\n", plain_on, pre_on);
    Check(pre_on > plain_on + 11000 && pre_on < plain_on + 13000, "reverb: pre-delay at the top: 250ms later");
    const float free = ReverbRun(0.f, 0.f, 0.f, 24000, 48000, true);
    const float ducked = ReverbRun(0.f, 0.f, 1.f, 24000, 48000, true);
    Check(ducked < free * .3f, "reverb: ducking turns the room down under the input");
}

// ======== the scene file: layout 3 ========

static void TestLayout()
{
    float defaults[kNumFx][kNumFxParams];
    for (size_t fx = 0; fx < kNumFx; fx++)
        for (size_t p = 0; p < kNumFxParams; p++)
            defaults[fx][p] = kFxParams[fx].defaults[p];
    // v0.11 (no layout): folder symmetry .4, crusher XOR .3, filter division .5, tape stop's
    // unused 4th 0; layout 2 (this branch's first test builds): a delay's page 2 as an
    // insert's then (mix 1, ..., level .75), which would freeze it now
    const char* old =
        "FRIZZ scenes 1\n"
        "scene 1\n"
        "folder 1 500000 0 1000000 400000\n"
        "crusher 0 0 0 1000000 300000\n"
        "filter 0 500000 500000 0 500000\n"
        "tapestop 0 600000 600000 0 0\n"
        "layout 2\n"
        "scene 2\n"
        "delay 1 250000 400000 500000 300000 1000000 0 500000 750000\n"
        "folder 1 500000 0 1000000 400000 1000000 0 500000 500000\n";
    FxScene s[kNumScenes];
    Check(ParseScenes(old, defaults, s), "layout: reads older files");
    Check(s[0].params[FX_FOLDER][5] == .4f && s[0].params[FX_FOLDER][3] == 0.f
              && s[0].params[FX_CRUSHER][5] == .3f && s[0].params[FX_CRUSHER][3] == 0.f
              && s[0].params[FX_FILTER][5] == .5f && s[0].params[FX_FILTER][3] == 0.f,
          "layout: v0.11's symmetry, XOR and LFO division move to page 2's knob 2, stereo off");
    Check(s[0].params[FX_TAPESTOP][3] == 1.f, "layout: the tape stop's depth a full stop");
    Check(s[1].params[FX_DELAY][4] == 0.f && s[1].params[FX_DELAY][5] == .5f
              && s[1].params[FX_DELAY][7] == 0.f && s[1].params[FX_DELAY][3] == .3f,
          "layout 2: a send's page 2 on its defaults (not frozen), page 1 kept");
    Check(s[1].params[FX_FOLDER][7] == .5f && s[1].params[FX_FOLDER][4] == 1.f
              && s[1].params[FX_FOLDER][5] == .4f,
          "layout 2: an insert's Mix and Level kept, its symmetry moved");
    char buf[kSceneFileMax];
    FormatScenes(s, buf, sizeof(buf));
    FxScene back[kNumScenes];
    Check(strstr(buf, "layout 3\n") && ParseScenes(buf, defaults, back)
              && back[1].params[FX_FOLDER][5] == .4f && back[0].params[FX_FOLDER][3] == 0.f,
          "layout 3: written, and read back as written");
}

// ======== the chain: an effect's key coming on again ========

static chompi::FxChain chain;

/** The chain at its defaults, every key off */
static void ChainInit()
{
    chain.Init(kSr, delay_mem, kDelayFrames, &reverb, fz_l, fz_r, kFreezerFrames, tape_l,
               tape_r, kTapeFrames);
    chain.SetTempo(120.f, 2000.f);
}

/** samples of in through the chain, the keys taken at every block of 24 as FxChain::Block
 *  does; the output's peak. level: the meter of fx after the first block, if asked */
template <class In>
static float ChainRun(size_t& n, size_t samples, In in, size_t fx = 0, float* level = nullptr)
{
    float peak = 0.f;
    for (size_t i = 0; i < samples; i++, n++)
    {
        if (i % 24 == 0)
        {
            if (level && i == 24)
                *level = chain.GetLevel(fx);
            float jump;
            chain.Block(&jump);
        }
        float l = in(n), r = l;
        chain.Process(&l, &r);
        peak = fmaxf(peak, fmaxf(fabsf(l), fabsf(r)));
    }
    return peak;
}

/** An insert with page 2 off its defaults: off and faded out, its page 2 drops out of the
 *  chain's work (out_busy_), and on again it's back. Level at 0 mutes the folder */
static void TestKeyOnPage2()
{
    ChainInit();
    size_t n = 0;
    auto sine = [](size_t i) { return Sine(i); };
    chain.SetParam(chompi::FX_FOLDER, chompi::FxOutput::kLevel, 0.f);
    chain.SetOn(chompi::FX_FOLDER, true);
    ChainRun(n, 24000, sine);
    const float muted = ChainRun(n, 12000, sine);
    chain.SetOn(chompi::FX_FOLDER, false);
    ChainRun(n, 24000, sine);
    const bool left_out = !chain.OutRunning(chompi::FX_FOLDER);
    const float off = ChainRun(n, 12000, sine);
    chain.SetOn(chompi::FX_FOLDER, true);
    ChainRun(n, 4800, sine);
    const float again = ChainRun(n, 12000, sine);
    printf("  folder at Level 0: on %.5f, off %.3f, on again %.5f\n", muted, off, again);
    Check(muted < 1e-3f && off > .25f, "key on again: an insert's Level 0 mutes it, off it's the input");
    Check(left_out, "key on again: an insert that's off leaves its page 2 out of the chain's work");
    Check(again < 1e-3f, "key on again: its page 2 is back (Level 0 mutes it again)");
}

/** A send with its Band turned: the key off long enough for the delay to sleep, then on again
 *  with silence going in. Silence comes out: Band's crossovers start afresh, not from what
 *  they held when the key went off (an offset the delay repeated) */
static void TestKeyOnBand()
{
    ChainInit();
    size_t n = 0;
    chain.SetParam(chompi::FX_DELAY, 1, 0.f);   // no feedback: one echo
    chain.SetParam(chompi::FX_DELAY, 3, 1.f);   // its level
    chain.SetParam(chompi::FX_DELAY, chompi::FxOutput::kBand, .2f); // the lows
    chain.SetOn(chompi::FX_DELAY, true);
    auto offset = [](size_t) { return .5f; };
    ChainRun(n, 48000, offset);
    chain.SetOn(chompi::FX_DELAY, false);
    ChainRun(n, 4800, offset); // faded out on it: what Band last saw
    auto silence = [](size_t) { return 0.f; };
    ChainRun(n, 12 * 48000, silence);
    chain.SetOn(chompi::FX_DELAY, true);
    const float peak = ChainRun(n, 48000, silence);
    printf("  delay, Band to the lows, on again on silence: peak %.6f\n", peak);
    Check(peak < 1e-6f, "key on again: a send's Band starts afresh, silence in is silence out");
}

/** An insert's meter, frozen while it's off: on again, it starts from nothing rather than
 *  from where it stopped (its key LED flashed at the old level) */
static void TestKeyOnMeter()
{
    ChainInit();
    size_t n = 0;
    auto loud = [](size_t i) { return Sine(i, 220.f, .9f); };
    chain.SetOn(chompi::FX_FOLDER, true);
    ChainRun(n, 24000, loud);
    chain.SetOn(chompi::FX_FOLDER, false);
    ChainRun(n, 24000, loud);
    const float stopped = chain.GetLevel(chompi::FX_FOLDER);
    auto silence = [](size_t) { return 0.f; };
    ChainRun(n, 4800, silence);
    chain.SetOn(chompi::FX_FOLDER, true);
    float level = 1.f;
    ChainRun(n, 48, silence, chompi::FX_FOLDER, &level);
    printf("  folder's meter: %.3f when it went off, %.3f on again on silence\n", stopped, level);
    Check(stopped > .5f && level < .1f, "key on again: an insert's meter starts from nothing");
}

int main()
{
    TestFreezerGate();
    TestShifterGrain();
    TestStereo();
    TestFlangerPolarity();
    TestResonator();
    TestSlicerShuffle();
    TestWarbleAge();
    TestTapeStop();
    TestDelay();
    TestReverb();
    TestLayout();
    TestKeyOnPage2();
    TestKeyOnBand();
    TestKeyOnMeter();
    return Finish();
}
