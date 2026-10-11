/** @file RainbowWavePage.h
 *  @brief One-time rainbow LED intro animation UiPage at startup. Draws
 *  a rainbow on the UI.
 */
#pragma once
#include "hardware.h"
#include "temp_led_stuff.h"

namespace chompi
{
    class RainbowPage : public daisy::UiPage
    {
    public:

        // roygbivr (roll over at end for programming ease)
        static constexpr int reds[8] = {255, 255, 255, 0, 0, 75, 238, 255};
        static constexpr int greens[8] = {0, 146, 255, 255, 0, 0, 130, 0};
        static constexpr int blues[8] = {0, 0, 0, 0, 255, 130, 238, 0};

        // the hue's step from one LED to the next: the panel's over the chain with its
        // porches, as it always was
        static constexpr float kPthStep = 6.f / 22;
        static constexpr float kSmtStepBlack = 6.f / 10;
        static constexpr float kSmtStepWhite = 6.f / 15;

        float idx = 0.f;
        static constexpr float inc = .1f;

        float gain = 0.f;

        uint32_t startt = 0;
        bool down = false;

        float fade = 1.f;
        void Draw(const daisy::UiCanvasDescriptor &canvasDescriptor) override
        {
            uint32_t now = System::GetNow();
            
            if(now - last_blink_time > 1)
            {
                last_blink_time = now;

                fade -= .01f;
                if(fade > 0.f)
                {
                    // what the boot glow left, fading out
                    for(uint8_t* c = led_pth_data[0]; c < led_pth_data[0] + kPthLeds * 3; c++)
                        *c = *c * fade;
                    for(uint8_t* c = led_smt_data[0]; c < led_smt_data[0] + kSmtLeds * 3; c++)
                        *c = *c * fade;
                }

                else
                {
                    if(startt == 0)
                        startt = now;

                    if(now - startt > 1500)
                    {
                        down = true;
                        startt = now;
                    }

                    idx += inc;
                    if(idx >= 7.f)
                        idx -= 7.f;

                    if(!down)
                        gain = gain < 1.f ? gain + .02f : gain;
                    else
                        gain = gain > 0.f ? gain - .02f : gain;

                    float fidx = idx + 1.5f;
                    if(fidx >= 7.f)
                        fidx -= 7.f;

                    Wave(fidx, kPthStep, kPthLeds, true, 0, 1);
                    Wave(idx, kSmtStepBlack, 10, false, 0, 1);
                    Wave(idx, kSmtStepWhite, 15, false, 24, -1);
                }
            }

            // ========   send the data   =========
            fill_led_data();
        }

        bool OnEncoderTurned(uint16_t encoderID,
                             int16_t turns,
                             uint16_t stepsPerRevolution) override
        {
            return true; // do nothing
        }

        bool OnButton(uint16_t buttonID,
                uint8_t numberOfPresses,
                bool isRetriggering) override
        {
            return true; // do nothing
        }

        void OnFocusGained() override {}

        bool IsClosable() { return down && System::GetNow() - startt > 1000; }

    private:
        /** One row of the wave: n LEDs, the first at first and the next dir on, their hues
         *  step apart from fidx; they light up from the last and go out from the first */
        void Wave(float fidx, float step, int n, bool pth, int first, int dir)
        {
            for(int i = 0; i < n; i++)
            {
                const size_t floor = fidx;
                const size_t ceil = floor + 1;
                const float frac = fidx - int(fidx);

                float fgain = daisysp::fclamp((gain * n) - (n - 1 - i), 0.f, 1.f);
                if(down)
                    fgain = daisysp::fclamp((gain * n) - i, 0.f, 1.f);
                fgain *= fgain;

                uint8_t r = frac * (reds[ceil] - reds[floor]) + reds[floor];
                uint8_t g = frac * (greens[ceil] - greens[floor]) + greens[floor];
                uint8_t b = frac * (blues[ceil] - blues[floor]) + blues[floor];

                fidx += step;
                if(fidx >= 7.f)
                    fidx -= 7.f;

                if(pth)
                    SetPthLed(first + dir * i, fgain * r, fgain * g, fgain * b);
                else
                    SetSmtLed(first + dir * i, fgain * r, fgain * g, fgain * b);
            }
        }

        uint32_t last_blink_time = 0;
    };

    constexpr int RainbowPage::reds[];
    constexpr int RainbowPage::greens[];
    constexpr int RainbowPage::blues[];
} // namespace chompi
