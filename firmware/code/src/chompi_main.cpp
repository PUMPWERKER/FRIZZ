/** @file chompi_main.cpp
 *  @brief Firmware entry point
 *
 *  CHOMPI (built on the Daisy Seed / STM32H7) has two places code runs, in
 *  order of priority:
 *   1. AudioCallback() - the audio ISR. Runs once per audio block (~24 samples
 *      at 48kHz here). Polls the controls, AUX's jack and MIDI (clock, and the panel played
 *      over it), and passes the input, AUX, the built-in mic or a computer's audio over USB,
 *      through the engine, and the master back to the computer.
 *   2. MainLoop() - Lowest priority, handles UI dispatch, battery checks, writing the FX
 *      scenes to the SD card, the card lent as a USB drive, and boot-time stuff.
 *  In between, TinyUSB's task (UsbDevice.h) on its own timer: USB, MIDI over it.
 */
#include "FrizzHot.h"
#include "hardware.h"
#include "ui.h"
#include "fatfs.h"
#include "passthroughEngine.h"
#include "SceneStore.h"
#include "EventLog.h"
#include "MidiControl.h"
#include "MidiOut.h"
#include "SdCard.h"
#include "Crash.h"
#include "CardLend.h"
#include "UsbDevice.h"
#if !FRIZZ_BENCH
#include "LoopLoader.h"
#endif
#if FRIZZ_BENCH
#include "Bench.h"
#endif

using namespace daisy;
using namespace chompi;

Hardware hw;
UserInterface ui;

// The SD card holds the FX scenes and the master settings (SceneStore.h)
SdmmcHandler sdmmc;
FatFSInterface fsi;
PassthroughEngine engine;
MidiClock midi_clock;
// the panel played and inspected over MIDI
MidiControl midi_control;
// MIDI clock and the loop's transport out, if the settings page asks (MidiOut.h)
MidiOut midi_out;
SceneStore scene_store;
// every key, knob and clock change since power-on, for a bug report (SHIFT + VOLUME held on
// the settings page)
EventLog event_log;
// SDRAM that is always written before it's read, so ZeroSDRAM leaves it (chompi_sram.lds's
// .sdram_noinit): the loop (Looper reads only frames it recorded) and the event log (each
// event before it's counted, the card's files at Start). An ordinary static on the twin
#ifdef __arm__
#define SDRAM_NOINIT __attribute__((section(".sdram_noinit")))
// the same, linked after it (chompi_sram.lds: .sdram_noinit*), so what came later doesn't
// move the loop and the event log
#define SDRAM_NOINIT_LATER __attribute__((section(".sdram_noinit.later")))
#else
#define SDRAM_NOINIT
#define SDRAM_NOINIT_LATER
#endif
EventLogMem SDRAM_NOINIT event_log_mem;
#if FRIZZ_BENCH
// FRIZZ-bench.bin (make BENCH=1): measures the audio callback's load (Bench.h)
Bench bench;
// where its time goes (BenchProfile.h)
BenchProfile chompi::bench_profile;
#endif

// the looper's, 32-byte aligned as the loop loader's DMA lands in it too (LoopControls.h)
int16_t SDRAM_NOINIT __attribute__((aligned(32))) loop_mem[kLoopMemSize];

// the crash record and the build running (Crash.h), in the backup SRAM, which a reset leaves
// alone: after libDaisy's boot_info (chompi_sram.lds: .backup_sram before .backup_sram.*),
// written before it's read. An ordinary static on the twin
#ifdef __arm__
#define BACKUP_SRAM __attribute__((section(".backup_sram.crash")))
#else
#define BACKUP_SRAM
#endif
crash::Backup BACKUP_SRAM crash_mem;
#if !FRIZZ_BENCH
// loops from the card (LoopLoader.h, #70): the buffer one loads into, 16 bars at 50 BPM, and
// the loader's own. Not in the bench, which has no room for it
uint8_t SDRAM_NOINIT_LATER __attribute__((aligned(32))) loop_load_mem[loops::kBufBytes];
loops::Mem SDRAM_NOINIT_LATER loop_loader_mem;
LoopLoader loop_loader;
#endif

// TEMPO's delay buffer: 10s of interleaved stereo float
static const size_t kDelayFrames = 480000;
float DSY_SDRAM_BSS delay_mem[kDelayFrames * 2];

// the freezer's buffers (FxFreezer.h): 1 bar at the slowest tempo (4.8s at 50 BPM), plus the
// stereo offset and the seam crossfade, per channel
static const size_t kFreezerFrames = 240000;
float DSY_SDRAM_BSS freezer_mem_l[kFreezerFrames];
float DSY_SDRAM_BSS freezer_mem_r[kFreezerFrames];

// the tape stop's buffers (FxTapeStop.h), a power of 2 per channel: its longest lag, a 2 bar
// stop at the slowest tempo on the steepest brake, 7.2s, fits in 2^19 frames (10.9s)
static const size_t kTapeStopFrames = 1u << 19;
float DSY_SDRAM_BSS tapestop_mem_l[kTapeStopFrames];
float DSY_SDRAM_BSS tapestop_mem_r[kTapeStopFrames];

// the reverb carries its 64KB buffer, in the fast DTCMRAM like WAVE's. Not zeroed at startup:
// Reverb::Init clears it
#define DSY_DTCMRAM_BSS __attribute__((section(".dtcmram_bss")))
daisysp::Reverb DSY_DTCMRAM_BSS reverb;

// Running count of audio samples since audio started, the time base for MIDI clock ticks
uint32_t sample_clock = 0;

// Set just before main() enters its loop. Until then main() blocks in its setup and the audio
// callback runs the UI (the boot animation); from then on only MainLoop does, so the UI is
// never re-entered from the interrupt
volatile bool main_loop_running = false;
// While main() reads the keys at boot (the shift registers, for shipping mode),
// the audio callback leaves them alone: both bit-banging the same chain garbles it
volatile bool main_reads_keys = false;

// read by the audio callback, which keeps the outputs muted and the engine still until main()
// has read the keys and MainLoop's first pass has checked the battery
volatile bool booting = true;
bool boot_done = false; // the boot ripple closed: the play page from here

/** AUX's jack, as TAPE and TEMPO read it (Hardware's jack_detect, high with a cable in): a
 *  cable plugged in makes AUX the input, pulled out the built-in mic. At power-on it decides;
 *  after that only a change does, kJackBlocks steady (20 ms), so the settings page's key
 *  (SettingsPage.h) holds until the next */
struct InputJack
{
    static const uint8_t kJackBlocks = 40;
    bool plugged = true;
    uint8_t steady = 0; // blocks the jack has read other than plugged

    /** True once a change has held kJackBlocks */
    bool Changed(bool level)
    {
        if (level == plugged)
        {
            steady = 0;
            return false;
        }
        if (++steady < kJackBlocks)
            return false;
        plugged = level;
        steady = 0;
        return true;
    }
};
InputJack input_jack;

// how long main() reads the keys at boot, for shipping mode, while the boot ripple runs
static const uint32_t kKeyReadMs = 500;

// a restart over MIDI waits this long at most for the card (MainLoop)
static const uint32_t kRestartWaitMs = 1000;
static uint32_t restart_asked = 0;

// where the SDRAM's buffers (DSY_SDRAM_BSS) begin and end: chompi_sram.lds's .sdram_bss
extern "C" uint32_t _ssdram_bss;
extern "C" uint32_t _esdram_bss;

/** What kCmdDev's IDENT tells (MidiControl.h): the image as the bootloader copied it to
 *  SRAM_EXEC, from the vector table to the end of .data's initial values, which is the .bin
 *  byte for byte (chompi_sram.lds: .data's values are copied out of it, never into it), its
 *  CRC-32, and why the chip reset since FRIZZ last started (RCC_RSR, cleared once read: a
 *  watchdog reset is recorded, Crash.h). The option bytes, for CRASH. The kind of build, and
 *  the bench's answer to BENCH */
#ifdef __arm__
extern "C" uint32_t g_pfnVectors, _sidata, _sdata, _edata, _stext;
#endif
/** kCmdDev's USB (#196): 0 -> the audio's counts (UsbDevice.h's Stats), 0 1 -> the computer's
 *  packets instead; 1 -> the drive's (CardStats); 2 X -> the card lent (X 1) or taken back (0),
 *  as the settings page's SHIFT + LOOP. Each answer starts with the flags: a computer 1, it
 *  plays 2, the card lent 4, USB audio on 8, it records 16. Its length, or -1 for BAD */
__attribute__((optimize("Os"))) static size_t UsbPut(uint8_t* d, size_t n, uint32_t v, size_t groups)
{
    for (size_t g = groups; g-- > 0;)
        d[n++] = static_cast<uint8_t>(g * 7 < 32 ? (v >> (g * 7)) & 0x7F : 0);
    return n;
}

__attribute__((optimize("Os"))) static int UsbDev(const uint8_t* arg, size_t len, uint8_t* d)
{
    size_t n = 0;
    const uint8_t part = len ? arg[0] : 0;
    d[n++] = static_cast<uint8_t>((usb::Host() ? 1 : 0) | (usb::RxStreaming() ? 2 : 0)
                                  | (cardlend::Get().lent ? 4 : 0) | (usb::Audio() ? 8 : 0)
                                  | (usb::TxStreaming() ? 16 : 0));
    const usb::Stats& s = usb::GetStats();
    if (part == 0 && len > 1 && arg[1] == 1)
    {
        for (uint32_t v : {s.rx_packets, s.rx_short, s.rx_long, s.rx_full})
            n = UsbPut(d, n, v, 5);
        n = UsbPut(d, n, s.rx_min == 0xFFFF ? 0 : s.rx_min, 2);
        n = UsbPut(d, n, s.rx_max, 2);
        usb::ResetFillRange();
        return static_cast<int>(n);
    }
    if (part == 0)
    {
        for (uint32_t v : {s.rx_blocks, s.rx_dry, s.rx_starts, s.tx_blocks, s.tx_full})
            n = UsbPut(d, n, v, 5);
        n = UsbPut(d, n, s.rx_fill, 2);
        n = UsbPut(d, n, s.tx_fill, 2);
        n = UsbPut(d, n, s.enumerations, 2);
        return static_cast<int>(n);
    }
    if (part == 1)
    {
        const usb::CardStats& c = usb::GetCardStats();
        for (uint32_t v : {c.reads, c.writes, c.kb_read, c.kb_written, c.errors, c.io_ms})
            n = UsbPut(d, n, v, 5);
        n = UsbPut(d, n, c.unit_ready, 2);
        n = UsbPut(d, n, c.capacity, 2);
        return static_cast<int>(n);
    }
    if (part == 2 && len > 1)
    {
        cardlend::State& card = cardlend::Get();
        if ((arg[1] != 0) != card.lent)
            card.asked = true;
        return static_cast<int>(n);
    }
    return -1;
}

static void DevInfo(dev::Info& info)
{
#ifdef __arm__
    info.image = reinterpret_cast<const uint8_t*>(&g_pfnVectors);
    info.image_len = reinterpret_cast<uint32_t>(&_sidata) - reinterpret_cast<uint32_t>(&g_pfnVectors)
                     + (reinterpret_cast<uint32_t>(&_edata) - reinterpret_cast<uint32_t>(&_sdata));
    info.reset_cause = RCC->RSR;
    RCC->RSR |= RCC_RSR_RMVF;
    info.option_bytes = FLASH->OPTSR_CUR;
    System::InitBackupSram();
#endif
    info.crc = crash::Crc32(info.image, info.image_len);
    info.crc_done = true;
    info.crash = &crash_mem;
    info.usb = UsbDev;
    crash::Boot(crash_mem, info.reset_cause, info.crc, info.image_len);
#if FRIZZ_BENCH
    info.kind = 1;
    info.bench = [](uint8_t part, uint32_t offset, uint8_t* d) { return bench.Report(part, offset, d); };
#else
    info.loops = [](const uint8_t* arg, size_t len, uint8_t* d) { return loop_loader.Dev(arg, len, d); };
#endif
}

#ifdef __arm__
/** A fault's registers into the crash record (Crash.h), then the chip's reset, as the launcher
 *  hands over: the bootloader starts the launcher. FrizzFaultEntry passes the stacked frame.
 *  The MPU is off in a HardFault (no HFNMIENA), so the backup SRAM is cached here as the
 *  default map has it: the record is cleaned out of the D-cache, or the reset loses it */
extern "C" __attribute__((used, noinline)) void FrizzFault(const uint32_t* frame, uint32_t ipsr)
{
    const uint32_t fault[] = {SCB->CFSR, SCB->HFSR, SCB->MMFAR, SCB->BFAR};
    crash::Write(crash_mem.record, ipsr & 0x1FF, crash_mem.running, System::GetNow(),
                 midi_control.dev.reset_cause, frame, fault);
    SCB_CleanDCache();
    NVIC_SystemReset();
}

/** HardFault, MemManage, BusFault and UsageFault: the stack the frame went to, by EXC_RETURN */
extern "C" __attribute__((naked)) void FrizzFaultEntry()
{
    __asm__ volatile("tst lr, #4    \n"
                     "ite eq        \n"
                     "mrseq r0, msp \n"
                     "mrsne r0, psp \n"
                     "mrs r1, ipsr  \n"
                     "b FrizzFault  \n");
}

/** The vector table, copied where the faults can be FrizzFaultEntry's: libDaisy's
 *  HardFault_Handler (system.cpp) stops at a breakpoint, which without a debugger locks the
 *  core up. The image's own table stays as loaded, for IDENT's CRC. In the DTCM, uncached,
 *  1 KB aligned as VTOR wants for the H750's 166 vectors */
static uint32_t __attribute__((section(".dtcmram_bss"), aligned(1024))) vectors[256];
static void CatchFaults()
{
    const uint32_t* from = reinterpret_cast<const uint32_t*>(SCB->VTOR);
    const size_t n = std::min<size_t>(256, &_stext - &g_pfnVectors);
    for (size_t i = 0; i < n; i++)
        vectors[i] = from[i];
    for (size_t i = 3; i <= 6; i++)
        vectors[i] = reinterpret_cast<uint32_t>(&FrizzFaultEntry);
    __DSB();
    SCB->VTOR = reinterpret_cast<uint32_t>(vectors);
    __DSB();
    __ISB();
}

/** The watchdog (Crash.h), fed only by the audio callback: LSI's 32 kHz by 32, a count a ms */
static void StartWatchdog()
{
    IWDG1->KR = 0xCCCC;
    IWDG1->KR = 0x5555;
    IWDG1->PR = 3;
    IWDG1->RLR = crash::kWatchdogMs;
    for (uint32_t i = 0; IWDG1->SR && i < 1000000; i++)
        ;
    IWDG1->KR = 0xAAAA;
}
static inline void FeedWatchdog() { IWDG1->KR = 0xAAAA; }
#else
static void CatchFaults() {}
static void StartWatchdog() {}
static inline void FeedWatchdog() {}
#endif

/** Clears the buffers in the Daisy Seed's external SDRAM at boot. The delay's, freezer's,
 *  tape stop's and reverb pre-delay's buffers live there, and unlike internal-RAM statics
 *  they aren't zeroed by the startup code. Only the ~10 of the 64 MB they take, which
 *  shortens the boot: the loop and the event log (SDRAM_NOINIT) need none */
void ZeroSDRAM()
{
    std::fill(&_ssdram_bss, &_esdram_bss, 0);
}

/** breakdown:
 *  Inputs:
 *  Channel 1 - Microphone
 *  Channel 2 - X
 *  Channel 3 - Aux L
 *  Channel 4 - Aux R
 *
 *  Outputs:
 *  Channel 1 - Headphone L
 *  Channel 2 - Headphone R
 *  Channel 3 - Master L
 *  Channel 4 - Master R
 */

// The audio ISR. Called by the Daisy audio driver once per block
FRIZZ_HOT_CALLBACK void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
#if FRIZZ_BENCH
    bench.BlockStart();
#else
    const uint32_t start_tick = System::GetTick(); // the load, for MIDI (MidiControl.h)
#endif
    FeedWatchdog(); // here only: a hung MainLoop doesn't set it off
    midi_clock.Process(sample_clock);
    sample_clock += size;
    // MIDI's Start, Stop, Continue and Song Position, for the loop before it plays this block
    MidiControl::Sync sync;
    while(midi_control.TakeSync(sync))
        engine.looper.Follow(sync.msg, sync.spp, sync.tick);
    BENCH_MARK(MIDI);

    if(!main_reads_keys)
    {
        hw.ProcessAllControls();
        BENCH_MARK(CONTROLS);
        ui.GenerateEvents();
        BENCH_MARK(EVENTS);
    }

    if(booting)
    {
        if(!main_loop_running)
            ui.DoEvents(false);
        midi_out.Skip();

        for(size_t i = 0; i < size; i++)
        {
            out[0][i] = out[1][i] = out[2][i] = out[3][i] = 0.f;
        }

        return;
    }

    if(input_jack.Changed(hw.jack_detect.Read()))
    {
        // AUX or the mic; USB, picked by hand, stays (#196)
        if(engine.Source() != PassthroughEngine::kUsb)
            engine.SetMicInput(!input_jack.plugged);
        event_log.Jack(input_jack.plugged);
    }

    // the computer's audio, the source USB (silence unless it plays), and whether it records
    engine.SetUsbInput(usb::Receive(size));
    const bool to_host = usb::TxStreaming();
    engine.SetUsbOut(to_host);

    // the developers' test signal and peak meters (MidiControl.h's dev::Audio), off unless
    // asked for over USB: then this flag, read once
    const bool dev_audio = midi_control.audio.on;
    if(dev_audio)
        in = midi_control.audio.In(in, size);
#if FRIZZ_BENCH
    engine.Process(bench.Input(in, size), out, size);
#else
    engine.Process(in, out, size);
#endif
    if(dev_audio)
        midi_control.audio.Out(out, size);
    // the master before VOLUME to the computer
    if(to_host)
        usb::Send(engine.UsbOut(), size);
    midi_out.Process(size, engine, midi_control.GetMidiOut());
#if FRIZZ_BENCH
    bench.BlockEnd(engine);
#else
    midi_control.BlockTime(System::GetTick() - start_tick);
#endif
}

uint32_t uit, now;

/** The card lent to the computer as a USB drive, or taken back, as the settings page or the
 *  developers' SysEx asked (CardLend.h), and the computer's transfers. True while it's lent */
static void CardRemount()
{
    f_mount(&fsi.GetSDFileSystem(), fsi.GetSDPath(), 1);
    EnterFrizzDir();
}

__attribute__((optimize("Os"))) static bool CardLendProcess(uint32_t now)
{
    cardlend::State& card = cardlend::Get();
    usb::CardTask();
    const bool gone = usb::TakeCardGone();
    const bool asked = card.asked;
    card.asked = false;
    if (card.lent && (gone || asked))
    {
        usb::CardMode(false);
        usb::CardTask(); // one the computer still waited on fails
        CardRemount();
        card.lent = false;
#if !FRIZZ_BENCH
        loop_loader.Request(LoopLoader::SCANNING); // the computer may have changed them
#endif
    }
    else if (!card.lent && asked)
    {
        const bool busy = engine.looper.GetState() == chompi::Looper::State::RECORDING || scene_store.Busy()
                          || event_log.Writing()
#if !FRIZZ_BENCH
                          || loop_loader.Busy()
#endif
            ;
        if (!usb::Host() || busy)
            card.refused.Start(now);
        else
        {
            // FatFs lets go of it: nothing of it cached that the computer would change under it
            f_mount(nullptr, fsi.GetSDPath(), 0);
            card.lent = true;
            usb::CardMode(true);
        }
    }
    return card.lent;
}

uint32_t batt;

void MainLoop(void* data)
{
    midi_control.dev.passes = midi_control.dev.passes + 1; // for kCmdDev's IDENT
#if FRIZZ_BENCH
    while (midi_control.dev.hang) // kCmdDev's TEST: a hung MainLoop, for RESET
        ;
#endif
    if(booting)
    {
        hw.LowBatteryLockoutCheck();
        booting = false;
    }
    else if(!boot_done && ui.BootDone())
    {
        ui.StopBootAnimation();
        boot_done = true;
    }

    now = daisy::System::GetNow();
    crash_mem.running.alive_ms = now; // when MainLoop last ran, for a watchdog's record

    if (now - uit > 1)
    {
        ui.DoEvents();
        uit = now;
    }

    // USB audio switched on the settings page: off the bus and back with the other layout
    if (ui.UsbAudio() != usb::Audio())
        usb::SetAudio(ui.UsbAudio());
#if FRIZZ_USB_SAFETY
    usb::Watch(now); // make USB_SAFETY=1: an enumeration that hangs restarts into the launcher
#endif

    // the card lent to the computer (CardLend.h): meanwhile nothing here touches it, what
    // waits for it waits
    const bool card_lent = CardLendProcess(now);

#if !FRIZZ_BENCH
    // loops from the card: a step, before the card's other users; it leaves them the card
    // when they want it (LoopLoader.h)
    if (!card_lent)
    {
        loop_loader.Process(scene_store.Busy() || event_log.Writing());
        // the Loops page's: the loads it asks for, the switch (LoopControls.h)
        ui.LoopsProcess();
    }
#endif
    // a scene saved, copied or deleted: the card is written here, never in the audio callback
    if (!card_lent)
        scene_store.Process(event_log.Writing());
    // a bug report asked for: written a chunk per pass, also from MainLoop only; not with
    // the card kept out of it (PERSIST 0), nor lent
    event_log.Process(now, midi_clock, scene_store.InRam() || card_lent);

    // a restart asked for over MIDI (MidiClock.h), once no report is being written and the
    // master settings that were waiting are on the card, or a second after it was asked for
    // whatever still comes in (a DAW's automation): the chip's reset, as the launcher hands
    // over, so the bootloader starts what's in QSPI
    if (midi_clock.RestartRequested() && !event_log.Writing())
    {
        if (!restart_asked)
            restart_asked = now | 1; // 0 is "not yet"
        if ((ui.MasterSettled() && !scene_store.Busy()) || now - restart_asked > kRestartWaitMs)
            NVIC_SystemReset();
    }

#if FRIZZ_BENCH
    bench.Process();
    bench.DrawLeds(now);
#endif

    if(now - batt > 20)
    {
        hw.LowBatteryLockoutCheck();
        batt = now;
    }

    System::DelayUs(10);
}

// the per-sample code (FRIZZ_HOT, make ITCM=1), from where the bootloader put it to ITCM
// (chompi_sram.lds)
#if defined(__arm__) && defined(FRIZZ_ITCM)
extern uint32_t _siitcmdata, _sitcmram, _eitcmram;
static void CopyItcm()
{
    // all of it written first: the ITCM has ECC, and a fetch running ahead into words never
    // written would read an ECC error (the code's own end is 8-byte aligned, the rest isn't)
    volatile uint32_t* const itcm = reinterpret_cast<volatile uint32_t*>(D1_ITCMRAM_BASE);
    for (size_t i = 0; i < 0x10000 / 4; i++)
        itcm[i] = 0;
    const uint32_t* from = &_siitcmdata;
    for (uint32_t* to = &_sitcmram; to < &_eitcmram;)
        *to++ = *from++;
    __DSB();
    __ISB();
}
#else
static void CopyItcm() {}
#endif

int main(void)
{
    CopyItcm();
    hw.Init();
    DevInfo(midi_control.dev);
    CatchFaults();

    midi_clock.Init(hw.seed.AudioSampleRate());

    hw.MpWrite(0x0c, 0B01010001); // set BATT_LOW to 3V, turn on

    hw.MpReadAll();

    for(size_t i = 0; i < 10; i++)
    {
        hw.LowBatteryLockoutCheck();
        System::Delay(10);
    }

    // USB's lines from the charger to the Daisy, and TinyUSB's task; FRIZZ goes on the bus
    // once the card's settings say how (usb::Start below), still long before the audio
    // callback runs the engine (#195)
    hw.usb_sw.Write(false);     // give USB control
    daisy::System::Delay(1); // Wait a sec
    hw.MpWrite(0x0a, 0B00100100); // AutoDPDM
    daisy::System::Delay(1); // Wait a sec
    hw.usb_sw.Write(true);     // take USB control
    usb::Init();

    /** SDMMC Init */
    System::Delay(100);
    SdmmcHandler::Config sd_cfg;
    sd_cfg.speed = SdmmcHandler::Speed::FAST;
    sd_cfg.width = SdmmcHandler::BusWidth::BITS_4;
    sdmmc.Init(sd_cfg);
    SdFlowControl();
    System::Delay(100);
    fsi.Init(FatFSInterface::Config::MEDIA_SD);
    SdDiskLink(fsi.GetSDPath());
    // the card as a USB drive, while lent (CardLend.h): its sectors through SdDisk.h's transfers
    usb::SetCard([](bool write, uint8_t* buf, uint32_t sector, uint32_t count) {
        return SdDiskTransfer(write, buf, sector, count);
    }, [] { return SdDiskSectors(); });
    System::Delay(100);
    // FRIZZ's files live in /FRIZZ, created on first start (SceneStore.h)
    scene_store.Init(&fsi.GetSDFileSystem(), fsi.GetSDPath());
    // on the bus, as a sound card or not (frizz_master.txt's usb_audio)
    usb::Start(scene_store.master.usb_audio);
#if !FRIZZ_BENCH
    // the loop slots' scan starts, a step a MainLoop pass
    loop_loader.Init(loop_load_mem, &loop_loader_mem, &fsi.GetSDFileSystem(), [] { return System::GetNow(); },
                     [] { return System::GetTick(); }, System::GetTickFreq());
#endif

    engine.Init(hw.seed.AudioSampleRate(), loop_mem, &midi_clock,
                delay_mem, kDelayFrames, &reverb,
                freezer_mem_l, freezer_mem_r, kFreezerFrames,
                tapestop_mem_l, tapestop_mem_r, kTapeStopFrames);

    LedSetup();
    event_log.Init(&event_log_mem, &fsi.GetSDFileSystem(), fsi.GetSDPath());
    midi_control.Init(&midi_clock, &event_log, scene_store.master.midi_channel,
                      scene_store.master.midi_transport, System::GetTickFreq(),
                      24.f / hw.seed.AudioSampleRate());
    midi_out.Init(&midi_clock, &midi_control.feedback_trs);
    // MIDI out's USB bytes from TinyUSB's task, so a MainLoop held up by the card doesn't hold
    // up the clock out over USB (#265)
    usb::SetTaskHook([] { midi_out.FlushUsb(); });
    ui.Init(&engine, &hw, &scene_store, &event_log, &midi_control);
#if !FRIZZ_BENCH
    ui.InitLoops(&loop_loader, loop_mem);
#endif
    input_jack.plugged = hw.jack_detect.Read();
    engine.SetMicInput(!input_jack.plugged); // AUX or the mic; USB only by hand
#if FRIZZ_BENCH
    bench.Init(hw.seed.AudioSampleRate(), 24, &fsi.GetSDFileSystem(), fsi.GetSDPath());
#endif

    hw.StartAudio(AudioCallback);
    // after the battery checks above, so their low-battery sleep (STOP, hardware.h), which
    // stops the clocks, never starts with the watchdog on: else every start would reset into
    // the same sleep again (Crash.h). Keep it after them
    StartWatchdog();

    // safe while audio runs: the engine (looper, delay) doesn't run until booting is done
    ZeroSDRAM();

    now = daisy::System::GetNow();
    uit = now;

    batt = now;

    // get any junk out of the SRs for kKeyReadMs, timed: 5000 reads 0.1 ms apart took longer
    // on the device, the reads' own time on top; CHOMPI, PLAY and LOOP held throughout (4 of
    // 5 reads): shipping mode
    uint32_t sleep_state = 0, reads = 0;
    const uint32_t read_start = System::GetNow();

    main_reads_keys = true;
    for(; System::GetNow() - read_start < kKeyReadMs; reads++)
    {
        hw.ProcessAllControls();
        sleep_state += hw.button_sr.State(int(Hardware::SwId::KEY_26))
                        && hw.button_sr.State(int(Hardware::SwId::KEY_27))
                        && hw.button_sr.State(int(Hardware::SwId::KEY_28));
        // a key pressed or held meanwhile is taken here, so ui.h's GenerateEvents never
        // sees it go down: its press reaches no page, and its release lets go of nothing
        for(int k = 0; k < static_cast<int>(Hardware::SwId::SR_LAST); k++)
        {
            hw.button_sr.RisingEdge(k);
            hw.button_sr.FallingEdge(k);
        }

        System::DelayUs(100);
    }

    main_reads_keys = false;

    if(sleep_state > reads / 5 * 4)
        hw.MpWrite(0x08, 0B10111111); // SHIPPING MODE

    // the event log starts here, the card read
    event_log.Start(daisy::System::GetNow(), hw.GetToggleState(), input_jack.plugged);
    main_loop_running = true;
    while (1)
    {
        MainLoop(nullptr);
    }
}
