# Effects across the firmwares

What each firmware's audio path does, as found in the source. TAPE, TEMPO and WAVE are
independent forks, so a same-named file (`reverb.h`, `DJFilter.h`, …) is a separate copy in each
`code/src/` (under `reference/firmware/`) and may differ. FRIZZ is the custom firmware in
`firmware/`, forked from WAVE.

None of the three original firmwares does real bit-depth reduction. The closest is TEMPO's
sample-rate reducer (decimation). TAPE's "lofi" knob page is saturation.

## Overview

| Effect | TAPE | TEMPO | WAVE | Implementation |
|---|:-:|:-:|:-:|---|
| **DJ filter** (one knob: LP ← off → HP, with resonance) | ✅ | ✅ | ✅ | `DJFilter.h` on top of `BasicMMF.h` |
| **Saturation / soft clip** (drive into `daisysp::SoftClip`, plus makeup gain) | ✅ | ✅ | ✅ | inline in the engine |
| **Reverb** (Rings/Clouds Griesinger topology: amount, time, lowpass, diffusion) | ✅ | ✅ (+ freeze) | ✅ | `reverb.h` + `fx_engine.h` |
| **Delay** (mono-in ping-pong, int16 in SDRAM) | ✅ | – | ✅ | `InterpolatedDelayLine.h` |
| **Tempo-synced delay** (clock divisions, random retrigger / reverse / pitch events, octave-up shimmer with random pan, buffer freeze) | – | ✅ | – | `granularDelay.h` (`granularDelay2.h` exists but isn't used) |
| **Wow & flutter ("warble")** (randomly modulated short delay) | ✅ | – | – | `Warble.h` |
| **Sample-rate reduction** (decimation) | – | ✅ | – | `daisysp::SampleRateReducer` in `SampleEngine.h` / `SliceEngine.h` |
| **Ducking compressor** (wet ducked by dry) | – | ✅ | – | `SimpleCompressor.h` |
| **Output compressor / limiter** ("final comp") | ✅ | ✅ | ✅ | `limiter.h` |
| **Filter LFO** (triangle, modulates cutoff) | – | – | ✅ | `subtractiveEngine.h` |
| **Pitch LFO / vibrato** (±2 semitones) | – | – | ✅ | `subtractiveEngine.h` |
| **Pan** | ✅ (per slot) | ✅ | ✅ | inline |
| **Reverse / varispeed** (negative pitch = reverse) | ✅ (voices + looper) | ✅ | – | sampler / player |
| **Looper** (record, scrub, varispeed) | ✅ | – | – | `LooperEngine.h` |

Utility processing, not creative effects: the mic input conditioning (`MicFilter.h`: a 150 Hz
high-pass and 5 notch filters against feedback), DC blockers, and the envelope followers that
drive the VU LEDs.

## Signal chain per firmware

- **TAPE** (`DSPEngine.h`): voices → soft clip → `ApplyFx` = DJ filter → saturation → warble →
  delay → reverb, a block that can sit before or after the looper (`fx_pre_loop`) → output
  compressor. The magic knob has three pages: reverb/delay, **lofi** (= saturation) and filter.
  Warble, filter resonance and delay time are in the SHIFT menu.
- **TEMPO** (`FxEngine.h`): each engine (chromatic, slice) runs pan → sample-rate reducer → DJ
  filter, then splits into a dry and a wet bus. Wet → granular delay → reverb → ducking compressor
  (keyed from dry), then output compressor → saturation. `setMasterRes` exists but no UI calls
  it, so TEMPO's filter has no resonance control.
- **WAVE** (`subtractiveEngine.h`): per voice wavetable → amp envelope → its own DJ filter, with a
  shared filter LFO and pitch LFO; voices summed → delay → reverb → output compressor →
  saturation → pan.
- **FRIZZ** (`FxChain.h`): AUX input → input/loop mix with the looper → freezer → shifter →
  folder → crusher → filter → flanger → phaser → slicer → wow & flutter → tape stop → delay → reverb
  (fed the delay's echoes too) → master compressor (`MasterComp.h`: threshold, ratio, attack,
  release, and mix and makeup on page 2, on its own key) → output gain → safety limiter
  (`limiter.h` at its lowest setting), the only thing that sets a level by itself. Every
  insert but the resonator and tape stop sits inside page 2's Mix, Band and Level
  (`FxOutput.h`); the sends get its Band on their input. The
  resonator's comb loops from after the phaser back to after the freezer.
  Each effect is a punch-in key, and the keys run in this order left to right, the chaos key
  (`FxChaos.h`, no sound of its own: it gates the latched effects and scrambles the loop)
  after the sends and before the compressor. (A randomizer that played the inserts on gate
  patterns sat on the last key until it was removed.)

## Unused effects in the vendored DaisySP

DaisySP is vendored in every firmware (`code/libs/DaisySP/Source/`) and its prebuilt
`libdaisysp.a` is linked, but the firmwares use little of it. Available without writing new DSP:

- **Effects:** `bitcrush`, `decimator` (bit depth + downsampling in one), `sampleratereducer`,
  `overdrive`, `fold` (a sample-and-hold rate reducer despite the name), `wavefolder` (an
  unfiltered triangle fold; FRIZZ's folder is its own, antialiased), `chorus`, `flanger`,
  `phaser` (not a chain of stages: identical allpass engines in parallel, on 77 KB of delay
  line a channel; FRIZZ's phaser is its own, #68), `tremolo`, `autowah`, `pitchshifter`, `reverbsc`
- **Filters:** `moogladder`, `svf`, `comb`, `biquad`, `allpass`, `tone` / `atone`, `mode`, `nlfilt`

Check the memory table after adding any of them, especially on TAPE, which has almost no SRAM
left.

## Ported to FRIZZ so far

FRIZZ's punch-in FX, one file each (`firmware/code/src/Fx*.h`), in signal order, which is also
their keys' order left to right (see the FRIZZ chain above). Their controls are in
[`MANUAL.md`](../MANUAL.md).

| Key | FRIZZ effect | Taken from |
|---|---|---|
| 1st white | Freezer (insert) | Bastl Instruments' Kastle 2 FX Wizard freezer (MIT) |
| 2nd white | Shifter (insert) | FRIZZ's own two-tap shifter with aligned splices, tuned in semitones; the swoop, feedback and stereo after Bastl Instruments' Kastle 2 FX Wizard shifter (MIT) |
| 3rd white | Folder (insert) | FRIZZ's own: a sine-to-triangle fold with first-order ADAA, after DaisySP's `wavefolder` (which is the triangle alone, unfiltered) |
| 4th white | Crusher (insert) | TEMPO's sample-rate reducer, plus FRIZZ's own bit quantizer and tone lowpass, and the XOR and trigger dive from Bastl Instruments' Kastle 2 FX Wizard crusher (MIT) |
| 5th white | DJ filter (insert) | WAVE's `DJFilter.h` + `BasicMMF.h`; the triangle LFO follows WAVE's filter LFO but is synced by `TempoClock.h` |
| 6th white | Flanger (insert) | Bastl Instruments' Kastle 2 FX Wizard flanger (MIT) |
| 7th white | Phaser (insert) | FRIZZ's own (#68): six first-order allpass stages a channel with feedback, the coefficient retuned every 8 samples from a triangle LFO, free (the flanger's rate curve) or synced by `TempoClock.h` (the filter LFO's divisions) on one split knob; knobs after the SP-404MK2's Phaser and the Elektron Tonverk's Phase 98 |
| 8th white | Resonator (comb loop from after the phaser back to after the freezer) | the feedback comb Bastl Instruments' Kastle 2 FX Wizard runs around every mode (MIT) |
| 9th white | Slicer (insert) | Bastl Instruments' Kastle 2 FX Wizard slicer (MIT) |
| 10th white | Wow & flutter (insert) | TAPE's `Warble.h` as knob 1, plus FRIZZ's own flutter, tone and stereo |
| 11th white | Tape stop (insert) | FRIZZ's own: a varispeed read head on an SDRAM buffer, tempo-synced stop and spin-up |
| 12th white | Delay (send) | TEMPO's `granularDelay.h`, clocked by FRIZZ's `TempoClock.h` (the loop, MIDI clock, taps or free running) |
| 13th white | Reverb (send, also fed the delay's echoes) | TEMPO's `reverb.h` + `fx_engine.h` |
| 14th white | Chaos key (no sound: gates the latched effects, scrambles the loop) | FRIZZ's own (#65), after the Red Panda Tensor's RAND and the Chase Bliss Blooper's Scrambler |
| 15th white | Master compressor (always on, after the chain) | FRIZZ's own (`MasterComp.h`) |

## Page 2 candidates (#40)

Candidates for the second knob page: all four knobs switched together by pressing any of
them, and back to page 1 when another effect is pressed or selected. #35 built that page
(parameters 4-7 of every effect, `FxParams.h`, in scenes, morphs, CC 18-21 and SysEx).

**Decided (2026-10-10), the first round:** page 2 is the same on every effect (`FxOutput.h`),
after the SP-404MK2's BALANCE and LEVEL and Elektron's per-machine MIX: knob 1 **Mix**, knob 3
**Band** (the effect on the lows or the highs only, the rest dry: a one-pole crossover, which
adds back up exactly), knob 4 **Level** (the effect's output, off, -36 to +12 dB). Knob 2 is
one parameter of each effect's own (decided the same day, built in #49): the freezer's gate,
the shifter's grain, the flanger's polarity, the resonator's env mod, the slicer's shuffle,
wow & flutter's age, the tape stop's darken, the delay's damping, the reverb's pre-delay;
the folder's symmetry, crusher's XOR and filter's LFO division moved there from page 1's
knob 4, which is now stereo on every insert (the tape stop's: depth). The sends' page 2 is
freeze, their own, Band and ducking; the resonator's env mod, Band into the loop and its
return's Level. The compressor's: mix, a sidechain highpass on Band's knob, makeup on
Level's.
The level matches (the folder's, the crusher's `LevelGuard`) and the compressor's automatic
makeup are gone (#38): nothing sets a level by itself but the safety limiter. The tables
below were the candidates before that; knob 4's "key press" role and knob 1's low cut on the
sends are taken by Level and Band.

### How the SP-404MK2 does it

Roland's SP-404MK2 has the same problem: dozens of effects, only three knobs (CTRL 1-3).
From its reference manual ([editing the effects][sp-edit], [MFX list][sp-mfx]):

- **One toggle for all knobs.** Pressing VALUE switches all three knobs between the main
  parameters and the sub-parameters, and back. Holding VALUE gives the sub-parameters only
  while it's held. That's the grouped page #35 proposes, plus a momentary variant.
- **Page 1 is for playing, page 2 for setting up.** The main page has what you turn while
  playing (Sync Delay: time, feedback, level; Reverb: type, time, level; Resonator: root,
  brightness, feedback). The sub-page has the set-and-forget values: damping and low/high cut
  (Sync Delay, Reverb), tempo sync on/off (Flanger, Slicer), mode or type (Slicer, Filter+
  Drive), panning, and pre-delay.
- **Dry/wet mix is a recurring sub-parameter:** BALANCE on the Flanger, Crusher and Chromatic
  PS.
- **Not every effect fills the page.** Crusher has 3 parameters, Scatter 4, Chromatic PS 5.
  Empty slots are normal.
- **Its display names the page.** FRIZZ has only LEDs, so the cue that page 2 is active has
  to be unmistakable.

Analogues worth stealing: Slicer's DEPTH (closed steps duck instead of mute) and SHUFFLE;
Stopper's DEPTH (slow down to a fraction rather than stop), FLT MOD (darker as it slows) and
AMP MOD; Resonator's CHORD (more than one comb) and ENV MOD (feedback follows the input);
Cassette Sim's DRIVE and AGE; Reverb's PRE DELAY.

[sp-edit]: https://static.roland.com/manuals/sp-404mk2_reference_v300/eng/62588913.html
[sp-mfx]: https://static.roland.com/manuals/sp-404mk2_reference_v300/eng/62589045.html

### A role per knob on page 2

As on page 1 (main, feedback, tone, stereo), each knob would have one kind of job on page 2:

| Knob | Page 2 role |
|---|---|
| 1 | **Mix**: dry against wet, for an insert. A send has its level on page 1, so its knob 1 is its low cut |
| 2 | **Time and groove**: sync, shuffle, step rate, grain, pre-delay |
| 3 | **Colour 2**: what page 1's tone doesn't do: a pre-filter, damping, drive |
| 4 | **The key press**: what pressing the key does (dive, swoop, restart, freeze, capture timing) |

A Mix for the inserts is the cheapest and most general candidate. Today an insert replaces
the signal while its key is on, and only its wet amount is faded (`FxChain.h`, `FxGate`), so
a Mix only scales where that fade ends: no new DSP. It also gives harmonies on the shifter
(dry + shifted) and parallel distortion on the folder and crusher.

### Per effect

Cost: **cheap** = a multiply or exposing a fixed constant (in brackets); **small** = a
one-pole filter, a noise source or a soft clip; **moderate** = a buffer or ported code;
**heavy** = a second copy of the effect's DSP (CPU, the callback is at its limit).

| Effect | Knob 1 | Knob 2 | Knob 3 | Knob 4 |
|---|---|---|---|---|
| Freezer | Mix: the live signal under the repeats (cheap) | Gate: each repeat's length, a stutter (cheap) | Darken: the repeats lose highs each pass (small) | Capture: on the next 16th (now), 8th, beat, bar or at once (cheap, now fixed at 1/16) |
| Shifter | Mix: dry + shifted, a harmony (cheap) | Grain: the 30 ms window shorter (glitchy) or longer (smooth) (cheap, `kWindowFrames`, within the 4096 buffer) | Fine: ±50 cents (cheap) | Swoop time: the 1 s fall (cheap, now fixed) |
| Folder | Mix: parallel folding (cheap) | — | Low pass-through: a highpass before the fold, the lows stay clean (small) | Level: a trim after the level match, about −12 to +6 dB (cheap; see *Level and mix*) |
| Crusher | Mix (cheap; the SP's BALANCE) | Jitter: the rate wobbles at random (small) | Pre-filter before the reduction (small; the SP's FILTER) | Dive depth: now 10x over 0.1 s (cheap, fixed) |
| Filter | Mix (cheap) | LFO shape: triangle, square, sample and hold (cheap) | Drive into the filter (small) | Press restarts the LFO: on or off (cheap) |
| Flanger | Polarity: negative feedback (hollow) to positive (metallic) (cheap). Not a Mix: page 1's Amount already sets depth and mix together | Sync: the rate as tempo divisions (cheap, `TempoClock.h`; the SP's SYNC) | Manual: the sweep's centre, 11.6 ms now (cheap, `kCentreFrames`) | Press restarts the sweep: on or off (cheap) |
| Resonator | Chord: a second comb at an interval (heavy; the SP's CHORD) | Glide: the pitch slides to a new note (cheap) | Env mod: feedback follows the input level (small, `EnvFollower.h`; the SP's ENV MOD) | — |
| Slicer | Depth: closed steps duck instead of mute (cheap; the SP's DEPTH) | Shuffle: the even steps late (cheap; the SP's SHUFFLE) | Step rate: 32nds, 16ths (now), 16th triplets, 8ths (cheap) | Attack: the 10 ms click to soft (cheap, now fixed) |
| Wow & flutter | Wow depth, apart from page 1's rate (cheap, `kWowSpanFrames`) | Flutter rate: 7.3 / 11.7 Hz now (cheap, `kFlutterHz`) | Saturation (small; Cassette Sim's DRIVE) | Dropouts: worn tape's level dips at random (small; Cassette Sim's AGE) |
| Tape stop | Depth: slow down to a fraction rather than stop (cheap; Stopper's DEPTH). Could also go on page 1's dark knob 4 | — | Darken as it slows (small; Stopper's FLT MOD) | Quiet: from which speed the level falls with it, 5% now (cheap, `kTapeQuietRate`; Stopper's AMP MOD) |
| Delay | Low cut in the feedback (small; the SP's L DAMP) | Ducking: the echoes duck under the input (moderate, TEMPO's `SimpleCompressor.h`) | High cut in the feedback, darker each repeat (small; the SP's H DAMP) | Freeze while the key is held: the buffer loops (moderate, TEMPO's buffer lock, dropped in FRIZZ, `granularDelay.h`) |
| Reverb | Low cut (small; the SP's LOW CUT) | Pre-delay (moderate: a buffer in SDRAM; the SP's PRE DELAY) | Ducking, as the delay's (moderate) | Freeze while the key is held (cheap: Rings' freeze, taken out in FRIZZ, `reverb.h`) |

The master compressor has its own key and knobs and isn't part of this (but see *Level and
mix*).

### Level and mix

(How it was before the decision above, and why the match went: the player sets the level.)
Two effects held their output to their input's level, so punching one in changed the sound,
not the loudness:

- **Folder:** a real level match, up and down, by up to +6 dB (`FxFolder.h`). A folder's
  output is about full scale whatever goes in, so without it the folder would mostly be a
  volume jump.
- **Crusher:** a ceiling only (`LevelGuard`, 0 dB headroom): never louder than its input,
  never turned up. It stops XOR and coarse bits on a quiet signal from blasting.

What the player can't do today is set the level themselves: both are fully wet and their four
knobs are taken. Page 2 fixes that without dropping the match:

- **Mix needs the match.** A Mix blends two signals at the same level, as the SP-404's
  BALANCE does; without the match, a folder at 32x against a quiet dry signal would make
  Mix mostly a volume knob.
- **Level is the way out.** A trim after the match, on the folder's free knob 4, is the
  usual hardware pattern: drive and level side by side (Elektron puts AMP VOL on the same page
  as overdrive and bit reduction; a drive pedal has Drive and Level). The crusher keeps its
  ceiling; its Mix is enough there.
- **The folder's tone should get quieter when darker,** as the crusher's does: its level
  match measures after the tone lowpass today (#37), a fix independent of page 2.
- **The master compressor's makeup** is fixed by its knobs, up to +14 dB at full amount and
  20:1, and can't be turned off (#38). A page 2 for the compressor, with a Makeup knob (auto,
  or a fixed amount), would settle it the same way; not for the first round.

### Recommendation

The mechanism (the page toggle, the LED cue, scenes, morph, MIDI CCs) costs the same for one
parameter as for forty. The first round should prove it with the cheap ones that change the
sound the most:

1. **Mix** on the shifter, folder, crusher, filter and freezer
2. **Depth** and **shuffle** on the slicer
3. **Freeze while the key is held** on the reverb, which is cheap; the delay's later
4. **High cut** and **low cut** in the delay's feedback
5. **Sync** on the flanger

The heavy ones (the resonator's chord) and the ones that need a buffer (pre-delay, ducking)
come later, each after a bench run. Tape stop depth may go on page 1's free knob 4 instead.

What it touches besides the effects: the scene file (`SceneStore.h`, which must still read
old files), the morph (`FxMorph.h`), the MIDI CC map (`MidiControl.h`), the LED cue for page
2, and code space (`docs/CAPACITY.md`).
