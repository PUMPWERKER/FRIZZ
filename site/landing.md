# FRIZZ website: front page

<!--
The copy of the front page of the FRIZZ website (pumpwerker.de/frizz) for this release.
The website's scripts/fetch-docs.mjs reads it at the release's tag; the rest of the page (the
keys, their knobs and colours, the download) comes from the manual and the firmware.

The format is fixed, and a file that breaks it fails the website's build:
- the sections below, all of them, in this order; in each, its fields as "- Name: value" lines
  right under the heading, then (where there is one) one paragraph after a blank line;
- Duties, what FRIZZ does for you, may be left out (it stays between Hero and Keys): no fields
  or paragraph of its own, 3 to 6 duties (###), each a title of at most 32 characters and one
  paragraph of at most 220, nothing else; the site numbers the tiles, so no digits in a title;
- Session: 3 to 6 moves (###), each with "- Controls:" (1 to 4 `control`s) and one paragraph;
- inline only **bold**, *emphasis* and [links](MANUAL.md#anchor), from the repo's root;
  *emphasis* in the hero's and the session's title marks the word drawn over;
- no HTML, and no more text than the page has room for (the website's fetch-docs has the limits).
-->

## Meta

- Title: FRIZZ, a free live looper and effects firmware for the CHOMPI
- Description: FRIZZ turns the CHOMPI into a live rig in one box: your loops from the card, effects you play on the keys, scenes that land on the bar line. Free, from the SD card.

## Hero

- Eyebrow: Free firmware for the CHOMPI
- Title: Your CHOMPI is a *live* rig now
- Assure: Free and open source. Installs from the SD card without touching the bootloader; you go back to stock firmware the same way.

**FRIZZ** turns your CHOMPI into a small live set in one box. Bring your own loops on the SD
card, or play in whatever you plug into AUX, its built-in mic or a computer on USB. Hold a white
key to throw an effect over it, let go to snap back, and drop to the next scene on the bar line.

## Duties

### Loops from the card

You bring your own WAVs, [8 banks of 15](MANUAL.md#loops-on-the-card), and the recordings you
made with TAPE, and switch between them at once, at the end of a pass or with a scene.

### Thirteen punch-in effects

You play 13 effects on the white keys, from the freezer to the reverb, each with four knobs and
a [second page](MANUAL.md#page-2): hold a key and it's on, let go and it's gone. Plus a chaos key.

### Four scenes

You keep every effect's knobs and latches in [four scenes](MANUAL.md#fx-scenes) on the card,
and recall one at once, drop to it on the bar line or morph there.

### A finished sound

You master the set on the way out: the [finalizer](MANUAL.md#finalizer) on key 15 has an input
trim, a 3-band EQ, compression, width and a limiter, and a safety limiter guards every output.

### A looper and three inputs

You catch up to 2:45 from AUX, the built-in mic or a computer on USB, free or in whole bars, and
play it faster, slower, backwards or scrubbed on the [transport knob](MANUAL.md#transport-knob).

### MIDI and USB

You clock it, play its keys and reach every parameter over MIDI on TRS or USB, and on a computer
it's a [sound card](MANUAL.md#usb-audio) too: 2 channels each way, 48 kHz, no driver.

## Keys

- Title: Hold a key. Let go.

The effect is on for as long as you hold its key. Hold it and press SHIFT to leave it on, and
knobs 1 to 4 shape the last one you touched. The keys run in signal order: the freezer catches
the clean sound, the tape stop halts the lot while the echoes ring on. Pick a key.

## Session

- Eyebrow: How it plays
- Title: A set, from the first loop to the *drop*

### Load your set

- Controls: `SHIFT + lower C#` `White keys`

Your loops go on the card from the [Companion](https://pumpwerker.de/frizz/companion/), over
the USB cable while FRIZZ plays. SHIFT and the lower C# open the Loops page: a white key switches
to its loop at once and keeps time, SHIFT and a white key at the end of the pass.

### Mangle it on the white keys

- Controls: `White keys` `Knobs 1 to 4`

Hold a key for as long as you want the effect, let go and it's gone; SHIFT leaves it on. Most
effects start silent, so one turn brings them in, and the knobs do the same job on each: 1 is
the main control, 2 the feedback where an effect has one.

### Save the build-up, recall the drop

- Controls: `Dark keys` `SHIFT + dark key`

Four dark keys hold scenes on the SD card: every effect’s knobs, and which ones are latched.
Press one and it all lands at once. Hold SHIFT and it morphs there instead, arriving at the end of
the bar. In the same SHIFT hold on the Loops page, the next loop lands with it.

### Finish it on key 15

- Controls: `Key 15` `SHIFT + key 15`

Key 15 is the finalizer, a master section after the effects: input, a 3-band EQ, compression,
width and a limiter that makes the set loud. SHIFT and key 15 step through its modules. At its
defaults it leaves the sound untouched.

### Catch something live

- Controls: `LOOP` `Transport knob`

Press LOOP, play, press LOOP again: up to 2:45 of whatever comes in, recorded before the
effects, so nothing you do to the sound is printed into it. The transport knob bends it from
double speed down to 1/16, through a standstill into reverse.

### Lock it to the rest of your setup

- Controls: `MIDI TRS` `USB` `PLAY + LOOP` `SHIFT + LOOP`

MIDI clock over TRS or USB sets the tempo for the tempo-synced effects; hold PLAY as you press
LOOP and the loop locks to whole bars of it. No clock? Tap the tempo in. Notes play the keys, CCs
reach every parameter, program changes recall scenes and loops.

## Get started

- Eyebrow: Get started
- Title: From the SD card to your first loop
- Install: Back up the card, swap the .bin file on it for FRIZZ, switch on. Going back to TAPE, TEMPO or WAVE works the same way.
- Quick start: Levels, your first loop and your first effects, in ten minutes.
- Manual link: Every control and every knob is in the manual

You need a CHOMPI, its microSD card and a computer with a card reader. No tools, no drivers, and
it doesn't matter which firmware the CHOMPI runs now.

## Credits

FRIZZ builds on the [CHOMPI open-source release](https://github.com/CHOMPI-Club/CHOMPI) by
CHOMPI Club and runs on Electrosmith's Daisy platform. Several effects come from Bastl
Instruments' Kastle 2 FX Wizard, the reverb and the frequency shifter from Mutable
Instruments (MIT). USB audio and the card drive build on lost-tty's
[MIXTAPE](https://github.com/lost-tty/CHOMPI) (MIT). Thanks to
[sfaber02](https://github.com/sfaber02) for the mono input switch and
[ugrossek](https://github.com/ugrossek) for the launcher's MIDI sender in FRIZZ's tools.
