/** @file SceneControls.h
 *  @brief What the play page does with the FX scene keys, without the hardware: recalling a
 *  slot or morphing to it (SHIFT, FxMorph.h), and TAPE's and TEMPO's SAVE / COPY / DELETE flow (pick the mode, the slot(s), then
 *  confirm). Like a mode key, a slot tapped again is deselected. NormalPage.h routes the
 *  keys here, writes the card (SceneStore.h) and draws the LEDs; test/controls.cpp runs it on
 *  the host.
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

template <class Engine>
class SceneControls
{
public:
    /** What pressing a slot did */
    enum class Slot
    {
        SELECTED,   // picked for the mode or deselected, or nothing to do
        RECALL,     // recall it: Recall(slot), with the audio interrupt blocked on the device
        MORPH,      // morph to it: Morph(slot), likewise
        MORPH_MORE, // the slot being morphed to: MorphMore(), likewise
        REFUSED,    // a slot the mode can't act on: empty where a saved one is needed, or
                    // blank; or SHIFT + another slot than the one being morphed to
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

    /** shift: SHIFT held, a morph rather than a recall; while one runs, only to the slot it
     *  runs to. In a mode, SHIFT makes no difference */
    Slot SlotPressed(size_t slot, bool shift = false)
    {
        const int s = static_cast<int>(slot);
        if (mode_ == SceneMode::NONE)
        {
            if (shift && fx_->Morphing())
                return s == morph_ && scenes_[slot].used ? Slot::MORPH_MORE : Slot::REFUSED;
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

    /** A scene key, all of it: SlotPressed, then the recall, morph or extra bar it calls for.
     *  REFUSED also when a morph can't take another bar. On the device, call it with the audio
     *  interrupt blocked, so a recall lands within one block */
    Slot Press(size_t slot, bool shift)
    {
        const Slot result = SlotPressed(slot, shift);
        switch (result)
        {
        case Slot::RECALL:
            Recall(slot);
            break;
        case Slot::MORPH:
            Morph(slot);
            break;
        case Slot::MORPH_MORE:
            return MorphMore() ? result : Slot::REFUSED;
        case Slot::SELECTED:
        case Slot::REFUSED:
            break;
        }
        return result;
    }

    /** Recalls a used slot, which becomes the active scene */
    void Recall(size_t slot)
    {
        fx_->Recall(scenes_[slot]);
        active_ = static_cast<int>(slot);
    }

    /** Morphs to a used slot, which becomes the active scene now */
    void Morph(size_t slot)
    {
        fx_->Morph(scenes_[slot]);
        from_ = active_;
        active_ = morph_ = static_cast<int>(slot);
    }

    /** The transport knob turned with SHIFT held, detents (signed): the crossfader, while a
     *  morph waits for SHIFT to be let go or is on the crossfader already (#66). False if not,
     *  so the knob turns the speed. On the device, with the audio interrupt blocked */
    bool FaderTurned(float detents)
    {
        if (!fx_->Fading() && !fx_->MorphHeld())
            return false;
        const float from = fx_->Fading() ? fx_->FaderPos() : 0.f;
        const float t = from + detents * kFaderStep;
        fx_->Fade(t < 0.f ? 0.f : (t > 1.f ? 1.f : t));
        return true;
    }

    /** A fader over MIDI: a running morph, held or gliding, to t. Reaching an end decides as
     *  letting go there would (EndFade): the scene's at once, the start's once it has been
     *  away from it. False if no morph runs. On the device, with the audio interrupt blocked */
    bool FaderTo(float t)
    {
        if (!fx_->Morphing())
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

    /** One bar line more for the running morph; false if it can't */
    inline bool MorphMore() { return fx_->MorphMore(); }

    /** Stops the morph where it is (SHIFT + PLAY): its scene stays active, edited. False if
     *  none runs */
    inline bool FreezeMorph() { return fx_->FreezeMorph(); }

    /** The slot a morph runs to, kNoScene if none runs */
    inline int Morphing() const
    {
        // a deleted target doesn't blink: the morph still lands, on what it was
        return fx_->Morphing() && morph_ != kNoScene && scenes_[morph_].used ? morph_ : kNoScene;
    }

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
    /** The controls changed since the active scene was recalled or saved */
    inline bool Edited() const { return fx_->Edited(); }

private:
    FxScene* scenes_ = nullptr;
    FxControls<Engine>* fx_ = nullptr;
    SceneMode mode_ = SceneMode::NONE;
    int sel_ = kNoScene;
    int src_ = kNoScene;
    int active_ = kNoScene;
    int morph_ = kNoScene; // the slot the last morph ran to
    int from_ = kNoScene;  // and the active one it started from
    bool fader_left_ = false; // a MIDI fader has been away from the start (FaderTo)
};

} // namespace chompi

#pragma GCC pop_options
