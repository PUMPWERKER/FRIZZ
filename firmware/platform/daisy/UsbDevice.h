/** @file UsbDevice.h
 *  @brief The USB-C port on TinyUSB (UsbDevice.cpp, #196): a sound card, USB MIDI, and the
 *  card as a drive.
 *
 *  - Audio, USB Audio Class 1, 48 kHz / 24 bit, 2 channels each way (tusb_config.h,
 *    usb_descriptors.c). The computer's stream is the input source USB (passthroughEngine.h,
 *    Receive); the master before VOLUME goes to it (Send). The computer follows the device's clock
 *    both ways: its stream into the device is held half a FIFO deep by the feedback endpoint (the
 *    FIFO's count), the device's to it goes in packets of 47, 48 or 49 frames by its FIFO's fill.
 *    CRS trims the USB clock (HSI48) to the computer's start of frame.
 *  - USB audio on or off (the firmware's setting; FRIZZ's: SHIFT + the upper F# on the settings
 *    page while USB is the source, saved in frizz_master.txt): off, the device is MIDI and the
 *    drive alone. The layout is chosen before the device goes on the bus (Start, after the
 *    card's settings are read), and a switch leaves the bus and comes back with the other one
 *    (SetAudio): MIDI is gone meanwhile.
 *  - MIDI (UsbMidiTransport.h): libDaisy's MidiHandler on TinyUSB. What comes in is parsed in
 *    TinyUSB's task, what goes out is queued (MainLoop's answers, MidiOut's bytes) and sent
 *    from the task. MidiOut's USB bytes are queued from the task too (SetTaskHook), so a
 *    MainLoop held up by the card doesn't hold up the clock out over USB (#265).
 *  - The card as a drive (CardLend.h): TinyUSB asks for sectors in its task, MainLoop moves
 *    them (CardTask) through SdDisk.h's transfers, the task answers.
 *
 *  Contexts. TinyUSB's task runs from TIM7 at 4 kHz, priority 2: below the USB interrupt and
 *  the audio callback (both 0, which never preempt each other), above MainLoop. TinyUSB is
 *  called only from that task and from the USB interrupt (and Receive/Send in the audio
 *  callback reach only its audio FIFOs, which the USB interrupt fills and empties at the same
 *  priority): MainLoop only posts what it wants and reads flags. TinyUSB's lock (OS_NONE)
 *  masks the USB interrupt, not TIM7, so a TinyUSB call from MainLoop could be preempted by
 *  the task in the middle of its event queue.
 *
 *  ST's USB stack is gone from the image: libDaisy's system.cpp has no OTG_HS interrupt
 *  handlers any more (THIRD_PARTY.md), UsbDevice.cpp has TinyUSB's. The virtual CHOMPI has
 *  its own of everything here (twin.cpp): what the computer plays, what the device sends it, the
 *  layout it comes up with, the card lent.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#ifndef PLATFORM_USB_SAFETY
#define PLATFORM_USB_SAFETY 0 // make USB_SAFETY=1: Watch() (a development build for a new stack)
#endif

namespace chompi
{
namespace usb
{
static constexpr size_t kChannels = 2; // each way
static constexpr size_t kMaxBlock = 48; // the most frames Receive() and Send() take at once

/** Once, as the USB lines are back from the charger (chompi_main.cpp): the port and the task's
 *  timer. The device isn't on the bus yet */
void Init();

/** Once the card's settings are read: on the bus, with the audio function or without */
void Start(bool audio);

/** USB audio switched: off the bus and back with the other layout; MIDI and the drive come
 *  back with it, MIDI answering again ~1.2 s after (measured for #281). From MainLoop */
void SetAudio(bool audio);

/** The layout the device is on the bus with (or will be): true with the sound card */
bool Audio();

/** A computer has the device configured and isn't suspending it, and it is on the bus */
bool Host();

/** The computer plays to the device */
bool RxStreaming();
/** The computer records from the device */
bool TxStreaming();

/** The computer's audio for this block, kChannels, as it sent it (-1..1); silence while it
 *  doesn't play, faded in as it starts, out as it stops or runs dry. From the audio callback */
const float* const* Receive(size_t size);

/** To the computer, kChannels (-1..1, clamped), dropped while it doesn't record. From the
 *  audio callback */
void Send(const float* const* to_host, size_t size);

/** Counts since boot, for the developers' SysEx (kCmdDev's USB) and tools/usbaudio.py */
struct Stats
{
    uint32_t rx_blocks; // blocks taken from the computer
    uint32_t rx_dry;    // its stream ran dry: faded out, waited to fill again
    uint32_t rx_starts; // it started playing
    uint32_t tx_blocks; // blocks sent
    uint32_t tx_full;   // blocks dropped, the IN FIFO full
    uint16_t rx_fill;   // the OUT FIFO's bytes at the last block
    uint16_t tx_fill;   // the IN FIFO's
    // the computer's packets (the USB interrupt): how many, of fewer / exactly / more frames
    // than a ms's 48 (the feedback at work), the OUT FIFO full after one (a packet's worth
    // lost), its fill's lowest and highest after a packet since the last ask
    uint32_t rx_packets, rx_short, rx_long, rx_full;
    uint16_t rx_min, rx_max;
    uint32_t enumerations; // times the device went on the bus
};
const Stats& GetStats();
/** The fill's extremes start again */
void ResetFillRange();

/** The card as a drive (CardLend.h): io moves sectors (SdDisk.h's transfers, MainLoop), sectors
 *  tells how many the card has, asked as it's lent (the HAL knows only once FatFs has set the
 *  card up) */
void SetCard(bool (*io)(bool write, uint8_t* buf, uint32_t sector, uint32_t count), uint32_t (*sectors)());
/** On: the computer finds the card in the drive, the firmware leaves it alone; off: an empty
 *  drive. The first TEST UNIT READY after it's on answers UNIT ATTENTION, the medium changed.
 *  From MainLoop */
void CardMode(bool on);
bool InCardMode();
/** True once when the computer ejected it, or USB went (unplugged, asleep) while it was on */
bool TakeCardGone();
/** From MainLoop, every pass: the computer's transfer, if one waits */
void CardTask();
struct CardStats
{
    uint32_t reads, writes;        // transfers
    uint32_t kb_read, kb_written;  // their KB
    uint32_t errors;
    uint32_t io_ms;                // the card's time in them
    uint32_t unit_ready, capacity; // the computer's TEST UNIT READY and READ CAPACITY: does it poll
};
const CardStats& GetCardStats();

/** A function TinyUSB's task calls every pass, before it sends MIDI: MidiOut::FlushUsb (#265) */
void SetTaskHook(void (*hook)());

#if PLATFORM_USB_SAFETY
/** From MainLoop, in a development build only (make USB_SAFETY=1), the ways back to the
 *  launcher without the power switch for a new USB stack: an enumeration that never finishes
 *  (the computer sends frames for 10 s, never configures the device), or a computer that configured
 *  it but sent it no MIDI for 60 s (the tools ask at once: the device is deaf to them), restarts
 *  into it. Never in a player's build: a computer that sends no MIDI is no fault */
void Watch(uint32_t now_ms);
#endif
} // namespace usb
} // namespace chompi
