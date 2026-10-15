// looper.cpp: checks the looper (Looper.h) on the host, without MIDI clock: a free recording
// plays back what was recorded at its length, play / pause, erase, the speed ladder and the
// refused quantized record, a glide that lands exactly on its speed, memory that wasn't cleared at
// boot sounding the same as cleared, the soft limit on what's written, and a switch while
// another's fade-out runs (#318). Exits 0 when everything passes. Run by unit.sh looper.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>
#include "check.h"
#include "Looper.h"

using namespace chompi;


static const size_t kBlock = 24;
static int16_t mem[kLoopMemSize];
static MidiClock midi_clock;
static Looper looper;

// the input: a ramp that tells every frame apart, left and right of opposite sign
static size_t frame = 0;
static float Input(size_t n) { return static_cast<float>(n % 20000) / 20000.f * .5f; }

/** One block through the looper; returns the last output sample (left) */
static float Block(float* out_l_first = nullptr)
{
    float in_l[kBlock], in_r[kBlock], out_l[kBlock], out_r[kBlock];
    for (size_t i = 0; i < kBlock; i++)
    {
        in_l[i] = Input(frame + i);
        in_r[i] = -in_l[i];
    }
    frame += kBlock;
    looper.Process(in_l, in_r, out_l, out_r, kBlock);
    if (out_l_first)
        *out_l_first = out_l[0];
    return out_l[kBlock - 1];
}

static void Blocks(size_t n)
{
    for (size_t i = 0; i < n; i++)
        Block();
}

static void TestRecordAndPlay()
{
    looper.Init(mem, &midi_clock);
    Check(looper.GetState() == Looper::State::EMPTY, "init: empty");

    looper.StartRecording(true);
    Block();
    Check(looper.GetState() == Looper::State::EMPTY, "quantized without a clock: ignored");
    Check(!looper.CanRecordQuantized(), "quantized without a clock: refused");

    looper.StartRecording(false);
    const size_t start = frame;
    const size_t blocks = 1000; // 0.5 s
    Blocks(blocks);
    Check(looper.GetState() == Looper::State::RECORDING, "recording");
    looper.StopRecording();
    float first;
    Block(&first);
    Check(looper.GetState() == Looper::State::PLAYING, "stop: plays at once");
    const size_t length = blocks * kBlock; // the stop lands at the start of the next block

    // past the loop-point crossfade, playback is the recording, frame for frame (int16)
    bool same = true;
    float worst = 0.f;
    size_t pos = kBlock;
    for (size_t b = 0; b < blocks - 2; b++)
    {
        float in_l[kBlock] = {}, in_r[kBlock] = {}, out_l[kBlock], out_r[kBlock];
        looper.Process(in_l, in_r, out_l, out_r, kBlock);
        for (size_t i = 0; i < kBlock; i++, pos++)
        {
            if (pos % length < kXfadeFrames)
                continue;
            const float want = Input(start + pos % length);
            const float err = fabsf(out_l[i] - want);
            worst = err > worst ? err : worst;
            same = same && err < 1e-4f && fabsf(out_r[i] + want) < 1e-4f;
        }
    }
    Check(same, "playback: the recording, frame for frame");
    const float p = looper.GetPosition();
    const float want_p = static_cast<float>(pos % length) / length;
    Check(fabsf(p - want_p) < 1e-3f, "position: frames played over the length");
    // around the loop, back at the start
    Blocks(2);
    Check(looper.GetPosition() < .01f, "the loop wraps at its length");
}

static void TestPauseAndErase()
{
    looper.TogglePlay();
    Blocks(200);
    Check(looper.GetState() == Looper::State::PAUSED, "pause");
    Check(fabsf(Block()) < 1e-4f, "paused: silent after the fade");
    looper.TogglePlay();
    Blocks(200);
    Check(looper.GetState() == Looper::State::PLAYING && fabsf(Block()) > 1e-3f, "play again: audible");

    looper.Erase();
    Block();
    Check(looper.GetState() == Looper::State::PAUSED, "erase: fades out first");
    Blocks(200);
    Check(looper.GetState() == Looper::State::EMPTY, "erase: then empty");
    looper.TogglePlay();
    Block();
    Check(looper.GetState() == Looper::State::EMPTY, "play on an empty looper: nothing");
}

/** A speed step or reset, picked up by the next block */
static void Step(int dir)
{
    looper.StepSpeed(dir);
    Block();
}

static void TestSpeed()
{
    looper.ResetSpeed();
    Block();
    Check(looper.GetSpeed() == 1.f, "speed: 1x");
    Step(1);
    Check(fabsf(looper.GetSpeed() - powf(2.f, 7.f / 12.f)) < 1e-5f, "up a fifth");
    Step(1);
    Check(fabsf(looper.GetSpeed() - 2.f) < 1e-5f, "up to 2x");
    Step(1);
    Check(fabsf(looper.GetSpeed() - 2.f) < 1e-5f, "no further than 2x");

    looper.ResetSpeed();
    Block();
    for (int i = 0; i < 8; i++)
        Step(-1);
    Check(fabsf(looper.GetSpeed() - 1.f / 16.f) < 1e-6f, "8 steps down: 1/16x");
    Step(-1);
    Check(fabsf(looper.GetSpeed() + 1.f / 16.f) < 1e-6f, "past 1/16x: reverse at the same speed");
    Step(-1);
    Check(looper.GetSpeed() < -1.f / 16.f, "further left: faster in reverse");
    looper.ResetSpeed();
    Block();
    Check(looper.GetSpeed() == 1.f, "reset: 1x forward");

    // several steps within one block all count, a reset drops the ones before it
    looper.StepSpeed(1);
    looper.StepSpeed(1);
    Block();
    Check(fabsf(looper.GetSpeed() - 2.f) < 1e-5f, "two steps in one block: both");
    looper.StepSpeed(-1);
    looper.ResetSpeed();
    looper.StepSpeed(1);
    Block();
    Check(fabsf(looper.GetSpeed() - powf(2.f, 7.f / 12.f)) < 1e-5f, "step, reset, step in one block: one step from 1x");
    looper.ResetSpeed();
    Block();
}

/** A semitone step, picked up by the next block */
static void Semi(int dir, int n = 1)
{
    for (int i = 0; i < n; i++)
        looper.StepSemitone(dir);
    Block();
}

static bool At(float speed) { return fabsf(looper.GetSpeed() - speed) < 1e-5f * fabsf(speed); }
static float Semis(int semis) { return powf(2.f, semis / 12.f); }

/** Semitone steps, and the ladder's steps from a point between two rungs */
static void TestSemitones()
{
    looper.ResetSpeed();
    Block();
    Semi(1);
    Check(At(Semis(1)), "semitone: up one");
    Semi(1, 11);
    Check(At(2.f), "semitone: 12 up: 2x");
    Semi(1);
    Check(At(2.f), "semitone: no further than 2x");
    looper.ResetSpeed();
    Block();
    Semi(-1, 48);
    Check(At(1.f / 16.f), "semitone: 48 down: 1/16x");
    // on through the hold zone (#174): 2 steps standing, then reverse from 1/16x to 2x
    Semi(-1);
    Check(looper.GetSpeed() == 0.f && looper.Standing() && looper.GetSemitones() == -48,
          "semitone: past 1/16x: a standstill, CC 90 still at 1/16x");
    Semi(-1);
    Check(looper.GetSpeed() == 0.f && looper.Standing(), "semitone: a step on, still standing");
    Semi(-1);
    Check(At(-1.f / 16.f) && !looper.Standing(), "semitone: a third: reverse at 1/16x");
    Semi(-1, 70);
    Check(At(-2.f), "semitone: on to 2x reverse, no further");
    Semi(1, 60);
    Check(At(-1.f / 16.f), "semitone: right from 2x reverse: slower, to 1/16x");
    Semi(1);
    Check(looper.GetSpeed() == 0.f && looper.Standing(), "semitone: right past 1/16x reverse: standing");
    Semi(-1);
    Check(At(-1.f / 16.f), "semitone: back left out of the zone: 1/16x reverse again");
    Semi(1, 3);
    Check(At(1.f / 16.f) && looper.GetSemitones() == -48, "semitone: right through the zone: 1/16x forward");
    Semi(-1, 2);
    Step(1);
    Check(At(1.f / 16.f), "ladder: from a standstill right: 1/16x forward");
    Semi(-1, 2);
    Step(-1);
    Check(At(-1.f / 16.f), "ladder: from a standstill left: 1/16x reverse");
    Semi(1, 2);
    looper.SetSemitones(-40);
    Block();
    Check(At(-Semis(-40)) && !looper.Standing(), "CC 90 at a standstill: those semitones, the way it last ran");

    // the ladder from between two rungs: the nearest rung in the turn's direction
    looper.ResetSpeed();
    Semi(1, 3);
    Step(1);
    Check(At(Semis(7)), "ladder: from +3 right: +7");
    looper.ResetSpeed();
    Semi(1, 3);
    Step(-1);
    Check(At(1.f), "ladder: from +3 left: 0");
    looper.ResetSpeed();
    Semi(1, 9);
    Step(1);
    Check(At(2.f), "ladder: from +9 right: +12");
    looper.ResetSpeed();
    Semi(-1, 47);
    Step(-1);
    Check(At(1.f / 16.f), "ladder: from -47 left: -48, still forward");
    Step(-1);
    Check(At(-1.f / 16.f), "ladder: then reverse at the same speed");

    // in reverse, semitones turn as the ladder does: right slower, left faster; right past
    // 1/16x the hold zone (#174)
    Semi(1);
    Check(looper.Standing(), "semitone, reverse: right at 1/16x: the hold zone");
    Semi(-1);
    Check(At(-1.f / 16.f), "semitone, reverse: and left again: 1/16x reverse");
    Semi(-1);
    Check(At(-Semis(-47)), "semitone, reverse: left: faster");
    Step(-1);
    Check(At(-Semis(-41)), "ladder, reverse: from -47 left: -41");

    // a reset drops the semitones posted before it
    Semi(1);
    looper.StepSemitone(1);
    looper.ResetSpeed();
    Block();
    Check(looper.GetSpeed() == 1.f, "semitone, reset in the same block: 1x forward");
}

/** A fresh unquantized loop of blocks blocks, playing */
static void Record(size_t blocks)
{
    looper.StartRecording(false);
    Blocks(blocks);
    looper.StopRecording();
    Block();
}

/** Plays until the looper stops playing; the position it last played at */
static float PlayUntilErased(size_t max_blocks)
{
    float last = looper.GetPosition();
    for (size_t b = 0; b < max_blocks && looper.GetState() == Looper::State::PLAYING; b++)
    {
        last = looper.GetPosition();
        Block();
    }
    return last;
}

static void TestEraseAtEnd()
{
    Record(100);
    Blocks(20);
    looper.EraseAtEnd();
    Block();
    Check(looper.IsErasePending() && looper.GetState() == Looper::State::PLAYING,
          "erase at the end: waits, playing");
    const float last = PlayUntilErased(200);
    Check(last > .95f && looper.IsErasing() && !looper.IsErasePending(),
          "erase at the end: fades out at the loop point");
    Blocks(200);
    Check(looper.GetState() == Looper::State::EMPTY, "erase at the end: then empty");

    Record(100);
    looper.EraseAtEnd();
    Block();
    looper.CancelErase();
    Block();
    Blocks(300);
    Check(looper.GetState() == Looper::State::PLAYING && !looper.IsErasePending(),
          "erase at the end, cancelled: plays on past the loop point");
    looper.Erase();
    Blocks(200);
    Check(looper.GetState() == Looper::State::EMPTY, "erase: at once");

    // in reverse, the loop point is crossed at the start
    Record(100);
    for (int i = 0; i < 9; i++)
        Step(-1);
    Blocks(2000); // the glide down into reverse
    looper.EraseAtEnd();
    Block();
    const float first = PlayUntilErased(40000);
    Check(first < .05f && looper.IsErasing(), "erase at the end, in reverse: at the loop's start");
    Blocks(200);
    Check(looper.GetState() == Looper::State::EMPTY, "... then empty");
    looper.ResetSpeed();
    Block();

    Record(100);
    looper.TogglePlay();
    Blocks(200);
    looper.EraseAtEnd();
    Block();
    Check(looper.GetState() == Looper::State::EMPTY, "erase at the end, paused and silent: at once");
}

/** Where the read head is, in whole frames */
static long Frame()
{
    return lroundf(looper.GetPosition() * static_cast<float>(looper.GetLength()));
}

/** After a glide the read head runs at the step's speed exactly, not a hair beside it: the
 *  slew used to stop short of it, ~0.06% fast after a fifth up and back to 1x */
static void TestGlideArrives()
{
    looper.Init(mem, &midi_clock);
    Record(1000); // 24000 frames
    const size_t kGlide = 5000; // 2.5 s, far past the ~0.2 s glide
    Step(1);
    Blocks(kGlide);
    looper.ResetSpeed();
    Blocks(kGlide);
    Check(looper.GetActualSpeed() == 1.f, "glide back to 1x: exactly 1x");
    const long from = Frame();
    const size_t kRun = 9000; // 216000 frames, 9 passes
    Blocks(kRun);
    const long moved = ((Frame() - from) % 24000 + 24000) % 24000;
    Check(moved <= 1 || moved >= 23999,
          "glide back to 1x: the read head moves a frame a sample (9 passes, within a frame)");

    Step(1);
    Step(1);
    Blocks(kGlide);
    Check(looper.GetActualSpeed() == looper.GetSpeed() && looper.GetSpeed() == 2.f,
          "glide up to 2x: exactly 2x");
    looper.ResetSpeed();
    Block();
    for (int i = 0; i < 8; i++)
        Step(-1);
    Blocks(kGlide * 3);
    Check(looper.GetActualSpeed() == looper.GetSpeed() && At(1.f / 16.f),
          "glide down to 1/16x: exactly the step's speed");
    Step(-1);
    Blocks(kGlide);
    Check(looper.GetActualSpeed() == looper.GetSpeed() && At(-1.f / 16.f),
          "through 1/16x into reverse: exactly -1/16x");
    for (int i = 0; i < 8; i++)
        Step(-1);
    Blocks(kGlide * 3);
    Check(looper.GetActualSpeed() == looper.GetSpeed() && At(-1.f),
          "on to 1x in reverse: exactly -1x");
    looper.Erase();
    Blocks(200);
}

/** One session from Init, every output sample (left, then right, a block at a time) into out:
 *  a recording played a fifth up from the start and back, scrambled, paused and scrubbed both ways, erased,
 *  then a shorter one, slowed down into reverse, erased */
static void Session(std::vector<float>& out)
{
    looper.Init(mem, &midi_clock);
    frame = 0;
    const auto run = [&out](size_t blocks) {
        for (size_t b = 0; b < blocks; b++)
        {
            float in_l[kBlock], in_r[kBlock], out_l[kBlock], out_r[kBlock];
            for (size_t i = 0; i < kBlock; i++)
            {
                in_l[i] = Input(frame + i);
                in_r[i] = -in_l[i];
            }
            frame += kBlock;
            looper.Process(in_l, in_r, out_l, out_r, kBlock);
            out.insert(out.end(), out_l, out_l + kBlock);
            out.insert(out.end(), out_r, out_r + kBlock);
        }
    };
    // the glide from the loop's first sample on reads ahead into the post-roll still being
    // written
    looper.StartRecording(false);
    run(500);
    looper.StopRecording();
    looper.StepSpeed(1);
    run(500);
    looper.ResetSpeed();
    run(800);
    looper.Scramble(3000);
    run(100);
    looper.Scramble(0);
    run(100);
    looper.TogglePlay();
    run(50);
    looper.Scrub(3);
    run(300);
    looper.Scrub(-6);
    run(300);
    looper.TogglePlay();
    run(100);
    looper.Erase();
    run(200);
    looper.StartRecording(false);
    run(120);
    looper.StopRecording();
    run(200);
    for (int i = 0; i < 9; i++)
        looper.StepSpeed(-1);
    run(2000);
    looper.Erase();
    run(200);
}

/** The loop's memory isn't cleared at boot (chompi_main.cpp's SDRAM_NOINIT): whatever it
 *  holds, the looper sounds the same, as it reads only what it recorded */
/** One block through the looper, all of its left output */
static void BlockOut(float* out_l)
{
    float in_l[kBlock], in_r[kBlock], out_r[kBlock];
    for (size_t i = 0; i < kBlock; i++)
    {
        in_l[i] = Input(frame + i);
        in_r[i] = -in_l[i];
    }
    frame += kBlock;
    looper.Process(in_l, in_r, out_l, out_r, kBlock);
}

/** #168: the play fade gets back to 1 exactly after a pause, so the loop plays as recorded,
 *  sample for sample, as it does before any pause; it used to stall at ~0.999985 */
static void TestFadeSettles()
{
    looper.Init(mem, &midi_clock);
    Record(1000); // 24000 frames, the first pass starting on a block
    std::vector<float> first(24000);
    for (size_t b = 0; b < 1000; b++)
    {
        const long at = Frame();
        float out[kBlock];
        BlockOut(out);
        for (size_t i = 0; i < kBlock; i++)
            first[(at + static_cast<long>(i)) % 24000] = out[i];
    }
    looper.TogglePlay();
    Blocks(200);
    looper.TogglePlay();
    Blocks(2000);
    bool exact = true;
    for (size_t b = 0; b < 1000; b++)
    {
        const long at = Frame();
        float out[kBlock];
        BlockOut(out);
        for (size_t i = 0; i < kBlock; i++)
            exact &= out[i] == first[(at + static_cast<long>(i)) % 24000];
    }
    Check(exact, "after a pause: plays as before it, sample for sample (#168)");
}

/** #174: into the hold zone the speed glides to 0 exactly, and the level with it; there the
 *  loop is silent; out of it the level comes back with the speed. No click either way */
static void TestStandstill()
{
    looper.Init(mem, &midi_clock);
    Record(1000);
    Semi(-1, 48);
    Blocks(6000);
    Check(At(1.f / 16.f) && looper.GetActualSpeed() == looper.GetSpeed(), "standstill: at 1/16x first");
    // the loop's left is a ramp up, which the read head climbs, ever so slowly
    float out[kBlock], last = 0.f, step = 0.f;
    BlockOut(out);
    last = out[kBlock - 1];
    const float before = fabsf(last);
    Semi(-1);
    // the level against the speed while it glides: about the speed's share of 1/16x
    float level_at_half = -1.f;
    bool silent = false;
    for (size_t b = 0; b < 8000; b++)
    {
        BlockOut(out);
        for (size_t i = 0; i < kBlock; i++)
        {
            step = std::max(step, fabsf(out[i] - last));
            last = out[i];
        }
        if (level_at_half < 0.f && looper.GetActualSpeed() < 1.f / 32.f)
            level_at_half = fabsf(out[kBlock - 1]);
        silent = out[kBlock - 1] == 0.f && out[0] == 0.f;
    }
    printf("      standstill: %.4f before, %.4f at half the speed, largest step %.5f\n", before,
           level_at_half, step);
    Check(looper.GetActualSpeed() == 0.f && looper.Standing(), "standstill: glided to 0x exactly");
    Check(silent, "standstill: the loop silent, no DC from the stopped read head");
    Check(level_at_half > before * .4f && level_at_half < before * .6f,
          "standstill: the level follows the speed down, half at half of 1/16x");
    Check(step < .01f, "standstill: into it without a click");
    Semi(-1, 2);
    step = 0.f;
    bool back = false;
    for (size_t b = 0; b < 8000; b++)
    {
        BlockOut(out);
        for (size_t i = 0; i < kBlock; i++)
        {
            // the first 0.75 s, the speed up to 98%: later the read head reaches the ramp's
            // own jump
            if (b < 1500)
                step = std::max(step, fabsf(out[i] - last));
            last = out[i];
        }
        back |= out[kBlock - 1] != 0.f;
    }
    Check(At(-1.f / 16.f) && looper.GetActualSpeed() == looper.GetSpeed() && back && step < .01f,
          "standstill: out of it, reverse at 1/16x, heard again, no click");
}

static void TestUnclearedMemory()
{
    std::vector<float> zeroed, garbage;
    std::fill(mem, mem + kLoopMemSize, 0);
    Session(zeroed);
    uint32_t x = 12345;
    for (int16_t& m : mem)
    {
        x = x * 1664525u + 1013904223u;
        m = static_cast<int16_t>(x >> 16);
    }
    Session(garbage);
    bool sound = false;
    for (float v : zeroed)
        sound = sound || fabsf(v) > .1f;
    Check(sound && zeroed.size() == garbage.size()
              && memcmp(zeroed.data(), garbage.data(), zeroed.size() * sizeof(float)) == 0,
          "memory not cleared before Init: the loop sounds the same, sample for sample");
}

/** Records amp * a 500 Hz sine (left; the right at half) for blocks, plays it back once: the
 *  playback's peak, and its worst departure from the sine scaled by the gain the peak shows
 *  (past the start's attack and the loop-point crossfade) */
static void SineLoop(float amp, size_t blocks, float& peak, float& worst)
{
    looper.Erase();
    for (int b = 0; b < 1000 && looper.GetState() != Looper::State::EMPTY; b++)
        Block();
    looper.StartRecording(false);
    float in_l[kBlock], in_r[kBlock], out_l[kBlock], out_r[kBlock];
    size_t n = 0;
    for (size_t b = 0; b <= blocks; b++)
    {
        if (b == blocks)
            looper.StopRecording();
        for (size_t i = 0; i < kBlock; i++, n++)
        {
            in_l[i] = amp * sinf(2.f * 3.14159265f * 500.f * n / 48000.f);
            in_r[i] = .5f * in_l[i];
        }
        looper.Process(in_l, in_r, out_l, out_r, kBlock);
    }
    std::vector<float> got;
    for (size_t b = 0; b < blocks - 1; b++)
    {
        float zl[kBlock] = {}, zr[kBlock] = {};
        looper.Process(zl, zr, out_l, out_r, kBlock);
        got.insert(got.end(), out_l, out_l + kBlock);
    }
    peak = 0.f;
    for (size_t i = got.size() / 2; i < got.size(); i++)
        peak = fmaxf(peak, fabsf(got[i]));
    const float gain = peak / amp;
    worst = 0.f;
    for (size_t i = got.size() / 2; i < got.size(); i++)
        worst = fmaxf(worst, fabsf(got[i] - gain * amp * sinf(2.f * 3.14159265f * 500.f * (i + kBlock) / 48000.f)));
}

/** The soft limit (#152): a hot input is held under kLoopCeiling, its shape kept (not
 *  clipped); one under the ceiling is recorded as it came; a loud recording leaves the next
 *  one alone */
static void TestSoftLimit()
{
    looper.Init(mem, &midi_clock);
    float peak, worst;
    SineLoop(.9f, 1000, peak, worst);
    Check(fabsf(peak - .9f) < .001f && worst < .001f, "under the ceiling: recorded as it came");
    SineLoop(2.25f, 1000, peak, worst);
    printf("      2.25 in: peak %.4f, worst departure from a sine %.5f\n", peak, worst);
    Check(peak <= kLoopCeiling + .0001f && peak > kLoopCeiling - .01f, "a hot input: held at the ceiling");
    Check(worst < .002f, "a hot input: a sine still, not clipped");
    SineLoop(.5f, 1000, peak, worst);
    Check(fabsf(peak - .5f) < .001f && worst < .001f, "after a loud recording: the next as it came");
}

/** #372: a chord recorded hot, 220 + 330 Hz at amp each (the right at half), 3 s, played back:
 *  its spectrum over 1 s from the middle (1 Hz bins, so both tones and every sideband of a gain
 *  that moves with the chord's 10 Hz pattern sit on a bin), the loudest bin away from the two
 *  tones in dB under the 220 Hz one, and the playback's peak */
static float ChordSidebands(float amp, float& peak)
{
    looper.Erase();
    for (int b = 0; b < 1000 && looper.GetState() != Looper::State::EMPTY; b++)
        Block();
    looper.StartRecording(false);
    const size_t blocks = 3 * 48000 / kBlock;
    float in_l[kBlock], in_r[kBlock], out_l[kBlock], out_r[kBlock];
    size_t n = 0;
    for (size_t b = 0; b <= blocks; b++)
    {
        if (b == blocks)
            looper.StopRecording();
        for (size_t i = 0; i < kBlock; i++, n++)
        {
            const double t = n / 48000.;
            in_l[i] = amp * static_cast<float>(sin(2 * M_PI * 220 * t) + sin(2 * M_PI * 330 * t));
            in_r[i] = .5f * in_l[i];
        }
        looper.Process(in_l, in_r, out_l, out_r, kBlock);
    }
    std::vector<float> got;
    for (size_t b = 0; b < blocks - 1; b++)
    {
        float zl[kBlock] = {}, zr[kBlock] = {};
        looper.Process(zl, zr, out_l, out_r, kBlock);
        got.insert(got.end(), out_l, out_l + kBlock);
    }
    const size_t from = 48000, len = 48000;
    peak = 0.f;
    for (size_t i = from; i < from + len; i++)
        peak = fmaxf(peak, fabsf(got[i]));
    double tone = 0., side = 0.;
    for (int f = 1; f <= 2000; f++)
    {
        double re = 0., im = 0.;
        const double w = 2 * M_PI * f / 48000.;
        for (size_t i = 0; i < len; i++)
        {
            re += got[from + i] * cos(w * i);
            im -= got[from + i] * sin(w * i);
        }
        const double mag = sqrt(re * re + im * im);
        if (f == 220)
            tone = mag;
        else if (f != 330)
            side = std::max(side, mag);
    }
    return static_cast<float>(20 * log10(side / tone + 1e-30));
}

/** #372: a hot chord's sampled peaks differ from one period to the next; the soft limit holds
 *  across them, so its gain stays put and adds no sidebands (it ran out between them: -51 dB) */
static void TestChordLimit()
{
    looper.Init(mem, &midi_clock);
    float peak;
    const float hot = ChordSidebands(.9f, peak);
    printf("      220 + 330 Hz at .9 each: sidebands %.1f dB under the tone, peak %.4f\n", hot, peak);
    Check(peak <= kLoopCeiling + .0001f && peak > kLoopCeiling - .02f, "a hot chord: held at the ceiling");
    Check(hot < -90.f, "a hot chord: the limit's gain stays put, no sidebands (#372)");
    const float cool = ChordSidebands(.4f, peak);
    printf("      220 + 330 Hz at .4 each: sidebands %.1f dB\n", cool);
    Check(cool < -90.f, "a chord under the ceiling: no sidebands either");
}

/** Loops from the card (Switch), each a level held, its head again after its end */
static const size_t kShort = 192; // 4 ms
static int16_t loop_a[(kShort + kXfadeFrames) * 2], loop_b[(kShort + kXfadeFrames) * 2],
    loop_c[(kShort + kXfadeFrames) * 2];
static void Level(int16_t* loop, float v)
{
    for (size_t i = 0; i < (kShort + kXfadeFrames) * 2; i++)
        loop[i] = static_cast<int16_t>(v * 32767.f);
}
static float last_l = 0.f;
/** The largest step between two samples (left) over blocks, from the last one before */
static float Steps(size_t blocks)
{
    float in[kBlock] = {}, out_l[kBlock], out_r[kBlock], worst = 0.f;
    for (size_t b = 0; b < blocks; b++)
    {
        looper.Process(in, in, out_l, out_r, kBlock);
        for (size_t i = 0; i < kBlock; i++)
        {
            worst = std::max(worst, fabsf(out_l[i] - last_l));
            last_l = out_l[i];
        }
    }
    return worst;
}

/** #318: a switch while the other voice still fades (a switch's fade-out, the crossfader
 *  held or let go) ends that fade early but smoothly, and the loop it leaves fades out from
 *  the gain it's heard at: no step. A hard step here is ~.1-.3 of these levels; the tail's
 *  2 ms fade steps under .01 */
static void TestSwitchDuringFade()
{
    looper.Init(mem, &midi_clock);
    Level(loop_a, .5f);
    Level(loop_b, -.4f);
    Level(loop_c, .25f);
    looper.Switch(loop_a, kShort, 0, Looper::kSwitchNow);
    Steps(300);
    // at once, then at the loop point 192 frames later, 40 % into the first's fade
    uint32_t was = looper.Switches();
    looper.Switch(loop_b, kShort, 0, Looper::kSwitchNow);
    float step = Steps(1);
    looper.Switch(loop_c, kShort, 0, looper.Wraps() + 1);
    step = std::max(step, Steps(60));
    printf("      at the loop point in a switch's fade: %u switches, largest step %.4f\n", looper.Switches() - was, step);
    Check(looper.Switches() - was == 2 && step < .01f, "a switch at the loop point within a switch's fade-out: no step");
    // the crossfader held at the middle, and a switch at once
    Steps(300);
    looper.Switch(loop_a, kShort, 0, Looper::kSwitchFade);
    Steps(2);
    looper.Fade(.5f);
    Steps(60);
    was = looper.Switches();
    looper.Switch(loop_b, kShort, 0, Looper::kSwitchNow);
    step = Steps(60);
    printf("      the crossfader held, a switch at once: %u switches, largest step %.4f\n", looper.Switches() - was, step);
    Check(looper.Switches() - was == 1 && step < .01f, "a switch at once with the crossfader held: no step");
    // the crossfader let go from the middle, and a switch at the loop point while it glides
    Steps(300);
    looper.Switch(loop_c, kShort, 0, Looper::kSwitchFade);
    Steps(2);
    looper.Fade(.5f);
    Steps(60);
    was = looper.Switches();
    looper.EndFade(false);
    step = Steps(1);
    looper.Switch(loop_a, kShort, 0, looper.Wraps() + 1);
    step = std::max(step, Steps(60));
    printf("      the crossfader let go, a switch at the loop point: %u switches, largest step %.4f\n",
           looper.Switches() - was, step);
    Check(looper.Switches() - was == 1 && step < .01f, "a switch at the loop point while the crossfader glides home: no step");
}

int main()
{
    TestRecordAndPlay();
    TestPauseAndErase();
    TestSpeed();
    TestSemitones();
    TestEraseAtEnd();
    TestGlideArrives();
    TestFadeSettles();
    TestStandstill();
    TestUnclearedMemory();
    TestSoftLimit();
    TestChordLimit();
    TestSwitchDuringFade();
    return Finish();
}
