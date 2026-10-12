// Freezer check: the freezer's (FxFreezer.h) capture and roll, at 120 BPM, on a ramp whose every
// sample is its own value, so the output tells which input sample it is. Built and run by
// unit.sh freezer.
//  - capture: pressed, the live signal passes bit for bit until the next 16th; from there it
//    passes one loop length more (the first pass, recording), then repeats what it recorded,
//    a 16th (6000 samples) each, starting on the 16th's sample
//  - roll (#173): a divider of the length by hand, off to 1/16, no shorter than 1/64 bar
//    (1500); a step takes at the next repeat's start, a capture starts on the knob's step
//  - released, the live signal is back once the gate has faded out
//  - the key's fade (FxGate) lands on 1, so a repeat carries none of the live input (#45)
#include <cmath>
#include <cstdio>
#include <vector>
#include "check.h"
#include "FxChain.h"

using namespace chompi;

static const float kSr = 48000.f;
static const size_t kFrames = 240000; // FRIZZ's kFreezerFrames
static float buf_l[kFrames], buf_r[kFrames];

/** Every sample its own value, 1/131072 from the next */
static float Ramp(size_t i) { return (static_cast<float>(i % 65536) / 65536.f - .5f) * .5f; }
/** Which sample of the ramp a value is (mod 65536) */
static long Index(float v) { return lround((v / .5f + .5f) * 65536.f); }

struct Run
{
    std::vector<float> out;
    size_t start; // the 16th's sample, where the capture starts
};

/** Presses it at sample 1000, a 16th at `start`, runs n samples, the left channel out; the
 *  input silent from the first repeat on, unless released or `live` */
static Run Play(float roll, size_t start, size_t n, size_t release_at = 0, bool live = false)
{
    static Freezer f;
    f.Init(kSr, buf_l, buf_r, kFrames);
    f.SetParam(Freezer::LENGTH, 0.f); // 1/16
    f.SetParam(Freezer::ROLL, roll);
    f.SetTempo(120.f);
    Run run{{}, start};
    for (size_t i = 0; i < n; i++)
    {
        if (i == 1000)
            f.SetOn(true);
        if (i == start)
            f.ClockPulse(0);
        if (release_at && i == release_at)
            f.SetOn(false);
        float l = live || release_at || i < start + 6000 ? Ramp(i) : 0.f, r = l;
        f.Process(&l, &r);
        run.out.push_back(l);
    }
    return run;
}

/** Where each repeat starts: the loop's sample past its seam crossfade (240) comes round */
static std::vector<size_t> Repeats(const Run& run)
{
    std::vector<size_t> starts;
    const long mark = Index(Ramp(run.start + 240));
    for (size_t t = run.start + 6000; t < run.out.size(); t++)
        if (Index(run.out[t]) == mark)
            starts.push_back(t - 240);
    return starts;
}

static std::vector<size_t> Lengths(const std::vector<size_t>& starts)
{
    std::vector<size_t> lens;
    for (size_t i = 1; i < starts.size(); i++)
        lens.push_back(starts[i] - starts[i - 1]);
    return lens;
}

static void Print(const char* what, const std::vector<size_t>& lens)
{
    printf("      %s:", what);
    for (size_t i = 0; i < lens.size() && i < 16; i++)
        printf(" %zu", lens[i]);
    printf("%s\n", lens.size() > 16 ? " ..." : "");
}

static void TestCapture()
{
    const size_t start = 3000;
    const Run run = Play(0.f, start, 60000);
    bool live = true;
    for (size_t t = 0; t < start + 6000; t++)
        live &= run.out[t] == Ramp(t);
    Check(live, "capture: pressed, the live signal passes bit for bit to the 16th and one loop past it");
    const std::vector<size_t> starts = Repeats(run);
    const std::vector<size_t> lens = Lengths(starts);
    Print("repeats", lens);
    bool sixteenths = !starts.empty() && starts[0] == start + 6000 && lens.size() >= 5;
    for (size_t len : lens)
        sixteenths &= len == 6000;
    Check(sixteenths, "capture: then it repeats from the 16th's sample, a 16th (6000 samples) each");
    // past the seam's crossfade, a repeat is the captured 16th, sample for sample
    bool same = true;
    for (size_t t = start + 6000 + 240; t < start + 12000; t++)
        same &= Index(run.out[t]) == Index(Ramp(t - 6000));
    Check(same, "capture: what it repeats is what came in from the 16th, sample for sample");
    // and bit for bit with the live input still coming in: none of it leaks through (#45)
    const Run on = Play(0.f, start, 30000, 0, true);
    bool exact = true;
    for (size_t t = start + 6000 + 240; t < start + 12000; t++)
        exact &= on.out[t] == Ramp(t - 6000);
    Check(exact, "capture: the repeat bit for bit while the live input goes on");
}

/** A knob turn during a run: the roll to `roll` at sample `at` */
struct RollTurn
{
    size_t at;
    float roll;
};

/** As Play, at a length (its knob) and a roll from the start, the roll turned at `turns`;
 *  the input the ramp for `first` samples from the 16th (the first pass), silent after. A
 *  release at `release_at` and a press again at `again` (0: none) */
static Run PlayRoll(float length, float roll, const std::vector<RollTurn>& turns, size_t first,
                    size_t n, size_t release_at = 0, size_t again = 0)
{
    static Freezer f;
    f.Init(kSr, buf_l, buf_r, kFrames);
    f.SetParam(Freezer::LENGTH, length);
    f.SetParam(Freezer::ROLL, roll);
    f.SetTempo(120.f);
    const size_t start = 3000;
    Run run{{}, start};
    for (size_t i = 0; i < n; i++)
    {
        if (i == 1000)
            f.SetOn(true);
        if (release_at && i == release_at)
            f.SetOn(false);
        if (again && i == again)
            f.SetOn(true);
        if (i % 3000 == 0) // every 16th
            f.ClockPulse(0);
        for (const RollTurn& t : turns)
            if (i == t.at)
                f.SetParam(Freezer::ROLL, t.roll);
        // the ramp in for each capture's first pass, the next 16th after a press again too
        const size_t next = again ? (again / 3000 + 1) * 3000 : 0;
        const bool in = i < start + first || (again && i >= next && i < next + first);
        float l = in ? Ramp(i) : 0.f, r = l;
        f.Process(&l, &r);
        run.out.push_back(l);
    }
    return run;
}

/** Where each repeat starts from `from` on: the capture's sample past its seam crossfade
 *  (240) comes round */
static std::vector<size_t> RepeatsFrom(const Run& run, size_t from)
{
    std::vector<size_t> starts;
    const long mark = Index(Ramp(run.start + 240));
    for (size_t t = from; t < run.out.size(); t++)
        if (Index(run.out[t]) == mark)
            starts.push_back(t - 240);
    return starts;
}

static bool All(const std::vector<size_t>& lens, size_t from, size_t to, size_t len)
{
    if (lens.size() < to)
        return false;
    for (size_t i = from; i < to; i++)
        if (lens[i] != len)
            return false;
    return true;
}

// Roll (#173): off, 1/2, 1/4, 1/8, 1/16 of the length on the knob (0, .25, .5, .75, 1), no
// shorter than 1/64 bar (1500 samples at 120 BPM); a step takes at the next repeat's start, so
// the grid holds; the knob is the state, a capture starts on it
static void TestRoll()
{
    const float quarter = 4.f / 7.f; // the length knob on 1/4 bar, 24000 samples
    // on 1/4 from before the press: the capture repeats a quarter of it, 6000, from its grab
    {
        const Run run = PlayRoll(quarter, .5f, {}, 6000, 60000);
        const std::vector<size_t> starts = RepeatsFrom(run, run.start + 6000);
        const std::vector<size_t> lens = Lengths(starts);
        Print("roll 1/4 from the press", lens);
        Check(!starts.empty() && starts[0] == run.start + 6000 && All(lens, 0, 5, 6000),
              "roll: on 1/4 at the press, the capture repeats 1/4 of the length from the grab");
    }
    // off, then 1/8 in the first repeat: that repeat ends whole, then 3000s; back to off in
    // one move at the next start: the whole length again
    {
        const size_t s = 3000;
        const Run run = PlayRoll(quarter, 0.f, {{s + 24000 + 5000, .75f}, {s + 48000 + 7000, 0.f}},
                                 24000, 160000);
        const std::vector<size_t> starts = RepeatsFrom(run, s + 24000);
        const std::vector<size_t> lens = Lengths(starts);
        Print("roll off, 1/8, off", lens);
        bool grid = starts.size() >= 7 && starts[0] == s + 24000 && starts[1] == s + 48000;
        for (size_t i = 2; grid && i < starts.size() && starts[i] <= s + 48000 + 9000; i++)
            grid &= starts[i] == s + 48000 + 3000 * (i - 1);
        Check(grid, "roll: turned to 1/8 in a repeat, it takes at the next repeat's start, then 3000s");
        // turned off at +7000 inside a 3000 repeat that started at +6000: it ends at +9000,
        // from there the whole 24000 again
        bool back = false;
        for (size_t i = 0; i + 2 < starts.size(); i++)
            if (starts[i] == s + 48000 + 9000)
                back = starts[i + 1] == starts[i] + 24000 && starts[i + 2] == starts[i] + 48000;
        Check(back, "roll: back to off, the whole length in one move at the next repeat's start");
    }
    // turned during the first pass: it takes where the first repeat starts
    {
        const Run run = PlayRoll(quarter, 0.f, {{3000 + 10000, .25f}}, 24000, 120000);
        const std::vector<size_t> starts = RepeatsFrom(run, run.start + 24000);
        const std::vector<size_t> lens = Lengths(starts);
        Print("roll to 1/2 in the first pass", lens);
        Check(!starts.empty() && starts[0] == run.start + 24000 && All(lens, 0, 4, 12000),
              "roll: turned in the first pass, the first repeat starts on the grab's length and is 1/2 of it");
    }
    // the floor: 1/16 of a 1/16 bar would be 1/256 bar, it stays at 1/64 (1500); and 1/8 of
    // a 1/8 triplet (1/12 bar) halves only twice, to 1/48 bar (2000)
    {
        const Run sixteenth = PlayRoll(0.f, 1.f, {}, 1500, 30000);
        const std::vector<size_t> lens = Lengths(RepeatsFrom(sixteenth, sixteenth.start + 1500));
        Print("roll 1/16 on a 1/16", lens);
        Check(All(lens, 0, 8, 1500), "roll: no shorter than 1/64 bar");
        const Run triplet = PlayRoll(1.f / 7.f, .75f, {}, 2000, 30000);
        const std::vector<size_t> tl = Lengths(RepeatsFrom(triplet, triplet.start + 2000));
        Print("roll 1/8 on a 1/8 triplet", tl);
        Check(All(tl, 0, 8, 2000), "roll: a triplet's halvings stop at 1/48 bar, the last one not under 1/64");
    }
    // the knob is the state: released and pressed again with it still on 1/4, the new capture
    // repeats a quarter from its grab, no counter behind it
    {
        const size_t again = 60001; // the next 16th: 63000
        const Run run = PlayRoll(quarter, .5f, {}, 6000, 120000, 40000, again);
        Run second = run;
        second.start = 63000;
        const std::vector<size_t> starts = RepeatsFrom(second, 63000 + 6000);
        const std::vector<size_t> lens = Lengths(starts);
        Print("the next capture, roll still on 1/4", lens);
        Check(!starts.empty() && starts[0] == 63000 + 6000 && All(lens, 0, 5, 6000),
              "roll: still on 1/4 at the next capture, it repeats 1/4 of the length from that grab");
    }
}

static void TestRelease()
{
    const Run run = Play(0.f, 3000, 48000, 20000);
    bool live = true;
    for (size_t t = 30000; t < run.out.size(); t++)
        live &= run.out[t] == Ramp(t);
    Check(live, "release: the live signal back bit for bit once it has faded out");
}

/** The key's fade lands on 1: in float the one-pole alone stalls at 1 - 7e-6, and a fully wet
 *  effect then carries its input 100dB down, a repeat a few 1e-7 off what it captured (#45) */
static void TestGate()
{
    FxGate g;
    g.Init();
    g.SetOn(true);
    float early = 0.f;
    for (int i = 0; i < 4800; i++) // 100 ms
    {
        g.Process();
        if (i == 500)
            early = g.Value();
    }
    Check(early > .5f && early < 1.f, "gate: the key fades in over a few ms");
    Check(g.Value() == 1.f, "gate: and lands on 1, not short of it");
}

int main()
{
    TestGate();
    TestCapture();
    TestRoll();
    TestRelease();
    return Finish();
}
