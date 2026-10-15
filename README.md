# FRIZZ

**FRIZZ** is an alternative firmware for the CHOMPI. It turns the CHOMPI into a stereo effects
box and looper for whatever you plug into its AUX input, for its built-in microphone, or for a
computer on USB.

- **A looper:** free-length loops, or loops locked to whole bars of an incoming MIDI clock.
  Varispeed in semitones (fifths and octaves with SHIFT), reverse and scrub on the big
  transport knob.
- **Loops from the SD card:** your own WAVs, 8 banks of 15, and the recordings you made with
  TAPE (its cubbis), one on each white key of the Loops and Cubbi pages. They switch at once
  in time, at the end of a pass or with a scene change, by hand or over MIDI (bank select and
  program change).
- **The Companion,** a web page at
  [frizz.friedrichwagner.eu/companion](https://frizz.friedrichwagner.eu/companion/), that puts
  your loops on the card over the USB cable while FRIZZ plays.
- **Thirteen punch-in effects on the white keys:** freezer, pitch shifter, wavefolder, bitcrusher,
  DJ filter, flanger, phaser, resonator, slicer, wow & flutter, tape stop, tempo-synced delay and reverb. Hold a key to play an effect,
  hold it and press SHIFT to latch it. Knobs 1-4 shape the last effect you touched. A chaos
  key next to them drops latched effects out at random and scrambles the loop's steps.
- **Four FX scenes on the dark keys in the middle:** save every effect's settings and latches, recall them
  at once, drop to them hard on the next bar line, morph to them on the bar line or crossfade
  by hand on the transport knob, copy and delete them. Kept on the SD card. A fifth key clears every effect at once.
- **An input/loop mix** between the live input and the loop, plus input gain, output gain and a
  headphone feed that can carry the input alone, all on the VOLUME knob.
- **Three inputs:** AUX, the built-in microphone while no cable is in AUX, or a computer on
  USB, each with its own input gain. Each can go through the effects or **past them** (a drum
  machine or a finished song next to your loop), and be monitored on the outputs or only
  recorded.
- **A USB sound card:** on a computer, FRIZZ records its master into a DAW and plays the
  computer's sound as its third input, without a driver; and it lends the SD card to the
  computer as a drive while it plays.
- **A finalizer** on its own key, a master section with input and drive, a 3-band EQ,
  compression and width, and a limiter for loudness; a safety limiter on every output.
- **MIDI** over TRS or USB: clock for quantized loops and the tempo-synced effects, notes
  playing the keys, CCs setting every parameter, program changes recalling scenes, a MIDI
  channel per effect for DAW automation, and the loop lined up with a DAW's Start and Song
  Position. FRIZZ can send its clock, the loop's Start and Stop, and what you change on the
  panel as CCs too.

FRIZZ is installed from the SD card like any CHOMPI firmware. It doesn't touch the bootloader,
and you can go back to the stock firmware the same way.

## Get started

| | |
|---|---|
| [**Install**](INSTALL.md) | Put FRIZZ on your CHOMPI, and go back to stock firmware |
| [**Quick start**](QUICKSTART.md) | Your first loop and your first effects in ten minutes |
| [**Manual**](MANUAL.md) | Every control, every effect, every knob |

## For developers

| | |
|---|---|
| [`firmware/`](firmware/) | The FRIZZ source. Its [README](firmware/README.md) covers the toolchain, the build, the host test harness and debugging |
| [`docs/`](docs/) | Design notes: the looper spec and an overview of the effects across the CHOMPI firmwares |
| [`reference/`](reference/) | The original CHOMPI open-source release, kept unchanged: the stock TAPE, TEMPO and WAVE firmwares, the factory SD cards, the bootloader and the hardware files |

## How FRIZZ is made

FRIZZ is written with the help of AI coding agents (Anthropic's Claude Code), and most commits
say so (`Co-Authored-By: Claude`). The design is mine: what FRIZZ does, its controls and
gestures, how it should sound and feel, and what goes into a release. The agents write most of
the code and the tests and propose details, which I accept or change. Nothing goes in on trust
alone:

- **On the computer:** a virtual CHOMPI ([`firmware/twin/`](firmware/twin/)) runs the whole
  firmware, unchanged, and the host checks ([`firmware/test/`](firmware/test/)) hold it to
  about 2,700 individual checks in 35 test programs (keys, LEDs, MIDI, timing, the card, the
  audio), plus 31 scenarios played from power-on and a bit-for-bit comparison of the engine's
  sound.
- **On the CHOMPI:** every firmware change runs on the device before it's merged, with its
  CPU load measured against the last one.
- **By ear and hand:** every release is played with real gear first, from a public
  release-test checklist (the issues labelled `release test`).

The decisions behind each feature are in the [issues](https://github.com/ZUHcYa/FrizZ/issues)
and pull requests.

## Credits and license

FRIZZ builds on the [CHOMPI open-source release](https://github.com/CHOMPI-Club/CHOMPI) by
CHOMPI Club and was forked from its WAVE firmware. It runs on Electrosmith's Daisy platform
(libDaisy, DaisySP), and several effects are ported from Bastl Instruments' Kastle 2 FX
Wizard. Its USB audio and the card drive run on TinyUSB and build on lost-tty's
[MIXTAPE](https://github.com/lost-tty/CHOMPI), USB audio for TAPE (MIT).
[`THIRD_PARTY.md`](THIRD_PARTY.md) has the full list.

FRIZZ is an independent project, not an official CHOMPI Club release. The code is **MIT**, see
[`LICENSE`](LICENSE). The CHOMPI name and marks are not covered by that license, see
[`TRADEMARKS.md`](TRADEMARKS.md).
