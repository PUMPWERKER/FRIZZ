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

static void Task()
{
    if(!connected)
    {
        connected = true;
        tud_connect();
    }
    tud_task();
    MidiTinyUsbTransport::Process();
}

bool Host() { return tud_ready(); }

// the frame number the core saw last, and since when frames come without a configuration
static uint32_t watch_frame = 0, watch_since = 0;
static bool watch_done = false;

void Watch(uint32_t now_ms)
{
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
    if(frame == watch_frame)
    {
        watch_since = 0; // no frames: no host (a charger, a battery), or it suspends us
        return;
    }
    watch_frame = frame;
    if(!watch_since)
        watch_since = now_ms | 1;
    else if(now_ms - watch_since > 10000)
        NVIC_SystemReset(); // as a restart over MIDI: the bootloader starts the launcher
}

#if FRIZZ_USB_AUDIO
bool RxStreaming() { return rx_on; }

const Stats& GetStats() { return stats; }

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

static MidiTinyUsbTransport::MidiRxParseCallback midi_rx_cb = nullptr;
static void* midi_rx_ctx = nullptr;

// filled from MainLoop with interrupts off, drained in Process(); an answer to a query is
// 45 bytes at most (MidiControl::Reply)
static constexpr size_t kMidiTxSize = 256;
static uint8_t midi_tx[kMidiTxSize];
static volatile size_t midi_tx_head = 0, midi_tx_tail = 0;
static volatile bool midi_tx_drop = false; // only Process() moves the tail

void MidiTinyUsbTransport::Reset() { midi_tx_drop = true; }

void MidiTinyUsbTransport::StartRx(MidiRxParseCallback callback, void* context)
{
    midi_rx_ctx = context;
    midi_rx_cb = callback;
}

size_t MidiTinyUsbTransport::Room() const { return kMidiTxSize - (midi_tx_head - midi_tx_tail); }

bool MidiTinyUsbTransport::Tx(uint8_t* buffer, size_t size)
{
    daisy::ScopedIrqBlocker block;
    const bool fits = (midi_tx_head - midi_tx_tail) + size <= kMidiTxSize;
    if(fits)
        for(size_t i = 0; i < size; i++)
            midi_tx[midi_tx_head++ % kMidiTxSize] = buffer[i];
    return fits;
}

void MidiTinyUsbTransport::Process()
{
    uint8_t buf[64];
    while(tud_midi_available())
    {
        const uint32_t n = tud_midi_stream_read(buf, sizeof(buf));
        if(n == 0)
            break;
        // the parser fills MidiHandler's queue, which the audio callback empties: it waits
        if(midi_rx_cb)
        {
            daisy::ScopedIrqBlocker block;
            midi_rx_cb(buf, n, midi_rx_ctx);
        }
    }

    // reset, or nobody listening: drop the queue
    if(midi_tx_drop || !tud_midi_mounted())
    {
        midi_tx_drop = false;
        midi_tx_tail = midi_tx_head;
        return;
    }

    while(midi_tx_tail != midi_tx_head)
    {
        const size_t pos = midi_tx_tail % kMidiTxSize;
        const size_t count = midi_tx_head - midi_tx_tail;
        const size_t chunk = count < kMidiTxSize - pos ? count : kMidiTxSize - pos;
        const uint32_t sent = tud_midi_stream_write(0, &midi_tx[pos], chunk);
        midi_tx_tail += sent;
        if(sent < chunk)
            break; // the rest next time
    }
}
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

#if FRIZZ_USB_AUDIO
    // after SET_CONFIGURATION every interface is at alt 0
    void tud_mount_cb(void)
    {
        chompi::usb::alt_rx = chompi::usb::alt_tx = false;
        chompi::usb::PostStop();
    }

    void tud_umount_cb(void) { chompi::usb::PostStop(); }

    void tud_suspend_cb(bool remote_wakeup_en)
    {
        (void)remote_wakeup_en;
        chompi::usb::PostStop();
    }

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
