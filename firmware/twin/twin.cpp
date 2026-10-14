/** @file twin.cpp
 *  @brief The virtual CHOMPI (twin.h): the firmware's chompi_main.cpp compiled as is, and the
 *  board it runs on (host/board.h): the clock, the pins with the 4021 chains and encoders on
 *  them, the MP2722 charger, the LED DMA and the audio driver.
 */
#ifdef __EMSCRIPTEN__
#include <emscripten/fiber.h>
#else
#include <ucontext.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

// the firmware itself: its main() becomes the coroutine below
#define main frizz_main
#include "chompi_main.cpp"
#undef main

// ZeroSDRAM's range, empty (host/daisy_core.h)
extern "C" uint32_t _ssdram_bss = 0;

#include "twin.h"

// The engine's tempo clock and the MIDI clock's source are private. An explicit template
// instantiation may name a private member, so Probe() reads them this way, without a getter
// in the firmware: compare.sh and ui-at.sh build older firmwares with this twin too
template <typename Tag, typename Tag::type M>
struct TwinReach
{
    friend typename Tag::type Reach(Tag) { return M; }
};
struct ReachSource
{
    typedef MidiClock::Source MidiClock::*type;
    friend type Reach(ReachSource);
};
template struct TwinReach<ReachSource, &MidiClock::source_>;
#if __has_include("TempoClock.h")
#define TWIN_HAS_TEMPO 1
struct ReachTempo
{
    typedef chompi::TempoClock PassthroughEngine::*type;
    friend type Reach(ReachTempo);
};
template struct TwinReach<ReachTempo, &PassthroughEngine::tempo_clock_>;
#endif

// the firmware's restart (chompi_main.cpp): the chip would reset; here it's noted
static bool restarted = false;
void NVIC_SystemReset() { restarted = true; }

namespace twin
{
// ======== clock and the two contexts ========
// One block, 24 samples at 48 kHz, is exactly 0.5 ms
static const uint64_t kBlockNs = 500000;
// What a GetNow() costs main(), so a loop that only polls the clock still moves on (the low
// battery warning's)
static const uint64_t kGetNowNs = 100;

static uint64_t block_ns = 0; // start of the current block: the audio callback's time
static uint64_t main_ns = 0;  // main()'s own time, ahead of block_ns while it runs
static uint64_t window_end = 0;
static bool in_main = false;
static bool booted = false;
static bool main_done = false;
static bool powered = true;

// main() on its own stack: ucontext natively, a fiber (Asyncify) in the browser
static std::vector<char> main_stack;
static void MainEntry();
#ifdef __EMSCRIPTEN__
static emscripten_fiber_t driver_fiber, main_fiber;
static std::vector<char> driver_unwind, main_unwind;
static void MainFiber(void*) { MainEntry(); }
static void StartMain()
{
    main_unwind.resize(1 << 20);
    driver_unwind.resize(1 << 20);
    emscripten_fiber_init(&main_fiber, MainFiber, nullptr, main_stack.data(), main_stack.size(),
                          main_unwind.data(), main_unwind.size());
    emscripten_fiber_init_from_current_context(&driver_fiber, driver_unwind.data(),
                                               driver_unwind.size());
}
static void ToMain() { emscripten_fiber_swap(&driver_fiber, &main_fiber); }
static void ToDriver() { emscripten_fiber_swap(&main_fiber, &driver_fiber); }
#else
static ucontext_t driver_ctx, main_ctx;
static void StartMain()
{
    getcontext(&main_ctx);
    main_ctx.uc_stack.ss_sp = main_stack.data();
    main_ctx.uc_stack.ss_size = main_stack.size();
    main_ctx.uc_link = nullptr;
    makecontext(&main_ctx, MainEntry, 0);
}
static void ToMain() { swapcontext(&driver_ctx, &main_ctx); }
static void ToDriver() { swapcontext(&main_ctx, &driver_ctx); }
#endif

static void Yield()
{
    in_main = false;
    ToDriver();
    in_main = true;
}

static void MainEntry()
{
    in_main = true;
    frizz_main();
    // main() never returns on the device; if it did, it would stay here
    main_done = true;
    for (;;)
        Yield();
}

bool InMain() { return in_main; }

uint64_t NowNs()
{
    if (!in_main)
        return block_ns;
    main_ns += kGetNowNs;
    while (main_ns >= window_end)
        Yield();
    return main_ns;
}

static uint64_t clock_start_ns = 0;
void SetClockStartMs(uint32_t ms) { clock_start_ns = uint64_t(ms) * 1000000; }
uint64_t FirmwareNs() { return NowNs() + clock_start_ns; }

// the audio callback's waits (the 4021's DelayTicks) take no time: it runs at its block's start
void DelayNs(uint64_t ns)
{
    if (!in_main)
        return;
    main_ns += ns;
    while (main_ns >= window_end)
        Yield();
}

// ======== the board's wiring (hardware.h) ========
static int PinId(const daisy::Pin& p) { return p.Id(); }

// the keys' chain: 5 CD4021s, 40 inputs, Hardware::SwId order; the encoders' chain: 1 CD4021,
// A and B of encoders 1-4. Inputs are active low (pulled up, a key pulls its input down)
struct Chain4021
{
    int clk, latch, data, size;
    bool inputs[40];
    bool loaded[40];
    int shifted;
    bool clk_level;
};

static Chain4021 keys_sr = {PinId(daisy::seed::D8), PinId(daisy::seed::D7),
                            PinId(daisy::seed::D9), 40, {}, {}, 0, false};
static Chain4021 enc_sr = {PinId(daisy::seed::D22), PinId(daisy::seed::D23),
                           PinId(daisy::seed::D19), 8, {}, {}, 0, false};

// encoders 5 and 6 are on their own pins, encoder 5's switch too
static const int kEnc5A = PinId(daisy::seed::D0), kEnc5B = PinId(daisy::seed::D20),
                 kEnc5Sw = PinId(daisy::seed::D10);
static const int kEnc6A = PinId(daisy::seed::D15), kEnc6B = PinId(daisy::seed::D17);

static bool pin_level[256];

// AUX's jack detection: high with a cable in (hardware.h's jack_detect), as at first
static const int kJackDetect = PinId(daisy::seed::D21);
static bool jack_plugged = true;

static void ChainWrite(Chain4021& c, int id, bool level)
{
    if (id == c.latch && level)
    {
        // parallel load: Q8 of the last chip shows the chain's last input at once
        memcpy(c.loaded, c.inputs, sizeof(c.loaded));
        c.shifted = 0;
    }
    else if (id == c.clk)
    {
        if (level && !c.clk_level && !pin_level[c.latch])
            c.shifted++;
        c.clk_level = level;
    }
}

static bool ChainRead(const Chain4021& c)
{
    // libDaisy's driver reads index (size - 1 - i) at the i-th clock: the wiring it was written for
    const int idx = c.size - 1 - c.shifted;
    return idx >= 0 ? c.loaded[idx] : false;
}

void PinWrite(int id, bool level)
{
    pin_level[id] = level;
    ChainWrite(keys_sr, id, level);
    ChainWrite(enc_sr, id, level);
}

bool PinRead(int id)
{
    if (id == keys_sr.data)
        return ChainRead(keys_sr);
    if (id == enc_sr.data)
        return ChainRead(enc_sr);
    return pin_level[id];
}

// ======== encoders: each detent a full quadrature cycle, B leading A for +1 ========
struct Quadrature
{
    int pending = 0;     // detents still to play, signed
    int step = 0;        // 0 at rest, 1..4 within a detent
    uint64_t step_end = 0;
};
static Quadrature quad[6];
static const uint64_t kQuadStepNs = 2000000; // 4 steps, 8 ms a detent

static void SetEncoderLines(int enc, bool a, bool b)
{
    if (enc < 4)
    {
        enc_sr.inputs[enc * 2] = a;
        enc_sr.inputs[enc * 2 + 1] = b;
    }
    else
    {
        pin_level[enc == 4 ? kEnc5A : kEnc6A] = a;
        pin_level[enc == 4 ? kEnc5B : kEnc6B] = b;
    }
}

static void StepEncoders()
{
    // (A, B) through one detent from rest (1, 1): +1 lowers B first, -1 lowers A first
    static const bool kCw[4][2] = {{1, 0}, {0, 0}, {0, 1}, {1, 1}};
    static const bool kCcw[4][2] = {{0, 1}, {0, 0}, {1, 0}, {1, 1}};
    for (int e = 0; e < 6; e++)
    {
        Quadrature& q = quad[e];
        if (q.step == 0 && q.pending == 0)
            continue;
        if (q.step != 0 && block_ns < q.step_end)
            continue;
        if (q.step == 4)
        {
            q.step = 0;
            q.pending += q.pending > 0 ? -1 : 1;
            if (q.pending == 0)
                continue;
        }
        const bool (*seq)[2] = q.pending > 0 ? kCw : kCcw;
        SetEncoderLines(e, seq[q.step][0], seq[q.step][1]);
        q.step++;
        q.step_end = block_ns + kQuadStepNs;
    }
}

// ======== the MP2722 charger on I2C ========
static uint8_t mp_regs[256];
static uint8_t mp_read_reg = 0;
static float batt_volts = 3.8f;
static bool batt_plugged = false;
static bool batt_full = false;

void I2cWrite(uint16_t, const uint8_t* data, size_t size)
{
    if (size == 1)
        mp_read_reg = data[0];
    else if (size >= 2)
    {
        mp_regs[data[0]] = data[1];
        // the firmware's shipping mode (hardware.h): the battery is cut off, unless on a cable
        if (data[0] == 0x08 && data[1] == 0xBF && !batt_plugged)
            powered = false;
    }
}

void I2cRead(uint16_t, uint8_t* data, size_t size)
{
    // BATT_LOW's threshold, as hardware.h sets it: 0x5D 3V3, otherwise 3V
    const float threshold = mp_regs[0x0c] == 0x5D ? 3.3f : 3.f;
    for (size_t i = 0; i < size; i++)
    {
        const uint8_t reg = mp_read_reg + i;
        uint8_t v = 0;
        if (reg == 0x12)
            v = batt_plugged ? 1 << 6 : 0; // VIN_GD
        else if (reg == 0x13)
            v = batt_plugged ? (batt_full ? 0b101 : 0b011) << 5 : 0; // CHG_STAT
        else if (reg == 0x16)
            v = batt_volts < threshold ? 1 << 4 : 0; // BATT_LOW_STAT
        data[i] = v;
    }
}

void Stop()
{
    // STOP mode: nothing runs until an interrupt; here, a while
    DelayNs(100000000);
}

// ======== LEDs: the DMA's PWM durations back to the bytes the WS2812s latch ========
struct LedDma
{
    const uint32_t* data = nullptr;
    size_t size = 0;
    DmaDone done = nullptr;
    void* context = nullptr;
    bool running = false;
};
static LedDma led_dma[5]; // by timer channel: 2 the keys' SMT chain, 4 the panel's PTH chain

void LedDmaStart(int channel, const uint32_t* data, size_t size, DmaDone done, void* context)
{
    led_dma[channel] = {data, size, done, context, true};
}

// a chain's DMA takes about a block; then its callback starts the other chain (temp_led_stuff.h)
static void StepLedDma()
{
    for (int ch : {2, 4})
    {
        if (!led_dma[ch].running)
            continue;
        led_dma[ch].running = false;
        if (led_dma[ch].done)
            led_dma[ch].done(led_dma[ch].context);
        break;
    }
}

// Each LED takes the first 24 pulses that reach it and passes the rest on; a zero-length
// pulse (the porch) is no bit at all. The PTH chips read R, G, B; the SMT chips G, R, B
static Rgb DecodeLed(int channel, int index, bool grb)
{
    const LedDma& d = led_dma[channel];
    Rgb out = {0, 0, 0};
    if (!d.data)
        return out;
    int bit = 0;
    const int first = index * 24;
    uint8_t bytes[3] = {0, 0, 0};
    for (size_t i = 0; i < d.size && bit < first + 24; i++)
    {
        if (d.data[i] == 0)
            continue;
        if (bit >= first)
        {
            const int b = bit - first;
            if (d.data[i] >= static_cast<uint32_t>((chompi::kOneTime + chompi::kZeroTime) / 2))
                bytes[b / 8] |= 1 << (7 - b % 8);
        }
        bit++;
    }
    if (bit < first + 24)
        return out; // the bitstream doesn't reach this LED
    out.r = grb ? bytes[1] : bytes[0];
    out.g = grb ? bytes[0] : bytes[1];
    out.b = bytes[2];
    return out;
}

Rgb PthLed(int index) { return powered ? DecodeLed(4, index, false) : Rgb{0, 0, 0}; }
Rgb SmtLed(int index) { return powered ? DecodeLed(2, index, true) : Rgb{0, 0, 0}; }

static uint8_t Scale(uint8_t v, int by) { return static_cast<uint8_t>(std::min(255, v * by)); }
Rgb PthLedFull(int index)
{
    const Rgb c = PthLed(index);
    return {Scale(c.r, 11), Scale(c.g, 11), Scale(c.b, 11)};
}
Rgb SmtLedFull(int index)
{
    const Rgb c = SmtLed(index);
    return {Scale(c.r, 4), Scale(c.g, 4), Scale(c.b, 4)};
}

// ======== audio ========
static AudioCallback audio_cb = nullptr;
void StartAudio(AudioCallback cb) { audio_cb = cb; }

// ======== MIDI ========
static UartRx uart_rx = nullptr;
static void* uart_context = nullptr;
static std::deque<uint8_t> midi_in;
void UartListen(UartRx rx, void* context)
{
    uart_rx = rx;
    uart_context = context;
}

static UartRx usb_rx = nullptr;
static void* usb_context = nullptr;
static std::deque<uint8_t> usb_in;
static std::string usb_out;
void UsbListen(UartRx rx, void* context)
{
    usb_rx = rx;
    usb_context = context;
}
static std::vector<MidiOutByte> midi_out;
static uint64_t uart_tx_free_ns = 0; // when the UART has sent everything it was given
static const uint64_t kUartByteNs = 320000; // 10 bits at 31250 baud
bool UartTx(uint8_t byte)
{
    // the transmit register is free once at most the byte before is still going out
    if (uart_tx_free_ns > block_ns + kUartByteNs)
        return false;
    const uint64_t start = uart_tx_free_ns > block_ns ? uart_tx_free_ns : block_ns;
    uart_tx_free_ns = start + kUartByteNs;
    midi_out.push_back({start / 1e6, byte, false});
    return true;
}
static bool usb_host = true;
bool UsbHost();
void SetUsbHost(bool host) { usb_host = host; }
static bool in_usb_task = false; // TinyUSB's task's pass (usb::SetTaskHook), at the block's time
void UsbTx(const uint8_t* data, size_t size)
{
    usb_out.append(reinterpret_cast<const char*>(data), size);
    // MIDI out's (MidiOut.h): the clock, the transport, a Song Position; not SysEx
    const uint64_t at = in_usb_task || main_ns < block_ns ? block_ns : main_ns;
    if (size > 0 && (data[0] == 0xF2 || data[0] >= 0xF8))
        for (size_t i = 0; i < size; i++)
            midi_out.push_back({at / 1e6, data[i], true});
}
// what arrived since the last block, handed over at its start, as the UART's DMA and the USB
// interrupt do before the audio callback polls them
static void Deliver(std::deque<uint8_t>& q, void (*rx)(uint8_t*, size_t, void*), void* context)
{
    if (!rx || q.empty())
        return;
    std::vector<uint8_t> bytes(q.begin(), q.end());
    q.clear();
    rx(bytes.data(), bytes.size(), context);
}

// ======== USB (UsbDevice.h, #196) ========
// The computer's side of FRIZZ's USB: on the bus with one layout or the other, away for a
// switch, its audio both ways, the card lent. A firmware from before TinyUSB has none of it
// (UsbDevice.h missing): MIDI over USB as libDaisy's, the rest idle
static const float* usb_audio_in = nullptr; // SetUsbAudioIn's, for this Run()
static const float* usb_block_in = nullptr; // this block's, interleaved L R
static bool usb_recording = false;
static std::vector<float> usb_audio_out;
static struct
{
    bool started = false, audio = true;
    uint64_t away_until_ns = 0; // a switch: off the bus until then
    int enumerations = 0;
    void (*hook)() = nullptr;
    bool card_on = false, card_gone = false;
} usb_dev;
static const uint64_t kUsbAwayNs = 300000000; // as UsbDevice.cpp's kAwayTicks

static bool UsbOnBus() { return usb_dev.started && block_ns >= usb_dev.away_until_ns; }
bool UsbHost() { return usb_host && (UsbOnBus() || !usb_dev.started); }

// ======== the API (twin.h) ========
void SetUsbAudioIn(const float* lr) { usb_audio_in = lr; }
void SetUsbRecording(bool on) { usb_recording = on; }
std::vector<float> TakeUsbAudioOut()
{
    std::vector<float> out;
    out.swap(usb_audio_out);
    return out;
}
bool UsbAudioOn() { return usb_host && UsbOnBus() && usb_dev.audio; }
int UsbEnumerations() { return usb_dev.enumerations; }
#if __has_include("CardLend.h")
bool CardLent() { return cardlend::Get().lent; }
#else
bool CardLent() { return false; }
#endif
void EjectCard()
{
    if (usb_dev.card_on)
        usb_dev.card_gone = true;
}

std::map<std::string, std::string>& CardFiles() { return FakeCard::Get().files; }
void SetCardPresent(bool present) { FakeCard::Get().present = present; }
void SetCardSpace(size_t bytes) { FakeCard::Get().space = bytes; }
int CardOpenFiles() { return FakeCard::Get().open; }
void SetCardRawPolls(int polls) { FakeCard::Get().raw_polls = polls; }
int CardRawClash() { return FakeCard::Get().raw_clash; }
static uint64_t card_write_ns = 0;
static void CardWriteTime(size_t) { DelayNs(card_write_ns); }
void SetCardWriteMs(uint32_t ms)
{
    card_write_ns = uint64_t(ms) * 1000000;
    FakeCard::Get().write_time = ms ? CardWriteTime : nullptr;
}

void Boot()
{
    if (booted)
        return;
    booted = true;
    // every input pulled up: no key down, encoders at rest
    for (bool& l : pin_level)
        l = true;
    pin_level[kJackDetect] = jack_plugged;
    for (bool& l : keys_sr.inputs)
        l = true;
    // the mode switch down, as it's played: the play page (SetToggle(true) puts it up)
    keys_sr.inputs[static_cast<int>(chompi::Hardware::SwId::SW_TOG)] = false;
    for (bool& l : enc_sr.inputs)
        l = true;
    // a card always has /FRIZZ's parent
    FakeCard::Get().dirs[""] = true;

    main_stack.resize(1 << 20);
    StartMain();
}

uint32_t NowMs() { return static_cast<uint32_t>(block_ns / 1000000); }

void Run(size_t blocks, const float* in, float* out)
{
    float in_buf[kChannels][kBlockSize], out_buf[kChannels][kBlockSize];
    const float* in_ptr[kChannels] = {in_buf[0], in_buf[1], in_buf[2], in_buf[3]};
    float* out_ptr[kChannels] = {out_buf[0], out_buf[1], out_buf[2], out_buf[3]};

    for (size_t b = 0; b < blocks; b++)
    {
        for (size_t i = 0; i < kBlockSize; i++)
            for (int c = 0; c < kChannels; c++)
            {
                in_buf[c][i] = in ? in[(b * kBlockSize + i) * kChannels + c] : 0.f;
                out_buf[c][i] = 0.f;
            }

        if (booted && powered)
        {
            StepEncoders();
            StepLedDma();

            Deliver(midi_in, uart_rx, uart_context);
            Deliver(usb_in, usb_rx, usb_context);

            usb_block_in = usb_audio_in ? usb_audio_in + b * kBlockSize * 2 : nullptr;
            if (audio_cb)
                audio_cb(in_ptr, out_ptr, kBlockSize);
            // TinyUSB's task, which on the device runs at 4 kHz on its own timer: a pass a block
            if (usb_dev.hook)
            {
                in_usb_task = true;
                usb_dev.hook();
                in_usb_task = false;
            }

            window_end = block_ns + kBlockNs;
            if (!main_done && main_ns < window_end)
                ToMain();
        }

        if (out)
            for (size_t i = 0; i < kBlockSize; i++)
                for (int c = 0; c < kChannels; c++)
                    out[(b * kBlockSize + i) * kChannels + c] = powered ? out_buf[c][i] : 0.f;

        block_ns += kBlockNs;
    }
    usb_audio_in = nullptr;
}

// ======== keys ========
#define TWIN_KEY(name) {#name, static_cast<int>(chompi::Hardware::SwId::name)}
struct KeyName
{
    const char* name;
    int sw;
};
static const KeyName kKeys[] = {
    TWIN_KEY(KEY_1),  TWIN_KEY(KEY_2),  TWIN_KEY(KEY_3),  TWIN_KEY(KEY_4),  TWIN_KEY(KEY_5),
    TWIN_KEY(KEY_6),  TWIN_KEY(KEY_7),  TWIN_KEY(KEY_8),  TWIN_KEY(KEY_9),  TWIN_KEY(KEY_10),
    TWIN_KEY(KEY_11), TWIN_KEY(KEY_12), TWIN_KEY(KEY_13), TWIN_KEY(KEY_14), TWIN_KEY(KEY_15),
    TWIN_KEY(KEY_16), TWIN_KEY(KEY_17), TWIN_KEY(KEY_18), TWIN_KEY(KEY_19), TWIN_KEY(KEY_20),
    TWIN_KEY(KEY_21), TWIN_KEY(KEY_22), TWIN_KEY(KEY_23), TWIN_KEY(KEY_24), TWIN_KEY(KEY_25),
    TWIN_KEY(KEY_26), TWIN_KEY(KEY_27), TWIN_KEY(KEY_28), TWIN_KEY(ENC_1_SW),
    TWIN_KEY(ENC_2_SW), TWIN_KEY(ENC_3_SW), TWIN_KEY(ENC_4_SW), TWIN_KEY(ENC_6_SW),
    {"ENC_5_SW", -1}, // on its own pin
};
#undef TWIN_KEY

const char* const* KeyNames()
{
    static const char* names[sizeof(kKeys) / sizeof(kKeys[0]) + 1];
    for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); i++)
        names[i] = kKeys[i].name;
    return names;
}

bool Press(const char* name, bool down)
{
    for (const KeyName& k : kKeys)
    {
        if (strcmp(k.name, name) != 0)
            continue;
        if (k.sw < 0)
            pin_level[kEnc5Sw] = !down;
        else
            keys_sr.inputs[k.sw] = !down;
        return true;
    }
    return false;
}

void Turn(int encoder, int detents)
{
    if (encoder >= 1 && encoder <= 6)
        quad[encoder - 1].pending += detents;
}

void SetToggle(bool raw_level)
{
    keys_sr.inputs[static_cast<int>(chompi::Hardware::SwId::SW_TOG)] = raw_level;
}

void SetJack(bool plugged)
{
    jack_plugged = plugged;
    pin_level[kJackDetect] = plugged;
}

void Midi(uint8_t byte) { midi_in.push_back(byte); }

double BlockMs() { return block_ns / 1e6; }

// Each value only if the firmware has it (0 otherwise), so an older firmware still builds
#define TWIN_READ(name, expr)                                                                      \
    template <typename T>                                                                          \
    static auto name(const T& o, int)->decltype(expr)                                              \
    {                                                                                              \
        return expr;                                                                               \
    }                                                                                              \
    template <typename T>                                                                          \
    static int name(const T&, long)                                                                \
    {                                                                                              \
        return 0;                                                                                  \
    }
TWIN_READ(ReadLocks, o.GetLocks())
TWIN_READ(ReadTempo, o.GetTempo())
TWIN_READ(ReadFxBpm, o.GetFxBpm())
TWIN_READ(ReadPosition, o.Position())
TWIN_READ(ReadLooper, &o.looper)
TWIN_READ(ReadLoopState, static_cast<int>(o.GetState()))
TWIN_READ(ReadLoopLength, o.GetLength())
TWIN_READ(ReadLoopBeats, o.GetBeats())
TWIN_READ(ReadLoopPos, o.GetPosition())
TWIN_READ(ReadLoopSpeed, o.GetActualSpeed())
#undef TWIN_READ

template <typename L>
static void ReadLoop(const L* looper, ClockState& c)
{
    c.loop_state = ReadLoopState(*looper, 0);
    c.loop_length = ReadLoopLength(*looper, 0);
    c.loop_beats = ReadLoopBeats(*looper, 0);
    c.loop_pos = ReadLoopPos(*looper, 0);
    c.loop_speed = ReadLoopSpeed(*looper, 0);
}
static void ReadLoop(int, ClockState&) {}

ClockState Probe()
{
    ClockState c = {};
    c.has_clock = midi_clock.HasClock();
    c.source = static_cast<int>(midi_clock.*Reach(ReachSource()));
    c.midi_bpm = midi_clock.GetBpm();
    c.tick_period = midi_clock.GetTickPeriod();
    c.ticks = midi_clock.GetTicks();
    c.locks = ReadLocks(midi_clock, 0);
#ifdef TWIN_HAS_TEMPO
    const chompi::TempoClock& t = engine.*Reach(ReachTempo());
    c.tempo = ReadTempo(t, 0);
    c.fx_bpm = ReadFxBpm(t, 0);
    c.position = ReadPosition(t, 0);
#endif
    ReadLoop(ReadLooper(engine, 0), c);
    return c;
}

void UsbMidi(uint8_t byte) { usb_in.push_back(byte); }

std::string TakeUsbOut()
{
    std::string out;
    out.swap(usb_out);
    return out;
}

std::vector<MidiOutByte> TakeMidiOut()
{
    std::vector<MidiOutByte> out;
    out.swap(midi_out);
    return out;
}

void SetBattery(float volts, bool plugged, bool full)
{
    batt_volts = volts;
    batt_plugged = plugged;
    batt_full = full;
}

bool Powered() { return powered; }

bool MainLoopRunning() { return main_loop_running; }

void HoldMain(double ms) { main_ns = std::max(main_ns, block_ns) + static_cast<uint64_t>(ms * 1e6); }

bool Restarted() { return ::restarted; }
} // namespace twin

// ======== FRIZZ's USB device (UsbDevice.h), the twin's own ========
#if __has_include("UsbDevice.h")
namespace chompi
{
namespace usb
{
using twin::usb_dev;
static Stats stats = {};
static CardStats card_stats = {};
// Receive's, as UsbDevice.cpp's: a 1 ms fade in as the computer starts, out over a block as
// it stops (the device's FIFO priming isn't modelled: the computer's audio comes at once)
static bool rx_was = false;
static float rx_gain = 0.f, rx_last[kChannels] = {};
static float rx_buf[kChannels][kMaxBlock];
static const float* const rx_ptr[kChannels] = {rx_buf[0], rx_buf[1]};

void Init() {}
void Start(bool audio)
{
    usb_dev.started = true;
    usb_dev.audio = audio;
    usb_dev.enumerations++;
}
void SetAudio(bool audio)
{
    if (audio == usb_dev.audio)
        return;
    usb_dev.audio = audio;
    usb_dev.away_until_ns = twin::block_ns + twin::kUsbAwayNs;
    usb_dev.enumerations++;
    if (usb_dev.card_on)
        usb_dev.card_gone = true; // as tud_umount_cb
}
bool Audio() { return usb_dev.audio; }
bool Host() { return twin::usb_host && twin::UsbOnBus(); }
bool RxStreaming() { return twin::UsbAudioOn() && twin::usb_block_in; }
bool TxStreaming() { return twin::UsbAudioOn() && twin::usb_recording; }

const float* const* Receive(size_t size)
{
    const bool on = RxStreaming();
    if (on && !rx_was)
    {
        rx_gain = 0.f;
        stats.rx_starts++;
    }
    rx_was = on;
    for (size_t c = 0; c < kChannels; c++)
    {
        if (on)
        {
            float gain = rx_gain;
            for (size_t i = 0; i < size; i++)
            {
                gain = fminf(gain + 1000.f / twin::kSampleRate, 1.f);
                rx_buf[c][i] = twin::usb_block_in[i * 2 + c] * gain;
            }
            rx_last[c] = rx_buf[c][size - 1];
        }
        else
        {
            for (size_t i = 0; i < size; i++)
                rx_buf[c][i] = rx_last[c] * (1.f - (i + 1) * (1.f / size));
            rx_last[c] = 0.f;
        }
    }
    if (on)
    {
        rx_gain = fminf(rx_gain + size * 1000.f / twin::kSampleRate, 1.f);
        stats.rx_blocks++;
    }
    return rx_ptr;
}

void Send(const float* const* to_host, size_t size)
{
    if (!TxStreaming())
        return;
    for (size_t i = 0; i < size; i++)
        for (size_t c = 0; c < kChannels; c++)
        {
            const float x = to_host[c][i];
            twin::usb_audio_out.push_back(x > 1.f ? 1.f : x < -1.f ? -1.f : x);
        }
    stats.tx_blocks++;
}

const Stats& GetStats()
{
    stats.enumerations = static_cast<uint32_t>(usb_dev.enumerations);
    return stats;
}
void ResetFillRange() {}

void SetCard(bool (*)(bool, uint8_t*, uint32_t, uint32_t), uint32_t (*)()) {}
void CardMode(bool on)
{
    usb_dev.card_on = on;
    usb_dev.card_gone = false;
}
bool InCardMode() { return usb_dev.card_on; }
bool TakeCardGone()
{
    // ejected, or the computer gone (SetUsbHost(false): unplugged)
    const bool gone = usb_dev.card_gone || (usb_dev.card_on && !Host());
    usb_dev.card_gone = false;
    return gone;
}
void CardTask() {}
const CardStats& GetCardStats() { return card_stats; }

void SetTaskHook(void (*hook)()) { usb_dev.hook = hook; }
} // namespace usb
} // namespace chompi
#endif
