# Authorship and Third-Party Notices

## Authorship

CHOMPI Club commissioned Electrosmith to engineer a production version of CHOMPI (2022-2023).
As such, Electrosmith is credited for the hardware, designed around Electrosmith's Daisy Seed2
DFM; the core firmware that ties it to the Daisy, including the TAPE firmware through version
1.0.9; as well as the original v6.2 bootloader.

TAPE 2.0, TEMPO and WAVE were then written at Chase Bliss on top of that platform, after CHOMPI
Club became part of Chase Bliss. CHOMPI Club, now part of Chase Bliss, owns this work and
released it under the MIT license; FRIZZ ([PUMPWERKER/FRIZZ](https://github.com/PUMPWERKER/FRIZZ))
was forked from WAVE, and the code here was moved out of FRIZZ (PUMPWERKER/FRIZZ#477), under
the same license (`LICENSE`). Electrosmith is credited as the designer of the hardware, and as
the author of the original firmware and platform, including the DSP and hardware-support source
they brought to the project.

## Third-party components

The work below is included under its own license. These notices, and the copyright attributions
in the source files, must be preserved in any redistribution.

| Component | Copyright | License | Where |
|---|---|---|---|
| **libDaisy** | © Electrosmith | MIT | `daisy/libs/libDaisy/`, with its `LICENSE`: Electrosmith's CHOMPI adaptation, patched (below) |
| **DaisySP** | © Electrosmith, Corp. | MIT | `daisy/libs/DaisySP/`, with its `LICENSE` |
| **STM32 HAL, USB Device/Host middleware, FatFs**, **CMSIS** | © STMicroelectronics; © ARM Limited | ST Ultimate Liberty / BSD-3-Clause, per file headers; Apache-2.0 | inside libDaisy (`daisy/libs/libDaisy/Drivers/`, `Middlewares/`) |
| **TinyUSB** 0.21.0 | © Ha Thach (tinyusb.org) | MIT | `daisy/libs/tinyusb/` (`src/` with the DWC2 driver as its only port, with its `LICENSE`): upstream's tag 0.21.0 byte for byte but for two patches made in FRIZZ, each a file in `patches/` and marked `FRIZZ` in the source (`VERSION` lists them). The platform's USB device, `daisy/UsbDevice.cpp` (PUMPWERKER/FRIZZ#196) |
| **lost-tty/CHOMPI ("MIXTAPE")**, USB audio and MIDI for TAPE | © 2026 CHOMPI Club; by nils (lost-tty), from [lost-tty/CHOMPI](https://github.com/lost-tty/CHOMPI) (commits a56545b, ab95f98) | MIT | the shape of the USB glue, each file saying what it takes in its head: `daisy/UsbDevice.cpp` (the PHY's bring-up, CRS, TinyUSB's task on TIM7, the audio FIFOs' glue, the drive's callbacks), `daisy/UsbMidiTransport.h` (libDaisy's MIDI transport on TinyUSB), `daisy/usb_descriptors.c` and `daisy/tusb_config.h` (how the functions sit together), `core/CardLend.h` (the card as a drive), and the `dcd_dwc2.c` fix in TinyUSB's `patches/0002` |
| **Mutable Instruments**' encoder classes | © Émilie Gillet | MIT | `boards/chompi/encoder.h`: its decoding influenced by them |

## libDaisy is Electrosmith's CHOMPI adaptation

libDaisy is vendored as Electrosmith's CHOMPI adaptation of v5.4.0. Don't replace
`daisy/libs/libDaisy/` with a stock version, and don't rebuild its archive as a whole. Against
the upstream v5.4.0 release, CHOMPI's copies differ in board and boot support: in all of them
`src/usbd/usbd_desc.c` (the USB device's identity strings) and `src/sys/system.h` (a
`BootInfo::Version` value for the CHOMPI bootloader); WAVE's, which this one starts from, adds
TIM16 as a fifth timer (`src/per/tim.h`, `tim.cpp`), two MIDI accessors (`src/hid/midi.h`:
`GetUartHandle`, `GetMutableTransport`) and a UART transmit path for tighter MIDI timing
(`src/per/uart.cpp`).

This copy (`daisy/libs/libDaisy/`) is WAVE's with eleven changes, made in FRIZZ:
- `src/usbd/usbd_desc.c` names the USB product "FrizZ".
- `src/hid/midi_parser.cpp` follows MIDI 1.0 where WAVE's didn't:
  - A real-time byte (a clock tick, 0xF8-0xFF) inside a SysEx or another message is passed on,
    and what it interrupted carries on (PUMPWERKER/FRIZZ#18).
  - A System Common message ends running status.
  - Its data bytes are counted by its own type, so a Song Position after a program change isn't
    read as one byte and a program change.
  - A status byte other than F7 inside a SysEx ends it, cut short, and starts a message of its
    own: the SysEx is dropped. It was kept as data, and everything up to the next F7 with it
    (PUMPWERKER/FRIZZ#166).
- `src/hid/usb_midi.cpp` sends a Song Position over USB once: WAVE's packed it, then fell
  through into its SysEx branch and packed it again, so hosts read it twice (PUMPWERKER/FRIZZ#157).
- `src/usbd/usbd_conf.c` starts EP0 IN afresh on every SETUP (`HAL_PCD_SetupStageCallback`):
  it clears a STALL left from the transfer before and flushes EP0's TxFIFO. ST's USB core
  stalls EP0 IN when the last packet of a reply is done; when the USB interrupt was answered
  late (the audio DMA's has the same priority) and the host's next SETUP had already come in,
  that STALL held into the next transfer, its reply stayed in the FIFO, and every reply after
  it went out one late: the USB names garbled (PUMPWERKER/FRIZZ#88).
- `src/sys/fatfs.cpp` and `src/sys/system.cpp` leave out the USB host: `FatFSInterface` no
  longer links a USB stick's driver for `MEDIA_USB` (the platform mounts the SD card only), and the
  OTG_HS interrupt handlers call only the device's handler (`HAL_PCD_IRQHandler`), not the
  host's. ST's USB host stack (`usbh_*`, `usb_host`, `stm32h7xx_hal_hcd`) isn't linked any
  more: 6.4 KB of code and 0.7 KB of data (PUMPWERKER/FRIZZ#196).
- `src/sys/system.cpp` has no OTG_HS interrupt handlers at all: the USB-C port runs on
  TinyUSB, whose handler is the platform's own (`daisy/UsbDevice.cpp`), so ST's device
  stack isn't linked either; libDaisy's own USB on that port (`UsbHandle`, `MidiUsbHandler`)
  doesn't work in this copy (PUMPWERKER/FRIZZ#196). The usbd changes below stay in the source and the archive,
  unused by the firmwares.
- `src/usbd/usbd_conf.h` and `src/usbd/usbd_conf.c` give the CDC class its handle (540 B) from a
  static buffer, `USBD_static_malloc`, as ST's own templates do, instead of `malloc`: newlib's
  `_sbrk` grows the heap from the end of `.bss` without a bound, so with internal RAM full it
  would run past it at boot (a freeze in the fork lost-tty/CHOMPI), where the buffer fails the link.
  One handle, for the one USB device; it's the same buffer on every Init, and freeing it
  does nothing (PUMPWERKER/FRIZZ#196).
- `src/per/uart.cpp` hands over what the MIDI jack's UART received at once when a clock tick
  (0xF8) comes in, as it does when the line goes quiet: `DmaListenStart` sets the USART's
  character match on 0xF8 (`CR2.ADD`, `ADDM7`, `CR1.CMIE`), and `UART_IRQHandler` takes the
  match like IDLE, once the DMA has the byte. Before, a tick followed closely by other bytes
  reached the parser only after them, 0.32 ms late a byte (PUMPWERKER/FRIZZ#322). Listening (`DmaListenStart`)
  is MIDI's alone on the platform.
- `src/per/uart.cpp` also hands over at each half of the listener's 256-byte ring, the DMA's
  half and full transfer: WAVE turned DMA1 Stream 5's interrupt off (`HAL_NVIC_DisableIRQ` in
  `DmaListenStart`, not in libDaisy itself), so a line that never paused for a byte's time and
  carried no tick wrapped the ring, more than 256 bytes and the laps were lost (PUMPWERKER/FRIZZ#356).
  `DmaListenStart` enables it with only the half and full transfer (TE and DME off: HAL's
  `UART_DMAError` would end the reception), and `DMA1_Stream5_IRQHandler` takes them while
  listening by the stream's own flags, not HAL's handler, which skips them once
  `dma_active_peripheral_` is -1 (after a DMA transmit, as WAVE's MIDI out made: the likely
  reason WAVE had it off, the interrupt pending for ever). The platform sends the jack's bytes itself
  (`MidiClock::TrsPut`), no DMA transmit. Same priority as the USART's interrupt (0), so the two
  never run into each other.
- `src/per/uart.cpp` starts the jack's receive again after a receive error (framing, noise,
  overrun: a cable plugged in while the other end sends): HAL takes any error during DMA
  reception as blocking and ends it, and `MidiHandler::Listen` starts it again only once
  `IsListening()` is false, which nothing set. `UART_IRQHandler` clears `listener_mode_` when HAL
  has ended the reception, and `HAL_UART_ErrorCallback` too, as upstream does since f7c63aee
  (libDaisy's #662); `DMA1_Stream5_IRQHandler` takes the aborted stream's flags itself meanwhile, so the
  abort's end doesn't reach `HAL_UART_Init` from the interrupt (PUMPWERKER/FRIZZ#361).
- `src/per/uart.cpp` leaves the USART on when the jack's receive starts again: turning `UE`
  off stops the transmitter at once too, cutting the byte MIDI out is sending (`MidiClock::TrsPut`),
  which `DmaListenStart` did each time to write `CR2.ADD`. `per/uart_listen.h` (made for FRIZZ's
  host check `uartlisten`) writes it only when CR2 doesn't hold it already (CR2 keeps it:
  `HAL_UART_Init`'s `UART_SetConfig` writes only STOP there), so only the first start turns the
  USART off. It clears the receive errors (ORE, NE, FE, PE) in ICR itself, as turning UE off did:
  bytes that came in while the reception was ended leave an overrun, which
  `HAL_UART_Receive_DMA` doesn't clear, and it would end the new reception at once. And an error of the listening handle that reaches `HAL_UART_ErrorCallback`
  (`DmaTransferFinished`) clears its flags in ICR instead of `HAL_UART_Init`, which turns it off
  as well; the start after it sets up what the receive needs (PUMPWERKER/FRIZZ#442).
- Code that no interrupt and not the audio callback runs is built for size (`-Os`), for code
  room (PUMPWERKER/FRIZZ#323); libDaisy builds everything `-O2`. Fourteen objects are built `-Os` whole:
  `UI.o`, `gpio.o`, `system.o`, `daisy_seed.o`, `codec_wm8731.o` and the HAL's `rcc`, `rcc_ex`,
  `qspi`, `ll_fmc`, `uart_ex`, `i2c_ex`, `tim_ex`, `pwr` and `stm32h7xx_hal.o`. In nine objects
  that do run in an interrupt, a `#pragma GCC optimize("Os")` (marked `FRIZZ` in the source, PUMPWERKER/FRIZZ#323) builds
  only the functions Init runs, or a blocking call from `MainLoop()`: the HAL UART's
  `UART_SetConfig` (5.3 KB at `-O2`, reached from the MIDI interrupt only through an error's
  `HAL_UART_Init`), the SAI's `HAL_SAI_Init`, `HAL_SAI_InitProtocol`, `HAL_SAI_Transmit_DMA`
  and `HAL_SAI_Receive_DMA`, `sai.cpp`'s `Init`, `InitPins` and `StartDmaTransfer`,
  `audio.cpp`'s `Init`s and `Start`, `tim.cpp`'s `Init` and `HAL_TIM_Base_MspInit`, the HAL
  timer's `TIM_Base_SetConfig` and `HAL_TIM_ConfigClockSource`, the HAL I2C's blocking
  `HAL_I2C_Master_Transmit` and `HAL_I2C_Slave_Transmit` with their waits, `i2c.cpp`'s `Init`
  and `ReceiveDma`, and `HAL_GPIO_DeInit`. What runs per block or in an interrupt stays as it
  was: in every one of these objects, each function the audio, MIDI, USB, LED (the PWM's DMA),
  battery (I2C), timer, SysTick or SD interrupts or the audio callback reach disassembles as
  before (`HAL_UART_IRQHandler`, `InternalCallback`, `HAL_DMA_IRQHandler`, the SAI's and the
  UART's DMA callbacks, `MidiParser::Parse` among them); an object where one of those changed
  at `-Os` stays `-O2` (the PR's table, PUMPWERKER/FRIZZ#323), but for five whose changed functions never run
  per block or per event: `stm32h7xx_hal_rcc` and `rcc_ex` (the MIDI interrupt reaches them only
  through a UART error's `HAL_UART_Init`), `UI.o` (the audio callback runs `UI::Process` only
  while booting, before `MainLoop()`), `stm32h7xx_hal_qspi` (QSPI is memory-mapped with its
  timeout off and no transfer is started, so its interrupt never fires) and `system.o` (only
  `HardFault_Handler` changed). The SD card's objects (`stm32h7xx_hal_sd`,
  `ll_sdmmc`, `sdmmc`, `sd_diskio`, `bsp_sd_diskio`) stay `-O2` all of them: the GCC pin is
  about the card's communication. This changes compilation, not behaviour.

Its prebuilt `build/libdaisy.a` is WAVE's with only the objects of those changes rebuilt:
`usbd_desc.o`, `midi_parser.o`, `usb_midi.o`, `usbd_conf.o`, `fatfs.o`, `system.o` (twice, the
second time without the handlers: from the source before, it disassembled as the archive's),
`usbd_cdc.o` (whose code changes with `usbd_conf.h`'s macro; of the other objects that include
it none does), `uart.o` (from the source before PUMPWERKER/FRIZZ#322, and again before PUMPWERKER/FRIZZ#356, it disassembled as the archive's; rebuilt into PUMPWERKER/FRIZZ#323's archive at libDaisy's `-O2`, and for PUMPWERKER/FRIZZ#361 and PUMPWERKER/FRIZZ#442; for PUMPWERKER/FRIZZ#442 only `DmaListenStart`, `DmaTransferFinished` and the callbacks that inline it changed, 104 B less), and
for PUMPWERKER/FRIZZ#323 the fourteen objects built `-Os` (`make OPT=-Os build/src/ui/UI.o`) and the nine with
the pragmas (at libDaisy's own `-O2`). They were built with GCC 10.3 and libDaisy's own flags (`make
build/src/hid/midi_parser.o` in its folder, then `arm-none-eabi-ar r build/libdaisy.a
build/src/hid/midi_parser.o`; delete the object first to rebuild it, `make -B` fails on the
`build` folder). Each of the other objects FRIZZ links, rebuilt the same way, disassembles as
the archive's (checked for all of them, PUMPWERKER/FRIZZ#323). The loose objects in `build/src/` stay WAVE's: only the archive is linked.
These reproduce WAVE's objects from WAVE's source: the same code, only the debug information's
paths differ, and FRIZZ.bin comes out byte for byte the same (checked for `usb_midi.o`, PUMPWERKER/FRIZZ#157,
and `usbd_conf.o`, PUMPWERKER/FRIZZ#88; `midi_parser.o` rebuilt from the source before PUMPWERKER/FRIZZ#166, and `fatfs.o`,
`system.o` and every object that includes `usbd_conf.h`, rebuilt from the source before PUMPWERKER/FRIZZ#196,
disassemble as the archive's). A changed `libdaisy.a` (or `libdaisysp.a`) links a firmware again
on its next `make` (FRIZZ's Makefile, PUMPWERKER/FRIZZ#161).

The platform doesn't use `src/util/sd_diskio.c`'s `SD_read` and `SD_write` (they stay in the archive,
unchanged): its own, `core/SdDisk.h`, take their place in FatFs's driver table
after `FatFSInterface::Init` (`daisy/SdCard.h`'s `SdDiskLink`, PUMPWERKER/FRIZZ#213), the rest of the driver (`SD_initialize`,
`SD_status`, `SD_ioctl`) being sd_diskio.c's. They make the same transfers, but end a wait on
an SD error after 1 s and recover the card, where sd_diskio.c waited 30 s. FRIZZ's `firmware/test/sddisk.cpp`
runs both, with FatFs, on the host.
## A note on the vendored libraries

Daisy is an open-source hardware and software platform by Electrosmith; libDaisy and DaisySP
are published by Electrosmith under the MIT license. The copies in this repository keep their
original copyright headers, license files and author attributions; nothing has been removed
from them. libDaisy is the CHOMPI adaptation described above, and DaisySP is as shipped with the
CHOMPI firmware (it carries no version tag).
