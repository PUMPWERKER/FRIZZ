// ui.cpp: plays keys, knobs and MIDI into the virtual CHOMPI (twin/twin.h), FRIZZ's whole
// firmware from power-on with its play page, LEDs and debouncing, and checks what the LEDs show
// and what comes out. Each case boots its own device (a forked process). Run by unit.sh ui.
#include <sys/wait.h>
#include <unistd.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <functional>
#include <list>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "check.h"
#include "script.h"
#include "twin.h"

using namespace twin;

// the play page's LEDs (NormalPage.h, FxSlots.h)
static const int kKnob1Led = 1, kPlayLed = 7, kLoopLed = 8;
static const int kFilterKeyLed = 20, kShifterKeyLed = 23, kFlangerKeyLed = 19;
static const int kVolumeEncoder = 6, kKnob1Encoder = 4; // SW6, SW4
static const int kTransportRevLed = 5, kTransportFwdLed = 6, kVolumeLed = 9;
static const int kSlot1Led = 1, kCompKeyLed = 10, kTapeStopKeyLed = 15;
// the settings page's (SettingsPage.h): white keys 1, 3, 11, 14, 15 (channels; the 15th is 15
// and 16), the lower octave's D#, F#, G#, A#, the upper octave's D# and G#
static const int kChannel1Led = 24, kChannel3Led = 22, kChannel11Led = 14, kChannel14Led = 11,
                 kChannel1516Led = 10, kTransportKeyLed = 1, kMonoKeyLed = 2, kFactorKeyLed = 3,
                 kBrightnessKeyLed = 4, kMidiOutKeyLed = 6, kSourceKeyLed = 8;
static const int kSaveKeyLed = 9; // KEY_25, the play page's SAVE

static bool sine = true;
static float amp = .3f, freq = 220.f, phase = 0.f;
static float clock_bpm = 0.f; // a MIDI clock into the jack while > 0
static double clock_next = 0.;
static float usb_clock_bpm = 0.f; // and one over USB
static double usb_clock_next = 0.;
static float last_out[2] = {0.f, 0.f}, max_step = 0.f; // the master out's largest step, L or R
static float hp_rms = 0.f; // the headphones' (left) RMS over the last RunMs

/** Runs ms with a sine (or silence) into AUX; the master out's RMS over the time */
static float RunMs(uint32_t ms)
{
    float in[kBlockSize * kChannels] = {}, out[kBlockSize * kChannels];
    double sum = 0., hp_sum = 0.;
    for (uint32_t b = 0; b < ms * 2; b++)
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
                last_out[c] = v;
            }
        }
    }
    hp_rms = ms ? sqrtf(hp_sum / (ms * 2 * kBlockSize)) : 0.f;
    return ms ? sqrtf(sum / (ms * 2 * kBlockSize)) : 0.f;
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

// The card across power cycles: a case can leave its card for the next (the cases run in order)
static std::string card_file;
static void KeepCard()
{
    std::ofstream f(card_file, std::ios::binary);
    for (auto& c : CardFiles())
        f << c.first << '\n' << c.second.size() << '\n' << c.second;
}
static void TakeCard()
{
    std::ifstream f(card_file, std::ios::binary);
    std::string path, len;
    while (std::getline(f, path) && std::getline(f, len))
    {
        std::string body(std::stoul(len), '\0');
        f.read(&body[0], body.size());
        CardFiles()[path] = body;
    }
}
static std::string Card(const char* path)
{
    auto it = CardFiles().find(path);
    return it == CardFiles().end() ? "" : it->second;
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

static void Tap(const char* key, uint32_t ms = 60)
{
    Press(key, true);
    RunMs(ms);
    Press(key, false);
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

/** SAVE, a slot (1-4), CHOMPI to confirm */
static void Save(int slot)
{
    Tap("KEY_25");
    RunMs(100);
    Tap(("KEY_" + std::to_string(16 + slot)).c_str());
    RunMs(300);
    Tap("KEY_26");
    RunMs(300);
}
static bool Same(const Rgb& a, const Rgb& b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

// MIDI (MidiControl.h): bytes into the jack, or over USB; FRIZZ listens on channel 16 at first
static void Trs(std::initializer_list<int> bytes)
{
    for (int b : bytes)
        Midi(static_cast<uint8_t>(b));
}
static void Usb(std::initializer_list<int> bytes)
{
    for (int b : bytes)
        UsbMidi(static_cast<uint8_t>(b));
}
static const int kNoteOn = 0x9F, kNoteOff = 0x8F, kCC = 0xBF, kPC = 0xCF;
static const int kFilterNote = 55; // KEY_5, the 5th white key: G above the base note, 48
static const int kChompiNote = 45; // the CHOMPI key: SHIFT
static const int kFilterLatchCC = 24, kFilterCutoffCC = 86, kCompAmountCC = 52;
static const int kHpCueCC = 59, kMonoCCNum = 60; // the headphone feed: 0 the master out, 127 the input alone
/** 14 bits of an answer as a knob's 0-1 (MidiControl.h's MidiToKnob), and 7 bits of a CC */
static float KnobOf(int hi, int lo)
{
    const float v = static_cast<float>((hi << 7) | lo);
    return v <= 8192.f ? .5f * v / 8192.f : .5f + .5f * (v - 8192.f) / 8191.f;
}
static float KnobOf7(int v) { return v <= 64 ? .5f * v / 64.f : .5f + .5f * (v - 64) / 63.f; }
/** A SysEx query over USB and its answer's data, or empty if none came */
static std::string Ask(std::initializer_list<int> query)
{
    TakeUsbOut();
    Usb({0xF0, 0x7D, 0x43, 0x48});
    Usb(query);
    Usb({0xF7});
    RunMs(20);
    const std::string out = TakeUsbOut();
    if (out.size() < 7 || out.compare(0, 4, "\xF0\x7D\x43\x48") != 0
        || static_cast<uint8_t>(out[4]) != (*query.begin() | 0x40))
        return "";
    return out.substr(5, out.size() - 6);
}

// boot, the rainbow, and the play page ready
static const uint32_t kReadyMs = 6000;

static std::vector<std::pair<const char*, std::function<void()>>> cases;

int main()
{
    cases.push_back({"boot", [] {
        const float booting = RunMs(1000);
        Check(booting == 0.f, "boot: the outputs stay muted while the boot animation runs");
        RunMs(kReadyMs - 1000);
        Check(RunMs(200) > .05f, "boot: then the input reaches the master out");
        bool all_dim = true;
        for (int led : {24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 13, 12})
            all_dim &= Max(SmtLedFull(led)) > 0 && Max(SmtLedFull(led)) < 80;
        Check(all_dim, "boot: every FX key glows dimly in its colour");
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

        // the compressor: threshold off, makeup +12dB by NRPN 1/55: only that gain
        Latch("KEY_3"); // the folder off again
        RunMs(300);
        const float flat = RunMs(300);
        Trs({kCC, 99, 1, kCC, 98, 55, kCC, 6, 64, kCC, 38, 0});
        RunMs(300);
        const std::string comp = Ask({0x21, 13});
        Check(comp.size() == 17 && comp[15] == 64 && comp[16] == 0,
              "fx-level: NRPN 1/55 is the compressor's page-2 knob 4, its makeup");
        const float made_up = RunMs(300);
        printf("      makeup .5: %.1fdB\n", 20.f * log10f(made_up / flat));
        Check(fabsf(20.f * log10f(made_up / flat) - 12.f) < .5f,
              "fx-level: the compressor's makeup at .5 is +12dB, and nothing else changes");
        Trs({kCC, 52, 40}); // a threshold, no makeup: quieter, nothing gives it back
        Trs({kCC, 99, 1, kCC, 98, 55, kCC, 6, 0, kCC, 38, 0});
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
    // white there, blue below, orange above; Level the same around 0dB; others no white
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
        Tap("ENC_4_SW"); // page 2: Level at 0dB, white
        RunMs(300);
        bool lvl_white = false;
        for (int i = 0; i < 700 && !lvl_white; i++)
        {
            RunMs(1);
            lvl_white = white(PthLedFull(kKnob1Led + 3)); // pulsing: white at its brightest
        }
        Check(lvl_white, "knob-colors: page 2's Level at 0dB, white");
    }});

    // FX page 2 (#35): a plain knob press turns all four knobs over, and back; their LEDs
    // pulse there. Another FX or the compressor goes back to page 1
    cases.push_back({"fx-page2", [] {
        RunMs(kReadyMs);
        // how far knob 1's LED swings over 700 ms: steady on page 1, a pulse on page 2
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
            return state.empty() ? -1 : state.back();
        };
        Tap("KEY_2"); // the shifter on the knobs
        RunMs(200);
        Check(swing() < 10 && page() == 0, "fx-page2: page 1 at first, its LEDs steady");
        Check(Max(PthLedFull(kKnob1Led + 1)) > 40, "fx-page2: page 1's knob 2 lit");
        Tap("ENC_4_SW"); // knob 1 pressed
        RunMs(100);
        Check(page() == 1, "fx-page2: a plain knob press turns to page 2");
        Check(swing() > 100, "fx-page2: page 2's LEDs pulse");
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
        RunMs(100);
        Check(page() == 0 && swing() < 10, "fx-page2: a press on another knob turns back to page 1");
        Tap("ENC_4_SW");
        RunMs(100);
        Tap("KEY_2"); // the same FX again: still page 2
        RunMs(100);
        Check(page() == 1, "fx-page2: pressing the same FX keeps page 2");
        Tap("KEY_5"); // another FX: page 1
        RunMs(100);
        Check(page() == 0, "fx-page2: another FX goes back to page 1");
        Tap("KEY_10"); // the tape stop: a page 2 too, now every effect has one
        RunMs(100);
        Tap("ENC_4_SW");
        RunMs(100);
        Check(page() == 1 && swing() > 100, "fx-page2: the tape stop has a page 2 too");
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
        Check(page() == 1 && swing() > 100, "fx-page2: the compressor has a page 2 too");
        // a page-2 value is part of a scene: saved with it, the blank scene puts it back
        Tap("KEY_2");
        RunMs(100);
        Save(1);
        RunMs(2500);
        const std::string file = Card("/FRIZZ/frizz_scenes.txt");
        Check(file.find("layout 3\n") != std::string::npos
                  && file.find("shifter 0 500000 0 0 0 500000 500000 500000 750000") != std::string::npos,
              "fx-page2: a scene keeps page 2 on the card, after page 1's four");
        Tap("ENC_4_SW");
        RunMs(100);
        const std::string state = Ask({0x20});
        Check(page() == 1 && state.size() > 6 && !(state[6] & 1),
              "fx-page2: turning the page leaves the scene unedited");
        Tap("KEY_16"); // the blank scene
        RunMs(300);
        Check(mix() == 1.f, "fx-page2: the blank scene puts the mix back on its default");
        Tap("KEY_17");
        RunMs(300);
        Check(fabsf(mix() - .5f) < 1e-3f, "fx-page2: recalling the scene brings it back");
        Check(page() == 1, "fx-page2: a recall keeps the page");
        // MIDI: the knob press's note turns the page; page 2 by NRPN in bank 1
        Trs({kNoteOn, 36, 100});
        RunMs(50);
        Trs({kNoteOff, 36, 0});
        RunMs(50);
        Check(page() == 0, "fx-page2: note 36, knob 1's press, turns the page too");
        Trs({kCC, 99, 1, kCC, 98, 74, kCC, 6, 0, kCC, 38, 0}); // the shifter's knob 1: CC 74
        RunMs(50);
        Check(mix() == 0.f, "fx-page2: NRPN 1/74 is the shifter's page-2 knob 1");
        Trs({kCC, 99, 1, kCC, 98, kFilterLatchCC, kCC, 6, 127}); // not a knob: nothing
        RunMs(50);
        Check(Max(SmtLedFull(kFilterKeyLed)) < 80, "fx-page2: NRPN 1 outside the knobs does nothing");
    }});

    // a scene file from v0.11, four values a line: page 2 loads on its defaults, so a latched
    // shifter keeps its full mix and sounds as it did
    cases.push_back({"fx-page2-v011", [] {
        CardFiles()["/FRIZZ/frizz_scenes.txt"]
            = "FRIZZ scenes 1\nscene 1\nshifter 1 791667 0 0 0\nfilter 0 300000 500000 0 666700\n";
        RunMs(kReadyMs);
        Tap("KEY_17"); // scene 1
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
        Check(Max(PthLedFull(kLoopLed)) == 0 && Max(PthLedFull(kPlayLed)) == 0, "looper: LOOP erases it");
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
        bool amber = true;
        for (int i = 0; i < kNumPthLeds; i++)
            amber &= PthLedFull(i).r > 100 || Max(PthLedFull(i)) == 0;
        Check(Powered() && amber, "battery: below 3V, every panel LED flashes yellow");
        RunMs(16000);
        Check(!Powered(), "battery: 15 s later it switches itself off");
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
        Known(red > 0, "battery-level: red below 3V, as MANUAL.md says (the yellow countdown comes first: #43)");
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
        Tap("KEY_17");
        RunMs(300);
        const std::vector<int> recalled = Watch(false, kSlot1Led, 1000);
        Check(*std::min_element(recalled.begin(), recalled.end()) > 240
                  && Max(SmtLedFull(kSlot1Led + 1)) < 60,
              "scene-leds: a recall makes its key the bright one, steady, and the other dim");
    }});

    // a morph: its key blinks on the beat; SHIFT + another scene key, or a 9th bar, blinks red
    cases.push_back({"scene-morph", [] {
        RunMs(kReadyMs);
        Latch("KEY_5");
        Save(1);
        RunMs(1000);
        Latch("KEY_2");
        Save(2);
        RunMs(1000);
        Tap("KEY_17");
        RunMs(500);
        Press("KEY_26", true); // SHIFT held: the morph waits for it
        RunMs(80);
        Tap("KEY_18");
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
        // another scene key: refused, 3 red blinks on it
        Check(RedBlinks("KEY_17", kSlot1Led) == 3 && Ask({0x20}).size() > 6 && (Ask({0x20})[6] & 2),
              "scene-morph: SHIFT + another scene key during it blinks that key red 3 times, and it morphs on");
        // the same key again adds bars, 8 in all; a 9th is refused
        int refused = 0;
        for (int bar = 2; bar <= 8; bar++)
            refused += RedBlinks("KEY_18", kSlot1Led + 1);
        Check(refused == 0 && RedBlinks("KEY_18", kSlot1Led + 1) == 3,
              "scene-morph: the same key adds bars up to 8 in all; a 9th blinks it red 3 times");
        Press("KEY_26", false);
        RunMs(20000);
        Check(Ask({0x20}).size() > 6 && !(Ask({0x20})[6] & 2) && Ask({0x20})[5] == 3,
              "scene-morph: SHIFT let go, it glides and lands on scene 2");
    }});

    // the chaos key (#65), the 11th white key: the effects latched drop out on random steps
    cases.push_back({"chaos-gates", [] {
        RunMs(kReadyMs);
        const float dry = RunMs(300);
        static const int kChaosKeyLed = 14;
        Latch("KEY_5");
        Turn(kKnob1Encoder, -60); // the filter closed: quiet while it's in
        RunMs(300);
        const float closed = RunMs(300);
        Latch("KEY_11");
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
        Latch("KEY_11");
        RunMs(300);
        int open_off = 0;
        for (int w = 0; w < 32; w++)
            open_off += RunMs(62) > dry * .5f;
        Check(open_off == 0 && Max(SmtLedFull(kFilterKeyLed)) > 200,
              "chaos-gates: chaos unlatched, the filter steady");

        // over MIDI: CC 119 latches it, NRPN 0/122 its FX chance
        const std::string before = Ask({0x21, 12});
        Trs({kCC, 99, 0, kCC, 98, 122, kCC, 6, 0, kCC, 38, 0});
        Trs({kCC, 119, 127});
        RunMs(300);
        const std::string st = Ask({0x20}), after = Ask({0x21, 12});
        Check(st.size() > 9 && ((static_cast<uint8_t>(st[8]) << 7 | static_cast<uint8_t>(st[9])) >> 12 & 1),
              "chaos-gates: CC 119 latches the chaos key");
        Check(before.size() > 2 && after.size() > 2 && before[1] != 0 && after[1] == 0 && after[2] == 0,
              "chaos-gates: NRPN 0/122 sets its FX chance (to 0)");
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
        Latch("KEY_11");
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
        Latch("KEY_11");
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
        printf("      dry %.3f, filter closed %.3f\n", dry, closed);
        Check(closed < dry * .3f, "scene-fader: scene 1, the filter closed, is quiet");

        Press("KEY_26", true);
        RunMs(80);
        Tap("KEY_16"); // to the blank scene, held
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
        Check(RunMs(300) < closed * 1.3f, "scene-fader: and all the way back, scene 1 again");
        RunMs(5000);
        Check(state().size() > 6 && (state()[6] & 2) && RunMs(300) < closed * 1.3f,
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
        Tap("KEY_17");
        RunMs(500);
        Press("KEY_26", true);
        RunMs(80);
        Tap("KEY_16");
        RunMs(100);
        Turn(5, 30);
        RunMs(300);
        Press("KEY_26", false);
        RunMs(500);
        st = state();
        Check(st.size() > 9 && !(st[6] & 3) && st[5] == 1 && st[9] == 0 && RunMs(300) > dry * .85f,
              "scene-fader: let go at the end, it lands on the blank scene, nothing latched");
        Tap("KEY_17");
        RunMs(500);
        Press("KEY_26", true);
        RunMs(80);
        Tap("KEY_16");
        RunMs(100);
        Turn(5, 6);
        RunMs(300);
        Turn(5, -6);
        RunMs(300);
        Press("KEY_26", false);
        RunMs(500);
        st = state();
        Check(st.size() > 9 && !(st[6] & 3) && st[5] == 2 && (st[9] & (1 << 4))
                  && RunMs(300) < closed * 1.3f,
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

    // the crossfader over MIDI (CC 118): a morph started by CC 62, taken to where the CC says
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
        Trs({kCC, 62, 0}); // to the blank scene, gliding over a bar
        RunMs(20);
        Trs({kCC, 118, 0});
        RunMs(3000);
        Check(state().size() > 6 && (state()[6] & 2) && RunMs(300) < closed * 1.3f,
              "midi fader: CC 118 takes the morph; at 0 it waits at the start, past the bar line");
        Trs({kCC, 118, 64});
        RunMs(400);
        const float half = RunMs(300);
        printf("      dry %.3f, closed %.3f, at 64 %.3f\n", dry, closed, half);
        Check(half > closed * 1.5f && half < dry * .9f, "midi fader: at 64, halfway");
        Trs({kCC, 118, 0});
        RunMs(300);
        std::string st = state();
        Check(st.size() > 9 && !(st[6] & 3) && st[5] == 2 && (st[9] & (1 << 4)),
              "midi fader: back to 0, scene 1 as it was");
        Trs({kCC, 62, 0});
        RunMs(20);
        Trs({kCC, 118, 127});
        RunMs(300);
        st = state();
        Check(st.size() > 9 && !(st[6] & 3) && st[5] == 1 && st[9] == 0 && RunMs(300) > dry * .85f,
              "midi fader: at 127, landed on the blank scene");
        Trs({kCC, 118, 64});
        RunMs(300);
        Check(state().size() > 6 && !(state()[6] & 2), "midi fader: without a morph, nothing");
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
        const int blinks = RedBlinks("KEY_19", kSlot1Led + 2); // to 400 ms past the wrap
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
        Tap("KEY_25");
        RunMs(100);
        Tap("KEY_18");
        const std::vector<int> pending = Runs(Watch(true, 0, 1200), 128);
        bool steady = pending.size() >= 3;
        for (int r : pending)
            steady &= r >= 230 && r <= 270;
        Check(steady, "clock-wrap: CHOMPI blinks steadily while a save waits after it");
    }});

    // ---- PR #7's hardware checklist, as far as it isn't about the CPU ----

    cases.push_back({"save-latch", [] {
        RunMs(kReadyMs);
        Tap("KEY_25"); // SAVE, slot 1 picked: CHOMPI would confirm
        RunMs(100);
        Tap("KEY_17");
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
        Press("KEY_10", true);
        RunMs(80);
        Press("KEY_26", true);
        RunMs(80);
        Turn(5, 4);
        RunMs(200);
        Press("KEY_10", false);
        RunMs(100);
        Press("KEY_26", false);
        RunMs(300);
        Check(Max(SmtLedFull(kTapeStopKeyLed)) > 2 * off, "latch-turn: FX key, CHOMPI held, transport turned, key let go: still latched");
        Tap("KEY_10");
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
        const Rgb stereo = SmtLedFull(kMonoKeyLed);
        Tap("KEY_18"); // F#: mono
        RunMs(3000);
        Check(Card("/FRIZZ/frizz_master.txt").find("mono 1") != std::string::npos,
              "settings-mono: F# of the lower octave switches the input to mono, and it's saved");
        const Rgb mono = SmtLedFull(kMonoKeyLed);
        Check(mono.r == mono.g && mono.g == mono.b && Max(mono) > 2 * Max(stereo),
              "settings-mono: its key is lit white while mono, dim while stereo");
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
        Tap("KEY_25"); // SAVE, slot 1 picked
        RunMs(100);
        Tap("KEY_17");
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
        Usb({kCC, kFilterCutoffCC, 0});
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
        // the 11th white key is the chaos key's there: dimly in its own colour, rose
        const Rgb chaos = SmtLedFull(kChannel11Led);
        Check(Max(chaos) < 60 && chaos.r > 2 * chaos.g && Max(SmtLedFull(kChannel14Led)) == 0,
              "settings-channel: back on the play page, the 14th key dark, the 11th the chaos key's, dim");
        SetToggle(true);
        RunMs(300);
        Tap("KEY_17"); // D#: transport following
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
        Tap("ENC_6_SW"); // page 2, the input gain: blue to red, never yellow
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

    // the LEDs' brightness: 100, 75, 50 %, no colour gone dark
    cases.push_back({"settings-brightness", [] {
        RunMs(kReadyMs);
        RunMs(300);
        const Rgb filter = SmtLed(kFilterKeyLed), vu = PthLed(kVolumeLed);
        SetToggle(true);
        RunMs(300);
        const int key_full = Max(SmtLed(kBrightnessKeyLed));
        Tap("KEY_20"); // A#: 75 %
        RunMs(300);
        const int key_75 = Max(SmtLed(kBrightnessKeyLed));
        Tap("KEY_20"); // 50 %
        RunMs(3000);
        Check(Card("/FRIZZ/frizz_master.txt").find("led_brightness 50") != std::string::npos,
              "settings-brightness: A# pressed twice steps 100, 75, 50 %, saved");
        const int key_50 = Max(SmtLed(kBrightnessKeyLed));
        Check(key_full > key_75 && key_75 > key_50 && key_50 > 0,
              "settings-brightness: its own key dims with every LED, so it shows the level");
        SetToggle(false);
        RunMs(300);
        const Rgb half = SmtLed(kFilterKeyLed);
        Check(abs(Max(half) - Max(filter) / 2) <= 1 && half.r > 0 && half.g > 0 && half.b > 0,
              "settings-brightness: an FX key that's off at half, its colour kept");
        Check(Max(PthLed(kVolumeLed)) > 0 && Max(PthLed(kVolumeLed)) < Max(vu),
              "settings-brightness: the panel's dimmer too, still lit");
        SetToggle(true);
        RunMs(300);
        Tap("KEY_20"); // and round to 100 %
        SetToggle(false);
        RunMs(300);
        Check(Same(SmtLed(kFilterKeyLed), filter), "settings-brightness: a third press is back to full");
    }});

    // the switch up from power-on: the settings page once booted
    cases.push_back({"settings-boot", [] {
        SetToggle(true);
        RunMs(kReadyMs);
        const float dry = RunMs(300);
        Press("KEY_10", true); // the tape stop's key: channel 10 here
        RunMs(1500);
        const float held = RunMs(300);
        Press("KEY_10", false);
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
        Check(Same(SmtLedFull(kSourceKeyLed), Rgb{252, 252, 252}), "clock-source: upper G# white for Auto");
        Tap("KEY_24"); // TRS
        RunMs(2000);
        Check(Probe().source == 1 && Probe().tempo == 120 && Same(SmtLedFull(kSourceKeyLed), Rgb{252, 152, 60}),
              "clock-source: TRS: the jack's still, the key orange");
        Tap("KEY_24"); // USB
        RunMs(300);
        Check(Probe().source == 2, "clock-source: USB: the lock moves to USB's ticks at once");
        RunMs(2000);
        Check(Probe().tempo == 90 && Same(SmtLedFull(kSourceKeyLed), Rgb{0, 0, 252}),
              "clock-source: and the effects follow its 90, the key blue");
        Tap("KEY_24"); // internal
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
        Tap("KEY_24"); // Auto again
        RunMs(2000);
        Check(Probe().has_clock, "clock-source: a fourth press is Auto again: a clock counts");
    }});

    // MIDI out (MidiOut.h): off, the jack, the jack and USB, on the upper D#
    cases.push_back({"midi-out-key", [] {
        RunMs(kReadyMs);
        SetToggle(true);
        RunMs(300);
        const Rgb off = SmtLedFull(kMidiOutKeyLed);
        Check(off.r == off.g && off.g == off.b && off.r > 0 && off.r < 60,
              "midi-out-key: upper D# dim white: off at first");
        TakeMidiOut();
        RunMs(500);
        Check(TakeMidiOut().empty(), "midi-out-key: and nothing goes out");
        Tap("KEY_22");
        RunMs(500);
        std::vector<MidiOutByte> sent = TakeMidiOut();
        bool trs = !sent.empty(), usb = false;
        for (const MidiOutByte& m : sent)
            usb = usb || m.usb;
        Check(Same(SmtLedFull(kMidiOutKeyLed), Rgb{252, 152, 60}) && trs && !usb,
              "midi-out-key: a press: the jack, orange, its clock going out");
        Tap("KEY_22");
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
        Tap("KEY_22");
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

    // the transport while a loop plays: semitones, 2 detents each, stopping at 2x and 1/16x;
    // SHIFT + turn the ladder of roots and fifths, 4 detents a step, the only way to reverse
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
        Turn(5, -200);
        const float slowest = speed();
        Check(near(slowest, -48), "transport-semitones: all the way left: 1/16x, still forward");
        shift_turn(-4);
        Check(speed() < 0.f && near(-Probe().loop_speed, -48), "transport-semitones: SHIFT + left from there: reverse");
        shift_turn(4);
        Check(near(speed(), -48), "transport-semitones: SHIFT + right: forward again");
        Tap("ENC_5_SW"); // the transport's press: back to 1x
        Check(near(speed(), 0), "transport-semitones: press: 1x");
        // over MIDI: CC 18 turns in semitones; with the CHOMPI key's note held, along the ladder
        Trs({kCC, 18, 1, kCC, 18, 1});
        Check(near(speed(), 1), "transport-semitones: CC 18, 2 detents: a semitone");
        Trs({kNoteOn, kChompiNote, 100});
        RunMs(60);
        for (int i = 0; i < 4; i++)
            Trs({kCC, 18, 1});
        RunMs(60);
        Trs({kNoteOn, kChompiNote, 0});
        Check(near(speed(), 7), "transport-semitones: CHOMPI's note held, CC 18 x4: the ladder, +7");
    }});

    cases.push_back({"flanger-click", [] {
        amp = .5f;
        RunMs(kReadyMs);
        Latch("KEY_6");
        Turn(2, 60); // the amount up
        RunMs(800);
        Save(1); // stereo at 0
        // stereo up: the right LFO runs free and drifts; then a recall takes it back to 0 within
        // 25 ms (clicks.cpp's case, on the device). How far they drifted decides the jump, so
        // after several times
        max_step = 0.f; // over the presses too: the jump comes as the recall lands
        for (uint32_t drift : {6000, 1500, 2500, 3500, 4500})
        {
            Turn(3, 50);
            RunMs(drift);
            Tap("KEY_17");
            RunMs(600);
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
        Tap("KEY_25");
        RunMs(100);
        Tap("KEY_17");
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
        Latch("KEY_10"); // the tape stop: silence on the master
        RunMs(3000);
        const float stopped = RunMs(500), hp_stopped = hp_rms;
        Check(stopped < .01f * master && hp_stopped < .01f * master,
              "headphones: a latched tape stop silences them with the master");
        Usb({kCC, kHpCueCC, 127});
        RunMs(300);
        const float cued = RunMs(500), hp_cued = hp_rms;
        Check(cued < .01f * master && hp_cued > .5f * master,
              "headphones: with the cue up, the input alone, while the master stays silent");
        Usb({kCC, kHpCueCC, 0});
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
        Tap("KEY_18");           // the card's scene in slot 2
        run(400);
        Save(3);                 // a change to the card after power-on
        run(300);
        Tap("KEY_6");
        run(1000);
        // MIDI: a note holding the filter, a CC, FRIZZ's SysEx key, all played in again
        Trs({kNoteOn, kFilterNote, 100, kCC, kFilterCutoffCC, 20});
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
                  && log.find("\nmidi BF 56 14\n") != std::string::npos
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
        Trs({kCC, kFilterCutoffCC, 0});
        RunMs(500);
        Check(RunMs(300) < dry * .5f, "midi notes: and a CC closes its cutoff");
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

    // controllers: latches, parameters, knob turns, the compressor, the mix
    cases.push_back({"midi-cc", [] {
        RunMs(kReadyMs);
        const float dry = RunMs(300);
        Trs({kCC, kFilterCutoffCC, 10, kCC, kFilterLatchCC, 127});
        RunMs(500);
        Check(RunMs(300) < dry * .5f, "midi cc: CC 24 latches the filter, CC 86 sets its cutoff");
        Check(Max(SmtLedFull(kFilterKeyLed)) > 80, "midi cc: its key lit");
        Trs({kCC, kFilterLatchCC, 0});
        RunMs(300);
        Check(fabsf(RunMs(300) - dry) < dry * .05f, "midi cc: CC 24 at 0 unlatches it");
        // relative: CC 14 turns knob 1 as its detents do, here the filter's
        Tap("KEY_5");
        RunMs(200);
        const Rgb before = PthLedFull(kKnob1Led);
        Trs({kCC, 14, 127, kCC, 14, 127, kCC, 14, 127}); // -1, three times
        RunMs(100);
        Check(!Same(PthLedFull(kKnob1Led), before), "midi cc: CC 14 turns knob 1");
        // NRPN: CC 86 in 14 bits, the cutoff's centre exactly
        Trs({kCC, 99, 0, kCC, 98, kFilterCutoffCC, kCC, 6, 64, kCC, 38, 0});
        RunMs(50);
        const std::string params = Ask({0x21, 4});
        Check(params.size() == 17 && params[1] == 64 && params[2] == 0,
              "midi cc: NRPN 86 sets the cutoff in 14 bits, 8192 its centre");
        Trs({kCC, kCompAmountCC, 127});
        RunMs(50);
        const std::string comp = Ask({0x21, 13});
        Check(comp.size() == 17 && comp[1] == 127, "midi cc: CC 52 is the compressor's threshold");
        Trs({kCC, 56, 0}); // output gain
        RunMs(300);
        Check(RunMs(300) < .001f, "midi cc: CC 56 sets the output gain");
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
        Check(Ask({0x23}).size() == 4, "midi sysex: 23 answers the load");
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
        Trs({kCC, 62, 1}); // morph to it: over a bar at 120 BPM, 2 s
        RunMs(200);
        const float midway = RunMs(100);
        RunMs(3000);
        Check(midway > dry * .5f && RunMs(300) < dry * .5f, "midi scenes: CC 62 morphs to it, landing on the bar");
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x31, 0, 8, 1, 0, 0, 0xF7});
        RunMs(20);
        const std::string refused = TakeUsbOut();
        Check(refused.size() == 9 && refused[7] == 1, "midi scenes: never into the blank scene");

        // CC 61 sets a morph's bars: the same morph 4 bars later (the same phase of the bar,
        // 2 s each at 120 BPM) over 2 bars lands a bar later than over 1
        auto morph_ms = [](int bars) {
            Trs({kPC, 0});
            RunMs(500);
            Trs({kCC, 61, bars, kCC, 62, 1});
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
              "midi scenes: CC 61 = 2 makes a CC 62 morph land a bar later");
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
        for (int i = 0; i < 24; i++)
            Trs({kCC, 18, 1});
        RunMs(1000);
        Check(Probe().loop_speed > 1.9f * speed, "midi cc more: CC 18 turns the transport: the loop speeds up");
        for (int i = 0; i < 24; i++)
            Trs({kCC, 18, 127});
        RunMs(1000);
        Check(fabsf(Probe().loop_speed - speed) < .02f, "midi cc more: and back down (the speed glides there)");
        Tap("KEY_28"); // erase it
        RunMs(1000);
        Check(Probe().loop_state == 0, "midi cc more: (the loop erased)");
        const int out_gain = at14(state(), 15);
        for (int i = 0; i < 20; i++)
            Trs({kCC, 19, 127});
        RunMs(500);
        Check(at14(state(), 15) < out_gain && RunMs(300) < dry * .8f,
              "midi cc more: CC 19 turns VOLUME: the master out down");
        for (int i = 0; i < 20; i++)
            Trs({kCC, 19, 1});
        RunMs(500);
        Check(at14(state(), 15) == out_gain, "midi cc more: and up again");

        Trs({kCC, 53, 0, kCC, 54, 64, kCC, 55, 127});
        RunMs(50);
        const std::string comp = Ask({0x21, 13});
        Check(comp.size() == 17 && at14(comp, 3) == 0 && at14(comp, 5) == 8192 && at14(comp, 7) == 16383,
              "midi cc more: CCs 53-55 are the compressor's knobs 2-4");

        const int in_gain = at14(state(), 17);
        Trs({kCC, 57, 0});
        RunMs(300);
        Check(at14(state(), 17) == 0 && RunMs(300) < .001f, "midi cc more: CC 57 sets the input gain: 0 silences it");
        Trs({kCC, 57, 127});
        RunMs(300);
        Check(at14(state(), 17) == 16383 && at14(state(), 17) != in_gain,
              "midi cc more: and 127 turns it all the way up");
        Trs({kCC, 58, 32});
        RunMs(50);
        Check(at14(state(), 13) == 4096, "midi cc more: CC 58 sets the mix");
        Trs({kCC, kMonoCCNum, 127});
        RunMs(50);
        Check(state().size() > 6 && (state()[6] & 32), "midi cc more: CC 60 at 127 makes the input mono");
        Trs({kCC, kMonoCCNum, 0});
        RunMs(50);
        Check(state().size() > 6 && !(state()[6] & 32), "midi cc more: and at 0 stereo");

        // CC 61 past 8 is 8 bars: a morph over CC 62 still going after 15 s, landed by 17 s
        Latch("KEY_5");
        Save(1);
        RunMs(1000);
        Tap("KEY_5");
        RunMs(300);
        Trs({kCC, 61, 127, kCC, 62, 1});
        RunMs(15000);
        const bool going = state().size() > 6 && (state()[6] & 2);
        RunMs(2500);
        Check(going && !(state()[6] & 2), "midi cc more: CC 61 above 8 makes a morph take 8 bars");
        // CC 63 stops one where it is
        Tap("KEY_5");
        RunMs(300);
        Trs({kCC, 61, 1, kCC, 62, 1});
        RunMs(500);
        const bool started = state()[6] & 2;
        Trs({kCC, 63, 127});
        RunMs(50);
        Check(started && !(state()[6] & 2), "midi cc more: CC 63 stops a morph before its bar");

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
        Trs({kCC, kFilterCutoffCC, 64});
        RunMs(500);
        max_step = 0.f;
        for (int v = 63; v >= 0; v--) // 0.5 down over 400 ms, 1/128 a message
        {
            Trs({kCC, kFilterCutoffCC, v});
            RunMs(6);
        }
        RunMs(200);
        const float by_midi = max_step;
        printf("      largest step: turned %.4f, automated %.4f\n", by_hand, by_midi);
        Check(by_midi <= by_hand * 1.25f, "midi automation: a CC sweep steps no harder than the knob turned as fast");
        // 3000 controllers over USB in 1.5 s, twice a ms, then a last one
        for (int i = 0; i < 1500; i++)
        {
            Usb({kCC, kFilterCutoffCC, (i * 37) % 128, kCC, kFilterCutoffCC + 1, (i * 11) % 128});
            RunMs(1);
        }
        Usb({kCC, kFilterCutoffCC, 100});
        RunMs(100);
        const std::string params = Ask({0x21, 4});
        Check(params.size() == 17 && fabsf(KnobOf(params[1], params[2]) - KnobOf7(100)) < 1e-3f,
              "midi automation: 3000 CCs in 1.5 s, and the cutoff lands on the last one");
    }});

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
        static std::list<std::string> names; // the cases keep pointers into it
        const std::string file = path.substr(path.rfind('/') + 1);
        names.push_back("scenario:" + file.substr(0, file.size() - 4));
        cases.push_back({names.back().c_str(), [path] {
            std::ifstream script(path);
            const int failed = PlayScript(script, nullptr, nullptr);
            Check(failed == 0, (path.substr(path.rfind('/') + 1) + ": played from power-on, every expect holds").c_str());
        }});
    }

    char card_name[] = "/tmp/frizz-ui-card-XXXXXX";
    const int card_fd = mkstemp(card_name);
    close(card_fd);
    card_file = card_name;

    // CASES="a b": only those, while working on them (a case that takes the card from the one
    // before it needs that one too)
    const char* only = getenv("CASES");
    int failed = 0;
    for (auto& c : cases)
    {
        if (only && !strstr((" " + std::string(only) + " ").c_str(), (" " + std::string(c.first) + " ").c_str()))
            continue;
        fflush(stdout);
        const pid_t pid = fork();
        if (pid == 0)
        {
            // a replay boots by its script
            if (strncmp(c.first, "bug-replay", 10) != 0 && strncmp(c.first, "scenario:", 9) != 0)
                Boot();
            c.second();
            fflush(stdout);
            _exit(failures > 255 ? 255 : failures);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status))
        {
            printf("FAIL  %s: crashed\n", c.first);
            failed++;
        }
        else
            failed += WEXITSTATUS(status);
    }
    unlink(card_name);
    failures += failed;
    return Finish();
}
