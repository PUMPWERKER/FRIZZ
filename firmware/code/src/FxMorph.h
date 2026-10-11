/** @file FxMorph.h
 *  @brief A scene morph (SHIFT + scene key): the FX glide from where they are to a scene and
 *  land on a bar line of the FX's clock (TempoClock.h), the first one ahead, or one more per
 *  AddBar. FxControls::Morph works out what each parameter and key does (FxMorphPlan); this
 *  runs it in the audio callback, between the UI and the FxChain, so the landing is exact to
 *  the clock pulse.
 *
 *  Held (SHIFT still down after the tap, Start's hold): nothing moves and the bar lines don't
 *  count, however long it's held; taps add bars. Release glides from where the sound still is
 *  to the next bar line from there, plus the bars tapped.
 *
 *  The glide follows the clock: the pulses counted since the start, plus the time since the
 *  last one, over the pulses expected to the landing. The landing itself is the bar line's
 *  pulse, wherever the transport went, so a wrong estimate only bends the glide. Glided
 *  values are sent once per block and slewed at the fast slew (FxChain::FastSlew), so the
 *  sound follows the glide closely instead of trailing it by the knobs' 100ms.
 *
 *  While it runs it owns every parameter: the UI's SetParam moves the parameter's
 *  destination, not the sound. An FX whose key it switches at the landing (or when its fade-in
 *  starts) is deferred: the UI's SetOn only says what it will be.
 *
 *  The crossfader (#66): Fader takes the glide out of the clock's hands into the UI's, 0 the
 *  start (A) and 1 the target (B), back and forth, and the bar lines no longer count. What
 *  glides follows the fader; a fade-in's key is on anywhere past A, a fade-out's until B; the
 *  rest switches in the middle, both ways: the stepped values of an FX on in both, and the
 *  keys of the FX without fade knobs. It ends as a morph does: Land (B), Freeze (where it
 *  is), or Freeze at A, which the UI takes back to where it was (FxControls::EndFade).
 */
#pragma once
#include "FxChain.h"
#include "FxParams.h"

namespace chompi
{

// What runs once per morph, not per block (Start, AddBar, Land, Freeze): built for size, as
// FxControls' FX_SCENE_ONCE. At -O3 their copies of the plan were unrolled into kilobytes
#define FX_MORPH_ONCE __attribute__((noinline, optimize("Os")))

// Taps of SHIFT + the scene key: the bar lines a morph may run to
static const uint32_t kMaxMorphBars = 8;
// A fade-in's key comes on this long after the start, once its fade knobs have slewed
// to their defaults: 5 of the fast slew's time constants (FxCommon.h)
static const uint32_t kMorphWakeSamples = 1200;

/** What a parameter does during a morph */
enum class MorphParam : uint8_t
{
    HOLD,     // stays at start, jumps to the target at the landing
    GLIDE,    // start to target
    FADE_OUT, // start to its default (silent), then the target at the landing
};

/** A morph, from FxControls::Morph */
struct FxMorphPlan
{
    float start[kNumFx][kNumFxParams];
    float target[kNumFx][kNumFxParams];
    MorphParam how[kNumFx][kNumFxParams];
    uint16_t deferred; // bit fx: its key is switched by the morph: at the landing, or at...
    uint16_t wake;     // ... kMorphWakeSamples for these (fade-ins)
    uint16_t was_on;   // bit fx: on at the start, what a deferred key is until it's switched
    uint16_t park;     // bit fx: if it ends off, its parameters stay where the glide left them
                       // (faded out) instead of jumping to the target at the landing
};

/** Chain is FxChain; a template only so test/tempo.cpp can watch what it's sent */
template <class Chain = FxChain>
class FxMorphT
{
public:
    void Init(Chain* chain)
    {
        chain_ = chain;
        plan_ = FxMorphPlan();
        active_ = false;
        land_ = false;
    }

    /** Starts plan, landing on the next bar line, pulses_to_bar pulses away (an estimate);
     *  held: waiting at the start until Release. The chain must have plan's start values
     *  already. Call with the audio interrupt blocked */
    FX_MORPH_ONCE void Start(const FxMorphPlan& plan, uint32_t pulses_to_bar, bool hold = false)
    {
        if (active_)
            Land();
        plan_ = plan;
        chain_->FastSlew();
        for (size_t fx = 0; fx < kNumFx; fx++)
            for (size_t p = 0; p < kNumFxParams; p++)
                live_[fx][p] = plan_.start[fx][p];
        deferred_ = plan_.deferred;
        pending_on_ = 0;
        bars_left_ = 1;
        expected_ = static_cast<float>(pulses_to_bar);
        base_ = 0.f;
        pos_ = 0.f;
        pulses_ = 0;
        since_pulse_ = 0.f;
        since_start_ = 0;
        land_ = false;
        holding_ = hold;
        manual_ = false;
        fader_ = 0.f;
        t0_ = t_ = 0.f;
        active_ = true;
    }

    /** SHIFT let go: a held morph glides from here to the bar line pulses_to_bar pulses
     *  away (an estimate), plus a bar line pulses_per_bar apart for each extra tap. Call
     *  with the audio interrupt blocked */
    void Release(uint32_t pulses_to_bar, uint32_t pulses_per_bar)
    {
        if (!active_ || !holding_ || manual_)
            return;
        holding_ = false;
        base_ = pos_;
        expected_ = pos_ + static_cast<float>(pulses_to_bar)
                    + static_cast<float>((bars_left_ - 1) * pulses_per_bar);
    }
    inline bool Holding() const { return active_ && holding_; }

    /** One bar line more, up to kMaxMorphBars. The glide carries on from where it is, now
     *  over pulses_per_bar more pulses. False if it can't. Call with the audio interrupt
     *  blocked */
    FX_MORPH_ONCE bool AddBar(uint32_t pulses_per_bar)
    {
        if (!active_ || land_ || manual_ || bars_left_ >= kMaxMorphBars)
            return false;
        bars_left_++;
        // from here, over what was left plus a bar. The plan keeps its start, which the
        // crossfader's 0 is (and the UI's start, FxControls::EndFade): the glide goes on from
        // where it got to, t0_ of the way
        t0_ = t_;
        expected_ += static_cast<float>(pulses_per_bar);
        base_ = pos_;
        return true;
    }

    /** Each clock pulse, with whether it's on a bar line (TempoClock::IsBarLine) */
    void Pulse(bool bar_line)
    {
        if (!active_)
            return;
        pulses_++;
        since_pulse_ = 0.f;
        if (bar_line && !holding_ && !manual_ && --bars_left_ == 0)
            land_ = true;
    }

    /** The crossfader: the glide at t, 0 the start to 1 the target, from now on in the UI's
     *  hands, held or gliding. False if none runs. Call with the audio interrupt blocked */
    bool Fader(float t)
    {
        if (!active_ || land_)
            return false;
        if (!manual_)
        {
            // every switched key goes through the fader from here, the ones a fade-in
            // switched already as they are now
            sent_on_ = (plan_.was_on & deferred_) | (pending_on_ & plan_.deferred & ~deferred_);
            deferred_ = plan_.deferred;
            holding_ = false;
            manual_ = true;
        }
        fader_ = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
        return true;
    }
    /** Whether the crossfader has it, and where */
    inline bool Manual() const { return active_ && manual_; }
    inline float FaderPos() const { return fader_; }

    /** Once per block, after the block's pulses: the glide, or the landing */
    void Process(size_t size, float pulse_samples)
    {
        if (!active_)
            return;
        if (land_)
        {
            Land();
            return;
        }
        if (manual_)
        {
            ProcessFader();
            return;
        }

        since_start_ += size;
        // a held morph wakes its fade-ins on the release, when their glide starts
        if (deferred_ & plan_.wake && !holding_ && since_start_ >= kMorphWakeSamples)
        {
            for (size_t fx = 0; fx < kNumFx; fx++)
            {
                const uint16_t bit = static_cast<uint16_t>(1u << fx);
                if (deferred_ & plan_.wake & bit)
                {
                    deferred_ &= static_cast<uint16_t>(~bit);
                    chain_->SetOn(fx, pending_on_ & bit);
                }
            }
        }

        // where between two pulses: never a whole one, the next pulse counts that
        since_pulse_ += static_cast<float>(size);
        float frac = pulse_samples > 0.f ? since_pulse_ / pulse_samples : 0.f;
        if (frac > .99f)
            frac = .99f;
        pos_ = static_cast<float>(pulses_) + frac;
        const float span = expected_ - base_;
        float t = span > 0.f ? (pos_ - base_) / span : 1.f;
        t = holding_ || t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
        // the rest of the way from where the last AddBar left it
        t = t0_ + (1.f - t0_) * t;
        t_ = t;

        chain_->FastSlew();
        for (size_t fx = 0; fx < kNumFx; fx++)
        {
            for (size_t p = 0; p < kNumFxParams; p++)
            {
                const MorphParam how = plan_.how[fx][p];
                if (how == MorphParam::HOLD)
                    continue;
                const float start = plan_.start[fx][p];
                const float val = start + (End(fx, p, how) - start) * t;
                if (val != live_[fx][p])
                {
                    live_[fx][p] = val;
                    chain_->SetParam(fx, p, val);
                }
            }
        }
    }

    /** Ends it now: every parameter on its target, the deferred keys switched; a parked FX
     *  that goes off stays faded out. Only what changes is sent, so an FX the scenes share
     *  runs on untouched. Call with the audio interrupt blocked, or from the audio callback */
    FX_MORPH_ONCE void Land()
    {
        if (!active_)
            return;
        chain_->FastSlew();
        for (size_t fx = 0; fx < kNumFx; fx++)
        {
            const uint16_t bit = static_cast<uint16_t>(1u << fx);
            const bool on = deferred_ & bit ? pending_on_ & bit : true;
            if (on || !(plan_.park & bit))
            {
                for (size_t p = 0; p < kNumFxParams; p++)
                {
                    if (plan_.target[fx][p] != live_[fx][p])
                        chain_->SetParam(fx, p, plan_.target[fx][p]);
                }
            }
            if (deferred_ & bit)
                chain_->SetOn(fx, on);
        }
        deferred_ = 0;
        land_ = false;
        active_ = false;
    }

    /** Stops it where it is (SHIFT + PLAY): the parameters stay where the glide got to, the
     *  keys not yet switched stay as they were. Fills params with where that is, unswitched
     *  with those keys, and was_on with which of them are on. False if it doesn't run. Call
     *  with the audio interrupt blocked */
    FX_MORPH_ONCE bool Freeze(float params[kNumFx][kNumFxParams], uint16_t* unswitched,
                              uint16_t* was_on)
    {
        if (!active_)
            return false;
        for (size_t fx = 0; fx < kNumFx; fx++)
            for (size_t p = 0; p < kNumFxParams; p++)
                params[fx][p] = live_[fx][p];
        // on the fader, the keys still as they were at the start
        const uint16_t unswitched_now = manual_ ? deferred_ & ~(sent_on_ ^ plan_.was_on) : deferred_;
        *unswitched = unswitched_now;
        *was_on = unswitched_now & plan_.was_on;
        deferred_ = 0;
        land_ = false;
        active_ = false;
        return true;
    }

    /** From the UI: a parameter's new destination while it runs. False if it doesn't run,
     *  so the value goes to the chain */
    bool SetParam(size_t fx, size_t param, float val)
    {
        if (!active_)
            return false;
        plan_.target[fx][param] = val;
        return active_; // landed meanwhile: then it's for the chain
    }

    /** From the UI: a key while it runs. False if the morph doesn't switch this one, so it
     *  goes to the chain */
    bool SetOn(size_t fx, bool on)
    {
        const uint16_t bit = static_cast<uint16_t>(1u << fx);
        if (!active_ || !(plan_.deferred & bit))
            return false;
        // also once a fade-in has switched it, for the crossfader to take up (Fader)
        if (on)
            pending_on_ |= bit;
        else
            pending_on_ &= static_cast<uint16_t>(~bit);
        return active_ && (deferred_ & bit); // switched meanwhile: then it's for the chain
    }

    inline bool Active() const { return active_; }

private:
    /** Where a gliding parameter glides to: a fade-out to its default, the rest to the target */
    inline float End(size_t fx, size_t p, MorphParam how) const
    {
        return how == MorphParam::FADE_OUT ? kFxParams[fx].defaults[p] : plan_.target[fx][p];
    }

    /** A block on the crossfader: see the file comment. Only what changes is sent, at the
     *  knobs' slew, as if they were turned */
    __attribute__((noinline, optimize("Os"))) void ProcessFader()
    {
        const float t = fader_;
        for (size_t fx = 0; fx < kNumFx; fx++)
        {
            const uint16_t bit = static_cast<uint16_t>(1u << fx);
            if (plan_.deferred & bit)
            {
                const bool past = plan_.wake & bit ? t > 0.f
                                                   : (plan_.park & bit ? t >= 1.f : t >= .5f);
                const bool on = (past ? pending_on_ : plan_.was_on) & bit;
                if (on != ((sent_on_ & bit) != 0))
                {
                    sent_on_ ^= bit;
                    chain_->SetOn(fx, on);
                }
            }
            for (size_t p = 0; p < kNumFxParams; p++)
            {
                const MorphParam how = plan_.how[fx][p];
                const float start = plan_.start[fx][p];
                float val;
                if (how == MorphParam::HOLD)
                {
                    // one going off keeps its own until the end (Land)
                    if (plan_.park & bit)
                        continue;
                    val = t >= .5f ? plan_.target[fx][p] : start;
                }
                else
                {
                    val = start + (End(fx, p, how) - start) * t;
                }
                if (val != live_[fx][p])
                {
                    live_[fx][p] = val;
                    chain_->SetParam(fx, p, val);
                }
            }
        }
    }

    Chain* chain_ = nullptr;
    FxMorphPlan plan_;
    float live_[kNumFx][kNumFxParams]; // what was last sent
    volatile uint16_t deferred_;       // the keys still to be switched
    volatile uint16_t pending_on_;     // what they'll be
    uint32_t bars_left_;
    float expected_;    // pulses from the start to the landing, estimated
    float base_;        // where the glide carried on from after the last AddBar
    float pos_;         // pulses since the start, with the fraction of the next
    float t_ = 0.f;     // how far the glide has got, 0 the start to 1 the target
    float t0_ = 0.f;    // and had got at the last AddBar
    uint32_t pulses_;
    float since_pulse_; // samples
    uint32_t since_start_;
    volatile bool land_;
    volatile bool active_;
    volatile bool holding_ = false; // started held, not released yet
    volatile bool manual_ = false;  // on the crossfader (Fader)
    volatile float fader_ = 0.f;    // its position, 0 the start to 1 the target
    uint16_t sent_on_ = 0;          // on the crossfader: the switched keys as sent
};
using FxMorph = FxMorphT<>;

} // namespace chompi
