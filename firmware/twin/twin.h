/** @file twin.h
 *  @brief The virtual CHOMPI: FRIZZ's own firmware (chompi_main.cpp and everything under it,
 *  libDaisy's UI, Switch, 4021 and MIDI code) running on the host against a simulated board.
 *
 *  Time only moves in Run(): each 0.5 ms block runs the audio callback, then main() (a
 *  coroutine) until it has used up the block. Everything is deterministic: the same inputs at
 *  the same times give the same samples and LEDs.
 *
 *  Power-on is Boot(); there is one power cycle per process (the firmware's globals are
 *  constructed once), so a test that needs a fresh device runs a fresh process.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace twin
{
static const size_t kBlockSize = 24;
static const float kSampleRate = 48000.f;
static const int kChannels = 4; // in: mic, X, aux L, aux R; out: headphone L/R, master L/R
static const int kNumPthLeds = 10; // the panel's through-hole LEDs
static const int kNumSmtLeds = 25; // the keys' LEDs

/** The SD card in the slot, as files keyed by their full path ("/FRIZZ/frizz_scenes.txt").
 *  Fill it before Boot() for a card with state on it; read it back any time */
std::map<std::string, std::string>& CardFiles();
void SetCardPresent(bool present);
/** Bytes the card can still take (a full card: 0; SIZE_MAX at first), and the files open on it */
void SetCardSpace(size_t bytes);
int CardOpenFiles();

/** Before the first Run(): the firmware's clock (System::GetNow, GetUs, GetTick) starts at ms
 *  instead of 0, so a check can cross the millisecond counter's wrap. NowMs() and the scripts
 *  still count from power-on */
void SetClockStartMs(uint32_t ms);

/** Power on: starts the firmware's main(). The card and any keys held should be set first */
void Boot();

/** Runs `blocks` audio blocks. in and out are interleaved, kChannels per frame,
 *  kBlockSize * blocks frames; in may be null (silence), out may be null */
void Run(size_t blocks, const float* in, float* out);

/** Milliseconds since power-on (the firmware's System::GetNow in the audio callback) */
uint32_t NowMs();

/** A key or switch by its name in hardware.h's Hardware::SwId (KEY_1 .. KEY_28,
 *  ENC_1_SW .. ENC_6_SW), down or up. False if there's no such name */
bool Press(const char* name, bool down);

/** Every name Press() takes */
const char* const* KeyNames();

/** Turns encoder 1..6 (SW1..SW6, hardware.h's EncoderId) by `detents`, + as the firmware's
 *  +1. The detents are played out one after another, 8 ms each */
void Turn(int encoder, int detents);

/** The mode switch, as the 4021 reads it (SW_TOG's raw level) */
void SetToggle(bool raw_level);

/** A byte into the MIDI jack (TRS, the UART) */
void Midi(uint8_t byte);

/** A byte into USB MIDI, as a computer sends it */
void UsbMidi(uint8_t byte);
/** Whether a host has USB configured: true at first; false as on a battery, no cable */
void SetUsbHost(bool host);

/** What the firmware sent out over USB MIDI since the last call, taken */
std::string TakeUsbOut();

/** A byte MIDI out sent (MidiOut.h): when it went out (ms; the jack's as the UART sends it,
 *  USB's as MainLoop does), and where */
struct MidiOutByte
{
    double ms;
    uint8_t byte;
    bool usb;
};
/** What went out of the MIDI jack, and MIDI out's clock, transport and Song Position over
 *  USB, since the last call, taken */
std::vector<MidiOutByte> TakeMidiOut();

/** The start of the current block in ms, to the block (NowMs() is whole ms) */
double BlockMs();

/** What the firmware makes of the MIDI clock, read out of it: MidiClock, the engine's
 *  TempoClock and the looper. For the checks of timing that LEDs and audio can't show */
struct ClockState
{
    // MidiClock.h
    bool has_clock;
    int source; // 0 none, 1 TRS, 2 USB
    float midi_bpm;
    float tick_period; // in samples, smoothed
    uint32_t ticks, locks;
    // TempoClock.h, as the engine has it (all 0 on a firmware without it)
    int tempo;     // the FX's whole BPM
    float fx_bpm;  // the FX's tempo for their times
    uint32_t position; // the last pulse's, 0..191
    // Looper.h
    int loop_state; // Looper::State: 0 empty, 1 recording, 2 playing, 3 paused
    size_t loop_length;
    uint32_t loop_beats;
    float loop_pos;
    float loop_speed;
};
ClockState Probe();

/** Whether the firmware's main() has entered its loop (after about 1 s): a bug report's times
 *  count from there */
bool MainLoopRunning();

/** Whether the firmware has reset the chip (NVIC_SystemReset): a restart asked for over MIDI */
bool Restarted();

/** The battery and charger the MP2722 reports */
void SetBattery(float volts, bool plugged, bool full = false);

/** False once the firmware has put the charger into shipping mode (the device is off) */
bool Powered();

struct Rgb
{
    uint8_t r, g, b;
};

/** The colour each LED is showing, decoded from the WS2812 bitstream the LED DMA is sending,
 *  as the raw byte the LED gets (FRIZZ scales PTH to /11 and SMT to /4 of full) */
Rgb PthLed(int index);
Rgb SmtLed(int index);

/** The same, scaled back up to 0..255 so the two chains compare */
Rgb PthLedFull(int index);
Rgb SmtLedFull(int index);
} // namespace twin
