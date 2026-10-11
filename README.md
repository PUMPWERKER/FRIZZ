# FRIZZ

**FRIZZ** is an alternative firmware for the CHOMPI. It turns the CHOMPI into a stereo effects
box and looper for whatever you plug into its AUX input.

- **A looper:** free-length loops, or loops locked to whole bars of an incoming MIDI clock.
  Varispeed in semitones (fifths and octaves with SHIFT), reverse and scrub on the big
  transport knob.
- **Twelve punch-in effects on the white keys:** freezer, pitch shifter, wavefolder, bitcrusher,
  DJ filter, flanger, resonator, slicer, wow & flutter, tape stop, tempo-synced delay and reverb. Hold a key to play an effect,
  hold it and press SHIFT to latch it. Knobs 1-4 shape the last effect you touched. A chaos
  key next to them drops latched effects out at random and scrambles the loop's steps.
- **Four FX scenes on the dark keys:** save every effect's settings and latches, recall them
  at once for a drop, morph to them on the bar line or crossfade by hand on the transport
  knob, copy and delete them. Kept on the SD card. A fifth key clears every effect at once.
- **An input/loop mix** between the live input and the loop, plus input gain, output gain and a
  headphone feed that can carry the dry input alone, all on the VOLUME knob.
- **A master compressor** on its own key, and a safety limiter on every output.
- **MIDI** over TRS or USB: clock for quantized loops and the tempo-synced effects; from
  v0.11 also notes playing the keys, CCs setting every parameter, program changes recalling scenes;
  from v0.12 FRIZZ can send its clock and the loop's Start and Stop too.

FRIZZ is installed from the SD card like any CHOMPI firmware. It doesn't touch the bootloader,
and you can go back to the stock firmware the same way.

## Get started

| | |
|---|---|
| [**Install**](INSTALL.md) | Put FRIZZ on your CHOMPI, and go back to stock firmware |
| [**Quick guide**](QUICKSTART.md) | Your first loop and your first effects in ten minutes |
| [**Manual**](MANUAL.md) | Every control, every effect, every knob |

## For developers

| | |
|---|---|
| [`firmware/`](firmware/) | The FRIZZ source. Its [README](firmware/README.md) covers the toolchain, the build, the host test harness and debugging |
| [`docs/`](docs/) | Design notes: the looper spec and an overview of the effects across the CHOMPI firmwares |
| [`reference/`](reference/) | The original CHOMPI open-source release, kept unchanged: the stock TAPE, TEMPO and WAVE firmwares, the factory SD cards, the bootloader and the hardware files |

## Credits and license

FRIZZ builds on the [CHOMPI open-source release](https://github.com/CHOMPI-Club/CHOMPI) by
CHOMPI Club and was forked from its WAVE firmware. It runs on Electrosmith's Daisy platform
(libDaisy, DaisySP), and several effects are ported from Bastl Instruments' Kastle 2 FX
Wizard. [`THIRD_PARTY.md`](THIRD_PARTY.md) has the full list.

FRIZZ is an independent project, not an official CHOMPI Club release. The code is **MIT**, see
[`LICENSE`](LICENSE). The CHOMPI name and marks are not covered by that license, see
[`TRADEMARKS.md`](TRADEMARKS.md).
