/** @file SceneControls.h
 *  @brief What the play page does with the FX scene keys, without the hardware: recalling a
 *  slot, the timed sequences of a SHIFT hold (#175, below), and TAPE's and TEMPO's SAVE / COPY
 *  / DELETE flow (pick the mode, the slot(s), then confirm). Like a mode key, a slot tapped
 *  again is deselected. NormalPage.h routes the keys here, writes the card (SceneStore.h) and
 *  draws the LEDs; test/controls.cpp runs it on the host.
 *
 *  The sequences (#175): SHIFT + a scene key A, tapped n times (n bars, up to kMaxMorphBars),
 *  starts one, held (FxMorph.h) until SHIFT goes up; the bars count from then. With SHIFT
 *  still held, what comes first decides it for the whole hold (Hold):
 *   - nothing: a morph, gliding over the n bars;
 *   - the transport pressed: a drop, A all at once on the n-th bar line;
 *   - the transport turned: the crossfader, once kFaderDetents went one way and the second of
 *     them is kFaderGuardMs old with no press meanwhile (a press, or a turn back, wins);
 *   - a second scene key B: a build-up, the morph to A over the n bars and B dropping hard
 *     on its end.
 *  Further taps of A add bars; nothing else does anything in that hold. One sequence at a
 *  time: in a later hold, SHIFT + a blinking scene key (A, or the one it drops to) stops it
 *  where it is, and SHIFT + another starts a new one from there. A plain scene key recalls at
 *  once, ending any. MIDI's morphs and drops (CC 3, CC 83: Remote) follow the same rules.
 *
 *  The first slot (kBlankSlot) is the blank scene: every effect off, every knob on its
 *  default. It can be recalled and copied from, but not saved over, copied over or deleted.
 */
#pragma once
#include "FxControls.h"

// the play page's logic, a key or a frame now and then and never per sample: built for size,
// as FRIZZ's code space is tight (SRAM_EXEC, docs/CAPACITY.md)
#pragma GCC push_options
#pragma GCC optimize("Os")

namespace chompi
{

enum class SceneMode
{
    NONE,
    SAVE,
    COPY,
    DELETE,
};

static const int kNoScene = -1;
// The crossfader on the transport knob (#66): a detent's way, 24 from end to end
static const float kFaderStep = 1.f / 24.f;
// It takes over only after this many detents one way (#175), the last of them this old with no
// transport press meanwhile: a turn while pressing for a drop isn't a crossfade
static const int kFaderDetents = 2;
static const uint32_t kFaderGuardMs = 150;

template <class Engine>
class SceneControls
{
public:
    /** What pressing a slot did */
    enum class Slot
    {
        SELECTED,   // picked for the mode or deselected, or nothing to do
        RECALL,     // recall it: Recall(slot), with the audio interrupt blocked on the device
        MORPH,      // a new sequence to it, held: Morph(slot), likewise
        MORPH_MORE, // A again in its hold: MorphMore(), likewise
        BUILD_UP,   // B in A's hold: a build-up, dropping to it at the morph's end
        CANCEL,     // a blinking scene in a later hold: the sequence stopped where it is
        REFUSED,    // a slot the mode can't act on: empty where a saved one is needed, or
                    // blank; or a bar more than kMaxMorphBars
    };

    void Init(FxScene* scenes, FxControls<Engine>* fx)
    {
        scenes_ = scenes;
        fx_ = fx;

        FxScene& blank = scenes_[kBlankSlot];
        blank.used = true;
        blank.latched = 0;
        for (size_t f = 0; f < kNumFx; f++)
            for (size_t p = 0; p < kNumFxParams; p++)
                blank.params[f][p] = kFxParams[f].defaults[p];

        mode_ = SceneMode::NONE;
        sel_ = src_ = active_ = morph_ = kNoScene;
    }

    /** Out of a mode, nothing picked: the settings page shown (NormalPage.h) */
    void Cancel()
    {
        mode_ = SceneMode::NONE;
        sel_ = src_ = kNoScene;
    }

    /** A mode key: the same one again cancels, another switches over */
    void ModePressed(SceneMode mode)
    {
        mode_ = mode_ == mode ? SceneMode::NONE : mode;
        sel_ = src_ = kNoScene;
    }

    /** Out of a SHIFT hold: what a press would do, a recall, or with shift a sequence (a hold's
     *  further keys: Press). In a mode, SHIFT makes no difference */
    Slot SlotPressed(size_t slot, bool shift = false)
    {
        const int s = static_cast<int>(slot);
        if (mode_ == SceneMode::NONE)
        {
            if (!scenes_[slot].used)
                return Slot::REFUSED;
            return shift ? Slot::MORPH : Slot::RECALL;
        }

        // COPY's source again: back to picking the source, once no destination is picked;
        // with one picked, the destination has to go first
        if (mode_ == SceneMode::COPY && s == src_)
        {
            if (sel_ != kNoScene)
                return Slot::REFUSED;
            src_ = kNoScene;
            return Slot::SELECTED;
        }
        if (!Valid(slot))
            return Slot::REFUSED;

        if (mode_ == SceneMode::COPY && src_ == kNoScene)
            src_ = s; // the source first, then the destination
        else
            sel_ = s == sel_ ? kNoScene : s;
        return Slot::SELECTED;
    }

    /** A scene key, all of it: the recall, or with SHIFT what it does in the hold (file
     *  comment), or the mode's pick. On the device, call it with the audio interrupt blocked,
     *  so a recall lands within one block */
    Slot Press(size_t slot, bool shift)
    {
        if (mode_ != SceneMode::NONE || !shift)
        {
            const Slot result = SlotPressed(slot, shift);
            if (result == Slot::RECALL)
                Recall(slot);
            return result;
        }

        const int s = static_cast<int>(slot);
        const bool runs = fx_->Morphing();
        // ended meanwhile (a program change's recall): the hold starts afresh
        if (hold_ != Hold::NONE && hold_ != Hold::SPENT && !runs)
            hold_ = Hold::NONE;
        switch (hold_)
        {
        case Hold::NONE:
            if (runs && (s == morph_ || s == drop_slot_))
            {
                fx_->FreezeMorph();
                hold_ = Hold::SPENT;
                return Slot::CANCEL;
            }
            if (!scenes_[slot].used)
                return Slot::REFUSED;
            // from where the one running got to
            if (runs)
                fx_->FreezeMorph();
            Morph(slot);
            hold_ = Hold::HELD;
            return Slot::MORPH;
        case Hold::HELD:
        case Hold::DROP:
        case Hold::BUILD:
            if (s == morph_)
                return MorphMore() ? Slot::MORPH_MORE : Slot::REFUSED;
            if (hold_ != Hold::HELD)
                return Slot::SELECTED;
            if (!scenes_[slot].used || !fx_->Drop(&scenes_[slot], true))
                return Slot::REFUSED;
            drop_slot_ = active_ = s;
            hold_ = Hold::BUILD;
            pend_ = 0;
            return Slot::BUILD_UP;
        case Hold::FADER:
            return s == morph_ ? Slot::REFUSED : Slot::SELECTED;
        case Hold::SPENT:
            break;
        }
        return Slot::SELECTED;
    }

    /** SHIFT went down: a new hold */
    inline void ShiftDown()
    {
        hold_ = Hold::NONE;
        pend_ = 0;
    }

    /** SHIFT let go: true if the hand's crossfader had the morph (then EndFade), else the
     *  held sequence is released (FxMorph::Release) */
    inline bool ShiftUp()
    {
        const bool fader = hold_ == Hold::FADER;
        ShiftDown();
        return fader;
    }

    /** The transport pressed with SHIFT held: a drop, if the hold's morph has nothing else yet.
     *  False if it did nothing. On the device, with the audio interrupt blocked */
    bool TransportPressed()
    {
        if (hold_ != Hold::HELD || !fx_->MorphHeld() || !fx_->Drop(nullptr, false))
            return false;
        drop_slot_ = morph_;
        hold_ = Hold::DROP;
        pend_ = 0;
        return true;
    }

    /** The transport turned with SHIFT held, detents (signed), now in ms: the crossfader's, or
     *  counted towards it (file comment), or nothing while the hold has a sequence of another
     *  kind. False if not the scenes', so the knob turns the speed. On the device, with the
     *  audio interrupt blocked */
    bool TransportTurned(int detents, uint32_t now)
    {
        // the hand's, or MIDI's (CC 80), which the hand takes over
        if (hold_ == Hold::FADER || (hold_ == Hold::NONE && fx_->Fading()))
        {
            if (!FaderTurned(static_cast<float>(detents)))
                return false;
            hold_ = Hold::FADER;
            return true;
        }
        if (hold_ == Hold::HELD && fx_->MorphHeld())
        {
            if ((detents > 0) != (pend_ > 0))
                pend_ = 0; // a turn back starts the count again
            const bool was = pend_ >= kFaderDetents || pend_ <= -kFaderDetents;
            pend_ += detents;
            if (!was && (pend_ >= kFaderDetents || pend_ <= -kFaderDetents))
                pend_at_ = now;
            return true;
        }
        return hold_ == Hold::HELD || hold_ == Hold::DROP || hold_ == Hold::BUILD;
    }

    /** Detents counted for the crossfader and waiting for kFaderGuardMs: call Tick */
    inline bool FaderWaiting() const { return pend_ >= kFaderDetents || pend_ <= -kFaderDetents; }

    /** Now and then, now in ms: the crossfader takes over once the detents counted for it are
     *  old enough, where they put it. True if it did. On the device, with the audio interrupt
     *  blocked */
    bool Tick(uint32_t now)
    {
        if (!FaderWaiting() || now - pend_at_ < kFaderGuardMs)
            return false;
        const int detents = pend_;
        pend_ = 0;
        if (hold_ != Hold::HELD || !FaderTurned(static_cast<float>(detents)))
            return false;
        hold_ = Hold::FADER;
        return true;
    }

    /** A morph (CC 3) or with drop a drop (CC 83) over MIDI, over bars bar lines: as SHIFT +
     *  the key tapped bars times, then the transport pressed for a drop; held until SHIFT goes
     *  up (FxMorph::Release, by the caller unless the hand holds SHIFT). A sequence running is
     *  stopped where it is first. False in a mode or for an empty slot. On the device, with
     *  the audio interrupt blocked */
    bool Remote(size_t slot, uint32_t bars, bool drop)
    {
        if (mode_ != SceneMode::NONE || !scenes_[slot].used)
            return false;
        if (fx_->Morphing())
            fx_->FreezeMorph();
        Morph(slot);
        for (uint32_t bar = 1; bar < bars; bar++)
            MorphMore();
        if (drop && fx_->Drop(nullptr, false))
            drop_slot_ = morph_;
        // a hold of the hand's has none of its own now
        if (hold_ != Hold::NONE)
            hold_ = Hold::SPENT;
        pend_ = 0;
        return true;
    }

    /** Recalls a used slot, which becomes the active scene */
    void Recall(size_t slot)
    {
        fx_->Recall(scenes_[slot]);
        active_ = static_cast<int>(slot);
    }

    /** Morphs to a used slot, held, which becomes the active scene now */
    void Morph(size_t slot)
    {
        fx_->Morph(scenes_[slot]);
        from_ = active_;
        active_ = morph_ = static_cast<int>(slot);
        drop_slot_ = kNoScene;
    }

    /** The crossfader turned, detents (signed): while a morph waits for SHIFT to be let go or
     *  is on the crossfader already (#66). False if not. On the device, with the audio
     *  interrupt blocked */
    bool FaderTurned(float detents)
    {
        if (!fx_->Fading() && !fx_->MorphHeld())
            return false;
        const float from = fx_->Fading() ? fx_->FaderPos() : 0.f;
        const float t = from + detents * kFaderStep;
        fx_->Fade(t < 0.f ? 0.f : (t > 1.f ? 1.f : t));
        return true;
    }

    /** A fader over MIDI: a running morph, held or gliding, to t; not a drop. Reaching an end decides as
     *  letting go there would (EndFade): the scene's at once, the start's once it has been
     *  away from it. False if no morph runs. On the device, with the audio interrupt blocked */
    bool FaderTo(float t)
    {
        if (!fx_->Morphing() || fx_->Dropping())
            return false;
        if (!fx_->Fading())
            fader_left_ = false;
        fx_->Fade(t);
        fader_left_ = fader_left_ || t > 0.f;
        if (t >= 1.f || (t <= 0.f && fader_left_))
            EndFade();
        return true;
    }

    /** SHIFT let go: the crossfader ends there (FxControls::EndFade), at the start's end back
     *  to the scene it started from. False if it didn't have the morph. On the device, with the
     *  audio interrupt blocked */
    bool EndFade()
    {
        if (!fx_->Fading())
            return false;
        // back at the start: its scene active again, unless it's been deleted meanwhile
        if (fx_->EndFade() < 0)
            active_ = from_ != kNoScene && scenes_[from_].used ? from_ : kNoScene;
        return true;
    }

    /** While the crossfader has the morph: where it is, and the slot it started from
     *  (kNoScene if none was active) */
    inline bool Fading() const { return fx_->Fading(); }
    inline float FaderPos() const { return fx_->FaderPos(); }
    inline int FadeFrom() const { return from_; }
    /** The hand's crossfader has the morph (until SHIFT goes up) */
    inline bool HandFader() const { return hold_ == Hold::FADER; }

    /** A drop waits for its line */
    inline bool Dropping() const { return fx_->Dropping(); }

    /** One bar line more for the running morph; false if it can't */
    inline bool MorphMore() { return fx_->MorphMore(); }

    /** Stops the sequence where it is (CC 81): its scene stays active, edited. False if none
     *  runs */
    inline bool FreezeMorph() { return fx_->FreezeMorph(); }

    /** The slot a morph runs to, kNoScene if none runs */
    inline int Morphing() const { return Shown(morph_); }
    /** The slot a drop lands on: the morph's for a drop, B for a build-up; else kNoScene */
    inline int DropSlot() const { return fx_->Dropping() ? Shown(drop_slot_) : kNoScene; }

    /** Whether pressing the slot would pick it in the current mode: SAVE any slot but the
     *  blank one, DELETE a saved one, COPY's source a saved one or the blank one, COPY's
     *  destination any but the source and the blank one. False without a mode */
    bool Valid(size_t slot) const
    {
        const bool used = scenes_[slot].used;
        const bool blank = slot == kBlankSlot;
        switch (mode_)
        {
        case SceneMode::SAVE:
            return !blank;
        case SceneMode::COPY:
            return src_ == kNoScene ? used : static_cast<int>(slot) != src_ && !blank;
        case SceneMode::DELETE:
            return used && !blank;
        case SceneMode::NONE:
            break;
        }
        return false;
    }

    /** True while the CHOMPI key would confirm: a mode with its slot picked */
    inline bool Armed() const { return mode_ != SceneMode::NONE && sel_ != kNoScene; }

    /** The CHOMPI key: carries out the armed mode on its slot and ends the mode. Returns the
     *  slot it changed, to be written to the card, or kNoScene if it wasn't armed */
    int Confirm()
    {
        if (!Armed())
            return kNoScene;

        FxScene& target = scenes_[sel_];
        switch (mode_)
        {
        case SceneMode::SAVE:
            fx_->Snapshot(target);
            active_ = sel_;
            break;
        case SceneMode::COPY:
            target = scenes_[src_];
            // the sound stays, so it no longer matches the active scene
            if (active_ == sel_)
                fx_->MarkEdited();
            break;
        case SceneMode::DELETE:
            target.used = false;
            if (active_ == sel_)
                active_ = kNoScene;
            break;
        case SceneMode::NONE:
            break;
        }

        const int changed = sel_;
        mode_ = SceneMode::NONE;
        sel_ = src_ = kNoScene;
        return changed;
    }

    inline SceneMode Mode() const { return mode_; }
    /** The slot the mode acts on, kNoScene until picked */
    inline int Selected() const { return sel_; }
    /** COPY's source, kNoScene until picked */
    inline int Source() const { return src_; }
    /** The last scene recalled or saved, kNoScene if none or deleted */
    inline int Active() const { return active_; }
    /** A slot emptied from outside (an empty scene put over SysEx): no longer the active one,
     *  as DELETE */
    void Emptied(size_t slot)
    {
        if (active_ == static_cast<int>(slot))
            active_ = kNoScene;
    }
    /** The controls changed since the active scene was recalled or saved */
    inline bool Edited() const { return fx_->Edited(); }

private:
    /** What a hold has done so far (file comment) */
    enum class Hold : uint8_t
    {
        NONE,  // nothing yet
        HELD,  // started a sequence, A tapped
        DROP,  // the transport pressed: a drop
        BUILD, // B tapped: a build-up
        FADER, // the crossfader in the hand
        SPENT, // stopped one, or MIDI started one: the rest does nothing
    };

    /** A slot that blinks while a sequence runs; a deleted one doesn't (it still lands, on
     *  what it was) */
    inline int Shown(int slot) const
    {
        return fx_->Morphing() && slot != kNoScene && scenes_[slot].used ? slot : kNoScene;
    }

    FxScene* scenes_ = nullptr;
    FxControls<Engine>* fx_ = nullptr;
    SceneMode mode_ = SceneMode::NONE;
    int sel_ = kNoScene;
    int src_ = kNoScene;
    int active_ = kNoScene;
    int morph_ = kNoScene; // the slot the last morph ran to (A)
    int from_ = kNoScene;  // and the active one it started from
    int drop_slot_ = kNoScene; // the slot its drop lands on: A, or B for a build-up
    bool fader_left_ = false; // a MIDI fader has been away from the start (FaderTo)
    Hold hold_ = Hold::NONE;  // this SHIFT hold's
    int pend_ = 0;            // detents counted towards the crossfader, signed
    uint32_t pend_at_ = 0;    // when they reached kFaderDetents
};

} // namespace chompi

#pragma GCC pop_options
