# FRIZZ Manual

Every control in FRIZZ. For a first session, start with the [quick guide](QUICKSTART.md).

**SHIFT** means holding the CHOMPI key. The CHOMPI key lights white while it acts as SHIFT.
The mode switch picks the page: **down** is the play page, everything below; **up** is the
[settings page](#settings-page), for what you set once rather than play. Before v0.11 the switch
did nothing, so if the keys don't play after updating, flip it down.

## Overview

The stereo AUX input goes to the headphone and master outputs through a volume stage taken
from TAPE's Volume Engine, with a looper on the wet side of a dry/wet mix. The looper can
record free-length loops or loops quantized to whole bars of an incoming MIDI clock. The
white keys punch in effects on the mixed signal, and four dark keys save and recall their
settings as scenes, next to one that clears them. A master compressor, set from the last
white key, sits after the effects. The built-in microphone is not used.

## VOLUME knob

| Control | Function | LED |
|---|---|---|
| Turn (page 1, default) | Output gain, headphone + master (default 75%) | VU meter of the master, scaled by gain: dim when silent, then green, yellow, pink at the loudest |
| Press, then turn (page 2) | Input gain, AUX (default 75%) | blue (0%) to red (100%) |
| Press twice, then turn (page 3) | Headphone feed: left towards the AUX input on its own, right back to the master out (default: the master out) | white (master) to green (input) |
| SHIFT + turn | Input/loop mix: input only to looper only | green (input) to purple (loop) |
| SHIFT + press | Resets the mix: the loop only while there is one, otherwise the input only | |

Turning moves the gains 2% per detent, the mix (SHIFT + turn) 4% and the headphone feed 1%.
Pressing again on page 3 returns to page 1.
Picking a page blinks its number in white: once for page 1, twice for 2, and so on. Whether a press
is SHIFT + press is decided when it goes down, so letting go of CHOMPI first doesn't also
change the page. The mix starts
on the input only, jumps to the loop only when a recording finishes and back to the input only
when the loop is erased; SHIFT + press puts it back there. The punch-in FX come after this mix,
so they act on the input as well as the loop.

**Headphone feed (page 3):** the master out always carries the full signal. Turned left, the
headphones carry the AUX input on its own instead, after input gain and VOLUME: no loop, no
effects, no mix, no compressor. Use it when the master goes to a PA, mixer or recorder and the
headphones are your monitor; with headphones alone, leave it at the master. It's back on the
master at every power-on.

(Page 3 used to be the master compressor, which has its own key now, see
[Master compressor](#master-compressor). The headphone feed used to be on the mode switch,
then on page 4. Mono and the battery check moved to the [settings page](#settings-page).)

## Settings page

From v0.11. With the mode switch **up**, the keys set what you'd set once for a setup rather
than play: the MIDI channel, MIDI transport following, the mono input, where FRIZZ takes a
MIDI clock from and how it follows it, whether it sends one, and how bright its LEDs are. The loop, the effects
and MIDI play on meanwhile; only your hands are on this page. Switch back down to play.

Each key sets one thing on its press. The white keys pick the MIDI channel; the dark keys
set the rest:

| Keys | Setting | Colour |
|---|---|---|
| White keys 1 to 14 | [MIDI channel](#channel) 1 to 14; the one in force lit, the others dim | light blue |
| White key 15 (the last) | Channel 15; pressed again 16, again 15, and so on (default 16) | light blue for 15, white for 16, dim for neither |
| C# (lower octave) | MIDI on every channel: lit when on. It only switches it on; a channel key switches it off | light blue |
| D# | [MIDI transport following](#start-and-stop): on (lit) / off (dim) | green |
| F# | [Mono input](#mono-input): mono (lit) / stereo (dim) | white |
| G# | [Clock factor](#midi-clock), each press the next: as sent (default), double, half. It also shows the [beat](#tempo): lit for the first half of every beat, dim for the second | yellow (as sent), red (double), light blue (half) |
| A# | [LED brightness](#led-brightness), each press the next: 100 % (default), 75 %, 50 % | purple, as bright as the LEDs are |
| D# (upper octave) | [MIDI out](#midi-out), each press the next: off (default), TRS, TRS and USB | dim (off), orange (TRS), white (TRS and USB) |
| G# (upper octave) | [Clock source](#clock-source), each press the next: Auto (default), TRS, USB, internal | white (Auto), orange (TRS), blue (USB), pink (internal) |

The upper octave's other dark keys do nothing.

- **The beat on G#** is the tempo the effects follow right now (see [Tempo](#tempo)): with a
  loop, the loop's at its speed; otherwise the MIDI clock's, with the clock factor applied
  (a 120 BPM clock at double beats at 240); otherwise the last tempo. It's there to see the
  tempo, not to play to: it can be a little late, up to a few hundredths of a second.

- **VOLUME's LED** shows the battery all the time: white while the charging cable is in,
  green above 3.3 V, yellow below, red below 3 V (about to switch off unless charging). Pull
  the cable and it shows the battery's colour within a few seconds; after that the level is
  read every 30 s.
- The transport LEDs are purple, so the page can't be mistaken for the play page.
- **SHIFT + VOLUME, held 2 s:** hold CHOMPI and press and hold the VOLUME knob, in either
  order; after 2 s it writes a [bug report](#bug-reports), once per hold.
- Otherwise the knobs, CHOMPI, PLAY and LOOP do nothing here.
- A key you're holding when you flip the switch stays where it went down until you let go:
  an effect you hold keeps going, and the key doesn't also change a setting. The same the
  other way round.
- Flipping up ends a scene mode (SAVE, COPY, DELETE): nothing is saved, copied or deleted,
  and you're out of the mode when you flip back down.
- The low-battery warning dims with the LEDs too.
- Every setting is saved on the card with the master compressor, 2 s after the last change,
  so it comes back at power-on. None is part of a scene.

### Mono input

Mono is for a mono (TS) cable, such as one from a Lyra-8 or a guitar pedal: the plug grounds
the right channel, so in stereo the source is heard on the left only. In mono the left channel
feeds both sides, and the effects' stereo knobs spread it from there. Its key (F# of the lower
octave) toggles it;
MIDI CC 60 sets it too.

### LED brightness

At 75 % or 50 % every LED is dimmer by that much, for a dark stage. A# steps through the
three; its own LED dims with all the others, so it shows where you are. The dim colours keep their
hue, and nothing lit at 100 % goes dark. Full is as bright as FRIZZ has always been.

## Looper

| Looper | Key | Result |
|---|---|---|
| Empty | LOOP | Start recording |
| Empty | hold PLAY, press LOOP | Start a **quantized** recording (needs MIDI clock from the [clock source](#clock-source), otherwise LOOP blinks red 3 times) |
| Recording | LOOP | Stop now, or for a quantized recording at the end of the current bar, then play |
| Loop exists | PLAY | Play / pause, when you let go of PLAY (not if you pressed LOOP while holding it) |
| Loop exists | LOOP | Erase now, as LOOP goes down |
| Loop exists, playing | hold PLAY, press LOOP | Erase at the **end of the loop** (LOOP blinks red until then; paused, it erases now) |
| Erase waiting for the end | LOOP / PLAY | LOOP erases now; PLAY takes the erase back, the loop plays on |
| Any | SHIFT + LOOP | Tap tempo (see [Tempo](#tempo)); never records, stops or erases |
| Any, while a scene morphs | SHIFT + PLAY | Stops the morph where it is (see [Morphing to a scene](#morphing-to-a-scene)); doesn't play or pause |

- **Quantized:** recording starts on the press, which counts as bar 1 (4/4). Ending it records
  to the end of the bar in progress, so the loop is always a whole number of bars. It closes
  immediately if the clock stops.
- **Erasing** uses the gestures that record: LOOP at once, PLAY + LOOP quantized, here to
  the loop's own end, so it needs no clock. For half a second after LOOP stopped a recording,
  LOOP doesn't erase, so a double press can't lose the new loop.
- **Length:** up to 2:45. The loop lives in RAM and is gone at power-off. There's no overdub.

Transport knob (the big purple one), once a loop exists:

| Control | Function |
|---|---|
| Turn while playing | Speed in semitones, from 2× down to 1/16× (2 detents per semitone); stops at both ends |
| SHIFT + turn while playing | Speed in 5ths and octaves (4 detents per step): from between two of them, to the next one in the turn's direction. Past 1/16× it flips into reverse and climbs back up to −2×: the only way into reverse |
| Turn while paused | Scrub |
| SHIFT + turn while paused | Nothing (while you hold a scene morph, the [crossfader](#crossfading-by-hand)) |
| Press | Back to 1× forward |
| SHIFT + press | Nothing (in v0.11 it wrote a [bug report](#bug-reports), now on the settings page) |

LEDs: LOOP is red while recording and blinks while a quantized recording finishes its bar or
an erase waits for the loop's end, at the slow blink everything waiting uses (a picked scene
slot, the CHOMPI key ready to confirm); a refusal is 3 quick blinks.
Throughout FRIZZ, 3 quick red blinks mean refused or not stored, and 3 white ones done.
Besides the effects' own colours, red lit or blinking slowly means something else: LOOP recording or waiting to erase, the
compressor's key while the safety limiter works, the input gain near 100% (VOLUME's page
2), and on the settings page the clock factor on double and the battery below 3 V.
While a loop plays, PLAY and LOOP crossfade in white to show the position (dimmed when
paused). The transport LEDs show speed and direction while it plays, the scrub speed in white
while it's paused, and nothing without a loop.

## Tempo

The effects that follow a tempo (the delay, the filter LFO, the freezer, the slicer, the tape stop, the chaos key's grid) take it from one of three places, the first that applies:

1. **The loop, while there is one**, with or without MIDI clock. Beat 1 is the loop's start,
   so the slicer chops on the loop's beat from the first pass and the delay's echoes land on
   it. The effects follow the transport knob: at half speed the tempo halves and in reverse
   the beats run backwards. While the loop is paused, the beat runs on by itself at the
   loop's tempo, so the effects keep working on the input; scrubbing doesn't move it. When
   the loop resumes, the beat snaps back onto it at the next 1/48 of a bar: a filter LFO
   or slicer can jump there. Once a loop exists, MIDI clock
   no longer matters: a tempo change in your DAW moves neither the loop nor the effects.
2. **MIDI clock**, when there's no loop, from the input the [clock source](#clock-source)
   picks (none when it's internal). The effects get it in whole BPM, measured over the last
   few seconds, so it holds still however unsteady the clock: a jump in tempo is followed at
   once, a change of a BPM or so within about 2 seconds.
3. **The last tempo**: tapped, from the last loop or from the clock. 120 BPM at power-on.

A loop gets a whole number of beats:
- **Quantized:** its bars from the clock it was recorded to, exactly.
- **Unquantized, after a tempo was set** (by a clock, taps or an earlier loop): the number
  of beats closest to that tempo. Record at the tempo you tapped and the loop takes it on.
- **Unquantized, with no tempo set:** a guess: 1, 2, 4, 8 … beats, whichever puts it
  between 80 and 160 BPM. It goes wrong on loops of 3 or 6 bars, or with a pickup, and may
  come out at half or double the tempo you had in mind. Tap to correct it.

**Tap tempo (SHIFT + LOOP):** tap at least three times, at the tempo you want; the last five
taps (four gaps) count, and a pause of over 2 s starts over. LOOP flashes on each tap: white,
or dark when it's already lit nearly white.
- With a loop, the taps pick how many beats the loop holds; the loop's length then gives the
  exact tempo, so the effects' beats stay locked to it even if the taps were a little off,
  delay times and freezer lengths included. Tapping
  at double the loop's tempo, for example, doubles the effects' tempo.
- Without a loop, the taps set the tempo, and the last tap lands on a beat.
- Without a loop while MIDI clock runs, the clock is the tempo: LOOP blinks red 3 times.
  With the [clock source](#clock-source) on internal, no clock counts, and the taps set the
  tempo.

The tempo is limited to 50-300 BPM: at very slow loop speeds the effects stop slowing down
at 50 BPM.

## Punch-in FX

Effects sit on the white keys and act on the whole mix, after the input/loop mix and before the
master compressor and the output gain. The looper records the input before the effects, so an effect is never
printed into a loop.

Effects that are off cost (almost) no processing time, but the ones that are on do. Some
heavy combinations (the shifter and the crusher with a loop playing and the master compressor
working hard, for example) can need more time than there is, which is heard as crackles;
switching one of them off clears it.

The keys run in signal order, left to right:

```
freezer -> shifter -> folder -> crusher -> filter -> flanger -> slicer -> wow & flutter
           |<------------------- resonator loop ------------------->|
  -> tape stop -> delay -> reverb
```

The inserts fill the first 10 white keys, the 11th is the [chaos key](#chaos-key), which
makes no sound of its own but plays with the others, the delay and reverb are the 12th and
13th, the 14th is free and the 15th, the last, is the
[master compressor](#master-compressor)'s. (The delay and reverb used to sit on the 11th and
12th keys, the compressor on the 14th, and the last key was the randomizer's, which is gone;
the chaos key takes up its idea.)

- **Inserts** (freezer, shifter, folder, crusher, filter, flanger, slicer): replace the
  signal while on and stop the moment they're off. The freezer comes first, so it captures
  the clean sound and everything after it works on the repeats. The folder folds the clean
  signal and the crusher grinds the folds; the filter sweeps both, and the flanger sweeps
  what they made. The slicer chops everything, including the resonator's ringing.
- **Tape** (wow & flutter, tape stop): the last two inserts, so they bend everything before
  them, the resonator's ringing too. They come before the sends: a tape stop slows the music
  to a halt while the echoes and the reverb ring on. The tape stop doesn't end the moment
  it's off: it spins back up first.
- **Resonator** (7th white key): a comb feedback loop. While on, it taps the signal after the
  flanger and feeds it back in after the freezer, so it rings through the shifter, folder,
  crusher, filter and flanger whenever they're on.
- **Sends** (delay, reverb): the key opens the effect's input, and its output is added to the
  signal, so tails ring out after the key is released. The delay's tail rings out as plain
  echoes: its random events only start while its key is on. The delay gets the inserts' output;
  the reverb gets that plus the delay's echoes, so the echoes are reverberated while both
  are on.

| Control | Function |
|---|---|
| Hold an FX key | Effect on while held |
| Hold an FX key, then SHIFT | Latch on / off; a latched effect stays on after release. The key goes down first, then SHIFT. The latch is settled when you let go of the key: holding an FX key and using SHIFT for something else (a coarse turn, tap tempo, the mix, selecting another effect) doesn't latch it. Hold several FX keys, then SHIFT, to latch them all |
| SHIFT, then an FX key | **Select:** the knobs now edit that effect, without hearing it. It stays off (or latched, if it was), and letting go of the key does nothing, also after letting go of SHIFT first. The key flashes (white, or dark while it's already nearly white). (Before, SHIFT first latched too: now only the key first does) |
| FX key on a latched effect | Clears the latch; the effect stays on until the key is released |
| Knobs 1-4 | The parameters of the most recently pressed or selected FX key, 1% per detent; stepped ones (shifter shift, filter LFO and delay divisions, freezer length and roll, slicer pattern and stereo, tape stop and spin-up times, the flanger's polarity, the chaos key's grid) move one step per 3 detents |
| SHIFT + knobs 1-4 | Coarse: jumps to the next point of the parameter's grid per detent (see below) |
| SHIFT + press knobs 1-4 | Resets that parameter to its default, on the page shown |
| Press knobs 1-4 | **Page 2:** all four knobs turn over to the effect's second page, and their LEDs pulse; press any knob again for page 1. Pressing or selecting another effect, or the compressor's key, goes back to page 1; pressing the same effect's key keeps page 2. Every effect but the chaos key has one, and the compressor too. See [Page 2](#page-2) |

The FX keys are dimly lit in their effect's colour while off and at full brightness while
on, where the audio coming out of the effect pushes the colour towards white, from -30 dBFS
up, the peaks most. The delay and reverb keys follow their returns, so after release they
glow with the tail, fading from full back to dim. A selected key flashes white, or dark while
a loud effect already lights it nearly white. The knob
LEDs show the parameter values in the effect's colours; a knob the effect doesn't use is
dark and does nothing. Values reset at power-off unless they're saved in a
[scene](#fx-scenes).

The knob LEDs use white for one thing only, a neutral point. A knob with a centre (the
shifter's shift and grain, the filter's cutoff, the delay's random and damping, every Band)
and Level (0 dB at 3/4) are white there, blue below and orange above, the same on every
effect, so you can see at a glance which way a knob is off. Every other knob goes from the
effect's first colour to its second.

The knobs do the same kind of job on every effect, so you can find a setting without
looking it up:

| Knob | Role | Where an effect has none |
|---|---|---|
| 1 | The main control: length, pitch, drive, rate, cutoff, pattern, wow, stop time, division, decay | — |
| 2 | Feedback (the filter's resonance) | its second main control: shape, bits, decay, flutter, spin-up time, diffusion |
| 3 | Tone or colour: tone, roll, swoop, amount, LFO depth, chance, curve, random | — |
| 4 | Stereo, or level on the delay and reverb | the tape stop's depth |

The chaos key makes no sound, so its knobs are its own: 1 the FX chance, 2 the scramble
chance, 3 the grid, 4 random to pattern.

Every effect starts silent or nearly so: the knob that brings it in (shifter shift, folder
drive, crusher rate and bits, filter cutoff, flanger amount, resonator feedback, wow and
flutter, delay and reverb level) starts at off, the others at a setting that sounds good, so
one turn brings the effect in. Three can't be silent: the freezer starts at 1 bar, so a
shorter press only hears the live signal; the slicer at every step on with the longest
decay, a slight pump; and the tape stop stops on every press, over 1/2 bar.
SHIFT + press on a knob takes it back there.

| Key | Effect | Knob 1 | Knob 2 | Knob 3 | Knob 4 |
|---|---|---|---|---|---|
| 1st white | Freezer: Kastle 2 FX Wizard's, as a beat repeat | Length: 1/16, 1/8T, 1/8, 1/4T, 1/4, 1/2T, 1/2, 1 bar (default 1 bar) | Feedback: the input overdubbed into the repeats (default 0, pure repeat) | Roll: the loop halves as it repeats, down to 1/64 bar: off, slow, … fast, one step per 3 detents (default off) | Stereo: the left loop up to 45 ms longer (default off) |
| 2nd white | Shifter: a two-tap pitch shifter, with Kastle 2 FX Wizard's swoop and feedback | Shift in semitones, -12 to +12, one step per 3 detents; centre off (default 0, off) | Feedback: the shifted sound spirals (default off) | Swoop: on the key press, the shift pushes up to 2 octaves further and falls back over 1 s (default off) | Stereo: the right channel up to a semitone higher (default off) |
| 3rd white | Folder: a sine-to-triangle wavefolder, antialiased | Drive: 1x up to 32x, from barely folding to many folds (default 1x) | Shape: sine (smooth) to triangle (bright) (default sine) | Tone, lowpass 200 Hz to open (default open) | Stereo: the right channel driven up to 2x harder (default off) |
| 4th white | Crusher: TEMPO's sample-rate reducer plus bit reduction | Rate, 21.6 kHz down to 480 Hz, halving every 18% of the knob (default 21.6 kHz) | Bits, 16 down to 2 (default 16 bits) | Tone, lowpass 200 Hz to open (default open) | Stereo: the right channel's rate up to an octave lower (default off) |
| 5th white | Filter: the DJ filter from TAPE, TEMPO and WAVE (WAVE's copy) | Cutoff: lowpass left of centre, highpass right, flat at centre (default centre, flat) | Resonance (default 50%) | LFO depth (default off) | Stereo: the right channel's LFO up to half a cycle behind, so the sweep moves across (default off; only with the LFO) |
| 6th white | Flanger: Kastle 2 FX Wizard's | Rate, 0.02 Hz to 50 Hz (default 0.55 Hz) | Feedback, up to 85% (default 50%) | Amount: sweep depth and mix together, the top is pure vibrato (default 0, dry) | Stereo: the right LFO runs free and detuned (default off) |
| 7th white | Resonator: the comb Kastle 2 FX Wizard runs around every mode | Pitch, 22 Hz to 880 Hz (default 110 Hz) | Feedback, up to 98% (default 0, off) | Tone: the loop's lowpass, 1 kHz to 15 kHz (default 6.6 kHz) | Stereo: the right channel up to 12 semitones higher (default off) |
| 8th white | Slicer: Kastle 2 FX Wizard's rhythmic gate | Pattern, 8 steps of 16ths: `x.......`, `x...x...`, `..x...x.`, `x....x..`, `x..x..x.`, `x.x.x.x.`, `x.x.xx..`, `xxxxxxxx` (default `xxxxxxxx`) | Decay, 10 ms to 1 s (default 1 s) | Chance: each step flipped at random, up to 90% (default off) | Stereo: the left channel plays a pattern up the list, the right one down, 0-7 apart (default off) |
| 9th white | Wow & flutter: TAPE's warble, plus flutter | Wow: TAPE's warble knob, how often the pitch drifts and how much of it you hear, together (default off) | Flutter: a fast, shallow wobble, up to 1.4% of pitch; real tape's 0.1-0.5% is the lower half (default off) | Tone, lowpass 200 Hz to open (default open) | Stereo: the right channel drifts on its own and wobbles out of step (default off) |
| 10th white | Tape stop | Stop time: 1/16, 1/8, 1/4, 1/2, 1 bar, 2 bars (default 1/2) | Spin-up time on release: off, 1/16, 1/8, 1/4, 1/2, 1 bar (default 1/4) | Curve: linear to a brake, fast at first then dragging (default linear) | Depth: how far it slows, from a full stop at the top (default) down to half speed (the SP-404MK2 Stopper's DEPTH) |
| 11th white | [Chaos](#chaos-key): no sound of its own, it plays with the effects you've latched | FX chance: how often each latched effect drops out of a step, from never to half the steps (default off) | Scramble chance: how often a step of the loop plays from elsewhere in it, from never to every step (default off) | Grid: 1/16, 1/8, 1/4, 1/2, 1 bar, on the effects' tempo (default 1/8) | Random to pattern: left, every step rolled anew; right, a bar rolled once and repeated (default random) |
| 12th white | Delay: TEMPO's tempo-synced delay | Division: 1/8, 1/4T, 1/4, 1/2T, 1/4., 1/2, 1/2., 1 bar, 2 bars (default 1/4) | Feedback (default 40%) | Random: left of centre retrigger / reverse / pitch events, right octave-up shimmer with random pan, centre off (default off) | Level (default 0) |
| 13th white | Reverb (TEMPO's / WAVE's) | Decay (default 60%) | Diffusion (default 60%) | Tone, dark to open (default 60%) | Level (default 0) |

### Page 2

A press on any of knobs 1-4 turns all four over to a second page of the selected effect's
(or the compressor's) parameters, the ones you set and leave rather than play; another
press turns them back. On
page 2 the knob LEDs pulse, so you can't mistake which page you're turning, and a knob the
effect has nothing on is dark and does nothing. Every effect but the chaos key has a page 2. Selecting another
effect or the compressor
always brings back page 1, so the main controls are under your fingers whenever you pick an
effect. SHIFT + turn and SHIFT + press work on page 2 as on page 1. Turning the page doesn't
edit the scene, and scenes, recalls and morphs carry page 2's values along with page 1's.

Page 2 is laid out the same on every effect that has one, so your hand learns it once:

| Knob | Page 2 | Default |
|---|---|---|
| 1 | **Mix:** the dry sound against the effect's, for parallel distortion, a harmony under the shifter, a half-frozen loop. On the delay and reverb: **Freeze** | fully the effect; freeze off |
| 2 | **The effect's own:** one setting each, below | see below |
| 3 | **Band:** which part of the sound the effect works on. In the middle, all of it. Turned left, only the lows, below a split that falls from 20 kHz to 40 Hz; turned right, only the highs, above one that rises from 20 Hz to 8 kHz. The rest passes dry, so a folder can grind the highs and leave the bass clean. On the delay and reverb it filters what goes in: right a low cut, left a high cut | the middle, all of it |
| 4 | **Level:** the effect's output, not the dry sound Mix blends in: off at the bottom, then -36 dB to +12 dB. On the delay and reverb (whose level is page 1's knob 4): **Ducking** | 0 dB, at 3/4; ducking off |

| Effect | Knob 1 | Knob 2, its own | Knob 3 | Knob 4 |
|---|---|---|---|---|
| Freezer | Mix | Gate: how much of each repeat you hear, all of it down to its first eighth, a stutter (default all) | Band | Level |
| Shifter | Mix | Grain: the window the shifter reads in, 10 ms (glitchy) through 30 ms (the centre, as before) to 50 ms (smooth) | Band | Level |
| Folder | Mix | Symmetry: a bias for uneven folds and even harmonics (was page 1's knob 4; default off) | Band | Level |
| Crusher | Mix | XOR: flips bits of every sample for a digital buzz (was page 1's knob 4; default off) | Band | Level |
| Filter | Mix | LFO division: 1/16, 1/8, 1/4, 1/2, 1 bar, 2 bars, 4 bars (was page 1's knob 4; default 1 bar) | Band | Level |
| Flanger | — (its Amount is its mix) | Polarity: the feedback positive (metallic, as before) or negative (hollow), two steps | Band | Level |
| Resonator | — | Env mod: the feedback rises with the input's level, so loud notes ring longer (the SP-404MK2's ENV MOD; default off) | Band: what goes into the ringing loop | Level of what it rings |
| Slicer | Mix: at less than full, closed steps duck instead of mute (the SP-404MK2's DEPTH) | Shuffle: the even steps late, up to two thirds of a 16th, a triplet swing (default straight) | Band | Level |
| Wow & flutter | Mix | Age: worn tape's dropouts, the level dipping at random, more often, deeper and longer as you turn it up (default none) | Band | Level |
| Tape stop | Mix | Darken: the highs fall away as the tape slows (default off) | Band | Level |
| Delay | Freeze: the input shut and the echoes held as they are, by degrees; no random events while frozen | Damping: in the middle none; right, every repeat darker (a high cut); left, every repeat thinner (a low cut) | Band | Ducking: the echoes duck under what you play, back up when it stops |
| Reverb | Freeze: the input shut and the room held | Pre-delay: up to 250 ms before the room starts (default none) | Band | Ducking |

- **Nothing evens out levels by itself.** An effect that adds level, like the folder driven
  or the crusher's XOR on a quiet sound, comes out louder, and Level is how you set it.
  Level and Mix fade in and out with the key, as the effect does; with the key off, the
  sound passes untouched whatever page 2 says. The tape stop keeps them on while it spins up
  after you let go.
- **At the defaults** an effect sounds exactly as without page 2.
- **Scenes from before** load with page 2 on its defaults (the shifter's mix kept from a
  test build that had it), and the folder's symmetry, the crusher's XOR and the filter's LFO
  division move from page 1's knob 4 to page 2's knob 2, with stereo off in their place.

SHIFT + turn moves one point of a fixed grid per detent, always to the next point in the
direction you turn, so a value set finely snaps onto the grid with the first coarse move:

| Parameter | Coarse grid |
|---|---|
| Resonator pitch | The notes at A440 (110 Hz is A2), F#0 to A5 |
| Shifter shift | -12, -7, -5, 0, +5, +7, +12 semitones (octaves, fifths, fourths) |
| Shifter stereo | Quarter semitones |
| Mix and Band (page 2) | 10% |
| Level (page 2) | 6 dB steps, with 0 dB on one |
| Crusher rate | 48 kHz divided by 4, 8, 16, 32, 64: 12 kHz, 6 kHz, 3 kHz, 1.5 kHz, 750 Hz |
| Crusher bits | Whole bits |
| Folder drive | Doublings: 1x, 2x, 4x, 8x, 16x, 32x |
| Folder and crusher tone | Octaves down from open: 20 kHz, 10 kHz, 5 kHz … 312 Hz |
| Resonator tone | Octaves down from 15 kHz: 7.5 kHz, 3.75 kHz, 1.9 kHz |
| Wow & flutter tone | Octaves down from open, as the folder's and crusher's |
| Stepped parameters (freezer length and roll, filter LFO and delay divisions, slicer pattern and stereo, tape stop and spin-up times, flanger polarity, chaos grid) | One step per detent instead of per 3 |
| Everything else, filter cutoff included | 10% steps |

Filter details:
- **LFO:** a triangle on the cutoff, like WAVE's filter LFO but synced to the same tempo as
  the delay. At full depth it sweeps half the cutoff knob either way, so from the centre it
  goes all the way from lowpass to highpass. It's at the centre of the cutoff on the beat and
  rises towards highpass first. Like the delay's events, the beat is counted from the loop's
  start, or without a loop from when the clock locked, not from the DAW's beat 1.
- LEDs: the key is pink; the knobs go pink to light blue.

Freezer details:
- **Capture:** pressing the key waits for the next 16th, then records. The first pass is the
  live signal, so there's no gap; after one length it repeats. Releasing the key goes back to
  the live signal. Pressed again before the repeats have faded, they play on until the next
  16th starts the new capture. It keeps recording past the loop (up to 5 s), so the length can be turned
  up while repeating; turned past what's recorded, it plays on through the recording until
  the length is reached.
- **Feedback:** at 0 the loop repeats unchanged. Turning up mixes the input into it (up to
  30% at 75%, 80% at the top, where the loop also fades by 10% per pass).
- **Roll:** a beat repeat's build-up, which Kastle doesn't have. The four steps after off
  halve the loop after 8, 4, 2 or 1 repeats at the starting length, and every stage after
  that lasts as long as the first: at 1/8 and the slowest step, a bar of 1/8s, a bar of
  1/16s, a bar of 1/32s, then 1/64s until release. Each halving repeats the start of the
  capture, following on from the last repeat through the seam crossfade. Turning it off
  goes back to the full length.
- Lengths follow the delay's tempo. The loop seam has a 5 ms crossfade (shorter on loops
  under 20 ms), which Kastle doesn't have.
- LEDs: the key is purple; the knobs go purple to light blue.

Slicer details:
- **Steps** are 16ths, counted from the [tempo](#tempo) like the filter LFO, so a pattern is half a
  bar. Each step that's on retriggers a 10 ms attack and the decay. Pressing the key also
  triggers it, so the signal doesn't drop out until the next step.
- LEDs: the key is yellow; the knobs go yellow to green.

Flanger details:
- A ~12 ms delay swept up to its full length either way by a triangle LFO. Like Kastle's
  Amount, one knob sets both the depth and the mix: around the middle it flanges and
  chorus-sweeps, at the top it's all delayed signal, a vibrato.
- Pressing the key restarts the sweep from the centre (Kastle's trigger input).
- **Feedback** goes through the swept delay, a classic flanger's resonance. Kastle's FEEDBACK
  is a short comb around every mode, at most 8% on the flanger, so this one is stronger.
- LEDs: the key is light blue; the knobs go light blue to purple.

Shifter details:
- Two taps read the input at the shifted speed, each for a 30 ms stretch before it starts
  over, crossfaded so the level stays even. Where a tap starts over is searched for, within
  7.5 ms, so it lines up with the other one: the pitch lands within about a cent of the
  interval and doesn't wobble.
- Kastle's shifter was a single tap that faded out and in at every wrap: its pitch knob
  slid from a slight detune to a buzzing, ring-mod-like tone, too coarse to set an
  interval. FRIZZ keeps its swoop, feedback and stereo, not the buzz.
- **Swoop** is Kastle's trigger envelope: 0.1 s up, 1 s back. Kastle tied its depth to TIME;
  here it has its own knob.
- **Feedback** sends the shifted output back into the delay, so each pass shifts again: a
  fifth stacks into fifths (Kastle: its comb around every mode).
- LEDs: the key is red; the shift knob goes blue (down) through white (off) to orange (up), as every knob with a centre does, the other page-1 knobs blue to red.

Folder details:
- **The fold:** past the fold point the signal is mirrored back, again and again as the
  drive rises, each fold adding harmonics. Drive is the folding: on loud material the folds
  start low on the knob, on quiet material higher up.
- **Level:** at 1x a quiet sound comes out at its own level. Driven, a folder's output
  rises towards full scale whatever goes in, so the folder gets louder as you drive it, the
  more so on quiet material. Nothing evens that out by itself: page 2's Level and Mix are
  there for it.
- **Symmetry** shifts the fold by up to a quarter of its period: at the top, the sine fold
  becomes a cosine, all even harmonics, an octave-ish edge. The DC the bias adds is blocked.
- **Aliasing:** the sine fold hardly aliases: on a sine below about 1.5 kHz the aliases stay
  more than 85 dB down at any drive. The triangle's corners do alias: first-order ADAA
  takes off 5 to 13 dB, but on bright material high up the drive it still adds some grit,
  as DaisySP's `Wavefolder` (the triangle alone, unfiltered) does more of. (DaisySP's `Fold`
  isn't a folder: it's a sample-rate reducer, the crusher's rate knob.)
- In the resonator's loop, the folder folds the ringing on every trip.
- LEDs: the key is magenta; the knobs go magenta to orange.

Crusher details:
- LEDs: the key is orange; the knobs go yellow to red.
- **Dive:** every press of the key drops the rate up to 10x over 0.1 s and lets it recover
  over 0.4 s, Kastle's trigger dive.
- **XOR** flips fixed bits of each sample as 16-bit (Kastle's constants, up to 4000), a buzz
  that's loudest where the signal crosses zero. On its own XOR would turn silence into a
  constant offset, so what it adds is DC-blocked.
- **Level:** the XOR's flips and the coarsest bits are a fixed size whatever the signal's
  level, so on a quiet sound they come out louder than it (XOR at full on a sound 40 dB down:
  about 22 dB louder). Nothing turns that down by itself: page 2's Level and Mix are there
  for it.

Resonator details:
- The loop holds a soft clipper, a lowpass (Tone) and a 50 Hz highpass, as on Kastle, so it
  saturates rather than runs away, whatever is inside it. On its own it's a comb on the dry
  sound; with the inserts inside the loop on, the ringing goes through them on every trip:
  through the shifter it spirals, through the crusher it turns grainy, and the filter shapes
  it the way a filter in a dub delay's feedback does. With the freezer on, it rings the
  repeats; with the slicer on, the ringing is chopped into bursts.
- Like Kastle, it turns the input down as feedback goes up (by half at the most). Kastle's
  comb is fixed per mode, 22-440 Hz with about 40% feedback at most; this one is tunable
  and stronger.
- LEDs: the key is lime; the knobs go orange to light blue.

Wow & flutter details:
- **Wow** is TAPE's warble, from its SHIFT menu: a short delay (2-20 ms) that now and then
  heads for a new length at a random speed, so the pitch drifts like a worn tape's. The knob
  sets both how often (0.1 to 30 times a second) and the mix: around the middle the drifting
  copy blends with the dry sound into a chorus, at the top you hear only the drift.
- **Flutter** is two fast wobbles, at 7.3 Hz and 11.7 Hz, like a capstan's and a pinch
  roller's. It brings the effect in over the first quarter of its knob, so it works without
  the wow.
- **Tone** darkens what comes off the tape, not the dry sound.
- LEDs: the key is teal; the knobs go teal to purple.

Tape stop details:
- **Press:** the tape slows to a standstill over the stop time and the pitch falls with it.
  Below 5% of speed the level fades too, as on a real machine, and while you hold the key it
  stays stopped and silent.
- **Release:** the tape spins back up from the live signal over the spin-up time, then
  crossfades (15 ms) back to the live signal once it's at speed. A spin-up has to start from
  live: a tape slower than real time falls behind, so it can't catch up by itself. With the
  spin-up off, it goes straight back.
- **Release halfway down** spins up from that speed. **Press while it spins up** and it
  slows again from there.
- **Curve:** linear slows the tape evenly. Turned up, it brakes: fast at first, then a long
  drag at the bottom. The spin-up mirrors it, a motor's quick start and slow settle.
- The times follow the [tempo](#tempo), taken when the stop or spin-up starts, so a stop
  pressed on a beat lands on a beat.
- It comes before the delay and reverb, so their tails ring on while the tape stops. A
  latched tape stop, or a scene that latches one, keeps the whole mix silent, and the sends
  get nothing new, until it's unlatched.
- LEDs: the key is amber; the knobs go amber to red.

Delay details:
- **Tempo:** the [tempo](#tempo): with a loop, its exact tempo, so the echoes stay on its
  beats; otherwise rounded to whole BPM. Limited to 50-300 BPM so 2 bars fit the 10 s buffer.
- **Random events** are rolled on every 8th note; the knob's distance from centre is the
  chance. (TEMPO rolled them on its arpeggiator's step instead.)
- **Beat phase:** the 8th notes that random events follow are counted from the loop's start.
  Without a loop, only the clock's tempo is used, not MIDI Start / Song Position, so they're
  counted from when the clock locked (or from power-on, or the last tap, without clock), not
  from the DAW's beat 1. Echo spacing is unaffected.

- LEDs: the key is green; division green (short) to light blue (long); random blue
  (events) through white (off) to orange (shimmer), as every knob with a centre.

Reverb details:
- LEDs: the key is blue; the knobs go light blue to purple.

### Chaos key

The 11th white key, after the Red Panda Tensor's RAND and the Chase Bliss Blooper's
Scrambler. It works like any effect's key (hold to play, hold + SHIFT to latch, part of
scenes and morphs), but makes no sound of its own. It plays with what's there, on its grid,
in two ways:

- **FX gates (knob 1):** the effects you've *latched* drop out of a step and come back on
  the next, at random. An effect you're holding by hand stays on: holding a latched key takes
  it out of chaos's hands while you hold it. With nothing latched, there's nothing to gate.
  The keys of the effects show it: a dropped one dims as if off. Chance only decides when
  they act, never how they sound: their knobs stay yours.
- **Loop scramble (knob 2):** on a random step the loop plays another of its steps, from
  elsewhere in the loop, and on the next step without one it plays in place again. The loop
  itself runs on, so its position, the effects' tempo and its end don't move. Each jump is
  crossfaded (5 ms). Without a loop, or while it's paused, nothing jumps.
- **Grid (knob 3):** how often it rolls the dice, 1/16 to 1 bar, on the effects' tempo (the
  loop, MIDI clock or taps), for both. Its steps are counted from the loop's start.
- **Random to pattern (knob 4):** at the left every step is rolled anew; turned right, a
  bar's steps are kept once rolled and only now and then rolled again, so at the right one
  bar repeats and grooves. Each time the key comes on (a press, a latch, a scene), it rolls
  a fresh bar.
- It has no page 2: a knob press does nothing.
- A morph fades it in and out on its two chances. A scene file from before it loads with it
  off.
- Effects that react to coming on do so each time chaos lets them back in: the freezer
  grabs, the tape stop stops, the slicer attacks. A latched tape stop under chaos makes
  random stops.
- Over MIDI: CC 119 latches it, and its knobs are NRPN only (see [Controllers](#controllers)).
- LEDs: the key is rose; the knobs go light blue to rose.

## Master compressor

A stereo compressor on the master out, after the effects and before VOLUME, so turning
VOLUME doesn't change how hard it works. It's always on; at its default threshold it's off.
It is a plain compressor: it only turns loud parts down, and turns anything up only by its
Makeup knob. The last white key, the 15th, selects it for knobs 1-4, which then turn
it the way they turn an effect: 1% per detent, SHIFT + turn coarse, SHIFT + press resets,
and a knob press for its [page 2](#page-2).
Pressing an FX key gives the knobs back to that effect. The key does nothing else: it has no
latch, and holding it doesn't switch anything. With SHIFT it's a select, as an FX key's is.

| Knob | Page 1 | Page 2 |
|---|---|---|
| 1 | Threshold, from 0 dB down to -30 dB (default 0, off) | Mix: dry to fully compressed, for parallel compression (default fully compressed) |
| 2 | Ratio: 1.5:1, 2:1, 4:1, 8:1, 20:1 across the knob, which are also its coarse points (default 4:1) | — |
| 3 | Attack, 1 ms to 30 ms (default 5.5 ms) | Sidechain highpass: what the compressor listens to, off or from 20 Hz up to 500 Hz, so the bass doesn't pump everything else (default off) |
| 4 | Release, 40 ms to 600 ms (default 155 ms) | Makeup: 0 dB to +24 dB, 3 dB a coarse step (default 0 dB) |

Mix, Sidechain and Makeup sit on the knobs an effect has its Mix, Band and Level on.

- **Linked:** one detector for both channels, so the stereo image doesn't shift. It has a
  soft knee, 6 dB wide.
- **Level:** 0 dB is the AUX input about 7 dB below full scale, with the input gain at its
  default. At threshold 0 and makeup 0 dB the signal passes untouched; at threshold 0 with
  makeup, it's that gain alone.
- **Settings from before** (v0.11): the old Speed knob becomes both Attack and Release, Mix
  moves to page 2, and Makeup starts at 0 dB, so a compressor you had set comes back quieter
  than before, by what the automatic makeup added (half of what a 0 dB signal lost).
- **Safety limiter:** after VOLUME, every output has a fixed limiter: the old one-knob
  compressor at its lowest setting, which was its default. It keeps the outputs within full
  scale; it is what FRIZZ always had, so nothing changes until you turn the threshold down.
  It's the only thing FRIZZ does to the level by itself, so it shows: the compressor's key
  turns red while it limits, from 1 dB of limiting on and fully red from 3 dB, held 0.3 s so
  a short peak is seen. Red means turn something down: VOLUME, an effect's Level, the
  makeup.
- **Kept:** the settings are saved to `FRIZZ/frizz_master.txt` on the card 2 s after the
  last turn, and come back at power-on. They're not part of a scene. Without a card, or when
  the card can't be written, the key blinks red 3 times instead, and FRIZZ tries again 2 s
  later, up to 3 times; the settings then last until power-off.
- LEDs: the key is white, dim, lighting up with the gain reduction, at full brightness from
  12 dB, and red while the safety limiter works (above); it flashes when pressed, white, or dark while it's already bright. While selected, the knobs go light blue (0%) to orange
  (100%).

## FX scenes

A scene holds every effect's four knob values and whether it's latched. The five dark keys of
the lower octave (C#, D#, F#, G#, A#) are scene keys:

- **The first (C#) is the blank scene:** every effect off and every knob on its default. It's
  always there, a one-press way back to no effects. You can't save over it or delete it,
  but you can copy it into a slot to start a scene from scratch.
- **The other four (D#, F#, G#, A#) hold your scenes.** They're saved to the SD card and
  come back at power-on. (Before the blank scene, the scenes sat one key further left.)

A saved scene stays as you saved it: changes you make after recalling one are only kept if you
save again.

| Control | Function |
|---|---|
| Scene key | Recall the scene at once: every effect's knobs jump to it, the effects it latched come on and all others are unlatched. FX keys you're holding stay on |
| Scene key of the active scene | Back to the scene as saved, dropping your changes |
| Scene key of an empty slot | Nothing (the key blinks red) |
| Blank scene key | Every effect off, every knob back on its default |
| SHIFT + scene key | **Morph** to the scene, landing at the end of the current bar (see below) |
| SHIFT + the same scene key again, while it morphs | One bar longer, up to 8 (past that, the key blinks red) |
| SHIFT + PLAY, while it morphs | Stop the morph where it is |
| SHIFT + scene key, then turn the transport knob (SHIFT still held) | **Crossfade** to the scene by hand (see below) |

A recall is meant for performing, a build-up on one scene and the drop on the next:

- **The knobs land within about 25 ms** instead of gliding over 100 ms like a turned knob.
  Stepped values (freezer length, slicer pattern, delay division …) switch at once.
- **An effect a recall turns on starts as if you'd pressed its key:** the freezer grabs fresh
  audio, the shifter swoops, the crusher dives, the flanger's sweep restarts, the slicer
  attacks, the tape stops.
- **An effect latched in both scenes keeps running** without restarting. A freeze held
  through the drop stays frozen.
- **Delay and reverb tails ring out** after a scene unlatches them, as they were: a scene
  changes the settings of a delay or reverb that's off only when its key next comes on.
  Turning its knobs meanwhile still moves the tail.
- The input/loop mix, the volumes, the master compressor and the looper aren't part of a
  scene. The compressor keeps its settings on the card on its own.

Save, copy and delete work like TAPE's and TEMPO's preset keys, on the last three dark keys,
no SHIFT needed:

| Key | Colour | Function |
|---|---|---|
| Last dark key (A# of the upper octave) | blue | **Save** the current effect settings into a slot |
| 2nd-to-last dark key (upper G#) | green | **Copy** one slot into another, without changing the sound |
| 3rd-to-last dark key (upper F#) | amber | **Delete** a slot |

1. **Tap the function's key.** It lights up fully; the scene keys now select slots instead of
   recalling them. The effects and knobs keep working.
   The scene keys you can tap light dimly in the function's colour, the others go dark:
   every slot but the blank one for save, the saved ones for delete, the saved ones and the
   blank one for copy's source.
2. **Tap the slot.** It blinks in the function's colour. For copy, tap the source first
   (it stays lit), then the destination. Tapping a picked slot again unpicks it (for copy,
   the destination first: the source blinks red while one is picked).
3. **Tap the CHOMPI key**, which blinks in the function's colour once there's something to
   confirm. It confirms when you let go; the slot flashes white and the function ends.
   Holding CHOMPI and using another key or knob is SHIFT as ever and doesn't confirm, so
   you can still latch or select an effect or turn a knob coarsely before you save. CHOMPI
   pressed while you hold an FX key latches it and doesn't confirm either.

**To get out without doing anything, tap the function's key again.** Tapping a different
function key switches to that function.

All three speak the same colours: the function's colour shows what will happen, white that
it's done, red that it was refused (a dark slot tapped) or not stored. The slot flashes red
instead of white when there's no SD card, or the card couldn't be written: the change works
until power-off but isn't stored. The next save, copy or delete tries the card again, also
one you've put in since.

Outside a function, the scene keys show the slots: dark when empty, dimly white when saved
(the blank one always is),
bright for the scene you recalled or saved last, and pulsing once you've turned a knob or
changed a latch since. The save, copy and delete keys are dimly lit in their colour.

The scenes live in `FRIZZ/frizz_scenes.txt` on the card, one line per effect, keyed by its
name. FRIZZ creates the folder on its first start (moving the file from the card root, where
older versions kept it); after that the card is written only when you save, copy or delete,
never on a recall. Saving takes a moment in which the LEDs may pause. A scene file FRIZZ can't
read (from another version, or edited into something else) starts it with no scenes but
isn't lost: the first save moves it to `frizz_scenes.bak`.

If FRIZZ couldn't read the card at power-on (rare: it loads from the card, so the card is
there), every save flashes red until you switch off and on again, so the scenes on the card
aren't replaced by the session's.

Take the card out or swap it only while the CHOMPI is off: a card pulled while FRIZZ runs
isn't looked after. To get files on or off it (scenes, bug reports), use the multi-firmware
launcher's USB storage mode instead of taking it out.

### Morphing to a scene

SHIFT + a scene key glides from where you are (the scene you're in, with whatever you've
changed since) to that scene, and lands exactly on a bar line: tap once and it lands at the
end of the bar you let go of SHIFT in; keep holding SHIFT and tap the same key again and it
lands a bar later, and so on, up to 8. A release in the last moment of a bar still lands on that bar's end.

The glide starts when you let go of SHIFT: while you hold it, nothing moves, however long,
and the bar lines don't count yet. On the release it glides to the next bar line from there,
plus one more for each extra tap, so you can hold it as long as you like and let go in time
with the music.

- **An effect on in both scenes:** its knobs glide from their value to the scene's. Stepped
  ones (delay division, slicer pattern, shifter shift, freezer length, filter LFO division …)
  switch on the bar line.
- **An effect the scene turns on fades in:** it comes on when the glide starts with the knob
  that brings it in at off, and that knob glides up to the scene's value. Its other knobs take the scene's
  values at once. The folder, crusher and filter colour the sound with their other knobs
  too, so all of theirs (but the filter's LFO division) start on their defaults and glide.
- **An effect the scene turns off fades out:** the same knobs glide back to their defaults,
  and on the bar line the effect goes off. It stays faded out (a delay or reverb tail rings
  out at that level) and takes the scene's settings when its key next comes on.
- **The freezer, shifter and slicer** have no knob to fade with: one the scene turns on or off
  switches on the bar line, so the freezer grabs, the shifter swoops and the slicer attacks on
  the downbeat.
- **SHIFT + the blank scene** fades every effect out onto the bar line.

The bar lines are the effects' (see [Tempo](#tempo)): the loop's bars, counted from its start,
with the loop's end always one too, so a 2-beat loop morphs to its end; without a loop, the
bars of MIDI clock or the tapped tempo, counted from when the clock locked, the last tap or
power-on. A loop playing backwards or paused keeps counting bars.

While it morphs:
- The scene key blinks on the beat. The scene is the active one from the press: saving
  meanwhile saves the scene as it will land.
- The knobs and keys already show the scene. Turning a knob changes where that parameter
  lands. The FX keys work as usual, except on an effect waiting to switch on or off (fading
  in, fading out, or waiting for the bar line): there they decide whether it's on once
  that's done. A select (SHIFT, then the key) decides nothing.
- A plain scene key ends the morph and recalls that scene at once (the morph's own key
  jumps straight to the end).
- SHIFT + another scene key, or an empty one, blinks red and changes nothing.
- **SHIFT + PLAY stops it where it is:** every knob stays at the value the glide got to, and
  an effect still waiting to switch stays as it was, so one fading out stays on, unless you
  pressed its key meanwhile: then it's as you left it. The scene
  key pulses as edited, since the sound is now between two scenes: save it to keep it.
  Without a morph, SHIFT + PLAY plays and pauses like PLAY.

### Crossfading by hand

A morph at the speed of your hand instead of the bar clock, like the Octatrack's crossfader:
press SHIFT + a scene key as for a morph, and before you let go of SHIFT, turn the transport
knob. The morph is now yours: the scene you're in (with whatever you've changed) on the
left, the one you picked on the right, and everything in between, back and forth as often
as you like. 24 detents take it from one end to the other. It works with or without a loop,
and the bar lines don't count while you hold it.

- **What fades is what a morph fades:** knobs on both scenes glide with the transport knob;
  an effect only the new scene has fades in as soon as you're off the left end, one only the
  old scene has fades out and goes off at the right end. What can't fade switches in the
  middle, either way: stepped knobs (delay division, slicer pattern, shifter shift …) and the
  freezer, shifter and slicer coming on or going off.
- **Let go of SHIFT** to leave it:
  - **all the way right:** you're in the new scene, as after a recall;
  - **all the way left:** you're back where you started, the scene you were in as it was
    (pulsing if you had changed it, steady if not);
  - **in between:** it stays where it is, as SHIFT + PLAY leaves a morph: the knobs where the
    fade got to, the new scene's key pulsing as edited. Save it to keep it.
- **LEDs:** the two scene keys light in their mix, one brighter as the other dims, and the
  transport LEDs show the position in white instead of the speed: the left one for the old
  scene, the right one for the new.
- Letting go without turning is a morph as before: it glides to the bar line. While you
  hold a morph, SHIFT + turn crossfades instead of stepping the speed in 5ths and octaves;
  once you've let go, the transport knob is back to speed and scrub at once.
- Over MIDI, CC 118 is the crossfader (see [Controllers](#controllers)).

## MIDI

From v0.11 (v0.10 takes MIDI clock only). FRIZZ takes MIDI from the TRS MIDI input and over USB (CHOMPI is a USB device, so USB MIDI
comes from a computer or a host), both alike. With [MIDI out](#midi-out) on, it sends its
clock and the loop's Start, Stop and Continue out of the TRS MIDI output, and over USB if you
ask; otherwise it sends nothing over TRS, and over USB it only answers FRIZZ's own queries
(see [Remote control](#remote-control)).

### MIDI clock

Quantized recording, and the effects' tempo while there's no loop, follow MIDI clock
(24 PPQN), from the input the clock source picks. The clock is read on every channel.

### Clock source

The upper octave's G# on the [settings page](#settings-page) picks whose clock counts, each
press the next:

- **Auto** (the default, and how FRIZZ always did it): whichever input ticks first, TRS or
  USB, until it has been silent for 0.5 s; then whichever ticks next.
- **TRS** or **USB**: only that input's clock; the other's is ignored, even while the chosen
  one is silent. A DAW over USB can then play notes and send CCs without its clock getting in
  the way of a sequencer on the jack, or the other way round.
- **Internal**: no MIDI clock at all. The tempo comes from the loop, your taps or the last
  tempo, and tap tempo works while a clock runs; a quantized recording is refused, as without
  a clock.

Switching to a source that leaves out the clock FRIZZ was following lets go of it at once, as
a clock that stops: a quantized recording ends where it is. Notes, controllers, program
changes and FRIZZ's own SysEx come in from both inputs whatever the source; MIDI Start and
Stop come only from the chosen input (see [Start and Stop](#start-and-stop)). It's kept as
`clock_source` in `FRIZZ/frizz_master.txt` (0 Auto, 1 TRS, 2 USB, 3 internal), and
`firmware/remote.py source auto|trs|usb|internal` sets it too.

The **clock factor** on the [settings page](#settings-page) makes FRIZZ follow the clock at
half its tempo or at double: a DAW at 70 BPM with FRIZZ thinking in 140, or the other way
round. It counts for everything that follows the clock: the bars of a quantized recording and
the effects' tempo. A loop, once there is one, keeps its own tempo. Changing it while a
quantized recording ends that recording where it is, as a lost clock does.

### Channel

Notes, controllers and program changes count on one channel: **16** at first, the one a
setup sending notes to other instruments over the same cable uses least. FRIZZ keeps it on
the card, in `FRIZZ/frizz_master.txt` (`midi_channel 16`; 0 listens on every channel). Change
it on the [settings page](#settings-page), in the file, or from a computer with
`firmware/remote.py channel N`.

### Notes: the keys

A note holds the key it lands on while it's on, as your finger would: an FX key punches its
effect in, a scene key recalls, and so on. The 25 keys are a keyboard from **note 48** (C3,
or C2 in Ableton Live): the white keys from 48 up, the dark keys on the sharps. Below them:

| Note | Key |
|---|---|
| 36, 37, 38, 39 | Knob 1, 2, 3, 4 pressed |
| 40 | The transport knob pressed |
| 41 | VOLUME pressed |
| 45 | CHOMPI (SHIFT) |
| 46 | PLAY |
| 47 | LOOP |

- A key held by your hand and by a note is pressed once, and let go when both have let go.
- The velocity doesn't matter; a note on at velocity 0 is a note off.
- All Notes Off (CC 123) or All Sound Off (CC 120) lets go of every key MIDI holds, for a
  note off that went missing.

### Controllers

| CC | Function |
|---|---|
| 14, 15, 16, 17 | Knob 1, 2, 3, 4 turned, relative: 1-63 turns that many detents right, 127-65 that many left (two's complement). SHIFT held turns coarsely, as by hand |
| 18 | The transport knob turned, relative, in detents as by hand: 2 per semitone. SHIFT held (the CHOMPI key's note, 45) turns it in 5ths and octaves, 4 detents a step |
| 19 | VOLUME turned, relative, on the page it's on |
| 20-31 | An effect's latch, by its key left to right, white keys 1-10, 12 and 13, the chaos key left out (20 the freezer, 29 the tape stop, 30 the delay, 31 the reverb): 64 and above latched, below off |
| 52, 53, 54, 55 | The master compressor's threshold, ratio, attack and release (page 2: NRPN, below) |
| 56 | Output gain |
| 57 | Input gain |
| 58 | Input/loop mix |
| 59 | Headphone feed: 0 the master out, 127 the AUX input on its own (the other way round from VOLUME's page 3, where turning left goes towards the input) |
| 60 | Mono input: 64 and above mono, below stereo |
| 61 | How many bars a morph over CC 62 takes, 1-8 (1 at first) |
| 62 | Morph to scene 0-4 (0 the blank one): as SHIFT + its key, landing on the bar line |
| 63 | 64 and above: stops a morph where it is |
| 70-117 | An effect's knob, set outright: 70 + 4 × the effect + the knob − 1, the effects counted from 0 in key order, the chaos key left out: the freezer 70-73, shifter 74-77, folder 78-81, crusher 82-85, filter 86-89, flanger 90-93, resonator 94-97, slicer 98-101, wow & flutter 102-105, tape stop 106-109, delay 110-113, reverb 114-117 |
| 118 | The [crossfader](#crossfading-by-hand): takes a running morph (CC 62's, or one you hold) to where it says, 0 the scene it started from, 127 the scene it goes to. Reaching 127 lands it; reaching 0 after being away from it goes back to where it started; in between it waits |
| 119 | The [chaos key](#chaos-key)'s latch: 64 and above latched, below off. Its knobs have no CCs: NRPN MSB 0, LSB 122-125 set knobs 1-4 |

- **Set outright** (CC 20-117), a value does what the knob or key would: the LEDs follow, the
  scene key pulses as edited, a morph lands where the CC put the parameter, a send ringing out
  takes it when its key next comes on. The knobs stay on the effect you picked. 64 is a knob's
  centre exactly (the filter's flat cutoff, a delay's random off), 127 its top. Stepped
  parameters take the nearest step.
- **14 bits:** NRPN with the CC's number as the parameter (NRPN MSB 0, LSB the CC) sets it
  in 16,384 steps (8192 the centre): CC 99 0, CC 98 the CC number, CC 6 the value's top 7
  bits, CC 38 its bottom 7.
- **[Page 2](#page-2)** of an effect's or the compressor's knobs has no CCs (too few are free):
  NRPN MSB 1 with the same knob's page-1 CC number as LSB sets it, so the shifter's page-2
  knob 1 (its mix) is CC 99 1, CC 98 74, and the compressor's makeup CC 99 1, CC 98 55,
  then CC 6 and CC 38 as above. Notes 36-39, the knob presses, turn the
  page as your hand does.
- Mod wheel, pitch bend, aftertouch and other controllers are ignored.

### Program changes

Program change 0-4 presses scene key 0-4 (0 the blank scene): a recall, or in a save, copy or
delete mode the slot. 5 and above are ignored.

### Start and Stop

With transport following on, MIDI Start and Continue play the loop and Stop pauses it, so
a DAW's play button starts and stops the loop with the song. It's off at first; switch it on
on the [settings page](#settings-page) (D# of the lower octave), with `firmware/remote.py
transport on`, or `midi_transport 1` in `FRIZZ/frizz_master.txt`.
Start plays on from where the loop is, as Continue does: the loop doesn't jump to its start.
With the [clock source](#clock-source) on TRS or USB, Start and Stop count only from that
input; on Auto or internal, from both.

### MIDI out

From v0.12. FRIZZ sends its clock, so a drum machine, a sequencer or a synth's arpeggio plays
in time with it, and with the loop. The upper octave's D# on the [settings
page](#settings-page) switches it, each press the next: **off** (the default, dim), **TRS**
(the TRS MIDI output, orange), **TRS and USB** (white). It's kept as `midi_out` in
`FRIZZ/frizz_master.txt` (0 off, 1 TRS, 2 TRS and USB), and `firmware/remote.py out
off|trs|all` sets it too.

**The clock** (24 PPQN) runs all the time while MIDI out is on, at the tempo FRIZZ thinks in,
the beat on the settings page's G#:

- **With a loop:** the loop's tempo, at its speed, with the loop's start on beat 1. In reverse
  the clock keeps running, at the loop's speed; while the loop is paused, it runs on at the
  loop's tempo.
- **Without a loop, with a MIDI clock coming in:** that clock, passed on tick for tick, with
  the [clock factor](#midi-clock): at double a tick more halfway between two, at half every
  other one.
- **Neither:** the last tempo (tapped, from the last loop or from a clock; 120 BPM at
  power-on).

**Start, Stop and Continue** follow the loop:

- A new loop starting to play sends **Start**, so the gear starts on the loop's beat 1. If
  the gear already plays from a DAW's Start that FRIZZ passed on, no second Start: it plays
  on, and the loop takes over the clock.
- A pause sends **Stop**. PLAY sends **Song Position** (where the loop goes on, to the 16th)
  and then **Continue** on that 16th, so the gear goes on where the loop does. Gear that
  ignores Song Position goes on where it stopped, which is the same place, unless the loop
  moved while paused.
- Without a loop, Start, Continue and Stop that come in from the clock's input are passed on.
- Erasing the loop sends nothing: the gear plays on, to the clock that's coming in or the
  loop's tempo.

**Not back to the sender:** while FRIZZ passes a clock on, nothing goes back out of the input
it comes in on. A DAW over USB gets nothing back over USB, gear on the TRS jacks nothing out
of TRS. With a loop, FRIZZ leads, and its clock goes out of every port MIDI out is set to.

Nothing else goes out: no notes, no controllers, no MIDI thru.

- Gear that follows any clock by itself takes FRIZZ's tempo as soon as MIDI out is on, even
  while nothing plays. That's why it's off at first.
- Over USB, the clock goes out between FRIZZ's other work: it's less steady than the TRS
  output's (by up to a millisecond or so), and while FRIZZ writes the card (a scene saved)
  its ticks wait and come out in a bunch. For tight timing, use TRS.
- Gear with MIDI thru on that sends FRIZZ's clock back into its TRS input: with the clock
  source on Auto, FRIZZ follows its own clock coming back. Set the clock source to internal
  or USB, or switch the gear's thru off.

### Remote control

FRIZZ's own SysEx (`F0 7D 43 48 ...`, documented in `firmware/code/src/MidiControl.h`) presses
keys, turns knobs, sets the channel, transport following, the clock source and MIDI out, and holds the mode switch up or down (until power-off) on
every channel; its keys and knobs act as your hands do, on the settings page too, and over USB answers what the play
page shows, every LED, the processing load, and sends and receives scenes. `firmware/remote.py`
uses it from a Linux computer: `state`, `leds`, `load`, `settings`, `channel 16`, `transport on|off`, `source usb`, `out off|trs|all`, `switch up|down|hand`, `scene get 2 my.json`, `scene put 3
my.json`, and `play SCRIPT --cpu`, which plays a scenario of the virtual CHOMPI on the device
and reports the worst load.

## Bug reports

From v0.11 (this hold from v0.12). On the [settings page](#settings-page) (mode switch up), hold SHIFT (CHOMPI) and
press and hold the VOLUME knob for 2 seconds: FRIZZ writes everything you did since switching
on to the card, as `/FRIZZ/bug-1.txt` (the next one `bug-2.txt`, and so on): every key, knob detent and
mode-switch flip with its time, the MIDI clock's tempo, every MIDI message FRIZZ acted on, and
the scenes and compressor the card held at power-on. Do it right after something went wrong
(flip the switch up, hold, flip it back down) and send the file with your report:
FRIZZ's developers play it on a virtual CHOMPI on their computer, which then does what yours
did, key for key.

- In v0.11 it was SHIFT + press the transport knob on the play page; that now does
  nothing, so it can't be pressed by accident while playing.
- While it's written, both transport LEDs blink white; then they blink 3 times, white when
  it's on the card, red when it isn't (no card, or a full one). A long session takes a moment,
  and the keys answer late until it's done; the sound plays on.
- It doesn't hold the audio you played in, so a problem that depends on the sound itself
  needs a description too, nor scenes sent over MIDI. Dense MIDI automation fills it
  faster: each controller message is an event. It holds about 260,000 events, hours of playing; a session longer
  than that is cut off at the end.
- It doesn't send anything anywhere and costs nothing while you play.

## Power and battery

- **Low battery:** unplugged with the battery nearly empty, the knob, transport, VOLUME,
  CHOMPI, PLAY and LOOP LEDs flash yellow for 15 s, then the CHOMPI switches itself off. Plugging in power
  during those 15 s stops it.
  On a charger too weak to run it with a low battery, it goes dark until it has charged.
- **Battery check:** the [settings page](#settings-page) shows the battery's level on VOLUME's
  LED.
- **Shipping mode:** hold CHOMPI, PLAY and LOOP while switching on: the battery is
  disconnected until the CHOMPI is plugged into power again. Use it to store it for weeks.
- **Hardware test:** FRIZZ has no self-test of its own. To check keys, knobs and LEDs, put the
  stock TAPE or WAVE firmware on a card and hold VOLUME while switching on.
