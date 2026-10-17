// The USB-C port on TinyUSB (UsbDevice.h). The bring-up (the PHY's pins, CRS, TinyUSB's
// task on TIM7), the audio FIFOs' glue (fade on start, stop and running dry, n-1/n/n+1 IN
// packets, the FIFO-count feedback) and the drive's callbacks follow lost-tty/CHOMPI's TAPE
// (MIT, github.com/lost-tty/CHOMPI, firmware/chompi-tape/code/src/UsbDevice.cpp, commits
// a56545b and ab95f98) and TinyUSB's examples (uac2_speaker_fb, uac2_headset, cdc_msc). FRIZZ's
// own: the two layouts and the switch between them, every TinyUSB call kept in its task, the
// drive's sectors moved by MainLoop, the counts, the development build's Watch().
#if defined(__arm__)
#include "UsbDevice.h"

#include <cmath>
#include <cstring>
#include "stm32h7xx_hal.h"
#include "daisy_core.h"
#include "sys/system.h"
#include "dev/sdram.h"
#include "tusb.h"
#include "usb_descriptors.h"
#include "UsbMidiTransport.h"

// built for size, as a firmware's code space is tight (docs/CAPACITY.md): it runs a few thousand
// times a second at most. Receive and Send, a block's frames in the audio callback, at -O3
#pragma GCC optimize("Os")
#define USB_AUDIO_BLOCK __attribute__((optimize("O3")))

namespace chompi
{
namespace usb
{
// ======== on the bus ========
// MainLoop asks (Start, SetAudio), the task does it: off the bus, the layout, back on
static volatile bool want_audio = true;
static volatile uint32_t bus_asked = 0; // bumped by each ask
static uint32_t bus_seen = 0;           // the task's
static volatile bool on_bus = false;    // the task's
static uint32_t away_ticks = 0;         // the task's passes off the bus still to go
// long enough off the bus for any computer to see the device gone: 300 ms of 4 kHz passes
static const uint32_t kAwayTicks = 1200;

static Stats stats = {};

// ======== audio ========
static constexpr size_t kBytes = PLATFORM_USB_BYTES_PER_SAMPLE;
static constexpr size_t kFrameBytes = kChannels * kBytes;
static constexpr float kFullScale = 8388608.f;
static constexpr int32_t kMax24 = 8388607;
static constexpr float kFadeInStep = 1000.f / PLATFORM_USB_SAMPLE_RATE; // 1 ms
// the feedback holds the computer's stream this deep in the OUT FIFO; it plays from there
static constexpr uint16_t kRxPrime = CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 2;

// what the computer asked for (an alt setting), posted by TinyUSB's callbacks in the task and
// taken by the audio callback at a block's start: TinyUSB clears a FIFO before it calls back
struct Request
{
    volatile bool on;
    volatile uint32_t count; // bumped on each change: a quick off and on still restarts
};
static Request req_rx = {}, req_tx = {};

static void Post(Request& req, bool on)
{
    req.on = on;
    req.count = req.count + 1;
}

static void PostStop()
{
    Post(req_rx, false);
    Post(req_tx, false);
}

// the alt settings, posted again on resume
static bool alt_rx = false, alt_tx = false;

// the audio callback's
static uint32_t seen_rx = 0, seen_tx = 0;
static bool rx_on = false, tx_on = false;
static bool rx_primed = false;
static float rx_gain = 0.f;
static float rx_last[kChannels];
static float rx_buf[kChannels][kMaxBlock];
static const float* rx_ptr[kChannels];

static void PickUpRequests()
{
    if (req_rx.count != seen_rx)
    {
        seen_rx = req_rx.count;
        if (req_rx.on && !rx_on)
            stats.rx_starts++;
        rx_on = req_rx.on;
        rx_primed = false;
    }
    if (req_tx.count != seen_tx)
    {
        seen_tx = req_tx.count;
        tx_on = req_tx.on;
    }
}

// ======== the drive ========
// TinyUSB asks in its task (an async transfer), MainLoop moves the sectors through a buffer
// of its own the SD's DMA reaches (SDRAM, 32-byte aligned; TinyUSB's is aligned to 4), the
// task answers
static uint8_t DSY_SDRAM_BSS __attribute__((aligned(32))) card_buf[CFG_TUD_MSC_EP_BUFSIZE];
static bool (*card_io)(bool, uint8_t*, uint32_t, uint32_t) = nullptr;
static uint32_t (*card_size)() = nullptr;
static volatile uint32_t card_sectors = 0; // as CardMode found it
static volatile bool card_on = false, card_gone = false;
static volatile bool card_attention = false; // the next TEST UNIT READY: the medium changed
struct CardRequest
{
    uint32_t lba;
    uint8_t* buf; // TinyUSB's
    uint32_t bytes;
    bool write;
    volatile bool posted;  // by the task, for MainLoop
    volatile int32_t done; // by MainLoop, for the task: the bytes moved, or TUD_MSC_RET_ERROR
    volatile bool answered; // MainLoop's done, waiting for the task
};
static CardRequest card_req = {};
static CardStats card_stats = {};
static uint32_t card_io_us = 0;

static void (*volatile task_hook)() = nullptr;

static void CardAnswer()
{
    if (!card_req.answered)
        return;
    const int32_t done = card_req.done;
    card_req.answered = false;
    card_req.posted = false;
    tud_msc_async_io_done(done, false);
}

// ======== TinyUSB's task, from TIM7 ========
static void Task()
{
    if (bus_asked != bus_seen)
    {
        bus_seen = bus_asked;
        if (on_bus)
        {
            tud_disconnect();
            on_bus = false;
            PostStop();
            away_ticks = kAwayTicks;
        }
        else if (!away_ticks)
            away_ticks = 1;
    }
    if (away_ticks && --away_ticks == 0)
    {
        usb_descriptors_audio(want_audio);
        tud_connect();
        on_bus = true;
        stats.enumerations++;
    }
    tud_task();
    if (task_hook)
        task_hook();
    MidiTinyUsbTransport::Process();
    CardAnswer();
}

void Init()
{
    for (size_t c = 0; c < kChannels; c++)
        rx_ptr[c] = rx_buf[c];
    ResetFillRange();

    // PB14/PB15: the OTG_HS core's full speed PHY
    __HAL_RCC_GPIOB_CLK_ENABLE();
    GPIO_InitTypeDef gpio = {};
    gpio.Pin = GPIO_PIN_14 | GPIO_PIN_15;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF12_OTG2_FS;
    HAL_GPIO_Init(GPIOB, &gpio);

    __HAL_RCC_USB_OTG_HS_CLK_ENABLE();
    HAL_PWREx_EnableUSBVoltageDetector();

    // USB runs on HSI48: trimmed to the computer's start of frame, as isochronous packets are
    // never sent again
    __HAL_RCC_CRS_CLK_ENABLE();
    RCC_CRSInitTypeDef crs = {};
    crs.Prescaler = RCC_CRS_SYNC_DIV1;
    crs.Source = RCC_CRS_SYNC_SOURCE_USB1;
    crs.Polarity = RCC_CRS_SYNC_POLARITY_RISING;
    crs.ReloadValue = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000, 1000);
    crs.ErrorLimitValue = RCC_CRS_ERRORLIMIT_DEFAULT;
    crs.HSI48CalibrationValue = RCC_CRS_HSI48CALIBRATION_DEFAULT;
    HAL_RCCEx_CRSConfig(&crs);

    // no VBUS pin: the D+/D- switch decides when a computer is there (chompi_main.cpp)
    tud_configure_dwc2_t cfg = CFG_TUD_CONFIGURE_DWC2_DEFAULT;
    cfg.vbus_sensing = false;
    tud_configure(PLATFORM_USB_RHPORT, TUD_CFGID_DWC2, &cfg);

    // the audio DMA's priority: the two never preempt each other
    HAL_NVIC_SetPriority(OTG_HS_IRQn, 0, 0);

    tusb_rhport_init_t dev_init = {};
    dev_init.role = TUSB_ROLE_DEVICE;
    dev_init.speed = TUSB_SPEED_FULL;
    tusb_init(PLATFORM_USB_RHPORT, &dev_init);
    tud_disconnect(); // off the bus until Start()

    // the task's timer: TIM7 at 4 kHz, below the SD card (1), above libDaisy's timers
    __HAL_RCC_TIM7_CLK_ENABLE();
    uint32_t clk = HAL_RCC_GetPCLK1Freq();
    if ((RCC->D2CFGR & RCC_D2CFGR_D2PPRE1) != RCC_D2CFGR_D2PPRE1_DIV1)
        clk *= 2; // APB timers run at twice a divided bus
    TIM7->PSC = clk / 1000000 - 1; // 1 MHz
    TIM7->ARR = 250 - 1;           // 4 kHz
    TIM7->EGR = TIM_EGR_UG;
    TIM7->SR = 0;
    TIM7->DIER = TIM_DIER_UIE;
    HAL_NVIC_SetPriority(TIM7_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(TIM7_IRQn);
    TIM7->CR1 = TIM_CR1_CEN;
}

void Start(bool audio)
{
    want_audio = audio;
    bus_asked = bus_asked + 1;
}

void SetAudio(bool audio)
{
    if (audio == want_audio)
        return;
    want_audio = audio;
    bus_asked = bus_asked + 1;
}

bool Audio() { return want_audio; }

// off the bus (the USB-audio switch), TinyUSB still says configured until the computer resets it
bool Host() { return on_bus && tud_ready(); }

void SetTaskHook(void (*hook)()) { task_hook = hook; }

// ======== audio, from the audio callback ========
bool RxStreaming() { return rx_on; }
bool TxStreaming() { return tx_on; }

const Stats& GetStats() { return stats; }

void ResetFillRange()
{
    stats.rx_min = 0xFFFF;
    stats.rx_max = 0;
}

static inline float Get24(const uint8_t* p) { return s242f(p[0] | p[1] << 8 | p[2] << 16); }

// libDaisy's f2s24 stops short of full scale, this doesn't
static inline void Put24(uint8_t* p, float x)
{
    int32_t v = static_cast<int32_t>(x * kFullScale);
    v = v > kMax24 ? kMax24 : v < -kMax24 - 1 ? -kMax24 - 1 : v;
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
}

USB_AUDIO_BLOCK const float* const* Receive(size_t size)
{
    PickUpRequests();
    const uint16_t need = static_cast<uint16_t>(size * kFrameBytes);
    const uint16_t have = rx_on ? tud_audio_available() : 0;
    stats.rx_fill = have;

    // plays once the FIFO is where the feedback holds it, and waits for that again after
    // running dry
    if (!rx_primed && have >= kRxPrime)
    {
        rx_primed = true;
        rx_gain = 0.f;
    }
    else if (rx_primed && have < need)
    {
        rx_primed = false;
        stats.rx_dry++;
    }

    if (!rx_primed)
    {
        // stopped or dry: down to silence over this block
        const float step = 1.f / size;
        for (size_t c = 0; c < kChannels; c++)
        {
            for (size_t i = 0; i < size; i++)
                rx_buf[c][i] = rx_last[c] * (1.f - (i + 1) * step);
            rx_last[c] = 0.f;
        }
        return rx_ptr;
    }

    uint8_t buf[kMaxBlock * kFrameBytes];
    tud_audio_read(buf, need);
    stats.rx_blocks++;

    for (size_t c = 0; c < kChannels; c++)
    {
        const uint8_t* p = buf + c * kBytes;
        float gain = rx_gain;
        for (size_t i = 0; i < size; i++, p += kFrameBytes)
        {
            gain = fminf(gain + kFadeInStep, 1.f);
            rx_buf[c][i] = Get24(p) * gain;
        }
        rx_last[c] = rx_buf[c][size - 1];
    }
    rx_gain = fminf(rx_gain + size * kFadeInStep, 1.f);
    return rx_ptr;
}

USB_AUDIO_BLOCK void Send(const float* const* to_host, size_t size)
{
    PickUpRequests();
    tu_fifo_t* ff = tud_audio_get_ep_in_ff(); // null until configured
    if (!tx_on || !ff)
        return;
    stats.tx_fill = tu_fifo_count(ff);

    uint8_t buf[kMaxBlock * kFrameBytes];
    for (size_t done = 0; done < size; done += kMaxBlock)
    {
        const size_t n = size - done < kMaxBlock ? size - done : kMaxBlock;
        const uint16_t len = static_cast<uint16_t>(n * kFrameBytes);
        if (tu_fifo_remaining(ff) < len)
        {
            stats.tx_full++;
            return;
        }
        for (size_t c = 0; c < kChannels; c++)
        {
            const float* x = to_host[c] + done;
            uint8_t* p = buf + c * kBytes;
            for (size_t i = 0; i < n; i++, p += kFrameBytes)
                Put24(p, x[i]);
        }
        tud_audio_write(buf, len);
        stats.tx_blocks++;
    }
}

// ======== the drive, from MainLoop ========
void SetCard(bool (*io)(bool, uint8_t*, uint32_t, uint32_t), uint32_t (*sectors)())
{
    card_io = io;
    card_size = sectors;
}

void CardMode(bool on)
{
    card_sectors = on && card_size ? card_size() : 0;
    card_attention = on;
    card_on = on;
    card_gone = false;
}

bool InCardMode() { return card_on; }

bool TakeCardGone()
{
    // cleared only once seen set: TinyUSB's task sets it from TIM7's interrupt, and an eject
    // between a read of false and the clear would be lost
    if (!card_gone)
        return false;
    card_gone = false;
    return true;
}

const CardStats& GetCardStats()
{
    card_stats.io_ms = card_io_us / 1000;
    return card_stats;
}

void CardTask()
{
    if (!card_req.posted || card_req.answered)
        return;
    asm volatile("" ::: "memory"); // the request's fields after its flag
    const uint32_t bytes = card_req.bytes;
    bool ok = card_on && card_io;
    if (ok)
    {
        const uint32_t t0 = daisy::System::GetUs();
        if (card_req.write)
            memcpy(card_buf, card_req.buf, bytes);
        ok = card_io(card_req.write, card_buf, card_req.lba, bytes / 512);
        if (ok && !card_req.write)
            memcpy(card_req.buf, card_buf, bytes);
        card_io_us += daisy::System::GetUs() - t0;
    }
    if (ok && card_req.write)
    {
        card_stats.writes++;
        card_stats.kb_written += bytes / 1024;
    }
    else if (ok)
    {
        card_stats.reads++;
        card_stats.kb_read += bytes / 1024;
    }
    else
        card_stats.errors++;
    card_req.done = ok ? static_cast<int32_t>(bytes) : TUD_MSC_RET_ERROR;
    asm volatile("" ::: "memory");
    card_req.answered = true; // the task tells TinyUSB
}

// TinyUSB's task: a transfer posted for MainLoop
static int32_t CardPost(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bytes, bool write)
{
    // within the card without lba + count, which wraps for an LBA near 2^32 (#342)
    if (!card_on || offset || !bytes || bytes % 512 || bytes > sizeof(card_buf) || card_req.posted
        || lba >= card_sectors || bytes / 512 > card_sectors - lba)
        return TUD_MSC_RET_ERROR;
    card_req.lba = lba;
    card_req.buf = buffer;
    card_req.bytes = bytes;
    card_req.write = write;
    asm volatile("" ::: "memory");
    card_req.posted = true;
    return TUD_MSC_RET_ASYNC;
}

#if PLATFORM_USB_SAFETY
// the frame number the core saw last and when it last moved, since when frames come without
// a configuration (MainLoop passes many times a 1 ms frame: a computer is there while it
// moved within kFramesGoneMs)
static uint32_t watch_frame = 0, watch_moved = 0, watch_since = 0;
static bool watch_done = false;
static const uint32_t kFramesGoneMs = 1000, kUnconfiguredMs = 10000;
static uint32_t mounted_since = 0; // configured, nothing heard

void Watch(uint32_t now_ms)
{
    if (tud_mounted() && !MidiTinyUsbTransport::Heard())
    {
        if (!mounted_since)
            mounted_since = now_ms | 1;
        else if ((now_ms | 1) - mounted_since > 60000) // now_ms | 1: the same ms ahead of an even now_ms
            NVIC_SystemReset(); // deaf to the tools: as a restart over MIDI, into the launcher
    }
    else
        mounted_since = 0;
    if (watch_done || !on_bus)
        return;
    if (tud_mounted())
    {
        watch_done = true; // enumerated once: from here on it's the computer's business
        return;
    }
    const USB_OTG_DeviceTypeDef* dev
        = reinterpret_cast<const USB_OTG_DeviceTypeDef*>(USB_OTG_HS_PERIPH_BASE + USB_OTG_DEVICE_BASE);
    const uint32_t frame = (dev->DSTS & USB_OTG_DSTS_FNSOF) >> USB_OTG_DSTS_FNSOF_Pos;
    if (frame != watch_frame)
    {
        watch_frame = frame;
        watch_moved = now_ms;
        if (!watch_since)
            watch_since = now_ms | 1;
    }
    else if (now_ms - watch_moved > kFramesGoneMs)
        watch_since = 0; // no frames: no computer (a charger, a battery), or it suspends the device
    if (watch_since && (now_ms | 1) - watch_since > kUnconfiguredMs)
        NVIC_SystemReset(); // as a restart over MIDI: the bootloader starts the launcher
}
#endif
} // namespace usb
} // namespace chompi

using namespace chompi::usb;

// TinyUSB's interrupt, its task's timer and its callbacks
extern "C"
{
    void TIM7_IRQHandler(void)
    {
        TIM7->SR = 0;
        Task();
    }

    void OTG_HS_IRQHandler(void) { tusb_int_handler(PLATFORM_USB_RHPORT, true); }

    // ---- the drive (cdc_msc's callbacks) ----
    void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
    {
        (void)lun;
        // the vendor tells a lent card from the USB storage firmware's (tools/chompi.py)
        memcpy(vendor_id, "FrizZ   ", 8);
        memcpy(product_id, "CHOMPI card     ", 16);
        memcpy(product_rev, "1.0 ", 4);
    }

    bool tud_msc_test_unit_ready_cb(uint8_t lun)
    {
        card_stats.unit_ready++;
        if (card_on && card_attention)
        {
            card_attention = false;
            tud_msc_set_sense(lun, SCSI_SENSE_UNIT_ATTENTION, 0x28, 0x00); // not ready to ready: medium changed
            return false;
        }
        if (card_on)
            return true;
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0x00); // medium not present
        return false;
    }

    void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size)
    {
        (void)lun;
        card_stats.capacity++;
        *block_count = card_on ? card_sectors : 0;
        *block_size = 512;
    }

    bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
    {
        (void)lun;
        (void)power_condition;
        if (load_eject && !start && card_on)
            card_gone = true; // ejected
        return true;
    }

    int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize)
    {
        (void)lun;
        return CardPost(lba, offset, static_cast<uint8_t*>(buffer), bufsize, false);
    }

    int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize)
    {
        (void)lun;
        return CardPost(lba, offset, buffer, bufsize, true);
    }

    int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize)
    {
        (void)scsi_cmd;
        (void)buffer;
        (void)bufsize;
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00); // invalid command
        return -1;
    }

    // ---- the device ----
    // after SET_CONFIGURATION every interface is at alt 0
    void tud_mount_cb(void)
    {
        alt_rx = alt_tx = false;
        PostStop();
    }

    void tud_umount_cb(void)
    {
        PostStop();
        if (card_on)
            card_gone = true;
    }

    // unplugged too, as far as the device can tell: there's no VBUS pin
    void tud_suspend_cb(bool remote_wakeup_en)
    {
        (void)remote_wakeup_en;
        PostStop();
        if (card_on)
            card_gone = true;
    }

    void tud_resume_cb(void)
    {
        Post(req_rx, alt_rx);
        Post(req_tx, alt_tx);
    }

    // ---- audio (uac2_speaker_fb's and uac2_headset's) ----
    bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const* p_request)
    {
        (void)rhport;
        const uint8_t itf = tu_u16_low(p_request->wIndex);
        const bool on = tu_u16_low(p_request->wValue) != 0;
        if (itf == ITF_AUDIO_STREAMING_RX)
            Post(req_rx, alt_rx = on);
        else if (itf == ITF_AUDIO_STREAMING_TX)
            Post(req_tx, alt_tx = on);
        return true;
    }

    bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const* p_request)
    {
        return tud_audio_set_itf_cb(rhport, p_request);
    }

    // each packet from the computer, in the USB interrupt, once it's in the OUT FIFO
    bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out,
                               uint8_t cur_alt_setting)
    {
        (void)rhport;
        (void)ep_out;
        (void)cur_alt_setting;
        const uint16_t frames = n_bytes_received / (kChannels * PLATFORM_USB_BYTES_PER_SAMPLE);
        Stats& s = stats;
        s.rx_packets++;
        s.rx_short += frames < PLATFORM_USB_SAMPLE_RATE / 1000;
        s.rx_long += frames > PLATFORM_USB_SAMPLE_RATE / 1000;
        const uint16_t fill = tud_audio_n_available(func_id);
        s.rx_full += fill >= CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ;
        s.rx_min = fill < s.rx_min ? fill : s.rx_min;
        s.rx_max = fill > s.rx_max ? fill : s.rx_max;
        return true;
    }

    void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t* feedback_param)
    {
        (void)func_id;
        (void)alt_itf;
        // the computer follows the device's clock: the feedback holds the OUT FIFO half full
        feedback_param->method = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
        feedback_param->sample_freq = PLATFORM_USB_SAMPLE_RATE;
        feedback_param->fifo_count.fifo_threshold = kRxPrime;
    }

    // the endpoints' sampling frequency (UAC1 5.2.3.2.3.1): one, 48 kHz
    bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const* p_request)
    {
        const uint8_t ctrl_sel = TU_U16_HIGH(p_request->wValue);
        if (ctrl_sel != AUDIO10_EP_CTRL_SAMPLING_FREQ || p_request->bRequest != AUDIO10_CS_REQ_GET_CUR)
            return false;
        const uint8_t freq[3] = {static_cast<uint8_t>(PLATFORM_USB_SAMPLE_RATE),
                                 static_cast<uint8_t>(PLATFORM_USB_SAMPLE_RATE >> 8),
                                 static_cast<uint8_t>(PLATFORM_USB_SAMPLE_RATE >> 16)};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, (void*)freq, sizeof(freq));
    }

    bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const* p_request, uint8_t* buf)
    {
        (void)rhport;
        const uint8_t ctrl_sel = TU_U16_HIGH(p_request->wValue);
        return ctrl_sel == AUDIO10_EP_CTRL_SAMPLING_FREQ && p_request->bRequest == AUDIO10_CS_REQ_SET_CUR
               && p_request->wLength == 3 && (buf[0] | buf[1] << 8 | buf[2] << 16) == PLATFORM_USB_SAMPLE_RATE;
    }
}
#endif
