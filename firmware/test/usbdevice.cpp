// usbdevice.cpp: FRIZZ's USB device (code/src/UsbDevice.cpp) itself, against TinyUSB in memory
// (usbdevice/tusb.h) and a card in memory, the task's passes run by hand (TIM7's interrupt).
// The virtual CHOMPI has a USB of its own (twin.cpp), so this is the one host check of the code
// that talks to TinyUSB (#331):
//   - the bus: Start, the USB-audio switch (SetAudio: off the bus for 300 ms, back with the
//     other layout), Host() false while off it though TinyUSB still says configured (#319);
//   - audio from the computer (Receive): primed half a FIFO deep, faded in, running dry faded
//     out and primed again, stopped, suspended and resumed; to it (Send): dropped while it
//     doesn't record, a block dropped whole when the IN FIFO is full, clamped at full scale;
//     the sample rate (48 kHz only) and the feedback's threshold;
//   - the drive: UNIT ATTENTION after a lend starts, NOT_READY without one, the capacity,
//     CardPost's bounds and one transfer at a time, the sectors MainLoop moves (CardTask) and
//     the answer the task gives, an error from the card, a lend ended mid-transfer, and each
//     way the lend ends from the computer's side (eject, unmount, suspend: card_gone, #330).
// Run by unit.sh usbdevice.
//
// unit includes: usbdevice
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include "check.h"
#include "tusb.h"
#include "stm32h7xx_hal.h"
#include "daisy_core.h"
#include "sys/system.h"
#include "dev/sdram.h"
// UsbDevice.cpp is the CHOMPI's alone (#if defined(__arm__)); every system header it reaches is
// in above, so the target's macro reaches nothing but FRIZZ's own (usb_descriptors.h,
// UsbMidiTransport.h, which it includes from code/src)
#define __arm__ 1
#include "../code/src/UsbDevice.cpp"
#undef __arm__

// ======== TinyUSB, the computer's side ========
namespace tusbstub
{
bool connected = false;
int connects = 0, disconnects = 0;
bool configured = false;
int tasks = 0;
bool inited = false;
std::deque<uint8_t> rx_fifo;
tu_fifo_t tx_fifo = {{}, CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ};
bool tx_ff = true;
std::vector<uint8_t> control;
Sense sense = {};
std::vector<int32_t> io_done;
} // namespace tusbstub
using namespace tusbstub;

void tud_disconnect()
{
    connected = false;
    disconnects++;
}
void tud_connect()
{
    connected = true;
    connects++;
}
void tud_task() { tasks++; }
bool tud_ready() { return configured; }
bool tud_mounted() { return configured; }
bool tud_configure(uint8_t, uint32_t, const void*) { return true; }
bool tusb_init(uint8_t, const tusb_rhport_init_t*) { return inited = true; }
void tusb_int_handler(uint8_t, bool) {}

uint16_t tud_audio_available() { return static_cast<uint16_t>(rx_fifo.size()); }
uint16_t tud_audio_n_available(uint8_t) { return tud_audio_available(); }
uint16_t tud_audio_read(void* buffer, uint16_t bufsize)
{
    const uint16_t n = std::min<uint16_t>(bufsize, tud_audio_available());
    std::copy(rx_fifo.begin(), rx_fifo.begin() + n, static_cast<uint8_t*>(buffer));
    rx_fifo.erase(rx_fifo.begin(), rx_fifo.begin() + n);
    return n;
}
tu_fifo_t* tud_audio_get_ep_in_ff() { return tx_ff ? &tx_fifo : nullptr; }
uint16_t tu_fifo_count(const tu_fifo_t* f) { return static_cast<uint16_t>(f->data.size()); }
uint16_t tu_fifo_remaining(const tu_fifo_t* f) { return static_cast<uint16_t>(f->depth - f->data.size()); }
uint16_t tud_audio_write(const void* data, uint16_t len)
{
    const uint16_t n = std::min(len, tu_fifo_remaining(&tx_fifo));
    tx_fifo.data.insert(tx_fifo.data.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + n);
    return n;
}
bool tud_audio_buffer_and_schedule_control_xfer(uint8_t, const tusb_control_request_t*, void* data, uint16_t len)
{
    control.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + len);
    return true;
}
void tud_msc_async_io_done(int32_t bytes_io, bool) { io_done.push_back(bytes_io); }
bool tud_msc_set_sense(uint8_t, uint8_t key, uint8_t asc, uint8_t ascq)
{
    sense = {key, asc, ascq};
    return true;
}
uint32_t tud_midi_available() { return 0; }
uint32_t tud_midi_stream_read(void*, uint32_t) { return 0; }
bool tud_midi_mounted() { return configured; }
uint32_t tud_midi_stream_write(uint8_t, const uint8_t*, uint32_t bufsize) { return bufsize; }

// usb_descriptors.c's: the layout asked for last
static int layout = -1; // 1 with audio, 0 without
extern "C" void usb_descriptors_audio(bool audio) { layout = audio; }

// ======== the card, in memory ========
static const uint32_t kSectors = 1024;
static std::vector<uint8_t> card(kSectors * 512);
struct Io
{
    bool write;
    uintptr_t buf;
    uint32_t sector, count;
};
static std::vector<Io> ios;
static bool card_fails = false;
static bool CardIo(bool write, uint8_t* buf, uint32_t sector, uint32_t count)
{
    ios.push_back({write, reinterpret_cast<uintptr_t>(buf), sector, count});
    if (card_fails || sector >= kSectors || count > kSectors - sector) // the SD card's own refusal
        return false;
    uint8_t* at = &card[size_t(sector) * 512];
    if (write)
        memcpy(at, buf, count * 512);
    else
        memcpy(buf, at, count * 512);
    return true;
}
static uint32_t CardSectors() { return kSectors; }

// ======== helpers ========
static void Pass(int n = 1)
{
    for (int i = 0; i < n; i++)
        TIM7_IRQHandler(); // TinyUSB's task, a 4 kHz pass
}

static const size_t kBlock = 24;              // the audio callback's
static const size_t kFrame = kChannels * kBytes; // 6 bytes
static const size_t kPrimeFrames = kRxPrime / kFrame;

static void Put(std::deque<uint8_t>& q, int32_t v)
{
    q.push_back(static_cast<uint8_t>(v));
    q.push_back(static_cast<uint8_t>(v >> 8));
    q.push_back(static_cast<uint8_t>(v >> 16));
}
/** frames of left, right (24-bit) into the OUT FIFO, as the computer plays */
static void Play(size_t frames, int32_t left = 0x400000, int32_t right = 0x200000)
{
    for (size_t i = 0; i < frames; i++)
    {
        Put(rx_fifo, left);
        Put(rx_fifo, right);
    }
}

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

static void Alt(uint8_t itf, bool on)
{
    tusb_control_request_t r = {};
    r.wIndex = itf;
    r.wValue = on;
    tud_audio_set_itf_cb(0, &r);
}

static tusb_control_request_t Control(uint8_t request, uint8_t ctrl, uint16_t length)
{
    tusb_control_request_t r = {};
    r.bRequest = request;
    r.wValue = static_cast<uint16_t>(ctrl << 8);
    r.wLength = length;
    return r;
}

static bool SetRate(uint32_t rate, uint16_t length = 3, uint8_t ctrl = AUDIO10_EP_CTRL_SAMPLING_FREQ)
{
    tusb_control_request_t r = Control(AUDIO10_CS_REQ_SET_CUR, ctrl, length);
    uint8_t buf[4] = {uint8_t(rate), uint8_t(rate >> 8), uint8_t(rate >> 16), uint8_t(rate >> 24)};
    return tud_audio_set_req_ep_cb(0, &r, buf);
}

static int hooked = 0;
static void Hook() { hooked++; }

int main()
{
    // ======== on the bus ========
    Init();
    Check(inited && !connected && disconnects == 1, "Init: TinyUSB up, FRIZZ off the bus until Start");
    // APB1 divided: its timers run at twice the 120 MHz
    Check(240000000u / ((TIM7->PSC + 1) * (TIM7->ARR + 1)) == 4000 && TIM7->CR1 == TIM_CR1_CEN,
          "the task's timer, TIM7, at 4 kHz");
    configured = true; // whatever TinyUSB says, off the bus FRIZZ has no host
    Check(!Host(), "Host(): not before FRIZZ is on the bus");
    configured = false;
    SetTaskHook(Hook);
    Start(true);
    Pass();
    Check(connected && layout == 1 && GetStats().enumerations == 1 && Audio(),
          "Start(true): on the bus at the next pass, with the sound card");
    Check(!Host(), "Host(): not until the computer configures FRIZZ");
    configured = true;
    Check(Host(), "Host() once it has");
    Check(tasks == 1 && hooked == 1, "each pass runs TinyUSB's task and the hook (MidiOut::FlushUsb)");

    SetAudio(true);
    Pass(2);
    Check(connected && disconnects == 1, "SetAudio to the layout it has: stays on the bus");

    SetAudio(false);
    Check(!Audio(), "SetAudio(false): Audio() says the layout to come at once");
    Pass();
    Check(!connected && disconnects == 2, "and the next pass leaves the bus");
    Check(configured && !Host(), "Host() false off the bus, though TinyUSB still says configured (#319)");
    Pass(kAwayTicks - 2);
    Check(!connected, "still off the bus 1 pass short of 300 ms");
    Pass();
    Check(connected && layout == 0 && GetStats().enumerations == 2 && Host(),
          "back after 300 ms (1200 passes), without the sound card");

    // switched again while away: one return, with the layout asked for last
    SetAudio(true);
    Pass(100);
    SetAudio(false);
    Pass(100);
    SetAudio(true);
    Pass(kAwayTicks - 200 - 1);
    Check(!connected && disconnects == 3, "switched twice more while off the bus: it stays off, no second leave");
    Pass();
    Check(connected && layout == 1 && GetStats().enumerations == 3 && connects == 3,
          "and comes back once, 300 ms after it left, with the last layout asked for");

    // ======== audio from the computer ========
    float zeros[kBlock] = {};
    const float* const* in = Receive(kBlock);
    Check(!RxStreaming() && !memcmp(in[0], zeros, sizeof(zeros)) && !memcmp(in[1], zeros, sizeof(zeros)),
          "Receive: silence while the computer doesn't play");
    Alt(ITF_AUDIO_STREAMING_RX, true);
    Play(kPrimeFrames - 1);
    in = Receive(kBlock);
    Check(RxStreaming() && GetStats().rx_starts == 1 && in[0][kBlock - 1] == 0.f && rx_fifo.size() == kRxPrime - kFrame,
          "it starts: silence, nothing read, until the FIFO is half full (the feedback's depth)");
    Play(1);
    in = Receive(kBlock);
    bool fade = true;
    for (size_t i = 0; i < kBlock; i++)
        fade = fade && Near(in[0][i], 0.5f * (i + 1) / 48.f) && Near(in[1][i], 0.25f * (i + 1) / 48.f);
    Check(fade && rx_fifo.size() == kRxPrime - kBlock * kFrame && GetStats().rx_blocks == 1,
          "half full: it plays, left and right, faded in over 1 ms");
    in = Receive(kBlock);
    Check(Near(in[0][0], 0.5f * 25 / 48.f) && Near(in[0][kBlock - 1], 0.5f), "the fade goes on in the next block, to full");
    Play(kBlock);
    in = Receive(kBlock);
    Check(Near(in[0][0], 0.5f) && Near(in[1][kBlock - 1], 0.25f), "then as the computer sent it");
    // full scale, and below zero: 24 bits sign-extended
    Play(kPrimeFrames, 0x7FFFFF, 0x800000);
    rx_fifo.erase(rx_fifo.begin(), rx_fifo.end() - kPrimeFrames * kFrame);
    in = Receive(kBlock);
    Check(Near(in[0][0], 8388607.f / 8388608.f) && Near(in[1][0], -1.f), "full scale both ways, the negative sign-extended");

    // running dry: faded out from the last sample, then silence until half full again
    rx_fifo.clear();
    Play(kBlock - 1);
    const uint32_t dry = GetStats().rx_dry;
    in = Receive(kBlock);
    bool out = true;
    for (size_t i = 0; i < kBlock; i++)
        out = out && Near(in[1][i], -1.f * (1.f - (i + 1) / float(kBlock)));
    Check(GetStats().rx_dry == dry + 1 && out && rx_fifo.size() == (kBlock - 1) * kFrame,
          "a block short: it ran dry, faded out over the block, the FIFO left as it was");
    in = Receive(kBlock);
    Check(in[0][0] == 0.f && in[1][kBlock - 1] == 0.f, "silence after");
    Play(kPrimeFrames - kBlock);
    in = Receive(kBlock);
    Check(in[0][kBlock - 1] == 0.f && GetStats().rx_dry == dry + 1, "still silent one frame short of half full");
    Play(1);
    in = Receive(kBlock);
    Check(Near(in[0][0], 0.5f / 48.f) && Near(in[0][kBlock - 1], 0.25f), "half full again: it plays, faded in again");

    // a quick off and on between two blocks: primed again, not played from where it was
    Alt(ITF_AUDIO_STREAMING_RX, false);
    Alt(ITF_AUDIO_STREAMING_RX, true);
    in = Receive(kBlock);
    Check(RxStreaming() && Near(in[0][0], 0.5f * 0.5f * (1.f - 1.f / kBlock)) && in[0][kBlock - 1] == 0.f,
          "off and on between two blocks: it stops, fading out, and waits to be half full again");

    // the computer stops
    rx_fifo.clear();
    Play(kPrimeFrames);
    Receive(kBlock);
    in = Receive(kBlock);
    const float last = in[0][kBlock - 1];
    Alt(ITF_AUDIO_STREAMING_RX, false);
    in = Receive(kBlock);
    Check(!RxStreaming() && last > 0.f && Near(in[0][0], last * (1.f - 1.f / kBlock)) && in[0][kBlock - 1] == 0.f,
          "the computer stops: faded out over a block from where it was");

    // suspended, resumed: the stream as the computer left it
    Alt(ITF_AUDIO_STREAMING_RX, true);
    Alt(ITF_AUDIO_STREAMING_TX, true);
    Receive(kBlock);
    tud_suspend_cb(false);
    Receive(kBlock);
    Check(!RxStreaming() && !TxStreaming(), "suspended: both streams stop");
    tud_resume_cb();
    Receive(kBlock);
    Check(RxStreaming() && TxStreaming(), "resumed: both again, as the computer had them");
    tud_mount_cb();
    Receive(kBlock);
    Check(!RxStreaming() && !TxStreaming(), "configured again (SET_CONFIGURATION): every interface at alt 0");

    // ======== audio to the computer ========
    float l[96], r[96];
    for (size_t i = 0; i < 96; i++)
        l[i] = r[i] = 0.f;
    l[0] = 1.f, r[0] = -1.f, l[1] = 1.5f, r[1] = -1.5f, l[2] = 0.5f, r[2] = -0.5f;
    const float* to_host[2] = {l, r};
    Send(to_host, kBlock);
    Check(tx_fifo.data.empty() && GetStats().tx_blocks == 0, "Send: nothing while the computer doesn't record");
    Alt(ITF_AUDIO_STREAMING_TX, true);
    tx_ff = false;
    Send(to_host, kBlock);
    Check(TxStreaming() && tx_fifo.data.empty(), "nor without TinyUSB's IN FIFO (not configured)");
    tx_ff = true;
    Send(to_host, kBlock);
    const std::vector<uint8_t> want = {0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x80, 0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x80,
                                       0x00, 0x00, 0x40, 0x00, 0x00, 0xC0};
    Check(tx_fifo.data.size() == kBlock * kFrame && std::equal(want.begin(), want.end(), tx_fifo.data.begin())
              && GetStats().tx_blocks == 1,
          "a block, left and right, 24 bits, clamped at full scale both ways");
    tx_fifo.data.clear();
    Send(to_host, 96);
    Check(tx_fifo.data.size() == 96 * kFrame && GetStats().tx_blocks == 3, "a long block goes in pieces of 48 frames");
    // full: a block that doesn't fit is dropped whole
    tx_fifo.data.assign(tx_fifo.depth - kBlock * kFrame + 1, 0);
    const uint32_t full = GetStats().tx_full;
    Send(to_host, kBlock);
    Check(GetStats().tx_full == full + 1 && tx_fifo.data.size() == tx_fifo.depth - kBlock * kFrame + 1
              && GetStats().tx_fill == tx_fifo.depth - kBlock * kFrame + 1,
          "the IN FIFO a byte short of a block: the block dropped whole, counted, the fill reported");
    tx_fifo.data.resize(tx_fifo.depth - kBlock * kFrame);
    Send(to_host, kBlock);
    Check(GetStats().tx_full == full + 1 && tu_fifo_remaining(&tx_fifo) == 0, "room for exactly a block: it goes");
    tx_fifo.data.clear();
    Alt(ITF_AUDIO_STREAMING_TX, false);
    Send(to_host, kBlock);
    Check(!TxStreaming() && tx_fifo.data.empty(), "the computer stops recording: nothing more");

    // ======== the rate, the feedback ========
    Check(SetRate(48000), "SET_CUR 48 kHz: taken");
    Check(!SetRate(44100) && !SetRate(96000), "44.1 or 96 kHz: refused");
    Check(!SetRate(48000, 4) && !SetRate(48000, 3, 0x02), "a length other than 3, a control other than the rate: refused");
    tusb_control_request_t get = Control(AUDIO10_CS_REQ_GET_CUR, AUDIO10_EP_CTRL_SAMPLING_FREQ, 3);
    control.clear();
    Check(tud_audio_get_req_ep_cb(0, &get) && control == std::vector<uint8_t>({0x80, 0xBB, 0x00}),
          "GET_CUR: 48000, 3 bytes");
    get.bRequest = 0x82; // GET_MIN
    Check(!tud_audio_get_req_ep_cb(0, &get), "any other GET: refused");
    audio_feedback_params_t fb = {};
    tud_audio_feedback_params_cb(0, 1, &fb);
    Check(fb.method == AUDIO_FEEDBACK_METHOD_FIFO_COUNT && fb.sample_freq == 48000
              && fb.fifo_count.fifo_threshold == CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 2,
          "the feedback: the FIFO's count, held at half the OUT FIFO, where Receive primes");

    // ======== the drive ========
    for (size_t i = 0; i < card.size(); i++)
        card[i] = static_cast<uint8_t>(i * 7 + i / 512);
    const std::vector<uint8_t> was = card;
    uint32_t blocks = 1;
    uint16_t size = 0;
    Check(!tud_msc_test_unit_ready_cb(0) && sense.key == SCSI_SENSE_NOT_READY && sense.asc == 0x3a,
          "no lend: TEST UNIT READY says no medium");
    tud_msc_capacity_cb(0, &blocks, &size);
    Check(blocks == 0 && size == 512, "and the capacity none");
    uint8_t buf[4096];
    Check(tud_msc_read10_cb(0, 0, 0, buf, 512) == TUD_MSC_RET_ERROR, "a read without a lend: refused");
    char vendor[8], product[16], rev[4];
    tud_msc_inquiry_cb(0, reinterpret_cast<uint8_t*>(vendor), reinterpret_cast<uint8_t*>(product),
                       reinterpret_cast<uint8_t*>(rev));
    Check(!memcmp(vendor, "FrizZ   ", 8), "INQUIRY: the vendor the tools tell a lent card by");

    SetCard(CardIo, CardSectors);
    CardMode(true);
    Check(InCardMode(), "lent");
    Check(!tud_msc_test_unit_ready_cb(0) && sense.key == SCSI_SENSE_UNIT_ATTENTION && sense.asc == 0x28,
          "the first TEST UNIT READY: UNIT ATTENTION, the medium changed");
    Check(tud_msc_test_unit_ready_cb(0), "then ready");
    tud_msc_capacity_cb(0, &blocks, &size);
    Check(blocks == kSectors && size == 512, "the capacity: the card's sectors");

    // a read: posted by the task, moved by MainLoop, answered by the task
    io_done.clear();
    Check(tud_msc_read10_cb(0, 10, 0, buf, 4096) == TUD_MSC_RET_ASYNC, "a read of 8 sectors: posted");
    Check(tud_msc_write10_cb(0, 30, 0, buf, 512) == TUD_MSC_RET_ERROR, "a second while it waits: refused");
    Pass();
    Check(io_done.empty() && ios.empty(), "nothing moved before MainLoop's CardTask");
    CardTask();
    Check(ios.size() == 1 && !ios[0].write && ios[0].sector == 10 && ios[0].count == 8 && ios[0].buf % 32 == 0,
          "CardTask: sectors 10-17 through FRIZZ's own buffer, 32-byte aligned (the SD's DMA)");
    Check(io_done.empty(), "answered only by the task");
    Pass();
    Check(io_done == std::vector<int32_t>({4096}) && !memcmp(buf, &card[10 * 512], 4096),
          "the next pass answers: 4096 bytes, the card's");
    Pass();
    Check(io_done.size() == 1, "once");

    // a write
    io_done.clear();
    ios.clear();
    memset(buf, 0xA5, sizeof(buf));
    Check(tud_msc_write10_cb(0, 20, 0, buf, 1024) == TUD_MSC_RET_ASYNC, "a write of 2 sectors: posted");
    CardTask();
    Pass();
    bool only = true;
    for (size_t i = 0; i < card.size(); i++)
        only = only && card[i] == (i >= 20 * 512 && i < 22 * 512 ? 0xA5 : was[i]);
    Check(io_done == std::vector<int32_t>({1024}) && ios.size() == 1 && ios[0].write && only,
          "written: sectors 20 and 21, nothing else");
    const CardStats& cs = GetCardStats();
    Check(cs.reads == 1 && cs.writes == 1 && cs.kb_read == 4 && cs.kb_written == 1 && cs.errors == 0 && cs.io_ms == 2,
          "counted: a read of 4 KB, a write of 1 KB, the card's time");

    // CardPost's bounds: whole sectors, one transfer's buffer at most, within the card
    ios.clear();
    io_done.clear();
    Check(tud_msc_read10_cb(0, 0, 512, buf, 512) == TUD_MSC_RET_ERROR, "an offset into a transfer: refused");
    Check(tud_msc_read10_cb(0, 0, 0, buf, 0) == TUD_MSC_RET_ERROR, "no bytes: refused");
    Check(tud_msc_write10_cb(0, 0, 0, buf, 100) == TUD_MSC_RET_ERROR, "part of a sector: refused");
    static uint8_t big[8192];
    Check(tud_msc_read10_cb(0, 0, 0, big, 8192) == TUD_MSC_RET_ERROR, "more than the 4 KB buffer: refused");
    Check(tud_msc_read10_cb(0, kSectors - 7, 0, buf, 4096) == TUD_MSC_RET_ERROR
              && tud_msc_write10_cb(0, kSectors, 0, buf, 512) == TUD_MSC_RET_ERROR,
          "past the card's last sector: refused");
    Check(tud_msc_read10_cb(0, kSectors - 8, 0, buf, 4096) == TUD_MSC_RET_ASYNC, "up to the last sector: posted");
    CardTask();
    Pass();
    Check(io_done == std::vector<int32_t>({4096}), "and moved");
    // an LBA near 2^32: lba + count wraps past the bound
    const std::vector<uint8_t> written = card;
    ios.clear();
    io_done.clear();
    const int32_t wrapped = tud_msc_write10_cb(0, 0xFFFFFFF9u, 0, buf, 4096);
    if (wrapped == TUD_MSC_RET_ASYNC)
    {
        CardTask();
        Pass();
    }
    Known(wrapped == TUD_MSC_RET_ERROR && ios.empty(),
          "a write at LBA 0xFFFFFFF9 (lba + 8 wraps to 1): refused, not handed to the card (#342)");
    if (!ios.empty())
        printf("      the card was asked for sector 0x%08X, %u sectors\n", ios[0].sector, ios[0].count);
    Check(card == written, "the card as it was (the card in memory refuses a sector past its end, as an SD card does)");

    // the card fails: an error to the computer, counted
    io_done.clear();
    card_fails = true;
    const uint32_t errors = GetCardStats().errors;
    tud_msc_read10_cb(0, 0, 0, buf, 512);
    CardTask();
    Pass();
    Check(io_done == std::vector<int32_t>({TUD_MSC_RET_ERROR}) && GetCardStats().errors == errors + 1,
          "a transfer the card fails: an error to the computer, counted");
    card_fails = false;

    // the lend ends while a transfer waits: answered with an error, the card untouched
    io_done.clear();
    ios.clear();
    Check(tud_msc_write10_cb(0, 40, 0, buf, 512) == TUD_MSC_RET_ASYNC, "a write posted");
    CardMode(false);
    CardTask();
    Pass();
    Check(ios.empty() && io_done == std::vector<int32_t>({TUD_MSC_RET_ERROR}) && !InCardMode(),
          "taken back before MainLoop moved it: an error, nothing written");
    Check(!tud_msc_test_unit_ready_cb(0) && sense.key == SCSI_SENSE_NOT_READY, "and the drive empty again");

    // ======== the lend ends from the computer's side (card_gone, #330) ========
    Check(!TakeCardGone(), "nothing gone yet");
    tud_msc_start_stop_cb(0, 0, false, true);
    Check(!TakeCardGone(), "an eject with no lend: nothing");
    CardMode(true);
    tud_msc_start_stop_cb(0, 0, true, true);
    tud_msc_start_stop_cb(0, 0, false, false);
    Check(!TakeCardGone(), "a load, a stop without eject: the lend goes on");
    tud_msc_start_stop_cb(0, 0, false, true);
    Check(TakeCardGone(), "ejected: gone");
    Check(!TakeCardGone(), "seen once");
    tud_umount_cb();
    Check(TakeCardGone(), "unmounted (unplugged) while lent: gone");
    tud_suspend_cb(false);
    Check(TakeCardGone(), "suspended (asleep, or unplugged: there's no VBUS pin) while lent: gone");
    tud_resume_cb();
    // an eject MainLoop didn't take before it took the card back itself: not the next lend's
    tud_msc_start_stop_cb(0, 0, false, true);
    CardMode(false);
    CardMode(true);
    Check(!TakeCardGone(), "an eject left from the last lend doesn't end the next one");
    Check(!tud_msc_test_unit_ready_cb(0) && sense.key == SCSI_SENSE_UNIT_ATTENTION, "which starts with UNIT ATTENTION again");
    CardMode(false);
    tud_umount_cb();
    tud_suspend_cb(false);
    Check(!TakeCardGone(), "unmounted or suspended with no lend: nothing");

    // anything else the computer asks: refused as an invalid command
    uint8_t cmd[16] = {0x1A}; // MODE SENSE (6)
    Check(tud_msc_scsi_cb(0, cmd, buf, 64) == -1 && sense.key == SCSI_SENSE_ILLEGAL_REQUEST && sense.asc == 0x20,
          "another SCSI command: ILLEGAL REQUEST, invalid command");
    Check(GetCardStats().unit_ready > 0 && GetCardStats().capacity == 2, "TEST UNIT READY and READ CAPACITY counted");
    return Finish();
}
