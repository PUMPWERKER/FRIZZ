# The PUMPWERKER platform

What every PUMPWERKER firmware for the CHOMPI hardware needs, whatever it plays: the boot over
the multi-firmware launcher, the keys, knobs and LEDs, the SD card, USB (TinyUSB: a UAC1 sound
card, MIDI, the card as a drive), the card lent to a computer while the firmware plays on, the
crash record and the watchdog, MIDI over the TRS jacks, and the patched libDaisy and DaisySP it
all stands on. A firmware brings its sound, its pages and its controls.

The code comes from [FRIZZ](https://github.com/PUMPWERKER/FRIZZ), moved here unchanged
([PUMPWERKER/FRIZZ#477](https://github.com/PUMPWERKER/FRIZZ/issues/477)). Issue numbers in its
comments (`#196`) are FRIZZ's.

## Three layers

| Folder | What | May include |
|---|---|---|
| `core/` | independent of the device: logic that runs and is tested on any computer | `core/` only |
| `daisy/` | any device on a Daisy Seed (STM32H750): the USB device, the SD card, MIDI's UART, the crash record; the vendored libraries in `daisy/libs/` | `core/`, `daisy/` |
| `boards/chompi/` | the CHOMPI's hardware: its pins, panel (keys, knobs, LEDs), microphone, boot screen and memory layout for the launcher's bootloader | all three |

`core/` includes nothing from `daisy/` or `boards/`, and `daisy/` nothing from `boards/`. A
file sits in the layer its content belongs to, as low as its includes allow. What would
need a seam to go lower stays where it fits today (below).

What's in them now:

- `core/`
  - `CardLend.h`: the card lent to the computer, the state a firmware shows and asks.
  - `LedSignal.h`: short LED signals (3 blinks, a flash) that end by themselves.
  - `SdDisk.h`: FatFs's reads and writes of the card with a timeout and recovery, pure logic.
  - `TapTempo.h`: a tempo from taps.
- `daisy/`
  - `Crash.h`: what a crash leaves for the next start, and the watchdog.
  - `MidiClock.h`: MIDI in over the jack's UART and USB, the clock's ticks and period.
  - `TempoClock.h`: the tempo and its clock pulses, from a loop, MIDI's clock or taps.
  - `SdCard.h`: the SDMMC's settings, reads of whole runs of sectors, and `SdDisk.h`'s link to
    FatFs.
  - `UsbDevice.h`, `UsbDevice.cpp`, `UsbMidiTransport.h`, `usb_descriptors.c`,
    `usb_descriptors.h`, `tusb_config.h`: the USB device on TinyUSB (audio, MIDI, the drive).
  - `libs/`: libDaisy (Electrosmith's CHOMPI adaptation, patched), DaisySP and TinyUSB
    0.21.0 (`THIRD_PARTY.md`).
- `boards/chompi/`
  - `hardware.h`: pins, shift registers, encoders, the charger, the battery lockout.
  - `encoder.h`, `encoder.cpp`: the knobs' encoders.
  - `temp_led_stuff.h`, `PanelLeds.h`: the LED chains and their brightness.
  - `PanelKeys.h`: the keys in their places on the panel, and each one's LED.
  - `BootPage.h`: the boot ripple.
  - `MicFilter.h`: the built-in microphone's filter.
  - `chompi_sram.lds`: the memory layout for CHOMPI's bootloader (`APP_TYPE=BOOT_SRAM`).

Candidates for a lower layer, each a change of code for later:

- `TempoClock.h` is core's content but takes a `MidiClock*`: a clock interface would let it go
  to `core/`.
- `MidiClock.h`'s clock half (the period, the clock factor) is core's, its receive half daisy's.
- `Crash.h`'s record and CRC are core's, its RAM ranges the H750's.
- `encoder.cpp`'s libDaisy stubs are daisy's.
- `usb_descriptors.c`'s maker string and VID:PID are the board's, its names and channel
  counts the firmware's: configuration points.
- `CardLend.h` names `LedSignal` without its namespace, so it compiles only after a
  `using namespace chompi`.

## A firmware takes a copy with a pin

A firmware doesn't take this repository as a submodule. It keeps a copy of one of its
commits, real files in `firmware/platform/`, laid out as here, and that commit's hash in
`firmware/platform/PIN`. So a firmware's tag builds from the firmware's repository alone, and
everything that reads a firmware's commit (`git worktree add`, `git archive`, CI's comparison
with a pull request's base) needs nothing else.

- **A change to the platform** is a pull request here first. Once it's merged, each firmware
  copies the new commit in, in a pull request of its own, and its checks run on it.
- **Copying a commit in:** FRIZZ's `firmware/tools/platform-sync.py COMMIT` replaces
  `firmware/platform/` with that commit's files and writes `PIN`. From a clone of this
  repository next to the firmware's checkout (`../platform`), else fetched from GitHub. The
  commit must be on GitHub, where every firmware's CI fetches it.
- **The check:** `platform-sync.py --check` compares the copy with `PIN`'s commit, file by
  file (content and mode), both ways. FRIZZ runs it in its host checks (`all.sh`, check
  `platform`). Nobody edits the copy inside a firmware: the check fails until the change has
  come through here.
- **PIN names a commit on `main`.** While a pull request here is open, a firmware's draft may
  pin its head to try it; before the firmware's pull request is ready, the platform's is merged
  and PIN names that commit on `main`. `--check` notes a PIN that isn't on `main`.
- **A pinned commit stays reachable:** pull requests here are merged with a merge commit (no
  squash, no rebase), and `main` is protected against force-pushes and deletion, as a
  firmware's tag is built again from its pin.

Each firmware gives its build these paths (FRIZZ's `firmware/code/src/Makefile`):

```make
PLATFORM = ../../platform
LIBDAISY_DIR = $(PLATFORM)/daisy/libs/libDaisy
DAISYSP_DIR = $(PLATFORM)/daisy/libs/DaisySP
TINYUSB_DIR = $(PLATFORM)/daisy/libs/tinyusb/src
LDSCRIPT = $(PLATFORM)/boards/chompi/chompi_sram.lds
C_INCLUDES = -I. -I$(PLATFORM)/core -I$(PLATFORM)/daisy -I$(PLATFORM)/boards/chompi -I$(TINYUSB_DIR)
```

and the platform's sources (`daisy/usb_descriptors.c`, `daisy/UsbDevice.cpp`,
`boards/chompi/encoder.cpp`) among its own. A firmware's file names stay different from the
platform's: the build finds objects by their file name, and the host checks copy both into one
folder. The toolchain is GNU Arm Embedded 10.3-2021.10 (GCC 10.3.1): newer GCC intermittently
breaks the SD card's communication, and the pin makes a build's bytes the same on every
machine.

## Names

The platform's macros say `PLATFORM_` (`PLATFORM_USB_CHANNELS_RX`, `PLATFORM_USB_SAFETY`,
`PLATFORM_DEV_TESTS`), its comments "the firmware" or "the device"; FRIZZ is named where a
fact is FRIZZ's (where the code came from, its settings page, its file names). Still FRIZZ's
today, configuration points to come: the USB product names ("FrizZ", "FrizZ card"), the
drive's vendor (`"FrizZ   "`), and the section names `.frizz_after_run*` in the linker
script, which FRIZZ's `FrizzHot.h` fills.

## Features are chosen at compile time

Every bigger part will be a choice a firmware makes when it's built, so one that doesn't use it
carries none of its code: USB audio's channels in and out (0 for none), the drive and the card
lent while playing, and the developers' test signal. Today the platform builds them all, as
FRIZZ uses them.

## License

MIT (`LICENSE`), as FRIZZ. The vendored libraries keep their own licenses and notices
(`THIRD_PARTY.md`). CHOMPI is CHOMPI Club's mark (`TRADEMARKS.md`); it names only the board's
folder and the hardware.
