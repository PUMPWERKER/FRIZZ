/** @file FxSlots.h
 *  @brief The punch-in FX keys: one entry per FxId (FxChain.h), in the same order, with the
 *  key and colours the play page (NormalPage.h) gives each, and the master compressor's key.
 *  Their LEDs are the keys' (PanelKeys.h). The keys follow the chain (FxChain.h): the 7th white key is the phaser's, next to the
 *  flanger, though it comes after the sends in FxId; the 14th is the chaos key's (FxChaos.h),
 *  which has no place in the chain, between the sends and the compressor. Their knobs
 *  (defaults, steps, coarse grids) are in FxParams.h. The controls are described in MANUAL.md.
 */
#pragma once
#include "FxChain.h"
#include "hardware.h"
#include "LedColors.h"
#include "PanelKeys.h"

namespace chompi
{

enum class FxKind
{
    INSERT, // replaces the signal while on
    SEND,   // adds its return; the key LED also glows with the tail after release
    LOOP,   // a feedback loop around some of the inserts (the resonator)
    CHAOS,  // no sound of its own: plays with the others (FxChaos.h)
};

struct FxSlot
{
    Hardware::SwId key;           // its LED: KeyLedOf (PanelKeys.h)
    const float* key_color;
    const float* knob_colors[3];  // the knob LEDs at 0 and 1 (the middle one unused: white is
                                  // a bipolar knob's neutral point, NormalPage.h KnobColor);
                                  // a split knob's lower half (FxParams::split) goes from
                                  // the first to the last within it
    FxKind kind;
};

static const FxSlot kFxSlots[] = {
    {Hardware::SwId::KEY_1, purple, {purple, white, med_blue}, FxKind::INSERT},   // freezer
    {Hardware::SwId::KEY_2, red, {blue, white, red}, FxKind::INSERT},             // shifter
    {Hardware::SwId::KEY_3, magenta, {magenta, white, orange}, FxKind::INSERT},   // folder
    {Hardware::SwId::KEY_4, orange, {yellow, orange, red}, FxKind::INSERT},       // crusher
    {Hardware::SwId::KEY_5, pink, {pink, white, med_blue}, FxKind::INSERT},       // filter
    {Hardware::SwId::KEY_6, med_blue, {med_blue, white, purple}, FxKind::INSERT}, // flanger
    {Hardware::SwId::KEY_8, lime, {orange, white, med_blue}, FxKind::LOOP},       // resonator
    {Hardware::SwId::KEY_9, yellow, {yellow, white, green}, FxKind::INSERT},      // slicer
    {Hardware::SwId::KEY_10, teal, {teal, white, purple}, FxKind::INSERT},        // wow & flutter
    {Hardware::SwId::KEY_11, amber, {amber, white, red}, FxKind::INSERT},         // tape stop
    {Hardware::SwId::KEY_12, green, {green, white, med_blue}, FxKind::SEND},      // delay
    {Hardware::SwId::KEY_13, blue, {med_blue, blue, purple}, FxKind::SEND},       // reverb
    {Hardware::SwId::KEY_7, violet, {violet, white, orange}, FxKind::INSERT},     // phaser
    {Hardware::SwId::KEY_14, rose, {med_blue, white, rose}, FxKind::CHAOS},       // chaos
};
static_assert(sizeof(kFxSlots) / sizeof(kFxSlots[0]) == kNumFx, "one per FxId");

// The phaser's second mode, the frequency shifter (#306, kFxModes): its key and knob LEDs, as
// a slot's
static const float* const kShifterKeyColor = azure;
static const float* const kShifterKnobColors[3] = {azure, white, orange};

// The master compressor's key (MasterComp.h), the last white key: white, lighting up with
// its gain reduction. Its knob LEDs at 0 / .5 / 1
static const Hardware::SwId kCompKey = Hardware::SwId::KEY_15;

/** The FxId on white key 0-14 (PanelKeys.h, left to right: MIDI's effect channels 1-15,
 *  MidiControl.h), kNumFx on the compressor's */
inline size_t FxOnWhiteKey(size_t white)
{
    for (size_t fx = 0; fx < kNumFx; fx++)
        if (white < kNumWhiteKeys && kFxSlots[fx].key == kWhiteKeys[white])
            return fx;
    return kNumFx;
}
/** The white key 0-14 of an FxId, or of the compressor (kNumFx) */
inline size_t WhiteKeyOfFx(size_t fx)
{
    for (size_t w = 0; w < kNumWhiteKeys; w++)
        if (FxOnWhiteKey(w) == fx)
            return w;
    return kNumWhiteKeys - 1;
}
static const uint8_t kCompKeyLed = KeyLedOf(kCompKey);
static const float* const kCompKnobColors[3] = {med_blue, white, orange};

} // namespace chompi
