/** @file FxControls.h
 *  @brief What the play page does with the punch-in FX keys and knobs 1-4, without the
 *  hardware: the parameters, which keys are held and latched, which FX the knobs edit and on
 *  which page, fine, stepped and coarse turns, and taking, recalling or morphing to a scene. NormalPage.h routes the keys and
 *  knobs here and draws the LEDs from it; test/controls.cpp runs it on the host.
 *
 *  The knobs have two pages, switched together: a plain press on any of them turns all four
 *  over to the selected FX's or the compressor's page 2 (its parameters 4-7, FxParams.h) and
 *  back; on an FX with nothing on page 2 it does nothing. Selecting
 *  another FX, or the compressor, goes back to page 1, so the main controls are under the
 *  fingers whenever an effect is picked. SHIFT + turn and SHIFT + press act on the page shown.
 *
 *  The master compressor (MasterComp.h) is edited here too: its key selects it for the knobs
 *  (kCompSelected), which then turn as they do for an FX (kCompParams). It's always on and
 *  not part of a scene, so its knobs leave the scene unedited.
 *
 *  Engine is PassthroughEngine on the device, a fake on the host. It needs SetFxOn(fx, on),
 *  SetFxParam(fx, param, val), SetCompParam(param, val), FastFxSlew() and the morph's
 *  StartFxMorph(plan), AddFxMorphBar(), LandFxMorph(), FreezeFxMorph(params, unswitched,
 *  was_on), FxMorphing(), the crossfader's FxMorphHeld() and FadeFxMorph(t), and
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

namespace chompi
{

// Knobs: 1% per detent; a stepped parameter moves one step per kFxDetentsPerStep detents
static const float kFxParamStep = .01f;
static const float kFxDetentsPerStep = 3.f;
// The sends: their tails ring on after their keys go off, so a recall or morph doesn't change
// an off send's settings in the engine until its key comes on again
static const uint16_t kFxSends = (1u << FX_DELAY) | (1u << FX_REVERB);
// Selected() while the knobs edit the master compressor
static const size_t kCompSelected = kNumFx;

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
        for (size_t p = 0; p < kNumFxParams; p++)
        {
            // an unused one stays on its default; a used one counts as changed and is sent
            comp_[p] = (kCompParams.knobs >> p) & 1 ? -1.f : kCompParams.defaults[p];
            SetComp(p, kCompParams.defaults[p]);
        }
        comp_changed_ = false;
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
        }
        else
        {
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

    /** The master compressor's key going down: selects it for the knobs. It has no gate or
     *  latch, so the release does nothing. With SHIFT, a select as an FX key's would be */
    void CompKeyPressed(bool shift)
    {
        if (selected_ != kCompSelected)
            ClearChunks();
        selected_ = kCompSelected;
        page_ = 0;
        if (shift)
            ShiftUsed();
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

    /** Knob 0-3 turned by detents (signed). Plain: 1% per detent, or a step per
     *  kFxDetentsPerStep for a stepped parameter. SHIFT: one point of the coarse grid per
     *  detent. A knob the page doesn't use does nothing */
    void KnobTurned(size_t knob, float detents, bool shift)
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
        if (steps == 0)
        {
            SetKnob(knob, val + detents * kFxParamStep);
            return;
        }

        // stepped: every kFxDetentsPerStep detents moves one step
        chunk_[knob] += detents;
        if (chunk_[knob] >= kFxDetentsPerStep || chunk_[knob] <= -kFxDetentsPerStep)
        {
            const float step = 1.f / (steps - 1);
            const float dir = chunk_[knob] > 0.f ? 1.f : -1.f;
            // snap to the step grid, so values set elsewhere can't drift off it
            const float idx = roundf(val / step) + dir;
            SetKnob(knob, idx * step);
            chunk_[knob] = 0.f;
        }
    }

    /** Knob 0-3 pressed: with SHIFT, resets that parameter to its default, on a knob the page
     *  uses. A plain press turns the page, all four knobs together, for an FX or the
     *  compressor with a page 2. True if the press did something */
    bool KnobPressed(size_t knob, bool shift)
    {
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
        if (fxp.steps[param])
        {
            const float step = 1.f / (fxp.steps[param] - 1);
            val = roundf(fclamp(val, 0.f, 1.f) / step) * step;
        }
        SetParam(fx, param, val);
    }

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
     *   - one it turns on: its fade knobs (FxParams::fade) start at their neutral defaults
     *     and glide to the scene, the rest jump now; the key comes on as soon as they've got
     *     there;
     *   - one it turns off: its fade knobs glide to their defaults and the key goes off at the
     *     landing; the engine keeps it there, silent, until its key next comes on;
     *   - an FX without fade knobs it turns on or off: all at the landing.
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
        for (size_t fx = 0; fx < kNumFx; fx++)
        {
            const FxParams& fxp = kFxParams[fx];
            const bool was = IsOn(fx);
            const bool will = held_[fx] || ((scene.latched >> fx) & 1);
            const uint16_t bit = Bit(fx);
            if (was)
                plan.was_on |= bit;
            if (was != will)
            {
                plan.deferred |= bit;
                if (will && fxp.fade)
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
                const bool fade = (fxp.fade >> p) & 1;
                float from = params_[fx][p];
                MorphParam how = MorphParam::HOLD;
                if (!was && !will)
                    from = to;
                else if (was && will)
                    how = fxp.steps[p] == 0 ? MorphParam::GLIDE : MorphParam::HOLD;
                else if (fxp.fade && will)
                {
                    from = fade ? fxp.defaults[p] : to;
                    how = fade ? MorphParam::GLIDE : MorphParam::HOLD;
                }
                else if (fxp.fade && fade)
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
        if (!engine_->FreezeFxMorph(live, &unswitched, &was_on))
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
    inline float CompParam(size_t param) const { return comp_[param]; }
    /** A compressor parameter set, from the card or MIDI; clamped and sent */
    void SetComp(size_t param, float val)
    {
        if (param >= kNumFxParams || !((kCompParams.knobs >> param) & 1))
            return;
        val = fclamp(val, 0.f, 1.f);
        if (val != comp_[param])
            comp_changed_ = true;
        comp_[param] = val;
        engine_->SetCompParam(param, val);
    }
    /** True once after the compressor's knobs changed, so the play page can keep them */
    bool TakeCompChanged()
    {
        const bool changed = comp_changed_;
        comp_changed_ = false;
        return changed;
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
    /** The FX the knobs edit: the last one pressed, or kCompSelected */
    inline size_t Selected() const { return selected_; }
    /** Knobs or latches changed since the last Snapshot or Recall */
    inline bool Edited() const { return edited_; }
    inline void MarkEdited() { edited_ = true; }

    /** The selected FX's or the compressor's knobs */
    inline const FxParams& Knobs() const
    {
        if (selected_ == kCompSelected)
            return kCompParams;
        return kFxParams[selected_];
    }
    /** Whether the knobs' FX or the compressor has parameters on page 2 */
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
        if (selected_ == kCompSelected)
            return comp_[ParamOf(knob)];
        return params_[selected_][ParamOf(knob)];
    }

private:
    static inline uint16_t Bit(size_t fx) { return static_cast<uint16_t>(1u << fx); }

    /** The parameters and latches into scene, used */
    FX_SCENE_ONCE void Capture(FxScene& scene) const
    {
        scene.used = true;
        scene.latched = 0;
        for (size_t fx = 0; fx < kNumFx; fx++)
        {
            if (latched_[fx])
                scene.latched |= Bit(fx);
            for (size_t p = 0; p < kNumFxParams; p++)
                scene.params[fx][p] = params_[fx][p];
        }
    }
    inline bool Stale(size_t fx) const { return (stale_ >> fx) & 1; }

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
        if (selected_ == kCompSelected)
            SetComp(ParamOf(knob), val);
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
    bool held_[kNumFx];
    bool latched_[kNumFx];
    OnRelease on_release_[kNumFx];
    size_t selected_ = 0;
    size_t page_ = 0;           // the knobs' page, 0 or 1 (ParamOf)
    float comp_[kNumFxParams];  // the master compressor's parameters, both pages
    bool comp_changed_ = false; // since the last TakeCompChanged
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
