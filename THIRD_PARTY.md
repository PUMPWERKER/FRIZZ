# Authorship and Third-Party Notices

## Authorship

CHOMPI Club commissioned Electrosmith to engineer a production version of CHOMPI (2022-2023).
As such, Electrosmith is credited for the hardware in `reference/hardware/hardware-pcb/`, designed around
Electrosmith's Daisy Seed2 DFM; the core firmware that ties it to the Daisy,
including the TAPE firmware through version 1.0.9; as well as the original v6.2 bootloader.

TAPE 2.0, TEMPO and WAVE were then written at Chase Bliss on top of that platform,
after CHOMPI Club became part of Chase Bliss.

CHOMPI Club, now part of Chase Bliss, owns this work and releases it under the MIT license in
`LICENSE`. Electrosmith is credited as the designer of the hardware, as the author of the original
firmware and platform — including the DSP and hardware-support source they brought to the
project — and as the author of the Daisy Bootloader the CHOMPI bootloader is built on.

## Third-party components

The work below is included under its own license. These notices, and the copyright attributions
in the source files, must be preserved in any redistribution.

| Component | Copyright | License | Where |
|---|---|---|---|
| **libDaisy** | © Electrosmith | MIT | `code/libs/libDaisy/` in each firmware folder, and `reference/firmware/chompi-bootloader-v6.4-beta/libs/libDaisy/` — each with its `LICENSE` |
| **DaisySP** | © Electrosmith, Corp. | MIT | `code/libs/DaisySP/` in each firmware folder, with its `LICENSE` |
| **coreJSON** | FreeRTOS / Amazon | MIT | `code/libs/coreJSON/` in each firmware folder, with its `LICENSE` |
| **Daisy Bootloader** | © Electrosmith | MIT | `reference/firmware/chompi-bootloader-v6.4-beta/shared/`, and the v6.2 bootloader in each stock firmware's `code/Chompi_Bootloader/` under `reference/firmware/` |
| **CMSIS** | © ARM Limited | Apache-2.0 | `cube_dfu/Drivers/CMSIS/` (with its `LICENSE.txt`) in `reference/firmware/chompi-bootloader-v6.4-beta/` and in each stock firmware's `code/Chompi_Bootloader/` under `reference/firmware/` |
| **Kastle 2 FX Wizard** (freezer, slicer, flanger, shifter, resonator comb, crusher XOR and dive) | © 2024 Marek Mach, Vaclav Mach (Bastl Instruments) | MIT | ported to `firmware/code/src/` (`FxFreezer.h`, `FxSlicer.h`, `FxFlanger.h`, `FxShifter.h`, `FxResonator.h`, and the crusher's in `FxCrusher.h`), with the license in `LICENSE-kastle2` next to them |
| **CHOMPI launcher's `midi-send.py`** | © 2026 CHOMPI Club; by ugrossek (chomplex music theory), from [sfaber02/CHOMPI-MULTI-FIRMWARE](https://github.com/sfaber02/CHOMPI-MULTI-FIRMWARE) (launcher v1.5) | MIT | `firmware/tools/midi_send.py`, unchanged but for a note of where it's from; `firmware/flash.py` and `firmware/tools/chompi.py` use it |
| **STM32 HAL, USB Device/Host middleware, FatFs** | © STMicroelectronics | ST Ultimate Liberty / BSD-3-Clause, per file headers | the same `cube_dfu/` folders |

## libDaisy is Electrosmith's CHOMPI adaptation

libDaisy is vendored as Electrosmith's CHOMPI adaptation of v5.4.0, and
each firmware depends on its own copy — don't replace a `code/libs/libDaisy/` with a stock
version. Against the upstream v5.4.0 release, the copies differ in board and boot support.
The changes a downstream user most needs to know about:

In all three:
- `src/usbd/usbd_desc.c` — USB device identity strings for this product.
- `src/sys/system.h` — an additional `BootInfo::Version` enum value for the CHOMPI bootloader.

In TAPE:
- `src/hid/midi.h` — MIDI send helpers (`SendNoteOn`, `SendNoteOff`, …) and a transport reset
  that the firmware calls. The build fails without them.
- `src/per/tim.h` / `src/per/tim.cpp` — timer init and start compiled at `-O0`, and the
  auto-reload preload disabled. This changes behavior, not compilation; keep the vendored files.

In TEMPO and WAVE:
- `src/per/tim.h` / `src/per/tim.cpp` — adds TIM16 as a fifth available timer (clock enable,
  NVIC priority, IRQ handler). TEMPO's clock and WAVE's MIDI clock run on it; the build fails
  without it.
- `src/hid/midi.h` — two added accessors (`GetUartHandle`, `GetMutableTransport`) used for the
  MIDI output path. The build fails without them.
- `src/per/uart.cpp` — the UART transmit path is modified for tighter MIDI timing
  (interrupt-blocking guards removed, direct-send path). This changes behavior, not
  compilation: the firmware builds against pristine upstream, but its MIDI output timing won't
  match the released firmware. Keep the vendored file.

FRIZZ's copy (`firmware/code/libs/libDaisy/`) is WAVE's with three changes:
- `src/usbd/usbd_desc.c` names the USB product "FrizZ".
- `src/hid/midi_parser.cpp` follows MIDI 1.0 where WAVE's didn't:
  - A real-time byte (a clock tick, 0xF8-0xFF) inside a SysEx or another message is passed on,
    and what it interrupted carries on (#18).
  - A System Common message ends running status.
  - Its data bytes are counted by its own type, so a Song Position after a program change isn't
    read as one byte and a program change.
  - A status byte other than F7 inside a SysEx ends it, cut short, and starts a message of its
    own: the SysEx is dropped. It was kept as data, and everything up to the next F7 with it
    (#166).
- `src/hid/usb_midi.cpp` sends a Song Position over USB once: WAVE's packed it, then fell
  through into its SysEx branch and packed it again, so hosts read it twice (#157).

Its prebuilt `build/libdaisy.a` is WAVE's with only those three objects, `usbd_desc.o`,
`midi_parser.o` and `usb_midi.o`, rebuilt from them. They were built with GCC 10.3 and
libDaisy's own flags (`make build/midi_parser.o` in its folder, then `arm-none-eabi-ar r
build/libdaisy.a build/midi_parser.o`). These reproduce WAVE's objects from WAVE's source: the
same code, only the debug information's paths differ, and FRIZZ.bin comes out byte for byte
the same (checked for `usb_midi.o`, #157; `midi_parser.o` rebuilt from the source before #166
disassembles as the archive's). A changed `libdaisy.a` (or `libdaisysp.a`) links
FRIZZ again on the next `make` (#161).

The bootloader's copy, `reference/firmware/chompi-bootloader-v6.4-beta/libs/libDaisy/`, is the same adaptation plus
the changes that bootloader needs, listed in its `LIBDAISY_PATCH.md`.

## DSP derived from Mutable Instruments

Portions of the FX engine in TAPE, TEMPO, WAVE and FRIZZ derive from Émilie Gillet's open-source work
(Mutable Instruments), released under the MIT license. In each firmware folder's `code/src/`:

- `reverb.h` — carries the full original MIT notice. Do not remove it.
- `fx_engine.h` — ported from `pichenettes/eurorack` (`plaits/dsp/fx/fx_engine.h`), original
  code by Émilie Gillet, 2014.
- `limiter.h` — extracted from `pichenettes/stmlib`.
- `encoder.h` — encoder decoding influenced by the Mutable Instruments encoder classes.

## The bootloader

`reference/firmware/chompi-bootloader-v6.4-beta/shared/` holds Electrosmith's Daisy Bootloader v6.4 source.
`dfu.cpp`, `dfu.h` and `dfu_log.h` are theirs unchanged; CHOMPI's additions are confined to
`bootloader.cpp` and `bootloader.h`, and are marked with `CHOMPI:` comments.

## Hardware

The boards in `reference/hardware/hardware-pcb/` are built around the Electrosmith Daisy Seed2 DFM;
Electrosmith publishes its pinout and documentation. Most of the other assembly
parts are listed with their manufacturer part numbers in `reference/hardware/hardware-pcb/CHOMPI_Rev4_BOM.csv`. The
EAGLE libraries used in the project are embedded in the `.sch` and `.brd` files.

## A note on the vendored libraries

Daisy is an open-source hardware and software platform by Electrosmith; libDaisy, DaisySP and
the Daisy Bootloader are published by Electrosmith under the MIT license. The copies in this
repository keep their original copyright headers, license files and author attributions;
nothing has been removed from them. libDaisy is the CHOMPI adaptation described above, DaisySP
is as shipped with the CHOMPI firmware (it carries no version tag), and coreJSON is v3.2.0 as
published.
