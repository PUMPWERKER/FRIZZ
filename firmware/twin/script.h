/** @file script.h
 *  @brief The script player behind frizz-twin (cli.cpp), and what ui.cpp replays a bug
 *  report with: README.md's commands, played into the virtual CHOMPI from power-on.
 */
#pragma once
#include <cstdint>
#include <cstdio>
#include <istream>
#include <string>
#include <vector>

namespace twin
{
/** Plays a script. leds (may be null) gets a line whenever the LEDs change, out (may be
 *  null) the master out, interleaved L R. Returns how many lines failed (an `expect` that
 *  didn't hold, an unknown command), each told on stderr */
int PlayScript(std::istream& src, FILE* leds, std::vector<float>* out);

/** Every LED now, as a line of the LED log: "pth RRGGBB x10 | smt RRGGBB x25" */
std::string LedLine();

/** A loop for the loop loader's card (#70, `card wav`): a WAV of 16-bit PCM at 48 kHz, frames
 *  of a sine at freq, amplitude .3, mono or stereo (the right a quarter period on), with an
 *  acid chunk of beats before its data if beats > 0 */
std::string SineWav(uint32_t frames, int channels, float freq, uint32_t beats = 0);
} // namespace twin
