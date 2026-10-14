/** @file cli.cpp
 *  @brief frizz-twin: plays a script of key presses, knob turns, MIDI and audio into the
 *  virtual CHOMPI (twin.h) from power-on (script.h), and writes what came out: the master out
 *  as a WAV, the LEDs whenever they change. See README.md for the script's commands.
 */
#include "script.h"
#include "twin.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

using namespace twin;

static void WriteWav(const char* path, const std::vector<float>& s)
{
    FILE* f = fopen(path, "wb");
    if (!f)
        return;
    const uint32_t data = s.size() * 4, riff = 36 + data, fmt_len = 16, rate = 48000,
                   bytes_per_sec = rate * 8;
    const uint16_t format = 3, channels = 2, align = 8, bits = 32;
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_len, 4, 1, f);
    fwrite(&format, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&bytes_per_sec, 4, 1, f);
    fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data, 4, 1, f);
    fwrite(s.data(), 4, s.size(), f);
    fclose(f);
}

int main(int argc, char** argv)
{
    const char* script = nullptr;
    const char* wav_out = nullptr;
    const char* led_out = nullptr;
    bool quiet = false;
    FILE* led_log = nullptr;
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            wav_out = argv[++i];
        else if (!strcmp(argv[i], "-l") && i + 1 < argc)
            led_out = argv[++i];
        else if (!strcmp(argv[i], "-q"))
            quiet = true;
        else
            script = argv[i];
    }
    if (!script)
    {
        fprintf(stderr, "usage: frizz-twin [-o out.wav] [-l leds.txt] [-q] SCRIPT|-\n");
        return 2;
    }
    if (led_out)
        led_log = !strcmp(led_out, "-") ? stdout : fopen(led_out, "w");

    std::ifstream file;
    std::istream* src = &std::cin;
    if (strcmp(script, "-"))
    {
        file.open(script);
        if (!file)
        {
            fprintf(stderr, "can't read %s\n", script);
            return 2;
        }
        src = &file;
    }

    std::vector<float> out_samples;
    const int failures = PlayScript(*src, led_log, &out_samples);

    if (wav_out)
        WriteWav(wav_out, out_samples);
    if (led_log && led_log != stdout)
        fclose(led_log);
    if (!quiet)
    {
        float peak = 0.f;
        double sum = 0.;
        for (float v : out_samples)
        {
            peak = std::max(peak, std::fabs(v));
            sum += double(v) * v;
        }
        fprintf(stderr, "%u ms, master out peak %.3f rms %.4f%s\n", NowMs(), peak,
                out_samples.empty() ? 0. : sqrt(sum / out_samples.size()),
                Powered() ? "" : ", powered off");
    }
    // a queue that overflowed dropped what the firmware sent or got (twin.h): not this script
    const Drops d = Dropped();
    if (d.midi_in || d.usb_midi_in || d.usb_out || d.midi_out || d.usb_audio_out)
    {
        fprintf(stderr,
                "frizz-twin: the twin's queues overflowed, dropped: MIDI in %llu, USB MIDI in %llu "
                "bytes, USB MIDI out %llu bytes, MIDI out %llu, USB audio out %llu frames\n",
                (unsigned long long)d.midi_in, (unsigned long long)d.usb_midi_in,
                (unsigned long long)d.usb_out, (unsigned long long)d.midi_out,
                (unsigned long long)d.usb_audio_out);
        return 3;
    }
    return failures ? 1 : 0;
}
