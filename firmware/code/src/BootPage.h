/** @file BootPage.h
 *  @brief The boot page: a slow glow in random colours while FRIZZ starts up (kBootScreenMs,
 *  chompi_main.cpp). It swallows every key and knob, so nothing pressed during boot reaches
 *  the play page.
 */
#pragma once
#include "hardware.h"
#include "temp_led_stuff.h"

namespace chompi
{
    class BootPage : public daisy::UiPage
    {
    public:

        void Init()
        {
            RandomColors();
        }

        void RandomColors()
        {
            r = System::GetNow() % 66;
            g = System::GetNow() % 53;
            b = System::GetNow() % 36;

            r = r / 66.f;
            g = g / 53.f;
            b = b / 36.f;
        }

        void Draw(const daisy::UiCanvasDescriptor &canvasDescriptor) override
        {
            bright += bright_inc;
            if(bright > 1.f)
            {
                bright_inc *= -1.f;
            }
            else if(bright < 0.f)
            {
                RandomColors();
                bright_inc *= -1.f;
            }                

            SetPthLedsFloat(r * bright, g * bright, b * bright);
            SetSmtLedsFloat(r * bright, g * bright, b * bright);

            // ========   send the data   =========
            fill_led_data();
        }

        bool OnEncoderTurned(uint16_t encoderID,
                             int16_t turns,
                             uint16_t stepsPerRevolution) override
        {
            return true; // swallowed
        }

        bool OnButton(uint16_t buttonID,
                uint8_t numberOfPresses,
                bool isRetriggering) override
        {
            return true; // swallowed
        }

    private:
        float r = 0.f;
        float g = 0.f;
        float b = 0.f;
        float bright = 0.f;
        float bright_inc = .01f;
    };
} // namespace chompi
