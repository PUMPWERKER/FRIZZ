/** @file PanelKeys.h
 *  @brief The keyboard's keys in their places on the panel, and each one's SMT LED: the one
 *  table the play page (FxSlots.h, NormalPage.h's scene keys), the settings page
 *  (SettingsPage.h) and MIDI's notes (MidiControl.h) go by.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "hardware.h"

namespace chompi
{

// The white keys left to right, then the dark keys: KEY_1-KEY_25 aren't in order in SwId (the
// shift registers' wiring)
static const size_t kNumWhiteKeys = 15, kNumDarkKeys = 10;
static constexpr Hardware::SwId kWhiteKeys[kNumWhiteKeys] = {
    Hardware::SwId::KEY_1,  Hardware::SwId::KEY_2,  Hardware::SwId::KEY_3,
    Hardware::SwId::KEY_4,  Hardware::SwId::KEY_5,  Hardware::SwId::KEY_6,
    Hardware::SwId::KEY_7,  Hardware::SwId::KEY_8,  Hardware::SwId::KEY_9,
    Hardware::SwId::KEY_10, Hardware::SwId::KEY_11, Hardware::SwId::KEY_12,
    Hardware::SwId::KEY_13, Hardware::SwId::KEY_14, Hardware::SwId::KEY_15};
static constexpr Hardware::SwId kDarkKeys[kNumDarkKeys] = {
    Hardware::SwId::KEY_16, Hardware::SwId::KEY_17, Hardware::SwId::KEY_18,
    Hardware::SwId::KEY_19, Hardware::SwId::KEY_20, Hardware::SwId::KEY_21,
    Hardware::SwId::KEY_22, Hardware::SwId::KEY_23, Hardware::SwId::KEY_24,
    Hardware::SwId::KEY_25};

/** A key's SMT LED (TestPage's led_map): the white keys right to left from 24, the dark keys
 *  left to right from 0; 0 for a key that isn't one of them */
constexpr uint8_t KeyLedOf(Hardware::SwId key)
{
    for (size_t w = 0; w < kNumWhiteKeys; w++)
        if (key == kWhiteKeys[w])
            return static_cast<uint8_t>(24 - w);
    for (size_t d = 0; d < kNumDarkKeys; d++)
        if (key == kDarkKeys[d])
            return static_cast<uint8_t>(d);
    return 0;
}

} // namespace chompi
