// tempo.cpp: checks the FX's tempo on the host: tap tempo (TapTempo.h), fitting and guessing a
// loop's beats, the tempo clock locked to a loop (TempoClock.h) and a quantized loop's beats
// (Looper.h), with a faked MIDI clock where it matters, and a scene morph landing on the
// clock's bar lines (FxMorph.h). Exits 0 when everything passes. Run by unit.sh tempo.
#include <cmath>
#include <cstdio>
#include <functional>
#include "check.h"
#include <vector>
#include "FxMorph.h"
#include "granularDelay.h"
#include "Looper.h"
#include "TapTempo.h"
#include "TempoClock.h"

using namespace chompi;


static const float kSr = 48000.f;
static const size_t kBlock = 24;

static void TestTaps()
{
    TapTempo tap;
    Check(!tap.Tap(1000) && !tap.Tap(1500), "taps: none from 1 or 2 taps");
    Check(tap.Tap(2000) && fabsf(tap.Bpm() - 120.f) < .01f, "taps: 3 at 0.5s give 120 BPM");
    tap.Tap(2600);
    Check(fabsf(tap.Bpm() - 60000.f / (1600.f / 3.f)) < .01f, "taps: the mean of the intervals");
    for (uint32_t t = 3000; t <= 6000; t += 400)
        tap.Tap(t);
    Check(fabsf(tap.Bpm() - 150.f) < .01f, "taps: only the last 4 intervals count");
    Check(!tap.Tap(9000), "taps: a gap over 2s starts over");
    tap.Tap(9300);
    Check(tap.Tap(9600) && fabsf(tap.Bpm() - 200.f) < .01f, "taps: and counts again from there");
}

static void TestFit()
{
    Check(GuessBeats(2 * 48000, kSr) == 4, "guess: 2s is 4 beats, 120 BPM");
    Check(GuessBeats(3 * 48000, kSr) == 4, "guess: 3s is 4 beats, 80 BPM");
    Check(GuessBeats(static_cast<size_t>(3.1f * 48000), kSr) == 8, "guess: 3.1s is 8 beats, 155 BPM");
    Check(GuessBeats(16 * 48000, kSr) == 32, "guess: 16s is 32 beats, 120 BPM");
    Check(GuessBeats(24000, kSr) == 1, "guess: a short loop is one beat");
    Check(FitBeats(static_cast<size_t>(1.5f * 48000), kSr, 120.f) == 3, "fit: 1.5s at 120 is 3 beats");
    Check(FitBeats(2 * 48000, kSr, 63.f) == 2, "fit: 2s near 60 is 2 beats");
    Check(FitBeats(2 * 48000, kSr, 10.f) == 1, "fit: at least 1 beat");
}

/** The pulse positions a block gives */
static std::vector<uint32_t> Pulses(TempoClock& clock, float pos, float speed, bool paused = false)
{
    std::vector<uint32_t> out;
    const uint32_t n = clock.Process(kBlock, pos, speed, paused);
    for (uint32_t i = 0; i < n; i++)
        out.push_back(clock.Pulse());
    return out;
}

static void TestLoopClock()
{
    MidiClock midi;
    TempoClock clock;
    clock.Init(kSr, &midi);
    Check(!clock.TempoSet() && clock.GetTempo() == kDefaultBpm, "free: 120 BPM, no tempo set");

    // a 2s loop of 4 beats: 48 pulses a pass, 120 BPM
    const size_t length = 2 * 48000;
    clock.SetLoop(length, 4);
    std::vector<uint32_t> p = Pulses(clock, 0.f, 1.f);
    Check(p.size() == 1 && p[0] == 0, "loop: beat 1 on the loop's start");
    Check(clock.GetTempo() == 120, "loop: its tempo");

    // one pass, a block at a time
    uint32_t count = 0, last = 0;
    bool ascending = true;
    for (size_t f = kBlock; f < length; f += kBlock)
        for (uint32_t pos : Pulses(clock, static_cast<float>(f) / length, 1.f))
        {
            ascending = ascending && pos == last + 1;
            last = pos;
            count++;
        }
    Check(count == 47 && last == 47 && ascending, "loop: 48 pulses a pass, counting up");
    p = Pulses(clock, 0.f, 1.f);
    Check(p.size() == 1 && p[0] == 0, "loop: the next pass starts on 0 again");

    // half speed: half the tempo; reverse: counting down across the loop point
    Pulses(clock, 0.f, .5f);
    Check(clock.GetTempo() == 60, "loop: half speed, half the tempo");
    p = Pulses(clock, .99f, -1.f);
    Check(p.size() == 1 && p[0] == 0 && clock.Reverse(), "loop: reverse, beat 1 as it crosses the loop point");
    p = Pulses(clock, 46.5f / 48.f, -1.f);
    Check(p.size() == 1 && p[0] == 47, "loop: reverse, each pulse where forward has it");
    p = Pulses(clock, 47.5f / 48.f, 1.f);
    Check(p.size() == 1 && p[0] == 47, "loop: and forward again over the same spot, the same pulse");
    Check(Pulses(clock, 47.5f / 48.f, -1.f).empty(), "loop: standing still, no pulses");
    Check(clock.GetTempo() == 120, "loop: reverse, the tempo of the speed's size");

    // paused at 10.5 pulses in: the grid runs on at the loop's tempo, 24 pulses a second,
    // counting up from where it stopped; scrubbing doesn't move it
    clock.SetLoop(length, 4);
    Pulses(clock, 0.f, 1.f);
    Pulses(clock, 10.5f / 48.f, 1.f);
    std::vector<uint32_t> paused;
    for (size_t f = 0; f < 48000; f += kBlock)
        for (uint32_t pos : Pulses(clock, (10.5f + f / 48000.f) / 48.f, 1.f, true))
            paused.push_back(pos);
    bool on = paused.size() >= 23 && paused.size() <= 25 && paused[0] == 11;
    for (size_t i = 1; on && i < paused.size(); i++)
        on = paused[i] == (paused[i - 1] + 1) % 48;
    Check(on && !clock.Reverse(), "paused: the grid runs on at the tempo, from where it stopped");
    Check(clock.GetTempo() == 120, "paused: the loop's tempo");
    p = Pulses(clock, 30.5f / 48.f, 1.f);
    Check(p.empty(), "resumed: no burst of pulses for the jump");
    p = Pulses(clock, 31.2f / 48.f, 1.f);
    Check(p.size() == 1 && p[0] == 31, "resumed: the next pulse on the loop's position");

    // a tap at double tempo: 8 beats, 96 pulses a pass
    clock.Tap(235.f);
    Check(clock.TempoSet(), "loop tap: a tempo is set");
    Pulses(clock, 0.f, 1.f);
    Check(clock.GetTempo() == 240, "loop tap: the loop refitted to 8 beats, 240 BPM");
    p = Pulses(clock, 1.f / 96.f + 1e-4f, 1.f);
    Check(p.size() == 1 && p[0] == 1, "loop tap: 96 pulses a pass");

    // a long loop: the position still wraps at the cycle
    clock.SetLoop(length, 4);
    clock.ClearLoop();
    Check(!clock.HasLoop() && clock.GetTempo() == 120, "erased: free at the loop's tempo");
    clock.SetLoop(16 * 48000, 32);
    Pulses(clock, 0.f, 1.f);
    Pulses(clock, 100.5f / 384.f, 1.f); // under half a pass per block, as any real block
    p = Pulses(clock, 200.5f / 384.f, 1.f);
    Check(!p.empty() && p.back() == 200 % kPulsesPerCycle, "loop: positions past the cycle wrap");
    clock.ClearLoop();

    // free running: a tap sets the tempo and puts a beat on it
    clock.Tap(90.f);
    Check(clock.GetTempo() == 90, "free tap: the tempo");
    p = Pulses(clock, 0.f, 1.f);
    Check(p.size() == 1 && p[0] % kPulsesPerBeat == 0, "free tap: a beat on the tap");
    clock.Tap(500.f);
    Check(clock.GetTempo() == kMaxBpm, "free tap: clamped to 300");
}

static void TestMidiClock()
{
    MidiClock midi;
    TempoClock clock;
    clock.Init(kSr, &midi);
    midi.has_clock = true;
    midi.bpm = 100.f;
    clock.Process(kBlock);
    Check(clock.GetTempo() == 100 && clock.TempoSet(), "clock: its tempo, and a tempo is set");
    Check(!clock.CanTap(), "clock, no loop: taps refused");
    clock.Tap(150.f);
    Check(clock.GetTempo() == 100, "clock, no loop: a tap changes nothing");

    // a loop overrides the clock, and its taps count
    clock.SetLoop(3 * 48000, 4);
    Pulses(clock, 0.f, 1.f);
    Check(clock.GetTempo() == 80 && clock.CanTap(), "loop over the clock: the loop's tempo, taps taken");
    midi.bpm = 130.f;
    midi.ticks = 48;
    Check(Pulses(clock, 0.f, 1.f).empty(), "loop over the clock: clock ticks give no pulses");
    Check(clock.GetTempo() == 80, "loop over the clock: a clock tempo change doesn't follow");
    clock.ClearLoop();
    clock.Process(kBlock);
    Check(clock.GetTempo() == 130, "erased: the clock again");
    midi.ticks = 52;
    Check(clock.Process(kBlock) == 2, "erased: counting the clock from where it was");
}

/** A 240 BPM clock to a looper: a tick every 500 samples */
static void TestQuantizedBeats()
{
    static int16_t mem[kLoopMemSize];
    MidiClock midi;
    chompi::Looper looper;
    looper.Init(mem, &midi);
    const float period = 500.f;
    midi.has_clock = true;
    midi.tick_period = period;
    midi.bpm = kSr * 60.f / (period * kTicksPerBeat);

    float in[kBlock] = {}, out_l[kBlock], out_r[kBlock];
    size_t now = 0;
    auto block = [&]() {
        now += kBlock;
        midi.ticks = static_cast<uint32_t>(now / period);
        midi.last_tick_time = static_cast<uint32_t>(midi.ticks * period);
        looper.Process(in, in, out_l, out_r, kBlock);
    };

    looper.StartRecording(true);
    block();
    // 1.5 bars (a bar is 96 ticks, 48000 samples), then stop: it records to the end of bar 2
    while (now < 72000)
        block();
    looper.StopRecording();
    while (looper.GetState() == chompi::Looper::State::RECORDING && now < 200000)
        block();
    Check(looper.GetState() == chompi::Looper::State::PLAYING && looper.GetBeats() == 8,
          "quantized: 2 bars, 8 beats");
    Check(looper.GetLength() > 95000 && looper.GetLength() < 97000, "quantized: 2 bars long");

    looper.Erase();
    for (int i = 0; i < 2000; i++)
        block();
    looper.StartRecording(false);
    for (int i = 0; i < 100; i++)
        block();
    looper.StopRecording();
    block();
    Check(looper.GetState() == chompi::Looper::State::PLAYING && looper.GetBeats() == 0,
          "unquantized: no beats known");

    // stopped within its first beat just as the clock locks again (no period yet): kept, at
    // its length, instead of closing on a bar of length 0 and coming out empty
    looper.Erase();
    for (int i = 0; i < 2000; i++)
        block();
    looper.StartRecording(true);
    for (int i = 0; i < 200; i++) // 4800 samples, under a beat
        block();
    midi.tick_period = 0.f;
    looper.StopRecording();
    block();
    Check(looper.GetState() == chompi::Looper::State::PLAYING && looper.GetLength() > 4000,
          "quantized, stopped as the clock locks again: the recording is kept");
    midi.tick_period = period;
}

/** A faked clock into a looper, as MidiClock delivers it: each tick counted in the block it
 *  came in and timed at that block's start. period(k) gives tick k's spacing in samples,
 *  delay(k) how late it arrives (a USB frame, a sender's jitter) */
struct FakedClock
{
    MidiClock midi;
    chompi::Looper looper;
    size_t now = 0;
    double next = 0.;     // the next tick's ideal time
    uint32_t k = 0;       // ticks sent
    std::function<double(uint32_t)> period, delay;
    float in[kBlock] = {}, out_l[kBlock], out_r[kBlock];

    void Init(int16_t* mem, double first_period)
    {
        looper.Init(mem, &midi);
        midi.has_clock = true;
        midi.locks = 1;
        midi.tick_period = static_cast<float>(first_period);
        midi.bpm = kSr * 60.f / (midi.tick_period * kTicksPerBeat);
    }
    void Block()
    {
        now += kBlock;
        // the ticks that arrived before this block's start
        while (next + delay(k) < now)
        {
            midi.ticks++;
            midi.last_tick_time = static_cast<uint32_t>(now);
            next += period(k);
            k++;
        }
        looper.Process(in, in, out_l, out_r, kBlock);
    }
    void Until(size_t t)
    {
        while (now < t)
            Block();
    }
};

/** The looper's tick period (Looper.h, LOOPER.md 2.2): a line through every tick, kept up to
 *  where the loop closes */
static void TestQuantizedFit()
{
    static int16_t mem[kLoopMemSize];
    uint32_t seed = 7;
    auto rnd = [&seed]() { // 0..1
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) / double(1 << 24);
    };

    // a DAW over USB at 120 BPM: each tick up to a 1 ms frame late plus 0.3 ms of jitter; a bar
    // stopped halfway, so most of it is measured after the press
    {
        FakedClock c;
        c.period = [](uint32_t) { return 1000.; };
        c.delay = [&rnd](uint32_t) { return 48. * rnd() + 14.4 * rnd(); };
        c.Init(mem, 1000.);
        c.Until(4800);
        c.looper.StartRecording(true);
        c.Block();
        c.Until(c.now + 48000);
        c.looper.StopRecording();
        c.Until(c.now + 60000);
        const long err = static_cast<long>(c.looper.GetLength()) - 96000;
        printf("      USB-like ticks, 1 bar stopped halfway: %zu samples (%+ld)\n",
               c.looper.GetLength(), err);
        Check(c.looper.GetBeats() == 4 && labs(err) <= 12,
              "quantized fit: a bar from jittered USB ticks within 12 samples of its length");
    }

    // the clock slows a little after the press: the loop's end follows the ticks that come in
    // until it closes, not the period at the press
    {
        FakedClock c;
        c.period = [](uint32_t k) { return k < 60 ? 1000. : 1010.; };
        c.delay = [](uint32_t) { return 0.; };
        c.Init(mem, 1000.);
        c.Until(4800);
        c.looper.StartRecording(true);
        c.Block();
        c.Until(c.now + 48000);
        c.looper.StopRecording();
        c.Until(c.now + 60000);
        printf("      slower after the press: %zu samples (96000 at the press's tempo)\n",
               c.looper.GetLength());
        Check(c.looper.GetLength() > 96200 && c.looper.GetLength() < 97000,
              "quantized fit: the end follows the ticks after the press, up to the close");
    }

    // the clock speeds up so much after the press that the bar's end, refitted, has already
    // passed: it closes at once on that end, a whole bar; what was recorded past it is less than
    // the post-roll the loop point crossfades from (kXfadeFrames), so it fades over cleanly
    {
        FakedClock c;
        c.period = [](uint32_t k) { return k < 86 ? 1000. : 400.; };
        c.delay = [](uint32_t) { return 0.; };
        c.Init(mem, 1000.);
        c.Until(4800);
        c.looper.StartRecording(true);
        c.Block();
        const size_t start = c.now;
        c.Until(start + 86400);
        c.looper.StopRecording();
        size_t written = 0;
        while (c.looper.GetState() == chompi::Looper::State::RECORDING && c.now < start + 200000)
        {
            c.Block();
            written = c.now - start;
        }
        printf("      much faster after the press: closed %zu samples in at %zu, %u beats\n",
               written, c.looper.GetLength(), c.looper.GetBeats());
        Check(c.looper.GetState() == chompi::Looper::State::PLAYING && c.looper.GetBeats() == 4
                  && c.looper.GetLength() < written
                  && written - c.looper.GetLength() < kXfadeFrames,
              "quantized fit: an end that has already passed closes at once, on it");
    }
}

/** Bar lines: the pulses Pulse() returns that IsBarLine, against PulsesToBarLine's estimate */
static void TestBarLines()
{
    MidiClock midi;
    TempoClock clock;
    clock.Init(kSr, &midi);

    // free: every 48 pulses
    for (int i = 0; i < 10; i++)
        clock.Pulse();
    Check(clock.PulsesToBarLine() == 38 && clock.PulsesPerBarLine() == kPulsesPerBar,
          "bar lines, free: 38 pulses after the 10th");
    uint32_t n = 0;
    while (!TempoClock::IsBarLine(clock.Pulse()))
        n++;
    Check(n + 1 == 38, "bar lines, free: and there it is");
    Check(clock.PulsesToBarLine() == kPulsesPerBar, "bar lines, free: right after one, a whole bar");
    Check(fabsf(clock.PulseSamples() - 2000.f) < .01f, "bar lines: 2000 samples a pulse at 120 BPM");

    // loops of 2, 4 and 6 beats, forward: a bar line every 48 pulses and on every wrap
    const uint32_t beats[] = {2, 4, 6};
    const uint32_t gaps[][2] = {{24, 24}, {48, 48}, {48, 24}};
    for (size_t l = 0; l < 3; l++)
    {
        const uint32_t pulses = beats[l] * kPulsesPerBeat;
        clock.SetLoop(pulses * 1000, beats[l]);
        Pulses(clock, 0.f, 1.f); // beat 1
        bool ok = true;
        uint32_t idx = 0;
        for (size_t gap = 0; gap < 2; gap++)
        {
            ok = ok && clock.PulsesToBarLine() == gaps[l][gap];
            uint32_t count = 0;
            do
            {
                idx++;
                count++;
                Pulses(clock, (idx % pulses + .5f) / pulses, 1.f);
            } while (!TempoClock::IsBarLine(clock.Position()));
            ok = ok && count == gaps[l][gap];
        }
        char what[80];
        snprintf(what, sizeof(what), "bar lines, %u-beat loop: %u then %u pulses apart",
                 static_cast<unsigned>(beats[l]), static_cast<unsigned>(gaps[l][0]),
                 static_cast<unsigned>(gaps[l][1]));
        Check(ok, what);
        Check(clock.PulsesPerBarLine() == (pulses < kPulsesPerBar ? pulses : kPulsesPerBar),
              "bar lines: a loop shorter than a bar has one every pass");
    }

    // reverse, a 4-beat loop crossing back over pulse 10: 9 down to 0 to go
    clock.SetLoop(48000, 4);
    Pulses(clock, 0.f, 1.f);
    Pulses(clock, 10.5f / 48.f, 1.f);
    Pulses(clock, 9.5f / 48.f, -1.f);
    Check(clock.Reverse() && clock.PulsesToBarLine() == 10, "bar lines, reverse: 9 down to 0 is 10 pulses");
    Pulses(clock, 9.3f / 48.f, -1.f); // between two pulses
    Check(clock.Reverse() && clock.PulsesToBarLine() == 10,
          "bar lines, reverse: still known between pulses");

    // a loop slowed below the FX's slowest tempo: the pulses' real rate
    clock.SetLoop(48000, 4); // 1s, 240 BPM
    Pulses(clock, 0.f, 1.f / 16.f);
    Check(clock.GetTempo() == kMinBpm && fabsf(clock.PulseSamples() - 4000.f * 16.f / 2.f / 2.f) < 1.f,
          "bar lines: a slowed loop's pulse period at its real rate, not the clamped tempo");
    clock.ClearLoop();
}

/** Records what a morph sends, in place of the FxChain */
struct FakeChain
{
    float params[kNumFx][kNumFxParams] = {};
    bool on[kNumFx] = {};
    void SetParam(size_t fx, size_t p, float v) { params[fx][p] = v; }
    void SetOn(size_t fx, bool o) { on[fx] = o; }
    void FastSlew() {}
};

/** One block as the engine runs it: the clock's pulses, then the morph. Returns whether it
 *  had a bar line */
static bool MorphBlock(TempoClock& clock, FxMorphT<FakeChain>& morph)
{
    bool bar = false;
    const uint32_t n = clock.Process(kBlock);
    for (uint32_t i = 0; i < n; i++)
    {
        const bool line = TempoClock::IsBarLine(clock.Pulse());
        bar = bar || line;
        morph.Pulse(line);
    }
    morph.Process(kBlock, clock.PulseSamples());
    return bar;
}

static FxMorphPlan Plan(MorphParam how, float from, float to)
{
    FxMorphPlan plan;
    plan.deferred = plan.wake = plan.was_on = plan.park = 0;
    for (size_t fx = 0; fx < kNumFx; fx++)
        for (size_t p = 0; p < kNumFxParams; p++)
        {
            plan.start[fx][p] = from;
            plan.target[fx][p] = to;
            plan.how[fx][p] = how;
        }
    return plan;
}

static void TestMorph()
{
    MidiClock midi;
    TempoClock clock;
    clock.Init(kSr, &midi); // free at 120 BPM: a bar is 2s, 4000 blocks
    FakeChain chain;
    FxMorphT<FakeChain> morph;
    morph.Init(&chain);

    // a quarter into a bar, one tap: lands on the bar line, 1.5s on
    while (clock.Position() != 12)
        MorphBlock(clock, morph);
    FxMorphPlan plan = Plan(MorphParam::GLIDE, 0.f, 1.f);
    plan.how[FX_DELAY][0] = MorphParam::HOLD;
    morph.Start(plan, clock.PulsesToBarLine());
    Check(morph.Active() && chain.params[FX_FILTER][0] == 0.f, "morph: starts where it is");
    int blocks = 0;
    float half = -1.f, last = 0.f;
    bool rising = true, held = true;
    bool bar = false;
    while (!bar)
    {
        bar = MorphBlock(clock, morph);
        blocks++;
        if (blocks == 1500)
            half = chain.params[FX_FILTER][0];
        rising = rising && chain.params[FX_FILTER][0] >= last;
        last = chain.params[FX_FILTER][0];
        if (!bar)
            held = held && chain.params[FX_DELAY][0] == 0.f;
    }
    Check(!morph.Active() && chain.params[FX_FILTER][0] == 1.f && chain.params[FX_DELAY][0] == 1.f,
          "morph: landed on the bar line, every parameter on the scene's");
    Check(blocks > 2990 && blocks <= 3001, "morph: three quarters of a bar, 3000 blocks");
    Check(fabsf(half - .5f) < .01f, "morph: halfway there halfway through");
    Check(rising, "morph: a steady glide");
    Check(held, "morph: a held (stepped) parameter switches only at the landing");

    // one tap just before a bar line still lands on it: strict
    while (clock.Position() != 47)
        MorphBlock(clock, morph);
    morph.Start(Plan(MorphParam::GLIDE, 0.f, 1.f), clock.PulsesToBarLine());
    blocks = 0;
    while (!MorphBlock(clock, morph))
        blocks++;
    Check(!morph.Active() && blocks < 100, "morph: a tap in the bar's last pulse lands on its line");

    // two taps, the second a quarter of a bar in: two bar lines, no jump at the tap
    while (clock.Position() != 0)
        MorphBlock(clock, morph);
    morph.Start(Plan(MorphParam::GLIDE, 0.f, 1.f), clock.PulsesToBarLine());
    for (int i = 0; i < 1000; i++)
        MorphBlock(clock, morph);
    const float before = chain.params[FX_FILTER][0];
    Check(morph.AddBar(clock.PulsesPerBarLine()), "morph: a second tap adds a bar");
    MorphBlock(clock, morph);
    Check(fabsf(chain.params[FX_FILTER][0] - before) < .002f, "morph: no jump at the tap");
    int lines = 0;
    blocks = 1001;
    while (morph.Active())
    {
        lines += MorphBlock(clock, morph);
        blocks++;
        if (blocks == 4000)
            half = chain.params[FX_FILTER][0];
    }
    Check(lines == 2 && blocks > 7990 && blocks <= 8001, "morph: two taps land on the second bar line");
    // a quarter there at the tap, then the rest over 7000 blocks: 3000 of them at 4000
    Check(fabsf(half - (.25f + .75f * 3.f / 7.f)) < .01f && chain.params[FX_FILTER][0] == 1.f,
          "morph: and glide over both bars");

    // held (SHIFT still down): two taps, nothing moves however long it's held, past bar lines
    // too; released a quarter into the third bar, it glides the whole way from there to the
    // next bar line plus the extra tap's
    while (clock.Position() != 0)
        MorphBlock(clock, morph);
    chain.params[FX_FILTER][0] = 0.f;
    morph.Start(Plan(MorphParam::GLIDE, 0.f, 1.f), clock.PulsesToBarLine(), true);
    Check(morph.Holding(), "held morph: holding");
    for (int i = 0; i < 1000; i++)
        MorphBlock(clock, morph);
    Check(morph.AddBar(clock.PulsesPerBarLine()), "held morph: a second tap adds a bar");
    lines = 0;
    for (int i = 0; i < 8000; i++)
        lines += MorphBlock(clock, morph);
    Check(chain.params[FX_FILTER][0] == 0.f && morph.Active() && lines == 2,
          "held morph: nothing moves while held, nor lands, past two bar lines");
    morph.Release(clock.PulsesToBarLine(), clock.PulsesPerBarLine());
    Check(!morph.Holding(), "held morph: released");
    blocks = 9000;
    lines = 0;
    while (morph.Active())
    {
        lines += MorphBlock(clock, morph);
        blocks++;
        if (blocks == 12500)
            half = chain.params[FX_FILTER][0];
    }
    Check(lines == 2 && blocks > 15990 && blocks <= 16001,
          "held morph: lands on the second bar line after the release");
    // from 0 at the release (9000) to 1 at 16000: half way at 12500
    Check(fabsf(half - .5f) < .01f && chain.params[FX_FILTER][0] == 1.f,
          "held morph: glides the whole way after the release");

    // the most taps
    morph.Start(Plan(MorphParam::GLIDE, 0.f, 1.f), clock.PulsesToBarLine());
    int taps = 1;
    while (morph.AddBar(clock.PulsesPerBarLine()))
        taps++;
    Check(taps == static_cast<int>(kMaxMorphBars), "morph: up to 8 bars");
    morph.Land();
    Check(!morph.Active() && chain.params[FX_FILTER][0] == 1.f, "morph: landed early, on the scene");

    // fade out: the amount glides to its default, the key stays on until the landing;
    // fade in: the key comes on once the amount is down
    plan = Plan(MorphParam::HOLD, .7f, .7f);
    plan.how[FX_DELAY][3] = MorphParam::FADE_OUT;
    plan.start[FX_DELAY][3] = .8f;
    plan.target[FX_DELAY][3] = .3f;
    plan.how[FX_REVERB][3] = MorphParam::GLIDE;
    plan.start[FX_REVERB][3] = 0.f;
    plan.target[FX_REVERB][3] = .9f;
    plan.deferred = (1u << FX_DELAY) | (1u << FX_REVERB);
    plan.wake = 1u << FX_REVERB;
    chain.on[FX_DELAY] = true;
    while (clock.Position() != 0)
        MorphBlock(clock, morph);
    morph.Start(plan, clock.PulsesToBarLine());
    morph.SetOn(FX_DELAY, false);
    morph.SetOn(FX_REVERB, true);
    Check(chain.on[FX_DELAY] && !chain.on[FX_REVERB], "fades: no key switched at the press");
    for (int i = 0; i < 60; i++)
        MorphBlock(clock, morph);
    Check(chain.on[FX_REVERB], "fades: the fade-in's key on after 25ms");
    bool still_on = true;
    float delay_level = 1.f;
    while (morph.Active())
    {
        still_on = still_on && chain.on[FX_DELAY];
        delay_level = chain.params[FX_DELAY][3];
        MorphBlock(clock, morph);
    }
    Check(still_on && delay_level < .01f, "fades: the fade-out's key on, its level down to 0");
    Check(!chain.on[FX_DELAY] && chain.params[FX_DELAY][3] == .3f,
          "fades: at the landing, the key off and the level the scene's");
    Check(chain.params[FX_REVERB][3] == .9f, "fades: the fade-in's level the scene's");

    // parked: a fade-out that ends off stays faded out, its key off, the rest untouched
    plan.park = 1u << FX_DELAY;
    chain.on[FX_DELAY] = true;
    chain.params[FX_DELAY][0] = .2f;
    plan.start[FX_DELAY][0] = .2f;
    plan.target[FX_DELAY][0] = .9f;
    plan.how[FX_DELAY][0] = MorphParam::HOLD;
    while (clock.Position() != 0)
        MorphBlock(clock, morph);
    morph.Start(plan, clock.PulsesToBarLine());
    morph.SetOn(FX_DELAY, false);
    while (morph.Active())
        MorphBlock(clock, morph);
    Check(!chain.on[FX_DELAY] && chain.params[FX_DELAY][3] < .01f && chain.params[FX_DELAY][0] == .2f,
          "parked: off, faded out, the rest as it was: the tail rings out");

    // a knob turned meanwhile moves the destination
    morph.Start(Plan(MorphParam::GLIDE, 0.f, 1.f), clock.PulsesToBarLine());
    Check(morph.SetParam(FX_FILTER, 0, .4f), "turned meanwhile: goes to the morph");
    while (morph.Active())
        MorphBlock(clock, morph);
    Check(chain.params[FX_FILTER][0] == .4f, "turned meanwhile: lands there");
    Check(!morph.SetParam(FX_FILTER, 0, .2f) && !morph.SetOn(FX_FILTER, true),
          "after it: straight to the chain");

    // stopped halfway (SHIFT + PLAY): everything stays there, the fade-out's key still on
    while (clock.Position() != 0)
        MorphBlock(clock, morph);
    plan = Plan(MorphParam::GLIDE, 0.f, 1.f);
    plan.deferred = plan.was_on = 1u << FX_DELAY;
    chain.on[FX_DELAY] = true;
    morph.Start(plan, clock.PulsesToBarLine());
    morph.SetOn(FX_DELAY, false);
    for (int i = 0; i < 2000; i++)
        MorphBlock(clock, morph);
    float live[kNumFx][kNumFxParams];
    uint16_t unswitched = 0, was_on = 0;
    Check(morph.Freeze(live, &unswitched, &was_on) && !morph.Active(), "stopped: it ends");
    Check(fabsf(live[FX_FILTER][0] - .5f) < .01f && chain.params[FX_FILTER][0] == live[FX_FILTER][0],
          "stopped: halfway, and that's what the chain has");
    Check(unswitched == (1u << FX_DELAY) && was_on == (1u << FX_DELAY) && chain.on[FX_DELAY],
          "stopped: the key it hadn't switched stays as it was");
    for (int i = 0; i < 4000; i++)
        MorphBlock(clock, morph);
    Check(chain.params[FX_FILTER][0] == live[FX_FILTER][0] && chain.on[FX_DELAY],
          "stopped: no landing at the bar line");
    Check(!morph.Freeze(live, &unswitched, &was_on), "stopped: nothing to stop any more");
}

/** The delay's reverse events at the slowest tempo: the longest divisions have no room */
static bool Near(float a, float b) { return fabsf(a - b) < 1e-5f; }

/** The crossfader taking a morph that a second tap gave another bar: its 0 is still where
 *  the morph started, which is what letting go there restores (FxControls::EndFade), not
 *  where the glide was at the tap */
static void TestFaderAfterBar()
{
    MidiClock midi;
    TempoClock clock;
    clock.Init(kSr, &midi);
    FakeChain chain;
    FxMorphT<FakeChain> morph;
    morph.Init(&chain);
    while (clock.Position() != 0)
        MorphBlock(clock, morph);
    morph.Start(Plan(MorphParam::GLIDE, .2f, .8f), clock.PulsesToBarLine());
    for (int i = 0; i < 1000; i++)
        MorphBlock(clock, morph);
    const float at_tap = chain.params[FX_FILTER][0];
    morph.AddBar(clock.PulsesPerBarLine());
    MorphBlock(clock, morph);
    Check(fabsf(chain.params[FX_FILTER][0] - at_tap) < .002f, "fader after a tap: no jump at the tap");
    for (int i = 0; i < 500; i++)
        MorphBlock(clock, morph);
    morph.Fader(0.f);
    MorphBlock(clock, morph);
    const float at_0 = chain.params[FX_FILTER][0];
    morph.Fader(1.f);
    MorphBlock(clock, morph);
    printf("  fader after a tap: %.3f at the tap, %.3f at 0, %.3f at 1\n", at_tap, at_0,
           chain.params[FX_FILTER][0]);
    Check(at_tap > .3f && at_0 == .2f && chain.params[FX_FILTER][0] == .8f,
          "fader after a tap: 0 is the morph's start, 1 the scene");
}

/** The crossfader (FxMorph::Fader): the glide in the hand, both ways */
static void TestFader()
{
    MidiClock midi;
    TempoClock clock;
    clock.Init(kSr, &midi);
    FakeChain chain;
    FxMorphT<FakeChain> morph;
    morph.Init(&chain);

    // the filter on in both, gliding with a stepped knob; the freezer switching (no fade
    // knobs); the delay fading out; the reverb fading in
    FxMorphPlan plan = Plan(MorphParam::HOLD, .7f, .7f);
    plan.how[FX_FILTER][0] = MorphParam::GLIDE;
    plan.start[FX_FILTER][0] = .2f;
    plan.target[FX_FILTER][0] = .8f;
    plan.start[FX_FILTER][5] = .2f;
    plan.target[FX_FILTER][5] = .6f;
    plan.how[FX_DELAY][3] = MorphParam::FADE_OUT;
    plan.start[FX_DELAY][3] = .6f;
    plan.start[FX_DELAY][0] = .25f;
    plan.target[FX_DELAY][0] = .5f;
    plan.how[FX_REVERB][3] = MorphParam::GLIDE;
    plan.start[FX_REVERB][3] = 0.f;
    plan.target[FX_REVERB][3] = .5f;
    const uint16_t freezer = 1u << FX_FREEZER, delay = 1u << FX_DELAY, reverb = 1u << FX_REVERB;
    plan.deferred = freezer | delay | reverb;
    plan.was_on = freezer | delay;
    plan.wake = reverb;
    plan.park = delay;
    chain.on[FX_FREEZER] = chain.on[FX_DELAY] = true;

    morph.Start(plan, clock.PulsesToBarLine(), true);
    // what FxControls tells it next: the scene's keys
    Check(morph.SetOn(FX_FREEZER, false) && morph.SetOn(FX_DELAY, false)
              && morph.SetOn(FX_REVERB, true) && !morph.SetOn(FX_FILTER, true),
          "fader: the morph takes the keys it switches");
    for (int b = 0; b < 4000; b++)
        MorphBlock(clock, morph);
    Check(morph.Holding() && chain.params[FX_FILTER][0] == 0.f && !chain.on[FX_REVERB],
          "fader: held, nothing moves yet");

    Check(morph.Fader(.25f) && morph.Manual() && !morph.Holding(), "fader: it takes the held morph");
    MorphBlock(clock, morph);
    Check(Near(chain.params[FX_FILTER][0], .35f) && Near(chain.params[FX_REVERB][3], .125f)
              && Near(chain.params[FX_DELAY][3], .45f),
          "fader: a quarter of the way, a quarter of each glide and fade");
    Check(chain.on[FX_REVERB] && chain.on[FX_DELAY] && chain.on[FX_FREEZER]
              && chain.params[FX_FILTER][5] == 0.f,
          "fader: before the middle, a fade-in on, a fade-out on, the switching ones and stepped values as they were");
    morph.Fader(.5f);
    MorphBlock(clock, morph);
    Check(!chain.on[FX_FREEZER] && chain.params[FX_FILTER][5] == .6f && chain.on[FX_DELAY]
              && chain.params[FX_DELAY][0] == 0.f,
          "fader: in the middle, the switching key and the stepped value switch; one going off keeps its own");
    morph.Fader(.25f);
    MorphBlock(clock, morph);
    Check(chain.on[FX_FREEZER] && chain.params[FX_FILTER][5] == .2f,
          "fader: and back before the middle, back again");
    morph.Fader(0.f);
    MorphBlock(clock, morph);
    Check(!chain.on[FX_REVERB] && chain.params[FX_FILTER][0] == .2f,
          "fader: at the start, the fade-in off again");
    morph.Fader(1.f);
    MorphBlock(clock, morph);
    Check(!chain.on[FX_DELAY] && chain.on[FX_REVERB] && chain.params[FX_DELAY][3] == 0.f,
          "fader: at the end, the fade-out off");
    bool lines = false;
    for (int b = 0; b < 12000; b++)
        lines = MorphBlock(clock, morph) || lines;
    Check(lines && morph.Active() && morph.Manual(), "fader: the bar lines don't land it");
    Check(!morph.AddBar(clock.PulsesPerBarLine()), "fader: no bars to add");

    // stopped in between: what's switched stays switched, the rest as at the start
    morph.Fader(.75f);
    MorphBlock(clock, morph);
    float live[kNumFx][kNumFxParams];
    uint16_t unswitched = 0, was_on = 0;
    Check(morph.Freeze(live, &unswitched, &was_on) && unswitched == delay && was_on == delay
              && Near(live[FX_FILTER][0], .65f) && !morph.Active(),
          "fader: stopped at 3/4, the delay still to switch, the glide where it got to");

    // landed from the end: the scene's
    chain = FakeChain();
    chain.on[FX_FREEZER] = chain.on[FX_DELAY] = true;
    morph.Start(plan, clock.PulsesToBarLine(), true);
    morph.SetOn(FX_FREEZER, false);
    morph.SetOn(FX_DELAY, false);
    morph.SetOn(FX_REVERB, true);
    morph.Fader(1.f);
    MorphBlock(clock, morph);
    morph.Land();
    Check(!morph.Active() && !chain.on[FX_DELAY] && !chain.on[FX_FREEZER] && chain.on[FX_REVERB]
              && chain.params[FX_FILTER][0] == .8f && chain.params[FX_DELAY][0] == 0.f,
          "fader: landed at the end, the scene's keys and values, one gone off parked");

    // a gliding morph taken: a fade-in it has switched already goes through the fader too
    chain = FakeChain();
    chain.on[FX_FREEZER] = chain.on[FX_DELAY] = true;
    morph.Start(plan, clock.PulsesToBarLine(), true);
    morph.SetOn(FX_FREEZER, false);
    morph.SetOn(FX_DELAY, false);
    morph.SetOn(FX_REVERB, true);
    morph.Release(clock.PulsesToBarLine(), clock.PulsesPerBarLine());
    for (int b = 0; b < 100; b++)
        MorphBlock(clock, morph);
    Check(chain.on[FX_REVERB] && !morph.SetOn(FX_REVERB, true), "fader: gliding, the fade-in woke");
    morph.Fader(0.f);
    MorphBlock(clock, morph);
    Check(!chain.on[FX_REVERB], "fader: taken back to the start, the fade-in off again");
    morph.Land();
}

static void TestDelayReverse()
{
    static const size_t kFrames = 480000; // the device's 10s
    static float mem[kFrames * 2];
    static granularDelay delay;
    delay.Init(mem, kFrames);
    auto settle = [&](int bpm, size_t div) {
        delay.SetTempo(bpm);
        delay.setDivision(div);
        float l, r;
        for (int i = 0; i < 48000; i++)
        {
            delay.write(0.f, 0.f);
            delay.read(&l, &r);
        }
    };
    settle(120, 8);
    Check(delay.ReverseFits(), "delay: 2 bars at 120 BPM, room for a reverse");
    settle(50, 8);
    Check(!delay.ReverseFits(), "delay: 2 bars at 50 BPM, no room: a retrigger instead");
    settle(50, 7);
    Check(delay.ReverseFits(), "delay: 1 bar at 50 BPM, room again");
}

/** The delay on a tempo change: a jump (a loop closing on its own tempo) crossfades to the
 *  new time instead of dragging the read head through the buffer; a small step still slides */
static void TestDelayTempoJump()
{
    static const size_t kFrames = 480000;
    static float mem[kFrames * 2];
    static granularDelay delay;
    auto run = [&](int from, int to, int* slid, int* swaps, bool* xfaded) {
        delay.Init(mem, kFrames);
        delay.setRandom(.5f); // no random events
        delay.setDivision(4);
        delay.SetTempo(from);
        float l, r;
        for (int i = 0; i < 48000; i++)
        {
            delay.write(0.f, 0.f);
            delay.read(&l, &r);
        }
        delay.SetTempo(to);
        *slid = *swaps = 0;
        *xfaded = false;
        float last[2] = {delay.myVoices[0].read_head_, delay.myVoices[1].read_head_};
        for (int i = 0; i < 48000; i++)
        {
            delay.write(0.f, 0.f);
            delay.read(&l, &r);
            for (int v = 0; v < 2; v++)
            {
                const delayVoice& voice = delay.myVoices[v];
                *xfaded = *xfaded || voice.div_crossfade_;
                float d = voice.read_head_ - last[v];
                if (d < -static_cast<float>(kFrames) / 2.f)
                    d += kFrames;
                last[v] = voice.read_head_;
                if (!voice.active_)
                    continue;
                if (fabsf(d) > 100.f)
                    (*swaps)++;
                else if (fabsf(d - 1.f) > .01f)
                    (*slid)++;
            }
        }
    };
    int slid, swaps;
    bool xfaded;
    run(120, 89, &slid, &swaps, &xfaded);
    Check(xfaded && swaps == 1 && slid == 0, "delay: a tempo jump crossfades to the new time");
    run(120, 119, &slid, &swaps, &xfaded);
    Check(!xfaded && swaps == 0 && slid > 0, "delay: a step of 1 BPM slides");
}

/** A loop whose tempo isn't whole: the delay's time is its beat, not the rounded BPM's, so
 *  the echoes don't drift off the loop */
static void TestDelayOnLoopBeat()
{
    MidiClock midi;
    TempoClock clock;
    clock.Init(kSr, &midi);
    const size_t length = 266400; // 5.55s, 8 beats: 86.49 BPM
    clock.SetLoop(length, 8);
    Pulses(clock, 0.f, 1.f);
    Check(clock.GetTempo() == 86, "loop of 86.49 BPM: whole tempo 86");
    Check(fabsf(clock.GetFxBpm() - 86.486f) < .01f, "loop of 86.49 BPM: the FX get it exactly");

    static const size_t kFrames = 480000;
    static float mem[kFrames * 2];
    static granularDelay delay;
    delay.Init(mem, kFrames);
    delay.setRandom(.5f);
    delay.setDivision(2); // 1/4
    delay.SetTempo(clock.GetFxBpm());
    float l, r;
    for (int i = 0; i < 48000; i++)
    {
        delay.write(0.f, 0.f);
        delay.read(&l, &r);
    }
    const float beat = static_cast<float>(length) / 8.f;
    Check(fabsf(delay.myVoices[0].delay_samples_ - beat) < 1.f,
          "delay on a loop of 86.49 BPM: a 1/4 is the loop's beat");
}

int main()
{
    TestTaps();
    TestFit();
    TestLoopClock();
    TestMidiClock();
    TestQuantizedBeats();
    TestQuantizedFit();
    TestBarLines();
    TestMorph();
    TestFader();
    TestFaderAfterBar();
    TestDelayReverse();
    TestDelayTempoJump();
    TestDelayOnLoopBeat();
    return Finish();
}
