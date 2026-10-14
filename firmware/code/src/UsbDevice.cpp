// FRIZZ's USB port on TinyUSB (UsbDevice.h). From lost-tty/CHOMPI's TAPE (MIT,
// github.com/lost-tty/CHOMPI, firmware/chompi-tape/code/src/UsbDevice.cpp, commit a56545b),
// without MSC; FRIZZ's additions: the jacks' polarity, the counts, Watch(), the MIDI parse
// with the audio callback held off. usb-audio-poc (#196), an experiment.
#if defined(__arm__)
#include "UsbDevice.h"

#include <cmath>
#include <cstring>
#include "stm32h7xx_hal.h"
#include "daisy_core.h"
#include "util/scopedirqblocker.h"
#include "tusb.h"
#include "usb_descriptors.h"
#include "sys/system.h"
#include "dev/sdram.h"

namespace chompi
{
namespace usb
{
static bool connected = false;

#if FRIZZ_USB_AUDIO
static constexpr size_t kBytes = FRIZZ_USB_BYTES_PER_SAMPLE;
static constexpr size_t kRxFrameBytes = kRxChannels * kBytes;
static constexpr size_t kTxFrameBytes = kTxChannels * kBytes;
static constexpr float kFullScale = 8388608.f;
static constexpr int32_t kMax24 = 8388607;
static constexpr float kFadeInStep = 1000.f / FRIZZ_USB_SAMPLE_RATE; // 1 ms
static constexpr uint16_t kRxPrime = CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 2; // the feedback holds the RX FIFO here
// AUX in and the line out turn the signal at the jacks (passthroughEngine.h, #209): turned here
// too, USB's audio is in phase with the jacks
static constexpr float kJackPolarity = -1.f;

// what the host asked for, posted by TinyUSB's callbacks in the task
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
static bool rx_primed = false; // the RX FIFO is where the feedback holds it
static float rx_gain = 0.f;
static float rx_last[kRxChannels];
static float rx_buf[kRxChannels][kMaxBlock];
static const float* rx_ptr[kRxChannels];
static Stats stats = {};

// TinyUSB clears a FIFO in tud_task() before its callback: changes are taken at block boundaries
static void PickUpRequests()
{
    if(req_rx.count != seen_rx)
    {
        seen_rx = req_rx.count;
        if(req_rx.on && !rx_on)
            stats.rx_starts++;
        rx_on = req_rx.on;
        rx_primed = false;
    }
    if(req_tx.count != seen_tx)
    {
        seen_tx = req_tx.count;
        tx_on = req_tx.on;
    }
}
#endif

void Init()
{
#if FRIZZ_USB_AUDIO
    for(size_t c = 0; c < kRxChannels; c++)
        rx_ptr[c] = rx_buf[c];
    ResetFillRange();
#endif

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

    // USB runs on HSI48: trim it to the host's start of frame
    __HAL_RCC_CRS_CLK_ENABLE();
    RCC_CRSInitTypeDef crs = {};
    crs.Prescaler = RCC_CRS_SYNC_DIV1;
    crs.Source = RCC_CRS_SYNC_SOURCE_USB1;
    crs.Polarity = RCC_CRS_SYNC_POLARITY_RISING;
    crs.ReloadValue = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000, 1000);
    crs.ErrorLimitValue = RCC_CRS_ERRORLIMIT_DEFAULT;
    crs.HSI48CalibrationValue = RCC_CRS_HSI48CALIBRATION_DEFAULT;
    HAL_RCCEx_CRSConfig(&crs);

    // no VBUS pin: the D+/D- switch decides when we're connected
    tud_configure_dwc2_t cfg = CFG_TUD_CONFIGURE_DWC2_DEFAULT;
    cfg.vbus_sensing = false;
    tud_configure(FRIZZ_USB_RHPORT, TUD_CFGID_DWC2, &cfg);

    // the audio DMA's priority: the two never preempt each other
    HAL_NVIC_SetPriority(OTG_HS_IRQn, 0, 0);

    tusb_rhport_init_t dev_init = {};
    dev_init.role = TUSB_ROLE_DEVICE;
    dev_init.speed = TUSB_SPEED_FULL;
    tusb_init(FRIZZ_USB_RHPORT, &dev_init);
    tud_disconnect(); // no D+ pull-up until Task() runs and can answer requests

    // the task timer: TIM7 at 4 kHz, below the SD card (1), above libDaisy's timers
    __HAL_RCC_TIM7_CLK_ENABLE();
    uint32_t clk = HAL_RCC_GetPCLK1Freq();
    if((RCC->D2CFGR & RCC_D2CFGR_D2PPRE1) != RCC_D2CFGR_D2PPRE1_DIV1)
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

static void (*volatile task_hook)() = nullptr;

void SetTaskHook(void (*hook)()) { task_hook = hook; }

static void Task()
{
    if(!connected)
    {
        connected = true;
        tud_connect();
    }
    tud_task();
    if(task_hook)
        task_hook();
    MidiTinyUsbTransport::Process();
}

bool Host() { return tud_ready(); }

// the frame number the core saw last and when it last moved, since when frames come without a
// configuration (MainLoop passes many times a 1 ms frame: the host is there while it moved
// within kFramesGoneMs)
static uint32_t watch_frame = 0, watch_moved = 0, watch_since = 0;
static bool watch_done = false;
static const uint32_t kFramesGoneMs = 1000, kUnconfiguredMs = 10000;

#if FRIZZ_USB_SAFETY
static uint32_t mounted_since = 0; // configured, nothing heard
#endif

void Watch(uint32_t now_ms)
{
#if FRIZZ_USB_SAFETY
    if(tud_mounted() && !MidiTinyUsbTransport::Heard())
    {
        if(!mounted_since)
            mounted_since = now_ms | 1;
        else if(now_ms - mounted_since > 60000)
            NVIC_SystemReset(); // deaf to the tools: as a restart over MIDI, into the launcher
    }
    else
        mounted_since = 0;
#endif
    if(watch_done || !connected)
        return;
    if(tud_mounted())
    {
        watch_done = true; // enumerated once: from here on it's the host's business
        return;
    }
    const USB_OTG_DeviceTypeDef* dev
        = reinterpret_cast<const USB_OTG_DeviceTypeDef*>(USB_OTG_HS_PERIPH_BASE + USB_OTG_DEVICE_BASE);
    const uint32_t frame = (dev->DSTS & USB_OTG_DSTS_FNSOF) >> USB_OTG_DSTS_FNSOF_Pos;
    if(frame != watch_frame)
    {
        watch_frame = frame;
        watch_moved = now_ms;
        if(!watch_since)
            watch_since = now_ms | 1;
    }
    else if(now_ms - watch_moved > kFramesGoneMs)
        watch_since = 0; // no frames: no host (a charger, a battery), or it suspends us
    if(watch_since && now_ms - watch_since > kUnconfiguredMs)
        NVIC_SystemReset(); // as a restart over MIDI: the bootloader starts the launcher
}

#if FRIZZ_USB_AUDIO
bool RxStreaming() { return rx_on; }

const Stats& GetStats() { return stats; }

void ResetFillRange()
{
    stats.rx_min = 0xFFFF;
    stats.rx_max = 0;
}

static inline float Get24(const uint8_t* p) { return kJackPolarity * s242f(p[0] | p[1] << 8 | p[2] << 16); }

// f2s24 clamps a 16 bit LSB short of full scale, this doesn't
static inline void Put24(uint8_t* p, float x)
{
    int32_t v = (int32_t)(kJackPolarity * x * kFullScale);
    v = v > kMax24 ? kMax24 : v < -kMax24 - 1 ? -kMax24 - 1 : v;
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
}

const float* const* Receive(size_t size)
{
    PickUpRequests();
    const uint16_t need = size * kRxFrameBytes;
    const uint16_t have = rx_on ? tud_audio_available() : 0;
    stats.rx_fill = have;

    // start once the FIFO is where the feedback holds it, and wait for that again after running dry
    if(!rx_primed && have >= kRxPrime)
    {
        rx_primed = true;
        rx_gain = 0.f;
    }
    else if(rx_primed && have < need)
    {
        rx_primed = false;
        stats.rx_dry++;
    }

    if(!rx_primed)
    {
        // stopped or ran dry: down to silence over this block
        const float step = 1.f / size;
        for(size_t c = 0; c < kRxChannels; c++)
        {
            for(size_t i = 0; i < size; i++)
                rx_buf[c][i] = rx_last[c] * (1.f - (i + 1) * step);
            rx_last[c] = 0.f;
        }
        return rx_ptr;
    }

    uint8_t buf[kMaxBlock * kRxFrameBytes];
    tud_audio_read(buf, need);
    stats.rx_blocks++;

    for(size_t c = 0; c < kRxChannels; c++)
    {
        const uint8_t* p = buf + c * kBytes;
        float gain = rx_gain;
        for(size_t i = 0; i < size; i++, p += kRxFrameBytes)
        {
            gain = fminf(gain + kFadeInStep, 1.f);
            rx_buf[c][i] = Get24(p) * gain;
        }
        rx_last[c] = rx_buf[c][size - 1];
    }
    rx_gain = fminf(rx_gain + size * kFadeInStep, 1.f);
    return rx_ptr;
}

void Send(const float* const* to_host, size_t size)
{
    PickUpRequests();
    tu_fifo_t* ff = tud_audio_get_ep_in_ff(); // null until configured
    if(!tx_on || !ff)
        return;
    stats.tx_fill = tu_fifo_count(ff);

    uint8_t buf[kMaxBlock * kTxFrameBytes];
    for(size_t done = 0; done < size; done += kMaxBlock)
    {
        const size_t n = size - done < kMaxBlock ? size - done : kMaxBlock;
        const uint16_t len = n * kTxFrameBytes;
        if(tu_fifo_remaining(ff) < len)
        {
            stats.tx_full++;
            return;
        }

        for(size_t c = 0; c < kTxChannels; c++)
        {
            const float* x = to_host[c] ? to_host[c] + done : nullptr;
            uint8_t* p = buf + c * kBytes;
            for(size_t i = 0; i < n; i++, p += kTxFrameBytes)
                Put24(p, x ? x[i] : 0.f);
        }
        tud_audio_write(buf, len);
        stats.tx_blocks++;
    }
}
#endif
} // namespace usb

#if FRIZZ_USB_MSC
namespace usb
{
// the card as a drive: TinyUSB asks in its task (an async transfer), MainLoop moves the
// sectors, through a buffer of its own the SD's DMA reaches (SDRAM, 32-byte aligned; TinyUSB's
// is aligned to 4)
static uint8_t DSY_SDRAM_BSS __attribute__((aligned(32))) card_buf[CFG_TUD_MSC_EP_BUFSIZE];
static bool (*card_io)(bool, uint8_t*, uint32_t, uint32_t) = nullptr;
static uint32_t card_sectors = 0;
static volatile bool card_on = false, card_gone = false;
struct CardRequest
{
    volatile bool posted;
    uint32_t lba;
    uint8_t* buf; // TinyUSB's
    uint32_t bytes;
    bool write;
};
static CardRequest card_req = {};
static CardStats card_stats = {};

void SetCard(bool (*io)(bool, uint8_t*, uint32_t, uint32_t), uint32_t sectors)
{
    card_io = io;
    card_sectors = sectors;
}

void CardMode(bool on)
{
    card_on = on;
    card_gone = false;
}

bool InCardMode() { return card_on; }

bool TakeCardGone()
{
    const bool gone = card_gone;
    card_gone = false;
    return gone;
}

const CardStats& GetCardStats() { return card_stats; }

void CardTask()
{
    if(!card_req.posted)
        return;
    const uint32_t bytes = card_req.bytes;
    bool ok = card_on && card_io;
    if(ok)
    {
        const uint32_t t0 = daisy::System::GetUs();
        if(card_req.write)
            memcpy(card_buf, card_req.buf, bytes);
        ok = card_io(card_req.write, card_buf, card_req.lba, bytes / 512);
        if(ok && !card_req.write)
            memcpy(card_req.buf, card_buf, bytes);
        card_stats.io_us += daisy::System::GetUs() - t0;
    }
    if(ok && card_req.write)
    {
        card_stats.writes++;
        card_stats.kb_written += bytes / 1024;
    }
    else if(ok)
    {
        card_stats.reads++;
        card_stats.kb_read += bytes / 1024;
    }
    else
        card_stats.errors++;
    card_req.posted = false;
    tud_msc_async_io_done(ok ? int32_t(bytes) : TUD_MSC_RET_ERROR, false);
}

// TinyUSB's task: posts the transfer for MainLoop
static int32_t CardPost(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bytes, bool write)
{
    if(!card_on || offset || !bytes || bytes % 512 || bytes > sizeof(card_buf) || card_req.posted
       || lba + bytes / 512 > card_sectors)
        return TUD_MSC_RET_ERROR;
    card_req.lba = lba;
    card_req.buf = buffer;
    card_req.bytes = bytes;
    card_req.write = write;
    card_req.posted = true;
    return TUD_MSC_RET_ASYNC;
}
} // namespace usb
#endif

} // namespace chompi

// TinyUSB's callbacks
extern "C"
{
    void TIM7_IRQHandler(void)
    {
        TIM7->SR = 0;
        chompi::usb::Task();
    }

    void OTG_HS_IRQHandler(void) { tusb_int_handler(FRIZZ_USB_RHPORT, true); }

#if FRIZZ_USB_MSC
    void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
    {
        (void)lun;
        memcpy(vendor_id, "FrizZ   ", 8);
        memcpy(product_id, "CHOMPI card     ", 16);
        memcpy(product_rev, "1.0 ", 4);
    }

    bool tud_msc_test_unit_ready_cb(uint8_t lun)
    {
        if(chompi::usb::card_on)
            return true;
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0x00); // medium not present
        return false;
    }

    void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size)
    {
        (void)lun;
        *block_count = chompi::usb::card_on ? chompi::usb::card_sectors : 0;
        *block_size = 512;
    }

    bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
    {
        (void)lun;
        (void)power_condition;
        if(load_eject && !start && chompi::usb::card_on)
            chompi::usb::card_gone = true;
        return true;
    }

    int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize)
    {
        (void)lun;
        return chompi::usb::CardPost(lba, offset, static_cast<uint8_t*>(buffer), bufsize, false);
    }

    int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize)
    {
        (void)lun;
        return chompi::usb::CardPost(lba, offset, buffer, bufsize, true);
    }

    int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize)
    {
        (void)scsi_cmd;
        (void)buffer;
        (void)bufsize;
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00); // invalid command
        return -1;
    }
#endif

#if FRIZZ_USB_AUDIO || FRIZZ_USB_MSC
    // after SET_CONFIGURATION every interface is at alt 0
    void tud_mount_cb(void)
    {
#if FRIZZ_USB_AUDIO
        chompi::usb::alt_rx = chompi::usb::alt_tx = false;
        chompi::usb::PostStop();
#endif
    }

    void tud_umount_cb(void)
    {
#if FRIZZ_USB_AUDIO
        chompi::usb::PostStop();
#endif
#if FRIZZ_USB_MSC
        if(chompi::usb::card_on)
            chompi::usb::card_gone = true;
#endif
    }

    // unplugged too, as far as FRIZZ can tell: there's no VBUS pin
    void tud_suspend_cb(bool remote_wakeup_en)
    {
        (void)remote_wakeup_en;
#if FRIZZ_USB_AUDIO
        chompi::usb::PostStop();
#endif
#if FRIZZ_USB_MSC
        if(chompi::usb::card_on)
            chompi::usb::card_gone = true;
#endif
    }
#endif

#if FRIZZ_USB_AUDIO
    void tud_resume_cb(void)
    {
        chompi::usb::Post(chompi::usb::req_rx, chompi::usb::alt_rx);
        chompi::usb::Post(chompi::usb::req_tx, chompi::usb::alt_tx);
    }

    bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const* p_request)
    {
        (void)rhport;
        const uint8_t itf = tu_u16_low(p_request->wIndex);
        const bool on = tu_u16_low(p_request->wValue) != 0;

        if(itf == ITF_NUM_AUDIO_STREAMING_RX)
            chompi::usb::Post(chompi::usb::req_rx, chompi::usb::alt_rx = on);
        else if(itf == ITF_NUM_AUDIO_STREAMING_TX)
            chompi::usb::Post(chompi::usb::req_tx, chompi::usb::alt_tx = on);
        return true;
    }

    // each packet from the host, in the USB interrupt, once it's in the OUT FIFO
    bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out,
                               uint8_t cur_alt_setting)
    {
        (void)rhport;
        (void)ep_out;
        (void)cur_alt_setting;
        chompi::usb::Stats& s = chompi::usb::stats;
        const uint16_t frames = n_bytes_received / chompi::usb::kRxFrameBytes;
        s.rx_packets++;
        s.rx_short += frames < FRIZZ_USB_SAMPLE_RATE / 1000;
        s.rx_long += frames > FRIZZ_USB_SAMPLE_RATE / 1000;
        const uint16_t fill = tud_audio_n_available(func_id);
        s.rx_full += fill >= CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ;
        s.rx_min = fill < s.rx_min ? fill : s.rx_min;
        s.rx_max = fill > s.rx_max ? fill : s.rx_max;
        return true;
    }

    bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const* p_request)
    {
        return tud_audio_set_itf_cb(rhport, p_request);
    }

    void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t* feedback_param)
    {
        (void)func_id;
        (void)alt_itf;
        // the host follows FRIZZ's clock
        feedback_param->method = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
        feedback_param->sample_freq = FRIZZ_USB_SAMPLE_RATE;
        feedback_param->fifo_count.fifo_threshold = chompi::usb::kRxPrime;
    }

#if !FRIZZ_UAC1
    // UAC2: one fixed clock, 48 kHz, always valid
    bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const* p_request)
    {
        const uint8_t entity = TU_U16_HIGH(p_request->wIndex);
        const uint8_t ctrl_sel = TU_U16_HIGH(p_request->wValue);

        if(entity != UAC_ENTITY_CLOCK)
            return false;

        if(ctrl_sel == AUDIO20_CS_CTRL_SAM_FREQ)
        {
            if(p_request->bRequest == AUDIO20_CS_REQ_CUR)
            {
                audio20_control_cur_4_t cur = {(int32_t)tu_htole32(FRIZZ_USB_SAMPLE_RATE)};
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &cur, sizeof(cur));
            }
            if(p_request->bRequest == AUDIO20_CS_REQ_RANGE)
            {
                audio20_control_range_4_n_t(1) range = {};
                range.wNumSubRanges = tu_htole16(1);
                range.subrange[0].bMin = (int32_t)tu_htole32(FRIZZ_USB_SAMPLE_RATE);
                range.subrange[0].bMax = (int32_t)tu_htole32(FRIZZ_USB_SAMPLE_RATE);
                range.subrange[0].bRes = 0;
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &range, sizeof(range));
            }
        }
        else if(ctrl_sel == AUDIO20_CS_CTRL_CLK_VALID && p_request->bRequest == AUDIO20_CS_REQ_CUR)
        {
            audio20_control_cur_1_t valid = {1};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &valid, sizeof(valid));
        }
        return false;
    }

    bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const* p_request, uint8_t* buf)
    {
        (void)rhport;
        const uint8_t entity = TU_U16_HIGH(p_request->wIndex);
        const uint8_t ctrl_sel = TU_U16_HIGH(p_request->wValue);

        return entity == UAC_ENTITY_CLOCK && ctrl_sel == AUDIO20_CS_CTRL_SAM_FREQ
               && p_request->bRequest == AUDIO20_CS_REQ_CUR && p_request->wLength == 4
               && tu_unaligned_read32(buf) == FRIZZ_USB_SAMPLE_RATE;
    }
#else
    // UAC1: the endpoints' sampling frequency, if a host asks though none is declared: 48 kHz
    bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const* p_request)
    {
        const uint8_t ctrl_sel = TU_U16_HIGH(p_request->wValue);
        if(ctrl_sel != AUDIO10_EP_CTRL_SAMPLING_FREQ || p_request->bRequest != AUDIO10_CS_REQ_GET_CUR)
            return false;
        const uint8_t freq[3] = {(uint8_t)FRIZZ_USB_SAMPLE_RATE, (uint8_t)(FRIZZ_USB_SAMPLE_RATE >> 8),
                                 (uint8_t)(FRIZZ_USB_SAMPLE_RATE >> 16)};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, (void*)freq, sizeof(freq));
    }

    bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const* p_request, uint8_t* buf)
    {
        (void)rhport;
        const uint8_t ctrl_sel = TU_U16_HIGH(p_request->wValue);
        return ctrl_sel == AUDIO10_EP_CTRL_SAMPLING_FREQ && p_request->bRequest == AUDIO10_CS_REQ_SET_CUR
               && p_request->wLength == 3 && (buf[0] | buf[1] << 8 | buf[2] << 16) == FRIZZ_USB_SAMPLE_RATE;
    }
#endif
#endif
}
#endif
