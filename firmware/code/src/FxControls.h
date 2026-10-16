/** @file FxControls.h
 *  @brief What the play page does with the punch-in FX keys and knobs 1-4, without the
 *  hardware: the parameters, which keys are held and latched, which FX the knobs edit and on
 *  which page, fine, stepped and coarse turns, and taking, recalling or morphing to a scene. NormalPage.h routes the keys and
 *  knobs here and draws the LEDs from it; test/controls.cpp runs it on the host.
 *
 *  The knobs have two pages, switched together: a plain press on any of them turns all four
 *  over to the selected FX's page 2 (its parameters 4-7, FxParams.h) and back; on an FX with
 *  nothing on page 2 it does nothing. Selecting
 *  another FX, or the finalizer, goes back to page 1, so the main controls are under the
 *  fingers whenever an effect is picked. SHIFT + turn and SHIFT + press act on the page shown.
 *
 *  The finalizer (Finalizer.h, #345) is edited here too: its key selects it for the knobs
 *  (kFinSelected), which then turn as they do for an FX, on the module shown (Module: Level,
 *  EQ, Comp or Width, kFinalizerKnobs). It has no page 2: a plain press of knob 1-3 mutes or
 *  unmutes a band instead, in the three band modules (#347). It's always there and not part
 *  of a scene, so its knobs leave the scene unedited; its bypass and mutes are live, not
 *  kept.
 *
 *  An effect with a second mode (kFxModes: the phaser's frequency shifter, #306) has it as a
 *  setting of its own beside its knobs (SetMode), which a scene keeps, and a recall or morph
 *  switches as a stepped parameter.
 *
 *  Engine is PassthroughEngine on the device, a fake on the host. It needs SetFxOn(fx, on),
 *  SetFxParam(fx, param, val), SetFxMode(fx, second), SetFinParam(param, val),
 *  SetFinBypass(on), SetFinMute(band, on), FastFxSlew() and the morph's StartFxMorph(plan),
 *  AddFxMorphBar(), LandFxMorph(), FreezeFxMorph(params, unswitched, was_on, modes),
 *  FxMorphing(), the crossfader's FxMorphHeld() and FadeFxMorph(t), a drop's
 *  DropFxMorph(to, ui_on, on, glide, modes) and FxDropping(), and
 *  SetFxPool(pool) for the chaos key (Pool).
 */
#pragma once
#include <math.h>
#include "FxMorph.h"
#include "FxParams.h"
#include "FxScenes.h"

// a scene's work, a press now and then: kept out of line and small, since FRIZZ's code
// space is tight (SRAM_EXEC) and two pages of parameters made it twice as long
#define FX_SCENE_ONCE __attribute__((noinline, optimize("Os")))

// the play page's logic, a key or a frame now and then and never per sample: built for size,
// as FRIZZ's code space is tight (SRAM_EXEC, docs/CAPACITY.md)
#pragma GCC push_options
#pragma GCC optimize("Os")

namespace chompi
{

// Knobs: 1% per detent; a stepped parameter moves one step per kFxDetentsPerStep detents
static const float kFxParamStep = .01f;
static const float kFxDetentsPerStep = 3.f;
// Turned fast, a plain turn's 1% grows (#375): 1x at kFxAccelSlowMs between detents (8 a
// second) or slower, up to kFxAccelMax times at kFxAccelFastMs (40 a second) or faster.
// Tuned by these
static const float kFxAccelSlowMs = 125.f;
static const float kFxAccelFastMs = 25.f;
static const float kFxAccelMax = 5.f;

/** A turn's factor on its step for a detent gap_ms after the knob's last one the same way
 *  (ui.h measures it; the first, or one turned back, counts as slow): 1 at slow_ms or slower,
 *  most at fast_ms or faster, linear in detents a second between, so it has no jump */
inline float TurnAccel(uint16_t gap_ms, float slow_ms, float fast_ms, float most)
{
    if (gap_ms >= slow_ms)
        return 1.f;
    if (gap_ms <= fast_ms)
        return most;
    const float rate = 1000.f / gap_ms, slow = 1000.f / slow_ms, fast = 1000.f / fast_ms;
    return 1.f + (most - 1.f) * (rate - slow) / (fast - slow);
}
/** Knob 1-4's plain turn's */
inline float FxKnobAccel(uint16_t gap_ms)
{
    return TurnAccel(gap_ms, kFxAccelSlowMs, kFxAccelFastMs, kFxAccelMax);
}
// Selected() while the knobs edit the finalizer
static const size_t kFinSelected = kNumFx;
// its modules (Finalizer::Module) and bands
static const size_t kFinLevel = 0, kFinBands = 3;

template <class Engine>
class FxControls
{
public:
    /** Every FX off, every parameter on its default, pushed to the engine */
    void Init(Engine* engine)
    {
        engine_ = engine;
        for (size_t fx = 0; fx < kNumFx; fx++)
        {
            held_[fx] = latched_[fx] = false;
            on_release_[fx] = OnRelease::KEEP;
            for (size_t p = 0; p < kNumFxParams; p++)
            {
                params_[fx][p] = -1.f; // so every default counts as a change and is sent
                SetParam(fx, p, kFxParams[fx].defaults[p]);
            }
            engine_->SetFxOn(fx, false);
        }
        modes_ = 0; // as the engine starts
        for (size_t p = 0; p < kNumFinParams; p++)
        {
            // an unused one stays on its default; a used one counts as changed and is sent
            const float def = FinDefault(p);
            fin_[p] = FinUsed(p) ? -1.f : def;
            SetFin(p, def);
        }
        fin_changed_ = 0;
        module_ = 0;
        mutes_ = 0;
        bypass_ = false;
        rebound_ = 0;
        ClearChunks();
        chunk_shift_ = false;
        selected_ = 0;
        page_ = 0;
        edited_ = false;
        stale_ = morph_touched_ = 0;
    }

    /** An FX key going down or up. Down selects it for the knobs. The effect is on for as long
     *  as the key is held; the latch is decided on the release, which is inaudible: SHIFT
     *  going down during the hold toggles it (ShiftPressed), a plain press clears it. With
     *  SHIFT already down, the press only selects it: it stays off, its latch stays, and the
     *  release does nothing, also once SHIFT is let go first. A release without its press
     *  (held through boot) does nothing */
    void KeyPressed(size_t fx, bool down, bool shift)
    {
        if (down)
        {
            // detents towards a step belong to the FX they were turned on; another FX shows
            // its page 1
            if (fx != selected_)
            {
                ClearChunks();
                page_ = 0;
            }
            selected_ = fx;
            if (shift)
            {
                ShiftUsed(); // a select: no latch for the keys held
                return;
            }
            on_release_[fx] = OnRelease::CLEAR;
            rebound_ &= static_cast<uint16_t>(~Bit(fx));
        }
        else
        {
            // what the release is about to change, for a bounce to take back (Rebound)
            rebound_ |= Bit(fx);
            was_held_ = static_cast<uint16_t>(held_[fx] ? was_held_ | Bit(fx) : was_held_ & ~Bit(fx));
            was_latched_ = static_cast<uint16_t>(latched_[fx] ? was_latched_ | Bit(fx)
                                                              : was_latched_ & ~Bit(fx));
            was_on_release_[fx] = on_release_[fx];
            if (!held_[fx])
                return;
            bool latched = latched_[fx];
            if (on_release_[fx] == OnRelease::CLEAR)
                latched = false;
            else if (on_release_[fx] == OnRelease::TOGGLE)
                latched = !latched;
            if (latched != latched_[fx])
            {
                edited_ = true;
                TouchedInMorph(fx);
            }
            latched_[fx] = latched;
            on_release_[fx] = OnRelease::KEEP;
        }
        held_[fx] = down;
        SendOn(fx);
    }

    /** SHIFT went down just after fx's plain press (NormalPage's grace window, #115): the
     *  press was SHIFT + key after all, a select. The effect goes off again unless latched, its
     *  latch stays, and the release does nothing. False if the press wasn't a plain one */
    bool LateSelect(size_t fx)
    {
        if (!held_[fx] || on_release_[fx] != OnRelease::CLEAR)
            return false;
        held_[fx] = false;
        on_release_[fx] = OnRelease::KEEP;
        SendOn(fx);
        return true;
    }

    /** fx's key back down just after its release (a bounce, NormalPage's grace window): the
     *  release is taken back, the key held, latched and pending as before it, so the next
     *  release decides as that one did. False if the key wasn't just let go */
    bool Rebound(size_t fx)
    {
        if (!((rebound_ >> fx) & 1))
            return false;
        rebound_ &= static_cast<uint16_t>(~Bit(fx));
        held_[fx] = (was_held_ >> fx) & 1;
        latched_[fx] = (was_latched_ >> fx) & 1;
        on_release_[fx] = was_on_release_[fx];
        SendOn(fx);
        return true;
    }

    /** The finalizer's key going down: selects it for the knobs, as an FX key does. With
     *  SHIFT, a select as an FX key's would be */
    void FinKeyPressed(bool shift)
    {
        Select(kFinSelected);
        if (shift)
            ShiftUsed();
    }

    /** fx, or kFinSelected, selected for the knobs without its key (MIDI's focus), on page 1
     *  unless it already was */
    void Select(size_t fx)
    {
        if (selected_ != fx)
        {
            ClearChunks();
            page_ = 0;
        }
        selected_ = fx;
    }

    /** SHIFT going down: every FX key held will toggle its latch on release. True if one
     *  will, so SHIFT is a latch combo. A second SHIFT press in the same hold is still that
     *  latch (it toggles once), so its release doesn't confirm a scene either. A key held
     *  from a select (KEEP) has no latch to toggle */
    bool ShiftPressed()
    {
        bool latch = false;
        for (size_t fx = 0; fx < kNumFx; fx++)
        {
            if (held_[fx] && on_release_[fx] != OnRelease::KEEP)
            {
                on_release_[fx] = OnRelease::TOGGLE;
                latch = true;
            }
        }
        return latch;
    }

    /** SHIFT was used for something else while FX keys were held (a coarse turn, tap tempo, a
     *  scene, a select...), or the CHOMPI key turned out to be a confirm: the latches go back
     *  to what the presses alone would do */
    void ShiftUsed()
    {
        for (size_t fx = 0; fx < kNumFx; fx++)
        {
            if (held_[fx] && on_release_[fx] == OnRelease::TOGGLE)
                on_release_[fx] = OnRelease::CLEAR;
        }
    }

    /** Knob 0-3 turned by detents (signed). Plain: 1% per detent, times accel turned fast
     *  (FxKnobAccel), or a step per kFxDetentsPerStep for a stepped parameter, a split one
     *  (FxParams::split) either by which half it's in. SHIFT: one point of the coarse grid per
     *  detent. A knob the page doesn't use does nothing */
    void KnobTurned(size_t knob, float detents, bool shift, float accel = 1.f)
    {
        const FxParams& fxp = Knobs();
        if (!KnobUsed(knob))
            return;
        const size_t param = ParamOf(knob);
        if (shift)
            ShiftUsed();

        const float val = Knob(knob);

        // fine and coarse turns count their detents separately
        if (shift != chunk_shift_)
        {
            ClearChunks();
            chunk_shift_ = shift;
        }

        if (shift)
        {
            // coarse: one grid point per detent
            chunk_[knob] += detents;
            while (chunk_[knob] >= 1.f || chunk_[knob] <= -1.f)
            {
                const float dir = chunk_[knob] > 0.f ? 1.f : -1.f;
                SetKnob(knob, CoarseStep(kFxGrids[fxp.coarse[param]], Knob(knob), dir));
                chunk_[knob] -= dir;
            }
            return;
        }

        const uint8_t steps = fxp.steps[param];
        const bool split = (fxp.split >> param) & 1;
        if (steps == 0 || (split && !SplitUpper(val)))
        {
            // a split knob's lower half stops on the upper's first step, reached from a hair
            // below it too: 1% steps add up to a little under it
            const float to = val + detents * accel * kFxParamStep;
            SetKnob(knob, split && SplitUpper(to + .5f * kFxParamStep) ? kSplitKnob : to);
            return;
        }

        // stepped: every kFxDetentsPerStep detents moves one step
        chunk_[knob] += detents;
        if (chunk_[knob] >= kFxDetentsPerStep || chunk_[knob] <= -kFxDetentsPerStep)
        {
            const float dir = chunk_[knob] > 0.f ? 1.f : -1.f;
            chunk_[knob] = 0.f;
            if (split)
            {
                // below the first step: the top of the lower half
                const float idx = static_cast<float>(SplitStep(val, steps)) + dir;
                SetKnob(knob, idx < 0.f ? kSplitKnob - kFxParamStep : SplitStepValue(idx, steps));
                return;
            }
            const float step = 1.f / (steps - 1);
            // snap to the step grid, so values set elsewhere can't drift off it
            const float idx = roundf(val / step) + dir;
            SetKnob(knob, idx * step);
        }
    }

    /** Knob 0-3 pressed: with SHIFT, resets that parameter to its default, on a knob the page
     *  uses. A plain press turns the page, all four knobs together, for an FX with a page 2;
     *  on the finalizer it mutes a band (SetMute). True if the press did something */
    bool KnobPressed(size_t knob, bool shift)
    {
        if (!shift && selected_ == kFinSelected)
        {
            // the finalizer: knob 1-3 mutes its band in the band modules (#347)
            if (module_ == kFinLevel || knob >= kFinBands)
                return false;
            SetMute(knob, !Muted(knob));
            return true;
        }
        if (!shift)
        {
            if (!HasPage2())
                return false;
            page_ ^= 1;
            ClearChunks();
            return true;
        }
        if (!KnobUsed(knob))
            return false;
        ShiftUsed();

        SetKnob(knob, Knobs().defaults[ParamOf(knob)]);
        chunk_[knob] = 0.f;
        return true;
    }

    /** A parameter set outright (MIDI, MidiControl.h), as a knob turned there would: a stepped
     *  one on its nearest step. Doesn't change which FX the knobs edit */
    void SetParamTo(size_t fx, size_t param, float val)
    {
        const FxParams& fxp = kFxParams[fx];
        if (param >= kNumFxParams || !((fxp.knobs >> param) & 1))
            return;
        if ((fxp.split >> param) & 1)
        {
            if (SplitUpper(val))
                val = SplitStepValue(static_cast<float>(SplitStep(val, fxp.steps[param])),
                                     fxp.steps[param]);
        }
        else if (fxp.steps[param])
        {
            const float step = 1.f / (fxp.steps[param] - 1);
            val = roundf(fclamp(val, 0.f, 1.f) / step) * step;
        }
        SetParam(fx, param, val);
    }

    /** fx in its second mode (kFxModes) or not, from SHIFT + its key twice or MIDI: the knobs
     *  stay where they are, read the other way. An effect without one stays as it is */
    void SetMode(size_t fx, bool second)
    {
        if (!(kFxModes >> fx & 1) || Mode(fx) == second)
            return;
        edited_ = true;
        modes_ ^= Bit(fx);
        engine_->SetFxMode(fx, second);
    }
    inline bool Mode(size_t fx) const { return modes_ >> fx & 1; }

    /** A latch set outright (MIDI): as the key's latch toggled, without the key. A key held
     *  meanwhile keeps the effect on, and its release leaves the latch as set */
    void SetLatch(size_t fx, bool on)
    {
        if (latched_[fx] != on)
        {
            edited_ = true;
            TouchedInMorph(fx);
        }
        latched_[fx] = on;
        if (held_[fx])
            on_release_[fx] = OnRelease::KEEP;
        SendOn(fx);
    }

    /** The parameters and latches into scene, which is then what the controls match */
    FX_SCENE_ONCE void Snapshot(FxScene& scene)
    {
        Capture(scene);
        edited_ = false;
    }

    /** Recalls scene at the fast slew. Only what changes is sent, so an effect the scenes
     *  share runs on untouched; the latches become the scene's and keys held stay on. A send
     *  the scene leaves off keeps ringing out as it was (kFxSends). On the device, call it with
     *  the audio interrupt blocked, so it lands within one block */
    FX_SCENE_ONCE void Recall(const FxScene& scene)
    {
        SceneDecides();
        engine_->LandFxMorph();
        engine_->FastFxSlew();
        SendModes(scene.modes);
        modes_ = scene.modes;
        for (size_t fx = 0; fx < kNumFx; fx++)
        {
            latched_[fx] = (scene.latched >> fx) & 1;
            const bool spare = !IsOn(fx) && (kFxSends >> fx & 1);
            for (size_t p = 0; p < kNumFxParams; p++)
            {
                if (scene.params[fx][p] == params_[fx][p])
                    continue;
                if (spare)
                {
                    params_[fx][p] = scene.params[fx][p];
                    stale_ |= Bit(fx);
                }
                else
                    SetParam(fx, p, scene.params[fx][p]);
            }
            SendOn(fx);
        }
        ClearChunks();
        edited_ = false;
    }

    /** Glides to scene, landing on the FX clock's next bar line (FxMorph.h). The controls
     *  show the scene at once: knobs turned and keys pressed meanwhile change where it lands.
     *  Per FX, from what it is now to what the scene makes it (held keys stay on):
     *   - off in both: the parameters jump now, unheard, except a send's, which rings out as
     *     it was (kFxSends);
     *   - on in both: continuous parameters glide, stepped ones switch at the landing;
     *   - one it turns on: its fade knobs (FxParams::fade), or the shifter's and slicer's Mix
     *     (MorphFades), start neutral (dry) and glide to the scene, the rest jump now; the key
     *     comes on as soon as they've got there;
     *   - one it turns off: its fade knobs, or the freezer's, shifter's and slicer's Mix, glide
     *     to neutral and the key goes off at the landing; the engine keeps it there, silent,
     *     until its key next comes on;
     *   - an FX that doesn't fade (or the freezer coming on): all at the landing.
     *  Only what changes is sent, as in a recall
     *  On the device, call it with the audio interrupt blocked */
    FX_SCENE_ONCE void Morph(const FxScene& scene)
    {
        SceneDecides();
        engine_->LandFxMorph();
        // where it starts, for the crossfader to go back to (EndFade)
        Capture(start_);
        start_edited_ = edited_;
        fading_ = false;
        FxMorphPlan plan;
        plan.deferred = plan.wake = plan.was_on = plan.park = 0;
        // a mode switches at once where its effect is off now, at the landing where it's on
        uint16_t now_on = 0;
        for (size_t fx = 0; fx < kNumFx; fx++)
            now_on |= IsOn(fx) ? Bit(fx) : 0;
        plan.mode_start = static_cast<uint16_t>((scene.modes & ~now_on) | (modes_ & now_on));
        SendModes(plan.mode_start);
        plan.mode_target = modes_ = scene.modes;
        for (size_t fx = 0; fx < kNumFx; fx++)
        {
            const FxParams& fxp = kFxParams[fx];
            const bool was = IsOn(fx);
            const bool will = held_[fx] || ((scene.latched >> fx) & 1);
            const uint16_t bit = Bit(fx);
            const uint8_t fades_in = MorphFades(fx, true), fades_out = MorphFades(fx, false);
            if (was)
                plan.was_on |= bit;
            if (was != will)
            {
                plan.deferred |= bit;
                if (will && fades_in)
                    plan.wake |= bit;
            }
            if (was && !will)
            {
                // parked at the landing: the engine lags the controls until it's on again
                plan.park |= bit;
                stale_ |= bit;
            }
            if (!was && !will && (kFxSends & bit))
            {
                // a send ringing out: untouched, it takes the scene when it next comes on
                for (size_t p = 0; p < kNumFxParams; p++)
                {
                    plan.start[fx][p] = plan.target[fx][p] = scene.params[fx][p];
                    plan.how[fx][p] = MorphParam::HOLD;
                    if (scene.params[fx][p] != params_[fx][p])
                        stale_ |= bit;
                }
                continue;
            }
            const bool stale = Stale(fx) && !was;
            for (size_t p = 0; p < kNumFxParams; p++)
            {
                const float to = scene.params[fx][p];
                float from = params_[fx][p];
                MorphParam how = MorphParam::HOLD;
                if (!was && !will)
                    from = to;
                else if (was && will)
                    // a split knob glides only within its lower half
                    how = fxp.steps[p] == 0
                                  || ((fxp.split >> p & 1) && !SplitUpper(from) && !SplitUpper(to))
                              ? MorphParam::GLIDE
                              : MorphParam::HOLD;
                else if (will)
                {
                    if (fades_in)
                    {
                        const bool fade = (fades_in >> p) & 1;
                        from = fade ? MorphNeutral(fx, p) : to;
                        how = fade ? MorphParam::GLIDE : MorphParam::HOLD;
                    }
                }
                else if ((fades_out >> p) & 1)
                    how = MorphParam::FADE_OUT;
                plan.start[fx][p] = from;
                plan.target[fx][p] = to;
                plan.how[fx][p] = how;
                // the morph starts from what the engine has
                if (stale || from != params_[fx][p])
                    engine_->SetFxParam(fx, p, from);
            }
            if (stale)
                stale_ &= static_cast<uint16_t>(~bit);
        }
        engine_->StartFxMorph(plan);

        for (size_t fx = 0; fx < kNumFx; fx++)
        {
            latched_[fx] = (scene.latched >> fx) & 1;
            for (size_t p = 0; p < kNumFxParams; p++)
                params_[fx][p] = scene.params[fx][p];
            engine_->SetFxOn(fx, IsOn(fx));
        }
        engine_->SetFxPool(Pool());
        morph_touched_ = 0;
        ClearChunks();
        edited_ = false;
    }

    /** One bar line more for the morph, up to kMaxMorphBars. False if there's none running or
     *  it's at the most. On the device, call it with the audio interrupt blocked */
    inline bool MorphMore() { return engine_->AddFxMorphBar(); }
    inline bool Morphing() const { return engine_->FxMorphing(); }

    /** Stops the morph where it is: the knobs take the values it got to, and an FX it hadn't
     *  switched yet stays as it was, unless its key was pressed meanwhile. Edited, since
     *  that's neither scene. False if none runs. On the device, call it with the audio
     *  interrupt blocked */
    FX_SCENE_ONCE bool FreezeMorph()
    {
        float live[kNumFx][kNumFxParams];
        uint16_t unswitched, was_on;
        if (!engine_->FreezeFxMorph(live, &unswitched, &was_on, &modes_))
            return false;
        SceneDecides();
        for (size_t fx = 0; fx < kNumFx; fx++)
        {
            const uint16_t bit = Bit(fx);
            // a send left ringing out keeps the scene for when it comes on
            if (Stale(fx) && !(unswitched & bit))
                continue;
            for (size_t p = 0; p < kNumFxParams; p++)
                params_[fx][p] = live[fx][p];
            stale_ &= static_cast<uint16_t>(~bit);
            if (unswitched & bit)
            {
                if (!(morph_touched_ & bit))
                    latched_[fx] = was_on & bit;
                SendOn(fx); // a key held meanwhile stays on
            }
        }
        ClearChunks();
        edited_ = true;
        return true;
    }

    /** Makes the held morph a drop (#175, FxMorph::Drop): hard to scene on its landing, the
     *  bar line SHIFT's release counts to, or without one to where the morph goes, edits and
     *  all; glide: a build-up, the morph glides on to the line first. The controls show it at
     *  once, as in a morph; what's off at the line keeps what the engine has until its key
     *  next comes on (stale_). False if no morph is held or the crossfader has it. On the
     *  device, call it with the audio interrupt blocked */
    FX_SCENE_ONCE bool Drop(const FxScene* scene, bool glide)
    {
        if (!engine_->FxMorphHeld() || Fading())
            return false;
        uint16_t ui_on = 0, on = 0;
        for (size_t fx = 0; fx < kNumFx; fx++)
            ui_on |= IsOn(fx) ? Bit(fx) : 0;
        if (scene)
        {
            SceneDecides();
            for (size_t fx = 0; fx < kNumFx; fx++)
            {
                latched_[fx] = (scene->latched >> fx) & 1;
                for (size_t p = 0; p < kNumFxParams; p++)
                    params_[fx][p] = scene->params[fx][p];
            }
            modes_ = scene->modes;
            morph_touched_ = 0;
            ClearChunks();
            edited_ = false;
        }
        for (size_t fx = 0; fx < kNumFx; fx++)
            on |= IsOn(fx) ? Bit(fx) : 0;
        if (!engine_->DropFxMorph(params_, ui_on, on, glide, modes_))
            return false;
        stale_ = static_cast<uint16_t>(~on & ((1u << kNumFx) - 1));
        for (size_t fx = 0; fx < kNumFx; fx++)
            engine_->SetFxOn(fx, IsOn(fx));
        engine_->SetFxPool(Pool());
        return true;
    }
    inline bool Dropping() const { return engine_->FxDropping(); }

    /** The crossfader (#66): the morph at t, 0 where it started to 1 the scene, in the
     *  hand from now on (FxMorph::Fader). False if none runs. On the device, call it with the
     *  audio interrupt blocked */
    bool Fade(float t)
    {
        if (!engine_->FadeFxMorph(t))
            return false;
        fader_ = t;
        fading_ = true;
        return true;
    }
    /** Whether the crossfader has the morph, and where it is */
    inline bool Fading() const { return fading_ && engine_->FxMorphing(); }
    inline float FaderPos() const { return fader_; }
    /** A morph started and waiting for SHIFT to be let go: the crossfader can take it */
    inline bool MorphHeld() const { return engine_->FxMorphHeld(); }

    /** The crossfader let go. At the scene's end the morph lands, as recalled: 1. At the
     *  start's it stops and everything is as it was before the morph, the knobs, the latches
     *  and whether the scene was edited: -1 (the UI takes back its active scene). In between it
     *  stops where it is, edited, as FreezeMorph: 0. On the device, call it with the audio
     *  interrupt blocked */
    FX_SCENE_ONCE int EndFade()
    {
        fading_ = false;
        if (fader_ >= 1.f)
        {
            engine_->LandFxMorph();
            return 1;
        }
        // stopped first, so no key the morph would switch at the landing goes off and on
        if (!FreezeMorph() || fader_ > 0.f)
            return 0;
        Recall(start_);
        edited_ = start_edited_;
        return -1;
    }

    /** The grid's next point from val in the direction dir, or val if there is none in
     *  0-1. A value within a hair of a point (1% of an even grid's spacing, .001 on a list
     *  of points) counts as on it, so it moves a whole step */
    static float CoarseStep(const FxGrid& grid, float val, float dir)
    {
        if (grid.points)
        {
            const float eps = .001f;
            if (dir > 0.f)
            {
                for (size_t i = 0; i < grid.num_points; i++)
                    if (grid.points[i] > val + eps)
                        return grid.points[i];
            }
            else
            {
                for (size_t i = grid.num_points; i-- > 0;)
                    if (grid.points[i] < val - eps)
                        return grid.points[i];
            }
            return val;
        }

        const float eps = grid.spacing * .01f;
        const float pos = (val - grid.origin) / grid.spacing;
        const float k = dir > 0.f ? floorf(pos + .01f) + 1.f : ceilf(pos - .01f) - 1.f;
        const float next = grid.origin + k * grid.spacing;
        return next < -eps || next > 1.f + eps ? val : next;
    }

    inline float Param(size_t fx, size_t param) const { return params_[fx][param]; }
    /** The finalizer's parameter p (m * kNumFxKnobs + k: module m's knob k), all of them */
    inline float FinParam(size_t p) const { return fin_[p]; }
    inline const float* FinParams() const { return fin_; }
    static inline bool FinUsed(size_t p)
    {
        return p < kNumFinParams && (kFinalizerKnobs[p / kNumFxKnobs].knobs >> (p % kNumFxKnobs) & 1);
    }
    static inline float FinDefault(size_t p)
    {
        return kFinalizerKnobs[p / kNumFxKnobs].defaults[p % kNumFxKnobs];
    }
    /** A finalizer parameter set, from its knob, the card or SysEx: clamped and sent, and
     *  kept (TakeFinChanged). keep false: MIDI's CCs, which the card never gets (#97 D6). A
     *  stepped one on its nearest step */
    void SetFin(size_t p, float val, bool keep = true)
    {
        if (!FinUsed(p))
            return;
        val = fclamp(val, 0.f, 1.f);
        const uint8_t steps = kFinalizerKnobs[p / kNumFxKnobs].steps[p % kNumFxKnobs];
        if (steps)
            val = roundf(val * (steps - 1)) / (steps - 1);
        if (keep && val != fin_[p])
            fin_changed_ |= static_cast<uint16_t>(1u << p);
        fin_[p] = val;
        engine_->SetFinParam(p, val);
    }
    /** The finalizer's parameters changed to be kept since last asked, a bit each, so the
     *  play page can keep those */
    uint16_t TakeFinChanged()
    {
        const uint16_t changed = fin_changed_;
        fin_changed_ = 0;
        return changed;
    }
    /** The module the knobs show on the finalizer's key, 0-3 (Finalizer::Module): the next,
     *  1 to 4 and round (SHIFT + its key, once it is selected), or one outright (MIDI) */
    inline size_t Module() const { return module_; }
    void SetModule(size_t m)
    {
        if (m < kFinModules && m != module_)
        {
            module_ = static_cast<uint8_t>(m);
            ClearChunks();
        }
    }
    void NextModule() { SetModule((module_ + 1) % kFinModules); }
    /** A band of the finalizer muted (#347): live, not kept */
    inline bool Muted(size_t band) const { return mutes_ >> band & 1; }
    void SetMute(size_t band, bool mute)
    {
        if (band >= kFinBands)
            return;
        mutes_ = static_cast<uint8_t>(mute ? mutes_ | 1u << band : mutes_ & ~(1u << band));
        engine_->SetFinMute(band, mute);
    }
    /** Every band unmuted: power-on, a preset recalled */
    void Unmute()
    {
        for (size_t b = 0; b < kFinBands; b++)
            SetMute(b, false);
    }
    /** The finalizer bypassed: live, not kept */
    inline bool Bypassed() const { return bypass_; }
    void SetBypass(bool bypass)
    {
        bypass_ = bypass;
        engine_->SetFinBypass(bypass);
    }
    inline bool IsLatched(size_t fx) const { return latched_[fx]; }
    /** The chaos key's pool (FxChaos.h): the effects latched, but not held, nor the chaos key
     *  itself, so what you play by hand stays on */
    uint16_t Pool() const
    {
        uint16_t pool = 0;
        for (size_t fx = 0; fx < kNumSoundFx; fx++)
            if (latched_[fx] && !held_[fx])
                pool |= Bit(fx);
        return pool;
    }
    inline bool IsOn(size_t fx) const { return held_[fx] || latched_[fx]; }
    /** The FX the knobs edit: the last one pressed, or kFinSelected */
    inline size_t Selected() const { return selected_; }
    /** Knobs or latches changed since the last Snapshot or Recall */
    inline bool Edited() const { return edited_; }
    inline void MarkEdited() { edited_ = true; }

    /** The selected FX's knobs, or the finalizer's module's */
    inline const FxParams& Knobs() const
    {
        if (selected_ == kFinSelected)
            return kFinalizerKnobs[module_];
        return kFxParams[selected_];
    }
    /** Whether the knobs' FX has parameters on page 2 (the finalizer has none) */
    inline bool HasPage2() const { return (Knobs().knobs >> kNumFxKnobs) != 0; }
    /** The page the knobs show, 0 or 1; always 0 without a page 2 */
    inline size_t Page() const { return page_; }
    /** The parameter knob 0-3 edits on the page shown */
    inline size_t ParamOf(size_t knob) const { return knob + kNumFxKnobs * page_; }
    /** Whether knob 0-3 does something on the page shown */
    inline bool KnobUsed(size_t knob) const { return (Knobs().knobs >> ParamOf(knob)) & 1; }
    /** Knob 0-3's value on the page shown */
    inline float Knob(size_t knob) const
    {
        if (selected_ == kFinSelected)
            return fin_[module_ * kNumFxKnobs + ParamOf(knob)];
        return params_[selected_][ParamOf(knob)];
    }

private:
    static inline uint16_t Bit(size_t fx) { return static_cast<uint16_t>(1u << fx); }

    /** The parameters and latches into scene, used */
    FX_SCENE_ONCE void Capture(FxScene& scene) const
    {
        scene.used = true;
        scene.latched = 0;
        scene.modes = modes_;
        for (size_t fx = 0; fx < kNumFx; fx++)
        {
            if (latched_[fx])
                scene.latched |= Bit(fx);
            for (size_t p = 0; p < kNumFxParams; p++)
                scene.params[fx][p] = params_[fx][p];
        }
    }
    inline bool Stale(size_t fx) const { return (stale_ >> fx) & 1; }

    /** The modes to the engine, those that differ from modes_ (which stays as it was) */
    void SendModes(uint16_t modes)
    {
        for (size_t fx = 0; fx < kNumFx; fx++)
            if ((modes ^ modes_) >> fx & 1)
                engine_->SetFxMode(fx, modes >> fx & 1);
    }

    /** The key's state to the engine; coming on, first the parameters it lags behind in */
    void SendOn(size_t fx)
    {
        if (IsOn(fx) && Stale(fx))
        {
            for (size_t p = 0; p < kNumFxParams; p++)
                engine_->SetFxParam(fx, p, params_[fx][p]);
            stale_ &= static_cast<uint16_t>(~Bit(fx));
        }
        engine_->SetFxOn(fx, IsOn(fx));
        engine_->SetFxPool(Pool());
    }

    /** A recall, morph or stop: the latches are the scene's, and the keys held keep them */
    void SceneDecides()
    {
        for (size_t fx = 0; fx < kNumFx; fx++)
            on_release_[fx] = OnRelease::KEEP;
    }

    /** A key pressed during a morph: it decides that FX's latch, also if the morph stops */
    void TouchedInMorph(size_t fx)
    {
        if (engine_->FxMorphing())
            morph_touched_ |= Bit(fx);
    }

    void ClearChunks()
    {
        for (size_t knob = 0; knob < kNumFxKnobs; knob++)
            chunk_[knob] = 0.f;
    }

    void SetKnob(size_t knob, float val)
    {
        if (selected_ == kFinSelected)
            SetFin(module_ * kNumFxKnobs + ParamOf(knob), val);
        else
            SetParam(selected_, ParamOf(knob), val);
    }

    void SetParam(size_t fx, size_t param, float val)
    {
        val = fclamp(val, 0.f, 1.f);
        if (val != params_[fx][param])
            edited_ = true;
        params_[fx][param] = val;
        engine_->SetFxParam(fx, param, val);
    }

    /** What a held key's release does to its latch */
    enum class OnRelease : uint8_t
    {
        KEEP,   // nothing: a scene decided it meanwhile
        CLEAR,  // a plain press
        TOGGLE, // SHIFT went down during the hold; undone by ShiftUsed
    };

    Engine* engine_ = nullptr;
    float params_[kNumFx][kNumFxParams];
    uint16_t modes_ = 0; // bit fx: in its second mode (kFxModes)
    bool held_[kNumFx];
    bool latched_[kNumFx];
    OnRelease on_release_[kNumFx];
    uint16_t rebound_ = 0;               // bit fx: let go, and Rebound may take that back
    uint16_t was_held_ = 0;              // bit fx: held before that release
    uint16_t was_latched_ = 0;           // bit fx: latched before it
    OnRelease was_on_release_[kNumFx];   // and what it was to do
    size_t selected_ = 0;
    size_t page_ = 0;           // the knobs' page, 0 or 1 (ParamOf)
    float fin_[kNumFinParams];  // the finalizer's parameters, module by module
    uint16_t fin_changed_ = 0;  // a bit a parameter, since the last TakeFinChanged
    uint8_t module_ = 0;        // the finalizer's module on the knobs (Module)
    uint8_t mutes_ = 0;         // bit b: its band b muted
    bool bypass_ = false;       // it's bypassed
    float chunk_[kNumFxKnobs];  // detents towards the next step or grid point
    bool chunk_shift_ = false;  // whether they were turned with SHIFT
    bool edited_ = false;
    uint16_t stale_ = 0;         // bit fx: off, and the engine has older parameters than
                                 // params_: a send ringing out, or an FX a morph parked
    uint16_t morph_touched_ = 0; // bit fx: its key pressed during the morph
    FxScene start_;              // where the last morph started, for the crossfader
    bool start_edited_ = false;  // and whether its scene was edited then
    bool fading_ = false;        // the crossfader has the morph (Fade)
    float fader_ = 0.f;          // where it is, 0 the start to 1 the scene
};

} // namespace chompi

#pragma GCC pop_options
