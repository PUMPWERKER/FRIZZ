/** @file PanelLeds.h
 *  @brief What the play page (NormalPage.h) and the settings page (SettingsPage.h) share about
 *  the LEDs: the panel's by their place, how dim a key is that's off, and a colour at a level.
 */
#pragma once
#include <stdint.h>
#include "temp_led_stuff.h"

namespace chompi
{

// the panel's (PTH) LEDs; knobs 1-4 are 1-4 (NormalPage.h's kFxKnobLeds)
static const uint8_t kChompiKeyLed = 0;
static const uint8_t kTransportLedRev = 5; // lit when playing in reverse
static const uint8_t kTransportLedFwd = 6; // lit when playing forward
static const uint8_t kPlayLed = 7;
static const uint8_t kLoopLed = 8;
static const uint8_t kVolumeLed = 9;

// A key's LED when it's off: dimly in its colour. The SMT LEDs have 64 steps
// (temp_led_stuff.h), and below about 8 of them the colours run together, so off is as dim as
// the keys go while keeping their colour.
static const float kFxOffLevel = .15f;

/** A colour (r / g / b 0..1, LedColors.h) at a level, on a key's LED or the panel's */
inline void SmtLed(uint8_t led, const float* color, float level)
{
    SetSmtLedFloat(led, level * color[0], level * color[1], level * color[2]);
}
inline void PthLed(uint8_t led, const float* color, float level)
{
    SetPthLedFloat(led, level * color[0], level * color[1], level * color[2]);
}

} // namespace chompi
