// Freezer check: the freezer's (FxFreezer.h) capture and roll, at 120 BPM, on a ramp whose every
// sample is its own value, so the output tells which input sample it is. Built and run by
// unit.sh freezer.
//  - capture: pressed, the live signal passes bit for bit until the next 16th; from there it
//    passes one loop length more (the first pass, recording), then repeats what it recorded,
//    a 16th (6000 samples) each, starting on the 16th's sample
//  - roll at its shortest stage: the loop halves after 1 repeat, again after 2 more, down to
//    1/64 bar (1500), where it stays; at its longest stage it holds 8 repeats first
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

static void TestRoll()
{
    const std::vector<size_t> fast = Lengths(Repeats(Play(1.f, 3000, 96000)));
    Print("roll, shortest stage", fast);
    const std::vector<size_t> want = {6000, 3000, 3000, 1500, 1500, 1500, 1500, 1500, 1500};
    bool halves = fast.size() >= want.size();
    for (size_t i = 0; halves && i < fast.size(); i++)
        halves &= fast[i] == (i < want.size() ? want[i] : 1500);
    Check(halves, "roll: halving after 1 repeat, then after 2, down to 1/64 bar, where it stays");
    const std::vector<size_t> slow = Lengths(Repeats(Play(.25f, 3000, 96000)));
    Print("roll, longest stage", slow);
    bool eight = slow.size() >= 10;
    for (size_t i = 0; eight && i < slow.size(); i++)
        eight &= slow[i] == (i < 8 ? 6000u : 3000u);
    Check(eight, "roll: at its longest stage, 8 repeats before the first halving");
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
