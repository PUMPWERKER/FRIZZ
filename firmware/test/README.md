# FRIZZ host checks

Everything that can be checked about FRIZZ without a CHOMPI: the audio engine compiled with the
host's `g++`, single parts of it, and the whole firmware on the virtual CHOMPI (`../twin/`).
What you need installed, and how long a first run takes, is in the developer guide
([`../README.md`](../README.md#3-test-on-the-host)): bash, `g++`, `python3` and git, no ARM
toolchain.

```bash
./all.sh                    # every check, one line each; exits 0 when all pass
./unit.sh tempo             # one unit check (NAME.cpp)
./check.sh                  # the engine at HEAD against the working tree
./check.sh 9da090e 647185b  # any two git refs ("work" = the working tree)
STRESS=1 ./run.sh work out.bin
```

| Check | What it looks at |
|---|---|
| `check.sh` | the engine harness (below): every output sample and FX meter of a fixed script, two versions compared; a refactor must be `bit-identical` |
| `doclinks` | not a `.cpp`: `../tools/doclinks.py --all`, the files the website is built from (MANUAL, QUICKSTART, INSTALL, `site/landing.md`, the key colours in `LedColors.h`/`FxSlots.h`) in every format frizz-web's `fetch-docs.mjs` parses, the pictures in `images/` as `tools/diagrams/diagrams.py` draws them (`--check`), and its `--selftest`, which breaks a copy of them case by case and checks each break is reported; needs `python3` |
| `pitch`, `splice`, `tape`, `delay`, `crusher`, `freezer`, `comp`, `clicks`, `level`, `sleep`, `inserts`, `page2` | parts of the engine on their own: the shifter's tuning and its splice search (its taps inside their delays, the grain turned at once too), wow and flutter and the tape stop, the delay's pitch-up and random events, its echoes on the beat and its damping turned on by a jump, the crusher's rate, bits and dive (through a sleep too), the freezer's capture and roll, the master compressor, moves that used to click, page 2's Mix, Band and Level (`FxOutput.h`), effects that are off costing no time (and `Smoothed::Settle` landing), the folder's bypass and level, the slicer's patterns, chance and stereo, page 2's Mix on the shifter, and each effect's own page-2 knob |
| `routing` | the engine's input routing (`passthroughEngine.h`, #177): each source's gates, so a source that isn't live adds nothing to the master at any sample, also while the source switches back and forth mid-fade (#168); every combination of source, effects and live on the master and the headphones' input feed; the looper recording the source live or not; the slews (`Glide`) settling exactly on 0 and 1, the mix at the loop leaving none of the input |
| `block` | the FX chain a block at a time (`FxChain::Process`, #136) against the same chain a sample at a time, bit for bit, output and meters: blocks of 1 to 40 samples (runs of 24 and the rest) and each block in two calls, through a script with every effect on, the resonator at its highest pitch and stereo with its tone slewing, page 2 off its defaults (the resonator's Band while its key fades out mid-block) and recalls' fast slews ending on a block's first, middle and last sample; page 2 leaving the chain's work (#142) on a block's first, middle and last sample each of three ways (an insert going quiet, an insert's page 2 back at its defaults, a send's Band back in the middle), and which page 2s are part of the work compared block by block. `block OUT` writes the script's output, meters and page-2 bits, to compare them with a build against another version's headers (`check.sh`'s harness never turns page 2) |
| `phaser` | the phaser (`FxPhaser.h`): its six stages' notches where they belong and unity between them, dry at depth 0, feedback bounded at the top and ringing down, the stereo LFO half a cycle behind, asleep bit for bit the input, the rate knob's free half in Hz and synced half in divisions, the LFOs running at their rate and on the clock's pulses |
| `chaos` | the chaos key (`FxChaos.h`): its dice on the grid, the pattern that repeats and the press that rolls a new one, the gates on the engine's pool of latched effects, the loop's scramble (another part of the loop, crossfaded, its position running on) |
| `scenes`, `store` | the scene and master files and the card: formats, a card not read at boot, backups (also of a file next to a cut save's `.tmp`) |
| `loops` | the loop loader (`LoopLoader.h`, #70): the WAV header's walk (what it takes and refuses; lengths that would wrap or run past the file), the slots' names, the scan of the bank folders a pass at a time, a load byte for byte (data at any offset, in fragments, past remounts, across the tick clock's wrap), its link map over passes (at most 3 of the FAT's sectors read in one, #212; a remount in its middle), what stops one (a read error, a run overdue, a card that doesn't come back, more fragments than the link map holds, a file shorter than its header or its clusters), the card shared with SceneStore's saves mid-load (no FatFs call while a run's DMA has the card), the CRC and the write test |
| `loopxfer` | the loop transfer (`LoopTransfer.h`, #278, `docs/LOOP_PROTOCOL.md`) with the loader's scan and header walk, on the card in memory: `loopxfer/vectors.json` replayed byte for byte (what `tools/loopxfer.py`'s model FRIZZ sends, the firmware sends, pass for pass, and the card ends as it does; and the file is what `loopxfer.py vectors` writes); a cubbi too long to copy, the fragment check (255 fragments taken, 300 refused), a power cut after every name change of a replacing write, a copy and a swap with the boot after it (whole files only, the swap undone or done), a card error at every name change of a replacing write, a copy, RENAME, SWAP and DELETE (every slot's SLOT as the card holds it, `host/fatfs.h`'s `fail_after`), no FatFs call while a load's DMA has the card, the free space counted a FAT sector a pass; only FRIZZ's slots and temp files ever change; needs `python3` |
| `sddisk` | FRIZZ's reads and writes for FatFs (`SdDisk.h`, #213) against what they replace, libDaisy's `sd_diskio.c` itself, both under FatFs itself (libDaisy's `ff.c`, built for the host from `host/sddisk/`) on a FAT32 card in memory: a scene's save and deletion, the master file, a bug report's writes and buffers that aren't 32-byte aligned make the same transfers, cache upkeep and card through either; a transfer that never ends, a card busy after a write, a read error, a refused transfer and a card that doesn't come back end within seconds, recovered (sd_diskio.c: 30 s); a save after one goes through; a write stopped mid-transfer waited for while the card programs, not set up again while it's busy (which left it at 400 kHz on the device); the device test's faults |
| `controls`, `keys`, `looper`, `tempo` | the play page's logic classes on their own: FX keys and knobs, SHIFT and the confirm, the looper (its glides landing exactly, its memory not cleared at boot, the soft limit on what it records), the tempo clock |
| `ui` | the whole firmware from power-on on the virtual CHOMPI: keys through the 4021s, LEDs, the headphones and the master out, the input (AUX's jack and the settings page's upper F#, G# and A#: the source, live and its effects (#243: in that order), each source's own and saved, every combination, A# dark while not live, CC 84, 116 and 117 with their feedback, the mic not live never on the master, not even while the jack switches (#168), its level, filter, loop and switch without a click, AUX dry past the effects and the compressor, the mix's automatism, an input gain per source, VOLUME on the settings page, the input's meter), the card (full too), bug reports (the jack in them too), MIDI (notes, the effect channels and the global one, what hosts send by themselves, program changes, Start/Stop, the SysEx and its USB answers); each case on a fresh device |
| `remote` | `../remote.py` itself against the virtual CHOMPI: the twin's USB MIDI on a pseudo-terminal, remote.py run as it is (`--device`) while the twin keeps the wall clock's pace: state, LEDs (PLAY's beat apart, #124), load, settings, scenes there and back, a script played with its `expect led` lines, at full brightness and at the card's 50 % (#94, knob 1 on the settings page, #176), one that uses the settings page refused without `--force` (#58), the switch down at a script's start whether the hand or SysEx holds it up, after `--fresh`'s restart too, and the hand's again after (#253), and one that turns the compressor's knobs (its key, or seeded from the device's state, or its CC) or saves a scene (#78), an effect's knobs played, one that breaks off or is interrupted leaving no key held and the switch with the hand, a script's `input sine` played as the test signal (SIGNAL), off again at its end and after Ctrl-C (#219), the crash record read and cleared (a fault's as the handler writes it, a watchdog's as the next start records it), RESET taken in the audio callback with its key only (#105), the loop transfer's `loops ls`, `put` (with `--cpu`; once with every 7th CHUNK dropped by the pseudo-terminal: go-back-N on the firmware), `mv`, `swap`, `rm`, `cp` and `rescan` (#278); and the device tools with the CHOMPI stubbed (`Python()`, a lock of their own, never the CHOMPI's): the lock taken only when they reach for it, the slot noted and told apart (FRIZZ-TEST, the bench), `flash.py` waiting for the FRIZZ it sent, `card.py`'s eject after a failed mount, a CHOMPI with garbled USB names (#88) found, and started again (twice at most, `chompi.py fix-name`), `expect led`'s brightness fitting every line, `play`'s `input sine` sent as SIGNAL and SIGNAL off at `input off`, `input wav`, the script's end, an error or Ctrl-C, a FRIZZ without SIGNAL skipping it (#219), `flash.py` refusing a tag whose `kFrizzVersion` isn't its own (#278), `play --fresh` starting the slot again (#123) and key 10's FRIZZ started again after a RAM-only play (#134); the setters FRIZZ saves kept in RAM (PERSIST 0) unless `--save`, a FRIZZ that restarted meanwhile failing the run, `flash.py` sending to keys 10 and 13 only a clean `v*` tag's build with the key named (nothing without a slot), and `midisuite.py`'s IDENT check per case and before putting things back, SIGTERM stopping the run, its exit codes (#171); a FRIZZ that doesn't get to the launcher, or is silent by its USB names, RESET before the power switch is asked for, another firmware never (#105); needs `python3` |
| `midisuite` | `../tools/midisuite.py` itself against the virtual CHOMPI, as `remote` does remote.py: USB and the TRS jack each on a pseudo-terminal, the jack wired both ways, only its out (A), only its in (B) or not at all, the twin at the wall clock's pace: every case passes on the firmware as it is (`--quick`), the wiring is found, what it can't show is a SKIP listed for the other wiring, MIDI out seen on TRS or over USB as the wiring has it, the card as it was after each run (PERSIST 0), Ctrl-C in a case putting things back and exiting 130 (#171). It checks the suite's logic, not the firmware's timing (`sync`, `midiout` and the device do): the pseudo-terminals' bytes reach the twin at the times they came, and a case that failed on `[timing]` checks alone isn't judged when the twin fell over 5 ms behind the wall clock (all.sh's load); a run that failed anyway with the twin that far behind runs once more, listed as `FLAKY` under the check, and fails if it fails again (`ptytwin.h`'s `Unjudged()` and `Twice()`, #183); each run prints how far the twin fell behind; `MIDISUITE="ARGS" ./unit.sh midisuite` runs it once with ARGS; needs `python3` |
| `reaper` | `../tools/reaper.py` itself against the virtual CHOMPI (`ptytwin.h`, as `midisuite`), with `reaper_stub.py` in REAPER's place (`FRIZZ_REAPER`): the stand-in does what `../tools/reaper.lua` does in REAPER (the command file, the log, FRIZZ's answers from its input) and sends REAPER's clock, Start / Continue / Stop / Song Position, a loop range's turns, CC lanes and its CC reset on stop as REAPER 7 does on the device; synchronized to its input (`daw-follows`), it plays on the ticks FRIZZ sends from Start, Stop, Song Position and Continue, as REAPER 7 was seen to. Wiring A: every case passes on the firmware as it is (`--quick`), the jitter in both audio modes with its table; without TRS the jitter is a SKIP; SIGTERM while REAPER runs a case stops the run, not its FAIL, things put back, exit 143 (#171). The cases with music (`daw-loop-audio`, `daw-loop-restart`, `daw-follows-audio`) play a generated song into the twin's AUX and record its line out through files the stand-in shares with `ptytwin.h` (its `Audio`: the song's time line, where it plays from when, the line out), so the tool's analysis runs on the firmware's own loop; they need numpy, from `~/.venvs/frizz` (measure.py's), and are a SKIP saying so without it. It checks the tool's logic, not REAPER nor the firmware's timing: a case that failed on `[timing]` checks alone with the twin over 5 ms behind isn't judged, and a run that failed anyway that far behind runs once more (`FLAKY`), as `midisuite`'s (#183); `REAPERCHECK="ARGS" ./unit.sh reaper` runs it once with ARGS; needs `python3` (and the venv for the music) |
| `measure` | `../tools/measure.py fx` with the CHOMPI and the audio interface stubbed (a stand-in answering GET and SET as `MidiControl.h` does): the effect latched alone over SysEx SET, by its key (KEY_7 the phaser, `FxSlots.h`) or its name, `none` for the dry line, while it records; its CCs on the effect's channel; the latches and its knobs put back afterwards, a recording that breaks off too; a latch that doesn't take ends it before anything plays (#187), a silent line in fails; needs `python3` (no numpy) |
| `docs` | `MANUAL.md`'s tables against the code, so the manual can't fall behind a change: each effect's chapter (and the compressor's) on its white key, its knob table with a text for each knob it uses and "—" for the others (`FxParams.h`), its LEDs' key colour and the knobs' first and last (`FxSlots.h`, named in the check's colour table); every stepped parameter's steps in its cell as the code's tables have them (in bars; the slicer's patterns bit for bit) with its default, and Punch-in FX's bullet naming each; the Coarse steps table against the grids (`kFxGrids`), its values computed from each knob's range, every other parameter in 10% steps; MIDI's notes, both CC tables and the effect channels against `midimap` (`MidiControl.h`), the CCs left free unused; the settings page's colours per key and per state (`SettingsPage.h`). Tables, lists and numbers, not prose. `FxSlots.h`, `PanelKeys.h`, `MidiControl.h` and `SettingsPage.h` need the board, so they're read as text, and one that doesn't parse fails; `DOCS_MANUAL=FILE ./unit.sh docs` checks another copy of the manual |
| `bench` | the CPU bench's firmware on the twin: it runs through and writes its file (the loads themselves need the device) |
| `twinlib` | the twin as a shared library, for tools that embed it (a host that loads the virtual CHOMPI into a process of its own): `../twin/build.sh` with `-fPIC -fvisibility=hidden -fvisibility-inlines-hidden` (`TWIN_DEFINES`, into `../twin/build/twinlib`), DaisySP with `-fPIC` (`lib.sh`'s `daisysp pic`), both linked with `twinlib/lib.cpp` by `g++ -shared -Wl,--no-undefined`, then loaded with `dlopen` and run: its one C function exported and the twin's own API not, the firmware booting and its main loop running after 2 s, its audio finite. It fails when the twin stops building position-independent or the library leaves a symbol undefined |
| `rtalloc` | the twin for hosts that drive it from a real-time thread (`../twin/twin.h`): `operator new` and `malloc` replaced in the check's process and counted while it plays 3 s, after a warm-up, a block at a time: MIDI in on the jack and USB (the clock, notes, SysEx asked and answered over USB), MIDI out on both ports, USB audio both ways, what came out taken with the `Take...(buf, max)` calls a few at a time; no allocation, nothing dropped. The calls that return a string or vector still give everything; full queues drop the newest (USB audio out a block whole) and count it in `Dropped()`, and the firmware runs on |
| `midi`, `sync` | MIDI in on the twin: ticks among and inside other messages, the jack and USB, which clock locks, a clock in pairs started late; (N)RPN turning no knob, a key tapped within a block, a Song Position after a program change, notes and CCs after a SysEx cut short (#166), the input's CCs 84, 116 and 117 in the order they came, each source keeping its own (#177), a SysEx in a bug report; and timing against a clock with a real sender's jitter: the FX's tempo, quantized loops' length and drift, the clock lost or switched mid-recording, the 2:45 limit, tap tempo, the loop following Start, Continue, Stop and Song Position |
| `usbmidi` | libDaisy's USB MIDI out (`code/libs/libDaisy/src/hid/usb_midi.cpp`, compiled as it is, the rest of libDaisy's USB stubbed in `usbmidi/`; since TinyUSB, #196, FRIZZ.bin no longer links it, TinyUSB's MIDI class packs the bytes, but the parser below is still FRIZZ's) against what a host reads from its packets (by their code index, as ALSA does): every message FRIZZ sends over USB arrives once and whole, the clock and transport, a Song Position (sent twice before #157), channel messages several to a transfer, SysEx of every length round a packet's three bytes; and in, a host's packets and the jack's bytes through libDaisy's MIDI parser (`midi_parser.cpp`, also as it is): a SysEx cut short by a note, a CC or another SysEx ends there (#166), a tick inside one is heard and it carries on (#18). The twin can't show the packets: its USB leaves them out |
| `usbtransport` | FRIZZ's USB MIDI transport on TinyUSB (`code/src/UsbMidiTransport.h`) behind libDaisy's `MidiHandler`, against TinyUSB's MIDI functions in memory (`usbtransport/`): `Listen()` starts the receive (the first PoC build on the device was deaf to USB MIDI without it, #196), the host's SysEx and notes reach the parser, answers reach the host whole, also a few bytes a pass, a full queue refuses a message whole, `Reset()` and a host gone drop the queue. Run it before the first flash of a build with a new USB stack |
| `midiout` | MIDI out on the twin (`MidiOut.h`), timed as the UART sends: off at first; an incoming clock passed on tick for tick, by the clock factor (also a host's ticks in pairs), never back to its input; nothing late from the boot screen; a free clock at the last tempo; a loop's Start, Stop, Song Position and Continue; the handover from a DAW's clock to a quantized loop, and the loop's Start to a port whose gear got none (#166); the transport kept over USB while MainLoop is held up (`HoldMain`, a slow card), ticks dropped instead (#166); the clock factor on the free clock and a loop's (#116), a loop of 3 beats at half steady across its starts |

A check prints `KNOWN` for a fault it has found in the firmware that isn't fixed yet (`Known()` in
`check.h`): that doesn't fail it, so `all.sh` stays green while the faults are listed. Once one
passes it prints `FIXED` and fails the run, and becomes a plain `Check()` with the fix.

`all.sh` runs the checks side by side, one per core (`JOBS=N` sets how many), once it has
built DaisySP and the twins they share; their lines come in the order above. On 12 cores it
takes about 50-60 s, `midisuite`, `reaper` and `remote` (the wall clock's pace, about 30-50 s each) and the
bench's 20 s on the twin the longest; the first run longer (~80 s), as it builds
DaisySP and the twin for the host (into `build/` here and `../twin/build/`, both ignored by
git). `remote`, `midisuite` and `reaper`, which play the twin at the wall clock's pace
against their tools' timeouts, and `bench` start first, at the normal priority;
the others run behind them (`nice`). A check that fails is named; one that exits non-zero
without naming a failing check, is killed, or leaves no exit code says so (#107), and each
check's whole output of the last run stays in `build/all-last/NAME.out`. Under a check
that passed, the faults it knows of (`KNOWN`) and the runs it had to repeat (`FLAKY`) are
listed. `CASES` is ignored there: `unit.sh` takes it.

The checks on the twin with many cases (`ui`, `midi`, `sync`, `midiout`) run them through
`cases.h`: each case on a freshly booted device, a forked process, as many at once as there are
cores (`CASE_JOBS=N` sets how many), their output in the order the cases were declared. A case
that takes the card another one kept (`KeepCard()`, `TakeCard()`) runs after it, on the same
card: `Chain()` in `ui.cpp` names those cases, and a `TakeCard()` with no card kept before it
fails. A case that has used `CASE_TIMEOUT` seconds (60) of the CPU, or run ten times that long
by the wall clock, is stopped and fails by its name, with what it printed: the CPU's time, so a
loaded machine (several `all.sh` side by side) doesn't stop a case that is only slower (#183). `CASES="a b" ./unit.sh ui` (or `midi`, `sync`, `midiout`) runs only those
cases, and the ones before them in their chains; a name that no case has fails the run.
`twintest.h` has the helpers these checks and `remote.cpp` share: `kReadyMs`, `Card()`, `Hex()`
and `Tap()`.

## Engine harness

`check.sh` compiles FRIZZ's audio engine (`code/src/passthroughEngine.h` and everything it
includes) with the host's `g++`, runs a fixed script of key presses and knob turns through
it, and writes every output sample and FX meter to a file. Two versions of the engine can
then be compared.

- **A change that shouldn't alter the sound** (a refactor, a rename): `check.sh` should print
  `bit-identical` and exit 0. Anything else means the sound changed.
- **A change that should** (a new effect, a new order, a retuned knob): `check.sh` prints the
  master out's loudness per segment for both versions, the peak and any NaNs, so a blow-up or
  an effect gone silent stands out.

What it covers: the whole engine as the audio callback runs it, with the delay's random events
seeded and MIDI clock absent (the tempo clock runs on its internal 120 BPM). What it doesn't:
the play page and the looper's recording (see `unit.sh controls` and `unit.sh looper` below), MIDI
clock, and anything about the hardware.

The script (`harness.cpp`) is a segment of 3 s per FX plus four: each FX on its own with a random knob turned
every 0.25 s, the inserts together, everything together, then the tails. `NOFX=1` runs it with
no FX switched on, to check that a segment actually exercises its effect; `STRESS=1` runs
everything at once with the resonator's loop at its most extreme for the whole run.

It drives the engine through `Init`, `SetFxOn`, `SetFxParam` and `GetFxLevel` with the
`chompi::FX_*` names, so it builds against FRIZZ from `9da090e` on, and sets the master
compressor's amount (its threshold since #38) to 0.3 (`SetCompParam`, or `SetFinalComp`
before `MasterComp.h`; the two compressors differ, so a run across that change isn't
bit-identical; nor is one across #38, which took the automatic makeup out, 1.5 dB at the
harness's 0.3 and 1.5:1). If that interface changes,
update `harness.cpp` along with it.

## Shifter pitch check

```bash
./unit.sh pitch
```

Runs a 220 Hz sine and a 220 Hz harmonic tone through the shifter (`FxShifter.h`, working
tree) at every interval from -12 to +12 semitones, and fails if any comes out more than 5
cents off or its level wobbles by more than 2 dB, and a 10 kHz sine a fifth up if it comes
out more than 0.6 dB under its level (the taps' Hermite reads keep it at -0.3 dB; linear ones,
before #305, lost 1.3 dB). The engine harness can't tell a shifter
that's out of tune from one that isn't; this can. Takes about 12 s. (A plain two-tap shifter
is up to 80 cents off and wobbles up to 10 dB; Kastle's single-tap one fails 48 of the 50.)

## Shifter splice check

```bash
./unit.sh splice
```

Holds the shifter's splice search (`Shifter::SpliceSearch`, `FxShifter.h`) against the search
as it was before #137 made it cheaper, a copy kept in `splice.cpp`: about 60000 searches on
noise, tones, their mixes, near-silence and clicks, at random places in the ring (a quarter
of them with reads that wrap it, the masked path), shifting up and down as far as the swoop
and the stereo offset go. Every search must pick the same lag, and so must the search spread
over two blocks as the shifter runs it, with a block of new samples written in between. Then
the shifter itself at the ends of its shift, grain, swoop and stereo, from power-on,
punched in again after the shift was turned while it was off, and turned from the centre
while it plays: a tap that sounds must never sit at the end of its delay (1 or the longest),
and waking in the direction it was left in, the taps go on where they were (#140). And back
at stereo 0 while it plays (#226): the right channel fades from its own taps to the left's
within a life, its steps sample to sample no larger than the left's (taking them at once
would jump, by 3 times as much and more), both channels splicing through the fade, and
after it L and R are the same to the bit; at every grain, with the swoop rising, and turned
apart again halfway (back to its own taps first). Built with `-mfma` instead, as the CHOMPI fuses its
multiply-adds, the searches come out the same too (checked by hand, not by `all.sh`).

## Tape FX check

```bash
./unit.sh tape
```

Runs a sine through wow & flutter (`FxWarble.h`) and the tape stop (`FxTapeStop.h`). Wow &
flutter on its defaults and the tape stop while off must pass the input bit for bit. The
flutter's pitch wobble, measured cycle by cycle, must stay within its bound (the depth
times the wobbles' rates) and reach at least 30% of it. TAPE's wow at the top must stay
finite and move the pitch. A stop must be silent once its time is up, on a linear curve and
a brake, also 2 bars at 50 BPM. Every spin-up, or none, must end on the input bit for bit,
also after releasing mid-stop and pressing mid-spin-up, with no step between samples larger
than a crossfade's.

## Scene file check

```bash
./unit.sh scenes
```

Checks the FX scene file (`FxScenes.h`): scenes written and read back come out within 1e-6
(a coarse grid point stays on its grid) and exactly on a second round trip. It also checks
that a hand-written file is read by effect name, with unknown effects and slots skipped and
missing ones on their defaults, and that a foreign, empty or truncated file doesn't break
anything. Last, it runs a filter jump through the engine with a recall's fast slew and with
a knob's. The recall must land sooner and within 30 ms, without a larger step between samples,
and hand back to the knobs' slew afterwards. It doesn't touch the card (`SceneStore.h`) or the
play page.

## Card check

```bash
./unit.sh store
```

Runs `SceneStore.h` against an SD card in memory (`host/fatfs.h`): the boot read, a `.tmp`
left by a cut save, the move from the card's root to `/FRIZZ`, an unreadable or oversized
scene file kept as `.bak`, and one deleted before the save. Then a card that couldn't be
read at boot: no save of that session writes it, scenes or master settings, even once it
answers, and after the next boot it's written again. It doesn't cover FatFs itself, or a
card pulled or swapped while FRIZZ runs, which isn't looked after.

## Clicks check

```bash
./unit.sh clicks
```

Runs a 220 Hz sine through moves that used to jump the sound and fails if the output steps
further between two samples than a smooth sweep does: the flanger's stereo knob turned back
to 0, as a recall or morph does, and the freezer pressed again while its release still fades
the loop out. It also guards the filter: its LFO division changed at full depth and high
resonance jumps the cutoff, but the output doesn't step further than the sweep does.

## Delay check

```bash
./unit.sh delay
```

Checks where the delay's voices (`granularDelay.h`) start a pitch-up event: half a bar back at
the default 1/4, right after Init as after a division change. Then its random events on the
8th-note edges: none with the random knob in the middle or the key off (a tail's edges); towards
0 retriggers, reverses and pitch-ups and -downs, each about as often, on half the edges at 0 and
a quarter at 0.25, in the centre; towards 1 only octave-up shimmers, on half the edges at 1,
panned at random to both sides; and at 2 bars and 50 BPM, where a reverse wouldn't fit the
buffer, a retrigger instead.

## Crusher check

```bash
./unit.sh crusher
```

The crusher (`FxCrusher.h`) on a 1 kHz sine: the reducer holds a sample 15 samples with the
rate knob in the middle and 100 at the top (3.2 kHz, 480 Hz), and at 0 lets the sine through
nearly as it was; the bits knob at the top leaves a handful of steps, at 0 the sine's every
value; a press dives the rate 10x lower 0.1 s on, and it's back to the knob's by 0.3 s. Held
values are compared within 1e-4 and a step counts once, as the reducer's BLEP spreads it over
two samples.

## Phaser check

```bash
./unit.sh phaser
```

The phaser (`FxPhaser.h`) with its LFO held still on the centre: at the centre knob's 0, .5 and
1 (100 Hz, 800 Hz, 6.4 kHz, within the Pade tangent's .5%) the stages' 90 degree point, and the
three notches below -30 dB where a stage turns 30, 90 and 150 degrees, unity between them. At
depth 0 the output is the input exactly, feedback and stereo up. Feedback at the top on noise,
sweeping fast: bounded, and down to nothing 0.5 s after the noise stops; it resonates between
the notches. Stereo at 1 puts the right channel's sweep at the bottom while the left's is at
the top. Off and faded out the output is the input bit for bit, and it comes on again from
silence. The rate knob: free .02 to 50 Hz below the centre, 1/16 to 4 bars from it up, a free
1 Hz LFO back where it was a second on, a synced one on the clock's pulses and gliding between
them. Turning the knob (fine, stepped, SHIFT across both halves) is `controls`' job.

## Freezer check

```bash
./unit.sh freezer
```

The freezer (`FxFreezer.h`) at 120 BPM on a ramp whose every sample is its own value: pressed,
the live signal passes bit for bit to the next 16th and one loop past it, then it repeats what
came in from the 16th, sample for sample past each seam's crossfade, a 16th each. The roll at
its shortest stage halves the loop after 1 repeat, after 2 more, and stops at 1/64 bar; at its
longest it holds 8 repeats first. Released, the live signal is back bit for bit once it has
faded out. (The gate fades in to 1 - 7e-6, not 1, #45, so a repeat carries the live input 100 dB
down: the check feeds silence once a capture is recorded.)

## Master compressor check

```bash
./unit.sh comp
```

Runs `MasterComp.h` on its own: threshold 0 (makeup 0) and mix 0 are exact bypasses,
threshold 0 with makeup is that gain alone, the static curve at each end of the ratio and
the threshold with no makeup of its own (within 0.2 dB, the soft knee too), the makeup knob
on everything, the attack and the release slowing from fast to slow, no gain ripple at 20:1 and the fastest speed on a
50 Hz or 100 Hz sine (the detector's hold), and the linked stereo. Then the whole engine with
everything up and a full-scale square starting from silence, so the outputs must stay within
1.0 through the safety limiter. Last, the settings file (`MasterSettings.h`): it round-trips,
grid points come back exactly, and foreign, unknown or partial files are read sensibly; an
old `compressor` line (four knobs) loads with its speed as attack and release and makeup at
0 dB, a `compressor2` line wins over it, and only the new line is written; the
mono line round-trips, a file without it reads as stereo, and the line of the removed
randomizer is skipped and not written again. It doesn't touch the card or the play page.

## Sleep check

```bash
./unit.sh sleep
```

Effects switched off stop costing time without changing what's heard (`FxGate::Asleep` and
`TailWatch` in `FxCommon.h`): the shifter, flanger and warble, off and faded out, pass their
input bit for bit and shape it again when switched back on; the delay and the reverb, off,
keep ringing out, sleep only once their tail has stayed silent (the delay's whole buffer, the
reverb's 2 s), add nothing while asleep, and come back on their key.

## Level check

```bash
./unit.sh level
```

Page 2's shared knobs (`FxOutput.h`) around real effects: at their defaults the effect runs
bit for bit as without them, and an effect that's off passes its input bit for bit whatever
they're set to; Level is 0 dB at 3/4, +12 dB at the top, 6 dB a coarse step and mutes at 0;
Band in the middle gives the effect everything, turned right only the highs (100 Hz passes
dry), turned left only the lows, and with Mix at 0 the two parts add back up to the signal;
a send's Band turned right is a low cut on what goes in. And that nothing turns the crusher
down by itself: XOR at full on a sine at -40 dBFS comes out about 22 dB louder.

## Play page, looper and tempo checks

```bash
./unit.sh controls
./unit.sh keys
./unit.sh looper
./unit.sh tempo
```

`unit.sh controls` runs the play page's logic (`FxControls.h`, `SceneControls.h`) against a fake
engine that records what it's sent: holding and latching in either order, fine, stepped and
coarse knob turns, SHIFT + press, the knobs' page 2 (a plain press, back to page 1 on another
effect or the compressor, page 2 in scenes and morphs), and the scene save / copy / delete / recall flow, including
that a recall sends only what changes, and what a scene morph does with each effect, its
taps and stopping it, and a SHIFT hold's gestures (#175: let go a morph, the transport pressed
a drop, turned the crossfader after its 2 detents and 150 ms, a second scene key a build-up;
whatever comes first holds; in a later hold a blinking key stops it, another starts a new one
from there; MIDI's CC 3 and CC 83 alike); and the compressor's key and knobs, which a recall leaves alone. `unit.sh keys` runs the CHOMPI, PLAY and LOOP keys (`PlayKeys.h`) against a
fake host: the confirm tap, SHIFT combos (SHIFT + PLAY is PLAY, #175), the looper's combos and the erase hold, and that a
release without its press does nothing. `unit.sh looper` runs `Looper.h` without MIDI clock: a free
recording plays back frame for frame at its length, play / pause, erase, the speed ladder, the
semitones running on through a standstill into reverse (#174: the speed onto 0x exactly, the
level with it, no click), the play fade back on 1 exactly after a pause (#168), and the refused
quantized record. `unit.sh tempo` checks the FX's tempo: tap tempo (`TapTempo.h`), how
a loop's beats are fitted or guessed, the tempo clock locked to a loop (`TempoClock.h`: beat 1
on the loop's start, counting down in reverse, standing still when paused, the tempo times the
speed, a tap refitting the beats, the loop overriding MIDI clock), the clock factor on each
source (#116: taps, a loop, a clock not doubled twice, the limit after it, the beat kept in
place when it changes, a grid over two passes for an odd number of beats at half) and the beats
of a loop recorded quantized to a faked clock (in the sender's beats too, for Song Position), and the looper's tick period (`Looper.h`'s line through the
ticks): a bar from jittered USB-like ticks within 12 samples of its length, the end following a
clock that slows after the press, and an end already passed when refitted closing at once,
within the post-roll; then the bar lines (free, on 2-, 4- and 6-beat loops, in
reverse) and a scene morph (`FxMorph.h`) on them: landing on the bar line, one per tap, the
glide, held while SHIFT is down however long, fades in and out, stopping it halfway, and a drop
(#153): hard in the bar line's block, its keys going off 25 ms before it, a glide under way
brought to its end there; and the
delay (`granularDelay.h`): reverse events only where they fit, and a tempo jump crossfading
to the new delay time while a 1 BPM step slides, and on a loop whose tempo isn't whole, a 1/4
exactly the loop's beat. These run the classes on their own; `unit.sh ui` below runs them
behind the play page.

## Page 2's own knobs check

```bash
./unit.sh page2
```

Each effect's own knob on page 1's knob 4 and the stereo on page 2's knob 2 (#117), on the
effects alone: the
freezer's gate leaves the start of each repeat and silences the rest, the shifter's grain
changes the sound, the folder's, crusher's and filter's stereo are off with both sides alike
and on make them differ, the flanger's negative polarity sounds different, the resonator's
env mod rings more on a loud input and its level turns the return down (off at 0), the
slicer's shuffle puts the even steps 2/3 of a 16th late and the rest on time, wow &
flutter's age dips the level, the tape stop's depth slows it without stopping and its
darken takes the highs off, the delay's freeze holds its echoes at a steady level, its
damping darkens or thins the repeats and its ducking turns them down under the input, the
reverb's freeze holds the room, its pre-delay at the top starts it 250 ms later and its
ducking turns it down. Then the scene file: v0.11's symmetry, XOR and LFO division stay on
page 1's knob 4 with stereo off, the tape stop's depth is a full stop, a send's page 2 from
the first test builds goes back to its defaults (not frozen), and layout 4 round-trips
(`scenes` checks layout 1-3 files moving stereo to page 2's knob 2).

## Folder and slicer check

```bash
./unit.sh inserts
```

The two effects with no check of their own before: the folder off passes its input bit for
bit; on, at 1x a quiet input comes out within 1 dB of its level, and driven far louder:
nothing matches it to the input. The slicer on a steady input, at 120 BPM: every
pattern hits on its own steps of the clock's 16ths, on both channels; chance at full flips
steps at random, both channels alike; stereo plays different patterns left and right.

## Whole-device check

```bash
./unit.sh ui
```

Runs FRIZZ's whole firmware from power-on on the virtual CHOMPI (`../twin/`: the play page,
the LEDs as the WS2812s get them, the keys through the 4021 chain and its debouncing), each
case on a freshly booted device:

- the outputs stay muted through the boot animation and then pass the input; the FX keys
  glow dimly;
- an FX key held lights and sounds and its knob 1 shapes it, and it is off again once released;
- hold + SHIFT latches it and a tap ends it;
- SHIFT + a key puts it on the knobs without sounding;
- LOOP records (red), plays back, PLAY pauses, and LOOP erases;
- PLAY + LOOP is refused without MIDI clock (red blinks) and records with one;
- VOLUME turns the master down;
- CHOMPI + PLAY + LOOP held at power-on is shipping mode;
- below 3 V, the panel flashes yellow and the device switches off after 15 s, but not on the
  charger.

And PR #7's hardware checklist, each of which fails on the firmware before it (`b9031c3`):

- an FX key held, CHOMPI tapped twice while SAVE waits: the effect latches and nothing is saved;
- an FX key held, CHOMPI held, the transport or a dark knob turned: still latched;
- VOLUME page 3: left, left, right, left, left stays stereo, three lefts make it mono, which is
  saved; VOLUME white for mono, light blue for stereo;
- a select flashes a dim key white and a bright one (the compressor working hard) dark;
- LOOP blinks quickly (100 ms) when a quantized record is refused and slowly (250 ms) while
  one closes;
- the transport LED stays lit at full speed both ways;
- no click from a recall that takes the flanger's stereo back to 0 (after the LFOs have
  drifted, in either channel), nor from the freezer pressed again in its release fade (8 ms
  after letting go: a key takes 7 ms to count as let go);
- the card over three power cycles: one that couldn't be read at boot flashes a save red and
  isn't written by that session, even once it answers, scenes or compressor; after a reboot
  the card's scene is there and a save adds to it.

And the bug report (`code/src/EventLog.h`): after a session (a latch, knob 1 with and without
SHIFT, a loop recorded and sped up, a scene recalled and another saved), SHIFT + VOLUME held
on the settings page writes `/FRIZZ/bug-1.txt` once 2 s have passed, the transport LEDs
blinking white then and not before, holding the card's
scenes as at power-on (not the one saved since) and the session's keys and knobs, and the
saves after it still go to `/FRIZZ` (writing it remounts the card), and a file of several
sectors comes out whole: the card here shifts whole sectors written from an unaligned
address, as the device's SD DMA does (found in the first report from a device). Played on a
fresh twin (`script.h`), the LEDs are the session's every millisecond up to the combo, and the
replay writes the same events again.

A bug report on a full card (the twin's card can run full, `SetCardSpace`, and counts the
files open, `CardOpenFiles`): the transport blinks red, the file isn't left open, and with
room again the next one is written. On a slow card (`SetCardWriteMs`: each write holds up
`main()` that long) a save that comes due while a report is written, after one that failed,
waits for it rather than mounting the card again under its open file (`remount-bug-report`,
#169; the card here, as ff.c, makes the files open before a mount invalid).

From the pre-1.0 audit (#169): a hand's tap tempo goes by when the audio callback saw LOOP,
not by MainLoop's 2 ms grid (`tap-at-key`); the compressor key isn't red from limiting
while the settings page was up (`limiter-led-settings`); an empty scene put over SysEx into
the active slot leaves no scene active (`scene-put-empty`); the developers' SET of a setting
keeps CC 86's, 116's and 117's values (`dev-set-keeps-midi`).

The bug report's hold (`bug-hold`): SHIFT + transport press on the play page (the combo until
v0.11) writes nothing, nor SHIFT + VOLUME let go after 1.5 s; VOLUME then SHIFT held 5 s
writes exactly one. With the card kept out of it (PERSIST 0, `bug-ram-only`, #171) the hold
writes nothing either.

And a restart over MIDI (`MidiClock.h`, for `flash.py`): FRIZZ's own SysEx resets the chip,
the launcher's PING or a longer message doesn't, and a setting changed just before it is on
the card first.

The headphones (outputs 0/1, which the other cases don't look at): the master out at 2/3 of
its level at first (`kHpGain` .2 to the line out's .3), silent with it under a latched tape
stop, the input alone with the cue up (CC 59, VOLUME's page 4) while the master stays silent,
and the master again with the cue down.

The battery: VOLUME's LED on the settings page white on the cable, green once it's pulled, yellow
below 3.3 V at the next 30 s read, white again with the cable (known: red never shows below 3 V,
as the yellow countdown comes first, #43), and the charger plugged in during the countdown
stopping it. The scene keys where #67 put them (the lower F# to the upper D#; SAVE, COPY,
DELETE green, blue, red on the upper F#, G#, A#; the lower C# and D# dark and doing nothing,
no SHIFT combo either; note 56 scene 1): a saved slot dim, the active one bright, pulsing from a fifth up
to full once a second once edited; a morph's key blinking on the beat, SHIFT + another scene key
during it and a 9th bar each blinking red 3 times. The compressor's key without a card: 3 red
blinks for the failed save and for each of its 3 retries, 2 s apart. And the controllers the
other MIDI cases don't send: 18 and 19 (the transport and VOLUME turned), 53-55, 57, 58, 60, 61
above 8, 63 and 120 (keys held by notes and by SysEx let go), and the mod wheel, pitch bend and
aftertouch changing nothing.

The millisecond counter's wrap (after 49.7 days on; `SetClockStartMs()` starts the firmware's
clock 9 s before it): a refused scene key's blinks just before it end and don't come back, a
loop recorded across it is as long as it was recorded, the compressor's knob turned before it
is saved after it, a scene saves and CHOMPI's pending blink keeps its pace after it. And every
scenario of `../twin/scenarios/` played from power-on, with its `expect` lines, and again on a
card as a player's may be, the compressor's page 1 and most settings not as the factory has them: the
device's scenarios start from that (#214). A script's `set` too: its answer kept from
`expect usb`, a value refused failing its line, and comp.txt without its sets failing there.

`CASES="scene-leds scene-morph" ./unit.sh ui` runs only those cases (and the cases before them
in their chains, `Chain()`), while working on them.

The flicker #7 fixed in the transport LED (a value just over 1 wrapping to dark) doesn't show
on the twin before the fix either, so that check guards only what it can see.

`../twin/ui-at.sh REF [CHECK]` runs these checks (or another twin check: `midi`, `sync`) on
another version's firmware: a new check should
fail on the version before its fix. It can't see time on the chip: the CPU load, so not
crackles either.

## MIDI in check

```bash
./unit.sh midi
```

Plays MIDI into the virtual CHOMPI's jack and over USB (`../twin/`: `MidiClock.h` and libDaisy's
parser and handlers as on the device) and counts what `MidiClock` makes of it (`twin::Probe()`):
every tick of a 120 BPM clock on either input; ticks between notes (with and without running
status), CCs, pressure, program changes, active sensing and SysEx; ticks in the middle of a note
or a CC, as MIDI allows; Start, Stop, Continue and Song Position changing nothing; stray data
bytes, undefined status bytes, cut messages and a SysEx longer than the parser's buffer. Then two
clocks: the first to tick is kept and the other's ticks ignored, the other takes over 0.5 s
after it stopped, with a new lock; two started in the same block; and no clock 0.5 s after the
last tick, the FX keeping its tempo, a new clock locking again.

A tick inside a SysEx counts too, and the SysEx around it still arrives whole: the restart SysEx
with a tick inside restarts (#18, fixed in the vendored parser, `THIRD_PARTY.md`).

## Timing check

```bash
./unit.sh sync
```

FRIZZ against a MIDI clock (`../twin/clockgen.h`) from five senders: exact; a hardware
sequencer on the jack (0.2 ms jitter, its crystal 50 ppm slow); a DAW over USB (1 ms frames, 0.3
ms jitter); one that sends its ticks in pairs, both in one USB frame; and one catching up, every
other tick late, a frame before the next. Each case prints what it measured; the limits are at the top of `sync.cpp`.

- The FX's tempo from the clock, no loop, for 30 s at 60, 90, 120, 174 and 300 BPM and at 120.4:
  how often it changes (it should hold still), and a pulse every 2 ticks. A step from 120 to
  121 BPM: the tempo there within 5 s, and still after. A ramp from 100 to 140 BPM: the tempo
  there within 0.5 s of its end.
- Quantized loops (PLAY + LOOP from the panel) of 1 and 4 bars at 90, 120 and 174 BPM: closed on
  their bars, their length against the bars the clock played, the drift of their loop point
  against the clock over 30 s (nothing pulls a loop back to the clock once it plays), 12 FX
  pulses a beat of the loop.
- No click where a quantized 1-bar loop wraps, from every sender at 120 and 174 BPM (a sine
  through it: the master out's largest step against the sine's own).
- A song change behind a closed loop: the clock's tempo moved, stopped, restarted over USB and
  joined by a second on the jack, while the loop's length, its beats, the FX's tempo and every
  pass stay as they were.
- The strict bar rule (a stop 12 ms after a bar line records another bar, 12 ms before doesn't),
  the clock lost and the jack switched to USB mid-recording (it closes, unquantized), a tempo
  change mid-recording (reported: no rule says what it should become), the loop ignoring the
  clock's tempo once closed and an erase following it again, a quantized recording cut to 82
  bars at 2:45, and tap tempo from SHIFT + LOOP: without a clock, refused with one, refitting a
  free loop's beats.

The limits that depend on the sender's luck (a tempo's changes, a loop's length and drift) are
checked over all cases together, after them, so one case just inside a limit by chance doesn't
pass for a fix. Known: the FX's tempo keeps jumping by 1-2 BPM at 174 and 300 BPM, and at a
tempo between two whole BPM (MidiClock's smoothing against the blocks' and USB's jitter, and
the rounding without hysteresis); and a quantized loop, though within a few samples of its bars
(the looper fits a line through every tick of the recording, `Looper.h`), still drifts up to 5.5
ms a minute against the clock when it's short and fast (1 bar at 174 BPM from a DAW): a bar of
ticks can't measure the tempo closer, and nothing follows the clock once the loop plays. The
drift is worked out from the length's error; the loop point's moves, measured only to a block,
have to agree with it. From the catching-up sender, a loop is as close to its bars as from the
others: the looper drops a tick that came late from its fit (#17).

## CPU bench check

```bash
./unit.sh bench
```

Runs the CPU bench's firmware (`FRIZZ-bench.bin`, `code/src/Bench.h`, built for the twin with
`FRIZZ_BENCH`) on the virtual CHOMPI from power-on to its end: every segment ends up in
`/FRIZZ/cpu.txt` in order with a max and a mean, nothing else still works in any segment (no
tail: the bench waits for them; the delay asleep before the first), the 4 s loop plays in every
loop segment, the worst is named, the split into the callback's parts has every segment, every segment's key is graded, the panel ends green, and the
bench's tune reaches the output throughout. Without a card, the panel blinks red at the end. The
loads are 0 there (no time passes on the twin while the callback runs): the numbers come from
the device only. A check asks for a twin with defines of its own with a
`// twin defines: ...` line, which builds it into `../twin/build/NAME`.

`timing.h` is what `midi.cpp`, `sync.cpp` and `midiout.cpp` share: two clocks into the twin
(the jack's and USB's), `twin::Probe()` block by block, each case on a fresh device
(`cases.h`), and `Record()`, a measurement handed back to `main()` for a check over all cases.

`host/` holds the stand-ins for the parts of libDaisy the engine touches: `daisy.h` (two sample
conversions), `MidiClock.h` (no clock, unless a test sets its fields, as `tempo.cpp`
does) and `fatfs.h` (an SD card in memory, for `store.cpp`, `loops.cpp`, `loopxfer.cpp` and the
twin; a mount makes the files open before it invalid, as in ff.c, and loses what was written to
one since its last `f_sync`; folders, fast seek's link map, a seek's cluster and the FAT sectors
following the chain reads (`fat_reads`), a file's clusters growing as it's written, and the raw
sector reads `SdCard.h` makes, counting any FatFs call made during one as `raw_clash`; names
found in any case, FatFs's `FRESULT` numbers, the free clusters and the FAT's sectors to count
them, and a power cut after the n-th name change, `cut_after`, for the loop transfer).
`loopxfer/` holds the loop transfer's vectors, `tools/loopxfer.py vectors`'s.
`host/sddisk/` is `sddisk.cpp`'s instead: the HAL's few calls `sd_diskio.c` makes and a `fatfs.h`
that is FatFs itself, whose C files a check names on a line `// unit sources:` (`lib.sh`).
DaisySP is compiled for the host once into `build/`, which is ignored.

## How the scripts are built

Every script sources `lib.sh`, which builds DaisySP for the host into `build/` (again when its
sources or `g++` change) and has `unit_test NAME`: it copies the working tree's headers next
to the host `MidiClock.h`, builds `NAME.cpp` with warnings on and runs it. FRIZZ's code is
built with `-funsigned-char` here and in the twin, as char is unsigned on the CHOMPI and signed
on x86. A test's own
warnings are shown; if it doesn't build, so is every message, including the headers'. The
`.cpp` checks share `check.h` (`Check()`, `Finish()`). A new check is just a `NAME.cpp`:
`unit.sh NAME` runs it and `all.sh` picks it up. A check that includes `twin.h` is linked
against the virtual CHOMPI instead (`../twin/build.sh` builds it when the source changed).

The harness also writes `<out>.names`, its FX in order, so `compare.py` matches segments by
name and a new effect doesn't break `check.sh`. Runs of older harnesses without it are told
apart by their size.
