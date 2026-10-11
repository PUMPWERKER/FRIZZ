/** @file MidiOut.h
 *  @brief MIDI clock, Start, Stop, Continue and Song Position out of the MIDI jack, and over
 *  USB if asked (the settings page's upper D#, MANUAL.md "MIDI out"; decided in #60). Off at
 *  first: then nothing goes out.
 *
 *  While it's on, the clock always runs, at the tempo FRIZZ thinks in, the beat on the
 *  settings page's G#; whether the gear behind plays is up to Start, Continue and Stop.
 *  - Passing on: with a MIDI clock coming in and no loop, its ticks go out as MidiClock counts
 *    them, so by the clock factor: at half every other one, at double one more halfway to the
 *    next, timed by the clock's period. Start, Continue and Stop from that input go out too.
 *  - Generating: with a loop, or without a clock, two ticks per pulse of the tempo clock
 *    (TempoClock.h, 12 PPQN), one on the pulse and one halfway to the next. With a loop they
 *    come from its play position, so beat 1 is the loop's start, at its speed (in reverse the
 *    clock runs on: it can't count down); paused, its grid runs on by itself.
 *  With a loop, the transport is the loop's, and nothing that comes in is passed on:
 *  - a new loop starting to play sends Start with its first tick, unless the gear already
 *    plays from a Start passed on: the ticks then just carry on (a quantized loop starts on
 *    the clock's bar line, and a Start would throw longer patterns back to their bar 1);
 *  - a pause sends Stop; playing on sends Song Position at the next 16th right away, then
 *    Continue with that 16th's tick, so gear that ignores Song Position resumes in step too.
 *  Erasing the loop sends nothing: the out passes the incoming clock on again, or runs on at
 *  the loop's tempo.
 *
 *  Never back where a clock comes from: while passing on, nothing goes out of the input it
 *  comes in on (the sender leads; a soft thru would feed FRIZZ its own ticks). Generating,
 *  everything goes to every port the setting has.
 *
 *  The jack's bytes go to the UART from the audio callback without waiting
 *  (MidiClock::TrsPut): a ring of them, drained as the UART takes them, two at once, then one
 *  every 0.32 ms. USB's are sent by MainLoop (FlushUsb), between the answers to queries,
 *  which keeps them out of a SysEx; while MainLoop writes the card (a scene saved), they wait,
 *  so USB's ticks can bunch up then.
 */
#pragma once
#include "MidiClock.h"
#include "passthroughEngine.h"

namespace chompi
{

class MidiOut
{
public:
    void Init(MidiClock* clock) { clock_ = clock; }

    /** Once per block from the audio callback, after the engine; ports is the setting
     *  (MidiControl's, from the settings page or a SysEx) */
    void Process(size_t size, PassthroughEngine& engine, MidiOutPorts ports)
    {
        ports_ = ports;
        const TempoClock& tempo = engine.Tempo();
        const bool loop = tempo.HasLoop();
        const bool paused = loop && engine.looper.GetState() == Looper::State::PAUSED;
        const bool passing = clock_->HasClock() && !loop;
        MidiClock::Source in_from;
        const uint8_t in = clock_->TakeTransport(in_from);
        const uint32_t ticks = clock_->GetTicks();

        if (ports_ != MidiOutPorts::OFF)
        {
            const MidiClock::Source back = passing ? clock_->Locked() : MidiClock::Source::NONE;

            // the loop's transport
            if (loop && !was_loop_)
                start_ = !paused && !(was_passing_ && playing_);
            else if (loop && paused && !was_paused_)
            {
                Send(kStop, back);
                playing_ = false;
                start_ = false;
                continue_at_ = kNone;
            }
            else if (loop && !paused && was_paused_)
            {
                continue_at_ = engine.Next16th();
                const uint32_t spp = continue_at_ / kPulsesPer16th;
                const uint8_t msg[3] = {0xF2, static_cast<uint8_t>(spp & 0x7F),
                                        static_cast<uint8_t>((spp >> 7) & 0x7F)};
                Send(msg, 3, back);
            }
            if (!loop)
            {
                start_ = false;
                continue_at_ = kNone;
                if (in)
                {
                    Send(in, in_from);
                    playing_ = in != kStop;
                }
            }

            // the ticks: a halfway one due first, then this block's
            if (half_due_ && static_cast<int32_t>(now_ - half_at_) >= 0)
            {
                half_due_ = false;
                SendTick(back);
            }
            if (passing)
            {
                // the ticks counted since the last block: at double two per tick that came,
                // the last one's second sent halfway to the next; more than one came only in
                // a bunch (USB's frames), and goes on as it came, every tick counted
                const bool twice = clock_->Factor() == ClockFactor::DOUBLE;
                uint32_t n = ticks - last_ticks_;
                if (n > kMaxBunch)
                    n = kMaxBunch;
                for (uint32_t i = twice ? 2 : 1; i < n; i++)
                    SendTick(back);
                if (n > 0)
                    Tick(twice ? clock_->GetTickPeriod() : 0.f, 0.f, back);
            }
            else if (engine.BlockPulses() > 0)
            {
                // a burst of pulses (a tap refit, a jump) sends one: the next re-aligns
                const uint32_t at = engine.BlockLoopPulse(0);
                if (start_)
                {
                    Send(kStart, back);
                    playing_ = true;
                    start_ = false;
                }
                else if (continue_at_ != kNone && at % kPulsesPer16th == 0)
                {
                    Send(kContinue, back);
                    playing_ = true;
                    continue_at_ = kNone;
                }
                // a tick within half a tick of the last one sent is that one, re-aligned: the
                // passed-on clock's bar line a quantized loop starts on, or a halfway tick
                // that came late as the grid moved
                const float tick = .5f * tempo.PulseSamples();
                Tick(tick, .5f * tick, back);
            }
            DrainTrs();
        }
        else
        {
            playing_ = false;
            half_due_ = false;
            start_ = false;
            continue_at_ = kNone;
            trs_.Clear();
        }

        last_ticks_ = ticks;
        was_loop_ = loop;
        was_paused_ = paused;
        was_passing_ = passing;
        now_ += size;
    }

    /** Instead of Process while FRIZZ starts up (the boot screen): what came in meanwhile is
     *  dropped, so a Start from then doesn't go out late, nor the ticks counted in a bunch */
    void Skip()
    {
        MidiClock::Source from;
        clock_->TakeTransport(from);
        last_ticks_ = clock_->GetTicks();
    }

    /** From MainLoop, every pass: USB's bytes, a message at a time */
    void FlushUsb()
    {
        uint8_t msg[3];
        size_t n;
        while ((n = usb_.PopMessage(msg)) > 0)
            clock_->SendUsb(msg, n);
    }

private:
    static const uint8_t kTick = 0xF8, kStart = 0xFA, kContinue = 0xFB, kStop = 0xFC;
    static const uint32_t kNone = 0xFFFFFFFF;
    static const uint32_t kMaxBunch = 4; // ticks passed on in one block, at most

    /** A byte ring, filled by the audio callback; read by it (the jack's) or by MainLoop
     *  (USB's), which only sees whole messages: the callback adds one before it returns */
    class Ring
    {
    public:
        void Push(const uint8_t* msg, size_t n)
        {
            uint32_t head = head_;
            if (kSize - (head - tail_) < n)
                return; // full: the UART or MainLoop is stuck, and old ticks are no use
            for (size_t i = 0; i < n; i++)
                buf_[head++ % kSize] = msg[i];
            head_ = head;
        }
        inline bool Empty() const { return head_ == tail_; }
        inline uint8_t Front() const { return buf_[tail_ % kSize]; }
        inline void Pop() { tail_ = tail_ + 1; }
        inline void Clear() { tail_ = head_; }
        /** The next message, its length (a Song Position's 3, else 1), 0 for none */
        size_t PopMessage(uint8_t* msg)
        {
            if (Empty())
                return 0;
            const size_t n = Front() == 0xF2 ? 3 : 1;
            for (size_t i = 0; i < n; i++)
            {
                msg[i] = Front();
                Pop();
            }
            return n;
        }

    private:
        static const uint32_t kSize = 32;
        uint8_t buf_[kSize];
        volatile uint32_t head_ = 0, tail_ = 0;
    };

    /** A message to every port the setting has but not_to */
    void Send(const uint8_t* msg, size_t n, MidiClock::Source not_to)
    {
        if (not_to != MidiClock::Source::TRS)
            trs_.Push(msg, n);
        if (ports_ == MidiOutPorts::TRS_USB && not_to != MidiClock::Source::USB)
            usb_.Push(msg, n);
    }
    inline void Send(uint8_t status, MidiClock::Source not_to) { Send(&status, 1, not_to); }

    /** A tick now, unless one went out less than min_gap samples ago, and another one
     *  half_after samples on if that's not 0 */
    void Tick(float half_after, float min_gap, MidiClock::Source not_to)
    {
        if (half_due_)
            SendTick(not_to); // the halfway one of the last, late
        if (min_gap <= 0.f || static_cast<float>(now_ - last_tick_at_) >= min_gap)
            SendTick(not_to);
        half_due_ = half_after > 0.f;
        half_at_ = now_ + static_cast<uint32_t>(half_after);
    }
    void SendTick(MidiClock::Source not_to)
    {
        Send(kTick, not_to);
        last_tick_at_ = now_;
    }

    void DrainTrs()
    {
        while (!trs_.Empty() && clock_->TrsPut(trs_.Front()))
            trs_.Pop();
    }

    MidiClock* clock_ = nullptr;
    MidiOutPorts ports_ = MidiOutPorts::OFF;
    Ring trs_, usb_;
    uint32_t now_ = 0;        // samples, from the first block
    uint32_t last_ticks_ = 0; // MidiClock's count at the last block
    bool was_loop_ = false, was_paused_ = false, was_passing_ = false;
    bool playing_ = false;    // the gear plays: a Start or Continue went out, no Stop since
    bool start_ = false;      // a new loop's Start waits for its first pulse
    uint32_t continue_at_ = kNone; // the loop pulse the Continue waits for
    bool half_due_ = false;   // a halfway tick waits for half_at_
    uint32_t half_at_ = 0;
    uint32_t last_tick_at_ = 0; // when the last tick went out
};

} // namespace chompi
