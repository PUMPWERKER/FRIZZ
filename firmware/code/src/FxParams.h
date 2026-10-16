/** @file FxParams.h
 *  @brief The punch-in FX's knobs: one entry per FxId (FxChain.h), in the same order, with
 *  which knobs each uses on its two pages, their defaults, which are stepped and their coarse grids for SHIFT +
 *  turn. No hardware here, so the play page's logic (FxControls.h) can be tested on the host.
 *  The keys, LEDs and colours that go with each FX are in FxSlots.h.
 *
 *  Every effect's knobs follow one pattern, so they're guessable without labels: knob 1 is the
 *  main control, 2 feedback (or resonance), 3 tone or colour, 4 a setting of the effect's own,
 *  the one played (#117), or a send's ducking. An effect without one of these puts its odd
 *  parameter there. Page 2 (parameters 4-7, a knob press away, FxControls.h) holds what is set
 *  and left, laid out the same on every effect too (FxOutput.h): knob 1 Mix, knob 2 stereo,
 *  knob 3 Band, knob 4 Level. The sends have freeze, their own, Band and their return's level
 *  there (until #172 ducking was on page 2's knob 4 and the level on page 1's, FxScenes.h's
 *  layout 5), the resonator stereo, Band into its loop and its return's level.
 *  The tape stop has no stereo: its depth is on page 1's knob 4, its own (darken) on page 2's
 *  knob 2 (HasStereo). The finalizer (key 15) has no page 2: its knobs show one module at a
 *  time (FxControls.h).
 *
 *  The defaults leave every insert silent or nearly so: the knob that brings it in (shift,
 *  drive, rate and bits, cutoff, amount, feedback, wow and flutter) starts neutral, the
 *  others where they sound good, so one turn brings the effect in. The freezer and slicer
 *  can't be neutral: they start at their most transparent setting. Nor can the tape stop:
 *  every press stops the tape. The sends are heard at once: their key brings them in, their
 *  Level at 0dB as everyone's (#172).
 */
#pragma once
#include "FxChain.h"

namespace chompi
{

/** A parameter's coarse grid, for SHIFT + turn: the points origin + k * spacing that lie in
 *  0-1, or, if points is set, that list in ascending order instead */
struct FxGrid
{
    float origin;
    float spacing;
    const float* points;
    uint8_t num_points;
};

// one step of a stepped parameter
static constexpr FxGrid StepGrid(size_t steps) { return {0.f, 1.f / (steps - 1), nullptr, 0}; }
// shifter shift (-12..+12 over 0-1): octaves, fifths and fourths
static const float kShiftPoints[] = {0.f, 5.f / 24.f, 7.f / 24.f, .5f, 17.f / 24.f, 19.f / 24.f, 1.f};
// the phaser's rate, a split knob (FxCommon.h): 10% steps in its free half, then each of its
// divisions
static_assert(kSplitKnob == .5f && Phaser::kNumDivisions == 7, "the points below");
static const float kPhaserRatePoints[] = {0.f, .1f, .2f, .3f, .4f, .5f, .5f + 1.f / 12.f,
                                          .5f + 2.f / 12.f, .75f, .5f + 4.f / 12.f,
                                          .5f + 5.f / 12.f, 1.f};

// The finalizer's stepped knobs (Finalizer.h): its drive's types, Boost, Tube and Fuzz, and
// its compressor's characters
static const uint8_t kFinTypes = 3, kFinCharacters = 6;

/** The coarse grids, by index into kFxGrids: FxParams keeps a byte per parameter rather than
 *  a copy of its grid */
enum FxGridId : uint8_t
{
    GRID_10,       // 10% steps, for parameters without musical values
    GRID_TONE,     // octaves of a tone lowpass, 200Hz * 100^val, counted down from fully
                   // open at the top: 20k, 10k, 5k ... 312Hz
    GRID_LEVEL,    // Level (FxOutput.h): 6dB steps, 0dB on one
    GRID_QUARTER,  // quarters: the shifter's stereo in quarter semitones
    GRID_EIGHTH,   // eighths: the finalizer's input in 3dB
    GRID_SHIFT,    // the shifter's shift: octaves, fifths and fourths (kShiftPoints)
    GRID_DRIVE,    // the folder's drive: doublings, 1x to 32x
    GRID_RATE,     // the crusher's rate: 48kHz / 4, 8 ... 64, so 12k, 6k, 3k, 1.5k, 750Hz
    GRID_BITS,     // the crusher's bits: whole bits
    GRID_PITCH,    // the resonator's pitch: the notes at A440, F#0 to A5
    GRID_RES_TONE, // the resonator's tone: octaves down from 15kHz
    GRID_FREEZER_LENGTH,
    GRID_FREEZER_ROLL,
    GRID_LFO_DIVISION,
    GRID_POLARITY,
    GRID_SLICER_PATTERN,
    GRID_TAPE_STOP,
    GRID_TAPE_START,
    GRID_DELAY_DIVISION,
    GRID_CHAOS_GRID,
    GRID_PHASER_RATE, // the phaser's rate: 10% in Hz, then its divisions (kPhaserRatePoints)
    GRID_OCTAVE6,     // octaves of a knob six octaves wide: the phaser's centre; sixths: the
                      // finalizer's Loud in 3dB
    GRID_FIN_TYPE,    // the finalizer's drive types
    GRID_FIN_CHARACTER, // and its compressor's characters
    kNumFxGrids,
};
static const FxGrid kFxGrids[] = {
    {0.f, .1f, nullptr, 0},
    {1.f, .150515f, nullptr, 0},
    {FxOutput::kLevelDefault, .125f, nullptr, 0},
    {0.f, .25f, nullptr, 0},
    {0.f, .125f, nullptr, 0},
    {0.f, 1.f, kShiftPoints, sizeof(kShiftPoints) / sizeof(kShiftPoints[0])},
    {0.f, .2f, nullptr, 0},
    {.154410f, .182088f, nullptr, 0},
    {0.f, 1.f / 14.f, nullptr, 0},
    {.436295f, .0156585f, nullptr, 0},
    {1.f, .255958f, nullptr, 0},
    StepGrid(Freezer::kNumLengths),
    StepGrid(Freezer::kNumRolls),
    StepGrid(Filter::kNumLfoDivisions),
    StepGrid(2),
    StepGrid(Slicer::kNumPatterns),
    StepGrid(TapeStop::kNumStops),
    StepGrid(TapeStop::kNumStarts),
    StepGrid(DelaySend::kNumDivisions),
    StepGrid(Chaos::kNumGrids),
    {0.f, 1.f, kPhaserRatePoints, sizeof(kPhaserRatePoints) / sizeof(kPhaserRatePoints[0])},
    {0.f, 1.f / 6.f, nullptr, 0},
    StepGrid(kFinTypes),
    StepGrid(kFinCharacters),
};
static_assert(sizeof(kFxGrids) / sizeof(kFxGrids[0]) == kNumFxGrids, "one per FxGridId");

struct FxParams
{
    uint8_t knobs;                // bit p: parameter p is used, page 1's 0-3, page 2's 4-7;
                                  // the rest have no effect and their knobs stay dark
    float defaults[kNumFxParams]; // the main knob neutral, see above; stepped ones on their grid
    uint8_t steps[kNumFxParams];  // stepped parameters' number of steps, 0 = continuous
    uint8_t coarse[kNumFxParams]; // SHIFT + turn: its grid in kFxGrids (FxGridId)
    uint8_t fade;                 // bit p: the parameters that leave the effect neutral at
                                  // their defaults, whatever the others say, and its Level
                                  // (page 2's knob 4, FxOutput.h), which at 0dB leaves a
                                  // neutral effect's sound alone. A scene morph fades the
                                  // effect in and out with them (FxMorph.h); 0: none, the
                                  // freezer, shifter and slicer fade with Mix (MorphFades).
                                  // A send's is its Level alone, which a morph fades from
                                  // and to mute (MorphNeutral)
    uint8_t bipolar;              // bit p: a knob with its neutral point at its default (the
                                  // centre, or Level's 0dB), white there on its LED, blue
                                  // below and orange above (NormalPage.h)
    uint8_t split;                // bit p: a split knob (FxCommon.h), continuous in its lower
                                  // half and stepped in its upper, in steps[p] steps there
                                  // (the phaser's rate: free in Hz, then tempo divisions)
};

// The effects with a second mode (#306), by bit: the phaser's frequency shifter (FxPhaser.h),
// on the same knobs. A scene keeps it (FxScene::modes), SHIFT + the key twice switches it
// (NormalPage.h)
static const uint16_t kFxModes = 1u << FX_PHASER;

// Page 2's knob 2, stereo on the effects that have it, and page 1's knob 4, their own
// setting: the other way round until #117 (FxScenes.h's layout 4)
static const size_t kFxOwnKnob = kNumFxKnobs - 1, kFxStereoParam = kNumFxKnobs + 1;
/** Whether fx has stereo: every insert but the tape stop, and the resonator */
constexpr bool HasStereo(size_t fx) { return fx < FX_TAPESTOP || fx == FX_PHASER; }

// page 2's shared knobs at their defaults (FxOutput.h): fully the effect's, all of the
// spectrum, 0dB
static constexpr float kP2Mix = FxOutput::kMixDefault, kP2Band = FxOutput::kBandDefault,
                       kP2Level = FxOutput::kLevelDefault;
// and Band and Level are bipolar, around those defaults
static const uint8_t kP2Bipolar = 0x40 | 0x80;

/** The Level knob (FxOutput::LevelGain) for a gain: its inverse, 0 for one under its -36dB.
 *  For the sends' level from before #172, a gain from 0 to 1 (FxScenes.h's layout 5) */
inline float LevelKnob(float gain)
{
    if (gain == 1.f)
        return FxOutput::kLevelDefault;
    if (gain <= 0.f)
        return 0.f;
    return fclamp(FxOutput::kLevelDefault + 20.f * log10f(gain) / 48.f, 0.f, 1.f);
}

static const FxParams kFxParams[] = {
    // freezer: length (1 bar: a tap shorter than that hears the live signal), feedback (pure
    // repeat), roll (off), gate (each repeat whole); page 2's stereo (off)
    {0xff, {1.f, 0.f, 0.f, 0.f, kP2Mix, 0.f, kP2Band, kP2Level}, {Freezer::kNumLengths, 0, Freezer::kNumRolls, 0},
     {GRID_FREEZER_LENGTH, GRID_10, GRID_FREEZER_ROLL, GRID_10, GRID_10, GRID_10, GRID_10, GRID_LEVEL},
     0, kP2Bipolar},
    // shifter: shift (0, off), feedback (off), swoop (off), grain (30ms, the centre); page
    // 2's stereo (off). Coarse stereo: quarter semitones
    {0xff, {.5f, 0.f, 0.f, .5f, kP2Mix, 0.f, kP2Band, kP2Level}, {Shifter::kNumShifts, 0, 0, 0},
     {GRID_SHIFT, GRID_10, GRID_10, GRID_10, GRID_10, GRID_QUARTER, GRID_10, GRID_LEVEL},
     0, kP2Bipolar | 0x01 | 0x08},
    // folder: drive (1x, barely folding), shape (sine), tone (open), symmetry (off: odd
    // harmonics only); page 2's stereo (off). Coarse drive: doublings, 1x to 32x
    {0xff, {0.f, 0.f, 1.f, 0.f, kP2Mix, 0.f, kP2Band, kP2Level}, {0, 0, 0, 0},
     {GRID_DRIVE, GRID_10, GRID_TONE, GRID_10, GRID_10, GRID_10, GRID_10, GRID_LEVEL},
     0xaf, kP2Bipolar},
    // crusher: rate (21.6kHz), bits (16), tone (open), XOR (off); page 2's stereo (off).
    // Coarse rate: 48kHz / 4, 8 ... 64, so 12k, 6k, 3k, 1.5k, 750Hz; coarse bits: whole bits
    {0xff, {0.f, 0.f, 1.f, 0.f, kP2Mix, 0.f, kP2Band, kP2Level}, {0, 0, 0, 0},
     {GRID_RATE, GRID_BITS, GRID_TONE, GRID_10, GRID_10, GRID_10, GRID_10, GRID_LEVEL},
     0xaf, kP2Bipolar},
    // filter: cutoff (centre, flat), resonance, LFO depth (off), LFO division (1 bar); page
    // 2's stereo (off: both LFOs in step). The cutoff is a DJ filter's (lowpass below the
    // centre, highpass above, on WAVE's knob law: DJFilter.h), not in Hz, so coarse is 10%
    {0xff, {.5f, .5f, 0.f, .6667f, kP2Mix, 0.f, kP2Band, kP2Level}, {0, 0, 0, Filter::kNumLfoDivisions},
     {GRID_10, GRID_10, GRID_10, GRID_LFO_DIVISION, GRID_10, GRID_10, GRID_10, GRID_LEVEL},
     0x87, kP2Bipolar | 0x01},
    // flanger: rate (.55Hz), feedback, amount (off: dry), polarity (positive); page 2's
    // stereo (off). No Mix on page 2: the amount is its mix
    {0xef, {.45f, .5f, 0.f, 0.f, kP2Mix, 0.f, kP2Band, kP2Level}, {0, 0, 0, 2},
     {GRID_10, GRID_10, GRID_10, GRID_POLARITY, GRID_10, GRID_10, GRID_10, GRID_LEVEL},
     0x84, kP2Bipolar},
    // resonator: pitch (110Hz), feedback (off), tone (6.6kHz), env mod (off); page 2: stereo
    // (off), the band its loop rings in (all), its return's level (0dB). Coarse pitch: the
    // notes at A440, F#0 to A5; coarse tone: octaves down from 15kHz
    {0xef, {.4364f, 0.f, .7f, 0.f, kP2Mix, 0.f, kP2Band, kP2Level}, {0, 0, 0, 0},
     {GRID_PITCH, GRID_10, GRID_RES_TONE, GRID_10, GRID_10, GRID_10, GRID_10, GRID_LEVEL},
     0x2, kP2Bipolar},
    // slicer: pattern (xxxxxxxx), decay (1s: a slight pump), chance (off), shuffle
    // (straight); page 2's stereo (off)
    {0xff, {1.f, 1.f, 0.f, 0.f, kP2Mix, 0.f, kP2Band, kP2Level}, {Slicer::kNumPatterns, 0, 0, 0, 0, Slicer::kNumPatterns},
     {GRID_SLICER_PATTERN, GRID_10, GRID_10, GRID_10, GRID_10, GRID_SLICER_PATTERN, GRID_10, GRID_LEVEL},
     0, kP2Bipolar},
    // wow & flutter: wow (off), flutter (off), tone (open), age (none: no dropouts); page
    // 2's stereo (off)
    {0xff, {0.f, 0.f, 1.f, 0.f, kP2Mix, 0.f, kP2Band, kP2Level}, {0, 0, 0, 0},
     {GRID_10, GRID_10, GRID_TONE, GRID_10, GRID_10, GRID_10, GRID_10, GRID_LEVEL},
     0x8b, kP2Bipolar},
    // tape stop: stop (1/2 bar), spin-up (1/4 bar), curve (linear), depth (a full stop); page
    // 2's own: darken (off)
    {0xff, {.6f, .6f, 0.f, 1.f, kP2Mix, 0.f, kP2Band, kP2Level}, {TapeStop::kNumStops, TapeStop::kNumStarts, 0, 0},
     {GRID_TAPE_STOP, GRID_TAPE_START, GRID_10, GRID_10, GRID_10, GRID_10, GRID_10, GRID_LEVEL},
     0, kP2Bipolar},
    // delay: division (1/4), feedback, random (off), ducking (off); page 2: freeze (off),
    // damping (none, the centre: lows cut left, highs right), band (all), level (0dB)
    {0xff, {.25f, .4f, .5f, 0.f, 0.f, .5f, kP2Band, kP2Level}, {DelaySend::kNumDivisions, 0, 0, 0},
     {GRID_DELAY_DIVISION, GRID_10, GRID_10, GRID_10, GRID_10, GRID_10, GRID_10, GRID_LEVEL},
     0x80, kP2Bipolar | 0x04 | 0x20},
    // reverb: decay, diffusion, tone, ducking (off); page 2: freeze (off), pre-delay (none),
    // band (all), level (0dB)
    {0xff, {.6f, .6f, .6f, 0.f, 0.f, 0.f, kP2Band, kP2Level}, {0, 0, 0, 0},
     {GRID_10, GRID_10, GRID_10, GRID_10, GRID_10, GRID_10, GRID_10, GRID_LEVEL},
     0x80, kP2Bipolar},
    // phaser: rate (.7Hz, free), feedback, depth (off: dry), centre (800Hz); page 2's stereo
    // (off). Rate is split: free on the left, the tempo's divisions on the right; coarse
    // centre: octaves
    {0xff, {.25f, .4f, 0.f, .5f, kP2Mix, 0.f, kP2Band, kP2Level}, {Phaser::kNumDivisions, 0, 0, 0},
     {GRID_PHASER_RATE, GRID_10, GRID_10, GRID_OCTAVE6, GRID_10, GRID_10, GRID_10, GRID_LEVEL},
     0x84, kP2Bipolar, 0x01},
    // chaos: FX chance (off), scramble chance (off), grid (1/8), random to pattern (random);
    // no page 2. Both chances fade it in and out
    {0x0f, {0.f, 0.f, .25f, 0.f, kP2Mix, 0.f, kP2Band, kP2Level}, {0, 0, Chaos::kNumGrids, 0},
     {GRID_10, GRID_10, GRID_CHAOS_GRID, GRID_10, GRID_10, GRID_10, GRID_10, GRID_10},
     0x3, 0},
};
static_assert(sizeof(kFxParams) / sizeof(kFxParams[0]) == kNumFx, "one per FxId");
static_assert(Freezer::STEREO == kFxStereoParam && Shifter::STEREO == kFxStereoParam
                  && Folder::STEREO == kFxStereoParam && Crusher::STEREO == kFxStereoParam
                  && Filter::STEREO == kFxStereoParam && Flanger::STEREO == kFxStereoParam
                  && Resonator::STEREO == kFxStereoParam && Slicer::STEREO == kFxStereoParam
                  && Warble::STEREO == kFxStereoParam && Phaser::STEREO == kFxStereoParam,
              "stereo on page 2's knob 2 (HasStereo)");
static_assert(kNumFxParams <= 8, "a bit per parameter in knobs and fade");
static_assert(DelaySend::LEVEL == FxOutput::kLevel && ReverbSend::LEVEL == FxOutput::kLevel
                  && DelaySend::DUCKING == kFxOwnKnob && ReverbSend::DUCKING == kFxOwnKnob,
              "the sends' level on page 2's knob 4, their ducking on page 1's (#172)");

// The finalizer's knobs (Finalizer.h), edited like an FX's from its own key, but always on
// and not part of a scene: four modules of four knobs, no page 2, parameter m * 4 + k on
// module m's knob k. Each at its default is neutral, and all of them together an exact bypass.
// Level: input (0dB, the centre), drive (off), type (Boost), Loud (off); coarse input and Loud
// in 3dB. EQ: the low, mid and high bands' levels (0dB, the centre). Comp: the three bands'
// characters (Standard & slow), Depth (off). Width: the three bands' widths (unchanged, the
// centre)
static const size_t kFinModules = 4, kNumFinParams = kFinModules * kNumFxKnobs;
static const FxParams kFinalizerKnobs[kFinModules] = {
    {0x0f, {.5f, 0.f, 0.f, 0.f}, {0, 0, kFinTypes, 0},
     {GRID_EIGHTH, GRID_10, GRID_FIN_TYPE, GRID_OCTAVE6}, 0, 0x01},
    {0x07, {.5f, .5f, .5f, 0.f}, {0, 0, 0, 0}, {GRID_10, GRID_10, GRID_10, GRID_10}, 0, 0x07},
    {0x0f, {.4f, .4f, .4f, 0.f}, {kFinCharacters, kFinCharacters, kFinCharacters, 0},
     {GRID_FIN_CHARACTER, GRID_FIN_CHARACTER, GRID_FIN_CHARACTER, GRID_10}, 0, 0},
    {0x07, {.5f, .5f, .5f, 0.f}, {0, 0, 0, 0}, {GRID_10, GRID_10, GRID_10, GRID_10}, 0, 0x07},
};

} // namespace chompi
