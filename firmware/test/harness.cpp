// FRIZZ engine harness: runs a fixed script of key presses and knob turns through
// PassthroughEngine on the host and writes every output sample and FX meter to a file, so two
// versions of the engine can be compared (see README.md). Built and run by run.sh.
//
// The script, one segment of 3s per FX plus four (14 with the folder, 16 with wow & flutter and
// the tape stop) at 48kHz in 24-sample blocks:
//  - each FX on its own, on for 2.5s with a random knob turned every 0.25s; the tape stop
//    pressed and released every 0.5s instead, so it stops and spins up
//  - the inserts together, then everything twice, then 3s of tails. Without the tape stop,
//    which would silence them and the sends
// Every parameter starts at 0.5, the master compressor's amount at 0.3. The input is a 110Hz
// saw, a gated 2kHz sine and a little noise, all deterministic, as are the effects' random
// numbers (seeded).
//
// Environment:
//  NOFX=1    never switches an FX on (shows whether a segment exercises its FX)
//  STRESS=1  everything on for the whole run, the resonator at full feedback with the
//            filter's resonance and the flanger's and shifter's feedback at the top
//
// Output, per block: 4 x 24 floats (headphone L/R, master L/R), then the FX meters in the
// order of kAll below, and <out>.names with kAll's names, one per line. compare.py reads the
// names; for older harnesses without them it tells the layouts apart by the file's size.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include "passthroughEngine.h"
#include "FxParams.h"

static const float kSr = 48000.f;
static const size_t kBlock = 24;

static int16_t loop_mem[kLoopMemSize];
static const size_t kDelayFrames = 480000;
static float delay_mem[kDelayFrames * 2];
static const size_t kFreezerFrames = 240000;
static float freezer_mem_l[kFreezerFrames];
static float freezer_mem_r[kFreezerFrames];
static daisysp::Reverb reverb;
#if __has_include("FxTapeStop.h")
static const size_t kTapeStopFrames = 1u << 19;
static float tapestop_mem_l[kTapeStopFrames];
static float tapestop_mem_r[kTapeStopFrames];
#endif
static chompi::MidiClock midi_clock;
static PassthroughEngine engine;

// The parameter on page 1's knob p (0-3) as the knobs were before #117 moved stereo from
// page 1's knob 4 to page 2's knob 2: the harness turns the same ones as on the builds before
static size_t Knob(size_t fx, size_t p)
{
    // the sends' level, page 1's knob 4 until #172, is on page 2's knob 4 now
    if (p == chompi::kFxOwnKnob && (fx == chompi::FX_DELAY || fx == chompi::FX_REVERB))
        return chompi::FxOutput::kLevel;
    return p == chompi::kFxOwnKnob && chompi::HasStereo(fx) ? chompi::kFxStereoParam : p;
}

// knob p (one of page 1's, as they were before #117 and #172) to val: a send's level, a gain
// then, on the Level knob's dB scale now
static void SetKnob(size_t fx, size_t p, float val)
{
    const size_t param = Knob(fx, p);
    const bool level = (fx == chompi::FX_DELAY || fx == chompi::FX_REVERB)
                       && param == chompi::FxOutput::kLevel;
    engine.SetFxParam(fx, param, level ? chompi::LevelKnob(val) : val);
}

static uint32_t lcg = 12345;
static float Rnd()
{
    lcg = lcg * 1664525u + 1013904223u;
    return static_cast<float>(lcg >> 8) / 16777216.f;
}

// The inserts (and the resonator), then the two sends. The folder only where that version
// of the engine has it, so older versions still build and run as they did.
static const size_t kAll[] = {chompi::FX_FILTER,  chompi::FX_CRUSHER, chompi::FX_FREEZER,
                              chompi::FX_SLICER,  chompi::FX_FLANGER, chompi::FX_SHIFTER,
                              chompi::FX_RESONATOR,
#if __has_include("FxFolder.h")
                              chompi::FX_FOLDER,
#endif
#if __has_include("FxTapeStop.h")
                              chompi::FX_WARBLE,  chompi::FX_TAPESTOP,
#endif
                              chompi::FX_DELAY,   chompi::FX_REVERB};
static const size_t kNum = sizeof(kAll) / sizeof(kAll[0]);
static const size_t kNumInserts = kNum - 2;

// Left out of the combined segments and STRESS: a stopped tape would silence them
static bool Combined(size_t fx)
{
#if __has_include("FxTapeStop.h")
    return fx != chompi::FX_TAPESTOP;
#else
    (void)fx;
    return true;
#endif
}

int main(int argc, char** argv)
{
    if (argc < 2)
        return 1;
    FILE* f = fopen(argv[1], "wb");
#if __has_include("FxScenes.h")
    // the segments' FX by name, for compare.py, so a new FX doesn't break the comparison
    const std::string names_path = std::string(argv[1]) + ".names";
    FILE* names = fopen(names_path.c_str(), "w");
    for (size_t i = 0; i < kNum; i++)
        fprintf(names, "%s\n", chompi::kFxNames[kAll[i]]);
    fclose(names);
#endif
    srand(1);

    engine.Init(kSr, loop_mem, &midi_clock, delay_mem, kDelayFrames, &reverb,
                freezer_mem_l, freezer_mem_r, kFreezerFrames
#if __has_include("FxTapeStop.h")
                , tapestop_mem_l, tapestop_mem_r, kTapeStopFrames
#endif
                );
    engine.SetMainGain(.75f);
    engine.SetInputGain(false, .75f);
    // the master compressor at work: its amount where the old one-knob compressor was
#if __has_include("MasterComp.h")
    engine.SetCompParam(chompi::MasterComp::kThreshold, .3f);
#else
    engine.SetFinalComp(.3f);
#endif
    engine.SetMix(0.f);
    // the defaults NormalPage pushes don't matter here: every parameter is set below
    for (size_t i = 0; i < kNum; i++)
        for (size_t p = 0; p < 4; p++)
            SetKnob(kAll[i], p, .5f);

    if (getenv("STRESS"))
    {
        // resonator at full feedback with the filter's resonance and the flanger's and
        // shifter's feedback at the top, all inside its loop, plus everything else on
        for (size_t i = 0; i < kNum; i++)
            engine.SetFxOn(kAll[i], Combined(kAll[i]));
        engine.SetFxParam(chompi::FX_RESONATOR, 1, 1.f);
        engine.SetFxParam(chompi::FX_RESONATOR, 2, 1.f);
        engine.SetFxParam(chompi::FX_FILTER, 0, .2f);
        engine.SetFxParam(chompi::FX_FILTER, 1, 1.f);
        engine.SetFxParam(chompi::FX_FLANGER, 2, .5f);
        engine.SetFxParam(chompi::FX_FLANGER, 1, 1.f);
        engine.SetFxParam(chompi::FX_SHIFTER, 1, 1.f);
        engine.SetFxParam(chompi::FX_DELAY, 1, 1.f);
        engine.SetFxParam(chompi::FX_REVERB, 0, 1.f);
        SetKnob(chompi::FX_REVERB, 3, 1.f);
    }
    // per-fx solo segments of 3s, then 3s of all inserts, 6s of everything, then tails
    const size_t seg = static_cast<size_t>(3.f * kSr / kBlock);
    const size_t total = (kNum + 4) * seg;
    const size_t knob_every = static_cast<size_t>(.25f * kSr / kBlock);

    float inl[kBlock], inr[kBlock], zero[kBlock] = {};
    float o0[kBlock], o1[kBlock], o2[kBlock], o3[kBlock];
    const float* in[4] = {zero, zero, inl, inr};
    float* out[4] = {o0, o1, o2, o3};
    double phase1 = 0, phase2 = 0;
    uint32_t noise = 777;

    for (size_t b = 0; b < total; b++)
    {
        const size_t s = b / seg, t = b % seg;
        bool on[kNum] = {};
        if (s < kNum)
            on[s] = Combined(kAll[s]) ? t < seg * 5 / 6 : (t * 6 / seg) % 2 == 0;
        else if (s < kNum + 1)
            for (size_t i = 0; i < kNumInserts; i++) on[i] = Combined(kAll[i]) && t < seg * 5 / 6;
        else if (s < kNum + 3)
            for (size_t i = 0; i < kNum; i++) on[i] = Combined(kAll[i]);

        static bool was_on[kNum] = {};
        for (size_t i = 0; i < kNum; i++)
        {
            if (getenv("STRESS"))
                continue;
            if (on[i] != was_on[i] && !getenv("NOFX"))
                engine.SetFxOn(kAll[i], on[i]);
            was_on[i] = on[i];
            if (on[i] && t % knob_every == 0)
                SetKnob(kAll[i], static_cast<size_t>(Rnd() * 4.f) % 4, Rnd());
        }

        for (size_t i = 0; i < kBlock; i++)
        {
            // a 110Hz saw, a gated 2kHz sine, a little noise
            phase1 += 110.0 / kSr; if (phase1 >= 1) phase1 -= 1;
            phase2 += 2000.0 / kSr;
            noise = noise * 1103515245u + 12345u;
            const float n = (static_cast<float>(noise >> 9) / 4194304.f - 1.f) * .02f;
            const float gate = ((b / 400) % 2) ? 1.f : 0.f;
            const float saw = static_cast<float>(phase1 * 2 - 1) * .3f;
            const float sine = static_cast<float>(sin(2 * M_PI * phase2)) * .2f * gate;
            inl[i] = saw + sine + n;
            inr[i] = saw * .8f - sine + n;
        }
        engine.Process(in, out, kBlock);

        fwrite(o0, sizeof(float), kBlock, f);
        fwrite(o1, sizeof(float), kBlock, f);
        fwrite(o2, sizeof(float), kBlock, f);
        fwrite(o3, sizeof(float), kBlock, f);
        for (size_t i = 0; i < kNum; i++)
        {
            const float m = engine.GetFxLevel(kAll[i]);
            fwrite(&m, sizeof(float), 1, f);
        }
    }
    fclose(f);
    return 0;
}
