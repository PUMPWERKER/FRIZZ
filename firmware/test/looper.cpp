// looper.cpp: checks the looper (Looper.h) on the host, without MIDI clock: a free recording
// plays back what was recorded at its length, play / pause, erase, the speed ladder and the
// refused quantized record, a glide that lands exactly on its speed, and memory that wasn't
// cleared at boot sounding the same as cleared. Exits 0 when everything passes. Run by unit.sh looper.
#include <cmath>
#include <cstdio>
#include <cstring>
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
    Semi(-1, 3);
    Check(At(1.f / 16.f), "semitone: past 1/16x: stays there, forward");

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

    // in reverse, semitones turn as the ladder does: right slower, left faster, no flip
    Semi(1);
    Check(At(-1.f / 16.f), "semitone, reverse: right at 1/16x: stays there, reverse");
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

int main()
{
    TestRecordAndPlay();
    TestPauseAndErase();
    TestSpeed();
    TestSemitones();
    TestEraseAtEnd();
    TestGlideArrives();
    TestUnclearedMemory();
    return Finish();
}
