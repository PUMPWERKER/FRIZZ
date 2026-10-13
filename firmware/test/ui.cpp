// ui.cpp: plays keys, knobs and MIDI into the virtual CHOMPI (twin/twin.h), FRIZZ's whole
// firmware from power-on with its play page, LEDs and debouncing, and checks what the LEDs show
// and what comes out. Each case boots its own device (a forked process, cases.h). Run by
// unit.sh ui.
#include <sys/wait.h>
#include <unistd.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "cases.h"
#include "check.h"
#include "script.h"
#include "twin.h"

using namespace twin;

// the play page's LEDs (NormalPage.h, FxSlots.h)
static const int kKnob1Led = 1, kPlayLed = 7, kLoopLed = 8;
static const int kBeatPulseMax = 70; // PLAY's beat with no loop: .25 white (NormalPage.h's kBeatPulse)
static const int kFilterKeyLed = 20, kShifterKeyLed = 23, kFlangerKeyLed = 19;
static const int kVolumeEncoder = 6, kKnob1Encoder = 4; // SW6, SW4
static const int kTransportRevLed = 5, kTransportFwdLed = 6, kVolumeLed = 9;
static const int kSlot1Led = 3, kCompKeyLed = 10, kTapeStopKeyLed = 14;
// the settings page's (SettingsPage.h): white keys 1, 3, 11, 14, 15 (channels; the 15th is 15
// and 16), the lower octave's D#, F#, G#, A#, the upper octave's C#, F#, G# and A#: the input's
// source (SHIFT: mono), effects and live (#177)
static const int kChannel1Led = 24, kChannel3Led = 22, kChannel11Led = 14, kChannel14Led = 11,
                 kChannel1516Led = 10, kBlockTrsKeyLed = 1, kTransportKeyLed = 2, kFactorKeyLed = 3,
                 kSourceKeyLed = 4, kMidiOutKeyLed = 5, kMonoKeyLed = 7, kInSourceKeyLed = 7,
                 kInFxKeyLed = 8, kLiveKeyLed = 9;
static const int kNumFxProbe = 15; // kCmdParams' effects and the compressor
static const int kFeedbackKeyLed = 6; // the settings page's upper D#: MIDI feedback
static const int kSaveKeyLed = 7; // KEY_23, the play page's SAVE
// the play page's scene keys (#67): the lower F#, G#, A#, the upper C#, D#, the blank scene
// first; SAVE, COPY, DELETE on the upper F#, G#, A#. The lower C# and D# are free
static const char* const kSceneKeys[5] = {"KEY_18", "KEY_19", "KEY_20", "KEY_21", "KEY_22"};
static const char *const kSaveKey = "KEY_23", *const kCopyKey = "KEY_24", *const kDeleteKey = "KEY_25";
static const int kChompiKeyLed = 0; // PanelLeds.h

static bool sine = true;
static float amp = .3f, freq = 220.f, phase = 0.f;
static float clock_bpm = 0.f; // a MIDI clock into the jack while > 0
static double clock_next = 0.;
static float usb_clock_bpm = 0.f; // and one over USB
static double usb_clock_next = 0.;
static float last_out[2] = {0.f, 0.f}, max_step = 0.f; // the master out's largest step, L or R
static float hp_rms = 0.f; // the headphones' (left) RMS over the last RunMs
static float out_peak = 0.f; // the master out's largest sample, L or R, since it was set to 0
static float mic_amp = 0.f, mic_freq = 1000.f, mic_phase = 0.f; // a sine into the mic

/** Runs blocks (half a ms each) with a sine (or silence) into AUX, and one into the mic
 *  while mic_amp > 0; the master out's RMS over the time. RunMs: as many ms */
static float RunBlocks(uint32_t blocks)
{
    float in[kBlockSize * kChannels] = {}, out[kBlockSize * kChannels];
    double sum = 0., hp_sum = 0.;
    for (uint32_t b = 0; b < blocks; b++)
    {
        while (clock_bpm > 0.f && clock_next <= NowMs())
        {
            Midi(0xF8);
            clock_next += 60000. / (clock_bpm * 24.);
        }
        while (usb_clock_bpm > 0.f && usb_clock_next <= NowMs())
        {
            UsbMidi(0xF8);
            usb_clock_next += 60000. / (usb_clock_bpm * 24.);
        }
        for (size_t i = 0; i < kBlockSize; i++)
        {
            const float s = sine ? amp * sinf(phase) : 0.f;
            phase = fmodf(phase + 2.f * float(M_PI) * freq / kSampleRate, 2.f * float(M_PI));
            in[i * kChannels + 2] = in[i * kChannels + 3] = s;
            in[i * kChannels] = mic_amp * sinf(mic_phase);
            mic_phase = fmodf(mic_phase + 2.f * float(M_PI) * mic_freq / kSampleRate, 2.f * float(M_PI));
        }
        Run(1, in, out);
        for (size_t i = 0; i < kBlockSize; i++)
        {
            const float o = out[i * kChannels + 2];
            sum += o * o;
            hp_sum += out[i * kChannels] * out[i * kChannels];
            for (int c = 0; c < 2; c++)
            {
                const float v = out[i * kChannels + 2 + c];
                max_step = std::max(max_step, fabsf(v - last_out[c]));
                out_peak = std::max(out_peak, fabsf(v));
                last_out[c] = v;
            }
        }
    }
    hp_rms = blocks ? sqrtf(hp_sum / (blocks * kBlockSize)) : 0.f;
    return blocks ? sqrtf(sum / (blocks * kBlockSize)) : 0.f;
}
static float RunMs(uint32_t ms) { return RunBlocks(ms * 2); }
#include "twintest.h" // after RunMs, which its Tap runs

/** SHIFT + the settings page's upper F#: AUX mono on / off (#177) */
static void TapMono()
{
    Press("KEY_26", true);
    RunMs(60);
    Tap("KEY_23");
    RunMs(60);
    Press("KEY_26", false);
}

/** Knob 1 turned on the settings page: the LEDs' brightness, 5 % a detent (#176) */
static void TurnBrightness(int detents)
{
    Turn(kKnob1Encoder, detents);
    RunMs(8 * static_cast<uint32_t>(abs(detents)) + 100);
}

/** The largest step between two samples of the master out over ms */
static float StepMs(uint32_t ms)
{
    max_step = 0.f;
    RunMs(ms);
    return max_step;
}

/** An LED's brightness (its brightest channel, at full scale) every ms for ms */
static std::vector<int> Watch(bool pth, int led, uint32_t ms)
{
    std::vector<int> seen;
    for (uint32_t t = 0; t < ms; t++)
    {
        RunMs(1);
        const Rgb c = pth ? PthLedFull(led) : SmtLedFull(led);
        seen.push_back(std::max(c.r, std::max(c.g, c.b)));
    }
    return seen;
}

/** How long an LED stays lit or dark at a time, in ms: the runs of Watch() between changes,
 *  without the first and the last (cut off by the watch) */
static std::vector<int> Runs(const std::vector<int>& seen, int lit_above)
{
    std::vector<int> runs;
    int len = 0;
    for (size_t i = 1; i < seen.size(); i++)
    {
        len++;
        if ((seen[i] > lit_above) != (seen[i - 1] > lit_above))
        {
            runs.push_back(len);
            len = 0;
        }
    }
    if (!runs.empty())
        runs.erase(runs.begin());
    return runs;
}

// a scene file's latch for an effect in a slot ("scene 2", "filter"): -1 if it isn't there
static int SavedLatch(const std::string& file, int slot, const char* fx)
{
    std::istringstream in(file);
    std::string line, want = "scene " + std::to_string(slot);
    bool in_slot = false;
    while (std::getline(in, line))
    {
        if (line.rfind("scene ", 0) == 0)
            in_slot = line == want;
        else if (in_slot && line.rfind(std::string(fx) + " ", 0) == 0)
            return line[strlen(fx) + 1] - '0';
    }
    return -1;
}

static int Max(const Rgb& c) { return std::max(c.r, std::max(c.g, c.b)); }

/** An SMT LED at its brightest over ms: a key that pulses, seen lit */
static Rgb Peak(int led, uint32_t ms)
{
    Rgb peak = SmtLedFull(led);
    for (uint32_t t = 0; t < ms; t++)
    {
        RunMs(1);
        const Rgb c = SmtLedFull(led);
        if (Max(c) > Max(peak))
            peak = c;
    }
    return peak;
}

/** Taps a key and counts the red blinks on a key's LED over the next 700 ms */
static int RedBlinks(const char* key, int led)
{
    Press(key, true);
    int blinks = 0;
    bool was_lit = false;
    for (int t = 0; t < 700; t++)
    {
        if (t == 60)
            Press(key, false);
        RunMs(1);
        const Rgb c = SmtLedFull(led);
        const bool lit = c.r > 128 && c.g == 0 && c.b == 0;
        blinks += lit && !was_lit;
        was_lit = lit;
    }
    return blinks;
}

/** PLAY held, LOOP: a quantized recording. The red blinks on LOOP's LED over 800 ms: 3
 *  when it's refused */
static int RedBlinksQuantized()
{
    Press("KEY_27", true);
    RunMs(80);
    Press("KEY_28", true);
    int blinks = 0;
    bool was_lit = false;
    for (int t = 0; t < 800; t++)
    {
        if (t == 60)
            Press("KEY_28", false);
        RunMs(1);
        const Rgb c = PthLedFull(kLoopLed);
        const bool lit = c.r > 128 && c.g < 40 && c.b < 40;
        blinks += lit && !was_lit;
        was_lit = lit;
    }
    Press("KEY_27", false);
    return blinks;
}

/** Holds an FX key and taps SHIFT: latched once the key is let go */
static void Latch(const char* key)
{
    Press(key, true);
    RunMs(80);
    Tap("KEY_26");
    Press(key, false);
    RunMs(200);
}

/** Keys pressed and let go to the ms: "+KEY_5" down, "-KEY_5" up, a number waits that many
 *  ms; keys in a row go down in the same ms */
static void Keys(const std::vector<const char*>& steps)
{
    for (const char* s : steps)
    {
        if (*s == '+' || *s == '-')
            Press(s + 1, *s == '+');
        else
            RunMs(static_cast<uint32_t>(atoi(s)));
    }
}

/** The FX latched, a bit each (FxId), as SysEx 0x20 answers them */
static unsigned LatchedFx(const std::string& st)
{
    return st.size() > 9 ? static_cast<uint8_t>(st[8]) << 7 | static_cast<uint8_t>(st[9]) : ~0u;
}

/** SAVE, a slot (1-4), CHOMPI to confirm */
static void Save(int slot)
{
    Tap(kSaveKey);
    RunMs(100);
    Tap(kSceneKeys[slot]);
    RunMs(300);
    Tap("KEY_26");
    RunMs(300);
}
static bool Same(const Rgb& a, const Rgb& b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

/** Runs to just past the tempo's next bar line (TempoClock.h, 12 pulses a beat, 4 a bar), so a
 *  morph started then lands a whole bar on, whatever the grid's phase since power-on */
static void ToBarLine()
{
    uint32_t was = Probe().position;
    for (int ms = 0; ms < 4000; ms++)
    {
        RunMs(1);
        const uint32_t now = Probe().position;
        if (now != was && now % 48 == 0)
            return;
        was = now;
    }
}

// MIDI (MidiControl.h): bytes into the jack, or over USB; FRIZZ listens on channel 16 at first
static void Trs(std::initializer_list<int> bytes)
{
    for (int b : bytes)
        Midi(static_cast<uint8_t>(b));
}
static void Usb(const std::vector<int>& bytes)
{
    for (int b : bytes)
        UsbMidi(static_cast<uint8_t>(b));
}
static const int kNoteOn = 0x9F, kNoteOff = 0x8F, kCC = 0xBF, kPC = 0xCF;
static const int kFilterNote = 55; // KEY_5, the 5th white key: G above the base note, 48
static const int kChompiNote = 45; // the CHOMPI key: SHIFT
// the global channel's latch of the filter's key, 5, and the compressor's threshold; the
// filter's own channel, 5, and its cutoff there, knob 1
static const int kFilterLatchCC = 106, kCompAmountCC = 24;
static const int kFilterCC = 0xB4, kFilterCutoffCC = 14;
static const int kHpCueCC = 89; // the headphone feed: 127 the master out, 0 the input alone
/** 14 bits of an answer as a knob's 0-1 (MidiControl.h's MidiToKnob), and 7 bits of a CC */
static float KnobOf(int hi, int lo)
{
    const float v = static_cast<float>((hi << 7) | lo);
    return v <= 8192.f ? .5f * v / 8192.f : .5f + .5f * (v - 8192.f) / 8191.f;
}
static float KnobOf7(int v) { return v <= 64 ? .5f * v / 64.f : .5f + .5f * (v - 64) / 63.f; }
/** A SysEx query over USB and its answer's data, or empty if none came */
static std::string Ask(const std::vector<int>& query)
{
    TakeUsbOut();
    Usb({0xF0, 0x7D, 0x43, 0x48});
    Usb(query);
    Usb({0xF7});
    RunMs(20);
    // the answer, among MIDI feedback's CCs if it's on
    const std::string out = TakeUsbOut();
    const std::string head = std::string("\xF0\x7D\x43\x48") + static_cast<char>(*query.begin() | 0x40);
    const size_t at = out.find(head), end = out.find('\xF7', at);
    if (at == std::string::npos || end == std::string::npos)
        return "";
    return out.substr(at + 5, end - at - 5);
}

/** groups 7-bit groups of a kCmdDev answer from at, the highest first */
static uint64_t DevNum(const std::string& d, size_t at, size_t groups)
{
    uint64_t v = 0;
    for (size_t i = 0; i < groups && at + i < d.size(); i++)
        v = v << 7 | static_cast<uint8_t>(d[at + i]);
    return v;
}

/** The CCs that went out of the jack since last asked, real-time bytes left out */
static std::vector<std::array<int, 3>> TrsCcs()
{
    std::vector<std::array<int, 3>> out;
    std::vector<int> m;
    for (const MidiOutByte& b : TakeMidiOut())
    {
        if (b.usb || b.byte >= 0xF8)
            continue;
        if (b.byte & 0x80)
            m.clear();
        m.push_back(b.byte);
        if (m.size() == 3 && (m[0] & 0xF0) == 0xB0)
        {
            out.push_back({m[0], m[1], m[2]});
            m.clear();
        }
    }
    return out;
}
/** The same over USB, its SysEx answers and real-time bytes left out */
static std::vector<std::array<int, 3>> UsbCcs()
{
    std::vector<std::array<int, 3>> out;
    std::vector<int> m;
    bool sysex = false;
    for (unsigned char c : TakeUsbOut())
    {
        if (c == 0xF0)
            sysex = true;
        if (sysex || c >= 0xF8)
        {
            sysex = sysex && c != 0xF7;
            continue;
        }
        if (c & 0x80)
            m.clear();
        m.push_back(c);
        if (m.size() == 3 && (m[0] & 0xF0) == 0xB0)
        {
            out.push_back({m[0], m[1], m[2]});
            m.clear();
        }
    }
    return out;
}
static bool HasCc(const std::vector<std::array<int, 3>>& v, int status, int cc, int value)
{
    for (const std::array<int, 3>& m : v)
        if (m[0] == status && m[1] == cc && m[2] == value)
            return true;
    return false;
}
/** The last value a CC was sent with, -1 if it wasn't */
static int LastCc(const std::vector<std::array<int, 3>>& v, int status, int cc)
{
    int last = -1;
    for (const std::array<int, 3>& m : v)
        if (m[0] == status && m[1] == cc)
            last = m[2];
    return last;
}
/** A knob's 0-1 as MIDI feedback's 7 bits (MidiFeedback.h's KnobTo7) */
static int Knob7(float k) { return k <= .5f ? static_cast<int>(k * 128.f + .5f) : static_cast<int>(64.f + (k - .5f) * 126.f + .5f); }

int main()
{
    // power-on to playable in about a second (#195): on the twin main() reads the keys from
    // ~0.4 s to ~0.9 s, the ripple runs meanwhile, then the play page and the sound
    cases.push_back({"boot", [] {
        const float booting = RunMs(850);
        Check(booting == 0.f, "boot: the outputs stay muted while the boot ripple runs");
        RunMs(150);
        Check(RunMs(200) > .05f, "boot: then the input reaches the master out, after 1 s");
        Tap("ENC_6_SW");
        RunMs(100);
        Check(Ask({0x20}).size() > 12 && Ask({0x20})[12] == 1,
              "boot: and the play page takes keys: VOLUME's press steps to the input gain's page");
        RunMs(kReadyMs);
        bool all_dim = true;
        for (int led : {24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 13, 12})
            all_dim &= Max(SmtLedFull(led)) > 0 && Max(SmtLedFull(led)) < 80;
        Check(all_dim, "boot: every FX key glows dimly in its colour");
    }});

    // the boot ripple (#195): light blue, from SHIFT outwards by where the LEDs sit, gone
    // when the play page opens. On the twin the audio starts at ~0.4 s, the ripple with it
    cases.push_back({"boot-ripple", [] {
        auto light_blue = [](const Rgb& c) { return c.b > 100 && c.g > c.r && c.b >= c.g; };
        RunMs(450);
        // the CHOMPI key's LED and the dark key beside SHIFT, ~24 mm from it; VOLUME's 259 mm
        const bool near = light_blue(PthLedFull(0)) && light_blue(SmtLedFull(1));
        Check(near && Max(PthLedFull(kVolumeLed)) == 0,
              "boot-ripple: first the LEDs next to SHIFT light up light blue, the far ones dark");
        RunMs(300);
        Check(light_blue(PthLedFull(8)) && Max(PthLedFull(0)) == 0 && Max(SmtLedFull(1)) == 0,
              "boot-ripple: then the ring has reached the far side, the middle dark again");
    }});

    // VOLUME pressed during the boot ripple and let go on the play page: no page change, as
    // the boot page had the press
    cases.push_back({"boot-volume", [] {
        RunMs(600);
        Press("ENC_6_SW", true);
        RunMs(kReadyMs);
        Press("ENC_6_SW", false);
        RunMs(300);
        const std::string st = Ask({0x20});
        Check(st.size() > 12 && st[12] == 0,
              "boot-volume: VOLUME held through the boot, let go: still the output gain's page");
        Tap("ENC_6_SW");
        RunMs(300);
        Check(Ask({0x20}).size() > 12 && Ask({0x20})[12] == 1,
              "boot-volume: then a press steps to the input gain's page");
    }});

    // keys held from power-on, through main()'s read for shipping mode and the ripple, let go
    // on the play page: neither the press nor the release does anything there
    cases.push_back({"boot-held", [] {
        Press("KEY_28", true);
        Press("KEY_1", true);
        RunMs(1500);
        const bool dark = Max(PthLedFull(kLoopLed)) == 0;
        Press("KEY_28", false);
        Press("KEY_1", false);
        RunMs(300);
        Check(dark && Max(PthLedFull(kLoopLed)) == 0, "boot-held: LOOP held from power-on records nothing");
        Check(LatchedFx(Ask({0x20})) == 0, "boot-held: an FX key held from power-on latches nothing");
        Tap("KEY_28");
        RunMs(100);
        const Rgb rec = PthLedFull(kLoopLed);
        Check(rec.r > 100 && rec.g == 0 && rec.b == 0, "boot-held: the next LOOP records");
    }});

    cases.push_back({"hold", [] {
        RunMs(kReadyMs);
        const float dry = RunMs(300);
        const int off = Max(SmtLedFull(kFilterKeyLed));
        Press("KEY_5", true);
        RunMs(100);
        Check(Max(SmtLedFull(kFilterKeyLed)) > 2 * off, "hold: the filter's key lights up while held");
        Turn(kKnob1Encoder, -40);
        RunMs(500);
        Check(RunMs(300) < dry * .5f, "hold: knob 1 closes the filter while its key is held");
        Press("KEY_5", false);
        RunMs(200);
        Check(fabsf(RunMs(300) - dry) < dry * .05f, "hold: released, the filter is off again");
        Check(Max(SmtLedFull(kFilterKeyLed)) == off, "hold: and its key back to dim");
    }});

    cases.push_back({"latch", [] {
        RunMs(kReadyMs);
        const float dry = RunMs(300);
        Press("KEY_5", true);
        RunMs(100);
        Turn(kKnob1Encoder, -40);
        RunMs(500);
        Tap("KEY_26"); // SHIFT while the key is held: latch
        Press("KEY_5", false);
        RunMs(300);
        Check(RunMs(300) < dry * .5f, "latch: hold the filter, then SHIFT: it stays on");
        Tap("KEY_5");
        RunMs(300);
        Check(fabsf(RunMs(300) - dry) < dry * .05f, "latch: a tap turns it off");
    }});

    cases.push_back({"select", [] {
        RunMs(kReadyMs);
        const float dry = RunMs(300);
        Tap("KEY_5"); // the filter on the knobs
        RunMs(200);
        Rgb filter_knobs[4];
        for (int k = 0; k < 4; k++)
            filter_knobs[k] = PthLedFull(kKnob1Led + k);
        Press("KEY_26", true);
        RunMs(100);
        Tap("KEY_2"); // SHIFT, then the shifter: picked without hearing it
        Press("KEY_26", false);
        RunMs(200);
        bool changed = false;
        for (int k = 0; k < 4; k++)
            changed |= !Same(PthLedFull(kKnob1Led + k), filter_knobs[k]);
        Check(changed, "select: SHIFT + a key puts its effect on the knobs");
        Check(fabsf(RunMs(300) - dry) < dry * .05f, "select: without switching it on");
        Check(Max(SmtLedFull(kShifterKeyLed)) < 80, "select: its key stays dim");
    }});

    // SHIFT and an FX key nearly together (#115): within kShiftGraceMs (15 ms) of the key's
    // press, SHIFT going down or let go counts as SHIFT first, a select, and a key's release
    // bouncing back is taken back; the latch gesture, key then SHIFT, has more time between.
    // Each from the filter latched (or not), the shifter on the knobs; the debounce sees a
    // press 8 ms after contact, a release 7 ms after
    cases.push_back({"select-grace", [] {
        RunMs(kReadyMs);
        const unsigned filter = 1u << 4;
        struct Seq
        {
            const char* what;
            bool latched_before, latched_after;
            std::vector<const char*> keys;
            // the bounce's press 15 ms after the release, kShiftGraceMs itself: taken back in
            // half the ms phases (#202)
            bool at_limit = false;
        };
        const std::vector<Seq> seqs = {
            {"SHIFT let go 1 ms after the key's contact", true, true,
             {"+KEY_26", "50", "+KEY_5", "1", "-KEY_26", "150", "-KEY_5"}},
            {"SHIFT let go in the key's ms", true, true,
             {"+KEY_26", "50", "+KEY_5", "-KEY_26", "150", "-KEY_5"}},
            {"SHIFT let go 40 ms before the key: a plain press", true, false,
             {"+KEY_26", "50", "-KEY_26", "40", "+KEY_5", "150", "-KEY_5"}},
            {"key and SHIFT in the same ms", true, true,
             {"+KEY_5", "+KEY_26", "100", "-KEY_5", "100", "-KEY_26"}},
            {"key, SHIFT 1 ms later", true, true,
             {"+KEY_5", "1", "+KEY_26", "100", "-KEY_5", "100", "-KEY_26"}},
            {"key, SHIFT 5 ms later", true, true,
             {"+KEY_5", "5", "+KEY_26", "100", "-KEY_5", "100", "-KEY_26"}},
            {"key, SHIFT 12 ms later", true, true,
             {"+KEY_5", "12", "+KEY_26", "100", "-KEY_5", "100", "-KEY_26"}},
            {"key, SHIFT 5 ms later, unlatched", false, false,
             {"+KEY_5", "5", "+KEY_26", "100", "-KEY_5", "100", "-KEY_26"}},
            {"key, SHIFT 25 ms later: the latch gesture unlatches", true, false,
             {"+KEY_5", "25", "+KEY_26", "100", "-KEY_5", "100", "-KEY_26"}},
            {"key, SHIFT 25 ms later: the latch gesture latches", false, true,
             {"+KEY_5", "25", "+KEY_26", "100", "-KEY_5", "100", "-KEY_26"}},
            {"SHIFT bouncing 4 ms, the key at its 2nd", true, true,
             {"+KEY_26", "1", "-KEY_26", "1", "+KEY_26", "+KEY_5", "1", "-KEY_26", "1", "+KEY_26",
              "100", "-KEY_26", "100", "-KEY_5"}},
            {"the key bouncing 4 ms, SHIFT let go at its 2nd", true, true,
             {"+KEY_26", "50", "+KEY_5", "1", "-KEY_5", "1", "-KEY_26", "+KEY_5", "1", "-KEY_5",
              "1", "+KEY_5", "150", "-KEY_5"}},
            {"select, SHIFT up, the key's release touching again 7 ms on", true, true,
             {"+KEY_26", "50", "+KEY_5", "100", "-KEY_26", "100", "-KEY_5", "7", "+KEY_5", "1",
              "-KEY_5"}, true},
            {"select, SHIFT up, the key's release touching again 6 ms on", true, true,
             {"+KEY_26", "50", "+KEY_5", "100", "-KEY_26", "100", "-KEY_5", "6", "+KEY_5", "1",
              "-KEY_5"}},
            {"select, the key's release touching again 7 ms on, SHIFT held", true, true,
             {"+KEY_26", "50", "+KEY_5", "100", "-KEY_5", "7", "+KEY_5", "1", "-KEY_5", "100",
              "-KEY_26"}},
            {"the latch gesture, its release touching again 7 ms on", false, true,
             {"+KEY_5", "100", "+KEY_26", "100", "-KEY_26", "100", "-KEY_5", "7", "+KEY_5", "1",
              "-KEY_5"}, true},
            {"the latch gesture, its release touching again 6 ms on", false, true,
             {"+KEY_5", "100", "+KEY_26", "100", "-KEY_26", "100", "-KEY_5", "6", "+KEY_5", "1",
              "-KEY_5"}},
            {"a plain tap", true, false, {"+KEY_5", "60", "-KEY_5"}},
            {"a plain tap, its release touching again 7 ms on", true, false,
             {"+KEY_5", "60", "-KEY_5", "7", "+KEY_5", "1", "-KEY_5"}},
            // the compressor's key, then SHIFT: SHIFT + its key, a select, so the filter
            // held meanwhile doesn't latch
            {"the filter held, the compressor's key, SHIFT 3 ms later", false, false,
             {"+KEY_5", "100", "+KEY_15", "3", "+KEY_26", "100", "-KEY_15", "-KEY_26", "100",
              "-KEY_5"}},
        };
        for (const Seq& s : seqs)
        {
            const bool comp = strstr(s.what, "compressor") != nullptr;
            // one at the limit twice, a ms apart: once in each phase of MainLoop's 2 ms tick
            bool ok = true;
            for (int pad = 0; pad < (s.at_limit ? 2 : 1); pad++)
            {
                if (((LatchedFx(Ask({0x20})) & filter) != 0) != s.latched_before)
                    Latch("KEY_5");
                Tap("KEY_2"); // the shifter on the knobs
                RunMs(300 + pad);
                const unsigned before = LatchedFx(Ask({0x20}));
                Keys(s.keys);
                RunMs(400);
                const std::string st = Ask({0x20});
                ok = ok && ((before & filter) != 0) == s.latched_before
                     && ((LatchedFx(st) & filter) != 0) == s.latched_after && st.size() > 4
                     && st[4] == (comp ? 14 : 4);
            }
            (s.at_limit ? Known : Check)(
                ok, (std::string("select-grace: ") + s.what + (s.latched_after ? ": latched" : ": unlatched")
                     + (comp ? ", the compressor selected" : ", the filter selected")
                     + (s.at_limit ? " (#202: in either ms phase)" : ""))
                        .c_str());
        }
    }});

    // a key pressed sounds at once, the grace window (#115) or not: the filter, turned down,
    // is heard within 10 ms of the key's contact (its debounce takes 8)
    cases.push_back({"select-grace-at-once", [] {
        RunMs(kReadyMs);
        const float dry = RunMs(300);
        Tap("KEY_5");
        RunMs(100);
        Turn(kKnob1Encoder, -40);
        RunMs(500);
        Press("KEY_5", true);
        RunMs(10);
        Check(RunMs(10) < dry * .5f, "select-grace-at-once: a key held sounds 10 ms after its contact");
        Press("KEY_5", false);
        RunMs(300);
        // the compressor's key, then SHIFT in the window, with a save waiting for CHOMPI: SHIFT +
        // its key, so CHOMPI let go doesn't confirm
        const std::string scenes = Card("/FRIZZ/frizz_scenes.txt");
        Tap(kSaveKey);
        RunMs(100);
        Tap(kSceneKeys[1]);
        RunMs(300);
        Keys({"+KEY_15", "3", "+KEY_26", "100", "-KEY_15", "100", "-KEY_26", "500"});
        Check(Card("/FRIZZ/frizz_scenes.txt") == scenes,
              "select-grace-at-once: the compressor's key, SHIFT 3 ms later: a select, no save confirmed");
        // and a select flashes the key, also one SHIFT made late
        Keys({"+KEY_2", "3", "+KEY_26"});
        const std::vector<int> seen = Watch(false, kShifterKeyLed, 120);
        RunMs(200);
        const int held = Max(SmtLedFull(kShifterKeyLed));
        Keys({"-KEY_2", "-KEY_26", "300"});
        Check(*std::max_element(seen.begin(), seen.end()) > 240 && held < 80,
              "select-grace-at-once: the shifter's key, SHIFT 3 ms later: it flashes white, then stays dim, off");
        // a MIDI note retriggered at once on a latched effect: plain presses, as ever
        Latch("KEY_5");
        Trs({kNoteOn, kFilterNote, 100});
        RunMs(100);
        Trs({kNoteOff, kFilterNote, 0, kNoteOn, kFilterNote, 100});
        RunMs(100);
        Trs({kNoteOff, kFilterNote, 0});
        RunMs(300);
        Check((LatchedFx(Ask({0x20})) >> 4 & 1) == 0,
              "select-grace-at-once: a MIDI note retriggered at once on a latched filter: unlatched on its last note off");
    }});

    // page 2's shared knobs (FxOutput.h, #40, #38): nothing sets a level by itself any more.
    // The folder driven is far louder than its input, its Level turns it down; Mix at 0 is the
    // dry signal; the compressor's makeup is a knob, also by NRPN in bank 1
    cases.push_back({"fx-level", [] {
        amp = .03f; // -30dBFS: quiet, so the folder's drive shows
        RunMs(kReadyMs);
        const float dry = RunMs(300);
        Latch("KEY_3"); // the folder
        Turn(kKnob1Encoder, 100); // drive full
        RunMs(800);
        const float driven = RunMs(300);
        printf("      the folder at -30dBFS: dry %.4f, driven %.4f\n", dry, driven);
        Check(driven > dry * 5.f, "fx-level: the folder driven is far louder than its input, "
                                  "nothing matches it");
        Tap("ENC_4_SW"); // page 2
        RunMs(100);
        Turn(3, -25); // knob 4, Level: 0dB to -12dB
        RunMs(800);
        const float down = RunMs(300);
        printf("      Level at -12dB: %.4f (%.1fdB)\n", down, 20.f * log10f(down / driven));
        Check(fabsf(20.f * log10f(down / driven) + 12.f) < 1.f, "fx-level: page 2's knob 4 is its Level, -12dB");
        Turn(kKnob1Encoder, -100); // knob 1, Mix: dry
        RunMs(800);
        const float mixed = RunMs(300);
        Check(fabsf(mixed / dry - 1.f) < .02f, "fx-level: page 2's knob 1 is its Mix, at 0 the dry signal");
        Turn(kKnob1Encoder, 100);
        Turn(2, -50); // knob 3, Band: the lows only, at its far left
        RunMs(800);
        Check(RunMs(300) < down * .9f, "fx-level: page 2's knob 3 moves the folder off a 220Hz sine's band");

        // the compressor: threshold off, makeup +12dB by CC 31: only that gain
        Latch("KEY_3"); // the folder off again
        RunMs(300);
        const float flat = RunMs(300);
        Trs({kCC, 31, 64});
        RunMs(300);
        const std::string comp = Ask({0x21, 14});
        Check(comp.size() == 17 && comp[15] == 64 && comp[16] == 0,
              "fx-level: CC 31 is the compressor's page-2 knob 4, its makeup");
        const float made_up = RunMs(300);
        printf("      makeup .5: %.1fdB\n", 20.f * log10f(made_up / flat));
        Check(fabsf(20.f * log10f(made_up / flat) - 12.f) < .5f,
              "fx-level: the compressor's makeup at .5 is +12dB, and nothing else changes");
        Trs({kCC, 24, 40, kCC, 31, 0}); // a threshold, no makeup: quieter, nothing gives it back
        RunMs(1500);
        Check(RunMs(300) <= flat * 1.01f, "fx-level: compressing without makeup is never louder");
        amp = .3f;
    }});

    // the safety limiter, the only thing that sets the level by itself, shows on the
    // compressor's key: red while it limits, held a moment, back to white after
    cases.push_back({"limiter-led", [] {
        amp = .05f;
        RunMs(kReadyMs);
        RunMs(500);
        const Rgb quiet = SmtLedFull(kCompKeyLed);
        Check(quiet.r == quiet.g && quiet.g == quiet.b, "limiter-led: a quiet signal: the key white (dim)");
        amp = 1.f;
        Turn(kVolumeEncoder, 100); // VOLUME up: the line outs into the limiter
        RunMs(600);
        const Rgb loud = SmtLedFull(kCompKeyLed);
        printf("      limiting: %02x%02x%02x\n", loud.r, loud.g, loud.b);
        Check(loud.r > 200 && loud.g < 60 && loud.b < 60, "limiter-led: limiting: the key red");
        amp = .02f;
        Turn(kVolumeEncoder, -100);
        RunMs(100);
        Check(SmtLedFull(kCompKeyLed).r > 200, "limiter-led: held a moment after it stops");
        RunMs(800);
        const Rgb after = SmtLedFull(kCompKeyLed);
        Check(after.r == after.g && after.g == after.b, "limiter-led: then white again");
        amp = .3f;
    }});

    // the knob LEDs: white only at a neutral point. A knob with a centre (the shifter's shift)
    // white there, blue below, orange above; page 2's Band the same around its centre; others
    // no white
    cases.push_back({"knob-colors", [] {
        RunMs(kReadyMs);
        Tap("KEY_2"); // the shifter
        RunMs(200);
        auto white = [](const Rgb& c) { return c.r > 200 && c.g > 200 && c.b > 200; };
        auto blue = [](const Rgb& c) { return c.b > 200 && c.r < 60 && c.g < 60; };
        auto orange = [](const Rgb& c) { return c.r > 200 && c.b < 100 && c.g > 80; };
        Check(white(PthLedFull(kKnob1Led)), "knob-colors: the shift at its centre, white");
        Turn(kKnob1Encoder, -40); // 12 semitones down
        RunMs(500);
        const Rgb down = PthLedFull(kKnob1Led);
        Turn(kKnob1Encoder, 80);
        RunMs(800);
        const Rgb up = PthLedFull(kKnob1Led);
        printf("      shift down %02x%02x%02x, up %02x%02x%02x\n", down.r, down.g, down.b, up.r, up.g, up.b);
        Check(blue(down) && orange(up), "knob-colors: turned down blue, up orange");
        Check(!white(PthLedFull(kKnob1Led + 1)), "knob-colors: the feedback (no centre) never white");
        Tap("ENC_4_SW"); // page 2: Band at its centre, white, once its number has blinked
        RunMs(500);
        Check(white(PthLedFull(kKnob1Led + 2)), "knob-colors: page 2's Band at its centre, white");
    }});

    // FX page 2 (#35): a plain knob press turns all four knobs over, and back. Another FX or
    // the compressor goes back to page 1. The LEDs there: knob-page-blink, fx-vu
    cases.push_back({"fx-page2", [] {
        RunMs(kReadyMs);
        // how far knob 1's LED swings over 700 ms: steady once the page's number has blinked
        auto swing = [] {
            const std::vector<int> seen = Watch(true, kKnob1Led, 700);
            return *std::max_element(seen.begin(), seen.end())
                   - *std::min_element(seen.begin(), seen.end());
        };
        auto mix = [] { // the shifter's page-2 knob 1, parameter 4
            const std::string p = Ask({0x21, 1});
            return p.size() == 17 ? KnobOf(p[9], p[10]) : -1.f;
        };
        auto page = [] {
            const std::string state = Ask({0x20});
            return state.size() > 24 ? state[24] : -1; // the FX knobs' page
        };
        Tap("KEY_2"); // the shifter on the knobs
        RunMs(200);
        Check(swing() < 10 && page() == 0, "fx-page2: page 1 at first, its LEDs steady");
        Check(Max(PthLedFull(kKnob1Led + 1)) > 40, "fx-page2: page 1's knob 2 lit");
        Tap("ENC_4_SW"); // knob 1 pressed
        RunMs(100);
        Check(page() == 1, "fx-page2: a plain knob press turns to page 2");
        RunMs(400);
        Check(Max(PthLedFull(kKnob1Led + 1)) > 0 && Max(PthLedFull(kKnob1Led + 2)) > 0
                  && Max(PthLedFull(kKnob1Led + 3)) > 0,
              "fx-page2: the shifter's page 2: all four knobs lit");
        Check(mix() == 1.f, "fx-page2: the mix starts fully shifted");
        Turn(kKnob1Encoder, -30); // 8 ms a detent
        RunMs(400);
        Check(fabsf(mix() - .7f) < 1e-3f, "fx-page2: knob 1 there turns the mix, 1% a detent");
        const std::string p1 = Ask({0x21, 1});
        Check(p1.size() == 17 && KnobOf(p1[1], p1[2]) == .5f, "fx-page2: page 1's shift untouched");
        Press("KEY_26", true);
        RunMs(50);
        Tap("ENC_4_SW");
        Press("KEY_26", false);
        RunMs(100);
        Check(mix() == 1.f && page() == 1, "fx-page2: SHIFT + press resets the mix, on page 2");
        Turn(kKnob1Encoder, -50);
        RunMs(600);
        Tap("ENC_1_SW"); // knob 2: any of the four turns the page
        RunMs(300);
        Check(page() == 0 && swing() < 10, "fx-page2: a press on another knob turns back to page 1");
        Tap("ENC_4_SW");
        RunMs(100);
        Tap("KEY_2"); // the same FX again: still page 2
        RunMs(100);
        Check(page() == 1, "fx-page2: pressing the same FX keeps page 2");
        Tap("KEY_5"); // another FX: page 1
        RunMs(100);
        Check(page() == 0, "fx-page2: another FX goes back to page 1");
        Tap("KEY_11"); // the tape stop: a page 2 too, now every effect has one
        RunMs(100);
        Tap("ENC_4_SW");
        RunMs(100);
        Check(page() == 1, "fx-page2: the tape stop has a page 2 too");
        Tap("KEY_2");
        RunMs(100);
        Check(page() == 0, "fx-page2: and another FX (the shifter) brings page 1 back");
        Tap("ENC_4_SW");
        RunMs(100);
        Tap("KEY_15"); // the compressor: page 1, and a page 2 of its own
        RunMs(100);
        Check(page() == 0, "fx-page2: the compressor's key goes back to page 1");
        Tap("ENC_4_SW");
        RunMs(100);
        Check(page() == 1, "fx-page2: the compressor has a page 2 too");
        // a page-2 value is part of a scene: saved with it, the blank scene puts it back
        Tap("KEY_2");
        RunMs(100);
        Save(1);
        RunMs(2500);
        const std::string file = Card("/FRIZZ/frizz_scenes.txt");
        Check(file.find("layout 5\n") != std::string::npos
                  && file.find("shifter 0 500000 0 0 500000 500000 0 500000 750000") != std::string::npos,
              "fx-page2: a scene keeps page 2 on the card, after page 1's four");
        Tap("ENC_4_SW");
        RunMs(100);
        const std::string state = Ask({0x20});
        Check(page() == 1 && state.size() > 6 && !(state[6] & 1),
              "fx-page2: turning the page leaves the scene unedited");
        Tap(kSceneKeys[0]); // the blank scene
        RunMs(300);
        Check(mix() == 1.f, "fx-page2: the blank scene puts the mix back on its default");
        Tap(kSceneKeys[1]);
        RunMs(300);
        Check(fabsf(mix() - .5f) < 1e-3f, "fx-page2: recalling the scene brings it back");
        Check(page() == 1, "fx-page2: a recall keeps the page");
        // MIDI: the knob press's note turns the page; page 2 by CC 18-21 on the effect's channel
        Trs({kNoteOn, 36, 100});
        RunMs(50);
        Trs({kNoteOff, 36, 0});
        RunMs(50);
        Check(page() == 0, "fx-page2: note 36, knob 1's press, turns the page too");
        Trs({0xB1, 18, 0}); // the shifter's channel, 2
        RunMs(50);
        Check(mix() == 0.f, "fx-page2: CC 18 on channel 2 is the shifter's page-2 knob 1");
        Trs({kFilterCC, 23, 127, kFilterCC, 102, 127}); // the global channel's, not an effect's
        RunMs(50);
        Check(Max(SmtLedFull(kFilterKeyLed)) < 80, "fx-page2: the global CCs do nothing on an effect's channel");
    }});

    // a scene file from v0.11, four values a line: page 2 loads on its defaults, so a latched
    // shifter keeps its full mix and sounds as it did
    cases.push_back({"fx-page2-v011", [] {
        CardFiles()["/FRIZZ/frizz_scenes.txt"]
            = "FRIZZ scenes 1\nscene 1\nshifter 1 791667 0 0 0\nfilter 0 300000 500000 0 666700\n";
        RunMs(kReadyMs);
        Tap(kSceneKeys[1]); // scene 1
        RunMs(300);
        const std::string p = Ask({0x21, 1});
        Check(p.size() == 17 && fabsf(KnobOf(p[1], p[2]) - .791667f) < 1e-3f
                  && KnobOf(p[9], p[10]) == 1.f,
              "fx-page2-v011: a v0.11 scene recalls page 1 as saved, the shifter's mix on its default");
        const std::string state = Ask({0x20});
        Check(state.size() > 9 && (state[9] & 2), "fx-page2-v011: and the shifter latched"); // latches: 8-9
    }});

    cases.push_back({"looper", [] {
        RunMs(kReadyMs);
        Tap("KEY_28");
        RunMs(100);
        const Rgb rec = PthLedFull(kLoopLed);
        Check(rec.r > 100 && rec.g == 0 && rec.b == 0, "looper: LOOP records, lit red");
        RunMs(2000);
        Tap("KEY_28");
        sine = false; // the loop alone
        RunMs(300);
        Check(RunMs(1000) > .05f, "looper: LOOP again plays the recording back");
        Check(Max(PthLedFull(kPlayLed)) + Max(PthLedFull(kLoopLed)) > 0, "looper: PLAY and LOOP show where it is");
        Tap("KEY_27");
        RunMs(300);
        Check(RunMs(300) < .001f, "looper: PLAY pauses it");
        Tap("KEY_28");
        RunMs(300);
        Check(Max(PthLedFull(kLoopLed)) == 0 && Max(PthLedFull(kPlayLed)) <= kBeatPulseMax,
              "looper: LOOP erases it: LOOP dark, PLAY at most the beat's dim pulse");
    }});

    cases.push_back({"quantize", [] {
        RunMs(kReadyMs);
        Press("KEY_27", true);
        RunMs(100);
        Tap("KEY_28"); // PLAY + LOOP without a clock: refused, LOOP blinks red
        Press("KEY_27", false);
        int red = 0;
        for (int i = 0; i < 40; i++)
        {
            RunMs(25);
            red += PthLedFull(kLoopLed).r > 100;
        }
        RunMs(1000);
        Check(red > 0 && Max(PthLedFull(kLoopLed)) == 0, "quantize: without MIDI clock, PLAY + LOOP blinks red and doesn't record");
        // 120 BPM: 24 ticks a beat, one every 20.8 ms
        for (int i = 0; i < 200; i++)
        {
            Midi(0xF8);
            RunMs(i % 6 == 5 ? 20 : 21);
        }
        Press("KEY_27", true);
        RunMs(100);
        Tap("KEY_28");
        Press("KEY_27", false);
        RunMs(100);
        Check(PthLedFull(kLoopLed).r > 100, "quantize: with MIDI clock, PLAY + LOOP records");
    }});

    cases.push_back({"volume", [] {
        RunMs(kReadyMs);
        const float before = RunMs(300);
        Turn(kVolumeEncoder, -30);
        RunMs(500);
        Check(RunMs(300) < before * .7f, "volume: turning VOLUME down lowers the master out");
    }});

    cases.push_back({"shipping", [] {
        Press("KEY_26", true);
        Press("KEY_27", true);
        Press("KEY_28", true);
        RunMs(3000);
        Check(!Powered(), "shipping: CHOMPI, PLAY and LOOP held at power-on switch it off");
    }});

    cases.push_back({"battery", [] {
        RunMs(kReadyMs);
        SetBattery(2.9f, false);
        RunMs(1000);
        // over a few flashes: the others lit yellow, VOLUME's red (#43), all dark in between
        bool right = true, yellow = false, red = false;
        for (int t = 0; t < 1000; t++)
        {
            RunMs(1);
            for (int i = 0; i < kNumPthLeds; i++)
            {
                const Rgb c = PthLedFull(i);
                if (Max(c) == 0)
                    continue;
                if (i == kVolumeLed)
                {
                    right &= c.r > 100 && c.g < 50 && c.b < 50;
                    red = true;
                }
                else
                {
                    right &= c.r > 100 && c.g > 100 && c.b < 50;
                    yellow = true;
                }
            }
        }
        Check(Powered() && right && yellow && red,
              "battery: below 3V, the panel LEDs flash yellow, VOLUME's red");
        RunMs(16000);
        Check(!Powered(), "battery: 15 s later it switches itself off");
    }});

    // the countdown dims with the LEDs: at 50 %, VOLUME's red at half, still red
    cases.push_back({"battery-dim", [] {
        auto top = [] {
            Rgb t{0, 0, 0};
            for (int ms = 0; ms < 1000; ms++)
            {
                RunMs(1);
                const Rgb c = PthLedFull(kVolumeLed);
                if (c.r > t.r)
                    t = c;
            }
            return t;
        };
        RunMs(kReadyMs);
        SetToggle(true);
        RunMs(200);
        TurnBrightness(-10); // 50 %
        SetToggle(false);
        RunMs(300);
        SetBattery(2.9f, false);
        RunMs(500);
        const Rgb half = top();
        printf("      VOLUME's red at 50 %%: %d %d %d\n", half.r, half.g, half.b);
        Check(half.r > 100 && half.r < 160 && half.g == 0 && half.b == 0,
              "battery-dim: the countdown's red dims with the LEDs' brightness");
    }});

    cases.push_back({"charging", [] {
        RunMs(kReadyMs);
        SetBattery(2.9f, true);
        RunMs(20000);
        Check(Powered() && RunMs(300) > .05f, "charging: below 3V on the charger, it keeps playing");
    }});

    // the charger plugged in during the 15 s countdown: it stops and FRIZZ plays on. Looked at
    // while the countdown would still run, 5.5-8 s into it; unplugged, the same window flashes
    auto countdown = [](bool plug) {
        auto amber = [] {
            const Rgb c = PthLedFull(kTransportRevLed);
            return c.r > 100 && c.g > 100 && c.b < 50;
        };
        RunMs(kReadyMs);
        SetBattery(2.9f, false);
        uint32_t waited = 0;
        while (!amber() && waited++ < 10000)
            RunMs(1);
        RunMs(5000);
        if (plug)
            SetBattery(2.9f, true);
        RunMs(500);
        int lit = 0;
        for (int i = 0; i < 2500; i++)
        {
            RunMs(1);
            lit += amber();
        }
        return std::make_pair(waited < 10000, lit);
    };
    cases.push_back({"charger-countdown", [countdown] {
        const auto seen = countdown(true);
        Check(seen.first, "charger-countdown: unplugged below 3V, the panel flashes yellow");
        Check(seen.second == 0 && Powered() && RunMs(300) > .05f,
              "charger-countdown: the charger plugged in 5 s into it, the flashing stops and FRIZZ plays on");
    }});
    cases.push_back({"charger-countdown-unplugged", [countdown] {
        const auto seen = countdown(false);
        Check(seen.first && seen.second > 500,
              "charger-countdown: (without the charger, the panel still flashes then)");
    }});

    // VOLUME's LED on the settings page shows the battery (MANUAL.md, Settings page)
    cases.push_back({"battery-level", [] {
        SetBattery(3.8f, true);
        RunMs(kReadyMs);
        SetToggle(true);
        RunMs(300);
        const Rgb plugged = PthLedFull(kVolumeLed);
        Check(plugged.r > 100 && plugged.r == plugged.g && plugged.g == plugged.b,
              "battery-level: white while the charging cable is in");
        SetBattery(3.8f, false);
        RunMs(3000);
        const Rgb high = PthLedFull(kVolumeLed);
        Check(high.g > 100 && high.r < 50 && high.b < 50,
              "battery-level: the cable pulled, green above 3.3V within a few seconds");
        SetBattery(3.1f, false);
        RunMs(5000);
        Check(Same(PthLedFull(kVolumeLed), high), "battery-level: below 3.3V it waits for the 30 s read");
        RunMs(30000);
        const Rgb medium = PthLedFull(kVolumeLed);
        Check(medium.r > 100 && medium.g > 100 && medium.b < 50, "battery-level: then yellow");
        SetBattery(3.1f, true);
        RunMs(1000);
        const Rgb again = PthLedFull(kVolumeLed);
        Check(again.r > 100 && again.r == again.g && again.g == again.b,
              "battery-level: and white again as soon as the cable is back");
        SetBattery(2.9f, false);
        int red = 0;
        for (int i = 0; i < 3000; i++)
        {
            RunMs(1);
            const Rgb c = PthLedFull(kVolumeLed);
            red += c.r > 100 && c.g < 50;
        }
        Check(red > 0, "battery-level: red below 3V, as MANUAL.md says (flashing, in the countdown)");
    }});

    // the compressor's knobs without a card: its key blinks red 3 times, and the save is tried
    // again 3 times, 2 s after each failure
    cases.push_back({"comp-no-card", [] {
        SetCardPresent(false);
        RunMs(kReadyMs);
        Tap("KEY_15"); // the compressor on the knobs
        RunMs(300);
        Turn(4, 20);
        const std::vector<int> seen = Watch(false, kCompKeyLed, 14000);
        // trains of red blinks (lit 100 ms, dark 100 ms), and how many blinks each
        std::vector<int> trains, blinks;
        int last_lit = -1000;
        for (size_t t = 1; t < seen.size(); t++)
        {
            if (seen[t] > 150 && seen[t - 1] <= 150)
            {
                if (static_cast<int>(t) - last_lit > 400)
                {
                    trains.push_back(static_cast<int>(t));
                    blinks.push_back(0);
                }
                blinks.back()++;
                last_lit = static_cast<int>(t);
            }
        }
        bool threes = !blinks.empty(), spaced = trains.size() > 1;
        for (int b : blinks)
            threes &= b == 3;
        for (size_t i = 1; i < trains.size(); i++)
            spaced &= trains[i] - trains[i - 1] >= 2000 && trains[i] - trains[i - 1] < 2300;
        printf("      red blinks at");
        for (size_t i = 0; i < trains.size(); i++)
            printf(" %d ms (%d)", trains[i], blinks[i]);
        printf("\n");
        Check(trains.size() == 4 && threes,
              "comp-no-card: without a card the compressor key blinks red 3 times, and again for each of 3 retries");
        Check(spaced, "comp-no-card: the retries 2 s apart");
        Check(Card("/FRIZZ/frizz_master.txt").empty(), "comp-no-card: and nothing is written");
    }});

    // the scene keys' LEDs: a used slot dim, the active one bright, pulsing once it's edited
    cases.push_back({"scene-leds", [] {
        RunMs(kReadyMs);
        Latch("KEY_5");
        Save(1);
        RunMs(1000);
        Latch("KEY_2");
        Save(2);
        RunMs(1000);
        const int used = Max(SmtLedFull(kSlot1Led)), active = Max(SmtLedFull(kSlot1Led + 1)),
                  empty = Max(SmtLedFull(kSlot1Led + 2));
        printf("      used %d, active %d, empty %d\n", used, active, empty);
        Check(active > 240 && used > 20 && used < 60 && empty == 0,
              "scene-leds: the active scene's key bright, another saved one dim, an empty one dark");
        Turn(kKnob1Encoder, 10); // the shifter's interval: the scene edited
        RunMs(300);
        const std::vector<int> pulse = Watch(false, kSlot1Led + 1, 2000);
        const int lo = *std::min_element(pulse.begin(), pulse.end()),
                  hi = *std::max_element(pulse.begin(), pulse.end());
        // .6 + .4 cos: from .2 up to full, once a second
        const size_t at_lo = std::min_element(pulse.begin(), pulse.end()) - pulse.begin();
        const int a_second_on = pulse[(at_lo + 1000) % pulse.size()];
        printf("      edited: %d to %d\n", lo, hi);
        Check(lo > 40 && lo < 65 && hi > 245 && a_second_on < lo + 5,
              "scene-leds: edited, it pulses from a fifth up to full and back once a second");
        Tap(kSceneKeys[1]);
        RunMs(300);
        const std::vector<int> recalled = Watch(false, kSlot1Led, 1000);
        Check(*std::min_element(recalled.begin(), recalled.end()) > 240
                  && Max(SmtLedFull(kSlot1Led + 1)) < 60,
              "scene-leds: a recall makes its key the bright one, steady, and the other dim");
    }});

    // the scene keys two dark keys to the right (#67): the blank scene on the lower F#, scenes
    // 1-4 to the upper D#; SAVE green, COPY blue, DELETE red on the upper F#, G#, A#; the lower
    // C# and D# free on the play page, dark, doing nothing
    cases.push_back({"scene-keys", [] {
        RunMs(kReadyMs);
        const Rgb dim_white{36, 36, 36};
        Check(Same(SmtLedFull(kSlot1Led - 1), dim_white) && Max(SmtLedFull(kSlot1Led)) == 0
                  && Max(SmtLedFull(kSlot1Led + 3)) == 0 && Max(SmtLedFull(0)) == 0
                  && Max(SmtLedFull(1)) == 0,
              "scene-keys: the blank scene dim on the lower F#, the empty slots dark, the lower C# and D# dark");
        Check(Same(SmtLedFull(kSaveKeyLed), Rgb{0, 36, 0}) && Same(SmtLedFull(kSaveKeyLed + 1), Rgb{0, 0, 36})
                  && Same(SmtLedFull(kSaveKeyLed + 2), Rgb{36, 0, 0}),
              "scene-keys: SAVE, COPY and DELETE dim green, blue and red on the upper F#, G#, A#");
        struct ModeKey
        {
            const char* key;
            int led, mode;
            Rgb lit;
        };
        bool modes = true;
        for (const ModeKey& m : {ModeKey{kSaveKey, kSaveKeyLed, 1, {0, 252, 0}},
                                 ModeKey{kCopyKey, kSaveKeyLed + 1, 2, {0, 0, 252}},
                                 ModeKey{kDeleteKey, kSaveKeyLed + 2, 3, {252, 0, 0}}})
        {
            Tap(m.key);
            RunMs(100);
            const std::string in = Ask({0x20});
            modes &= in.size() > 7 && in[7] == m.mode && Same(SmtLedFull(m.led), m.lit);
            Tap(m.key);
            RunMs(100);
            const std::string out = Ask({0x20});
            modes &= out.size() > 7 && out[7] == 0;
        }
        Check(modes, "scene-keys: each one, tapped, is its mode, its key fully lit; again, the mode ends");
        Latch("KEY_5");
        Save(1);
        RunMs(1000);
        Latch("KEY_2");
        Save(4);
        RunMs(1000);
        Check(Max(SmtLedFull(kSlot1Led + 3)) > 240 && Max(SmtLedFull(kSlot1Led)) > 20
                  && Max(SmtLedFull(kSlot1Led)) < 60,
              "scene-keys: scene 4 saved on the upper D#, bright; scene 1 on the lower G#, dim");
        Tap(kSceneKeys[0]);
        RunMs(300);
        const std::string scenes = Card("/FRIZZ/frizz_scenes.txt");
        Tap("KEY_16");
        Tap("KEY_17");
        RunMs(300);
        const std::string after = Ask({0x20});
        Check(after.size() > 7 && after[5] == 1 && after[7] == 0 && Max(SmtLedFull(0)) == 0
                  && Max(SmtLedFull(1)) == 0,
              "scene-keys: the lower C# and D# recall nothing, pick no mode and stay dark");
        Tap(kSaveKey);
        RunMs(100);
        Tap("KEY_16");
        RunMs(300);
        Tap("KEY_26");
        RunMs(1000);
        Check(Card("/FRIZZ/frizz_scenes.txt") == scenes && Max(SmtLedFull(0)) == 0,
              "scene-keys: in SAVE they pick no slot: CHOMPI saves nothing");
        // a slot picked, CHOMPI held and one of them tapped: a key without a function is no
        // SHIFT combo (NormalPage::OnButton), so CHOMPI let go confirms, as the upper C# and D#
        // did before
        Tap(kSceneKeys[2]);
        RunMs(300);
        Keys({"+KEY_26", "100", "+KEY_16", "60", "-KEY_16", "100", "-KEY_26", "1000"});
        const std::string combo = Ask({0x20});
        Check(Card("/FRIZZ/frizz_scenes.txt") != scenes && combo.size() > 7 && combo[7] == 0,
              "scene-keys: SAVE, a slot picked, CHOMPI + the lower C#: no combo, CHOMPI confirms");
        // the settings page lights them (every channel, the block over the jack): back on the
        // play page they're dark again
        SetToggle(true);
        RunMs(300);
        const bool lit_up = Max(SmtLedFull(1)) > 0;
        SetToggle(false);
        RunMs(300);
        Check(lit_up && Max(SmtLedFull(0)) == 0 && Max(SmtLedFull(1)) == 0,
              "scene-keys: lit on the settings page, the lower C# and D# go dark on the play page");
        // MIDI's notes follow the keys: 56, the lower G#, is scene 1
        Trs({kNoteOn, 56, 100});
        RunMs(60);
        Trs({kNoteOff, 56, 0});
        RunMs(300);
        const std::string by_note = Ask({0x20});
        Check(by_note.size() > 5 && by_note[5] == 2 && Max(SmtLedFull(kSlot1Led)) > 240,
              "scene-keys: note 56, the lower G#, recalls scene 1");
    }});

    // a morph: its key blinks on the beat; SHIFT + an empty slot, or a 9th bar, blinks red
    cases.push_back({"scene-morph", [] {
        RunMs(kReadyMs);
        Latch("KEY_5");
        Save(1);
        RunMs(1000);
        Latch("KEY_2");
        Save(2);
        RunMs(1000);
        Tap(kSceneKeys[1]);
        RunMs(500);
        Press("KEY_26", true); // SHIFT held: the morph waits for it
        RunMs(80);
        Tap(kSceneKeys[2]);
        RunMs(100);
        const std::vector<int> beat = Watch(false, kSlot1Led + 1, 2000);
        const std::vector<int> runs = Runs(beat, 128);
        bool on_beat = runs.size() >= 6;
        for (int r : runs)
            on_beat &= r >= 230 && r <= 270;
        printf("      morph blinks:");
        for (int r : runs)
            printf(" %d", r);
        printf(" ms\n");
        Check(on_beat, "scene-morph: the key it morphs to blinks on the beat, 250 ms at 120 BPM");
        // an empty slot: refused, 3 red blinks on it (another saved one is a build-up, #175)
        Check(RedBlinks(kSceneKeys[3], kSlot1Led + 2) == 3 && Ask({0x20}).size() > 6 && (Ask({0x20})[6] & 2),
              "scene-morph: SHIFT + an empty slot during it blinks that key red 3 times, and it morphs on");
        // the same key again adds bars, 8 in all; a 9th is refused
        int refused = 0;
        for (int bar = 2; bar <= 8; bar++)
            refused += RedBlinks(kSceneKeys[2], kSlot1Led + 1);
        Check(refused == 0 && RedBlinks(kSceneKeys[2], kSlot1Led + 1) == 3,
              "scene-morph: the same key adds bars up to 8 in all; a 9th blinks it red 3 times");
        Press("KEY_26", false);
        RunMs(20000);
        Check(Ask({0x20}).size() > 6 && !(Ask({0x20})[6] & 2) && Ask({0x20})[5] == 3,
              "scene-morph: SHIFT let go, it glides and lands on scene 2");
    }});

    // the chaos key (#65), the 14th white key: the effects latched drop out on random steps
    cases.push_back({"chaos-gates", [] {
        RunMs(kReadyMs);
        const float dry = RunMs(300);
        static const int kChaosKeyLed = 11;
        Latch("KEY_5");
        Turn(kKnob1Encoder, -60); // the filter closed: quiet while it's in
        RunMs(300);
        const float closed = RunMs(300);
        Latch("KEY_14");
        Turn(kKnob1Encoder, 100); // its FX chance at the top
        RunMs(300);
        const Rgb chaos = SmtLedFull(kChaosKeyLed);
        Check(Max(chaos) > 200 && chaos.r > 2 * chaos.g, "chaos-gates: latched, its key lit, rose");
        // 16ths at 120 BPM: 125 ms; the filter out of about half of them
        int out = 0, in = 0, dim = 0, lit = 0;
        for (int w = 0; w < 64; w++)
        {
            const float rms = RunMs(62);
            out += rms > dry * .5f;
            in += rms < closed * 2.f + .001f;
            const int led = Max(SmtLedFull(kFilterKeyLed));
            dim += led < 60;
            lit += led > 200;
        }
        printf("      dry %.3f, closed %.3f; of 64 windows %d open, %d closed; its key %d dim, %d lit\n",
               dry, closed, out, in, dim, lit);
        Check(out > 10 && in > 10, "chaos-gates: the filter drops out on some steps and is back on others");
        Check(dim > 10 && lit > 10, "chaos-gates: the filter's key dims while it's out");

        // held, the filter is the hand's: on all the time
        Press("KEY_5", true);
        RunMs(100);
        int open_held = 0;
        for (int w = 0; w < 32; w++)
            open_held += RunMs(62) > dry * .5f;
        Press("KEY_5", false); // a plain press: it unlatches
        RunMs(100);
        Check(open_held == 0, "chaos-gates: a latched key held stays on, out of chaos's hands");

        // unlatched chaos: the filter stays in
        Latch("KEY_5");
        Latch("KEY_14");
        RunMs(300);
        int open_off = 0;
        for (int w = 0; w < 32; w++)
            open_off += RunMs(62) > dry * .5f;
        Check(open_off == 0 && Max(SmtLedFull(kFilterKeyLed)) > 200,
              "chaos-gates: chaos unlatched, the filter steady");

        // over MIDI: CC 115 latches it, CC 14 on its channel, 14, its FX chance
        const std::string before = Ask({0x21, 13});
        Trs({0xBD, 14, 0});
        Trs({kCC, 115, 127});
        RunMs(300);
        const std::string st = Ask({0x20}), after = Ask({0x21, 13});
        Check(st.size() > 9 && ((static_cast<uint8_t>(st[8]) << 7 | static_cast<uint8_t>(st[9])) >> 13 & 1),
              "chaos-gates: CC 115 latches the chaos key");
        Check(before.size() > 2 && after.size() > 2 && before[1] != 0 && after[1] == 0 && after[2] == 0,
              "chaos-gates: CC 14 on channel 14 sets its FX chance (to 0)");
        int open_midi = 0;
        for (int w = 0; w < 32; w++)
            open_midi += RunMs(62) > dry * .5f;
        Check(open_midi == 0, "chaos-gates: at a chance of 0, nothing drops out");
    }});

    // the chaos key's scramble: on random steps the loop plays from elsewhere, its position
    // running on. A 2 s loop, silent in its first half
    cases.push_back({"chaos-scramble", [] {
        RunMs(kReadyMs);
        amp = 0.f;
        Tap("KEY_28");
        RunMs(1000);
        amp = .3f;
        RunMs(940);
        Tap("KEY_28");
        RunMs(500);
        const float loud = RunMs(1000) * 2.f; // half of it is the tone
        auto elsewhere = [loud](int windows) {
            // windows in the silent half that sound
            int n = 0;
            for (int w = 0; w < windows; w++)
            {
                const float pos = Probe().loop_pos;
                const float rms = RunMs(40);
                const float end = Probe().loop_pos;
                if (pos > .05f && end < .45f && end > pos)
                    n += rms > loud * .3f;
            }
            return n;
        };
        amp = 0.f; // only the loop from here
        const int before = elsewhere(150);
        Latch("KEY_14");
        Turn(1, 100); // knob 2 (SW1), scramble chance at the top
        RunMs(300);
        const int during = elsewhere(150);
        const float pos = Probe().loop_pos;
        RunMs(500);
        const float moved = Probe().loop_pos - pos;
        printf("      loop %.3f s; loud %.3f; sounding in the silent half: %d before, %d scrambled; moved %.3f in 0.5 s\n",
               Probe().loop_length / kSampleRate, loud, before, during, moved);
        Check(before == 0 && during > 5, "chaos-scramble: the loop's silent half plays from elsewhere");
        Check(fabsf((moved < 0.f ? moved + 1.f : moved) - .25f) < .02f,
              "chaos-scramble: the loop's position runs on as ever");
        Latch("KEY_14");
        RunMs(300);
        Check(elsewhere(150) == 0, "chaos-scramble: chaos off, the loop in place again");
    }});

    // the crossfader (#66): SHIFT + a scene key, then the transport knob, no loop needed;
    // scene 1 has the filter latched and closed, quiet, the blank scene opens it again
    cases.push_back({"scene-fader", [] {
        freq = 3000.f; // high, so the filter halfway open still takes some
        RunMs(kReadyMs);
        const float dry = RunMs(300);
        auto state = [] { return Ask({0x20}); };
        Latch("KEY_5");
        Turn(kKnob1Encoder, -60); // the cutoff closed: a lowpass far below the sine
        RunMs(300);
        Save(1);
        RunMs(1000);
        const float closed = RunMs(300);
        // back at the closed filter: about as quiet, with a floor 40dB under the dry, as
        // closed is all but exactly 0 since the key's fade lands on 1 (#45)
        const float quiet = closed * 1.3f + dry * .01f;
        printf("      dry %.3f, filter closed %.3f\n", dry, closed);
        Check(closed < dry * .3f, "scene-fader: scene 1, the filter closed, is quiet");

        Press("KEY_26", true);
        RunMs(80);
        Tap(kSceneKeys[0]); // to the blank scene, held
        RunMs(100);
        Turn(5, 12); // halfway
        RunMs(400);
        const float half = RunMs(300);
        const int a = Max(SmtLedFull(kSlot1Led)), b = Max(SmtLedFull(kSlot1Led - 1));
        const Rgb rev = PthLedFull(kTransportRevLed), fwd = PthLedFull(kTransportFwdLed);
        printf("      halfway %.3f; keys A %d, B %d; transport %d %d\n", half, a, b, Max(rev), Max(fwd));
        Check(half > closed * 1.5f && half < dry * .9f,
              "scene-fader: halfway, the sound halfway between the scenes");
        Check(a > 100 && a < 200 && b > 100 && b < 200,
              "scene-fader: the two scene keys at half brightness each");
        Check(Max(rev) > 100 && Max(rev) < 160 && rev.r == rev.g && rev.g == rev.b
                  && Max(fwd) > 100 && Max(fwd) < 160 && fwd.r == fwd.b,
              "scene-fader: the transport LEDs show where it is, in white, without a loop");
        Turn(5, 12);
        RunMs(400);
        const float at_b = RunMs(300);
        Check(at_b > dry * .85f && Max(SmtLedFull(kSlot1Led - 1)) > 240
                  && Max(SmtLedFull(kSlot1Led)) < 60,
              "scene-fader: all the way right, the blank scene, its key bright");
        Turn(5, -24);
        RunMs(400);
        Check(RunMs(300) < quiet, "scene-fader: and all the way back, scene 1 again");
        RunMs(5000);
        Check(state().size() > 6 && (state()[6] & 2) && RunMs(300) < quiet,
              "scene-fader: held, the bar lines don't land it");

        // let go halfway: it stays there, the blank scene edited
        Turn(5, 12);
        RunMs(300);
        Press("KEY_26", false);
        RunMs(300);
        const float left = RunMs(300);
        RunMs(5000);
        std::string st = state();
        Check(st.size() > 6 && !(st[6] & 2) && (st[6] & 1) && st[5] == 1
                  && fabsf(RunMs(300) - left) < left * .1f && left > closed * 1.5f,
              "scene-fader: let go halfway, it stays there, the blank scene active and edited");

        // let go at the end: landed on the blank scene; at the start: scene 1 as it was
        Tap(kSceneKeys[1]);
        RunMs(500);
        Press("KEY_26", true);
        RunMs(80);
        Tap(kSceneKeys[0]);
        RunMs(100);
        Turn(5, 30);
        RunMs(300);
        Press("KEY_26", false);
        RunMs(500);
        st = state();
        Check(st.size() > 9 && !(st[6] & 3) && st[5] == 1 && st[9] == 0 && RunMs(300) > dry * .85f,
              "scene-fader: let go at the end, it lands on the blank scene, nothing latched");
        Tap(kSceneKeys[1]);
        RunMs(500);
        Press("KEY_26", true);
        RunMs(80);
        Tap(kSceneKeys[0]);
        RunMs(100);
        Turn(5, 6);
        RunMs(300);
        Turn(5, -6);
        RunMs(300);
        Press("KEY_26", false);
        RunMs(500);
        st = state();
        Check(st.size() > 9 && !(st[6] & 3) && st[5] == 2 && (st[9] & (1 << 4))
                  && RunMs(300) < quiet,
              "scene-fader: let go at the start, scene 1 is back as it was, not edited");

        // SHIFT + turn without a held morph: the speed as ever, no fader
        Press("KEY_26", true);
        RunMs(80);
        Turn(5, 6);
        RunMs(300);
        Check(Max(PthLedFull(kTransportRevLed)) == 0 && Max(PthLedFull(kTransportFwdLed)) == 0,
              "scene-fader: SHIFT + turn without a morph shows nothing (no loop)");
        Press("KEY_26", false);
        RunMs(300);
    }});

    // the crossfader over MIDI (CC 80): a morph started by CC 3, taken to where the CC says
    cases.push_back({"midi-fader", [] {
        freq = 3000.f; // high, so the filter halfway open still takes some
        RunMs(kReadyMs);
        const float dry = RunMs(300);
        auto state = [] { return Ask({0x20}); };
        Latch("KEY_5");
        Turn(kKnob1Encoder, -60);
        RunMs(300);
        Save(1);
        RunMs(1000);
        const float closed = RunMs(300);
        // back at the closed filter: about as quiet, with a floor 40dB under the dry, as
        // closed is all but exactly 0 since the key's fade lands on 1 (#45)
        const float quiet = closed * 1.3f + dry * .01f;
        Trs({kCC, 3, 0}); // to the blank scene, gliding over a bar
        RunMs(20);
        Trs({kCC, 80, 0});
        RunMs(3000);
        Check(state().size() > 6 && (state()[6] & 2) && RunMs(300) < quiet,
              "midi fader: CC 80 takes the morph; at 0 it waits at the start, past the bar line");
        Trs({kCC, 80, 64});
        RunMs(400);
        const float half = RunMs(300);
        printf("      dry %.3f, closed %.3f, at 64 %.3f\n", dry, closed, half);
        Check(half > closed * 1.5f && half < dry * .9f, "midi fader: at 64, halfway");
        Trs({kCC, 80, 0});
        RunMs(300);
        std::string st = state();
        Check(st.size() > 9 && !(st[6] & 3) && st[5] == 2 && (st[9] & (1 << 4)),
              "midi fader: back to 0, scene 1 as it was");
        Trs({kCC, 3, 127, kCC, 3, 0}); // 127 is no scene, so 0 is a change again
        RunMs(20);
        Trs({kCC, 80, 127});
        RunMs(300);
        st = state();
        Check(st.size() > 9 && !(st[6] & 3) && st[5] == 1 && st[9] == 0 && RunMs(300) > dry * .85f,
              "midi fader: at 127, landed on the blank scene");
        Trs({kCC, 80, 64});
        RunMs(300);
        Check(state().size() > 6 && !(state()[6] & 2), "midi fader: without a morph, nothing");
    }});

    // a CHOMPI tap while CC 80 holds a morph: the morph stays MIDI's (only the hand's fader
    // ends where SHIFT is let go), so CC 80 still lands it, and at 0 the tap doesn't take it
    // back to where it started
    cases.push_back({"midi-fader-tap", [] {
        freq = 3000.f;
        RunMs(kReadyMs);
        const float dry = RunMs(300);
        auto state = [] { return Ask({0x20}); };
        auto landed = [&] {
            const std::string st = state();
            return st.size() > 9 && !(st[6] & 3) && st[5] == 1 && st[9] == 0
                   && RunMs(300) > dry * .85f;
        };
        Latch("KEY_5");
        Turn(kKnob1Encoder, -60);
        RunMs(300);
        Save(1);
        RunMs(1000);

        Trs({kCC, 3, 0}); // to the blank scene
        RunMs(20);
        Trs({kCC, 80, 64});
        RunMs(400);
        Tap("KEY_26");
        RunMs(300);
        Check(state().size() > 6 && (state()[6] & 2),
              "midi-fader-tap: halfway, a CHOMPI tap leaves CC 80 its morph");
        Trs({kCC, 80, 127});
        RunMs(300);
        Check(landed(), "midi-fader-tap: and CC 80 at 127 then lands it");

        Tap(kSceneKeys[1]); // scene 1 again
        RunMs(300);
        Trs({kCC, 3, 127, kCC, 3, 0}); // 127 is no scene, so 0 is a change again
        RunMs(20);
        Trs({kCC, 80, 0});
        RunMs(400);
        Tap("KEY_26");
        RunMs(300);
        Check(state().size() > 6 && (state()[6] & 2),
              "midi-fader-tap: at 0, the tap doesn't take the morph back");
        Trs({kCC, 80, 127});
        RunMs(300);
        Check(landed(), "midi-fader-tap: and CC 80 at 127 lands it");

        // CC 3 to the scene it runs to: no bar more while the fader has it, nothing else
        Tap(kSceneKeys[1]);
        RunMs(300);
        Trs({kCC, 3, 127, kCC, 3, 0}); // 127 is no scene, so 0 is a change again
        RunMs(20);
        Trs({kCC, 80, 64});
        RunMs(400);
        Trs({kCC, 3, 0});
        RunMs(300);
        Check(state().size() > 6 && (state()[6] & 2),
              "midi-fader-tap: CC 3 sent again meanwhile changes nothing: CC 80 keeps its morph");
        Trs({kCC, 80, 127});
        RunMs(300);
        Check(landed(), "midi-fader-tap: which CC 80 at 127 lands");
    }});

    // a scene sent over SysEx into the active slot: the sound stays, so it's edited (as COPY
    // onto it), and SAVE stores what plays
    cases.push_back({"scene-put-active", [] {
        RunMs(kReadyMs);
        Latch("KEY_5");
        Save(1);
        RunMs(1000);
        std::string st = Ask({0x20});
        Check(st.size() > 6 && st[5] == 2 && !(st[6] & 1), "scene-put-active: scene 1 saved, unedited");
        bool taken = true;
        for (int part = 0; part < 8; part++)
        {
            std::vector<int> put = {0x31, 1, part};
            put.resize(put.size() + 32, 0);
            const std::string r = Ask(put);
            taken &= r.size() > 2 && r[2] == 0;
        }
        const std::string r = Ask({0x31, 1, 8, 1, 0, 0});
        taken &= r.size() > 2 && r[2] == 0;
        RunMs(1000);
        st = Ask({0x20});
        Check(taken && st.size() > 6 && st[5] == 2 && (st[6] & 1),
              "scene-put-active: sent into it over SysEx, the active scene 1 is edited");
    }});

    // the millisecond counter wrapping (after 49.7 days on): the signals started just before it
    // end on time and don't come back, and timing across it holds
    cases.push_back({"clock-wrap", [] {
        const uint32_t wrap = kReadyMs + 3000; // ms after power-on
        SetClockStartMs(static_cast<uint32_t>(0x100000000ull - wrap));
        RunMs(kReadyMs);
        Check(RunMs(300) > .05f, "clock-wrap: it boots on a clock 9 s before its wrap");
        Tap("KEY_15"); // the compressor's amount: saved 2 s after, across the wrap
        RunMs(300);
        Turn(4, 20);
        RunMs(300);
        // a refused quantized record: LOOP's quick red blinks, then a loop recorded across it
        Press("KEY_27", true);
        RunMs(80);
        Tap("KEY_28");
        Press("KEY_27", false);
        RunMs(1000);
        while (NowMs() < wrap - 800)
            RunMs(1);
        const uint32_t rec_at = NowMs();
        Tap("KEY_28");
        while (NowMs() < wrap - 300)
            RunMs(1);
        // a refused scene key (an empty slot), 300 ms before
        const int blinks = RedBlinks(kSceneKeys[3], kSlot1Led + 2); // to 400 ms past the wrap
        Check(blinks == 3, "clock-wrap: a scene key refused 300 ms before it blinks red 3 times across it");
        RunMs(300);
        const std::vector<int> slot = Watch(false, kSlot1Led + 2, 3000);
        Check(*std::max_element(slot.begin(), slot.end()) == 0,
              "clock-wrap: a refused scene key's red blinks just before the wrap end and don't come back");
        const std::vector<int> loop = Watch(true, kLoopLed, 500);
        Check(Probe().loop_state == 1 && *std::min_element(loop.begin(), loop.end()) > 100,
              "clock-wrap: LOOP refused before, records on, lit, without blinking");
        const float recorded = (NowMs() - rec_at) / 1000.f;
        Tap("KEY_28");
        RunMs(300);
        printf("      the loop: %.3f s, recorded for %.3f s\n", Probe().loop_length / kSampleRate, recorded);
        Check(Probe().loop_state == 2 && fabsf(Probe().loop_length / kSampleRate - recorded) < .02f,
              "clock-wrap: the loop recorded across it plays, as long as it was recorded");
        Check(Card("/FRIZZ/frizz_master.txt").find("comp") != std::string::npos,
              "clock-wrap: the compressor's knob turned before it is saved after it");
        // a scene saved after it, and SAVE's pending blink on CHOMPI
        Latch("KEY_5");
        Save(1);
        RunMs(1000);
        Check(SavedLatch(Card("/FRIZZ/frizz_scenes.txt"), 1, "filter") == 1,
              "clock-wrap: a scene saved after it is on the card");
        Tap(kSaveKey);
        RunMs(100);
        Tap(kSceneKeys[2]);
        const std::vector<int> pending = Runs(Watch(true, 0, 1200), 128);
        bool steady = pending.size() >= 3;
        for (int r : pending)
            steady &= r >= 230 && r <= 270;
        Check(steady, "clock-wrap: CHOMPI blinks steadily while a save waits after it");
    }});

    // ---- PR #7's hardware checklist, as far as it isn't about the CPU ----

    cases.push_back({"save-latch", [] {
        RunMs(kReadyMs);
        Tap(kSaveKey); // SAVE, slot 1 picked: CHOMPI would confirm
        RunMs(100);
        Tap(kSceneKeys[1]);
        RunMs(300);
        const int off = Max(SmtLedFull(kFilterKeyLed));
        Press("KEY_5", true);
        RunMs(100);
        Tap("KEY_26");
        RunMs(100);
        Tap("KEY_26");
        RunMs(100);
        Press("KEY_5", false);
        RunMs(2500);
        Check(Max(SmtLedFull(kFilterKeyLed)) > 2 * off, "save-latch: an FX key held, CHOMPI twice while SAVE waits: the effect latches");
        Check(Card("/FRIZZ/frizz_scenes.txt").empty(), "save-latch: and nothing is saved");
    }});

    cases.push_back({"latch-turn", [] {
        RunMs(kReadyMs);
        const int off = Max(SmtLedFull(kTapeStopKeyLed));
        // the tape stop, CHOMPI held, the transport knob turned: still latched
        Press("KEY_11", true);
        RunMs(80);
        Press("KEY_26", true);
        RunMs(80);
        Turn(5, 4);
        RunMs(200);
        Press("KEY_11", false);
        RunMs(100);
        Press("KEY_26", false);
        RunMs(300);
        Check(Max(SmtLedFull(kTapeStopKeyLed)) > 2 * off, "latch-turn: FX key, CHOMPI held, transport turned, key let go: still latched");
        Tap("KEY_11");
        RunMs(300);
        // and a dark knob: the flanger's page-2 knob 1 (it has no Mix)
        const int flanger_off = Max(SmtLedFull(kFlangerKeyLed));
        Press("KEY_6", true);
        RunMs(80);
        Tap("ENC_4_SW"); // page 2
        RunMs(80);
        Press("KEY_26", true);
        RunMs(80);
        Turn(kKnob1Encoder, 4);
        RunMs(200);
        Press("KEY_6", false);
        RunMs(100);
        Press("KEY_26", false);
        RunMs(300);
        Check(Max(SmtLedFull(kFlangerKeyLed)) > 2 * flanger_off, "latch-turn: the same with a dark knob: still latched");
    }});

    // ---- the settings page (SettingsPage.h): the mode switch up ----

    // the mono input, now a key there (it was VOLUME's page 3)
    cases.push_back({"settings-mono", [] {
        RunMs(kReadyMs);
        SetToggle(true);
        RunMs(300);
        Press("KEY_26", true); // SHIFT: F# shows mono
        RunMs(100);
        const Rgb stereo = SmtLedFull(kMonoKeyLed);
        Tap("KEY_23"); // SHIFT + the upper F#: mono
        RunMs(100);
        const Rgb mono = SmtLedFull(kMonoKeyLed);
        Press("KEY_26", false);
        RunMs(100);
        const Rgb source = SmtLedFull(kMonoKeyLed);
        RunMs(3000);
        Check(Card("/FRIZZ/frizz_master.txt").find("mono 1") != std::string::npos,
              "settings-mono: SHIFT + F# of the upper octave switches AUX to mono, and it's saved");
        Check(mono.r == mono.g && mono.g == mono.b && Max(mono) > 2 * Max(stereo)
                  && stereo.r == stereo.g && stereo.g == stereo.b,
              "settings-mono: while SHIFT is held its key is white, lit while mono, dim while stereo");
        Check(source.g > 200 && source.r < 40 && source.b < 40,
              "settings-mono: SHIFT let go, it shows the source again: AUX, green");
        const std::string state = Ask({0x20});
        Check(state.size() > 6 && (state[6] & 32), "settings-mono: the play page's state says mono");
        // VOLUME's pages are three now: out, in, headphones, and back to out
        SetToggle(false);
        RunMs(300);
        for (int i = 0; i < 3; i++)
        {
            Tap("ENC_6_SW");
            RunMs(300);
        }
        const float before = RunMs(300);
        Turn(kVolumeEncoder, -30);
        RunMs(500);
        Check(RunMs(300) < before * .7f, "settings-mono: VOLUME pressed three times is back on the output gain");
    }});

    // a key held through the flip stays the side's it went down on
    cases.push_back({"settings-flip", [] {
        RunMs(kReadyMs);
        const float dry = RunMs(300);
        Press("KEY_5", true); // the filter, punched in
        RunMs(100);
        Turn(kKnob1Encoder, -40);
        RunMs(500);
        SetToggle(true);
        RunMs(500);
        Check(RunMs(300) < dry * .5f, "settings-flip: an FX key held while the switch goes up stays in");
        Turn(kKnob1Encoder, 40); // nothing on the settings page
        RunMs(500);
        Check(RunMs(300) < dry * .5f, "settings-flip: a knob turned on the settings page does nothing");
        Press("KEY_5", false);
        RunMs(300);
        Check(fabsf(RunMs(300) - dry) < dry * .05f, "settings-flip: let go there, it's off");
        RunMs(2500);
        Check(Card("/FRIZZ/frizz_master.txt").find("midi_channel 5") == std::string::npos,
              "settings-flip: and its release set nothing");
        // the other way: a settings key held while the switch goes down presses nothing
        Press("KEY_5", true);
        RunMs(100);
        SetToggle(false);
        RunMs(500);
        Check(fabsf(RunMs(300) - dry) < dry * .05f,
              "settings-flip: a key gone down on the settings page doesn't punch in on the play page");
        Press("KEY_5", false);
        RunMs(2500);
        Check(Card("/FRIZZ/frizz_master.txt").find("midi_channel 5\n") != std::string::npos,
              "settings-flip: the 5th white key set channel 5 on its press");
        // LOOP pressed up there doesn't record
        SetToggle(true);
        RunMs(300);
        Tap("KEY_28");
        RunMs(500);
        Check(Probe().loop_state == 0, "settings-flip: LOOP on the settings page doesn't record");
    }});

    // a scene mode is left as the switch goes up: CHOMPI back down doesn't confirm it
    cases.push_back({"settings-save", [] {
        RunMs(kReadyMs);
        Latch("KEY_5");
        Tap(kSaveKey); // SAVE, slot 1 picked
        RunMs(100);
        Tap(kSceneKeys[1]);
        RunMs(300);
        SetToggle(true);
        RunMs(300);
        SetToggle(false);
        RunMs(300);
        Tap("KEY_26");
        RunMs(2500);
        Check(Card("/FRIZZ/frizz_scenes.txt").empty(), "settings-save: SAVE armed, a flip up and down: CHOMPI saves nothing");
        Check(Max(SmtLedFull(kSaveKeyLed)) < 80, "settings-save: and SAVE's key is back to dim");
    }});

    // MIDI plays on with the switch up: its notes are the play page's keys
    cases.push_back({"settings-midi", [] {
        RunMs(kReadyMs);
        const float dry = RunMs(300);
        SetToggle(true);
        RunMs(300);
        Usb({kFilterCC, kFilterCutoffCC, 0});
        Usb({kNoteOn, kFilterNote, 100});
        RunMs(500);
        Check(RunMs(300) < dry * .5f, "settings-midi: a note punches the filter in with the switch up");
        Usb({kNoteOff, kFilterNote, 0});
        RunMs(300);
        Check(fabsf(RunMs(300) - dry) < dry * .05f, "settings-midi: and its note off lets go");
        RunMs(2500);
        Check(Card("/FRIZZ/frizz_master.txt").find("midi_channel 16\n") != std::string::npos
                  || Card("/FRIZZ/frizz_master.txt").empty(),
              "settings-midi: the note set no channel");
    }});

    // the MIDI channel and transport following on the keys
    cases.push_back({"settings-channel", [] {
        RunMs(kReadyMs);
        SetToggle(true);
        RunMs(300);
        const Rgb ch16 = SmtLedFull(kChannel1516Led);
        Check(ch16.r > 200 && ch16.g > 200 && ch16.b > 200 && Max(SmtLedFull(kChannel1Led)) < 80,
              "settings-channel: channel 16 at first: the 15th white key white, the others dim");
        Tap("KEY_15");
        RunMs(300);
        const Rgb ch15 = SmtLedFull(kChannel1516Led);
        Check(ch15.r < 40 && ch15.b > 200 && Ask({0x24}).substr(0, 1) == std::string(1, 15),
              "settings-channel: pressed, channel 15: the key light blue");
        Tap("KEY_15");
        RunMs(300);
        Check(Ask({0x24}).substr(0, 1) == std::string(1, 16), "settings-channel: again, 16");
        Tap("KEY_1"); // channel 1
        RunMs(300);
        Check(Max(SmtLedFull(kChannel1Led)) > 2 * Max(SmtLedFull(kChannel1516Led)),
              "settings-channel: the first white key picks channel 1, the 15th dim");
        Tap("KEY_15");
        RunMs(300);
        Check(Ask({0x24}).substr(0, 1) == std::string(1, 15),
              "settings-channel: from another channel, the 15th key picks 15 first");
        Tap("KEY_1");
        RunMs(300);
        SetToggle(false);
        RunMs(300);
        // the 7th white key is the phaser's there, the 11th the tape stop's, the 14th the chaos
        // key's: dimly in their own colours, violet, amber and rose
        const Rgb phaser = SmtLedFull(18), tapestop = SmtLedFull(kChannel11Led),
                  chaos = SmtLedFull(kChannel14Led);
        Check(Max(phaser) < 60 && phaser.b > 2 * phaser.r && phaser.r > phaser.g && Max(tapestop) < 60
                  && tapestop.r > 2 * tapestop.g && tapestop.b == 0 && Max(chaos) < 60 && chaos.r > 2 * chaos.g,
              "settings-channel: back on the play page, the 7th key the phaser's, the 11th the tape stop's, the 14th the chaos key's, dim");
        SetToggle(true);
        RunMs(300);
        Tap("KEY_18"); // F#: transport following
        RunMs(3000);
        const std::string master = Card("/FRIZZ/frizz_master.txt");
        Check(master.find("midi_channel 1\n") != std::string::npos
                  && master.find("midi_transport 1") != std::string::npos,
              "settings-channel: both saved");
        const std::string settings = Ask({0x24});
        Check(settings.size() == 4 && settings[0] == 1 && settings[1] == 1,
              "settings-channel: and in force (SysEx settings)");
        Tap("KEY_16"); // C#: every channel
        RunMs(300);
        const std::string all = Ask({0x24});
        Check(all.size() == 4 && all[0] == 0, "settings-channel: C# listens on every channel");
    }});

    // the battery on VOLUME's LED, all the time; the transport LEDs purple
    cases.push_back({"settings-battery", [] {
        RunMs(kReadyMs);
        SetBattery(3.2f, false);
        SetToggle(true);
        RunMs(32000); // the level is read every 30 s (Hardware::BMCMediumBattCheck)
        const Rgb vol = PthLedFull(kVolumeLed);
        Check(vol.r > 100 && vol.g > 100 && vol.b < 40, "settings-battery: VOLUME shows the battery, yellow below 3.3 V");
        const Rgb rev = PthLedFull(kTransportRevLed), fwd = PthLedFull(kTransportFwdLed);
        Check(Same(rev, fwd) && rev.b > rev.r && rev.r > rev.g, "settings-battery: the transport LEDs are purple");
        SetToggle(false);
        RunMs(300);
        sine = false; // page 2's input meter, silent: dim, never yellow
        Tap("ENC_6_SW");
        RunMs(1000);
        Press("ENC_6_SW", true);
        RunMs(2000);
        const Rgb held = PthLedFull(kVolumeLed);
        Press("ENC_6_SW", false);
        RunMs(300);
        Check(!(held.r > 100 && held.g > 100 && held.b < 40), "settings-battery: VOLUME held on the play page no longer shows it");
    }});

    // VOLUME on the settings page follows the charger cable: white while it's in, charging or
    // full, and the battery's colour within a few seconds of pulling it (it held white for 20
    // minutes after a full charge, and showed green while charging)
    cases.push_back({"settings-charger", [] {
        auto white = [] {
            const Rgb c = PthLedFull(kVolumeLed);
            return c.r > 150 && c.g > 150 && c.b > 150;
        };
        auto green = [] {
            const Rgb c = PthLedFull(kVolumeLed);
            return c.g > 100 && c.r < 60 && c.b < 60;
        };
        SetBattery(3.9f, true, false); // charging
        RunMs(kReadyMs);
        SetToggle(true);
        RunMs(2000);
        Check(white(), "settings-charger: charging, VOLUME is white");
        SetBattery(3.9f, false, false);
        RunMs(3000);
        Check(green(), "settings-charger: pulled while charging, green within 3 s");
        SetBattery(4.1f, true, true); // full
        RunMs(2000);
        Check(white(), "settings-charger: full on the charger, white");
        SetBattery(4.1f, false, false);
        RunMs(3000);
        Check(green(), "settings-charger: pulled when full, green within 3 s");
        SetBattery(3.2f, false, false);
        RunMs(32000); // the level is read every 30 s while it runs on the battery
        const Rgb c = PthLedFull(kVolumeLed);
        Check(c.r > 100 && c.g > 100 && c.b < 40, "settings-charger: on the battery, yellow once below 3.3 V");
        SetBattery(3.8f, false, false);
    }});

    // the LEDs' brightness (#176): knob 1 on the settings page, 25-100 % by 5 % a detent,
    // SHIFT + turn by quarters, SHIFT + press 100 %; no colour gone dark
    static auto brightness_saved = [](int percent) {
        return Card("/FRIZZ/frizz_master.txt").find("\nled_brightness " + std::to_string(percent) + "\n")
               != std::string::npos;
    };
    cases.push_back({"settings-brightness", [] {
        RunMs(kReadyMs);
        RunMs(300);
        const Rgb filter = SmtLed(kFilterKeyLed), vu = PthLed(kVolumeLed);
        SetToggle(true);
        RunMs(300);
        const Rgb knob_full = PthLed(kKnob1Led);
        Check(knob_full.r == knob_full.g && knob_full.g == knob_full.b && knob_full.r > 0
                  && Max(PthLed(2)) == 0 && Max(PthLed(3)) == 0 && Max(PthLed(4)) == 0,
              "settings-brightness: knob 1's LED white, knobs 2-4 dark");
        TurnBrightness(-1); // 95 %
        RunMs(3000);
        Check(brightness_saved(95), "settings-brightness: a detent down is 95 %, saved 2 s after");
        TurnBrightness(-9); // 50 %
        const int knob_50 = Max(PthLed(kKnob1Led));
        RunMs(3000);
        Check(brightness_saved(50), "settings-brightness: nine more is 50 %, saved");
        Check(knob_50 > 0 && knob_50 < Max(knob_full),
              "settings-brightness: its own LED dims with every LED, so it shows the level");
        SetToggle(false);
        RunMs(300);
        const Rgb half = SmtLed(kFilterKeyLed);
        Check(abs(Max(half) - Max(filter) / 2) <= 1 && half.r > 0 && half.g > 0 && half.b > 0,
              "settings-brightness: an FX key that's off at half, its colour kept");
        Check(Max(PthLed(kVolumeLed)) > 0 && Max(PthLed(kVolumeLed)) < Max(vu),
              "settings-brightness: the panel's dimmer too, still lit");
        SetToggle(true);
        RunMs(300);
        TurnBrightness(-30); // past the bottom
        RunMs(3000);
        Check(brightness_saved(25), "settings-brightness: it stops at 25 %");
        SetToggle(false);
        RunMs(300);
        const Rgb low = SmtLed(kFilterKeyLed);
        SetToggle(true);
        RunMs(300);
        printf("      an FX key that's off: full %d %d %d, at 25 %% %d %d %d\n", filter.r, filter.g, filter.b,
               low.r, low.g, low.b);
        Check((low.r > 0) == (filter.r > 0) && (low.g > 0) == (filter.g > 0) && (low.b > 0) == (filter.b > 0),
              "settings-brightness: at 25 % an FX key that's off keeps its colour");
        TurnBrightness(2); // 35 %
        Press("KEY_26", true);
        RunMs(100);
        TurnBrightness(1); // SHIFT: the next quarter up, 50 %
        RunMs(3000);
        const bool up50 = brightness_saved(50);
        TurnBrightness(1); // 75 %
        TurnBrightness(-1); // and down again, 50 %
        TurnBrightness(-1); // 25 %
        RunMs(3000);
        const bool down25 = brightness_saved(25);
        TurnBrightness(4); // 100 %, not past it
        RunMs(3000);
        const bool up100 = brightness_saved(100);
        Press("KEY_26", false);
        RunMs(100);
        Check(up50 && down25 && up100,
              "settings-brightness: SHIFT + turn steps by quarters, 35 % up to 50, then 75, 50, 25, and up to 100");
        TurnBrightness(-3); // 85 %
        RunMs(3000);
        Tap("ENC_1_SW"); // a press alone: nothing
        RunMs(3000);
        const bool kept = brightness_saved(85);
        Press("KEY_26", true);
        RunMs(100);
        Tap("ENC_1_SW"); // SHIFT + press: 100 %
        RunMs(100);
        Press("KEY_26", false);
        RunMs(3000);
        Check(kept && brightness_saved(100), "settings-brightness: knob 1 pressed does nothing, SHIFT + press is 100 %");
        SetToggle(false);
        RunMs(300);
        Check(Same(SmtLed(kFilterKeyLed), filter), "settings-brightness: at 100 % as full as before");
    }});

    // an old file's brightness loads as it was, the upper G# no longer sets it (#176)
    cases.push_back({"settings-brightness-old", [] {
        CardFiles()["/FRIZZ/frizz_master.txt"] = "FRIZZ master 1\nled_brightness 75\n";
        RunMs(kReadyMs);
        RunMs(300);
        const Rgb dim = SmtLed(kFilterKeyLed);
        SetToggle(true);
        RunMs(300);
        Tap("KEY_24"); // the upper G#: the source's effects now
        RunMs(3000);
        Check(brightness_saved(75), "settings-brightness-old: a file's 75 % loads, and the upper G# leaves it");
        TurnBrightness(5);
        SetToggle(false);
        RunMs(300);
        printf("      an FX key that's off: at 75 %% %d, 5 detents up %d\n", Max(dim), Max(SmtLed(kFilterKeyLed)));
        Check(Max(SmtLed(kFilterKeyLed)) > Max(dim),
              "settings-brightness-old: the LEDs at 75 % from it, and up from there by the knob");
    }});

    // the switch up from power-on: the settings page once booted
    cases.push_back({"settings-boot", [] {
        SetToggle(true);
        RunMs(kReadyMs);
        const float dry = RunMs(300);
        Press("KEY_11", true); // the tape stop's key: channel 11 here
        RunMs(1500);
        const float held = RunMs(300);
        Press("KEY_11", false);
        Check(fabsf(held - dry) < dry * .05f,
              "settings-boot: switched on with the switch up, the keys set: the tape stop's doesn't stop");
        Check(Max(PthLedFull(kTransportRevLed)) > 0, "settings-boot: and the page shows");
    }});

    // PLAY held through the flip acts on its release as ever; LOOP up there doesn't erase
    cases.push_back({"settings-play", [] {
        RunMs(kReadyMs);
        Tap("KEY_28");
        RunMs(1500);
        Tap("KEY_28");
        RunMs(500);
        Check(Probe().loop_state == 2, "settings-play: a loop plays");
        Press("KEY_27", true);
        RunMs(100);
        SetToggle(true);
        RunMs(500);
        Press("KEY_27", false);
        RunMs(300);
        Check(Probe().loop_state == 3, "settings-play: PLAY held through the flip up pauses on its release");
        Tap("KEY_28");
        Tap("KEY_27");
        RunMs(500);
        Check(Probe().loop_state == 3, "settings-play: LOOP and PLAY up there neither erase nor play");
    }});

    // a card with every setting on: in force from power-on
    cases.push_back({"settings-kept", [] {
        CardFiles()["/FRIZZ/frizz_master.txt"]
            = "FRIZZ master 1\nmono 1\nmidi_channel 3\nmidi_transport 1\nclock_factor 200\nclock_source 1\nled_brightness 50\n";
        clock_bpm = 120.f;
        RunMs(kReadyMs);
        RunMs(2000);
        Check(Probe().tempo == 240, "settings-kept: the clock factor x2 from the card: 120 BPM followed at 240");
        const int filter = Max(SmtLed(kFilterKeyLed));
        Check(filter >= 3 && filter <= 5, "settings-kept: the LEDs at half from the card (an off FX key's 9 at 4)");
        const std::string state = Ask({0x20});
        Check(state.size() > 6 && (state[6] & 32), "settings-kept: mono from the card");
        const std::string settings = Ask({0x24});
        Check(settings.size() == 4 && settings[0] == 3 && settings[1] == 1 && settings[2] == 1,
              "settings-kept: channel 3, transport following and the clock source TRS from the card");
        SetToggle(true);
        RunMs(300);
        Check(Max(SmtLed(kMonoKeyLed)) > 0 && Max(SmtLed(kChannel3Led)) > Max(SmtLed(kChannel1Led))
                  && SmtLedFull(kSourceKeyLed).r > 100 && SmtLedFull(kSourceKeyLed).g > SmtLedFull(kSourceKeyLed).b
                  && SmtLedFull(kSourceKeyLed).b > 0,
              "settings-kept: and the page shows them, the source key orange");
    }});

    // the clock's tempo factor: x1/2, x1, x2 of a 120 BPM clock
    cases.push_back({"settings-factor", [] {
        clock_bpm = 120.f;
        RunMs(kReadyMs);
        RunMs(2000);
        Check(Probe().tempo == 120, "settings-factor: a 120 BPM clock, the FX at 120");
        SetToggle(true);
        RunMs(300);
        const Rgb one = Peak(kFactorKeyLed, 600); // it pulses on the beats: seen lit
        Tap("KEY_19"); // G#: x1 to x2
        RunMs(3000);
        Check(Probe().tempo == 240, "settings-factor: G# doubles it to 240");
        const Rgb two = Peak(kFactorKeyLed, 600);
        Check(Card("/FRIZZ/frizz_master.txt").find("clock_factor 200") != std::string::npos,
              "settings-factor: saved");
        Tap("KEY_19"); // x2 to x1/2
        RunMs(3000);
        Check(Probe().tempo == 60, "settings-factor: G# again halves it to 60");
        const Rgb half = Peak(kFactorKeyLed, 1100);
        Check(one.r > 200 && one.g > 200 && one.b < 40 && two.r > 200 && two.g < 40
                  && half.b > 200 && half.r < 40,
              "settings-factor: its key yellow at x1, red at x2, light blue at x1/2");
        // a quantized loop of one bar at 60: 4 s
        SetToggle(false);
        RunMs(300);
        Press("KEY_27", true);
        RunMs(100);
        Tap("KEY_28");
        Press("KEY_27", false);
        RunMs(3000);
        Tap("KEY_28"); // closes at the bar's end
        RunMs(3000);
        const ClockState c = Probe();
        Check(c.loop_state == 2 && c.loop_beats == 4 && fabs(c.loop_length - 4. * 48000.) < 48.,
              "settings-factor: a quantized bar at x1/2 holds 4 beats of 60 BPM");
        printf("      loop: %zu frames, %u beats\n", c.loop_length, c.loop_beats);
    }});

    // the factor's key pulses on the beats the effects follow: lit half a beat, dim half
    cases.push_back({"settings-beat", [] {
        clock_bpm = 120.f;
        RunMs(kReadyMs);
        RunMs(2000);
        SetToggle(true);
        RunMs(300);
        // the LEDs are drawn from MainLoop, which now and then comes ~20 ms late
        auto runs_within = [](const std::vector<int>& runs, int lo, int hi) {
            bool ok = runs.size() >= 4;
            for (int r : runs)
                ok &= r >= lo && r <= hi;
            if (!ok)
            {
                printf("      runs:");
                for (int r : runs)
                    printf(" %d", r);
                printf("\n");
            }
            return ok;
        };
        const std::vector<int> one = Runs(Watch(false, kFactorKeyLed, 2000), 100);
        Check(runs_within(one, 225, 275), "settings-beat: at 120 BPM, G# lit 250 ms and dim 250 ms");
        Tap("KEY_19"); // x2
        RunMs(3000);
        const std::vector<int> two = Runs(Watch(false, kFactorKeyLed, 2000), 100);

        Check(runs_within(two, 100, 150), "settings-beat: at x2 (240 BPM), 125 ms each");
        const std::vector<int> dim = Watch(false, kFactorKeyLed, 300);
        Check(*std::min_element(dim.begin(), dim.end()) > 0, "settings-beat: dim, never dark: its colour shows");
        // no clock: the last tempo, 240
        clock_bpm = 0.f;
        RunMs(2000);
        Check(runs_within(Runs(Watch(false, kFactorKeyLed, 2000), 100), 100, 150),
              "settings-beat: the clock gone, it beats on at the last tempo");
    }});

    // the clock source: whose ticks count (MidiClock.h). A sequencer on the jack at 120, a DAW
    // over USB at 90
    cases.push_back({"clock-source", [] {
        clock_bpm = 120.f;
        RunMs(1000);
        usb_clock_bpm = 90.f;
        usb_clock_next = NowMs();
        RunMs(kReadyMs - 1000);
        RunMs(2000);
        Check(Probe().source == 1 && Probe().tempo == 120,
              "clock-source: Auto at first: the jack ticked first, so its 120 counts");
        SetToggle(true);
        RunMs(300);
        Check(Same(SmtLedFull(kSourceKeyLed), Rgb{252, 252, 252}), "clock-source: lower A# white for Auto");
        Tap("KEY_20"); // TRS
        RunMs(2000);
        Check(Probe().source == 1 && Probe().tempo == 120 && Same(SmtLedFull(kSourceKeyLed), Rgb{252, 152, 60}),
              "clock-source: TRS: the jack's still, the key orange");
        Tap("KEY_20"); // USB
        RunMs(300);
        Check(Probe().source == 2, "clock-source: USB: the lock moves to USB's ticks at once");
        RunMs(2000);
        Check(Probe().tempo == 90 && Same(SmtLedFull(kSourceKeyLed), Rgb{0, 0, 252}),
              "clock-source: and the effects follow its 90, the key blue");
        Tap("KEY_20"); // internal
        RunMs(1000);
        Check(!Probe().has_clock && Probe().tempo == 90 && Same(SmtLedFull(kSourceKeyLed), Rgb{252, 88, 156}),
              "clock-source: internal: no clock while both run, the last tempo kept, the key pink");
        RunMs(2500);
        Check(Card("/FRIZZ/frizz_master.txt").find("clock_source 3") != std::string::npos,
              "clock-source: saved");
        SetToggle(false);
        RunMs(300);
        // a quantized recording: refused, as without a clock
        Check(RedBlinksQuantized() == 3, "clock-source: internal: a quantized recording is refused");
        // a tap sets the tempo, as without a clock: 100 BPM
        Press("KEY_26", true);
        for (int i = 0; i < 5; i++)
        {
            Tap("KEY_28");
            RunMs(540);
        }
        Press("KEY_26", false);
        RunMs(300);
        Check(Probe().tempo == 100, "clock-source: internal: tap tempo works while clocks run");
        printf("      tapped: %d BPM\n", Probe().tempo);
        SetToggle(true);
        RunMs(300);
        Tap("KEY_20"); // Auto again
        RunMs(2000);
        Check(Probe().has_clock, "clock-source: a fourth press is Auto again: a clock counts");
    }});

    // MIDI out (MidiOut.h): off, the jack, the jack and USB, on the upper C#
    cases.push_back({"midi-out-key", [] {
        RunMs(kReadyMs);
        SetToggle(true);
        RunMs(300);
        const Rgb off = SmtLedFull(kMidiOutKeyLed);
        Check(off.r == off.g && off.g == off.b && off.r > 0 && off.r < 60,
              "midi-out-key: upper C# dim white: off at first");
        TakeMidiOut();
        RunMs(500);
        Check(TakeMidiOut().empty(), "midi-out-key: and nothing goes out");
        Tap("KEY_21");
        RunMs(500);
        std::vector<MidiOutByte> sent = TakeMidiOut();
        bool trs = !sent.empty(), usb = false;
        for (const MidiOutByte& m : sent)
            usb = usb || m.usb;
        Check(Same(SmtLedFull(kMidiOutKeyLed), Rgb{252, 152, 60}) && trs && !usb,
              "midi-out-key: a press: the jack, orange, its clock going out");
        Tap("KEY_21");
        RunMs(500);
        sent = TakeMidiOut();
        usb = false;
        for (const MidiOutByte& m : sent)
            usb = usb || m.usb;
        Check(Same(SmtLedFull(kMidiOutKeyLed), Rgb{252, 252, 252}) && usb,
              "midi-out-key: another: the jack and USB, white");
        RunMs(2500);
        Check(Card("/FRIZZ/frizz_master.txt").find("midi_out 2") != std::string::npos,
              "midi-out-key: saved");
        Tap("KEY_21");
        RunMs(300);
        TakeMidiOut();
        RunMs(500);
        Check(TakeMidiOut().empty() && Same(SmtLedFull(kMidiOutKeyLed), off),
              "midi-out-key: a third: off again");
    }});

    // MIDI Start / Stop only from the chosen input, from both in Auto and internal; the source
    // over SysEx (0x13 2 N), and its answer's third byte
    cases.push_back({"clock-source-transport", [] {
        RunMs(kReadyMs);
        Tap("KEY_28");
        RunMs(2000);
        Tap("KEY_28");
        sine = false;
        RunMs(500);
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x13, 1, 1, 0xF7}); // transport following on
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x13, 2, 1, 0xF7}); // the source: TRS
        RunMs(20);
        Check(Ask({0x24}).size() == 4 && Ask({0x24})[2] == 1,
              "clock-source-transport: SysEx sets TRS, and a query over USB is still answered");
        Usb({0xFC});
        RunMs(300);
        Check(RunMs(300) > .05f, "clock-source-transport: TRS: USB's Stop doesn't pause the loop");
        Trs({0xFC});
        RunMs(300);
        Check(RunMs(300) < .001f, "clock-source-transport: the jack's does");
        Usb({0xFA});
        RunMs(300);
        Check(RunMs(300) < .001f, "clock-source-transport: nor does USB's Start play it");
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x13, 2, 2, 0xF7}); // USB
        RunMs(20);
        Usb({0xFA});
        RunMs(300);
        Check(RunMs(300) > .05f, "clock-source-transport: USB: USB's Start plays it");
        Trs({0xFC});
        RunMs(300);
        Check(RunMs(300) > .05f, "clock-source-transport: and the jack's Stop doesn't pause it");
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x13, 2, 3, 0xF7}); // internal
        RunMs(20);
        Trs({0xFC});
        RunMs(300);
        Check(RunMs(300) < .001f, "clock-source-transport: internal: Start and Stop from both, the jack's");
        Usb({0xFA});
        RunMs(300);
        Check(RunMs(300) > .05f, "clock-source-transport: and USB's");
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x13, 2, 4, 0xF7}); // no such source
        RunMs(20);
        Check(Ask({0x24}).size() == 4 && Ask({0x24})[2] == 3, "clock-source-transport: a source past 3 is ignored");
    }});

    // a bug report with USB's clock counted: the replay plays it over USB, or the card's source
    // would ignore it (EventLog.h)
    cases.push_back({"clock-source-log", [] {
        CardFiles()["/FRIZZ/frizz_master.txt"] = "FRIZZ master 1\nmidi_transport 1\nclock_source 2\n";
        clock_bpm = 140.f;
        usb_clock_bpm = 100.f;
        RunMs(kReadyMs);
        RunMs(3000);
        Usb({0xFC});
        Trs({0xFA});
        RunMs(500);
        SetToggle(true); // the bug report: SHIFT + VOLUME held 2 s on the settings page
        RunMs(300);
        Press("KEY_26", true);
        RunMs(100);
        Press("ENC_6_SW", true);
        RunMs(2500);
        Press("ENC_6_SW", false);
        Press("KEY_26", false);
        RunMs(300);
        SetToggle(false);
        RunMs(1000);
        Check(Probe().source == 2 && Probe().tempo == 100, "clock-source-log: USB's 100 counts, the jack's 140 doesn't");
        const std::string log = Card("/FRIZZ/bug-1.txt");
        const size_t clock = log.find("\nclock 100.");
        Check(clock != std::string::npos && log.compare(log.find('\n', clock + 1) - 4, 4, " usb") == 0
                  && log.find("\nclock 140") == std::string::npos,
              "clock-source-log: the log has USB's clock, as USB's, and not the jack's");
        Check(log.find("\nusb FC\n") != std::string::npos && log.find("\nmidi FA\n") == std::string::npos,
              "clock-source-log: and USB's Stop, as USB's; the jack's Start wasn't acted on");
        KeepCard();
    }});

    cases.push_back({"bug-replay-usb", [] {
        TakeCard();
        const std::string log = Card("/FRIZZ/bug-1.txt");
        CardFiles().clear();
        std::istringstream script(log + "\nwait 3000\n");
        const int failed = PlayScript(script, nullptr, nullptr);
        Check(failed == 0 && !log.empty() && Probe().source == 2 && Probe().tempo == 100,
              "clock-source-log: played back, USB's clock counts again: 100 BPM");
    }});

    cases.push_back({"select-flash", [] {
        RunMs(kReadyMs);
        // a dim compressor key: a select flashes it white
        Press("KEY_26", true);
        RunMs(50);
        Press("KEY_15", true);
        const std::vector<int> dim = Watch(false, kCompKeyLed, 120);
        Press("KEY_15", false);
        Press("KEY_26", false);
        RunMs(300);
        Check(*std::max_element(dim.begin(), dim.end()) > 240, "select-flash: a dim key flashes white");
        // working hard: it flashes dark
        amp = .9f;
        freq = 110.f;
        Tap("KEY_15");
        Turn(1, 40); // ratio up
        RunMs(500);
        Turn(4, 100); // amount all the way
        RunMs(2000);
        Check(Max(SmtLedFull(kCompKeyLed)) > 150, "select-flash: the compressor key lights up as it reduces");
        Press("KEY_26", true);
        RunMs(50);
        Press("KEY_15", true);
        const std::vector<int> bright = Watch(false, kCompKeyLed, 120);
        Press("KEY_15", false);
        Press("KEY_26", false);
        Check(*std::min_element(bright.begin(), bright.end()) == 0, "select-flash: a bright key flashes dark");
    }});

    cases.push_back({"blinks", [] {
        RunMs(kReadyMs);
        // refused (no clock): 3 quick blinks
        Press("KEY_27", true);
        RunMs(80);
        Press("KEY_28", true);
        const std::vector<int> refused = Watch(true, kLoopLed, 800);
        Press("KEY_28", false);
        Press("KEY_27", false);
        const std::vector<int> quick = Runs(refused, 50);
        bool all_quick = !quick.empty();
        for (int r : quick)
            all_quick &= r >= 80 && r <= 120;
        Check(all_quick, "blinks: a refused quantized record blinks quickly (100 ms)");
        // a quantized recording closing: the slow blink
        clock_bpm = 120.f;
        clock_next = NowMs();
        RunMs(1000);
        Press("KEY_27", true);
        RunMs(80);
        Tap("KEY_28");
        Press("KEY_27", false);
        RunMs(2700); // into the 2nd bar
        Tap("KEY_28"); // closes at the bar's end
        const std::vector<int> slow = Runs(Watch(true, kLoopLed, 1100), 50);
        bool all_slow = !slow.empty();
        for (int r : slow)
            all_slow &= r >= 230 && r <= 270;
        Check(all_slow, "blinks: a quantized recording closing blinks slowly (250 ms)");
    }});

    cases.push_back({"transport-leds", [] {
        RunMs(kReadyMs);
        Tap("KEY_28");
        RunMs(2000);
        Tap("KEY_28");
        RunMs(300);
        Turn(5, 40); // up to the fastest forward
        RunMs(800);
        const std::vector<int> fwd = Watch(true, kTransportFwdLed, 1000);
        Check(*std::min_element(fwd.begin(), fwd.end()) > 0, "transport-leds: at full speed forward the LED never goes dark");
        Press("KEY_26", true); // SHIFT: the ladder, which flips to reverse past 1/16x
        RunMs(60);
        Turn(5, -100); // all the way to the fastest reverse
        RunMs(1000);
        Press("KEY_26", false);
        RunMs(1500);
        const std::vector<int> rev = Watch(true, kTransportRevLed, 1000);
        Check(*std::min_element(rev.begin(), rev.end()) > 0, "transport-leds: nor at full speed in reverse");
    }});

    // the transport while a loop plays: semitones, 2 detents each, stopping at 2x either way and
    // running on from 1/16x through a standstill into reverse (#174); SHIFT + turn the ladder of
    // roots and fifths, 4 detents a step, which flips at 1/16x
    cases.push_back({"transport-semitones", [] {
        RunMs(kReadyMs);
        Tap("KEY_28");
        RunMs(1000);
        Tap("KEY_28");
        RunMs(300);
        auto speed = [] { RunMs(2000); return Probe().loop_speed; }; // after the glide
        auto near = [](float a, int semis) { return fabsf(a - powf(2.f, semis / 12.f)) < .005f; };
        auto shift_turn = [](int detents) {
            Press("KEY_26", true);
            RunMs(60);
            Turn(5, detents);
            RunMs(60);
            Press("KEY_26", false);
        };
        Turn(5, 2);
        Check(near(speed(), 1), "transport-semitones: 2 detents right: a semitone up");
        Turn(5, 1);
        Check(near(speed(), 1), "transport-semitones: 1 more: not yet");
        Turn(5, 1);
        Check(near(speed(), 2), "transport-semitones: 2: the next one");
        shift_turn(4);
        Check(near(speed(), 7), "transport-semitones: SHIFT + 4 detents from +2: the fifth, +7");
        shift_turn(-4);
        Check(near(speed(), 0), "transport-semitones: SHIFT + 4 detents left from +7: 1x");
        shift_turn(3);
        Check(near(speed(), 0), "transport-semitones: SHIFT + 3 detents: not yet");
        Turn(5, -1);
        Check(near(speed(), 0), "transport-semitones: detents don't carry over from SHIFT to the semitones");
        Turn(5, 100);
        Check(near(speed(), 12), "transport-semitones: all the way right: 2x");
        Tap("ENC_5_SW"); // 1x, and no half step left over from the 1 detent above
        Turn(5, -300); // 48 semitones, the standstill's 2 steps and 1 more, 60 again: 222
        RunMs(3000); // the detents take their time, the glide after them
        Check(speed() < 0.f && near(-Probe().loop_speed, 12),
              "transport-semitones: all the way left: on through a standstill to 2x reverse (#174)");
        Turn(5, 120);
        RunMs(3000);
        Check(Probe().loop_speed == -1.f / 16.f, "transport-semitones: 120 detents back: 1/16x reverse");
        Turn(5, 2);
        RunMs(1000); // the glide onto 0x exactly: ~1.8 s
        Check(speed() == 0.f, "transport-semitones: 2 more right: a standstill");
        Turn(5, 2);
        Check(speed() == 0.f, "transport-semitones: 4: still standing");
        Turn(5, 2);
        Check(near(speed(), -48), "transport-semitones: 6: 1/16x forward");
        shift_turn(-4);
        Check(speed() < 0.f && near(-Probe().loop_speed, -48), "transport-semitones: SHIFT + left from there: reverse");
        shift_turn(4);
        Check(near(speed(), -48), "transport-semitones: SHIFT + right: forward again");
        Tap("ENC_5_SW"); // the transport's press: back to 1x
        Check(near(speed(), 0), "transport-semitones: press: 1x");
        // over MIDI: CC 90 sets it outright, in semitones: 0 1/16x, 64 1x, 127 2x
        Trs({kCC, 90, 69});
        Check(near(speed(), 1), "transport-semitones: CC 90 at 69: a semitone up");
        Trs({kCC, 90, 127});
        Check(near(speed(), 12), "transport-semitones: CC 90 at 127: 2x");
        Trs({kCC, 90, 0});
        Check(near(speed(), -48), "transport-semitones: CC 90 at 0: 1/16x");
        Trs({kCC, 90, 64});
        Check(near(speed(), 0), "transport-semitones: CC 90 at 64: 1x");
    }});

    // #174: the standstill between 1/16x and 1/16x reverse. The loop glides to 0x and fades
    // with it, silent there, both transport LEDs red; the FX clock runs on by itself as in a
    // pause, at the slowest speed's tempo (50 BPM), so a morph lands meanwhile; turned on, the
    // loop plays in reverse, heard again, and the clock follows it
    cases.push_back({"transport-standstill", [] {
        RunMs(kReadyMs);
        auto state = [] { return Ask({0x20}); };
        Latch("KEY_5"); // the filter, for a scene to morph to
        Save(1);
        Tap(kSceneKeys[0]);
        RunMs(300);
        Tap("KEY_28");
        RunMs(2000);
        Tap("KEY_28");
        RunMs(300);
        sine = false; // the loop alone
        RunMs(500);
        const float loud = RunMs(500);
        Turn(5, -96); // 48 semitones: 1/16x
        RunMs(3000);
        Check(fabsf(Probe().loop_speed - 1.f / 16.f) < 1e-4f && RunMs(300) > 0.f,
              "transport-standstill: 1/16x, still heard");
        max_step = 0.f;
        Turn(5, -2);
        RunMs(200);
        const float gliding = RunMs(100);
        RunMs(3000);
        const float step = max_step;
        const float still = RunMs(500);
        printf("      loop %.4f, gliding %.4f, standing %.6f, largest step %.4f\n", loud, gliding, still, step);
        Check(Probe().loop_speed == 0.f && still == 0.f, "transport-standstill: 2 detents more: 0x, silent");
        Check(gliding > 0.f && step < .02f, "transport-standstill: faded with the glide, no click");
        const Rgb rev = PthLedFull(kTransportRevLed), fwd = PthLedFull(kTransportFwdLed);
        Check(rev.r > 200 && rev.g < 40 && fwd.r > 200 && fwd.g < 40, "transport-standstill: both transport LEDs red");
        Check(Probe().tempo == 50, "transport-standstill: the FX at the slowest speed's tempo, 50 BPM");
        const uint32_t pos = Probe().position;
        RunMs(1000);
        Check(Probe().position != pos, "transport-standstill: the FX clock runs on by itself");
        // a morph to scene 1, started just after a bar line: the next one 4.8 s later at 50 BPM
        for (int b = 0; b < 20000 && Probe().position % 48 != 1; b++)
            RunBlocks(1);
        Press("KEY_26", true);
        RunMs(60);
        Tap(kSceneKeys[1]);
        RunMs(60);
        Press("KEY_26", false);
        RunMs(3500);
        const bool morphing = state().size() > 6 && (state()[6] & 2);
        RunMs(1500);
        Check(morphing && !(state()[6] & 2) && state()[5] == 2 && Probe().loop_speed == 0.f,
              "transport-standstill: a morph meanwhile lands on the clock's bar line, the loop still standing");
        Turn(5, -4);
        RunMs(3000);
        Check(fabsf(Probe().loop_speed + 1.f / 16.f) < 1e-4f && RunMs(500) > 0.f,
              "transport-standstill: 4 detents on: 1/16x reverse, heard again");
        const float at = Probe().loop_pos;
        RunMs(2000);
        Check(Probe().loop_pos < at || Probe().loop_pos > at + .5f, "transport-standstill: the loop runs backwards");
        Tap("ENC_5_SW");
        RunMs(2000);
        Check(Probe().loop_speed == 1.f && Probe().tempo != 50, "transport-standstill: press: 1x, the loop's tempo again");
    }});

    cases.push_back({"flanger-click", [] {
        amp = .5f;
        RunMs(kReadyMs);
        Latch("KEY_6");
        Turn(2, 60); // the amount up
        RunMs(500);
        Tap("ENC_4_SW"); // page 2: stereo on knob 2 (#117); the timing as before it (#130)
        RunMs(240);
        Save(1); // stereo at 0
        // stereo up: the right LFO runs free and drifts; then a recall takes it back to 0
        // (clicks.cpp's case, on the device). How far they drifted decides how far the right
        // delay has to glide, so after several times, the whole sequence shifted 4 ways: at
        // 560 ms it clicked (#130)
        max_step = 0.f; // over the presses too: the jump comes as the recall lands
        for (uint32_t shift : {0, 560, 1130, 1710})
        {
            RunMs(shift);
            for (uint32_t drift : {6000, 1500, 2500, 3500, 4500})
            {
                Turn(1, 50);
                RunMs(drift);
                Tap(kSceneKeys[1]);
                RunMs(600);
            }
        }
        const float most = max_step;
        const float mono = StepMs(1000); // the flanger as saved, stereo at 0
        printf("      flanger: largest step %.4f around the recalls, %.4f as saved\n", most, mono);
        Check(most < mono * 1.5f, "flanger-click: stereo turned up, then a recall back to 0: no click in either channel");
    }});

    cases.push_back({"freezer-click", [] {
        RunMs(kReadyMs);
        amp = .5f;
        freq = 520.f;
        const float live = StepMs(300); // the input the new press captures, as it is
        freq = 220.f;
        RunMs(100);
        Press("KEY_1", true);
        RunMs(2500);
        freq = 520.f; // the live input now differs from what's frozen
        const float frozen = std::max(live, StepMs(500));
        Press("KEY_1", false);
        RunMs(8); // just into the repeats' fade (a key needs 7 ms to count as let go)
        max_step = 0.f; // from the press on
        Press("KEY_1", true);
        RunMs(300);
        const float again = max_step;
        Press("KEY_1", false);
        printf("      freezer: largest step %.4f pressed again, %.4f frozen or live\n", again, frozen);
        Check(again < frozen * 1.5f, "freezer-click: pressed again while the repeats fade: no click");
    }});

    // the card across power cycles; one boot that couldn't read it never writes it
    cases.push_back({"card-1", [] {
        RunMs(kReadyMs);
        Latch("KEY_6");
        Save(2); // the card's own scene, in slot 2
        Tap("KEY_15");
        Turn(4, 20); // the card's own compressor
        RunMs(3000);
        Check(SavedLatch(Card("/FRIZZ/frizz_scenes.txt"), 2, "flanger") == 1, "card: a scene saved into slot 2 is on the card");
        KeepCard();
    }});

    // the mount fails at boot (FRIZZ itself came from the card): a save flashes red, and once
    // the card answers, the session still doesn't write it, scenes or compressor
    cases.push_back({"card-2", [] {
        TakeCard();
        const std::string old_scenes = Card("/FRIZZ/frizz_scenes.txt");
        const std::string old_master = Card("/FRIZZ/frizz_master.txt");
        SetCardPresent(false);
        RunMs(kReadyMs);
        Latch("KEY_5");
        Tap(kSaveKey);
        RunMs(100);
        Tap(kSceneKeys[1]);
        RunMs(300);
        Tap("KEY_26");
        int red = 0;
        for (int i = 0; i < 300; i++)
        {
            RunMs(1);
            const Rgb c = SmtLedFull(kSlot1Led);
            red += c.r > 150 && c.g < 60 && c.b < 60;
        }
        Check(red > 0, "card: not read at boot, a save flashes the slot red");
        Tap("KEY_15");
        Turn(4, 40); // a compressor of this session
        RunMs(3000);
        SetCardPresent(true);
        RunMs(500);
        Save(1);
        RunMs(2500);
        Check(Card("/FRIZZ/frizz_scenes.txt") == old_scenes && Card("/FRIZZ/frizz_master.txt") == old_master &&
                  !old_master.empty() && CardFiles().size() == 2,
              "card: not read at boot, the session never writes it, even once it answers");
        KeepCard();
    }});

    // the next boot reads it, and saves go to it again
    cases.push_back({"card-3", [] {
        TakeCard();
        RunMs(kReadyMs);
        Check(Max(SmtLedFull(kSlot1Led)) == 0 && Max(SmtLedFull(kSlot1Led + 1)) > 0,
              "card: after a reboot the card's scene is there, the slot saved without it dark");
        Latch("KEY_5");
        Save(1);
        RunMs(2500);
        const std::string scenes = Card("/FRIZZ/frizz_scenes.txt");
        Check(SavedLatch(scenes, 1, "filter") == 1 && SavedLatch(scenes, 2, "flanger") == 1,
              "card: read at boot, a save adds its scene to the card's");
        KeepCard();
    }});

    // the headphone outputs (passthroughEngine.h): the master out at first, the dry input
    // alone with the cue all the way up (VOLUME's page 4, here CC 59)
    cases.push_back({"headphones", [] {
        RunMs(kReadyMs);
        const float master = RunMs(500), hp = hp_rms;
        // at their own level: kHpGain .2 to the line out's .3
        Check(master > .05f && fabsf(hp - master * 2.f / 3.f) < .01f * master,
              "headphones: carry the master out at first, at 2/3 of its level");
        Latch("KEY_11"); // the tape stop: silence on the master
        RunMs(3000);
        const float stopped = RunMs(500), hp_stopped = hp_rms;
        Check(stopped < .01f * master && hp_stopped < .01f * master,
              "headphones: a latched tape stop silences them with the master");
        Usb({kCC, kHpCueCC, 0});
        RunMs(300);
        const float cued = RunMs(500), hp_cued = hp_rms;
        Check(cued < .01f * master && hp_cued > .5f * master,
              "headphones: CC 89 at 0, as VOLUME's knob turned left: the input alone, while the master stays silent");
        Usb({kCC, kHpCueCC, 127});
        RunMs(300);
        RunMs(500);
        Check(hp_rms < .01f * master, "headphones: the cue back down, the master out again");
    }});

    // a restart over MIDI (MidiClock.h), for the launcher: only on FRIZZ's own SysEx
    cases.push_back({"restart", [] {
        RunMs(kReadyMs);
        for (uint8_t b : {0xF0, 0x7D, 0x43, 0x48, 0x01, 0xF7}) // the launcher's PING
            Midi(b);
        for (uint8_t b : {0xF0, 0x7D, 0x43, 0x48, 0x10, 0x00, 0xF7}) // one byte too many
            Midi(b);
        RunMs(50);
        Check(!Restarted(), "restart: other SysEx, the launcher's own too, don't restart FRIZZ");
        for (uint8_t b : {0xF0, 0x7D, 0x43, 0x48, 0x10, 0xF7})
            Midi(b);
        RunMs(50);
        Check(Restarted(), "restart: F0 7D 43 48 10 F7 restarts it (the chip's reset)");
    }});

    // a restart right after a setting changed: the master file waits 2 s for it to rest, but
    // a restart doesn't, so the setting has to reach the card first
    cases.push_back({"restart-saves", [] {
        RunMs(kReadyMs);
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x13, 0, 5, 0xF7}); // the channel: 5
        RunMs(100);
        for (uint8_t b : {0xF0, 0x7D, 0x43, 0x48, 0x10, 0xF7})
            Midi(b);
        RunMs(200);
        Check(Restarted() && Card("/FRIZZ/frizz_master.txt").find("midi_channel 5\n") != std::string::npos,
              "restart: a setting changed just before is on the card when it restarts");
    }});

    // a restart while a DAW's automation keeps changing the compressor: it waits for the card
    // once, not for the automation to stop
    cases.push_back({"restart-automation", [] {
        RunMs(kReadyMs);
        bool asked = false;
        uint32_t asked_at = 0;
        for (int i = 0; i < 300 && !Restarted(); i++) // 3 s of a CC every 10 ms
        {
            Usb({kCC, kCompAmountCC, static_cast<uint8_t>(i % 128)});
            RunMs(10);
            if (i == 50)
            {
                for (uint8_t b : {0xF0, 0x7D, 0x43, 0x48, 0x10, 0xF7})
                    Midi(b);
                asked = true;
                asked_at = NowMs();
            }
        }
        const uint32_t took = NowMs() - asked_at;
        printf("      restarted %u ms after it was asked for\n", took);
        Check(asked && Restarted() && took <= 1200,
              "restart: under a stream of compressor CCs, within a second of being asked");
    }});

    // the event log (EventLog.h): a session, SHIFT + VOLUME held on the settings page, its
    // file on the card
    cases.push_back({"bug-log", [] {
        TakeCard(); // card-3's: scenes in slots 1 and 2
        CardFiles().erase("/FRIZZ/frizz_scenes.bak");
        std::ofstream leds(card_file + ".leds");
        auto run = [&](uint32_t ms) {
            for (uint32_t t = 0; t < ms; t++)
            {
                RunMs(1);
                leds << NowMs() << ' ' << LedLine() << '\n';
            }
        };
        run(kReadyMs);
        Latch("KEY_6");
        Turn(4, 7);              // knob 1
        Press("KEY_26", true);   // SHIFT + knob 1: coarse
        run(20);
        Turn(4, -3);
        run(60);
        Press("KEY_26", false);
        run(200);
        Tap("KEY_28");           // record
        run(2000);
        Tap("KEY_28");           // play
        run(500);
        Turn(5, 6);              // the transport: speed
        run(500);
        Tap(kSceneKeys[2]);           // the card's scene in slot 2
        run(400);
        Save(3);                 // a change to the card after power-on
        run(300);
        Tap("KEY_6");
        run(1000);
        // MIDI: a note holding the filter, a CC, FRIZZ's SysEx key, all played in again
        Trs({kNoteOn, kFilterNote, 100, kFilterCC, kFilterCutoffCC, 20});
        run(300);
        Trs({kNoteOff, kFilterNote, 0});
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x11, 15, 1, 0xF7});
        run(300);
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x11, 15, 0, 0xF7});
        run(300);
        // a long sweep of knob 1: the file runs over several sectors, written in pieces
        for (int i = 0; i < 6; i++)
        {
            Turn(4, i % 2 ? -60 : 60);
            run(600);
        }
        run(500);
        SetToggle(true); // the settings page
        run(300);
        Press("KEY_26", true);
        run(100);
        const uint32_t combo = NowMs();
        Press("ENC_6_SW", true);
        leds << "# combo " << combo << '\n';
        // nothing for 2 s, then the transport LEDs blink white over the page's purple
        bool early = false, white = false;
        for (int i = 0; i < 2500; i++)
        {
            RunMs(1);
            const Rgb rev = PthLedFull(kTransportRevLed), fwd = PthLedFull(kTransportFwdLed);
            const bool lit = std::min({rev.r, rev.g, rev.b, fwd.r, fwd.g, fwd.b}) > 200;
            (i < 1990 ? early : white) |= lit;
        }
        Press("ENC_6_SW", false);
        Press("KEY_26", false);
        RunMs(300);
        SetToggle(false);
        RunMs(1000);
        const std::string log = Card("/FRIZZ/bug-1.txt");
        // the card goes on as before: a save and the compressor land in /FRIZZ
        const std::string scenes = Card("/FRIZZ/frizz_scenes.txt");
        const std::string master = Card("/FRIZZ/frizz_master.txt");
        Save(4);
        Tap("KEY_15");
        Turn(4, 10);
        RunMs(3000);
        Check(Card("/FRIZZ/frizz_scenes.txt") != scenes && Card("/FRIZZ/frizz_master.txt") != master
                  && Card("/frizz_scenes.txt").empty() && Card("/frizz_master.txt").empty(),
              "bug log: saves after it still go to /FRIZZ");
        Check(log.rfind("# FRIZZ event log 1:", 0) == 0 && log.find("# written here") != std::string::npos,
              "bug log: SHIFT + VOLUME held 2 s on the settings page writes /FRIZZ/bug-1.txt");
        Check(!early && white, "bug log: and the transport LEDs blink white, after the 2 s");
        Check(log.find("card file /FRIZZ/frizz_scenes.txt\n|") != std::string::npos
                  && log.find("|scene 3") == std::string::npos,
              "bug log: with the card's scenes as they were at power-on, not as saved since");
        Check(log.find("\nturn 5 1\n") != std::string::npos && log.find("\nup KEY_28\n") != std::string::npos,
              "bug log: and the session's keys and knobs");
        Check(log.find("\nmidi 9F 37 64\n") != std::string::npos
                  && log.find("\nmidi B4 0E 14\n") != std::string::npos
                  && log.find("\nmidi F0 7D 43 48 11 0F 01 F7\n") != std::string::npos,
              "bug log: and the MIDI that came, as it came");
        // every line one the twin reads, over all its sectors
        std::istringstream lines(log);
        std::string line;
        int bad = 0;
        while (std::getline(lines, line))
            bad += !(line.empty() || line[0] == '#' || line[0] == '|' || line.rfind("at ", 0) == 0
                     || line.rfind("down ", 0) == 0 || line.rfind("up ", 0) == 0
                     || line.rfind("turn ", 0) == 0 || line.rfind("card file ", 0) == 0
                     || line.rfind("toggle ", 0) == 0 || line.rfind("input ", 0) == 0
                     || line.rfind("mic ", 0) == 0 || line.rfind("jack ", 0) == 0
                     || line.rfind("midi ", 0) == 0
                     || line == "booted");
        Check(log.size() > 3 * 512 && bad == 0,
              "bug log: a file of several sectors comes out whole (the SD DMA's alignment)");
        KeepCard();
    }});

    // a bug report on a full card: the transport blinks red, and the file that couldn't be
    // written isn't left open (EventLog::Begin closed it only when the head got written); with
    // room again, the next one is written
    cases.push_back({"bug-full", [] {
        TakeCard(); // with scenes on it, so the file's head takes more than a sector
        for (auto it = CardFiles().begin(); it != CardFiles().end();)
            it = it->first.rfind("/FRIZZ/bug-", 0) == 0 ? CardFiles().erase(it) : std::next(it);
        RunMs(kReadyMs);
        Tap("KEY_6");
        RunMs(500);
        SetCardSpace(300); // the head of the file (the card's scenes) doesn't fit
        SetToggle(true);
        RunMs(300);
        auto combo = [] {
            Press("KEY_26", true);
            RunMs(100);
            Press("ENC_6_SW", true);
            bool red = false, white = false;
            for (int i = 0; i < 3500; i++)
            {
                RunMs(1);
                const Rgb rev = PthLedFull(kTransportRevLed);
                red |= rev.r > 200 && rev.g < 80 && rev.b < 80;
                white |= std::min({rev.r, rev.g, rev.b}) > 200;
                if (i == 2300)
                {
                    Press("ENC_6_SW", false);
                    Press("KEY_26", false);
                }
            }
            return std::make_pair(red, white);
        };
        const auto full = combo();
        Check(full.first, "bug log, card full: the transport blinks red");
        Check(CardOpenFiles() == 0, "bug log, card full: no file left open");
        SetCardSpace(SIZE_MAX);
        RunMs(1000);
        const auto room = combo();
        RunMs(1000);
        Check(room.second && Card("/FRIZZ/bug-2.txt").find("# written here") != std::string::npos
                  && CardOpenFiles() == 0,
              "bug log, card full: with room again, the next one is written, and closed");
    }});

    // the log played back on a fresh twin: the same LEDs every ms up to the combo, and the
    // same log written at the end
    cases.push_back({"bug-replay", [] {
        TakeCard();
        const std::string log = Card("/FRIZZ/bug-1.txt");
        CardFiles().clear(); // the card is what the log says
        FILE* leds = tmpfile();
        std::istringstream script(log + "\nwait 3000\n"); // past the 2 s hold that wrote it
        const int failed = PlayScript(script, leds, nullptr);
        Check(failed == 0 && !log.empty(), "bug log: the twin plays it without a fault");

        // the original, ms by ms, and the replay's changes
        std::ifstream orig(card_file + ".leds");
        std::map<uint32_t, std::string> want;
        uint32_t combo = 0;
        std::string line;
        while (std::getline(orig, line))
        {
            if (line.rfind("# combo ", 0) == 0)
                combo = std::stoul(line.substr(8));
            else
                want[std::stoul(line)] = line.substr(line.find(' ') + 1);
        }
        rewind(leds);
        std::map<uint32_t, std::string> got_changes;
        char buf[1024];
        while (fgets(buf, sizeof(buf), leds))
        {
            std::string l(buf);
            l.erase(l.find_last_not_of(" \n") + 1);
            const size_t sp = l.find_first_not_of(' ');
            const size_t end = l.find(' ', sp);
            got_changes[std::stoul(l.substr(sp, end - sp))] = l.substr(l.find("pth"));
        }
        fclose(leds);
        uint32_t differ = 0, first = 0;
        std::string got;
        auto next = got_changes.begin();
        for (auto& w : want)
        {
            if (w.first >= combo)
                break;
            while (next != got_changes.end() && next->first <= w.first)
                got = (next++)->second;
            if (got != w.second && !differ++)
                first = w.first;
        }
        if (differ)
            printf("      %u ms differ, from %u ms\n", differ, first);
        Check(combo > 0 && differ == 0, "bug log: played back, the LEDs are the session's, every ms up to the combo");

        // and it ends at the same combo, so the replay writes the same events
        const std::string again = Card("/FRIZZ/bug-1.txt");
        auto events = [](const std::string& t) { return t.substr(t.find("booted\n")); };
        Check(again.find("booted\n") != std::string::npos && events(again) == events(log),
              "bug log: and the replay writes the same log again");
        unlink((card_file + ".leds").c_str());
    }});

    // the bug report's hold: SHIFT + transport press on the play page no longer writes one
    // (it did until v0.11), nor SHIFT + VOLUME let go before 2 s on the settings page; held
    // long, in either order, it writes one, once
    cases.push_back({"bug-hold", [] {
        auto bugs = [] {
            int n = 0;
            for (auto& f : CardFiles())
                n += f.first.rfind("/FRIZZ/bug-", 0) == 0;
            return n;
        };
        RunMs(kReadyMs);
        const int before = bugs();
        Press("KEY_26", true);
        RunMs(100);
        Press("ENC_5_SW", true);
        RunMs(3000);
        Press("ENC_5_SW", false);
        Press("KEY_26", false);
        RunMs(1000);
        Check(bugs() == before, "bug hold: SHIFT + transport press on the play page writes nothing");
        SetToggle(true);
        RunMs(300);
        Press("KEY_26", true);
        RunMs(100);
        Press("ENC_6_SW", true);
        RunMs(1500);
        Press("ENC_6_SW", false);
        RunMs(100);
        Press("KEY_26", false);
        RunMs(1000);
        Check(bugs() == before, "bug hold: SHIFT + VOLUME let go after 1.5 s writes nothing");
        const int short_hold = bugs();
        Press("ENC_6_SW", true);
        RunMs(100);
        Press("KEY_26", true);
        RunMs(5000);
        Press("KEY_26", false);
        Press("ENC_6_SW", false);
        RunMs(1000);
        Check(bugs() == short_hold + 1, "bug hold: VOLUME + SHIFT held 5 s writes one report");
    }});

    // with the card kept out of it (kCmdDev's PERSIST 0, a test on the device) the hold
    // writes no report either: a bug report played there leaves the card as it was (#171)
    cases.push_back({"bug-ram-only", [] {
        RunMs(kReadyMs);
        const auto files = CardFiles();
        Check(Ask({0x3E, 1, 1, 0}) == std::string("\x01\x01\x00\x01", 4), "bug ram only: PERSIST 0");
        SetToggle(true);
        RunMs(300);
        Press("KEY_26", true);
        RunMs(100);
        Press("ENC_6_SW", true);
        RunMs(3000);
        Press("ENC_6_SW", false);
        Press("KEY_26", false);
        RunMs(1000);
        Check(CardFiles() == files, "bug ram only: SHIFT + VOLUME held 3 s on the settings page writes nothing");
    }});

    // notes press the keys, on FRIZZ's channel only, and a key held by the hand and by MIDI
    // is one press
    cases.push_back({"midi-notes", [] {
        RunMs(kReadyMs);
        const float dry = RunMs(300);
        const int off = Max(SmtLedFull(kFilterKeyLed));
        Trs({0x90, kFilterNote, 100}); // channel 1
        RunMs(100);
        Check(Max(SmtLedFull(kFilterKeyLed)) == off, "midi notes: a note on another channel does nothing");
        Trs({kNoteOn, kFilterNote, 100});
        RunMs(100);
        Check(Max(SmtLedFull(kFilterKeyLed)) > 2 * off, "midi notes: a note on channel 16 holds its key: the filter");
        Trs({kCC, 14, 0});
        RunMs(500);
        Check(RunMs(300) < dry * .5f,
              "midi notes: and the global channel's CC 14 closes its cutoff: its key put it in focus");
        Press("KEY_5", true);
        RunMs(100);
        Trs({kNoteOn, kFilterNote, 0}); // a note on at velocity 0 is a note off
        RunMs(200);
        Check(RunMs(300) < dry * .5f, "midi notes: the hand holds it on after the note's off");
        Press("KEY_5", false);
        RunMs(300);
        Check(fabsf(RunMs(300) - dry) < dry * .05f, "midi notes: and lets go of it");
        Trs({kNoteOn, kFilterNote, 100});
        Trs({kCC, 123, 0}); // all notes off
        RunMs(300);
        Check(fabsf(RunMs(300) - dry) < dry * .05f, "midi notes: all notes off lets go of every key MIDI holds");
        // notes 45-47: CHOMPI, PLAY, LOOP; LOOP records
        Trs({kNoteOn, 47, 100});
        RunMs(60);
        Trs({kNoteOff, 47, 0});
        RunMs(100);
        Check(PthLedFull(kLoopLed).r > 100, "midi notes: note 47 is LOOP");
    }});

    // controllers: latches, an effect's channel, the focus, the compressor, a gain
    cases.push_back({"midi-cc", [] {
        RunMs(kReadyMs);
        const float dry = RunMs(300);
        Trs({kFilterCC, kFilterCutoffCC, 10, kCC, kFilterLatchCC, 127});
        RunMs(500);
        Check(RunMs(300) < dry * .5f,
              "midi cc: CC 106 latches the filter, CC 14 on its channel, 5, sets its cutoff");
        Check(Max(SmtLedFull(kFilterKeyLed)) > 80, "midi cc: its key lit");
        Trs({kCC, kFilterLatchCC, 0});
        RunMs(300);
        Check(fabsf(RunMs(300) - dry) < dry * .05f, "midi cc: CC 106 at 0 unlatches it");
        Trs({kFilterCC, 22, 127});
        RunMs(300);
        Check(RunMs(300) < dry * .5f, "midi cc: CC 22 on its channel latches it too");
        // a latch acts as its lane crosses 64, not on every value: an automation lane sending
        // 127 again doesn't take back what the hand did
        Latch("KEY_5");
        RunMs(300);
        Trs({kFilterCC, 22, 127, kFilterCC, 22, 100});
        RunMs(300);
        Check(fabsf(RunMs(300) - dry) < dry * .05f,
              "midi cc: unlatched by hand, CC 22 at 127 again leaves it off: it acts as it crosses 64");
        Trs({kFilterCC, 22, 0, kFilterCC, 22, 127});
        RunMs(300);
        Check(RunMs(300) < dry * .5f, "midi cc: down and up again, it latches");
        // the focus: CC 23 picks a key, and the global CC 14 right after it goes there
        Trs({kCC, 23, 5, kCC, 14, 64});
        RunMs(50);
        const std::string params = Ask({0x21, 4}), st = Ask({0x20});
        Check(params.size() == 17 && params[1] == 64 && params[2] == 0,
              "midi cc: CC 23 at 5 focuses the filter, and CC 14 right after sets its cutoff (64, its centre)");
        Check(st.size() > 4 && st[4] == 4, "midi cc: the knobs edit it, as SHIFT + its key would");
        Trs({kCC, kCompAmountCC, 127});
        RunMs(50);
        std::string comp = Ask({0x21, 14});
        Check(comp.size() == 17 && comp[1] == 127, "midi cc: CC 24 is the compressor's threshold");
        Trs({0xBE, 14, 0});
        RunMs(50);
        comp = Ask({0x21, 14});
        Check(comp.size() == 17 && comp[1] == 0, "midi cc: and CC 14 on its channel, 15");
        // the last latches by key: 114 the reverb's (key 13, FxId 11), 115 the chaos key's
        // (key 14, FxId 13)
        Trs({kCC, 114, 127, kCC, 115, 127});
        RunMs(100);
        const std::string last = Ask({0x20});
        const int latched = last.size() > 9 ? static_cast<uint8_t>(last[8]) << 7 | static_cast<uint8_t>(last[9]) : 0;
        Check((latched >> 11 & 1) && (latched >> 13 & 1) && !(latched >> 12 & 1),
              "midi cc: CC 114 latches the reverb, CC 115 the chaos key");
        Trs({kCC, 85, 0}); // output gain
        RunMs(300);
        Check(RunMs(300) < .001f, "midi cc: CC 85 sets the output gain");
    }});

    // the channels (#97): the global one's map wins on it; all channels, the global map only;
    // the effect channels over the jack only if the settings page lets them
    cases.push_back({"midi-channels", [] {
        RunMs(kReadyMs);
        auto param = [](int fx, int p = 0) {
            const std::string d = Ask({0x21, fx});
            return d.size() == 17 ? d[1 + 2 * p] << 7 | d[2 + 2 * p] : -1;
        };
        const int freezer = param(0), filter = param(4);
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x13, 0, 5, 0xF7}); // the global channel: 5
        RunMs(50);
        Trs({0xB4, 14, 0, 0xB4, 106, 127});
        RunMs(300);
        Check(param(4) == filter && param(0) == 0 && Max(SmtLedFull(kFilterKeyLed)) > 80,
              "midi-channels: global on 5, its CC 14 is the focus's (the freezer), 106 latches the filter");
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x13, 0, 0, 0xF7}); // all channels
        RunMs(50);
        Trs({0xB2, 15, 64, 0xB2, 106, 0}); // channel 3: the folder's, if the block counted
        RunMs(300);
        Check(param(0, 1) == 64 << 7 && param(2, 1) != 64 << 7 && Max(SmtLedFull(kFilterKeyLed)) < 80,
              "midi-channels: on all channels, channel 3 is the global map too: the focus and the latches");
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x13, 0, 16, 0xF7});
        RunMs(50);
        // the block over the jack switched off on the settings page's lower D#, USB's still counts
        SetToggle(true);
        RunMs(300);
        Check(Max(SmtLedFull(kBlockTrsKeyLed)) > 80, "midi-channels: the lower D# lit: the block over the jack, at first");
        Tap("KEY_17");
        RunMs(100);
        Check(Max(SmtLedFull(kBlockTrsKeyLed)) < 80, "midi-channels: pressed, dim");
        SetToggle(false);
        RunMs(300);
        Trs({0xB4, 14, 20});
        RunMs(50);
        Check(param(4) == filter, "midi-channels: the block off over the jack: channel 5's CC 14 does nothing there");
        Usb({0xB4, 14, 20});
        RunMs(50);
        Check(param(4) == 20 << 7, "midi-channels: and still counts over USB");
        RunMs(2500);
        Check(Card("/FRIZZ/frizz_master.txt").find("midi_block_trs 0\n") != std::string::npos,
              "midi-channels: kept on the card");
        (void)freezer;
    }});

    // nothing that comes in by CC goes to the card (#97 D6): the compressor's CC changes it,
    // but what's kept is the hand's
    cases.push_back({"midi-not-kept", [] {
        RunMs(kReadyMs);
        const std::string before = Card("/FRIZZ/frizz_master.txt");
        Trs({kCC, kCompAmountCC, 100, kCC, 85, 10});
        RunMs(2500);
        const std::string comp = Ask({0x21, 14});
        Check(comp.size() == 17 && comp[1] > 90 && Card("/FRIZZ/frizz_master.txt") == before,
              "midi-not-kept: CC 24 sets the compressor's threshold, and nothing's written");
        Tap("KEY_15");
        Turn(1, 10); // its ratio, by hand
        RunMs(2500);
        const std::string master = Card("/FRIZZ/frizz_master.txt");
        Check(master.find("\ncompressor2 0 ") != std::string::npos,
              "midi-not-kept: a knob turned by hand writes the card, with the threshold as the hand left it");
    }});

    // a DAW chasing every lane as it starts: more latches in one USB packet than the queue
    // of what acts once holds, every one as its last message said
    cases.push_back({"midi-latch-burst", [] {
        RunMs(kReadyMs);
        std::vector<int> burst;
        for (int key = 0; key < 13; key++) // all on, all off, keys 1-7 on again
            burst.insert(burst.end(), {kCC, 102 + key, 127});
        for (int key = 0; key < 13; key++)
            burst.insert(burst.end(), {kCC, 102 + key, 0});
        for (int key = 0; key < 7; key++)
            burst.insert(burst.end(), {kCC, 102 + key, 127});
        burst.insert(burst.end(), {0xB0, 22, 0, 0xB0, 22, 127}); // the freezer's own channel too
        Usb(burst);
        RunMs(300);
        const std::string st = Ask({0x20});
        const int latched = st.size() > 9 ? static_cast<uint8_t>(st[8]) << 7 | static_cast<uint8_t>(st[9]) : -1;
        // keys 1-7: the freezer, shifter, folder, crusher, filter, flanger (FxId 0-5), phaser (12)
        const int want = 0x3f | 1 << 12;
        printf("      latched %04x, want %04x\n", latched, want);
        Check(latched == want, "midi-latch-burst: 41 latch messages in one packet, each latch as its last said");
    }});

    // MIDI feedback (#104, MidiFeedback.h): the hand's changes out as the CCs that set them
    cases.push_back({"midi-feedback", [] {
        RunMs(kReadyMs);
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x13, 3, 2, 0xF7}); // MIDI out: the jack and USB
        RunMs(100);
        TrsCcs();
        UsbCcs();
        Turn(kKnob1Encoder, -10);
        RunMs(300);
        Check(TrsCcs().empty() && UsbCcs().empty(), "midi-feedback: off at first: a knob turned sends no CC");
        SetToggle(true);
        RunMs(300);
        const bool dim = Max(SmtLedFull(kFeedbackKeyLed)) < 80;
        Tap("KEY_22");
        RunMs(100);
        Check(dim && Max(SmtLedFull(kFeedbackKeyLed)) > 80, "midi-feedback: the upper D# switches it on, lit");
        SetToggle(false);
        RunMs(300);
        TrsCcs();
        UsbCcs();
        Tap("KEY_5"); // the filter, in focus
        RunMs(100);
        Check(HasCc(TrsCcs(), 0xBF, 23, 5), "midi-feedback: a key picking the filter sends CC 23 at 5");
        UsbCcs();
        Turn(kKnob1Encoder, -20);
        RunMs(500);
        const std::vector<std::array<int, 3>> trs = TrsCcs(), usb = UsbCcs();
        const std::string p = Ask({0x21, 4});
        const int want = p.size() == 17 ? Knob7(KnobOf(p[1], p[2])) : -1;
        printf("      the cutoff at %d: the jack's last %d and %d, USB's %d\n", want, LastCc(trs, 0xB4, 14),
               LastCc(trs, 0xBF, 14), LastCc(usb, 0xB4, 14));
        Check(want >= 0 && LastCc(trs, 0xB4, 14) == want && LastCc(trs, 0xBF, 14) == want
                  && LastCc(usb, 0xB4, 14) == want && LastCc(usb, 0xBF, 14) == want,
              "midi-feedback: knob 1 by hand: CC 14 on the filter's channel and the global one, where it stopped, on both ports");
        // a value goes once a 10 ms sweep at most: the gaps between CC 14's on its channel
        Turn(kKnob1Encoder, 60);
        RunMs(1000);
        std::vector<double> at;
        std::vector<int> m;
        for (const MidiOutByte& b : TakeMidiOut())
        {
            if (b.usb || b.byte >= 0xF8)
                continue;
            if (b.byte & 0x80)
                m.clear();
            m.push_back(b.byte);
            if (m.size() == 3 && m[0] == 0xB4 && m[1] == 14)
                at.push_back(b.ms);
        }
        double gap = 1e9;
        for (size_t i = 1; i < at.size(); i++)
            gap = std::min(gap, at[i] - at[i - 1]);
        printf("      60 detents: CC 14 sent %zu times, %.1f ms apart at least\n", at.size(), gap);
        Check(at.size() > 3 && gap >= 9., "midi-feedback: a value goes at most once a 10 ms sweep");
        // what came in isn't sent back
        UsbCcs();
        Usb({0xB4, 14, 30, kCC, 14, 90, kCC, 106, 127, kCC, 85, 100});
        RunMs(200);
        const std::vector<std::array<int, 3>> echo = TrsCcs(), echo_usb = UsbCcs();
        for (const std::array<int, 3>& e : echo)
            printf("      echo %02X %d %d\n", e[0], e[1], e[2]);
        Check(echo.empty() && echo_usb.empty(), "midi-feedback: CCs that came in by MIDI aren't sent back");
        Latch("KEY_5"); // by hand: off again
        RunMs(100);
        const std::vector<std::array<int, 3>> latch = TrsCcs();
        Check(HasCc(latch, 0xB4, 22, 0) && HasCc(latch, 0xBF, 106, 0),
              "midi-feedback: a latch by hand: CC 22 on its channel and CC 106");
        Turn(kVolumeEncoder, -5);
        RunMs(100);
        Check(LastCc(TrsCcs(), 0xBF, 85) >= 0, "midi-feedback: VOLUME by hand: CC 85");
        // on a battery, no host: nothing tried over USB (#84), the jack's as ever
        SetUsbHost(false);
        TrsCcs();
        TakeUsbOut();
        Turn(kKnob1Encoder, 10);
        RunMs(500);
        const std::vector<std::array<int, 3>> no_host = TrsCcs();
        Check(LastCc(no_host, 0xB4, 14) >= 0 && TakeUsbOut().empty(),
              "midi-feedback: without a USB host, nothing goes to USB, the jack's CCs still do");
        SetUsbHost(true);
        RunMs(2500);
        Check(Card("/FRIZZ/frizz_master.txt").find("midi_feedback 1") != std::string::npos,
              "midi-feedback: kept on the card");
    }});

    // a scene's recall sends what changed, once; a morph only where it lands; the effect
    // channels not on the jack when they don't count there
    cases.push_back({"midi-feedback-scenes", [] {
        RunMs(kReadyMs);
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x13, 3, 1, 0xF7});           // MIDI out: the jack
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x3E, 3, 1, 0x23, 8, 0, 1, 0xF7}); // midi_feedback 1
        RunMs(100);
        Latch("KEY_5");
        Turn(kKnob1Encoder, -40);
        RunMs(300);
        Save(1);
        RunMs(500);
        TrsCcs();
        Tap(kSceneKeys[0]); // the blank scene: the filter off, its cutoff back
        RunMs(500);
        std::map<int, int> count;
        bool latch_off = false;
        for (const std::array<int, 3>& m : TrsCcs())
        {
            count[m[0] << 8 | m[1]]++;
            latch_off = latch_off || (m[0] == 0xB4 && m[1] == 22 && m[2] == 0);
        }
        bool once = !count.empty();
        for (const auto& c : count)
            once = once && c.second == 1;
        Check(latch_off && once, "midi-feedback-scenes: a recall sends what changed, each once");
        // a morph to scene 1: nothing of the filter's while it glides, where it lands after
        ToBarLine();
        Press("KEY_26", true);
        RunMs(60);
        Tap(kSceneKeys[1]);
        Press("KEY_26", false);
        RunMs(800);
        int gliding = 0;
        for (const std::array<int, 3>& m : TrsCcs())
            gliding += m[0] == 0xB4;
        RunMs(4000);
        const std::vector<std::array<int, 3>> landed = TrsCcs();
        const std::string p = Ask({0x21, 4});
        Check(gliding == 0 && HasCc(landed, 0xB4, 22, 127)
                  && p.size() == 17 && LastCc(landed, 0xB4, 14) == Knob7(KnobOf(p[1], p[2])),
              "midi-feedback-scenes: a morph sends nothing while it glides, then where it landed");
        // the effect channels off on the jack: only the global channel's there
        SetToggle(true);
        RunMs(300);
        Tap("KEY_17");
        SetToggle(false);
        RunMs(300);
        TrsCcs();
        Turn(kKnob1Encoder, -10);
        RunMs(100);
        const std::vector<std::array<int, 3>> global_only = TrsCcs();
        Check(LastCc(global_only, 0xBF, 14) >= 0 && LastCc(global_only, 0xB4, 14) < 0,
              "midi-feedback-scenes: with the effect channels off on the jack, only channel 16's go there");
    }});

    // the beat on the PLAY LED (#110), the tempo the effects follow (as the settings page's G#)
    cases.push_back({"beat-led", [] {
        RunMs(kReadyMs);
        // PLAY's level every ms for ms, and the loop's position
        auto watch = [](uint32_t ms, std::vector<int>& play, std::vector<float>& pos) {
            play.clear();
            pos.clear();
            for (uint32_t t = 0; t < ms; t++)
            {
                RunMs(1);
                play.push_back(PthLedFull(kPlayLed).r);
                pos.push_back(Probe().loop_pos);
            }
        };
        // a pulse's starts: where the level rises over a threshold
        auto rises = [](const std::vector<int>& v, int over) {
            std::vector<size_t> at;
            for (size_t i = 1; i < v.size(); i++)
                if (v[i] > over && v[i - 1] <= over)
                    at.push_back(i);
            return at;
        };
        std::vector<int> play;
        std::vector<float> pos;
        // no loop: dim white for the first half of each beat, at 120 BPM (power-on's tempo)
        watch(2000, play, pos);
        std::vector<size_t> beats = rises(play, 10);
        int lit = 0, top = 0;
        for (int v : play)
        {
            lit += v > 10;
            top = std::max(top, v);
        }
        printf("      no loop: %zu beats in 2 s, lit %d ms, at most %d\n", beats.size(), lit, top);
        Check(beats.size() == 4 && lit > 900 && lit < 1100 && top > 40 && top <= kBeatPulseMax
                  && Same(PthLedFull(kPlayLed), Rgb{0, 0, 0}) == (play.back() <= 10),
              "beat-led: no loop: PLAY pulses dim white on each beat, the first half lit");
        // recording: LOOP red, PLAY counting the beats
        Tap("KEY_28");
        RunMs(10);
        watch(1000, play, pos);
        const Rgb rec = PthLedFull(kLoopLed);
        Check(rises(play, 10).size() == 2 && rec.r > 200 && rec.g == 0,
              "beat-led: recording: LOOP red, PLAY pulsing the beat");
        RunMs(980); // a 2 s loop: 4 beats at 120 BPM, a bar
        Tap("KEY_28");
        RunMs(600);
        // playing: the crossfade as before, a short flash on each beat, more on the bar's 1
        watch(4000, play, pos);
        std::vector<int> off; // the flash, either way off the crossfade
        for (size_t i = 0; i < play.size(); i++)
            off.push_back(abs(play[i] - static_cast<int>((1.f - pos[i]) * 252.f + .5f)));
        beats = rises(off, 40);
        int down = 0, other = 0;
        for (size_t b : beats)
        {
            int peak = 0, len = 0;
            for (size_t i = b; i < off.size() && off[i] > 40; i++, len++)
                peak = std::max(peak, off[i]);
            const bool one = pos[b] < .05f || pos[b] > .95f; // a 1-bar loop: its start
            (one ? down : other) = std::max(one ? down : other, peak);
            printf("      flash at %zu ms (loop at %.3f): %d off for %d ms\n", b, pos[b], peak, len);
        }
        Check(beats.size() == 8 && down > other + 40,
              "beat-led: playing: a flash on each beat over the crossfade, the bar's 1 the strongest, at the loop's start too");
        // paused: the dim position, the beat on it
        Tap("KEY_27");
        RunMs(300);
        watch(2000, play, pos);
        Check(rises(play, static_cast<int>((1.f - pos[0]) * 252.f * .3f) + 10).size() == 4,
              "beat-led: paused: the beat pulses over the dim position");
        // LOOP's red blinks stay: an erase at the loop's end waits, blinking
        Tap("KEY_27");
        RunMs(100);
        Press("KEY_27", true);
        RunMs(60);
        Tap("KEY_28");
        Press("KEY_27", false);
        int red = 0;
        for (int t = 0; t < 600; t++)
        {
            RunMs(1);
            const Rgb l = PthLedFull(kLoopLed);
            red += l.r > 200 && l.g == 0;
        }
        Check(red > 100, "beat-led: LOOP's red blink for an erase waiting shows through");
    }});

    // the beat dims with the LEDs: at 50 %, half
    cases.push_back({"beat-led-dim", [] {
        RunMs(kReadyMs);
        auto top = [] {
            int t = 0;
            for (int ms = 0; ms < 1000; ms++)
            {
                RunMs(1);
                t = std::max(t, static_cast<int>(PthLedFull(kPlayLed).r));
            }
            return t;
        };
        const int full = top();
        SetToggle(true);
        RunMs(200);
        TurnBrightness(-10); // 50 %
        SetToggle(false);
        RunMs(300);
        const int half = top();
        printf("      the beat's top %d at 100 %%, %d at 50 %%\n", full, half);
        Check(half > 0 && half < full * .7f, "beat-led-dim: the beat dims with the LEDs' brightness");
    }});

    // what hosts send by themselves reaches nothing: an RPN (a DAW's pitch-bend range) with its
    // data entry and the null RPN, on the global channel and an effect's; data increment;
    // keyboards' CC 1, 7, 64, 71, 74, 91, 93; Reset All Controllers (#97)
    cases.push_back({"midi-ignored", [] {
        RunMs(kReadyMs);
        std::string before[kNumFxProbe];
        for (int fx = 0; fx < kNumFxProbe; fx++)
            before[fx] = Ask({0x21, fx});
        const std::string state = Ask({0x20});
        for (int status : {kCC, 0xB7, 0xB8})
        {
            Trs({status, 101, 0, status, 100, 0, status, 6, 12, status, 38, 0, status, 96, 1,
                 status, 97, 1, status, 101, 127, status, 100, 127});
            Trs({status, 1, 100, status, 7, 100, status, 64, 127, status, 71, 100, status, 74, 20,
                 status, 91, 100, status, 93, 100, status, 121, 0, status, 98, 3, status, 99, 1});
        }
        RunMs(300);
        bool same = true;
        for (int fx = 0; fx < kNumFxProbe; fx++)
            same = same && Ask({0x21, fx}) == before[fx];
        const std::string after = Ask({0x20});
        Check(same && after.size() > 20 && after.substr(8, 13) == state.substr(8, 13),
              "midi-ignored: (N)RPN, data entry, keyboards' CCs and Reset All Controllers change nothing");
    }});

    // FRIZZ's SysEx: keys, detents and the settings in, the state, LEDs, load out over USB
    cases.push_back({"midi-sysex", [] {
        RunMs(kReadyMs);
        const float dry = RunMs(300);
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x11, 11, 1, 0xF7}); // KEY_5's SwId: the filter
        RunMs(100);
        Check(Max(SmtLedFull(kFilterKeyLed)) > 80, "midi sysex: 11 holds a key by its SwId");
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x12, 4, 0x7F - 39, 0xF7}); // knob 1 (SW4), -40
        RunMs(500);
        Check(RunMs(300) < dry * .5f, "midi sysex: 12 turns a knob by its SWn: the cutoff");
        // several detents at once count each, as the hand's: 6 on a stepped knob are 2 steps
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x11, 25, 1, 0xF7}); // KEY_12: the delay
        RunMs(80);
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x11, 25, 0, 0xF7});
        RunMs(80);
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x12, 4, 6, 0xF7});
        RunMs(100);
        const std::string delay = Ask({0x21, 10});
        Check(delay.size() == 17 && delay[1] == 64 && delay[2] == 0,
              "midi sysex: 12 turning 6 detents moves the delay's division 2 steps, 1/4 to 1/4.");
        const std::string state = Ask({0x20});
        Check(state.size() >= 12 && state[0] == 0 && state[4] == 10 && (state[11] >> 4 & 1),
              "midi sysex: 20 answers the state: no loop, the delay selected, the filter on");
        bool leds_ok = true;
        for (int part = 0; part < 4; part++)
        {
            const std::string leds = Ask({0x22, part});
            static const int kFirst[] = {0, 0, 9, 17}, kCount[] = {10, 9, 8, 8};
            leds_ok &= leds.size() == 1u + 3 * kCount[part] && leds[0] == part;
            for (int i = 0; leds_ok && i < kCount[part]; i++)
            {
                const Rgb c = part == 0 ? PthLed(i) : SmtLed(kFirst[part] + i);
                leds_ok &= leds[1 + 3 * i] == c.r && leds[2 + 3 * i] == c.g && leds[3 + 3 * i] == c.b;
            }
        }
        Check(leds_ok, "midi sysex: 22 answers every LED as it's lit");
        const std::string load = Ask({0x23});
        Check(load.size() == 10 && load[4] == 0 && load[5] == 0 && load[6] == 0
                  && ((load[7] << 14) | (load[8] << 7) | load[9]) > 10,
              "midi sysex: 23 answers the load, the blocks over 100 % (none) and the blocks");
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x11, 11, 0, 0xF7});
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x13, 0, 1, 0xF7}); // channel 1
        RunMs(300);
        Check(fabsf(RunMs(300) - dry) < dry * .05f, "midi sysex: 11 lets go of it");
        Trs({0x90, kFilterNote, 100});
        RunMs(100);
        Check(Max(SmtLedFull(kFilterKeyLed)) > 80, "midi sysex: 13 sets the channel: now 1");
        Check(Ask({0x24}) == std::string("\x01\x00\x00\x00", 4), "midi sysex: 24 answers the settings");
        RunMs(2500);
        Check(Card("/FRIZZ/frizz_master.txt").find("midi_channel 1\n") != std::string::npos,
              "midi sysex: the channel goes to the card");
        // queries come over USB only, where the answer goes: none over the jack
        TakeUsbOut();
        Trs({0xF0, 0x7D, 0x43, 0x48, 0x20, 0xF7});
        RunMs(20);
        Check(TakeUsbOut().empty(), "midi sysex: a query over the jack isn't answered");
    }});

    // the developers' SysEx (kCmdDev, 0x3E): IDENT, GET and SET by (group, index), PERSIST 0,
    // over USB only, each answered with its SEQ and a status
    cases.push_back({"midi-dev", [] {
        RunMs(kReadyMs);
        const std::string ident = Ask({0x3E, 0, 5});
        Check(ident.size() == 29 && ident[0] == 0 && ident[1] == 5 && ident[2] == 0 && ident[3] == 1
                  && ident[4] == 0 && ident[28] == 0,
              "midi dev: IDENT answers, its SEQ back, version 1, FRIZZ, the card written");
        const auto get7 = [](const std::string& d, size_t at, size_t groups) {
            uint64_t v = 0;
            for (size_t i = 0; i < groups; i++)
                v = v << 7 | static_cast<uint8_t>(d[at + i]);
            return v;
        };
        if (ident.size() == 29)
        {
            const uint64_t up = get7(ident, 13, 5), passes = get7(ident, 18, 5);
            Check(up >= static_cast<uint64_t>(kReadyMs) && up <= NowMs() && passes > 100,
                  "midi dev: with the uptime and MainLoop's passes");
        }
        // GET: the filter's knobs, both pages; SET its cutoff and the compressor's threshold
        const std::string filter = Ask({0x3E, 2, 6, 4});
        Check(filter.size() == 5 + 16 && filter[2] == 0 && filter[3] == 4 && filter[4] == 8,
              "midi dev: GET an effect's 8 knobs, by its FxId");
        const std::string set = Ask({0x3E, 3, 7, 4, 0, 0x20, 0});
        const std::string params = Ask({0x21, 4});
        Check(set == std::string("\x03\x07\x00\x04\x00", 5) && params.size() == 17 && params[1] == 0x20
                  && params[2] == 0,
              "midi dev: SET the cutoff in 14 bits, as kCmdParams then reads it");
        Ask({0x3E, 3, 8, 0x20, 0, 0x7F, 0x7F});
        const std::string comp = Ask({0x3E, 2, 9, 0x20});
        Check(comp.size() == 21 && comp[5] == 0x7F && comp[6] == 0x7F,
              "midi dev: the compressor is group 0x20, whatever the effects' number");
        // VOLUME's: the output gain to 0
        const float dry = RunMs(300);
        Ask({0x3E, 3, 10, 0x22, 0, 0, 0});
        RunMs(300);
        Check(dry > .01f && RunMs(300) < .001f, "midi dev: SET VOLUME's output gain");
        Ask({0x3E, 3, 11, 0x22, 0, 0x40, 0});
        // the settings by masterfile::kLines: clock_factor (3) to 200, led_brightness (6) 20 refused
        const std::string factor = Ask({0x3E, 3, 12, 0x23, 3, 1, 200 - 128});
        const std::string dim = Ask({0x3E, 3, 13, 0x23, 6, 0, 20});
        const std::string settings = Ask({0x3E, 2, 14, 0x23});
        Check(factor.size() == 5 && factor[2] == 0 && dim.size() == 5 && dim[2] == 1,
              "midi dev: SET a setting, a value it doesn't take refused");
        Check(settings.size() == 5 + 30 && settings[4] == 15 && settings[11] == 1 && settings[12] == 200 - 128
                  && settings[17] == 0 && settings[18] == 100 && settings[7] == 0 && settings[8] == 16
                  && settings[19] == 0 && settings[20] == 1 && settings[21] == 0 && settings[22] == 0
                  && settings[24] == 0 && settings[26] == 75 && settings[28] == 75
                  && settings[30] == 0 && settings[32] == 1 && settings[34] == 0,
              "midi dev: GET the settings: the clock factor x2, the LEDs at 100 %, channel 16, the effect channels over the jack, no feedback, AUX through the effects, both input gains 75 %, the mic through them, AUX live, the mic not");
        Check(Ask({0x3E, 3, 15, 0x23, 15, 0, 1})[2] == 1 && Ask({0x3E, 2, 16, 0x30})[2] == 1
                  && Ask({0x3E, 0x30, 17})[2] == 1,
              "midi dev: no such setting, group or sub-command: refused, answered");
        RunMs(2500);
        Check(Card("/FRIZZ/frizz_master.txt").find("clock_factor 200\n") != std::string::npos,
              "midi dev: a setting SET goes to the card, as its key's");

        // PERSIST 0: nothing more on the card until a restart, though the page shows it saved
        const std::string master = Card("/FRIZZ/frizz_master.txt"), scenes = Card("/FRIZZ/frizz_scenes.txt");
        const std::string persist = Ask({0x3E, 1, 18, 0});
        Check(persist == std::string("\x01\x12\x00\x01", 4) && Ask({0x3E, 0, 19})[28] == 1,
              "midi dev: PERSIST 0, and IDENT says so");
        Ask({0x3E, 3, 20, 0x23, 3, 0, 50});
        Ask({0x3E, 3, 21, 0x20, 1, 0, 0});
        Tap("KEY_5");
        Save(1);
        RunMs(2500);
        const std::string slot = Ask({0x30, 1, 8});
        Check(slot.size() == 5 && slot[2] == 1, "midi dev: a scene saved after it is in its slot");
        Check(Card("/FRIZZ/frizz_master.txt") == master && Card("/FRIZZ/frizz_scenes.txt") == scenes,
              "midi dev: but neither it nor the settings are on the card");

        // several at once are queued, each answered; none over the jack; FRIZZ's answers
        // echoed back ask nothing
        TakeUsbOut();
        for (int seq = 30; seq < 34; seq++)
            Usb({0xF0, 0x7D, 0x43, 0x48, 0x3E, 0, seq, 0xF7});
        RunMs(30);
        const std::string four = TakeUsbOut();
        int answered = 0;
        for (size_t at = four.find("\x7E"); at != std::string::npos; at = four.find("\x7E", at + 1))
            answered += four[at + 1] == 0 && four[at + 2] >= 30 && four[at + 2] < 34;
        Check(answered == 4, "midi dev: 4 at once, 4 answers");
        Trs({0xF0, 0x7D, 0x43, 0x48, 0x3E, 0, 1, 0xF7});
        RunMs(20);
        Check(TakeUsbOut().empty(), "midi dev: none over the jack");
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x60, 1, 2, 0xF7}); // an echo of 0x20's answer
        Check(Ask({0x20}).size() > 12, "midi dev: an answer echoed back doesn't take the query's slot");
    }});

    // the developers' probes (kCmdDev, #105): PROBE, LEVELS, SIGNAL, RESEED, BATTERY. Read
    // only, but SIGNAL; nothing of theirs runs in the audio callback until asked
    cases.push_back({"midi-dev-probe", [] {
        RunMs(kReadyMs);
        // PROBE 1, the looper: a second recorded by LOOP, playing
        const std::string empty = Ask({0x3E, 5, 1, 1});
        Tap("KEY_28");
        RunMs(1000);
        Tap("KEY_28");
        RunMs(200);
        const std::string loop = Ask({0x3E, 5, 2, 1});
        Check(empty.size() == 24 && empty[2] == 0 && empty[3] == 0 && DevNum(empty, 5, 4) == 0,
              "midi dev: PROBE 1, the looper empty");
        if (loop.size() == 24)
        {
            const uint64_t length = DevNum(loop, 5, 4), pos = DevNum(loop, 9, 4);
            Check(loop[3] == 2 && length > 47000 && length < 51000 && pos > 0 && pos < length
                      && loop[16] == 64 && DevNum(loop, 17, 2) == 3000 && DevNum(loop, 19, 2) == 3000,
                  "midi dev: PROBE 1, the loop playing, its length a second, where it is, at 1x");
        }
        else
            Check(false, "midi dev: PROBE 1 answers 21 bytes");
        // PROBE 0, the clock: none, then 120 BPM into the jack; the effects follow the loop
        const std::string none = Ask({0x3E, 5, 3, 0});
        Check(none.size() == 32 && none[2] == 0 && none[3] == 0 && none[4] == 0 && none[5] == 1
                  && !(none[6] & 1) && (none[6] & 2) && DevNum(none, 21, 3) > 0,
              "midi dev: PROBE 0, no clock (source auto, none locked, x1), the loop's tempo");
        clock_bpm = 120.f;
        clock_next = NowMs();
        RunMs(3000);
        const std::string clock = Ask({0x3E, 5, 4, 0});
        if (clock.size() == 32)
        {
            const double bpm = DevNum(clock, 7, 3) / 100., sent = DevNum(clock, 10, 3) / 100.,
                         fx = DevNum(clock, 21, 3) / 100.;
            Check(clock[4] == 1 && (clock[6] & 1) && fabs(bpm - 120.) < .5 && fabs(sent - 120.) < .5
                      && fabs(fx - DevNum(none, 21, 3) / 100.) < .01 && DevNum(clock, 13, 5) > 100
                      && DevNum(clock, 18, 3) >= 1,
                  "midi dev: PROBE 0, the jack's clock locked at 120 and counted, the effects on the loop's");
        }
        else
            Check(false, "midi dev: PROBE 0 answers 29 bytes");
        Check(Ask({0x3E, 5, 5, 2})[2] == 1, "midi dev: PROBE 2 refused");
    }});

    cases.push_back({"midi-dev-levels", [] {
        RunMs(kReadyMs);
        // the meters start with the first ask, which says so; the next has the peaks since:
        // AUX's sine at .3 in, the outputs
        const std::string first = Ask({0x3E, 6, 1, 1});
        RunMs(200);
        const std::string next = Ask({0x3E, 6, 2, 1});
        Check(first.size() == 22 && first[2] == 0 && first[3] == 1 && DevNum(first, 10, 2) == 0
                  && next.size() == 22 && next[3] == 0,
              "midi dev: LEVELS 1, the peak meters started by the first ask, which says so");
        if (next.size() == 22)
        {
            const double in_l = DevNum(next, 10, 2) / 8192., in_r = DevNum(next, 12, 2) / 8192.;
            Check(fabs(in_l - .3) < .01 && fabs(in_r - .3) < .01 && DevNum(next, 8, 2) == 0
                      && DevNum(next, 18, 2) > 300 && DevNum(next, 14, 2) > 300 && DevNum(next, 4, 2) > 0,
                  "midi dev: LEVELS 1, AUX's peaks .3, the mic's none, the outputs' and the VU meter");
        }
        sine = false;
        RunMs(200);
        Ask({0x3E, 6, 3, 1});
        RunMs(200);
        const std::string quiet = Ask({0x3E, 6, 4, 1});
        Check(quiet.size() == 22 && DevNum(quiet, 10, 2) == 0 && DevNum(quiet, 12, 2) == 0,
              "midi dev: LEVELS 1, the peaks since the last ask: the input silent");
        // LEVELS 0, the effects' meters: the filter held, the reverb off
        sine = true;
        Press("KEY_5", true);
        RunMs(300);
        const std::string fx = Ask({0x3E, 6, 5, 0});
        Press("KEY_5", false);
        Check(fx.size() == 31 && DevNum(fx, 3 + 2 * 4, 2) > 100 && DevNum(fx, 3 + 2 * 11, 2) == 0,
              "midi dev: LEVELS 0, the effects' meters: the filter's lit, the reverb's not");
        Check(Ask({0x3E, 6, 6, 2})[2] == 1, "midi dev: LEVELS 2 refused");
    }});

    cases.push_back({"midi-dev-signal", [] {
        sine = false; // nothing into AUX
        RunMs(kReadyMs);
        const float before = RunMs(300);
        // a sine at 1000 Hz, half scale, in the input's place
        const std::string on = Ask({0x3E, 7, 1, 1, 1000 >> 7, 1000 & 0x7F, 0x40, 0});
        const float sig = RunMs(300);
        Ask({0x3E, 6, 2, 1});
        RunMs(100);
        const std::string peaks = Ask({0x3E, 6, 3, 1});
        Check(before < 1e-4f && on == std::string("\x07\x01\x00\x01", 4) && sig > .05f,
              "midi dev: SIGNAL a sine where the input was silent");
        Check(peaks.size() == 22 && fabs(DevNum(peaks, 10, 2) / 8192. - .5) < .01
                  && fabs(DevNum(peaks, 8, 2) / 8192. - .5) < .01,
              "midi dev: SIGNAL at half scale, in AUX's and the mic's place, as LEVELS shows");
        // a saw and noise sound too; the mic, AUX unplugged, takes it as well
        Ask({0x3E, 7, 4, 2, 0, 110, 0x40, 0});
        const float saw = RunMs(300);
        Ask({0x3E, 7, 5, 3, 0, 1, 0x40, 0});
        const float noise = RunMs(300);
        SetJack(false);
        Usb({kCC, kHpCueCC, 0}); // the mic is never on the master (#152): the headphones' cue
        RunMs(500);
        RunMs(300);
        const float mic = hp_rms;
        SetJack(true);
        Usb({kCC, kHpCueCC, 127});
        RunMs(500);
        Check(saw > .05f && noise > .05f && mic > .01f, "midi dev: SIGNAL's saw and noise, into the mic too");
        // off: the input again (silent)
        const std::string off = Ask({0x3E, 7, 6, 0});
        RunMs(500);
        Check(off == std::string("\x07\x06\x00\x00", 4) && RunMs(300) < 1e-4f,
              "midi dev: SIGNAL 0, the input again");
        Check(Ask({0x3E, 7, 7, 4, 0, 1, 0, 1})[2] == 1 && Ask({0x3E, 7, 8, 1, 0, 0, 0, 1})[2] == 1
                  && Ask({0x3E, 7, 9, 1})[2] == 1,
              "midi dev: SIGNAL refuses no such wave, 0 Hz, or no frequency");
    }});

    cases.push_back({"midi-dev-reseed", [] {
        RunMs(kReadyMs);
        // xorshift32 (FxCommon.h's Rng): the draws from start to state, -1 if not within 1M
        const auto draws = [](uint32_t start, uint32_t state) {
            uint32_t x = start ? start : 1;
            for (int n = 0; n < 1000000; n++)
            {
                if (x == state)
                    return n;
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
            }
            return -1;
        };
        const uint32_t kSeeds[3] = {0x6A09E667u, 0x2545F491u, 0x6C8E9CF5u}; // chaos, slicer, delay
        // and wow & flutter's, an LCG from 1 (FxWarble.h): its state right after a RESEED
        // the chaos key latched: it draws every step, 17 at least
        Ask({0x3E, 3, 1, 0x21, 13, 0, 1});
        const std::string read = Ask({0x3E, 8, 2});
        Check(read.size() == 28 && read[2] == 0 && DevNum(read, 3, 5) == 0
                  && draws(kSeeds[1], static_cast<uint32_t>(DevNum(read, 13, 5))) >= 0
                  && draws(kSeeds[2], static_cast<uint32_t>(DevNum(read, 18, 5))) >= 0,
              "midi dev: RESEED without a seed reads: 0 since power-on, the states on its sequences");
        int n[2] = {-1, -1};
        for (int run = 0; run < 2; run++)
        {
            const std::string set = Ask({0x3E, 8, 3 + run, 0, 0, 0, 12345 >> 7, 12345 & 0x7F});
            const uint32_t seed = static_cast<uint32_t>(DevNum(set, 3, 5));
            const bool fresh = set.size() == 28 && seed == 12345
                               && draws(seed ^ kSeeds[0], static_cast<uint32_t>(DevNum(set, 8, 5))) < 40
                               && DevNum(set, 23, 5) == (12345 ^ 1);
            RunMs(2000);
            const std::string later = Ask({0x3E, 8, 5 + run});
            n[run] = fresh ? draws(seed ^ kSeeds[0], static_cast<uint32_t>(DevNum(later, 8, 5))) : -1;
        }
        Check(n[0] > 100 && n[1] > 100 && abs(n[0] - n[1]) <= 40,
              "midi dev: RESEED 12345 twice: chaos draws on from it each time, as many in as long");
        if (n[0] <= 100 || n[1] <= 100)
            printf("      draws %d, %d\n", n[0], n[1]);
        const std::string zero = Ask({0x3E, 8, 7, 0, 0, 0, 0, 0});
        Check(zero.size() == 28 && draws(kSeeds[2], static_cast<uint32_t>(DevNum(zero, 18, 5))) == 0
                  && draws(kSeeds[0], static_cast<uint32_t>(DevNum(zero, 8, 5))) < 40 && DevNum(zero, 23, 5) == 1,
              "midi dev: RESEED 0, as at power-on, wow & flutter's too");
        Check(Ask({0x3E, 8, 8, 1, 2})[2] == 1, "midi dev: RESEED with a short seed refused");
    }});

    cases.push_back({"midi-dev-battery", [] {
        // on the battery at 3.8 V: HIGH once the level check has read it (a second on)
        SetBattery(3.8f, false);
        RunMs(kReadyMs + 1500);
        const std::string high = Ask({0x3E, 9, 1});
        Check(high.size() == 27 && high[2] == 0 && high[3] == 1 && high[4] == 1 && high[6] == 0
                  && DevNum(high, 7, 2) == 0 && DevNum(high, 9, 2) == 0,
              "midi dev: BATTERY on the battery, above 3V3: HIGH, no cable, not low");
        // the cable in: FULL at once, VIN_GD in its raw status
        SetBattery(3.8f, true);
        RunMs(500);
        const std::string usb = Ask({0x3E, 9, 2});
        Check(usb.size() == 27 && usb[3] == 0 && DevNum(usb, 7, 2) == 0xFF && DevNum(usb, 17, 2) == 0x40,
              "midi dev: BATTERY on the cable: FULL, VIN_GD debounced and raw");
        // pulled at 3.1 V: MEDIUM once read again, a second on
        SetBattery(3.1f, false);
        RunMs(2000);
        const std::string medium = Ask({0x3E, 9, 3});
        Check(medium.size() == 27 && medium[3] == 2 && DevNum(medium, 7, 2) == 0,
              "midi dev: BATTERY pulled at 3.1 V: MEDIUM");
    }});

    // scenes over SysEx and program changes, morphs over CC
    cases.push_back({"midi-scenes", [] {
        RunMs(kReadyMs);
        const float dry = RunMs(300);
        // the blank scene, with the filter latched and closed, into slot 1
        // parts 0-3 page 1's knobs, 4-7 page 2's, 8 the latches
        std::string blank[9];
        bool all = true;
        for (int part = 0; part < 9; part++)
        {
            blank[part] = Ask({0x30, 0, static_cast<uint8_t>(part)});
            all &= blank[part].size() == (part < 8 ? 34u : 5u);
        }
        // the shifter's mix (FX 1, page 2's knob 1: part 4, 2nd effect) on its default, 1
        all &= blank[4].size() == 34u && blank[4][10] == 127 && blank[4][11] == 127;
        Check(all, "midi scenes: 30 answers a scene in 9 parts, page 2 in 4-7");
        if (!all)
            return;
        std::string ack;
        for (int part = 0; part < 9; part++)
        {
            std::string data = blank[part].substr(2);
            if (part == 1)
                data[0] = data[1] = 0; // the filter (FX 4, the 1st in part 1): cutoff 0
            if (part == 8)
                data[2] = 1 << 4;              // latched: the filter
            TakeUsbOut();
            Usb({0xF0, 0x7D, 0x43, 0x48, 0x31, 1, static_cast<uint8_t>(part)});
            for (char c : data)
                UsbMidi(static_cast<uint8_t>(c));
            Usb({0xF7});
            RunMs(20);
            const std::string out = TakeUsbOut();
            ack += out.size() >= 9 ? out.substr(5, 3) : "";
        }
        Check(ack.size() == 27 && ack[26] == 0, "midi scenes: 31 stores one, part by part");
        RunMs(2500);
        Check(SavedLatch(Card("/FRIZZ/frizz_scenes.txt"), 1, "filter") == 1, "midi scenes: on the card");
        Trs({kPC, 1});
        RunMs(500);
        Check(RunMs(300) < dry * .5f, "midi scenes: program change 1 recalls it");
        Trs({kPC, 0});
        RunMs(500);
        Check(fabsf(RunMs(300) - dry) < dry * .05f, "midi scenes: program change 0, the blank scene");
        ToBarLine();
        Trs({kCC, 3, 1}); // morph to it: over a bar at 120 BPM, 2 s
        RunMs(200);
        const float midway = RunMs(100);
        RunMs(3000);
        Check(midway > dry * .5f && RunMs(300) < dry * .5f, "midi scenes: CC 3 morphs to it, landing on the bar");
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x31, 0, 8, 1, 0, 0, 0xF7});
        RunMs(20);
        const std::string refused = TakeUsbOut();
        Check(refused.size() == 9 && refused[7] == 1, "midi scenes: never into the blank scene");

        // CC 9 sets a morph's bars: the same morph 4 bars later (the same phase of the bar,
        // 2 s each at 120 BPM) over 2 bars lands a bar later than over 1
        auto morph_ms = [](int bars) {
            Trs({kPC, 0});
            RunMs(500);
            Trs({kCC, 9, bars, kCC, 3, 127, kCC, 3, 1});
            const uint32_t start = NowMs();
            std::string state;
            do
                state = Ask({0x20}); // 20 ms each
            while (NowMs() - start < 6000 && state.size() > 6 && (state[6] & 2));
            return NowMs() - start;
        };
        const uint32_t t0 = NowMs();
        const uint32_t one = morph_ms(1);
        RunMs(t0 + 8000 - NowMs());
        const uint32_t two = morph_ms(2);
        printf("      morph over 1 bar: %u ms, over 2: %u ms\n", one, two);
        Check(one <= 2100 && two > one + 1900 && two < one + 2100,
              "midi scenes: CC 9 = 2 makes a CC 3 morph land a bar later");
    }});

    // MIDI Start / Stop play and pause the loop, once switched on
    cases.push_back({"midi-transport", [] {
        RunMs(kReadyMs);
        Tap("KEY_28");
        RunMs(2000);
        Tap("KEY_28");
        sine = false;
        RunMs(500);
        Trs({0xFC});
        RunMs(300);
        Check(RunMs(300) > .05f, "midi transport: off at first: Stop doesn't pause the loop");
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x13, 1, 1, 0xF7});
        RunMs(20);
        Trs({0xFC});
        RunMs(300);
        Check(RunMs(300) < .001f, "midi transport: switched on, Stop pauses it");
        Trs({0xFA});
        RunMs(300);
        Check(RunMs(300) > .05f, "midi transport: and Start plays it");
        // kept on the card with the channel, for the next case's power-on
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x13, 0, 1, 0xF7});
        RunMs(2500);
        KeepCard();
    }});

    // the settings across a power cycle, and transport following switched off again
    cases.push_back({"midi-kept", [] {
        TakeCard();
        RunMs(kReadyMs);
        Check(Ask({0x24}) == std::string("\x01\x01\x00\x00", 4),
              "midi kept: channel 1 and transport following are back after power-on");
        Trs({0x90, kFilterNote, 100});
        RunMs(100);
        Check(Max(SmtLedFull(kFilterKeyLed)) > 80, "midi kept: a note on channel 1 holds its key");
        Trs({0x80, kFilterNote, 0});
        Tap("KEY_28");
        RunMs(2000);
        Tap("KEY_28");
        sine = false;
        RunMs(500);
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x13, 1, 0, 0xF7});
        RunMs(20);
        Trs({0xFC});
        RunMs(300);
        Check(RunMs(300) > .05f, "midi kept: transport following off again: Stop doesn't pause");
    }});

    // the controllers midi-cc doesn't: the transport and VOLUME turned, the compressor's other
    // knobs, the input gain, the mix, mono, a morph's bars and its stop, all sound off; and what
    // FRIZZ ignores: the mod wheel, pitch bend, aftertouch
    cases.push_back({"midi-cc-more", [] {
        RunMs(kReadyMs);
        const float dry = RunMs(300);
        auto state = [] { return Ask({0x20}); };
        auto at14 = [](const std::string& d, size_t i) {
            return d.size() > i + 1 ? (static_cast<uint8_t>(d[i]) << 7) | static_cast<uint8_t>(d[i + 1]) : -1;
        };
        // a loop, for the transport
        Tap("KEY_28");
        RunMs(1000);
        Tap("KEY_28");
        RunMs(300);
        const float speed = Probe().loop_speed;
        Trs({kCC, 90, 127});
        RunMs(2000);
        Check(Probe().loop_speed > 1.9f * speed, "midi cc more: CC 90 at 127: the loop at 2x");
        Trs({kCC, 90, 64});
        RunMs(2000);
        Check(fabsf(Probe().loop_speed - speed) < .02f, "midi cc more: and at 64 back to 1x (the speed glides there)");
        Tap("KEY_28"); // erase it
        RunMs(1000);
        Check(Probe().loop_state == 0, "midi cc more: (the loop erased)");
        Trs({kCC, 90, 127});
        RunMs(300);
        Check(fabsf(Probe().loop_speed - 1.f) < .001f, "midi cc more: without a loop, CC 90 does nothing");
        const int out_gain = at14(state(), 15);
        Trs({kCC, 85, 32});
        RunMs(500);
        Check(at14(state(), 15) == 4096 && at14(state(), 15) != out_gain && RunMs(300) < dry * .8f,
              "midi cc more: CC 85 sets the master out: down");

        Trs({kCC, 25, 0, kCC, 26, 64, kCC, 27, 127});
        RunMs(50);
        const std::string comp = Ask({0x21, 14});
        Check(comp.size() == 17 && at14(comp, 3) == 0 && at14(comp, 5) == 8192 && at14(comp, 7) == 16383,
              "midi cc more: CCs 25-27 are the compressor's knobs 2-4");

        const int in_gain = at14(state(), 17);
        Trs({kCC, 86, 0});
        RunMs(300);
        Check(at14(state(), 17) == 0 && RunMs(300) < .001f, "midi cc more: CC 86 sets the input gain: 0 silences it");
        Trs({kCC, 86, 127});
        RunMs(300);
        Check(at14(state(), 17) == 16383 && at14(state(), 17) != in_gain,
              "midi cc more: and 127 turns it all the way up");
        Trs({kCC, 87, 32});
        RunMs(50);
        Check(at14(state(), 13) == 4096, "midi cc more: CC 87 sets the mix");
        Trs({kCC, 60, 127});
        RunMs(50);
        Check(state().size() > 6 && !(state()[6] & 32), "midi cc more: CC 60 doesn't make the input mono any more");

        // CC 9 past 8 is 8 bars: a morph over CC 3 still going after 15 s, landed by 17 s
        Latch("KEY_5");
        Save(1);
        RunMs(1000);
        Tap("KEY_5");
        RunMs(300);
        Trs({kCC, 9, 127, kCC, 3, 1});
        RunMs(15000);
        const bool going = state().size() > 6 && (state()[6] & 2);
        RunMs(2500);
        Check(going && !(state()[6] & 2), "midi cc more: CC 9 above 8 makes a morph take 8 bars");
        // CC 81 stops one where it is
        Tap("KEY_5");
        RunMs(300);
        Trs({kCC, 9, 1, kCC, 3, 127, kCC, 3, 1});
        RunMs(500);
        const bool started = state()[6] & 2;
        Trs({kCC, 81, 127});
        RunMs(50);
        Check(started && !(state()[6] & 2), "midi cc more: CC 81 stops a morph before its bar");
        // as it crosses 64 upward, not again while it stays up
        Tap("KEY_5");
        RunMs(300);
        Trs({kCC, 3, 127, kCC, 3, 1});
        RunMs(300);
        Trs({kCC, 81, 127});
        RunMs(50);
        const bool still = state()[6] & 2;
        Trs({kCC, 81, 0, kCC, 81, 127});
        RunMs(50);
        Check(still && !(state()[6] & 2), "midi cc more: CC 81 at 127 again doesn't stop the next morph; down and up does");

        // CC 82: a tap a crossing of 64 upward, timed as it came: 4 at 120 BPM
        for (int i = 0; i < 4; i++)
        {
            Trs({kCC, 82, 127, kCC, 82, 127}); // the second isn't a crossing
            RunMs(250);
            Trs({kCC, 82, 0});
            RunMs(250);
        }
        const int bpm10 = at14(state(), 21);
        printf("      tapped %.1f BPM\n", bpm10 / 10.);
        Check(abs(bpm10 - 1200) <= 10, "midi cc more: CC 82 taps the tempo: 4 at 500 ms, 120 BPM");

        // CC 120: every key MIDI holds let go, by note and by FRIZZ's SysEx
        Tap("KEY_5"); // the filter off, as the morph left it
        RunMs(300);
        const int off = Max(SmtLedFull(kFilterKeyLed));
        Trs({kNoteOn, kFilterNote, 100});
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x11, 25, 1, 0xF7}); // KEY_12: the delay
        RunMs(200);
        const bool held = Max(SmtLedFull(kFilterKeyLed)) > 2 * off && __builtin_popcount(at14(state(), 10)) == 2;
        Trs({kCC, 120, 0});
        RunMs(300);

        Check(held && Max(SmtLedFull(kFilterKeyLed)) == off && at14(state(), 10) == 0,
              "midi cc more: CC 120 lets go of the keys held by notes and by SysEx");

        // ignored: the mod wheel, pitch bend, channel and key pressure, also in running status
        const std::string before = state(), filter = Ask({0x21, 4});
        Trs({kCC, 1, 127, 0xEF, 0x7F, 0x7F, 0x00, 0x00, 0xDF, 100, 0xAF, kFilterNote, 100});
        RunMs(300);
        Check(state() == before && Ask({0x21, 4}) == filter,
              "midi cc more: the mod wheel, pitch bend and aftertouch change nothing");
        Trs({0xEF, 0x00, 0x40, kCC, kFilterLatchCC, 127});
        RunMs(300);
        Check(Max(SmtLedFull(kFilterKeyLed)) > 2 * off, "midi cc more: a CC right after a pitch bend still works");
    }});

    // a DAW's automation: no harder steps than a hand's turn at the same speed, and a dense
    // stream of controllers lands on its last value
    cases.push_back({"midi-automation", [] {
        RunMs(kReadyMs);
        Latch("KEY_5"); // the filter, on the knobs
        RunMs(300);
        Turn(kKnob1Encoder, -50); // 0.5 down over 400 ms, 1% a detent
        const float by_hand = StepMs(600);
        Trs({kFilterCC, kFilterCutoffCC, 64});
        RunMs(500);
        max_step = 0.f;
        for (int v = 63; v >= 0; v--) // 0.5 down over 400 ms, 1/128 a message
        {
            Trs({kFilterCC, kFilterCutoffCC, v});
            RunMs(6);
        }
        RunMs(200);
        const float by_midi = max_step;
        printf("      largest step: turned %.4f, automated %.4f\n", by_hand, by_midi);
        Check(by_midi <= by_hand * 1.25f, "midi automation: a CC sweep steps no harder than the knob turned as fast");
        // 3000 controllers over USB in 1.5 s, twice a ms, then a last one
        for (int i = 0; i < 1500; i++)
        {
            Usb({kFilterCC, kFilterCutoffCC, (i * 37) % 128, kFilterCC, kFilterCutoffCC + 1, (i * 11) % 128});
            RunMs(1);
        }
        Usb({kFilterCC, kFilterCutoffCC, 100});
        RunMs(100);
        const std::string params = Ask({0x21, 4});
        Check(params.size() == 17 && fabsf(KnobOf(params[1], params[2]) - KnobOf7(100)) < 1e-3f,
              "midi automation: 3000 CCs in 1.5 s, and the cutoff lands on the last one");
    }});

    // the compressor's key again keeps its page 2, as an FX key does (#81)
    cases.push_back({"comp-page2", [] {
        RunMs(kReadyMs);
        auto page = [] {
            const std::string state = Ask({0x20});
            return state.size() > 24 ? state[24] : -1; // the FX knobs' page
        };
        Tap("KEY_15"); // the compressor on the knobs
        RunMs(100);
        Tap("ENC_4_SW"); // its page 2
        RunMs(100);
        Tap("KEY_15");
        RunMs(100);
        Check(page() == 1, "comp-page2: pressing the compressor's key again keeps page 2");
        Tap("KEY_2"); // an FX, and the compressor back: page 1
        RunMs(100);
        Tap("KEY_15");
        RunMs(100);
        Check(page() == 0, "comp-page2: from an FX, the compressor's key shows page 1");
    }});

    // a setting the card can't take: its key on the settings page blinks red 3 times, and
    // again for each retry (#81)
    cases.push_back({"settings-refused", [] {
        SetCardPresent(false);
        RunMs(kReadyMs);
        SetToggle(true);
        RunMs(300);
        TapMono(); // SHIFT + the upper F#: mono
        // pure red rising edges on its LED, and the moments each train starts
        std::vector<int> trains, blinks;
        bool was_red = false;
        int last_red = -1000;
        for (int t = 0; t < 9000; t++)
        {
            RunMs(1);
            const Rgb c = SmtLedFull(kMonoKeyLed);
            const bool red = c.r > 150 && c.g < 40 && c.b < 40;
            if (red && !was_red)
            {
                if (t - last_red > 400)
                {
                    trains.push_back(t);
                    blinks.push_back(0);
                }
                blinks.back()++;
                last_red = t;
            }
            was_red = red;
        }
        printf("      red blinks at");
        for (size_t i = 0; i < trains.size(); i++)
            printf(" %d ms (%d)", trains[i], blinks[i]);
        printf("\n");
        bool threes = !blinks.empty();
        for (int b : blinks)
            threes &= b == 3;
        Check(!trains.empty() && trains[0] >= 1900 && trains[0] < 2400 && threes,
              "settings-refused: without a card, the mono key blinks red 3 times once its write fails");
        Check(trains.size() == 4, "settings-refused: and again for each of 3 retries");
        const Rgb source = SmtLedFull(kMonoKeyLed);
        const std::string state = Ask({0x20});
        Check(source.g > 150 && source.r < 40 && source.b < 40 && state.size() > 6 && (state[6] & 32),
              "settings-refused: then green again (the source, AUX): mono is in force");
        Check(Max(SmtLedFull(kTransportKeyLed)) < 80, "settings-refused: the other keys don't blink");
    }});

    // a setting saved, then the card gone: the compressor's write failing blinks no setting's key
    cases.push_back({"settings-refused-saved", [] {
        RunMs(kReadyMs);
        SetToggle(true);
        RunMs(300);
        TapMono(); // mono, saved
        RunMs(3000);
        Check(Card("/FRIZZ/frizz_master.txt").find("mono 1") != std::string::npos,
              "settings-refused-saved: mono saved");
        SetCardPresent(false);
        // the compressor set over SysEx (kCmdDev's SET), kept as its knob is: a write, which fails
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x3E, 3, 1, 0x20, 0, 20, 0, 0xF7});
        int red = 0, comp_red = 0;
        for (int t = 0; t < 3000; t++) // its first try fails 2 s on
        {
            RunMs(1);
            const Rgb c = SmtLedFull(kMonoKeyLed);
            red += c.r > 150 && c.g < 40 && c.b < 40;
        }
        SetToggle(false);
        for (int t = 0; t < 2000; t++) // and its retry, on the play page
        {
            RunMs(1);
            const Rgb k = SmtLedFull(kCompKeyLed);
            comp_red += k.r > 150 && k.g < 40 && k.b < 40;
        }
        Check(comp_red > 0, "settings-refused-saved: the compressor's write fails (its key blinks red)");
        Check(red == 0, "settings-refused-saved: the mono key, saved before, doesn't blink");
    }});

    // the CHOMPI key lights white while SHIFT is held on the settings page, through the bug
    // report's hold too (#81)
    cases.push_back({"settings-shift", [] {
        RunMs(kReadyMs);
        SetToggle(true);
        RunMs(300);
        Check(Max(PthLedFull(kChompiKeyLed)) == 0, "settings-shift: the CHOMPI key dark at first");
        Press("KEY_26", true);
        RunMs(100);
        const Rgb held = PthLedFull(kChompiKeyLed);
        Check(held.r > 150 && held.r == held.g && held.g == held.b,
              "settings-shift: SHIFT held on the settings page lights it white");
        Press("ENC_6_SW", true); // SHIFT + VOLUME: the bug report's hold
        RunMs(1500);
        const Rgb hold = PthLedFull(kChompiKeyLed);
        Check(hold.r > 150 && hold.r == hold.g && hold.g == hold.b,
              "settings-shift: and through SHIFT + VOLUME's hold");
        Press("ENC_6_SW", false);
        Press("KEY_26", false);
        RunMs(100);
        Check(Max(PthLedFull(kChompiKeyLed)) == 0, "settings-shift: dark again once let go");
        // SHIFT held on the play page through the flip up: still SHIFT, still white
        SetToggle(false);
        RunMs(300);
        Press("KEY_26", true);
        RunMs(100);
        SetToggle(true);
        RunMs(300);
        Check(Max(PthLedFull(kChompiKeyLed)) > 150, "settings-shift: SHIFT held through the flip up stays lit");
        Press("KEY_26", false);
        RunMs(100);
        Check(Max(PthLedFull(kChompiKeyLed)) == 0, "settings-shift: and dark once let go there");
    }});

    // the phaser (#68) on the 11th white key: its notches sweep through a sine at its centre,
    // its key violet, its rate knob's LED in its colours while free and green once synced, its
    // knobs on NRPN 0/64-67 and 1/64-67
    cases.push_back({"phaser", [] {
        static const int kPhaserKeyLed = 18;
        freq = 800.f; // the centre at its default: the middle notch sweeps across it
        RunMs(kReadyMs);
        const float dry = RunMs(300);
        Latch("KEY_7");
        Turn(2, 30); // knob 3 (SW2), depth .3: the notches all in, +-.75 octaves
        RunMs(500);
        float low = 1.f, high = 0.f;
        for (int w = 0; w < 100; w++) // 2 s at .7Hz: the notch crosses the sine twice a cycle
        {
            const float rms = RunMs(20);
            low = fminf(low, rms);
            high = fmaxf(high, rms);
        }
        printf("      dry %.3f; latched, 20 ms windows from %.3f to %.3f\n", dry, low, high);
        Check(low < dry * .3f && high > dry * .8f,
              "phaser: KEY_7 latched sweeps a notch through an 800Hz sine, its centre");
        // lit, violet, whitened by its meter as every key is
        const Rgb key = SmtLedFull(kPhaserKeyLed);
        printf("      its key %02x%02x%02x\n", key.r, key.g, key.b);
        Check(key.b > 200 && key.b > key.r && key.r > key.g, "phaser: its key lit, violet");

        // knob 1, the rate, at .25: free, between violet and orange; synced over CC 14 on its
        // channel, 7: green
        const Rgb free_rate = PthLedFull(kKnob1Led);
        Check(free_rate.r > 120 && free_rate.b > 120 && free_rate.g > 30 && free_rate.g < 120,
              "phaser: its rate free, knob 1's LED between violet and orange");
        Trs({0xB6, 14, 64}); // the centre
        RunMs(100);
        const std::string params = Ask({0x21, 12});
        const Rgb synced = PthLedFull(kKnob1Led);
        Check(params.size() == 17 && params[0] == 12 && params[1] == 64 && params[2] == 0,
              "phaser: CC 14 on channel 7 sets its rate, the centre the first division");
        Check(synced.g > 200 && synced.r < 40 && synced.b < 40, "phaser: synced, knob 1's LED green");
        Trs({0xB6, 14, 127}); // the top: 4 bars
        RunMs(100);
        const Rgb bars4 = PthLedFull(kKnob1Led);
        Check(bars4.r > 200 && bars4.g > 200 && bars4.b < 40, "phaser: 4 bars, knob 1's LED yellow");
        Trs({0xB6, 19, 0, 0xB6, 16, 0}); // page 2's knob 2: the centre to 100Hz; knob 3: depth 0, dry
        RunMs(100);
        const std::string page2 = Ask({0x21, 12});
        Check(page2.size() == 17 && page2[11] == 0 && page2[12] == 0 && page2[5] == 0 && page2[6] == 0,
              "phaser: CC 19 sets its centre (page 2's knob 2), CC 16 its depth");
        RunMs(300);
        const float at0 = RunMs(300);
        printf("      depth 0 over MIDI: %.4f (dry %.4f)\n", at0, dry);
        Check(fabsf(at0 - dry) < dry * .01f, "phaser: at depth 0 it's dry again");

        // off: its key dim; the 14th white key is the chaos key's
        Latch("KEY_7");
        Press("KEY_7", true);
        RunMs(80);
        Press("KEY_7", false);
        RunMs(300);
        const Rgb off = SmtLedFull(kPhaserKeyLed);
        Check(Max(off) < 60 && off.b > off.g, "phaser: off, its key dim violet");
        Latch("KEY_14");
        const std::string st = Ask({0x20});
        Check(st.size() > 9 && ((static_cast<uint8_t>(st[8]) << 7 | static_cast<uint8_t>(st[9])) == 1u << 13),
              "phaser: KEY_14 latches the chaos key (FxId 13), the phaser off");
    }});

    // ======== #117: stereo on page 2, the page's number blinked, Level's LED a VU ========

    // turning the FX knobs' page blinks its number on all four knob LEDs in white, as VOLUME's
    // pages do: twice for page 2, once for page 1; then they're steady, page 2 no longer pulses
    cases.push_back({"knob-page-blink", [] {
        RunMs(kReadyMs);
        Tap("KEY_2"); // the shifter: knob 2 blue on both pages (feedback, stereo at 0)
        RunMs(300);
        auto white = [](const Rgb& c) { return std::min(c.r, std::min(c.g, c.b)) > 200; };
        int all_white = 0;
        // a knob press: the white blinks on knob 2's LED over 600 ms, and how many of the
        // four are white 30 ms in
        auto turn = [&](bool shift) {
            if (shift)
            {
                Press("KEY_26", true);
                RunMs(50);
            }
            Press("ENC_4_SW", true);
            int blinks = 0;
            bool was = false;
            all_white = 0;
            for (int t = 0; t < 600; t++)
            {
                if (t == 60)
                    Press("ENC_4_SW", false);
                RunMs(1);
                if (t == 30)
                    for (int k = 0; k < 4; k++)
                        all_white += white(PthLedFull(kKnob1Led + k));
                const bool w = white(PthLedFull(kKnob1Led + 1));
                blinks += w && !was;
                was = w;
            }
            if (shift)
                Press("KEY_26", false);
            return blinks;
        };
        auto swing = [] {
            const std::vector<int> seen = Watch(true, kKnob1Led + 1, 700);
            return *std::max_element(seen.begin(), seen.end())
                   - *std::min_element(seen.begin(), seen.end());
        };
        const int to2 = turn(false);
        printf("      page 2: %d blinks, %d knobs white\n", to2, all_white);
        Check(to2 == 2 && all_white == 4, "knob-page-blink: page 2 blinks twice, on all four knobs");
        Check(swing() < 10, "knob-page-blink: then page 2's LEDs are steady, no pulse");
        const int to1 = turn(false);
        Check(to1 == 1 && all_white == 4, "knob-page-blink: page 1 blinks once");
        Check(turn(true) == 0, "knob-page-blink: SHIFT + press resets, turns no page and blinks nothing");
        Tap("KEY_15"); // the compressor's page 2 too
        RunMs(300);
        Check(turn(false) == 2, "knob-page-blink: the compressor's page 2 blinks twice too");
    }});

    // page 2's knob 4 (Level) shows the effect's output as a VU meter in VOLUME's colours, as
    // bright as Level turns it; a send's its return, its tail too
    cases.push_back({"fx-vu", [] {
        RunMs(kReadyMs);
        Latch("KEY_2"); // the shifter at 0 semitones: the input through it
        Tap("ENC_4_SW");
        RunMs(800);
        const Rgb on = PthLedFull(kKnob1Led + 3);
        Turn(3, -40); // Level from 0dB (.75) to .35
        RunMs(800);
        const Rgb down = PthLedFull(kKnob1Led + 3);
        Turn(3, 40);
        Press("KEY_2", true); // off again
        RunMs(80);
        Press("KEY_2", false);
        RunMs(800);
        const Rgb off = PthLedFull(kKnob1Led + 3);
        printf("      shifter's VU: on %02x%02x%02x, Level down %02x%02x%02x, off %02x%02x%02x\n",
               on.r, on.g, on.b, down.r, down.g, down.b, off.r, off.g, off.b);
        Check(Max(on) > 120 && on.b < Max(on) / 2, "fx-vu: on page 2, knob 4 is the output's VU, not Level's white");
        Check(Max(down) < Max(on) / 2, "fx-vu: Level turned down, it dims");
        Check(Max(off) < 40, "fx-vu: the effect off: down to the floor");
        Latch("KEY_12"); // the delay, at its defaults (#172)
        Tap("ENC_4_SW");
        RunMs(1500);
        const Rgb echoes = PthLedFull(kKnob1Led + 3);
        Turn(3, -40); // its Level, as an insert's, from 0dB (.75) to .35
        RunMs(800);
        const Rgb quieter = PthLedFull(kKnob1Led + 3);
        Turn(3, 40);
        amp = 0.f; // the input stops: the echoes die away
        RunMs(4000);
        const Rgb gone = PthLedFull(kKnob1Led + 3);
        amp = .3f;
        printf("      delay's return: %02x%02x%02x, Level down %02x%02x%02x, gone %02x%02x%02x\n",
               echoes.r, echoes.g, echoes.b, quieter.r, quieter.g, quieter.b, gone.r, gone.g, gone.b);
        Check(Max(echoes) > 120 && Max(gone) < 40,
              "fx-vu: the delay's knob 4 there (its Level, #172): its return");
        Check(Max(quieter) < Max(echoes) / 2, "fx-vu: the delay's Level turned down, it dims as an insert's");
    }});

    // the freezer's roll (#173): a divider of the length by hand, off, 1/2, 1/4, 1/8, 1/16; a
    // step takes at the next repeat's start, on the capture's 16th grid; off is the whole
    // length again; the knob is the state, so the next capture starts on it
    cases.push_back({"freezer-roll", [] {
        RunMs(kReadyMs);
        freq = 233.f; // no whole number of its cycles in the lengths below
        static uint32_t seed = 7;
        static std::vector<float> e;      // the master out's RMS, block by block
        static std::vector<uint32_t> pos; // and the FX clock's pulse then
        static std::vector<double> ms;    // and the twin's time
        e.clear();
        pos.clear();
        ms.clear();
        // the input's level random block by block, so the repeats' envelope shows their length
        auto run = [](size_t blocks) {
            for (size_t b = 0; b < blocks; b++)
            {
                seed = seed * 1664525u + 1013904223u;
                amp = .05f + .25f * static_cast<float>(seed >> 8) / 16777216.f;
                ms.push_back(BlockMs());
                e.push_back(RunBlocks(1));
                pos.push_back(Probe().position);
            }
        };
        // the shortest of 1/32 to 1/4 bar (125 to 1000 blocks at 120 BPM) the last `over`
        // blocks repeat in
        auto period = [](size_t over) {
            for (size_t p : {125u, 250u, 500u, 1000u})
            {
                float worst = 0.f;
                for (size_t t = e.size() - over; t < e.size(); t++)
                    worst = std::max(worst, fabsf(e[t] - e[t - p]));
                if (worst < 1e-5f)
                    return static_cast<int>(p);
            }
            return -1;
        };
        auto roll = [] {
            const std::string p = Ask({0x21, 0});
            return p.size() == 17 ? KnobOf(p[5], p[6]) : -1.f;
        };
        // turned at block `at` from repeats of `from` blocks to `to`: where the new length took.
        // Both start the same, from the capture's start: the first block that parts from the
        // old repeats, less the shorter length
        auto took = [](size_t at, size_t from, size_t to) {
            for (size_t t = at; t < e.size(); t++)
                if (fabsf(e[t] - e[t - from]) > 1e-5f)
                    return static_cast<long>(t - std::min(from, to));
            return -1L;
        };
        auto on_16th = [](long b) {
            for (long t = b - 2; t <= b + 2; t++)
                if (t > 0 && static_cast<size_t>(t) < pos.size() && pos[t] != pos[t - 1]
                    && pos[t] % 3 == 0)
                    return true;
            return false;
        };
        printf("      the FX's tempo: %.1f BPM\n", Probe().fx_bpm);
        run(100);
        Latch("KEY_1");
        Turn(kKnob1Encoder, -9); // the length from 1 bar to 1/4
        run(600);
        run(3000);
        Check(period(2000) == 1000, "freezer-roll: off, it repeats the length (1/4 bar)");
        struct Step
        {
            int detents;
            float knob;
            int len;
            const char* what;
        };
        const Step steps[] = {{3, .25f, 500, "1/2"}, {3, .5f, 250, "1/4"}, {3, .75f, 125, "1/8"}};
        int was = 1000;
        long last = -1; // where the last step took
        for (const Step& st : steps)
        {
            const size_t at = e.size();
            Turn(2, st.detents);
            run(3000);
            const long b = took(at, static_cast<size_t>(was), static_cast<size_t>(st.len));
            const int len = period(1500);
            // Ask runs the twin on, unrecorded: a stretch recorded after it before the next turn
            const float knob = roll();
            printf("      roll %s: knob %.3f, repeats of %d blocks, from block %ld (turned at %zu, pulse %u)\n",
                   st.what, knob, len, b, at, b >= 0 ? pos[b] : 0u);
            Check(fabsf(knob - st.knob) < .01f && len == st.len,
                  (std::string("freezer-roll: a step right, ") + st.what + " of the length").c_str());
            Check(b >= static_cast<long>(at) && on_16th(b),
                  (std::string("freezer-roll: ") + st.what + " takes at a repeat's start, on the 16th grid").c_str());
            run(1200);
            was = st.len;
            last = b;
        }
        Turn(2, -9); // back to off in one move
        const size_t at = e.size();
        run(4000);
        const long b = took(at, 125, 1000);
        const int len = period(2000);
        // the 1/8 repeats are 1/32 notes, 62.5 ms: off takes at one of their starts
        const double since = b >= 0 && last >= 0 ? ms[b] - ms[last] : -1.;
        const double off_grid = fabs(since - 62.5 * floor(since / 62.5 + .5));
        printf("      roll off: repeats of %d blocks, from block %ld (turned at %zu), %.1f ms after 1/8 took\n",
               len, b, at, since);
        Check(roll() == 0.f && len == 1000 && b >= static_cast<long>(at) && off_grid <= 1.,
              "freezer-roll: back to off, the whole length again, from a 1/8 repeat's start");

        // the knob is the state: still on 1/4, the next capture repeats a quarter from the start
        Turn(2, 6);
        run(200);
        Tap("KEY_1"); // unlatched
        run(1000);
        Latch("KEY_1");
        run(2500);
        const int next = period(1500);
        printf("      the next capture, the roll on %.2f: repeats of %d blocks\n", roll(), next);
        Check(fabsf(roll() - .5f) < .01f && next == 250,
              "freezer-roll: still on 1/4 at the next capture, it repeats 1/4 of the length");
        freq = 220.f;
        amp = .3f;
    }});

    // the sends (#172): heard at once at their defaults, as every effect is: their Level on
    // page 2's knob 4 at 0dB, on the inserts' scale; their Ducking on page 1's knob 4. The MIDI
    // map follows: CC 21 on their channel is Level, CC 17 Ducking
    cases.push_back({"send-level", [] {
        RunMs(kReadyMs);
        // what a send returns after a 300 ms burst of the sine: its echoes or its room alone.
        // Silent after it, so the next burst comes out of silence too: a sine held between
        // them would add its own echoes, in phase on the beat (#167), and measure louder
        auto tail = [] {
            amp = .3f;
            RunMs(300);
            amp = 0.f;
            RunMs(100);
            const float t = RunMs(600);
            RunMs(5000); // rung out
            return t;
        };
        auto param = [](int fx, int i) {
            const std::string p = Ask({0x21, fx});
            return p.size() == 17 ? KnobOf(p[1 + 2 * i], p[2 + 2 * i]) : -1.f;
        };
        amp = 0.f;
        RunMs(500);
        const float silence = RunMs(600);
        Latch("KEY_12"); // the delay, at its defaults
        const float echoes = tail();
        printf("      the delay at its defaults: echoes %.4f (silence %.6f)\n", echoes, silence);
        Check(echoes > .02f && silence < 1e-4f, "send-level: the delay's key at its defaults: its echoes heard");
        Check(fabsf(param(10, 7) - .75f) < 1e-3f && param(10, 3) == 0.f,
              "send-level: the delay's Level on page 2's knob 4 at 0dB, Ducking on page 1's knob 4 off");
        Tap("ENC_4_SW"); // page 2
        RunMs(100);
        Turn(3, -25); // knob 4, Level: 0dB to -12dB
        RunMs(800);
        const float down = tail();
        printf("      Level at -12dB: %.4f (%.1fdB)\n", down, 20.f * log10f(down / echoes));
        Check(fabsf(param(10, 7) - .5f) < .011f && fabsf(20.f * log10f(down / echoes) + 12.f) < 1.f,
              "send-level: page 2's knob 4 is the delay's Level, -12dB");
        Turn(3, -100); // all the way down: muted
        RunMs(800);
        Check(param(10, 7) == 0.f && tail() < 1e-4f, "send-level: Level at the bottom mutes the echoes");
        Latch("KEY_12"); // off again
        RunMs(5000);

        Latch("KEY_13"); // the reverb, at its defaults
        const float room = tail();
        printf("      the reverb at its defaults: its room %.4f\n", room);
        Check(room > .01f && fabsf(param(11, 7) - .75f) < 1e-3f && param(11, 3) == 0.f,
              "send-level: the reverb's key at its defaults: its room heard, Level at 0dB on page 2");
        // MIDI: CC 21 on its channel (13) is Level, CC 17 Ducking
        Trs({0xBC, 21, 32, 0xBC, 17, 100});
        RunMs(100);
        Check(fabsf(param(11, 7) - KnobOf7(32)) < .01f && fabsf(param(11, 3) - KnobOf7(100)) < .01f,
              "send-level: CC 21 on the reverb's channel is its Level, CC 17 its Ducking");
        Latch("KEY_13");
        amp = .3f;
    }});

    // the MIDI map follows the knobs: CC 17 on an effect's channel is page 1's knob 4, its own
    // setting (the shifter's grain, the centre at first), CC 19 page 2's knob 2, stereo
    cases.push_back({"cc-stereo", [] {
        RunMs(kReadyMs);
        auto param = [](int i) {
            const std::string p = Ask({0x21, 1}); // the shifter's knobs
            return p.size() == 17 ? KnobOf(p[1 + 2 * i], p[2 + 2 * i]) : -1.f;
        };
        Check(param(3) == .5f && param(5) == 0.f, "cc-stereo: the shifter's grain on knob 4 (at its centre), stereo on page 2's knob 2");
        Trs({0xB1, 19, 127}); // channel 2, the shifter's
        RunMs(50);
        Check(param(5) == 1.f && param(3) == .5f, "cc-stereo: CC 19 turns stereo");
        Trs({0xB1, 17, 0});
        RunMs(50);
        Check(param(3) == 0.f && param(5) == 1.f, "cc-stereo: CC 17 turns the grain");
    }});

    // ---- the built-in mic (#113): AUX's jack picks the source at power-on and on each change,
    // the settings page's upper F# the other until the next (#177); TAPE's gain and filter ----
    static const int kInputKeyLed = kInSourceKeyLed; // the settings page's upper F#: the source
    static auto green_led = [](const Rgb& c) { return c.g > 150 && c.r < 40 && c.b < 40; };
    static auto magenta_led = [](const Rgb& c) { return c.r > 150 && c.b > 150 && c.g < 40; };
    // what the input carries of AUX's sine on its own, then of the mic's on its own: on the
    // headphones' cue, the input alone, as the mic isn't live unless asked for (#177)
    static auto heard = [](float& aux, float& mic) {
        const float a = amp, m = mic_amp;
        Usb({kCC, kHpCueCC, 0});
        mic_amp = 0.f;
        RunMs(50);
        RunMs(200);
        aux = hp_rms;
        amp = 0.f;
        mic_amp = m;
        RunMs(50);
        RunMs(200);
        mic = hp_rms;
        amp = a;
    };
    static auto is_aux = [](const char* what) {
        float aux, mic;
        heard(aux, mic);
        printf("      %s: AUX %.4f, the mic %.4f\n", what, aux, mic);
        return aux > .03f && mic < aux * .02f;
    };
    static auto is_mic = [](const char* what) {
        float aux, mic;
        heard(aux, mic);
        printf("      %s: AUX %.4f, the mic %.4f\n", what, aux, mic);
        return mic > .03f && aux < mic * .02f;
    };

    cases.push_back({"mic-boot", [] {
        SetJack(false); // no cable in AUX at power-on
        mic_amp = .1f;
        RunMs(kReadyMs);
        Check(is_mic("no cable"), "mic-boot: no cable in AUX at power-on: the mic is the input, AUX unheard");
        SetToggle(true);
        RunMs(300);
        Check(magenta_led(SmtLedFull(kInputKeyLed)), "mic-boot: the settings page's upper F# is magenta");
    }});

    cases.push_back({"mic-jack", [] {
        mic_amp = .1f;
        RunMs(kReadyMs);
        Check(is_aux("a cable"), "mic-jack: a cable in AUX at power-on: AUX is the input, the mic unheard");
        SetToggle(true);
        RunMs(300);
        Check(green_led(SmtLedFull(kInputKeyLed)), "mic-jack: the settings page's upper F# is green");
        SetToggle(false);
        RunMs(300);
        // a bounce shorter than the debouncing (20 ms) changes nothing
        SetJack(false);
        RunMs(10);
        SetJack(true);
        RunMs(100);
        Check(is_aux("a 10 ms bounce"), "mic-jack: the jack out for 10 ms: still AUX");
        SetJack(false);
        RunMs(100);
        Check(is_mic("pulled"), "mic-jack: the cable pulled: the mic");
        SetJack(true);
        RunMs(100);
        Check(is_aux("plugged again"), "mic-jack: plugged in again: AUX");
    }});

    // the upper F# (#177): the source, AUX, the mic, round again, not to the card; the jack
    // picks again at its next change
    static auto white_led = [](const Rgb& c) { return c.r > 150 && c.g > 150 && c.b > 150; };
    cases.push_back({"mic-key", [] {
        mic_amp = .1f;
        RunMs(kReadyMs);
        const std::string card = Card("/FRIZZ/frizz_master.txt");
        SetToggle(true);
        RunMs(300);
        Tap("KEY_23");
        RunMs(100);
        Check(magenta_led(SmtLedFull(kInputKeyLed)) && is_mic("the key once"),
              "mic-key: the upper F# with a cable in: the mic, its key magenta");
        Tap("KEY_23");
        RunMs(100);
        Check(green_led(SmtLedFull(kInputKeyLed)) && is_aux("the key twice"),
              "mic-key: again: AUX, its key green");
        Tap("KEY_23");
        RunMs(3000);
        Check(Card("/FRIZZ/frizz_master.txt") == card, "mic-key: the source isn't saved");
        // the jack's next change decides again: plugged, AUX; pulled, the mic
        SetJack(false);
        RunMs(100);
        SetJack(true);
        RunMs(100);
        Check(green_led(SmtLedFull(kInputKeyLed)) && is_aux("the key, then pulled and plugged"),
              "mic-key: the mic by the key, then the cable pulled and plugged in again: AUX");
        // on the play page the key is SAVE again, not the source
        SetToggle(false);
        RunMs(300);
        Tap(kSaveKey);
        RunMs(100);
        Tap(kSaveKey);
        RunMs(300);
        SetToggle(true);
        RunMs(300);
        Check(green_led(SmtLedFull(kInputKeyLed)), "mic-key: on the play page the upper F# doesn't switch the source");
    }});

    cases.push_back({"mic-loop", [] {
        SetJack(false);
        amp = 0.f; // nothing into AUX
        mic_amp = .1f;
        RunMs(kReadyMs);
        Tap("KEY_28"); // record
        RunMs(2000);
        Tap("KEY_28"); // play
        RunMs(500);
        mic_amp = 0.f;
        RunMs(300);
        const float loop = RunMs(500);
        SetJack(true);
        RunMs(300);
        const float aux_loop = RunMs(500);
        printf("      the loop recorded from the mic: %.4f, with a cable in AUX %.4f\n", loop, aux_loop);
        Check(Probe().loop_state == 2 && loop > .03f,
              "mic-loop: the looper records the mic, and plays it once it's silent");
        Check(fabsf(aux_loop - loop) < loop * .1f, "mic-loop: and plays it on with AUX the input again");
    }});

    cases.push_back({"mic-level", [] {
        SetJack(false);
        amp = 0.f;
        mic_amp = .05f;
        RunMs(kReadyMs);
        // the mic isn't live out of the box (#177): on the headphones' cue, the input alone
        Usb({kCC, kHpCueCC, 0});
        RunMs(100);
        const float on_master = RunMs(500);
        const float m1k = hp_rms;
        Check(on_master < 1e-4f && m1k > .01f, "mic-level: the mic on the headphones' cue, not on the master unless live");
        mic_freq = 50.f;
        RunMs(100);
        RunMs(500);
        const float m50 = hp_rms;
        // above it nothing: TAPE's notches (4 kHz one of them) are left out (#127), so a quiet
        // tone at 4 kHz is as loud as one beside it
        mic_amp = .0005f;
        mic_freq = 3600.f;
        RunMs(100);
        RunMs(500);
        const float q3600 = hp_rms;
        mic_freq = 4000.f;
        RunMs(300);
        RunMs(500);
        const float q4k = hp_rms;
        mic_freq = 1000.f;
        mic_amp = .05f;
        printf("      the mic at 1 kHz %.4f, 50 Hz %.4f; quietly at 3.6 kHz %.6f, 4 kHz %.6f\n", m1k, m50,
               q3600, q4k);
        Check(m50 < m1k * .3f, "mic-level: 50 Hz well below 1 kHz (the highpass at 150 Hz)");
        Check(fabsf(q4k - q3600) < q3600 * .05f, "mic-level: a quiet tone at 4 kHz as loud as one at 3.6 kHz: no notch (#127)");
        // as loud on both sides, and the mono input changes nothing
        float l2 = 0.f, r2 = 0.f;
        {
            float in[kBlockSize * kChannels] = {}, out[kBlockSize * kChannels];
            for (int b = 0; b < 400; b++)
            {
                for (size_t i = 0; i < kBlockSize; i++)
                {
                    in[i * kChannels] = mic_amp * sinf(mic_phase);
                    mic_phase = fmodf(mic_phase + 2.f * float(M_PI) * mic_freq / kSampleRate, 2.f * float(M_PI));
                }
                Run(1, in, out);
                for (size_t i = 0; i < kBlockSize; i++)
                {
                    l2 += out[i * kChannels] * out[i * kChannels];
                    r2 += out[i * kChannels + 1] * out[i * kChannels + 1];
                }
            }
        }
        Check(fabsf(l2 - r2) < l2 * .001f, "mic-level: the mic on both sides alike");
        SetToggle(true);
        RunMs(300);
        TapMono();
        RunMs(300);
        SetToggle(false);
        RunMs(300);
        RunMs(500);
        const float mono = hp_rms;
        Check(fabsf(mono - m1k) < m1k * .02f, "mic-level: the mono input doesn't change the mic");
        // TAPE's gains: the mic x5, AUX x3 (and both after the input gain)
        mic_amp = 0.f;
        amp = .05f;
        freq = 1000.f;
        SetJack(true);
        RunMs(300);
        RunMs(500);
        const float a1k = hp_rms;
        printf("      AUX at 1 kHz %.4f: the mic %.2f times as loud\n", a1k, m1k / a1k);
        Check(m1k / a1k > 1.4f && m1k / a1k < 1.95f, "mic-level: the mic about 5/3 as loud as AUX at the same level");
        // one input gain per source (#152): turned with AUX heard, the mic's stays
        Turn(kVolumeEncoder, 0);
        Tap("ENC_6_SW"); // VOLUME's page 2: the input gain
        RunMs(300);
        Turn(kVolumeEncoder, -20); // 75 % to 35 %
        RunMs(500);
        RunMs(500);
        const float a_low = hp_rms;
        amp = 0.f;
        mic_amp = .05f;
        SetJack(false);
        RunMs(300);
        RunMs(500);
        const float m_same = hp_rms;
        printf("      AUX at 35 %% input gain %.4f; the mic then %.4f\n", a_low, m_same);
        Check(a_low < a1k * .6f && a_low > a1k * .3f && fabsf(m_same - m1k) < m1k * .02f,
              "mic-level: the input gain turned with AUX heard turns AUX down, not the mic");
        Turn(kVolumeEncoder, -20); // the mic's, 75 % to 35 %
        RunMs(500);
        RunMs(500);
        const float m_low = hp_rms;
        amp = .05f;
        mic_amp = 0.f;
        SetJack(true);
        RunMs(300);
        RunMs(500);
        printf("      the mic at 35 %% input gain %.4f; AUX then %.4f\n", m_low, hp_rms);
        Check(m_low < m1k * .6f && m_low > m1k * .3f && fabsf(hp_rms - a_low) < a_low * .02f,
              "mic-level: turned with the mic heard, the mic's own, AUX's as it was");
        RunMs(3000);
        const std::string master = Card("/FRIZZ/frizz_master.txt");
        Check(master.find("\ngain_aux 35\n") != std::string::npos && master.find("\ngain_mic 35\n") != std::string::npos,
              "mic-level: both gains go to the card");
    }});

    cases.push_back({"mic-click", [] {
        mic_amp = 0.f;
        RunMs(kReadyMs);
        const float steady = StepMs(300);
        SetJack(false); // AUX's sine to the silent mic
        const float to_mic = StepMs(300);
        mic_amp = .1f;
        RunMs(300);
        const float mic_steady = StepMs(300);
        SetJack(true);
        const float to_aux = StepMs(300);
        printf("      largest steps: AUX %.4f, to the mic %.4f, the mic %.4f, back to AUX %.4f\n", steady,
               to_mic, mic_steady, to_aux);
        Check(to_mic < steady * 1.5f && to_aux < std::max(steady, mic_steady) * 1.5f,
              "mic-click: switching between AUX and the mic crossfades, no click");
    }});

    // ---- the routing (#152): AUX dry past the effects and the compressor, CC 84, the mix's
    // automatism, VOLUME on the settings page, the input's meter ----
    static const int kSourceCC = 84, kInGainCC = 86, kDryCC = 116, kLiveCC = 117;
    static auto mix_now = [] {
        const std::string st = Ask({0x20});
        return st.size() > 14 ? KnobOf(st[13], st[14]) : -1.f;
    };
    cases.push_back({"aux-dry", [] {
        RunMs(kReadyMs);
        const std::string card = Card("/FRIZZ/frizz_master.txt");
        const float fx = RunMs(500);
        const float steady = StepMs(300);
        Usb({kCC, kDryCC, 127}); // AUX dry
        const float to_dry = StepMs(300);
        const float dry = RunMs(500);
        Usb({kCC, 24, 127}); // the compressor's threshold all the way down
        Usb({kCC, 25, 127}); // its ratio all the way up
        RunMs(1000);
        const float dry_comp = RunMs(500);
        Latch("KEY_11"); // the tape stop: silence for what goes through the effects
        RunMs(3000);
        const float dry_stopped = RunMs(500);
        Usb({kCC, kDryCC, 0}); // through the effects again
        RunMs(500);
        const float fx_stopped = RunMs(500);
        Latch("KEY_11");
        RunMs(3000);
        const float fx_comp = RunMs(500);
        printf("      AUX through the effects %.4f, dry %.4f; the compressor hard: dry %.4f, through %.4f; "
               "the tape stop: dry %.4f, through %.4f; steps %.4f, to dry %.4f\n", fx, dry, dry_comp, fx_comp,
               dry_stopped, fx_stopped, steady, to_dry);
        Check(dry > .01f && fabsf(dry_comp - dry) < dry * .001f && fx_comp < fx * .7f,
              "aux-dry: the compressor works on AUX through the effects, not on AUX dry");
        Check(fabsf(dry_stopped - dry) < dry * .001f && fx_stopped < dry * .01f,
              "aux-dry: nor do the effects: the tape stop silences AUX through them only");
        Check(to_dry < steady * 1.5f, "aux-dry: switching to AUX dry crossfades, no click");
        // VOLUME and the mix still have it
        Usb({kCC, kDryCC, 64}); // AUX dry, the bottom of its range
        Usb({kCC, 85, 0});         // the output gain to 0
        RunMs(500);
        const float muted = RunMs(300);
        Usb({kCC, 85, 96});
        Usb({kCC, 87, 127}); // the mix to the loop alone
        RunMs(500);
        const float loop_only = RunMs(300);
        Usb({kCC, 87, 64});
        RunMs(500);
        const float half = RunMs(300);
        printf("      AUX dry: VOLUME at 0 %.5f; the mix at the loop %.5f, in the middle %.4f\n", muted,
               loop_only, half);
        Check(muted < 1e-4f, "aux-dry: VOLUME turns it down");
        Check(loop_only < 1e-4f && half > dry * .6f && half < dry * .8f,
              "aux-dry: the mix's input share scales it, equal-power");
        // CC 116 and 86 aren't kept on the card (#97), also when the hand saves something else
        Usb({kCC, kInGainCC, 32});
        RunMs(3000);
        Check(Card("/FRIZZ/frizz_master.txt") == card, "aux-dry: CC 116 leaves the card as it was");
        SetToggle(true);
        RunMs(300);
        TapMono(); // mono, by hand: the settings go to the card
        RunMs(3000);
        const std::string saved = Card("/FRIZZ/frizz_master.txt");
        Check(saved.find("\nmono 1\n") != std::string::npos && saved.find("\naux_dry 0\n") != std::string::npos
                  && saved.find("\ngain_aux 75\n") != std::string::npos,
              "aux-dry: a setting saved by hand after them: AUX's variant and gain as the hand left them");
    }});

    // the mix's automatism, the same with AUX dry: a recording's close goes to the loop alone,
    // its erase back to the input alone
    cases.push_back({"aux-dry-loop", [] {
        RunMs(kReadyMs);
        Usb({kCC, kDryCC, 127});
        RunMs(300);
        const float live = RunMs(300);
        Tap("KEY_28"); // record
        RunMs(1000);
        Tap("KEY_28"); // and play
        RunMs(700);
        const float mix_loop = mix_now();
        const float both = RunMs(300);
        amp = 0.f;
        RunMs(300);
        const float loop = RunMs(500);
        Tap("KEY_28"); // erase
        RunMs(500);
        const float mix_erased = mix_now();
        amp = .3f;
        RunMs(300);
        const float again = RunMs(300);
        printf("      AUX dry %.4f; recorded: mix %.2f, %.4f, the loop alone %.4f; erased: mix %.2f, %.4f\n",
               live, mix_loop, both, loop, mix_erased, again);
        Check(mix_loop == 1.f && loop > .01f && fabsf(both - loop) < loop * .2f,
              "aux-dry-loop: a recording closes on the loop alone: AUX dry silent, not heard twice");
        Check(mix_erased == 0.f && fabsf(again - live) < live * .01f,
              "aux-dry-loop: an erase goes back to the input alone, AUX dry");
    }});

    // ---- the input per source (#177): the settings page's upper F# the source, G# its
    // effects (through / dry, dark while not live), A# it live; each source's own, saved ----
    static auto key_lit = [](int led) { return Max(SmtLedFull(led)) > 150; };
    static auto key_dim = [](int led) { const int m = Max(SmtLedFull(led)); return m > 0 && m < 80; };
    static auto key_dark = [](int led) { return Max(SmtLedFull(led)) == 0; };
    // the master's level of the source alone, and whether the effects have it: the tape stop
    // latched silences what goes through them
    struct Heard
    {
        float master, stopped, phones;
    };
    static auto heard_on_master = [] {
        Heard h;
        SetToggle(false); // the play page, for the tape stop's key
        RunMs(300);
        h.master = RunMs(300);
        Usb({kCC, kHpCueCC, 0}); // the headphones: the input alone
        RunMs(300);
        RunMs(300);
        h.phones = hp_rms;
        Usb({kCC, kHpCueCC, 127});
        Latch("KEY_11");
        RunMs(3000);
        h.stopped = RunMs(300);
        Latch("KEY_11");
        RunMs(3000);
        SetToggle(true);
        RunMs(300);
        return h;
    };
    // every combination of the table in MANUAL.md, the mix at the input: AUX through, dry,
    // off; then the mic, off as it starts, through, dry
    cases.push_back({"input-combos", [] {
        mic_amp = 0.f;
        RunMs(kReadyMs);
        SetToggle(true);
        RunMs(300);
        const bool aux_through_keys = green_led(SmtLedFull(kInFxKeyLed)) && key_lit(kLiveKeyLed)
                                      && green_led(SmtLedFull(kInSourceKeyLed));
        const Heard aux_through = heard_on_master();
        Tap("KEY_24"); // dry
        RunMs(100);
        const bool aux_dry_keys = white_led(SmtLedFull(kInFxKeyLed)) && key_lit(kLiveKeyLed);
        const Heard aux_dry = heard_on_master();
        Tap("KEY_25"); // off
        RunMs(100);
        const bool aux_off_keys = key_dark(kInFxKeyLed) && key_dim(kLiveKeyLed);
        const Heard aux_off = heard_on_master();
        printf("      AUX through: %.4f, the tape stop %.4f, headphones %.4f; dry: %.4f, %.4f, %.4f; "
               "off: %.4f, %.4f, %.4f\n", aux_through.master, aux_through.stopped, aux_through.phones,
               aux_dry.master, aux_dry.stopped, aux_dry.phones, aux_off.master, aux_off.stopped,
               aux_off.phones);
        Check(aux_through_keys && aux_through.master > .01f && aux_through.stopped < aux_through.master * .01f
                  && aux_through.phones > .01f,
              "input-combos: AUX through the effects, live (F# green, G# green, A# lit): through the effects");
        Check(aux_dry_keys && fabsf(aux_dry.stopped - aux_dry.master) < aux_dry.master * .001f
                  && aux_dry.master > .01f && aux_dry.phones > .01f,
              "input-combos: AUX dry, live (G# white): past the effects");
        Check(aux_off_keys && aux_off.master < 1e-5f && aux_off.stopped < 1e-5f && aux_off.phones > .01f,
              "input-combos: AUX not live (G# dark, A# dim): silent on the master, on the headphones' input feed");

        amp = 0.f;
        mic_amp = .05f;
        Tap("KEY_23"); // the mic
        RunMs(100);
        const bool mic_off_keys = magenta_led(SmtLedFull(kInSourceKeyLed)) && key_dark(kInFxKeyLed)
                                  && key_dim(kLiveKeyLed);
        const Heard mic_off = heard_on_master();
        Tap("KEY_25"); // live
        RunMs(100);
        const bool mic_through_keys = green_led(SmtLedFull(kInFxKeyLed)) && key_lit(kLiveKeyLed);
        const Heard mic_through = heard_on_master();
        Tap("KEY_24"); // dry
        RunMs(100);
        const bool mic_dry_keys = white_led(SmtLedFull(kInFxKeyLed)) && key_lit(kLiveKeyLed);
        const Heard mic_dry = heard_on_master();
        printf("      the mic off: %.4f, %.4f, headphones %.4f; through: %.4f, %.4f, %.4f; dry: %.4f, %.4f, %.4f\n",
               mic_off.master, mic_off.stopped, mic_off.phones, mic_through.master, mic_through.stopped,
               mic_through.phones, mic_dry.master, mic_dry.stopped, mic_dry.phones);
        Check(mic_off_keys && mic_off.master < 1e-5f && mic_off.phones > .01f,
              "input-combos: the mic as it starts, not live (F# magenta, G# dark, A# dim): on the headphones' input feed only");
        Check(mic_through_keys && mic_through.master > .01f && mic_through.stopped < mic_through.master * .01f,
              "input-combos: the mic live, through the effects: the tape stop has it");
        Check(mic_dry_keys && mic_dry.master > .01f && fabsf(mic_dry.stopped - mic_dry.master) < mic_dry.master * .001f,
              "input-combos: the mic live, dry: past the effects");
    }});

    // each source keeps its own G# and A#: the jack (or F#) brings up the other's; all four
    // saved, the source not
    cases.push_back({"input-per-source", [] {
        RunMs(kReadyMs);
        SetToggle(true);
        RunMs(300);
        Tap("KEY_24"); // AUX dry
        RunMs(100);
        Tap("KEY_25"); // AUX off
        RunMs(100);
        SetJack(false); // the mic: its own, through and off
        RunMs(100);
        const bool mic_own = magenta_led(SmtLedFull(kInSourceKeyLed)) && key_dark(kInFxKeyLed)
                             && key_dim(kLiveKeyLed);
        Tap("KEY_25"); // the mic live
        RunMs(100);
        const bool mic_live = green_led(SmtLedFull(kInFxKeyLed)) && key_lit(kLiveKeyLed);
        SetJack(true); // AUX: dry and off, as left
        RunMs(100);
        const bool aux_kept = green_led(SmtLedFull(kInSourceKeyLed)) && key_dark(kInFxKeyLed)
                              && key_dim(kLiveKeyLed);
        Tap("KEY_25"); // AUX live again: dry, as left
        RunMs(100);
        const bool aux_dry = white_led(SmtLedFull(kInFxKeyLed)) && key_lit(kLiveKeyLed);
        Tap("KEY_23"); // F#: the mic, live through the effects, as left
        RunMs(100);
        const bool mic_kept = magenta_led(SmtLedFull(kInSourceKeyLed)) && green_led(SmtLedFull(kInFxKeyLed))
                              && key_lit(kLiveKeyLed);
        RunMs(3000);
        const std::string card = Card("/FRIZZ/frizz_master.txt");
        Check(mic_own, "input-per-source: AUX dry and off, the jack pulled: the mic's own, through the effects, not live");
        Check(mic_live, "input-per-source: the mic's A#: live, its G# lit green");
        Check(aux_kept, "input-per-source: the cable back: AUX's as it was left, off, G# dark");
        Check(aux_dry, "input-per-source: AUX live again: dry, as it was left");
        Check(mic_kept, "input-per-source: F# to the mic: live through the effects, as it was left");
        Check(card.find("\naux_dry 1\n") != std::string::npos && card.find("\nmic_dry 0\n") != std::string::npos
                  && card.find("\naux_live 1\n") != std::string::npos
                  && card.find("\nmic_live 1\n") != std::string::npos,
              "input-per-source: all four on the card");
    }});

    // ... and come back from the card at power-on, the jack picking the source
    cases.push_back({"input-per-source-boot", [] {
        CardFiles()["/FRIZZ/frizz_master.txt"] = "FRIZZ master 1\naux_dry 1\nmic_dry 1\naux_live 0\nmic_live 1\n";
        SetJack(false);
        RunMs(kReadyMs);
        SetToggle(true);
        RunMs(300);
        const bool mic = magenta_led(SmtLedFull(kInSourceKeyLed)) && white_led(SmtLedFull(kInFxKeyLed))
                         && key_lit(kLiveKeyLed);
        SetJack(true);
        RunMs(100);
        const bool aux = green_led(SmtLedFull(kInSourceKeyLed)) && key_dark(kInFxKeyLed) && key_dim(kLiveKeyLed);
        Check(mic && aux, "input-per-source-boot: from the card: the mic dry and live, AUX off");
    }});

    // the upper G# while dark (not live): it changes nothing and blinks red 3 times
    cases.push_back({"input-dark-fx", [] {
        RunMs(kReadyMs);
        SetToggle(true);
        RunMs(300);
        Tap("KEY_25"); // AUX off: G# dark
        RunMs(3000);
        const std::string card = Card("/FRIZZ/frizz_master.txt");
        Tap("KEY_24");
        int blinks = 0;
        bool was_red = false;
        for (int t = 0; t < 2000; t++)
        {
            RunMs(1);
            const Rgb c = SmtLedFull(kInFxKeyLed);
            const bool red = c.r > 150 && c.g < 40 && c.b < 40;
            blinks += red && !was_red;
            was_red = red;
        }
        const bool dark = key_dark(kInFxKeyLed);
        RunMs(2000);
        Tap("KEY_25"); // live again: through, as it was
        RunMs(100);
        Check(blinks == 3 && dark, "input-dark-fx: G# dark pressed: 3 red blinks, then dark again");
        Check(Card("/FRIZZ/frizz_master.txt") == card && green_led(SmtLedFull(kInFxKeyLed)),
              "input-dark-fx: and it changed nothing: through the effects when live again, the card as it was");
    }});

    // #168: the mic not live leaks nothing onto the master while the jack switches the source
    // back and forth, whether AUX goes through the effects or dry (a single gate on the
    // crossfaded input let up to a quarter of it through)
    cases.push_back({"mic-leak", [] {
        amp = 0.f; // AUX silent: only the mic could reach the master
        mic_amp = .3f;
        RunMs(kReadyMs);
        float peaks[2];
        for (int dry = 0; dry < 2; dry++)
        {
            if (dry)
                Usb({kCC, kDryCC, 127});
            RunMs(300);
            out_peak = 0.f;
            for (int k = 0; k < 6; k++)
            {
                SetJack(k % 2 == 1); // pulled, plugged, ...
                RunMs(100);
            }
            peaks[dry] = out_peak;
        }
        printf("      the master's peak while the jack switches, the mic not live: AUX through %g, dry %g\n",
               peaks[0], peaks[1]);
        Check(peaks[0] == 0.f, "mic-leak: AUX through the effects: nothing of the mic on the master, ever");
        Check(peaks[1] == 0.f, "mic-leak: AUX dry: nothing of the mic on the master, ever");
    }});

    // VOLUME on the settings page (#152): page 1 the output, page 2 the input gain, no page 3;
    // its LED back to the battery 2 s after; SHIFT + turn nothing
    cases.push_back({"settings-volume", [] {
        RunMs(kReadyMs);
        auto state = [](int at) {
            const std::string st = Ask({0x20});
            return st.size() > static_cast<size_t>(at + 1) ? KnobOf(st[at], st[at + 1]) : -1.f;
        };
        auto page = [] {
            const std::string st = Ask({0x20});
            return st.size() > 12 ? st[12] : -1;
        };
        SetToggle(true);
        RunMs(300);
        const Rgb battery = PthLedFull(kVolumeLed);
        Turn(kVolumeEncoder, -10); // page 1: the output, 75 % to 55 %
        RunMs(300);
        const Rgb shown = PthLedFull(kVolumeLed);
        Check(fabsf(state(15) - .55f) < .01f && fabsf(state(17) - .75f) < .01f,
              "settings-volume: VOLUME turned on the settings page: page 1, the output gain");
        RunMs(2200);
        Check(!Same(shown, battery) && Same(PthLedFull(kVolumeLed), battery),
              "settings-volume: its LED shows the page, then the battery again after 2 s");
        Tap("ENC_6_SW");
        RunMs(1500);
        Turn(kVolumeEncoder, -10);
        RunMs(300);
        Check(page() == 1 && fabsf(state(17) - .55f) < .01f && fabsf(state(15) - .55f) < .01f,
              "settings-volume: a press, page 2: the input gain");
        RunMs(3000);
        Check(Card("/FRIZZ/frizz_master.txt").find("\ngain_aux 55\n") != std::string::npos,
              "settings-volume: the input gain goes to the card");
        Press("KEY_26", true); // SHIFT
        RunMs(100);
        Turn(kVolumeEncoder, 10);
        RunMs(300);
        Press("KEY_26", false);
        RunMs(100);
        Check(fabsf(state(17) - .55f) < .01f && mix_now() == 0.f, "settings-volume: SHIFT + turn does nothing there");
        Press("KEY_26", true); // SHIFT + press: the bug report's hold, let go before it
        RunMs(100);
        Tap("ENC_6_SW");
        RunMs(100);
        Press("KEY_26", false);
        RunMs(1500);
        Check(page() == 1 && mix_now() == 0.f, "settings-volume: SHIFT + press turns no page there");
        Tap("ENC_6_SW");
        RunMs(1500);
        Check(page() == 0, "settings-volume: a press again: page 1");
        // from the play page's page 3 (the headphones): the settings page turns page 1, a press page 2
        SetToggle(false);
        RunMs(300);
        Tap("ENC_6_SW");
        RunMs(1500);
        Tap("ENC_6_SW");
        RunMs(1500);
        SetToggle(true);
        RunMs(300);
        Turn(kVolumeEncoder, 5);
        RunMs(300);
        Check(page() == 2 && fabsf(state(15) - .65f) < .01f && state(19) == 0.f,
              "settings-volume: from the headphones' page 3: a turn sets the output");
        Tap("ENC_6_SW");
        RunMs(1500);
        Check(page() == 1, "settings-volume: and a press goes to page 2");
    }});

    // VOLUME's page 2: the input's meter (#152), red where the loop's soft limit takes it, the
    // gain's colour for 1 s after a turn
    cases.push_back({"input-vu", [] {
        RunMs(kReadyMs);
        Tap("ENC_6_SW");
        amp = .05f; // x2.25: -19 dB under the loop's ceiling
        RunMs(1500);
        const Rgb quiet = PthLedFull(kVolumeLed);
        amp = .38f; // under it by 1 dB
        RunMs(500);
        const Rgb near = PthLedFull(kVolumeLed);
        amp = .6f; // over it
        RunMs(200);
        const Rgb hot = PthLedFull(kVolumeLed);
        amp = 0.f;
        RunMs(1000);
        const Rgb silent = PthLedFull(kVolumeLed);
        Turn(kVolumeEncoder, 1);
        RunMs(200);
        const Rgb turned = PthLedFull(kVolumeLed);
        RunMs(1200);
        const Rgb after = PthLedFull(kVolumeLed);
        printf("      quiet %d %d %d, near %d %d %d, hot %d %d %d, silent %d %d %d, turned %d %d %d\n", quiet.r,
               quiet.g, quiet.b, near.r, near.g, near.b, hot.r, hot.g, hot.b, silent.r, silent.g, silent.b,
               turned.r, turned.g, turned.b);
        Check(quiet.g > 100 && quiet.r < quiet.g && quiet.b < 40, "input-vu: a quiet input: green");
        Check(near.r > 150 && near.g > 150 && near.b < 40, "input-vu: just under the loop's ceiling: yellow");
        Check(hot.r > 150 && hot.g < 40 && hot.b < 40, "input-vu: over it: red");
        Check(Max(silent) < 40, "input-vu: silence: dim");
        Check(turned.b > 40 && Max(after) < 40, "input-vu: a turn shows the gain for a second, then the meter");
    }});

    // MIDI feedback (#109): the upper F#, G# and A# send CC 84, 116 and 117 (#177); what came
    // isn't sent back
    cases.push_back({"source-feedback", [] {
        RunMs(kReadyMs);
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x13, 3, 2, 0xF7}); // MIDI out: the jack and USB
        SetToggle(true);
        RunMs(300);
        Tap("KEY_22"); // feedback on
        RunMs(300);
        TrsCcs();
        UsbCcs();
        Tap("KEY_24"); // AUX dry
        RunMs(300);
        Check(HasCc(TrsCcs(), 0xBF, kDryCC, 127) && HasCc(UsbCcs(), 0xBF, kDryCC, 127),
              "source-feedback: the upper G# to AUX dry sends CC 116 at 127");
        Tap("KEY_25"); // AUX not live
        RunMs(300);
        Check(HasCc(UsbCcs(), 0xBF, kLiveCC, 0), "source-feedback: the upper A# AUX off sends CC 117 at 0");
        Tap("KEY_23"); // the mic: its own, through and not live
        RunMs(300);
        const auto ccs = UsbCcs();
        Check(HasCc(ccs, 0xBF, kSourceCC, 127) && HasCc(ccs, 0xBF, kDryCC, 0) && !HasCc(ccs, 0xBF, kLiveCC, 127),
              "source-feedback: the upper F# to the mic sends CC 84 at 127 and the mic's CC 116 at 0");
        Usb({kCC, kSourceCC, 0}); // AUX
        RunMs(300);
        const auto back = UsbCcs(), trs = TrsCcs();
        printf("      after CC 84 at 0: %zu CCs back over USB, %zu over TRS\n", back.size(), trs.size());
        Check(!HasCc(back, 0xBF, kSourceCC, 0) && !HasCc(trs, 0xBF, kSourceCC, 0) && HasCc(back, 0xBF, kDryCC, 127)
                  && green_led(SmtLedFull(kInputKeyLed)),
              "source-feedback: CC 84 picks AUX, and isn't sent back; AUX's own CC 116 (dry) is");
        SetJack(false); // the jack: the mic
        RunMs(300);
        Check(HasCc(UsbCcs(), 0xBF, kSourceCC, 127), "source-feedback: the jack's mic sends it too");
    }});

    // a bug report knows AUX's jack: as it was at power-on, and each change; its replay plays
    // them, so it ends on the input the session had
    cases.push_back({"mic-bug-log", [] {
        SetJack(false);
        RunMs(kReadyMs);
        SetJack(true);
        RunMs(500);
        SetJack(false);
        RunMs(500);
        SetToggle(true);
        RunMs(300);
        Tap("KEY_23"); // AUX, by hand
        RunMs(300);
        Press("KEY_26", true);
        RunMs(100);
        Press("ENC_6_SW", true);
        RunMs(2500);
        Press("ENC_6_SW", false);
        Press("KEY_26", false);
        RunMs(1000);
        const std::string log = Card("/FRIZZ/bug-1.txt");
        const size_t booted = log.find("\nbooted\n"), in = log.find("\njack in\n", booted),
                     out = log.find("\njack out\n", booted);
        Check(booted != std::string::npos && log.rfind("\njack out\n", booted) != std::string::npos
                  && log.find("\nmic sine ") < booted,
              "mic-bug-log: no cable at power-on: the report's head says jack out, and plays a tone into the mic");
        Check(in != std::string::npos && out != std::string::npos && in < out,
              "mic-bug-log: the cable in, then out, among its events");
        KeepCard();
    }});
    cases.push_back({"bug-replay-mic", [] {
        TakeCard();
        const std::string log = Card("/FRIZZ/bug-1.txt");
        CardFiles().clear();
        std::istringstream script(log + "\nwait 3000\n");
        Check(PlayScript(script, nullptr, nullptr) == 0 && !log.empty(), "mic-bug-log: the twin plays it");
        // no cable, a cable, none, the key: AUX, as in the session (without the jack's lines
        // it would be the mic)
        mic_amp = .1f;
        // the log ends in the report's hold, SHIFT down: let go, F# shows the source again
        Press("ENC_6_SW", false);
        Press("KEY_26", false);
        RunMs(300);
        Check(green_led(SmtLedFull(kInputKeyLed)) && is_aux("replayed"),
              "mic-bug-log: played back, it ends on AUX by the key, as the session did");
    }});

    // ===== #116: the clock factor times every tempo the effects follow, not only a clock's =====
    {
        // SHIFT + LOOP, 5 taps ms apart
        static auto tap_tempo = [](uint32_t ms) {
            Press("KEY_26", true);
            for (int i = 0; i < 5; i++)
            {
                Tap("KEY_28");
                RunMs(ms - 60);
            }
            Press("KEY_26", false);
            RunMs(300);
        };
        // the settings page's G#, presses times (x1 -> x2 -> x1/2 -> x1), and back to play
        static auto factor = [](int presses) {
            SetToggle(true);
            RunMs(300);
            for (int i = 0; i < presses; i++)
            {
                Tap("KEY_19");
                RunMs(100);
            }
            SetToggle(false);
            RunMs(300);
        };

        // the use case: a song on AUX, its tempo tapped, the effects at double or half of it
        cases.push_back({"factor-tap", [] {
            RunMs(kReadyMs);
            tap_tempo(600);
            Check(Probe().tempo == 100, "factor-tap: 100 BPM tapped, no clock");
            SetToggle(true);
            RunMs(300);
            Tap("KEY_19"); // x2
            RunMs(500);
            Check(Probe().tempo == 200 && Probe().fx_bpm == 200.f, "factor-tap: x2, the effects at 200");
            // G# on the effects' beats: lit half a beat of 200, 150 ms
            std::vector<int> runs = Runs(Watch(false, kFactorKeyLed, 2000), 100);
            bool on = runs.size() >= 4;
            for (int r : runs)
                on &= r >= 125 && r <= 175;
            Check(on, "factor-tap: G# beats at 200");
            Tap("KEY_19"); // x1/2
            RunMs(500);
            Check(Probe().tempo == 50, "factor-tap: x1/2, the effects at 50");
            Tap("KEY_19"); // x1
            RunMs(500);
            Check(Probe().tempo == 100, "factor-tap: x1, the tapped 100 again");
            // past 300 after the factor: 200 tapped, x2 is 300
            SetToggle(false);
            RunMs(300);
            tap_tempo(300);
            factor(1);
            Check(Probe().tempo == 300, "factor-tap: 200 tapped at x2 is 300, the limit after the factor");
            factor(1); // x1/2
            Check(Probe().tempo == 100, "factor-tap: and x1/2 of it 100");
        }});

        // a loop recorded without a clock: the effects at the factor of its tempo, the loop as
        // it was
        cases.push_back({"factor-loop", [] {
            RunMs(kReadyMs);
            Tap("KEY_28");
            RunMs(2000);
            Tap("KEY_28");
            RunMs(1000);
            const ClockState c = Probe();
            printf("      loop: %zu frames, the effects at %.2f BPM\n", c.loop_length, c.fx_bpm);
            Check(c.loop_state == 2 && fabsf(c.fx_bpm - 120.f) < 6.f, "factor-loop: a 2 s loop, 4 beats of about 120 guessed");
            factor(1); // x2
            Check(fabsf(Probe().fx_bpm - 2.f * c.fx_bpm) < .01f, "factor-loop: x2, the effects at twice its tempo");
            factor(1); // x1/2
            const ClockState h = Probe();
            Check(fabsf(h.fx_bpm - .5f * c.fx_bpm) < .01f, "factor-loop: x1/2, the effects at half its tempo");
            Check(h.loop_length == c.loop_length && h.loop_beats == c.loop_beats,
                  "factor-loop: the loop as it was, its length and beats");
        }});

        // a loop recorded to a clock at x2: as before, the effects at the factor of the clock,
        // not twice; then the factor follows on the loop
        cases.push_back({"factor-clock-loop", [] {
            clock_bpm = 120.f;
            RunMs(kReadyMs);
            RunMs(2000);
            factor(1); // x2
            RunMs(2000);
            Check(Probe().tempo == 240, "factor-clock-loop: a 120 BPM clock at x2, 240");
            Press("KEY_27", true);
            RunMs(100);
            Tap("KEY_28");
            Press("KEY_27", false);
            RunMs(1500);
            Tap("KEY_28"); // closes at the bar's end
            RunMs(2000);
            const ClockState c = Probe();
            Check(c.loop_state == 2 && c.loop_beats > 0 && c.tempo == 240,
                  "factor-clock-loop: a quantized loop at x2, the effects at 240 as before, not 480");
            factor(1); // x1/2
            const ClockState h = Probe();
            Check(h.tempo == 60, "factor-clock-loop: x1/2 on the loop, the effects at 60");
            Check(h.loop_length == c.loop_length && h.loop_beats == c.loop_beats,
                  "factor-clock-loop: the loop as it was, its length and beats");
            printf("      loop: %zu frames, %u beats\n", c.loop_length, c.loop_beats);
        }});

        // 3 beats at x1/2 are a beat and a half a pass: the effects' grid runs over two passes,
        // beat 1 on every other loop start, the other start halfway through a beat (decided in
        // #116)
        cases.push_back({"factor-odd", [] {
            RunMs(kReadyMs);
            tap_tempo(500); // 120
            Tap("KEY_28");
            RunMs(1500);
            Tap("KEY_28"); // 1.5 s: 3 beats of 120
            RunMs(500);
            const float one = Probe().fx_bpm;
            factor(2); // x1/2
            printf("      the effects at %.2f BPM, at x1/2 %.2f\n", one, Probe().fx_bpm);
            Check(fabsf(one - 120.f) < 6.f && fabsf(Probe().fx_bpm - .5f * one) < .01f,
                  "factor-odd: 3 beats of about 120, at x1/2 the effects at half that");
            // the grid's position just after each loop start
            std::vector<uint32_t> at_start;
            float last = Probe().loop_pos;
            for (int ms = 0; ms < 8000 && at_start.size() < 4; ms += 5)
            {
                RunMs(5);
                const ClockState c = Probe();
                if (c.loop_pos < last)
                    at_start.push_back(c.position);
                last = c.loop_pos;
            }
            printf("      at the loop's starts:");
            for (uint32_t pos : at_start)
                printf(" %u", pos);
            printf("\n");
            bool alternate = at_start.size() == 4;
            for (size_t i = 0; alternate && i < at_start.size(); i++)
                alternate = (at_start[i] == 0 || at_start[i] == 18) && (i == 0 || at_start[i] != at_start[i - 1]);
            Check(alternate, "factor-odd: the loop starts on the grid's 0 and 18 in turn");
        }});
    }

    // #118: a morph and the crossfader fade the freezer, shifter and slicer with their page 2
    // Mix instead of switching them: the slicer sparse and short, much quieter than dry, so
    // its Mix shows in the level
    {
        // the slicer latched, one 16th of 8 at 10 ms decay, saved as scene 1; dry and sliced
        static float dry, sliced;
        static auto slicer_scene = [] {
            RunMs(kReadyMs);
            dry = RunMs(1000);
            Latch("KEY_9");
            Turn(kKnob1Encoder, -100); // pattern: the sparsest
            Turn(1, -100);             // knob 2, decay: 10 ms
            RunMs(500);
            sliced = RunMs(1000); // a pattern is half a bar at 120 BPM
            Save(1);
            RunMs(1000);
            printf("      dry %.3f, sliced %.3f\n", dry, sliced);
            Check(sliced < dry * .3f, "morph-mix: the slicer sparse and short, much quieter than dry");
        };
        // SHIFT + key 4 taps: a morph over 4 bars once SHIFT goes up; the level a second at a
        // time until it has landed
        static auto morph4 = [](const char* key) {
            Press("KEY_26", true);
            RunMs(80);
            for (int tap = 0; tap < 4; tap++)
            {
                Tap(key);
                RunMs(100);
            }
            Press("KEY_26", false);
            std::vector<float> level;
            for (int s = 0; s < 10; s++)
                level.push_back(RunMs(1000));
            printf("      over the morph:");
            for (float l : level)
                printf(" %.3f", l);
            printf("\n");
            return level;
        };
        // the windows clearly between lo and hi
        static auto between = [](const std::vector<float>& level, float lo, float hi) {
            int n = 0;
            for (float l : level)
                n += l > lo + .15f * (hi - lo) && l < hi - .15f * (hi - lo);
            return n;
        };

        cases.push_back({"morph-mix-slicer", [] {
            slicer_scene();
            const std::vector<float> out = morph4(kSceneKeys[0]); // to the blank scene
            Check(between(out, sliced, dry) >= 3 && out.back() > dry * .9f,
                  "morph-mix-slicer: turned off, its Mix glides to dry over the morph, then it's off");
            bool rising = true;
            for (size_t i = 1; i < out.size(); i++)
                rising &= out[i] > out[i - 1] - .02f * dry;
            Check(rising, "morph-mix-slicer: steadily");
            const std::vector<float> in = morph4(kSceneKeys[1]); // back to scene 1
            Check(between(in, sliced, dry) >= 3 && in.back() < sliced * 1.3f,
                  "morph-mix-slicer: turned on, it comes on at dry and its Mix glides to the scene's");
            const std::string st = Ask({0x20});
            Check(st.size() > 9 && !(st[6] & 2) && st[5] == 2 && (LatchedFx(st) >> 7 & 1),
                  "morph-mix-slicer: landed on scene 1, the slicer (FX 7) latched");
        }});

        cases.push_back({"morph-mix-fader", [] {
            slicer_scene();
            Press("KEY_26", true);
            RunMs(80);
            Tap(kSceneKeys[0]); // to the blank scene, held
            RunMs(100);
            Turn(5, 6); // a quarter
            RunMs(500);
            const float quarter = RunMs(1000);
            Turn(5, 6); // halfway
            RunMs(500);
            const float half = RunMs(1000);
            Turn(5, -12); // back to the start
            RunMs(500);
            const float start = RunMs(1000);
            printf("      a quarter %.3f, halfway %.3f, back at the start %.3f\n", quarter, half, start);
            Check(quarter > sliced * 1.5f && quarter < dry * .5f,
                  "morph-mix-fader: a quarter of the way, the slicer at three quarters of its Mix");
            Check(half > quarter * 1.2f && half < dry * .75f,
                  "morph-mix-fader: halfway, at half its Mix, not switched off");
            Check(start < sliced * 1.3f, "morph-mix-fader: back at the start, sliced as before");
            Turn(5, 30);
            RunMs(500);
            Press("KEY_26", false);
            RunMs(500);
            const std::string st = Ask({0x20});
            Check(RunMs(1000) > dry * .9f && st.size() > 9 && !(st[6] & 2) && LatchedFx(st) == 0,
                  "morph-mix-fader: all the way, dry; let go, it lands on the blank scene");
            // the slicer parked at Mix 0: a recall of scene 1 brings it back at its Mix
            Tap(kSceneKeys[1]);
            RunMs(500);
            const float back = RunMs(1000);
            printf("      scene 1 recalled: %.3f\n", back);
            Check(back < sliced * 1.3f, "morph-mix-fader: then scene 1 recalled, the slicer sliced again, not dry");
        }});

        // the shifter, a fifth up, in and out by a morph: no click where it comes on at dry
        // or goes off at the landing (a guard: it switched without one before too)
        cases.push_back({"morph-mix-shifter", [] {
            RunMs(kReadyMs);
            const float dry_step = StepMs(1000);
            Latch("KEY_2");
            Turn(kKnob1Encoder, 14); // shift up
            RunMs(500);
            const float shifted_step = StepMs(2000);
            Save(1);
            RunMs(1000);
            const float steady = std::max(dry_step, shifted_step);
            max_step = 0.f;
            morph4(kSceneKeys[0]);
            const float out = max_step;
            max_step = 0.f;
            morph4(kSceneKeys[1]);
            const float in = max_step;
            printf("      largest step: steady %.4f (dry %.4f), fading out %.4f, in %.4f\n", steady,
                   dry_step, out, in);
            Check(out < steady * 1.5f && in < steady * 1.5f,
                  "morph-mix-shifter: fading out and in with its Mix: no click");
            const std::string st = Ask({0x20});
            Check(st.size() > 9 && st[5] == 2 && (LatchedFx(st) >> 1 & 1),
                  "morph-mix-shifter: landed on scene 1, the shifter (FX 1) latched");
        }});

        // the freezer: frozen on a sine, the input then silent; a morph to the blank scene
        // fades the repeats out with its Mix, instead of cutting them at the landing
        cases.push_back({"morph-mix-freezer", [] {
            RunMs(kReadyMs);
            const float dry = RunMs(1000);
            Latch("KEY_1");
            RunMs(3000);
            Save(1);
            RunMs(1000);
            amp = 0.f;
            RunMs(500);
            const float frozen = RunMs(1000);
            printf("      dry %.3f, frozen %.3f\n", dry, frozen);
            Check(frozen > dry * .5f, "morph-mix-freezer: the input silent, the freezer repeats the sine");
            const std::vector<float> out = morph4(kSceneKeys[0]);
            Check(between(out, 0.f, frozen) >= 3 && out.back() < frozen * .05f,
                  "morph-mix-freezer: turned off, its repeats fade with its Mix over the morph");
        }});
    }

    // ===== #153, #175: a hard drop to a scene on a bar line: SHIFT + scene key, the transport
    // pressed, SHIFT let go; the bars count from the release =====
    {
        // scene 1 (kSceneKeys[1]): the filter on and closed, much quieter than dry; then the blank
        // scene (kSceneKeys[0]), dry. A 2 kHz sine, a period a block, so a block's level is steady
        static auto filter_scene = [] {
            freq = 2000.f;
            Latch("KEY_5");
            Turn(kKnob1Encoder, -60);
            RunMs(300);
            Save(1);
            Tap(kSceneKeys[0]);
            RunMs(500);
        };
        // to the block after the FX clock's next bar line (its position a multiple of 48)
        static auto past_line = [] {
            uint32_t pos = Probe().position;
            for (int b = 0; b < 40000; b++)
            {
                RunBlocks(1);
                const uint32_t now = Probe().position;
                if (now != pos && now % 48 == 0)
                    return;
                pos = now;
            }
        };
        // the gesture, all let go: SHIFT, the scene key taps times, the transport pressed, the
        // scene key after more times. The level's largest change meanwhile, relative to where
        // it was
        static auto drop = [](const char* key, int taps, int after = 0) {
            const float level = RunBlocks(1);
            float moved = 0.f;
            auto run = [&](uint32_t ms) { moved = std::max(moved, fabsf(RunMs(ms) - level) / level); };
            Press("KEY_26", true);
            run(60);
            for (int t = 0; t < taps; t++)
            {
                Press(key, true);
                run(60);
                Press(key, false);
                run(40);
            }
            Press("ENC_5_SW", true);
            run(60);
            Press("ENC_5_SW", false);
            run(40);
            for (int t = 0; t < after; t++)
            {
                Press(key, true);
                run(60);
                Press(key, false);
                run(40);
            }
            Press("KEY_26", false);
            run(100);
            return moved;
        };
        // block by block, up to ms: the blocks in which the FX clock crossed a bar line, and the
        // first whose level is off `level` by more than 3 %
        struct Watched
        {
            std::vector<int> lines;
            int changed = -1;
            float loop_pos = -1.f; // where a loop played at the first line
        };
        static auto watch = [](float level, uint32_t ms) {
            Watched w;
            uint32_t pos = Probe().position;
            for (int b = 0; b < static_cast<int>(ms * 2); b++)
            {
                const float rms = RunBlocks(1);
                const uint32_t now = Probe().position;
                if (now != pos && now % 48 == 0)
                {
                    if (w.lines.empty())
                        w.loop_pos = Probe().loop_pos;
                    w.lines.push_back(b);
                }
                pos = now;
                if (w.changed < 0 && fabsf(rms - level) > .03f * level)
                    w.changed = b;
            }
            return w;
        };
        static auto lines = [](const Watched& w) {
            printf("      bar lines in blocks");
            for (int l : w.lines)
                printf(" %d", l);
            printf(", the sound changed in %d\n", w.changed);
        };
        auto state = [] { return Ask({0x20}); };

        // free at 120 BPM: the key blinks red on the beat, the scene lands in the line's block
        cases.push_back({"scene-drop", [state] {
            RunMs(kReadyMs);
            filter_scene();
            const float dry = RunBlocks(1);
            past_line();
            const float moved = drop(kSceneKeys[1], 1);
            printf("      the level moved %.4f during the gesture\n", moved);
            Check(moved < .03f, "scene-drop: the gesture itself changes nothing in the sound");
            const std::vector<int> beat = Watch(false, kSlot1Led, 1000);
            const std::vector<int> runs = Runs(beat, 128);
            bool on_beat = runs.size() >= 2;
            for (int r : runs)
                on_beat &= r >= 230 && r <= 270;
            Rgb lit = {};
            for (int t = 0; t < 300 && Max(lit) < 200; t++)
            {
                RunMs(1);
                lit = SmtLedFull(kSlot1Led);
            }
            printf("      drop blinks:");
            for (int r : runs)
                printf(" %d", r);
            printf(" ms, lit %s\n", Hex(lit).c_str());
            Check(on_beat && lit.r > 200 && lit.g == 0 && lit.b == 0,
                  "scene-drop: while it waits, its key blinks red on the beat");
            Check(state().size() > 6 && (state()[6] & 2) && state()[5] == 2,
                  "scene-drop: waiting, its scene the active one");
            const Watched w = watch(dry, 2000);
            lines(w);
            Check(w.lines.size() == 1 && w.changed == w.lines[0],
                  "scene-drop: nothing before the bar line, the scene in its block");
            RunMs(300);
            Check(RunMs(300) < dry * .5f && !(state()[6] & 2) && state()[5] == 2,
                  "scene-drop: landed on scene 1, the filter closed");
        }});

        // the same on a MIDI clock (100 BPM) and on a tapped tempo (90 BPM)
        static auto on_line = [](const char* name) {
            const float dry = RunBlocks(1);
            past_line();
            const float moved = drop(kSceneKeys[1], 1);
            const Watched w = watch(dry, 3000);
            lines(w);
            Check(moved < .03f && w.lines.size() >= 1 && w.changed == w.lines[0],
                  (std::string(name) + ": nothing at the press, the scene in the next bar line's block").c_str());
        };
        cases.push_back({"scene-drop-clock", [] {
            clock_bpm = 100.f;
            RunMs(kReadyMs);
            filter_scene();
            RunMs(1000);
            Check(Probe().tempo == 100, "scene-drop-clock: following the clock at 100 BPM");
            on_line("scene-drop-clock");
        }});
        cases.push_back({"scene-drop-tap", [] {
            RunMs(kReadyMs);
            filter_scene();
            Press("KEY_26", true);
            for (int i = 0; i < 5; i++)
            {
                Tap("KEY_28");
                RunMs(607);
            }
            Press("KEY_26", false);
            RunMs(300);
            Check(Probe().tempo == 90, "scene-drop-tap: tapped to 90 BPM");
            on_line("scene-drop-tap");
        }});

        // with a loop: on its end, when that's the next line; the press doesn't touch the loop
        cases.push_back({"scene-drop-loop", [] {
            RunMs(kReadyMs);
            filter_scene();
            Tap("KEY_28");
            RunMs(1440); // 1.5 s, the tap's 60 ms in
            Tap("KEY_28");
            RunMs(500);
            sine = false; // the loop alone: the same sine
            RunMs(500);
            Check(Probe().loop_state == 2, "scene-drop-loop: a loop playing");
            // into its last bar line before the end: the end is the next one
            const float len = Probe().loop_length;
            for (int b = 0; b < 20000 && Probe().loop_pos * len < len - .55f * kSampleRate; b++)
                RunBlocks(1);
            const float dry = RunBlocks(1);
            drop(kSceneKeys[1], 1);
            const Watched w = watch(dry, 1000);
            lines(w);
            printf("      loop %.3f s, at %.4f of it on the line\n", len / kSampleRate, w.loop_pos);
            Check(w.lines.size() >= 1 && w.changed == w.lines[0] && w.loop_pos < .01f,
                  "scene-drop-loop: the scene on the loop's end, in its block");
            Check(Probe().loop_state == 2 && Probe().loop_speed == 1.f,
                  "scene-drop-loop: the loop plays on at 1x");
        }});

        // three taps: the third bar line; a tap after the press one more; SHIFT + its key in a
        // later hold takes it back, as does a plain scene key
        cases.push_back({"scene-drop-bars", [state] {
            RunMs(kReadyMs);
            filter_scene();
            const float dry = RunBlocks(1);
            past_line();
            drop(kSceneKeys[1], 3);
            Watched w = watch(dry, 6500);
            lines(w);
            Check(w.lines.size() >= 3 && w.changed == w.lines[2], "scene-drop-bars: three taps, the third bar line");
            Tap(kSceneKeys[0]);
            RunMs(500);
            past_line();
            drop(kSceneKeys[1], 1, 1);
            w = watch(dry, 4500);
            lines(w);
            Check(w.lines.size() >= 2 && w.changed == w.lines[1],
                  "scene-drop-bars: its key tapped again after the press, a bar later");
            Tap(kSceneKeys[0]);
            RunMs(500);
            past_line();
            drop(kSceneKeys[1], 1);
            RunMs(500);
            Press("KEY_26", true);
            RunMs(60);
            Tap(kSceneKeys[1]);
            Press("KEY_26", false);
            RunMs(100);
            w = watch(dry, 2500);
            Check(w.changed < 0 && !(state()[6] & 2) && state()[5] == 2 && (state()[6] & 1),
                  "scene-drop-bars: SHIFT + its red key in a later hold: no drop, the scene edited");
            Tap(kSceneKeys[0]);
            RunMs(500);
            past_line();
            drop(kSceneKeys[1], 1);
            RunMs(500);
            Tap(kSceneKeys[0]);
            RunMs(100);
            w = watch(dry, 2500);
            Check(w.changed < 0 && !(state()[6] & 2) && state()[5] == 1,
                  "scene-drop-bars: a plain scene key meanwhile recalls at once, no drop after");
        }});

        // what goes off ends on the line: from scene 1 to the blank scene, the filter's fade
        // out over before the line, not after it
        cases.push_back({"scene-drop-off", [] {
            RunMs(kReadyMs);
            filter_scene();
            const float dry = RunBlocks(1);
            Tap(kSceneKeys[1]);
            RunMs(500);
            const float closed = RunBlocks(1);
            past_line();
            drop(kSceneKeys[0], 1);
            // to the line, keeping the last 40 blocks' levels
            std::vector<float> level;
            uint32_t pos = Probe().position;
            for (int b = 0; b < 5000; b++)
            {
                level.push_back(RunBlocks(1));
                const uint32_t now = Probe().position;
                if (now != pos && now % 48 == 0)
                    break;
                pos = now;
            }
            const size_t n = level.size();
            printf("      dry %.4f, closed %.4f; 60 blocks before the line %.4f, 20 %.4f, at it %.4f\n",
                   dry, closed, level[n - 61], level[n - 21], level[n - 1]);
            Check(n > 100 && level[n - 61] <= closed + dry * .05f && level[n - 21] > dry * .9f
                      && fabsf(level[n - 1] - dry) < .03f * dry,
                  "scene-drop-off: the filter quiet 30 ms before the line, gone by it");
        }});

        // build-up, then drop: SHIFT + scene 1, then the blank scene, let go: the morph to scene 1
        // (the filter closing) glides over the bar, scene 1's key white on the beat, the blank
        // scene's red, and the blank scene lands on the line, dry. SHIFT + a blinking key in a
        // later hold stops a sequence where it is
        cases.push_back({"scene-drop-morph", [state] {
            RunMs(kReadyMs);
            filter_scene();
            const float dry = RunBlocks(1);
            past_line();
            Press("KEY_26", true);
            RunMs(60);
            Tap(kSceneKeys[1]);
            RunMs(40);
            Tap(kSceneKeys[0]);
            RunMs(40);
            Press("KEY_26", false);
            RunMs(20);
            Check(state()[5] == 1 && (state()[6] & 2), "scene-drop-morph: SHIFT + scene 1, then the blank one: a build-up");
            Rgb a = {}, b = {};
            for (int t = 0; t < 600; t++)
            {
                RunMs(1);
                if (Max(SmtLedFull(kSlot1Led)) > Max(a))
                    a = SmtLedFull(kSlot1Led);
                if (Max(SmtLedFull(kSlot1Led - 1)) > Max(b))
                    b = SmtLedFull(kSlot1Led - 1);
            }
            printf("      blinking: scene 1 %s, the blank scene %s\n", Hex(a).c_str(), Hex(b).c_str());
            Check(a.r > 200 && a.g > 200 && a.b > 200 && b.r > 200 && b.g == 0 && b.b == 0,
                  "scene-drop-morph: scene 1 blinks white, the blank scene red");
            std::vector<float> level;
            uint32_t pos = Probe().position;
            for (int b = 0; b < 5000; b++)
            {
                level.push_back(RunBlocks(1));
                const uint32_t now = Probe().position;
                if (now != pos && now % 48 == 0)
                    break;
                pos = now;
            }
            const size_t n = level.size();
            printf("      dry %.4f, 100 blocks before the line %.4f, 2 %.4f, at it %.4f\n", dry,
                   level[n - 101], level[n - 2], level[n - 1]);
            // a glide: the level passes through the middle on its way down, not a jump
            bool middle = false;
            for (size_t i = 0; i < n / 2; i++)
                middle |= level[i] > dry * .2f && level[i] < dry * .8f;
            Check(n > 1000 && middle && level[n - 101] < dry * .1f,
                  "scene-drop-morph: the morph to scene 1 glides on to the line");
            Check(fabsf(level[n - 1] - dry) < .03f * dry, "scene-drop-morph: the blank scene on the line, dry");
            RunMs(300);
            Check(!(state()[6] & 2) && state()[5] == 1 && !(state()[6] & 1),
                  "scene-drop-morph: landed on the blank scene");

            // SHIFT + scene 1 in one hold, let go; SHIFT + scene 1 in another: stopped where it is
            past_line();
            Press("KEY_26", true);
            RunMs(60);
            Tap(kSceneKeys[1]);
            Press("KEY_26", false);
            RunMs(1000);
            Press("KEY_26", true);
            RunMs(60);
            Tap(kSceneKeys[1]);
            Press("KEY_26", false);
            RunMs(100);
            const float stopped = RunMs(300);
            RunMs(2500);
            Check(!(state()[6] & 2) && (state()[6] & 1) && state()[5] == 2
                      && fabsf(RunMs(300) - stopped) < .05f * stopped && stopped < dry * .9f,
                  "scene-drop-morph: SHIFT + its blinking key in a later hold: stopped where it was, edited");

            // a build-up stopped by its red key
            Tap(kSceneKeys[0]);
            RunMs(300);
            Press("KEY_26", true);
            RunMs(60);
            Tap(kSceneKeys[1]);
            RunMs(40);
            Tap(kSceneKeys[0]);
            Press("KEY_26", false);
            RunMs(300);
            Press("KEY_26", true);
            RunMs(60);
            Tap(kSceneKeys[0]);
            Press("KEY_26", false);
            RunMs(100);
            Check(!(state()[6] & 2), "scene-drop-morph: SHIFT + the build-up's red key stops it too");
        }});

        // SHIFT + another scene in a later hold: the sequence running stopped where it got to,
        // a new one from there, without a jump
        cases.push_back({"scene-replace", [state] {
            RunMs(kReadyMs);
            filter_scene();
            const float dry = RunBlocks(1);
            past_line();
            Press("KEY_26", true);
            RunMs(60);
            Tap(kSceneKeys[1]);
            RunMs(40);
            Tap(kSceneKeys[1]);
            Press("KEY_26", false);
            RunMs(150); // over 2 bars: some way down
            const float before = RunMs(20);
            Press("KEY_26", true);
            RunMs(60);
            Tap(kSceneKeys[0]);
            Press("KEY_26", false);
            const float after = RunMs(20);
            printf("      dry %.4f, before %.4f, after %.4f\n", dry, before, after);
            // where it got to isn't scene 1's closed filter: landing that first would fall silent
            Check(before > dry * .1f && fabsf(after - before) < .05f * before && state()[5] == 1
                      && (state()[6] & 2),
                  "scene-replace: SHIFT + another scene: a morph to it from where the other got to");
            RunMs(4500);
            Check(!(state()[6] & 2) && state()[5] == 1 && fabsf(RunMs(300) - dry) < .03f * dry,
                  "scene-replace: and it lands there");
        }});

        // the crossfader's protection: 2 detents with a press within 150 ms are a drop; 2
        // detents alone take the crossfader 150 ms later; 1 detent nothing
        cases.push_back({"scene-fader-guard", [state] {
            RunMs(kReadyMs);
            filter_scene();
            auto fader_shown = [] {
                const Rgb rev = PthLedFull(kTransportRevLed);
                return rev.r > 100 && rev.r == rev.g && rev.g == rev.b;
            };
            past_line();
            Press("KEY_26", true);
            RunMs(60);
            Tap(kSceneKeys[1]);
            Turn(5, 2);
            RunMs(60);
            Tap("ENC_5_SW");
            RunMs(400);
            Check(!fader_shown() && (state()[6] & 2), "scene-fader-guard: 2 detents and a press within 150 ms: no crossfader");
            Press("KEY_26", false);
            Rgb red = {};
            for (int t = 0; t < 600; t++)
            {
                RunMs(1);
                if (Max(SmtLedFull(kSlot1Led)) > Max(red))
                    red = SmtLedFull(kSlot1Led);
            }
            Check(red.r > 200 && red.g == 0, "scene-fader-guard: a drop, its key red");
            Tap(kSceneKeys[0]);
            RunMs(300);
            Press("KEY_26", true);
            RunMs(60);
            Tap(kSceneKeys[1]);
            Turn(5, 1);
            RunMs(400);
            Check(!fader_shown(), "scene-fader-guard: 1 detent: nothing");
            Turn(5, 1);
            RunMs(100);
            Check(!fader_shown(), "scene-fader-guard: 2: not yet within 150 ms");
            RunMs(200);
            Check(fader_shown(), "scene-fader-guard: then the crossfader, the transport LEDs white");
            Tap("ENC_5_SW");
            Press("KEY_26", false);
            RunMs(300);
            Check(!(state()[6] & 2) && (state()[6] & 1), "scene-fader-guard: a press after it no drop; let go, it stops where it is");
        }});

        // PLAY has no part in the scene gestures any more: SHIFT + PLAY pauses the loop, the
        // morph held runs on
        cases.push_back({"scene-play", [state] {
            RunMs(kReadyMs);
            filter_scene();
            Tap("KEY_28");
            RunMs(1440);
            Tap("KEY_28");
            RunMs(500);
            Press("KEY_26", true);
            RunMs(60);
            Tap(kSceneKeys[1]);
            Tap("KEY_27");
            RunMs(100);
            Check(Probe().loop_state == 3 && (state()[6] & 2), "scene-play: SHIFT + PLAY pauses, the morph held");
            Press("KEY_26", false);
            RunMs(4000);
            Check(!(state()[6] & 2) && state()[5] == 2, "scene-play: let go, the morph lands");
        }});

        // CC 83 (channel 16): a drop to the scene given, on the next bar line, CC 9's later
        cases.push_back({"midi-drop", [state] {
            RunMs(kReadyMs);
            filter_scene();
            const float dry = RunBlocks(1);
            past_line();
            Trs({0xBF, 83, 1});
            RunMs(100);
            Check(state()[5] == 2 && (state()[6] & 2), "midi-drop: CC 83 = 1, a drop to scene 1");
            Watched w = watch(dry, 2500);
            lines(w);
            Check(w.lines.size() >= 1 && w.changed == w.lines[0], "midi-drop: on the next bar line");
            Trs({0xBF, 83, 0});
            RunMs(100);
            w = watch(RunBlocks(1), 2500);
            Check(w.changed >= 0 && fabsf(RunBlocks(1) - dry) < .03f * dry,
                  "midi-drop: CC 83 = 0, back to the blank scene");
            past_line();
            Trs({0xBF, 9, 2, 0xBF, 83, 1});
            w = watch(dry, 4500);
            lines(w);
            Check(w.lines.size() >= 2 && w.changed == w.lines[1], "midi-drop: with CC 9 = 2, the second bar line");
        }});
    }

    // ===== #169: the pre-1.0 audit's UI faults =====
    {
        // a hand's tap tempo by when the keys went down, not when MainLoop (every 2 ms, or
        // later behind a card write) got to them: taps 210 and 211 ms apart, 421 ms over two
        // intervals, 285.04 BPM; MainLoop's 2 ms grid sees 420 or 422, 286 or 284 BPM
        cases.push_back({"tap-at-key", [] {
            RunMs(kReadyMs);
            Press("KEY_26", true);
            RunMs(100);
            Tap("KEY_28");
            RunMs(210 - 60);
            Tap("KEY_28");
            RunMs(211 - 60);
            Tap("KEY_28");
            RunMs(100);
            Press("KEY_26", false);
            RunMs(300);
            printf("      tapped: %d BPM\n", Probe().tempo);
            Check(Probe().tempo == 285, "tap-at-key: 3 taps 421 ms over, 285 BPM");
        }});

        // the safety limiter at work while the settings page is up shows on the compressor's
        // key once it's back only if it still works: the page takes what it saw meanwhile
        cases.push_back({"limiter-led-settings", [] {
            amp = 1.f;
            RunMs(kReadyMs);
            Turn(kVolumeEncoder, 100); // VOLUME up: the line outs into the limiter
            RunMs(600);
            Check(SmtLedFull(kCompKeyLed).r > 200, "limiter-led-settings: limiting: the key red");
            SetToggle(true);
            RunMs(500);
            amp = 0.f; // quiet now, while the settings page is up
            RunMs(1500);
            SetToggle(false);
            RunMs(200);
            const Rgb back = SmtLedFull(kCompKeyLed);
            printf("      back on the play page: %02x%02x%02x\n", back.r, back.g, back.b);
            Check(back.r == back.g && back.g == back.b, "limiter-led-settings: back, the key white, no old red");
            Turn(kVolumeEncoder, -100);
            amp = .3f;
        }});

        // an empty scene sent over SysEx into the active slot (the dev path, midisuite's
        // restore): no scene is active then, as after DELETE, not an empty slot lit as one
        cases.push_back({"scene-put-empty", [] {
            RunMs(kReadyMs);
            Latch("KEY_5");
            Save(1);
            RunMs(1000);
            std::string st = Ask({0x20});
            Check(st.size() > 6 && st[5] == 2, "scene-put-empty: scene 1 saved, active");
            bool taken = true;
            for (int part = 0; part < 8; part++)
            {
                std::vector<int> put = {0x31, 1, part};
                put.resize(put.size() + 32, 0);
                const std::string r = Ask(put);
                taken &= r.size() > 2 && r[2] == 0;
            }
            const std::string r = Ask({0x31, 1, 8, 0, 0, 0}); // used 0
            taken &= r.size() > 2 && r[2] == 0;
            RunMs(1000);
            st = Ask({0x20});
            Check(taken && st.size() > 6 && st[5] == 0 && !(st[6] & 1),
                  "scene-put-empty: put empty into it over SysEx, no scene active");
        }});

        // after a save that failed, the next one mounts the card again, which makes a file
        // open on it invalid: not while a bug report is being written, whose file that is.
        // The master settings' save comes due while the report writes, on a slow card
        cases.push_back({"remount-bug-report", [] {
            RunMs(kReadyMs);
            SetCardSpace(0); // the scene save fails
            Latch("KEY_5");
            Save(1);
            RunMs(1000);
            SetCardSpace(SIZE_MAX);
            SetCardWriteMs(100);
            SetToggle(true);
            RunMs(300);
            Press("KEY_26", true);
            RunMs(100);
            Press("ENC_6_SW", true); // held 2 s: the report, written over some 100 ms writes
            RunMs(100);
            const std::string master = Card("/FRIZZ/frizz_master.txt");
            Ask({0x3E, 3, 1, 0x22, 1, 0x30, 0}); // AUX's input gain: the master settings saved 2 s on
            RunMs(2280);
            Press("ENC_6_SW", false);
            Press("KEY_26", false);
            RunMs(3000);
            const std::string bug = Card("/FRIZZ/bug-1.txt");
            Check(bug.find("# written here") != std::string::npos && CardOpenFiles() == 0,
                  "remount-bug-report: the report written whole, and closed");
            const std::string now = Card("/FRIZZ/frizz_master.txt");
            Check(now != master && now.find("compressor2") != std::string::npos,
                  "remount-bug-report: the master settings written after it");
        }});

        // the developers' SET of a setting leaves what MIDI set without the card: CC 86's
        // input gain, CC 116's effects and CC 117's live (#97, #177)
        cases.push_back({"dev-set-keeps-midi", [] {
            RunMs(kReadyMs);
            Trs({kCC, 86, 100, kCC, 116, 127, kCC, 117, 0}); // AUX: its gain, dry, not live
            RunMs(100);
            const std::string midi = Ask({0x20});
            Check(midi.size() > 25 && (midi[25] & 6) == 2, "dev-set-keeps-midi: CC 116 dry, CC 117 not live");
            const std::string set = Ask({0x3E, 3, 1, 0x23, 6, 0, 50}); // the LEDs at 50 %
            RunMs(100);
            const std::string after = Ask({0x20});
            printf("      in gain %02x%02x -> %02x%02x, input %d -> %d\n", midi[17], midi[18],
                   after[17], after[18], midi[25], after[25]);
            Check(set.size() > 2 && set[2] == 0 && after.size() > 25 && after.substr(17, 2) == midi.substr(17, 2)
                      && after[25] == midi[25],
                  "dev-set-keeps-midi: SET the LEDs, the gain, dry and live stay as MIDI set them");
            // a SET of one of them sets it, even to the value the card has
            Ask({0x3E, 3, 2, 0x23, 9, 0, 0});  // aux_dry 0: through the effects again
            Ask({0x3E, 3, 3, 0x23, 10, 0, 75}); // gain_aux 75 %
            RunMs(100);
            const std::string own = Ask({0x20});
            Check(own.size() > 25 && !(own[25] & 2) && own.substr(17, 2) != midi.substr(17, 2)
                      && !(own[25] & 4),
                  "dev-set-keeps-midi: SET aux_dry 0 and gain_aux 75 take, CC 117's live stays off");
        }});
    }

    // every scenario of ../twin/scenarios played from power-on, its expect lines holding
    static std::vector<std::string> scenarios;
    {
        const std::string dir = std::string(__FILE__).substr(0, std::string(__FILE__).rfind('/') + 1)
                                + "../twin/scenarios/";
        FILE* ls = popen(("ls " + dir + "*.txt").c_str(), "r");
        char path[512];
        while (ls && fgets(path, sizeof(path), ls))
        {
            scenarios.push_back(path);
            scenarios.back().erase(scenarios.back().find_last_not_of("\n") + 1);
        }
        if (ls)
            pclose(ls);
    }
    Check(scenarios.size() >= 10, "scenarios: found in twin/scenarios");
    for (const std::string& path : scenarios)
    {
        const std::string file = path.substr(path.rfind('/') + 1);
        cases.push_back({"scenario:" + file.substr(0, file.size() - 4), [path] {
            std::ifstream script(path);
            const int failed = PlayScript(script, nullptr, nullptr);
            Check(failed == 0, (path.substr(path.rfind('/') + 1) + ": played from power-on, every expect holds").c_str());
        }});
    }

    // the cases that take the card one before them kept (KeepCard, TakeCard)
    Chain({"clock-source-log", "bug-replay-usb"});
    Chain({"card-1", "card-2", "card-3", "bug-log", "bug-full", "bug-replay"});
    Chain({"midi-transport", "midi-kept"});
    Chain({"mic-bug-log", "bug-replay-mic"});
    // a replay boots by its script
    RunAllCases([](const Case& c) {
        return c.name.compare(0, 10, "bug-replay") != 0 && c.name.compare(0, 9, "scenario:") != 0;
    });
    return Finish();
}
