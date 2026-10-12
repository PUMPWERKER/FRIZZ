/** @file MidiOut.h
 *  @brief MIDI clock, Start, Stop, Continue and Song Position out of the MIDI jack, and over
 *  USB if asked (the settings page's upper C#, MANUAL.md "MIDI out"; decided in #60). Off at
 *  first: then nothing goes out.
 *
 *  While it's on, the clock always runs, at the tempo FRIZZ thinks in, the beat on the
 *  settings page's G#; whether the gear behind plays is up to Start, Continue and Stop.
 *  - Passing on: with a MIDI clock coming in and no loop, its ticks go out as MidiClock counts
 *    them, so by the clock factor: at half every other one, at double one more halfway to the
 *    next, timed by the clock's period. Start, Continue and Stop from that input go out too.
 *  - Generating: with a loop, or without a clock, two ticks per pulse of the tempo clock
 *    (TempoClock.h, 12 PPQN), one on the pulse and one halfway to the next, so by the clock
 *    factor as the effects are. With a loop they come from its play position, so beat 1 is
 *    the loop's start, at its speed (in reverse the clock runs on: it can't count down);
 *    paused, its grid runs on by itself. At x1/2 a loop of an odd number of beats has its
 *    grid over two passes, beat 1 on every other start, and the Song Position counts in it.
 *  With a loop, the transport is the loop's, and nothing that comes in is passed on:
 *  - a new loop starting to play sends Start with its first tick, unless the gear already
 *    plays from a Start passed on: the ticks then just carry on (a quantized loop starts on
 *    the clock's bar line, and a Start would throw longer patterns back to their bar 1). That
 *    goes by port: the jack's gear gets the loop's Start if the one passed on went only over
 *    USB (it came in on the jack), and USB's likewise;
 *  - a pause sends Stop; playing on sends Song Position at the next 16th right away, then
 *    Continue with that 16th's tick, so gear that ignores Song Position resumes in step too;
 *  - a playing loop taken back to its start by MIDI's Start (Looper::Follow) sends Start with
 *    its first tick again.
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
 *  so USB's ticks can bunch up then. Ticks leave room in a ring for the transport
 *  (kTransportRoom): held up long enough to fill it, USB drops ticks, not a Stop, a Song
 *  Position and Continue or a Start.
 */
#pragma once
#include "MidiClock.h"
#include "MidiFeedback.h"
#include "passthroughEngine.h"

namespace chompi
{

class MidiOut
{
public:
    /** feedback: MIDI feedback's bytes for the jack (MidiFeedback.h), sent after the clock's */
    void Init(MidiClock* clock, FeedbackRing* feedback)
    {
        clock_ = clock;
        feedback_ = feedback;
    }

    /** Once per block from the audio callback, after the engine; ports is the setting
     *  (MidiControl's, from the settings page or a SysEx) */
    void Process(size_t size, PassthroughEngine& engine, MidiOutPorts ports)
    {
        ports_ = ports;
        playing_ &= Ports(MidiClock::Source::NONE); // a port left out: its gear unknown
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
                start_ = paused ? 0 : was_passing_ ? kAllPorts & ~playing_ : kAllPorts;
            else if (loop && engine.looper.Rewinds() != rewinds_)
                start_ = kAllPorts; // back to its start by MIDI's Start (Looper::Follow)
            else if (loop && paused && !was_paused_)
            {
                Transport(kStop, Ports(back));
                start_ = 0;
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
                start_ = 0;
                continue_at_ = kNone;
                if (in)
                    Transport(in, Ports(in_from));
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
                    Transport(kStart, start_ & Ports(back));
                    start_ = 0;
                }
                else if (continue_at_ != kNone && at % kPulsesPer16th == 0)
                {
                    Transport(kContinue, Ports(back));
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
            playing_ = 0;
            half_due_ = false;
            start_ = 0;
            continue_at_ = kNone;
            trs_.Clear();
            feedback_->Clear();
            feedback_sent_ = 0;
        }

        last_ticks_ = ticks;
        rewinds_ = engine.looper.Rewinds();
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

    /** From MainLoop, every pass: USB's bytes, a message at a time, never waiting. Without
     *  a host (on a battery) they're dropped unsent (#84); one USB can't take yet (the
     *  transfer before still going, MIDI feedback's or an answer's) waits for the next pass,
     *  for kUsbWaitMs at most */
    void FlushUsb()
    {
        uint8_t msg[3];
        size_t n;
        while ((n = PeekMessage(usb_, msg)) > 0)
        {
            const uint32_t now = System::GetNow();
            if (!clock_->UsbHost())
            {
                usb_.Clear();
                usb_waiting_ = false;
                return;
            }
            if (!clock_->TrySendUsb(msg, n))
            {
                if (!usb_waiting_)
                    usb_waiting_since_ = now;
                usb_waiting_ = true;
                if (now - usb_waiting_since_ <= kUsbWaitMs)
                    return;
            }
            usb_waiting_ = false;
            usb_.Drop(n);
        }
    }

private:
    static const uint8_t kTick = 0xF8, kStart = 0xFA, kContinue = 0xFB, kStop = 0xFC;
    static const uint32_t kNone = 0xFFFFFFFF;
    static const uint32_t kUsbWaitMs = 5;
    FeedbackRing* feedback_ = nullptr;
    uint32_t feedback_sent_ = 0; // its bytes the UART took
    uint32_t rewinds_ = 0; // the looper's Rewinds() as last seen
    bool usb_waiting_ = false;        // USB didn't take the next message
    uint32_t usb_waiting_since_ = 0;
    static const uint32_t kMaxBunch = 4; // ticks passed on in one block, at most
    static const uint8_t kTrs = 1, kUsb = 2, kAllPorts = kTrs | kUsb; // ports, as a mask
    /** Bytes a ring keeps for the transport, which ticks don't take: Stop, Song Position and
     *  Continue, Stop, Start (#166) */
    static const size_t kTransportRoom = 8;

    /** The clock's bytes for a port, filled by the audio callback; read by it (the jack's)
     *  or by MainLoop (USB's), which only sees whole messages */
    typedef ByteRing<32> Ring;

    /** ring's next message, left in: its length (a Song Position's 3, else 1), 0 for none */
    static size_t PeekMessage(const Ring& ring, uint8_t* msg)
    {
        if (ring.Empty())
            return 0;
        const size_t n = ring.Front() == 0xF2 ? 3 : 1;
        for (size_t i = 0; i < n; i++)
            msg[i] = ring.At(i);
        return n;
    }

    /** The ports the setting has but not_to */
    uint8_t Ports(MidiClock::Source not_to) const
    {
        return (not_to != MidiClock::Source::TRS ? kTrs : 0)
               | (ports_ == MidiOutPorts::TRS_USB && not_to != MidiClock::Source::USB ? kUsb : 0);
    }

    /** A message to ports: the ones it went to. A tick leaves kTransportRoom in a ring. Not
     *  inlined: Process sends from many places */
    __attribute__((noinline)) uint8_t Send(const uint8_t* msg, size_t n, uint8_t ports)
    {
        uint8_t sent = 0;
        if ((ports & kTrs) && Push(trs_, msg, n))
            sent |= kTrs;
        if ((ports & kUsb) && Push(usb_, msg, n))
            sent |= kUsb;
        return sent;
    }
    inline uint8_t Send(const uint8_t* msg, size_t n, MidiClock::Source not_to)
    {
        return Send(msg, n, Ports(not_to));
    }
    inline uint8_t Send(uint8_t status, MidiClock::Source not_to) { return Send(&status, 1, not_to); }
    static bool Push(Ring& ring, const uint8_t* msg, size_t n)
    {
        if (msg[0] == kTick && ring.Room() < n + kTransportRoom)
            return false; // full but for the transport's room: the UART or MainLoop is stuck
        return ring.Push(msg, n);
    }

    /** Start, Continue or Stop to ports: the gear plays there, or not, where it went */
    void Transport(uint8_t status, uint8_t ports)
    {
        const uint8_t sent = Send(&status, 1, ports);
        if (status == kStop)
            playing_ &= ~sent;
        else
            playing_ |= sent;
    }

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

    /** The clock's bytes first: a real-time one (a tick) goes at once, also between a
     *  feedback message's bytes, as MIDI lets it; the others (Start, Stop, Song Position)
     *  wait for the end of one. Then MIDI feedback's, one a block: the UART has sent it by
     *  the next block (0.32 ms of 0.5), so a tick never waits behind feedback */
    void DrainTrs()
    {
        bool fed = false;
        for (;;)
        {
            const bool mid = feedback_sent_ % 3 != 0; // feedback's are 3-byte CCs
            if (!trs_.Empty() && (trs_.Front() >= 0xF8 || !mid))
            {
                if (!clock_->TrsPut(trs_.Front()))
                    return;
                trs_.Pop();
            }
            else if (!fed && !feedback_->Empty() && (trs_.Empty() || mid))
            {
                if (!clock_->TrsPut(feedback_->Front()))
                    return;
                feedback_->Pop();
                feedback_sent_++;
                fed = true;
            }
            else
                return;
        }
    }

    MidiClock* clock_ = nullptr;
    MidiOutPorts ports_ = MidiOutPorts::OFF;
    Ring trs_, usb_;
    uint32_t now_ = 0;        // samples, from the first block
    uint32_t last_ticks_ = 0; // MidiClock's count at the last block
    bool was_loop_ = false, was_paused_ = false, was_passing_ = false;
    uint8_t playing_ = 0;     // the ports whose gear plays: a Start or Continue went there, no Stop since
    uint8_t start_ = 0;       // the ports a new loop's Start waits for its first pulse for
    uint32_t continue_at_ = kNone; // the loop pulse the Continue waits for
    bool half_due_ = false;   // a halfway tick waits for half_at_
    uint32_t half_at_ = 0;
    uint32_t last_tick_at_ = 0; // when the last tick went out
};

} // namespace chompi
