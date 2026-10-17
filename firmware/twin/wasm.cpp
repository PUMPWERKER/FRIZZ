/** @file wasm.cpp
 *  @brief twin.h as plain C functions for the browser (web/worker.js calls them through
 *  Emscripten). Audio and LEDs go through buffers in the module's memory: twin_in() and
 *  twin_out() hold one call's worth of interleaved frames, twin_leds() 35 RGB triples.
 */
#include <emscripten/emscripten.h>
#include <string>
#include "twin.h"

using namespace twin;

static const size_t kMaxBlocks = 64;
static float in_buf[kMaxBlocks * kBlockSize * kChannels];
static float out_buf[kMaxBlocks * kBlockSize * kChannels];
static uint8_t leds[(kNumPthLeds + kNumSmtLeds) * 3];
static std::string card_text;

extern "C" {

EMSCRIPTEN_KEEPALIVE float* twin_in() { return in_buf; }
EMSCRIPTEN_KEEPALIVE float* twin_out() { return out_buf; }
EMSCRIPTEN_KEEPALIVE int twin_max_blocks() { return kMaxBlocks; }

EMSCRIPTEN_KEEPALIVE void twin_boot() { Boot(); }

/** Runs `blocks` (at most twin_max_blocks()) from twin_in() into twin_out() */
EMSCRIPTEN_KEEPALIVE void twin_run(int blocks)
{
    Run(blocks > static_cast<int>(kMaxBlocks) ? kMaxBlocks : blocks, in_buf, out_buf);
}

EMSCRIPTEN_KEEPALIVE uint32_t twin_now_ms() { return NowMs(); }
EMSCRIPTEN_KEEPALIVE int twin_press(const char* name, int down) { return Press(name, down); }
EMSCRIPTEN_KEEPALIVE void twin_turn(int encoder, int detents) { Turn(encoder, detents); }
EMSCRIPTEN_KEEPALIVE void twin_toggle(int raw_level) { SetToggle(raw_level); }
EMSCRIPTEN_KEEPALIVE void twin_midi(int byte) { Midi(static_cast<uint8_t>(byte)); }
EMSCRIPTEN_KEEPALIVE void twin_battery(float volts, int plugged, int full)
{
    SetBattery(volts, plugged, full);
}
EMSCRIPTEN_KEEPALIVE int twin_powered() { return Powered(); }
EMSCRIPTEN_KEEPALIVE void twin_eject_card() { EjectCard(); }
EMSCRIPTEN_KEEPALIVE int twin_main_loop_running() { return MainLoopRunning(); }

/** The LEDs at full scale: the 10 panel LEDs, then the 25 key LEDs, R G B each */
EMSCRIPTEN_KEEPALIVE uint8_t* twin_leds()
{
    for (int i = 0; i < kNumPthLeds + kNumSmtLeds; i++)
    {
        const Rgb c = i < kNumPthLeds ? PthLedFull(i) : SmtLedFull(i - kNumPthLeds);
        leds[i * 3] = c.r;
        leds[i * 3 + 1] = c.g;
        leds[i * 3 + 2] = c.b;
    }
    return leds;
}

/** The card: a file put on it (before twin_boot, to start with state), and every file on it
 *  as "path\n<length>\ncontents" records */
EMSCRIPTEN_KEEPALIVE void twin_card_put(const char* path, const char* text)
{
    CardFiles()[path] = text;
}
EMSCRIPTEN_KEEPALIVE const char* twin_card_files()
{
    card_text.clear();
    for (auto& f : CardFiles())
        card_text += f.first + "\n" + std::to_string(f.second.size()) + "\n" + f.second;
    return card_text.c_str();
}
}

#ifndef TWIN_VERSION
#define TWIN_VERSION "unknown"
#endif
/** The commit the firmware was built from, "+changes" when the working tree differed */
extern "C" EMSCRIPTEN_KEEPALIVE const char* twin_version() { return TWIN_VERSION; }
