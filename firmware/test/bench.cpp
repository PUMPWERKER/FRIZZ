// bench.cpp: runs the CPU bench's firmware (FRIZZ-bench.bin, Bench.h) on the virtual CHOMPI
// from power-on to its end and checks that it gets there: every segment run and written to
// /FRIZZ/cpu.txt, the keys' LEDs graded, the panel green, the effects actually switched in
// and out. The loads themselves are 0 here: no time passes on the twin while the callback
// runs, so only the device measures them. Run by unit.sh bench.
// twin defines: -DFRIZZ_BENCH=1
#include <sys/wait.h>
#include <unistd.h>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>
#include "check.h"
#include "twin.h"

using namespace twin;

static const char* const kSegments[] = {
    "idle",       "freezer",   "shifter",      "folder",      "crusher",
    "filter",     "flanger",   "resonator",    "slicer",      "warble",
    "tapestop",   "compressor", "inserts",     "recording",   "loop",
    "loop+inserts", "loop+delay", "loop+scene4+delay", "loop+reverb", "everything",
    "stress",     "loop+everything", "loop+inserts+chaos", "loop+everything+page2",
};
static const size_t kNum = sizeof(kSegments) / sizeof(kSegments[0]);
// the bench waits before each segment for what it doesn't use to rest, so nothing may still
// be working in any segment that isn't part of it
static const size_t kClean = kNum;

// Without a card: the bench runs, then the panel blinks red
static int NoCard()
{
    SetCardPresent(false);
    Boot();
    // the boot, the run with its pauses for tails (each at most 30 s), the end
    Run(150000 * 2, nullptr, nullptr);
    int lit = 0, dark = 0;
    for (int i = 0; i < 1000; i++)
    {
        Run(2, nullptr, nullptr);
        const Rgb c = PthLedFull(0);
        lit += c.r > 200 && c.g == 0;
        dark += c.r == 0 && c.g == 0 && c.b == 0;
    }
    Check(lit > 300 && dark > 300, "bench: without a card the panel blinks red at the end");
    return failures;
}

int main()
{
    // the run without a card in a process of its own, side by side with the one with a card
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0)
    {
        const int result = NoCard();
        fflush(stdout);
        _exit(result);
    }

    Boot();
    // the boot animation, about 10 s for the delay to rest, then 24 segments of 3 s with the
    // 4 s recording among them and the pauses for tails between them, until cpu.txt is
    // written; the input is the bench's own tune, so none is fed here
    std::vector<float> rms; // the master out's level every 3 s from when the bench starts
    float out[kBlockSize * kChannels];
    double sum = 0.;
    const uint32_t start_ms = 11400;
    const auto& card = CardFiles();
    for (uint32_t b = 0; b < 200000 * 2 && !card.count("/FRIZZ/cpu.txt"); b++)
    {
        Run(1, nullptr, out);
        for (size_t i = 0; i < kBlockSize; i++)
            sum += out[i * kChannels + 2] * out[i * kChannels + 2];
        if (NowMs() >= start_ms && (NowMs() - start_ms) % 3000 == 0 && b % 2 == 1)
        {
            rms.push_back(sqrtf(sum / (6000 * kBlockSize)));
            sum = 0.;
        }
    }
    printf("      the run took %.1f s\n", NowMs() / 1000.);
    Run(2000 * 2, nullptr, nullptr); // the LEDs' last frame

    const auto it = card.find("/FRIZZ/cpu.txt");
    Check(it != card.end(), "bench: /FRIZZ/cpu.txt is written at the end");
    const std::string text = it == card.end() ? "" : it->second;
    size_t found = 0;
    bool clean = true;
    std::istringstream in(text);
    std::string line;
    for (size_t s = 0; std::getline(in, line);)
        if (s < kNum && line.rfind(std::string(kSegments[s]) + " ", 0) == 0 && line.size() >= 32)
        {
            // "name  max  mean  still working...": what follows the 35th column
            if (s < kClean && line.size() > 35)
            {
                printf("      %s\n", line.c_str());
                clean = false;
            }
            found++;
            s++;
        }
    Check(found == kNum, "bench: every segment has its line, in order, with a max and a mean");
    Check(clean, "bench: nothing else still works in any segment");
    Check(text.find(" s, playing in every loop segment") != std::string::npos
              && text.find("# loop:    4.0 s") != std::string::npos,
          "bench: the 4 s loop plays in every loop segment");
    Check(text.find("# worst: ") != std::string::npos, "bench: and the worst is named");
    // where the time goes: every segment again with its parts, then with each effect's
    const auto Rows = [&](const char* title, size_t length) {
        const size_t at = text.find(title);
        size_t rows = 0;
        if (at == std::string::npos)
            return rows;
        std::istringstream rest(text.substr(at));
        for (size_t s = 0; std::getline(rest, line);)
            if (s < kNum && line.rfind(std::string(kSegments[s]) + " ", 0) == 0
                && line.size() >= length)
            {
                rows++;
                s++;
            }
        return rows;
    };
    Check(Rows("# where the mean goes", 18 + 11 * 7) == kNum
              && text.find("MHz by the audio clock") != std::string::npos,
          "bench: and split into the callback's parts, with the clock it ran at");
    Check(Rows("# the FX chain by effect", 18 + 12 * 10) == kNum,
          "bench: and the FX chain into its effects");

    bool keys = true;
    for (int led : {24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 0, 1, 2, 3, 4, 5, 6, 7})
    {
        const Rgb c = SmtLedFull(led);
        keys &= c.g > 200 && c.r == 0 && c.b == 0; // green: the twin's loads are 0
    }
    Check(keys, "bench: a key per segment, graded green");
    bool panel = true;
    for (int i = 0; i < kNumPthLeds; i++)
        panel &= PthLedFull(i).g > 200 && PthLedFull(i).r == 0;
    Check(panel, "bench: at the end the panel is green");

    // the tune goes through: every 3 s after the boot something comes out, but for the tape
    // stop's segment (the windows don't line up with the segments exactly)
    int quiet = 0;
    for (size_t w = 1; w < rms.size(); w++)
        quiet += rms[w] < .002f;
    Check(rms.size() >= kNum && quiet <= 2, "bench: its tune reaches the master out throughout");

    // it answers FRIZZ's SysEx as FRIZZ does: the tools can't tell the two apart by asking, and
    // note the slot they started instead (tools/chompi.py). Its load reads 0, as the bench
    // never counts it there (chompi_main.cpp); on the twin FRIZZ's does too, no time passing
    const auto Ask = [](uint8_t cmd) {
        TakeUsbOut();
        for (uint8_t b : {0xF0, 0x7D, 0x43, 0x48, int(cmd), 0xF7})
            UsbMidi(b);
        Run(100 * 2, nullptr, nullptr);
        const std::string out = TakeUsbOut();
        const size_t at = out.find(std::string("\xF0\x7D\x43\x48", 4) + char(cmd | 0x40));
        return at == std::string::npos ? std::string() : out.substr(at + 5, out.find('\xF7', at) - at - 5);
    };
    const std::string settings = Ask(0x24), load = Ask(0x23);
    Check(settings.size() >= 2, "bench: it answers FRIZZ's settings query (0x24), as FRIZZ does");
    Check(load == std::string(4, '\0'), "bench: and its load (0x23) reads 0");

    fflush(stdout);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        failures++;
    return Finish();
}
