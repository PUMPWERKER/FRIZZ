/** @file PlayKeys.h
 *  @brief What the play page does with the CHOMPI, PLAY and LOOP keys, without the hardware.
 *  NormalPage.h routes the keys here; test/keys.cpp runs it on the host.
 *
 *  CHOMPI held is SHIFT. In a scene mode with a slot picked (SceneArmed), a tap of it
 *  confirms: on its release, and only if nothing else was pressed or turned while it was
 *  held, so holding it still works as SHIFT there. An FX key held, then SHIFT, latches it, and
 *  SHIFT, then an FX key, selects it (FxControls.h): both are SHIFT combos, so the release
 *  doesn't confirm. SHIFT + anything else is that combo, and a latch the SHIFT press had
 *  queued for a held FX key is dropped (ShiftUsed).
 *
 *  LOOP acts on press, so recording starts, stops and erases exactly then. The gestures that
 *  record on an empty looper erase a loop: LOOP at once, PLAY held + LOOP at the loop's end
 *  (quantized). LOOP while that waits erases at once; PLAY alone takes it back. For
 *  kEraseLockMs after LOOP stopped a recording, LOOP doesn't erase, so a double press or a
 *  bounce can't lose the new loop. PLAY acts on release, and only if nothing combined with it
 *  during the hold, so the PLAY + LOOP combos never also toggle play. SHIFT + LOOP is tap
 *  tempo and only that; SHIFT + PLAY stops a scene morph, or without one is PLAY. A release
 *  whose press wasn't seen (a key held through the boot animation) does nothing.
 *
 *  Host is NormalPage on the device, a fake on the host. It needs: SceneArmed(),
 *  ConfirmScene(), ShiftPressed() (true if it queued a latch) and ShiftUsed() (FxControls'),
 *  FreezeMorph() (true if one was stopped), Tap(), Refused() (LOOP can't do it), Now() in ms,
 *  and the looper's LooperState(), CanRecordQuantized(), StartRecording(quantized),
 *  StopRecording(), TogglePlay(), Erase(), EraseAtEnd(), CancelErase(), ErasePending().
 */
#pragma once
#include <stdint.h>
#include "Looper.h"

// the play page's logic, a key or a frame now and then and never per sample: built for size,
// as FRIZZ's code space is tight (SRAM_EXEC, docs/CAPACITY.md)
#pragma GCC push_options
#pragma GCC optimize("Os")

namespace chompi
{

template <class Host>
class PlayKeys
{
public:
    void Init(Host* host) { host_ = host; }

    inline bool Shift() const { return chompi_down_; }
    /** CHOMPI is held and has been used as SHIFT, so its release won't confirm */
    inline bool ShiftCombo() const { return chompi_down_ && combo_; }

    void Chompi(bool down)
    {
        if (down)
        {
            chompi_down_ = true;
            combo_ = host_->ShiftPressed(); // an FX key held: a latch, not a confirm
            return;
        }
        if (!chompi_down_)
            return;
        chompi_down_ = false;
        if (!combo_ && host_->SceneArmed())
        {
            host_->ShiftUsed(); // a confirm, not SHIFT for the FX keys held
            host_->ConfirmScene();
        }
    }

    /** Any other key or knob going down or turning, but an FX key: with CHOMPI held, that's a
     *  SHIFT combo */
    void Used()
    {
        if (!chompi_down_)
            return;
        combo_ = true;
        host_->ShiftUsed();
    }

    /** An FX key going down: with CHOMPI held, a select, so CHOMPI won't confirm */
    void FxKey()
    {
        if (chompi_down_)
            combo_ = true;
    }

    void Play(bool down)
    {
        if (down)
        {
            Used();
            play_down_ = true;
            // SHIFT + PLAY: stops a morph, and only that; without one, PLAY as ever
            play_combo_ = Shift() && host_->FreezeMorph();
            return;
        }
        if (!play_down_)
            return;
        play_down_ = false;
        if (play_combo_)
            return;
        if (host_->ErasePending())
            host_->CancelErase(); // and only that: the loop plays on
        else
            host_->TogglePlay();
    }

    void Loop(bool down)
    {
        if (down)
        {
            Used();
            // SHIFT + LOOP: tap tempo; it doesn't count as LOOP held for PLAY's combos, nor
            // lets a held PLAY toggle on release
            if (Shift())
            {
                if (play_down_)
                    play_combo_ = true;
                host_->Tap();
                return;
            }
            LoopPressed();
        }
    }

    static const uint32_t kEraseLockMs = 500;

private:
    void LoopPressed()
    {
        if (play_down_)
            play_combo_ = true; // PLAY's release must not toggle

        switch (host_->LooperState())
        {
        case Looper::State::EMPTY:
            if (!play_down_)
                host_->StartRecording(false);
            else if (host_->CanRecordQuantized())
                host_->StartRecording(true);
            else
                host_->Refused();
            break;

        case Looper::State::RECORDING:
            host_->StopRecording();
            stopped_ = true;
            stop_time_ = host_->Now();
            break;

        case Looper::State::PLAYING:
        case Looper::State::PAUSED:
            if (stopped_ && host_->Now() - stop_time_ < kEraseLockMs)
                break;
            stopped_ = false; // the lock is over, also once the ms count wraps
            if (play_down_ && !host_->ErasePending())
                host_->EraseAtEnd();
            else
                host_->Erase();
            break;
        }
    }

    Host* host_ = nullptr;
    bool chompi_down_ = false;
    bool combo_ = false;      // something used with CHOMPI during this hold
    bool play_down_ = false;
    bool play_combo_ = false; // something combined with this PLAY hold: no toggle
    bool stopped_ = false;   // LOOP has stopped a recording, at stop_time_
    uint32_t stop_time_ = 0;
};

} // namespace chompi

#pragma GCC pop_options
