# FRIZZ Manual

Every control in FRIZZ. For a first session, start with the [quick guide](QUICKSTART.md).

## Overview

FRIZZ turns the CHOMPI into a stereo effects box and looper for whatever you plug into its AUX
input, or for its built-in microphone. You record a loop and play it faster, slower or
backwards, punch effects in over it and over the live input on the white keys, and keep the
effects' settings as scenes, to recall, morph or drop to on the bar line. A master compressor
sits on the outputs, and MIDI over TRS and USB reaches every control.

### How to read this manual

- **SHIFT** means holding the CHOMPI key. The **play page** and the **settings page** are the
  mode switch's two positions, down and up; the knobs' **page 2** and VOLUME's pages are
  something else, picked with a press of the knob. [Playing basics](#playing-basics) has all
  three.
- **The keys** are named as on a keyboard: the white keys counted from the left, the 1st to the
  15th, and the dark keys by their note, C#, D#, F#, G# and A#, in the **lower** (left) or the
  **upper** (right) octave.
- The chapters go from the panel and the way the sound runs, through the looper, the tempo and
  one chapter per effect, to the scenes and the settings. [Key
  combinations](#key-combinations) has every gesture in one table, and the appendices the
  [MIDI](#appendix-midi) reference and the [technical data](#appendix-technical-data).

## Panel

![A schematic of the panel: the 15 white keys with the effects, the chaos key and the master
compressor; above them the 10 dark keys with the blank scene, scenes 1-4, save, copy and
delete; behind them the mode switch, CHOMPI, knobs 1-4, the transport knob, PLAY, LOOP and
VOLUME; and the jacks](images/panel.svg)

On the play page (the mode switch down), the controls are:

| Control | In FRIZZ |
|---|---|
| White keys 1-13 | The [punch-in effects](#punch-in-fx), in the order the sound goes through them: [Freezer](#freezer), [Shifter](#shifter), [Folder](#folder), [Crusher](#crusher), [Filter](#filter), [Flanger](#flanger), [Phaser](#phaser), [Resonator](#resonator), [Slicer](#slicer), [Wow & flutter](#wow--flutter), [Tape stop](#tape-stop), [Delay](#delay), [Reverb](#reverb) |
| White key 14 | The [Chaos key](#chaos-key) |
| White key 15, the last | The [Master compressor](#master-compressor) on knobs 1-4 |
| Lower C# and D# | Nothing, kept free |
| Lower F#, G#, A#, upper C#, D# | The [FX scenes](#fx-scenes): the blank scene, then scenes 1-4 |
| Upper F#, G#, A# | [Save, copy and delete](#save-copy-and-delete) a scene |
| Knobs 1-4 | The effect picked last, or the compressor; a press turns them to its [page 2](#page-2) |
| The transport knob, the big purple one | The loop's speed and direction, a scrub while it's paused ([Transport knob](#transport-knob)); with a scene morph held, the crossfader and the drop |
| VOLUME | Output gain, input gain and the headphone feed; with SHIFT, the input/loop mix ([VOLUME knob](#volume-knob)) |
| CHOMPI | SHIFT; it confirms a save, copy or delete |
| PLAY, LOOP | The [Looper](#looper); SHIFT + LOOP taps the [tempo](#tap-tempo) |
| The mode switch | Down the play page, up the [settings page](#settings-page) |
| AUX in | The stereo input; with no cable in it, the built-in microphone is the input ([Input: AUX or the mic](#input-aux-or-the-mic)) |
| The outputs | The master out and the headphones |
| MIDI in and out (TRS), USB | [MIDI](#appendix-midi), the clock and FRIZZ's [remote control](#remote-control) |
| The SD card | Scenes, settings and bug reports ([Card and files](#card-and-files)) |

On the settings page, the keys and knob 1 set other things: see [Settings page](#settings-page).

## Sound architecture

How the sound goes through FRIZZ, from the input to the outputs, and where the tempo comes
from.

### Signal flow

![The signal flow: the input after its gain to the looper and the input/loop mix, the mix
through the effects and the master compressor, a dry source joining after them, then VOLUME
and a safety limiter on the line out and on the headphones, whose feed can blend in the input
alone](images/signal-flow.svg)

The stereo AUX input, or the built-in microphone while no cable is in AUX (see
[below](#input-aux-or-the-mic)), goes to the headphone and master outputs
through a volume stage taken from TAPE's Volume Engine, with a looper on the wet side of a
dry/wet mix. The looper can record free-length loops or loops quantized to whole bars of an
incoming MIDI clock. The white keys punch in effects on the mixed signal, and four dark keys
in the middle save and recall their settings as scenes, next to one that clears them. A master
compressor, set from the last white key, sits after the effects.

After VOLUME, a fixed safety limiter keeps every output within full scale (see [Master
compressor](#master-compressor)). The master out always carries the whole signal; the
headphones carry the same, or the input on its own (see [VOLUME knob](#volume-knob)).

### Input: AUX or the mic

![The input: AUX or the mic, each with its own gain, picked by the upper F# or AUX's jack; the
source as it is to the looper and the headphone feed; the upper G# through the effects or dry,
the upper A# live on the outputs or not; the dry share joining after the compressor](images/input.svg)

FRIZZ hears CHOMPI's built-in microphone too, as TAPE and TEMPO do. The upper
octave's F#, G# and A# on the [settings page](#settings-page) set the input left to right, as
the signal goes: **the source** (F#), whether it goes **through the effects** (G#), and whether it's **live** on the outputs at all
(A#).

- **F#, the source:** AUX or the mic. AUX's jack picks: with a cable plugged in, it's AUX;
  pull it out and it's the mic; plug one in and it's AUX again. At power-on the jack decides
  the same way. F# picks the other by hand, until a cable next goes in or out of AUX. A jack
  change while recording just switches. The source isn't saved.
- **G#, the effects:** through them, through the input/loop mix, the effects and the master
  compressor; or **dry**, past the effects and the compressor, at the
  mix's input share, straight to VOLUME and the safety limiter. Lit green for through, white
  for dry; dark while the source isn't live, when it doesn't matter (pressed then, it blinks red
  3 times and changes nothing; its setting is kept for when the source is live again).
- **A#, live:** the source on the outputs or not, lit when it is. Not live, you can still
  record it and hear it on the headphones' input-only feed, without it on the master.

G# and A# are **each source's own**: they show and set the source heard now, and switching the
source (F# or the jack) brings up the other's. Both sources' are saved. Out of the box AUX is
live through the effects, and the mic is through the effects but **not live**: a
loudspeaker near CHOMPI would feed it back.

**The looper always records the source as it is**, whatever G# and A# say; the loop always
goes through the effects. The mix (SHIFT + VOLUME) and its automatism work for either source: it goes to
the loop alone when a recording finishes and back to the input alone when the loop is erased,
so a loop isn't heard twice; with the source not live, the mix's input share is silent. The
headphone feed (VOLUME's page 3) too: turned left, the source on its own.

What you hear, by the mix's position (SHIFT + VOLUME). The headphones carry the same as the
line out with the feed at the master, the source on its own with it turned left, a blend
between:

| F# source | G# effects | A# live | Line, mix at "input" (no loop, or after an erase) | Line, mix in the middle | Line, mix at "loop" (after a recording) | Headphones, feed left |
|---|---|---|---|---|---|---|
| AUX | through | on | AUX through the effects + compressor | AUX and the loop together through the effects + compressor | the loop through the effects + compressor | AUX pure |
| AUX | dry | on | AUX pure (VOLUME and the limiter only) | AUX pure + the loop through the effects | the loop through the effects | AUX pure |
| AUX | – | off | silence (delay/reverb tails at most) | the loop alone through the effects, at its share | the loop through the effects | AUX pure |
| mic | through | on ⚠ | the mic through the effects + compressor | the mic and the loop together through the effects | the loop through the effects | mic pure |
| mic | dry | on ⚠ | the mic pure | the mic pure + the loop through the effects | the loop through the effects | mic pure |
| mic | – | off | silence | the loop alone through the effects, at its share | the loop through the effects | mic pure |

- "–": G# doesn't matter while the source isn't live (its key dark, its value kept).
- The looper records the source as it is in every row.

**⚠ The mic live feeds back** through a loudspeaker near CHOMPI, through the effects more so
(a delay or a reverb repeats it). Switch it live with headphones, or with the speakers well
away and down. Mind the headphones' volume too: turned up near CHOMPI, they can feed back
into the mic as well.

**AUX dry** is for a source that should stay as it is while you play the loop through the
effects: a drum machine, a mastered song. Record it, and the mix goes to the loop alone as
always; turn the mix to the middle to play an existing loop through the effects next to the
device. **AUX not live** records AUX without hearing it on the master (on the headphones'
input-only feed you still do).

**The mic** is mono, the same on both sides. FRIZZ treats it as TAPE does: 5 times the input
gain (AUX gets 3 times), its DC taken out, and a highpass at about 150 Hz (TAPE's narrow
notches above it are left out: they did next to nothing). VOLUME's page 2 sets the input gain
of the source heard: the mic's is its own, kept apart from AUX's and saved. A switch of the
source, of G# or of A# fades over 5 ms, so it doesn't click; the source going out fades out of
the outputs while the one coming in fades in, so a source that isn't live is never heard, not
even during a switch.

Over MIDI, CC 84 picks the source, CC 116 sets its effects and CC 117 it live (see
[Controllers](#controllers)), without saving them.

### Effect chain

![The effects in the order FRIZZ computes them: the freezer, the shifter to the phaser inside
the resonator's feedback loop, the slicer, wow & flutter and the tape stop, the delay and
reverb as sends; then a dry input, the master compressor, VOLUME and the safety limiters; below
them every insert's page 2](images/fx-chain.svg)

The keys run in signal order, left to right:

```
freezer -> shifter -> folder -> crusher -> filter -> flanger -> phaser -> slicer -> wow & flutter
           |<------------------- resonator loop -------------------->|
  -> tape stop -> delay -> reverb
```

The inserts fill the first 11 white keys, the [phaser](#phaser) the 7th, next to the
flanger, and the resonator's loop the 8th; the delay and reverb are the 12th and 13th, the
14th is the [chaos key](#chaos-key), which makes no sound of its own but plays with the
others, and the 15th, the last, is the [master compressor](#master-compressor)'s.

- **Inserts** (freezer, shifter, folder, crusher, filter, flanger, phaser, slicer): replace
  the signal while on and stop the moment they're off. The freezer comes first, so it
  captures the clean sound and everything after it works on the repeats. The folder folds
  the clean signal and the crusher grinds the folds; the filter sweeps both, and the flanger
  and the phaser, the two sweeping modulations side by side, sweep what they made. The
  slicer chops everything, including the resonator's ringing.
- **Tape** (wow & flutter, tape stop): the last two inserts, so they bend everything before
  them, the resonator's ringing too. They come before the sends: a tape stop slows the music
  to a halt while the echoes and the reverb ring on. The tape stop doesn't end the moment
  it's off: it spins back up first.
- **Resonator** (8th white key): a comb feedback loop. While on, it taps the signal after the
  phaser and feeds it back in after the freezer, so it rings through the shifter, folder,
  crusher, filter, flanger and phaser whenever they're on.
- **Sends** (delay, reverb): the key opens the effect's input, and its output is added to the
  signal, so tails ring out after the key is released. The delay's tail rings out as plain
  echoes: its random events only start while its key is on. The delay gets the inserts' output;
  the reverb gets that plus the delay's echoes, so the echoes are reverberated while both
  are on.

### Tempo and MIDI

![Clock and MIDI: the clock source and the clock factor, the quantized recording, transport
following, the tempo from the loop, the MIDI clock or the tapped tempo driving the effects,
the morphs' bar lines, the beat LEDs and MIDI out, and MIDI feedback](images/clock-midi.svg)

The effects that follow a tempo, a quantized recording and MIDI out share one tempo: the
loop's while there is one, otherwise a MIDI clock from the [clock source](#clock-source),
otherwise the last tempo, tapped or held. The [clock factor](#clock-factor) multiplies it. See
[Tempo and transport](#tempo-and-transport).

MIDI comes in over TRS and USB alike: notes play the keys, controllers set the knobs, program
changes recall scenes, and a DAW's Start and Stop can play and pause the loop. MIDI out sends
FRIZZ's clock and the loop's Start, Stop and Continue, and with MIDI feedback what you change on
the panel. See [Appendix: MIDI](#appendix-midi).

## Card and files

FRIZZ keeps what it saves on the SD card, in a folder `FRIZZ`, as text files. Leave the card in.

| File | Holds | Written |
|---|---|---|
| `FRIZZ/frizz_scenes.txt` | The four [FX scenes](#fx-scenes) | When you save, copy or delete a scene |
| `FRIZZ/frizz_master.txt` | The [master compressor](#master-compressor) and the [settings page](#settings-page): the MIDI settings, the LEDs' brightness, AUX mono, and for AUX and the mic each their input gain, effects and live | 2 s after the last change |
| `FRIZZ/frizz_scenes.bak`, `frizz_master.bak` | The versions before | With each write of the file |
| `FRIZZ/bug-1.txt`, `bug-2.txt` … | [Bug reports](#bug-reports) | When you ask for one |

### Scenes and settings on the card

The scenes live in `FRIZZ/frizz_scenes.txt` on the card, one line per effect, keyed by its
name. FRIZZ creates the folder on its first start; after that the card is written only when
you save, copy or delete, never on a recall. Saving takes a moment in which the LEDs may pause. A scene file FRIZZ can't
read (from another version, or edited into something else) starts it with no scenes but
isn't lost: the first save moves it to `frizz_scenes.bak`.

If FRIZZ couldn't read the card at power-on (rare: it loads from the card, so the card is
there), every save flashes red until you switch off and on again, so the scenes on the card
aren't replaced by the session's.

`frizz_master.txt` holds one setting per line, which can be edited on a computer too:

| Line | Setting | Values |
|---|---|---|
| `midi_channel` | The global [MIDI channel](#channels) | 1-16, 0 every channel |
| `midi_block_trs` | [Effect channels over TRS](#channels) | 1 on, 0 off |
| `midi_transport` | [MIDI transport following](#start-and-stop) | 1 on, 0 off |
| `clock_factor` | [Clock factor](#clock-factor) | 100 as sent, 200 double, 50 half |
| `clock_source` | [Clock source](#clock-source) | 0 Auto, 1 TRS, 2 USB, 3 internal |
| `midi_out` | [MIDI out](#midi-out) | 0 off, 1 TRS, 2 TRS and USB |
| `midi_feedback` | [MIDI feedback](#midi-feedback) | 1 on, 0 off |
| `led_brightness` | [LED brightness](#led-brightness) | 25-100 |
| `mono` | [Mono input](#mono-input) | 1 mono, 0 stereo |
| `gain_aux`, `gain_mic` | Each source's input gain ([VOLUME knob](#volume-knob)) | 0-100 |
| `aux_dry`, `mic_dry` | Each source's effects ([Input: AUX or the mic](#input-aux-or-the-mic)) | 0 through them, 1 dry |
| `aux_live`, `mic_live` | Each source live | 1 on, 0 off |
| `compressor2` | The master compressor's knobs | |

A line FRIZZ doesn't know is skipped, and a setting left out keeps its default.

### Taking the card out

Take the card out or swap it only while the CHOMPI is off: a card pulled while FRIZZ runs
isn't looked after. To get files on or off it (scenes, bug reports), use the multi-firmware
launcher's USB storage mode instead of taking it out.

With the [multi-firmware launcher](INSTALL.md#with-the-multi-firmware-launcher), FRIZZ is one
firmware of several on the card: at power-on each key with a firmware lights up, and pressing
one starts it. FRIZZ keeps its `FRIZZ` folder apart from the other firmwares' files. The
launcher's key 15 is the USB storage mode: the card shows up on the computer as a drive, so you
can copy files (bug reports, scenes) without taking the card out. Leave it with overdub, then
the CHOMPI key.

### Files from an older FRIZZ

- A `frizz_scenes.txt` in the card's top folder, where older versions kept it, is moved into
  the `FRIZZ` folder at the first start.
- **Scenes from before page 2** load with page 2 on its defaults (the shifter's mix kept from a
  test build that had it), and with every value where its knob went: stereo to page 2's
  knob 2, the effect's own setting to page 1's knob 4, the delay's and reverb's level to page
  2's knob 4 at the same loudness in dB (one below -36 dB off) and their ducking to page 1's
  knob 4.
- A scene file from before the phaser or the chaos key loads with them off, on their defaults.
- **Compressor settings from before its Attack and Release:** the old Speed knob becomes both
  Attack and Release, Mix moves to page 2, and Makeup starts at 0 dB, so a compressor you had set comes back quieter
  than before, by what the automatic makeup added (half of what a 0 dB signal lost).
- The LEDs' brightness an older FRIZZ saved, 50 %, 75 % or 100 %, is kept.

## Playing basics

### SHIFT and the mode switch

**SHIFT** means holding the CHOMPI key. The CHOMPI key lights white while it acts as SHIFT.
The mode switch picks the page: **down** is the play page, which most of this manual is about; **up** is the
[settings page](#settings-page), for what you set once rather than play. If the keys don't
play, flip it down.

Holding CHOMPI while you use another key or knob is SHIFT; tapping it alone confirms a scene
[save, copy or delete](#save-copy-and-delete).

### VOLUME knob

| Control | Function | LED |
|---|---|---|
| Turn (page 1, default) | Output gain, headphone + master (default 75%) | VU meter of the master, scaled by gain: dim when silent, then green, yellow, pink at the loudest |
| Press, then turn (page 2) | Input gain of the input you hear, AUX or the mic: each has its own, saved (default 75%) | the input's meter: dim when silent, then green, yellow, **red where the loop would clip**; while you turn, the gain: blue (0%) to red (100%) |
| Press twice, then turn (page 3) | Headphone feed: left towards the input on its own, right back to the master out (default: the master out) | white (master) to green (input) |
| SHIFT + turn | Input/loop mix: input only to looper only | green (input) to purple (loop) |
| SHIFT + press | Resets the mix: the loop only while there is one, otherwise the input only | |

Turning moves the gains 2% per detent, the mix (SHIFT + turn) 4% and the headphone feed 1%.
The input gain is the one setting here that's saved: AUX and the mic each keep their own on the
card (2 s after the last turn), and switching the input picks its gain. The output gain, the
mix and the headphone feed start from their defaults at every power-on.

**The input's meter (page 2):** it shows what the looper records, after the input gain. It's
red where the loop's limiter takes the level down (just under full scale, see
[Looper](#looper)), held a moment so a short peak is seen; turn the gain down until it only
flashes red on the loudest hits. For a second after a turn it shows the gain instead, blue to
red.
Pressing again on page 3 returns to page 1.
Picking a page blinks its number in white: once for page 1, twice for 2, and so on. Whether a press
is SHIFT + press is decided when it goes down, so letting go of CHOMPI first doesn't also
change the page. The mix starts
on the input only, jumps to the loop only when a recording finishes and back to the input only
when the loop is erased; SHIFT + press puts it back there. The punch-in FX come after this mix,
so they act on the input as well as the loop.

**Headphone feed (page 3):** the master out always carries the full signal. Turned left, the
headphones carry the input (AUX or the mic) on its own instead, after input gain and VOLUME: no loop, no
effects, no mix, no compressor. It's where you hear the mic while it isn't live (see
[The input](#input-aux-or-the-mic)). Use it when the master goes to a PA, mixer or recorder and the
headphones are your monitor; with headphones alone, leave it at the master. It's back on the
master at every power-on.

Mono and the battery check are on the [settings page](#settings-page), the master compressor
on its own key (see [Master compressor](#master-compressor)).

### Pages

Some controls do more than one thing, each on a page of its own, picked with a press:
VOLUME's pages 1-3 (above), and knobs 1-4's [page 2](#page-2), the second set of the selected
effect's parameters. Picking a page blinks its number on the LEDs in white. These pages are
not the play page and the settings page, which the mode switch picks.

### LED colours

Every effect has its own colour, on its key and its knobs (see [Keys and knobs](#keys-and-knobs)).
Throughout FRIZZ, 3 quick red blinks mean refused or not stored, and 3 white ones done.
Besides the effects' own colours, red lit or blinking slowly means something else: LOOP
recording or waiting to erase, the compressor's key while the safety limiter works, the input gain near 100% (VOLUME's page
2), on the settings page the clock factor on double, and VOLUME in the low-battery countdown.

Waiting for something blinks slowly: LOOP while a recording finishes its bar, a picked scene
slot, the CHOMPI key ready to confirm. On the knobs, white is a parameter's neutral point.
The [LED brightness](#led-brightness) sets how bright all of them are.

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
| Any | SHIFT + LOOP | Tap tempo (see [Tap tempo](#tap-tempo)); never records, stops or erases |
| Loop exists | SHIFT + PLAY | As PLAY: play / pause. The [scene gestures](#timed-scene-changes) don't use PLAY |

- **Quantized:** recording starts on the press, which counts as bar 1 (4/4). Ending it records
  to the end of the bar in progress, so the loop is always a whole number of bars. It closes
  immediately if the clock stops.
- **Erasing** uses the gestures that record: LOOP at once, PLAY + LOOP quantized, here to
  the loop's own end, so it needs no clock. For half a second after LOOP stopped a recording,
  LOOP doesn't erase, so a double press can't lose the new loop.
- **Length:** up to 2:45. The loop lives in RAM and is gone at power-off. There's no overdub.
- **Level:** the loop doesn't clip. Live, a hot input is float until the safety limiter;
  the loop is stored at 16 bits, so what goes into it is held just under full scale by a
  limiter of its own, which takes a peak at once and lets go over about 150 ms. Below that it
  records exactly what came in. A source loud enough to reach it records a little quieter
  than it sounds live (from about 7 dB below the input's full scale at input gain 75%).

LEDs: LOOP is red while recording and blinks while a quantized recording finishes its bar or
an erase waits for the loop's end, at the slow blink everything waiting uses (a picked scene
slot, the CHOMPI key ready to confirm); a refusal is 3 quick blinks.
While a loop plays, PLAY and LOOP crossfade in white to show the position (dimmed when
paused).

PLAY also shows the beat the effects follow (see [Tempo and transport](#tempo-and-transport)), as the settings
page's G# does:

| Looper | PLAY |
|---|---|
| No loop | dim white for the first half of each beat |
| Recording | the same, a count while LOOP is red |
| Playing | the crossfade, with a short flash on each beat (whiter, or darker where PLAY is already bright), stronger on each bar's 1 |
| Paused | the dim position, the beat pulsing over it (the tempo runs on) |

LOOP's red blinks (a recording closing, an erase waiting, a refusal) show as above. The beat
dims with the [LED brightness](#led-brightness).

## Tempo and transport

The effects that follow a tempo (the delay, the filter LFO, the freezer, the slicer, the tape stop, the phaser's rate in its synced half, the chaos key's grid) take it from one of three places, the first that applies:

1. **The loop, while there is one**, with or without MIDI clock. Beat 1 is the loop's start,
   so the slicer chops on the loop's beat from the first pass and the delay's echoes land on
   it. The effects follow the transport knob: at half speed the tempo halves and in reverse
   the beats run backwards. While the loop is paused, the beat runs on by itself at the
   loop's tempo, so the effects keep working on the input; scrubbing doesn't move it. At a
   [standstill](#transport-knob) it does the same, at the slowest speed's tempo (50 BPM). When
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

### Tap tempo

**SHIFT + LOOP:** tap at least three times, at the tempo you want; the last five
taps (four gaps) count, and a pause of over 2 s starts over. LOOP flashes on each tap: white,
or dark when it's already lit nearly white.
- With a loop, the taps pick how many beats the loop holds; the loop's length then gives the
  exact tempo, so the effects' beats stay locked to it even if the taps were a little off,
  delay times and freezer lengths included. Tapping
  at double the loop's tempo, for example, doubles the effects' tempo.
- Without a loop, the taps set the tempo, and the last tap lands on a beat.
- Tap the tempo you hear: the [clock factor](#clock-factor) applies on top of it.
- Without a loop while MIDI clock runs, the clock is the tempo: LOOP blinks red 3 times.
  With the [clock source](#clock-source) on internal, no clock counts, and the taps set the
  tempo.

The tempo is limited to 50-300 BPM, after the clock factor: at very slow loop speeds the
effects stop slowing down at 50 BPM.

### Clock factor

The lower octave's G# on the [settings page](#settings-page) sets it, each press the next:
**as sent** (yellow, the default), **double** (red), **half** (light blue). It multiplies the
effects' tempo, whichever of the three above it comes from: tap the tempo of a song on AUX
(SHIFT + LOOP), then put the delay, the slicer and the rest at double or half of it; or
record a loop and let the effects run at half its tempo. The loop itself stays as it is: its
length, its beats, and the bars a quantized recording ends on. With MIDI clock, FRIZZ
follows the clock at the factor (see [MIDI clock](#midi-clock)), so a loop recorded to a
120 BPM clock at double has the effects at 240.

- Changed while a loop plays, the effects' beat stays on the loop: beat 1 on its start.
  Without a loop, a tapped beat stays on the beats you tapped: at double with one more
  halfway, at half on every other one.
- At half, a loop of an odd number of beats (three, say) holds a beat and a half per pass.
  The effects' beats run on evenly across its start, so beat 1 lands on the loop's start
  every other pass, and halfway through a beat on the others. A quantized loop always holds
  whole bars, so it's never one of them.
- The 50-300 BPM limit applies after the factor: 200 tapped at double is 300, at half 100.
- The beat on G# and PLAY and the clock [MIDI out](#midi-out) sends show the tempo with the
  factor.

It's kept as `clock_factor` in `FRIZZ/frizz_master.txt` (50, 100 or 200).

### Transport knob

The transport knob, the big purple one, works once a loop exists:

| Control | Function |
|---|---|
| Turn while playing | Speed in semitones (2 detents per semitone), as TAPE's knob runs: from 2× down to 1/16×, then a **standstill** for about 4 detents (the tape glides to a stop, 0×), then reverse from −1/16× up to −2×; the same way back. Stops at 2× and −2× |
| SHIFT + turn while playing | Speed in 5ths and octaves (4 detents per step): from between two of them, to the next one in the turn's direction. Past 1/16× it flips into reverse at the same speed and climbs back up to −2×; from a standstill, 1/16× the way you turn |
| Turn while paused | Scrub |
| SHIFT + turn while paused | Nothing (while you hold a scene morph, the [crossfader](#crossfading-by-hand)) |
| Press | Back to 1× forward |
| SHIFT + press | With a scene morph held: a [drop](#dropping-to-a-scene). Otherwise nothing (the [bug report](#bug-reports) is on the settings page) |

- **Standstill** (the transport knob between 1/16× and −1/16×): the tape glides to a stop
  and the loop fades out with it, a tape stop rather than a click, and stays silent while it
  stands. The effects' beat runs on by itself as while paused (see [Tempo and transport](#tempo-and-transport)), at the
  slowest speed's tempo, 50 BPM, so a scene morph still lands on its bar line. Turned on, the
  loop fades back in with the speed, and the beat snaps back onto it.

The transport LEDs show speed and direction while it plays (both red at a
standstill), the scrub speed in white while it's paused, and nothing without a loop.

With MIDI transport following on, a DAW's Start and Stop play and pause the loop too: see
[Start and Stop](#start-and-stop).

## Punch-in FX

Effects sit on the white keys and act on the whole mix, after the input/loop mix and before the
master compressor and the output gain. The looper records the input before the effects, so an effect is never
printed into a loop.

Inserts stop the moment they're off, the delay and reverb ring out: how each kind sits in
the chain is in [Effect chain](#effect-chain). Some heavy combinations of effects can need more
processing time than there is, heard as crackles (see [Processing
load](#processing-load)).

### Playing an effect

| Control | Function |
|---|---|
| Hold an FX key | Effect on while held |
| Hold an FX key, then SHIFT | Latch on / off; a latched effect stays on after release. The key goes down first, then SHIFT, a moment later (SHIFT within about 15 ms of the key counts as SHIFT first: a select). The latch is settled when you let go of the key: holding an FX key and using SHIFT for something else (a coarse turn, tap tempo, the mix, selecting another effect) doesn't latch it. Hold several FX keys, then SHIFT, to latch them all |
| SHIFT, then an FX key | **Select:** the knobs now edit that effect, without hearing it. It stays off (or latched, if it was), and letting go of the key does nothing, also after letting go of SHIFT first, even just before the key goes all the way down (up to about 15 ms). The key flashes (white, or dark while it's already nearly white). Only the key first, then SHIFT, latches |
| FX key on a latched effect | Clears the latch; the effect stays on until the key is released |
| Knobs 1-4 | The parameters of the most recently pressed or selected FX key, 1% per detent; stepped ones (shifter shift, filter LFO and delay divisions, freezer length and roll, slicer pattern and stereo, tape stop and spin-up times, the flanger's polarity, the chaos key's grid, the phaser's rate right of its centre) move one step per 3 detents |
| SHIFT + knobs 1-4 | Coarse: jumps to the next point of the parameter's grid per detent (see [Coarse steps](#coarse-steps)) |
| SHIFT + press knobs 1-4 | Resets that parameter to its default, on the page shown |
| Press knobs 1-4 | **Page 2:** all four knobs turn over to the effect's second page, and the page's number blinks on their LEDs, twice (page 1 once), as VOLUME's pages do; press any knob again for page 1. Pressing or selecting another effect, or the compressor's key, goes back to page 1; pressing the same effect's key, or the compressor's while it's selected, keeps page 2. Every effect but the chaos key has one, and the compressor too. See [Page 2](#page-2) |

### Keys and knobs

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
is white there, blue below and orange above, the same on every effect, so you can see at a
glance which way a knob is off. Every other knob goes from the effect's first colour to its
second. The phaser's rate shows which half it's in: its own colours while it runs free,
green to yellow once it follows the tempo. Page 2's knob 4 shows what the effect puts out
instead of its value (see [Page 2](#page-2)).

The knobs do the same kind of job on every effect, so you can find a setting without
looking it up:

| Knob | Role | Where an effect has none |
|---|---|---|
| 1 | The main control: length, pitch, drive, rate, cutoff, pattern, wow, stop time, division, decay | — |
| 2 | Feedback (the filter's resonance) | its second main control: shape, bits, decay, flutter, spin-up time, diffusion |
| 3 | Tone or colour: tone, roll, swoop, amount, LFO depth, chance, curve, random | — |
| 4 | A setting of the effect's own, the one you play: gate, grain, symmetry, XOR, LFO division, polarity, centre, env mod, shuffle, age; ducking on the delay and reverb | the tape stop's depth |

The chaos key makes no sound, so its knobs are its own: 1 the FX chance, 2 the scramble
chance, 3 the grid, 4 random to pattern.

Every insert starts silent or nearly so: the knob that brings it in (shifter shift, folder
drive, crusher rate and bits, filter cutoff, flanger amount, resonator feedback, wow and
flutter, phaser depth) starts at off, the others at a setting that sounds good, so one turn
brings the effect in. Three can't be silent: the freezer starts at 1 bar, so a shorter press
only hears the live signal; the slicer at every step on with the longest decay, a slight
pump; and the tape stop stops on every press, over 1/2 bar. The delay and reverb are heard
from the first press: their key is what sends sound into them, and their level (page 2's
knob 4) starts at 0 dB, as every effect's.
SHIFT + press on a knob takes it back there.

Each effect has its own chapter after this one, with its knobs on both pages and its details:
[Freezer](#freezer), [Shifter](#shifter), [Folder](#folder), [Crusher](#crusher),
[Filter](#filter), [Flanger](#flanger), [Phaser](#phaser), [Resonator](#resonator),
[Slicer](#slicer), [Wow & flutter](#wow--flutter), [Tape stop](#tape-stop),
[Delay](#delay), [Reverb](#reverb) and the [Chaos key](#chaos-key).

### Page 2

A press on any of knobs 1-4 turns all four over to a second page of the selected effect's
(or the compressor's) parameters, the ones you set and leave rather than play; another
press turns them back. Turning the page blinks its number on the four knob LEDs in white,
as VOLUME's pages do: twice for page 2, once for page 1 (a turn cuts it short). A knob the
effect has nothing on is dark and does nothing. Every effect but the chaos key has a page 2. Selecting another
effect or the compressor
always brings back page 1, so the main controls are under your fingers whenever you pick an
effect; pressing the key of the one already selected keeps its page. SHIFT + turn and SHIFT + press work on page 2 as on page 1. Turning the page doesn't
edit the scene, and scenes, recalls and morphs carry page 2's values along with page 1's.

Page 2 is laid out the same on every effect that has one, so your hand learns it once:

| Knob | Page 2 | Default |
|---|---|---|
| 1 | **Mix:** the dry sound against the effect's, for parallel distortion, a harmony under the shifter, a half-frozen loop. On the delay and reverb: **Freeze** | fully the effect; freeze off |
| 2 | **Stereo:** how far the right channel parts from the left, each effect its own way (in its section). The tape stop, delay and reverb, which have no stereo, have a setting of their own there: darken, damping, pre-delay | off |
| 3 | **Band:** which part of the sound the effect works on. In the middle, all of it. Turned left, only the lows, below a split that falls from 20 kHz to 40 Hz; turned right, only the highs, above one that rises from 20 Hz to 8 kHz. The rest passes dry, so a folder can grind the highs and leave the bass clean. On the delay and reverb it filters what goes in: right a low cut, left a high cut | the middle, all of it |
| 4 | **Level:** the effect's output, not the dry sound Mix blends in: off at the bottom, then -36 dB to +12 dB. Its LED is a meter of the effect's output (below). On the delay and reverb, the level of what comes back: the echoes, the room | 0 dB, at 3/4 |

**Knob 4's LED on page 2 is a VU meter** of what the effect puts out, in the colours of
VOLUME's meter: from dim through green and yellow to pink at full scale (-30 dBFS up), and as
bright as Level turns it, the way VOLUME's LED is as bright as VOLUME. So page 2 shows itself
whenever the effect sounds, and you see what Level does. It's measured after Level and Mix,
so at less than full Mix it shows some of the dry sound too. An effect that's off shows only
the dim floor. On the delay and reverb it shows their return (the echoes, the room, their
tails too, after the key is let go). The compressor's page 2 shows its
Makeup's value instead, as its other knobs show theirs: its key already shows how hard it works.

- **Nothing evens out levels by itself.** An effect that adds level, like the folder driven
  or the crusher's XOR on a quiet sound, comes out louder, and Level is how you set it.
  Level and Mix fade in and out with the key, as the effect does; with the key off, the
  sound passes untouched whatever page 2 says. The tape stop keeps them on while it spins up
  after you let go. The delay's and reverb's key fades what goes into them instead, and their
  Level stays, so their tails ring out at it.
- **At the defaults** an effect sounds exactly as without page 2.

### Coarse steps

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
| Phaser rate | Left of the centre the knob's 10% steps (0.02, 0.07, 0.4, 1 and 14 Hz), then each division, one per detent: from the free half up, the first stop is 1/16 on the centre; from the synced half down, past 1/16 onto the free half's 10% |
| Phaser centre | Octaves: 100, 200, 400, 800 Hz … 6.4 kHz |
| Everything else, filter cutoff included | 10% steps |

## Freezer

The 1st white key: Kastle 2 FX Wizard's freezer, as a beat repeat.

| Knob | Page 1 | Page 2 |
|---|---|---|
| 1 | Length: 1/16, 1/8T, 1/8, 1/4T, 1/4, 1/2T, 1/2, 1 bar (default 1 bar) | Mix |
| 2 | Feedback: the input overdubbed into the repeats (default 0, pure repeat) | Stereo: the left loop up to 45 ms longer (default off) |
| 3 | Roll: the length divided, off, 1/2, 1/4, 1/8, 1/16, no shorter than 1/64 bar; one step per 3 detents (default off) | Band |
| 4 | Gate: how much of each repeat you hear, all of it down to its first eighth, a stutter (default all) | Level |

- **Capture:** pressing the key waits for the next 16th, then records. The first pass is the
  live signal, so there's no gap; after one length it repeats. Releasing the key goes back to
  the live signal. Pressed again before the repeats have faded, they play on until the next
  16th starts the new capture. It keeps recording past the loop (up to 5 s), so the length can be turned
  up while repeating; turned past what's recorded, it plays on through the recording until
  the length is reached.
- **Feedback:** at 0 the loop repeats unchanged. Turning up mixes the input into it (up to
  30% at 75%, 80% at the top, where the loop also fades by 10% per pass).
- **Roll:** a beat repeat's build-up, played by hand, which Kastle doesn't have. Each step
  right halves the repeat: 1/2, 1/4, 1/8 and 1/16 of the length knob 1 sets, never shorter
  than 1/64 bar (a triplet's stops a halving before). A step takes at the start of the next
  repeat, so the repeats stay on the grid, and each repeats the start of the capture,
  following on from the last repeat through the seam crossfade. Turned back to off, the next
  repeat is the whole length again, in one move. Where the knob is is the roll: nothing
  counts along behind it, and a capture with the roll still on 1/4 repeats a quarter of the
  length from the start. Set the length to 1/4, press the key, then turn the roll right a
  step at a time (1/8, 1/16, 1/32, 1/64) as the build-up; let go of the key, or turn the roll
  back to off.
- **Gate** silences the rest of each repeat, faded in and out over 2 ms: turned up, the
  repeats turn into a stutter.
- Lengths follow the delay's tempo. The loop seam has a 5 ms crossfade (shorter on loops
  under 20 ms), which Kastle doesn't have.
- LEDs: the key is purple; the knobs go purple to light blue.

## Shifter

The 2nd white key: a two-tap pitch shifter, with Kastle 2 FX Wizard's swoop and feedback.

| Knob | Page 1 | Page 2 |
|---|---|---|
| 1 | Shift in semitones, -12 to +12, one step per 3 detents; centre off (default 0, off) | Mix: the dry sound under the shifted one, a harmony |
| 2 | Feedback: the shifted sound spirals (default off) | Stereo: the right channel up to a semitone higher; SHIFT + turn in quarter semitones (default off) |
| 3 | Swoop: on the key press, the shift pushes up to 2 octaves further and falls back over 1 s (default off) | Band |
| 4 | Grain: the window the shifter reads in, 10 ms (glitchy) through 30 ms (the centre, the default) to 50 ms (smooth) | Level |

- Two taps read the input at the shifted speed, each for a grain's stretch before it starts
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
- LEDs: the key is red; the shift and the grain go blue (down) through white (the centre) to
  orange (up), as every knob with a centre does, feedback and swoop blue to red.

## Folder

The 3rd white key: a sine-to-triangle wavefolder, antialiased.

| Knob | Page 1 | Page 2 |
|---|---|---|
| 1 | Drive: 1x up to 32x, from barely folding to many folds (default 1x) | Mix |
| 2 | Shape: sine (smooth) to triangle (bright) (default sine) | Stereo: the right channel driven up to 2x harder (default off) |
| 3 | Tone, lowpass 200 Hz to open (default open) | Band |
| 4 | Symmetry: a bias for uneven folds and even harmonics (default off) | Level |

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

## Crusher

The 4th white key: TEMPO's sample-rate reducer plus bit reduction, with Kastle 2 FX
Wizard's XOR and dive.

| Knob | Page 1 | Page 2 |
|---|---|---|
| 1 | Rate, 21.6 kHz down to 480 Hz, halving every 18% of the knob (default 21.6 kHz) | Mix |
| 2 | Bits, 16 down to 2 (default 16 bits) | Stereo: the right channel's rate up to an octave lower (default off) |
| 3 | Tone, lowpass 200 Hz to open (default open) | Band |
| 4 | XOR: flips bits of every sample for a digital buzz (default off) | Level |

- **Dive:** every press of the key drops the rate up to 10x over 0.1 s and lets it recover
  over 0.4 s, Kastle's trigger dive.
- **XOR** flips fixed bits of each sample as 16-bit (Kastle's constants, up to 4000), a buzz
  that's loudest where the signal crosses zero. On its own XOR would turn silence into a
  constant offset, so what it adds is DC-blocked.
- **Level:** the XOR's flips and the coarsest bits are a fixed size whatever the signal's
  level, so on a quiet sound they come out louder than it (XOR at full on a sound 40 dB down:
  about 22 dB louder). Nothing turns that down by itself: page 2's Level and Mix are there
  for it.
- LEDs: the key is orange; the knobs go yellow to red.

## Filter

The 5th white key: the DJ filter from TAPE, TEMPO and WAVE (WAVE's copy), with an LFO.

| Knob | Page 1 | Page 2 |
|---|---|---|
| 1 | Cutoff: lowpass left of centre, highpass right, flat at centre (default centre, flat) | Mix |
| 2 | Resonance (default 50%) | Stereo: the right channel's LFO up to half a cycle behind, so the sweep moves across (default off; only with the LFO) |
| 3 | LFO depth (default off) | Band |
| 4 | LFO division: 1/16, 1/8, 1/4, 1/2, 1 bar, 2 bars, 4 bars (default 1 bar) | Level |

- **LFO:** a triangle on the cutoff, like WAVE's filter LFO but synced to the same tempo as
  the delay. At full depth it sweeps half the cutoff knob either way, so from the centre it
  goes all the way from lowpass to highpass. It's at the centre of the cutoff on the beat and
  rises towards highpass first. Like the delay's events, the beat is counted from the loop's
  start, or without a loop from when the clock locked, not from the DAW's beat 1.
- LEDs: the key is pink; the cutoff goes blue through white (flat) to orange, the other knobs
  pink to light blue.

## Flanger

The 6th white key: Kastle 2 FX Wizard's flanger.

| Knob | Page 1 | Page 2 |
|---|---|---|
| 1 | Rate, 0.02 Hz to 50 Hz (default 0.55 Hz) | — (its Amount is its mix) |
| 2 | Feedback, up to 85% (default 42.5%, the knob at half) | Stereo: the right LFO runs free and detuned (default off) |
| 3 | Amount: sweep depth and mix together, the top is pure vibrato (default 0, dry) | Band |
| 4 | Polarity: the feedback positive (metallic, the default) or negative (hollow), two steps | Level |

- A ~12 ms delay swept up to its full length either way by a triangle LFO. Like Kastle's
  Amount, one knob sets both the depth and the mix: around the middle it flanges and
  chorus-sweeps, at the top it's all delayed signal, a vibrato.
- Pressing the key restarts the sweep from the centre (Kastle's trigger input).
- **Feedback** goes through the swept delay, a classic flanger's resonance. Kastle's FEEDBACK
  is a short comb around every mode, at most 8% on the flanger, so this one is stronger.
- LEDs: the key is light blue; the knobs go light blue to purple.

## Phaser

The 7th white key, next to the flanger, after the SP-404MK2's Phaser and the Elektron
Tonverk's Phase 98, but FRIZZ's own: six allpass stages in a row on each channel, the sound added to them, and
feedback from the end of the chain back into it. Six stages cut three notches into the
sound, the middle one on the sweep's frequency, one about two octaves below it and one about
two above; a triangle LFO sweeps them up and down in octaves around the centre (knob 4).

| Knob | Page 1 | Page 2 |
|---|---|---|
| 1 | Rate, in two halves: left of the centre free, 0.02 Hz to 50 Hz; from the centre right on the tempo, 1/16, 1/8, 1/4, 1/2, 1 bar, 2 bars, 4 bars (default 0.7 Hz, free) | Mix |
| 2 | Feedback: the resonance between the notches, up to 85% (default 34%, the knob at 40%) | Stereo: the right channel's sweep up to half a cycle behind (default off) |
| 3 | Depth: brings the notches in and widens the sweep, up to 2.5 octaves either way of the centre (default 0, dry) | Band |
| 4 | Centre: the frequency the sweep moves around, 100 Hz to 6.4 kHz in octaves (default 800 Hz, in the middle) | Level |

- **Rate (knob 1), two halves on one knob.** Left of the centre it runs free, from 0.02 Hz
  (a sweep a minute) to 50 Hz (a buzz), on the flanger's curve. From the centre right it
  follows the [tempo](#tempo-and-transport), as the filter's LFO does: 1/16 on the centre, then 1/8, 1/4,
  1/2, a bar, 2 bars and 4 bars at the top, a cycle each. So turning right goes faster in
  the free half, and from the centre on, slower again, through the divisions; the two halves
  meet where both are fast. The knob's LED shows the half: the phaser's colours (violet to
  orange) while free, green on 1/16 to yellow on 4 bars once synced.
  - Turning it by hand: 1% a detent in the free half, a division per 3 detents in the synced
    half. Turning up out of the free half stops on 1/16 first; turning down from 1/16 takes 3
    detents back into the free half, just below the centre.
  - SHIFT + turn: 10% steps in the free half, a division a detent in the synced half,
    across the centre both ways.
  - Synced, the sweep is at the centre on the beat and rises first, like the filter's LFO,
    its beat counted from the loop's start (or from when the clock locked).
- **Feedback (knob 2)** sends the chain's output back into it: the notches sharpen and the
  peaks between them ring, up to 85%. The peaks get louder with it; it's turned down a
  little to make up.
- **Depth (knob 3)** brings the phaser in: at 0 the sound is dry, whatever the other knobs
  say. Over the first fifth of the knob the notches fade in, and from there on it only widens
  the sweep, up to 2.5 octaves either way of the centre at the top (from 800 Hz: 140 Hz to
  4.5 kHz). A morph fades the phaser in and out on it.
- **Centre (knob 4):** where the sweep is centred; SHIFT + turn moves it in octaves.
- **Stereo (page 2's knob 2):** the right channel's sweep lags the left's, up to half a
  cycle, so at the top one channel's notches go up while the other's come down.
- **In the resonator's loop:** with the resonator on, its ringing goes through the phaser on
  every trip, so the sweep moves through the resonances.
- **Level:** a phaser takes some of the sound into its notches, about 3 dB of a full mix,
  more with feedback up; page 2's Level is there for it.
- Over MIDI it's channel 7 (CC 14-21 its knobs, CC 22 its latch), and CC 108 latches it on the
  global channel (see [Controllers](#controllers)). Its key's note (B, 59) holds it as any
  key's does.
- LEDs: the key is violet; the knobs go violet to orange, the rate's synced half green to
  yellow.

## Resonator

The 8th white key: the comb Kastle 2 FX Wizard runs around every mode, as a loop of its own.
While on, it taps the signal after the phaser and feeds it back in after the freezer.

| Knob | Page 1 | Page 2 |
|---|---|---|
| 1 | Pitch, 22 Hz to 880 Hz (default 110 Hz) | — |
| 2 | Feedback, up to 98% (default 0, off) | Stereo: the right channel up to 12 semitones higher (default off) |
| 3 | Tone: the loop's lowpass, 1 kHz to 15 kHz (default 6.6 kHz) | Band: what goes into the ringing loop |
| 4 | Env mod: the feedback rises with the input's level, so loud notes ring longer (the SP-404MK2's ENV MOD; default off) | Level of what it rings |

- The loop holds a soft clipper, a lowpass (Tone) and a 50 Hz highpass, as on Kastle, so it
  saturates rather than runs away, whatever is inside it. On its own it's a comb on the dry
  sound; with the inserts inside the loop on, the ringing goes through them on every trip:
  through the shifter it spirals, through the crusher it turns grainy, the filter shapes
  it the way a filter in a dub delay's feedback does, and the phaser sweeps through it. With the freezer on, it rings the
  repeats; with the slicer on, the ringing is chopped into bursts.
- **Feedback inside the loop:** an insert in the loop with feedback or resonance of its own
  (the phaser, the flanger, the filter's resonance) raises the loop's gain. The phaser's
  peaks between its notches are about +4 dB at its default feedback and +16 dB at 85%, so
  with it on, the ringing runs away (and the soft clipper holds it) already at a low
  resonator feedback. Nothing evens that out by itself: for less, turn the inserts' feedback
  or the resonator's down.
- Like Kastle, it turns the input down as feedback goes up (by half at the most). Kastle's
  comb is fixed per mode, 22-440 Hz with about 40% feedback at most; this one is tunable
  and stronger.
- LEDs: the key is lime; the knobs go orange to light blue.

## Slicer

The 9th white key: Kastle 2 FX Wizard's rhythmic gate.

| Knob | Page 1 | Page 2 |
|---|---|---|
| 1 | Pattern, 8 steps of 16ths: `x.......`, `x...x...`, `..x...x.`, `x....x..`, `x..x..x.`, `x.x.x.x.`, `x.x.xx..`, `xxxxxxxx` (default `xxxxxxxx`) | Mix: at less than full, closed steps duck instead of mute (the SP-404MK2's DEPTH) |
| 2 | Decay, 10 ms to 1 s (default 1 s) | Stereo: the left channel plays a pattern up the list, the right one down, 0-7 apart (default off) |
| 3 | Chance: each step flipped at random, up to 90% (default off) | Band |
| 4 | Shuffle: the even steps late, up to two thirds of a 16th, a triplet swing (default straight) | Level |

- **Steps** are 16ths, counted from the [tempo](#tempo-and-transport) like the filter LFO, so a pattern is half a
  bar. Each step that's on starts the envelope again: a 10 ms attack, then the decay (knob 2).
- **Pressing the key** starts the envelope again too, at once, the same 10 ms attack and
  decay, whatever step the pattern is on: the sound comes in on the press, and doesn't drop out
  before the next step that's on.
- LEDs: the key is yellow; the knobs go yellow to green.

## Wow & flutter

The 10th white key: TAPE's warble, plus flutter.

| Knob | Page 1 | Page 2 |
|---|---|---|
| 1 | Wow: TAPE's warble knob, how often the pitch drifts and how much of it you hear, together (default off) | Mix |
| 2 | Flutter: a fast, shallow wobble, up to 1.4% of pitch; real tape's 0.1-0.5% is the lower half (default off) | Stereo: the right channel drifts on its own and wobbles out of step (default off) |
| 3 | Tone, lowpass 200 Hz to open (default open) | Band |
| 4 | Age: worn tape's dropouts, the level dipping at random, more often, deeper and longer as you turn it up (default none) | Level |

- **Wow** is TAPE's warble, from its SHIFT menu: a short delay (2-20 ms) that now and then
  heads for a new length at a random speed, so the pitch drifts like a worn tape's. The knob
  sets both how often (0.1 to 30 times a second) and the mix: around the middle the drifting
  copy blends with the dry sound into a chorus, at the top you hear only the drift.
- **Flutter** is two fast wobbles, at 7.3 Hz and 11.7 Hz, like a capstan's and a pinch
  roller's. It brings the effect in over the first quarter of its knob, so it works without
  the wow.
- **Tone** darkens what comes off the tape, not the dry sound.
- LEDs: the key is teal; the knobs go teal to purple.

## Tape stop

The 11th white key. It has no stereo, so its knob 4 is its depth and page 2's knob 2 its own.

| Knob | Page 1 | Page 2 |
|---|---|---|
| 1 | Stop time: 1/16, 1/8, 1/4, 1/2, 1 bar, 2 bars (default 1/2) | Mix |
| 2 | Spin-up time on release: off, 1/16, 1/8, 1/4, 1/2, 1 bar (default 1/4) | Darken: the highs fall away as the tape slows (default off) |
| 3 | Curve: linear to a brake, fast at first then dragging (default linear) | Band |
| 4 | Depth: how far it slows, from a full stop at the top (default) down to half speed (the SP-404MK2 Stopper's DEPTH) | Level |

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
- The times follow the [tempo](#tempo-and-transport), taken when the stop or spin-up starts, so a stop
  pressed on a beat lands on a beat.
- It comes before the delay and reverb, so their tails ring on while the tape stops. A
  latched tape stop, or a scene that latches one, keeps the whole mix silent, and the sends
  get nothing new, until it's unlatched. Page 2's Mix and Level stay on while it spins up
  after you let go.
- LEDs: the key is amber; the knobs go amber to red.

## Delay

The 12th white key: TEMPO's tempo-synced delay, a send.

| Knob | Page 1 | Page 2 |
|---|---|---|
| 1 | Division: 1/8, 1/4T, 1/4, 1/2T, 1/4., 1/2, 1/2., 1 bar, 2 bars (default 1/4) | Freeze: the input shut and the echoes held as they are, by degrees; no random events while frozen (default off) |
| 2 | Feedback (default 40%) | Damping: in the middle none; right, every repeat darker (a high cut); left, every repeat thinner (a low cut) |
| 3 | Random: left of centre retrigger / reverse / pitch events, right octave-up shimmer with random pan, centre off (default off) | Band: what goes in, right a low cut, left a high cut |
| 4 | Ducking: the echoes duck under what you play, back up when it stops (default off) | Level: the echoes' level, off at the bottom, then -36 dB to +12 dB (default 0 dB, at 3/4) |

- **Tempo:** the [tempo](#tempo-and-transport): with a loop, its exact tempo, so the echoes stay on its
  beats; otherwise rounded to whole BPM. Limited to 50-300 BPM so 2 bars fit the 10 s buffer.
- **Stereo:** the left echoes come 20 ms after their beat, the right ones 10 ms, which spreads
  them. The spread is only on what comes out, not fed back, so every repeat is as late as the
  first and the echoes stay on the beats, however long they ring.
- **Random events** are rolled on every 8th note; the knob's distance from centre is the
  chance. (TEMPO rolled them on its arpeggiator's step instead.)
- **Beat phase:** the 8th notes that random events follow are counted from the loop's start.
  Without a loop, only the clock's tempo is used, not MIDI Start / Song Position, so they're
  counted from when the clock locked (or from power-on, or the last tap, without clock), not
  from the DAW's beat 1. Echo spacing is unaffected.
- LEDs: the key is green; division green (short) to light blue (long); random and damping
  blue through white (off) to orange, as every knob with a centre. On page 2, knob 4's LED
  shows the echoes coming back (see [Page 2](#page-2)).

## Reverb

The 13th white key: TEMPO's and WAVE's reverb, a send.

| Knob | Page 1 | Page 2 |
|---|---|---|
| 1 | Decay (default 60%) | Freeze: the input shut and the room held (default off) |
| 2 | Diffusion (default 60%) | Pre-delay: up to 250 ms before the room starts (default none) |
| 3 | Tone, dark to open (default 60%) | Band: what goes in, right a low cut, left a high cut |
| 4 | Ducking: the room ducks under what you play (default off) | Level: the room's level, off at the bottom, then -36 dB to +12 dB (default 0 dB, at 3/4) |

- It gets the inserts' output and the delay's echoes, so the echoes are reverberated while
  both are on.
- LEDs: the key is blue; the knobs go light blue to purple. On page 2, knob 4's LED shows
  the room coming back (see [Page 2](#page-2)).

## Chaos key

The 14th white key, after the Red Panda Tensor's RAND and the Chase Bliss Blooper's
Scrambler. It works like any effect's key (hold to play, hold + SHIFT to latch, part of
scenes and morphs), but makes no sound of its own. It plays with what's there, on its grid,
in two ways. It has no page 2: a knob press does nothing.

| Knob | Page 1 |
|---|---|
| 1 | FX chance: how often each latched effect drops out of a step, from never to half the steps (default off) |
| 2 | Scramble chance: how often a step of the loop plays from elsewhere in it, from never to every step (default off) |
| 3 | Grid: 1/16, 1/8, 1/4, 1/2, 1 bar, on the effects' tempo (default 1/8) |
| 4 | Random to pattern: left, every step rolled anew; right, a bar rolled once and repeated (default random) |

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
- A morph fades it in and out on its two chances.
- Effects that react to coming on do so each time chaos lets them back in: the freezer
  grabs, the tape stop stops, the slicer attacks. A latched tape stop under chaos makes
  random stops.
- Over MIDI it's channel 14 (CC 14-17 its knobs, CC 22 its latch), and CC 115 latches it on
  the global channel (see [Controllers](#controllers)).
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

A scene holds every effect's four knob values and whether it's latched. Five dark keys in the
middle are scene keys: F#, G# and A# of the lower octave, C# and D# of the upper one.

- **The first (lower F#) is the blank scene:** every effect off and every knob on its
  default. It's always there, a one-press way back to no effects. You can't save over it or
  delete it, but you can copy it into a slot to start a scene from scratch.
- **The other four (lower G#, lower A#, upper C#, upper D#) hold your scenes 1-4.** They're
  saved to the SD card and come back at power-on.

The lower octave's C# and D# do nothing on the play page; they're kept free for pages to come.

A saved scene stays as you saved it: changes you make after recalling one are only kept if you
save again.

| Control | Function |
|---|---|
| Scene key | Recall the scene at once: every effect's knobs jump to it, the effects it latched come on and all others are unlatched. FX keys you're holding stay on |
| Scene key of the active scene | Back to the scene as saved, dropping your changes |
| Scene key of an empty slot | Nothing (the key blinks red) |
| Blank scene key | Every effect off, every knob back on its default |
| SHIFT + scene key, let go | **Morph** to the scene, landing at the end of the bar you let go of SHIFT in (see [below](#timed-scene-changes)) |
| SHIFT + the same scene key again (SHIFT still held) | One bar more, up to 8 (past that, the key blinks red) |
| SHIFT + scene key, then press the transport knob (SHIFT still held) | **Drop** to the scene hard on the bar line |
| SHIFT + scene key, then turn the transport knob (SHIFT still held) | **Crossfade** to the scene by hand |
| SHIFT + scene key A, then scene key B (SHIFT still held) | **Build-up + drop:** the morph to A, B dropping hard at its end |
| SHIFT + a blinking scene key (a new SHIFT hold) | Stop the morph, drop or build-up where it is |
| SHIFT + another scene key (a new SHIFT hold), while one runs | Stop it where it is and start the new one from there |

A recall is meant for performing, a build-up on one scene and the drop on the next:

- **The knobs land within about 25 ms** instead of gliding over 100 ms like a turned knob.
  Stepped values (freezer length, slicer pattern, delay division …) switch at once. The
  flanger's stereo can take up to a tenth of a second: its right channel bends back onto
  the left one rather than jumping there, which would click.
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

### Save, copy and delete

Save, copy and delete are on the last three dark keys, no SHIFT needed. They work like TAPE's
and TEMPO's preset keys, but in the opposite order and their own colours: save first, delete
on the outside, in red as a warning.

| Key | Colour | Function |
|---|---|---|
| F# of the upper octave | green | **Save** the current effect settings into a slot |
| Upper G# | blue | **Copy** one slot into another, without changing the sound |
| Upper A# (the last dark key) | red | **Delete** a slot |

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
it's done, red that it was refused (a dark slot tapped) or not stored. In delete, whose own
colour is red, a refusal is told apart by its rhythm: 3 quick blinks, where the picked slot
and the CHOMPI key blink steadily. The slot flashes red
instead of white when there's no SD card, or the card couldn't be written: the change works
until power-off but isn't stored. The next save, copy or delete tries the card again, also
one you've put in since.

Outside a function, the scene keys show the slots: dark when empty, dimly white when saved
(the blank one always is),
bright for the scene you recalled or saved last, and pulsing once you've turned a knob or
changed a latch since. The save, copy and delete keys are dimly lit in their colour.

### Timed scene changes

Everything that changes scenes in time with the music is one gesture, in one SHIFT hold:
**SHIFT + a scene key, tapped once per bar** (1 to 8 bars), and then, still holding SHIFT:

| With SHIFT still held | Result |
|---|---|
| nothing: let go of SHIFT | **[Morph](#morphing-to-a-scene)**, gliding over the bars |
| **press** the transport knob | **[Drop](#dropping-to-a-scene)**: the scene lands hard on the last of those bar lines |
| **turn** the transport knob | **[Crossfader](#crossfading-by-hand)**, the morph in your hand |
| a second scene key B | **[Build-up + drop](#dropping-to-a-scene)**: the morph to the first scene over the bars, and B dropping hard at its end |

- **The bars count from letting go of SHIFT**, for a drop too: hold it as long as you like
  and let go in time with the music. Only the scene taps count; pressing the transport knob
  again does nothing.
- **Whatever comes first holds** until you let go of SHIFT: once you've pressed the transport
  knob, turning it does nothing, and the other way round. The crossfader takes over only
  after 2 detents in one direction, the second at least 150 ms old with no press of the knob
  meanwhile, so the knob moving under your finger as you press it doesn't count.
- **One at a time.** A morph, a drop or a build-up runs until it lands; nothing joins it or
  adds to it in a later SHIFT hold:
  - **SHIFT + a blinking scene key** (the one it goes to, or the one it drops to) stops the
    whole sequence where it is: the knobs stay where the glide got to, the scene key pulses
    as edited, and a drop still waiting doesn't happen.
  - **SHIFT + another scene key** stops it where it is and starts a new one from there.
  - **A plain scene key** recalls that scene at once and ends it.
- PLAY isn't part of it: PLAY and SHIFT + PLAY only play and pause.
- Over MIDI, CC 3 and CC 83 follow the same rules (see [Controllers](#controllers)).

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
- **An effect the scene turns on fades in:** it comes on when the glide starts with the knobs
  that fade it at their defaults (below), and they glide up to the scene's values. Its other
  knobs take the scene's values at once.
- **An effect the scene turns off fades out:** the same knobs glide back to their defaults (the
  delay's and reverb's Level to off, so their tails fade with it), and on the bar line the
  effect goes off. It stays faded out and takes the scene's settings when its key next comes
  on.
- **The knobs that fade an effect** are the one that brings it in, and on most also page 2's
  Level (from and to 0 dB):
  - the folder and the crusher: all four of page 1 (they colour the sound with every one),
    page 2's stereo and Level;
  - the filter: cutoff, resonance and LFO depth (not the LFO division), and Level;
  - the flanger: amount and Level; the phaser: depth and Level; wow & flutter: wow, flutter,
    age and Level;
  - the resonator: its feedback alone;
  - the delay and reverb: their Level alone, up from off and back down to off, so what comes
    back fades, the echoes or the room;
  - the chaos key: its two chances.

  Mix and Band take the scene's values at once on these.
- **The freezer, shifter and slicer** have no knob that can glide them in (the shifter's
  shift is stepped), so they fade with their page 2 **Mix** instead: one the scene turns off
  glides to dry and goes off on the bar line; the shifter or slicer the scene turns on comes
  on when the glide starts, dry, and its Mix glides up to the scene's. The freezer coming on
  still switches on the bar line, so it grabs on the downbeat: a grab in the middle of a bar
  sounds worse than a hard one on the beat.
- **The tape stop** has nothing to fade with: it switches on the bar line, both ways. Coming
  on, it starts its stop there; going off, it spins up from there.
- **SHIFT + the blank scene** fades every effect out onto the bar line.

The bar lines are the effects' (see [Tempo and transport](#tempo-and-transport)): the loop's bars, counted from its start,
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
- **SHIFT + its blinking key stops it where it is:** every knob stays at the value the glide
  got to, and an effect still waiting to switch stays as it was, so one fading out stays on,
  unless you pressed its key meanwhile: then it's as you left it. The scene key pulses as
  edited, since the sound is now between two scenes: save it to keep it.
- **SHIFT + another scene key** stops it there too and starts a morph (or drop, crossfade,
  build-up) from where it got to; an empty one blinks red and changes nothing.

### Dropping to a scene

A morph builds up; a drop switches hard, on the downbeat, after the "drop mode" of the
Neuzeit Instruments Drop. Hold SHIFT, tap a scene key, and press the transport knob before you
let go of SHIFT: the scene lands all at once, as a recall would, but in time, on the bar line
the morph would have landed on. Tap the scene key more than once and it lands a bar later per
extra tap (up to 8), also after the press. The bar lines are the morph's, counted from letting
go of SHIFT: the loop's bars and its end, or those of MIDI clock or the tapped tempo.

- **Build-up + drop:** hold SHIFT, tap scene key A (once per bar), then scene key B, and let
  go: the morph glides to A over the bars, and B lands hard on the line where it ends. Use it
  to ride a build-up right to the downbeat. A blinks white on the beat, B red. More taps of A
  add bars; B again, another scene key or the transport knob do nothing.
- **On the line:** an effect the scene turns on starts on the line, as after a recall: the
  freezer grabs, the slicer attacks, the tape stops, the shifter swoops. An effect it turns
  off has faded out by the line (it goes off 25 ms before), so nothing of it spills past the
  downbeat. Until then, everything sounds as it was.
- **While it waits,** its scene key blinks red on the beat, and the knobs and keys already
  show the scene, as in a morph. Turning a knob changes where it lands.
- **A plain scene key** meanwhile recalls that scene at once and takes the drop back.
  SHIFT + its red key (or a build-up's white one) in a later SHIFT hold stops it where it
  is: the drop doesn't happen.
- Pressing the transport knob doesn't touch the loop's speed while it makes a drop.
- Once the transport knob has the morph as the [crossfader](#crossfading-by-hand), a press
  does nothing, and the crossfader can't take a drop.
- Over MIDI, CC 83 drops to a scene (see [Controllers](#controllers)).

### Crossfading by hand

A morph at the speed of your hand instead of the bar clock, like the Octatrack's crossfader:
press SHIFT + a scene key as for a morph, and before you let go of SHIFT, turn the transport
knob. After 2 detents one way, and 150 ms without a press of the knob (which would make it a
drop), the morph is yours, where those detents put it: the scene you're in (with whatever you've changed) on the
left, the one you picked on the right, and everything in between, back and forth as often
as you like. 24 detents take it from one end to the other. It works with or without a loop,
and the bar lines don't count while you hold it.

- **What fades is what a morph fades:** knobs on both scenes glide with the transport knob;
  an effect only the new scene has fades in as soon as you're off the left end, one only the
  old scene has fades out and goes off at the right end. The freezer, shifter and slicer fade
  with their Mix, as in a morph. What can't fade switches in the middle, either way: stepped
  knobs (delay division, slicer pattern, shifter shift …), the tape stop, and the freezer
  coming on, which grabs there.
- **Let go of SHIFT** to leave it:
  - **all the way right:** you're in the new scene, as after a recall;
  - **all the way left:** you're back where you started, the scene you were in as it was
    (pulsing if you had changed it, steady if not);
  - **in between:** it stays where it is, as stopping a morph leaves it: the knobs where the
    fade got to, the new scene's key pulsing as edited. Save it to keep it.
- **LEDs:** the two scene keys light in their mix, one brighter as the other dims, and the
  transport LEDs show the position in white instead of the speed: the left one for the old
  scene, the right one for the new.
- Letting go without turning is a plain morph: it glides to the bar line. While you
  hold a morph, SHIFT + turn crossfades instead of stepping the speed in 5ths and octaves;
  once you've let go, the transport knob is back to speed and scrub at once.
- Over MIDI, CC 80 is the crossfader (see [Controllers](#controllers)).

## Settings page

With the mode switch **up**, the keys set what you'd set once for a setup rather
than play: the MIDI channel, MIDI transport following, where FRIZZ takes a MIDI clock from
and how it follows it, whether it sends one, how bright its LEDs are, and the input: its
source (AUX or the built-in mic), whether it goes through the effects, whether it's live, AUX
mono, and its input gain. The loop, the effects and MIDI play on meanwhile; only your hands are
on this page. Switch back down to play.

Each key sets one thing on its press. The white keys pick the MIDI channel; the dark keys
and knob 1 set the rest:

| Control | Setting | Colour |
|---|---|---|
| White keys 1 to 14 | [MIDI channel](#channels) 1 to 14; the one in force lit, the others dim | light blue |
| White key 15 (the last) | Channel 15; pressed again 16, again 15, and so on (default 16) | light blue for 15, white for 16, dim for neither |
| C# (lower octave) | MIDI on every channel: lit when on. It only switches it on; a channel key switches it off | light blue |
| D# | [Effect channels over TRS](#channels): on (lit, default) / off (dim). Over USB they always count | light blue |
| F# | [MIDI transport following](#start-and-stop): on (lit) / off (dim) | green |
| G# | [Clock factor](#clock-factor), each press the next: as sent (default), double, half. It also shows the [beat](#tempo-and-transport): lit for the first half of every beat, dim for the second | yellow (as sent), red (double), light blue (half) |
| A# | [Clock source](#clock-source), each press the next: Auto (default), TRS, USB, internal | white (Auto), orange (TRS), blue (USB), pink (internal) |
| C# (upper octave) | [MIDI out](#midi-out), each press the next: off (default), TRS, TRS and USB | dim (off), orange (TRS), white (TRS and USB) |
| D# (upper octave) | [MIDI feedback](#midi-feedback): on (lit) / off (dim, default) | light blue |
| F# (upper octave) | [The input's source](#input-aux-or-the-mic), each press the other: AUX / the built-in mic. Not saved: AUX's jack picks again when a cable goes in or out | green (AUX), magenta (mic) |
| SHIFT + F# (upper octave) | [Mono input](#mono-input), AUX only: mono / stereo (default). While you hold SHIFT, F# shows it: lit when mono, dim when stereo | white |
| G# (upper octave) | [The source's effects](#input-aux-or-the-mic): through them (default) / dry, past them and the compressor. Dark while the source isn't live: pressed then, it blinks red 3 times and changes nothing | green (through), white (dry) |
| A# (upper octave) | [The source live](#input-aux-or-the-mic), on the outputs: on (lit) / off (dim). AUX on, the mic off at first | white |
| Knob 1 | [LED brightness](#led-brightness): turned, 5 % a detent from 25 % to 100 % (default); SHIFT + turn the next of 25, 50, 75, 100 %; SHIFT + press 100 % | its LED white, as bright as the LEDs are |

The dark keys are grouped by topic: the lower octave's are MIDI in, the upper octave's MIDI out
and the rest.

- **The beat on G#** is the tempo the effects follow right now (see [Tempo and transport](#tempo-and-transport)): with a
  loop, the loop's at its speed; otherwise the MIDI clock's; otherwise the last tempo, each
  with the clock factor applied (a loop, a clock or a tapped tempo of 120 at double beats at
  240). It's there to see the
  tempo, not to play to: it can be a little late, up to a few hundredths of a second. The
  play page shows it on PLAY too (see [Looper](#looper)).

- **VOLUME** works here as on the play page, on two pages: turned, page 1 sets the output
  gain, page 2 the input gain (of the input you hear); a press picks the other page. There's
  no page 3 and no mix here: SHIFT + turn does nothing. Coming from the play page's page 3,
  it's on page 1.
- **VOLUME's LED** shows the battery, but for 2 s after you turn or press VOLUME, when it
  shows its page as on the play page: white while the charging cable is in,
  green above 3.3 V, yellow below. Below 3 V, on any page, the low-battery countdown starts
  ([Power and battery](#power-and-battery)) and VOLUME's LED flashes red. Pull
  the cable and it shows the battery's colour within a few seconds; after that the level is
  read every 30 s.
- The transport LEDs are purple, so the page can't be mistaken for the play page.
- **SHIFT + VOLUME, held 2 s:** hold CHOMPI and press and hold the VOLUME knob, in either
  order; after 2 s it writes a [bug report](#bug-reports), once per hold. CHOMPI lights white
  while you hold it, as on the play page.
- Otherwise knobs 2-4 (their LEDs dark), CHOMPI (but as SHIFT), PLAY and LOOP do nothing
  here.
- A key you're holding when you flip the switch stays where it went down until you let go:
  an effect you hold keeps going, and the key doesn't also change a setting. The same the
  other way round.
- Flipping up ends a scene mode (SAVE, COPY, DELETE): nothing is saved, copied or deleted,
  and you're out of the mode when you flip back down.
- The low-battery warning dims with the LEDs too.
- Every setting but the source itself (AUX or the mic) is saved on the card with the master
  compressor, 2 s after the last change, so it comes back at power-on: each source's effects
  and live, and both input gains too. None is part of a scene. Without a card, or when the
  card can't be written, the key you changed last (or knob 1's LED, for the brightness) blinks
  red 3 times, and FRIZZ tries again 2 s later, up to 3 times; the setting then lasts until
  power-off.

### Mono input

Mono is for a mono (TS) cable, such as one from a Lyra-8 or a guitar pedal: the plug grounds
the right channel, so in stereo the source is heard on the left only. In mono the left channel
feeds both sides, and the effects' stereo knobs spread it from there. SHIFT + F# of the upper
octave toggles it; while you hold SHIFT, F# shows it (lit when mono). It's for AUX only: the
mic is mono anyway, and plays on both sides either way.

### LED brightness

Knob 1 on the settings page sets how bright every LED is, for a dark stage: turned, 5 % a
detent, from 25 % to 100 %; SHIFT + turn goes to the next of 25, 50, 75 and 100 %; SHIFT +
press is back to 100 %. Its own LED is white and dims with all the others, so it shows where
you are. The dim colours keep their hue, and nothing lit at 100 % goes dark; below 25 % they
would. Saved 2 s after the last turn.

## Key combinations

Every gesture on one page; each row links to where it's explained. SHIFT is holding CHOMPI.

### On the play page

| Gesture | Does | See |
|---|---|---|
| LOOP | Record; stop and play; erase | [Looper](#looper) |
| PLAY | Play / pause, when you let go | [Looper](#looper) |
| Hold PLAY, press LOOP | No loop: a quantized recording. A loop playing: erase at its end | [Looper](#looper) |
| PLAY while an erase waits | Takes the erase back | [Looper](#looper) |
| SHIFT + LOOP | Tap tempo | [Tap tempo](#tap-tempo) |
| SHIFT + PLAY | As PLAY | [Looper](#looper) |
| Turn the transport knob | Speed in semitones; paused, a scrub | [Transport knob](#transport-knob) |
| SHIFT + turn the transport knob | Speed in 5ths and octaves | [Transport knob](#transport-knob) |
| Press the transport knob | Back to 1× forward | [Transport knob](#transport-knob) |
| Turn VOLUME | Page 1 output gain, page 2 input gain, page 3 the headphone feed | [VOLUME knob](#volume-knob) |
| Press VOLUME | The next page | [VOLUME knob](#volume-knob) |
| SHIFT + turn VOLUME | Input/loop mix | [VOLUME knob](#volume-knob) |
| SHIFT + press VOLUME | The mix back to the loop alone, or the input alone without a loop | [VOLUME knob](#volume-knob) |
| Hold an FX key | The effect on while held | [Playing an effect](#playing-an-effect) |
| Hold FX keys, then SHIFT | Latch them on, or off | [Playing an effect](#playing-an-effect) |
| SHIFT, then an FX key | Select it for the knobs, without hearing it | [Playing an effect](#playing-an-effect) |
| An FX key on a latched effect | Unlatch it; on until you let go | [Playing an effect](#playing-an-effect) |
| The last white key | The master compressor on the knobs | [Master compressor](#master-compressor) |
| Turn knobs 1-4 | The selected effect's parameters | [Keys and knobs](#keys-and-knobs) |
| SHIFT + turn knobs 1-4 | Coarse, to the next point of the grid | [Coarse steps](#coarse-steps) |
| SHIFT + press knobs 1-4 | That parameter back to its default | [Playing an effect](#playing-an-effect) |
| Press knobs 1-4 | Page 2, and back to page 1 | [Page 2](#page-2) |
| A scene key | Recall the scene | [FX scenes](#fx-scenes) |
| The active scene's key | Back to the scene as saved | [FX scenes](#fx-scenes) |
| The blank scene's key (lower F#) | Every effect off, every knob on its default | [FX scenes](#fx-scenes) |
| SHIFT + a scene key, let go | Morph to it on the bar line; tap it again for each bar more, up to 8 | [Morphing to a scene](#morphing-to-a-scene) |
| SHIFT + a scene key, press the transport knob | Drop to it on the bar line | [Dropping to a scene](#dropping-to-a-scene) |
| SHIFT + a scene key, turn the transport knob | The crossfader | [Crossfading by hand](#crossfading-by-hand) |
| SHIFT + scene key A, then scene key B | Build-up to A, B dropping at its end | [Dropping to a scene](#dropping-to-a-scene) |
| SHIFT + a blinking scene key | Stop the morph, drop or build-up where it is | [Timed scene changes](#timed-scene-changes) |
| SHIFT + another scene key, while one runs | Stop it there and start the new one from there | [Timed scene changes](#timed-scene-changes) |
| Upper F#, G# or A#, the slot (copy: the source, then the destination), then CHOMPI | Save, copy or delete a scene | [Save, copy and delete](#save-copy-and-delete) |
| The function's key again, or the mode switch up | Out of save, copy or delete, doing nothing | [Save, copy and delete](#save-copy-and-delete) |

### On the settings page

| Gesture | Does | See |
|---|---|---|
| White keys 1-15 | The global MIDI channel; the 15th again for 16 | [Channels](#channels) |
| Lower C# | MIDI on every channel | [Channels](#channels) |
| Lower D# | Effect channels over TRS | [Channels](#channels) |
| Lower F# | MIDI transport following | [Start and Stop](#start-and-stop) |
| Lower G# | Clock factor: as sent, double, half | [Clock factor](#clock-factor) |
| Lower A# | Clock source: Auto, TRS, USB, internal | [Clock source](#clock-source) |
| Upper C# | MIDI out: off, TRS, TRS and USB | [MIDI out](#midi-out) |
| Upper D# | MIDI feedback | [MIDI feedback](#midi-feedback) |
| Upper F# | The input's source: AUX or the mic | [Input: AUX or the mic](#input-aux-or-the-mic) |
| SHIFT + upper F# | AUX mono | [Mono input](#mono-input) |
| Upper G# | The source through the effects, or dry | [Input: AUX or the mic](#input-aux-or-the-mic) |
| Upper A# | The source live on the outputs | [Input: AUX or the mic](#input-aux-or-the-mic) |
| Turn knob 1; SHIFT + turn; SHIFT + press | LED brightness; in steps of 25 %; back to 100 % | [LED brightness](#led-brightness) |
| Turn VOLUME; press | Output gain, input gain | [Settings page](#settings-page) |
| SHIFT + VOLUME held 2 s | A bug report | [Bug reports](#bug-reports) |

### At power-on

| Gesture | Does | See |
|---|---|---|
| Hold CHOMPI, PLAY and LOOP while switching on | Shipping mode | [Power and battery](#power-and-battery) |

## Power, battery and bug reports

### Power and battery

- **Low battery:** unplugged with the battery nearly empty (below 3 V), the knob, transport,
  CHOMPI, PLAY and LOOP LEDs flash yellow for 15 s and VOLUME's red, on any page, then the
  CHOMPI switches itself off. Plugging in power during those 15 s stops it.
  On a charger too weak to run it with a low battery, it goes dark until it has charged.
- **Battery check:** the [settings page](#settings-page) shows the battery's level on VOLUME's
  LED.
- **Shipping mode:** hold CHOMPI, PLAY and LOOP while switching on: the battery is
  disconnected until the CHOMPI is plugged into power again. Use it to store it for weeks.
- **Hardware test:** FRIZZ has no self-test of its own. To check keys, knobs and LEDs, put the
  stock TAPE or WAVE firmware on a card and hold VOLUME while switching on.

### Bug reports

On the [settings page](#settings-page) (mode switch up), hold SHIFT (CHOMPI) and
press and hold the VOLUME knob for 2 seconds: FRIZZ writes everything you did since switching
on to the card, as `/FRIZZ/bug-1.txt` (the next one `bug-2.txt`, and so on): every key, knob detent and
mode-switch flip with its time, the MIDI clock's tempo, every MIDI message FRIZZ acted on, and
the scenes and compressor the card held at power-on. Do it right after something went wrong
(flip the switch up, hold, flip it back down) and send the file with your report:
FRIZZ's developers play it on a virtual CHOMPI on their computer, which then does what yours
did, key for key.

- It's on the settings page so it can't be written by accident while playing.
- While it's written, both transport LEDs blink white; then they blink 3 times, white when
  it's on the card, red when it isn't (no card, or a full one). A long session takes a moment,
  and the keys answer late until it's done; the sound plays on.
- It doesn't hold the audio you played in, so a problem that depends on the sound itself
  needs a description too, nor scenes sent over MIDI. Dense MIDI automation fills it
  faster: each controller message is an event. It holds about 260,000 events, hours of playing; a session longer
  than that is cut off at the end.
- It doesn't send anything anywhere and costs nothing while you play.

## Appendix: MIDI

FRIZZ takes MIDI from the TRS MIDI input and over USB (CHOMPI is a USB device, so USB MIDI
comes from a computer or a host), both alike. With [MIDI out](#midi-out) on, it sends its
clock and the loop's Start, Stop and Continue out of the TRS MIDI output, and over USB if you
ask; otherwise it sends nothing over TRS, and over USB it only answers FRIZZ's own queries
(see [Remote control](#remote-control)).

### MIDI clock

Quantized recording, and the effects' tempo while there's no loop, follow MIDI clock
(24 PPQN), from the input the clock source picks. The clock is read on every channel.

### Clock source

The lower octave's A# on the [settings page](#settings-page) picks whose clock counts, each
press the next:

- **Auto** (the default): whichever input ticks first, TRS or
  USB, until it has been silent for 0.5 s; then whichever ticks next.
- **TRS** or **USB**: only that input's clock; the other's is ignored, even while the chosen
  one is silent. A DAW over USB can then play notes and send CCs without its clock getting in
  the way of a sequencer on the jack, or the other way round.
- **Internal**: no MIDI clock at all. The tempo comes from the loop, your taps or the last
  tempo, and tap tempo works while a clock runs; a quantized recording is refused, as without
  a clock.

Switching to a source that leaves out the clock FRIZZ was following lets go of it at once, as
a clock that stops: a quantized recording ends where it is. Notes, controllers, program
changes and FRIZZ's own SysEx come in from both inputs whatever the source; MIDI Start, Stop,
Continue and Song Position come only from the chosen input (see [Start and Stop](#start-and-stop)). It's kept as
`clock_source` in `FRIZZ/frizz_master.txt` (0 Auto, 1 TRS, 2 USB, 3 internal), and
`firmware/remote.py source auto|trs|usb|internal` sets it too.

The [clock factor](#clock-factor) on the [settings page](#settings-page) makes FRIZZ follow
the clock at half its tempo or at double: a DAW at 70 BPM with FRIZZ thinking in 140, or the
other way round. It counts for everything that follows the clock: the bars of a quantized
recording and the effects' tempo. A loop recorded to it keeps those bars when the factor
changes later; the effects then follow the loop at the new factor. Changing it during a
quantized recording ends that recording where it is, as a lost clock does.

### Channels

FRIZZ listens on 16 channels, with two kinds of map:

- **The global channel**, **16** at first: notes (the keys), program changes (scenes), the
  looper, the scenes, the gains, every effect's latch, the compressor, and the knobs of the
  effect in focus. That alone covers FRIZZ from a hardware controller or a TRS chain.
- **The effect channels 1-15**, one per white key, left to right: 1 freezer, 2 shifter,
  3 folder, 4 crusher, 5 filter, 6 flanger, 7 phaser, 8 resonator, 9 slicer, 10 wow & flutter,
  11 tape stop, 12 delay, 13 reverb, 14 chaos key, 15 master compressor. Each has the same
  CCs, so in a DAW one MIDI track per effect has the same automation lanes everywhere.

**Knobs back to 0 when the DAW stops:** REAPER 7 as it comes sends every CC its tracks played
back to 0 when you stop, so the knobs you automated jump to 0. To keep them where the
automation left them, untick *Reset MIDI CC/Pitch on: playback stop* (and *playback
loop/skip*, which does the same at a loop's turn) in REAPER's Preferences, Playback, or enter
`cc0-120=-1` under *CC reset overrides* there. Other DAWs may do something similar.

On the global channel, its own map wins: set it to 5, and channel 5 is the global channel,
with the filter reached through the focus (CC 23 and CC 14-22) and its latch (CC 106). Set to
every channel (0), only the global map counts, on all of them, and the effect channels are off.

Change the global channel on the [settings page](#settings-page), in `FRIZZ/frizz_master.txt`
(`midi_channel 16`; 0 listens on every channel), or from a computer with
`firmware/remote.py channel N`.

**A shared TRS chain:** over USB, FRIZZ is a port of its own. Over TRS, the effect channels
take channels 1-15 of the cable too, so other gear on the same chain that listens or sends on
them reaches FRIZZ's effects. If that gets in the way, switch the effect channels off over TRS
with the settings page's lower D# (`midi_block_trs 0` in `FRIZZ/frizz_master.txt`); over USB
they still count, and the global channel works over both.

### Notes: the keys

A note holds the key it lands on while it's on, as your finger would: an FX key punches its
effect in, a scene key recalls, and so on. The 25 keys are a keyboard from **note 48** (C3,
or C2 in Ableton Live): the white keys from 48 up, the dark keys on the sharps, so the
[scene keys](#fx-scenes) are notes 54 (the blank scene), 56, 58, 61 and 63 (scenes 1-4), and
save, copy and delete 66, 68 and 70. Below them:

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

**On each effect channel (1-15)**, and on the global channel for the effect in focus:

| CC | Function |
|---|---|
| 14, 15, 16, 17 | Page 1's knobs 1-4, set outright |
| 18, 19, 20, 21 | [Page 2](#page-2)'s knobs 1-4 (mix or the sends' freeze, stereo or its own, band, level), set outright |
| 22 | Latch: 64 and above on, below off, acting as the value crosses 64. The compressor (channel 15) has none, nor a page 2 knob 2: its CC 19 does nothing |

**On the global channel:**

| CC | Function |
|---|---|
| 14-22 | The effect in focus, as on its own channel |
| 23 | Focus: 1-15, the white key whose knobs CC 14-22 reach (15 the compressor). It's the effect the knobs edit, as SHIFT + its key picks it, and it follows when you pick one by hand |
| 24-31 | The master compressor's knobs, as channel 15's 14-21: 24-27 page 1 (threshold, ratio, attack, release), 28, 30 and 31 page 2 (mix, sidechain highpass, makeup); 29 does nothing |
| 84 | [The input's source](#input-aux-or-the-mic): 0-63 AUX, 64-127 the mic, as the settings page's upper F# picks it |
| 85 | Output gain |
| 86 | Input gain, of the input heard (AUX or the mic) |
| 87 | Input/loop mix |
| 89 | Headphone feed, as VOLUME's page 3: 127 the master out, 0 the input (AUX or the mic) on its own |
| 90 | The loop's speed in semitones, set outright: 64 as recorded (1×), 0 the slowest (1/16×), 127 double. Only with a loop; forward or in reverse as it runs |
| 3 | Morph to scene 0-4 (0 the blank one), as SHIFT + its key, landing on the bar line (CC 9's), counted from the CC. During a morph, drop or build-up, it stops that where it is and starts the new one from there. It acts as the value changes: to morph to the same scene again, send another value first (127, no scene) |
| 9 | How many bars a morph over CC 3, or a drop over CC 83, takes, 1-8 (1 at first) |
| 80 | The [crossfader](#crossfading-by-hand): takes a running morph (CC 3's, or one you hold) to where it says, 0 the scene it started from, 127 the scene it goes to. Reaching 127 lands it; reaching 0 after being away from it goes back to where it started; in between it waits |
| 81 | Stops a morph, drop or build-up where it is, as the value crosses 64 upward |
| 82 | A [tempo tap](#tap-tempo), as the value crosses 64 upward: a button sending 127 and 0 |
| 83 | [Drop](#dropping-to-a-scene) to scene 0-4 (0 the blank one) hard on the next bar line, or CC 9's, counted from the CC, as SHIFT + its key and a press of the transport knob. Like CC 3, it stops a sequence running and acts as the value changes: to drop to the same scene again, send another value first (127, no scene) |
| 102-115 | The latch of white key 1-14 (102 the freezer … 108 the phaser … 114 the reverb, 115 the chaos key), as CC 22 |
| 116 | [The source's effects](#input-aux-or-the-mic), the source heard now: 0-63 through them, 64-127 dry, as the settings page's upper G#. Set also while the source isn't live |
| 117 | [The source live](#input-aux-or-the-mic), the source heard now: 0-63 off, 64-127 on, as the settings page's upper A# |
| 120, 123 | All Sound Off, All Notes Off: let go of every key MIDI holds |

- **Set outright**, a value does what the knob or key would: the LEDs follow, the scene key
  pulses as edited, a morph lands where the CC put the parameter, a send ringing out takes it at
  once. The knobs stay on the effect you picked (except with CC 23). 64 is a knob's centre
  exactly (the filter's flat cutoff, a delay's random off), 127 its top. Stepped parameters
  take the nearest step.
- **Switches act as they change** (latches, CC 3, 81, 82, 83): an automation lane that sends the
  same value again does nothing, so a latch you switched by hand stays as you left it until the
  lane crosses 64 again. Use 0 and 127.
- **Automation isn't saved:** nothing that comes in by CC goes to the card. After a restart the
  compressor is as you left it by hand (its knobs, the settings page, or FRIZZ's SysEx), the
  source (CC 84) as the jack picks it, each source's effects and live (CC 116, 117) as the
  settings page's G# and A# left them, the input gains as you last turned them,
  and the output gain and the mix at their power-on levels. Scenes you save keep what the CCs set, as ever.
- **Left free on purpose**, so what hosts and keyboards send by themselves reaches nothing:
  bank select (0, 32), mod wheel (1), breath (2), foot (4), volume (7), pan (10), expression
  (11), the pedals (64-69), keyboards' sound controllers (70-79), NRPN and RPN with their data entry (6, 38,
  96-101: a DAW's pitch-bend range doesn't turn a knob), 88, effects depths (91-95: a
  keyboard sends reverb depth, 91, at power-on), Reset All
  Controllers (121). CC 46-53 stay free for a 14-bit mode later.
- Notes 36-39, the knob presses, turn the page as your hand does. Mod wheel, pitch bend,
  aftertouch and other controllers are ignored. Notes and program changes count on the global
  channel only.

### Program changes

Program change 0-4 presses scene key 0-4 (0 the blank scene): a recall, or in a save, copy or
delete mode the slot. 5 and above are ignored.

### Start and Stop

With transport following on, a DAW's play button starts and stops the loop with the song. It's
off at first; switch it on on the [settings page](#settings-page) (F# of the lower octave),
with `firmware/remote.py transport on`, or `midi_transport 1` in `FRIZZ/frizz_master.txt`.

| Message | A loop recorded to the clock (quantized) | A free loop |
|---|---|---|
| **Start** | back to its start, playing from the next clock tick (playing, it glides there) | plays on from where it is |
| **Continue** | plays on from where it is, from the next clock tick | plays on |
| **Stop** | pauses | pauses |
| **Song Position**, while paused | moves it there, by its length in the sender's 16ths (a 2-bar loop is 32: position 36 is 4 in; with the [clock factor](#clock-factor) at half when it was recorded, a bar of the loop is two of the sender's) | ignored |

So a DAW's Start lines the loop up with its bar 1, and moving its playhead while stopped takes
the loop along. A loop counts as recorded to the clock if its recording ended on the clock's
bars (PLAY held + LOOP with a clock coming in); one closed early because the clock stopped
counts as free. Song Position while playing is ignored, as are MIDI Machine Control and MIDI
Time Code. Without a clock coming in, Start and Continue play at once.

With the [clock source](#clock-source) on TRS or USB, Start, Stop, Continue and Song Position
count only from that input; on Auto or internal, from both.

### MIDI out

FRIZZ sends its clock, so a drum machine, a sequencer or a synth's arpeggio plays
in time with it, and with the loop. The upper octave's C# on the [settings
page](#settings-page) switches it, each press the next: **off** (the default, dim), **TRS**
(the TRS MIDI output, orange), **TRS and USB** (white). It's kept as `midi_out` in
`FRIZZ/frizz_master.txt` (0 off, 1 TRS, 2 TRS and USB), and `firmware/remote.py out
off|trs|all` sets it too.

**The clock** (24 PPQN) runs all the time while MIDI out is on, at the tempo FRIZZ thinks in,
the beat on the settings page's G#:

- **With a loop:** the loop's tempo, at its speed and the [clock factor](#clock-factor), with
  the loop's start on beat 1 (at half, a loop of an odd number of beats on every other pass,
  so the clock runs evenly). In reverse
  the clock keeps running, at the loop's speed; while the loop is paused, it runs on at the
  loop's tempo.
- **Without a loop, with a MIDI clock coming in:** that clock, passed on tick for tick, with
  the [clock factor](#clock-factor): at double a tick more halfway between two, at half every
  other one.
- **Neither:** the last tempo (tapped, from the last loop or from a clock; 120 BPM at
  power-on), with the clock factor.

**Start, Stop and Continue** follow the loop:

- A new loop starting to play sends **Start**, so the gear starts on the loop's beat 1. If
  the gear already plays from a DAW's Start that FRIZZ passed on, no second Start: it plays
  on, and the loop takes over the clock.
- A pause sends **Stop**. PLAY sends **Song Position** (where the loop goes on, to the 16th)
  and then **Continue** on that 16th, so the gear goes on where the loop does. Gear that
  ignores Song Position goes on where it stopped, which is the same place, unless the loop
  moved while paused. A [standstill](#looper) of the transport knob is a pause to the gear
  too: Stop going into it, Song Position and Continue when you turn out of it.
- A loop taken back to its start by a DAW's Start (with [transport
  following](#start-and-stop) on) sends **Start** with that tick, so the gear starts over with it.
- Without a loop, Start, Continue and Stop that come in from the clock's input are passed on.
- Erasing the loop sends nothing: the gear plays on, to the clock that's coming in or the
  loop's tempo.

**Not back to the sender:** while FRIZZ passes a clock on, nothing goes back out of the input
it comes in on. A DAW over USB gets nothing back over USB, gear on the TRS jacks nothing out
of TRS. With a loop, FRIZZ leads, and its clock goes out of every port MIDI out is set to.

Nothing else goes out: no notes, no MIDI thru, and controllers only with [MIDI
feedback](#midi-feedback) on.

- Gear that follows any clock by itself takes FRIZZ's tempo as soon as MIDI out is on, even
  while nothing plays. That's why it's off at first.
- Over USB, the clock goes out between FRIZZ's other work: it's less steady than the TRS
  output's (by up to a millisecond or so), and while FRIZZ writes the card (a scene saved)
  its ticks wait and come out in a bunch. For tight timing, use TRS.
- A DAW that follows FRIZZ's clock (synced to an external MIDI clock): set its project tempo
  roughly to the loop's. REAPER, for one, keeps its own tempo and follows a clock smoothly
  only within about half to twice it (measured); further off, it plays in jumps. So the
  [clock factor](#clock-factor) at x2 or x1/2 may need the DAW's tempo moved along. It also
  holds still about a second after Start before it runs, then plays in step.
- Gear with MIDI thru on that sends FRIZZ's clock back into its TRS input: with the clock
  source on Auto, FRIZZ follows its own clock coming back. Set the clock source to internal
  or USB, or switch the gear's thru off. With [transport following](#start-and-stop) on and a
  loop, FRIZZ's Start comes back too: it takes the loop back to its start, which sends Start
  again, and the loop starts over and over. The clock source on internal doesn't stop that
  (on internal, Start and Stop count from both inputs): set it to USB, switch transport
  following off, or the gear's thru.

### MIDI feedback

What you change on the panel goes out as the CCs that would set it (see
[Controllers](#controllers)), so a controller with LED rings or motor faders, or a DAW
picking up a knob, follows the panel. Off at first; the upper octave's D# on the [settings
page](#settings-page) switches it (`midi_feedback 1` in `FRIZZ/frizz_master.txt`). It goes
where [MIDI out](#midi-out) goes, so MIDI out has to be on too.

- **What goes out:** an effect's knobs (CC 14-21) and latch (CC 22) on its own channel, and
  for the effect in focus also on the global channel (CC 14-22); the latches by key (CC
  102-115), the focus (CC 23) as you pick an effect, the compressor (CC 24-31), the gains and
  mix (CC 85-87), the headphone feed (CC 89), the input (CC 84: 0 AUX, 127 the mic, also
  when the jack switches it; CC 116 and 117, 0 or 127, the source's effects and live, also
  when a switch of the source brings up the other's), the loop's speed (CC 90), and the crossfader
  (CC 80) while you hold it by hand.
- **A scene** recalled sends what it changed, once. A morph sends only where it lands, once
  it has. The chaos key changes no setting, so it sends nothing.
- **Never back:** what came in by MIDI, or by FRIZZ's SysEx, isn't sent back, so a DAW
  recording what it also sends doesn't loop.
- **Paced:** a value goes at most once every 10 ms, and only when its 7-bit value changed.
  A scene's many changes take a moment to go out of TRS (about 660 CCs a second). The clock
  goes first, so its timing stays as without feedback.
- With [the effect channels](#channels) off over TRS, only the global channel's go out of
  TRS; over USB, all. On every channel (omni), only the global ones go out, on channel 16.
- Switching it on sends nothing by itself: a controller learns a value when it next changes.

### Remote control

FRIZZ's own SysEx (`F0 7D 43 48 ...`, documented in `firmware/code/src/MidiControl.h`) presses
keys, turns knobs, sets the channel, transport following, the clock source and MIDI out, and holds the mode switch up or down (until power-off) on
every channel; its keys and knobs act as your hands do, on the settings page too, and over USB answers what the play
page shows, every LED, the processing load, and sends and receives scenes. `firmware/remote.py`
uses it from a Linux computer: `state`, `leds`, `load`, `settings`, `channel 16`, `transport on|off`, `source usb`, `out off|trs|all`, `switch up|down|hand`, `scene get 2 my.json`, `scene put 3
my.json`, and `play SCRIPT --cpu`, which plays a scenario of the virtual CHOMPI on the device
and reports the worst load and how many audio blocks ran late (crackles).

The `7D` in that header is MIDI's ID for projects without a manufacturer ID of their own, and
the multi-firmware launcher uses the same header. FRIZZ keeps it until the launcher moves to an ID of its
own, and then follows (#103).

For developers, one more command over USB, `0x3E` (`firmware/README.md`, *The developers'
SysEx*), lets the tools tell which build runs (`remote.py ident`), read and set any effect or
compressor knob, VOLUME's levels and every setting of the settings page by number rather than
by CC (`remote.py get` / `set`), follow the CPU bench and fetch its results, keep a test
from writing the card until FRIZZ restarts (`remote.py ram-only`; `play` does this first), read
the clock, the looper, the levels and the battery, play a test signal in place of the input
until FRIZZ restarts, and start the random numbers from a seed.

## Appendix: Technical data

The numbers behind FRIZZ, for planning a setup. The timings were measured on a CHOMPI.

### Audio

- 48 kHz, stereo in and out, through a 24-bit codec. FRIZZ works in floating point, in blocks
  of 24 samples (0.5 ms).
- The loop: up to 2:45 of stereo, stored at 16 bits in RAM, gone at power-off; a limiter of
  its own keeps what goes in just under full scale (see [Looper](#looper)).
- The freezer records up to 5 s; the delay holds 10 s, so 2 bars fit at the slowest tempo.
- The tempo: 50-300 BPM, 120 at power-on.
- Every output has a fixed safety limiter after VOLUME (see [Master
  compressor](#master-compressor)).

### Processing load

Effects that are off cost (almost) no processing time, but the ones that are on do. Some
heavy combinations (the shifter and the crusher with a loop playing and the master compressor
working hard, for example) can need more time than there is, which is heard as crackles;
switching one of them off clears it.

Measured on a build that runs a set of combinations by itself, as a share of the time each
block has: FRIZZ alone about 35 %, one effect at most about 50 %, all the inserts with a loop
about 70 %, every effect at once about 80 %. Every effect at once with a loop, and page 2
turned off its defaults on all of them (Mix, Band and Level), needs more than there is.

### Timing

| What | Measured |
|---|---|
| A quantized loop against the clock it was recorded to, over TRS | 0.4 ms a minute at most (1 bar, at 120 and 174 BPM) |
| The same over USB | 4 bars: 0.35 ms a minute at most; 1 bar: from −2.2 to +2.3 ms a minute, depending on the recording |
| A loop recorded to REAPER over USB (87 BPM, 4 bars) | 0.66 ms a minute; every bar within 1.5 ms of the song |
| The effects' tempo following a clock that changes | 10 BPM within about 0.4 s, 1 BPM within about 2 s |
| The clock source on Auto, its clock stopping | The other input's clock counts within about 0.8 s |
| MIDI clock out over TRS (120 BPM) | Jitter 0.53 ms; 0.64 ms with MIDI feedback sending 1652 CCs meanwhile |
| MIDI clock out over USB | Its tempo right (120.08 BPM for 120), its ticks less even, bunched while the card is written |
| A computer's clock over USB, passed on out of TRS (200 and 300 BPM) | Each tick 0.13-0.16 ms off even on average, 99 % within 0.5 ms |
| MIDI feedback | One CC per value every 10 ms at most |
| Switching on | Sound about a second after FRIZZ starts |

### Formats

- The card: microSD, FAT32. FRIZZ's files are text, in the `FRIZZ` folder (see [Card and
  files](#card-and-files)).
- MIDI clock in and out at 24 PPQN. The keys are notes 48 to 72, the knob presses and the
  other keys below them (see [Notes: the keys](#notes-the-keys)).
- FRIZZ's own SysEx starts `F0 7D 43 48` (see [Remote control](#remote-control)).
