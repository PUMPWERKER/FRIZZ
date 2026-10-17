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
 *
 *  Everything here is called from one thread at a time. A host may call Run() from a
 *  real-time audio thread: the queues between it and the firmware (MIDI in and out, USB MIDI
 *  out, USB audio out) have fixed sizes, so Run(), Midi(), UsbMidi() and the Take...(buf, max)
 *  calls never allocate. The Take...() calls that return a string or vector do; so does the
 *  card (CardFiles() is a std::map): a Run() in which main() writes it (a setting saved 2 s
 *  after it changed, a scene, a bug report, the loop transfer) allocates.
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
/** Each write to the card holds up main() for ms, as a slow card does on the device (0 at
 *  first: no time) */
void SetCardWriteMs(uint32_t ms);
/** The loop loader's raw reads past FatFs (SdCard.h): each ends on the polls-th look (2 at
 *  first); and the FatFs calls made while one ran, which on the device would cut into it */
void SetCardRawPolls(int polls);
int CardRawClash();
/** FatFs's calls (and raw reads) made while the card was unmounted (lent), from power-on */
int CardUnmountedCalls();
/** The card didn't answer once: FatFs mounts it again inside its next call with a path, back
 *  in the root (ff.c's find_volume); and the n-th f_readdir from now fails (FR_DISK_ERR), once */
void CardGlitch();
void CardReaddirFail(int n);

/** Before the first Run(): the firmware's clock (System::GetNow, GetUs, GetTick) starts at ms
 *  instead of 0, so a check can cross the millisecond counter's wrap. NowMs() and the scripts
 *  still count from power-on */
void SetClockStartMs(uint32_t ms);

/** The audio interrupt at the firmware's FRIZZ_PREEMPT(point) each time main() passes it, as
 *  the device's may come anywhere (not under a ScopedIrqBlocker): main() gives up the rest of
 *  its block there and goes on after the next callback, or after `blocks` of them (main()
 *  held up there, as by higher-priority work). nullptr: nowhere (at first) */
void PreemptAt(const char* point, int blocks = 1);

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
 *  +1. The detents are played out one after another, ms each: 8 a fast turn (FRIZZ's knobs
 *  speed up from 125 ms down, #375), from 125 the slow one */
void Turn(int encoder, int detents, int ms = 8);

/** Encoder 1..6's A and B lines as they are from the next block on (true high, both high at
 *  rest), for a turn played edge by edge: a contact's bounce, a nudge that turns back. False,
 *  and nothing set, while a Turn() of it plays */
bool EncoderLines(int encoder, bool a, bool b);

/** The mode switch, as the 4021 reads it (SW_TOG's raw level) */
void SetToggle(bool raw_level);

/** A cable in AUX's jack (true, as at first) or none: the jack detection the firmware reads.
 *  Before Boot() too, for a power-on without one. The audio into the mic is Run()'s channel 0 */
void SetJack(bool plugged);

/** The queues' sizes. MIDI in: what Midi() and UsbMidi() queue for the next block, each
 *  port its own; the next Run() hands it all to the firmware in one piece, as the UART's DMA
 *  and the USB interrupt do. Out: what the firmware sent until it's taken.
 *  A full queue drops what comes next, the newest, counted in Dropped(): MIDI in byte by
 *  byte, USB MIDI out a message (one transmit) whole, MIDI out byte by byte, USB audio out a
 *  block whole. The firmware doesn't notice: the UART's timing and USB's answers go on as if
 *  it had been sent */
static const size_t kMidiInCapacity = 65536;    // bytes per port, waiting for the next block
static const size_t kUsbOutCapacity = 65536;    // bytes, TakeUsbOut()'s
static const size_t kMidiOutCapacity = 16384;   // MidiOutBytes, TakeMidiOut()'s
static const size_t kUsbAudioOutFrames = 48000; // frames (L R), TakeUsbAudioOut()'s: 1 s

/** What a full queue dropped since power-on: bytes, MidiOutBytes, frames */
struct Drops
{
    uint64_t midi_in, usb_midi_in, usb_out, midi_out, usb_audio_out;
};
Drops Dropped();

/** A byte into the MIDI jack (TRS, the UART) */
void Midi(uint8_t byte);

/** A byte into USB MIDI, as a computer sends it */
void UsbMidi(uint8_t byte);
/** Whether a host has USB configured: true at first; false as on a battery, no cable */
void SetUsbHost(bool host);

/** What the firmware sent out over USB MIDI since the last call, taken */
std::string TakeUsbOut();
/** The same without allocating: at most max bytes of it into buf, the oldest first; the rest
 *  stays for the next call. Returns how many */
size_t TakeUsbOut(uint8_t* buf, size_t max);

/** USB audio, the computer's side (UsbDevice.h, #196). What it plays into FRIZZ in the next
 *  Run(), interleaved L R, as many frames as Run's (null: it doesn't play), the source USB;
 *  taken by that Run(). Heard only while FRIZZ is on the bus as a sound card (UsbAudioOn) */
void SetUsbAudioIn(const float* lr);
/** Whether the computer records FRIZZ (the master before VOLUME); what it got since the last
 *  call, interleaved L R, taken */
void SetUsbRecording(bool on);
std::vector<float> TakeUsbAudioOut();
/** The same without allocating: at most max_frames frames of it into lr (L R), the oldest
 *  first; the rest stays for the next call. Returns how many frames */
size_t TakeUsbAudioOut(float* lr, size_t max_frames);
/** Whether FRIZZ is on the bus as a sound card (USB audio on, not away for a switch), and how
 *  often it went on the bus since power-on */
bool UsbAudioOn();
int UsbEnumerations();
/** The card lent to the computer as a drive (CardLend.h), and the computer ejecting it */
bool CardLent();
void EjectCard();

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
/** The same without allocating: at most max of them into buf, the oldest first; the rest
 *  stays for the next call. Returns how many */
size_t TakeMidiOut(MidiOutByte* buf, size_t max);

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
    uint32_t block_pulses; // the last block's, up to 4 (PassthroughEngine::BlockPulses)
    // Looper.h
    int loop_state; // Looper::State: 0 empty, 1 recording, 2 playing, 3 paused
    size_t loop_length;
    uint32_t loop_beats;
    float loop_pos;
    float loop_speed;
    uint32_t loop_switches; // the loops switched to (Looper::Switches)
};
ClockState Probe();

/** Whether the firmware's main() has entered its loop (after about 1 s): a bug report's times
 *  count from there */
bool MainLoopRunning();
/** MainLoop held up for ms from now, as by a slow card write: the audio callback runs on */
void HoldMain(double ms);

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
