# FRIZZ firmware: developer guide

The FRIZZ source, how to build it, test it and get it onto a CHOMPI. To install a finished
`FRIZZ.bin`, see [`INSTALL.md`](../INSTALL.md) instead. To find out what the controls do, see
[`MANUAL.md`](../MANUAL.md).

FRIZZ was forked from CHOMPI's WAVE 1.0 firmware (`reference/firmware/chompi-wave/`). Since then
the two have diverged, so treat WAVE as reference, not as a shared core.

## Layout

```
code/src/                 the firmware
code/libs/                vendored libDaisy and DaisySP (patched, MIT; never swap in upstream)
bin/                      cpu.txt (the CPU bench's numbers for this source, below), the v6.2
                          bootloader binary and its install script; the bootloader's source
                          is in reference/firmware/chompi-wave/code/Chompi_Bootloader/
test/                     host-side checks: the engine against HEAD, and unit checks (unit.sh NAME)
twin/                     the virtual CHOMPI: the whole firmware on the PC, on a simulated board
flash.py, card.py         sending a build to the CHOMPI and reaching its card over USB, through the
                          multi-firmware launcher
remote.py                 playing and querying a running FRIZZ over USB MIDI
tools/                    what those share (chompi.py, the launcher's midi_send.py), the git hooks,
                          builds.py (both builds and their md5), and measure.py: timing and
                          sound measured on the device
```

Design notes live in [`../docs/`](../docs/): the looper spec (`LOOPER.md`) and an overview of
the effects across the CHOMPI firmwares (`FX_OVERVIEW.md`).

## 1. Toolchain

To build the firmware for the CHOMPI, FRIZZ needs **GNU Arm Embedded 10.3-2021.10** (GCC 10.3.1), the Daisy toolchain default. Newer
compilers can build it, but their builds sometimes break SD card communication intermittently.

- **Linux:** download ARM's official tarball (`gcc-arm-none-eabi-10.3-2021.10-x86_64-linux.tar.bz2`),
  unpack it, for example to `~/opt/`, and put its `bin/` first on your `PATH`:

  ```bash
  export PATH="$HOME/opt/gcc-arm-none-eabi-10.3-2021.10/bin:$PATH"
  ```

- **macOS:** the [Daisy toolchain installer](https://github.com/electro-smith/DaisyToolchain)
  installs the compiler along with `openocd` and `dfu-util`. If other versions are installed
  too, put 10.3's `bin/` first on your `PATH`.

Don't use a distribution's or Homebrew's `arm-none-eabi-gcc`. It's a much newer GCC and usually
comes without newlib. Check what you have:

```bash
arm-none-eabi-gcc --version    # should say 10.3.1
```

You also need `make`. `dfu-util` is optional (only for reinstalling the bootloader). The checks
on the computer don't need any of this: see [3](#3-test-on-the-host).

## 2. Build

```bash
cd firmware/code/src
make              # or make -j8
make clean        # start fresh
```

The output goes to `code/src/build/`:

- `FRIZZ.bin`: the firmware you put on the SD card
- `FRIZZ.elf`: the same firmware with debug info, for gdb

`make BENCH=1` builds the CPU bench into `code/src/build-bench/` ([below](#measure-the-cpu-load)).
Neither is committed: a build is named by its md5, and `tools/builds.py` builds both and
prints it (`tools/builds.py md5` only prints). That works because GCC 10.3 builds the same
source to the same bytes on any machine; another compiler gives other bytes (and the risk
above), so `builds.py` and `flash.py` refuse to build with one.

Read the memory table at the end. FRIZZ runs from SRAM, so a build that compiles can still
fail to link if it doesn't fit. Warnings are normal; only lines that say `error` mean the build
failed.

The prebuilt `libdaisy.a` and `libdaisysp.a` are committed under `code/libs/*/build/`, so you
don't normally rebuild them. If you change a library, run `make` in `code/libs/libDaisy` or
`code/libs/DaisySP` with the same compiler.

## 3. Test on the host

The checks and the virtual CHOMPI build FRIZZ for the computer, not for the CHOMPI, so they
need no ARM toolchain, only:

- **bash, git, coreutils** (`md5sum`), **`g++`** with C++14 and **`python3`** (no packages).
  Tried on Linux (Fedora, GCC 16, Python 3.14). **macOS is untested** and won't work as it
  is: it has no `md5sum`, and the twin's coroutine (`ucontext`) needs `_XOPEN_SOURCE` there.
  Use Linux, a Linux VM or a container until someone ports it.
- For the twin in the browser only: **Emscripten** (below).

Nothing else gets installed: the first run builds DaisySP and the twin for the computer into
`test/build/` and `twin/build/` (ignored by git). From a fresh clone:

```bash
cd firmware/test
./all.sh          # everything, side by side, one line each: about 30 s on 12 cores, the first time more
./check.sh        # engine at HEAD vs the working tree: a refactor must print "bit-identical"
./unit.sh tempo   # one unit check (each NAME.cpp; ui runs the whole firmware on the twin)
```

[`test/README.md`](test/README.md) says what each check covers.

To try a change before flashing it, play it on the virtual CHOMPI: the firmware from
power-on, keys, knobs, MIDI and audio from a script, the master out as a WAV and the LEDs as
text ([`twin/README.md`](twin/README.md)):

```bash
cd firmware/twin
./run.sh -o out.wav -l - examples/filter.txt
web/serve.sh      # the same in the browser, to play and hear: http://localhost:8765
```

Neither shows the CPU load or anything else about the chip; that takes the device.

The browser twin needs Emscripten in `~/opt/emsdk` (or `em++` on the `PATH`); it's been used
with 6.0.11:

```bash
git clone https://github.com/emscripten-core/emsdk.git ~/opt/emsdk
cd ~/opt/emsdk && ./emsdk install 6.0.11 && ./emsdk activate 6.0.11
```

`web/serve.sh` finds it there by itself. Its first build compiles DaisySP for the browser too,
so it takes longer; after a change, a reload rebuilds in about 20 s.

**A bug from a player:** SHIFT + VOLUME press held 2 s on the settings page writes `/FRIZZ/bug-N.txt`
(MANUAL.md, *Bug reports*), a twin script of everything since power-on with the card's files
from then. `twin/run.sh -o out.wav -l leds.txt bug-1.txt` plays it again, up to the combo;
once it shows the bug, turn it into a case in `test/ui.cpp`. What it holds and what not:
[`twin/README.md`](twin/README.md#bug-reports-from-the-device).

## 4. Put it on the CHOMPI

**Over USB, with the multi-firmware launcher** (the quick way while developing). The
[CHOMPI launcher](https://github.com/sfaber02/CHOMPI-MULTI-FIRMWARE/releases) by hiwatts,
chomplex music theory and lnetzel sits on the card as `CHOMPI.bin`, keeps firmwares in
`/FIRMWARE/NN_NAME.bin` and starts the one whose key you press (NN). While its picker shows,
it takes a firmware over USB MIDI, writes it to its slot and starts it:

```bash
cd firmware
./flash.py            # builds FRIZZ.bin, restarts the CHOMPI into the launcher, sends it to slot 10
./flash.py --bench    # FRIZZ-bench.bin to slot 11
./flash.py --no-build # the build that's in code/src/build/ (build-bench/ with --bench), as it is
./flash.py --run 11   # starts what's in slot 11 already, sending nothing
./flash.py --test     # a branch's build to the test slot (12), as FRIZZ-TEST
./flash.py --list     # what's on each key
```

Each send prints the md5 of what it sends, the name of that build in a pull request.

**One at a time.** Every tool here takes a lock (`tools/chompi.py`, in `/run/user/UID`) when
it first reaches for the CHOMPI, and waits while another has it, saying who; what doesn't
touch it (`measure.py compare`) never waits. `tools/chompi.py hold CMD` keeps it over a whole
sequence of tools, `tools/chompi.py hold` until Ctrl-C (for playing it by hand).

**Which build runs** FRIZZ says itself (the developers' SysEx's IDENT, below): the CRC-32 and
length of the image it runs, which `tools/builds.py` prints next to the md5, and whether it's
the bench. `flash.py` notes the build it sends to a slot, beside the lock, and after sending
asks the build running and fails if it isn't the one sent; `--run N` says which build it
started. `remote.py` and `measure.py` find the slot of the build running by those notes (one
started by hand too), say which it is, take `--slot 10|12` for one in particular, started
unless it's the one running, and never take the bench for FRIZZ. A FRIZZ from before IDENT
(or a build the tools didn't send) can't say, and answers as FRIZZ-TEST and the bench do; for
those the tools go by the slot they last started, also noted beside the lock (`hold` without
a command forgets that note).

**Garbled USB names** (#88): now and then (2 in ~310 starts) FRIZZ comes up with its USB
names one string off (product `Љ`, maker `FrizZ`); the tools find it by the Daisy's USB id
then, and say so. A tool that starts FRIZZ, FRIZZ-TEST or the bench (`flash.py`, `--run`,
`card.py --then`, `remote.py`) and finds it so starts the slot again from the launcher, twice
at most, sending nothing, then asks for the power switch. `tools/chompi.py fix-name [SLOT]`
does it for one found running so: it starts the slot noted as running (10, 11 or 12) again.

Sending replaces whatever is in that slot, so set yours (`FRIZZ_SLOT=4 ./flash.py`,
`BENCH_SLOT`, `TEST_SLOT`, or `--slot`) if FRIZZ isn't on key 10. Without `--slot`, it
refuses a key that holds another firmware than FRIZZ (`--slot` replaces it anyway). Linux only (ALSA), Python 3, no
packages; it uses the launcher's own client, `tools/midi_send.py`. On macOS or Windows, the
launcher's web page (https://ugrossek.github.io/CHOMPI/, Chrome or Edge) does the same.
FRIZZ keeps its files in `/FRIZZ`, so it shares the card with the other firmwares.

**The card over USB.** The launcher's key 15 is lnetzel's USB storage firmware: the card shows
up as a drive. `./card.py` uses it with no hands at the panel:

```bash
./card.py get                  # FRIZZ/cpu.txt and every bug report, into card/
./card.py get FRIZZ/x.txt -o . # any file
./card.py put FILE FRIZZ/      # a file onto the card
./card.py ls FRIZZ             # a folder
./card.py mount                # just mounts it and prints where; ./card.py done ends it
                               # (in a hold, which keeps the CHOMPI between them:
                               # tools/chompi.py hold bash)
```

Each brings the CHOMPI into the storage firmware, mounts the card, does its part, ejects it and
starts FRIZZ again (`--then N` another slot, `--then none` stays at the picker), about 30 s in
all. `remote.py` likewise starts FRIZZ if the CHOMPI is elsewhere, and `flash.py` sends from
wherever it is.

All of them go through the launcher (`tools/chompi.py`): a running FRIZZ restarts into it over
USB MIDI (the SysEx `F0 7D 43 48 10 F7`, `MidiClock.h`), the storage firmware restarts on an
eject, and the launcher starts a slot on its `RUN` command. The last two need
[launcher v1.5](https://github.com/sfaber02/CHOMPI-MULTI-FIRMWARE/releases/tag/launcher-v1.5)
(the [setup page](https://ugrossek.github.io/CHOMPI/setup/) updates an older one) and
[USB storage v1.6](https://github.com/lnetzel/CHOMPI-lnetzel/releases/tag/usb-storage-v1.6).
A card set up from the launcher's v1.5 release still has USB storage v1.5 on key 15: copy
v1.6's `.bin` over `FIRMWARE/15_USB_STORAGE.bin` (or `./flash.py FILE --slot 15 --name
USB_STORAGE`). With an older launcher or storage firmware the tools still work, but say which
key to press (15 for the card, overdub then CHOMPI to leave it, 10 for FRIZZ). With an older FRIZZ or
another firmware running, switch the CHOMPI off and on when they say so (they wait up to 2
minutes). Mounting needs udisks, as any desktop has.

**Remote control over USB MIDI.** `./remote.py` talks to a running FRIZZ over its SysEx
(`code/src/MidiControl.h`): `state`, `leds`, `load [--every S]` (with the blocks that took
longer than a block, which crackle), `settings`, `channel N`, `transport on|off`,
`source auto|trs|usb|internal`, `out off|trs|all`, `scene get|put SLOT FILE`, the developers'
`ident`, `ram-only`, `get GROUP`, `set GROUP INDEX VALUE` and `bench [--wait] [--out FILE]`
(below), and `play SCRIPT [--cpu] [--fresh]`, which plays a twin
script (`twin/scenarios/`, a bug report) on the device: its keys and knobs over SysEx at the
script's times, its MIDI and clock, its `expect led` lines checked against the device's LEDs, at the
brightness the card's `led_brightness` sets (100, 75 or 50 %: the one level that fits every
line, said when it isn't 100 %).
With `--cpu` it reports the worst load FRIZZ.bin itself had, measured in the audio callback as
the bench does, and how many blocks took longer than a block, so a scenario can be tried for
crackles on the build that plays, not the bench's (it says how many readings went unanswered,
if any). Whatever ends a script (an error, Ctrl-C), it lets go of the keys it holds and gives
the mode switch back to the hand.

A script plays on FRIZZ as it stands: a twin script starts from power-on, but on the device the
scripts played before it since FRIZZ started have left the effect selected, its knobs, the
page or a loop in RAM, so its `expect led` lines can fail after another one (#123).
`play --fresh` first starts the slot again through the launcher, as `flash.py --run N` does:
FRIZZ boots from the card, which a start doesn't write, with nothing left from before. Every
`play` after the first since `flash.py` started the slot wants it (about 10 s each); a failed
`expect led` without it says so. Not with `--device` or `--no-start`, which can't restart it.

`play` first stops FRIZZ writing the card until it restarts (PERSIST 0), so a script may
change the settings, the compressor or the scenes: they stay in RAM, and the card keeps what
key 10's FRIZZ shares. On a slot other than the test slot (12) it starts that slot again at
the end, from the card, so it saves again. `--save` leaves the card written as FRIZZ writes
it. A FRIZZ from before PERSIST, or `--save`, gets the old guard: `play` refuses a script that
would change what FRIZZ keeps on the card, saying which line and which file: the settings
page (`twin/scenarios/settings.txt`, most bug reports, #58) and the compressor's knobs
(`comp.txt`: its key, then knobs 1-4; or its CCs over MIDI) go to `/FRIZZ/frizz_master.txt`,
with no `.bak`; SAVE / COPY / DELETE confirmed (`scenes.txt`) or a scene put over SysEx to
`/FRIZZ/frizz_scenes.txt`, whose `.bak` keeps only the one before (#78). It reads where the
device stands first (the page, what the knobs edit, the scene mode); `--force` plays it
anyway. Linux only, Python 3, no packages, like `flash.py`.

**The developers' SysEx** (`0x3E`, `MidiControl.h`'s `namespace dev`) is for the tools, not
for players (MANUAL.md lists it only briefly): `F0 7D 43 48 3E SUB SEQ ARGS F7`, over USB
only, answered `F0 7D 43 48 7E SUB SEQ STATUS DATA F7`. SEQ comes back, so a tool matches its
answer; STATUS is 0 ok, 1 refused (an unknown sub-command, group, index, or a value a setting
doesn't take), 2 not in this build, 3 not yet. Requests wait in a queue of their own (8), apart
from the other queries' one slot that `play --cpu`'s load readings use, and MainLoop answers
one every 2 ms; a request that finds the queue full gets no answer, so the tools ask again
(every request can be repeated: they set, never step). Numbers go in 7-bit groups, the highest
first; a knob's value in 14 bits, as `0x21` has them.

| SUB | Asks | Answers |
|---|---|---|
| `00` IDENT | — | version (1), kind (0 FRIZZ, 1 the bench), the image's CRC-32 (5 groups) and length (3) as the bootloader loaded it (`zlib.crc32` of the `.bin`; 0 on the twin), uptime ms (5), MainLoop's passes (5), `RCC_RSR` at boot (5, as the bootloader left it, not cleared), flags (1: RAM only) |
| `01` PERSIST | `00` | flags. From now until a restart nothing is written to the card: a scene saved, copied or deleted, the master settings, as if written (the page shows them saved). One way: a restart (`flash.py --run N`) writes again, reading the card as it was. Bug reports (`bug-N.txt`) are still written: a new file each, never one key 10 reads |
| `02` GET | GROUP | GROUP, COUNT, then COUNT values in 14 bits |
| `03` SET | GROUP INDEX VALUE(2) | GROUP INDEX; applied in MainLoop through the setters the knobs and keys use, and saved as theirs are (unless RAM only) |
| `04` BENCH | `00` | the bench's state: running, settling, the segment, how many, written, stored, `cpu.txt`'s length (3) |
| | `01` OFFSET(3) | up to 32 bytes of `cpu.txt` from OFFSET, as it went to the card (status 3 while it runs); FRIZZ.bin answers status 2 |

The groups of GET and SET: `00`-`1F` an effect's knobs by its FxId (`FxParams.h`: 0-3 page 1,
4-7 page 2), `20` the compressor's, `21` the latches (index by FxId, 0 or 1), `22` VOLUME's
(0 output gain, 1 input gain, 2 mix, 3 headphone feed), `23` the card's settings as
`masterfile::kLines` lists them (`MasterSettings.h`: mono, MIDI channel, transport, clock
factor, clock source, MIDI out, LED brightness), their values as `frizz_master.txt` has them.
The groups don't follow the MIDI map (CCs), so a tool keeps working when it changes. SETs
aren't in the event log: a bug report taken after them won't replay them.

**From the card:** copy `build/FRIZZ.bin` to the SD card and power on, as described in
[`INSTALL.md`](../INSTALL.md). FRIZZ is a `BOOT_SRAM` app: CHOMPI's bootloader copies it from
the card into QSPI flash and runs it from SRAM. Standard Daisy flashing advice doesn't apply.

Every CHOMPI already has the bootloader, and an SD update never touches it. Only a blank or
erased Daisy Seed needs it installed. To do that, hold BOOT and tap RESET to enter DFU mode,
then run `bin/install_bootloader.sh`.

## Measure timing and sound on the device

`tools/measure.py` measures what the twin can't, on the real CHOMPI, with no hands at the
panel. It needs an audio interface: one of its outputs into AUX, the CHOMPI's line out into
its input 1, its MIDI out into the TRS jack (found by name; `FRIZZ_AUX_SINK`,
`FRIZZ_LINE_SOURCE`, `FRIZZ_TRS_MIDI` set them). Python 3 with numpy (a venv:
`python3 -m venv ~/.venvs/frizz && ~/.venvs/frizz/bin/pip install numpy`).

```bash
cd firmware/tools
~/.venvs/frizz/bin/python measure.py clock --mode pairs --bpm 120     # FRIZZ's tempo from a USB clock
~/.venvs/frizz/bin/python measure.py drift --bpm 174 --seconds 180    # a 1-bar loop against a TRS clock
~/.venvs/frizz/bin/python measure.py material --music a.wav b.wav --out music.wav
~/.venvs/frizz/bin/python measure.py fx KEY_13 --cc 17=100 --material music.wav --out rev-main
~/.venvs/frizz/bin/python measure.py compare rev-main.f32 rev-branch.f32
```

- `clock` paces a MIDI clock over USB by the computer's clock (to about 0.1 ms) and reads
  FRIZZ's tempo every 50 ms: single ticks, `pairs` (two in one USB packet), `catchup` (every
  other tick late, a frame before the next) or `late` (one tick in 48 sent 10 ms late).
- `drift` plays generated material into AUX, records a quantized loop with PLAY + LOOP over
  SysEx against a clock on the TRS jack, records the line out, and finds the loop's length
  by cross-correlating every pass with the first. The computer's and the interface's clocks
  are matched by a line through the recording's packet arrivals. Pace the clock by the
  computer, never by the recording: PipeWire delivers it in packets, ~21 ms of jitter.
  Measure 3 minutes or more: a 1-bar loop is good to about ±1.5 ms a minute. The generated
  material never repeats, so a pass can't be matched to the wrong place (real music loops
  can be).
- `fx` latches an effect and sets its level (the sends' level knobs start at 0: CC 113 the
  delay, 117 the reverb), plays material into AUX and records the line out; `compare` gives
  two recordings' level and octave bands, for an A/B of two builds. Real music
  (`material --music`) is the better test here.

For an A/B, send each build to slot 10 in turn (`flash.py FILE --slot 10`) and measure from a
fresh start each time; `remote.py load` and `remote.py play SCRIPT --cpu` give the load.

## Measure the CPU load

The audio callback has 0.5 ms per block, and neither the host checks nor the virtual CHOMPI
can tell how much of it a change uses. The CPU bench can, on the device:

```bash
cd firmware/code/src
make BENCH=1      # build-bench/FRIZZ-bench.bin; the normal build is untouched
```

1. With the launcher, `./flash.py --bench` puts it on its own key (11) and starts it. Without
   it, put `FRIZZ-bench.bin` on the card instead of
   `FRIZZ.bin` (the bootloader takes the first `.bin` it finds, whatever its name) and switch
   on. The bootloader flashes it as usual.
2. After the boot animation the bench waits about 10 s for every effect to rest (only CHOMPI
   glows dimly), then runs by itself, about 90 s: between segments it waits for the tails of
   effects the next one doesn't use to ring out. Don't touch anything: the keys light up one
   per segment, green below 80% load, amber below 95%, red above; the one running blinks white.
3. At the end every panel LED is green (all below 95%) or red. Blinking red: `cpu.txt`
   couldn't be written (no card?).
4. On the computer, `./remote.py bench --wait --out card/cpu.txt` follows it over USB until
   it's done and fetches the `cpu.txt` it wrote (the developers' SysEx, BENCH); from the card,
   with the launcher: `./card.py get`. `FRIZZ/cpu.txt` has every segment's highest and mean load, any effect
   still working outside its segment (a tail), and whether the loop played. Below, where each
   segment's mean goes: the clock the chip ran at, and the mean split into the callback's
   parts (MIDI, the controls, the UI's events, the input, the looper, the tempo, the FX chain,
   the compressor, the output and limiters), counted in the core's cycles (`BenchProfile.h`).
   Put `FRIZZ.bin` back on the card to play again. `tools/builds.py cpu` files the
   `cpu.txt` in `card/` as `bin/cpu.txt`, headed by the md5 of the bench build it measured
   (and refuses one from another source).

The segments: nothing on, each effect alone (its knobs moving every 0.25 s), the compressor,
the inserts together, recording a loop, the loop alone, with the inserts, with the delay, with
PR #7's scene 4 (shifter +7, folder, crusher, slicer, compressor at 1) and the delay, with the
reverb, then everything, everything with every feedback at the top (the harness's STRESS), and
the loop with everything. The sends come last because their tails run on (the delay's for 10
s). The signal is the bench's own, so runs compare: a little tune in A minor at 120 BPM (Am F
C G, an arpeggio, bass, kick, hi-hat), made by a few oscillators, so it costs no room for a
recording. It's made for the next block after each measurement, so its own cost isn't in the
numbers (it was in the first runs', 2026-10-08, with a saw, a gated sine and noise).

The bench measures `FRIZZ-bench.bin`, whose memory layout differs from `FRIZZ.bin`'s; a
crackle from the layout alone (b5c658c) can show in one and not the other. It tells what the
code costs; whether `FRIZZ.bin` crackles, only playing it tells.

## Before a pull request

Work on a branch off `main`, never on `main` itself, in a worktree of its own
(`git worktree add ../FrizZ-<branch> -b <branch> origin/main`), and open a pull request for it
(a draft is fine). `tools/install-hooks.sh` installs a pre-commit hook that refuses commits on
`main` and reminds you of the changelog. Before each commit that touches `code/`, `test/` or `twin/`:

1. **`test/all.sh` passes.** A check that fails is fixed, or, when the change is meant to
   alter what it checks, updated in the same commit, saying so in the commit message.
2. **A refactor changes nothing:** `test/check.sh` and `twin/compare.sh` print
   `bit-identical`.
3. **What the twin can show, a check shows:** for a change to keys, LEDs, the card, levels or
   clicks, add a case to `test/ui.cpp` and make sure `twin/ui-at.sh origin/main` (or `twin/ui-at.sh origin/main sync` for a timing check) fails it (it
   passes with your change). `git fetch` first: a stale local `main` compares with an old
   version.
4. **Players read about it:** what they notice goes into [`CHANGELOG.md`](../CHANGELOG.md)
   under *Unreleased*, and a changed control into [`MANUAL.md`](../MANUAL.md).

The pull request lists what changed, what the twin checked (with `twin/compare.sh origin/main
HEAD`'s output, every difference explained), what was checked on the device (a firmware
branch's build on the test slot, key 12, named by the md5 `flash.py` printed: the bench's
`cpu.txt` against `main`'s `bin/cpu.txt`, scenarios played with `remote.py play --cpu`), and what only a player can
judge, for the release test (feel, sound by ear, real MIDI gear).
[`CLAUDE.md`](../CLAUDE.md) has the full workflow, including the release test and test builds.

## 5. Debug

This needs an STLINK-V3MINIE and a Daisy Seed with a soldered debug header. The back panel
doesn't fit over the header, so a second Seed for development is handy.

```bash
openocd -f interface/stlink.cfg -f target/stm32h7x.cfg     # terminal 1
arm-none-eabi-gdb code/src/build/FRIZZ.elf                   # terminal 2
(gdb) target remote localhost:3333
```

If a CHOMPI stops responding and nothing else works, reinstall CHOMPI's v6.2 bootloader with
`bin/install_bootloader.sh` (or flash `bin/CHOMPI_Bootloader_V6_2_0.bin` to `0x08000000` with
an ST-Link), then reinstall FRIZZ from the card. The web tool at https://flash.daisy.audio can
erase a stuck Seed, but the generic Daisy bootloader it offers is not CHOMPI's.

## Where things are in `code/src`

```
chompi_main.cpp        entry point: audio callback, main loop, boot sequence, AUX's jack picking the input
passthroughEngine.h    the engine: the input (AUX or the built-in mic) and its gain, the looper, input/loop mix ("dry/wet" in the code), punch-in FX,
                       master compressor, output gain, headphone cue, safety limiter
FxChain.h              the punch-in effects in their processing order, with a level meter each
FxParams.h             each effect's knobs: how many, defaults, steps, coarse grids; the compressor's too
FxSlots.h              each effect's key, LED and colours; the compressor's key
MasterComp.h           the master compressor: amount, ratio, speed, mix, stereo-linked
MasterSettings.h       what's kept on the card outside the scenes (the compressor's knobs, the
                       mono input, the MIDI channel and transport following), and its file format
SceneStore.h           the card: /FRIZZ, the scene file and the master file, written from MainLoop
FxScenes.h             the FX scenes and their file format
FxControls.h           the FX keys and knobs: latches, fine / stepped / coarse turns, scene snapshot and recall;
                       the compressor's knobs too
SceneControls.h        the scene keys: recall, morph, and the save / copy / delete flow
FxMorph.h              a scene morph: glides the effects to a scene, landing on a bar line
PlayKeys.h             the CHOMPI, PLAY and LOOP keys: SHIFT, the confirm tap, the looper's combos
FxCommon.h             what the effects share: the key's fade, smoothed settings, the base class,
                       tone lowpass, press envelope, stereo delay line
Fx*.h                  one effect each: Freezer, Shifter, Folder, Crusher, Filter, Flanger,
                       Resonator, Slicer, Warble (wow & flutter), TapeStop, Delay, Reverb
LICENSE-kastle2        the MIT license of the effects ported from Bastl's Kastle 2 FX Wizard
LedColors.h            the LED colours
DJFilter.h, BasicMMF.h WAVE's DJ filter
granularDelay.h        TEMPO's tempo-synced delay, without its freeze
reverb.h, fx_engine.h  TEMPO's reverb
TempoClock.h           the tempo and the shared 12 PPQN pulse position for the clocked effects: from the
                       loop, MIDI clock, taps or free running
TapTempo.h             tap tempo
Looper.h               the looper: recording, quantized end, playback, speed, scrub
MidiClock.h            MIDI clock input over TRS and USB; hands the rest to MidiControl.h
MidiControl.h          MIDI control: notes as keys, CCs, program changes, FRIZZ's SysEx (keys,
                       knobs, queries answered over USB for remote.py, the developers' 0x3E)
MidiOut.h              MIDI out: the clock and the loop's Start, Stop, Continue, Song Position,
                       out of the jack and over USB, if the settings page asks
NormalPage.h           the play page: routes the controls (VOLUME, PLAY/LOOP, transport, FX and scene keys) and draws the LEDs
SettingsPage.h         the settings page (the mode switch up): MIDI channel and transport, mono,
                       the clock factor, the LED brightness, the input (AUX or the mic), the
                       battery on VOLUME
LedSignal.h            the play page's short LED signals: 3 red or white blinks, a flash
ui.h                   page plumbing: events, page switching
EventLog.h             the bug report: every key, knob and MIDI event since power-on, written as
                       a twin script to /FRIZZ/bug-N.txt on SHIFT + VOLUME held (settings page)
Bench.h, BenchProfile.h
                       the CPU bench (only in FRIZZ-bench.bin, `make BENCH=1`) and its cycle marks
limiter.h, EnvFollower.h
                       the safety limiter and the VU meter
MicFilter.h            the built-in mic's highpass, TAPE's (without its notches)
hardware.h             the CHOMPI hardware: encoders, keys, switches, LEDs, battery
encoder.h / .cpp       encoder driver
temp_led_stuff.h       LED driver (ui_utils.h: LED flush/clear helpers)
BootPage.h, RainbowWavePage.h
                       boot animation, rainbow-wave animation
chompi_sram.lds        linker script (the firmware runs from SRAM, placed there by the bootloader)
```

## Rules the code follows

- **No file I/O and no blocking calls in the audio callback.** FRIZZ reads the SD card once at
  boot (the FX scenes and the master settings, after changing into `/FRIZZ`, which it
  creates on a new card) and writes it only from `MainLoop`, when a scene is saved, copied or
  deleted, 2 s after the compressor's knobs, the mono input or the MIDI settings were last
  changed (`SceneStore.h`), or for a bug report (`EventLog.h`).
- **MIDI is read in the audio callback, acted on in `MainLoop`.** `MidiControl.h` only notes
  what came (keys, detents, a table of controller values); the play page takes the rest in
  `MainLoop` and answers queries over USB from there, never from the interrupt.
- **Card buffers:** FatFs hands the whole sectors of a read or write straight between your
  buffer and the SD card's DMA. So a buffer for the card lives in internal RAM (a global or a
  member of one: not on the stack, which is in DTCM, and not in SDRAM, neither of which the
  DMA reaches reliably), is aligned (`alignas(32)`, for the cache's lines), and every write
  of it starts at its beginning on a sector of the file: write whole sectors, keep the rest
  for the next write (`EventLog::Flush`). Otherwise the sectors come out shifted, bytes
  repeated and others lost; the twin's card does the same (`test/host/fatfs.h`).
- Large buffers (the delay, the freezer, the tape stop) live in SDRAM (`DSY_SDRAM_BSS`) and are
  cleared at boot (`ZeroSDRAM`). The loop and the event log, always written before they're
  read, are in SDRAM too but not cleared (`SDRAM_NOINIT`, `.sdram_noinit`): what goes there
  must never be read before it's written.
- `__attribute__((optimize("-O0")))` and similar per-function overrides are deliberate
  workarounds inherited from the stock firmware. Don't remove them as leftovers. Code that runs
  rarely (`EVENT_LOG_ONCE`, `MIDI_CONTROL_ONCE`) is built for size (`-Os`, noinline): code
  space is the tightest memory ([`docs/CAPACITY.md`](../docs/CAPACITY.md)).

## License

MIT, see [`LICENSE`](../LICENSE). The effects ported from Bastl Instruments' Kastle 2 FX Wizard
carry their own MIT license in `code/src/LICENSE-kastle2`. [`THIRD_PARTY.md`](../THIRD_PARTY.md)
lists everything FRIZZ builds on.
