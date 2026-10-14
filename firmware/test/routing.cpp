// routing.cpp: checks the engine's input routing (passthroughEngine.h, #177): each source's
// gates, so a source that isn't live adds nothing to the outputs at any sample, also while
// the source switches (#168 item 1); the looper's input the source, live or not; and the
// slews that settle exactly on their targets (#168 item 2: Glide, the mix, the headphone
// cue); and each output's polarity against AUX (#209). Exits 0 when everything passes. Run by
// unit.sh routing.
#include <cmath>
#include <cstdio>
#include "check.h"
#include "passthroughEngine.h"

using namespace chompi;

static const float kSr = 48000.f;
static const size_t kBlock = 24;

static int16_t loop_mem[kLoopMemSize];
static const size_t kDelayFrames = 480000;
static float delay_mem[kDelayFrames * 2];
static const size_t kFreezerFrames = 240000;
static float freezer_mem_l[kFreezerFrames], freezer_mem_r[kFreezerFrames];
static const size_t kTapeStopFrames = 1u << 19;
static float tapestop_mem_l[kTapeStopFrames], tapestop_mem_r[kTapeStopFrames];
static daisysp::Reverb reverb;
static MidiClock midi_clock;
static PassthroughEngine engine;

static float aux_amp = 0.f, mic_amp = 0.f;
static uint32_t n = 0; // samples played

static void Play(size_t blocks, float* master = nullptr, float* phones = nullptr);

static void Init()
{
    engine.Init(kSr, loop_mem, &midi_clock, delay_mem, kDelayFrames, &reverb,
                freezer_mem_l, freezer_mem_r, kFreezerFrames,
                tapestop_mem_l, tapestop_mem_r, kTapeStopFrames);
    engine.SetMainGain(.75f);
    engine.SetInputGain(false, .75f);
    engine.SetInputGain(true, .75f);
    // what Init leaves as it was: the defaults, settled in silence
    engine.SetMicInput(false);
    for (bool mic : {false, true})
    {
        engine.SetDry(mic, false);
        engine.SetLive(mic, !mic);
    }
    engine.SetMix(0.f);
    engine.SetHeadphoneCue(0.f);
    aux_amp = mic_amp = 0.f;
    Play(4000);
}

/** Plays blocks: a sine into AUX (440 Hz) and one into the mic (1 kHz), each at its amp. The
 *  master's and the headphones' peaks over them, from the left */
static void Play(size_t blocks, float* master, float* phones)
{
    float mic[kBlock], x[kBlock] = {}, auxl[kBlock], auxr[kBlock];
    float o[4][kBlock];
    const float* in[4] = {mic, x, auxl, auxr};
    float* out[4] = {o[0], o[1], o[2], o[3]};
    float m = 0.f, h = 0.f;
    for (size_t b = 0; b < blocks; b++)
    {
        for (size_t i = 0; i < kBlock; i++, n++)
        {
            auxl[i] = auxr[i] = aux_amp * sinf(2.f * float(M_PI) * 440.f * n / kSr);
            mic[i] = mic_amp * sinf(2.f * float(M_PI) * 1000.f * n / kSr);
        }
        engine.Process(in, out, kBlock);
        for (size_t i = 0; i < kBlock; i++)
        {
            m = fmaxf(m, fmaxf(fabsf(o[2][i]), fabsf(o[3][i])));
            h = fmaxf(h, fabsf(o[0][i]));
        }
    }
    if (master)
        *master = m;
    if (phones)
        *phones = h;
}

/** A source that isn't live adds nothing to the master at any sample, through any switch of
 *  the source, mid-fade too (#168): the mic alone sounds, not live, while the source goes back
 *  and forth to AUX, live through the effects, then dry */
static void TestNoLeak()
{
    for (bool dry : {false, true})
    {
        Init();
        engine.SetDry(false, dry);
        mic_amp = .5f; // AUX silent: only the mic could reach the master
        Play(2000);
        float peak = 0.f, p;
        for (int k = 0; k < 12; k++)
        {
            engine.SetMicInput(k % 2 == 0);
            // a whole fade (240 samples), or a block or a few into one
            Play(k < 6 ? 20 : static_cast<size_t>(1 + k % 3), &p);
            peak = fmaxf(peak, p);
        }
        engine.SetMicInput(false);
        Play(200, &p);
        peak = fmaxf(peak, p);
        printf("      the mic not live, the source switched to it and back, AUX %s: the master's peak %g\n",
               dry ? "dry" : "through the effects", peak);
        Check(peak == 0.f, dry ? "no leak: the mic not live, AUX dry: nothing of it on the master, ever"
                               : "no leak: the mic not live, AUX through the effects: nothing of it on the master, ever");
    }

    // and the other way: AUX not live, the mic live, AUX alone sounding
    Init();
    engine.SetLive(false, false);
    engine.SetLive(true, true);
    Play(200); // AUX's gates shut
    aux_amp = .5f;
    float peak = 0.f, p;
    for (int k = 0; k < 12; k++)
    {
        engine.SetMicInput(k % 2 == 1);
        Play(k < 6 ? 20 : static_cast<size_t>(1 + k % 3), &p);
        peak = fmaxf(peak, p);
    }
    Check(peak == 0.f, "no leak: AUX not live, the mic live: nothing of AUX on the master, ever");
}

/** Each combination's master and headphones (the cue at the input alone): live through the
 *  effects or dry, the source heard; not live, nothing on the master, the headphones as ever */
static void TestCombinations()
{
    for (bool mic : {false, true})
        for (int c = 0; c < 3; c++)
        {
            Init();
            const bool live = c < 2, dry = c == 1;
            engine.SetMicInput(mic);
            engine.SetDry(mic, dry);
            engine.SetLive(mic, live);
            engine.SetHeadphoneCue(1.f);
            (mic ? mic_amp : aux_amp) = .1f;
            Play(4000);
            float master, phones;
            Play(400, &master, &phones);
            printf("      %s, %s: the master %.4f, the headphones %.4f\n", mic ? "the mic" : "AUX",
                   !live ? "not live" : dry ? "dry, live" : "through the effects, live", master, phones);
            const std::string what = std::string(mic ? "the mic" : "AUX")
                                     + (!live ? " not live: nothing on the master" : " live: on the master")
                                     + ", on the headphones' input feed";
            Check(phones > .005f && (live ? master > .005f : master == 0.f), what.c_str());
        }
}

/** The looper records the source as it is, live or not */
static void TestRecordsRaw()
{
    for (bool live : {true, false})
    {
        Init();
        engine.SetLive(false, live);
        aux_amp = .1f;
        Play(400);
        engine.looper.StartRecording(false);
        Play(2000);
        engine.looper.StopRecording();
        aux_amp = 0.f;
        Play(200);
        float master;
        Play(1000, &master);
        engine.SetMix(1.f); // the loop alone
        Play(2000, &master);
        printf("      AUX %s, recorded: the loop on the master %.4f\n", live ? "live" : "not live", master);
        Check(master > .01f, live ? "the looper records AUX live" : "the looper records AUX not live too");
    }
}

/** Plays blocks of a bump into AUX every `every` samples (a 2 ms raised cosine, upwards, at
 *  amp), and gives the sample of the largest magnitude, with its sign, on the line out's left
 *  and on the headphones' left */
static void PlayBumps(size_t blocks, uint32_t every, float amp, float* line, float* phones)
{
    static const uint32_t kBump = 96;
    float mic[kBlock] = {}, x[kBlock] = {}, auxl[kBlock], auxr[kBlock];
    float o[4][kBlock];
    const float* in[4] = {mic, x, auxl, auxr};
    float* out[4] = {o[0], o[1], o[2], o[3]};
    float l = 0.f, h = 0.f;
    for (size_t b = 0; b < blocks; b++)
    {
        for (size_t i = 0; i < kBlock; i++, n++)
        {
            const uint32_t at = n % every;
            auxl[i] = auxr[i] = at < kBump ? amp * .5f * (1.f - cosf(2.f * float(M_PI) * at / kBump)) : 0.f;
        }
        engine.Process(in, out, kBlock);
        for (size_t i = 0; i < kBlock; i++)
        {
            if (fabsf(o[2][i]) > fabsf(l))
                l = o[2][i];
            if (fabsf(o[0][i]) > fabsf(h))
                h = o[0][i];
        }
    }
    *line = l;
    *phones = h;
}

/** Each output's polarity against AUX (#209). This check, like the twin, sees the digital
 *  signal; the CHOMPI's analog stages turn it (CC_Chompi_Rev4.sch): AUX in once (U16C/D), the
 *  line out once (U16A/B after the PCM3060), the headphones not at all (U2B and the TPA6110,
 *  twice). So the line out comes out at its jack as AUX went in when it carries the input's
 *  sign, and the headphones only when they carry it turned: digitally the headphones are
 *  against the line out, and at the jacks both are in phase with AUX. Through the effects,
 *  dry, with the cue anywhere from the mix to the input, and from the loop */
static void TestPolarity()
{
    struct Case
    {
        const char* what;
        bool dry;
        float cue;
    };
    const Case cases[] = {
        {"through the effects, the headphones on the mix", false, 0.f},
        {"dry, the headphones on the mix", true, 0.f},
        {"through the effects, the headphones' cue halfway", false, .5f},
        {"through the effects, the headphones on the input", false, 1.f},
    };
    float line, phones;
    for (const Case& c : cases)
    {
        Init();
        engine.SetDry(false, c.dry);
        engine.SetHeadphoneCue(c.cue);
        PlayBumps(400, 4800, 0.f, &line, &phones); // the cue and the gates settled, in silence
        PlayBumps(400, 4800, .3f, &line, &phones);
        printf("      a bump up into AUX, %s: the line out %+.4f, the headphones %+.4f\n", c.what,
               line, phones);
        const std::string what = std::string("polarity, ") + c.what
                                 + ": the line out as AUX, the headphones turned";
        Check(line > .01f && phones < -.01f, what.c_str());
    }

    // the loop: bumps recorded, then the loop alone
    Init();
    engine.looper.StartRecording(false);
    PlayBumps(2000, 4800, .3f, &line, &phones);
    engine.looper.StopRecording();
    engine.SetMix(1.f);
    PlayBumps(2000, 4800, 0.f, &line, &phones); // the mix at the loop
    PlayBumps(2000, 4800, 0.f, &line, &phones);
    printf("      bumps up recorded from AUX, the loop alone: the line out %+.4f, the headphones %+.4f\n",
           line, phones);
    Check(line > .01f && phones < -.01f,
          "polarity, the loop alone: the line out as AUX, the headphones turned");
}

/** Glide settles exactly on its target, from either side (#168): near 1 a fonepole step stalls */
static void TestGlide()
{
    float up = 0.f, down = 1.f;
    int up_at = -1, down_at = -1;
    for (int i = 0; i < 48000; i++)
    {
        up = chompi::Glide(up, 1.f, .001f);
        down = chompi::Glide(down, 0.f, .001f);
        if (up == 1.f && up_at < 0)
            up_at = i;
        if (down == 0.f && down_at < 0)
            down_at = i;
    }
    float stalled = 0.f;
    for (int i = 0; i < 48000; i++)
        fonepole(stalled, 1.f, .001f);
    printf("      Glide: 1 after %d samples, 0 after %d; fonepole alone stalls at 1 - %g\n", up_at,
           down_at, 1. - stalled);
    Check(up == 1.f && up_at > 0 && up_at < 24000, "glide: up to exactly 1, within half a second");
    Check(down == 0.f && down_at > 0 && down_at < 24000, "glide: down to exactly 0");

    // the mix to the loop alone: AUX's share gone, as far as cos(pi/2) in floats goes
    Init();
    aux_amp = .1f;
    float live, gone;
    Play(2000, &live);
    engine.SetMix(1.f);
    Play(4000);
    Play(400, &gone);
    printf("      the mix at the input %.4f, at the loop %g\n", live, gone);
    Check(gone < live * 1e-6f, "glide: the mix at the loop alone leaves none of the input");
}

int main()
{
    TestNoLeak();
    TestCombinations();
    TestRecordsRaw();
    TestGlide();
    TestPolarity();
    return Finish();
}
