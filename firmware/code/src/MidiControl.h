/** @file MidiControl.h
 *  @brief FRIZZ played and inspected over MIDI (MANUAL.md, "MIDI"): notes press the keys,
 *  CCs set a knob or a latch outright, program changes recall scenes, the transport plays the
 *  loop, and FRIZZ's own SysEx presses keys, turns knobs and asks what the device shows, for a
 *  computer to test it (firmware/remote.py).
 *
 *  Channels (#97): the one set (MasterSettings::midi_channel, 16 at first) is the global
 *  channel, with the notes, program changes, the looper, the scenes, the gains, every latch
 *  and the knobs of the effect they edit (the focus). Channels 1-15 are the effect block, one
 *  per white key, left to right (FxSlots.h: 14 the chaos key, 15 the compressor), the same CCs
 *  on each: a DAW's track per effect. On the global channel its own map wins; on all channels
 *  (0) only the global map counts, and the block is off. Over the jack the block is a setting
 *  (BlockTrs, the settings page's), since a chain of gear shares that cable's channels.
 *
 *  MidiClock.h reads both inputs in the audio callback and hands every message but the clock
 *  to Receive(), which only notes what came:
 *  - the notes, as keys in the order they came (TakeKeys, merged with the hand's in ui.h, so a
 *    key held by both is pressed once);
 *  - the transport, for the looper in the same callback (TakeSync);
 *  - for the play page, which takes them in MainLoop (NormalPage::Remote): the latest value of
 *    every knob, gain and fader, every latch and the focus, marked as changed (so a DAW's dense
 *    automation is a table, never a queue that fills), and in the order they came, what acts
 *    once: a morph, its stop, a tap, a program change. Latches and those act as they change,
 *    not on every message, so an automation lane sending its value again doesn't fire them
 *    again; one that doesn't fit the queue isn't taken as sent, so the next message tries
 *    again;
 *  - a query to answer, which goes out over USB from MainLoop (Reply), never from the
 *    interrupt.
 *  Nothing that comes in by CC is written to the card (#97 D6).
 *
 *  Every message that changes something is logged for a bug report (EventLog.h) as it came, so
 *  the replay plays it in again; a scene sent over SysEx isn't, so a replay starts from the
 *  card's scenes.
 *
 *  SysEx: F0 7D 43 48 CMD DATA F7, the launcher's header (MidiClock.h). FRIZZ's commands start
 *  at 0x10, above the launcher's 01-07, and an answer is the command | 0x40, as the
 *  launcher's are: 0x50 and up, so no answer of FRIZZ's passes as the launcher's.
 *
 *  0x3E is the developers' (kCmdDev, firmware/README.md): F0 7D 43 48 3E SUB SEQ ARGS F7, over
 *  USB only, answered F0 7D 43 48 7E SUB SEQ STATUS DATA F7, SEQ the request's, so a tool
 *  matches its answer. It has a queue of its own (kDevQueue), apart from the other queries'
 *  one slot, and every request taken is answered, with a status if nothing else: a tool that
 *  gets no answer asks again, which every request can take (they set, never step).
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include "daisy.h"
#include "hardware.h"
#include "EventLog.h"
#include "MidiClock.h"

// once each and not in a hurry: FRIZZ's code space is tight (SRAM_EXEC), as EventLog's
#define MIDI_CONTROL_ONCE __attribute__((noinline, optimize("Os")))

namespace chompi
{

namespace midimap
{
// Notes, on the global channel: the 25 keys as a keyboard from kBaseNote, CHOMPI, PLAY and
// LOOP below it, the knob presses below those
static const uint8_t kBaseNote = 48;   // KEY_1, the lowest white key
static const uint8_t kChompiNote = 45; // then PLAY 46, LOOP 47
static const uint8_t kKnobNote = 36;   // knobs 1-4, the transport, VOLUME: 36-41
static const uint8_t kNumKnobs = 6;

// The effect block: channel k is white key k's
static const uint8_t kBlockChannels = 15;
static const uint8_t kKeyParams = 8; // an effect's knobs, both pages (kNumFxParams)

// On an effect channel, and on the global one for the effect in focus
static const uint8_t kKnobCC = 14;  // 14-17 page 1's knobs 1-4, 18-21 page 2's
static const uint8_t kLatchCC = 22; // 64 and up on, below off: acts as it crosses 64
// The global channel's
static const uint8_t kMorphCC = 3;      // morph to the scene 0-4, as the value changes
static const uint8_t kMorphBarsCC = 9;  // over how many bars, 1-8 (kMaxMorphBars)
static const uint8_t kFocusCC = 23;     // 1-15: the white key whose knobs 14-22 reach
static const uint8_t kCompCC = 24;      // 24-31: the compressor's knobs, as channel 15's 14-21
static const uint8_t kFaderCC = 80;     // the crossfader: a running morph from its start to its scene
static const uint8_t kStopMorphCC = 81; // stops a morph where it is, crossing 64 upward
static const uint8_t kTapCC = 82;       // a tempo tap, crossing 64 upward
static const uint8_t kOutGainCC = 85, kInGainCC = 86, kMixCC = 87;
static const uint8_t kHpCueCC = 89;     // the headphone feed as VOLUME's knob: 0 the input, 127 the master
static const uint8_t kSpeedCC = 90;     // the loop's speed in semitones: 0 1/16x, 64 1x, 127 2x
static const uint8_t kKeyLatchCC = 102; // 102-115: white key 1-14's latch, as 22
static const uint8_t kAllSoundOffCC = 120, kAllNotesOffCC = 123;
static const uint8_t kMorphBarsMax = 8;
// Free on purpose, so what hosts and keyboards send by themselves reaches nothing: 0/32, 1,
// 2, 4, 7, 10, 11 and 64-79; 6/38 and 96-101, an (N)RPN's data and selects (a DAW's
// pitch-bend range); 88; 91-95; 121 (Reset All Controllers) and the channel modes. 46-53, the
// LSBs of 14-21, stay free for a 14-bit mode later

/** What TakeValue gives: white key k's knob p (both pages) as k * kKeyParams + p, the
 *  compressor's as the 15th key's, then the rest */
enum Value : uint8_t
{
    kOutGain = kBlockChannels * kKeyParams,
    kInGain,
    kMix,
    kHpCue,
    kSpeed,
    kFader,
    kNumValues,
};
} // namespace midimap

// FRIZZ's SysEx commands (after F0 7D 43 48)
enum MidiCmd : uint8_t
{
    kCmdKey = 0x11,      // KEY DOWN: a key (Hardware::SwId) held or let go
    kCmdTurn = 0x12,     // ENC DETENTS: encoder 1-6 (SW1-SW6), detents in 7-bit two's complement
    kCmdSetting = 0x13,  // ID VALUE: 0 the channel (0 all, 1-16), 1 transport following (0/1),
                         // 2 the clock source (0 Auto, 1 TRS, 2 USB, 3 internal), 3 MIDI
                         // out (0 off, 1 the jack, 2 the jack and USB)
    kCmdSwitch = 0x14,   // POS: the mode switch, 0 as it stands, 1 down, 2 up, until power-off
    kCmdState = 0x20,    // what the play page shows
    kCmdParams = 0x21,   // FX: an effect's knobs (12 the phaser's, 13 the chaos key's, 14 the
                         // compressor's), 14 bits each
    kCmdLeds = 0x22,     // PART: the LEDs as their bytes, part 0 the panel's, 1-3 the keys'
    kCmdLoad = 0x23,     // the audio callback's load since the last ask: max and mean, 14 bits
                         // each, then the blocks longer than a block (crackles) and the
                         // blocks, 3 7-bit groups each
    kCmdSettings = 0x24, // the channel, transport following, the clock source, MIDI out
    kCmdSceneGet = 0x30, // SLOT PART: a scene, in kSceneParts parts
    kCmdScenePut = 0x31, // SLOT PART DATA: likewise; the last part stores it
    kCmdDev = 0x3E,      // SUB SEQ ARGS: the developers' (namespace dev)
    kCmdReply = 0x40,
};

/** kCmdDev's sub-commands, their groups and statuses. Numbers in 7-bit groups, the highest
 *  first; a knob's value in 14 bits as kCmdParams has it */
namespace dev
{
static const uint8_t kVersion = 1;
enum Sub : uint8_t
{
    IDENT = 0,   // -> version, kind (0 FRIZZ, 1 the bench), the image's CRC-32 (5) and length
                 //    (3) as loaded (zlib.crc32 of the .bin; 0 on the twin), uptime ms (5),
                 //    MainLoop's passes (5), RCC_RSR at boot (5), flags (kRamOnly)
    PERSIST = 1, // 0: the card isn't written until a restart (SceneStore::KeepInRam); -> flags
    GET = 2,     // GROUP -> GROUP COUNT, then COUNT values, 14 bits each
    SET = 3,     // GROUP INDEX VALUE(2): as the knob or setting would; -> GROUP INDEX
    BENCH = 4,   // 0 -> its state (Bench::Report); 1 OFFSET(3) -> up to kBenchChunk bytes
                 //    of cpu.txt from there. FRIZZ-bench.bin only
};
enum Status : uint8_t
{
    OK = 0,
    BAD = 1,      // an unknown sub-command, group or index, or a value the setting doesn't take
    NOT_HERE = 2, // not in this build (BENCH in FRIZZ.bin)
    NOT_YET = 3,  // the bench is still running
};
// the groups of GET and SET: an effect's knobs by FxId (0-7: page 1, then page 2), then
// numbers that don't move as effects join
static const uint8_t kGroupComp = 0x20;     // the compressor's knobs, 0-7
static const uint8_t kGroupLatch = 0x21;    // the latches, by FxId: 0 or 1
static const uint8_t kGroupVolume = 0x22;   // 0 output gain, 1 input gain, 2 mix, 3 headphones
static const uint8_t kGroupSettings = 0x23; // the card's settings, as masterfile::kLines lists
                                            // them, their values as the file has them
static const uint8_t kRamOnly = 1;          // IDENT's flags: PERSIST 0 is on
static const size_t kBenchChunk = 32;

/** What chompi_main.cpp knows for IDENT and BENCH */
struct Info
{
    uint8_t kind = 0;                 // 0 FRIZZ, 1 the bench
    const uint8_t* image = nullptr;   // the image as the bootloader loaded it
    uint32_t image_len = 0;
    uint32_t crc = 0;                 // its CRC-32, once worked out (on the first IDENT)
    bool crc_done = false;
    volatile uint32_t passes = 0;     // MainLoop's
    uint32_t reset_cause = 0;         // RCC_RSR at boot
    // the bench's answer to BENCH (Bench::Report) into d: its length, or -1 for NOT_YET
    int (*bench)(uint8_t part, uint32_t offset, uint8_t* d) = nullptr;
};
} // namespace dev
static const uint8_t kMidiHeader[] = {0x7D, 0x43, 0x48};
// a scene over SysEx: 4 parts of 4 effects' page-1 knobs, the same 4 for page 2, then the
// latches. The last part of a page has the phaser, the chaos key and room for 2 more (zeros)
static const size_t kFxPerPart = 4;
static const size_t kFxParts = 4;
static const size_t kSceneParts = 2 * kFxParts + 1;
static const uint16_t kMidiMax14 = 16383;

/** A MIDI value as a knob's 0-1, with the centre exactly on .5: 7 bits (64) or 14 (8192) */
inline float MidiToKnob(uint16_t v, bool fine)
{
    const float centre = fine ? 8192.f : 64.f, top = fine ? 16383.f : 127.f;
    const float f = static_cast<float>(v);
    return f <= centre ? .5f * f / centre : .5f + .5f * (f - centre) / (top - centre);
}

/** A knob's 0-1 as 14 bits, the other way round */
inline uint16_t KnobToMidi14(float k)
{
    k = k < 0.f ? 0.f : (k > 1.f ? 1.f : k);
    const float v = k <= .5f ? k * 2.f * 8192.f : 8192.f + (k - .5f) * 2.f * 8191.f;
    return static_cast<uint16_t>(v + .5f);
}

class MidiControl
{
public:
    /** What a SysEx asked to be answered, for the play page */
    struct Query
    {
        uint8_t cmd, a, b;
        uint8_t data[32]; // a scene part's
        uint8_t len;
    };
    /** A kCmdDev request */
    struct DevQuery
    {
        uint8_t sub, seq, len;
        uint8_t arg[5];
    };
    /** What acts once, for the play page, in the order it came (TakeEvent) */
    struct Event
    {
        enum Kind : uint8_t
        {
            MORPH,      // arg: the scene 0-4 | the bars (1-8) << 3
            STOP_MORPH, //
            TAP,        // at ms
            PROGRAM,    // arg: the scene 0-4
        };
        uint8_t kind, arg;
        uint16_t ms; // when it came, System::GetNow()'s lower 16 bits
    };
    /** The transport, for the looper (Looper::Follow) */
    struct Sync
    {
        uint8_t msg;   // 0xFA Start, 0xFB Continue, 0xFC Stop, 0xF2 Song Position
        uint16_t spp;  // Song Position's, in 16ths
        uint32_t tick; // the clock's last tick as it came (MidiClock::GetLastTick)
    };

    void Init(MidiClock* clock, EventLog* log, uint8_t channel, bool transport, float tick_freq,
              float block_seconds)
    {
        clock_ = clock;
        log_ = log;
        channel_ = channel;
        transport_ = transport;
        ticks_per_block_ = tick_freq * block_seconds;
        block_ticks_ = static_cast<uint32_t>(ticks_per_block_);
        clock_->SetListener(&MidiControl::Listen, this);
    }

    // ---- from the audio callback ----

    /** The keys MIDI's notes hold for this block, by Hardware::SwId: always the play page's.
     *  The notes are taken in the order they came, up to one that would change a key a second
     *  time; it waits for the next block, so a key tapped faster than a block is still pressed
     *  and let go */
    uint64_t TakeKeys()
    {
        uint64_t changed = 0;
        while (note_read_ != note_write_)
        {
            const uint8_t n = notes_[note_read_ % kNoteQueue];
            const uint64_t bit = n == kAllKeysUp ? keys_ : 1ull << (n & kNoteKey);
            if (changed & bit)
                break;
            changed |= bit;
            ApplyNote(n);
            note_read_++;
        }
        return keys_;
    }
    /** The keys FRIZZ's SysEx holds (kCmdKey): the panel's, as the hand's, so they reach the
     *  settings page too (ui.h) */
    inline uint64_t PanelKeys() const { return panel_keys_; }
    /** The mode switch as SysEx set it (kCmdSwitch): 0 as it stands, 1 down, 2 up */
    inline uint8_t Switch() const { return switch_; }

    /** A detent FRIZZ's SysEx turned knob 0-5 (the knobs' order: 1-4, transport, VOLUME,
     *  kCmdTurn), taken: +-1, or 0. One a block, as a hand's come: the play page counts stepped
     *  knobs and the transport's speed steps detent by detent */
    inline int TakePanelTurn(size_t knob) { return TakeOne(panel_turns_[knob]); }

    /** The transport that came, taken in order: for the looper, before the engine runs */
    inline bool TakeSync(Sync& s)
    {
        if (sync_read_ == sync_write_)
            return false;
        s = syncs_[sync_read_ % kSyncQueue];
        sync_read_++;
        return true;
    }

    /** The callback's own time in system ticks, for kCmdLoad */
    inline void BlockTime(uint32_t ticks)
    {
        const float load = ticks / ticks_per_block_;
        load_max_ = load > load_max_ ? load : load_max_;
        load_sum_ += load;
        load_blocks_++;
        load_over_ += ticks > block_ticks_; // longer than a block: a crackle
    }

    // ---- from MainLoop (the play page) ----

    /** The next value changed since it was last taken (midimap::Value) and its 7 bits; false
     *  when there's none */
    MIDI_CONTROL_ONCE bool TakeValue(uint8_t& id, uint8_t& raw)
    {
        daisy::ScopedIrqBlocker irq;
        for (size_t w = 0; w < kValueWords; w++)
        {
            if (!changed_[w])
                continue;
            const uint32_t bit = changed_[w] & (~changed_[w] + 1);
            changed_[w] &= ~bit;
            id = static_cast<uint8_t>(w * 32 + __builtin_ctz(bit));
            raw = values_[id];
            return true;
        }
        return false;
    }

    /** The next thing that acts once, in the order they came */
    inline bool TakeEvent(Event& e)
    {
        daisy::ScopedIrqBlocker irq;
        if (event_read_ == event_write_)
            return false;
        e = events_[event_read_ % kEventQueue];
        event_read_++;
        return true;
    }

    /** A latch MIDI changed since it was last taken: its white key 0-13 and whether it's on */
    inline bool TakeLatch(uint8_t& key, bool& on)
    {
        daisy::ScopedIrqBlocker irq;
        if (!latch_changed_)
            return false;
        key = static_cast<uint8_t>(__builtin_ctz(latch_changed_));
        latch_changed_ &= latch_changed_ - 1;
        on = (latch_on_ >> key) & 1;
        return true;
    }

    /** The white key 0-14 CC 23 put in focus since last asked, for the knobs to edit */
    inline bool TakeFocus(uint8_t& key)
    {
        daisy::ScopedIrqBlocker irq;
        if (!focus_changed_)
            return false;
        focus_changed_ = false;
        key = focus_;
        return true;
    }

    /** The white key 0-14 whose knobs the global channel's 14-22 reach: the play page's, as
     *  the hand selects, and CC 23's at once, so a knob right after it goes to the new one */
    inline void SetFocus(uint8_t key) { focus_ = key; }

    /** A SysEx asking for an answer, taken */
    inline bool TakeQuery(Query& q)
    {
        daisy::ScopedIrqBlocker irq;
        if (!query_pending_)
            return false;
        q = query_;
        query_pending_ = false;
        return true;
    }

    /** A kCmdDev request, taken */
    inline bool TakeDev(DevQuery& q)
    {
        daisy::ScopedIrqBlocker irq;
        if (dev_read_ == dev_write_)
            return false;
        q = dev_queue_[dev_read_ % kDevQueue];
        dev_read_++;
        return true;
    }

    /** The settings changed over SysEx since last asked: they go to the card */
    inline bool TakeSettingsChanged()
    {
        daisy::ScopedIrqBlocker irq;
        const bool c = settings_changed_;
        settings_changed_ = false;
        return c;
    }

    inline uint8_t Channel() const { return channel_; }
    inline bool Transport() const { return transport_; }
    inline bool BlockTrs() const { return block_trs_; }

    /** The settings page's (SettingsPage.h): the channel (0 all), transport following, the
     *  effect block over the jack too, and how the clock is followed and from where */
    inline void SetChannel(uint8_t channel) { channel_ = channel; }
    inline void SetTransport(bool on) { transport_ = on; }
    inline void SetBlockTrs(bool on) { block_trs_ = on; }
    inline void SetClockFactor(ClockFactor factor) { clock_->SetFactor(factor); }
    inline void SetClockSource(ClockSource source) { clock_->SetSource(source); }
    inline ClockSource GetClockSource() const { return clock_->GetSource(); }
    /** Where MIDI out goes (MidiOut.h), read by the audio callback every block */
    inline void SetMidiOut(MidiOutPorts ports) { out_ = ports; }
    inline MidiOutPorts GetMidiOut() const { return out_; }

    /** The load since the last call, max and mean, in 1/1000 of a block, and the blocks
     *  that took longer than a block (over) of how many */
    void TakeLoad(uint16_t& max, uint16_t& mean, uint32_t& over, uint32_t& blocks)
    {
        daisy::ScopedIrqBlocker irq;
        max = static_cast<uint16_t>(load_max_ * 1000.f + .5f);
        mean = load_blocks_ ? static_cast<uint16_t>(load_sum_ / load_blocks_ * 1000. + .5) : 0;
        over = load_over_;
        blocks = load_blocks_;
        load_max_ = 0.f;
        load_sum_ = 0.;
        load_blocks_ = 0;
        load_over_ = 0;
    }

    /** What IDENT and BENCH answer from, chompi_main.cpp's */
    dev::Info dev;

    /** An answer to a query, over USB: F0, the header, cmd | 0x40, the data, F7. Kept to
     *  16 USB packets (64 bytes), one transfer */
    MIDI_CONTROL_ONCE void Reply(uint8_t cmd, const uint8_t* data, size_t len)
    {
        uint8_t msg[kMaxReply + 6];
        if (len > kMaxReply)
            len = kMaxReply;
        size_t n = 0;
        msg[n++] = 0xF0;
        for (uint8_t b : kMidiHeader)
            msg[n++] = b;
        msg[n++] = cmd | kCmdReply;
        for (size_t i = 0; i < len; i++)
            msg[n++] = data[i] & 0x7F;
        msg[n++] = 0xF7;
        clock_->SendUsb(msg, n);
    }
    static const size_t kMaxReply = 39; // + 6 = 45 bytes, 15 USB packets

private:
    static void Listen(void* self, const MidiEvent& e, bool usb)
    {
        static_cast<MidiControl*>(self)->Receive(e, usb);
    }

    MIDI_CONTROL_ONCE void Receive(const MidiEvent& e, bool usb)
    {
        using namespace midimap;
        if (e.type == SystemCommon && e.sc_type == SystemExclusive)
        {
            SysEx(e.sysex_data, e.sysex_message_len, usb);
            return;
        }
        if (e.type == SystemRealTime || (e.type == SystemCommon && e.sc_type == SongPositionPointer))
        {
            // the transport from the clock source's input only, from both in Auto and internal
            if (!transport_ || !FromSource(clock_->GetSource(), usb))
                return;
            const uint8_t t = e.type == SystemCommon ? 0xF2 : TransportByte(e);
            if (!t)
                return;
            const uint16_t spp = t == 0xF2 ? static_cast<uint16_t>(e.data[0] | e.data[1] << 7) : 0;
            if (static_cast<uint8_t>(sync_write_ - sync_read_) < kSyncQueue)
            {
                syncs_[sync_write_ % kSyncQueue] = {t, spp, clock_->GetLastTick()};
                sync_write_++;
            }
            Log(t, t == 0xF2 ? e.data[0] : 0, t == 0xF2 ? e.data[1] : 0, usb);
            return;
        }
        // libDaisy calls CC 120-127 channel mode messages, and then a CC after them in
        // running status too: they're all controllers here
        const MidiMessageType type = e.type == ChannelMode ? ControlChange : e.type;
        if (type > PitchBend)
            return;
        // the global channel's map, or on all of them; else the effect block's, over USB or
        // the jack if it's let
        const uint8_t channel = static_cast<uint8_t>(e.channel + 1);
        const bool global = !channel_ || channel == channel_;
        if (!global && (channel > kBlockChannels || !(usb || block_trs_)))
            return;

        const uint8_t status = static_cast<uint8_t>(0x80 | (type << 4) | e.channel);
        const uint8_t d0 = e.data[0], d1 = e.data[1];
        switch (type)
        {
        case NoteOff:
        case NoteOn:
        {
            const int key = NoteKey(d0);
            if (!global || key < 0)
                return;
            PushNote(static_cast<uint8_t>(key | (type == NoteOn && d1 > 0 ? kNoteDown : 0)));
            break;
        }
        case ControlChange:
            if (!(global ? GlobalControl(d0, d1) : KeyControl(channel - 1, d0, d1)))
                return;
            break;
        case ProgramChange:
            if (!global || d0 >= kNumSlots)
                return;
            Push(Event::PROGRAM, d0);
            break;
        default:
            return;
        }
        Log(status, d0, d1, usb);
    }

    /** A controller on white key key's channel, or the focus's: false for one it doesn't
     *  use or a latch that didn't cross. latch: its source for Crosses */
    bool KeyControl(uint8_t key, uint8_t cc, uint8_t v, uint8_t latch)
    {
        using namespace midimap;
        if (cc >= kKnobCC && cc < kKnobCC + kKeyParams)
        {
            Set(static_cast<uint8_t>(key * kKeyParams + cc - kKnobCC), v);
            return true;
        }
        // the compressor's channel has no latch
        if (cc != kLatchCC || key >= kBlockChannels - 1)
            return false;
        return Latch(latch, key, v);
    }
    inline bool KeyControl(uint8_t key, uint8_t cc, uint8_t v) { return KeyControl(key, cc, v, key); }

    /** A controller on the global channel: false for one it doesn't use, or a switch that
     *  didn't cross */
    bool GlobalControl(uint8_t cc, uint8_t v)
    {
        using namespace midimap;
        if (cc >= kKnobCC && cc <= kLatchCC)
            return KeyControl(focus_, cc, v, kFocusLatch);
        if (cc >= kCompCC && cc < kCompCC + kKeyParams)
            return KeyControl(kBlockChannels - 1, static_cast<uint8_t>(cc - kCompCC + kKnobCC), v);
        if (cc >= kKeyLatchCC && cc < kKeyLatchCC + kBlockChannels - 1)
            return Latch(static_cast<uint8_t>(kKeyLatches + cc - kKeyLatchCC), cc - kKeyLatchCC, v);
        bool on;
        switch (cc)
        {
        case kFocusCC:
            if (v < 1 || v > kBlockChannels)
                return false;
            focus_ = v - 1;
            focus_changed_ = true;
            return true;
        case kMorphCC:
            // taken as sent only once it's queued, so a dropped one acts on the next
            if (v == morph_last_
                || (v < kNumSlots && !Push(Event::MORPH, static_cast<uint8_t>(v | morph_bars_ << 3))))
                return false;
            morph_last_ = v;
            return v < kNumSlots;
        case kMorphBarsCC:
            morph_bars_ = v < 1 ? 1 : (v > kMorphBarsMax ? kMorphBarsMax : v);
            return true;
        case kStopMorphCC:
        case kTapCC:
        {
            // a crossing either way changes what the next does, so both are logged; one
            // upward is taken only once it's queued
            const uint8_t source = cc == kTapCC ? kTapSwitch : kStopSwitch;
            if (!Crosses(source, v, on) || (on && !Push(cc == kTapCC ? Event::TAP : Event::STOP_MORPH, 0)))
                return false;
            Side(source, on);
            return true;
        }
        case kAllSoundOffCC:
        case kAllNotesOffCC:
            PushNote(kAllKeysUp);
            panel_keys_ = 0;
            return true;
        case kOutGainCC:
            Set(kOutGain, v);
            return true;
        case kInGainCC:
            Set(kInGain, v);
            return true;
        case kMixCC:
            Set(kMix, v);
            return true;
        case kHpCueCC:
            Set(kHpCue, v);
            return true;
        case kSpeedCC:
            Set(kSpeed, v);
            return true;
        case kFaderCC:
            Set(kFader, v);
            return true;
        default:
            return false;
        }
    }

    /** A latch from switch source, for white key key, as it crosses 64 */
    bool Latch(uint8_t source, uint8_t key, uint8_t v)
    {
        bool on;
        if (!Crosses(source, v, on))
            return false;
        Side(source, on);
        const uint16_t bit = static_cast<uint16_t>(1u << key);
        latch_on_ = on ? latch_on_ | bit : latch_on_ & ~bit;
        latch_changed_ |= bit;
        return true;
    }

    /** Whether switch source changes side of 64 with v (on: to 64 and up); its first value
     *  is a change */
    bool Crosses(uint8_t source, uint8_t v, bool& on) const
    {
        const uint32_t bit = 1u << source;
        on = v >= 64;
        return !(known_ & bit) || ((high_ & bit) != 0) != on;
    }
    /** And it did */
    void Side(uint8_t source, bool on)
    {
        const uint32_t bit = 1u << source;
        known_ |= bit;
        high_ = on ? high_ | bit : high_ & ~bit;
    }

    void Set(uint8_t id, uint8_t v)
    {
        values_[id] = v;
        changed_[id / 32] |= 1u << (id % 32);
    }

    /** Something that acts once, for MainLoop; false, dropped, if MainLoop is that far behind */
    bool Push(uint8_t kind, uint8_t arg)
    {
        if (static_cast<uint8_t>(event_write_ - event_read_) >= kEventQueue)
            return false;
        events_[event_write_ % kEventQueue] = {kind, arg, static_cast<uint16_t>(daisy::System::GetNow())};
        event_write_ = event_write_ + 1;
        return true;
    }

    /** A note for TakeKeys; with the queue full, the oldest is applied without its block, so
     *  only a tap can be lost, never a key's state */
    void PushNote(uint8_t n)
    {
        if (static_cast<uint8_t>(note_write_ - note_read_) >= kNoteQueue)
            ApplyNote(notes_[note_read_++ % kNoteQueue]);
        notes_[note_write_ % kNoteQueue] = n;
        note_write_++;
    }
    void ApplyNote(uint8_t n)
    {
        const uint64_t bit = 1ull << (n & kNoteKey);
        keys_ = n == kAllKeysUp ? 0 : (n & kNoteDown ? keys_ | bit : keys_ & ~bit);
    }

    /** The key a note plays, by Hardware::SwId, or -1 */
    static int NoteKey(uint8_t note)
    {
        using namespace midimap;
        using S = Hardware::SwId;
        // a keyboard's octave: which of the white keys or the dark keys each semitone is
        static const int8_t kWhite[12] = {0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6};
        static const int8_t kDark[12] = {-1, 0, -1, 1, -1, -1, 2, -1, 3, -1, 4, -1};
        static const S kWhiteKeys[15] = {S::KEY_1,  S::KEY_2,  S::KEY_3,  S::KEY_4,  S::KEY_5,
                                         S::KEY_6,  S::KEY_7,  S::KEY_8,  S::KEY_9,  S::KEY_10,
                                         S::KEY_11, S::KEY_12, S::KEY_13, S::KEY_14, S::KEY_15};
        static const S kDarkKeys[10] = {S::KEY_16, S::KEY_17, S::KEY_18, S::KEY_19, S::KEY_20,
                                        S::KEY_21, S::KEY_22, S::KEY_23, S::KEY_24, S::KEY_25};
        static const int kKnobKeys[kNumKnobs] = {
            static_cast<int>(S::ENC_4_SW), static_cast<int>(S::ENC_1_SW),
            static_cast<int>(S::ENC_2_SW), static_cast<int>(S::ENC_3_SW), ENC_5_SW,
            static_cast<int>(S::ENC_6_SW)};

        if (note >= kKnobNote && note < kKnobNote + kNumKnobs)
            return kKnobKeys[note - kKnobNote];
        if (note == kChompiNote)
            return static_cast<int>(S::KEY_26);
        if (note == kChompiNote + 1)
            return static_cast<int>(S::KEY_27);
        if (note == kChompiNote + 2)
            return static_cast<int>(S::KEY_28);
        if (note < kBaseNote || note > kBaseNote + 24)
            return -1;
        const int off = note - kBaseNote, octave = off / 12, semi = off % 12;
        if (kWhite[semi] >= 0)
            return static_cast<int>(kWhiteKeys[octave * 7 + kWhite[semi]]);
        return static_cast<int>(kDarkKeys[octave * 5 + kDark[semi]]);
    }

    void SysEx(const uint8_t* d, size_t len, bool usb)
    {
        if (len < sizeof(kMidiHeader) + 1 || memcmp(d, kMidiHeader, sizeof(kMidiHeader)) != 0)
            return;
        const uint8_t cmd = d[3];
        const uint8_t* arg = d + 4;
        const size_t n = len - 4;
        switch (cmd)
        {
        case kCmdKey:
            if (n < 2 || arg[0] >= static_cast<uint8_t>(Hardware::SwId::SR_LAST))
                return;
            if (arg[1])
                panel_keys_ |= 1ull << arg[0];
            else
                panel_keys_ &= ~(1ull << arg[0]);
            break;
        case kCmdTurn:
        {
            static const uint8_t kKnobOf[6] = {1, 2, 3, 0, 4, 5}; // ui.h's encoder_map
            if (n < 2 || arg[0] < 1 || arg[0] > 6)
                return;
            panel_turns_[kKnobOf[arg[0] - 1]] += arg[1] < 64 ? arg[1] : static_cast<int>(arg[1]) - 128;
            break;
        }
        case kCmdSwitch:
            if (n < 1 || arg[0] > 2)
                return;
            switch_ = arg[0];
            break;
        case kCmdDev:
            // over USB, where the answer goes; a full queue drops it, unanswered
            if (!usb || n < 2 || static_cast<uint8_t>(dev_write_ - dev_read_) >= kDevQueue)
                return;
            {
                DevQuery& q = dev_queue_[dev_write_ % kDevQueue];
                q.sub = arg[0];
                q.seq = arg[1];
                q.len = static_cast<uint8_t>(n - 2 > sizeof(q.arg) ? sizeof(q.arg) : n - 2);
                memcpy(q.arg, arg + 2, q.len);
                dev_write_ = dev_write_ + 1;
            }
            return;
        case kCmdSetting:
            if (n < 2 || arg[0] > 3 || (arg[0] == 0 && arg[1] > 16)
                || (arg[0] == 2 && arg[1] >= kNumClockSources)
                || (arg[0] == 3 && arg[1] >= kNumMidiOutPorts))
                return;
            if (arg[0] == 0)
                channel_ = arg[1];
            else if (arg[0] == 1)
                transport_ = arg[1] != 0;
            else if (arg[0] == 2)
                clock_->SetSource(static_cast<ClockSource>(arg[1]));
            else
                out_ = static_cast<MidiOutPorts>(arg[1]);
            settings_changed_ = true;
            break;
        default:
            // the rest only ask, over USB, where the answer goes; an answer (a DAW echoing
            // FRIZZ's own back) asks nothing
            if (!usb || query_pending_ || cmd >= kCmdReply)
                return;
            query_.cmd = cmd;
            query_.a = n > 0 ? arg[0] : 0;
            query_.b = n > 1 ? arg[1] : 0;
            query_.len = n > 2 ? static_cast<uint8_t>(n - 2 > sizeof(query_.data)
                                                          ? sizeof(query_.data)
                                                          : n - 2)
                               : 0;
            memcpy(query_.data, arg + 2, query_.len);
            query_pending_ = true;
            return;
        }
        // the switch has one byte: what's after it is a longer SysEx's before it
        log_->Add(EventLog::SYSEX, cmd, static_cast<int16_t>((arg[0] << 7) | (n > 1 ? arg[1] : 0)));
    }

    void Log(uint8_t status, uint8_t d0, uint8_t d1, bool usb)
    {
        log_->Add(usb ? EventLog::MIDI_USB : EventLog::MIDI, status,
                  static_cast<int16_t>((d0 << 7) | d1));
    }

    MidiClock* clock_ = nullptr;
    EventLog* log_ = nullptr;
    volatile uint8_t channel_ = 0;
    volatile bool transport_ = false;
    volatile MidiOutPorts out_ = MidiOutPorts::OFF;
    volatile bool settings_changed_ = false;

    /** One detent of a count, taken: +-1, or 0 */
    static inline int TakeOne(volatile int& t)
    {
        const int v = t;
        const int one = v > 0 ? 1 : (v < 0 ? -1 : 0);
        t = v - one;
        return one;
    }

    // the notes, in the order they came: a key's SwId, | kNoteDown for a press, or kAllKeysUp
    static const uint8_t kNoteKey = 0x3F, kNoteDown = 0x40, kAllKeysUp = 0x80;
    static const size_t kNoteQueue = 32;
    uint8_t notes_[kNoteQueue];
    uint8_t note_write_ = 0, note_read_ = 0;
    uint64_t keys_ = 0;                // what TakeKeys has taken of them
    volatile uint64_t panel_keys_ = 0; // SysEx's
    volatile int panel_turns_[midimap::kNumKnobs] = {}; // SysEx's
    volatile uint8_t switch_ = 0;
    volatile bool block_trs_ = true;

    // by midimap::Value
    static const size_t kValueWords = (midimap::kNumValues + 31) / 32;
    uint8_t values_[midimap::kNumValues] = {};
    uint32_t changed_[kValueWords] = {};
    // what acts once, for MainLoop
    static const size_t kEventQueue = 16;
    Event events_[kEventQueue];
    volatile uint8_t event_write_ = 0, event_read_ = 0;
    // the transport, for the looper
    static const size_t kSyncQueue = 4;
    Sync syncs_[kSyncQueue];
    uint8_t sync_write_ = 0, sync_read_ = 0;

    uint8_t focus_ = 0; // the white key the global channel's 14-22 reach
    volatile bool focus_changed_ = false; // by CC 23
    // the latches by white key, and which changed since MainLoop took them
    uint16_t latch_on_ = 0;
    volatile uint16_t latch_changed_ = 0;
    // the switches' sides (Crosses), a bit each: the effect channels' latches (their key),
    // the focus's, the global latches (kKeyLatches + key), stop morph and tap
    static const uint8_t kFocusLatch = midimap::kBlockChannels;
    static const uint8_t kKeyLatches = kFocusLatch + 1;
    static const uint8_t kStopSwitch = kKeyLatches + midimap::kBlockChannels - 1;
    static const uint8_t kTapSwitch = kStopSwitch + 1;
    static_assert(kTapSwitch < 32, "a bit per switch");
    uint32_t known_ = 0, high_ = 0; // sent a value yet; it was 64 or more
    uint8_t morph_last_ = 0xFF;     // CC 3's last value, 0xFF for none yet
    uint8_t morph_bars_ = 1;

    Query query_;
    volatile bool query_pending_ = false;
    static const size_t kDevQueue = 8; // kCmdDev's requests, waiting for MainLoop
    DevQuery dev_queue_[kDevQueue];
    volatile uint8_t dev_write_ = 0, dev_read_ = 0;

    float ticks_per_block_ = 1.f;
    float load_max_ = 0.f;
    double load_sum_ = 0.; // a float stops adding ~0.45 a block after a few hours unread
    uint32_t load_blocks_ = 0;
    uint32_t load_over_ = 0;
    uint32_t block_ticks_ = 1;
};

} // namespace chompi
