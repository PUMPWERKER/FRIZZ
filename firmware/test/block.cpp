// block.cpp: the FX chain a block at a time (FxChain::Process, #136) against the same chain a
// sample at a time, bit for bit: the output and every meter. One script of keys, knobs, clock
// pulses and scene recalls' fast slews, at the same samples, on three chains: one sample per
// call, a block per call (blocks of 1 to 40 samples, so runs of kFxMaxBlock and the rest), and
// each block in two calls. What the script has to cover:
//  - every effect on, the resonator at its highest pitch and stereo (its loop shortest, its
//    Feeds reading the furthest ahead of its Taps) with its tone slewing (Tap's lowpass);
//  - page 2 off its defaults, the resonator's Band among them while its key fades out, so it
//    goes idle in the middle of a block;
//  - recalls' fast slews ending on a block's first sample, in its middle and on its last;
//  - page 2 leaving the chain's work (FxChain::OutRunning, #142) on a block's first sample, in
//    its middle and on its last, each of three ways: an insert going quiet with its page 2 off
//    its defaults, an insert's page 2 settling back to its defaults while it's on, and a send's
//    or the resonator's Band settling back to the middle. Which page 2s are part of the work is
//    compared too, block by block: once page 2 rests, leaving it out changes no sample.
// Exits 0 when everything passes. Run by unit.sh block. block OUT writes the sample-per-call
// chain's output, meters and page-2 bits to OUT, so the script can be compared bit for bit
// with another version of FxChain.h (built against its headers): what check.sh's harness
// doesn't turn, page 2.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "check.h"
#include "FxChain.h"

using namespace chompi;

static const float kSr = 48000.f;
static const size_t kSamples = 30 * 48000;
static const size_t kPulseSamples = 2000; // 12 PPQN at 120 BPM

/** A chain with buffers of its own */
struct Rig
{
    static const size_t kDelayFrames = 480000, kFreezerFrames = 240000, kTapeFrames = 1u << 19;
    std::vector<float> delay = std::vector<float>(kDelayFrames * 2);
    std::vector<float> fz_l = std::vector<float>(kFreezerFrames);
    std::vector<float> fz_r = std::vector<float>(kFreezerFrames);
    std::vector<float> tape_l = std::vector<float>(kTapeFrames);
    std::vector<float> tape_r = std::vector<float>(kTapeFrames);
    daisysp::Reverb reverb;
    FxChain chain;

    std::vector<float> out;    // l, r per sample
    std::vector<float> meters; // kNumFx per block
    std::vector<uint16_t> busy; // per block, bit fx: its page 2 part of the work after it
    size_t switch_at[3] = {};  // fast slews ending on a block's first sample, middle, last
    // page 2 leaving the work (PER_SAMPLE only): [how][where in the block], how one of
    enum { QUIET, DEFAULTS, SEND, kHows };
    size_t cleared[kHows][3] = {};
    bool on[kNumFx] = {}; // the keys, as the script set them

    void Init()
    {
        chain.Init(kSr, delay.data(), kDelayFrames, &reverb, fz_l.data(), fz_r.data(),
                   kFreezerFrames, tape_l.data(), tape_r.data(), kTapeFrames);
        chain.SetTempo(120.f, static_cast<float>(kPulseSamples));
    }
};

struct Dice
{
    uint32_t s;
    uint32_t Next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float Uniform() { return static_cast<float>(Next() >> 8) / 16777216.f; }
    size_t Below(size_t n) { return Next() % n; }
};

static float Input(size_t i)
{
    const float t = static_cast<float>(i) / kSr;
    // a chord with an envelope, so the gates, the env mod and the compressor-like parts move
    const float env = .5f + .5f * sinf(2.f * float(M_PI) * .7f * t);
    return env * (.3f * sinf(2.f * float(M_PI) * 220.f * t) + .15f * sinf(2.f * float(M_PI) * 331.f * t));
}

enum Split
{
    PER_SAMPLE, // one sample per call
    PER_BLOCK,  // the block in one call
    TWO_CALLS,  // the block in two calls
};

/** Bit fx: its page 2 part of the chain's work now */
static uint16_t Busy(const FxChain& c)
{
    uint16_t bits = 0;
    for (size_t fx = 0; fx < kNumSoundFx; fx++)
        if (c.OutRunning(fx))
            bits |= static_cast<uint16_t>(1u << fx);
    return bits;
}

/** The script on rig, its blocks cut by split. The same blocks (their lengths) and the same
 *  events on every rig */
static void Play(Rig& rig, Split split)
{
    FxSlew::coeff = kFxParamCoeff; // a static, shared by every chain: as at boot
    rig.Init();
    FxChain& c = rig.chain;
    Dice blocks{12345}, events{777}, cuts{99};
    rig.out.assign(kSamples * 2, 0.f);
    rig.meters.clear();

    // every effect on, the resonator at its top, page 2 off its defaults here and there
    for (size_t fx = 0; fx < kNumSoundFx; fx++)
    {
        c.SetOn(fx, true);
        rig.on[fx] = true;
    }
    c.SetParam(FX_RESONATOR, chompi::Resonator::PITCH, 1.f);
    c.SetParam(FX_RESONATOR, chompi::Resonator::STEREO, 1.f);
    c.SetParam(FX_RESONATOR, chompi::Resonator::FEEDBACK, .9f);
    c.SetParam(FX_RESONATOR, chompi::Resonator::TONE, 0.f);
    c.SetParam(FX_RESONATOR, chompi::Resonator::ENV_MOD, .5f);
    c.SetParam(FX_RESONATOR, FxOutput::kBand, .3f);
    c.SetParam(FX_FOLDER, FxOutput::kLevel, .5f);
    c.SetParam(FX_SHIFTER, FxOutput::kMix, .6f);
    c.SetParam(FX_DELAY, FxOutput::kBand, .7f);
    c.SetParam(FX_REVERB, FxOutput::kBand, .2f);

    size_t i = 0, block = 0;
    size_t slew_end = SIZE_MAX; // the sample a fast slew ends on (FxChain::FastSlew: 2400 on)
    uint32_t next_pulse = 0;
    while (i < kSamples)
    {
        size_t n = 1 + blocks.Below(40);
        if (n > kSamples - i)
            n = kSamples - i;

        // between blocks, as the engine does: the clock's pulses, the keys, the knobs
        while (next_pulse * kPulseSamples < i + n)
            c.ClockPulse(next_pulse++);
        if (block == 50)
            c.SetParam(FX_RESONATOR, chompi::Resonator::TONE, 1.f); // the loop's lowpass slews open
        if (block > 100 && events.Below(12) == 0)
        {
            const size_t fx = events.Below(kNumSoundFx);
            const size_t param = events.Below(kNumFxParams);
            c.SetParam(fx, param, events.Uniform());
        }
        if (block > 100 && events.Below(40) == 0)
        {
            // a key off and on again; the resonator's often, its Band busy as it goes idle
            const size_t fx = events.Below(3) == 0 ? FX_RESONATOR : events.Below(kNumSoundFx);
            rig.on[fx] = events.Below(2) == 0;
            c.SetOn(fx, rig.on[fx]);
        }
        if (block > 100 && events.Below(30) == 0)
        {
            // page 2 back to its defaults (an insert's three shared knobs, a send's Band), or
            // off them again
            const size_t fx = events.Below(kNumSoundFx);
            const bool band_only = fx == FX_DELAY || fx == FX_REVERB || fx == FX_RESONATOR;
            const bool reset = events.Below(3) != 0;
            c.SetParam(fx, FxOutput::kBand, reset ? FxOutput::kBandDefault : events.Uniform());
            if (!band_only)
            {
                c.SetParam(fx, FxOutput::kMix, reset ? FxOutput::kMixDefault : events.Uniform());
                c.SetParam(fx, FxOutput::kLevel, reset ? FxOutput::kLevelDefault : events.Uniform());
            }
        }
        if (block > 100 && events.Below(150) == 0)
        {
            // a scene recall: the fast slew, and its knobs
            c.FastSlew();
            slew_end = i + 2399;
            for (size_t k = 0; k < 6; k++)
                c.SetParam(events.Below(kNumSoundFx), events.Below(kNumFxParams), events.Uniform());
            c.SetParam(FX_RESONATOR, chompi::Resonator::PITCH, 1.f);
            c.SetParam(FX_RESONATOR, chompi::Resonator::STEREO, 1.f);
        }
        if (slew_end >= i && slew_end < i + n)
            rig.switch_at[slew_end == i ? 0 : (slew_end == i + n - 1 ? 2 : 1)]++;
        float jump;
        c.Block(&jump);

        float l[64], r[64];
        for (size_t k = 0; k < n; k++)
            l[k] = r[k] = Input(i + k);
        r[0] *= .9f; // a little stereo
        switch (split)
        {
        case PER_SAMPLE:
            for (size_t k = 0; k < n; k++)
            {
                const uint16_t before = Busy(c);
                c.Process(&l[k], &r[k]);
                const uint16_t gone = before & ~Busy(c);
                const size_t where = k == 0 ? 0 : (k == n - 1 ? 2 : 1);
                for (size_t fx = 0; fx < kNumSoundFx; fx++)
                    if (gone >> fx & 1)
                    {
                        const bool send = fx == FX_DELAY || fx == FX_REVERB || fx == FX_RESONATOR;
                        if (send)
                            rig.cleared[Rig::SEND][where]++;
                        else if (!rig.on[fx])
                            rig.cleared[Rig::QUIET][where]++;
                        else
                            rig.cleared[Rig::DEFAULTS][where]++;
                    }
            }
            break;
        case PER_BLOCK:
            c.Process(l, r, n);
            break;
        case TWO_CALLS:
        {
            const size_t first = cuts.Below(n + 1);
            c.Process(l, r, first);
            c.Process(l + first, r + first, n - first);
            break;
        }
        }
        for (size_t k = 0; k < n; k++)
        {
            rig.out[(i + k) * 2] = l[k];
            rig.out[(i + k) * 2 + 1] = r[k];
        }
        for (size_t fx = 0; fx < kNumFx; fx++)
            rig.meters.push_back(c.GetLevel(fx));
        rig.busy.push_back(Busy(c));
        i += n;
        block++;
    }
}

/** Where a and b part: the first sample (or meter, or block) that differs, or -1 */
template <class T>
static long FirstDiff(const std::vector<T>& a, const std::vector<T>& b)
{
    if (a.size() != b.size())
        return 0;
    for (size_t k = 0; k < a.size(); k++)
        if (memcmp(&a[k], &b[k], sizeof(T)) != 0)
            return static_cast<long>(k);
    return -1;
}

static Rig rigs[3];

template <class T>
static void Write(FILE* f, const std::vector<T>& v)
{
    fwrite(v.data(), sizeof(T), v.size(), f);
}

int main(int argc, char** argv)
{
    Play(rigs[PER_SAMPLE], PER_SAMPLE);
    Play(rigs[PER_BLOCK], PER_BLOCK);
    Play(rigs[TWO_CALLS], TWO_CALLS);

    const Rig& ref = rigs[PER_SAMPLE];
    double rms = 0.;
    for (float x : ref.out)
        rms += static_cast<double>(x) * x;
    rms = sqrt(rms / static_cast<double>(ref.out.size()));
    printf("  %zu samples, rms %.4f; fast slews ending on a block's first sample %zu, middle %zu, last %zu\n",
           kSamples, rms, ref.switch_at[0], ref.switch_at[1], ref.switch_at[2]);
    Check(rms > .01f, "the script: sound comes out");
    Check(ref.switch_at[0] > 0 && ref.switch_at[1] > 0 && ref.switch_at[2] > 0,
          "the script: recalls' fast slews end on a block's first sample, in its middle and on its last");
    static const char* const kHow[] = {"an insert going quiet", "an insert at its defaults",
                                       "a send's or the resonator's Band in the middle"};
    bool every = true;
    for (size_t how = 0; how < Rig::kHows; how++)
    {
        printf("  page 2 leaving the work, %s: on a block's first sample %zu, middle %zu, last %zu\n",
               kHow[how], ref.cleared[how][0], ref.cleared[how][1], ref.cleared[how][2]);
        for (size_t where = 0; where < 3; where++)
            every = every && ref.cleared[how][where] > 0;
    }
    Check(every, "the script: page 2 leaves the work each of three ways, on a block's first sample, in its middle and on its last");
    if (argc > 1)
    {
        FILE* f = fopen(argv[1], "wb");
        if (!f)
        {
            printf("  can't write %s\n", argv[1]);
            return 1;
        }
        Write(f, ref.out);
        Write(f, ref.meters);
        Write(f, ref.busy);
        fclose(f);
    }

    const long out_b = FirstDiff(ref.out, rigs[PER_BLOCK].out);
    const long met_b = FirstDiff(ref.meters, rigs[PER_BLOCK].meters);
    if (out_b >= 0)
        printf("  a block per call parts at sample %ld\n", out_b / 2);
    Check(out_b < 0 && met_b < 0, "a block per call: the output and the meters bit for bit as a sample per call");
    Check(FirstDiff(ref.busy, rigs[PER_BLOCK].busy) < 0,
          "a block per call: page 2 part of the work after the same blocks as a sample per call");
    const long out_t = FirstDiff(ref.out, rigs[TWO_CALLS].out);
    const long met_t = FirstDiff(ref.meters, rigs[TWO_CALLS].meters);
    if (out_t >= 0)
        printf("  two calls a block part at sample %ld\n", out_t / 2);
    Check(out_t < 0 && met_t < 0, "a block in two calls: the same, bit for bit");
    Check(FirstDiff(ref.busy, rigs[TWO_CALLS].busy) < 0, "a block in two calls: page 2 the same");
    return Finish();
}
