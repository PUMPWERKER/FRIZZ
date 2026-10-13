/** @file BootPage.h
 *  @brief The boot page: a stone dropped into water at SHIFT. A light-blue ring spreads over
 *  the panel from the CHOMPI key, by where each LED sits, and runs out in kRippleMs, while
 *  main() reads the keys for shipping mode (chompi_main.cpp); MainLoop closes it once it's
 *  done. It swallows every key and knob, so nothing pressed during boot reaches the play page.
 */
#pragma once
#include "hardware.h"
#include "temp_led_stuff.h"

namespace chompi
{
    class BootPage : public daisy::UiPage
    {
    public:
        /** How long the ripple runs, from its first frame */
        static constexpr uint32_t kRippleMs = 500;

        /** True once the ripple has run out */
        bool Done() const { return start_ != 0 && System::GetNow() - start_ >= kRippleMs; }

        void Draw(const daisy::UiCanvasDescriptor &canvasDescriptor) override
        {
            const uint32_t now = System::GetNow();
            if(start_ == 0)
                start_ = now ? now : 1;
            const float t = static_cast<float>(now - start_);

            for(int i = 0; i < kPthLeds; i++)
            {
                const float l = Level(kPthMm[i], t);
                SetPthLedFloat(i, l * kColour[0], l * kColour[1], l * kColour[2]);
            }
            for(int i = 0; i < kSmtLeds; i++)
            {
                const float l = Level(kSmtMm[i], t);
                SetSmtLedFloat(i, l * kColour[0], l * kColour[1], l * kColour[2]);
            }

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
        static constexpr float kColour[3] = {.35f, .8f, 1.f}; // light blue
        static constexpr float kSpeed = .65f;     // mm per ms: the far end (261 mm) at 400 ms
        static constexpr float kWidthMm = 40.f;   // the ring's half width
        static constexpr float kTrailMm = 70.f;   // a weaker second ring this far behind
        static constexpr float kTrailLevel = .4f;

        /** Each LED's distance from SHIFT (KEY26) in mm, in the chains' order: from the board
         *  file's positions (twin/web/layout.json) */
        static constexpr uint16_t kPthMm[kPthLeds] = {24, 35, 64, 96, 129, 159, 179, 199, 219, 259};
        static constexpr uint16_t kSmtMm[kSmtLeds] = {26,  23,  51,  70,  89,  129, 149, 188, 208,
                                                       228, 261, 241, 221, 202, 182, 163, 143, 124,
                                                       105, 87,  70,  55,  45,  42,  48};

        /** A ring's brightness x mm from its middle: a smooth bump, 0 beyond kWidthMm */
        static float Ring(float x)
        {
            const float u = x * x / (kWidthMm * kWidthMm);
            return u < 1.f ? (1.f - u) * (1.f - u) : 0.f;
        }

        /** An LED mm from SHIFT, t ms into the ripple: the rings as they pass, fading out */
        static float Level(float mm, float t)
        {
            if(t >= kRippleMs)
                return 0.f;
            const float r = kSpeed * t;
            const float age = t / kRippleMs;
            return (1.f - age * age) * (Ring(mm - r) + kTrailLevel * Ring(mm - r + kTrailMm));
        }

        uint32_t start_ = 0; // the first frame's time, 0 before it
    };

    constexpr float BootPage::kColour[];
    constexpr uint16_t BootPage::kPthMm[];
    constexpr uint16_t BootPage::kSmtMm[];
} // namespace chompi
