/** @file chompi_main.cpp
 *  @brief Firmware entry point
 *
 *  CHOMPI (built on the Daisy Seed / STM32H7) has two places code runs, in
 *  order of priority:
 *   1. AudioCallback() - the audio ISR. Runs once per audio block (~24 samples
 *      at 48kHz here). Polls the controls and MIDI (clock, and the panel played over it), and
 *      passes the AUX input through the engine.
 *   2. MainLoop() - Lowest priority, handles UI dispatch, battery checks, writing the FX
 *      scenes to the SD card, and boot-time stuff.
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
#if FRIZZ_BENCH
#include "Bench.h"
#endif

using namespace daisy;
using namespace chompi;

// How long the boot animation runs (and the outputs stay muted) before audio starts
static const uint32_t kBootScreenMs = 1250;

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
EventLogMem DSY_SDRAM_BSS event_log_mem;
#if FRIZZ_BENCH
// FRIZZ-bench.bin (make BENCH=1): measures the audio callback's load (Bench.h)
Bench bench;
// where its time goes (BenchProfile.h)
BenchProfile chompi::bench_profile;
#endif

int16_t DSY_SDRAM_BSS loop_mem[kLoopMemSize];

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

// both read by the audio callback, which keeps the outputs muted until they are done
volatile bool booting = true;
bool rainbow_done = false;
volatile bool loading_screen = true;

// a restart over MIDI waits this long at most for the card (MainLoop)
static const uint32_t kRestartWaitMs = 1000;
static uint32_t restart_asked = 0;

// where the SDRAM's buffers (DSY_SDRAM_BSS) begin and end: chompi_sram.lds's .sdram_bss
extern "C" uint32_t _ssdram_bss;
extern "C" uint32_t _esdram_bss;

/** Clears the buffers in the Daisy Seed's external SDRAM at boot. The loop's, delay's,
 *  freezer's and tape stop's buffers live there, and unlike internal-RAM statics they aren't
 *  zeroed by the startup code. Only the ~43 of the 64 MB they take, which shortens the boot */
void ZeroSDRAM()
{
    std::fill(&_ssdram_bss, &_esdram_bss, 0);
}

/** breakdown:
 *  Inputs:
 *  Channel 1 - Microphone (unused)
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
    midi_clock.Process(sample_clock);
    sample_clock += size;
    BENCH_MARK(MIDI);

    if(!main_reads_keys)
    {
        hw.ProcessAllControls();
        BENCH_MARK(CONTROLS);
        ui.GenerateEvents();
        BENCH_MARK(EVENTS);
    }

    if(booting || loading_screen)
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

#if FRIZZ_BENCH
    engine.Process(bench.Input(in, size), out, size);
    midi_out.Process(size, engine, midi_control.GetMidiOut());
    bench.BlockEnd(engine);
#else
    engine.Process(in, out, size);
    midi_out.Process(size, engine, midi_control.GetMidiOut());
    midi_control.BlockTime(System::GetTick() - start_tick);
#endif
}

uint32_t uit, now, boot_start;

#if !NO_BATT
uint32_t batt;
#endif

void MainLoop(void* data)
{
    if(booting)
    {
        hw.LowBatteryLockoutCheck();
        booting = false;
    }
    else if(!rainbow_done && !loading_screen)
    {
        ui.StopBootAnimation();
        ui.RainbowWave();
        rainbow_done = true;
    }

    now = daisy::System::GetNow();

    if (now - uit > 1)
    {
        ui.DoEvents();
        uit = now;
    }

    // MIDI out's bytes for USB, which only MainLoop may send (MidiOut.h)
    midi_out.FlushUsb();

    // a scene saved, copied or deleted: the card is written here, never in the audio callback
    scene_store.Process();
    // a bug report asked for: written a chunk per pass, also from MainLoop only
    event_log.Process(now, midi_clock);

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

    if (loading_screen && now - boot_start > kBootScreenMs)
        loading_screen = false;

    #if !NO_BATT

    if(ui.InRainbows())
    {
        batt = now;
    }
    else if(now - batt > 20)
    {
        hw.LowBatteryLockoutCheck();
        batt = now;
    }
    #endif

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

    midi_clock.Init(hw.seed.AudioSampleRate());

    hw.MpWrite(0x0c, 0B01010001); // set BATT_LOW to 3V, turn on

    hw.MpReadAll();

    for(size_t i = 0; i < 10; i++)
    {
        hw.LowBatteryLockoutCheck();
        System::Delay(10);
    }

    /** SDMMC Init */
    System::Delay(100);
    SdmmcHandler::Config sd_cfg;
    sd_cfg.speed = SdmmcHandler::Speed::FAST;
    sd_cfg.width = SdmmcHandler::BusWidth::BITS_4;
    sdmmc.Init(sd_cfg);
    System::Delay(100);
    fsi.Init(FatFSInterface::Config::MEDIA_SD);
    System::Delay(100);
    // FRIZZ's files live in /FRIZZ, created on first start (SceneStore.h)
    scene_store.Init(&fsi.GetSDFileSystem(), fsi.GetSDPath());

    engine.Init(hw.seed.AudioSampleRate(), loop_mem, &midi_clock,
                delay_mem, kDelayFrames, &reverb,
                freezer_mem_l, freezer_mem_r, kFreezerFrames,
                tapestop_mem_l, tapestop_mem_r, kTapeStopFrames);

    LedSetup();
    event_log.Init(&event_log_mem, &fsi.GetSDFileSystem(), fsi.GetSDPath());
    midi_control.Init(&midi_clock, &event_log, scene_store.master.midi_channel,
                      scene_store.master.midi_transport, System::GetTickFreq(),
                      24.f / hw.seed.AudioSampleRate());
    midi_out.Init(&midi_clock);
    ui.Init(&engine, &hw, &scene_store, &event_log, &midi_control);
#if FRIZZ_BENCH
    bench.Init(hw.seed.AudioSampleRate(), 24, &fsi.GetSDFileSystem(), fsi.GetSDPath());
#endif

    hw.StartAudio(AudioCallback);

    // safe while audio runs: the engine (looper, delay) doesn't run until booting is done
    ZeroSDRAM();

    now = daisy::System::GetNow();
    uit = now;
    boot_start = now;

    #if !NO_BATT
    batt = now;
    #endif

    // get any junk out of the SRs, takes .5s; CHOMPI, PLAY and LOOP held throughout: shipping mode
    uint32_t sleep_state = 0;

    main_reads_keys = true;
    for(int i = 0; i < 5000; i++)
    {
        hw.ProcessAllControls();
        sleep_state += hw.button_sr.State(int(Hardware::SwId::KEY_26))
                        && hw.button_sr.State(int(Hardware::SwId::KEY_27))
                        && hw.button_sr.State(int(Hardware::SwId::KEY_28));

        System::DelayUs(100);
    }

    main_reads_keys = false;

    if(sleep_state > 4000)
        hw.MpWrite(0x08, 0B10111111); // SHIPPING MODE

    hw.usb_sw.Write(false);     // give USB control
    daisy::System::Delay(1); // Wait a sec
    hw.MpWrite(0x0a, 0B00100100); // AutoDPDM
    daisy::System::Delay(1); // Wait a sec
    hw.usb_sw.Write(true);     // take USB control

    // the event log starts here, its memory cleared and the card read
    event_log.Start(daisy::System::GetNow(), hw.GetToggleState());
    main_loop_running = true;
    while (1)
    {
        MainLoop(nullptr);
    }
}
