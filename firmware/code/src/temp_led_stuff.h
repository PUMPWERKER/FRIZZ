/** This is a temporary kludge of stuff necessary to run the
 *  LEDs without the ideal API in libDaisy yet.
 *
 *  CHOMPI's two LED chains (PTH is the through-hole panel LEDs, SMT is
 *  the 25 keybed LEDs, both WS2812-style addressable RGB) are driven by timer
 *  PWM+DMA. Each color bit becomes one PWM pulse whose ON-duration
 *  encodes a 1 or 0 (kOneTime/kZeroTime), in timer ticks. Each chain
 *  needs a few LED-times of "porch" (kPorchSize) at each end sending zero pulses.
 *  The setup creates the correct timing and format for the data signal using the libDaisy timers.
 *  The buffers are set per LED throughout the UI pages Draw() function then formatted
 *  and put in the DMA buffers in fill_led_data() which is called at the end of Draw()
 */
#pragma once
#include "daisy_seed.h"

namespace chompi
{
    /** Global timer refs. */
    daisy::TimerHandle tim3_smt, tim5_pth;
    daisy::TimChannel ledPthPwm, ledSmtPwm;

    /** The LEDs: the panel's 10 and the keys' 25. Each chain sends kPorchSize LEDs' worth
     *  of zero pulses before and after them, which only the DMA buffers hold */
    const int kPthLeds = 10;
    const int kSmtLeds = 25;
    const int kPorchSize = 6;
    const int kPthChain = kPthLeds + 2 * kPorchSize;
    const int kSmtChain = kSmtLeds + 2 * kPorchSize;

    const size_t kOutPthDataSize = kPthChain * 3 * 8;
    const size_t kOutSmtDataSize = kSmtChain * 3 * 8;

    uint8_t led_pth_data[kPthLeds][3]; /**< RGB data */
    uint8_t led_smt_data[kSmtLeds][3]; /**< RGB data */

    uint32_t DMA_BUFFER_MEM_SECTION
        output_pth_data[kOutPthDataSize]; /**< PWM lengths data, one "duration" per bit */
    uint32_t DMA_BUFFER_MEM_SECTION
        output_smt_data[kOutSmtDataSize]; /**< PWM lengths data, one "duration" per bit */

    /** Tweaked for Rev2 hardware */
    const int kOneTime = 20; /**< measured 0.68us */
    const int kZeroTime = 10; /**< measured 0.334us */

    void EndOfLeds(void* context);

    /** @brief setup LEDs */
    void LedSetup()
    {
        // zero out the buffers: the porches stay so (fill_led_data only writes the LEDs')
        std::fill(led_pth_data[0], led_pth_data[0] + kPthLeds * 3, 0);
        std::fill(led_smt_data[0], led_smt_data[0] + kSmtLeds * 3, 0);

        std::fill(output_pth_data, output_pth_data + kOutPthDataSize, 0);
        std::fill(output_smt_data, output_smt_data + kOutSmtDataSize, 0);

        /** Config: Init copies it, so it needn't outlive the setup */
        daisy::TimerHandle::Config tim3_cfg, tim5_cfg;
        daisy::TimChannel::Config t3chn2_cfg, t5chn4_cfg;
        tim3_cfg.periph = daisy::TimerHandle::Config::Peripheral::TIM_3;
        tim5_cfg.periph = daisy::TimerHandle::Config::Peripheral::TIM_5;
        tim3_cfg.dir = daisy::TimerHandle::Config::CounterDir::UP;
        tim5_cfg.dir = daisy::TimerHandle::Config::CounterDir::UP;

        /** Init */
        tim3_smt.Init(tim3_cfg);
        tim5_pth.Init(tim5_cfg);

        /** Generate period for timer
         *  This is a marvelously useful little tidbit that should be put into a TimerHandle function or something.
         */
        uint32_t prescaler = 8;
        uint32_t tickspeed = (daisy::System::GetPClk2Freq() * 2) / prescaler;
        uint32_t target_pulse_freq = 833332; /**< 1.2 microsecond symbol length */
        uint32_t period = (tickspeed / target_pulse_freq) - 1;
        tim3_smt.SetPrescaler(prescaler - 1); /**< ps=0 is divide by 1 and so on.*/
        tim3_smt.SetPeriod(period);
        tim5_pth.SetPrescaler(prescaler - 1); /**< ps=0 is divide by 1 and so on.*/
        tim5_pth.SetPeriod(period);

        /** TIM3 Ch2 is for another set:
         *  All keyboard LEDs (looks like K1 - K28 in sequence)
         */
        t3chn2_cfg.tim = &tim3_smt;
        t3chn2_cfg.chn = daisy::TimChannel::Config::Channel::TWO;
        t3chn2_cfg.mode = daisy::TimChannel::Config::Mode::PWM;
        t3chn2_cfg.polarity = daisy::TimChannel::Config::Polarity::HIGH;
        t3chn2_cfg.pin = daisy::seed::D18;

        /** TIM5_CH4 is for another
         *  For the 10 PTH leds
         */
        t5chn4_cfg.tim = &tim5_pth;
        t5chn4_cfg.chn = daisy::TimChannel::Config::Channel::FOUR;
        t5chn4_cfg.mode = daisy::TimChannel::Config::Mode::PWM;
        t5chn4_cfg.polarity = daisy::TimChannel::Config::Polarity::HIGH;
        t5chn4_cfg.pin = daisy::seed::D16;

        /** and finally initialize */

        ledPthPwm.Init(t5chn4_cfg);
        ledSmtPwm.Init(t3chn2_cfg);

        ledSmtPwm.Start();
        ledSmtPwm.StartDma(output_smt_data, kOutSmtDataSize, EndOfLeds, (void *)&ledSmtPwm);
    }

    /** buff expects that 8 elements are available for the one 8-bit color val*/
    void populate_bits(uint8_t color_val, uint32_t *buff)
    {
        for (int i = 0; i < 8; i++)
        {
            buff[i] = (color_val & (1 << (7 - i))) > 0 ? kOneTime : kZeroTime;
        }
    }

    /** The LEDs' colours into the DMA buffers, after the porch: the panel's in RGB order,
     *  the keys' in GRB */
    void fill_led_data()
    {
        for (int i = 0; i < kPthLeds; i++)
        {
            uint32_t* out = &output_pth_data[(kPorchSize + i) * 3 * 8];
            populate_bits(led_pth_data[i][0], out);
            populate_bits(led_pth_data[i][1], out + 8);
            populate_bits(led_pth_data[i][2], out + 16);
        }
        for (int i = 0; i < kSmtLeds; i++)
        {
            uint32_t* out = &output_smt_data[(kPorchSize + i) * 3 * 8];
            populate_bits(led_smt_data[i][1], out);
            populate_bits(led_smt_data[i][0], out + 8);
            populate_bits(led_smt_data[i][2], out + 16);
        }
    }

    /** The brightness the settings page sets (SettingsPage.h), in quarters: 4 is full, as
     *  FRIZZ always was, 3 and 2 dimmer */
    static uint8_t led_quarters = 4;
    inline void SetLedQuarters(uint8_t quarters) { led_quarters = quarters; }

    /** A colour byte to the chain's: v / div at full, less when dimmed, rounded in one step so
     *  a dim colour keeps its hue, and a channel lit at full stays lit */
    inline uint8_t LedDim(uint8_t v, uint32_t div)
    {
        const uint32_t d = v * led_quarters / (4u * div);
        return static_cast<uint8_t>(d == 0 && v >= div ? 1 : d);
    }

   void SetPthLed(int index, uint8_t r, uint8_t g, uint8_t b)
    {
        led_pth_data[index][0] = LedDim(r, 11);
        led_pth_data[index][1] = LedDim(g, 11);
        led_pth_data[index][2] = LedDim(b, 11);
    }
    void SetSmtLed(int index, uint8_t r, uint8_t g, uint8_t b)
    {
        led_smt_data[index][0] = LedDim(r, 4);
        led_smt_data[index][1] = LedDim(g, 4);
        led_smt_data[index][2] = LedDim(b, 4);
    }

    /** Every LED of a chain dark */
    inline void PthLedsOff() { std::fill(led_pth_data[0], led_pth_data[0] + kPthLeds * 3, 0); }
    inline void SmtLedsOff() { std::fill(led_smt_data[0], led_smt_data[0] + kSmtLeds * 3, 0); }

    /** 0..1 to 0..255; outside 0..1 the conversion to uint8_t would wrap (1.004 is dark) */
    inline uint8_t LedByte(float v)
    {
        return static_cast<uint8_t>((v < 0.f ? 0.f : (v > 1.f ? 1.f : v)) * 255.f);
    }
    void SetPthLedFloat(int index, float r, float g, float b)
    {
        SetPthLed(index, LedByte(r), LedByte(g), LedByte(b));
    }
    void SetSmtLedFloat(int index, float r, float g, float b)
    {
        SetSmtLed(index, LedByte(r), LedByte(g), LedByte(b));
    }

    /** Every LED of a chain in one colour: the first's, copied */
    __attribute__((noinline)) void SetPthLedsFloat(float r, float g, float b)
    {
        SetPthLedFloat(0, r, g, b);
        for (int i = 1; i < kPthLeds; i++)
            std::copy(led_pth_data[0], led_pth_data[0] + 3, led_pth_data[i]);
    }
    __attribute__((noinline)) void SetSmtLedsFloat(float r, float g, float b)
    {
        SetSmtLedFloat(0, r, g, b);
        for (int i = 1; i < kSmtLeds; i++)
            std::copy(led_smt_data[0], led_smt_data[0] + 3, led_smt_data[i]);
    }

    void EndOfLeds(void *context)
    {
        daisy::TimChannel *pwm = (daisy::TimChannel *)context;

        // restart from the top
        if(pwm->GetConfig().chn == daisy::TimChannel::Config::Channel::FOUR) // PTH stop, start SMT
        {
            pwm->SetPwm(0);
            ledSmtPwm.Start();
            ledSmtPwm.StartDma(output_smt_data, kOutSmtDataSize, EndOfLeds, (void *)&ledSmtPwm);
        }
        else // channel TWO
        {
            pwm->SetPwm(0);
            ledPthPwm.Start();
            ledPthPwm.StartDma(output_pth_data, kOutPthDataSize, EndOfLeds, (void *)&ledPthPwm);
        }
    }

    // ======== helper functions for color crossfading, a channel at a time (NormalPage.h's
    // Xfade does all three) ========
    float color_xfade(float start, float end, float idx)
    {
        return (1.f - idx) * start + idx * end;
    }

    float color_triple_xfade(float start, float mid, float end, float idx)
    {
        if(idx < .5f)
        {
            idx *= 2.f;
            return color_xfade(start, mid, idx);
        }
        else
        {
            idx = (idx - .5f) * 2.f;
            return color_xfade(mid, end, idx);
        }
    }

    float color_quad_xfade(float start, float mid1, float mid2, float end, float idx)
    {
        // thirds, so idx 1 lands on end rather than past it
        idx = idx < 0.f ? 0.f : (idx > 1.f ? 1.f : idx) * 3.f;
        if(idx < 1.f)
            return color_xfade(start, mid1, idx);
        else if(idx < 2.f)
            return color_xfade(mid1, mid2, idx - 1.f);
        else
            return color_xfade(mid2, end, idx - 2.f);
    }
} // namespace chompi