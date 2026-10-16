// page2.cpp: checks each effect's own knobs on page 2 (#40) and the stereo that took page 1's
// knob 4 on the folder, crusher and filter: the freezer's gate, the shifter's grain, the
// folder's, crusher's and filter's stereo and their moved knobs, the flanger's polarity, the
// resonator's env mod and return level, the slicer's shuffle, wow & flutter's age, the tape
// stop's depth and darken, the delay's freeze (its highs too, at a delay between two samples),
// damping and ducking, the reverb's freeze (its
// level a minute on, and the held room's pitch: no detuning, no wider swing, #249; quiet
// material held 5 minutes, #419),
// pre-delay and ducking; and the scene file's layout 3 (FxScenes.h). Exits 0 when everything
// passes. Run by unit.sh page2.
#include <algorithm>
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
    Check(mono == 0.f && wide > .1f, "folder: page 2's knob 2 is stereo: off both sides alike, on they differ");

    static chompi::Crusher cr;
    cr.Init(kSr);
    cr.SetParam(chompi::Crusher::RATE, .6f);
    cr.SetParam(chompi::Crusher::TONE, 1.f);
    cr.SnapParams();
    cr.SetOn(true);
    const float cmono = StereoSpread(cr, .3f);
    cr.SetParam(chompi::Crusher::STEREO, 1.f);
    const float cwide = StereoSpread(cr, .3f);
    Check(cmono == 0.f && cwide > .05f, "crusher: page 2's knob 2 is stereo, the right rate lower");

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
    Check(fm.Rel() == 0.f && fw.Rel() > .05f, "filter: page 2's knob 2 is stereo, the right LFO lagging");

    // their own on page 1's knob 4 (#117): symmetry, XOR, LFO division
    Check(kFxParams[FX_FOLDER].defaults[chompi::Folder::SYMMETRY] == 0.f && chompi::Folder::SYMMETRY == 3
              && chompi::Crusher::XOR == 3 && chompi::Filter::LFO_DIVISION == 3
              && kFxParams[FX_FILTER].steps[3] == chompi::Filter::kNumLfoDivisions
              && kFxParams[FX_FILTER].steps[5] == 0,
          "folder, crusher, filter: symmetry, XOR and the LFO division on page 1's knob 4");
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
                       float freq = 220.f, bool keep_input = false, float bpm = 120.f)
{
    static chompi::DelaySend d;
    d.Init(48000.f, delay_mem, kDelayFrames);
    d.SetTempo(bpm);
    d.SetParam(chompi::DelaySend::DIVISION, .25f);
    d.SetParam(chompi::DelaySend::FEEDBACK, .5f);
    d.SetParam(chompi::DelaySend::RANDOM, .5f);
    d.SetParam(chompi::DelaySend::LEVEL, kP2Level);
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
    {
        // at 140 BPM a 1/4 is 20571.43 samples: read between two, linearly, every pass took the
        // highs down again, and frozen (the loop's gain 1) an 8kHz tone lost 12dB in 7s;
        // frozen, the delay reads on whole samples
        // over 3 passes each (61714 samples), so both windows hold the same loop
        const float hi_early = DelayTail(1.f, .5f, 0.f, 96000, 96000 + 61714, 8000.f, false, 140.f);
        const float hi_late = DelayTail(1.f, .5f, 0.f, 420000, 420000 + 61714, 8000.f, false, 140.f);
        printf("  delay frozen at 140 BPM, 8 kHz: at 2 s %.4f, at 9 s %.4f (%.1f dB)\n", hi_early, hi_late,
               20.f * log10f(hi_late / hi_early));
        Check(hi_late > hi_early * .89f && hi_late < hi_early * 1.12f,
              "delay: frozen between two samples (140 BPM), an 8kHz tone holds its level within 1dB");
    }
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
    v.SetParam(chompi::ReverbSend::LEVEL, kP2Level);
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

/** The pitch of a tone around hz in y from s0 to s1, as #313's tail measure: an 8192-point
 *  Hann STFT every 512 samples, each frame's peak within 5% of hz interpolated on its log
 *  power; the track's median in Hz and its spread (standard deviation) in cents */
static void TonePitch(const std::vector<float>& y, size_t s0, size_t s1, float hz, double* median,
                      double* spread)
{
    static const size_t kN = 8192, kHop = 512; // a bin 5.86Hz
    const size_t kLo = static_cast<size_t>(hz * .95f * kN / kSr);
    const size_t kHi = static_cast<size_t>(hz * 1.05f * kN / kSr) + 1;
    static float tables_hz = 0.f;
    static std::vector<double> win, cs, sn;
    if (win.empty())
    {
        win.resize(kN);
        for (size_t i = 0; i < kN; i++)
            win[i] = .5 - .5 * cos(2. * M_PI * static_cast<double>(i) / kN);
    }
    if (tables_hz != hz)
    {
        tables_hz = hz;
        cs.resize((kHi - kLo + 2) * kN);
        sn.resize(cs.size());
        for (size_t k = kLo - 1; k <= kHi; k++)
            for (size_t i = 0; i < kN; i++)
            {
                const double a = 2. * M_PI * static_cast<double>(k * i % kN) / kN;
                cs[(k - kLo + 1) * kN + i] = cos(a) * win[i];
                sn[(k - kLo + 1) * kN + i] = sin(a) * win[i];
            }
    }
    std::vector<double> track;
    for (size_t s = s0; s + kN <= s1; s += kHop)
    {
        std::vector<double> pw(kHi - kLo + 2);
        for (size_t k = kLo - 1; k <= kHi; k++)
        {
            const double* c = &cs[(k - kLo + 1) * kN];
            const double* q = &sn[(k - kLo + 1) * kN];
            double re = 0., im = 0.;
            for (size_t i = 0; i < kN; i++)
            {
                re += y[s + i] * c[i];
                im += y[s + i] * q[i];
            }
            pw[k - kLo + 1] = re * re + im * im;
        }
        size_t m = 1;
        for (size_t j = 2; j < kHi - kLo + 1; j++)
            if (pw[j] > pw[m])
                m = j;
        const double a = log(pw[m - 1] + 1e-30), b = log(pw[m] + 1e-30), c = log(pw[m + 1] + 1e-30);
        const double bin = static_cast<double>(kLo - 1 + m) + (a - c) / (2. * (a - 2. * b + c));
        track.push_back(bin * kSr / kN);
    }
    std::vector<double> sorted = track;
    std::sort(sorted.begin(), sorted.end());
    *median = sorted[sorted.size() / 2];
    double sum = 0., sum2 = 0.;
    for (double f : track)
    {
        const double cents = 1200. * log2(f / *median);
        sum += cents;
        sum2 += cents * cents;
    }
    const double n = static_cast<double>(track.size());
    *spread = sqrt(fmax(sum2 / n - (sum / n) * (sum / n), 0.));
}

/** #249: a tone into the reverb, frozen at 0.5s as it ends: the room it holds keeps its
 *  pitch, from 1s on and a minute later, its swing no wider than the tank's modulation makes
 *  it (#313), and its level: it doesn't grow, and keeps its highs (#394) */
static void TestReverbFreezePitch(float hz)
{
    static chompi::ReverbSend v;
    v.Init(kSr, &reverb);
    v.SetParam(chompi::ReverbSend::DECAY, .5f);
    v.SetParam(chompi::ReverbSend::LEVEL, kP2Level);
    v.SetOn(true);
    const size_t kEarly = 48000, kLate = 61 * 48000, kSpan = 110400; // 2.3s
    std::vector<float> y(kLate + kSpan);
    for (size_t i = 0; i < y.size(); i++)
    {
        if (i == 24000)
            v.SetParam(chompi::ReverbSend::FREEZE, 1.f);
        const float in = i >= 4800 && i < 24000 ? Sine(i, hz) : 0.f;
        float l = 0.f, r = 0.f;
        v.Process(in, in, &l, &r);
        y[i] = l;
    }
    double early_hz, early_spread, late_hz, late_spread;
    TonePitch(y, kEarly, kEarly + kSpan, hz, &early_hz, &early_spread);
    TonePitch(y, kLate, kLate + kSpan, hz, &late_hz, &late_spread);
    double early_sum = 0., late_sum = 0.;
    for (size_t i = 0; i < kSpan; i++)
    {
        early_sum += y[kEarly + i] * y[kEarly + i];
        late_sum += y[kLate + i] * y[kLate + i];
    }
    const double growth = 10. * log10(late_sum / (early_sum + 1e-30));
    const double off = 1200. * log2(early_hz / hz), drift = 1200. * log2(late_hz / early_hz);
    printf("  reverb frozen on %.0fHz: at 1s %.2fHz (%+.1f cents), spread %.2f cents; a minute on %.2fHz "
           "(%+.1f cents), spread %.2f cents; level %+.2fdB\n",
           hz, early_hz, off, early_spread, late_hz, drift, late_spread, growth);
    // measured on main: within 4 cents of 2kHz (a detuning by ear is tens of cents), a swing
    // of 3.1 and 3.7 cents (the tank's modulation at twice the depth, before #313: 4.5 and
    // 5.3). The level fell, ~15dB a minute at 2kHz and the 8kHz tone gone (#394): the tank's
    // modulated reads, interpolated linearly, took a little of the highs on each trip. Frozen,
    // they're allpass-interpolated at half the depth: -0.5dB at 2kHz, -0.8 at 8kHz, a spread
    // of 2-3.5 cents (at full depth 8kHz's spread grew to 6 cents in the minute). With the
    // tank's writes rounded (#419), 0.0dB at both
    Check(fabs(off) < 10. && fabs(drift) < 10., "reverb: frozen, the room holds its pitch, at 1s and a minute on");
    Check(early_spread < 4.5 && late_spread < 4.5, "reverb: frozen, the held room's pitch swings no wider than the tank's modulation");
    Check(growth < 1., "reverb: frozen a minute, the held tone doesn't grow");
    Check(growth > -2., "reverb: frozen a minute, the held tone keeps its highs (#394)");
}

/** #419: what the frozen room holds, 5 minutes on: in from 0.1 to 0.5s (`amp` of noise, or a
 *  500Hz tone of rms `tone`), frozen at `freeze_at`. Its level in dB, a second every 10s (*db,
 *  31), and the last second's 4th difference, the floor above the tone, in dB (*hf). All
 *  exactly 0 from the freeze on: *silent */
static void ReverbHold(float amp, float tone, size_t freeze_at, double* db, double* hf, bool* silent)
{
    static chompi::ReverbSend v;
    v.Init(kSr, &reverb);
    v.SetParam(chompi::ReverbSend::DECAY, .5f);
    v.SetParam(chompi::ReverbSend::LEVEL, kP2Level);
    v.SetOn(true);
    uint32_t seed = 1;
    const size_t kWindow = 48000, kEvery = 480000, kEnd = freeze_at + 48000 + 30 * kEvery + kWindow;
    double sum = 0., diff = 0.;
    float y[5] = {};
    *silent = true;
    for (size_t i = 0; i < kEnd; i++)
    {
        if (i == freeze_at)
            v.SetParam(chompi::ReverbSend::FREEZE, 1.f);
        float in = 0.f;
        if (i >= 4800 && i < 24000)
        {
            seed = seed * 1664525u + 1013904223u;
            in = amp * static_cast<float>(static_cast<int32_t>(seed)) * (1.f / 2147483648.f)
               + tone * 1.41421356f * sinf(2.f * float(M_PI) * 500.f * static_cast<float>(i) / kSr);
        }
        float l = 0.f, r = 0.f;
        v.Process(in, in, &l, &r);
        if (i >= freeze_at && l != 0.f)
            *silent = false;
        if (i < freeze_at + 48000)
            continue;
        const size_t at = (i - freeze_at - 48000) % kEvery;
        if (at >= kWindow)
            continue;
        memmove(y + 1, y, 4 * sizeof(float));
        y[0] = l;
        sum += static_cast<double>(l) * l;
        const double d = y[0] - 4. * y[1] + 6. * y[2] - 4. * y[3] + y[4];
        diff += d * d;
        if (at == kWindow - 1)
        {
            *db++ = 10. * log10(sum / kWindow + 1e-30);
            *hf = 10. * log10(diff / kWindow + 1e-30);
            sum = diff = 0.;
        }
    }
}

/** #419: frozen, quiet material holds (the tank's 16-bit writes rounded, not truncated): noise
 *  at -24dBFS and a tone at -40dBFS keep their level for 5 minutes, the floor of rounding adds
 *  no more than a loud freeze has always had, and a room gone silent stays silent */
static void TestReverbFreezeQuiet()
{
    double noise[31], tone[31], hf, unused;
    bool silent;
    ReverbHold(.063f * sqrtf(3.f), 0.f, 24000, noise, &unused, &silent);
    ReverbHold(0.f, .0141f, 24000, tone, &hf, &silent);
    double noise_worst = 0., tone_worst = 0.;
    for (int k = 0; k < 31; k++)
    {
        noise_worst = fmax(noise_worst, fabs(noise[k] - noise[0]));
        tone_worst = fmax(tone_worst, fabs(tone[k] - tone[0]));
    }
    printf("  reverb frozen 5 min: noise %.1fdB, %+.2fdB at 1 min, %+.2fdB at 5 (worst %.2f); "
           "a tone %.1fdB, %+.2fdB at 5 min (worst %.2f), its floor %.1fdB\n",
           noise[0], noise[6] - noise[0], noise[30] - noise[0], noise_worst, tone[0],
           tone[30] - tone[0], tone_worst, hf);
    // measured on main (truncating): the noise -1.5dB at 30s and gone (exactly 0) at 3.5
    // minutes, the tone gone within 90s. Rounded: -0.3dB in 5 minutes, both. The rounding's
    // errors add up in a tank that loses nothing, a floor rising 3dB as the hold doubles (8 to
    // 20kHz -60dBFS at 5 minutes, -53 at 30); main had it too, under material loud enough not
    // to fade: a -8dBFS tone's floor there, 5 minutes on, is this check's -39.8dB
    Check(noise_worst < 1. && tone_worst < 1., "reverb: frozen 5 minutes, quiet noise and a quiet tone hold their level (#419)");
    Check(hf < -39.8 + 1., "reverb: frozen 5 minutes, the floor under a quiet tone no higher than main's under a loud one");
    ReverbHold(.063f * sqrtf(3.f), 0.f, 24000 + 6 * 48000, tone, &hf, &silent);
    Check(silent, "reverb: a room rung out to silence and frozen stays silent for 5 minutes");
}

/** An 8th-order highpass at 4kHz (4 RBJ biquads, Q .707): what's above a
 *  1kHz tone, which it takes ~90dB down */
struct Highpass
{
    double b0, b1, b2, a1, a2;
    double z[4][2] = {};
    Highpass()
    {
        const double w = 2. * M_PI * 4000. / kSr, c = cos(w), al = sin(w) / (2. * .7071), a0 = 1. + al;
        b0 = (1. + c) / 2. / a0;
        b1 = -(1. + c) / a0;
        b2 = b0;
        a1 = -2. * c / a0;
        a2 = (1. - al) / a0;
    }
    double Process(double x)
    {
        for (auto& s : z)
        {
            const double y = b0 * x + s[0];
            s[0] = b1 * x - a1 * y + s[1];
            s[1] = b2 * x - a2 * y;
            x = y;
        }
        return x;
    }
};

/** A 1kHz tone into the reverb, the pre-delay at `from`, turned to `to` at 1s (or not, from
 *  == to): the room's energy above 4kHz over the next half second against all of it, in dB */
static float PreDelayTurn(float from, float to, bool recall)
{
    static chompi::ReverbSend v;
    v.Init(kSr, &reverb);
    v.SetParam(chompi::ReverbSend::DECAY, .5f);
    v.SetParam(chompi::ReverbSend::LEVEL, kP2Level);
    v.SetParam(chompi::ReverbSend::PRE_DELAY, from);
    v.SetOn(true);
    Highpass hp;
    double hi = 0., all = 0.;
    for (size_t i = 0; i < 72000; i++)
    {
        if (i == 48000)
        {
            // a scene recall's or a morph's faster slew (FxChain::FastSlew)
            if (recall)
                FxSlew::coeff = kFxRecallCoeff;
            v.SetParam(chompi::ReverbSend::PRE_DELAY, to);
        }
        float l = 0.f, r = 0.f;
        const float in = Sine(i, 1000.f);
        v.Process(in, in, &l, &r);
        const double h = hp.Process(l);
        if (i >= 48000)
        {
            hi += h * h;
            all += static_cast<double>(l) * l;
        }
    }
    FxSlew::coeff = kFxParamCoeff;
    return static_cast<float>(10. * log10(hi / all + 1e-30));
}

/** The pre-delay turned (a knob, MIDI, a recall): it used to slide its read head through the
 *  buffer, up to 12 frames a sample (48 after a recall), a sped-up and reversed burst into
 *  the room, a chirp; a 1kHz tone came out with highs ~0dB against it. It crossfades to the
 *  new time instead: no more highs than the room has without the turn */
static void TestPreDelayTurn()
{
    const float still = PreDelayTurn(.5f, .5f, false);
    const float shorter = PreDelayTurn(.5f, 0.f, false), longer = PreDelayTurn(.5f, 1.f, false);
    const float recalled = fmaxf(PreDelayTurn(1.f, .1f, true), PreDelayTurn(.1f, 1.f, true));
    printf("  reverb, 1 kHz, above 4 kHz: %.1f dB still, turned shorter %.1f, longer %.1f, recalled %.1f\n",
           still, shorter, longer, recalled);
    Check(fmaxf(shorter, longer) < still + 6.f && recalled < still + 6.f,
          "reverb: the pre-delay turned, no chirp: no more highs than without the turn");
}

static void TestReverb()
{
    const float fades = ReverbRun(0.f, 0.f, 0.f, 200000, 240000);
    const float held = ReverbRun(1.f, 0.f, 0.f, 200000, 240000);
    printf("  reverb 4 s on: plain %.5f, frozen %.5f\n", fades, held);
    Check(held > 20.f * fades && held > 1e-3f, "reverb: freeze holds the room");
    {
        // held a minute, the room doesn't grow: the smear's read stays inside its allpass (read
        // as 10 +- 80, Beads' numbers taken for FRIZZ's -1..1 LFO, it reached into del2's past,
        // a second feedback path, and the frozen room rose ~16dB to the clip)
        const float early = ReverbRun(1.f, 0.f, 0.f, 48000, 96000);
        const float late = ReverbRun(1.f, 0.f, 0.f, 24000 + 60 * 48000, 24000 + 61 * 48000);
        printf("  reverb frozen: %.5f at 1 s, %.5f a minute on\n", early, late);
        Check(late > 1e-3f && late < early * 1.12f, "reverb: frozen a minute, the room doesn't grow");
    }
    size_t plain_on = 0, pre_on = 0;
    ReverbRun(0.f, 0.f, 0.f, 0, 30000, false, &plain_on);
    ReverbRun(0.f, 1.f, 0.f, 0, 30000, false, &pre_on);
    printf("  reverb onset: %zu, with 250ms pre-delay %zu\n", plain_on, pre_on);
    Check(pre_on > plain_on + 11000 && pre_on < plain_on + 13000, "reverb: pre-delay at the top: 250ms later");
    {
        // turned back to 0, the pre-delay switches off: its slew lands on 0 instead of
        // stalling on a denormal above it, the SDRAM written and read for good (#167)
        static chompi::ReverbSend v;
        v.Init(kSr, &reverb);
        v.SetOn(true);
        v.SetParam(chompi::ReverbSend::PRE_DELAY, .5f);
        float l = 0.f, r = 0.f;
        for (size_t i = 0; i < 48000; i++)
            v.Process(Sine(i), Sine(i), &l, &r);
        const bool up = v.PreDelaying();
        v.SetParam(chompi::ReverbSend::PRE_DELAY, 0.f);
        for (size_t i = 0; i < 96000; i++)
            v.Process(0.f, 0.f, &l, &r);
        Check(up && !v.PreDelaying(), "reverb: the pre-delay turned back to 0 switches off");
    }
    const float free = ReverbRun(0.f, 0.f, 0.f, 24000, 48000, true);
    const float ducked = ReverbRun(0.f, 0.f, 1.f, 24000, 48000, true);
    Check(ducked < free * .3f, "reverb: ducking turns the room down under the input");
}

// ======== the scene file: layout 3, and 4 (#117) ========

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
    Check(s[0].params[FX_FOLDER][3] == .4f && s[0].params[FX_FOLDER][5] == 0.f
              && s[0].params[FX_CRUSHER][3] == .3f && s[0].params[FX_CRUSHER][5] == 0.f
              && s[0].params[FX_FILTER][3] == .5f && s[0].params[FX_FILTER][5] == 0.f,
          "layout: v0.11's symmetry, XOR and LFO division stay on page 1's knob 4, stereo off");
    Check(s[0].params[FX_TAPESTOP][3] == 1.f, "layout: the tape stop's depth a full stop");
    Check(s[1].params[FX_DELAY][4] == 0.f && s[1].params[FX_DELAY][5] == .5f
              && s[1].params[FX_DELAY][3] == 0.f
              && s[1].params[FX_DELAY][7] == LevelKnob(.3f),
          "layout 2: a send's page 2 on its defaults (not frozen), page 1 kept (its level, a "
          "gain of .3, on page 2's knob 4 in dB since layout 5, its ducking off)");
    Check(s[1].params[FX_FOLDER][7] == .5f && s[1].params[FX_FOLDER][4] == 1.f
              && s[1].params[FX_FOLDER][3] == .4f && s[1].params[FX_FOLDER][5] == 0.f,
          "layout 2: an insert's Mix and Level kept, its symmetry on page 1's knob 4");
    char buf[kSceneFileMax];
    FormatScenes(s, buf, sizeof(buf));
    FxScene back[kNumScenes];
    Check(strstr(buf, "layout 5\n") && ParseScenes(buf, defaults, back)
              && back[1].params[FX_FOLDER][3] == .4f && back[0].params[FX_FOLDER][5] == 0.f
              && fabsf(back[1].params[FX_DELAY][7] - LevelKnob(.3f)) < 1e-6f,
          "layout 5: written, and read back as written");
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
    chain.SetParam(chompi::FX_DELAY, chompi::FxOutput::kLevel, kP2Level); // its level, 0dB
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
    TestPreDelayTurn();
    TestReverbFreezePitch(2000.f);
    TestReverbFreezePitch(8000.f);
    TestReverbFreezeQuiet();
    TestLayout();
    TestKeyOnPage2();
    TestKeyOnBand();
    TestKeyOnMeter();
    return Finish();
}
