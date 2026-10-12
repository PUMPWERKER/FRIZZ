// keys.cpp: checks the play page's CHOMPI, PLAY and LOOP keys (PlayKeys.h) against a fake
// host that records what they do. Exits 0 when everything passes. Run by unit.sh keys.
#include <cstdio>
#include "check.h"
#include "PlayKeys.h"

using namespace chompi;

struct FakeHost
{
    bool armed = false;
    bool morphing = false;
    bool fx_held = false; // an FX key held, so SHIFT going down queues a latch
    bool can_quantize = true;
    Looper::State state = Looper::State::EMPTY;
    uint32_t now = 0;
    int confirms = 0, shift_presses = 0, shift_uses = 0, freezes = 0, taps = 0, refusals = 0;
    int toggles = 0, records = 0, quantized = 0, stops = 0;
    int erases = 0, erases_at_end = 0, cancels = 0;
    bool erase_pending = false;

    bool SceneArmed() const { return armed; }
    void ConfirmScene()
    {
        confirms++;
        armed = false;
    }
    bool ShiftPressed()
    {
        shift_presses++;
        return fx_held;
    }
    void ShiftUsed() { shift_uses++; }
    bool drop = false; // a scene key tapped in this SHIFT hold: PLAY drops to it
    int drops = 0;
    bool DropScene()
    {
        if (!drop)
            return false;
        drop = false;
        drops++;
        return true;
    }
    bool FreezeMorph()
    {
        if (!morphing)
            return false;
        morphing = false;
        freezes++;
        return true;
    }
    void Tap() { taps++; }
    void Refused() { refusals++; }
    uint32_t Now() const { return now; }
    Looper::State LooperState() const { return state; }
    bool CanRecordQuantized() const { return can_quantize; }
    void StartRecording(bool q)
    {
        records++;
        quantized += q;
        state = Looper::State::RECORDING;
    }
    void StopRecording()
    {
        stops++;
        state = Looper::State::PLAYING;
    }
    void TogglePlay() { toggles++; }
    void Erase()
    {
        erases++;
        erase_pending = false;
        state = Looper::State::EMPTY;
    }
    void EraseAtEnd()
    {
        erases_at_end++;
        erase_pending = true;
    }
    void CancelErase()
    {
        cancels++;
        erase_pending = false;
    }
    bool ErasePending() const { return erase_pending; }
};

using Keys = PlayKeys<FakeHost>;

static void Fresh(FakeHost& h, Keys& k)
{
    h = FakeHost();
    k = Keys();
    k.Init(&h);
}

static void TestChompi()
{
    FakeHost h;
    Keys k;
    Fresh(h, k);

    k.Chompi(true);
    Check(k.Shift() && h.shift_presses == 1, "CHOMPI held: SHIFT");
    k.Chompi(false);
    Check(!k.Shift() && h.confirms == 0, "CHOMPI tap without a mode: nothing to confirm");

    h.armed = true;
    k.Chompi(true);
    Check(h.confirms == 0, "armed: no confirm on the press");
    k.Chompi(false);
    Check(h.confirms == 1 && h.shift_uses == 1, "armed: a tap confirms on release, not as SHIFT for held FX");

    h.armed = true;
    k.Chompi(true);
    k.Used(); // a coarse turn, say
    Check(k.ShiftCombo(), "armed, held + a knob: a SHIFT combo");
    k.Chompi(false);
    Check(h.confirms == 1 && h.armed, "armed, held + a knob: no confirm, still armed");

    k.Chompi(true);
    k.FxKey();
    k.Chompi(false);
    Check(h.confirms == 1, "armed, held + an FX key (a select): no confirm");

    h.fx_held = true;
    k.Chompi(true);
    Check(k.ShiftCombo(), "armed, an FX key held, then CHOMPI (a latch): a SHIFT combo");
    k.Chompi(false);
    Check(h.confirms == 1 && h.armed, "... no confirm, still armed");
    h.fx_held = false;

    k.Chompi(false);
    Check(h.confirms == 1, "a CHOMPI release without its press: nothing");
}

static void TestPlayLoop()
{
    FakeHost h;
    Keys k;
    Fresh(h, k);

    k.Loop(true);
    Check(h.records == 1 && h.quantized == 0, "LOOP on empty: records at once");
    k.Loop(false);
    k.Loop(true);
    Check(h.stops == 1, "LOOP again: stops, plays");
    k.Loop(false);

    k.Play(true);
    Check(h.toggles == 0, "PLAY: nothing on the press");
    k.Play(false);
    Check(h.toggles == 1, "PLAY: play / pause on release");
    k.Play(false);
    Check(h.toggles == 1, "a PLAY release without its press: nothing");

    // LOOP on a loop: erases at once, but not just after it stopped a recording
    k.Loop(true);
    Check(h.erases == 0, "LOOP right after the stop: no erase");
    k.Loop(false);
    h.now = Keys::kEraseLockMs - 1;
    k.Loop(true);
    Check(h.erases == 0, "LOOP within the lock: no erase");
    k.Loop(false);
    h.now = Keys::kEraseLockMs;
    k.Loop(true);
    Check(h.erases == 1 && h.state == Looper::State::EMPTY, "LOOP on a loop: erases at once");
    k.Loop(false);

    // PLAY held + LOOP on a loop: erases at its end, and PLAY's release doesn't toggle
    h.state = Looper::State::PLAYING;
    k.Play(true);
    k.Loop(true);
    Check(h.erases_at_end == 1 && h.erases == 1, "PLAY + LOOP on a loop: erase at its end");
    k.Loop(false);
    k.Play(false);
    Check(h.toggles == 1 && h.cancels == 0, "... PLAY's release neither toggles nor cancels");

    // waiting: PLAY alone takes it back, without a toggle
    k.Play(true);
    k.Play(false);
    Check(h.cancels == 1 && h.toggles == 1 && !h.erase_pending, "waiting: PLAY cancels, no toggle");

    // waiting: LOOP erases at once, with PLAY held or not
    k.Play(true);
    k.Loop(true);
    k.Loop(false);
    k.Play(false);
    Check(h.erases_at_end == 2, "PLAY + LOOP again: waits again");
    k.Loop(true);
    Check(h.erases == 2 && !h.erase_pending, "waiting: LOOP erases at once");
    k.Loop(false);
    h.state = Looper::State::PLAYING;
    k.Play(true);
    k.Loop(true);
    k.Loop(false);
    k.Loop(true);
    Check(h.erases == 3 && h.erases_at_end == 3, "waiting, PLAY held: LOOP erases at once");
    k.Loop(false);
    k.Play(false);
    Check(h.toggles == 1 && h.cancels == 1, "... PLAY's release does nothing");

    // LOOP first erases; PLAY after it is nothing more
    h.state = Looper::State::PLAYING;
    k.Loop(true);
    k.Play(true);
    k.Play(false);
    k.Loop(false);
    Check(h.erases == 4 && h.erases_at_end == 3 && h.toggles == 2,
          "LOOP, then PLAY: one erase; PLAY is PLAY, on the empty looper");

    // quantized record: PLAY held, LOOP on an empty looper
    h.state = Looper::State::EMPTY;
    k.Play(true);
    k.Loop(true);
    Check(h.quantized == 1, "PLAY + LOOP on empty: quantized record");
    k.Loop(false);
    k.Play(false);
    Check(h.toggles == 2, "... and PLAY's release doesn't toggle");
    h.state = Looper::State::EMPTY;
    h.can_quantize = false;
    k.Play(true);
    k.Loop(true);
    Check(h.refusals == 1 && h.records == 2, "quantized record without a clock: refused");
    k.Loop(false);
    k.Play(false);
}

static void TestShiftCombos()
{
    FakeHost h;
    Keys k;
    Fresh(h, k);
    h.state = Looper::State::PLAYING;

    // SHIFT + LOOP: a tap, nothing else
    k.Chompi(true);
    k.Loop(true);
    k.Loop(false);
    Check(h.taps == 1 && h.records == 0 && h.stops == 0 && h.shift_uses == 1,
          "SHIFT + LOOP: tap tempo only, and SHIFT used");
    k.Play(true);
    k.Loop(true);
    k.Loop(false);
    k.Play(false);
    Check(h.taps == 2 && h.toggles == 0, "PLAY held + SHIFT + LOOP: no toggle on PLAY's release");

    // SHIFT + PLAY: stops a morph; without one, play / pause
    h.morphing = true;
    k.Play(true);
    k.Play(false);
    Check(h.freezes == 1 && h.toggles == 0, "SHIFT + PLAY in a morph: stopped, no toggle");
    k.Play(true);
    k.Play(false);
    Check(h.freezes == 1 && h.toggles == 1, "SHIFT + PLAY without one: play / pause");

    // #153: a scene key tapped in this SHIFT hold makes PLAY a drop, rather than a stop, and
    // PLAY's release doesn't toggle
    h.morphing = h.drop = true;
    k.Play(true);
    k.Play(false);
    Check(h.drops == 1 && h.freezes == 1 && h.morphing && h.toggles == 1,
          "SHIFT + scene key + PLAY: a drop, not a stop, no toggle");
    k.Play(true);
    k.Play(false);
    Check(h.drops == 1 && h.freezes == 2 && h.toggles == 1,
          "SHIFT + PLAY again: no scene tapped since, it stops the drop");
    k.Chompi(false);
}

int main()
{
    TestChompi();
    TestPlayLoop();
    TestShiftCombos();
    return Finish();
}
