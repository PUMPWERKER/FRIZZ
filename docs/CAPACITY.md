# Capacity: what's free on the CHOMPI, and how to make room

What a new feature can still use, measured on 2026-10-14 after the loop loader (#70) and
with FRIZZ's USB on TinyUSB, a sound card and the card as a drive (#196; the history below). `make` prints the memory table after every build; the numbers below come from it and
from `build/FRIZZ.map`.

## Memory

| Region | Size | Used | Free | What's in it |
|---|---|---|---|---|
| `SRAM_EXEC` (AXI SRAM) | 272 KB | 263,744 B (95 %) | **~14 KB** | all the code, and the initial values of `.data` |
| `SRAM` (AXI SRAM) | 240 KB | 220,172 B (90 %) | ~25 KB | `.data` and `.bss`: the engine (101 KB), the two MIDI receive queues (`midi_clock`, 74 KB), the event log's index (9.5 KB), the scene store (5.7 KB), libDaisy's buffers |
| `DTCMRAM` | 128 KB | 72,752 B | ~58 KB, shared with the stack | the reverb (64 KB), the delay's output lines (6 KB, #185), FRIZZ's copy of the vector table (1 KB, 1 KB aligned: the faults' handler, `Crash.h`, #105); the stack grows down from its top |
| `RAM_D2` | 32 KB | 22,312 B | ~10 KB | DMA buffers (audio, LEDs, SD), not cached |
| `RAM_D2CACHE` | 256 KB | 0 | **256 KB** | nothing (the `.d2_bss` section points there) |
| `ITCMRAM` | 64 KB | 0 (~40 KB with `make ITCM=1`) | 64 KB | the per-sample code with `make ITCM=1` (`FRIZZ_HOT`, `FrizzHot.h`, #51), copied there at boot; off by default |
| `RAM_D3` | 64 KB | 0 | 64 KB | nothing |
| `BACKUP_SRAM` | 4 KB | 92 B | ~4 KB | libDaisy's `boot_info` (12 B, first: the bootloader and the launcher hand over through it), then the crash record and the build running (80 B, `Crash.h`, #105); a reset leaves it, a power-off doesn't |
| `SDRAM` | 64 MB | 43.9 MB | **~23 MB** | the loop (31.7 MB), the tape stop (4.2 MB), the delay (3.8 MB), the event log (2.1 MB), the freezer (1.9 MB), the reverb's pre-delay (0.1 MB); measured on `FRIZZ.bin` md5 `1eb11808` |

**The loop loader (#70, for 1.1)** adds a second loop buffer, 16 bars at 50 BPM (14.75 MB,
`.sdram_noinit.later`, linked after the loop and the event log so they keep their addresses),
and its 3.9 KB beside it: SDRAM 58.6 MB (87.4 %), **~8.5 MB** free. In FRIZZ.bin ~5 KB of code
(~2.2 KB of it FatFs's fast seek and folder reading, linked for the first time) and 124 B of
data; the bench build hasn't the loader.

**USB on TinyUSB (#196, 2026-10-14).** FRIZZ's USB runs on TinyUSB 0.21.0 instead of ST's
device stack: a USB Audio Class 1 sound card, MIDI and the card as a drive (MSC). TinyUSB's
core, its DWC2 driver and the three classes take ~19 KB of code at `-Os`, FRIZZ's glue
(`UsbDevice.cpp`, `-Os` but for the audio callback's `Receive` and `Send`) ~2.3 KB, the
descriptors ~0.8 KB; ST's stack, its CDC class and the `printf` its debug level pulled in
(#271) are gone. Net: `FRIZZ.bin` 261,032 → 268,024 B of code (+6,992 B, **~10 KB free**),
226,568 → 220,172 B of data (−6,396: the CDC buffers gone, TinyUSB's FIFOs and the drive's
4 KB buffer in), `RAM_D2` −1,496 B; the bench build 267,216 → 274,104 B (+6,888 B, **~4.3 KB
free**) and 234,664 → 228,204 B of data (**~17 KB free**); SDRAM +4 KB (the drive's
transfer buffer, `.sdram_bss`). The three input sources and the settings page's new keys are
in those figures (from `main` 962505f).

**The USB host stack dropped (#196, 2026-10-14).** libDaisy linked ST's USB host stack (a USB
stick's driver for FatFs, the host's interrupt handler) though FRIZZ is a USB device only:
without it (`THIRD_PARTY.md`), 6,556 B less code in both builds, `FRIZZ.bin` 267,588 →
261,032 B and the bench build 273,772 → 267,216 B (it had ~4.7 KB left), and 732 B less data.
The CDC class's handle (540 B) moved from the heap into `.bss` in the same change, so the data
counts it now: 226,760 → 226,568 B, the bench build's 234,856 → 234,664 B (from `main`
b608028).

**The bench build hits the walls first.** `FRIZZ-bench.bin` carries the bench as well: its
code is at 269,744 B (97 %, **~8.6 KB free**, since the card code's `-Os`, step 3 below) and its data at 228,204 B (93 %, **~17 KB
free**). So a feature of more than ~8 KB of code, or ~17 KB of data in internal RAM, needs
room made first (below), or the bench build stops linking before `FRIZZ.bin` does. Data grows into
SDRAM or DTCM where it can; for internal RAM, step 4 below frees 64 KB.

Page 2's Mix, Band and Level on every effect and the compressor's page 2 (#40, #38,
`FxOutput.h`) then took ~7.3 KB of code, and each effect's own page-2 knob (#40) ~3.8 KB
more: `FRIZZ.bin` at 228,412 B (~50 KB free), the bench build at 246,508 B (~32 KB) with its
data at 239,092 B (~6.5 KB free). The reverb's pre-delay took 128 KB of SDRAM. MIDI out,
the chaos key, the crossfader and the settings page's later keys took the code to 241,588 B
(~36 KB free) and the bench build's to 259,588 B (~18 KB), until the play page's logic went
`-Os` (step 3): -15.5 KB in both.

The engine cleanup (#90, 2026-10-11, after #86 and #89) gave some back: `FRIZZ.bin` at
216,828 B of code (~60 KB free) and 223,424 B of data (~22 KB free), the bench build at
233,036 B (~44 KB) and 231,332 B (**~14 KB free**), from 218,108 / 228,032 and 235,140 /
240,068 on `main` (cee7f72).
The event log's copies of the card's files (4.5 KB, read only for a bug report) moved into
its SDRAM (`EventLogMem`), the bench writes `cpu.txt` through a 4 KB window instead of an
8 KB buffer, and `frizz_master.txt`'s lines are a table (1.6 KB of code). In SDRAM the loop
and the event log moved to `.sdram_noinit` (`SDRAM_NOINIT`, `chompi_sram.lds`): always
written before they're read, they aren't cleared at boot, so `ZeroSDRAM` clears 10.1 MB
instead of 43.9.

The phaser (#68, 2026-10-11), a 14th effect, took 2.4 KB of code and 0.8 KB of data
(1.0 KB in the bench build): `FRIZZ.bin` at 219,444 B of code (~58 KB free) and 224,216 B
of data (~21 KB free), the bench build at 235,708 B (~42 KB) and 232,348 B (**~13 KB
free**), from 217,076 / 223,440 and 233,252 / 231,348 on `main` (56685b0). The data is the
14th effect's share of every per-effect table (page 2's `FxOutput`, the meters, the knobs,
the scenes and the morph's plan), the phaser itself (~150 B, no SDRAM), and 128 B more for
each scene file buffer (`kSceneFileMax` 4096 to 4224: 4 scenes of 14 effects take 4,172 B);
the bench adds a 25th segment's rows.

The developers' SysEx (#97 part 1, 2026-10-11: IDENT, PERSIST, GET/SET, BENCH, the load's
crackle count) took 1.8 KB of code and 96 B of data (a queue of 8 requests, the counters):
`FRIZZ.bin` at 221,244 B of code (~57 KB free) and 224,312 B of data (~21 KB free), the
bench build at 237,556 B (~41 KB) and 232,476 B (**~13 KB free**), from 219,444 / 224,216
and 235,708 / 232,348 on `main` (493c13c). The bench's `cpu.txt` window moved from a static
into the bench, for BENCH to read; no new buffer.

The FX chain and the master compressor a block at a time (#136, 2026-10-12) took 1.2 KB of
code and 96 B of data (the resonator's lowpass per sample of a block, for its Taps):
`FRIZZ.bin` at 232,792 B of code (~45 KB free) and 224,896 B of data (~20 KB free), the
bench build at 250,156 B (~28 KB) and 233,060 B (**~12 KB free**), from 231,616 / 224,800
and 248,732 / 232,964 on `main` (50754d9). By function: `FxChain::Run` 12.7 KB against
`FxChain::Process`'s 12.5 KB, `PassthroughEngine::Process` +0.3 KB, and the freezer's and
the tape stop's loops (`FxChain::Insert`, out of line as their `Process` was) +0.7 KB. No
per-sample path is kept beside the block one (the prototype in #21 kept both, +5.7…7.8 KB).

Page 2's busy case inline in the chain's loops (#142, 2026-10-12) took 4.0 KB of code
(3.6 KB in the bench build), no data: `FRIZZ.bin` at 238,768 B (~39 KB free), the bench build
at 255,636 B (~22 KB), from 234,768 and 252,020 on `main` (0459aa4). By function:
`FxChain::Run` +1.2 KB, the freezer's and the tape stop's `Insert` +0.6 and +0.7 KB, the
crusher's `Insert` out of line now (1.9 KB, where its `Process` was in `Run`), `FxOutput::Begin`
and `Join` out of line (0.3 KB, shared), against the cold `OutBegin`, `OutEnd` and `OutBand`
gone (-0.7 KB). Begin and Join inlined as well took 1.8 KB more and measured the same, within
the layout's noise.

The split moved by 40 KB (step 1 below) after the FX knobs' page 2 (#35) had left ~21 KB of
code, ~3.4 KB in the bench build. The scene work (`Recall`, `Morph`, the scene file) is
compiled `-Os` and out of line (`FX_SCENE_ONCE`), as the event log's and MIDI's are.

The FX layer's cleanup (#89) took 11.3 KB of code off against `main` before #86: `FRIZZ.bin`
241,588 → 230,316 B, the bench build 259,588 → 247,556 B; data 96 B less (228,304 B; the
bench build's 240,340 B). The scene morph's `Start`, `AddBar`, `Land` and `Freeze` built
`-Os` and out of line (`FX_MORPH_ONCE`, -7.3 KB: their copies of the 104-float plan had been
unrolled and inlined into the scene keys' code), the effects' `Init`s too (`FX_ONCE`,
-2.4 KB), the shifter's `Splice` out of line (-1.8 KB: `Shifter::Process` 5.7 → 1.7 KB) and
the coarse grids by index (-1.2 KB). #86 builds the play page `-Os` as well, which overlaps
with the morph's share: the two together save less than their sum. On top of #89 the play
page's `-Os` takes 12.0 KB (`FRIZZ.bin` 229,996 → 217,980 B, the bench build 247,212 →
235,004 B).

Where the code goes (`.text` + `.rodata` by object, from the map):

| | Bytes |
|---|---|
| FRIZZ itself (`chompi_main.o`: one translation unit, `-O3`) | 96,600 |
| libDaisy and the STM32 HAL | 87,700 |
| FatFs (`ff.o`, `ccsbcs.o`, `diskio.o`) | 14,700 |
| newlib-nano, libm, libgcc | 11,400 |

The biggest single functions are `FxChain::Run` (14.0 KB, the chain over a block, #136, #142) and
`PassthroughEngine::Process` (10.9 KB, everything inlined into them; 2026-10-12), then `Shifter::Process`,
`Looper::Process` and `FxChain::Init` (5.5 KB each). `NormalPage::OnButton` was 7.9 KB at
`-O3`, 1.1 KB at `-Os`.

To see it again after a build (`firmware/code/src`):

```bash
awk '/^Linker script and memory map/{go=1}
go && /^ \.(text|rodata)/ { if (NF>=4) {sz=strtonum($3); f=$4} else {getline; sz=strtonum($2); f=$3}
  sub(/.*\//, "", f); s[f]+=sz }
END {for (k in s) print s[k], k}' build/FRIZZ.map | sort -rn | head -20
arm-none-eabi-nm -S --size-sort -C build/FRIZZ.elf | tail -30      # largest symbols
```

## CPU

The audio callback, not memory, is what limits FRIZZ today: about 45 % of each 0.5 ms block
idle, about 100 % with a loop playing and ~11 effects on, where it starts to crackle (a known
limit, parked). See `CLAUDE.md`, *The audio callback is CPU-bound*, and measure with the bench.
A feature that runs per sample costs CPU in that budget; one that runs in `MainLoop()` doesn't.

## Making room for code, the biggest lever first

Steps 1 and, for the play page and the card code, 3 are done, and of 5 the USB host stack and ST's device stack; the rest isn't built yet: do it when a feature needs it, each step on
its own branch with a device test (the layout can change the timing: b5c658c).

1. **Move the split between code and data** (done, #26: 232 KB / 280 KB became 272 KB /
   240 KB in `chompi_sram.lds`). The bootloader copies up to 480 KB of image into the 512 KB
   AXI SRAM (`reference/firmware/chompi-bootloader-v6.4-beta/shared/bootloader.cpp:102`,
   `sram_program[SRAM_SPACE - 32768]`), and the launcher takes images up to 512 KB
   (`ImageLooksValid` in its `chainload.h`, staged in a 512 KB SDRAM buffer). What they copy
   is the image, the code and `.data`'s initial values (217 KB), not the split, which is
   FRIZZ's own choice. It stopped at 40 KB because the bench build's data needs the rest;
   step 4 or 2 frees data so it can move further.
2. **Move cold data out of AXI SRAM** into the unused 256 KB of D2 (`.d2_bss`, an
   `__attribute__((section(".d2_bss")))` on the global), so step 1 can move further: the MIDI
   receive queues (`midi_clock`, 74 KB, read once per block), the scene store, the event log's
   index. Not the engine: it's read every sample and AXI SRAM is the fastest after the TCMs.
   D2 is reachable by DMA but is cached, so nothing a DMA writes may go there. To check
   first: libDaisy enables the D2 SRAM clocks only with `DATA_IN_D2_SRAM`
   (`libs/libDaisy/src/sys/system_stm32h7xx.c:203`); `RAM_D2`'s DMA buffers already work at
   0x30000000, but SRAM2/3 (0x30020000 up) may need `__HAL_RCC_D2SRAM2_CLK_ENABLE()` and
   `__HAL_RCC_D2SRAM3_CLK_ENABLE()` at boot. A zeroing loop is needed too: startup code
   clears only `.bss`.
3. **Build rarely run code for size.** `EventLog.h` (`EVENT_LOG_ONCE`) and `MidiControl.h`
   (`MIDI_CONTROL_ONCE`) mark such functions `noinline, optimize("Os")`. The play page's
   logic is built `-Os` whole (done, 2026-10-11: `#pragma GCC optimize("Os")` over
   `NormalPage.h`, `PlayKeys.h`, `SceneControls.h`, `SettingsPage.h` and `FxControls.h`,
   -15.5 KB before #89, -12.0 KB after), but for what the audio callback calls (`NormalPage::ShowSettings`,
   `ResetSmtLeds`), which stays as the callback is built; check with
   `arm-none-eabi-objdump -d` that nothing the callback reaches lost its inlining. The card
   code is too (done, 2026-10-14, #285: the same pragma over `SceneStore.h`,
   `MasterSettings.h` and `FxScenes.h`), -4,280 B of code in `FRIZZ.bin` (263,744 B, ~14 KB
   free) and -4,360 B in the bench build (269,744 B, ~8.6 KB free), most of it the store's
   `Init` and `Process` no longer inlined into `main` (6.2 → 3.4 KB) and `ParseMaster`,
   `PutUint` and `PutValues` no longer unrolled. Two kinds of function stay out of it:
   one-liners `-O3` code calls (`SceneStore::Busy`, `InRam`, …), as GCC won't inline a
   function with an `optimize` attribute into a caller built otherwise; and what calls an
   engine helper `-Os` code won't inline (`SendsTo5`/`SendsFrom5` and `FxOutput::LevelGain`):
   the helper's copy left out of line changed how the engine's own `FxOutput::SetParam` was
   built. Compare every function's code with main's, not only the sizes. In the bench build
   the -O0 control polling (`Hardware::ProcessAllControls`, the profile's `ctrl`) went from
   7.2 to 7.8 % in every segment though its code and data are unchanged: only libDaisy's GPIO
   and timer code moved relative to it. `aligned(64)` on it gave 7.5 on main and 7.6 here, so
   that column moves about half a point with the layout alone; FRIZZ.bin's own load didn't
   move (`play --cpu`, idle 32.3 → 32.1). The boot and init
   code is left: a few KB more; measure with the map. The per-sample path stays `-O3`.
4. **Shrink the MIDI queues.** libDaisy's `MidiHandler` keeps `FIFO<MidiEvent, 256>`
   (`libs/libDaisy/src/hid/midi.h:261`), each event with a 128-byte SysEx buffer: 37 KB per
   transport. FRIZZ drains them every block, so 32 events would do: -64 KB of data. It's a
   change to vendored libDaisy: rebuild `libdaisy.a` with GCC 10.3 and list it in
   `THIRD_PARTY.md`.
5. **Trim libDaisy and the HAL**: the USB host stack is gone (6.4 KB, #196), and with TinyUSB
   ST's device stack, its CDC class and the `printf` its `USBD_DEBUG_LEVEL` pulled in (#271).
   What's left, in vendored code: the QSPI driver (3.7 KB).

Not recommended: link-time optimisation (GCC 10.3, and it would merge the `-O0` workarounds
into their callers), and running code from QSPI flash (slow, and it's the bootloader's).

**The CPU and the code's size.** The per-sample code is far bigger than the 16 KB I-cache,
so the callback is bound by it: every byte on that path costs, even code that is only
skipped. The FX chain and the compressor therefore run a block at a time (#136): each
effect's loop over the block's 24 samples before the next effect's, so its code is fetched
once a block rather than once a sample (`FxChain::Process`). That took the bench's
`everything` from 92.0 / 105.9 % (mean / max) to 73.1 / 83.2 and idle from 40.3 to 33.2, and
`fader.txt`'s worst block from 100 to 84 % (#136, measured against `main` in one hold). Page 2's wrapper cost ~6 points of "everything" at its defaults until the chain
skipped it with a bitmask and moved the busy case out of line (`__attribute__((cold))`); with
the chain a block at a time, the bitmask is read once per run and the busy case is inline at
the end of the run's loop (`FxChain::Insert`, #142), which took `loop+everything+page2` from
103.4 / 127.3 to 95.4 / 118.6 % and left the segments at the defaults as they were or a
little lower (-0.1 to -0.9, against `main` in one hold). Rare paths belong out of line; but a small change also shifts the
layout, by ±2.5 points either way, so measure each one. The play page's `-Os` (#86) is such a
case: none of its code runs in the callback, yet before #89 the engine it moved by 15 KB ran
1-2.8 points slower in the lighter bench segments (the same source without the pragma: as
`main`); on top of #89 it measures as `main`, ±1.2. `-O3` stays: `-O2` and `-Os` came out
slower.

**For the CPU, not for room: ITCM** (an experiment, #51, off by default). ITCM (64 KB) is the
core's fastest code memory, with no cache to miss. With
`make ITCM=1` (built apart in `build-itcm/`, `build-bench-itcm/` with `BENCH=1`) the per-sample functions
(`FxChain::Process` and `Run`, the effects that aren't inlined into them, `PassthroughEngine::Process`,
the looper, the delay's read, the reverb, `AudioCallback`) are marked `FRIZZ_HOT` and run
from ITCM: `.itcmram` loads from `SRAM_EXEC` (`chompi_sram.lds`) and `main()` copies it
(`CopyItcm`) before anything else. It saves no code room: the image still carries it. Calls
out of it go through the linker's long-branch veneers. A function that runs every sample and
grows big belongs there; mark it `FRIZZ_HOT`. Its first device run came up on USB but never
answered (the main loop starved, no fault: a fault resets into the launcher), so it stays off
until a debugger shows why (#51).
