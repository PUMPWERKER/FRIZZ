/** @file script.cpp
 *  @brief The script player (script.h): a script of key presses, knob turns, MIDI and audio
 *  played into the virtual CHOMPI from power-on. README.md lists the commands.
 */
#include "script.h"
#include "clockgen.h"
#include "twin.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace twin
{
static std::vector<float>* out_samples = nullptr; // the master out, interleaved L R
static FILE* led_log = nullptr;
static std::string last_leds;
static int failures = 0;

// the audio into AUX (L and R) and into the mic (a WAV's left): a sine, a WAV file looped,
// or nothing
struct Source
{
    enum { OFF, SINE, WAV } mode = OFF;
    float freq = 220.f, amp = .3f, phase = 0.f;
    std::vector<float> wav; // interleaved L R
    size_t pos = 0;

    void Next(float& l, float& r)
    {
        l = r = 0.f;
        if (mode == SINE)
        {
            l = r = amp * sinf(phase);
            phase += 2.f * float(M_PI) * freq / kSampleRate;
            if (phase > 2.f * float(M_PI))
                phase -= 2.f * float(M_PI);
        }
        else if (mode == WAV && !wav.empty())
        {
            l = wav[pos];
            r = wav[pos + 1];
            pos = (pos + 2) % wav.size();
        }
    }
};
static Source aux_in, mic_in;
// what a computer plays into FRIZZ over USB (#196): while it isn't OFF, it plays
static Source usb_in;

// MIDI clock out of a virtual sequencer on the jack and a DAW over USB (clockgen.h)
static ClockGen clock_trs, clock_usb;

// what FRIZZ sent over USB that the script hasn't looked at yet: TakeUsbOut's, less the
// answers to its own `set`s
static std::string usb_kept;
static std::string TakeUsb()
{
    std::string out = usb_kept + TakeUsbOut();
    usb_kept.clear();
    return out;
}

static std::string Hex(const Rgb& c)
{
    char s[8];
    snprintf(s, sizeof(s), "%02x%02x%02x", c.r, c.g, c.b);
    return s;
}

std::string LedLine()
{
    std::string s = "pth";
    for (int i = 0; i < kNumPthLeds; i++)
        s += " " + Hex(PthLedFull(i));
    s += " | smt";
    for (int i = 0; i < kNumSmtLeds; i++)
        s += " " + Hex(SmtLedFull(i));
    return s;
}

// bytes as " HH HH ..."
static std::string UsbHex(const std::string& bytes)
{
    std::string s;
    for (unsigned char c : bytes)
    {
        char hex[4];
        snprintf(hex, sizeof(hex), " %02X", c);
        s += hex;
    }
    return s;
}

static void LogLeds()
{
    const std::string now = LedLine();
    if (now == last_leds)
        return;
    last_leds = now;
    if (led_log)
        fprintf(led_log, "%7u %s\n", NowMs(), now.c_str());
}

static void RunMs(uint32_t ms)
{
    const size_t blocks = ms * 2;
    float in[kBlockSize * kChannels], out[kBlockSize * kChannels], usb[kBlockSize * 2];
    for (size_t b = 0; b < blocks; b++)
    {
        if (usb_in.mode != Source::OFF)
        {
            for (size_t i = 0; i < kBlockSize; i++)
                usb_in.Next(usb[i * 2], usb[i * 2 + 1]);
            SetUsbAudioIn(usb);
        }
        clock_trs.Step(BlockMs());
        clock_usb.Step(BlockMs());
        for (size_t i = 0; i < kBlockSize; i++)
        {
            float l, r, mic, unused;
            aux_in.Next(l, r);
            mic_in.Next(mic, unused);
            in[i * kChannels + 0] = mic;
            in[i * kChannels + 1] = 0.f;
            in[i * kChannels + 2] = l;
            in[i * kChannels + 3] = r;
        }
        Run(1, in, out);
        // what came out taken each block, so the twin's queues never fill (twin.h): USB's kept
        // for the script, MIDI out and what the computer records not looked at
        usb_kept += TakeUsbOut();
        static MidiOutByte midi_out[256];
        while (TakeMidiOut(midi_out, 256) == 256)
            ;
        static float recorded[kBlockSize * 2];
        while (TakeUsbAudioOut(recorded, kBlockSize) == kBlockSize)
            ;
        for (size_t i = 0; out_samples && i < kBlockSize; i++)
        {
            out_samples->push_back(out[i * kChannels + 2]);
            out_samples->push_back(out[i * kChannels + 3]);
        }
        if (b % 2 == 1)
            LogLeds();
    }
}

// A WAV file's samples as interleaved stereo floats: 16-bit PCM or 32-bit float, mono or stereo
static bool ReadWav(const char* path, std::vector<float>& to)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 12 || memcmp(d.data(), "RIFF", 4) || memcmp(d.data() + 8, "WAVE", 4))
        return false;
    uint16_t format = 0, channels = 0, bits = 0;
    for (size_t p = 12; p + 8 <= d.size();)
    {
        uint32_t len;
        memcpy(&len, &d[p + 4], 4);
        if (!memcmp(&d[p], "fmt ", 4))
        {
            memcpy(&format, &d[p + 8], 2);
            memcpy(&channels, &d[p + 10], 2);
            memcpy(&bits, &d[p + 22], 2);
        }
        else if (!memcmp(&d[p], "data", 4) && channels)
        {
            const size_t frames = len / (channels * bits / 8);
            for (size_t i = 0; i < frames; i++)
                for (int c = 0; c < 2; c++)
                {
                    const size_t at = p + 8 + (i * channels + (c < channels ? c : 0)) * bits / 8;
                    float v = 0.f;
                    if (format == 1 && bits == 16)
                    {
                        int16_t s;
                        memcpy(&s, &d[at], 2);
                        v = s / 32768.f;
                    }
                    else if (format == 3 && bits == 32)
                        memcpy(&v, &d[at], 4);
                    else
                        return false;
                    to.push_back(v);
                }
            return true;
        }
        p += 8 + len + (len & 1);
    }
    return false;
}

std::string SineWav(uint32_t frames, int channels, float freq, uint32_t beats)
{
    if (channels != 1 && channels != 2)
        return "";
    std::string w = "RIFF";
    auto u32 = [&](std::string& to, uint32_t v) {
        for (int i = 0; i < 4; i++)
            to.push_back(static_cast<char>(v >> (8 * i)));
    };
    const uint32_t bytes = frames * 2u * static_cast<uint32_t>(channels);
    u32(w, 36 + bytes + (beats ? 32 : 0));
    w += "WAVEfmt ";
    for (uint32_t v : {16u, 1u | static_cast<uint32_t>(channels) << 16, 48000u,
                       48000u * 2u * static_cast<uint32_t>(channels),
                       2u * static_cast<uint32_t>(channels) | 16u << 16})
        u32(w, v);
    if (beats)
    {
        w += "acid";
        for (uint32_t v : {24u, 0u, 60u, 0u, beats, 4u | 4u << 16, 0x42f00000u}) // 120.0f
            u32(w, v);
    }
    w += "data";
    u32(w, bytes);
    for (uint32_t i = 0; i < frames; i++)
        for (int c = 0; c < channels; c++)
        {
            const float ph = 6.2831853f * freq * static_cast<float>(i) / 48000.f + c * 1.5707963f;
            const int16_t v = static_cast<int16_t>(lrintf(.3f * 32767.f * sinf(ph)));
            w.push_back(static_cast<char>(v & 0xFF));
            w.push_back(static_cast<char>((v >> 8) & 0xFF));
        }
    return w;
}

// `set`'s groups and names, as remote.py's set has them: the effects by FxId, then the
// developers' SysEx's groups (MidiControl.h's namespace dev)
static const char* const kFxNames[] = {"freezer", "shifter",  "folder", "crusher", "filter",
                                       "flanger", "resonator", "slicer", "warble",  "tapestop",
                                       "delay",   "reverb",    "phaser", "chaos"};
static const int kNumFxNames = sizeof(kFxNames) / sizeof(kFxNames[0]);
static const int kGroupComp = 0x20, kGroupLatch = 0x21, kGroupVolume = 0x22, kGroupSettings = 0x23;
static const char* const kVolumeNames[] = {"out", "in", "mix", "headphones"};
// masterfile::kLines' order (MasterSettings.h)
static const char* const kSettingNames[] = {
    "mono",     "midi_channel",  "midi_transport", "clock_factor", "clock_source",
    "midi_out", "led_brightness", "midi_block_trs", "midi_feedback", "aux_dry",
    "gain_aux", "gain_mic",       "mic_dry",        "aux_live",      "mic_live",
    "gain_usb", "usb_dry",        "usb_live",       "usb_audio"};

static int IndexOf(const char* const* names, int count, const std::string& name)
{
    for (int i = 0; i < count; i++)
        if (name == names[i])
            return i;
    return -1;
}

/** A whole number 0..max from text, or -1 */
static long Whole(const std::string& s, long max)
{
    char* end = nullptr;
    const long v = strtol(s.c_str(), &end, 10);
    return s.empty() || *end || v < 0 || v > max ? -1 : v;
}

/** set GROUP INDEX VALUE: the developers' SET over USB, as remote.py's set sends it, played
 *  until FRIZZ answers (MainLoop does, within a few ms; from power-on, once it runs), the
 *  answer kept from the script's `replies` and `expect usb`. An empty string, or why not */
static std::string DevSet(const std::string& group_name, const std::string& index_name,
                          const std::string& value)
{
    int group = IndexOf(kFxNames, kNumFxNames, group_name);
    const char* const* names = nullptr;
    int count = 0;
    if (group_name == "comp")
        group = kGroupComp;
    else if (group_name == "latch")
    {
        group = kGroupLatch;
        names = kFxNames;
        count = kNumFxNames;
    }
    else if (group_name == "volume")
    {
        group = kGroupVolume;
        names = kVolumeNames;
        count = sizeof(kVolumeNames) / sizeof(kVolumeNames[0]);
    }
    else if (group_name == "settings")
    {
        group = kGroupSettings;
        names = kSettingNames;
        count = sizeof(kSettingNames) / sizeof(kSettingNames[0]);
    }
    if (group < 0)
        return "no group " + group_name;
    long index = names ? IndexOf(names, count, index_name) : -1;
    if (index < 0)
        index = Whole(index_name, 127);
    if (index < 0)
        return "no index " + index_name;
    long raw;
    if (group == kGroupSettings || group == kGroupLatch)
        raw = value == "on" ? 1 : value == "off" ? 0 : Whole(value, 16383);
    else
    {
        // a knob's 0-1 in 14 bits, 8192 its centre (MidiControl.h's KnobToMidi14)
        char* end = nullptr;
        const float k = strtof(value.c_str(), &end);
        raw = value.empty() || *end || !(k >= 0.f && k <= 1.f)
                  ? -1
                  : static_cast<long>((k <= .5f ? k * 2.f * 8192.f : 8192.f + (k - .5f) * 2.f * 8191.f) + .5f);
    }
    if (raw < 0)
        return "no value " + value;

    static uint8_t seq = 0;
    seq = static_cast<uint8_t>(seq % 127 + 1);
    const uint8_t set[] = {0xF0, 0x7D, 0x43, 0x48, 0x3E, 3, seq, static_cast<uint8_t>(group),
                           static_cast<uint8_t>(index), static_cast<uint8_t>(raw >> 7 & 0x7F),
                           static_cast<uint8_t>(raw & 0x7F), 0xF7};
    for (uint8_t b : set)
        UsbMidi(b);
    // FRIZZ answers once its main loop runs (`booted`), the card read
    while (!MainLoopRunning() && NowMs() < 10000)
        RunMs(1);
    // the answer: F0 7D 43 48 7E 03 SEQ STATUS GROUP INDEX F7
    const std::string answer = std::string("\xF0\x7D\x43\x48\x7E\x03", 6) + static_cast<char>(seq);
    for (int ms = 0; ms < 100; ms++)
    {
        RunMs(1);
        usb_kept += TakeUsbOut();
        const size_t at = usb_kept.find(answer);
        const size_t end = at == std::string::npos ? at : usb_kept.find('\xF7', at);
        if (end == std::string::npos)
            continue;
        const uint8_t status = static_cast<uint8_t>(usb_kept[at + answer.size()]);
        usb_kept.erase(at, end + 1 - at);
        return status == 0 ? "" : "refused";
    }
    return "no answer";
}

static void Fail(int line, const std::string& what)
{
    fprintf(stderr, "line %d: %s\n", line, what.c_str());
    failures++;
}

int PlayScript(std::istream& src_in, FILE* leds, std::vector<float>* out)
{
    std::istream* src = &src_in;
    led_log = leds;
    out_samples = out;
    failures = 0;
    usb_kept.clear();
    std::string text;
    int line_no = 0;
    uint32_t at_base = 0; // `at` counts from here: power-on, or the main loop's start (booted)
    bool booted = false;
    auto boot = [&]() {
        if (!booted)
            Boot();
        booted = true;
    };
    while (std::getline(*src, text))
    {
        line_no++;
        std::istringstream in(text);
        std::string cmd;
        if (!(in >> cmd) || cmd[0] == '#')
            continue;

        if (cmd == "card")
        {
            // card put PATH FILE / card text PATH "..." / card dump
            std::string what, path;
            in >> what;
            if (what == "file" && in >> path)
            {
                // the file's lines follow, each behind a |
                std::string& f = CardFiles()[path];
                f.clear();
                while (src->peek() == '|' && std::getline(*src, text))
                {
                    line_no++;
                    f += text.substr(1) + "\n";
                }
            }
            else if (what == "put" && in >> path)
            {
                std::string from;
                in >> from;
                std::ifstream f(from, std::ios::binary);
                if (!f)
                    Fail(line_no, "can't read " + from);
                std::stringstream ss;
                ss << f.rdbuf();
                CardFiles()[path] = ss.str();
            }
            else if (what == "wav" && in >> path)
            {
                // a loop for the loop loader (#70): SECONDS of a sine at HZ, 16-bit 48 kHz,
                // CHANNELS 1 or 2 (the right a quarter period on), an acid chunk of BEATS
                float seconds = 0.f, hz = 0.f;
                int channels = 0;
                uint32_t beats = 0;
                in >> seconds >> channels >> hz >> beats;
                if (seconds <= 0.f || (channels != 1 && channels != 2))
                    Fail(line_no, "card wav PATH SECONDS CHANNELS HZ [BEATS]");
                CardFiles()[path] = SineWav(static_cast<uint32_t>(seconds * 48000.f + .5f), channels, hz, beats);
            }
            else if (what == "remove")
                SetCardPresent(false);
            else if (what == "insert")
                SetCardPresent(true);
            else if (what == "dump")
                for (auto& f : CardFiles())
                    printf("--- %s\n%s\n", f.first.c_str(), f.second.c_str());
            else
                Fail(line_no, "card: file PATH, put PATH FILE, wav PATH SECONDS CHANNELS HZ [BEATS], remove, insert or dump");
            continue;
        }
        if (cmd == "battery")
        {
            float v = 3.8f;
            std::string a, b;
            in >> v >> a >> b;
            SetBattery(v, a == "plugged" || b == "plugged", a == "full" || b == "full");
            continue;
        }
        if (cmd == "input" || cmd == "mic" || cmd == "usbaudio")
        {
            Source& src = cmd == "mic" ? mic_in : cmd == "usbaudio" ? usb_in : aux_in;
            std::string what;
            in >> what;
            if (cmd == "usbaudio" && (what == "record" || what == "eject"))
            {
                // usbaudio record on|off: the computer records FRIZZ; usbaudio eject: it ejects
                // the card lent to it
                std::string on;
                if (what == "eject")
                    EjectCard();
                else if (in >> on && (on == "on" || on == "off"))
                    SetUsbRecording(on == "on");
                else
                    Fail(line_no, "usbaudio record on|off");
                continue;
            }
            if (what == "sine")
            {
                in >> src.freq >> src.amp;
                src.mode = Source::SINE;
            }
            else if (what == "wav")
            {
                std::string path;
                in >> path;
                src.wav.clear();
                src.pos = 0;
                if (ReadWav(path.c_str(), src.wav))
                    src.mode = Source::WAV;
                else
                    Fail(line_no, "can't read " + path + " (16-bit PCM or 32-bit float WAV)");
            }
            else
                src.mode = Source::OFF;
            continue;
        }
        if (cmd == "jack")
        {
            std::string what;
            in >> what;
            if (what == "in" || what == "out")
                SetJack(what == "in");
            else
                Fail(line_no, "jack in or out");
            continue;
        }

        // everything else happens to the running device
        boot();
        if (cmd == "wait")
        {
            uint32_t ms = 0;
            in >> ms;
            RunMs(ms);
        }
        else if (cmd == "at")
        {
            uint32_t ms = 0;
            in >> ms;
            ms += at_base;
            if (ms > NowMs())
                RunMs(ms - NowMs());
        }
        else if (cmd == "booted")
        {
            // a bug report's times count from when main() enters its loop (EventLog.h)
            while (!MainLoopRunning() && NowMs() < 10000)
                RunMs(1);
            at_base = NowMs();
        }
        else if (cmd == "down" || cmd == "up" || cmd == "tap")
        {
            std::string name;
            uint32_t hold = 60;
            in >> name >> hold;
            if (!Press(name.c_str(), cmd != "up"))
                Fail(line_no, "no key " + name);
            if (cmd == "tap")
            {
                RunMs(hold);
                Press(name.c_str(), false);
            }
        }
        else if (cmd == "turn")
        {
            int enc = 0, detents = 0;
            in >> enc >> detents;
            Turn(enc, detents);
        }
        else if (cmd == "toggle")
        {
            int level = 1;
            in >> level;
            SetToggle(level);
        }
        else if (cmd == "midi")
        {
            std::string b;
            while (in >> b)
                Midi(static_cast<uint8_t>(strtol(b.c_str(), nullptr, 16)));
        }
        else if (cmd == "usb")
        {
            std::string b;
            while (in >> b)
                UsbMidi(static_cast<uint8_t>(strtol(b.c_str(), nullptr, 16)));
        }
        else if (cmd == "replies")
        {
            // what came back over USB since the last look, as hex
            printf("%7u usb%s\n", NowMs(), UsbHex(TakeUsb()).c_str());
        }
        else if (cmd == "clock")
        {
            // clock BPM [jitter MS] [drift PPM] [usb] [ramp BPM MS] [seed N]: the jack's, or
            // with usb USB's; clock 0 [usb] stops it
            ClockGen::Config c;
            in >> c.bpm;
            std::string opt;
            while (in >> opt)
            {
                if (opt == "jitter")
                    in >> c.jitter_ms;
                else if (opt == "drift")
                    in >> c.drift_ppm;
                else if (opt == "usb")
                    c.usb = true;
                else if (opt == "ramp")
                    in >> c.ramp_to >> c.ramp_ms;
                else if (opt == "seed")
                    in >> c.seed;
                else
                    Fail(line_no, "clock: no option " + opt);
            }
            ClockGen& gen = c.usb ? clock_usb : clock_trs;
            if (c.bpm > 0.)
                gen.Start(c, BlockMs());
            else
                gen.Stop();
        }
        else if (cmd == "set")
        {
            // set GROUP INDEX VALUE: a knob, latch, VOLUME's value or setting, absolutely
            std::string group, index, value;
            in >> group >> index >> value;
            const std::string why = DevSet(group, index, value);
            if (!why.empty())
                Fail(line_no, "set " + group + " " + index + " " + value + ": " + why);
        }
        else if (cmd == "leds")
            printf("%7u %s\n", NowMs(), LedLine().c_str());
        else if (cmd == "expect")
        {
            // expect led pth|smt INDEX RRGGBB (at full scale), expect off, expect on
            std::string what, chain, want;
            int index = 0;
            in >> what;
            if (what == "led")
            {
                if (!(in >> chain >> index >> want) || (chain != "pth" && chain != "smt")
                    || index < 0 || index >= (chain == "pth" ? kNumPthLeds : kNumSmtLeds)
                    || want.size() != 6)
                {
                    Fail(line_no, "expect led pth|smt INDEX RRGGBB");
                    continue;
                }
                for (char& ch : want)
                    ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
                const std::string got = Hex(chain == "pth" ? PthLedFull(index)
                                                           : SmtLedFull(index));
                if (got != want)
                    Fail(line_no, chain + " " + std::to_string(index) + " is " + got + ", not " +
                                      want);
            }
            else if (what == "usb")
            {
                // expect usb HEX...: what came back over USB since the last look, all of it
                std::string b, want;
                while (in >> b)
                {
                    char hex[4];
                    snprintf(hex, sizeof(hex), " %02X",
                             static_cast<unsigned>(strtol(b.c_str(), nullptr, 16)));
                    want += hex;
                }
                const std::string got = UsbHex(TakeUsb());
                if (got != want)
                    Fail(line_no, "usb sent" + (got.empty() ? std::string(" nothing") : got) +
                                      ", not" + want);
            }
            else if (what == "usbaudio")
            {
                // expect usbaudio on|off: FRIZZ on the bus as a sound card, or not
                std::string want;
                in >> want;
                if (want != "on" && want != "off")
                    Fail(line_no, "expect usbaudio on|off");
                else if (UsbAudioOn() != (want == "on"))
                    Fail(line_no, std::string("USB audio is ") + (UsbAudioOn() ? "on" : "off"));
            }
            else if (what == "off")
            {
                if (Powered())
                    Fail(line_no, "still powered");
            }
            else if (what == "on")
            {
                if (!Powered())
                    Fail(line_no, "powered off");
            }
            else
                Fail(line_no, "expect led, usb, usbaudio, on or off");
        }
        else
            Fail(line_no, "unknown command " + cmd);
    }

    return failures;
}
} // namespace twin
