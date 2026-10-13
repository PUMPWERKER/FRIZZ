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
./all.sh          # everything, side by side, one line each: about a minute on 12 cores, the first ~80 s
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
./flash.py --test     # builds FRIZZ.bin, restarts the CHOMPI into the launcher, sends it to the
                      # test slot (12), as FRIZZ-TEST: a branch's build
./flash.py --bench    # FRIZZ-bench.bin to slot 11
./flash.py --no-build # the build that's in code/src/build/ (build-bench/ with --bench), as it is
./flash.py --run 11   # starts what's in slot 11 already, sending nothing
./flash.py --slot 13  # a beta's build, from a worktree of its v* tag, to key 13 (below)
./flash.py --list     # what's on each key: restarts FRIZZ into the launcher, a loop in RAM lost
```

Each send prints the md5 of what it sends, the name of that build in a pull request.

**The keys** (#171): 10 is the latest release (`FRIZZ`), 13 the latest beta (`FRIZZ-BETA`),
both the user's; 12 a branch's build under test (`FRIZZ-TEST`), 11 the bench, 15 the USB
storage firmware. `flash.py` sends to 10 or 13 only when the key is named (`--slot 10`,
`--slot 13`) and only a release's or a beta's build: the checkout stands on a `v*` tag with
nothing changed or added in `code/`, and what's sent is the build `make` makes there (a file
named too: the same bytes). So a release or a beta goes on from a worktree of its tag (`git
worktree add ../FrizZ-v1.0.0 v1.0.0`, then `./flash.py --slot 10` there), when the user asks.
A bare `./flash.py` sends nothing and says which options there are; the bench never goes to 10
or 13. Sessions never send to 10 or 13 unasked (CLAUDE.md).

**One at a time.** Every tool here takes a lock (`tools/chompi.py`, in `/run/user/UID`) when
it first reaches for the CHOMPI, and waits while another has it, saying who; what doesn't
touch it (`measure.py compare`) never waits. `tools/chompi.py hold CMD` keeps it over a whole
sequence of tools, `tools/chompi.py hold` until Ctrl-C (for playing it by hand).

**Which build runs** FRIZZ says itself (the developers' SysEx's IDENT, below): the CRC-32 and
length of the image it runs, which `tools/builds.py` prints next to the md5, and whether it's
the bench. `flash.py` notes the build it sends to a slot, beside the lock, and after sending
asks the build running and fails if it isn't the one sent; `--run N` says which build it
started. `remote.py` and `measure.py` find the slot of the build running by those notes (one
started by hand too), say which it is, take `--slot 10|12|13` for one in particular, started
unless it's the one running, and never take the bench for FRIZZ. A FRIZZ from before IDENT
(or a build the tools didn't send) can't say, and answers as FRIZZ-TEST and the bench do; for
those the tools go by the slot they last started, also noted beside the lock (`hold` without
a command forgets that note).

**Garbled USB names** (#88): now and then (2 in ~310 starts) FRIZZ comes up with its USB
names one string off (product `Љ`, maker `FrizZ`); the tools find it by the Daisy's USB id
then, and say so. A tool that starts FRIZZ, FRIZZ-BETA, FRIZZ-TEST or the bench (`flash.py`, `--run`,
`card.py --then`, `remote.py`) and finds it so starts the slot again from the launcher, twice
at most, sending nothing, then asks for the power switch. `tools/chompi.py fix-name [SLOT]`
does it for one found running so: it starts the slot noted as running (10, 11, 12 or 13) again.

Sending replaces whatever is in that slot, so set yours (`FRIZZ_SLOT`, `BETA_SLOT`,
`BENCH_SLOT`, `TEST_SLOT`, or `--slot`) if your keys differ. Without `--slot` (`--test`,
`--bench`), it refuses a key that holds another firmware than FRIZZ (`--slot` replaces it
anyway). Linux only (ALSA), Python 3, no
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
`ident`, `ram-only`, `get GROUP`, `set GROUP INDEX VALUE`, `bench [--wait] [--out FILE]`,
`probe`, `levels`, `signal sine|saw|noise HZ LEVEL` / `signal off`, `reseed [SEED]`,
`battery`, `reset`, `crash [--clear]`, `crash-test` and `loops [--scan]` / `loops load BANK SLOT
[--fault no-flow|past-end|cut-short|reinit] [--check WAV]` / `loops write KB CHUNK` (the loop
loader, #70: a load's time, the audio callback's load meanwhile, MainLoop's longest step, its CRC
against WAV's data) (below), and `play SCRIPT [--cpu] [--fresh]`, which plays a twin
script (`twin/scenarios/`, a bug report) on the device: its keys and knobs over SysEx at the
script's times, its MIDI and clock, its `expect led` lines checked against the device's LEDs, at the
brightness the card's `led_brightness` sets (25-100 % by 5: the levels that fit every line,
said when it isn't 100 %). Its `input sine HZ AMP` goes as the developers' test signal
(`SIGNAL`, below), which takes AUX's place where the twin's sine does, in the input the audio
callback hands the engine, at the same full scale, so the effects hear what they hear on the
twin (#219); `input off` and the script's end, however it ends, turn it off again. Unlike the
twin's, it takes the mic's place too, its frequency is whole hertz and its phase starts
afresh with each line. Its other inputs (`input wav`, `mic`, `jack`) and the rest only the
twin has (the card, the battery, power-on) are skipped, saying so.
With `--cpu` it reports the worst load FRIZZ.bin itself had, measured in the audio callback as
the bench does, and how many blocks took longer than a block, so a scenario can be tried for
crackles on the build that plays, not the bench's (it says how many readings went unanswered,
if any). Whatever ends a script (an error, Ctrl-C), it lets go of the keys it holds, gives
the mode switch back to the hand and turns the test signal off.

A script plays on FRIZZ as it stands: a twin script starts from power-on, but on the device the
scripts played before it since FRIZZ started have left the effect selected, its knobs, the
page or a loop in RAM, so its `expect led` lines can fail after another one (#123). And it
starts from the player's card, not a fresh one: a script's `set GROUP INDEX VALUE` goes as
the developers' `SET` (below), as `./remote.py set` sends it, so the scenarios set what they
depend on first, the compressor's knobs or a setting, to what a fresh card has (#214); a `set`
FRIZZ refuses fails its line.
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
page (`twin/scenarios/settings.txt`, most bug reports, #58; its upper G# and A# too, as
each source's effects and live are saved, `mic.txt`, #152, #177), VOLUME turned on its page
2, the input gain
(`volume.txt`, #152), and the compressor's knobs (`comp.txt`: its key, then knobs 1-4, or with
it in focus by CC 23; its CCs themselves never reach the card, #97), and a script's `set` of
a setting, the compressor or the input gain (most scenarios' first lines, #214) go to
`/FRIZZ/frizz_master.txt`, with no `.bak`; SAVE / COPY / DELETE confirmed (`scenes.txt`) or a
scene put over SysEx to `/FRIZZ/frizz_scenes.txt`, whose `.bak` keeps only the one before
(#78).

The setters FRIZZ saves (`channel`, `transport`, `source`, `out`, `scene put`, and `set` for
`settings`, `comp` and VOLUME's input gain) keep the card out of it the same way unless
`--save`: the change stays in RAM, and on a slot other than the test slot FRIZZ is started
again from the card afterwards, so it saves again, the change gone (#171). A FRIZZ from before
PERSIST is refused without `--save`. Before that restart, and at the end of a script, `IDENT`
says whether FRIZZ still keeps the card out of it: one that restarted meanwhile (which ends
PERSIST 0) wrote what came after to the card, and the command fails, saying so. It reads where the
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
| `00` IDENT | — | version (1), kind (0 FRIZZ, 1 the bench), the image's CRC-32 (5 groups) and length (3) as the bootloader loaded it (`zlib.crc32` of the `.bin`; 0 on the twin), uptime ms (5), MainLoop's passes (5), `RCC_RSR` at boot (5: the resets since FRIZZ last started, which clears it once read, so a watchdog reset can be told, below), flags (1: RAM only) |
| `01` PERSIST | `00` | flags. From now until a restart nothing is written to the card: a scene saved, copied or deleted, the master settings, as if written (the page shows them saved). One way: a restart (`flash.py --run N`) writes again, reading the card as it was. Nor a bug report (`bug-N.txt`): SHIFT + VOLUME's hold then counts as failed (#171) |
| `02` GET | GROUP | GROUP, COUNT, then COUNT values in 14 bits |
| `03` SET | GROUP INDEX VALUE(2) | GROUP INDEX; applied in MainLoop through the setters the knobs and keys use, and saved as theirs are (unless RAM only) |
| `04` BENCH | `00` | the bench's state: running, settling, the segment, how many, written, stored, `cpu.txt`'s length (3) |
| | `01` OFFSET(3) | up to 32 bytes of `cpu.txt` from OFFSET, as it went to the card (status 3 while it runs); FRIZZ.bin answers status 2 |
| `05` PROBE | `00` | the clock: the source set (0 Auto, 1 TRS, 2 USB, 3 internal), the one counted (0 none, 1 TRS, 2 USB), the factor (0 x1/2, 1 x1, 2 x2), flags (1 a clock counted, 2 a loop sets the effects' tempo, 4 a tempo set, by a clock, a loop or taps, 8 the pulses running backwards, 16 transport followed), its tempo by the factor and as sent (3 each, BPM x 100), the ticks counted (5), how often it locked (3), the effects' tempo (3, x 100) and whole (2), the FX clock's pulse (3) and a loop's pulses (3) |
| | `01` | the looper: its state (0 empty, 1 recording, 2 playing, 3 paused), flags (1 closing, 2 erase waiting, 4 erasing, 8 can record quantized), length and position in frames (4 each), beats (3), semitones + 64, speed and the speed now (2 each, x 1000 + 2000), rewinds by Start (3) |
| `06` LEVELS | `00` | the effects' meters, as their keys show them, 0-16383 each, by FxId |
| | `01` | flags (1: the peak meters just started), the VU meter (2), the compressor's reduction (2, dB x 100), the peaks since the last ask (2 each, 8192 full scale, up to twice it): the input as the engine takes it (mic, AUX L, R), the headphones L, R, the line outs L, R |
| `07` SIGNAL | WAVE FREQ(2) LEVEL(2) | WAVE. 1 sine, 2 saw, 3 noise at FREQ Hz (1-16383) and LEVEL (16383 full scale) in place of the mic and AUX, from where the engine takes them; `00` alone the input again |
| `08` RESEED | SEED(5) or — | the seed, then the states (5 each) of chaos's, the slicer's, the delay's and wow & flutter's random numbers, each from its power-on seed xor SEED: 0 is power-on's. Without SEED it only reads |
| `09` BATTERY | — | the level (0 full, on the cable; 1 above 3.3 V; 2 3-3.3 V; 3 below 3 V), the level the check last read, the check's step (0 resting, 1-2 reading at 3.3 V), 1 if the last read failed, the last 8 reads (2 each, the newest lowest) of the cable (VIN_GD), the battery below the threshold, a legacy cable and the input current limited, then the MP2722's status registers 0x11-0x16 (2 each) |
| `0A` RESET | `52 53 54` ("RST") | no answer: the chip resets at once, into the launcher (without it, FRIZZ starts again). Taken in the audio callback as it comes, so it works with MainLoop hung; it doesn't wait for the card, as `10`'s restart does. Sent once: a FRIZZ that started again meanwhile would take a second one |
| `0B` CRASH | `00` | flags (1: a crash recorded), its kind (1 the watchdog, else the exception: 3 HardFault, as every fault is, since MemManage, BusFault and UsageFault aren't enabled and escalate; CFSR tells which), how many since cleared (3), uptime ms at the fault (5; the watchdog's: MainLoop's last pass), the CRC-32 (5) and length (3) of the build that crashed, `RCC_RSR` at that start (5), SP (5), and `FLASH_OPTSR_CUR` (5), the option bytes, whether or not one is recorded; zeros for none |
| | `01` | PC, LR, xPSR, CFSR, HFSR, MMFAR, BFAR (5 each) |
| | `02` | clears it -> flags |
| `0C` TEST | WHAT | the bench only (FRIZZ.bin answers status 2): 1 MainLoop hangs (after answering), 2 a bus fault in MainLoop (a read where nothing is), 3 the audio callback hangs, 4 an undefined instruction in it (3 and 4 unanswered, as they come) |
| `0D` LOOPS | `00` | the loop loader (`LoopLoader.h`, #70; FRIZZ.bin only, status 2 in the bench): its state (0 idle, 1 scanning, 2 loading, 3 loaded, 4 failed, 5 checking, 6 writing, 7 written), the bank and slot loading, the result (`loops::Result`: why a load or the write test stopped, 0 if it didn't), the SD error (5, `HAL_SD_ERROR_*`), bytes read (4) of (4), ms (3), runs (3), MainLoop's longest step since the last `00` in µs (3), the SDMMC's CLKCR (5), the card recovered (1) and set up again (1), the file's fragments (2) |
| | `01` BANK | 1 while the scan runs, then the bank's 15 slots' results (0 empty, 1 mono, 2 stereo, more: refused, why) |
| | `02` BANK SLOT | the slot's result, frames (4), its data's offset in the file (5) |
| | `03` / `04` BANK SLOT FAULTS | scans again / loads the slot (banks and slots from 0). FAULTS for a test: 1 the flow control off for it, 2 its third run read past the card's end, 4 its third run given no time, 8 the recovery after it sets the card up from scratch (`HAL_SD_DeInit`, `BSP_SD_Init`). Refused (status 1) while something runs |
| | `05` [`01`] | the state, the CRC-32 (5) of the data loaded (zlib's); with `01` it starts, 16 KB a MainLoop pass |
| | `06` KB(2) CHUNK(2) | the write test: KB (1-1024) written to `/FRIZZ/LOOPS/T/w.tmp` in CHUNK-byte writes, the `.bin` it replaces deleted, renamed, read back, compared, then the file, the folder and a `/FRIZZ/LOOPS` it made deleted |

The groups of GET and SET: `00`-`1F` an effect's knobs by its FxId (`FxParams.h`: 0-3 page 1,
4-7 page 2), `20` the compressor's, `21` the latches (index by FxId, 0 or 1), `22` VOLUME's
(0 output gain, 1 input gain, 2 mix, 3 headphone feed), `23` the card's settings as
`masterfile::kLines` lists them (`MasterSettings.h`: mono, MIDI channel, transport, clock
factor, clock source, MIDI out, LED brightness, the effect channels over TRS, MIDI feedback,
AUX dry, the input gains of AUX and the mic, the mic dry, AUX live, the mic live), their values
as `frizz_master.txt` has them.
The groups don't follow the MIDI map (CCs), so a tool keeps working when it changes. SETs
aren't in the event log: a bug report taken after them won't replay them.

LOOPS drives the loop loader until it has its controls (#70): `remote.py loops` (below) and
`tools/loopfiles.py`, which makes its test files and puts them on the card in a test folder
over USB storage (`card.py mount` in a hold), `/FRIZZ/LOOPS/A-C`, A/02 and A/12-13 written in
pieces so they lie in fragments; `loopfiles.py clean ROOT` deletes `/FRIZZ/LOOPS` again. The
write test writes the card whatever PERSIST says, only in `/FRIZZ/LOOPS/T`, and deletes what
it wrote.

PROBE to BATTERY are for tests without hands or an audio interface. All but SIGNAL and RESEED
only read: BATTERY answers what the last reads left (the charger isn't asked, its voltage only
known as above or below 3 V and 3.3 V). SIGNAL and the peak meters live in RAM until FRIZZ
restarts and cost the audio callback one branch until asked; the first LEVELS starts the
meters, so `remote.py levels` asks, waits 0.25 s and asks again. RESEED's states let a tool
count how many numbers each has drawn since the seed (`remote.py reseed`): two runs from the
same seed draw the same numbers. The reverb's modulation (DaisySP's ReverbSc, vendored) has
random numbers of its own, which RESEED leaves running on from power-on. Neither is in the
event log.

**A hung or crashed FRIZZ** (#105, `Crash.h`). RESET brings back a FRIZZ whose MainLoop hangs
without the power switch: `remote.py reset`, and `tools/chompi.py` sends it itself when FRIZZ
doesn't get to the launcher (a restart taken but not done within 15 s, or FRIZZ by its USB
names silent for 20 s), before it asks for the switch. A fault (HardFault, MemManage,
BusFault, UsageFault) writes a record of its registers to the backup SRAM, through a vector
table of FRIZZ's own in the DTCM (libDaisy's `HardFault_Handler` stops at a breakpoint, which
locks the core up without a debugger), and resets the chip; nothing goes to the card. The
watchdog (IWDG1) is fed only at the top of the audio callback and resets the chip once it
hasn't run for 1 s: a MainLoop that hangs or blocks (a card failing for 30 s, a bug report)
doesn't set it off. It starts once audio runs, after the boot's card reads and battery checks,
and a reset stops it (the option bytes' `IWDG1_SW`: started by software; CRASH says), so the
launcher and the other slots never run with it. The next FRIZZ start records a watchdog reset
(`RCC_RSR`, which FRIZZ clears once read), with when MainLoop last ran.
`remote.py crash` reads the record, whatever slot runs (the backup SRAM keeps it through the
launcher and other builds, until a power-off or `--clear`): the kind, the build that crashed
(by its CRC-32, as IDENT names builds) and the `addr2line` line for its PC and LR, with the
`.elf` if that build is the one in `code/src/build/` or `build-bench/`. `remote.py crash-test`
(the bench only) hangs or faults on purpose, to check all three on the device.

**From the card:** copy `build/FRIZZ.bin` to the SD card and power on, as described in
[`INSTALL.md`](../INSTALL.md). FRIZZ is a `BOOT_SRAM` app: CHOMPI's bootloader copies it from
the card into QSPI flash and runs it from SRAM. Standard Daisy flashing advice doesn't apply.

Every CHOMPI already has the bootloader, and an SD update never touches it. Only a blank or
erased Daisy Seed needs it installed. To do that, hold BOOT and tap RESET to enter DFU mode,
then run `bin/install_bootloader.sh`.

## Measure timing and sound on the device

`tools/measure.py` measures what the twin can't, on the real CHOMPI, with no hands at the
panel. It needs an audio interface: its line outputs 3+4 into AUX, the CHOMPI's headphone out
into its inputs 3+4 (the left, input 3, is recorded; the bench is wired so, the line out stays
free), its MIDI out into the TRS jack (found by name,
the Scarlett's `HiFi__Line4__sink` and `HiFi__Line8__source`; `FRIZZ_AUX_SINK`,
`FRIZZ_LINE_SOURCE`, `FRIZZ_TRS_MIDI` set them). Outputs 1+2 are the user's monitors: the
tools never play into them. In the Scarlett's mixer (alsa-scarlett-gui), line outputs 3+4 get
PCM 3+4 directly, not a mix, or nothing played into `HiFi__Line4__sink` reaches AUX. Linux
restores the mixer from `/var/lib/alsa/asound.state` whenever the interface is plugged in: once
it's set, `sudo alsactl store` keeps it (it had reverted to Mix G/H, a loop through AUX). Python 3 with numpy (a venv:
`python3 -m venv ~/.venvs/frizz && ~/.venvs/frizz/bin/pip install numpy`).

```bash
cd firmware/tools
~/.venvs/frizz/bin/python measure.py clock --mode pairs --bpm 120     # FRIZZ's tempo from a USB clock
~/.venvs/frizz/bin/python measure.py drift --bpm 174 --seconds 180    # a 1-bar loop against a TRS clock
~/.venvs/frizz/bin/python measure.py material --music a.wav b.wav --out music.wav
~/.venvs/frizz/bin/python measure.py fx KEY_13 --cc 17=100 --material music.wav --out rev-main
~/.venvs/frizz/bin/python measure.py fx none --material music.wav --out dry   # nothing latched
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
- `fx` latches the effect alone over SysEx SET (by its key or its name; `none` latches
  nothing, the dry line to compare against), reads the latches back and stops unless exactly
  that one took (#187: the key held and SHIFT tapped over SysEx didn't always latch on the
  device, and every effect recorded the dry line), sends the CCs on the effect's channel
  (e.g. `--cc 21=80`, its Level), plays material into AUX and records the line out; it says
  what was latched and the knobs FRIZZ has, and puts the latches and the effect's knobs back
  afterwards. `compare` gives two recordings' level and octave bands, for an A/B of two
  builds. Real music (`material --music`) is the better test here; level and bands don't
  show an effect's timing (an echo's place), which takes a script of its own.

`tools/loopclip.py [--source aux|signal]` asks whether the loop distorts where the live input
doesn't (#152): a 1 kHz sine at rising levels, into AUX or as FRIZZ's own test signal
(`remote.py signal`), the line out recorded with the input alone and then with a loop of it
alone, the two recordings' THD side by side. Run it on an empty looper.

For an A/B, send each build to the test slot in turn (`flash.py FILE --slot 12`) and measure
from a fresh start each time; `remote.py load` and `remote.py play SCRIPT --cpu` give the load.

### The MIDI suite: the release test's MIDI items, unattended

`tools/midisuite.py` runs the release test's MIDI clock, sync, MIDI out and MIDI control
items (#55, sections 7 and 8; #114) on the device, with the computer as an idealised DAW: a
clock over USB and out of the interface's MIDI into the TRS jack (steady, fast, nudged, in
pairs, started late), Start / Stop / Continue / Song Position, the clock source and factor,
transport following, CCs on the effect channels and the global one, RPNs, morphs over CC 3 /
80, MIDI out and feedback, a DAW echoing back what it gets. It reads what FRIZZ makes of it
(STATE's tempo, looper and position, GET's knobs and latches, the LEDs) and times every byte
its MIDI out sends, on USB and, through the interface's MIDI in, on TRS. Each case says what
it sent, what it read, its tolerance and the #55 item, PASS / FAIL / SKIP; the end is a
summary in Markdown for the PR, with the cases still open with the other wiring and what
stays with the user. Python 3 only (no numpy); `tools/midiport.py` has the clock and the
timed capture it shares with `measure.py`.

One command per wiring runs it all; for a release candidate, its `FRIZZ.bin` (built at its
tag, md5 checked) goes to the test slot instead of the branch's build:

```bash
cd firmware && tools/chompi.py hold sh -c '
  ./flash.py FRIZZ.bin --slot 12 &&
  { tools/midisuite.py --out card/midisuite.md; echo "suite: $?"; }
  ./flash.py --run 10'
tools/midisuite.py --list                           # the cases, and what wiring each needs
tools/midisuite.py --cases out-loop,start-spp -v    # a few, every check as it's made
```

- **The wiring** has one TRS adapter, one way or the other: the CHOMPI's TRS out into the
  interface's MIDI in (A: MIDI out is captured on TRS, a clock passed on from USB), or the
  interface's MIDI out into the TRS in (B: TRS clock and CCs in, MIDI out seen over USB). The
  suite finds out which first (MIDI out on TRS for a second; a 137 BPM clock out of the
  interface that the tempo follows), or `--wiring out|in|both|none` says. What the wiring
  can't show is a SKIP, and the summary lists those cases with the `--cases` to run once the
  adapter is turned round; a case the wiring can show that sees nothing is a FAIL.
- **The card stays out of it:** it asks for PERSIST 0 first and refuses a FRIZZ without it.
  Each case sets the settings it needs (from the global channel 16, everything else off), and
  the knobs, latches, VOLUME's levels and the scenes 1-2 the morph cases put are set back
  after each case, the settings at the end, in RAM. Before each case and before putting
  things back it asks FRIZZ (IDENT) whether it still keeps the card out of it: one that
  restarted meanwhile would write the card with every setting after, so the run stops there
  (FAIL), nothing put back (#171). It plays on slot 12 (`--slot`).
- **Stopped:** Ctrl-C, SIGTERM or SIGHUP put things back first, then the summary; it exits
  130, 143 or 129 then, 1 for a FAIL or a failed putting back, 0 otherwise (`reaper.py`
  alike).
- **How long:** about 7 minutes. `loop-drift` and `out-x2` ("over a few minutes") measure
  40 s; `--long` makes it 3 minutes each, `--quick` 8 s (the host check). `loop-drift`
  records 4 bars: a 1-bar loop's length, taken from a USB clock's ticks, scattered by up to
  ±2.3 ms a minute from one recording to the next on beta.1 (#20), a 4-bar loop's by 0.4. Its
  two halves alike say a drift is the recording's length; apart, that the glide back to 1x was
  still settling. "No jumps" is the position stepping (the median distance from the line,
  every two beats), not a single late tick: USB, or this computer busy.
- **What it reads, and what not:** while a morph runs, GET shows the scene it goes to (the
  knobs "already show the scene"), so the morph cases read MIDI's crossfader off the transport
  LEDs; what is heard on the way stays the user's. Checks on timing (rates, jitter, a tempo
  held) are marked `[timing]` with `-v`.
- `test/midisuite.cpp` runs it on the twin, wired every way, and `MIDISUITE="--cases ID -v"
  test/unit.sh midisuite` plays it there once, to work on a case without the CHOMPI.

### REAPER: a real DAW against FRIZZ

`tools/reaper.py` (#114, step 2) plays the DAW items with REAPER 7 itself
(`~/Apps/REAPER/reaper`, or `FRIZZ_REAPER`): its MIDI clock at 120, 200 and 300 BPM, its
Start / Stop / Continue / Song Position on play, stop, locating and a loop range's turns,
automation lanes on the effect channels, a track monitoring FRIZZ's input, and its clock's
jitter against a clock from this computer (#83); and the other way round, REAPER synced to
FRIZZ's MIDI out (`daw-follows`, #150); and with music (#55's DAW items by ear): REAPER
playing a song into AUX and recording the line out (`daw-loop-audio`, `daw-loop-restart`,
`daw-follows-audio`, below). Wiring A (the CHOMPI's TRS out into the interface's MIDI in)
shows what FRIZZ passes on; without it the jitter is a SKIP.

```bash
cd firmware && tools/chompi.py hold sh -c '
  ./flash.py --test --no-build &&
  { tools/reaper.py --out card/reaper.md; echo "reaper: $?"; }
  ./flash.py --run 10'
tools/reaper.py --list                          # the cases
tools/reaper.py --cases daw-transport -v        # one, with REAPER's log
tools/reaper.py devices --audio dummy           # what REAPER sees, FRIZZ untouched
# the cases with music: numpy, so measure.py's venv
~/.venvs/frizz/bin/python tools/reaper.py --audio jack \
  --cases daw-loop-audio,daw-loop-restart,daw-follows-audio --out card/reaper-audio.md
# daw-follows-audio's timing needs a song whose bars differ: --material generated
```

- **One REAPER per audio mode** (`--audio jack,dummy`): the first runs every case, the last
  the jitter again and `daw-ccreset` with REAPER's preferences as they come, then the synced
  one below. About 7 minutes;
  `--seconds` sets the jitter's window (30 s).
- **REAPER has FRIZZ's USB node to itself** while it runs, so remote.py and flash.py can't
  reach FRIZZ meanwhile. The tool sends FRIZZ's SysEx through REAPER instead (a command file
  `tools/reaper.lua` reads ~30 times a second) and takes the answers from REAPER's MIDI input,
  so midisuite.py's `Rig` works on it, a query taking ~70 ms. Before and after, the tool is on
  the node itself: RAM only (PERSIST 0), the wiring, the jitter's reference, everything put
  back.
- **The user's REAPER stays untouched:** a configuration of its own in a temporary directory
  (the license copied in), deleted after; a file in `~/.config/REAPER` or `~/Apps/REAPER`
  newer than the start fails the run. Its window shows for a moment, and an evaluation copy
  its notice. Nothing reaches Line 1/2: a silent project, the master muted, its JACK ports
  linked to nothing (`jack_options=2`; a link fails the run), the music's REAPER only to AUX
  and from the line out (below). A REAPER that runs no script
  within 20 s, or doesn't quit when asked, has a dialog in the way: killed, and said so.
- **JACK** (through pipewire-jack, the default) takes FRIZZ's MIDI as PipeWire's bridge ports
  (`Midi-Bridge:FrizZ: MIDI 1`), as a desktop REAPER here does; **dummy** uses ALSA's raw node
  (`hw:FrizZ`). Its CC reset on stop and at a loop's turn is off (`ccresettab=cc0-120=-1`)
  but in the last REAPER.
- **REAPER following FRIZZ** (`daw-follows`) runs in a REAPER of its own after the others,
  started only when the case is: its project synchronized to its MIDI input, its clock out
  off (`midiouts_clock=0`; the tool's SysEx still goes out through it). The switch is the
  project's, not reaper.ini's ("External timecode synchronization", the transport's right
  click): the project file's `SMPTESYNC 5 30 100 40 1000 300 4223 0 0 0 0`, which
  `reaper.lua` opens first and reads back (`projsmptesync` 5: on, and start playing on a valid
  clock; `projsmpteinput` 4223: MIDI clock and Song Position from all inputs). FRIZZ at 100
  tapped, a free loop of 4 beats, MIDI out on TRS and USB: REAPER starts with the loop's
  Start on its beat 1, its beats in step with the loop's, pauses on a pause (play state 2)
  and goes on at PLAY's Song Position, and follows the clock factor. REAPER keeps its own tempo (120) and
  play rate (1.0) and varies its speed to keep its beats on the clock's, holding still about
  a second first (it seeks 1 s ahead), and only within about half to twice its tempo (seen
  smooth at 0.8x and 1.6x): x1/2 against its 120 (0.4x) goes in jumps, as did x2 against a
  project at 60 (3.3x, a try by hand), so x1/2 is judged with its project at 60. `--quick`
  (the host check) leaves out the x1/2 against 120, which is only noted, and listens 2 s to
  each clock factor instead of 4.
- **With music** (`sound` cases): a REAPER of its own after the first, JACK only, its audio
  linked by the tool and nothing else: its outputs 3/4 into AUX's sink (measure.py's
  `FRIZZ_AUX_SINK`, the Scarlett's line outputs 3+4), the line out's left (`FRIZZ_LINE_SOURCE`)
  into its input 1; any other link fails the run, and so does a Scarlett whose Analogue Output
  3/4 isn't PCM 3/4 (read with amixer, never set). A track plays the song to outputs 3/4 alone
  (no master), at a gain that puts its peak at -7 dBFS; a track records input 1, not monitored.
  The song: `--material FILE`, a WAV of whole bars at 87 BPM (`FRIZZ_DAW_MATERIAL`; else the
  longest WAV in `card/audio`, the main checkout's if this one has none; `generated`:
  measure.py's notes and noise), kept out of git. They need numpy: without it they're a SKIP
  saying so, the other cases run on. `--keep DIR` keeps the takes and the song (in `card/`).
  - FRIZZ records its loop of 4 bars from notes on a track of REAPER's (PLAY 46 held, LOOP 47
    on bar 4, channel 16), as a DAW user would: a quantized recording starts on the press
    (MANUAL), so a press by SysEx lands wherever this computer is. The notes' track goes
    once the loop is in.
  - Each bar of the take is found in the song by correlation (GCC-PHAT, `measure.find()`)
    near where the time line puts it: before the loop the song through FRIZZ live (the dry
    signal: REAPER's latency compensation, about +38 ms off under pipewire-jack, plus the
    CHOMPI's own), after it the loop, against the dry signal. The CHOMPI's line out is upside
    down against AUX; the search takes either polarity and says which.
  - `daw-loop-audio`: the loop over `--minutes` (2), bar by bar, its drift in ms a minute, its
    length (PROBE) against 4 bars. `daw-loop-restart`: REAPER stopped and played from the
    start, the playhead into the loop's middle, a loop range; at x1 and x1/2. After Start the
    loop's start sits where its LOOP note put it, as REAPER's MIDI reaches FRIZZ against its
    audio; the other restarts are judged against that. A loop range's turns each start where
    REAPER's MIDI at the turn lands, on the edge of one of its blocks (0-21 ms at 1024
    samples), so each turn is judged steady and within a block. `daw-follows-audio`: the loop
    recorded, REAPER restarted synchronized to FRIZZ (as `daw-follows`), playing the song into
    AUX, FRIZZ's mix in the middle (CC 87 64): the line out (pw-record; REAPER recording would
    run on by itself, deaf to a Stop) carries the song as REAPER plays it and the loop, found
    apart near where REAPER's trace puts the song; the transport turned into a standstill
    (#174) and pressed back to 1x, the loop against the song before and after. A song whose
    bars are all alike (a drum loop) can't be told from the loop: said so, not judged;
    `--material generated` measures it.
  - On the twin (`test/reaper.cpp`) the stand-in plays the song into the twin's AUX and records
    its line out (`test/ptytwin.h`'s Audio), with measure.py's generated material.
- **Stopped and restarted** as `midisuite.py`: Ctrl-C, SIGTERM or SIGHUP put things back and
  close REAPER first (exit 130, 143, 129), and a FRIZZ that restarted stops the run (#171).
- `test/reaper.cpp` runs it on the twin with `test/reaper_stub.py` in REAPER's place, and
  `REAPERCHECK="--cases ID -v" test/unit.sh reaper` plays it there once.

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
SettingsPage.h         the settings page (the mode switch up): MIDI channel and transport, the
                       clock factor, the LED brightness (knob 1), the input (its source, effects
                       and live, each source's own; mono), the battery on VOLUME
LedSignal.h            the play page's short LED signals: 3 red or white blinks, a flash
ui.h                   page plumbing: events, page switching
EventLog.h             the bug report: every key, knob and MIDI event since power-on, written as
                       a twin script to /FRIZZ/bug-N.txt on SHIFT + VOLUME held (settings page)
Crash.h                the crash record in the backup SRAM and the watchdog fed by the audio
                       callback (#105; the fault handler is in chompi_main.cpp)
Bench.h, BenchProfile.h
                       the CPU bench (only in FRIZZ-bench.bin, `make BENCH=1`) and its cycle marks
limiter.h, EnvFollower.h
                       the safety limiter and the VU meter
MicFilter.h            the built-in mic's highpass, TAPE's (without its notches)
hardware.h             the CHOMPI hardware: encoders, keys, switches, LEDs, battery
encoder.h / .cpp       encoder driver
temp_led_stuff.h       LED driver (ui_utils.h: LED flush/clear helpers)
BootPage.h             boot animation: the ripple from SHIFT
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
