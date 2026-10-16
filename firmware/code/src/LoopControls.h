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
 *  The plan, one at a time (the user's decisions in #70), set by Plan, which MIDI will
 *  share (#259). In a SHIFT hold the first key decides what's counted (#175's rule):
 *   - a white key alone: at once (NOW), by time (Looper::Switch's keep: as far into its pass
 *     as the loop playing is), with the looper's crossfade, as soon as it's loaded. The
 *     playing loop's own key starts it again from the top;
 *   - a white key first in a SHIFT hold, tapped n times: at the end of the n-th pass of the
 *     loop playing (PASSES), counted from SHIFT's release; not loaded by then, at the first
 *     loop point it is. Empty or paused: at once. Scene keys after it in the hold join it
 *     (WithScene): A morphs over the n passes, A then B builds up to A and drops B, the
 *     transport drops A, all landing at the n-th pass end with the loop (the engine's
 *     ReleaseFxMorphAt); their taps count nothing more;
 *   - a scene key first: the bars are counted on it, and a white key in the hold only says
 *     which loop (LANDING): where the morph lands, or with the drop; not loaded by then, at
 *     the first bar line after it that it is;
 *   - the crossfader in the hand (FADER, a scene key in the hold and the transport turned):
 *     the two loops mixed as the two scenes (Looper's kSwitchFade, Fade), the new one by
 *     time; SHIFT let go, the nearer side wins (ShiftUp);
 *   - the last white key counts; SHIFT + the planned loop's key in a later hold cancels the
 *     plan, and with it the scene sequence (NormalPage). A sequence that ends without landing
 *     (a recall, its stop, another one) takes its loop with it, and an erase every plan.
 *  Every switch keeps the tape's speed and direction (Looper.h).
 */
#pragma once
#include <string.h>
#include "LoopLoader.h"
#include "passthroughEngine.h"
#include "util/scopedirqblocker.h"

// where the twin brings the audio interrupt in the middle of MainLoop (twin::PreemptAt)
#ifndef FRIZZ_PREEMPT
#define FRIZZ_PREEMPT(point)
#endif

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
    /** When a planned loop switches (file comment) */
    enum class Kind : uint8_t
    {
        NOW,
        PASSES,
        LANDING,
        FADER,
    };
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
        const Looper& looper = engine_->looper;
        if (!Playable(Bank(), slot))
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
        // the old loop's own key on the crossfader: it's there already
        if (shift && (spent || (fader && id == playing_)))
            return Key::NOTHING;
        const bool playing = looper.GetState() == Looper::State::PLAYING;
        // a white key first in the hold counts passes, its scene keys after it too
        const bool passes = HoldPasses() || !sequence;
        const bool scene = WithScene();
        Plan(id, !shift || !playing ? Kind::NOW : fader ? Kind::FADER : passes ? Kind::PASSES : Kind::LANDING,
             1, shift);
        with_scene_ = scene && kind_ == Kind::PASSES;
        return Key::PLANNED;
    }

    /** A loop's program change (#259, MidiControl.h): LoopLoader's bank, the key's slot, CC
     *  54's timing (0 at once, 1-8 the n-th pass end, 127 with the scene sequence, at once if
     *  none waits: sequence). Empty or paused, at once, as by hand; a slot without a loop, or
     *  the looper recording, nothing. What it switches isn't fed back (TakeSwitched). With
     *  the audio interrupt blocked */
    void Remote(size_t bank, size_t slot, uint8_t timing, bool sequence)
    {
        if (!Playable(bank, slot))
            return;
        const bool now = timing == 0 || engine_->looper.GetState() != Looper::State::PLAYING
                         || (timing > kMaxLoopPasses && !sequence);
        Plan(static_cast<int>(bank * loops::kSlots + slot),
             now ? Kind::NOW : timing > kMaxLoopPasses ? Kind::LANDING : Kind::PASSES, timing, false);
        midi_ = true;
    }

    /** The loop the looper switched to since the last ask, for MIDI feedback (#259), kNone
     *  if none, MIDI planned it, or it was told when planned (TakeTold) */
    int TakeSwitched()
    {
        const int id = switched_;
        switched_ = kNone;
        return id;
    }
    /** For MIDI feedback (#400): the hand's plan at the pass end since the last ask, told as
     *  SHIFT let go counts its passes (passes: CC 54's), else kNone; cancelled: one told
     *  before went since without switching (CC 81). One told and gone between two asks is
     *  neither */
    int TakeTold(uint8_t& passes, bool& cancelled)
    {
        const int id = told_;
        told_ = kNone;
        passes = static_cast<uint8_t>(passes_);
        cancelled = untold_;
        untold_ = false;
        return id;
    }

    /** Loop id (bank * 15 + slot) planned to switch as kind says, PASSES at the end of the
     *  passes-th pass; hold: in a SHIFT hold, its passes counted from ShiftUp. The hand's and
     *  MIDI's (#259). With the audio interrupt blocked */
    void Plan(int id, Kind kind, uint32_t passes, bool hold)
    {
        Cancel();
        plan_ = id;
        midi_ = false;
        kind_ = kind;
        in_hold_ = hold;
        passes_ = passes;
        at_ = engine_->looper.Wraps() + passes;
        with_scene_ = false;
        if (kind == Kind::LANDING)
            engine_->LoopOnLanding(true);
    }

    /** A scene key's result in a SHIFT hold or out of one (SceneControls::Slot): a sequence
     *  started in the hold takes the hold's loop with it; a recall, another sequence or a
     *  stop end a plan landing with the sequence. With the audio interrupt blocked, so a
     *  recall's landing doesn't count */
    template <class Slot>
    void Scene(Slot result)
    {
        // a sequence stopped in a hold that began with a white key isn't that key's
        if (result == Slot::CANCEL && !HoldPasses())
            Cancel();
        else if (result == Slot::MORPH && HoldPasses())
            with_scene_ = true; // it lands with the loop, at the passes' end
        else if (result == Slot::RECALL || result == Slot::MORPH)
            SequenceEnded();
    }
    /** The sequence ended without landing (CC 81, MIDI's morph): its loop goes too, and the
     *  crossfader's new one. With the audio interrupt blocked */
    void SequenceEnded()
    {
        if (plan_ != kNone && (kind_ == Kind::FADER || (!in_hold_ && (kind_ == Kind::LANDING || with_scene_))))
            Cancel();
    }
    /** The hand's crossfader took the hold's sequence: its loop beside the playing one, mixed
     *  by it; the passes counted are void */
    void FaderTook()
    {
        if (plan_ == kNone || !in_hold_ || (kind_ != Kind::LANDING && kind_ != Kind::PASSES))
            return;
        if (plan_ == playing_)
        {
            Cancel(); // on the crossfader it's there already
            return;
        }
        Unpost();
        kind_ = Kind::FADER;
        with_scene_ = false;
    }
    /** This SHIFT hold began with a white key: it counts passes, and its scene keys land with
     *  the loop at their end (SceneControls::Press's passes) */
    inline bool HoldPasses() const { return in_hold_ && plan_ != kNone && kind_ == Kind::PASSES; }
    /** A scene sequence lands with the loop at the passes' end */
    inline bool WithScene() const { return HoldPasses() && with_scene_; }
    /** SHIFT let go: the passes count from here (At); the crossfader's loop to the nearer side
     *  of fader (0-1): from the middle on it takes over, else it goes */
    void ShiftUp(float fader)
    {
        if (in_hold_ && kind_ == Kind::PASSES && plan_ != kNone)
        {
            at_ = engine_->looper.Wraps() + passes_;
            landings_ = engine_->FxLandings();
            // told now, as MIDI would plan it, not again as it switches
            told_ = plan_;
            fed_ = true;
        }
        if (in_hold_ && kind_ == Kind::FADER)
        {
            // from the middle on: the fader's detents (1/24) add up to a hair under it
            const bool take = fader > .49f;
            if (posted_ != kNone)
                engine_->looper.EndFade(take);
            if (take)
                kind_ = Kind::NOW; // not loaded yet: at once when it is, by time
            else
                Cancel();
        }
        in_hold_ = false;
    }
    /** The looper's Wraps() a PASSES plan switches at */
    inline uint32_t At() const { return at_; }
    /** No plan. With the audio interrupt blocked */
    void Cancel()
    {
        Unpost();
        // one told goes without switching: CC 81 after it, or neither if it wasn't asked yet
        if (fed_ && plan_ != kNone)
        {
            if (told_ == plan_)
                told_ = kNone;
            else
                untold_ = true;
        }
        fed_ = false;
        plan_ = kNone;
    }

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

    /** The loop transfer moved slot from's file to slot to, and with swap to's to from
     *  (LoopTransfer::TakeMoved), before it says they changed (Forget): the loop playing goes
     *  with its file. Out of line: inlined into MainLoop, it moved GCC's inlining in
     *  FxChain::Run (a call to Shifter::Process left out of line), which the audio callback
     *  runs (#285) */
    __attribute__((noinline)) void Moved(int from, int to, bool swap)
    {
        carried_ = static_cast<int8_t>(playing_ == from ? to : swap && playing_ == to ? from : kNone);
        if (carried_ != kNone)
            playing_ = carried_;
    }
    /** The loop transfer changed slot id (bank * 15 + slot; LoopTransfer.h, docs/LOOP_PROTOCOL.md
     *  5.5): no buffer holds it any more; the loop playing plays on, but unless its file moved
     *  there (Moved) the slot doesn't hold it, so its key loads what the slot holds now; a load
     *  of it finishes as nobody's; a plan for it loads again when its time comes, or goes if the
     *  slot is empty now, as if cancelled */
    void Forget(int id)
    {
        for (Buf& b : bufs_)
            if (b.id == id)
                b.id = kNone;
        if (playing_ == id && id != carried_)
            playing_ = kNone;
        if (load_ == id)
            forget_load_ = true;
        if (plan_ != id)
            return;
        ScopedIrqBlocker irq;
        if (!Playable(static_cast<size_t>(id) / loops::kSlots, static_cast<size_t>(id) % loops::kSlots))
            Cancel();
        else if (posted_ == id && kind_ != Kind::FADER)
        {
            // handed to the looper with the old file's: taken back, loaded again
            engine_->looper.CancelSwitch();
            posted_ = kNone;
        }
    }
    /** For SLOT's flags (LoopTransfer.h): the looper plays slot id's loop (1), a buffer holds
     *  it ready (2) */
    uint8_t Flags(int id) const { return (id == playing_ ? 1 : 0) | (Ready(id) >= 0 ? 2 : 0); }

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

    /** Each MainLoop pass, after the loader's: what the looper did, the load, the switch.
     *  card: the card is there to load from (not lent, CardLend.h); a loop loaded switches
     *  without it */
    void Process(bool card)
    {
        Looper& looper = engine_->looper;
        carried_ = kNone;
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

        // the looper's switches and whether the one posted still waits, at one instant: the
        // switch done between two looks would seem not taken, and go again, cutting its own
        // fade-out short (#302)
        uint32_t switches;
        bool waiting;
        {
            ScopedIrqBlocker irq;
            switches = looper.Switches();
            waiting = looper.SwitchWaiting() || looper.Fading();
        }
        FRIZZ_PREEMPT("loops-switched");
        if (switches != switches_)
        {
            switches_ = switches;
            started_ = started_ || was_empty_;
            playing_ = posted_;
            switched_ = posted_midi_ ? kNone : posted_;
            if (plan_ == posted_)
                plan_ = kNone;
            posted_ = kNone;
        }
        // not taken (the looper erasing then): handed over again
        if (posted_ != kNone && !waiting)
            posted_ = kNone;
        // a recording: the recording's buffer holds no loop of the card's any more
        if (state == Looper::State::RECORDING)
            bufs_[0].id = kNone;
        // empty or recording: no loop of the card's plays. Not "the looper plays the
        // recording's buffer": a card loop loaded there plays from its start when its samples
        // start on a sector
        if (state == Looper::State::EMPTY || state == Looper::State::RECORDING)
            playing_ = kNone;
        // the card scanned again (the take-back after a lend, RESCAN): a slot whose file has
        // other frames or beats than the loop playing doesn't hold it any more, as after the
        // loop transfer's replace (#332); one nobody touched keeps it
        if (playing_ != kNone && st != LoopLoader::SCANNING)
        {
            const loops::Slot& s = loader_->GetSlot(static_cast<size_t>(playing_) / loops::kSlots,
                                                    static_cast<size_t>(playing_) % loops::kSlots);
            if (s.frames != looper.GetLength() || s.beats != looper.LoadedBeats())
                playing_ = kNone;
        }
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
        // one landing at the passes' end: once it's landed, the loop's alone; ended otherwise,
        // the loop goes with it
        if (with_scene_ && !in_hold_ && !engine_->FxMorphing())
        {
            ScopedIrqBlocker irq;
            if (engine_->FxLandings() == landings_)
                Cancel();
            with_scene_ = false;
        }

        Load(card);
        if (plan_ == kNone || posted_ != kNone || (kind_ == Kind::PASSES && in_hold_))
            return;
        const int b = Ready(plan_);
        if (b < 0)
            return;
        const Buf& buf = bufs_[b];
        const uint32_t at = kind_ == Kind::NOW       ? Looper::kSwitchNow
                            : kind_ == Kind::LANDING ? Looper::kSwitchHeld
                            : kind_ == Kind::FADER   ? Looper::kSwitchFade
                                                     : at_;
        ScopedIrqBlocker irq;
        // by time, but the playing loop's own key starts it again; the passes' end is its
        // start anyway (by time, the tape's direction and fraction kept)
        looper.Switch(buf.data, buf.frames, buf.beats, at, kind_ != Kind::LANDING && plan_ != playing_);
        posted_ = plan_;
        posted_midi_ = midi_ || fed_;
    }

private:
    /** A buffer, and the loop it holds ready (id kNone: none) */
    struct Buf
    {
        uint8_t* mem = nullptr;
        int id = kNone;
        const int16_t* data = nullptr;
        uint32_t frames = 0, beats = 0;
    };

    /** A loop in bank's slot, and the looper not recording */
    bool Playable(size_t bank, size_t slot) const
    {
        const uint8_t r = loader_->GetSlot(bank, slot).result;
        return (r == loops::MONO || r == loops::STEREO)
               && engine_->looper.GetState() != Looper::State::RECORDING;
    }

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

    /** The switch posted for the plan taken back (the crossfader's loop fading out), and the
     *  engine's wait for the landing */
    void Unpost()
    {
        Looper& looper = engine_->looper;
        looper.CancelSwitch();
        if (looper.Fading())
            looper.EndFade(false);
        engine_->LoopOnLanding(false);
        posted_ = kNone;
    }

    /** The plan's loop loaded, a step: started into the buffer the looper doesn't read
     *  (the loader's first), spread to stereo, its post-roll; one load at a time, a new
     *  plan's waiting for the last to stop, and for the card */
    void Load(bool card)
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
        if (plan_ == kNone || Ready(plan_) >= 0 || loader_->Busy() || !card)
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
        forget_load_ = false;
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
        buf.id = forget_load_ ? kNone : load_; // its slot changed meanwhile: loaded again
        forget_load_ = false;
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
    bool with_scene_ = false; // PASSES: a scene sequence lands with it (WithScene)
    uint32_t passes_ = 1;    // PASSES: the taps
    uint32_t at_ = 0;        // and the looper's Wraps() it switches at
    uint32_t landings_ = 0;  // the morph's landings when its sequence was released (WithScene)
    int posted_ = kNone;     // switch handed to the looper, for this loop
    bool midi_ = false, posted_midi_ = false; // the plan MIDI's, and the switch posted (or told)
    bool fed_ = false;       // the plan told for MIDI feedback (TakeTold)
    bool untold_ = false;    // one told went without switching
    int told_ = kNone;       // told, not asked yet
    int8_t carried_ = kNone; // playing_, moved there in this pass (Moved; FRIZZ's slots only)
    int switched_ = kNone;   // switched to by the hand's plan, for MIDI feedback (TakeSwitched)
    int playing_ = kNone;    // the slot whose file the looper plays
    int load_ = kNone;       // loading, into bufs_[target_]
    bool forget_load_ = false; // and its slot changed since (Forget)
    int target_ = 1;
    uint32_t spread_ = 0;    // frames of a mono loop still to spread, or kSpreadDone
    uint32_t switches_ = 0;  // the looper's Switches() as last seen
    bool was_empty_ = true, had_loop_ = false, started_ = false;
    int failed_ = kNone;
};

} // namespace chompi

#pragma GCC pop_options
