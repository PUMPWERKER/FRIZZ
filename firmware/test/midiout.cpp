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

/** The phases the loop's start is tried in, 0.5 ms apart: a tick at 120 BPM */
static const int kPhases = 42;

/** Around status (Start, Continue), the one in v, as Record()s under key: "key" the ms from
 *  it to the tick after it (1e3 if there's none, or not one status), "key-next" from that
 *  tick to the next (one tick, not two at once), "key-before" from the last tick before it
 *  to the one after it */
static void RecordAround(const std::string& key, const std::vector<MidiOutByte>& v, uint8_t status)
{
    std::vector<double> before, after;
    int n = 0;
    for (const MidiOutByte& b : v)
        if (b.byte == status)
            n++;
        else if (b.byte == 0xF8)
            (n ? after : before).push_back(b.ms);
    const double at = [&] {
        for (const MidiOutByte& b : v)
            if (b.byte == status)
                return b.ms;
        return 0.;
    }();
    if (n != 1 || after.size() < 2 || before.empty())
    {
        Record(key, 1e3);
        return;
    }
    Record(key, after[0] - at);
    Record(key + "-next", after[1] - after[0]);
    Record(key + "-before", after[0] - before.back());
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

/** The ticks' mean phase on the 120 BPM grid, ms, from the first of them */
static double Phase(const std::vector<MidiOutByte>& v, double origin)
{
    double sum = 0.;
    int n = 0;
    for (const MidiOutByte& b : v)
        if (b.byte == 0xF8)
        {
            double ph = fmod(b.ms - origin, kTick120);
            if (ph > kTick120 / 2)
                ph -= kTick120;
            sum += ph;
            n++;
        }
    return n ? sum / n : 1e9;
}

int main()
{
    // the finalizer's Loud (#345): while its limiter delays the output 1 ms, the clock out is
    // 1 ms later too, and back once it's off
    cases.push_back({"fin-delay", [] {
        RunMs(kReadyMs);
        SetOut(1);
        RunMs(500);
        TakeSent();
        RunMs(2000);
        const std::vector<MidiOutByte> before = TakeSent().trs;
        const double origin = before.empty() ? 0. : before[0].ms;
        for (uint8_t b : {0xBF, 27, 10}) // Loud a little up: its limiter and delay in
            UsbMidi(b);
        RunMs(500);
        TakeSent();
        RunMs(2000);
        const std::vector<MidiOutByte> delayed = TakeSent().trs;
        for (uint8_t b : {0xBF, 27, 0})
            UsbMidi(b);
        RunMs(500);
        TakeSent();
        RunMs(2000);
        const std::vector<MidiOutByte> after = TakeSent().trs;
        const double a = Phase(before, origin), d = Phase(delayed, origin), z = Phase(after, origin);
        Report("fin-delay: the ticks' phase %.3f ms, with Loud %.3f, after %.3f", a, d, z);
        Check(fabs(d - a - 1.) < .1, "fin-delay: Loud up: the clock out 1 ms later, as the output");
        Check(fabs(z - a) < .1, "fin-delay: Loud at 0: back where it was");
        Check(Count(delayed, 0xF8) >= 95, "fin-delay: and no tick lost");
    }});

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

    // #198: the loop's start in every phase of the free-running clock, a tick (20.8 ms at 120)
    // in blocks (0.5 ms): its Start goes with its first tick on the jack and over USB, never
    // one tick late because the free-running one went out just before, nor with two. Judged
    // over all phases together (the summary)
    for (int step = 0; step < kPhases; step++)
        cases.push_back({"start-phase-" + std::to_string(step), [step] {
            RunMs(kReadyMs + .5 * step);
            SetOut(2);
            Tap("KEY_28");
            RunMs(900);
            TakeSent(); // with the ticks before it
            RunMs(100);
            Tap("KEY_28");
            RunUntil([] { return Probe().loop_state == 2; }, 500);
            RunMs(200);
            const Sent s = TakeSent();
            RecordAround("start-trs", s.trs, 0xFA);
            RecordAround("start-usb", s.usb, 0xFA);
        }});

    // the same for playing on after a pause, Continue with its 16th's tick: the paused loop's
    // grid ran on, so its ticks are on it already
    for (int step = 0; step < kPhases; step++)
        cases.push_back({"continue-phase-" + std::to_string(step), [step] {
            RunMs(kReadyMs);
            SetOut(2);
            RecordLoop(1000);
            RunMs(500);
            Tap("KEY_27"); // pause
            RunMs(400 + .5 * step);
            TakeSent();
            RunMs(100);
            Tap("KEY_27"); // play on
            RunMs(300);
            const Sent s = TakeSent();
            RecordAround("continue-trs", s.trs, 0xFB);
            RecordAround("continue-usb", s.usb, 0xFB);
        }});

    // and out of a standstill (#174), whose clock ran at 50 BPM, off the loop's grid: a tick
    // of it (50 ms) in 2.5 ms steps
    for (int step = 0; step < 20; step++)
        cases.push_back({"standstill-phase-" + std::to_string(step), [step] {
            RunMs(kReadyMs);
            SetOut(2);
            RecordLoop(1000);
            RunMs(500);
            Turn(5, -98); // 1/16x, then a standstill
            RunMs(1800 + 2.5 * step);
            TakeSent();
            RunMs(200);
            Tap("ENC_5_SW"); // 1x
            RunMs(1000);
            const Sent s = TakeSent();
            RecordAround("standstill-trs", s.trs, 0xFB);
            RecordAround("standstill-usb", s.usb, 0xFB);
        }});

    // #174: a standstill is a pause to the gear behind: Stop going into it, the clock running
    // on at 50 BPM; turned out of it, Song Position, then Continue on that 16th
    cases.push_back({"standstill", [] {
        RunMs(kReadyMs);
        SetOut(1);
        RecordLoop(2000);
        RunMs(1000);
        TakeSent();
        Turn(5, -98); // 1/16x, then a standstill
        RunMs(4000);
        Sent s = TakeSent();
        Check(Count(s.trs, 0xFC) == 1 && Count(s.trs, 0xFB) == 0, "standstill: Stop goes out, once");
        RunMs(3000);
        s = TakeSent();
        double mean, worst;
        Gaps(s.trs, mean, worst);
        Report("standstill: a tick every %.2f ms (50 BPM: %.2f)", mean, 60000. / (50. * 24.));
        Check(fabs(mean - 60000. / (50. * 24.)) < .2 && Count(s.trs, 0xFC) == 0,
              "standstill: the clock runs on at 50 BPM, nothing else");
        Tap("ENC_5_SW"); // 1x
        RunMs(1500);
        s = TakeSent();
        size_t spp = 0, cont = 0;
        while (spp < s.trs.size() && s.trs[spp].byte != 0xF2)
            spp++;
        while (cont < s.trs.size() && s.trs[cont].byte != 0xFB)
            cont++;
        Check(spp + 2 < s.trs.size() && cont + 1 < s.trs.size() && spp < cont && Count(s.trs, 0xFB) == 1,
              "standstill: turned out of it, Song Position, then Continue");
        if (cont + 1 < s.trs.size())
            Check(s.trs[cont + 1].byte == 0xF8 && s.trs[cont + 1].ms - s.trs[cont].ms < .5,
                  "standstill: Continue goes with a tick");
    }});

    // a DAW's Start, followed (#97 D4): the loop recorded to its clock goes back to its start,
    // and so does the gear behind: Start goes out with that tick, the clock steady through it
    cases.push_back({"follow-start", [] {
        StartUsb(Clock(120., 0., true));
        RunMs(kReadyMs);
        SetOut(1);
        for (uint8_t b : {0xF0, 0x7D, 0x43, 0x48, 0x13, 0x01, 0x01, 0xF7}) // transport following
            UsbMidi(b);
        Press("KEY_27", true); // PLAY held, LOOP: quantized
        RunMs(80);
        Tap("KEY_28");
        Press("KEY_27", false);
        RunMs(2500);
        Tap("KEY_28"); // closes at the end of bar 2
        RunUntil([] { return Probe().loop_state == 2; }, 2500);
        RunMs(2600);
        TakeSent();
        UsbMidi(0xFA);
        RunMs(1000);
        const Sent s = TakeSent();
        double mean, worst;
        Gaps(s.trs, mean, worst);
        std::string seq;
        for (const MidiOutByte& b : s.trs)
            if (b.byte != 0xF8)
            {
                char h[8];
                snprintf(h, sizeof(h), "%02X ", b.byte);
                seq += h;
            }
        Report("follow-start: out %s; a tick every %.3f ms (+-%.2f)", seq.c_str(), mean, worst);
        size_t start = 0;
        while (start < s.trs.size() && s.trs[start].byte != 0xFA)
            start++;
        Check(start + 1 < s.trs.size() && s.trs[start + 1].byte == 0xF8
                  && s.trs[start + 1].ms - s.trs[start].ms < .5 && Count(s.trs, 0xFA) == 1,
              "follow-start: the loop back at its start sends Start, with its tick");
        Check(fabs(mean - kTick120) < .1 && worst < .8, "follow-start: the clock steady through it");
        // paused, Start plays it from its start: as PLAY does, Song Position of the next 16th
        // (the first after the start, 1), then Continue on its tick
        UsbMidi(0xFC);
        RunMs(500);
        TakeSent();
        UsbMidi(0xFA);
        RunMs(500);
        const Sent p = TakeSent();
        size_t spp = 0, cont = 0;
        while (spp < p.trs.size() && p.trs[spp].byte != 0xF2)
            spp++;
        while (cont < p.trs.size() && p.trs[cont].byte != 0xFB)
            cont++;
        const bool both = spp + 2 < p.trs.size() && cont < p.trs.size();
        Report("follow-start: paused, Start: Song Position %d, Continue %.1f ms after it",
               both ? p.trs[spp + 1].byte : -1, both ? p.trs[cont].ms - p.trs[spp].ms : 0.);
        Check(both && p.trs[spp + 1].byte == 1 && p.trs[spp + 2].byte == 0
                  && fabs(p.trs[cont].ms - p.trs[spp].ms - 125.) < 2. && Count(p.trs, 0xFA) == 0,
              "follow-start: paused, Start: Song Position 1, Continue a 16th after, on the loop's 16th 1");
    }});

    // MIDI feedback on the jack (#104): its CCs fill the gaps, the clock keeps its time, a
    // tick even between a CC's bytes
    cases.push_back({"feedback", [] {
        StartUsb(Clock(120., 0., true));
        RunMs(kReadyMs);
        SetOut(1);
        RunMs(500);
        TakeSent();
        RunMs(2000); // without feedback: the clock as passed on over USB's 1 ms frames
        double base_mean, base_worst;
        Gaps(TakeSent().trs, base_mean, base_worst);
        for (uint8_t b : {0xF0, 0x7D, 0x43, 0x48, 0x3E, 0x03, 0x01, 0x23, 0x08, 0x00, 0x01, 0xF7})
            UsbMidi(b); // kCmdDev SET: midi_feedback 1
        RunMs(100);
        TakeSent();
        // every effect picked and two knobs turned: the focus's values, and theirs, in a rush
        for (int key = 1; key <= 13; key++)
        {
            Tap(("KEY_" + std::to_string(key)).c_str(), 20);
            Turn(4, 30);
            Turn(1, -30);
            RunMs(40);
        }
        RunMs(1500);
        const Sent s = TakeSent();
        int ccs = 0;
        for (const MidiOutByte& b : s.trs)
            ccs += (b.byte & 0xF0) == 0xB0;
        double mean, worst;
        Gaps(s.trs, mean, worst);
        Report("feedback: %d CCs on the jack; a tick every %.3f ms (+-%.2f), without them +-%.2f", ccs,
               mean, worst, base_worst);
        Check(ccs > 100, "feedback: the hand's changes go out of the jack as CCs");
        Check(fabs(mean - kTick120) < .05 && worst <= base_worst + .05,
              "feedback: the clock's ticks keep their time through them, as without feedback");
    }});

    // on a battery, no USB host: MIDI out's USB messages aren't tried, nor kept for later
    // (#84); the jack's clock as ever
    cases.push_back({"no-host", [] {
        RunMs(kReadyMs);
        SetOut(2);
        RunMs(500);
        SetUsbHost(false);
        TakeSent();
        TakeUsbOut();
        RunMs(2000);
        const Sent s = TakeSent();
        const bool none = s.usb.empty() && TakeUsbOut().empty();
        SetUsbHost(true);
        RunMs(500);
        const Sent back = TakeSent();
        Report("no-host: %zu ticks on the jack, %zu bytes over USB; with a host again %zu", s.trs.size(),
               s.usb.size(), back.usb.size());
        Check(Count(s.trs, 0xF8) > 80 && none, "no-host: without a USB host nothing goes to USB, the jack's clock runs");
        Check(Count(back.usb, 0xF8) > 15 && Count(back.usb, 0xF8) < 30,
              "no-host: with a host again, USB's clock goes on, without the ticks from meanwhile");
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

    // a quantized loop on a clock passed on, the gear behind stopped (no Start came): the
    // loop starts on the clock's bar line, where a passed-on tick just went, and its Start
    // goes with exactly one tick, on the jack and over USB (#198)
    cases.push_back({"handover-stopped", [] {
        RunMs(kReadyMs);
        SetOut(2);
        StartTrs(Clock(120.));
        RunMs(1000);
        Press("KEY_27", true); // PLAY held, LOOP: quantized
        RunMs(80);
        Tap("KEY_28");
        RunMs(20);
        Press("KEY_27", false);
        RunUntil([] { return Probe().loop_state == 1; }, 3000);
        RunMs(1000);
        Tap("KEY_28"); // the bar ends it
        RunUntil([] { return Probe().loop_state == 2; }, 3000);
        RunMs(1000);
        const Sent s = TakeSent();
        for (const std::vector<MidiOutByte>* v : {&s.trs, &s.usb})
        {
            const char* port = v == &s.trs ? "the jack" : "USB";
            size_t start = 0;
            while (start < v->size() && (*v)[start].byte != 0xFA)
                start++;
            std::vector<double> after;
            for (size_t i = start + 1; i < v->size() && after.size() < 2; i++)
                if ((*v)[i].byte == 0xF8)
                    after.push_back((*v)[i].ms);
            const bool both = start < v->size() && after.size() == 2;
            Report("handover-stopped: %s: Start, a tick %.2f ms on, the next %.2f ms after it", port,
                   both ? after[0] - (*v)[start].ms : -1., both ? after[1] - after[0] : -1.);
            Check(both && after[0] - (*v)[start].ms < 2. && after[1] - after[0] > .5 * kTick120,
                  (std::string("handover-stopped: ") + port + ": Start with one tick").c_str());
        }
    }});

    // Start came in on the jack and went out over USB only, then a loop: the jack's gear never
    // got a Start, so it gets the loop's; USB's already plays, so not it (#166)
    cases.push_back({"handover-trs", [] {
        RunMs(kReadyMs);
        SetOut(2);
        StartTrs(Clock(120.));
        RunMs(1000);
        Midi(0xFA);
        RunMs(500);
        TakeSent();
        RecordLoop(2000);
        RunMs(1000);
        const Sent s = TakeSent();
        Report("handover-trs: Start %d to the jack, %d over USB", Count(s.trs, 0xFA), Count(s.usb, 0xFA));
        Check(Count(s.trs, 0xFA) == 1, "handover-trs: the loop's Start goes to the jack, which had none");
        Check(Count(s.usb, 0xFA) == 0, "handover-trs: not over USB, whose gear already plays");
        // the jack's Start with its tick, whatever went over USB just before; USB's gear,
        // which plays on, gets no tick more for it (#198)
        size_t start = 0;
        while (start < s.trs.size() && s.trs[start].byte != 0xFA)
            start++;
        double closest = 1e3;
        for (size_t i = 1; i < s.usb.size(); i++)
            closest = std::min(closest, s.usb[i].ms - s.usb[i - 1].ms);
        Report("handover-trs: USB's ticks at least %.2f ms apart", closest);
        Check(start + 1 < s.trs.size() && s.trs[start + 1].byte == 0xF8
                  && s.trs[start + 1].ms - s.trs[start].ms < .5,
              "handover-trs: the jack's Start goes with a tick");
        Check(closest > .5 * kTick120, "handover-trs: and no tick more over USB");
    }});

    // MainLoop held up (a scene written to a slow card) while the loop pauses and plays on:
    // USB's ring fills with ticks, but the transport still has room in it, and goes out once
    // MainLoop runs again; ticks are dropped instead (#166)
    cases.push_back({"usb-held", [] {
        RunMs(kReadyMs);
        SetOut(2);
        for (uint8_t b : {0xF0, 0x7D, 0x43, 0x48, 0x13, 0x01, 0x01, 0xF7}) // transport following on
            UsbMidi(b);
        RunMs(20);
        RecordLoop(2000);
        RunMs(1000);
        TakeSent();
        HoldMain(1500);
        RunMs(800);
        Midi(0xFC); // the jack's Stop pauses the loop, in the audio callback
        RunMs(200);
        Midi(0xFB); // and its Continue plays on
        RunMs(1500);
        const Sent s = TakeSent();
        std::string usb;
        for (const MidiOutByte& b : s.usb)
            if (b.byte != 0xF8)
                usb += (usb.empty() ? "" : " ") + std::to_string(b.byte);
        Report("usb-held: USB's transport after the hold: \"%s\", %d ticks", usb.c_str(), Count(s.usb, 0xF8));
        Check(Count(s.trs, 0xFC) == 1 && Count(s.trs, 0xFB) == 1, "usb-held: the jack sends Stop and Continue meanwhile");
        Check(Count(s.usb, 0xFC) == 1 && Count(s.usb, 0xF2) == 1 && Count(s.usb, 0xFB) == 1,
              "usb-held: USB's Stop, Song Position and Continue go out after the hold, none dropped");
    }});

    // a clock on the jack isn't sent back there, but a loop leads, and its clock goes out
    cases.push_back({"lead", [] {
        RunMs(kReadyMs);
        SetOut(1);
        // what FRIZZ's own clock sent before one came in isn't the jack's: whether a tick went
        // out then depends on the grid's phase since power-on (#195)
        TakeSent();
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

    // at double, a host stalled and catching up, 4 ticks in one block: 8 go out, not the 4
    // that a block passes on at most of what came (audit 2026-10-16, M2)
    cases.push_back({"factor-bunch", [] {
        RunMs(kReadyMs);
        SetOut(1);
        StartUsb(Clock(120.));
        RunMs(1000);
        Factor(1); // x2
        RunMs(1000);
        bool all = true;
        for (int round = 0; round < 3; round++)
        {
            usb.Stop();
            RunMs(3 * kTick120); // the last one's halfway tick long gone
            TakeSent();
            for (int i = 0; i < 4; i++)
                UsbMidi(0xF8);
            RunMs(.75 * kTick120); // the bunch's halfway one too
            const int out = Count(TakeSent().trs, 0xF8);
            Report("double, 4 ticks in one block: %d out", out);
            all = all && out == 8;
            StartUsb(Clock(120.));
            RunMs(500);
        }
        Check(all, "factor-bunch: at double, a bunch of 4 goes out as 8");
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

    // ===== #116: the factor on every tempo, so on the clock FRIZZ makes too =====

    // no clock, no loop: the last tempo, at double and half
    cases.push_back({"factor-free", [] {
        RunMs(kReadyMs);
        SetOut(1);
        Factor(1); // x2
        TakeSent();
        RunMs(5000);
        Sent s = TakeSent();
        double mean, worst;
        Gaps(s.trs, mean, worst);
        Report("factor-free: x2, a tick every %.3f ms (+-%.2f)", mean, worst);
        Check(fabs(mean - kTick120 / 2) < .05 && worst < .6, "factor-free: x2 of the last 120, ticks at 240");
        Factor(1); // x1/2
        TakeSent();
        RunMs(5000);
        s = TakeSent();
        Gaps(s.trs, mean, worst);
        Report("factor-free: x1/2, a tick every %.3f ms (+-%.2f)", mean, worst);
        Check(fabs(mean - 2 * kTick120) < .1 && worst < .8, "factor-free: x1/2, ticks at 60");
    }});

    // a loop at x1/2: the ticks at half its tempo, and on PLAY the Song Position in the
    // half-time 16ths, Continue on its tick
    cases.push_back({"factor-loop", [] {
        RunMs(kReadyMs);
        SetOut(1);
        RecordLoop(2000);
        const ClockState one = Probe();
        // 4 beats guessed: at x1/2 2 beats a pass
        const int beats = static_cast<int>(one.loop_length / kSr * one.fx_bpm / 60. + .5) / 2;
        Factor(2); // x1/2
        TakeSent();
        RunMs(4000);
        Sent s = TakeSent();
        const double tick = one.loop_length / kSr * 1000. / (beats * 24.);
        double mean, worst;
        Gaps(s.trs, mean, worst);
        Report("factor-loop: x1/2, %d beats a pass, a tick every %.3f ms, sent every %.3f (+-%.2f)",
               beats, tick, mean, worst);
        Check(beats == 2 && fabs(mean - tick) < .05 && worst < .8, "factor-loop: x1/2, the ticks at half the loop's tempo");
        Tap("KEY_27"); // pause
        RunMs(1000);
        s = TakeSent();
        Check(Count(s.trs, 0xFC) == 1, "factor-loop: a pause sends Stop");
        Tap("KEY_27"); // play on
        RunMs(1000);
        s = TakeSent();
        size_t spp = 0, cont = 0;
        while (spp < s.trs.size() && s.trs[spp].byte != 0xF2)
            spp++;
        while (cont < s.trs.size() && s.trs[cont].byte != 0xFB)
            cont++;
        const bool both = spp + 2 < s.trs.size() && cont + 1 < s.trs.size() && spp < cont;
        Check(both, "factor-loop: PLAY sends Song Position, then Continue");
        if (both)
        {
            const int pos = s.trs[spp + 1].byte | s.trs[spp + 2].byte << 7;
            const double since = BlockMs() - s.trs[cont].ms;
            double at16 = Probe().loop_pos * beats * 4 - since / (tick * 6.);
            at16 = fmod(at16 + beats * 4, beats * 4);
            Report("factor-loop: Song Position %d, the loop at half-time 16th %.2f then", pos, at16);
            Check(pos >= 0 && pos < beats * 4 && (fabs(at16 - pos) < .3 || fabs(at16 - pos) > beats * 4 - .3),
                  "factor-loop: on the half-time 16th the Song Position named");
        }
    }});

    // a loop of 3 beats at x1/2: a beat and a half a pass, the clock steady across its starts
    // (the grid over two passes, decided in #116): 36 ticks a pass
    cases.push_back({"factor-odd", [] {
        RunMs(kReadyMs);
        SetOut(1);
        twin::Press("KEY_26", true); // SHIFT + LOOP: 120
        for (int i = 0; i < 5; i++)
        {
            Tap("KEY_28");
            RunMs(440);
        }
        twin::Press("KEY_26", false);
        RunMs(300);
        RecordLoop(1500);
        Factor(2); // x1/2
        TakeSent();
        RunMs(6000);
        const Sent s = TakeSent();
        const ClockState c = Probe();
        const double tick = c.loop_length / kSr * 1000. / 36.;
        double mean, worst;
        Gaps(s.trs, mean, worst);
        Report("factor-odd: a tick every %.3f ms (+-%.2f), %.3f for 36 a pass", mean, worst, tick);
        Check(fabs(mean - tick) < .05 && worst < .8, "factor-odd: 3 beats at x1/2, 36 ticks a pass, steady across its starts");
    }});

    return RunCases([](const std::map<std::string, std::vector<double>>& r) {
        // in how many phases Start or Continue went out without its tick right after it (on
        // the jack the UART's next byte, over USB MainLoop's next passes), or with two; and
        // how close the tick before came to it: what the fix costs gear that follows tempo
        for (const char* key : {"start-trs", "start-usb", "continue-trs", "continue-usb",
                                "standstill-trs", "standstill-usb"})
        {
            const std::string k = key;
            auto it = r.find(k);
            if (it == r.end())
                continue; // its cases not run (CASES)
            const bool usb = k.find("usb") != std::string::npos;
            int late = 0;
            for (double g : it->second)
                late += g >= (usb ? 2. : .5);
            double next = 1e3, before = 1e3;
            for (double g : r.count(k + "-next") ? r.at(k + "-next") : std::vector<double>())
                next = std::min(next, g);
            for (double g : r.count(k + "-before") ? r.at(k + "-before") : std::vector<double>())
                before = std::min(before, g);
            Report("%s: %d of %d phases without the tick right after (the gap at most %.2f ms); "
                   "the next one at least %.2f ms on; the one before at least %.2f ms before",
                   key, late, (int)it->second.size(), Worst(r, key), next, before);
            Check(late == 0, (k + ": in every phase, the tick right after it").c_str());
            Check(next > 15., (k + ": and only one").c_str());
        }
    });
}
