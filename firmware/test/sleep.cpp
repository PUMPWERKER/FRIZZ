// sleep.cpp: checks that an effect switched off stops costing time without changing what's
// heard (FxGate::Asleep, TailWatch in FxCommon.h): an insert off and faded out passes its
// input exactly, a send sleeps only once its tail has rung out, and both come back on their
// key; the filter's and the phaser's tempo LFO wake where one that never slept is, after a
// long sleep with the tempo moving. Exits 0 when everything passes. Run by unit.sh sleep.
#include <cmath>
#include <cstdio>
#include "check.h"
// the resonator's follower, to see where it wakes
#define private public
#include "FxChain.h"
#undef private

using namespace chompi;

static const float kSr = 48000.f;

static float Sine(long n) { return .3f * sinf(2.f * 3.14159265f * 220.f * n / kSr); }

/** An insert, on for a while and then off: once faded out its output is its input, bit for
 *  bit, and it's Idle; on again, it shapes the sound again */
template <class Fx>
static void TestInsert(Fx& fx, const char* name)
{
    fx.Init(kSr);
    for (size_t p = 0; p < kNumFxParams; p++)
        fx.SetParam(p, .7f);
    fx.SetOn(true);
    long n = 0;
    for (; n < 24000; n++)
    {
        float l = Sine(n), r = Sine(n);
        fx.Process(&l, &r);
    }
    fx.SetOn(false);
    bool exact = true;
    for (long end = n + 48000; n < end; n++)
    {
        float l = Sine(n), r = Sine(n);
        const float in = l;
        fx.Process(&l, &r);
        if (n > end - 24000)
            exact = exact && l == in && r == in;
    }
    char what[96];
    snprintf(what, sizeof what, "%s: off, its output is its input exactly", name);
    Check(exact && fx.Idle(), what);

    fx.SetOn(true);
    bool shaped = false;
    for (long end = n + 24000; n < end; n++)
    {
        float l = Sine(n), r = Sine(n);
        const float in = l;
        fx.Process(&l, &r);
        shaped = shaped || fabsf(l - in) > 1e-3f;
    }
    snprintf(what, sizeof what, "%s: on again, it shapes the sound", name);
    Check(shaped && !fx.Idle(), what);
}

/** The filter's or the phaser's tempo LFO (FxTempoLfo.h), counted while asleep and caught up
 *  on waking: 10s asleep while the clock's pulses come unevenly (±1.5%), its estimate of their
 *  spacing wobbles (±0.5%) and the tempo moves from 120 to 130 BPM, then woken: its LFO where
 *  the same effect's is that stayed on all along. Caught up at the tempo it woke at, the whole
 *  sleep, it was off by up to a pulse */
template <class Fx, class Cycle>
static void TestLfoSleep(const char* name, Fx& a, Fx& b, Cycle cycle)
{
    a.SetOn(true);
    b.SetOn(true);
    uint32_t pos = 0;
    double next = 0.;
    long n = 0;
    const long sleep_at = 24000, wake_at = sleep_at + 480000, end = wake_at + 24;
    for (; n < end; n += 24)
    {
        if (n == sleep_at)
            a.SetOn(false);
        if (n == wake_at)
            a.SetOn(true);
        const float t = fminf(fmaxf(static_cast<float>(n - sleep_at) / 480000.f, 0.f), 1.f);
        const float spacing = 60.f * kSr / ((120.f + 10.f * t) * 12.f);
        // the clock's estimate of it wobbling from block to block, as a MIDI clock's does
        const float estimate = spacing * ((n / 24) % 2 ? 1.005f : .995f);
        a.SetPulseSamples(estimate);
        b.SetPulseSamples(estimate);
        while (next < n + 24)
        {
            a.ClockPulse(pos);
            b.ClockPulse(pos);
            pos++;
            next += spacing * (pos % 2 ? 1.015 : .985);
        }
        for (long i = n; i < n + 24; i++)
        {
            float l = Sine(i), r = l, l2 = l, r2 = l;
            a.Process(&l, &r);
            b.Process(&l2, &r2);
        }
    }
    float apart = fabsf(cycle(a) - cycle(b));
    apart = fminf(apart, 1.f - apart);
    printf("      %s: woken after 10s, its LFO %.4f of a cycle from one that never slept\n", name, apart);
    char what[112];
    snprintf(what, sizeof what, "%s: its LFO wakes where it would be, the tempo moved while it slept", name);
    Check(a.Idle() == false && apart < .01f, what);
}

static void TestLfoSleeps()
{
    static chompi::Filter fa, fb;
    for (chompi::Filter* f : {&fa, &fb})
    {
        f->Init(kSr);
        f->SetParam(chompi::Filter::LFO_DEPTH, 1.f);
        f->SetParam(chompi::Filter::LFO_DIVISION, 0.f); // 1/16: a pulse is a third of its cycle
    }
    TestLfoSleep("filter", fa, fb, [](chompi::Filter& f) { return f.lfo_.Cycle(f.lfo_div_pulses_); });
    static chompi::Phaser pa, pb;
    for (chompi::Phaser* p : {&pa, &pb})
    {
        p->Init(kSr);
        p->SetParam(chompi::Phaser::RATE, SplitStepValue(0.f, chompi::Phaser::kNumDivisions)); // synced, 1/16
    }
    TestLfoSleep("phaser", pa, pb, [](chompi::Phaser& p) { return p.lfo_.Cycle(p.div_pulses_); });
}

/** PressEnvelope::Skip, what an effect that slept catches up on (the slicer), against the
 *  same samples one by one: pressed before it slept, in its attack, in its decay */
static void TestSkip()
{
    const float inc = 1.f / 480.f, decay = Decay60dBCoeff(.3f, kSr);
    bool close = true;
    for (uint32_t before : {0u, 100u, 2000u})
        for (uint32_t n : {1u, 50u, 479u, 480u, 5000u, 48000u})
        {
            PressEnvelope a, b;
            a.Press();
            b.Press();
            for (uint32_t i = 0; i < before; i++)
            {
                a.Process(inc, decay);
                b.Process(inc, decay);
            }
            for (uint32_t i = 0; i < n; i++)
                a.Process(inc, decay);
            b.Skip(n, inc, decay);
            close = close && fabsf(a.value - b.value) < 1e-4f && a.attacking == b.attacking;
        }
    Check(close, "PressEnvelope::Skip: as many samples of Process, within 1e-4");
}

/** The tape stop, off and on the live signal: it doesn't write its buffer (in SDRAM on the
 *  device) at all, and a press still slows from the sound just before it. The buffer is
 *  filled with a value no input has, so a read of a frame it didn't record shows */
static void TestTapeStop()
{
    static const size_t kFrames = 1u << 14;
    static float buf_l[kFrames], buf_r[kFrames];
    static const float kStale = 1000.f;
    for (size_t i = 0; i < kFrames; i++)
        buf_l[i] = buf_r[i] = kStale;
    static TapeStop t;
    t.Init(kSr, buf_l, buf_r, kFrames);
    t.SetParam(TapeStop::DEPTH, 0.f); // half speed, so it plays on rather than going silent
    long n = 0;
    bool exact = true;
    for (; n < 24000; n++)
    {
        float l = Sine(n), r = Sine(n);
        const float in = l;
        t.Process(&l, &r);
        exact = exact && l == in && r == in;
    }
    bool untouched = true;
    for (size_t i = 0; i < kFrames; i++)
        untouched = untouched && buf_l[i] == kStale && buf_r[i] == kStale;
    Check(exact && untouched, "tape stop: off, its output is its input and its buffer untouched");

    t.SetOn(true);
    bool recorded = true, slowed = false;
    for (long end = n + 12000; n < end; n++)
    {
        float l = Sine(n), r = Sine(n);
        const float in = l;
        t.Process(&l, &r);
        recorded = recorded && fabsf(l) < 1.f && fabsf(r) < 1.f;
        slowed = slowed || fabsf(l - in) > 1e-3f;
    }
    Check(recorded && slowed, "tape stop: pressed, it slows what it recorded from the press on");
}

/** The resonator, whose loop wraps the inserts (Feed before them, Tap after): off and faded
 *  out, it passes its input exactly; on again, it rings */
static void TestResonator()
{
    static chompi::Resonator reso;
    reso.Init(kSr);
    reso.SetParam(chompi::Resonator::FEEDBACK, .9f);
    reso.SetOn(true);
    long n = 0;
    const auto Run = [&](long samples, bool* exact, bool* rings) {
        for (long end = n + samples; n < end; n++)
        {
            float l = Sine(n), r = Sine(n);
            const float in = l;
            reso.Feed(&l, &r);
            reso.Tap(l, r);
            if (exact && n > end - 24000)
                *exact = *exact && l == in && r == in;
            if (rings)
                *rings = *rings || fabsf(l - in) > 1e-3f;
        }
    };
    Run(24000, nullptr, nullptr);
    reso.SetOn(false);
    bool exact = true;
    Run(48000, &exact, nullptr);
    Check(exact && reso.Idle(), "resonator: off, its output is its input exactly");
    reso.SetOn(true);
    bool rings = false;
    Run(24000, nullptr, &rings);
    Check(rings && !reso.Idle(), "resonator: on again, it rings");
}

/** The resonator with env mod up, loud, then off and asleep: punched in again on a quiet
 *  input, its follower starts from silence, not from the loud level it had, which pushed the
 *  feedback up for ~100ms (#167) */
static void TestResonatorEnv()
{
    static chompi::Resonator reso;
    reso.Init(kSr);
    reso.SetParam(chompi::Resonator::FEEDBACK, .3f);
    reso.SetParam(chompi::Resonator::ENV_MOD, 1.f);
    reso.SetOn(true);
    long n = 0;
    const auto Run = [&](long samples, float gain) {
        for (long end = n + samples; n < end; n++)
        {
            float l = 3.f * gain * Sine(n), r = l;
            reso.Feed(&l, &r);
            reso.Tap(l, r);
        }
    };
    Run(24000, 1.f);
    const float loud = reso.env_;
    reso.SetOn(false);
    Run(48000, 1.f);
    const bool asleep = reso.Idle();
    reso.SetOn(true);
    Run(1, .01f);
    printf("      resonator's env mod: %.3f loud, %.5f on waking on a quiet input\n", loud,
           reso.env_);
    Check(asleep && loud > .5f && reso.env_ < .01f,
          "resonator: punched in after a sleep, its env mod starts from silence");
}

/** A Smoothed's Settle lands on the target exactly, so what waits on it (the filter's
 *  resonance, the delay's feedback, page 2, the compressor, the reverb's pre-delay) comes to
 *  rest: up to 1, where a step falls below a float's resolution, down to 0, through the
 *  denormals, and between; at the knobs' slew and a recall's. Process doesn't, as on main:
 *  landing there cost a morph too much (#167, finding 6) */
static void TestSmoothed()
{
    struct Move
    {
        float from, to, coeff;
    } moves[] = {
        {0.f, 1.f, chompi::kFxParamCoeff},   {1.f, 0.f, chompi::kFxParamCoeff},
        {.3f, .7f, chompi::kFxParamCoeff},   {0.f, 1.f, chompi::kFxRecallCoeff},
        {1.f, 0.f, chompi::kFxRecallCoeff},  {2400.f, 480.f, chompi::kFxParamCoeff},
        {1e-4f, 0.f, chompi::kFxParamCoeff},
    };
    bool all = true;
    for (const Move& m : moves)
    {
        chompi::Smoothed s;
        s.Reset(m.from);
        s.target = m.to;
        long at = -1;
        for (long i = 0; i < 10 * 48000 && at < 0; i++)
            if (!s.Settle(m.coeff) && s.value == m.to)
                at = i;
        printf("      Smoothed::Settle %g to %g at %g: at rest after %ld samples\n", m.from, m.to,
               m.coeff, at);
        all = all && at > 0 && at < 48000;
    }
    Check(all, "Smoothed: Settle lands on its target exactly, within a second");
}

static float delay_mem[480000 * 2];

/** The delay: off, it keeps ringing out; it sleeps only after its whole buffer of silence,
 *  and wakes on its key */
static void TestDelay()
{
    static DelaySend delay;
    const size_t frames = 48000; // a 1s buffer, so the test is short
    delay.Init(48000.f, delay_mem, frames);
    delay.SetTempo(120.f);
    delay.SetParam(DelaySend::DIVISION, .25f);
    delay.SetParam(DelaySend::FEEDBACK, .3f);
    delay.SetParam(DelaySend::LEVEL, FxOutput::kLevelDefault);
    delay.SetOn(true);
    long n = 0;
    for (; n < 4800; n++)
    {
        float l = 0.f, r = 0.f;
        delay.Process(Sine(n), Sine(n), &l, &r);
    }
    delay.SetOn(false);
    bool rang = false;
    for (long end = n + 48000; n < end; n++) // the 1/4 at 120 BPM comes back after 0.5s
    {
        float l = 0.f, r = 0.f;
        delay.Process(Sine(n), Sine(n), &l, &r);
        rang = rang || fabsf(l) > 1e-3f;
    }
    Check(rang && !delay.Sleeping(), "delay: off, its echoes ring out");

    long slept_at = -1;
    for (long end = n + 20 * 48000; n < end && slept_at < 0; n++)
    {
        float l = 0.f, r = 0.f;
        delay.Process(Sine(n), Sine(n), &l, &r);
        if (delay.Sleeping())
            slept_at = n;
    }
    Check(slept_at >= 0, "delay: once rung out, it sleeps");

    // asleep it adds nothing
    float l = 0.f, r = 0.f;
    delay.Process(Sine(n), Sine(n), &l, &r);
    n++;
    Check(l == 0.f && r == 0.f, "delay: asleep, nothing added");

    delay.SetOn(true);
    bool echo = false;
    for (long end = n + 48000; n < end; n++)
    {
        float l = 0.f, r = 0.f;
        delay.Process(Sine(n), Sine(n), &l, &r);
        echo = echo || fabsf(l) > 1e-2f;
    }
    Check(echo && !delay.Sleeping(), "delay: on again, it echoes");
}

/** The reverb: the same, after 2s of silence */
static void TestReverb()
{
    static daisysp::Reverb engine;
    static ReverbSend reverb;
    reverb.Init(kSr, &engine);
    reverb.SetParam(ReverbSend::DECAY, .5f);
    reverb.SetParam(ReverbSend::LEVEL, FxOutput::kLevelDefault);
    reverb.SetOn(true);
    long n = 0;
    for (; n < 4800; n++)
    {
        float l = 0.f, r = 0.f;
        reverb.Process(Sine(n), Sine(n), &l, &r);
    }
    reverb.SetOn(false);
    bool rang = false;
    for (long end = n + 4800; n < end; n++)
    {
        float l = 0.f, r = 0.f;
        reverb.Process(Sine(n), Sine(n), &l, &r);
        rang = rang || fabsf(l) > 1e-3f;
    }
    Check(rang && !reverb.Sleeping(), "reverb: off, its tail rings out");

    long slept_at = -1;
    for (long end = n + 60 * 48000; n < end && slept_at < 0; n++)
    {
        float l = 0.f, r = 0.f;
        reverb.Process(Sine(n), Sine(n), &l, &r);
        if (reverb.Sleeping())
            slept_at = n;
    }
    Check(slept_at >= 0, "reverb: once rung out, it sleeps");

    reverb.SetOn(true);
    bool wet = false;
    for (long end = n + 24000; n < end; n++)
    {
        float l = 0.f, r = 0.f;
        reverb.Process(Sine(n), Sine(n), &l, &r);
        wet = wet || fabsf(l) > 1e-2f;
    }
    Check(wet && !reverb.Sleeping(), "reverb: on again, it reverberates");
}

int main()
{
    static chompi::Shifter shifter;
    static chompi::Flanger flanger;
    static chompi::Warble warble;
    static chompi::Crusher crusher;
    static chompi::Filter filter;
    static chompi::Slicer slicer;
    TestInsert(shifter, "shifter");
    TestInsert(flanger, "flanger");
    TestInsert(warble, "warble");
    TestInsert(crusher, "crusher");
    TestInsert(filter, "filter");
    TestInsert(slicer, "slicer");
    TestSkip();
    TestLfoSleeps();
    TestTapeStop();
    TestResonator();
    TestResonatorEnv();
    TestSmoothed();
    TestDelay();
    TestReverb();
    return Finish();
}
