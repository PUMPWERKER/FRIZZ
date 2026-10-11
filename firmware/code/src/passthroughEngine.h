/** @file passthroughEngine.h
 *  @brief Audio engine: the stereo AUX input goes to both outputs through the Volume
 *  Engine: input gain -> input/loop mix -> punch-in FX (FxChain.h) -> master compressor
 *  (MasterComp.h) -> output gain -> safety limiter.
 *
 *  In the code the mix is dry/wet: dry is the input on its own, wet is the looper's playback
 *  on its own. The looper records the dry signal (see Looper.h).
 *
 *  The headphones mirror the master out, or with SetHeadphoneCue() blend in the input on its
 *  own: after the input gain and VOLUME, but no loop, no FX, no MIX and no compressor. The
 *  headphones' safety limiter comes after that blend, so it guards the input too.
 *
 *  The FX's tempo (TempoClock.h) comes from the loop while there is one, otherwise from MIDI
 *  clock or taps. A scene morph (FxMorph.h) sits between the UI and the FX and lands on that
 *  clock's bar lines.
 *
 *  The input level, output level and safety limiter are ported from TAPE's DSPEngine
 *  so the gains match the hardware the way TAPE tuned them. The limiter is TAPE's master
 *  compressor at its lowest setting, which is what FRIZZ's own knob for it started at.
 */
#pragma once
#include "FrizzHot.h"
#include <atomic>
#include "daisy.h"
#include "daisysp.h"
#include "BenchProfile.h"
#include "EnvFollower.h"
#include "FxChain.h"
#include "FxMorph.h"
#include "limiter.h"
#include "Looper.h"
#include "MasterComp.h"
#include "TempoClock.h"

using namespace daisy;
using namespace daisysp;

static constexpr float kLineOutGain = .3f;
static constexpr float kHpGain = .2f;
static constexpr float kLineInGain = 3.f;
// the safety limiter's threshold and makeup (limiter.h)
static constexpr float kLimThresh = .25f;
static constexpr float kLimMakeup = .9f;

class PassthroughEngine
{
public:

    PassthroughEngine() {};
    ~PassthroughEngine() {};

    void Init(float sample_rate, int16_t* loop_mem, chompi::MidiClock* midi_clock,
              float* delay_mem, size_t delay_frames,
              daisysp::Reverb* reverb,
              float* freezer_mem_l, float* freezer_mem_r, size_t freezer_frames,
              float* tapestop_mem_l, float* tapestop_mem_r, size_t tapestop_frames)
    {
        sample_rate_ = sample_rate;
        looper.Init(loop_mem, midi_clock);
        tempo_clock_.Init(sample_rate, midi_clock);
        fx_.Init(sample_rate, delay_mem, delay_frames, reverb,
                 freezer_mem_l, freezer_mem_r, freezer_frames,
                 tapestop_mem_l, tapestop_mem_r, tapestop_frames);
        morph_.Init(&fx_);
        comp_.Init(sample_rate);

        dcblock_line_in_l_.Init(sample_rate);
        dcblock_line_in_r_.Init(sample_rate);

        lim_hp_l_.Init();
        lim_hp_r_.Init();
        lim_line_l_.Init();
        lim_line_r_.Init();

        output_env_follower.Init();
    }

    /** Inputs: 0 mic (unused), 1 X, 2 aux L, 3 aux R
     *  Outputs: 0/1 headphone L/R, 2/3 master L/R */
    FRIZZ_HOT void Process(const float *const *in, float **out, size_t size)
    {
        float dryl[size], dryr[size], wetl[size], wetr[size];

        for (size_t i = 0; i < size; i++)
        {
            // Every setting has a target and a live value; fonepole() slews the live
            // value toward the target (a ~21ms time constant) so knob turns don't zipper.
            fonepole(ingain_, ingain_target_, .001f);

            // mono: a TS plug grounds the right channel, so the left (its tip) feeds both
            const float in_r = mono_in_ ? in[2][i] : in[3][i];
            dryl[i] = dcblock_line_in_l_.Process(in[2][i] * ingain_ * kLineInGain);
            dryr[i] = dcblock_line_in_r_.Process(in_r * ingain_ * kLineInGain);
        }
        BENCH_MARK(INPUT);

        looper.Process(dryl, dryr, wetl, wetr, size);
        BENCH_MARK(LOOPER);

        // the FX's tempo and clock, once per block, from the loop while there is one
        SyncLoopTempo();
        const float tap = tap_bpm_.exchange(0.f);
        if (tap > 0.f)
            tempo_clock_.Tap(tap);
        const uint32_t pulses = tempo_clock_.Process(
            size, looper.GetPosition(), looper.GetActualSpeed(),
            looper.GetState() == chompi::Looper::State::PAUSED);
        fx_.SetTempo(tempo_clock_.GetFxBpm(), tempo_clock_.PulseSamples());
        // where the loop was before this block's pulses, for MIDI out's next 16th (MidiOut.h)
        next_16th_ = tempo_clock_.HasLoop() ? tempo_clock_.NextLoopPulseOn(chompi::kPulsesPer16th) : 0;
        block_pulses_ = 0;
        for (uint32_t p = 0; p < pulses; p++)
        {
            const uint32_t pos = tempo_clock_.Pulse();
            fx_.ClockPulse(pos, tempo_clock_.Reverse());
            morph_.Pulse(chompi::TempoClock::IsBarLine(pos));
            if (block_pulses_ < kMaxBlockPulses)
                block_loop_pulse_[block_pulses_++] = tempo_clock_.LoopPulse();
        }
        morph_.Process(size, tempo_clock_.PulseSamples());
        // the keys, what the chaos key drops of them, and where it plays the loop
        float jump;
        if (fx_.Block(&jump))
            Scramble(jump);
        BENCH_MARK(TEMPO);

        for (size_t i = 0; i < size; i++)
        {
            fonepole(mgain_, mgain_target_, .001f);
            // equal-power crossfade so the middle of the knob doesn't dip in level; the
            // cosf and sinf only while the knob slews, the mix usually sits still
            if (mix_ != mix_target_)
            {
                fonepole(mix_, mix_target_, .001f);
                if (fabsf(mix_ - mix_target_) < 1e-5f)
                    mix_ = mix_target_;
                dry_amt_ = cosf(mix_ * HALFPI_F);
                wet_amt_ = sinf(mix_ * HALFPI_F);
            }
            const float dry_amt = dry_amt_;
            const float wet_amt = wet_amt_;
            float sigl = dryl[i] * dry_amt + wetl[i] * wet_amt;
            float sigr = dryr[i] * dry_amt + wetr[i] * wet_amt;

            // punch-in FX, on the mix so they work on the input, the loop or both, and
            // before the output gain so they don't change with the VOLUME knob
            BENCH_MARK(OUTPUT);
            fx_.Process(&sigl, &sigr); // marks each effect itself
            comp_.Process(&sigl, &sigr);
            BENCH_MARK(COMP);

            // headphone and master gain
            const float hpl = sigl * kHpGain * mgain_;
            const float hpr = sigr * kHpGain * mgain_;
            out[2][i] = sigl * kLineOutGain * mgain_;
            out[3][i] = sigr * kLineOutGain * mgain_;

            // the VU meter shows the master's signal (at the headphones' level, before the
            // limiter), whatever the headphones carry
            output_env_follower.Process(hpl + hpr);

            // headphone feed: the cue blends from the master's mirror to the input on its own,
            // slewed so a jump doesn't click; at rest on the mirror (as it usually is) no blend
            if (hp_cue_ != hp_cue_target_)
                fonepole(hp_cue_, hp_cue_target_, .001f);
            if (hp_cue_ == 0.f)
            {
                out[0][i] = hpl;
                out[1][i] = hpr;
            }
            else
            {
                out[0][i] = hpl + (dryl[i] * kHpGain * mgain_ - hpl) * hp_cue_;
                out[1][i] = hpr + (dryr[i] * kHpGain * mgain_ - hpr) * hp_cue_;
            }

            // safety limiter: TAPE's master compressor at its lowest setting (limiter.h)
            out[0][i] = lim_hp_l_.ProcessComp(out[0][i], 1.f, kLimThresh, 1.f, kLimMakeup);
            out[1][i] = lim_hp_r_.ProcessComp(out[1][i], 1.f, kLimThresh, 1.f, kLimMakeup);
            out[2][i] = lim_line_l_.ProcessComp(out[2][i], 1.f, kLimThresh, 1.f, kLimMakeup);
            out[3][i] = lim_line_r_.ProcessComp(out[3][i], 1.f, kLimThresh, 1.f, kLimMakeup);
        }
        // the line outs' deepest limiting this block, for the compressor key's LED; kept the
        // deepest until the play page takes it
        const float lim = fminf(lim_line_l_.Gain(), lim_line_r_.Gain());
        if (lim < lim_gain_)
            lim_gain_ = lim;
        BENCH_MARK(OUTPUT);
    }

    inline void SetMainGain(float gain) { mgain_target_ = gain; }
    inline void SetInputGain(float gain) { ingain_target_ = gain; }
    /** The master compressor's knobs (MasterComp.h), 0..1 */
    inline void SetCompParam(size_t param, float val) { comp_.SetParam(param, val); }
    /** Its gain reduction now, in dB (<= 0) */
    inline float GetCompReduction() const { return comp_.GetReduction(); }
    /** The safety limiter's deepest gain on the line outs since the last call, 0..1 (1: it
     *  didn't limit). From MainLoop: a float read and written whole, so no lock */
    inline float TakeLimiterGain()
    {
        const float g = lim_gain_;
        lim_gain_ = 1.f;
        return g;
    }
    /** 0 = dry (input only), 1 = wet (looper/buffer only) */
    inline void SetMix(float mix) { mix_target_ = mix; }
    /** Headphones: 0 = mirror the master out, 1 = the dry input on its own */
    inline void SetHeadphoneCue(float cue) { hp_cue_target_ = cue; }
    /** AUX input: false = stereo, true = mono, the left channel to both sides */
    inline void SetMonoInput(bool mono) { mono_in_ = mono; }

    /** Punch-in FX, by FxId (FxChain.h). While a morph runs, they go to it (FxMorph.h) */
    inline void SetFxOn(size_t fx, bool on)
    {
        if (!morph_.SetOn(fx, on))
            fx_.SetOn(fx, on);
    }
    inline void SetFxParam(size_t fx, size_t param, float val)
    {
        if (!morph_.SetParam(fx, param, val))
            fx_.SetParam(fx, param, val);
    }
    /** Before a scene recall's SetFxParams: they land at the recall's slew (FxCommon.h) */
    inline void FastFxSlew() { fx_.FastSlew(); }
    /** A scene morph (FxMorph.h) to the next bar line of the FX's clock, one more per
     *  AddFxMorphBar; held until ReleaseFxMorph (SHIFT let go); LandFxMorph ends it at once.
     *  All with the audio interrupt blocked */
    void StartFxMorph(const chompi::FxMorphPlan& plan)
    {
        morph_.Start(plan, tempo_clock_.PulsesToBarLine(), true);
    }
    void ReleaseFxMorph()
    {
        morph_.Release(tempo_clock_.PulsesToBarLine(), tempo_clock_.PulsesPerBarLine());
    }
    bool AddFxMorphBar() { return morph_.AddBar(tempo_clock_.PulsesPerBarLine()); }
    void LandFxMorph() { morph_.Land(); }
    /** Stops the morph where it is (FxMorph::Freeze); with the audio interrupt blocked */
    bool FreezeFxMorph(float params[chompi::kNumFx][chompi::kNumFxParams],
                       uint16_t* unswitched, uint16_t* was_on)
    {
        return morph_.Freeze(params, unswitched, was_on);
    }
    inline bool FxMorphing() const { return morph_.Active(); }
    /** Started and still held for SHIFT (FxMorph::Holding) */
    inline bool FxMorphHeld() const { return morph_.Holding(); }
    /** The crossfader (FxMorph::Fader): the morph at t, 0 where it started to 1 the scene,
     *  from now on in the UI's hands; with the audio interrupt blocked. False if none runs */
    bool FadeFxMorph(float t) { return morph_.Fader(t); }
    /** The tempo clock, and the pulses its last block counted with each one's place in the
     *  loop (TempoClock::LoopPulse), at most kMaxBlockPulses: for MIDI out (MidiOut.h) */
    inline const chompi::TempoClock& Tempo() const { return tempo_clock_; }
    inline uint32_t BlockPulses() const { return block_pulses_; }
    inline uint32_t BlockLoopPulse(uint32_t i) const { return block_loop_pulse_[i]; }
    /** With a loop: the loop pulse of the next 16th as the block began, before its pulses */
    inline uint32_t Next16th() const { return next_16th_; }
    static const uint32_t kMaxBlockPulses = 4;

    /** The chaos key's pool, the effects it may drop (FxChaos.h, FxControls::Pool) */
    inline void SetFxPool(uint16_t pool) { fx_.SetPool(pool); }
    /** Bit fx: dropped by the chaos key now, for the key LEDs */
    inline uint16_t FxDropped() const { return fx_.Dropped(); }
    /** The FX clock's position, 0..TempoClock's kPulsesPerCycle - 1, for blinking on its beats */
    inline uint32_t FxClockPosition() const { return tempo_clock_.Position(); }
    /** The tempo the effects follow, for MIDI's state query (MidiControl.h) */
    inline float FxBpm() const { return tempo_clock_.GetFxBpm(); }
    /** 0..1, for the FX key LEDs: an insert's output, a send's return */
    inline float GetFxLevel(size_t fx) { return fx_.GetLevel(fx); }
    /** Off and costing no more than off (FxChain::Resting): for the CPU bench */
    inline bool FxResting(size_t fx) const { return fx_.Resting(fx); }

    inline float GetVUSample() { return output_env_follower.GetLastSamp(); }

    /** A tapped tempo, from the UI (TapTempo.h): applied at the next block */
    inline void TapTempo(float bpm) { tap_bpm_.store(bpm); }
    /** Whether a tap would be taken: not without a loop while MIDI clock runs */
    inline bool CanTap() const { return tempo_clock_.CanTap(); }

    chompi::Looper looper;

private:
    /** A chaos step's scramble (FxChaos.h): the loop plays the step jump of the way round its
     *  other steps (on the chaos key's grid), or in place at 0, or while it isn't playing */
    void Scramble(float jump)
    {
        const uint32_t loop = tempo_clock_.LoopPulses(), grid = fx_.ChaosGridPulses();
        const uint32_t steps = loop / grid; // whole steps: a loop of 5 beats has 2 halves
        size_t offset = 0;
        if (jump > 0.f && steps >= 2 && looper.GetState() == chompi::Looper::State::PLAYING)
        {
            uint32_t n = 1 + static_cast<uint32_t>(jump * static_cast<float>(steps - 1));
            n = n >= steps ? steps - 1 : n;
            // n steps of the grid on, in the loop's frames: always on a step's start
            offset = static_cast<size_t>(static_cast<uint64_t>(n) * grid * looper.GetLength() / loop);
        }
        looper.Scramble(offset);
    }

    /** Hands a loop that just closed to the tempo clock, and takes an erased one away. A
     *  quantized loop knows its beats; an unquantized one fits the tempo set before it, or
     *  is guessed */
    void SyncLoopTempo()
    {
        const chompi::Looper::State state = looper.GetState();
        const bool loop = state == chompi::Looper::State::PLAYING
                          || state == chompi::Looper::State::PAUSED;
        if (loop && !tempo_clock_.HasLoop())
        {
            const size_t length = looper.GetLength();
            uint32_t beats = looper.GetBeats();
            if (beats == 0)
                beats = tempo_clock_.TempoSet()
                            ? chompi::FitBeats(length, sample_rate_, tempo_clock_.GetBpm())
                            : chompi::GuessBeats(length, sample_rate_);
            tempo_clock_.SetLoop(length, beats);
        }
        else if (!loop && tempo_clock_.HasLoop())
            tempo_clock_.ClearLoop();
    }

    float sample_rate_;
    std::atomic<float> tap_bpm_{0.f};
    daisysp::DcBlock dcblock_line_in_l_, dcblock_line_in_r_;
    chompi::Limiter lim_hp_l_, lim_hp_r_, lim_line_l_, lim_line_r_;
    volatile float lim_gain_ = 1.f; // TakeLimiterGain
    chompi::EnvFollower output_env_follower;
    chompi::TempoClock tempo_clock_;
    uint32_t block_pulses_ = 0; // the last block's pulses, up to kMaxBlockPulses
    uint32_t block_loop_pulse_[kMaxBlockPulses] = {};
    uint32_t next_16th_ = 0;
    chompi::FxChain fx_;
    chompi::FxMorph morph_;
    chompi::MasterComp comp_;
    volatile bool mono_in_ = false;
    // live values start at 0 and slew up to the targets the play page sets at boot
    float mgain_ = 0.f, mgain_target_ = 0.f;
    float ingain_ = 0.f, ingain_target_ = 0.f;
    float mix_ = 0.f, mix_target_ = 0.f;
    float dry_amt_ = 1.f, wet_amt_ = 0.f; // the crossfade at mix_
    float hp_cue_ = 0.f, hp_cue_target_ = 0.f;
};
