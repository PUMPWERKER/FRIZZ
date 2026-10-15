/** @file LedColors.h
 *  @brief The LED colours the play page uses, r / g / b 0..1 (SetPthLedFloat, SetSmtLedFloat).
 */
#pragma once

namespace chompi
{

static const float white[3] = {1.f, 1.f, 1.f};
static const float red[3] = {1.f, 0.f, 0.f};
static const float yellow[3] = {1.f, .95f, 0.05f};
static const float green[3] = {0.f, 1.f, 0.f};
static const float med_blue[3] = {0.f, .84f, 1.f};
static const float blue[3] = {0.f, 0.f, 1.f};
static const float pink[3] = {1.f, .36f, .62f};
static const float purple[3] = {.58f, .05f, 1.f};
static const float orange[3] = {1.f, .6f, .24f};
static const float lime[3] = {.5f, 1.f, 0.f};
static const float magenta[3] = {1.f, 0.f, 1.f};
static const float teal[3] = {0.f, 1.f, .55f};
static const float amber[3] = {1.f, .35f, 0.f};
static const float rose[3] = {1.f, .08f, .3f};
static const float violet[3] = {.4f, 0.f, 1.f}; // between the blue and the purple
static const float azure[3] = {0.f, .4f, 1.f};  // between the light blue and the blue
static const float cyan[3] = {0.f, 1.f, 1.f};   // the finalizer's Comp module (FxSlots.h)

} // namespace chompi
