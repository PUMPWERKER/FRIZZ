/** @file EventLog.h
 *  @brief The event recorder: every key, knob detent, mode-switch flip, MIDI clock change and
 *  MIDI message FRIZZ acted on (MidiControl.h) since power-on, written to /FRIZZ/bug-N.txt when
 *  SHIFT + VOLUME press are held 2 s on the settings page (SettingsPage.h; MANUAL.md, "Bug
 *  reports"). The file is a script for the virtual CHOMPI (firmware/twin/README.md): the
 *  card's FRIZZ files as they were at power-on, then the controls at the times they were
 *  used, so the twin plays the session again from power-on.
 *
 *  The audio callback adds the events (ui.h's GenerateEvents) to an append-only list in SDRAM
 *  (only the CPU touches it; the text going to and from the card stays in internal RAM, as
 *  all FRIZZ's card buffers do),
 *  writing the slot before it counts it; MainLoop writes the file a chunk per pass, up to the
 *  count it saw at the press, so neither waits for the other. The list starts when main()
 *  enters its loop (Start: the card read; its SDRAM isn't cleared at boot, SDRAM_NOINIT, so
 *  nothing in it is read before it's written); the times count from
 *  there, and the script's `booted` line lines them up with the twin's own start. The audio
 *  in isn't recorded: the replay plays a tone.
 *
 *  A logged time is when the firmware saw the change, after its debouncing; the file gives
 *  when the hand did it (kLead), as the twin debounces too. A MIDI message is logged as it
 *  came in and played in again then, on the input it came on (the jack or USB), and so is
 *  the clock: the clock source (MidiClock.h) counts only one of them.
 */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "daisy.h"
#include "fatfs.h"
#include "hardware.h"
#include "FxScenes.h"
#include "MasterSettings.h"
#include "SceneStore.h"
#include "MidiClock.h"

// once each and small, not inlined wherever they're used: FRIZZ's code space is tight
// (SRAM_EXEC), and none of them is in a hurry
#define EVENT_LOG_ONCE __attribute__((noinline, optimize("Os")))

namespace chompi
{

struct LoggedEvent
{
    uint32_t ms;   // since Start
    int16_t value; // a key: 1 down, 0 up; a detent: +-1; the switch: its level; the clock: BPM x10;
                   // MIDI: its two data bytes, 7 bits each, the first on top
    uint8_t kind;  // EventLog::Kind
    uint8_t id;    // a key: its SwId; a knob: its encoder, 1-6 (SW1-SW6); the clock: how long
                   // before it was logged it changed, in 10 ms; MIDI: its status byte, or
                   // FRIZZ's SysEx command
};

// 2 MB of SDRAM: 260,000 events, far more than a session
static const size_t kEventLogSize = 1u << 18;
static const size_t kEventLogText = 4096; // written a chunk this size at a time
static const size_t kSector = 512;        // the card's, and FatFs's

/** Its list, and the card's FRIZZ files as they were at power-on (only a bug report reads
 *  them), in SDRAM (chompi_main.cpp) */
struct EventLogMem
{
    LoggedEvent events[kEventLogSize];
    char scenes[kSceneFileMax];
    char master[kMasterFileMax];
};

// Hardware::SwId's names, as the twin's scripts take them
static const char* const kSwNames[] = {
    "ENC_1_SW", "ENC_2_SW", "ENC_3_SW", "ENC_4_SW", "ENC_5_SW", "KEY_26", "SW_TOG", "KEY_16",
    "KEY_2",    "KEY_3",    "KEY_4",    "KEY_5",    "KEY_17",   "KEY_18", "KEY_19", "KEY_1",
    "KEY_6",    "KEY_7",    "KEY_8",    "KEY_9",    "KEY_10",   "KEY_20", "KEY_21", "KEY_22",
    "KEY_11",   "KEY_12",   "KEY_13",   "KEY_14",   "KEY_15",   "KEY_23", "KEY_24", "KEY_25",
    "ENC_6_SW", "KEY_27",   "KEY_28",   "NC_1",     "NC_2",     "NC_3",   "NC_4",   "NC_5",
};
static_assert(sizeof(kSwNames) / sizeof(kSwNames[0])
                  == static_cast<size_t>(Hardware::SwId::SR_LAST),
              "a name for every SwId");

class EventLog
{
public:
    enum Kind : uint8_t
    {
        KEY,
        TURN,
        TOGGLE,
        CLOCK,
        MIDI,  // a channel message, or Start / Continue / Stop
        SYSEX, // FRIZZ's own, with two data bytes (MidiControl.h)
        CLOCK_USB, // as CLOCK and MIDI, from USB
        MIDI_USB,
    };

    void Init(EventLogMem* mem, FATFS* fs, const char* path)
    {
        mem_ = mem;
        // until Start, no card files (its memory isn't cleared at boot)
        mem_->scenes[0] = mem_->master[0] = '\0';
        fs_ = fs;
        path_ = path;
    }

    /** From main() as it enters its loop: the list starts, with the mode switch as it is
     *  and the card's FRIZZ files as SceneStore left them at boot */
    void Start(uint32_t now, bool toggle)
    {
        // relative, as SceneStore's: /FRIZZ, or the root on a card where it couldn't be made
        Snapshot(kSceneFile, mem_->scenes, kSceneFileMax);
        Snapshot(kMasterFile, mem_->master, kMasterFileMax);
        toggle_ = toggle_at_start_ = toggle;
        start_ = now;
        started_ = true;
    }

    /** From the audio callback, or with its interrupt blocked */
    EVENT_LOG_ONCE void Add(Kind kind, uint8_t id, int16_t value)
    {
        if (!started_)
            return;
        if (count_ >= kEventLogSize)
        {
            full_ = true;
            return;
        }
        LoggedEvent& e = mem_->events[count_];
        e.ms = daisy::System::GetNow() - start_;
        e.value = value;
        e.kind = kind;
        e.id = id;
        count_ = count_ + 1;
    }

    /** From the audio callback: the mode switch as Hardware reads it, logged when it flips */
    EVENT_LOG_ONCE void Toggle(bool state)
    {
        if (started_ && state != toggle_)
        {
            toggle_ = state;
            Add(TOGGLE, 0, state ? 0 : 1);
        }
    }

    /** From the play page, as SHIFT + VOLUME press have been held 2 s on the settings page.
     *  Ignored while a file is being written */
    void RequestWrite()
    {
        if (!writing_)
            requested_ = true;
    }

    inline bool Writing() const { return writing_ || requested_; }
    /** How many files were written, and how many failed: the play page shows each new one */
    inline uint32_t Written() const { return written_; }
    inline uint32_t Failed() const { return failed_; }

    /** From MainLoop: follows the MIDI clock, and writes a requested file a chunk at a time */
    EVENT_LOG_ONCE void Process(uint32_t now, const MidiClock& clock)
    {
        if (started_)
            FollowClock(now, clock);

        if (requested_ && !writing_)
        {
            requested_ = false;
            end_ = count_;
            next_ = 0;
            at_ = 0;
            if (Begin())
                writing_ = true;
            else
                failed_++;
            return;
        }
        if (writing_)
        {
            const bool more = next_ < end_;
            if (!(more ? WriteEvents() : Finish()))
            {
                f_close(&file_);
                writing_ = false;
                failed_++;
            }
            else if (!more)
            {
                writing_ = false;
                written_++;
            }
        }
    }

private:
    static const uint32_t kClockCheckMs = 500;
    static const int16_t kClockSteady = 3;    // tenths of a BPM between two checks: settled
    static const int16_t kClockChange = 10;   // a running clock's change worth logging
    static const uint32_t kClockLossMs = kClockTimeoutSamples / 48; // MidiClock's timeout, at 48 kHz
    // how long the firmware takes to see a hand's change, measured on the twin, which runs
    // the same debouncing: a key let go, or pressed (on the shift registers, a millisecond
    // more than the transport's switch on its own pin), a detent's quadrature steps (knobs
    // 1-4 on the shift registers, a millisecond more than the transport and VOLUME on their
    // own pins), the mode switch through Hardware::GetToggleState's 100 blocks, either way
    static const uint32_t kKeyLeadMs = 7;
    static const uint32_t kSrKeyDownLeadMs = 8;
    static const uint32_t kSrTurnLeadMs = 4;
    static const uint32_t kTurnLeadMs = 3;
    static const uint32_t kToggleLowLeadMs = 58, kToggleHighLeadMs = 56;
    // MIDI has nothing to debounce: its 1 ms is the twin's `booted`, which looks for the main
    // loop once a millisecond, so its times count from a millisecond after the log's
    static const uint32_t kMidiLeadMs = 1;

    /** How long before the firmware saw it the hand (or the clock) did it */
    static uint32_t Lead(const LoggedEvent& e)
    {
        switch (e.kind)
        {
        case KEY: return e.value && e.id != ENC_5_SW ? kSrKeyDownLeadMs : kKeyLeadMs;
        case TURN: return e.id <= 4 ? kSrTurnLeadMs : kTurnLeadMs;
        case TOGGLE: return e.value ? kToggleHighLeadMs : kToggleLowLeadMs;
        case CLOCK:
        case CLOCK_USB: return e.id * 10u;
        default: return kMidiLeadMs;
        }
    }

    /** The clock is logged once its tempo has settled, dated back to when it locked; a loss at
     *  once, dated back to its last tick; a tempo change of a running clock once it settles */
    EVENT_LOG_ONCE void FollowClock(uint32_t now, const MidiClock& clock)
    {
        if (clock.SenderLocks() != locks_seen_)
        {
            locks_seen_ = clock.SenderLocks();
            locked_at_ = now;
            // lost and found on the other input within a pass: the replay stops the one it had
            if (clock_logged_ != 0 && clock.UsbClock() != (clock_kind_ == CLOCK_USB))
                AddFromMain(clock_kind_, kClockLossMs / 10, 0);
            clock_logged_ = 0;
            clock_last_ = 0;
        }
        if (!clock.HasClock())
        {
            if (clock_logged_ != 0)
                AddFromMain(clock_kind_, kClockLossMs / 10, 0);
            clock_logged_ = 0;
            return;
        }
        if (now - clock_checked_ < kClockCheckMs)
            return;
        clock_checked_ = now;
        const int16_t tenths = static_cast<int16_t>(clock.SenderBpm() * 10.f + .5f);
        const bool steady = tenths > 0 && abs(tenths - clock_last_) <= kClockSteady;
        clock_last_ = tenths;
        if (steady && (clock_logged_ == 0 || abs(tenths - clock_logged_) > kClockChange))
        {
            const uint32_t since = clock_logged_ == 0 ? (now - locked_at_) / 10 : 0;
            clock_kind_ = clock.UsbClock() ? CLOCK_USB : CLOCK; // the loss stops the same
            AddFromMain(clock_kind_, since > 255 ? 255 : since, tenths);
            clock_logged_ = tenths;
        }
    }
    void AddFromMain(Kind kind, uint8_t id, int16_t value)
    {
        daisy::ScopedIrqBlocker irq;
        Add(kind, id, value);
    }

    /** The file's text into to (in SDRAM), terminated; empty when it can't be read. Read
     *  through text_: the card's DMA only gets internal RAM (the class comment) */
    EVENT_LOG_ONCE void Snapshot(const char* name, char* to, size_t size)
    {
        static_assert(sizeof(text_) >= kSceneFileMax && kSceneFileMax >= kMasterFileMax,
                      "a snapshot fits text_");
        to[0] = '\0';
        if (f_open(&file_, name, FA_READ) != FR_OK)
            return;
        UINT len = 0;
        const FRESULT res = f_read(&file_, text_, size - 1, &len);
        f_close(&file_);
        if (res != FR_OK)
            len = 0;
        memcpy(to, text_, len);
        to[len] = '\0';
    }

    // the text being put together in text_
    EVENT_LOG_ONCE void Put(const char* s)
    {
        const size_t len = strlen(s);
        memcpy(text_ + pos_, s, len);
        pos_ += len;
    }
    EVENT_LOG_ONCE void PutNum(uint32_t n)
    {
        char digits[10];
        int i = 0;
        do
        {
            digits[i++] = static_cast<char>('0' + n % 10);
            n /= 10;
        } while (n);
        while (i)
            text_[pos_++] = digits[--i];
    }
    /** A byte as " HH" */
    EVENT_LOG_ONCE void PutHex(uint8_t b)
    {
        static const char kDigits[] = "0123456789ABCDEF";
        text_[pos_++] = ' ';
        text_[pos_++] = kDigits[b >> 4];
        text_[pos_++] = kDigits[b & 15];
    }
    /** A MIDI message as the twin's `midi` line (`usb` for USB's), as many data bytes as its
     *  status has */
    EVENT_LOG_ONCE void PutMidi(const LoggedEvent& e)
    {
        Put(e.kind == MIDI_USB ? "usb" : "midi");
        PutHex(e.id);
        const uint8_t type = e.id & 0xF0;
        if (e.id >= 0xF0)
            return;
        PutHex(static_cast<uint8_t>(e.value >> 7));
        if (type != 0xC0 && type != 0xD0)
            PutHex(e.value & 0x7F);
    }
    /** text_ to the file in whole sectors, the rest kept for the next (all: everything, at the
     *  end). FatFs hands whole sectors straight from text_ to the SD card's DMA, which reads
     *  from a word-aligned address: so every write starts on a sector of the file and at the
     *  start of text_, or the sectors come out shifted (bytes repeated, others lost) */
    EVENT_LOG_ONCE bool Flush(bool all = false)
    {
        const size_t n = all ? pos_ : pos_ - pos_ % kSector;
        if (n == 0)
            return true;
        UINT written = 0;
        const FRESULT res = f_write(&file_, text_, static_cast<UINT>(n), &written);
        memmove(text_, text_ + n, pos_ - n);
        pos_ -= n;
        return res == FR_OK && written == n;
    }
    /** A card file as the script's `card file` block: every line behind a `|` */
    EVENT_LOG_ONCE bool PutFile(const char* path, const char* text)
    {
        if (!text[0])
            return true;
        Put("card file ");
        Put(path);
        Put("\n|");
        char last = '|';
        for (const char* c = text; *c; c++)
        {
            if (pos_ >= kEventLogText && !Flush())
                return false;
            if (*c == '\r')
                continue;
            text_[pos_++] = last = *c;
            if (*c == '\n' && c[1])
                text_[pos_++] = '|';
        }
        // by what was copied, not text_[pos_ - 1]: a Flush can have just emptied text_
        if (last != '\n')
            text_[pos_++] = '\n';
        return Flush();
    }

    /** Opens the next free bug-N.txt (in /FRIZZ, as the scenes) and writes the head of the script */
    EVENT_LOG_ONCE bool Begin()
    {
        if (f_mount(fs_, path_, 1) != FR_OK)
            return false;
        // a mount goes back to the root, and SceneStore's paths are relative to /FRIZZ
        EnterFrizzDir();
        char name[24];
        for (number_ = 1; number_ < 1000; number_++)
        {
            pos_ = 0;
            Put("bug-");
            PutNum(number_);
            Put(".txt");
            memcpy(name, text_, pos_);
            name[pos_] = '\0';
            FILINFO info;
            if (f_stat(name, &info) != FR_OK)
                break;
        }
        pos_ = 0;
        if (number_ == 1000 || f_open(&file_, name, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
            return false;

        Put("# FRIZZ event log ");
        PutNum(number_);
        Put(": the keys, knobs, mode switch and MIDI from power-on to SHIFT + VOLUME\n"
            "# held, the card's FRIZZ files as they were at power-on. The virtual CHOMPI plays\n"
            "# it again: firmware/twin/run.sh -o out.wav -l leds.txt bug-N.txt\n"
            "# The audio in isn't in it: the replay plays a tone into AUX (the input line).\n"
            "# Nor are scenes sent over MIDI: the replay has the card's.\n");
        if (full_)
            Put("# The log was full: it ends early, and the replay too.\n");
        // the replay puts them where FRIZZ keeps them, /FRIZZ
        if (!Flush() || !PutFile("/FRIZZ/frizz_scenes.txt", mem_->scenes)
            || !PutFile("/FRIZZ/frizz_master.txt", mem_->master))
        {
            f_close(&file_); // open: a failed write mustn't leave it so
            return false;
        }
        Put("input sine 220 0.3\ntoggle ");
        PutNum(toggle_at_start_ ? 0 : 1);
        Put("\nbooted\n");
        if (Flush())
            return true;
        f_close(&file_);
        return false;
    }

    /** The next chunk of events, as `at` and command lines */
    EVENT_LOG_ONCE bool WriteEvents()
    {
        while (next_ < end_ && pos_ < kEventLogText)
        {
            const LoggedEvent& e = mem_->events[next_++];
            const uint32_t lead = Lead(e);
            const uint32_t at = e.ms > lead ? e.ms - lead : 0;
            if (at > at_)
            {
                at_ = at;
                Put("at ");
                PutNum(at);
                Put("\n");
            }
            switch (e.kind)
            {
            case KEY:
                Put(e.value ? "down " : "up ");
                Put(e.id < static_cast<uint8_t>(Hardware::SwId::SR_LAST) ? kSwNames[e.id] : "?");
                break;
            case TURN:
                Put("turn ");
                PutNum(e.id);
                Put(e.value > 0 ? " 1" : " -1");
                break;
            case TOGGLE:
                Put("toggle ");
                PutNum(e.value);
                break;
            case MIDI:
            case MIDI_USB:
                PutMidi(e);
                break;
            case SYSEX:
                Put("midi F0 7D 43 48");
                PutHex(e.id);
                PutHex(static_cast<uint8_t>(e.value >> 7));
                PutHex(e.value & 0x7F);
                Put(" F7");
                break;
            default:
                Put("clock ");
                PutNum(e.value / 10);
                Put(".");
                PutNum(e.value % 10);
                if (e.kind == CLOCK_USB)
                    Put(" usb");
                break;
            }
            Put("\n");
        }
        return Flush();
    }

    bool Finish()
    {
        Put("# written here, ");
        PutNum(end_);
        Put(" events\n");
        const bool ok = Flush(true);
        return f_close(&file_) == FR_OK && ok;
    }

    EventLogMem* mem_ = nullptr;
    // the text going to and from the card: whole cache lines, as the SD driver keeps the
    // cache in step with its DMA by them (SceneStore's buf_ too)
    alignas(32) char text_[kEventLogText + 128];
    FATFS* fs_ = nullptr;
    const char* path_ = nullptr;
    FIL file_;

    volatile bool started_ = false;
    uint32_t start_ = 0;
    volatile size_t count_ = 0;
    volatile bool full_ = false;
    bool toggle_ = false, toggle_at_start_ = false;
    uint32_t clock_checked_ = 0;
    uint32_t locks_seen_ = 0, locked_at_ = 0;
    int16_t clock_logged_ = 0; // the tempo the log has, 0 for none
    int16_t clock_last_ = 0;   // at the last check
    Kind clock_kind_ = CLOCK;  // the input of the clock the log has

    volatile bool requested_ = false;
    bool writing_ = false;
    size_t end_ = 0, next_ = 0;
    uint32_t at_ = 0;
    size_t pos_ = 0;
    uint32_t number_ = 0;
    uint32_t written_ = 0, failed_ = 0;
};

} // namespace chompi
