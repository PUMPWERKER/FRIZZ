/** @file FxChain.h
 *  @brief The punch-in effects in their processing order, with a level meter each for the
 *  key LEDs. The engine runs the chain on the summed signal, after the dry/wet mix and
 *  before the output gain and master compressor (passthroughEngine.h). FxSlots.h says which
 *  key and LEDs go with each effect, FxParams.h its knobs.
 *
 *  The order follows a pedalboard's: source, pitch, dirt, filter, modulation, gate, tape,
 *  space. The keys follow it too, left to right:
 *   freezer -> shifter -> folder -> crusher -> filter -> flanger -> slicer -> wow & flutter
 *              |<-------------- resonator loop --------------->|
 *     -> tape stop -> delay -> reverb
 *  The freezer comes first so it captures the clean sound and everything after it works on
 *  the repeats. The folder comes before the crusher, so it folds the clean signal and the
 *  crusher grinds the folds. The filter sweeps the dirt, and the flanger the harmonics it
 *  made. The slicer is the last insert, the final gate: it chops everything including the
 *  resonator's ringing, and the sends get the chopped sound. Wow & flutter and the tape stop
 *  come after it, so they bend everything before them; the sends after them, so a tape stop
 *  leaves the echoes and the reverb ringing. The delay's echoes feed the reverb as well as the
 *  output.
 *
 *  Three kinds:
 *   - insert: replaces the signal while its key is on; only the wet amount is faded. Page 2's
 *     Mix, Band and Level wrap it (FxOutput.h).
 *   - send (delay, reverb): the key fades what goes into the effect, and its return is added
 *     to the signal, so tails ring out after the key is released. Page 2's Band filters what
 *     goes in; the rest of their page 2 is their own.
 *   - loop (resonator): a comb feedback loop from after the flanger back to after the
 *     freezer, so the filter is in the loop and the slicer outside it. Page 2's Band filters
 *     what goes into the loop, its Level is the return's (FxResonator.h).
 *
 *  And the chaos key (FxChaos.h), last of the FxIds though its key sits between the tape stop
 *  and the delay: no sound and no place in the chain, it gates the others. The keys reach the
 *  effects a block after they're set, together with what chaos drops (Block), so a key set
 *  from MainLoop and one chaos changes in the audio callback never cross.
 */
#pragma once
#include <atomic>
#include "BenchProfile.h"
#include "EnvFollower.h"
#include "FxChaos.h"
#include "FxCrusher.h"
#include "FxDelay.h"
#include "FxFilter.h"
#include "FxFlanger.h"
#include "FxFolder.h"
#include "FxFreezer.h"
#include "FxOutput.h"
#include "FxResonator.h"
#include "FxReverb.h"
#include "FxShifter.h"
#include "FxSlicer.h"
#include "FxTapeStop.h"
#include "FxWarble.h"

namespace chompi
{

/** The effects, in the order of their keys, left to right (FxSlots.h) */
enum FxId
{
    FX_FREEZER,
    FX_SHIFTER,
    FX_FOLDER,
    FX_CRUSHER,
    FX_FILTER,
    FX_FLANGER,
    FX_RESONATOR,
    FX_SLICER,
    FX_WARBLE,
    FX_TAPESTOP,
    FX_DELAY,
    FX_REVERB,
    FX_CHAOS, // the 11th white key, but last here so the others keep their numbers (MIDI)
    kNumFx,
};
// The effects in the chain, with a sound of their own: all but the chaos key
static const size_t kNumSoundFx = FX_CHAOS;
#if FRIZZ_BENCH
static_assert(BenchProfile::COMP - BenchProfile::FX0 == kNumSoundFx, "a bench part per effect");
#endif

/** Their names in the scene file (FxScenes.h): fixed, so saved scenes survive new effects
 *  and a new order */
static const char* const kFxNames[] = {
    "freezer", "shifter", "folder", "crusher", "filter",
    "flanger", "resonator", "slicer", "warble", "tapestop",
    "delay", "reverb", "chaos",
};
static_assert(sizeof(kFxNames) / sizeof(kFxNames[0]) == kNumFx, "one per FxId");

// How long a scene recall's fast slew lasts, 50ms at 48kHz: 10 of its time constants
// (FxCommon.h), well past where it has settled
static const uint32_t kFxRecallSlewSamples = 2400;

// Into the meters' EnvFollowers, which add 5x: full brightness at about 1 (L+R)/2
static const float kFxMeterScale = .1f;

class FxChain
{
public:
    /** The delay's, the reverb's, the freezer's and the tape stop's buffers are statics in
     *  chompi_main.cpp: SDRAM for the delay, freezer and tape stop, DTCMRAM for the reverb.
     *  Once at boot */
    FX_ONCE void Init(float sample_rate,
              float* delay_mem, size_t delay_frames,
              daisysp::Reverb* reverb,
              float* freezer_mem_l, float* freezer_mem_r, size_t freezer_frames,
              float* tapestop_mem_l, float* tapestop_mem_r, size_t tapestop_frames)
    {
        filter_.Init(sample_rate);
        crusher_.Init(sample_rate);
        folder_.Init(sample_rate);
        delay_.Init(sample_rate, delay_mem, delay_frames);
        reverb_.Init(sample_rate, reverb);
        freezer_.Init(sample_rate, freezer_mem_l, freezer_mem_r, freezer_frames);
        slicer_.Init(sample_rate);
        flanger_.Init(sample_rate);
        shifter_.Init(sample_rate);
        resonator_.Init(sample_rate);
        warble_.Init(sample_rate);
        tapestop_.Init(sample_rate, tapestop_mem_l, tapestop_mem_r, tapestop_frames);
        chaos_.Init();

        fx_[FX_FILTER] = &filter_;
        fx_[FX_CRUSHER] = &crusher_;
        fx_[FX_FOLDER] = &folder_;
        fx_[FX_FREEZER] = &freezer_;
        fx_[FX_SLICER] = &slicer_;
        fx_[FX_FLANGER] = &flanger_;
        fx_[FX_SHIFTER] = &shifter_;
        fx_[FX_RESONATOR] = &resonator_;
        fx_[FX_WARBLE] = &warble_;
        fx_[FX_TAPESTOP] = &tapestop_;
        fx_[FX_DELAY] = &delay_;
        fx_[FX_REVERB] = &reverb_;
        fx_[FX_CHAOS] = &chaos_;

        for (size_t fx = 0; fx < kNumFx; fx++)
            meter_[fx].Init();
        for (size_t fx = 0; fx < kNumSoundFx; fx++)
            out_[fx].Init(sample_rate);
        out_busy_.store(0);
        keys_.store(0);
        applied_ = pool_ = dropped_ = 0;
        fast_slew_left_ = 0;
    }

    /** Once per block: the tempo and the pulses' real spacing in samples (TempoClock), then
     *  one call per clock pulse in the block with the clock's position (TempoClock::Pulse) */
    void SetTempo(float bpm, float pulse_samples)
    {
        delay_.SetTempo(bpm);
        filter_.SetPulseSamples(pulse_samples);
        slicer_.SetPulseSamples(pulse_samples);
        freezer_.SetTempo(bpm);
        tapestop_.SetTempo(bpm);
    }
    /** reverse: the position counts down, a loop playing backwards (TempoClock.h) */
    void ClockPulse(uint32_t pos, bool reverse = false)
    {
        delay_.ClockPulse(pos);
        filter_.ClockPulse(pos, reverse);
        freezer_.ClockPulse(pos);
        slicer_.ClockPulse(pos);
        chaos_.ClockPulse(pos);
    }

    /** Once per block, after the pulses and the morph, before the samples: the keys set
     *  since, and what chaos drops of its pool. True on a chaos step (FxChaos.h), with where
     *  the loop plays it */
    bool Block(float* jump)
    {
        // the chaos key's own first, so it drops nothing from the block it goes off in
        const uint32_t keys = keys_.load(std::memory_order_relaxed);
        const uint32_t chaos = 1u << FX_CHAOS;
        if ((keys ^ applied_) & chaos)
        {
            applied_ ^= chaos;
            chaos_.SetOn(keys & chaos);
        }
        const bool step = chaos_.TakeStep(jump);
        const uint32_t drop = chaos_.Drops() & pool_;
        const uint32_t want = keys & ~drop;
        dropped_ = drop;
        if (want != applied_)
        {
            const uint32_t changed = want ^ applied_;
            applied_ = want;
            for (size_t fx = 0; fx < kNumFx; fx++)
                if (changed >> fx & 1)
                {
                    const bool on = want >> fx & 1;
                    if (on)
                        KeyOn(fx);
                    fx_[fx]->SetOn(on);
                }
        }
        return step;
    }
    /** The chaos key's pool, from the UI: the effects it may drop (FxControls::Pool) */
    inline void SetPool(uint16_t pool) { pool_ = pool; }
    /** Bit fx: dropped from this step by chaos, for the key LEDs */
    inline uint16_t Dropped() const { return static_cast<uint16_t>(dropped_); }
    /** Its grid in clock pulses, for the scramble's steps */
    inline uint32_t ChaosGridPulses() const { return chaos_.GridPulses(); }

    /** One sample through the chain. The meters follow each insert's output and each send's
     *  return, so the send keys show the tails. */
    FRIZZ_HOT void Process(float* l, float* r)
    {
        if (fast_slew_left_ > 0 && --fast_slew_left_ == 0)
            FxSlew::coeff = kFxParamCoeff;

        Insert(FX_FREEZER, freezer_, l, r);
        // the resonator's loop wraps everything from here to the flanger
        resonator_.Feed(l, r);
        if (!resonator_.Idle())
            Meter(FX_RESONATOR, resonator_.Return());
        BENCH_MARK_FX(FX_RESONATOR);
        Insert(FX_SHIFTER, shifter_, l, r);
        Insert(FX_FOLDER, folder_, l, r);
        Insert(FX_CRUSHER, crusher_, l, r);
        Insert(FX_FILTER, filter_, l, r);
        Insert(FX_FLANGER, flanger_, l, r);
        {
            float tl = *l, tr = *r;
            SendBand(FX_RESONATOR, resonator_.Idle(), &tl, &tr);
            resonator_.Tap(tl, tr);
        }
        BENCH_MARK_FX(FX_RESONATOR);
        Insert(FX_SLICER, slicer_, l, r);
        Insert(FX_WARBLE, warble_, l, r);
        Insert(FX_TAPESTOP, tapestop_, l, r);

        // sends: the delay from the inserts' output, the reverb from that plus the delay's
        // return, so the echoes are reverberated. Both returns are added on top.
        const float sendl = *l, sendr = *r;
        float inl = sendl, inr = sendr;
        SendBand(FX_DELAY, delay_.Idle(), &inl, &inr); // what goes in is faded with the key
        delay_.Process(inl, inr, l, r);
        const float delayl = *l, delayr = *r;
        if (!delay_.Sleeping())
            Meter(FX_DELAY, delayl - sendl + delayr - sendr);
        BENCH_MARK_FX(FX_DELAY);
        inl = delayl;
        inr = delayr;
        SendBand(FX_REVERB, reverb_.Idle(), &inl, &inr);
        reverb_.Process(inl, inr, l, r);
        if (!reverb_.Sleeping())
            Meter(FX_REVERB, *l - delayl + *r - delayr);
        BENCH_MARK_FX(FX_REVERB);
    }

    /** One insert, with its page 2's Mix, Band and Level (out_[fx]) while they're Busy, and
     *  its meter. The effect's own Process in one place, so it's inlined once */
    template <class Fx>
    inline void Insert(size_t fx, Fx& effect, float* l, float* r)
    {
        // the busy case out of line and at the end, so a chain at its defaults runs through
        // as little code as without page 2: the audio callback is bound by the I-cache (#51)
        const bool busy = __builtin_expect((OutBusy() & (1u << fx)) != 0, 0);
        bool split = false;
        if (busy)
            split = OutBegin(fx, effect.Quiet(), l, r);
        effect.Process(l, r);
        if (busy)
            OutEnd(fx, split, effect.Fade(), l, r);
        if (!effect.Idle())
            Meter(fx, *l + *r);
        BENCH_MARK_FX(fx);
    }
    __attribute__((noinline, cold)) bool OutBegin(size_t fx, bool idle, float* l, float* r)
    {
        const bool split = out_[fx].Begin(idle, l, r);
        // off and faded out with its knobs at rest: nothing to do until its key comes on
        // again, which sets it busy again (KeyOn)
        if (idle && !out_[fx].Moving())
            ClearOutBusy(fx);
        return split;
    }
    __attribute__((noinline, cold)) void OutEnd(size_t fx, bool split, float fade, float* l,
                                                float* r)
    {
        if (!out_[fx].End(split, l, r, fade))
            ClearOutBusy(fx);
    }

    /** What goes into a send or the resonator's loop, through its page 2's Band while that's
     *  Banding and the effect is on (idle: its Idle()) */
    inline void SendBand(size_t fx, bool idle, float* l, float* r)
    {
        if (__builtin_expect(!idle && (OutBusy() & (1u << fx)), 0))
            OutBand(fx, l, r);
    }
    __attribute__((noinline, cold)) void OutBand(size_t fx, float* l, float* r)
    {
        if (!out_[fx].Band(l, r))
            ClearOutBusy(fx);
    }

    /** From the UI or the morph: the key reaches the effect at the next Block */
    inline void SetOn(size_t fx, bool on)
    {
        if (on)
            keys_.fetch_or(1u << fx, std::memory_order_relaxed);
        else
            keys_.fetch_and(~(1u << fx), std::memory_order_relaxed);
    }
    __attribute__((noinline)) void SetParam(size_t fx, size_t param, float val)
    {
        if (fx == FX_CHAOS)
        {
            chaos_.SetParam(param, val);
            return;
        }
        // page 2's shared knobs go to the effect's FxOutput: all three on an insert, Band
        // alone on the sends and the resonator, whose other page-2 knobs are their own
        const bool own = fx == FX_DELAY || fx == FX_REVERB || fx == FX_RESONATOR;
        if (own ? param == FxOutput::kBand && out_[fx].SetParam(param, val)
                : out_[fx].SetParam(param, val))
        {
            // after the knob is set, so the audio sees it moving
            out_busy_.fetch_or(static_cast<uint16_t>(1u << fx), std::memory_order_release);
            return;
        }
        fx_[fx]->SetParam(param, val);
    }
    /** Before a scene recall's SetParams: the knobs slew at kFxRecallCoeff for
     *  kFxRecallSlewSamples, so the new scene lands at once */
    void FastSlew()
    {
        FxSlew::coeff = kFxRecallCoeff;
        fast_slew_left_ = kFxRecallSlewSamples;
    }
    /** Whether fx's page 2 is part of the chain's work now (out_busy_), for the tests */
    inline bool OutRunning(size_t fx) const { return OutBusy() >> fx & 1; }
    /** 0..1, for the key LEDs */
    inline float GetLevel(size_t fx) { return meter_[fx].GetLastSamp(); }

    /** Off and doing no more than it does off: an insert faded out, a send whose tail has gone
     *  quiet (TailWatch). For the CPU bench (Bench.h), which reports what was still working */
    __attribute__((noinline)) bool Resting(size_t fx) const // the bench calls it in many places
    {
        if (fx == FX_DELAY)
            return delay_.Sleeping();
        if (fx == FX_REVERB)
            return reverb_.Sleeping();
        if (fx == FX_TAPESTOP)
            return tapestop_.Resting();
        return fx_[fx]->Idle();
    }

private:
    inline void Meter(size_t fx, float sum) { meter_[fx].Process(sum * kFxMeterScale); }
    inline uint16_t OutBusy() const { return out_busy_.load(std::memory_order_relaxed); }
    inline void ClearOutBusy(size_t fx)
    {
        out_busy_.fetch_and(static_cast<uint16_t>(~(1u << fx)), std::memory_order_relaxed);
    }

    /** An effect's key comes on (or chaos lets it through again), at Block, before the
     *  effect has it. From quiet, its meter and its Band start afresh (a send's isn't run while it's
     *  idle, so it kept what it held when the key went off: a decaying offset that the delay
     *  repeated); and its page 2 runs again if it's off its defaults (an insert that's off
     *  leaves it out, OutBegin) */
    inline void KeyOn(size_t fx)
    {
        if (fx >= kNumSoundFx)
            return;
        // the tape stop sounds on after its key while it spins up: not quiet until it rests
        if (fx == FX_TAPESTOP ? tapestop_.Resting() : fx_[fx]->Idle())
            out_[fx].Rest();
        // an insert's or the resonator's meter stood still while it was idle: from nothing,
        // so its key doesn't flash at the old level. A send's runs on with its tail
        if (fx < FX_DELAY && fx_[fx]->Idle())
            meter_[fx].Init();
        if (out_[fx].Busy())
            out_busy_.fetch_or(static_cast<uint16_t>(1u << fx), std::memory_order_relaxed);
    }

    Filter filter_;
    Crusher crusher_;
    Folder folder_;
    Freezer freezer_;
    Slicer slicer_;
    Flanger flanger_;
    Shifter shifter_;
    Resonator resonator_;
    Warble warble_;
    TapeStop tapestop_;
    DelaySend delay_;
    ReverbSend reverb_;
    Chaos chaos_;
    FxBase* fx_[kNumFx];
    FxOutput out_[kNumSoundFx]; // page 2's Mix, Band and Level (FxOutput.h)
    // the effects whose out_ is Busy (an insert) or Banding (a send, the resonator), by bit:
    // the rest run alone, their out_ untouched, which keeps a chain at its defaults as cheap
    // as without them, and an insert that's off too. Set by SetParam (from MainLoop, or the
    // morph in the audio callback) and KeyOn, cleared by Insert and SendBand: atomic, so a
    // bit the audio callback sets or clears isn't lost to MainLoop's
    std::atomic<uint16_t> out_busy_{0};
    EnvFollower meter_[kNumFx];
    uint32_t fast_slew_left_; // samples of FastSlew to go
    std::atomic<uint32_t> keys_{0}; // bit fx: its key on, as the UI or the morph set it
    uint32_t applied_ = 0;          // and as the effects have it, less what chaos drops
    volatile uint32_t pool_ = 0;    // the chaos key's pool (SetPool)
    volatile uint32_t dropped_ = 0; // what it drops of it now
};

} // namespace chompi
