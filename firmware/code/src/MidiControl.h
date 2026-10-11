/** @file MidiControl.h
 *  @brief FRIZZ played and inspected over MIDI (MANUAL.md, "MIDI"): notes press the keys,
 *  CCs turn the knobs or set a parameter outright, program changes recall scenes, and FRIZZ's
 *  own SysEx presses keys, turns knobs and asks what the device shows, for a computer to
 *  test it (firmware/remote.py).
 *
 *  MidiClock.h reads both inputs in the audio callback and hands every message but the clock
 *  to Event(), which only notes what came: the keys MIDI holds (Keys(), merged with the hand's
 *  in ui.h, so a key held by both is pressed once), detents (TakeTurn(), ui.h), and for the
 *  play page, which takes them in MainLoop (NormalPage::Remote): the latest value of every
 *  absolute controller, marked as changed (so a DAW's dense automation is a table, never a
 *  queue that fills), a program change, the transport and a query to answer. The answer goes
 *  out over USB from MainLoop (Reply), never from the interrupt.
 *
 *  Channel messages count on the channel set (MasterSettings::midi_channel, 0 for all).
 *  Every message that changes something is logged for a bug report (EventLog.h) as it came, so
 *  the replay plays it in again; a scene sent over SysEx isn't, so a replay starts from the
 *  card's scenes.
 *
 *  SysEx: F0 7D 43 48 CMD DATA F7, the launcher's header (MidiClock.h). FRIZZ's commands start
 *  at 0x11, above the launcher's 01-04, and an answer is the command | 0x40, as the
 *  launcher's are: 0x51 and up, so no answer of FRIZZ's passes as the launcher's.
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
// Notes: the 25 keys as a keyboard from kBaseNote, CHOMPI, PLAY and LOOP below it, the knob
// presses below those
static const uint8_t kBaseNote = 48;   // KEY_1, the lowest white key
static const uint8_t kChompiNote = 45; // then PLAY 46, LOOP 47
static const uint8_t kKnobNote = 36;   // knobs 1-4, the transport, VOLUME: 36-41
static const uint8_t kNumKnobs = 6;

// Controllers
static const uint8_t kTurnCC = 14;   // 14-19: knobs 1-4, transport, VOLUME, relative
static const uint8_t kLatchCC = 20;  // 20-31: an effect's latch, by its key, left to right
static const uint8_t kCompCC = 52;   // 52-55: the master compressor's knobs 1-4
static const uint8_t kOutGainCC = 56;
static const uint8_t kInGainCC = 57;
static const uint8_t kMixCC = 58;
static const uint8_t kHpCueCC = 59;
static const uint8_t kMonoCC = 60;
static const uint8_t kMorphBarsCC = 61; // how many bars a morph over MIDI takes, 1-8
static const uint8_t kMorphCC = 62;     // morph to the scene 0-4
static const uint8_t kStopMorphCC = 63; // stops a morph where it is
static const uint8_t kParamCC = 70;     // 70-117: effect FX's knob P at 70 + 4 FX + P
static const uint8_t kFaderCC = 118;    // the crossfader: a running morph from its start to its scene
// the effects with CCs: the first 12 FxIds; the chaos key (FX_CHAOS) has CC 119 for its
// latch and its knobs on NRPN alone (bank 0, 122-125), since 118-121 are taken or reserved
static const size_t kMidiFx = 12;
static const uint8_t kChaosLatchCC = 119;
static const uint8_t kChaosNrpn = 122;
static const uint8_t kAllSoundOffCC = 120, kAllNotesOffCC = 123;
// NRPN: in bank (MSB) 0, parameter number = the CC above, its value in 14 bits; in bank 1,
// page 2 of the FX knobs and the compressor's (FxControls.h), numbered as page 1's CCs
// (kParamCC, kCompCC). No CCs are left for page 2
static const uint8_t kNrpnMsbCC = 99, kNrpnLsbCC = 98, kDataMsbCC = 6, kDataLsbCC = 38;
static const uint8_t kRpnMsbCC = 101, kRpnLsbCC = 100;
static const uint8_t kPage2Bank = 1;
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
    kCmdParams = 0x21,   // FX: an effect's knobs (12 the chaos key's, 13 the compressor's), 14 bits each
    kCmdLeds = 0x22,     // PART: the LEDs as their bytes, part 0 the panel's, 1-3 the keys'
    kCmdLoad = 0x23,     // the audio callback's load since the last ask
    kCmdSettings = 0x24, // the channel, transport following, the clock source, MIDI out
    kCmdSceneGet = 0x30, // SLOT PART: a scene, in kSceneParts parts
    kCmdScenePut = 0x31, // SLOT PART DATA: likewise; the last part stores it
    kCmdReply = 0x40,
};
static const uint8_t kMidiHeader[] = {0x7D, 0x43, 0x48};
// a scene over SysEx: 4 parts of 4 effects' page-1 knobs, the same 4 for page 2, then the
// latches. The last part of a page has the chaos key and room for 3 more (zeros)
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

    void Init(MidiClock* clock, EventLog* log, uint8_t channel, bool transport, float tick_freq,
              float block_seconds)
    {
        clock_ = clock;
        log_ = log;
        channel_ = channel;
        transport_ = transport;
        ticks_per_block_ = tick_freq * block_seconds;
        clock_->SetListener(&MidiControl::Listen, this);
    }

    // ---- from the audio callback ----

    /** The keys MIDI's notes hold, by Hardware::SwId: always the play page's */
    inline uint64_t Keys() const { return keys_; }
    /** The keys FRIZZ's SysEx holds (kCmdKey): the panel's, as the hand's, so they reach the
     *  settings page too (ui.h) */
    inline uint64_t PanelKeys() const { return panel_keys_; }
    /** The mode switch as SysEx set it (kCmdSwitch): 0 as it stands, 1 down, 2 up */
    inline uint8_t Switch() const { return switch_; }

    /** A detent MIDI turned knob 0-5 (the knobs' order: 1-4, transport, VOLUME), taken: +-1,
     *  or 0. One a block, as a hand's come: the play page counts stepped knobs and the
     *  transport's speed steps detent by detent */
    inline int TakeTurn(size_t knob) { return TakeOne(turns_[knob]); }
    /** The same for FRIZZ's SysEx detents (kCmdTurn): the panel's, as the hand's */
    inline int TakePanelTurn(size_t knob) { return TakeOne(panel_turns_[knob]); }

    /** The callback's own time in system ticks, for kCmdLoad */
    inline void BlockTime(uint32_t ticks)
    {
        const float load = ticks / ticks_per_block_;
        load_max_ = load > load_max_ ? load : load_max_;
        load_sum_ += load;
        load_blocks_++;
    }

    // ---- from MainLoop (the play page) ----

    /** The next controller changed since it was last taken: its CC number (128 + it for page
     *  2's, NRPN bank 1), its value as a knob's 0-1 and in 7 bits (raw); false when there's
     *  none */
    MIDI_CONTROL_ONCE bool TakeValue(uint16_t& cc, float& value, uint8_t& raw)
    {
        daisy::ScopedIrqBlocker irq;
        for (size_t w = 0; w < kValueWords; w++)
        {
            if (!changed_[w])
                continue;
            const uint32_t bit = changed_[w] & (~changed_[w] + 1);
            changed_[w] &= ~bit;
            cc = static_cast<uint16_t>(w * 32 + __builtin_ctz(bit));
            const bool fine = fine_[w] & bit;
            fine_[w] &= ~bit;
            value = MidiToKnob(values_[cc], fine);
            raw = static_cast<uint8_t>(fine ? values_[cc] >> 7 : values_[cc]);
            return true;
        }
        return false;
    }

    /** A program change, taken: its number, or -1 */
    inline int TakeProgram()
    {
        daisy::ScopedIrqBlocker irq;
        const int p = program_;
        program_ = -1;
        return p;
    }

    /** MIDI Start or Continue (1), Stop (-1), or nothing (0), taken; only while following */
    inline int TakeTransport()
    {
        daisy::ScopedIrqBlocker irq;
        const int t = transport_cmd_;
        transport_cmd_ = 0;
        return t;
    }

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

    /** The settings page's (SettingsPage.h): the channel (0 all), transport following, and
     *  how the clock is followed and from where */
    inline void SetChannel(uint8_t channel) { channel_ = channel; }
    inline void SetTransport(bool on) { transport_ = on; }
    inline void SetClockFactor(ClockFactor factor) { clock_->SetFactor(factor); }
    inline void SetClockSource(ClockSource source) { clock_->SetSource(source); }
    inline ClockSource GetClockSource() const { return clock_->GetSource(); }
    /** Where MIDI out goes (MidiOut.h), read by the audio callback every block */
    inline void SetMidiOut(MidiOutPorts ports) { out_ = ports; }
    inline MidiOutPorts GetMidiOut() const { return out_; }

    /** The load since the last call, max and mean, in 1/1000 of a block */
    void TakeLoad(uint16_t& max, uint16_t& mean)
    {
        daisy::ScopedIrqBlocker irq;
        max = static_cast<uint16_t>(load_max_ * 1000.f + .5f);
        mean = load_blocks_ ? static_cast<uint16_t>(load_sum_ / load_blocks_ * 1000. + .5) : 0;
        load_max_ = 0.f;
        load_sum_ = 0.;
        load_blocks_ = 0;
    }

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
        static_cast<MidiControl*>(self)->Event(e, usb);
    }

    MIDI_CONTROL_ONCE void Event(const MidiEvent& e, bool usb)
    {
        using namespace midimap;
        if (e.type == SystemCommon && e.sc_type == SystemExclusive)
        {
            SysEx(e.sysex_data, e.sysex_message_len, usb);
            return;
        }
        if (e.type == SystemRealTime)
        {
            // Start and Stop from the clock source's input only, from both in Auto and internal
            if (!transport_ || !FromSource(clock_->GetSource(), usb))
                return;
            const uint8_t t = TransportByte(e);
            if (t)
            {
                transport_cmd_ = t == 0xFC ? -1 : 1;
                Log(t, 0, 0, usb);
            }
            return;
        }
        // libDaisy calls CC 120-127 channel mode messages, and then a CC after them in
        // running status too: they're all controllers here
        const MidiMessageType type = e.type == ChannelMode ? ControlChange : e.type;
        if (type > PitchBend || (channel_ && e.channel != channel_ - 1))
            return;

        const uint8_t status = static_cast<uint8_t>(0x80 | (type << 4) | e.channel);
        const uint8_t d0 = e.data[0], d1 = e.data[1];
        switch (type)
        {
        case NoteOff:
        case NoteOn:
        {
            const int key = NoteKey(d0);
            if (key < 0)
                return;
            const uint64_t bit = 1ull << key;
            if (type == NoteOn && d1 > 0)
                keys_ |= bit;
            else
                keys_ &= ~bit;
            break;
        }
        case ControlChange:
            if (!ControlChanged(d0, d1))
                return;
            break;
        case ProgramChange:
            if (d0 >= kNumSlots)
                return;
            program_ = d0;
            break;
        default:
            return;
        }
        Log(status, d0, d1, usb);
    }

    /** False for a controller FRIZZ doesn't use */
    bool ControlChanged(uint8_t cc, uint8_t v)
    {
        using namespace midimap;
        if (cc >= kTurnCC && cc < kTurnCC + kNumKnobs)
        {
            // relative, two's complement: 1-63 up, 65-127 down
            if (v == 0 || v == 64)
                return false;
            // taken one a block: a DAW's fast turns would pile up and run on for seconds
            const int t = turns_[cc - kTurnCC] + (v < 64 ? v : static_cast<int>(v) - 128);
            turns_[cc - kTurnCC] = t > kMaxTurns ? kMaxTurns : (t < -kMaxTurns ? -kMaxTurns : t);
            return true;
        }
        if (cc == kAllSoundOffCC || cc == kAllNotesOffCC)
        {
            keys_ = 0;
            panel_keys_ = 0;
            return true;
        }
        if (cc == kNrpnMsbCC)
        {
            nrpn_msb_ = v;
            data_msb_ = 0; // a new parameter's value starts afresh
            return true;
        }
        if (cc == kNrpnLsbCC)
        {
            nrpn_lsb_ = v;
            data_msb_ = 0;
            return true;
        }
        // an RPN chosen (a DAW's pitch-bend range): CC 6 and 38 are its, no NRPN's. CC 100
        // and 101 are the slicer's knobs 3 and 4 too, so they go on to Absolute
        if (cc == kRpnMsbCC || cc == kRpnLsbCC)
            nrpn_msb_ = 0xFF;
        if (cc == kDataMsbCC || cc == kDataLsbCC)
        {
            // bank 0: an absolute controller; bank 1: page 2 of an effect's or the
            // compressor's knobs
            const bool page2 = nrpn_msb_ == kPage2Bank
                               && ((nrpn_lsb_ >= kParamCC && nrpn_lsb_ < kParamCC + kMidiFx * kNumFxKnobs)
                                   || (nrpn_lsb_ >= kCompCC && nrpn_lsb_ < kCompCC + kNumFxKnobs));
            const bool page1 = nrpn_msb_ == 0
                               && (Absolute(nrpn_lsb_)
                                   || (nrpn_lsb_ >= kChaosNrpn && nrpn_lsb_ < kChaosNrpn + kNumFxKnobs));
            if (!page1 && !page2)
                return false;
            if (cc == kDataMsbCC)
                data_msb_ = v;
            Set(static_cast<uint16_t>(nrpn_msb_ * 128 + nrpn_lsb_),
                static_cast<uint16_t>((data_msb_ << 7) | (cc == kDataLsbCC ? v : 0)), true);
            return true;
        }
        if (!Absolute(cc))
            return false;
        Set(cc, v, false);
        return true;
    }

    static bool Absolute(uint8_t cc)
    {
        using namespace midimap;
        return (cc >= kLatchCC && cc < kLatchCC + kMidiFx) || cc == kChaosLatchCC
               || (cc >= kCompCC && cc <= kStopMorphCC) || cc == kFaderCC
               || (cc >= kParamCC && cc < kParamCC + kMidiFx * kNumFxKnobs);
    }

    void Set(uint16_t cc, uint16_t v, bool fine)
    {
        values_[cc] = v;
        const uint32_t bit = 1u << (cc % 32);
        changed_[cc / 32] |= bit;
        if (fine)
            fine_[cc / 32] |= bit;
        else
            fine_[cc / 32] &= ~bit;
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
            // the rest only ask, over USB, where the answer goes
            if (!usb || query_pending_)
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

    volatile uint64_t keys_ = 0;       // notes'
    volatile uint64_t panel_keys_ = 0; // SysEx's
    static const int kMaxTurns = 64; // detents waiting, at most: 32 ms of them
    volatile int turns_[midimap::kNumKnobs] = {};       // CCs'
    volatile int panel_turns_[midimap::kNumKnobs] = {}; // SysEx's
    volatile uint8_t switch_ = 0;

    // by CC number, and 128 + it for page 2's NRPN (bank 1)
    static const size_t kValueWords = 256 / 32;
    uint16_t values_[256] = {};
    uint32_t changed_[kValueWords] = {}, fine_[kValueWords] = {};
    uint8_t nrpn_msb_ = 0xFF, nrpn_lsb_ = 0xFF, data_msb_ = 0; // 0xFF: none yet
    volatile int program_ = -1;
    volatile int transport_cmd_ = 0;

    Query query_;
    volatile bool query_pending_ = false;

    float ticks_per_block_ = 1.f;
    float load_max_ = 0.f;
    double load_sum_ = 0.; // a float stops adding ~0.45 a block after a few hours unread
    uint32_t load_blocks_ = 0;
};

} // namespace chompi
