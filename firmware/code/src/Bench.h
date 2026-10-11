/** @file Bench.h
 *  @brief The CPU bench, only in FRIZZ-bench.bin (make BENCH=1; the normal build leaves it out).
 *
 *  After the boot animation it plays a fixed list of segments through the engine, 3 s each, on
 *  a tune of its own (Tune: A minor at 120 BPM, Am F C G, an arpeggio, bass, kick and hi-hat,
 *  made by a few oscillators, so it takes no room for a recording), and measures the audio
 *  callback's load in each, all of it (controls, UI events, MIDI, the engine) but the tune,
 *  which is made for the next block after the measurement, by the system timer as libDaisy's CpuLoadMeter does: the
 *  highest and the mean, as parts of the 0.5 ms a block has. When it's done it writes them to
 *  /FRIZZ/cpu.txt, with the effects that were still working in a segment they're not part of
 *  (a delay or reverb tail: the delay sleeps only after 10 s of silence) and whether the loop
 *  played. Before each segment it waits until every effect that isn't part of it rests (at
 *  most 30 s), so no tail runs into the next one: the delay sleeps only after 10 s of silence,
 *  and works for its first 10 s after power-on too.
 *
 *  It measures FRIZZ-bench.bin, whose memory layout isn't FRIZZ.bin's: a crackle that comes
 *  from the layout alone (b5c658c) may show in one and not the other.
 *
 *  LEDs: one key per segment as it's done, white keys left to right, then the dark keys: green
 *  below 80%, amber below 95%, red above; the segment running blinks white. At the end every
 *  panel LED is green (everything below 95%) or red; red blinking means cpu.txt couldn't be
 *  written. Switch off and on to run it again.
 *
 *  Where the time goes: each segment's mean also split into the callback's parts
 *  (BenchProfile.h), counted in the core's cycles, with the clocks and caches the chip runs on.
 *
 *  The segments are set in the audio callback after the measurement, so setting them doesn't
 *  count; the results are read in MainLoop once the last one is done.
 */
#pragma once
#include <string.h>
#include "daisy.h"
#include "fatfs.h"
#include "BenchProfile.h"
#include "passthroughEngine.h"
#include "SceneStore.h"
#include "temp_led_stuff.h"

#ifndef FRIZZ_BENCH_SOURCE // the Makefile passes a checksum of the source; the host builds don't
#define FRIZZ_BENCH_SOURCE "host"
#endif

namespace chompi
{

/** The bench's signal: a little tune in A minor at 120 BPM, one chord a bar (Am F C G), an
 *  arpeggio in eighths panned left and right, bass on the beats, a kick on 1 and 3, a hi-hat
 *  on the off-beats. A few oscillators, the same every run */
class Tune
{
public:
    void Next(float& l, float& r)
    {
        static const uint32_t kBeat = 24000, kEighth = kBeat / 2, kBar = 4 * kBeat;
        static const uint8_t kRoots[4] = {45, 41, 48, 43}; // A2 F2 C3 G2
        static const bool kMinor[4] = {true, false, false, false};
        const uint32_t in_bar = t_ % kBar;
        const size_t chord = (t_ / kBar) % 4;
        if (in_bar % kEighth == 0)
        {
            // the arpeggio: up and down the chord over two octaves
            static const uint8_t kSteps[8] = {0, 1, 2, 3, 4, 3, 2, 1};
            const uint8_t step = kSteps[in_bar / kEighth];
            const uint8_t third = kMinor[chord] ? 3 : 4;
            const uint8_t tones[3] = {0, third, 7};
            arp_inc_ = Hz(kRoots[chord] + 12 + tones[step % 3] + 12 * (step / 3)) / 48000.f;
            arp_env_ = 1.f;
            arp_pan_ = -arp_pan_;
            if (in_bar % kBeat == 0)
            {
                bass_inc_ = Hz(kRoots[chord] - 12) / 48000.f;
                bass_env_ = 1.f;
                if ((in_bar / kBeat) % 2 == 0)
                    kick_env_ = 1.f;
            }
            else
                hat_env_ = 1.f;
        }
        t_++;

        arp_phase_ += arp_inc_;
        arp_phase_ -= arp_phase_ >= 1.f ? 1.f : 0.f;
        arp_lp_ += .15f * ((2.f * arp_phase_ - 1.f) - arp_lp_); // a saw, softened
        arp_env_ *= .99985f;
        bass_phase_ += bass_inc_;
        bass_phase_ -= bass_phase_ >= 1.f ? 1.f : 0.f;
        const float bass = (4.f * fabsf(bass_phase_ - .5f) - 1.f) * bass_env_; // a triangle
        bass_env_ *= .99993f;
        kick_phase_ += (45.f + 110.f * kick_env_ * kick_env_) / 48000.f;
        kick_phase_ -= kick_phase_ >= 1.f ? 1.f : 0.f;
        const float kick = sinf(6.2831853f * kick_phase_) * kick_env_;
        kick_env_ *= .9996f;
        seed_ = seed_ * 1664525u + 1013904223u;
        const float hat = (static_cast<float>(seed_ >> 8) / 8388608.f - 1.f) * hat_env_;
        hat_env_ *= .996f;

        const float arp = .36f * arp_lp_ * arp_env_, mid = .4f * bass + .6f * kick;
        l = mid + arp * (1.f - .4f * arp_pan_) + .08f * hat;
        r = mid + arp * (1.f + .4f * arp_pan_) - .08f * hat;
    }

private:
    static float Hz(int note) { return 440.f * powf(2.f, (note - 69) / 12.f); }

    uint32_t t_ = 0, seed_ = 1;
    float arp_phase_ = 0.f, arp_inc_ = 0.f, arp_lp_ = 0.f, arp_env_ = 0.f, arp_pan_ = 1.f;
    float bass_phase_ = 0.f, bass_inc_ = 0.f, bass_env_ = 0.f;
    float kick_phase_ = 0.f, kick_env_ = 0.f, hat_env_ = 0.f;
};

class Bench
{
public:
    static const size_t kSegmentBlocks = 6000; // 3 s at 48 kHz in 24-sample blocks
    static const size_t kRecordBlocks = 8000;  // the loop: 4 s
    static const size_t kKnobBlocks = 500;     // a knob moves every 0.25 s
    static const size_t kSettleBlocks = 60000; // waiting for the effects to rest: 30 s at most

    void Init(float sample_rate, size_t block_size, FATFS* fs, const char* path)
    {
        // ticks of the system timer in one block's time
        ticks_per_block_ = daisy::System::GetTickFreq() * (block_size / sample_rate);
        block_size_ = block_size < kMaxBlock ? block_size : kMaxBlock;
        fs_ = fs;
        path_ = path;
        BenchProfile::Enable();
    }

    /** At the very start of the audio callback */
    inline void BlockStart()
    {
        start_tick_ = daisy::System::GetTick();
        const uint32_t last = bench_profile.start;
        bench_profile.Start();
        block_period_ = bench_profile.start - last; // the cycles from one block to the next
    }

    /** The bench's tune into AUX in place of the inputs, while it runs: made the block before */
    const float* const* Input(const float* const* in, size_t size)
    {
        if (!running_)
            return in;
        inputs_[0] = in[0];
        inputs_[1] = in[1];
        inputs_[2] = in_l_;
        inputs_[3] = in_r_;
        return inputs_;
    }

    /** At the very end of the audio callback: the measurement, then the next step, then the
     *  tune's next block, none of which counts */
    void BlockEnd(PassthroughEngine& engine)
    {
        const uint32_t ticks = daisy::System::GetTick() - start_tick_;
        block_cycles_ = bench_profile.Total();
        block_ticks_ = ticks;
        const float load = static_cast<float>(ticks) / ticks_per_block_;
        Step(engine, load);
        if (running_)
            for (size_t i = 0; i < block_size_; i++)
                tune_.Next(in_l_[i], in_r_[i]);
    }

    void Step(PassthroughEngine& engine, float load)
    {
        if (!started_)
        {
            started_ = true;
            running_ = true;
            settling_ = true;
            next_ = 0;
            for (size_t fx = 0; fx < kNumFx; fx++)
                engine.SetFxOn(fx, false);
            return;
        }
        if (!running_)
            return;
        if (settling_)
        {
            // until every effect the next segment doesn't use rests: at the start the delay
            // keeps working for 10 s after power-on, later a send's tail rings out
            const uint16_t used = kSegments[next_].fx;
            bool resting = true;
            for (size_t fx = 0; fx < kNumFx; fx++)
                resting &= (used & (1u << fx)) || engine.FxResting(fx);
            if (resting || ++blocks_ >= kSettleBlocks)
            {
                settling_ = false;
                Begin(engine, next_);
            }
            return;
        }

        max_[segment_] = fmaxf(max_[segment_], load);
        sum_ += load;
        for (size_t p = 0; p < BenchProfile::kNumParts; p++)
            part_sum_[p] += bench_profile.cycles[p];
        cycles_sum_ += block_cycles_;
        ticks_sum_ += block_ticks_;
        period_sum_ += block_period_;
        blocks_++;
        const Segment& seg = kSegments[segment_];
        if (!(seg.flags & (STRESS | SCENE4 | RECORD)) && blocks_ % kKnobBlocks == 0)
            TurnAKnob(engine);
        size_t length = kSegmentBlocks;
        if (seg.flags & RECORD)
            length = kRecordBlocks;
        if (blocks_ < length)
            return;

        mean_[segment_] = static_cast<float>(sum_ / blocks_);
        for (size_t p = 0; p < BenchProfile::kNumParts; p++)
            parts_[segment_][p] = static_cast<float>(part_sum_[p]) / blocks_;
        cycles_[segment_] = static_cast<float>(cycles_sum_) / blocks_;
        period_[segment_] = static_cast<float>(period_sum_) / blocks_;
        all_cycles_ += cycles_sum_;
        all_ticks_ += ticks_sum_;
        all_periods_ += period_sum_;
        all_blocks_ += blocks_;
        // what still worked without being part of the segment, and the loop
        for (size_t fx = 0; fx < kNumFx; fx++)
            if (!(seg.fx & (1u << fx)) && !engine.FxResting(fx))
                awake_[segment_] |= static_cast<uint16_t>(1u << fx);
        if (seg.flags & RECORD)
        {
            engine.looper.StopRecording();
            loop_frames_ = engine.looper.GetLength(); // 0 until the stop lands: read below
        }
        if (seg.flags & LOOP)
        {
            loop_frames_ = engine.looper.GetLength();
            if (engine.looper.GetState() != Looper::State::PLAYING)
                loop_missing_ = true;
        }
        Next(engine, segment_ + 1);
    }

    /** The next segment, once what it doesn't use has come to rest */
    void Next(PassthroughEngine& engine, size_t s)
    {
        if (s >= kNumSegments)
        {
            Begin(engine, s);
            return;
        }
        for (size_t fx = 0; fx < kNumFx; fx++)
            if (!(kSegments[s].fx & (1u << fx)))
                engine.SetFxOn(fx, false);
        next_ = s;
        segment_ = s; // the one done is graded, this one stays dark until it runs
        blocks_ = 0;
        settling_ = true;
    }

    /** In MainLoop: writes cpu.txt once the last segment is done */
    void Process()
    {
        if (running_ || !started_ || written_)
            return;
        written_ = true;
        stored_ = Write();
    }

    /** In MainLoop, after the UI has drawn: the bench's LEDs over it */
    void DrawLeds(uint32_t now)
    {
        if (!started_)
            return;
        for (size_t s = 0; s < kNumSegments && s < kNumKeyLeds; s++)
        {
            float r = 0.f, g = 0.f, b = 0.f;
            if (s < segment_ || !running_)
                Grade(max_[s], r, g, b);
            else if (s == segment_ && !settling_)
                r = g = b = (now / 150) % 2 ? .6f : 0.f;
            SetSmtLedFloat(kKeyLeds[s], r, g, b);
        }
        float worst = 0.f;
        for (size_t s = 0; s < kNumSegments; s++)
            worst = fmaxf(worst, max_[s]);
        for (int i = 0; i < kPthLeds; i++)
        {
            float r = 0.f, g = 0.f, b = 0.f;
            if (running_)
                r = g = b = i == 0 ? .3f : 0.f; // CHOMPI dimly: the bench is running
            else if (written_ && !stored_)
                r = (now / 250) % 2 ? 1.f : 0.f;
            else if (written_)
                (worst < kRed ? g : r) = 1.f;
            SetPthLedFloat(i, r, g, b);
        }
        fill_led_data();
    }

private:
    // what a segment plays
    enum Flags : uint8_t
    {
        LOOP = 1,     // a loop plays (recorded just before the first of these)
        STRESS = 2,   // the harness's STRESS: every feedback at the top
        SCENE4 = 4,   // PR #7's scene 4: shifter +7, folder, crusher, slicer, compressor at 1
        COMP = 8,     // the compressor's amount at 1
        RECORD = 16,  // records the loop, 4 s
        CHAOS = 64,   // the chaos key on the inserts, every 16th: half its chances, the loop
                      // scrambled too
        PAGE2 = 32,   // page 2 off its defaults on every effect: its Mix, Band and Level, so
                      // FxOutput.h's crossover and blend run (at their defaults they don't),
                      // and each effect's own knobs (the sends' freeze stays off)
    };
    struct Segment
    {
        const char* name;
        uint16_t fx; // bit FxId: on
        uint8_t flags;
    };
    static constexpr uint16_t Bit(int fx) { return static_cast<uint16_t>(1u << fx); }
    // the effects with a sound: the chaos key has a segment of its own
    static const uint16_t kAllFx = (1u << kNumSoundFx) - 1;
    // everything but the tape stop, which would silence the rest
    static const uint16_t kEverything = kAllFx & ~(1u << FX_TAPESTOP);
    static const uint16_t kInserts = kEverything & ~(1u << FX_DELAY) & ~(1u << FX_REVERB);
    static const uint16_t kScene4 = (1u << FX_SHIFTER) | (1u << FX_FOLDER) | (1u << FX_CRUSHER)
                                    | (1u << FX_SLICER);

    static constexpr size_t kNumSegments = 24;
    static const Segment kSegments[kNumSegments];

    static const size_t kMaxBlock = 48;
    static const int kNumKeyLeds = 25;
    // the keys' LEDs: the white keys left to right (FxSlots.h's key_led), then the dark keys
    static constexpr uint8_t kKeyLeds[kNumKeyLeds] = {24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14,
                                                       13, 12, 11, 10, 0,  1,  2,  3,  4,  5,
                                                       6,  7,  8,  9};
    static constexpr float kAmber = .8f, kRed = .95f;

    static void Grade(float load, float& r, float& g, float& b)
    {
        b = 0.f;
        if (load < kAmber)
            r = 0.f, g = 1.f;
        else if (load < kRed)
            r = 1.f, g = .5f;
        else
            r = 1.f, g = 0.f;
    }

    void Begin(PassthroughEngine& engine, size_t s)
    {
        segment_ = s;
        blocks_ = 0;
        sum_ = 0.;
        for (size_t p = 0; p < BenchProfile::kNumParts; p++)
            part_sum_[p] = 0;
        cycles_sum_ = ticks_sum_ = period_sum_ = 0;
        if (s >= kNumSegments)
        {
            running_ = false;
            for (size_t fx = 0; fx < kNumFx; fx++)
                engine.SetFxOn(fx, false);
            engine.SetMix(0.f);
            return;
        }
        const Segment& seg = kSegments[s];
        engine.SetMainGain(.75f);
        engine.SetInputGain(.75f);
        engine.SetCompParam(MasterComp::kThreshold, seg.flags & (COMP | SCENE4) ? 1.f : .3f);
        for (size_t fx = 0; fx < kNumFx; fx++)
        {
            for (size_t p = 0; p < kNumFxKnobs; p++) // page 1's
                engine.SetFxParam(fx, p, .5f);
            // page 2: its defaults, or for PAGE2 off them: an insert's half mix, its own knob
            // at .3, the band on the highs above ~1kHz and the level 6dB down; a send's
            // damping or pre-delay, band and half ducking, its freeze off
            const bool send = fx == FX_DELAY || fx == FX_REVERB;
            static const float kInsert2[] = {.5f, .3f, .75f, .625f};
            static const float kSend2[] = {0.f, .7f, .75f, .5f};
            for (size_t p = kNumFxKnobs; p < kNumFxParams; p++)
                engine.SetFxParam(fx, p,
                                  seg.flags & PAGE2 ? (send ? kSend2 : kInsert2)[p - kNumFxKnobs]
                                                    : kFxParams[fx].defaults[p]);
        }
        if (seg.flags & STRESS)
        {
            // the harness's STRESS (test/harness.cpp): the resonator's loop at its most extreme
            engine.SetFxParam(FX_RESONATOR, 1, 1.f);
            engine.SetFxParam(FX_RESONATOR, 2, 1.f);
            engine.SetFxParam(FX_FILTER, 0, .2f);
            engine.SetFxParam(FX_FILTER, 1, 1.f);
            engine.SetFxParam(FX_FLANGER, 2, .5f);
            engine.SetFxParam(FX_FLANGER, 1, 1.f);
            engine.SetFxParam(FX_SHIFTER, 1, 1.f);
            engine.SetFxParam(FX_DELAY, 1, 1.f);
            engine.SetFxParam(FX_REVERB, 0, 1.f);
            engine.SetFxParam(FX_REVERB, 3, 1.f);
        }
        if (seg.flags & SCENE4)
            engine.SetFxParam(FX_SHIFTER, 0, .5f + 7.f / 24.f);
        engine.SetFxPool(seg.flags & CHAOS ? kInserts : 0);
        if (seg.flags & CHAOS)
            engine.SetFxParam(FX_CHAOS, Chaos::GRID, 0.f);
        for (size_t fx = 0; fx < kNumFx; fx++)
            engine.SetFxOn(fx, seg.fx & (1u << fx));
        if (seg.flags & RECORD)
        {
            engine.SetMix(0.f);
            engine.looper.StartRecording(false);
        }
        else
            engine.SetMix(seg.flags & LOOP ? .5f : 0.f);
    }

    void TurnAKnob(PassthroughEngine& engine)
    {
        const uint16_t on = kSegments[segment_].fx;
        if (!on)
            return;
        size_t fx;
        do
            fx = static_cast<size_t>(Random() * kNumFx) % kNumFx;
        while (!(on & (1u << fx)));
        engine.SetFxParam(fx, static_cast<size_t>(Random() * kNumFxKnobs) % kNumFxKnobs,
                          Random());
    }

    inline float Random()
    {
        lcg_ = lcg_ * 1664525u + 1013904223u;
        return static_cast<float>(lcg_ >> 8) / 16777216.f;
    }

    // a load as a percentage in 6 characters, "  87.3": the firmware's printf has no floats
    static void Percent(float load, char* out)
    {
        int tenths = static_cast<int>(load * 1000.f + .5f);
        if (tenths > 99999)
            tenths = 99999;
        int whole = tenths / 10;
        out[5] = static_cast<char>('0' + tenths % 10);
        out[4] = '.';
        for (int i = 3; i >= 0; i--)
        {
            out[i] = whole > 0 || i == 3 ? static_cast<char>('0' + whole % 10) : ' ';
            whole /= 10;
        }
    }

    struct Text
    {
        alignas(32) char buf[8192]; // whole cache lines for the SD DMA, as SceneStore's
        size_t pos = 0;
        void Put(const char* s)
        {
            while (*s && pos + 1 < sizeof(buf))
                buf[pos++] = *s++;
        }
        void Pad(size_t to)
        {
            while (pos < to && pos + 1 < sizeof(buf))
                buf[pos++] = ' ';
        }
        void Load(float load)
        {
            char p[7] = {};
            Percent(load, p);
            Put(p);
        }
        void UInt(uint32_t n, size_t width = 0)
        {
            char d[11];
            size_t len = 0;
            do
                d[len++] = static_cast<char>('0' + n % 10);
            while ((n /= 10) > 0);
            for (size_t i = len; i < width; i++)
                Put(" ");
            while (len > 0 && pos + 1 < sizeof(buf))
                buf[pos++] = d[--len];
        }
    };

    /** Where the time went: each segment's mean split into the callback's parts */
    void WriteParts(Text& t)
    {
        // the core's clock, measured two ways: against the audio's own clock (a block's cycles
        // from one block to the next, 0.5 ms by the codec), and against the system timer, whose
        // rate libDaisy works out from the clock settings rather than measures
        const float block_s = static_cast<float>(block_size_) / 48000.f;
        const float core_hz = all_blocks_ > 0 ? all_periods_ / all_blocks_ / block_s : 0.f;
        const float timer_hz = all_ticks_ > 0 ? static_cast<float>(all_cycles_) / all_ticks_
                                                    * daisy::System::GetTickFreq()
                                              : 0.f;
        t.Put("\n# the chip: core ");
#ifdef __arm__
        t.UInt(SystemCoreClock / 1000000);
        t.Put(" MHz set, ");
#endif
        t.UInt(static_cast<uint32_t>(core_hz / 1e6f + .5f));
        t.Put(" MHz by the audio clock, ");
        t.UInt(static_cast<uint32_t>(timer_hz / 1e6f + .5f));
        t.Put(" MHz by the system timer (");
        t.UInt(daisy::System::GetTickFreq() / 1000000);
        t.Put(" MHz)");
#ifdef __arm__
        t.Put(", HCLK ");
        t.UInt(HAL_RCC_GetHCLKFreq() / 1000000);
        t.Put(" MHz, I-cache ");
        t.Put(SCB->CCR & SCB_CCR_IC_Msk ? "on" : "off");
        t.Put(", D-cache ");
        t.Put(SCB->CCR & SCB_CCR_DC_Msk ? "on" : "off");
        t.Put(", bootloader ");
        t.UInt(static_cast<uint32_t>(daisy::System::GetBootloaderVersion()));
#endif
        t.Put("\n# where the mean goes, % of the block by the audio clock (BenchProfile.h);\n");
        t.Put("# total: the whole callback so, to set against the mean above\n");
        static const char* const kParts[] = {"midi", "ctrl", "events", "input", "looper", "tempo",
                                             "fx",   "comp", "output", "rest",   "total"};
        const size_t fx = BenchProfile::FX0;
        PartsHeader(t, kParts, sizeof(kParts) / sizeof(kParts[0]), 7);
        for (size_t s = 0; s < kNumSegments; s++)
        {
            const float period = period_[s];
            float row[11] = {};
            float marked = 0.f;
            for (size_t p = 0; p < BenchProfile::kNumParts; p++)
                marked += parts_[s][p];
            for (size_t p = 0; p < fx; p++)
                row[p] = parts_[s][p];
            for (size_t e = 0; e < kNumSoundFx; e++)
                row[fx] += parts_[s][fx + e];
            row[fx + 1] = parts_[s][BenchProfile::COMP];
            row[fx + 2] = parts_[s][BenchProfile::OUTPUT];
            row[fx + 3] = cycles_[s] - marked; // the callback's cycles not in any part
            row[fx + 4] = cycles_[s];
            PartsRow(t, s, row, 11, period, 7);
        }
        t.Put("\n# the FX chain by effect, % of the block, each with its meter\n");
        PartsHeader(t, kFxNames, kNumSoundFx, 10);
        for (size_t s = 0; s < kNumSegments; s++)
            PartsRow(t, s, &parts_[s][fx], kNumSoundFx, period_[s], 10);
    }

    static void PartsHeader(Text& t, const char* const* names, size_t n, size_t width)
    {
        t.Put("# segment         ");
        for (size_t i = 0; i < n; i++)
        {
            const size_t col = t.pos;
            t.Put(" ");
            t.Pad(col + width - strlen(names[i]));
            t.Put(names[i]);
        }
        t.Put("\n");
    }

    static void PartsRow(Text& t, size_t s, const float* cycles, size_t n, float period,
                         size_t width)
    {
        const size_t line = t.pos;
        t.Put(kSegments[s].name);
        t.Pad(line + 18);
        for (size_t i = 0; i < n; i++)
        {
            t.Pad(t.pos + width - 6);
            t.Load(period > 0.f ? cycles[i] / period : 0.f);
        }
        t.Put("\n");
    }

    bool Write()
    {
        static Text t;
        t.pos = 0;
        t.Put("FRIZZ cpu bench, source " FRIZZ_BENCH_SOURCE "\n");
        t.Put("# of the 0.5 ms a block has; measured in FRIZZ-bench.bin, whose memory layout\n");
        t.Put("# isn't FRIZZ.bin's\n");
        t.Put("# segment           max %  mean %  still working\n");
        float worst = 0.f;
        size_t worst_at = 0;
        for (size_t s = 0; s < kNumSegments; s++)
        {
            const size_t line = t.pos;
            t.Put(kSegments[s].name);
            t.Pad(line + 18);
            t.Load(max_[s]);
            t.Put("  ");
            t.Load(mean_[s]);
            if (awake_[s])
                t.Pad(line + 35);
            for (size_t fx = 0; fx < kNumFx; fx++)
                if (awake_[s] & (1u << fx))
                {
                    t.Put(" ");
                    t.Put(kFxNames[fx]);
                }
            t.Put("\n");
            if (max_[s] > worst)
            {
                worst = max_[s];
                worst_at = s;
            }
        }
        t.Put("# worst: ");
        t.Put(kSegments[worst_at].name);
        t.Put("\n# loop: ");
        char len[8] = {};
        Percent(static_cast<float>(loop_frames_) / 48000.f / 100.f, len); // seconds, 1 decimal
        t.Put(len);
        t.Put(loop_missing_ || loop_frames_ == 0 ? " s, NOT playing in every loop segment\n"
                                                 : " s, playing in every loop segment\n");
        WriteParts(t);

        if (f_mount(fs_, path_, 1) != FR_OK)
            return false;
        EnterFrizzDir(); // a mount goes back to the root; SceneStore's paths are relative
        static FIL file;
        if (f_open(&file, "/FRIZZ/cpu.txt", FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
            return false;
        UINT written = 0;
        const FRESULT res = f_write(&file, t.buf, static_cast<UINT>(t.pos), &written);
        return f_close(&file) == FR_OK && res == FR_OK && written == t.pos;
    }

    float ticks_per_block_ = 1.f;
    uint32_t start_tick_ = 0;
    FATFS* fs_ = nullptr;
    const char* path_ = nullptr;
    volatile bool started_ = false, running_ = false;
    bool settling_ = false;
    bool written_ = false, stored_ = false;
    volatile size_t segment_ = 0;
    size_t blocks_ = 0;
    size_t next_ = 0;        // the segment a settle waits for
    size_t block_size_ = 24;
    double sum_ = 0.;
    float max_[kNumSegments] = {};
    float mean_[kNumSegments] = {};
    uint16_t awake_[kNumSegments] = {};
    // where the time goes (BenchProfile.h): this segment's sums so far, and each one's means
    uint32_t block_cycles_ = 0, block_ticks_ = 0;
    uint64_t part_sum_[BenchProfile::kNumParts] = {};
    uint32_t block_period_ = 0;
    uint64_t cycles_sum_ = 0, ticks_sum_ = 0, period_sum_ = 0;
    uint64_t all_cycles_ = 0, all_ticks_ = 0, all_periods_ = 0, all_blocks_ = 0;
    float parts_[kNumSegments][BenchProfile::kNumParts] = {};
    float cycles_[kNumSegments] = {}, period_[kNumSegments] = {};
    size_t loop_frames_ = 0;
    bool loop_missing_ = false;
    uint32_t lcg_ = 12345;
    Tune tune_;
    float in_l_[kMaxBlock] = {}, in_r_[kMaxBlock] = {};
    const float* inputs_[4];
};

// The clean segments first, the sends last: a delay or reverb tail would run on into the next
// segments (the delay sleeps only after 10 s of silence). The delay before the reverb, so the
// loop with the delay, the case that crackled, is measured on its own
const Bench::Segment Bench::kSegments[Bench::kNumSegments] = {
    {"idle", 0, 0},
    {"freezer", Bench::Bit(FX_FREEZER), 0},
    {"shifter", Bench::Bit(FX_SHIFTER), 0},
    {"folder", Bench::Bit(FX_FOLDER), 0},
    {"crusher", Bench::Bit(FX_CRUSHER), 0},
    {"filter", Bench::Bit(FX_FILTER), 0},
    {"flanger", Bench::Bit(FX_FLANGER), 0},
    {"resonator", Bench::Bit(FX_RESONATOR), 0},
    {"slicer", Bench::Bit(FX_SLICER), 0},
    {"warble", Bench::Bit(FX_WARBLE), 0},
    {"tapestop", Bench::Bit(FX_TAPESTOP), 0},
    {"compressor", 0, Bench::COMP},
    {"inserts", Bench::kInserts, 0},
    {"recording", 0, Bench::RECORD},
    {"loop", 0, Bench::LOOP},
    {"loop+inserts", Bench::kInserts, Bench::LOOP},
    {"loop+delay", Bench::Bit(FX_DELAY), Bench::LOOP},
    {"loop+scene4+delay", Bench::kScene4 | Bench::Bit(FX_DELAY), Bench::LOOP | Bench::SCENE4},
    {"loop+reverb", Bench::Bit(FX_REVERB), Bench::LOOP},
    {"everything", Bench::kEverything, Bench::COMP},
    {"stress", Bench::kEverything, Bench::STRESS | Bench::COMP},
    {"loop+everything", Bench::kEverything, Bench::LOOP | Bench::COMP},
    // after the 22 above, so they stay comparable with older cpu.txt files: the chaos key
    // gating the inserts on the loop, every 16th, and scrambling it
    {"loop+inserts+chaos", Bench::kInserts | Bench::Bit(FX_CHAOS), Bench::LOOP | Bench::CHAOS},
    // last: the segment after it would start with every insert's page 2 settling back to its
    // defaults, page 2's cost, as the chaos segment did after it (114 % for a block)
    {"loop+everything+page2", Bench::kEverything, Bench::LOOP | Bench::COMP | Bench::PAGE2},
};
constexpr uint8_t Bench::kKeyLeds[];

} // namespace chompi
