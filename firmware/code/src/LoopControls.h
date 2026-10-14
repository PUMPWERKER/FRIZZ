/** @file LoopControls.h
 *  @brief The Loops page's and the Cubbi page's loops (#70, MANUAL.md "Loop loader"): which
 *  loop the white keys pick and when it takes over, the loads behind it, and what the keys'
 *  LEDs show. The Loops page has FRIZZ's own loops (banks a-h), the Cubbi page TAPE's cubbis
 *  (a-e, LoopLoader's from kCubbiBank); both are one row of loops, with one plan, and each page
 *  remembers the bank it showed. NormalPage.h routes the pages' keys here and draws;
 *  LoopLoader.h reads the card; the looper switches (Looper::Switch) and the engine times a
 *  switch with a scene sequence (LoopOnLanding). Everything here runs from MainLoop.
 *
 *  A loop is the loop: there are two buffers, the recording's own (the looper's, Init's
 *  rec) and the loader's, and a loop is loaded into the one the looper doesn't read, so the
 *  next one waits there while the last plays on. Switching away from a recording loses it.
 *  A buffer remembers what it holds, so going back to the loop before needs no load. A
 *  mono file is spread to stereo where it landed, a step a pass, and a loop's first
 *  kXfadeFrames are copied after its end, the looper's post-roll; only then is it ready.
 *
 *  The plan, one at a time (the user's decisions in #70):
 *   - a white key alone: at once (NOW), with the looper's crossfade, as soon as it's loaded;
 *   - SHIFT + a white key, tapped n times: at the end of the n-th pass of the loop playing
 *     (PASSES), counted from SHIFT's release; not loaded by then, at the first loop point it
 *     is. Empty or paused: at once;
 *   - in a SHIFT hold with a scene sequence (before or after the scene key): where the morph
 *     lands, or with the drop (LANDING); not loaded by then, at the first bar line after it
 *     that it is. With the crossfader in the hand: at once;
 *   - the last white key counts; SHIFT + the planned loop's key in a later hold cancels the
 *     plan, and with it the scene sequence (NormalPage). A sequence that ends without landing
 *     (a recall, its stop, another one) takes its loop with it, and an erase every plan.
 */
#pragma once
#include <string.h>
#include "LoopLoader.h"
#include "passthroughEngine.h"

// the play page's logic, built for size (SRAM_EXEC, docs/CAPACITY.md)
#pragma GCC push_options
#pragma GCC optimize("Os")

namespace chompi
{

static const uint32_t kMaxLoopPasses = 8;       // SHIFT + a white key's taps
static const uint32_t kExpandFrames = 16384;    // a mono loop spread to stereo, a pass's worth

class LoopControls
{
public:
    static const int kNone = -1;
    /** What a white key did */
    enum class Key
    {
        PLANNED,   // a new plan
        COUNTED,   // the planned key again in its hold: a pass more, or nothing
        CANCELLED, // SHIFT + the planned key in a later hold: the plan's gone (and its sequence)
        REFUSED,   // no loop there, or the looper records
        NOTHING,   // in a SHIFT hold that cancelled a sequence
    };
    /** What a slot's LED shows */
    enum class Led
    {
        DARK,     // empty
        FILLED,   // a loop
        BAD,      // a file refused (LoopLoader's Result)
        PLAYING,  //
        PLANNED,  // waiting, loaded
        LOADING,  // waiting, not loaded yet
    };

    /** rec: the looper's own buffer (Looper::Init's); the loader loads into it or its own */
    void Init(PassthroughEngine* engine, LoopLoader* loader, int16_t* rec)
    {
        engine_ = engine;
        loader_ = loader;
        bufs_[0].mem = reinterpret_cast<uint8_t*>(rec);
        bufs_[1].mem = const_cast<uint8_t*>(loader->Home());
    }

    // ---- the pages and their banks ----
    /** The bank shown (LoopLoader's: the Cubbi page's from kCubbiBank) */
    inline size_t Bank() const { return bank_[cubbi_]; }
    /** The Cubbi page shown, not the Loops page */
    inline bool Cubbi() const { return cubbi_; }
    /** The next bank (dir 1) or the one before (-1) of the page with a file in it, kNone at
     *  the end: where C# or D# leads, in its colour */
    int Neighbour(int dir) const
    {
        for (int b = static_cast<int>(Bank()) + dir; b >= static_cast<int>(First()) && b < static_cast<int>(Last()); b += dir)
            if (BankUsed(static_cast<size_t>(b)))
                return b;
        return kNone;
    }
    /** To Neighbour(dir); false at the end */
    bool StepBank(int dir)
    {
        const int b = Neighbour(dir);
        if (b == kNone)
            return false;
        bank_[cubbi_] = static_cast<size_t>(b);
        return true;
    }
    /** On a page opening: the first bank with a file, unless the one it showed has one. False
     *  if none has */
    bool Show(bool cubbi)
    {
        cubbi_ = cubbi;
        if (BankUsed(Bank()))
            return true;
        bank_[cubbi_] = First();
        return BankUsed(First()) || StepBank(1);
    }

    // ---- the keys (NormalPage) ----

    /** A white key, slot 0-14 of the bank shown. shift: SHIFT held; sequence: this SHIFT hold
     *  started a scene sequence; fader: its crossfader is in the hand; spent: it stopped one.
     *  On the device with the audio interrupt blocked */
    Key Press(size_t slot, bool shift, bool sequence, bool fader, bool spent)
    {
        const loops::Slot& s = loader_->GetSlot(Bank(), slot);
        const Looper& looper = engine_->looper;
        if ((s.result != loops::MONO && s.result != loops::STEREO)
            || looper.GetState() == Looper::State::RECORDING)
            return Key::REFUSED;
        const int id = static_cast<int>(Bank() * loops::kSlots + slot);
        if (shift && plan_ == id)
        {
            if (!in_hold_)
            {
                Cancel();
                return Key::CANCELLED;
            }
            if (kind_ == Kind::PASSES && passes_ < kMaxLoopPasses)
                passes_++;
            return Key::COUNTED;
        }
        if (shift && spent)
            return Key::NOTHING;
        Cancel();
        plan_ = id;
        in_hold_ = shift;
        passes_ = 1;
        const bool playing = looper.GetState() == Looper::State::PLAYING;
        kind_ = !shift || fader || !playing ? Kind::NOW : sequence ? Kind::LANDING : Kind::PASSES;
        if (kind_ == Kind::LANDING)
            engine_->LoopOnLanding(true);
        return Key::PLANNED;
    }

    /** A scene key's result in a SHIFT hold or out of one (SceneControls::Slot): a sequence
     *  started in the hold takes the hold's loop with it; a recall, another sequence or a
     *  stop end a plan landing with the sequence. With the audio interrupt blocked, so a
     *  recall's landing doesn't count */
    template <class Slot>
    void Scene(Slot result)
    {
        if (result == Slot::CANCEL)
            Cancel();
        else if (result == Slot::MORPH && in_hold_ && plan_ != kNone && kind_ == Kind::PASSES)
        {
            kind_ = Kind::LANDING;
            engine_->LoopOnLanding(true);
        }
        else if (result == Slot::RECALL || result == Slot::MORPH)
            SequenceEnded();
    }
    /** The sequence ended without landing (CC 81, MIDI's morph): its loop goes too. With the
     *  audio interrupt blocked */
    void SequenceEnded()
    {
        if (plan_ != kNone && kind_ == Kind::LANDING && !in_hold_)
            Cancel();
    }
    /** The hand's crossfader took the hold's sequence: its loop at once */
    void FaderTook()
    {
        if (plan_ != kNone && in_hold_ && kind_ == Kind::LANDING)
        {
            Unpost();
            kind_ = Kind::NOW;
        }
    }
    /** SHIFT let go: the passes count from here */
    void ShiftUp()
    {
        if (in_hold_ && kind_ == Kind::PASSES)
            at_ = engine_->looper.Wraps() + passes_;
        in_hold_ = false;
    }
    /** No plan. With the audio interrupt blocked */
    void Cancel()
    {
        Unpost();
        plan_ = kNone;
    }

    /** A plan waits, its key blinking: for SHIFT + a blinking key */
    inline bool Planned() const { return plan_ != kNone; }
    /** Its loop lands with the scene sequence */
    inline bool WithSequence() const { return plan_ != kNone && kind_ == Kind::LANDING; }

    /** Whether a load (or its spreading) writes into the recording's buffer: a recording
     *  waits for it */
    inline bool WritingRec() const { return load_ != kNone && target_ == 0; }

    /** A switch from an empty looper happened since the last ask: the mix goes wet */
    bool TakeStarted()
    {
        const bool started = started_;
        started_ = false;
        return started;
    }
    /** A load failed since the last ask: its slot (in its bank), else kNone */
    int TakeFailed()
    {
        const int failed = failed_;
        failed_ = kNone;
        return failed;
    }

    /** A slot of the bank shown */
    Led SlotLed(size_t slot) const
    {
        const int id = static_cast<int>(Bank() * loops::kSlots + slot);
        const uint8_t result = loader_->GetSlot(Bank(), slot).result;
        if (id == plan_)
            return Ready(id) >= 0 ? Led::PLANNED : Led::LOADING;
        if (id == playing_)
            return Led::PLAYING;
        if (result == loops::EMPTY)
            return Led::DARK;
        return result == loops::MONO || result == loops::STEREO ? Led::FILLED : Led::BAD;
    }

    /** Each MainLoop pass, after the loader's: what the looper did, the load, the switch */
    void Process()
    {
        Looper& looper = engine_->looper;
        const Looper::State state = looper.GetState();
        // the developers' loads go into the loader's buffer: not while it's played or ours,
        // and what it held is gone after one
        loader_->DevBuffer(load_ == kNone && !looper.Reads(bufs_[1].mem));
        const LoopLoader::State st = loader_->GetState();
        if (load_ == kNone && (st == LoopLoader::LOADING || st == LoopLoader::WRITING))
            bufs_[1].id = kNone;
        // scanned again (the developers' SysEx): a slot may hold another file now
        if (st == LoopLoader::SCANNING)
            bufs_[0].id = bufs_[1].id = kNone;

        if (looper.Switches() != switches_)
        {
            switches_ = looper.Switches();
            started_ = started_ || was_empty_;
            playing_ = posted_;
            if (plan_ == posted_)
                plan_ = kNone;
            posted_ = kNone;
        }
        // not taken (the looper erasing then): handed over again
        if (posted_ != kNone && !looper.SwitchWaiting())
            posted_ = kNone;
        // a recording: the recording's buffer holds no loop of the card's any more
        if (state == Looper::State::RECORDING)
            bufs_[0].id = kNone;
        if (!looper.Loaded())
            playing_ = kNone;
        // an erase takes every plan with it
        const bool loop = (state == Looper::State::PLAYING || state == Looper::State::PAUSED)
                          && !looper.IsErasing();
        if (had_loop_ && !loop && plan_ != kNone)
        {
            ScopedIrqBlocker irq;
            Cancel();
        }
        had_loop_ = loop;
        was_empty_ = state == Looper::State::EMPTY;
        // a sequence stopped where it was by MIDI (CC 81) or replaced: its loop goes too
        if (plan_ != kNone && kind_ == Kind::LANDING && !in_hold_ && engine_->LoopOnLanding()
            && !engine_->FxMorphing() && !engine_->LoopLanded())
        {
            ScopedIrqBlocker irq;
            Cancel();
        }

        Load();
        if (plan_ == kNone || posted_ != kNone || (kind_ == Kind::PASSES && in_hold_))
            return;
        const int b = Ready(plan_);
        if (b < 0)
            return;
        const Buf& buf = bufs_[b];
        const uint32_t at = kind_ == Kind::NOW       ? Looper::kSwitchNow
                            : kind_ == Kind::LANDING ? Looper::kSwitchHeld
                                                     : at_;
        ScopedIrqBlocker irq;
        looper.Switch(buf.data, buf.frames, buf.beats, at);
        posted_ = plan_;
    }

private:
    enum class Kind : uint8_t
    {
        NOW,
        PASSES,
        LANDING,
    };
    /** A buffer, and the loop it holds ready (id kNone: none) */
    struct Buf
    {
        uint8_t* mem = nullptr;
        int id = kNone;
        const int16_t* data = nullptr;
        uint32_t frames = 0, beats = 0;
    };

    inline size_t First() const { return cubbi_ ? loops::kCubbiBank : 0; }
    inline size_t Last() const { return cubbi_ ? loops::kBanks : loops::kFrizzBanks; }

    bool BankUsed(size_t bank) const
    {
        for (size_t s = 0; s < loops::kSlots; s++)
            if (loader_->GetSlot(bank, s).result != loops::EMPTY)
                return true;
        return false;
    }

    /** The buffer holding id ready, -1 for none */
    int Ready(int id) const
    {
        for (int b = 0; b < 2; b++)
            if (bufs_[b].id == id && id != kNone)
                return b;
        return -1;
    }

    /** The switch posted for the plan taken back, and the engine's wait for the landing */
    void Unpost()
    {
        engine_->looper.CancelSwitch();
        engine_->LoopOnLanding(false);
        posted_ = kNone;
    }

    /** The plan's loop loaded, a step: started into the buffer the looper doesn't read
     *  (the loader's first), spread to stereo, its post-roll; one load at a time, a new
     *  plan's waiting for the last to stop */
    void Load()
    {
        const Looper& looper = engine_->looper;
        if (load_ != kNone)
        {
            if (spread_)
            {
                Spread();
                return;
            }
            const LoopLoader::State st = loader_->GetState();
            if (st == LoopLoader::LOADING)
            {
                if (load_ != plan_)
                    loader_->Cancel();
                return;
            }
            if (st == LoopLoader::LOADED && loader_->Buffer() == bufs_[target_].mem)
            {
                Buf& buf = bufs_[target_];
                buf.data = loader_->Data();
                buf.frames = loader_->Frames();
                buf.beats = loader_->Beats();
                spread_ = loader_->Channels() == 1 ? buf.frames : 0;
                spread_ = spread_ ? spread_ : kSpreadDone;
                return;
            }
            if (st == LoopLoader::FAILED && load_ == plan_)
            {
                failed_ = plan_;
                ScopedIrqBlocker irq;
                Cancel();
            }
            load_ = kNone;
            return;
        }
        if (plan_ == kNone || Ready(plan_) >= 0 || loader_->Busy())
            return;
        // the loader's buffer unless the looper reads it, then the recording's
        int b = looper.Reads(bufs_[1].mem) ? 0 : 1;
        if (looper.Reads(bufs_[b].mem))
            return; // both: a crossfade still reads the other, a moment
        const size_t bank = static_cast<size_t>(plan_) / loops::kSlots;
        const size_t slot = static_cast<size_t>(plan_) % loops::kSlots;
        if (!loader_->Request(LoopLoader::LOADING, static_cast<uint8_t>(bank),
                              static_cast<uint8_t>(slot), 0, bufs_[b].mem))
            return;
        bufs_[b].id = kNone;
        target_ = b;
        load_ = plan_;
        spread_ = 0;
    }

    /** A mono loop spread to two channels where it landed, from its end down a pass's worth
     *  at a time, then its first frames after its end (the looper's post-roll): ready */
    void Spread()
    {
        Buf& buf = bufs_[target_];
        int16_t* d = const_cast<int16_t*>(buf.data);
        if (spread_ != kSpreadDone)
        {
            const uint32_t to = spread_ > kExpandFrames ? spread_ - kExpandFrames : 0;
            for (uint32_t i = spread_; i-- > to;)
                d[2 * i] = d[2 * i + 1] = d[i];
            spread_ = to ? to : kSpreadDone;
            return;
        }
        const uint32_t head = buf.frames < kXfadeFrames ? buf.frames : kXfadeFrames;
        memcpy(d + 2 * buf.frames, d, head * 4);
        buf.id = load_;
        load_ = kNone;
        spread_ = 0;
    }

    static const uint32_t kSpreadDone = 0xFFFFFFFFu;

    PassthroughEngine* engine_ = nullptr;
    LoopLoader* loader_ = nullptr;
    Buf bufs_[2];            // the recording's, the loader's
    size_t bank_[2] = {0, loops::kCubbiBank}; // shown: the Loops page's, the Cubbi page's
    bool cubbi_ = false;     // the Cubbi page shown (or last shown)
    int plan_ = kNone;       // the loop planned (bank * 15 + slot)
    Kind kind_ = Kind::NOW;  // and when it switches
    bool in_hold_ = false;   // planned in this SHIFT hold
    uint32_t passes_ = 1;    // PASSES: the taps
    uint32_t at_ = 0;        // and the looper's Wraps() it switches at
    int posted_ = kNone;     // switch handed to the looper, for this loop
    int playing_ = kNone;    // the loop from the card the looper plays
    int load_ = kNone;       // loading, into bufs_[target_]
    int target_ = 1;
    uint32_t spread_ = 0;    // frames of a mono loop still to spread, or kSpreadDone
    uint32_t switches_ = 0;  // the looper's Switches() as last seen
    bool was_empty_ = true, had_loop_ = false, started_ = false;
    int failed_ = kNone;
};

} // namespace chompi

#pragma GCC pop_options
