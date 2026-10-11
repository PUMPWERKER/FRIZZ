// timing.h: what midi.cpp, sync.cpp and midiout.cpp share: the virtual CHOMPI (twin.h) with up
// to two MIDI clocks into it (clockgen.h, the jack's and USB's), read out block by block with
// twin::Probe(), and each case on a freshly booted device (a forked process, cases.h).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "cases.h"
#include "check.h"
#include "clockgen.h"
#include "twin.h"

using namespace twin;

static const double kSr = 48000.;

static ClockGen trs, usb; // a sequencer on the jack and a DAW over USB
// called after every block, to follow what the firmware does
static std::function<void()> each_block;

// a sine into AUX while sine_amp > 0, and the master out's largest step between two samples
// (either channel) since max_step was last set to 0
static float sine_amp = 0.f, sine_hz = 220.f, sine_phase = 0.f;
static float max_step = 0.f, last_out[2] = {0.f, 0.f};

static void RunBlocks(size_t n)
{
    float in[kBlockSize * kChannels] = {}, out[kBlockSize * kChannels];
    for (size_t b = 0; b < n; b++)
    {
        trs.Step(BlockMs());
        usb.Step(BlockMs());
        for (size_t i = 0; i < kBlockSize; i++)
        {
            const float v = sine_amp * sinf(sine_phase);
            sine_phase = fmodf(sine_phase + 2.f * float(M_PI) * sine_hz / kSampleRate,
                               2.f * float(M_PI));
            in[i * kChannels + 2] = in[i * kChannels + 3] = v;
        }
        Run(1, in, out);
        for (size_t i = 0; i < kBlockSize; i++)
            for (int c = 0; c < 2; c++)
            {
                const float v = out[i * kChannels + 2 + c];
                max_step = std::max(max_step, fabsf(v - last_out[c]));
                last_out[c] = v;
            }
        if (each_block)
            each_block();
    }
}
static void RunMs(double ms) { RunBlocks(static_cast<size_t>(ms * 2. + .5)); }
#include "twintest.h" // after RunMs, which its Tap runs

/** Runs until cond holds or max_ms has passed; whether it held */
static bool RunUntil(const std::function<bool()>& cond, double max_ms)
{
    for (size_t b = 0; b < max_ms * 2; b++)
    {
        if (cond())
            return true;
        RunBlocks(1);
    }
    return cond();
}

static ClockGen::Config Clock(double bpm, double jitter_ms = 0., bool on_usb = false,
                              double drift_ppm = 0., bool pairs = false, double pair_gap_ms = 0.)
{
    ClockGen::Config c;
    c.pairs = pairs;
    c.pair_gap_ms = pair_gap_ms;
    c.bpm = bpm;
    c.jitter_ms = jitter_ms;
    c.usb = on_usb;
    c.drift_ppm = drift_ppm;
    return c;
}
static void StartTrs(const ClockGen::Config& c) { trs.Start(c, BlockMs()); }
static void StartUsb(ClockGen::Config c)
{
    c.usb = true;
    usb.Start(c, BlockMs());
}

/** A line of measurements, indented under the checks */
#define Report(...)                                                                                \
    do                                                                                             \
    {                                                                                              \
        printf("      ");                                                                          \
        printf(__VA_ARGS__);                                                                       \
        printf("\n");                                                                              \
    } while (0)


// A measurement a case hands back to main, for a check over all of them (RunCases' summary):
// one that holds or fails in every case together, so a single case just inside a limit by
// chance doesn't count as a fix
static std::string results_path;
__attribute__((unused)) static void Record(const std::string& key, double value)
{
    if (FILE* f = fopen(results_path.c_str(), "a"))
    {
        fprintf(f, "%s %.9g\n", key.c_str(), value);
        fclose(f);
    }
}
static std::map<std::string, std::vector<double>> Recorded()
{
    std::map<std::string, std::vector<double>> r;
    if (FILE* f = fopen(results_path.c_str(), "r"))
    {
        char key[128];
        double v;
        while (fscanf(f, "%127s %lf", key, &v) == 2)
            r[key].push_back(v);
        fclose(f);
    }
    return r;
}
/** The largest |value| recorded under key, 0 if none */
__attribute__((unused)) static double Worst(const std::map<std::string, std::vector<double>>& r, const char* key)
{
    double w = 0.;
    auto it = r.find(key);
    if (it != r.end())
        for (double v : it->second)
            w = std::max(w, fabs(v));
    return w;
}

/** Runs every case on its own device, then summary (if any) on what they recorded; the exit
 *  code is main's */
static int RunCases(const std::function<void(const std::map<std::string, std::vector<double>>&)>&
                        summary = nullptr)
{
    results_path = RunDir() + "/results";
    RunAllCases();
    if (summary)
    {
        printf("over all cases:\n");
        summary(Recorded());
    }
    return Finish();
}
