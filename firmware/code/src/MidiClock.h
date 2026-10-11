/** @file MidiClock.h
 *  @brief MIDI clock input over TRS (UART) and USB, used to quantize looper recordings.
 *
 *  Only TimingClock ticks (24 PPQN) are used here; every other message goes to the listener,
 *  MidiControl.h, which plays the panel from it.
 *  Which input's ticks count is the clock source (the settings page, SettingsPage.h): Auto,
 *  TRS, USB or internal. In Auto, the first source that ticks gets locked, and ticks from
 *  the other source are ignored until the locked one has been silent for
 *  kClockTimeoutSamples, after which whichever source ticks next takes over. TRS and USB
 *  count only that input's ticks, internal none: the tempo then comes from the loop, taps or
 *  the last tempo (TempoClock.h). Every other message is read from both inputs whatever the
 *  source; MidiControl.h takes Start and Stop only from the chosen one.
 *
 *  Time is measured in samples on the audio clock (the running sample count passed into
 *  Process()), so tick timestamps line up with the audio the looper records. They're only as
 *  precise as the audio block (24 samples = 0.5ms) plus the transport's own latency.
 *
 *  The UART/USB setup is from WAVE's MidiManager.h. See LOOPER.md 1.4. MIDI out (MidiOut.h)
 *  sends through it too: a byte at a time to the jack's UART (TrsPut), and over USB.
 *
 *  The tempo factor (the settings page, SettingsPage.h) makes FRIZZ follow the clock at half
 *  or double its tempo: what the looper and TempoClock read (the ticks, their period, the
 *  tempo) counts every tick twice, or every other one. The event log keeps what was sent
 *  (SenderBpm, SenderLocks), since a bug report's card carries the factor.
 *
 *  One other message is read: the SysEx F0 7D 43 48 10 F7 asks for a restart, so a computer
 *  can get the CHOMPI back to the multi-firmware launcher without the power switch
 *  (firmware/flash.py). The header is the launcher's (7D, non-commercial, then "CH";
 *  github.com/sfaber02/CHOMPI, firmware/chompi-launcher/PROTOCOL.md); 10 is FRIZZ's own
 *  command, above the launcher's 01-04. MainLoop does the restart (chompi_main.cpp).
 */
#pragma once
#include <string.h>
#include "daisy.h"

using namespace daisy;

namespace chompi
{

// a restart, after the F0: the launcher's header, FRIZZ's command
static const uint8_t kRestartSysEx[] = {0x7D, 0x43, 0x48, 0x10};

static const uint32_t kTicksPerBeat = 24;
static const uint32_t kBeatsPerBar = 4;                 // 4/4 fixed
static const uint32_t kTicksPerBar = kTicksPerBeat * kBeatsPerBar;
static const uint32_t kClockTimeoutSamples = 24000;     // 0.5s at 48kHz
static const float kHold = .125f; // an interval off the period by more waits for the next

// How FRIZZ follows the clock: at half its tempo, as sent, or at double
enum class ClockFactor : uint8_t
{
    HALF,
    ONE,
    DOUBLE,
};

// Whose ticks count: the first input that ticks, the jack's, USB's, or none
enum class ClockSource : uint8_t
{
    AUTO,
    TRS,
    USB,
    INTERNAL,
};
static const uint8_t kNumClockSources = 4;

// Where MIDI out (MidiOut.h) goes: nowhere, the jack, or the jack and USB
enum class MidiOutPorts : uint8_t
{
    OFF,
    TRS,
    TRS_USB,
};
static const uint8_t kNumMidiOutPorts = 3;

/** Whether a message from an input (usb, or the jack) passes the source: TRS and USB only
 *  their own, Auto and internal both */
inline bool FromSource(ClockSource source, bool usb)
{
    return source == ClockSource::TRS ? !usb : source == ClockSource::USB ? usb : true;
}

/** A Start (0xFA), Continue (0xFB) or Stop (0xFC) as its status byte, 0 for any other
 *  message: for MIDI out to pass on (MidiClock) and the loop to follow (MidiControl.h) */
inline uint8_t TransportByte(const MidiEvent& e)
{
    if (e.type != SystemRealTime)
        return 0;
    return e.srt_type == Start ? 0xFA : e.srt_type == Continue ? 0xFB : e.srt_type == Stop ? 0xFC : 0;
}

class MidiClock
{
public:
    enum class Source
    {
        NONE,
        TRS,
        USB,
    };

    MidiClock() {}
    ~MidiClock() {}

    void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;

        MidiUartHandler::Config uart_midi_cfg;
        uart_midi.Init(uart_midi_cfg);
        uart_midi.StartReceive();

        MidiUsbHandler::Config usb_midi_cfg;
        usb_midi_cfg.transport_config.periph = MidiUsbTransport::Config::EXTERNAL;
        usb_midi.Init(usb_midi_cfg);
        usb_midi.Listen();

        source_ = Source::NONE;
        ticks_ = 0;
        locks_ = 0;
        changes_ = 0;
        counted_at_ = 0;
        factor_ = ClockFactor::ONE;
        period_ = 0.f;
        held_ = 0.f;
    }

    /** Drains both MIDI inputs. Call once per audio block from the audio callback.
     *  @param now running sample count at the start of this block */
    void Process(uint32_t now)
    {
        if (source_ != Source::NONE && now - last_tick_ > kClockTimeoutSamples)
            source_ = Source::NONE;

        // UART disables itself on errors such as overrun; Listen() restarts it
        uart_midi.Listen();

        while (uart_midi.HasEvents())
            HandleEvent(uart_midi.PopEvent(), Source::TRS, now);

        while (usb_midi.HasEvents())
            HandleEvent(usb_midi.PopEvent(), Source::USB, now);
    }

    /** True while a source is locked, i.e. a tick arrived within the timeout */
    inline bool HasClock() const { return source_ != Source::NONE; }
    /** True while USB's ticks are the ones counted: for the event log */
    inline bool UsbClock() const { return source_ == Source::USB; }

    /** How many times a source has locked, or the factor changed: a change means the clock
     *  was lost and found again, maybe within one block, or counts differently now, so a
     *  count of ticks across it means nothing */
    inline uint32_t GetLocks() const { return locks_ + changes_; }

    /** Running count of ticks from the locked source, by the factor. Only differences are
     *  meaningful: the looper snapshots it at the record press and counts from there. */
    inline uint32_t GetTicks() const { return ticks_; }

    /** Sample count of the most recent tick counted */
    inline uint32_t GetLastTickTime() const { return counted_at_; }

    /** Smoothed tick period in samples, by the factor, 0 until two ticks have arrived from one
     *  source */
    inline float GetTickPeriod() const
    {
        return factor_ == ClockFactor::HALF     ? 2.f * period_
               : factor_ == ClockFactor::DOUBLE ? .5f * period_
                                                : period_;
    }

    /** Smoothed tempo, by the factor, 0 when there's no period yet */
    inline float GetBpm() const { return Bpm(GetTickPeriod()); }

    /** The tempo and the locks as sent, whatever the factor: for the event log */
    inline float SenderBpm() const { return Bpm(period_); }
    inline uint32_t SenderLocks() const { return locks_; }

    /** From MainLoop: the factor, from the next tick on */
    void SetFactor(ClockFactor factor)
    {
        if (factor == factor_)
            return;
        ScopedIrqBlocker irq;
        factor_ = factor;
        changes_++;
    }
    inline ClockFactor Factor() const { return factor_; }

    /** From MainLoop or a SysEx: whose ticks count. A source it leaves out is let go at once,
     *  as a lost clock is (HasClock, GetLocks) */
    void SetSource(ClockSource source)
    {
        ScopedIrqBlocker irq;
        if (source == setting_)
            return;
        setting_ = source;
        if (source_ != Source::NONE && !Counts(source_))
        {
            source_ = Source::NONE;
            changes_++;
        }
    }
    inline ClockSource GetSource() const { return setting_; }

    /** True once a restart was asked for over MIDI, from either input */
    inline bool RestartRequested() const { return restart_; }

    /** Who gets every message but the clock, as it's read: in the audio callback */
    typedef void (*Listener)(void* context, const MidiEvent& event, bool usb);
    void SetListener(Listener listener, void* context)
    {
        listener_ = listener;
        listener_context_ = context;
    }

    /** A message out over USB. From MainLoop only: the transport waits for the bus */
    void SendUsb(uint8_t* bytes, size_t size) { usb_midi.SendMessage(bytes, size); }

    /** One byte out of the MIDI jack if the UART can take it now, without waiting: from the
     *  audio callback (MidiOut.h). False while it's busy: at 31250 baud a byte takes 0.32 ms,
     *  and the UART holds one more while it sends */
    bool TrsPut(uint8_t byte)
    {
#if defined(__arm__)
        // the MIDI UART (MidiUartHandler's default USART1); libDaisy only sends blocking
        if (!(USART1->ISR & USART_ISR_TXE_TXFNF))
            return false;
        USART1->TDR = byte;
        return true;
#else
        return uart_midi.GetMutableTransport().GetUartHandle().PutByte(byte);
#endif
    }

    /** The input whose ticks are counted now, NONE without a clock */
    inline Source Locked() const { return source_; }

    /** A Start (0xFA), Continue (0xFB) or Stop (0xFC) from an input whose clock would count
     *  (the one locked to, or either before a lock: a DAW sends Start before its first tick),
     *  the last of this block's, taken, and the input it came on: for MIDI out to pass on
     *  (MidiOut.h); 0 for none */
    inline uint8_t TakeTransport(Source& from)
    {
        const uint8_t t = transport_in_;
        transport_in_ = 0;
        from = transport_from_;
        return t;
    }

private:
    inline float Bpm(float period) const
    {
        return period > 0.f ? sample_rate_ * 60.f / (period * kTicksPerBeat) : 0.f;
    }

    /** Whether the source setting lets an input's ticks count */
    inline bool Counts(Source from) const
    {
        return setting_ != ClockSource::INTERNAL && FromSource(setting_, from == Source::USB);
    }

    /** One step of the period's smoothing, against block-granularity and USB-frame jitter.
     *  A pair of ticks sent close together (in one USB packet, or a late one catching up)
     *  comes in as a long interval and a short one: smoothed one by one, they pulled the
     *  tempo a tenth up and down, and it was read mostly after the short one, 5% fast. A lone
     *  late tick does the same in smaller steps (10 ms late at 120 BPM: the FX at 114). So an
     *  interval off by more than kHold waits for the next tick (HandleEvent) */
    inline void Smooth(float interval)
    {
        period_ = period_ > 0.f ? period_ + .1f * (interval - period_) : interval;
    }

    void HandleEvent(const MidiEvent& event, Source from, uint32_t now)
    {
        if (event.type == SystemCommon && event.sc_type == SystemExclusive)
        {
            restart_ = restart_
                       || (event.sysex_message_len == sizeof(kRestartSysEx)
                           && memcmp(event.sysex_data, kRestartSysEx, sizeof(kRestartSysEx)) == 0);
        }
        const uint8_t transport = TransportByte(event);
        if (transport && Counts(from) && (source_ == Source::NONE || from == source_))
        {
            transport_in_ = transport;
            transport_from_ = from;
        }
        if (event.type != SystemRealTime || event.srt_type != TimingClock)
        {
            if (listener_)
                listener_(listener_context_, event, from == Source::USB);
            return;
        }

        if (!Counts(from))
            return;
        if (source_ == Source::NONE)
        {
            // new lock: the first tick only gives a timestamp, no period yet
            source_ = from;
            period_ = 0.f;
            locks_++;
            held_ = 0.f;
            skipped_ = true; // at half, the first tick counts
        }
        else if (from != source_)
        {
            return;
        }
        else
        {
            const float interval = static_cast<float>(now - last_tick_);
            if (held_ > 0.f)
            {
                // an interval off the period waited for this one: if together they come
                // closer to two periods than it alone to one, they were a pair (two ticks
                // sent together, a late or early one, one catching up), so their mean is the
                // period; otherwise the held one was real, a tempo change or a lost tick
                if (fabsf(held_ + interval - 2.f * period_) < fabsf(held_ - period_))
                    Smooth(.5f * (held_ + interval));
                else
                {
                    Smooth(held_);
                    Smooth(interval);
                }
                held_ = 0.f;
            }
            else if (period_ > 0.f && fabsf(interval - period_) > kHold * period_)
                held_ = interval; // waits for the next tick (above)
            else
                Smooth(interval);
        }

        last_tick_ = now;
        // counted twice, as it came, or every other one
        if (factor_ == ClockFactor::HALF)
        {
            skipped_ = !skipped_;
            if (skipped_)
                return;
        }
        ticks_ += factor_ == ClockFactor::DOUBLE ? 2 : 1;
        counted_at_ = now;
    }

    MidiUartHandler uart_midi;
    MidiUsbHandler usb_midi;

    float sample_rate_;
    Source source_;
    uint32_t ticks_;      // counted, by the factor
    uint32_t locks_;
    uint32_t changes_;    // of the factor
    uint32_t last_tick_;  // the last tick that came
    uint32_t counted_at_; // the last tick counted
    volatile ClockFactor factor_;
    volatile ClockSource setting_ = ClockSource::AUTO;
    bool skipped_ = false; // at half: the last tick wasn't counted
    float period_;
    float held_ = 0.f; // an interval waiting for the next tick, 0 for none
    volatile bool restart_ = false;
    uint8_t transport_in_ = 0; // TakeTransport's
    Source transport_from_ = Source::NONE;
    Listener listener_ = nullptr;
    void* listener_context_ = nullptr;
};

} // namespace chompi
