# FRIZZ website: front page

<!-- The text of frizz.friedrichwagner.eu's front page, read by frizz-web's fetch-docs at the
release tag. Its format is fixed: firmware/tools/doclinks.py checks it. The design, the key row's
kicker, the download step and the trademark note stay in frizz-web. -->

## Meta

- Title: FRIZZ, a free looper and effects firmware for the CHOMPI
- Description: FRIZZ turns the CHOMPI into a stereo looper and effects box: catch a loop, then hold a white key to play an effect over it. Free, installed from the SD card.

## Hero

- Eyebrow: Free firmware for the CHOMPI
- Title: Loop it, then *mangle* it live
- Assure: Free and open source. Installs from the SD card without touching the bootloader; you go back to stock firmware the same way.

**FRIZZ** turns your CHOMPI into a stereo looper and effects box for whatever you plug into its
AUX input: a synth, a drum machine, anything with a line out, or the built-in mic. Catch a loop,
hold a white key to throw an effect over it, and let go to snap back.

## Keys

- Title: Hold a key. Let go.

The effect is on for as long as you hold its key. Hold it and press SHIFT to leave it on, and
knobs 1 to 4 shape the last one you touched. The keys run in signal order: the freezer catches
the clean sound, the tape stop halts the lot while the echoes ring on. Pick a key.

## Session

- Eyebrow: How it plays
- Title: From the first loop to the *drop*

### Catch a loop

- Controls: `LOOP`

Press LOOP, play, press LOOP again: up to 2:45 of whatever comes in. The looper records before
the effects, so nothing you do to the sound is ever printed into it.

### Bend it on the transport knob

- Controls: `Transport knob` `SHIFT + turn`

Turn the transport knob and the loop runs from double speed down to 1/16 in semitones, then
through a standstill into reverse; hold SHIFT to jump in fifths and octaves. Paused, the knob
scrubs. Press it for plain speed, forwards.

### Throw effects over it

- Controls: `White keys` `Knobs 1 to 4`

Hold a key for as long as you want the effect. Most effects start silent, so one turn brings
them in, and the knobs do the same job on each: 1 is the main control, 2 the feedback where an
effect has one.

### Save the build-up, recall the drop

- Controls: `Dark keys` `SHIFT + dark key`

Four dark keys hold scenes on the SD card: every effect's knobs, and which ones are latched.
Press one and it all lands at once. Hold SHIFT and it morphs there instead, arriving at the end
of the bar. The dark key left of them is the blank scene: everything off.

### Lock it to the rest of your setup

- Controls: `MIDI TRS` `USB` `PLAY + LOOP` `SHIFT + LOOP`

MIDI clock over TRS or USB sets the tempo for the tempo-synced effects; hold PLAY as you press
LOOP and the loop locks to whole bars of it. No clock? Tap the tempo in. Notes play the keys,
CCs reach every parameter, program changes recall scenes.

## Get started

- Eyebrow: Get started
- Title: From the SD card to your first loop
- Install: Back up the card, swap the .bin file on it for FRIZZ, switch on. Going back to TAPE, TEMPO or WAVE works the same way.
- Quick start: Levels, your first loop and your first effects, in ten minutes.
- Manual link: Every control and every knob is in the manual

You need a CHOMPI, its microSD card and a computer with a card reader. No tools, no drivers, and
it doesn't matter which firmware the CHOMPI runs now.

## Credits

FRIZZ builds on the [CHOMPI open-source release](https://github.com/CHOMPI-Club/CHOMPI) by CHOMPI
Club and runs on Electrosmith's Daisy platform. Several effects are ported from Bastl
Instruments' Kastle 2 FX Wizard.
