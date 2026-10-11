// controls.cpp: checks the play page's FX and scene logic (FxControls.h, SceneControls.h)
// against a fake engine that records what it's sent. Exits 0 when everything passes. Run by
// unit.sh controls.
#include <cmath>
#include <cstdio>
#include "check.h"
#include "SceneControls.h"

using namespace chompi;


static bool Near(float a, float b) { return fabsf(a - b) < 1e-5f; }

/** Records what the controls send */
struct FakeEngine
{
    bool on[kNumFx] = {};
    float params[kNumFx][kNumFxParams] = {};
    int param_calls = 0;
    int fast_slews = 0;

    void SetFxOn(size_t fx, bool o) { on[fx] = o; }
    uint16_t pool = 0; // the chaos key's
    void SetFxPool(uint16_t p) { pool = p; }
    void SetFxParam(size_t fx, size_t p, float v)
    {
        params[fx][p] = v;
        param_calls++;
    }
    void FastFxSlew() { fast_slews++; }
    float comp[kNumFxParams] = {};
    void SetCompParam(size_t p, float v) { comp[p] = v; }

    // the morph: what it was started with, and its bar lines
    FxMorphPlan plan = {};
    bool morphing = false;
    uint32_t bars = 0;
    int landings = 0;
    void StartFxMorph(const FxMorphPlan& p)
    {
        plan = p;
        morphing = true;
        held = true;
        bars = 1;
    }
    // the crossfader: a morph held for SHIFT, taken to where the fader is
    bool held = false;
    float fader = -1.f;
    bool FxMorphHeld() const { return morphing && held; }
    bool FadeFxMorph(float t)
    {
        if (!morphing)
            return false;
        held = false;
        fader = t;
        return true;
    }
    bool AddFxMorphBar()
    {
        if (!morphing || bars >= kMaxMorphBars)
            return false;
        bars++;
        return true;
    }
    void LandFxMorph()
    {
        if (morphing)
            landings++;
        morphing = false;
    }
    bool FxMorphing() const { return morphing; }
    // a freeze reports the plan's start values, as if it hadn't moved yet
    bool FreezeFxMorph(float live[kNumFx][kNumFxParams], uint16_t* unswitched, uint16_t* was_on)
    {
        if (!morphing)
            return false;
        for (size_t f = 0; f < kNumFx; f++)
            for (size_t p = 0; p < kNumFxParams; p++)
                live[f][p] = plan.start[f][p];
        *unswitched = plan.deferred;
        *was_on = plan.deferred & plan.was_on;
        morphing = false;
        return true;
    }
};

using Fx = FxControls<FakeEngine>;
using Scenes = SceneControls<FakeEngine>;

static void Fresh(FakeEngine& e, Fx& fx)
{
    e = FakeEngine();
    fx.Init(&e);
}

static void TestInit()
{
    FakeEngine e;
    Fx fx;
    Fresh(e, fx);
    bool defaults = true, off = true;
    for (size_t f = 0; f < kNumFx; f++)
    {
        off = off && !e.on[f] && !fx.IsOn(f);
        for (size_t p = 0; p < kNumFxParams; p++)
            defaults = defaults && e.params[f][p] == kFxParams[f].defaults[p] &&
                       fx.Param(f, p) == kFxParams[f].defaults[p];
    }
    Check(defaults, "init: every parameter on its default, also in the engine");
    Check(off, "init: every FX off");
    Check(e.param_calls == static_cast<int>(kNumFx * kNumFxParams), "init: every parameter sent once");
}

// the latch: the key held, then SHIFT going down, then the key's release
static void Latch(Fx& fx, size_t f)
{
    fx.KeyPressed(f, true, false);
    fx.ShiftPressed();
    fx.KeyPressed(f, false, true);
}

static void TestKeys()
{
    FakeEngine e;
    Fx fx;
    Fresh(e, fx);

    fx.KeyPressed(FX_CRUSHER, true, false);
    Check(e.on[FX_CRUSHER] && fx.Selected() == FX_CRUSHER, "hold: on, and the knobs edit it");
    fx.KeyPressed(FX_CRUSHER, false, false);
    Check(!e.on[FX_CRUSHER], "release: off");

    // the key first, then SHIFT
    fx.KeyPressed(FX_DELAY, true, false);
    Check(fx.ShiftPressed(), "key, then SHIFT: a latch combo");
    fx.KeyPressed(FX_DELAY, false, true);
    Check(e.on[FX_DELAY] && fx.IsLatched(FX_DELAY), "key, then SHIFT: latched, stays on after release");

    // a plain press clears the latch, on release; it stays on while held
    fx.KeyPressed(FX_DELAY, true, false);
    Check(e.on[FX_DELAY], "key on a latched FX: on while held");
    fx.KeyPressed(FX_DELAY, false, false);
    Check(!e.on[FX_DELAY] && !fx.IsLatched(FX_DELAY), "... unlatched and off on release");

    // key, then SHIFT again unlatches
    Latch(fx, FX_DELAY);
    Latch(fx, FX_DELAY);
    Check(!e.on[FX_DELAY] && !fx.IsLatched(FX_DELAY), "key, then SHIFT on a latched FX: unlatched");

    // SHIFT first, then the key: a select, silent
    fx.KeyPressed(FX_FILTER, true, true);
    Check(fx.Selected() == FX_FILTER && !e.on[FX_FILTER], "SHIFT + key: the knobs edit it, it stays off");
    fx.KeyPressed(FX_FILTER, false, true);
    Check(!e.on[FX_FILTER] && !fx.IsLatched(FX_FILTER), "... and its release does nothing");
    Latch(fx, FX_DELAY);
    fx.KeyPressed(FX_FILTER, true, false);
    fx.KeyPressed(FX_FILTER, false, false);
    fx.KeyPressed(FX_DELAY, true, true);
    fx.KeyPressed(FX_DELAY, false, true);
    Check(fx.Selected() == FX_DELAY && e.on[FX_DELAY] && fx.IsLatched(FX_DELAY),
          "SHIFT + key on a latched FX: selected, still latched");
    Latch(fx, FX_DELAY);

    // SHIFT first, then the key and a coarse turn: edits it, silently
    fx.KeyPressed(FX_FOLDER, true, true);
    fx.KnobTurned(0, 1.f, true);
    fx.KeyPressed(FX_FOLDER, false, true);
    Check(fx.Param(FX_FOLDER, 0) != kFxParams[FX_FOLDER].defaults[0] && !e.on[FX_FOLDER] &&
              !fx.IsLatched(FX_FOLDER),
          "SHIFT + key, then a coarse turn: edited, still off");

    // SHIFT let go before the key, then SHIFT again: the key was a select, it stays silent
    fx.KeyPressed(FX_FOLDER, true, true);
    Check(!fx.ShiftPressed(), "a key held from a select: no latch combo");
    fx.KeyPressed(FX_FOLDER, false, true);
    Check(!e.on[FX_FOLDER] && !fx.IsLatched(FX_FOLDER), "... and no latch on its release");

    // SHIFT going down with no FX key held changes nothing
    Check(!fx.ShiftPressed(), "SHIFT alone: no latch combo");
    bool none = true;
    for (size_t f = 0; f < kNumFx; f++)
        none = none && !fx.IsLatched(f);
    Check(none, "SHIFT alone latches nothing");

    // the key, then SHIFT for something else: no latch
    fx.KeyPressed(FX_FILTER, true, false);
    fx.ShiftPressed();
    fx.KnobTurned(0, 1.f, true); // a coarse turn
    fx.KeyPressed(FX_FILTER, false, true);
    Check(!e.on[FX_FILTER] && !fx.IsLatched(FX_FILTER), "key, SHIFT + a coarse turn: not latched");
    fx.KeyPressed(FX_FILTER, true, false);
    fx.ShiftPressed();
    fx.ShiftUsed(); // tap tempo, the mix, a scene key ...
    fx.KeyPressed(FX_FILTER, false, true);
    Check(!fx.IsLatched(FX_FILTER), "key, SHIFT used for another combo: not latched");

    // one key held, then SHIFT, then another key: that's a select, the first doesn't latch
    fx.KeyPressed(FX_FILTER, true, false);
    fx.ShiftPressed();
    fx.KeyPressed(FX_CRUSHER, true, true);
    fx.KeyPressed(FX_CRUSHER, false, true);
    fx.KeyPressed(FX_FILTER, false, true);
    Check(fx.Selected() == FX_CRUSHER && !fx.IsLatched(FX_FILTER) && !fx.IsLatched(FX_CRUSHER) &&
              !e.on[FX_FILTER] && !e.on[FX_CRUSHER],
          "key, SHIFT, another key: that one selected, neither latched");

    // two keys held, then SHIFT: both latch
    fx.KeyPressed(FX_FILTER, true, false);
    fx.KeyPressed(FX_CRUSHER, true, false);
    fx.ShiftPressed();
    fx.KeyPressed(FX_FILTER, false, true);
    fx.KeyPressed(FX_CRUSHER, false, true);
    Check(fx.IsLatched(FX_FILTER) && fx.IsLatched(FX_CRUSHER), "two keys, then SHIFT: both latched");
    Latch(fx, FX_FILTER);
    Latch(fx, FX_CRUSHER);

    // the key, then SHIFT twice (a confirm tapped while holding it): still the one latch
    fx.KeyPressed(FX_FILTER, true, false);
    Check(fx.ShiftPressed(), "key, then SHIFT: a latch combo");
    Check(fx.ShiftPressed(), "... and SHIFT again in the same hold: still one");
    fx.KeyPressed(FX_FILTER, false, true);
    Check(fx.IsLatched(FX_FILTER) && e.on[FX_FILTER], "key, SHIFT twice: latched");
    Latch(fx, FX_FILTER);

    // the key, then SHIFT + a knob the FX doesn't use (the flanger's page-2 knob 1, it has no
    // Mix): still a latch
    fx.KeyPressed(FX_FLANGER, true, false);
    fx.KnobPressed(0, false);
    fx.ShiftPressed();
    fx.KnobTurned(0, 1.f, true);
    fx.KnobPressed(0, true);
    fx.KeyPressed(FX_FLANGER, false, true);
    Check(fx.IsLatched(FX_FLANGER), "key, SHIFT + a dark knob turned or pressed: still latched");
    fx.KnobPressed(0, false);
    Latch(fx, FX_TAPESTOP);

    // a release without its press (held through boot) does nothing
    fx.KeyPressed(FX_SLICER, false, false);
    Check(!e.on[FX_SLICER] && !fx.IsLatched(FX_SLICER), "a release without its press: nothing");
}

static void TestKnobs()
{
    FakeEngine e;
    Fx fx;
    Fresh(e, fx);

    // continuous: 1% per detent, clamped
    fx.KeyPressed(FX_REVERB, true, false);
    const float decay = fx.Param(FX_REVERB, 0);
    fx.KnobTurned(0, 5.f, false);
    Check(Near(fx.Param(FX_REVERB, 0), decay + .05f), "fine: 1% per detent");
    Check(e.params[FX_REVERB][0] == fx.Param(FX_REVERB, 0), "fine: sent to the engine");
    fx.KnobTurned(0, 500.f, false);
    Check(fx.Param(FX_REVERB, 0) == 1.f, "fine: clamped at 1");

    // stepped: a step per 3 detents, snapped to the step grid
    fx.KeyPressed(FX_DELAY, true, false);
    const float step = 1.f / (DelaySend::kNumDivisions - 1);
    const float div = fx.Param(FX_DELAY, 0);
    fx.KnobTurned(0, 2.f, false);
    Check(fx.Param(FX_DELAY, 0) == div, "stepped: 2 detents do nothing yet");
    fx.KnobTurned(0, 1.f, false);
    Check(Near(fx.Param(FX_DELAY, 0), div + step), "stepped: the 3rd moves one step");
    fx.KnobTurned(0, -3.f, false);
    Check(Near(fx.Param(FX_DELAY, 0), div), "stepped: and back");

    // coarse, a spacing grid: the folder's drive in .2 steps
    fx.KeyPressed(FX_FOLDER, true, false);
    fx.KnobTurned(0, 3.f, false); // 0 -> .03, off the grid
    fx.KnobTurned(0, 1.f, true);
    Check(Near(fx.Param(FX_FOLDER, 0), .2f), "coarse: off the grid, snaps to the next point up");
    fx.KnobTurned(0, 1.f, true);
    Check(Near(fx.Param(FX_FOLDER, 0), .4f), "coarse: on a point, moves a whole step");
    fx.KnobTurned(0, 10.f, true);
    Check(Near(fx.Param(FX_FOLDER, 0), 1.f), "coarse: stops at the top");
    fx.KnobTurned(0, -1.f, true);
    Check(Near(fx.Param(FX_FOLDER, 0), .8f), "coarse: down a step");

    // coarse, a list of points: the shifter's intervals
    fx.KeyPressed(FX_SHIFTER, true, false);
    fx.KnobTurned(0, 1.f, true); // 0 -> +5
    Check(Near(fx.Param(FX_SHIFTER, 0), 17.f / 24.f), "coarse points: 0 up to +5");
    fx.KnobTurned(0, 2.f, true); // +5 -> +7 -> +12
    Check(Near(fx.Param(FX_SHIFTER, 0), 1.f), "coarse points: two more up is +12");
    fx.KnobTurned(0, 1.f, true);
    Check(Near(fx.Param(FX_SHIFTER, 0), 1.f), "coarse points: nothing above +12");
    fx.KnobTurned(0, -3.f, true); // +12 -> +7 -> +5 -> 0
    Check(Near(fx.Param(FX_SHIFTER, 0), .5f), "coarse points: three down from +12 is 0");

    // SHIFT + press resets, a plain press doesn't (it turns the page, TestPages)
    fx.KeyPressed(FX_REVERB, true, false);
    fx.KnobPressed(0, false);
    Check(fx.Param(FX_REVERB, 0) == 1.f, "plain press: no reset");
    fx.KnobPressed(0, false);
    fx.KnobPressed(0, true);
    Check(fx.Param(FX_REVERB, 0) == kFxParams[FX_REVERB].defaults[0], "SHIFT + press: the default");

    // leftover detents stay with their FX and their mode
    fx.KeyPressed(FX_DELAY, true, false);
    const float div2 = fx.Param(FX_DELAY, 0);
    fx.KnobTurned(0, 2.f, false);
    fx.KeyPressed(FX_SLICER, true, false);
    const float pattern = fx.Param(FX_SLICER, 0);
    fx.KnobTurned(0, 1.f, false);
    Check(fx.Param(FX_SLICER, 0) == pattern, "detents don't carry over to another FX");
    fx.KeyPressed(FX_DELAY, true, false);
    fx.KnobTurned(0, 2.f, false);
    fx.KnobTurned(0, .5f, true);
    Check(fx.Param(FX_DELAY, 0) == div2, "fine detents don't carry over into coarse");
    fx.KnobTurned(0, .5f, true);
    Check(fx.Param(FX_DELAY, 0) != div2, "... coarse counts its own");

    // the grid's helper on its own
    const FxGrid g = {0.f, .25f, nullptr, 0};
    Check(Near(Fx::CoarseStep(g, .5f, 1.f), .75f) && Near(Fx::CoarseStep(g, .5f, -1.f), .25f),
          "CoarseStep: a point moves to its neighbours");
    Check(Fx::CoarseStep(g, 0.f, -1.f) == 0.f && Fx::CoarseStep(g, 1.f, 1.f) == 1.f,
          "CoarseStep: nothing past 0 or 1");
}

// page 2 (#35): a plain knob press turns all four knobs over to the FX's parameters 4-7
static void TestPages()
{
    FakeEngine e;
    Fx fx;
    Fresh(e, fx);
    fx.KeyPressed(FX_SHIFTER, true, false);
    Check(fx.Page() == 0 && fx.KnobUsed(1), "pages: page 1 at first, every knob of it used");
    Check(fx.KnobPressed(2, false) && fx.Page() == 1, "pages: a plain press on any knob turns to page 2");
    Check(!fx.Edited(), "pages: turning the page isn't an edit");
    Check(fx.KnobUsed(0) && fx.KnobUsed(1) && fx.KnobUsed(2) && fx.KnobUsed(3),
          "pages: the shifter's page 2: Mix, its grain, Band and Level");
    Check(fx.Knob(0) == 1.f, "pages: the mix starts fully shifted");
    fx.KnobTurned(0, -40.f, false);
    Check(Near(fx.Param(FX_SHIFTER, 4), .6f) && Near(e.params[FX_SHIFTER][4], .6f)
              && fx.Param(FX_SHIFTER, 0) == .5f,
          "pages: knob 1 turns parameter 4, page 1's knob 1 stays");
    Check(Near(fx.Knob(0), .6f), "pages: the knob reads page 2's value");
    fx.KnobTurned(0, 1.f, true);
    Check(Near(fx.Param(FX_SHIFTER, 4), .7f), "pages: SHIFT + turn on page 2, its coarse grid");
    fx.KnobPressed(0, true);
    Check(fx.Param(FX_SHIFTER, 4) == 1.f && fx.Page() == 1, "pages: SHIFT + press resets it, on page 2");
    fx.KeyPressed(FX_SHIFTER, false, false);
    fx.KeyPressed(FX_SHIFTER, true, false);
    Check(fx.Page() == 1, "pages: the same FX pressed again keeps page 2");
    fx.KeyPressed(FX_FLANGER, true, true);
    Check(fx.Page() == 0, "pages: another FX, also selected with SHIFT, goes back to page 1");
    Check(fx.KnobPressed(0, false) && fx.Page() == 1 && !fx.KnobUsed(0) && fx.KnobUsed(1),
          "pages: the flanger's page 2 has no Mix, its knob 1 dark");
    const int calls = e.param_calls;
    fx.KnobTurned(0, 10.f, false);
    fx.KnobPressed(0, true);
    Check(e.param_calls == calls, "pages: an unused knob there does nothing, turned or reset");
    fx.KeyPressed(FX_DELAY, true, true);
    fx.KnobPressed(0, false);
    Check(fx.Page() == 1 && fx.KnobUsed(0) && fx.KnobUsed(1) && fx.KnobUsed(2) && fx.KnobUsed(3)
              && fx.Knob(0) == 0.f && fx.Knob(1) == .5f && fx.Knob(3) == 0.f,
          "pages: a send's page 2: freeze (off), its own, Band, ducking (off)");
    fx.KeyPressed(FX_SHIFTER, true, false);
    fx.KnobPressed(0, false);
    fx.KnobPressed(0, false);
    Check(fx.Page() == 0, "pages: a second press turns back to page 1");
    fx.KnobPressed(0, false);
    fx.CompKeyPressed(false);
    Check(fx.Page() == 0, "pages: the compressor's key goes back to page 1");
    Check(fx.KnobPressed(0, false) && fx.Page() == 1, "pages: the compressor has a page 2");
    Check(fx.KnobUsed(0) && !fx.KnobUsed(1) && fx.KnobUsed(2) && fx.KnobUsed(3)
              && fx.Knob(0) == 1.f && fx.Knob(2) == 0.f && fx.Knob(3) == 0.f,
          "pages: the compressor's page 2: Mix on knob 1 (fully compressed), the sidechain "
          "highpass on knob 3 (off), Makeup on knob 4 (0dB)");
    fx.KnobTurned(3, 25.f, false);
    Check(Near(fx.CompParam(7), .25f) && Near(e.comp[7], .25f) && fx.CompParam(3) == .5f,
          "pages: the compressor's knob 4 there turns its makeup, not its release");
    fx.KnobPressed(3, true);
    Check(fx.CompParam(7) == 0.f, "pages: SHIFT + press resets the makeup to 0dB");
    fx.KnobPressed(0, false);
    // MIDI: page 2 outright, but not where an effect has none
    fx.SetParamTo(FX_SHIFTER, 4, .25f);
    fx.SetParamTo(FX_FLANGER, 4, .25f);
    Check(fx.Param(FX_SHIFTER, 4) == .25f && fx.Param(FX_FLANGER, 4) == kFxParams[FX_FLANGER].defaults[4],
          "pages: set outright on page 2, only where the FX uses it");
    // a scene keeps page 2, and a recall puts it back
    FxScene scene;
    fx.Snapshot(scene);
    Check(scene.params[FX_SHIFTER][4] == .25f, "pages: a snapshot takes page 2");
    fx.SetParamTo(FX_SHIFTER, 4, 1.f);
    fx.Recall(scene);
    Check(fx.Param(FX_SHIFTER, 4) == .25f && e.params[FX_SHIFTER][4] == .25f,
          "pages: a recall puts page 2 back, also in the engine");
    // a morph glides it with page 1's, the shifter on in both
    fx.SetLatch(FX_SHIFTER, true);
    fx.Snapshot(scene);
    scene.params[FX_SHIFTER][4] = .75f;
    fx.Morph(scene);
    Check(e.plan.how[FX_SHIFTER][4] == MorphParam::GLIDE && e.plan.start[FX_SHIFTER][4] == .25f
              && e.plan.target[FX_SHIFTER][4] == .75f,
          "pages: a morph glides page 2's knobs too");
}

static void TestComp()
{
    FakeEngine e;
    Fx fx;
    Fresh(e, fx);
    bool defaults = true;
    for (size_t p = 0; p < kNumFxKnobs; p++)
        defaults = defaults && e.comp[p] == kCompParams.defaults[p] &&
                   fx.CompParam(p) == kCompParams.defaults[p];
    Check(defaults, "compressor: init on its defaults, also in the engine");
    Check(!fx.TakeCompChanged(), "compressor: init isn't a change to keep");

    // its key selects it; the knobs then edit it and leave the FX alone
    fx.KeyPressed(FX_FILTER, true, false);
    fx.KeyPressed(FX_FILTER, false, false);
    const float filter = fx.Param(FX_FILTER, 0);
    fx.CompKeyPressed(false);
    Check(fx.Selected() == kCompSelected, "compressor key: the knobs edit it");
    fx.KnobTurned(0, 10.f, false);
    Check(Near(fx.CompParam(0), .1f) && Near(e.comp[0], .1f) && fx.Param(FX_FILTER, 0) == filter,
          "compressor: a fine turn moves its knob, not the FX's");
    Check(fx.TakeCompChanged() == 1 && !fx.TakeCompChanged(), "compressor: a turn is a change, once, of its knob");
    fx.SetComp(1, .9f, false);
    Check(Near(fx.CompParam(1), .9f) && Near(e.comp[1], .9f) && !fx.TakeCompChanged(),
          "compressor: set by MIDI's CC it changes, but isn't a change to keep (#97)");
    fx.SetComp(1, kCompParams.defaults[1]);
    fx.TakeCompChanged();
    Check(!fx.Edited(), "compressor: not part of a scene, so the scene isn't edited");

    // coarse: the ratio's grid points
    fx.KnobTurned(1, 1.f, true);
    Check(Near(fx.CompParam(1), .75f), "compressor: a coarse ratio turn goes to the next point");
    fx.KnobPressed(1, true);
    Check(fx.CompParam(1) == kCompParams.defaults[1], "compressor: SHIFT + knob press resets it");
    fx.KnobTurned(3, 200.f, false);
    Check(fx.CompParam(3) == 1.f, "compressor: knobs stop at 1");

    // with SHIFT, a select as an FX key's: a key held doesn't latch
    fx.KeyPressed(FX_CRUSHER, true, false);
    fx.ShiftPressed();
    fx.CompKeyPressed(true);
    fx.KeyPressed(FX_CRUSHER, false, true);
    Check(fx.Selected() == kCompSelected && !fx.IsLatched(FX_CRUSHER),
          "key, SHIFT, the compressor key: it's selected, the key doesn't latch");

    // an FX key takes the knobs back
    fx.KeyPressed(FX_FILTER, true, false);
    fx.KeyPressed(FX_FILTER, false, false);
    fx.KnobTurned(0, 1.f, false);
    Check(fx.Selected() == FX_FILTER && Near(fx.Param(FX_FILTER, 0), filter + .01f) &&
              Near(fx.CompParam(0), .1f),
          "an FX key: the knobs edit it again");

    // a scene recall leaves it as it is
    FxScene scene;
    fx.Snapshot(scene);
    scene.params[FX_FILTER][0] = .9f;
    fx.Recall(scene);
    Check(Near(fx.CompParam(0), .1f) && Near(e.comp[0], .1f), "a recall leaves the compressor alone");

    // set from the card
    fx.SetComp(0, 1.5f);
    Check(fx.CompParam(0) == 1.f && e.comp[0] == 1.f, "SetComp: clamped and sent");
}

static void TestScenes()
{
    FakeEngine e;
    Fx fx;
    Fresh(e, fx);
    FxScene store[kNumSlots] = {};
    Scenes sc;
    sc.Init(store, &fx);

    Check(sc.SlotPressed(1) == Scenes::Slot::REFUSED, "recall an empty slot: refused");

    // save the filter latched, its cutoff turned
    Latch(fx, FX_FILTER);
    fx.KnobTurned(0, 10.f, false);
    const float cutoff = fx.Param(FX_FILTER, 0);
    sc.ModePressed(SceneMode::SAVE);
    Check(!sc.Armed(), "save: not armed before a slot");
    Check(sc.Valid(1) && sc.Valid(4), "save: every slot valid, empty ones too");
    sc.SlotPressed(2);
    Check(sc.Armed() && sc.Selected() == 2, "save: armed on its slot");
    sc.SlotPressed(2);
    Check(!sc.Armed() && sc.Mode() == SceneMode::SAVE, "save: the slot again deselects it");
    sc.SlotPressed(2);
    Check(sc.Confirm() == 2, "save: confirm changes slot 3");
    Check(store[2].used && (store[2].latched >> FX_FILTER & 1) && store[2].params[FX_FILTER][0] == cutoff,
          "save: the slot holds the latch and the knob");
    Check(sc.Active() == 2 && !sc.Edited() && sc.Mode() == SceneMode::NONE,
          "save: active, not edited, the mode ends");

    // change the sound, then recall
    fx.KnobTurned(0, 10.f, false);
    fx.KeyPressed(FX_FILTER, true, false);
    fx.KeyPressed(FX_CRUSHER, true, false);
    Check(sc.Edited(), "a change after saving: edited");
    fx.KeyPressed(FX_FILTER, false, false);
    const int calls = e.param_calls;
    Check(sc.SlotPressed(2) == Scenes::Slot::RECALL, "recall a saved slot");
    sc.Recall(2);
    Check(e.fast_slews == 1, "recall: at the fast slew");
    Check(e.param_calls - calls == 1, "recall: only the changed parameter sent");
    Check(fx.Param(FX_FILTER, 0) == cutoff && fx.IsLatched(FX_FILTER) && e.on[FX_FILTER],
          "recall: the knob and the latch come back");
    Check(e.on[FX_CRUSHER] && !fx.IsLatched(FX_CRUSHER), "recall: a key held stays on");
    Check(!sc.Edited() && sc.Active() == 2, "recall: active, not edited");
    fx.KeyPressed(FX_CRUSHER, false, false);

    // copy 3 to 4, then over the active one
    sc.ModePressed(SceneMode::COPY);
    Check(sc.Valid(2) && !sc.Valid(3), "copy: only saved slots valid as the source");
    Check(sc.SlotPressed(3) == Scenes::Slot::REFUSED, "copy: an empty source refused");
    sc.SlotPressed(2);
    Check(sc.Source() == 2 && !sc.Armed(), "copy: the source, not armed yet");
    Check(!sc.Valid(2) && sc.Valid(3), "copy: any slot but the source valid as the destination");
    sc.SlotPressed(2);
    Check(!sc.Armed() && sc.Source() == kNoScene, "copy: the source again deselects it");
    sc.SlotPressed(2);
    sc.SlotPressed(3);
    Check(sc.SlotPressed(2) == Scenes::Slot::REFUSED && sc.Source() == 2 && sc.Selected() == 3,
          "copy: the source with a destination picked: refused, both stay");
    sc.SlotPressed(3);
    Check(!sc.Armed() && sc.Source() == 2, "copy: the destination again deselects it, the source stays");
    sc.SlotPressed(3);
    Check(sc.Confirm() == 3 && store[3].used && store[3].params[FX_FILTER][0] == cutoff, "copy: 3 to 4");
    Check(!sc.Edited(), "copy elsewhere: still not edited");
    store[1] = store[3];
    store[1].params[FX_FILTER][0] = 0.f;
    sc.ModePressed(SceneMode::COPY);
    sc.SlotPressed(1);
    sc.SlotPressed(2);
    sc.Confirm();
    Check(sc.Edited() && sc.Active() == 2, "copy over the active scene: edited");

    // mode keys
    sc.ModePressed(SceneMode::DELETE);
    sc.ModePressed(SceneMode::DELETE);
    Check(sc.Mode() == SceneMode::NONE, "the mode key again cancels");
    sc.ModePressed(SceneMode::DELETE);
    sc.SlotPressed(2);
    sc.ModePressed(SceneMode::SAVE);
    Check(sc.Mode() == SceneMode::SAVE && sc.Selected() == kNoScene, "another mode key switches, the slot cleared");
    Check(sc.Confirm() == kNoScene, "confirm without a slot: nothing");

    // delete the active one
    sc.ModePressed(SceneMode::SAVE);
    sc.ModePressed(SceneMode::DELETE);
    Check(sc.Valid(2) && !sc.Valid(4), "delete: only saved slots valid");
    sc.SlotPressed(2);
    sc.SlotPressed(2);
    Check(!sc.Armed(), "delete: the slot again deselects it");
    sc.SlotPressed(2);
    Check(sc.Confirm() == 2 && !store[2].used && sc.Active() == kNoScene, "delete the active scene");
    sc.ModePressed(SceneMode::DELETE);
    Check(sc.SlotPressed(2) == Scenes::Slot::REFUSED, "delete: an empty slot refused");
    sc.ModePressed(SceneMode::DELETE);

    // the blank slot: every effect off, every knob on its default
    bool blank = store[kBlankSlot].used && store[kBlankSlot].latched == 0;
    for (size_t f = 0; f < kNumFx; f++)
        for (size_t p = 0; p < kNumFxParams; p++)
            blank = blank && store[kBlankSlot].params[f][p] == kFxParams[f].defaults[p];
    Check(blank, "blank: filled by Init, all off and on the defaults");
    Latch(fx, FX_DELAY);
    fx.KnobTurned(3, 10.f, false);
    Check(sc.SlotPressed(kBlankSlot) == Scenes::Slot::RECALL, "blank: recalled");
    sc.Recall(kBlankSlot);
    bool cleared = sc.Active() == static_cast<int>(kBlankSlot) && !fx.IsLatched(FX_DELAY) && !e.on[FX_DELAY];
    for (size_t f = 0; f < kNumFx; f++)
        for (size_t p = 0; p < kNumFxParams; p++)
            cleared = cleared && fx.Param(f, p) == kFxParams[f].defaults[p];
    Check(cleared, "blank: unlatches everything, the knobs back on their defaults");

    sc.ModePressed(SceneMode::SAVE);
    Check(!sc.Valid(kBlankSlot) && sc.SlotPressed(kBlankSlot) == Scenes::Slot::REFUSED && !sc.Armed(),
          "blank: can't be saved over");
    sc.ModePressed(SceneMode::DELETE);
    Check(!sc.Valid(kBlankSlot) && sc.SlotPressed(kBlankSlot) == Scenes::Slot::REFUSED && !sc.Armed(),
          "blank: can't be deleted");
    sc.ModePressed(SceneMode::COPY);
    Check(sc.Valid(kBlankSlot), "blank: a copy's source");
    sc.SlotPressed(1);
    Check(!sc.Valid(kBlankSlot) && sc.SlotPressed(kBlankSlot) == Scenes::Slot::REFUSED && !sc.Armed(),
          "blank: not a copy's destination");
    sc.SlotPressed(1);
    sc.SlotPressed(kBlankSlot);
    sc.SlotPressed(4);
    Check(sc.Confirm() == 4 && store[4].used && store[4].latched == 0
              && store[4].params[FX_DELAY][3] == kFxParams[FX_DELAY].defaults[3],
          "blank: copied into a slot as a starting point");
}

static void TestMorph()
{
    FakeEngine e;
    Fx fx;
    Fresh(e, fx);
    FxScene store[kNumSlots] = {};
    Scenes sc;
    sc.Init(store, &fx);

    // scene 2: filter, delay and freezer on; scene 3: filter, reverb and slicer on
    for (size_t s = 1; s <= 2; s++)
    {
        store[s] = store[kBlankSlot];
        store[s].latched = static_cast<uint16_t>(1u << FX_FILTER);
    }
    store[1].latched |= (1u << FX_DELAY) | (1u << FX_FREEZER);
    store[1].params[FX_FILTER][0] = .2f;
    store[1].params[FX_DELAY][3] = .6f;
    store[2].latched |= (1u << FX_REVERB) | (1u << FX_SLICER);
    store[2].params[FX_FILTER][0] = .8f;
    store[2].params[FX_FILTER][5] = 0.f;  // stepped: the LFO division
    store[2].params[FX_REVERB][3] = .5f;
    store[2].params[FX_REVERB][0] = .9f;  // decay, not an amount knob
    store[2].params[FX_FLANGER][0] = .3f; // off in both
    store[2].latched |= 1u << FX_FOLDER;  // on, its tone closed: everything fades
    store[2].params[FX_FOLDER][2] = .3f;
    store[1].params[FX_FOLDER][2] = .6f;  // what it had while off
    store[2].params[FX_FOLDER][7] = 1.f;  // its Level at +12dB: faded in too

    Check(sc.SlotPressed(3, true) == Scenes::Slot::REFUSED, "morph to an empty slot: refused");
    sc.ModePressed(SceneMode::SAVE);
    Check(sc.SlotPressed(3, true) == Scenes::Slot::SELECTED && sc.Selected() == 3,
          "SHIFT + scene in a mode: picks, no morph");
    sc.ModePressed(SceneMode::SAVE);

    sc.Recall(1);
    fx.KeyPressed(FX_CRUSHER, true, false); // held through the morph
    Check(sc.SlotPressed(2, true) == Scenes::Slot::MORPH, "SHIFT + a saved scene: morph");
    sc.Morph(2);
    const FxMorphPlan& plan = e.plan;
    Check(e.morphing && sc.Morphing() == 2 && sc.Active() == 2 && !sc.Edited(),
          "morph: running, its scene active and not edited");
    Check(fx.Param(FX_FILTER, 0) == .8f && fx.IsLatched(FX_REVERB) && !fx.IsLatched(FX_DELAY),
          "morph: the controls show the scene at once");
    Check(plan.how[FX_FILTER][0] == MorphParam::GLIDE && plan.start[FX_FILTER][0] == .2f
              && plan.target[FX_FILTER][0] == .8f,
          "on in both: a continuous knob glides from where it is");
    Check(plan.how[FX_FILTER][5] == MorphParam::HOLD && plan.target[FX_FILTER][5] == 0.f,
          "on in both: a stepped one switches at the landing");
    Check(plan.how[FX_DELAY][3] == MorphParam::FADE_OUT && plan.how[FX_DELAY][0] == MorphParam::HOLD
              && (plan.deferred >> FX_DELAY & 1) && !(plan.wake >> FX_DELAY & 1),
          "turned off: its level fades out, the key goes off at the landing");
    Check(plan.how[FX_REVERB][3] == MorphParam::GLIDE && plan.start[FX_REVERB][3] == 0.f
              && plan.how[FX_REVERB][0] == MorphParam::HOLD && plan.start[FX_REVERB][0] == .9f
              && (plan.deferred >> FX_REVERB & 1) && (plan.wake >> FX_REVERB & 1),
          "turned on: its level fades in from silent, the rest jumps now");
    Check(plan.how[FX_FOLDER][7] == MorphParam::GLIDE
              && plan.start[FX_FOLDER][7] == kFxParams[FX_FOLDER].defaults[7]
              && plan.target[FX_FOLDER][7] == 1.f && plan.how[FX_FOLDER][4] == MorphParam::HOLD,
          "turned on: page 2's Level glides from 0dB, Mix doesn't need to");
    Check((plan.deferred >> FX_FREEZER & 1) && (plan.deferred >> FX_SLICER & 1)
              && !(plan.wake >> FX_FREEZER & 1) && !(plan.wake >> FX_SLICER & 1)
              && plan.how[FX_SLICER][1] == MorphParam::HOLD,
          "no amount knob: switched at the landing");
    Check(plan.start[FX_FLANGER][0] == .3f && plan.how[FX_FLANGER][0] == MorphParam::HOLD,
          "off in both: jumps now");
    Check(plan.how[FX_FOLDER][2] == MorphParam::GLIDE && plan.start[FX_FOLDER][2] == 1.f
              && e.params[FX_FOLDER][2] == 1.f,
          "turned on, a folder: its tone too fades in from neutral, sent before the start");
    Check(!(plan.deferred >> FX_CRUSHER & 1) && e.on[FX_CRUSHER], "a held key stays on");

    // more taps on the same key, not on another
    Check(sc.SlotPressed(2, true) == Scenes::Slot::MORPH_MORE && sc.MorphMore() && e.bars == 2,
          "SHIFT + the same key: a bar more");
    while (sc.MorphMore())
        ;
    Check(e.bars == kMaxMorphBars, "... up to 8");
    Check(sc.SlotPressed(1, true) == Scenes::Slot::REFUSED, "SHIFT + another key meanwhile: refused");
    fx.KeyPressed(FX_CRUSHER, false, false);

    // SHIFT + PLAY: stopped where it is, which is neither scene
    Check(sc.FreezeMorph() && !e.morphing && sc.Morphing() == kNoScene, "stopped: it ends");
    Check(sc.Active() == 2 && sc.Edited(), "stopped: its scene active, edited");
    Check(fx.Param(FX_FILTER, 0) == .2f && fx.Param(FX_REVERB, 3) == 0.f,
          "stopped: the knobs show where it got to");
    Check(fx.IsLatched(FX_DELAY) && e.on[FX_DELAY] && !fx.IsLatched(FX_REVERB) && !e.on[FX_REVERB]
              && fx.IsLatched(FX_FILTER),
          "stopped: keys it hadn't switched as they were");
    Check(!sc.FreezeMorph(), "stopped: nothing to stop any more");

    // again, then a plain press: the morph lands, then the recall
    sc.Recall(1);
    sc.Morph(2);
    Check(sc.SlotPressed(1) == Scenes::Slot::RECALL, "a plain key meanwhile: a recall");
    sc.Recall(1);
    Check(!e.morphing && e.landings == 1 && sc.Morphing() == kNoScene && sc.Active() == 1,
          "... which ends the morph first");
}

static void TestTails()
{
    FakeEngine e;
    Fx fx;
    Fresh(e, fx);
    FxScene store[kNumSlots] = {};
    Scenes sc;
    sc.Init(store, &fx);

    // scene 2: the delay latched at level .6, division and feedback turned
    store[1] = store[kBlankSlot];
    store[1].latched = 1u << FX_DELAY;
    store[1].params[FX_DELAY][3] = .6f;
    store[1].params[FX_DELAY][1] = .8f;
    sc.Recall(1);
    Check(e.on[FX_DELAY] && e.params[FX_DELAY][3] == .6f, "tails: the delay on at .6");

    // the blank scene turns it off: its echoes ring out as they were
    sc.Recall(kBlankSlot);
    Check(!e.on[FX_DELAY] && e.params[FX_DELAY][3] == .6f && e.params[FX_DELAY][1] == .8f,
          "recall: a send it turns off rings out, its settings untouched");
    Check(fx.Param(FX_DELAY, 3) == 0.f && !sc.Edited(), "recall: the knobs show the scene");
    fx.KeyPressed(FX_DELAY, true, false);
    Check(e.on[FX_DELAY] && e.params[FX_DELAY][3] == 0.f && e.params[FX_DELAY][1] == .4f,
          "recall: its key on again, it takes the scene's settings");
    fx.KeyPressed(FX_DELAY, false, false);

    // a knob turned while it rings out moves the tail
    sc.Recall(1);
    sc.Recall(kBlankSlot);
    fx.KeyPressed(FX_DELAY, true, false);
    fx.KeyPressed(FX_DELAY, false, false);
    sc.Recall(1);
    sc.Recall(kBlankSlot);
    fx.KnobTurned(3, 10.f, false); // the delay is still the one the knobs edit
    Check(Near(e.params[FX_DELAY][3], .1f), "a knob turned on a send ringing out moves it");

    // a morph between two scenes without the delay leaves its tail alone
    store[2] = store[kBlankSlot];
    store[2].params[FX_DELAY][3] = .3f;
    sc.Recall(1);
    sc.Recall(kBlankSlot);
    sc.Morph(2);
    Check(e.plan.how[FX_DELAY][3] == MorphParam::HOLD && e.params[FX_DELAY][3] == .6f,
          "morph: a send off in both rings out untouched");
    sc.Recall(kBlankSlot);

    // a morph that turns an effect off parks it faded out
    sc.Recall(1);
    sc.Morph(kBlankSlot);
    Check(e.plan.park == (1u << FX_DELAY) && e.plan.how[FX_DELAY][3] == MorphParam::FADE_OUT,
          "morph: the one it turns off is parked at the landing");
    e.LandFxMorph();
    fx.KeyPressed(FX_DELAY, true, false);
    Check(e.params[FX_DELAY][3] == 0.f && e.params[FX_DELAY][1] == .4f,
          "morph: parked, then its key on: the scene's settings first");
    fx.KeyPressed(FX_DELAY, false, false);

    // a latch changed during a morph survives stopping it
    sc.Recall(1);
    sc.Morph(kBlankSlot); // the delay fading out
    Latch(fx, FX_DELAY); // latched again meanwhile
    sc.FreezeMorph();
    Check(fx.IsLatched(FX_DELAY) && e.on[FX_DELAY], "stopped: a latch changed meanwhile stays");
    sc.Recall(1);
    sc.Morph(kBlankSlot);
    fx.KeyPressed(FX_DELAY, true, true); // a select doesn't touch it
    fx.KeyPressed(FX_DELAY, false, true);
    sc.FreezeMorph();
    Check(fx.IsLatched(FX_DELAY) && e.on[FX_DELAY], "stopped untouched: it stays as it was, on");

    // a deleted target stops blinking, and takes no more taps
    sc.Recall(kBlankSlot);
    sc.Morph(1);
    store[1].used = false;
    Check(sc.Morphing() == kNoScene && sc.SlotPressed(1, true) == Scenes::Slot::REFUSED,
          "morph to a deleted slot: no blink, no more bars");
    sc.Recall(kBlankSlot);
}

/** The crossfader (SceneControls::FaderTurned, FaderTo, EndFade) */
static void TestFader()
{
    FakeEngine e;
    Fx fx;
    Fresh(e, fx);
    FxScene store[kNumSlots] = {};
    Scenes sc;
    sc.Init(store, &fx);

    // scene 1: the filter latched at .2, the reverb off at level .3; scene 2: the filter at
    // .8 and the reverb latched at .5
    store[1] = store[2] = store[kBlankSlot];
    store[1].latched = store[2].latched = 1u << FX_FILTER;
    store[1].params[FX_FILTER][0] = .2f;
    store[1].params[FX_REVERB][3] = .3f;
    store[2].params[FX_FILTER][0] = .8f;
    store[2].latched |= 1u << FX_REVERB;
    store[2].params[FX_REVERB][3] = .5f;

    Check(!sc.FaderTurned(3.f) && !sc.FaderTo(.5f) && !sc.EndFade(),
          "fader: nothing without a morph");

    // from scene 1 edited, to scene 2 and back to the start: as it was
    sc.Recall(1);
    fx.KeyPressed(FX_FILTER, true, false);
    fx.KeyPressed(FX_FILTER, false, false);
    Latch(fx, FX_FILTER);
    fx.KnobTurned(0, 10.f, false); // the filter at .3
    Check(sc.Edited() && Near(fx.Param(FX_FILTER, 0), .3f), "fader: scene 1, edited");
    sc.Morph(2);
    Check(sc.FaderTurned(3.f) && sc.Fading() && Near(sc.FaderPos(), 3.f / 24.f)
              && Near(e.fader, 3.f / 24.f) && sc.FadeFrom() == 1 && sc.Morphing() == 2,
          "fader: SHIFT + turn on a held morph takes it, 24 detents from end to end");
    sc.FaderTurned(-10.f);
    Check(sc.FaderPos() == 0.f, "fader: it stops at the start");
    Check(sc.EndFade() && !e.morphing && !sc.Fading() && sc.Active() == 1 && sc.Edited(),
          "fader: let go at the start, the morph ends and scene 1 is back, edited as it was");
    Check(Near(fx.Param(FX_FILTER, 0), .3f) && fx.IsLatched(FX_FILTER)
              && !fx.IsLatched(FX_REVERB) && !e.on[FX_REVERB] && fx.Param(FX_REVERB, 3) == .3f,
          "fader: ... its knobs and latches too, the reverb off at its level");
    Check(!sc.EndFade(), "fader: and only once");

    // let go in between: stopped where it is, the scene it went to edited
    sc.Recall(1);
    sc.Morph(2);
    sc.FaderTurned(12.f);
    Check(sc.EndFade() && !e.morphing && sc.Active() == 2 && sc.Edited(),
          "fader: let go in the middle, it stays there, scene 2 edited");

    // let go at the end: landed, as recalled
    sc.Recall(1);
    sc.Morph(2);
    sc.FaderTurned(30.f);
    const int landings = e.landings;
    Check(sc.FaderPos() == 1.f && sc.EndFade() && e.landings == landings + 1 && sc.Active() == 2
              && !sc.Edited() && fx.IsLatched(FX_REVERB),
          "fader: let go at the end, it lands on scene 2");

    // a morph SHIFT has let go of glides on its own: SHIFT + turn is the speed again
    sc.Recall(1);
    sc.Morph(2);
    e.held = false;
    Check(!sc.FaderTurned(3.f) && !sc.Fading(), "fader: not on a morph already gliding");
    sc.Recall(1);

    // over MIDI: any running morph; the ends decide
    sc.Morph(2);
    e.held = false;
    Check(sc.FaderTo(0.f) && sc.Fading(), "fader midi: takes a gliding morph, at the start it waits");
    sc.FaderTo(.5f);
    Check(sc.Fading() && Near(e.fader, .5f), "fader midi: in between");
    Check(sc.FaderTo(0.f) && !sc.Fading() && !e.morphing && sc.Active() == 1,
          "fader midi: back at the start after being away, scene 1 again");
    sc.Morph(2);
    Check(sc.FaderTo(1.f) && !e.morphing && sc.Active() == 2 && !sc.Edited(),
          "fader midi: at the end, landed");

    // the start deleted meanwhile: no scene active
    sc.Recall(1);
    sc.Morph(2);
    sc.FaderTurned(1.f);
    store[1].used = false;
    sc.FaderTurned(-1.f);
    Check(sc.EndFade() && sc.Active() == kNoScene, "fader: back to a deleted scene, none active");
    store[1].used = true;
}

/** The chaos key's pool (FxControls::Pool): the effects latched and not held */
static void TestPool()
{
    FakeEngine e;
    Fx fx;
    Fresh(e, fx);
    Latch(fx, FX_FILTER);
    Latch(fx, FX_DELAY);
    Check(e.pool == ((1u << FX_FILTER) | (1u << FX_DELAY)), "pool: the effects latched");
    Latch(fx, FX_CHAOS);
    Check(e.pool == ((1u << FX_FILTER) | (1u << FX_DELAY)), "pool: never the chaos key itself");
    fx.KeyPressed(FX_FILTER, true, false);
    Check(e.pool == (1u << FX_DELAY), "pool: a latched key held is the hand's, out of it");
    fx.KeyPressed(FX_FILTER, false, false); // a plain press: unlatched
    Check(e.pool == (1u << FX_DELAY), "pool: unlatched, out of it");
    fx.KeyPressed(FX_CRUSHER, true, false);
    Check(e.pool == (1u << FX_DELAY), "pool: a key held, not latched, isn't in it");
    fx.KeyPressed(FX_CRUSHER, false, false);
    fx.SetLatch(FX_SLICER, true);
    Check(e.pool == ((1u << FX_DELAY) | (1u << FX_SLICER)), "pool: a latch over MIDI");

    // a recall and a morph send the scene's, and so does the crossfader going back
    FxScene store[kNumSlots] = {};
    Scenes sc;
    sc.Init(store, &fx);
    store[1] = store[kBlankSlot];
    store[1].latched = (1u << FX_FOLDER) | (1u << FX_CHAOS);
    sc.Recall(1);
    Check(e.pool == (1u << FX_FOLDER) && e.on[FX_CHAOS], "pool: a recall's, the chaos key latched in it on");
    sc.Morph(kBlankSlot);
    Check(e.pool == 0, "pool: a morph's, at once");
    sc.EndFade();
    sc.Recall(1);
    sc.Morph(kBlankSlot);
    sc.FaderTurned(-1.f);
    sc.EndFade();
    Check(e.pool == (1u << FX_FOLDER), "pool: the crossfader let go at the start, the start's again");
}

/** The phaser's rate (#68), a split knob (FxParams::split): free below the centre, 1% a
 *  detent; the tempo's divisions from it up, a step per 3 detents; SHIFT on its own points
 *  across both; MIDI on the nearest division in the upper half; a morph holds it across */
static void TestSplit()
{
    FakeEngine e;
    Fx fx;
    Fresh(e, fx);
    const size_t kRate = chompi::Phaser::RATE;
    const float kDiv = .5f / (chompi::Phaser::kNumDivisions - 1); // a division's width on the knob
    auto rate = [&fx, kRate] { return fx.Param(FX_PHASER, kRate); };
    fx.KeyPressed(FX_PHASER, true, false);
    Check(rate() == .25f && !SplitUpper(rate()), "split: the phaser's rate starts free, at .25");
    for (int i = 0; i < 24; i++)
        fx.KnobTurned(0, 1.f, false);
    Check(Near(rate(), .49f), "split: fine turns in the free half, 1% a detent");
    fx.KnobTurned(0, 1.f, false);
    Check(rate() == kSplitKnob, "split: the next detent lands on the first division, the centre");
    fx.KnobTurned(0, 2.f, false);
    Check(rate() == kSplitKnob, "split: in the synced half, 2 detents do nothing yet");
    fx.KnobTurned(0, 1.f, false);
    Check(Near(rate(), .5f + kDiv), "split: the 3rd moves a division, 1/8");
    fx.KnobTurned(0, -3.f, false);
    fx.KnobTurned(0, -3.f, false);
    Check(Near(rate(), .49f), "split: 3 detents down from the first division: the top of the free half");
    fx.KnobTurned(0, 10.f, false);
    Check(rate() == kSplitKnob, "split: a fast turn up out of the free half stops on the first division");
    for (int i = 0; i < 20; i++)
        fx.KnobTurned(0, 3.f, false);
    Check(rate() == 1.f, "split: and steps up to 4 bars at the top");

    // SHIFT: the free half's 10%, then each division
    fx.KnobPressed(0, true); // SHIFT + press: its default
    fx.KnobTurned(0, 1.f, true);
    Check(Near(rate(), .3f), "split: SHIFT + turn in the free half: the next 10%");
    fx.KnobTurned(0, 2.f, true);
    Check(rate() == kSplitKnob, "split: SHIFT + turn across the centre: on the first division");
    fx.KnobTurned(0, 2.f, true);
    Check(Near(rate(), .5f + 2.f * kDiv), "split: SHIFT + turn in the synced half: a division a detent");
    fx.KnobTurned(0, -3.f, true);
    Check(Near(rate(), .4f), "split: and back down across the centre onto the free half's 10%");

    // MIDI: the upper half on its nearest division, the lower as sent
    fx.SetParamTo(FX_PHASER, kRate, .62f);
    Check(Near(rate(), .5f + kDiv), "split: MIDI in the synced half: the nearest division");
    fx.SetParamTo(FX_PHASER, kRate, .5f);
    Check(rate() == kSplitKnob, "split: MIDI's centre (64, 8192) is the first division");
    fx.SetParamTo(FX_PHASER, kRate, .4999f);
    Check(rate() == .4999f, "split: MIDI just below it: free, as sent");

    // a morph glides it within the free half, and holds it from one half to the other
    FxScene store[kNumSlots] = {};
    Scenes sc;
    sc.Init(store, &fx);
    for (size_t s = 1; s <= 3; s++)
    {
        store[s] = store[kBlankSlot];
        store[s].latched = 1u << FX_PHASER;
    }
    store[1].params[FX_PHASER][kRate] = .2f;
    store[2].params[FX_PHASER][kRate] = .4f;
    store[3].params[FX_PHASER][kRate] = .5f + 3.f * kDiv;
    sc.Recall(1);
    sc.Morph(2);
    Check(e.plan.how[FX_PHASER][kRate] == MorphParam::GLIDE, "split: a morph within the free half glides");
    e.LandFxMorph();
    sc.Recall(1);
    sc.Morph(3);
    Check(e.plan.how[FX_PHASER][kRate] == MorphParam::HOLD, "split: a morph into the synced half holds");
}

int main()
{
    TestInit();
    TestKeys();
    TestKnobs();
    TestPages();
    TestComp();
    TestScenes();
    TestMorph();
    TestTails();
    TestFader();
    TestPool();
    TestSplit();
    return Finish();
}
