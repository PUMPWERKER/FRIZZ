/** @file passthroughEngine.h
 *  @brief Audio engine: the stereo AUX input, or the built-in mic, goes to both outputs
 *  through the Volume Engine: input gain -> input/loop mix -> punch-in FX (FxChain.h) ->
 *  master compressor (MasterComp.h) -> output gain -> safety limiter.
 *
 *  The source is AUX or the mic (#152), and each has its own settings (#177): live or not, on
 *  the outputs at all; and if live, through the effects, as above, or dry, its share of the
 *  mix added after the compressor, past the effects and the compressor (a mastered song, a
 *  drum machine), still through VOLUME and the limiter. The mic is off unless asked for: a
 *  speaker would feed it back. The looper records the source as it is, whatever those say,
 *  and so do the headphones' input on its own. Each source has its own input gain.
 *
 *  A switch fades over kMicFadeSamples. Each source has its own gates to the effects and past
 *  them, each fading towards its own target, and each takes only its own source: so during a
 *  switch of the source the one going fades out of the outputs while the one coming fades in,
 *  and a source that isn't live adds nothing at any sample (#168: one gate on the crossfaded
 *  input let the mic through, up to a quarter, while the two fades crossed).
 *
 *  The mic (SetMicInput) is TAPE's: DC blocked after the input gain and its own x5, highpassed
 *  (MicFilter.h), and the same on both sides. It replaces AUX for everything that takes the
 *  input: the looper, the headphones' input on its own. The mic's filter runs only while the
 *  mic is heard.
 *
 *  In the code the mix is dry/wet: dry is the input on its own, wet is the looper's playback
 *  on its own. The looper records the dry signal (see Looper.h).
 *
 *  The headphones mirror the master out, or with SetHeadphoneCue() blend in the input on its
 *  own: after the input gain and VOLUME, but no loop, no FX, no MIX and no compressor. The
 *  headphones' safety limiter comes after that blend, so it guards the input too. They go
 *  out turned against the line out: the CHOMPI's headphone stages don't turn the signal, its
 *  AUX in and line out stages do, so turned they're in phase with AUX at the jacks (#209).
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
#include "MicFilter.h"
#include "TempoClock.h"

using namespace daisy;
using namespace daisysp;

static constexpr float kLineOutGain = .3f;
static constexpr float kHpGain = .2f;
static constexpr float kLineInGain = 3.f;
static constexpr float kMicGain = 5.f; // TAPE's, after the input gain as there
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
        dcblock_mic_in_.Init(sample_rate);
        mic_filter_.Init(sample_rate);

        lim_hp_l_.Init();
        lim_hp_r_.Init();
        lim_line_l_.Init();
        lim_line_r_.Init();

        output_env_follower.Init();
    }

    /** Inputs: 0 mic, 1 X, 2 aux L, 3 aux R
     *  Outputs: 0/1 headphone L/R, 2/3 master L/R */
    FRIZZ_HOT void Process(const float *const *in, float **out, size_t size)
    {
        float dryl[size], dryr[size], wetl[size], wetr[size];
        // the input's share through the effects and past them, before the mix; filled here
        // only during a switch of the source (ProcessSwitch)
        float sigl[size], sigr[size], bypl[size], bypr[size];

        // the source heard, and where it goes: through the effects, past them, or nowhere
        const bool mic = mic_in_;
        const size_t src = mic ? 1 : 0, other = mic ? 0 : 1;
        const bool live = live_[src], dry = dry_[src];
        const float to_fx = live && !dry ? 1.f : 0.f;
        const float to_dry = live && dry ? 1.f : 0.f;
        // a switch of the source done: the input at it alone, the other's gates shut
        const bool switching = mic_fade_ != (mic ? 1.f : 0.f) || in_fx_[other] != 0.f
                               || in_dry_[other] != 0.f;
        if (switching)
            ProcessSwitch(in, dryl, dryr, sigl, sigr, bypl, bypr, size, mic, to_fx, to_dry);
        else if (!mic)
        {
            // AUX alone, as it usually is; the mic's gain, unheard, stands at its target
            mic_gain_ = mic_gain_target_;
            for (size_t i = 0; i < size; i++)
            {
                // Every setting has a target and a live value; fonepole() slews the live
                // value toward the target (a ~21ms time constant) so knob turns don't zipper.
                fonepole(aux_gain_, aux_gain_target_, .001f);

                // mono: a TS plug grounds the right channel, so the left (its tip) feeds both
                const float in_r = mono_in_ ? in[2][i] : in[3][i];
                dryl[i] = dcblock_line_in_l_.Process(in[2][i] * aux_gain_ * kLineInGain);
                dryr[i] = dcblock_line_in_r_.Process(in_r * aux_gain_ * kLineInGain);
            }
        }
        else
        {
            // the mic alone; AUX's gain, unheard, at its target
            aux_gain_ = aux_gain_target_;
            for (size_t i = 0; i < size; i++)
            {
                fonepole(mic_gain_, mic_gain_target_, .001f);
                dryl[i] = dryr[i] = Mic(in[0][i]);
            }
        }
        // the input's peak as the looper records it, while the UI shows its meter
        if (in_meter_)
        {
            float peak = in_peak_;
            for (size_t i = 0; i < size; i++)
                peak = fmaxf(peak, fmaxf(fabsf(dryl[i]), fabsf(dryr[i])));
            in_peak_ = peak;
        }
        BENCH_MARK(INPUT);

        looper.Process(dryl, dryr, wetl, wetr, size);
        BENCH_MARK(LOOPER);

        // the FX's tempo and clock, once per block, from the loop while there is one
        SyncLoopTempo();
        if (looper.Rewinds() != rewinds_)
        {
            rewinds_ = looper.Rewinds();
            tempo_clock_.Rewound();
        }
        const float tap = tap_bpm_.exchange(0.f);
        if (tap > 0.f)
            tempo_clock_.Tap(tap);
        const uint32_t pulses = tempo_clock_.Process(
            size, looper.GetPosition(), looper.GetActualSpeed(),
            looper.GetState() == chompi::Looper::State::PAUSED || looper.Standing());
        fx_.SetTempo(tempo_clock_.GetFxBpm(), tempo_clock_.PulseSamples());
        // where the loop was before this block's pulses, for MIDI out's next 16th (MidiOut.h)
        next_16th_ = tempo_clock_.HasLoop() ? tempo_clock_.NextLoopPulseOn(chompi::kPulsesPer16th) : 0;
        block_pulses_ = 0;
        bool bar_line = false;
        for (uint32_t p = 0; p < pulses; p++)
        {
            const uint32_t pos = tempo_clock_.Pulse();
            fx_.ClockPulse(pos, tempo_clock_.Reverse());
            bar_line |= chompi::TempoClock::IsBarLine(pos);
            morph_.Pulse(chompi::TempoClock::IsBarLine(pos));
            if (block_pulses_ < kMaxBlockPulses)
                block_loop_pulse_[block_pulses_++] = tempo_clock_.LoopPulse();
        }
        morph_.Process(size, tempo_clock_.PulseSamples());
        if (__builtin_expect(loop_landing_, 0))
            LoopLanding(bar_line);
        if (morph_.Dropping())
            morph_.DropLead(tempo_clock_.PulsesToBarLine(), tempo_clock_.PulseSamples());
        // the keys, what the chaos key drops of them, and where it plays the loop
        float jump;
        if (fx_.Block(&jump))
            Scramble(jump);
        BENCH_MARK(TEMPO);

        // the source's gates, fading on a change; the dry branch's buffer only while it's heard
        float& in_fx = in_fx_[src];
        float& in_dry = in_dry_[src];
        const bool routed = !switching && in_fx == 1.f && in_dry == 0.f && to_fx == 1.f; // as ever
        const bool bypass = switching || in_dry != 0.f || to_dry != 0.f;
        if (switching)
            for (size_t i = 0; i < size; i++)
            {
                // the gates applied already (ProcessSwitch): the mix's input share on them
                MixStep();
                const float dry_amt = dry_amt_;
                const float wet_amt = wet_amt_;
                sigl[i] = sigl[i] * dry_amt + wetl[i] * wet_amt;
                sigr[i] = sigr[i] * dry_amt + wetr[i] * wet_amt;
                bypl[i] *= dry_amt;
                bypr[i] *= dry_amt;
            }
        else if (routed)
            for (size_t i = 0; i < size; i++)
            {
                MixStep();
                const float dry_amt = dry_amt_;
                const float wet_amt = wet_amt_;
                sigl[i] = dryl[i] * dry_amt + wetl[i] * wet_amt;
                sigr[i] = dryr[i] * dry_amt + wetr[i] * wet_amt;
            }
        else
            for (size_t i = 0; i < size; i++)
            {
                MixStep();
                const float dry_amt = dry_amt_;
                const float wet_amt = wet_amt_;
                in_fx = Toward(in_fx, to_fx);
                in_dry = Toward(in_dry, to_dry);
                const float fx_amt = dry_amt * in_fx;
                sigl[i] = dryl[i] * fx_amt + wetl[i] * wet_amt;
                sigr[i] = dryr[i] * fx_amt + wetr[i] * wet_amt;
                const float dry_branch = dry_amt * in_dry;
                bypl[i] = dryl[i] * dry_branch;
                bypr[i] = dryr[i] * dry_branch;
            }
        BENCH_MARK(OUTPUT);

        // punch-in FX, on the mix so they work on the input, the loop or both, and before the
        // output gain so they don't change with the VOLUME knob. The FX and the compressor a
        // block at a time, each effect's code in the I-cache for the whole block (#136)
        fx_.Process(sigl, sigr, size); // marks each effect itself
        comp_.Process(sigl, sigr, size);
        BENCH_MARK(COMP);

        // the dry branch joins after the compressor, before VOLUME and the limiter
        if (bypass)
            for (size_t i = 0; i < size; i++)
            {
                sigl[i] += bypl[i];
                sigr[i] += bypr[i];
            }

        for (size_t i = 0; i < size; i++)
        {
            fonepole(mgain_, mgain_target_, .001f);
            const float sl = sigl[i], sr = sigr[i];

            // headphone and master gain
            const float hpl = sl * kHpGain * mgain_;
            const float hpr = sr * kHpGain * mgain_;
            out[2][i] = sl * kLineOutGain * mgain_;
            out[3][i] = sr * kLineOutGain * mgain_;

            // the VU meter shows the master's signal (at the headphones' level, before the
            // limiter), whatever the headphones carry
            output_env_follower.Process(hpl + hpr);

            // headphone feed: the cue blends from the master's mirror to the input on its own,
            // slewed so a jump doesn't click; at rest on the mirror (as it usually is) no blend
            if (hp_cue_ != hp_cue_target_)
                hp_cue_ = chompi::Glide(hp_cue_, hp_cue_target_, .001f); // on 0 the blend stops
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

            // safety limiter: TAPE's master compressor at its lowest setting (limiter.h). The
            // headphones turned (#209): the analog stages turn AUX in and the line out once
            // each, the headphones' none, so turned here they come out in phase with AUX, as
            // the line out does (routing.cpp's TestPolarity)
            out[0][i] = -lim_hp_l_.ProcessComp(out[0][i], 1.f, kLimThresh, 1.f, kLimMakeup);
            out[1][i] = -lim_hp_r_.ProcessComp(out[1][i], 1.f, kLimThresh, 1.f, kLimMakeup);
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
    /** A source's input gain, 0..1: AUX's, or with mic the mic's */
    inline void SetInputGain(bool mic, float gain) { (mic ? mic_gain_target_ : aux_gain_target_) = gain; }
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
    /** The source: false = AUX, true = the built-in mic, faded over kMicFadeSamples */
    inline void SetMicInput(bool mic) { mic_in_ = mic; }
    inline bool MicInput() const { return mic_in_; }
    /** A source's effects (#177), AUX's (mic false) or the mic's: false = through them, true =
     *  dry, past them and the compressor */
    inline void SetDry(bool mic, bool dry) { dry_[mic] = dry; }
    inline bool Dry(bool mic) const { return dry_[mic]; }
    inline void SetAuxDry(bool dry) { SetDry(false, dry); }
    /** And whether it's live, on the outputs at all */
    inline void SetLive(bool mic, bool live) { live_[mic] = live; }
    inline bool Live(bool mic) const { return live_[mic]; }
    /** The input's meter on (VOLUME's page 2 shown) or off, which costs nothing */
    inline void MeterInput(bool on) { in_meter_ = on; }
    /** The input's peak, as the looper records it (full scale 1), since the last call; from
     *  MainLoop, a float read and written whole */
    inline float TakeInputPeak()
    {
        const float p = in_peak_;
        in_peak_ = 0.f;
        return p;
    }

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
    /** Makes the held morph a drop on its landing, gliding there first for a build-up
     *  (FxMorph::Drop); with the audio interrupt blocked */
    bool DropFxMorph(const float to[chompi::kNumFx][chompi::kNumFxParams], uint16_t ui_on,
                     uint16_t on, bool glide)
    {
        return morph_.Drop(to, ui_on, on, glide);
    }
    inline bool FxDropping() const { return morph_.Dropping(); }
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
    /** A loop from the card planned with the scene sequence (#70, LoopControls.h): the looper's
     *  held switch (Looper::Switch) goes where the morph lands from now on, or, not loaded by
     *  then, at the first bar line after that it is. Off again, nothing. With the audio
     *  interrupt blocked */
    void LoopOnLanding(bool on)
    {
        loop_landing_ = on;
        loop_landed_ = false;
        landings_ = morph_.Landings();
    }
    inline bool LoopOnLanding() const { return loop_landing_; }
    /** The morph landed since LoopOnLanding, the loop not switched yet */
    inline bool LoopLanded() const { return loop_landing_ && loop_landed_; }

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
    /** RESEED's (FxChain::Reseed), from MainLoop with the audio interrupt blocked */
    inline void Reseed(uint32_t seed) { fx_.Reseed(seed); }
    inline uint32_t RandomState(size_t i) { return fx_.RandomState(i); }
    /** Off and costing no more than off (FxChain::Resting): for the CPU bench */
    inline bool FxResting(size_t fx) const { return fx_.Resting(fx); }

    inline float GetVUSample() { return output_env_follower.GetLastSamp(); }

    /** A tapped tempo, from the UI (TapTempo.h): applied at the next block */
    inline void TapTempo(float bpm) { tap_bpm_.store(bpm); }
    /** Whether a tap would be taken: not without a loop while MIDI clock runs */
    inline bool CanTap() const { return tempo_clock_.CanTap(); }

    chompi::Looper looper;

private:
    /** The mic's sample: after its gain, DC blocked and filtered */
    __attribute__((always_inline)) inline float Mic(float in)
    {
        return mic_filter_.Process(dcblock_mic_in_.Process(in * mic_gain_ * kMicGain));
    }

    /** The input during a switch of the source (mic: towards the mic): the looper's crossfades
     *  from one source to the other, and each source goes through its own gates, fading
     *  towards to_fx and to_dry for the source heard and shut for the other. sig and byp get
     *  the input's share through the effects and past them, before the mix */
    FRIZZ_HOT void ProcessSwitch(const float *const *in, float* dryl, float* dryr, float* sigl,
                                 float* sigr, float* bypl, float* bypr, size_t size, bool mic,
                                 float to_fx, float to_dry)
    {
        const float aux_fx = mic ? 0.f : to_fx, aux_dry = mic ? 0.f : to_dry;
        const float mic_fx = mic ? to_fx : 0.f, mic_dry = mic ? to_dry : 0.f;
        for (size_t i = 0; i < size; i++)
        {
            fonepole(mic_gain_, mic_gain_target_, .001f);
            const float m = Mic(in[0][i]);
            fonepole(aux_gain_, aux_gain_target_, .001f);
            const float in_r = mono_in_ ? in[2][i] : in[3][i];
            const float l = dcblock_line_in_l_.Process(in[2][i] * aux_gain_ * kLineInGain);
            const float r = dcblock_line_in_r_.Process(in_r * aux_gain_ * kLineInGain);
            mic_fade_ = Toward(mic_fade_, mic ? 1.f : 0.f);
            dryl[i] = l + (m - l) * mic_fade_;
            dryr[i] = r + (m - r) * mic_fade_;
            in_fx_[0] = Toward(in_fx_[0], aux_fx);
            in_dry_[0] = Toward(in_dry_[0], aux_dry);
            in_fx_[1] = Toward(in_fx_[1], mic_fx);
            in_dry_[1] = Toward(in_dry_[1], mic_dry);
            sigl[i] = l * in_fx_[0] + m * in_fx_[1];
            sigr[i] = r * in_fx_[0] + m * in_fx_[1];
            bypl[i] = l * in_dry_[0] + m * in_dry_[1];
            bypr[i] = r * in_dry_[0] + m * in_dry_[1];
        }
    }

    /** The mix's sample: equal-power, so the middle of the knob doesn't dip in level; the cosf
     *  and sinf only while the knob slews, the mix usually sits still */
    __attribute__((always_inline)) inline void MixStep()
    {
        if (mix_ != mix_target_)
        {
            mix_ = chompi::Glide(mix_, mix_target_, .001f);
            dry_amt_ = cosf(mix_ * HALFPI_F);
            wet_amt_ = sinf(mix_ * HALFPI_F);
        }
    }

    // a switch between AUX and the mic crossfades over 5 ms, so neither clicks; so does one
    // of where the input goes (Input)
    static constexpr float kMicFadeSamples = 240.f;
    static constexpr float kMicFadeStep = 1.f / kMicFadeSamples;
    static inline float Toward(float v, float target)
    {
        return v < target ? fminf(v + kMicFadeStep, target) : fmaxf(v - kMicFadeStep, target);
    }

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

    /** The landing a loop waits for (LoopOnLanding): the switch released then, or at a bar
     *  line after it */
    __attribute__((noinline, cold)) void LoopLanding(bool bar_line)
    {
        const bool now = !loop_landed_ && morph_.Landings() != landings_;
        loop_landed_ = loop_landed_ || now;
        if (loop_landed_ && (now || bar_line) && looper.SwitchHeld())
        {
            looper.ReleaseSwitch();
            loop_landing_ = false;
        }
    }

    /** Hands a loop that just closed to the tempo clock, and takes an erased one away. A
     *  quantized loop knows its beats, as the clock was sent (the tempo clock applies the
     *  factor); an unquantized one fits the tempo set before it, or is guessed. A loop from
     *  the card (Looper::Switch) takes over the clock as it comes in, with its own beats or
     *  the power of two nearest the tempo (NearestBeats) */
    void SyncLoopTempo()
    {
        const chompi::Looper::State state = looper.GetState();
        const bool loop = state == chompi::Looper::State::PLAYING
                          || state == chompi::Looper::State::PAUSED;
        if (loop && (!tempo_clock_.HasLoop() || looper.Switches() != switches_))
        {
            const size_t length = looper.GetLength();
            uint32_t beats = looper.GetSenderBeats();
            if (looper.Switches() != switches_)
            {
                switches_ = looper.Switches();
                beats = looper.LoadedBeats();
                if (beats == 0)
                    beats = tempo_clock_.HasLoop() || tempo_clock_.TempoSet()
                                ? chompi::NearestBeats(length, sample_rate_, tempo_clock_.FitBpm())
                                : chompi::GuessBeats(length, sample_rate_);
            }
            else if (beats == 0)
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
    daisysp::DcBlock dcblock_line_in_l_, dcblock_line_in_r_, dcblock_mic_in_;
    chompi::MicFilter mic_filter_;
    chompi::Limiter lim_hp_l_, lim_hp_r_, lim_line_l_, lim_line_r_;
    volatile float lim_gain_ = 1.f; // TakeLimiterGain
    chompi::EnvFollower output_env_follower;
    chompi::TempoClock tempo_clock_;
    uint32_t rewinds_ = 0; // the looper's Rewinds() as last seen
    uint32_t switches_ = 0; // and its Switches()
    volatile bool loop_landing_ = false; // a loop waits for the morph's landing (LoopOnLanding)
    bool loop_landed_ = false;           // which has come
    uint32_t landings_ = 0;              // the morph's Landings() when it began to wait
    uint32_t block_pulses_ = 0; // the last block's pulses, up to kMaxBlockPulses
    uint32_t block_loop_pulse_[kMaxBlockPulses] = {};
    uint32_t next_16th_ = 0;
    chompi::FxChain fx_;
    chompi::FxMorph morph_;
    chompi::MasterComp comp_;
    volatile bool mono_in_ = false;
    volatile bool mic_in_ = false;
    // each source's settings, AUX's then the mic's: past the effects, live
    volatile bool dry_[2] = {false, false};
    volatile bool live_[2] = {true, false};
    float mic_fade_ = 0.f; // the looper's input: 0 AUX .. 1 the mic, towards mic_in_
    // each source's gates, AUX's then the mic's: its share through the effects, past them
    float in_fx_[2] = {1.f, 0.f}, in_dry_[2] = {0.f, 0.f};
    volatile bool in_meter_ = false;
    volatile float in_peak_ = 0.f; // TakeInputPeak
    // live values start at 0 and slew up to the targets the play page sets at boot
    float mgain_ = 0.f, mgain_target_ = 0.f;
    float aux_gain_ = 0.f, aux_gain_target_ = 0.f;
    float mic_gain_ = 0.f, mic_gain_target_ = 0.f;
    float mix_ = 0.f, mix_target_ = 0.f;
    float dry_amt_ = 1.f, wet_amt_ = 0.f; // the crossfade at mix_
    float hp_cue_ = 0.f, hp_cue_target_ = 0.f;
};
