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
| `pitch`, `tape`, `delay`, `crusher`, `freezer`, `comp`, `clicks`, `level`, `sleep`, `inserts`, `page2` | parts of the engine on their own: the shifter's tuning, wow and flutter and the tape stop, the delay's pitch-up and random events, the crusher's rate, bits and dive, the freezer's capture and roll, the master compressor, moves that used to click, page 2's Mix, Band and Level (`FxOutput.h`), effects that are off costing no time, the folder's bypass and level, the slicer's patterns, chance and stereo, page 2's Mix on the shifter, and each effect's own page-2 knob |
| `chaos` | the chaos key (`FxChaos.h`): its dice on the grid, the pattern that repeats and the press that rolls a new one, the gates on the engine's pool of latched effects, the loop's scramble (another part of the loop, crossfaded, its position running on) |
| `scenes`, `store` | the scene and master files and the card: formats, a card not read at boot, backups |
| `controls`, `keys`, `looper`, `tempo` | the play page's logic classes on their own: FX keys and knobs, SHIFT and the confirm, the looper, the tempo clock |
| `ui` | the whole firmware from power-on on the virtual CHOMPI: keys through the 4021s, LEDs, the headphones and the master out, the card (full too), bug reports, MIDI (notes, CCs, NRPN, program changes, Start/Stop, the SysEx and its USB answers); each case on a fresh device |
| `remote` | `../remote.py` itself against the virtual CHOMPI: the twin's USB MIDI on a pseudo-terminal, remote.py run as it is (`--device`) while the twin keeps the wall clock's pace: state, LEDs, load, settings, scenes there and back, a script played with its `expect led` lines, one that uses the settings page refused without `--force` (#58); needs `python3` |
| `bench` | the CPU bench's firmware on the twin: it runs through and writes its file (the loads themselves need the device) |
| `midi`, `sync` | MIDI in on the twin: ticks among and inside other messages, the jack and USB, which clock locks; and timing against a clock with a real sender's jitter: the FX's tempo, quantized loops' length and drift, the clock lost or switched mid-recording, the 2:45 limit, tap tempo |
| `midiout` | MIDI out on the twin (`MidiOut.h`), timed as the UART sends: off at first; an incoming clock passed on tick for tick, by the clock factor, never back to its input; a free clock at the last tempo; a loop's Start, Stop, Song Position and Continue; the handover from a DAW's clock to a quantized loop |

A check prints `KNOWN` for a fault it has found in the firmware that isn't fixed yet (`Known()` in
`check.h`): that doesn't fail it, so `all.sh` stays green while the faults are listed. Once one
passes it prints `FIXED` and fails the run, and becomes a plain `Check()` with the fix.

`all.sh` takes about 2 minutes; the first run longer, as it builds DaisySP and the twin for the
host (into `build/` here and `../twin/build/`, both ignored by git).

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
cents off or its level wobbles by more than 2 dB. The engine harness can't tell a shifter
that's out of tune from one that isn't; this can. Takes about 12 s. (A plain two-tap shifter
is up to 80 cents off and wobbles up to 10 dB; Kastle's single-tap one fails 48 of the 50.)

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
taps and stopping it; and the compressor's key and knobs, which a recall leaves alone. `unit.sh keys` runs the CHOMPI, PLAY and LOOP keys (`PlayKeys.h`) against a
fake host: the confirm tap, SHIFT combos, the looper's combos and the erase hold, and that a
release without its press does nothing. `unit.sh looper` runs `Looper.h` without MIDI clock: a free
recording plays back frame for frame at its length, play / pause, erase, the speed ladder, and
the refused quantized record. `unit.sh tempo` checks the FX's tempo: tap tempo (`TapTempo.h`), how
a loop's beats are fitted or guessed, the tempo clock locked to a loop (`TempoClock.h`: beat 1
on the loop's start, counting down in reverse, standing still when paused, the tempo times the
speed, a tap refitting the beats, the loop overriding MIDI clock) and the beats of a loop
recorded quantized to a faked clock, and the looper's tick period (`Looper.h`'s line through the
ticks): a bar from jittered USB-like ticks within 12 samples of its length, the end following a
clock that slows after the press, and an end already passed when refitted closing at once,
within the post-roll; then the bar lines (free, on 2-, 4- and 6-beat loops, in
reverse) and a scene morph (`FxMorph.h`) on them: landing on the bar line, one per tap, the
glide, held while SHIFT is down however long, fades in and out, stopping it halfway; and the
delay (`granularDelay.h`): reverse events only where they fit, and a tempo jump crossfading
to the new delay time while a 1 BPM step slides, and on a loop whose tempo isn't whole, a 1/4
exactly the loop's beat. These run the classes on their own; `unit.sh ui` below runs them
behind the play page.

## Page 2's own knobs check

```bash
./unit.sh page2
```

Each effect's own knob on page 2 and the stereo on page 1's knob 4, on the effects alone: the
freezer's gate leaves the start of each repeat and silences the rest, the shifter's grain
changes the sound, the folder's, crusher's and filter's stereo are off with both sides alike
and on make them differ, the flanger's negative polarity sounds different, the resonator's
env mod rings more on a loud input and its level turns the return down (off at 0), the
slicer's shuffle puts the even steps 2/3 of a 16th late and the rest on time, wow &
flutter's age dips the level, the tape stop's depth slows it without stopping and its
darken takes the highs off, the delay's freeze holds its echoes at a steady level, its
damping darkens or thins the repeats and its ducking turns them down under the input, the
reverb's freeze holds the room, its pre-delay at the top starts it 250 ms later and its
ducking turns it down. Then the scene file: v0.11's symmetry, XOR and LFO division move to
page 2's knob 2 with stereo off, the tape stop's depth is a full stop, a send's page 2 from
the first test builds goes back to its defaults (not frozen), and layout 3 round-trips.

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
room again the next one is written.

The bug report's hold (`bug-hold`): SHIFT + transport press on the play page (the combo until
v0.11) writes nothing, nor SHIFT + VOLUME let go after 1.5 s; VOLUME then SHIFT held 5 s
writes exactly one.

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
stopping it. The scene keys: a saved slot dim, the active one bright, pulsing from a fifth up
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
scenario of `../twin/scenarios/` played from power-on, with its `expect` lines.

`CASES="scene-leds scene-morph" ./unit.sh ui` runs only those cases, while working on them.

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

Known: a tick inside a SysEx is lost (the parser keeps it as SysEx data), so a DAW's SysEx
during the clock costs ticks and misreads the tempo; and the restart SysEx with a tick inside
doesn't restart.

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

`timing.h` is what `midi.cpp` and `sync.cpp` share: two clocks into the twin (the jack's and
USB's), `twin::Probe()` block by block, each case on a fresh device, and `Record()`, a
measurement handed back to `main()` for a check over all cases.

`host/` holds the stand-ins for the parts of libDaisy the engine touches: `daisy.h` (two sample
conversions), `MidiClock.h` (no clock, unless a test sets its fields, as `tempo.cpp`
does) and `fatfs.h` (an SD card in memory, for `store.cpp`). DaisySP is compiled for the host once into `build/`,
which is ignored.

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
