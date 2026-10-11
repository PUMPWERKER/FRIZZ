// midiout.cpp: MIDI out (MidiOut.h, #60) on the virtual CHOMPI: the clock, Start, Stop,
// Continue and Song Position that go out of the MIDI jack and over USB, timed as the UART
// sends them. Off at first; passing an incoming clock on, by the clock factor, never back
// where it comes from; generating it from a loop or the last tempo; a loop's Start, Stop and
// Continue; nothing stale from the boot screen. Each case on a fresh device. Run by unit.sh
// midiout.
#include "timing.h"
#include "twin.h"

static const double kTick120 = 60000. / (120. * 24.); // ms

/** MIDI out on: 0 off, 1 the jack, 2 the jack and USB, by FRIZZ's SysEx over USB */
static void SetOut(uint8_t ports)
{
    for (uint8_t b : {0xF0, 0x7D, 0x43, 0x48, 0x13, 0x03, (int)ports, 0xF7})
        UsbMidi(b);
    RunMs(20);
}

/** What MIDI out sent since the last call, by port */
struct Sent
{
    std::vector<MidiOutByte> trs, usb;
};
static Sent TakeSent()
{
    Sent s;
    for (const MidiOutByte& b : TakeMidiOut())
        (b.usb ? s.usb : s.trs).push_back(b);
    return s;
}

static int Count(const std::vector<MidiOutByte>& v, uint8_t byte)
{
    int n = 0;
    for (const MidiOutByte& b : v)
        n += b.byte == byte;
    return n;
}

/** The gaps between the ticks: their mean, and the largest distance from it */
static void Gaps(const std::vector<MidiOutByte>& v, double& mean, double& worst)
{
    std::vector<double> t;
    for (const MidiOutByte& b : v)
        if (b.byte == 0xF8)
            t.push_back(b.ms);
    mean = worst = 0.;
    if (t.size() < 2)
        return;
    mean = (t.back() - t.front()) / (t.size() - 1);
    for (size_t i = 1; i < t.size(); i++)
        worst = std::max(worst, fabs(t[i] - t[i - 1] - mean));
}

/** LOOP twice, ms apart: an unquantized loop, playing */
static void RecordLoop(double ms)
{
    Tap("KEY_28");
    RunMs(ms);
    Tap("KEY_28");
    RunUntil([] { return Probe().loop_state == 2; }, 500);
}

/** The settings page's G#, presses times: x1 -> x2 -> x1/2 */
static void Factor(int presses)
{
    SetToggle(true);
    RunMs(300);
    for (int i = 0; i < presses; i++)
    {
        Tap("KEY_19");
        RunMs(100);
    }
    SetToggle(false);
    RunMs(300);
}

int main()
{
    // off at first: a clock in, a loop, a pause: nothing goes out anywhere
    cases.push_back({"off", [] {
        RunMs(kReadyMs);
        StartUsb(Clock(120.));
        RunMs(2000);
        UsbMidi(0xFA);
        RecordLoop(2000);
        RunMs(1000);
        Tap("KEY_27");
        RunMs(500);
        Tap("KEY_27");
        RunMs(1000);
        const Sent s = TakeSent();
        Check(s.trs.empty() && s.usb.empty(), "off: off at first, nothing goes out of the jack or over USB");
    }});

    // on, with no clock and no loop: the last tempo (120 at power-on), and no Start
    cases.push_back({"free", [] {
        RunMs(kReadyMs);
        SetOut(1);
        RunMs(100);
        TakeSent();
        RunMs(5000);
        const Sent s = TakeSent();
        double mean, worst;
        Gaps(s.trs, mean, worst);
        Report("free: %d ticks in 5 s, every %.3f ms (+-%.2f)", Count(s.trs, 0xF8), mean, worst);
        Check(fabs(mean - kTick120) < .05 && worst < .6,
              "free: the jack's clock runs at 120 BPM by itself, steady to the block");
        Check(Count(s.trs, 0xF8) == (int)s.trs.size(), "free: ticks only, no Start");
        Check(s.usb.empty(), "free: nothing over USB while it's set to the jack");
    }});

    // a clock in over USB passed on to the jack, tick for tick; never back to USB
    for (bool in_usb : {true, false})
        cases.push_back({in_usb ? "pass-usb" : "pass-trs", [in_usb] {
            RunMs(kReadyMs);
            SetOut(2);
            if (in_usb)
                StartUsb(Clock(120.));
            else
                StartTrs(Clock(120.));
            RunMs(2000);
            TakeSent();
            const uint64_t before = in_usb ? usb.Sent() : trs.Sent();
            RunMs(5000);
            const Sent s = TakeSent();
            const int in = static_cast<int>((in_usb ? usb.Sent() : trs.Sent()) - before);
            const std::vector<MidiOutByte>& there = in_usb ? s.trs : s.usb;
            const std::vector<MidiOutByte>& back = in_usb ? s.usb : s.trs;
            double mean, worst;
            Gaps(there, mean, worst);
            Report("%s: %d ticks in, %d out the other way, every %.3f ms (+-%.2f)",
                   in_usb ? "pass-usb" : "pass-trs", in, Count(there, 0xF8), mean, worst);
            Check(abs(Count(there, 0xF8) - in) <= 1,
                  in_usb ? "pass-usb: USB's clock goes out of the jack, tick for tick"
                         : "pass-trs: the jack's clock goes out over USB, tick for tick");
            Check(back.empty(), in_usb ? "pass-usb: and nothing goes back over USB"
                                       : "pass-trs: and nothing goes back out of the jack");
        }});

    // the clock factor: at double a tick more halfway, at half every other one
    cases.push_back({"factor", [] {
        RunMs(kReadyMs);
        SetOut(1);
        StartUsb(Clock(120.));
        RunMs(1000);
        Factor(1); // x2
        RunMs(1000);
        TakeSent();
        uint64_t before = usb.Sent();
        RunMs(5000);
        Sent s = TakeSent();
        int in = static_cast<int>(usb.Sent() - before);
        double mean, worst;
        Gaps(s.trs, mean, worst);
        Report("double: %d in, %d out, every %.3f ms (+-%.2f)", in, Count(s.trs, 0xF8), mean, worst);
        // USB's clock comes in up to a frame late, so the halfway tick lands up to that off
        Check(abs(Count(s.trs, 0xF8) - 2 * in) <= 2 && fabs(mean - kTick120 / 2) < .05 && worst < 1.2,
              "factor: at double, twice the ticks, evenly");
        Factor(1); // x1/2
        RunMs(1000);
        TakeSent();
        before = usb.Sent();
        RunMs(5000);
        s = TakeSent();
        in = static_cast<int>(usb.Sent() - before);
        Gaps(s.trs, mean, worst);
        Report("half: %d in, %d out, every %.3f ms (+-%.2f)", in, Count(s.trs, 0xF8), mean, worst);
        Check(abs(2 * Count(s.trs, 0xF8) - in) <= 2 && fabs(mean - 2 * kTick120) < .1,
              "factor: at half, every other one");
    }});

    // Start and Stop from the clock's input go out too, while there's no loop
    cases.push_back({"transport", [] {
        RunMs(kReadyMs);
        SetOut(1);
        StartUsb(Clock(120.));
        RunMs(1000);
        TakeSent();
        UsbMidi(0xFA);
        RunMs(500);
        UsbMidi(0xFC);
        RunMs(100);
        const Sent s = TakeSent();
        Check(Count(s.trs, 0xFA) == 1 && Count(s.trs, 0xFC) == 1,
              "transport: a DAW's Start and Stop are passed on");
    }});

    // a loop leads: Start with its first tick, its tempo, Stop on a pause, and on PLAY the
    // Song Position of the next 16th, then Continue with that 16th's tick
    cases.push_back({"loop", [] {
        RunMs(kReadyMs);
        SetOut(1);
        Tap("KEY_28");
        RunMs(2000);
        TakeSent();
        Tap("KEY_28");
        RunUntil([] { return Probe().loop_state == 2; }, 500);
        RunMs(3000);
        Sent s = TakeSent();
        size_t start = 0;
        while (start < s.trs.size() && s.trs[start].byte != 0xFA)
            start++;
        Check(start < s.trs.size() && Count(s.trs, 0xFA) == 1, "loop: a new loop sends Start");
        Check(start + 1 < s.trs.size() && s.trs[start + 1].byte == 0xF8
                  && s.trs[start + 1].ms - s.trs[start].ms < .5,
              "loop: with its first tick right after");
        const ClockState c = Probe();
        // an unquantized loop's beats are fitted to the tempo (TempoClock.h), which says them
        const int beats = static_cast<int>(c.loop_length / kSr * c.fx_bpm / 60. + .5);
        const double tick = c.loop_length / kSr * 1000. / (beats * 24.);
        double mean, worst;
        std::vector<MidiOutByte> after(s.trs.begin() + start, s.trs.end());
        Gaps(after, mean, worst);
        Report("loop: %d beats, a tick every %.3f ms, sent every %.3f (+-%.2f)", beats, tick, mean,
               worst);
        Check(fabs(mean - tick) < .05 && worst < .8, "loop: the ticks at the loop's tempo");

        Tap("KEY_27"); // pause
        RunMs(1000);
        s = TakeSent();
        Check(Count(s.trs, 0xFC) == 1, "loop: a pause sends Stop");
        Check(Count(s.trs, 0xF8) > 40, "loop: and the clock runs on while paused");

        Tap("KEY_27"); // play on
        RunMs(1000);
        s = TakeSent();
        size_t spp = 0, cont = 0;
        while (spp < s.trs.size() && s.trs[spp].byte != 0xF2)
            spp++;
        while (cont < s.trs.size() && s.trs[cont].byte != 0xFB)
            cont++;
        const bool both = spp + 2 < s.trs.size() && cont + 1 < s.trs.size() && spp < cont;
        Check(both, "loop: PLAY sends Song Position, then Continue");
        if (both)
        {
            const int pos = s.trs[spp + 1].byte | s.trs[spp + 2].byte << 7;
            Report("loop: Song Position %d (16ths), Continue %.2f ms after it", pos,
                   s.trs[cont].ms - s.trs[spp].ms);
            Check(pos >= 0 && pos < beats * 4, "loop: the Song Position is in the loop");
            Check(s.trs[cont + 1].byte == 0xF8 && s.trs[cont + 1].ms - s.trs[cont].ms < .5,
                  "loop: Continue goes with a tick");
            const ClockState p = Probe();
            // the 16th the Continue went with, from the loop's position now and the time since
            const double since = BlockMs() - s.trs[cont].ms;
            double at16 = p.loop_pos * beats * 4 - since / (tick * 6.);
            at16 = fmod(at16 + beats * 4, beats * 4);
            Report("loop: the loop was at 16th %.2f then", at16);
            Check(fabs(at16 - pos) < .3 || fabs(at16 - pos) > beats * 4 - .3,
                  "loop: on the 16th the Song Position named");
        }
    }});

    // a DAW's Start was passed on, then a quantized loop: no second Start, and the ticks carry
    // on, its start on the clock's bar line
    cases.push_back({"handover", [] {
        RunMs(kReadyMs);
        SetOut(1);
        StartUsb(Clock(120.));
        RunMs(1000);
        UsbMidi(0xFA);
        RunMs(500);
        TakeSent();
        Press("KEY_27", true); // PLAY held, LOOP: quantized
        RunMs(80);
        Tap("KEY_28");
        RunMs(20);
        Press("KEY_27", false);
        RunUntil([] { return Probe().loop_state == 1; }, 3000);
        RunMs(1000);
        Tap("KEY_28"); // the bar ends it
        RunUntil([] { return Probe().loop_state == 2; }, 3000);
        RunMs(2000);
        const Sent s = TakeSent();
        double mean, worst;
        Gaps(s.trs, mean, worst);
        Report("handover: ticks every %.3f ms (+-%.2f) across the loop's start", mean, worst);
        Check(Count(s.trs, 0xFA) == 0, "handover: the gear already plays, so the loop sends no Start");
        Check(worst < .5 * kTick120, "handover: the ticks carry on across the loop's start");
    }});

    // a clock on the jack isn't sent back there, but a loop leads, and its clock goes out
    cases.push_back({"lead", [] {
        RunMs(kReadyMs);
        SetOut(1);
        StartTrs(Clock(120.));
        RunMs(2000);
        Sent s = TakeSent();
        Check(s.trs.empty(), "lead: the jack's own clock isn't sent back to it");
        RecordLoop(2000);
        RunMs(2000);
        s = TakeSent();
        Check(Count(s.trs, 0xF8) > 80 && Count(s.trs, 0xFA) == 1,
              "lead: with a loop FRIZZ leads: Start and its clock go out of the jack");
    }});

    // at double, a host that sends its ticks in pairs (two in a block): every one counts
    // twice, the pair's too
    cases.push_back({"factor-pairs", [] {
        RunMs(kReadyMs);
        SetOut(1);
        StartUsb(Clock(120., .3, true, 0., true));
        RunMs(1000);
        Factor(1); // x2
        RunMs(1000);
        TakeSent();
        const uint64_t before = usb.Sent();
        RunMs(5000);
        const Sent s = TakeSent();
        const int in = static_cast<int>(usb.Sent() - before);
        Report("double, pairs: %d in, %d out", in, Count(s.trs, 0xF8));
        Check(abs(Count(s.trs, 0xF8) - 2 * in) <= 2, "factor-pairs: at double, twice the ticks of a host sending pairs");
    }});

    // a Start that came while FRIZZ was starting up isn't sent out late, once it's up, nor
    // the ticks of a clock running then in one bunch
    cases.push_back({"boot-start", [] {
        StartUsb(Clock(120.));
        RunMs(500); // the boot screen
        SetOut(1);
        UsbMidi(0xFA);
        RunMs(kReadyMs - 520);
        const Sent s = TakeSent();
        Check(Count(s.trs, 0xFA) == 0, "boot-start: a Start from the boot screen doesn't go out after it");
        double closest = 1e9;
        for (size_t i = 1; i < s.trs.size(); i++)
            if (s.trs[i].byte == 0xF8 && s.trs[i - 1].byte == 0xF8)
                closest = std::min(closest, s.trs[i].ms - s.trs[i - 1].ms);
        Report("boot-start: %d ticks out, the closest two %.2f ms apart", Count(s.trs, 0xF8), closest);
        Check(Count(s.trs, 0xF8) > 100 && closest > .5 * kTick120,
              "boot-start: the clock's ticks from then don't go out in a bunch");
    }});

    return RunCases();
}
