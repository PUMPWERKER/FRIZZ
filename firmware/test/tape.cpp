// Tape FX check: wow & flutter (FxWarble.h) and the tape stop (FxTapeStop.h) on a 220Hz sine.
// Built and run by unit.sh tape.
//  - wow & flutter on its defaults, and the tape stop off, pass the input bit for bit
//  - the flutter's pitch wobble (by zero crossings) stays within its 2 pi f A bound, real
//    tape's at half the knob; TAPE's wow at the top stays finite and bounded
//  - a stop is silent once its time is up, on a linear and a brake curve; a spin-up, or none,
//    ends on the input bit for bit; releasing mid-stop and pressing mid-spin-up stay smooth
#include <cmath>
#include <vector>
#include "FxTapeStop.h"
#include "FxWarble.h"
#include "check.h"

using namespace chompi;

static const float kSr = 48000.f;
// a 220Hz sine at .5 moves at most .0144 a sample; a crossfade between unrelated points of it
// at most about twice that
static const float kMaxStep = .05f;

static float Sine(size_t i, float hz = 220.f)
{
    return .5f * sinf(2.f * static_cast<float>(M_PI) * hz * static_cast<float>(i) / kSr);
}

/** The largest and smallest frequency of x between from and to, cycle by cycle (rising zero
 *  crossings, interpolated) */
static void FreqRange(const std::vector<float>& x, size_t from, size_t to, float* lo, float* hi)
{
    *lo = 1e9f;
    *hi = 0.f;
    float last = -1.f;
    for (size_t i = from + 1; i < to; i++)
    {
        if (x[i - 1] < 0.f && x[i] >= 0.f)
        {
            const float at = static_cast<float>(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
            if (last >= 0.f)
            {
                const float f = kSr / (at - last);
                *lo = fminf(*lo, f);
                *hi = fmaxf(*hi, f);
            }
            last = at;
        }
    }
}

static Warble warble;

/** n samples of a sine (hz) through wow & flutter with these knobs, on; the left channel */
static std::vector<float> RunWarble(const float knobs[4], size_t n, float hz, bool* finite)
{
    warble.Init(kSr);
    for (size_t p = 0; p < 4; p++)
        warble.SetParam(p, knobs[p]);
    warble.SetOn(true);
    std::vector<float> out(n);
    *finite = true;
    for (size_t i = 0; i < n; i++)
    {
        float l = Sine(i, hz), r = l;
        warble.Process(&l, &r);
        out[i] = l;
        if (!std::isfinite(l) || !std::isfinite(r) || fabsf(l) > 1.f)
            *finite = false;
    }
    return out;
}

static void CheckWarble()
{
    // defaults: wow off, flutter off, tone open, stereo off
    {
        warble.Init(kSr);
        const float defaults[4] = {0.f, 0.f, 1.f, 0.f};
        for (size_t p = 0; p < 4; p++)
            warble.SetParam(p, defaults[p]);
        warble.SetOn(true);
        bool same = true;
        for (size_t i = 0; i < 48000; i++)
        {
            const float in = Sine(i);
            float l = in, r = -in;
            warble.Process(&l, &r);
            same = same && l == in && r == -in;
        }
        Check(same, "wow & flutter on its defaults: the input bit for bit");
    }

    // flutter alone: the pitch moves with the delay's slope, the depth times the wobbles'
    // weighted rates times the parabolic sine's slope, 8 per cycle. 1.4% at the top, and half
    // the knob a quarter of that. 1kHz, so a cycle is short against the wobble
    const float hz = 1000.f;
    for (float knob : {1.f, .5f})
    {
        const float knobs[4] = {0.f, knob, 1.f, 0.f};
        bool finite;
        const std::vector<float> out = RunWarble(knobs, 3 * 48000, hz, &finite);
        float lo, hi;
        FreqRange(out, 48000, 3 * 48000, &lo, &hi);
        const float dev = 100.f * fmaxf(hi / hz - 1.f, 1.f - lo / hz);
        const float rates = kFlutterWeights[0] * kFlutterHz[0] + kFlutterWeights[1] * kFlutterHz[1];
        const float bound = 1.15f * 100.f * kFlutterMaxFrames * knob * knob * 8.f * rates / kSr;
        printf("      flutter %.0f%%: pitch within +-%.2f%% (bound %.2f%%)\n", knob * 100.f, dev, bound);
        char what[96];
        snprintf(what, sizeof(what), "flutter at %.0f%%: wobbles, within its bound", knob * 100.f);
        Check(finite && dev > .3f * bound && dev < bound, what);
    }

    // TAPE's wow at the top, all knobs up: finite and bounded, and it moves the pitch
    {
        const float knobs[4] = {1.f, 1.f, .5f, 1.f};
        bool finite;
        const std::vector<float> out = RunWarble(knobs, 5 * 48000, hz, &finite);
        float lo, hi;
        FreqRange(out, 48000, 5 * 48000, &lo, &hi);
        printf("      wow at the top: %.0f to %.0f Hz\n", lo, hi);
        Check(finite && hi - lo > 10.f, "wow at the top: bounded, and the pitch drifts");
    }
}

static const size_t kTapeFrames = 1u << 19;
static float tape_l[kTapeFrames], tape_r[kTapeFrames];
static TapeStop tapestop;

/** A run of the tape stop: the sine in, the left channel out, and the largest step between
 *  samples, from sample at on */
struct TapeRun
{
    size_t i = 0;
    float prev = 0.f;
    float max_step = 0.f;
    bool finite = true;

    float Step()
    {
        const float in = Sine(i);
        float l = in, r = -in;
        tapestop.Process(&l, &r);
        if (!std::isfinite(l) || !std::isfinite(r))
            finite = false;
        if (i > 0)
            max_step = fmaxf(max_step, fabsf(l - prev));
        prev = l;
        i++;
        return l;
    }
    /** n samples; whether every one was the input (exact) */
    bool Live(size_t n)
    {
        bool same = true;
        for (size_t k = 0; k < n; k++)
        {
            const float in = Sine(i);
            same = Step() == in && same;
        }
        return same;
    }
    /** n samples; whether every one was silent */
    bool Silent(size_t n)
    {
        bool silent = true;
        for (size_t k = 0; k < n; k++)
            silent = Step() == 0.f && silent;
        return silent;
    }
    void Run(size_t n)
    {
        for (size_t k = 0; k < n; k++)
            Step();
    }
};

// at 120 BPM: a 16th is 6000 frames
static const size_t k16th = 6000;

static void InitTapeStop(float stop, float start, float curve)
{
    tapestop.Init(kSr, tape_l, tape_r, kTapeFrames);
    tapestop.SetTempo(120);
    tapestop.SetParam(TapeStop::STOP, stop);
    tapestop.SetParam(TapeStop::START, start);
    tapestop.SetParam(TapeStop::CURVE, curve);
}

static void CheckTapeStop()
{
    // stop 1/4 (step 2 of 5: .4), spin-up 1/8 (step 2: .4)
    {
        InitTapeStop(.4f, .4f, 0.f);
        TapeRun run;
        Check(run.Live(48000), "tape stop off: the input bit for bit");

        tapestop.SetOn(true);
        // the pitch falls: fewer cycles in the stop's second half than its first
        size_t crossings[2] = {0, 0};
        float last = run.prev;
        for (size_t k = 0; k < 4 * k16th; k++)
        {
            const float x = run.Step();
            if (last < 0.f && x >= 0.f)
                crossings[k < 2 * k16th]++;
            last = x;
        }
        printf("      stop: %zu then %zu cycles\n", crossings[1], crossings[0]);
        Check(crossings[0] < crossings[1] / 2, "a stop: the pitch falls");
        run.Run(10);
        Check(run.Silent(48000), "stopped: silent while the key is held");

        tapestop.SetOn(false);
        run.Run(2 * k16th + 720 + 10);
        Check(run.Live(48000), "spun up over 1/8: then the input bit for bit");
        Check(run.finite && run.max_step < kMaxStep, "the stop and spin-up: no step between samples");
        printf("      largest step %.4f\n", run.max_step);
    }

    // the brake: a long drag at the end, still silent once the time is up
    {
        InitTapeStop(.4f, .4f, 1.f);
        TapeRun run;
        tapestop.SetOn(true);
        run.Run(4 * k16th + 10);
        Check(run.Silent(4800), "the brake: silent once the stop's time is up");
        tapestop.SetOn(false);
        run.Run(2 * k16th + 720 + 10);
        Check(run.Live(4800) && run.max_step < kMaxStep, "the brake: spins up to the input, smoothly");
    }

    // spin-up off: straight back, in the splice's 15ms
    {
        InitTapeStop(.4f, 0.f, 0.f);
        TapeRun run;
        tapestop.SetOn(true);
        run.Run(5 * k16th);
        tapestop.SetOn(false);
        run.Run(720 + 2);
        Check(run.Live(4800) && run.max_step < kMaxStep, "spin-up off: back to the input at once");
    }

    // released halfway down, pressed again halfway up, released again
    {
        InitTapeStop(.4f, .4f, 0.f);
        TapeRun run;
        run.Run(1000);
        tapestop.SetOn(true);
        run.Run(2 * k16th);
        tapestop.SetOn(false);
        run.Run(k16th / 2);
        tapestop.SetOn(true);
        run.Run(k16th);
        tapestop.SetOn(false);
        run.Run(2 * k16th + 720 + 10);
        Check(run.Live(4800), "released mid-stop, pressed mid-spin-up: back on the input");
        Check(run.finite && run.max_step < kMaxStep, "... with no step between samples");
        printf("      largest step %.4f\n", run.max_step);
    }

    // held at depth 0 (half speed) for 30 s: the head falls behind by half a second each
    // second and would reach the buffer's end (2^19 frames) after 22 s; it jumps back to the
    // live signal there, crossfaded, and goes on at half speed rather than at full
    {
        InitTapeStop(.4f, .4f, 0.f);
        tapestop.SetParam(TapeStop::DEPTH, 0.f);
        TapeRun run;
        run.Run(4800);
        tapestop.SetOn(true);
        run.Run(4 * k16th + 4800);
        run.max_step = 0.f;
        size_t cycles = 0;
        float last = run.prev;
        const size_t from = static_cast<size_t>(20.f * kSr), to = static_cast<size_t>(30.f * kSr);
        for (size_t k = run.i; k < to; k++)
        {
            const float x = run.Step();
            if (k >= from && last < 0.f && x >= 0.f)
                cycles++;
            last = x;
        }
        const float hz = static_cast<float>(cycles) / 10.f;
        printf("      held at depth 0, 20-30 s: %.1f Hz, largest step %.4f\n", hz, run.max_step);
        Check(hz > 104.f && hz < 116.f, "held long at depth 0: half speed throughout, past the buffer's end");
        Check(run.finite && run.max_step < kMaxStep, "... with no step between samples");
    }

    // the longest stop at the slowest tempo on the steepest brake: the lag stays in the buffer
    {
        tapestop.Init(kSr, tape_l, tape_r, kTapeFrames);
        tapestop.SetTempo(kMinBpm);
        tapestop.SetParam(TapeStop::STOP, 1.f);
        tapestop.SetParam(TapeStop::START, 1.f);
        tapestop.SetParam(TapeStop::CURVE, 1.f);
        TapeRun run;
        tapestop.SetOn(true);
        const size_t two_bars = static_cast<size_t>(2.f * 240.f * kSr / kMinBpm);
        run.Run(two_bars + 10);
        Check(run.Silent(4800), "2 bars at the slowest tempo: stops");
        tapestop.SetOn(false);
        run.Run(two_bars / 2 + 720 + 10);
        Check(run.finite && run.Live(4800), "... and spins up a bar to the input");
    }
}

int main()
{
    CheckWarble();
    CheckTapeStop();
    return Finish();
}
