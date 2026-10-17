# The virtual CHOMPI

FRIZZ's own firmware running on a PC: `chompi_main.cpp` and everything under it (the play page,
the engine, the card, MIDI clock), and libDaisy's UI, Switch, 4021 and MIDI code, compiled
unchanged with the host's `g++`. Only the board underneath is simulated (`host/`):

| On the device | In the twin |
|---|---|
| 28 keys and 6 encoder switches through five CD4021s, bit-banged by libDaisy's driver | the same driver clocking a model of the chain: keys debounce exactly as on the device |
| encoders 1-4 on a sixth 4021, 5 and 6 on their own pins | a full quadrature cycle per detent, 8 ms |
| 35 WS2812 LEDs fed by timer PWM + DMA | the DMA's buffer decoded back to the bytes each LED latches |
| MP2722 charger on I²C | a battery with a voltage, a charger plugged or not, shipping mode |
| SAI audio, 24-sample blocks at 48 kHz | the same callback, block by block: AUX L and R and the built-in mic in |
| AUX's jack detection on a pin | the pin, a cable in until a script pulls it (`jack`) |
| MIDI in over the TRS jack's UART | bytes fed to libDaisy's parser, at the next block |
| USB MIDI | bytes in to libDaisy's parser, at the next block; what FRIZZ sends kept for the script (`usb`, `replies`) |
| SD card | `test/host/fatfs.h`, a card in memory |
| the SDRAM buffers, cleared at boot | ordinary statics, zero from the start; the linker script's range to clear is empty, and an older firmware's `std::fill` at 0xc0000000 is skipped (`host/daisy_core.h`) |

The firmware's `main()` runs as a coroutine: each 0.5 ms block runs the audio callback, then
`main()` until it has used up the block (its time moves with `System::Delay*` and `GetNow`).
So the boot sequence, the 2 ms UI tick, the battery checks and the card writes run as on the
device, and a run is deterministic: the same inputs give the same samples and LEDs. It runs
13 (every effect on, a loop playing) to 20 times faster than real time.

What it can't show: anything about time on the chip. The CPU load (crackles), the caches and
memory placement, the codec, the card's speed. Those need the device (CLAUDE.md, *FRIZZ's
audio callback is CPU-bound*). Nor races between the audio interrupt and `main()`: here the
callback only comes between blocks, `main()` only gives way in `GetNow` and `System::Delay*`,
the charger's I2C reply arrives at once and each LED chain's DMA finishes once per block. A
race found on the device can be forced at one place: the firmware marks it
`FRIZZ_PREEMPT("name")` (nothing on the CHOMPI), and a check's `twin::PreemptAt("name")` brings
the callback there each time `main()` passes it, unless a `ScopedIrqBlocker` holds it off
(#302, `ui.cpp`'s `loops-switch-fade`), or several callbacks with `PreemptAt("name", blocks)`
(`midi-focus-race`: the jack's MIDI reaches MidiControl a block after it came).

## In the browser

```bash
web/serve.sh          # serves http://localhost:8765
```

Each load of the page first rebuilds the twin if the firmware or the twin changed (under a
second to check, about 20 s to rebuild), so reloading the page always plays the working tree
as it is; a build that fails shows its messages instead. The firmware's commit is at the top of
the page, with `+changes` when the working tree differs from it.

The panel is drawn from CHOMPI's own board file (`tools/board_layout.py` → `web/layout.json`):
every key, knob and LED where it sits on the board, the LEDs in the order their data runs, so
LED *n* on the page is the firmware's LED *n*. The firmware runs in a worker (Emscripten, its
`main()` as a fiber), about 13 times faster than real time with every effect on; it makes the
audio about 40 ms ahead of the speakers. No special server headers are needed, so any static
host would do.

- Keys: click; the computer keyboard (white keys Q..P and A..G, dark keys 1..0, SHIFT is Shift,
  PLAY Space, LOOP Enter); right-click holds a key down, for combos and for power-on with keys held.
- Knobs: scroll or drag up and down, click the centre to press.
- Into AUX: a test tone, an audio file (looped), or the mic / a line in. Out: the master out or
  the headphones. AUX's jack always has a cable in there, so FRIZZ's own mic input is for
  scripts (`mic`, `jack`).
- MIDI clock: one of its own at a set tempo, or a connected MIDI device's (WebMIDI).
- Battery: its voltage and the charger, for the low-battery lockout.
- Switching off and on restarts the firmware (a new worker); the SD card's files are kept in the
  browser and can be downloaded.
- Record the output as a WAV, and download what you played as a script: it replays in the
  browser (from power-on) and in `run.sh`, step for step, as both are deterministic.

Nobody has tried the mic input or WebMIDI on real devices yet.

`build.sh wasm` needs Emscripten: [emsdk](https://emscripten.org/docs/getting_started/downloads.html)
in `~/opt/emsdk` (or `em++` on the PATH); the developer guide
([`../README.md`](../README.md#3-test-on-the-host)) has the three commands that install it.

## On the command line

```bash
./run.sh -o out.wav -l leds.txt examples/filter.txt   # builds if needed, plays the script
./build.sh                                           # just build: build/frizz-twin, build/libtwin.a
```

`-o` writes the master out (32-bit float stereo WAV), `-l` every change of the LEDs (`-` for
stdout): the time in ms, the 10 panel LEDs (`pth`, in `NormalPage.h`'s numbering: 0 CHOMPI,
1-4 knobs, 5/6 transport, 7 PLAY, 8 LOOP, 9 VOLUME), then the 25 key LEDs (`smt`), each as
`rrggbb` scaled back to full (FRIZZ sends the panel at 1/11 and the keys at 1/4).

`../test/ui.cpp` (`unit.sh ui`) uses the same twin through `twin.h`.

## Two versions side by side

```bash
./compare.sh                 # HEAD against the working tree: a refactor must be bit-identical
./compare.sh origin/main HEAD   # what this branch changes for a player
./compare.sh v0.10 origin/main  # since a release
./ui-at.sh origin/main          # ../test/ui.cpp's checks on another version's firmware
./ui-at.sh origin/main sync     # any other twin check (midi, sync, ...) the same way
```

Any git ref works. Compare with `origin/main` after a `git fetch` rather than a local `main`,
which may lag behind what's merged.

`compare.sh` builds the twin on each version's firmware (once per commit, kept in
`build/compare/`), plays every scenario in `scenarios/` on both and compares what came out:
`bit-identical`, or from when the sound parts and how far below the signal the difference is,
and which LEDs differ how often (LED logs of different lengths, or one cut short, said as such;
`compare.py --selftest`, run by `../test/unit.sh compare`). The scenarios cover every effect on its own, effects latched
together with coarse turns and resets, the compressor swept, the looper through every speed,
scenes saved, recalled, morphed, copied and deleted, VOLUME's pages and the mix, and MIDI clock.
Their `expect` lines (LEDs that show a state, not a meter: LOOP recording, the transport, the
scene and mode keys, the settings page's keys, the knobs where they were turned) make them
checks too: `unit.sh ui` plays every scenario and fails on one that doesn't hold, so a change
that means to alter what a scenario shows updates its `expect` lines with it. Against an older
version, `compare.sh` prints the `expect` lines that don't hold there (`line N: ...`) and goes on
comparing: they don't make it fail.
It exits 0 when every scenario is bit-identical and 1 when one differs; 2 when it couldn't
compare: a twin that didn't build, a scenario the twin crashed in, or one `compare.py` couldn't
compare (said on its line, the scenarios after it compared all the same) or gave no line for. CI
fails on 2, not on a difference (`.github/scripts/against-base.sh`).
On the device (`remote.py play`) a scenario starts from the player's card, not a fresh one: so
it first `set`s, to what a fresh card has, every setting and compressor knob its `expect` lines
depend on (before `at 6000`, so the times stay as they were), and `unit.sh ui` plays each one
again on a card with the compressor's page 1 and most settings not as the factory has them
(`card:NAME`, #214).
A new one is just another `scenarios/NAME.txt`. Both versions run with the working tree's twin,
so a firmware from before the twin compares too (back to the move from `firmware/frizz/`).


## Scripts

One command per line, `#` starts a comment. The device is switched on at the first command
that needs it; time moves only with `wait` and `at`. Keys debounce for about 7 ms, as on the
device, so a script that lets go of SHIFT and turns a knob in the same millisecond has turned
it with SHIFT held: leave the gaps a hand would.

| Command | |
|---|---|
| `wait MS` / `at MS` | run for MS / until MS after power-on (after `booted`: after the main loop's start) |
| `booted` | run until the firmware's `main()` enters its loop (about 1 s); `at` counts from there. A bug report's times do |
| `down KEY` / `up KEY` / `tap KEY [MS]` | a key by its `Hardware::SwId` name: `KEY_1` .. `KEY_28` (white keys 1-15 are `KEY_1`-`KEY_15`; CHOMPI, PLAY, LOOP are `KEY_26`-`KEY_28`), `ENC_1_SW` .. `ENC_6_SW`. `tap` holds it 60 ms |
| `turn ENC N [MS]` | encoder 1-6 (SW1-SW6: 4 is knob 1, 1-3 knobs 2-4, 5 the transport, 6 VOLUME) by N detents. They play out in the background, 8 ms each (or MS), as a hand turns: `wait` for them before the next key. 8 ms is a fast turn, which knobs 1-4 and VOLUME speed up (#375): from 125 ms a detent they move their plain 1 % (VOLUME's 2 %), so a script setting a value turns them at `130` |
| `toggle 0\|1` | the mode switch, as the 4021 reads it: 0 down (the play page, where the twin starts), 1 up (the settings page) |
| `input sine HZ AMP` / `input wav FILE` / `input off` | what goes into AUX (a WAV loops) |
| `mic sine HZ AMP` / `mic wav FILE` / `mic off` | what goes into the built-in mic (a WAV's left channel), heard while the mic is the input |
| `jack in` / `jack out` | a cable in AUX's jack, as at first, or none: none at power-on (`jack out` before the first command that needs the device) or pulled later makes the mic the input (`chompi_main.cpp`) |
| `midi HEX...` | raw bytes into the MIDI jack |
| `usb HEX...` | raw bytes into USB MIDI, as a computer sends them (FRIZZ's queries are answered only there) |
| `clock BPM [jitter MS] [drift PPM] [usb] [ramp BPM MS] [seed N]` | a running MIDI clock into the jack, or with `usb` over USB (one of each can run): each tick up to MS early or late, the sender's clock PPM slow, USB's 1 ms frames, a ramp to another tempo over MS (`clockgen.h`). `clock 0 [usb]` stops it |
| `replies` / `expect usb HEX...` | print what FRIZZ sent over USB since the last look / fail unless it's exactly that |
| `set GROUP INDEX VALUE` | a value set outright, as `remote.py set` does it (the developers' `SET` over USB, its answer kept from `replies`): GROUP an effect by name (its knobs 0-7, page 1 then page 2), `fin` (the finalizer's 16 knobs, module by module; `comp` the same), `latch` (INDEX an effect), `volume` (`out`, `in`, `mix`, `headphones`) or `settings` (INDEX a line of `frizz_master.txt`); VALUE a knob's 0-1, or a latch's or setting's number (`on`, `off`). It waits for FRIZZ's main loop and its answer (a few ms); one FRIZZ refuses fails |
| `battery V [plugged] [full]` | the battery's voltage and the charger |
| `card put PATH FILE` / `card remove` / `card insert` / `card dump` | the SD card: put a file on it before power-on (`/FRIZZ/frizz_scenes.txt`), take it out, print it |
| `card file PATH`, then lines starting with `\|` | a file on the card before power-on, its text in the script: each line after its `\|`; after `booted`, put there at that time (a bug report's lend) |
| `usbaudio eject` | the computer ejects the card lent to it (CardLend.h) |
| `leds` | print the LEDs now |
| `expect led pth\|smt N RRGGBB` / `expect on` / `expect off` | fail (exit 1) unless so; a malformed `expect` fails too |

## Bug reports from the device

SHIFT + VOLUME press held 2 s on a CHOMPI's settings page writes `/FRIZZ/bug-N.txt` (`code/src/EventLog.h`, MANUAL.md's
*Bug reports*): a script of this kind. It puts the card's `frizz_scenes.txt` and
`frizz_master.txt` as they were at power-on on the twin's card (`card file`), plays a 220 Hz
tone into AUX and the mic (the audio in isn't recorded: change the `input` or `mic` line to
play a WAV instead), `jack out` if AUX had no cable at power-on, and from `booted` on, every
key, detent, mode-switch flip, cable in or out of AUX and MIDI clock change at the time the
hand made it: the device logs when its debouncing saw it, and gives the time back by the
latency the twin measures for the same code (a key 7-8 ms, a detent 3-4 ms, the mode switch
56-58 ms, the jack 20 ms). So

```bash
./run.sh -o out.wav -l leds.txt bug-1.txt
```

plays the session again and ends where the combo was pressed (with the twin writing its own
`bug-1.txt`). On the twin itself the replay is exact: `unit.sh ui` records a session, plays its
file on a fresh twin, and gets the same LEDs every millisecond and the same file back. From a
device, what the twin can't know differs: the audio (so the FX keys' and VOLUME's meters),
the MIDI clock's phase (logged as a tempo once it settles, back-dated to its lock), the battery,
and the loops a computer changed on the card lent to it. A lend and its take-back are comment
lines (`# card lent`, `# card taken back; read: settings scene 2`), as the loop transfer's jobs
are (`# loop transfer`); the replay lends and takes back by the gesture's keys. What the
take-back read again (#461), `frizz_master.txt` and `frizz_scenes.txt` as the computer left
them, follows the lend as `card file` blocks, so the replay's take-back reads the same; a
take-back by the computer (an eject, the cable) is `usbaudio eject` there. The first 8 lends
carry their files; a later one says so.
The browser plays it too (Replay), in its 16 ms steps. Once it shows the bug, make it a case
in `test/ui.cpp`.

## Files

- `twin.h` / `twin.cpp`: the API and the simulated board; `twin.cpp` includes `chompi_main.cpp`.
  The queues between a host and the firmware (MIDI in, USB MIDI and MIDI out, USB audio out)
  have fixed sizes, so hosts that drive the twin from a real-time thread can call `Run()`,
  `Midi()`, `UsbMidi()` and the `Take...(buf, max)` calls without allocating
  (`../test/rtalloc.cpp` checks it). A full queue drops the newest and counts it
  (`Dropped()`): `frizz-twin` then exits 3 with what was dropped (4 when it couldn't write its
  WAV or LED log whole, a full disk), and a case of `ui.cpp`,
  `sync.cpp`, `midi.cpp` or `midiout.cpp` fails (`cases.h`).
- `host/`: what replaces libDaisy's hardware layer (`daisy.h`, `daisy_seed.h`, `per/`, `sys/`, ...).
- `script.cpp` / `script.h`: the script player; `cli.cpp`: `frizz-twin`, around it.
- `clockgen.h`: a MIDI clock out of a virtual sequencer, with a real sender's jitter, drift, USB
  frames and ramps, for the scripts and `../test/sync.cpp`.
- `twin.h`'s `Probe()` reads what the firmware makes of the clock (MidiClock, the engine's
  TempoClock, the looper) for the checks of timing that the LEDs and audio can't show. The
  tempo clock is private in the engine; `twin.cpp` reaches it without a getter in the firmware,
  and reads each value only if the firmware has it (0 otherwise), so `Probe()` doesn't keep an
  older firmware from building (`compare.sh`, `ui-at.sh`).
- `build.sh`: copies the firmware (FRIZZ's `code/src` and the platform's layers, `../platform/`,
  into one folder: `../test/sources.sh`), the libDaisy files it uses and `host/` into
  `build/tree` and builds there, so every include resolves to either the real file or its stand-in. `build.sh
  wasm` builds the browser's `web/build/frizz-twin.{js,wasm}` the same way.
- `wasm.cpp`: `twin.h` as C functions for the browser.
- `web/`: the page (`index.html`, `app.js`), the worker running the twin (`worker.js`), the audio
  thread (`worklet.js`), the panel (`layout.json`), `serve.sh` / `serve.py` (the server that
  rebuilds on each page load).
- `tools/board_layout.py`: makes `web/layout.json` from the board file.
- `compare.sh` / `compare.py`, `ui-at.sh`, `ref.sh` (building another version's twin),
  `scenarios/`: two versions side by side.
