// midi.cpp: FRIZZ's MIDI input (MidiClock.h, behind libDaisy's parser and handlers) on the
// virtual CHOMPI: clock ticks among other messages and inside them, the messages it ignores,
// rubbish, and the two inputs, the jack and USB: which one locks, and when the other takes
// over. Each case on a fresh device. Run by unit.sh midi.
#include "timing.h"
#include "twin.h"

/** count ticks, one every period_ms, each as the bytes bytes(k) (which hold its F8s), into the
 *  jack or over USB. Returns how many F8 went in */
static uint32_t Feed(bool on_usb, double period_ms, int count,
                     const std::function<std::vector<uint8_t>(int)>& bytes)
{
    uint32_t f8 = 0;
    const double t0 = BlockMs();
    for (int k = 0; k < count; k++)
    {
        while (BlockMs() < t0 + k * period_ms)
            RunBlocks(1);
        for (uint8_t b : bytes(k))
        {
            f8 += b == 0xF8;
            if (on_usb)
                UsbMidi(b);
            else
                Midi(b);
        }
    }
    RunMs(5);
    return f8;
}

static const double kTick120 = 60000. / (120. * 24.); // ms

/** The ticks the firmware counted from the clock it locked to while bytes went in */
static void ExpectTicks(const char* name, bool on_usb,
                        const std::function<std::vector<uint8_t>(int)>& bytes, bool known_fault,
                        const char* what)
{
    RunMs(kReadyMs);
    const uint32_t before = Probe().ticks;
    const uint32_t sent = Feed(on_usb, kTick120, 480, bytes);
    const ClockState c = Probe();
    const uint32_t got = c.ticks - before;
    Report("%s: %u ticks sent, %u counted, %.2f BPM", name, sent, got, c.midi_bpm);
    const bool ok = got == sent && c.has_clock && fabsf(c.midi_bpm - 120.f) < .5f;
    (known_fault ? Known : Check)(ok, what);
}

int main()
{
    // plain ticks, on each input
    for (bool on_usb : {false, true})
        cases.push_back({on_usb ? "usb" : "trs", [on_usb] {
            RunMs(kReadyMs);
            Check(!Probe().has_clock, "no clock before any tick");
            StartTrs(Clock(0.));
            ClockGen::Config c = Clock(120.);
            if (on_usb)
                StartUsb(c);
            else
                StartTrs(c);
            RunMs(10000);
            const ClockState p = Probe();
            const uint64_t sent = on_usb ? usb.Sent() : trs.Sent();
            Report("%s: %llu ticks sent, %u counted, %.3f BPM", on_usb ? "USB" : "TRS",
                   (unsigned long long)sent, p.ticks, p.midi_bpm);
            Check(p.has_clock && p.source == (on_usb ? 2 : 1),
                  on_usb ? "USB: the clock locks, on USB" : "TRS: the clock locks, on the jack");
            Check(p.ticks == sent, on_usb ? "USB: every tick counted" : "TRS: every tick counted");
            Check(fabsf(p.midi_bpm - 120.f) < .2f && p.tempo == 120,
                  on_usb ? "USB: 120 BPM, the FX at 120" : "TRS: 120 BPM, the FX at 120");
        }});

    // between whole messages: notes with and without running status, CCs, pressure, program
    // changes, active sensing, a short SysEx
    cases.push_back({"between", [] {
        ExpectTicks("between", false, [](int k) {
            switch (k % 6)
            {
            case 0: return std::vector<uint8_t>{0x90, 0x3C, 0x64, 0x3E, 0x64, 0xF8};
            case 1: return std::vector<uint8_t>{0xF8, 0xB0, 0x07, 0x7F, 0x0A, 0x40};
            case 2: return std::vector<uint8_t>{0xD0, 0x40, 0xF8, 0xC0, 0x05};
            case 3: return std::vector<uint8_t>{0xFE, 0xF8, 0xE0, 0x00, 0x40};
            case 4: return std::vector<uint8_t>{0xF0, 0x01, 0x02, 0x03, 0xF7, 0xF8};
            default: return std::vector<uint8_t>{0x80, 0x3C, 0x00, 0xF8};
            }
        }, false, "between: ticks among notes, CCs, pressure, program changes, active sensing and SysEx all counted");
    }});

    // MIDI lets a real-time byte go anywhere, even in the middle of another message
    cases.push_back({"inside", [] {
        ExpectTicks("inside", false, [](int k) {
            switch (k % 3)
            {
            case 0: return std::vector<uint8_t>{0x90, 0xF8, 0x3C, 0x64};
            case 1: return std::vector<uint8_t>{0xB0, 0x07, 0xF8, 0x7F};
            default: return std::vector<uint8_t>{0x3C, 0xF8, 0x00}; // running status
            }
        }, false, "inside: a tick in the middle of a note or CC counted");
    }});

    cases.push_back({"inside-sysex", [] {
        // a DAW sending a SysEx dump while the clock runs: the ticks fall inside it
        ExpectTicks("inside-sysex", false, [](int k) {
            if (k % 4 == 0)
                return std::vector<uint8_t>{0xF0, 0x7E, 0x00, 0xF8, 0x06, 0x01, 0xF7};
            return std::vector<uint8_t>{0xF8};
        }, true, "inside SysEx: a tick in the middle of a SysEx counted (#18)");
    }});

    cases.push_back({"restart-tick", [] {
        RunMs(kReadyMs);
        StartTrs(Clock(120.));
        RunMs(1000);
        for (uint8_t b : {0xF0, 0x7D, 0x43, 0xF8, 0x48, 0x10, 0xF7})
            Midi(b);
        RunMs(50);
        Known(Restarted(), "restart: FRIZZ's restart SysEx with a clock tick inside it still restarts (#18)");
    }});

    // Start, Stop, Continue and Song Position are ignored (LOOPER.md 1.4): they don't reset or
    // stop the count, nor the tempo
    cases.push_back({"transport", [] {
        ExpectTicks("transport", false, [](int k) {
            if (k == 100)
                return std::vector<uint8_t>{0xFC, 0xF8};
            if (k == 200)
                return std::vector<uint8_t>{0xF2, 0x10, 0x00, 0xFB, 0xF8};
            if (k == 300)
                return std::vector<uint8_t>{0xFA, 0xF8};
            return std::vector<uint8_t>{0xF8};
        }, false, "transport: Stop, Song Position, Continue and Start change nothing");
    }});

    // rubbish: stray data bytes, undefined status bytes, a message cut short by the next, a
    // SysEx longer than the parser's buffer, an F7 without an F0
    cases.push_back({"rubbish", [] {
        ExpectTicks("rubbish", false, [](int k) {
            switch (k % 7)
            {
            case 0: return std::vector<uint8_t>{0x12, 0x34, 0xF8};
            case 1: return std::vector<uint8_t>{0xF4, 0xF8, 0xF5};
            case 2: return std::vector<uint8_t>{0x90, 0x3C, 0xF8};
            case 3: return std::vector<uint8_t>{0xF9, 0xFD, 0xF8};
            case 4: return std::vector<uint8_t>{0xF7, 0xF8, 0xE0};
            case 5:
            {
                std::vector<uint8_t> v = {0xF0};
                v.insert(v.end(), 200, 0x55);
                v.push_back(0xF7);
                v.push_back(0xF8);
                return v;
            }
            default: return std::vector<uint8_t>{0xF8};
            }
        }, false, "rubbish: stray bytes, undefined status, cut messages, a long SysEx: every tick counted");
    }});

    // the same streams over USB
    cases.push_back({"usb-between", [] {
        ExpectTicks("usb-between", true, [](int k) {
            return k % 2 ? std::vector<uint8_t>{0x90, 0x3C, 0x64, 0xF8}
                         : std::vector<uint8_t>{0xF8, 0xB0, 0x07, 0x7F, 0xFE};
        }, false, "USB: ticks among notes and CCs all counted");
    }});

    // two clocks: the one that ticks first is kept until it has been silent 0.5 s
    cases.push_back({"first-wins", [] {
        RunMs(kReadyMs);
        StartTrs(Clock(120.));
        RunMs(2000);
        StartUsb(Clock(90.));
        RunMs(5000);
        ClockState c = Probe();
        Report("both running: source %d, %.2f BPM, FX at %d, %u locks", c.source, c.midi_bpm,
               c.tempo, c.locks);
        Check(c.source == 1 && c.tempo == 120 && c.locks == 1,
              "two clocks: the jack's, first, is kept; USB's ticks are ignored");
        Check(c.ticks == trs.Sent(), "two clocks: only the jack's ticks counted");

        // the jack's stops: USB takes over once it has been silent for 0.5 s
        trs.Stop();
        const double stopped = BlockMs();
        RunUntil([] { return Probe().source == 2; }, 2000);
        const double took = BlockMs() - stopped;
        RunMs(3000);
        c = Probe();
        Report("jack stopped: USB took over after %.1f ms, %.2f BPM, FX at %d, %u locks", took,
               c.midi_bpm, c.tempo, c.locks);
        Check(took > 490 && took < 560, "two clocks: USB takes over 0.5 s after the jack stopped");
        Check(c.tempo == 90 && c.locks == 2, "two clocks: then at USB's tempo, a new lock");
    }});

    // both start in the same block at different tempos: one of them, and only it, counts
    cases.push_back({"same-time", [] {
        RunMs(kReadyMs);
        StartTrs(Clock(120.));
        StartUsb(Clock(100.));
        int changes = 0, last = -1;
        each_block = [&] {
            const ClockState c = Probe();
            if (c.source != last)
                changes++;
            last = c.source;
        };
        RunMs(5000);
        each_block = nullptr;
        const ClockState c = Probe();
        Report("started together: source %d, %.2f BPM, FX at %d, source changed %d times",
               c.source, c.midi_bpm, c.tempo, changes);
        Check(c.source == 1 && c.tempo == 120 && changes == 1,
              "two clocks started together: the jack's wins (it's read first) and stays");
    }});

    // no tick for 0.5 s is no clock; the FX keep the tempo
    cases.push_back({"timeout", [] {
        RunMs(kReadyMs);
        StartTrs(Clock(100.));
        RunMs(3000);
        trs.Stop();
        const double stopped = BlockMs();
        RunUntil([] { return !Probe().has_clock; }, 2000);
        const double lost = BlockMs() - stopped;
        RunMs(2000);
        Report("stopped: no clock after %.1f ms, FX at %d", lost, Probe().tempo);
        Check(lost > 460 && lost < 530, "timeout: no clock 0.5 s after the last tick");
        Check(Probe().tempo == 100, "timeout: the FX keep the clock's tempo");
        StartTrs(Clock(140.));
        RunMs(1000);
        Check(Probe().has_clock && Probe().tempo == 140 && Probe().locks == 2,
              "timeout: a clock again locks again, at its tempo");
    }});

    // a lone late tick among steady ones (a sender's hiccup, a busy jack): the tempo stays
    for (double late_ms : {3., 10.})
        cases.push_back({late_ms < 5. ? "late-tick-3ms" : "late-tick-10ms", [late_ms] {
            RunMs(kReadyMs);
            const double t0 = BlockMs();
            float lo = 1000.f, hi = 0.f;
            int tempo_lo = 1000, tempo_hi = 0;
            for (int k = 0; k < 960; k++)
            {
                // one tick in 48 (every other beat) late, its neighbours on time
                const double at = t0 + k * kTick120 + (k % 48 == 47 ? late_ms : 0.);
                while (BlockMs() < at)
                {
                    RunBlocks(1);
                    if (k < 96) // settling
                        continue;
                    const ClockState c = Probe();
                    lo = fminf(lo, c.midi_bpm);
                    hi = fmaxf(hi, c.midi_bpm);
                    tempo_lo = std::min(tempo_lo, c.tempo);
                    tempo_hi = std::max(tempo_hi, c.tempo);
                }
                Midi(0xF8);
            }
            Report("a tick %.0f ms late every 2 beats: %.2f..%.2f BPM, FX at %d..%d", late_ms, lo,
                   hi, tempo_lo, tempo_hi);
            Check(lo > 119.5f && hi < 120.5f && tempo_lo == 120 && tempo_hi == 120,
                  "a lone late tick doesn't move the tempo");
        }});

    // a host that sends its ticks two at a time (in one USB packet), started once FRIZZ is up:
    // whichever tick of a pair it locks on, it follows the clock's tempo, not half of it (#92)
    cases.push_back({"pairs-late", [] {
        RunMs(kReadyMs);
        bool all = true;
        for (int phase = 0; phase < 4; phase++)
        {
            StartUsb(Clock(120., .3, true, 0., true));
            RunMs(8000);
            const ClockState c = Probe();
            Report("pairs from %.0f ms: %.2f BPM, FX at %d", BlockMs() - 8000., c.midi_bpm, c.tempo);
            all &= fabsf(c.midi_bpm - 120.f) < 1.f && c.tempo == 120;
            usb.Stop();
            RunMs(1000 + 7 * phase); // lost, then found again on another phase
        }
        Check(all, "pairs-late: a clock in pairs started after power-on is followed at its tempo (#92)");
    }});

    return RunCases();
}
