# FRIZZ looper: spec and implementation plan

Status: implemented (steps 1-6 below) and tested on hardware (2026-10-05). It works as
specified. The looper feeds the **wet** side of the dry/wet mix (SHIFT + VOLUME). The user docs
call it the **input/loop mix**: the punch-in FX come after it, so the "dry" side isn't dry once
an effect is on. The test showed that "dry/wet" sets the wrong expectation.

Behaviour follows the CHOMPI TAPE 2.0 guidebook, Level 04 (Looper Engine), with the
deviations listed below. Where this document and the guidebook disagree, this document wins.

---

## 1. Spec

### 1.1 What gets recorded

- The AUX input **after input gain** (the same signal as the dry side of the mix), in stereo.
- One loop, held in SDRAM. Max length **2:45** (TAPE's `kMaxRamBuffSize`). Wiped at power-off.
  Nothing is written to the SD card.
- What you hear while recording is up to the dry/wet knob: fully wet means you hear nothing
  until playback starts.
- **The mix jumps automatically:** to **100% wet** when a recording closes into playback (by a
  LOOP press, at the end of a quantized bar, at 2:45, or on clock loss), and to **100% dry**
  the moment an erase fires, so the input fades in while the loop fades out. SHIFT + VOLUME
  can still set anything in between afterwards.

### 1.2 Keys

`PLAY` = PLAY/PAUSE key (`KEY_27`), `LOOP` = LOOP key (`KEY_28`).

| State | Action | Result |
|---|---|---|
| Empty | `LOOP` | Start **unquantized** recording immediately |
| Empty | hold `PLAY`, press `LOOP` | Start **quantized** recording immediately (needs MIDI clock, see 1.3) |
| Empty, no clock | hold `PLAY`, press `LOOP` | **Refused**: LOOP LED blinks red, nothing recorded |
| Recording (unquantized) | `LOOP` | Stop recording now. The loop is exactly what was recorded. Playback starts seamlessly |
| Recording (quantized) | `LOOP` | Keep recording to the end of the current bar, then close the loop. Playback starts seamlessly at that bar line |
| Loop exists | `PLAY` | Toggle play / pause |
| Loop exists | `LOOP` | **Erase** now (not within 500 ms of the press that stopped the recording). LEDs above both keys go dark |
| Loop playing | hold `PLAY`, press `LOOP` | **Erase at the loop's end**: when the read head next crosses the loop point, either direction. Paused: now |
| Erase waiting | `LOOP` / `PLAY` | `LOOP` erases now; `PLAY` takes it back without a toggle |
| Any | SHIFT + `LOOP` | Tap tempo (`TapTempo.h`), never a looper action |

Not implemented, on purpose: overdub, overdub decay, re-recording over an existing loop,
TAPE's monitor-routing modes.

### 1.3 Quantized recording

- 4/4 fixed. One bar = **96 MIDI clock ticks** (24 PPQN).
- **The start is not quantized.** The press that starts recording is the downbeat of bar 1.
  MIDI Start and Song Position neither start nor align a recording. (With transport following
  switched on, Start/Continue and Stop play and pause a recorded loop, Start takes a quantized
  one back to its start on the next tick and Song Position while paused moves it: MANUAL.md,
  *Start and Stop*, `Looper::Follow`.)
- **The end is quantized, strictly.** Pressing `LOOP` records to the end of the bar in progress.
  There's no grace window: a press one tick after a bar line records almost a full extra bar.
- The loop is always a whole number of bars, minimum 1.
- **"No clock"** means no tick received in the last 0.5 s. Quantized recording is refused then.
- The clock matters **only while a quantized recording is running.** After that the loop
  free-runs and ignores the incoming tempo, and the loop is the punch-in FX's clock
  (`TempoClock.h`, MANUAL.md "Tempo and transport"): its bars from the clock give its beats, and the FX's
  beat grid starts at the loop's start and follows its play position.
- **Clock stops mid-recording** (no tick for 0.5 s, e.g. the DAW is stopped before the bar
  ends): the loop closes immediately at the current position, as if unquantized.

### 1.3a Reaching the 2:45 limit

- **Unquantized:** the loop closes at 2:45 and playback starts (TAPE's behaviour).
- **Quantized:** the loop is cut back to the last complete bar and playback starts.

### 1.4 MIDI clock input

- Sources: **TRS** (UART) and **USB** (device mode, same USB-C port as charging).
- **Lock to the first source that ticks** (the clock source's Auto, the default). Ticks from
  the other source are ignored until the locked source has been silent for 0.5 s, then
  whichever ticks next takes over. The settings page can pin it to TRS or USB, or ignore MIDI
  clock (internal): `MidiClock.h`, MANUAL.md *Clock source*.
- For the looper, only clock ticks are used. (Since v0.11 FRIZZ also takes notes, CCs, program
  changes and, optionally, Start/Stop: MANUAL.md, *Appendix: MIDI*.) MIDI out isn't needed for the
  looper; since v0.12 FRIZZ sends its clock and the loop's transport (MANUAL.md, *MIDI out*).

USB-MIDI implications (already shipped in WAVE/TAPE/TEMPO, so low risk):
- CHOMPI is a USB *device*. It works with a computer/DAW or gear with a USB *host* port; a
  synth that is itself a USB device can't send clock to it over USB (use TRS).
- USB delivers in 1 ms frames, so ticks jitter by up to 1 ms. Fine at any sane tempo.

### 1.5 Transport knob (encoder 5, the big purple one)

- **Turn while playing:** speed in **semitones**, 2 detents per step, from **2×** down to
  **1/16×** (+12 to −48); the ends stop it, it never flips to reverse. Pitch changes with
  speed, like tape. In reverse, it turns the same way (right slower, left faster).
- **SHIFT + turn while playing:** TAPE's quantized looper pitch (`SetLooperPitchQuantized`
  in TAPE's `DSPEngine.h`): **steps of 5ths and octaves**, alternating ×1.5 / ×1.335 so every
  second step is an octave, 4 detents per step. From a point between two rungs (after
  semitones) it goes to the nearest rung in the turn's direction. Past **1/16×** it flips
  into **reverse** and climbs the same ladder to **−2×**: the only way into reverse.
- **Press:** reset to 1× forward.
- **Turn while paused:** scrub through the loop; SHIFT + turn does nothing.
- No loop: the knob does nothing.

### 1.6 LEDs

| LED | State | Look |
|---|---|---|
| LOOP key | recording | red |
| LOOP key | quantized, `LOOP` pressed, waiting for bar end | red, blinking |
| LOOP key | quantized refused (no clock) | 3 fast red blinks |
| LOOP key | erase waiting for the loop's end | red, blinking |
| PLAY + LOOP keys | playing | white, crossfading PLAY → LOOP to show the position in the loop (as TAPE) |
| PLAY + LOOP keys | paused | dim white at the current position |
| PLAY + LOOP keys | empty / just erased | off |
| Transport (PTH 5/6) | loop exists | TAPE's speed colours: blue above 1×, red towards stop; LED 5 vs 6 shows direction |
| Transport (PTH 5/6) | empty | off |

---

## 2. Implementation plan

### 2.1 Reuse vs write new

TAPE's `LooperEngine.h` sits on `FileSampler` (`Sampler.h`, 373 lines), which is built for
WAV-file playback, overdub and tape slew. Most of that we don't want. Plan:

- **Take TAPE's buffer size** (`kMaxRamBuffSize`: 2:45 stereo int16, SDRAM-page aligned), but
  not `RamBuffer.h` itself: its read/write heads step one frame at a time, and step 4 needs a
  fractional read head. `Looper.h` indexes the buffer directly.
- **Copy** the 5ths/octaves stepping logic from TAPE's `SetLooperPitchQuantized`.
- **Write new** `Looper.h` (~200 lines): record, play/pause, erase, a fractional read head
  for varispeed and reverse (linear or Hermite interpolation), scrub, and a short
  crossfade at the loop point so it doesn't click.
- **Write new** `MidiClock.h`: UART + USB MIDI init (from WAVE's `MidiManager.h`), clock tick
  handling (as in TEMPO's `MidiManager.h`), source lock and timeout.

### 2.2 Timing detail: making the loop exactly N bars long

A press lands somewhere *between* two ticks. Simply stopping on the 96·N-th tick after the press
would make the loop up to one tick (~21 ms at 120 BPM) too short or too long, so it would slip
against the beat on every repeat.

Fix: count ticks to find **N** (the bar in progress when `LOOP` is pressed), measure the tick
period **T** in samples (averaged over the recording), and close the loop at exactly
`96 · N · T` samples after the press. The audio block size is 24 samples (0.5 ms), which bounds
the error.

Notes for implementing this against `MidiClock.h`:
- **T is the slope of a least-squares line through the ticks since the press:** one point
  (tick count, tick time) per block a tick came in, kept up to where the loop closes, not only
  to the end press: a loop of one bar stopped halfway would be measured over half a bar. A tick's
  time is only as precise as the block it came in (0.5 ms) and over USB the 1 ms frame it was
  sent in; the span from the first tick to the last kept both ends' error (up to 82 samples a
  loop from a DAW, 50 ms a minute of drift), the line averages it out over every tick (a few
  samples). Until a beat of ticks is in, `GetTickPeriod()` stands in. Its smoothing only weights
  the last ~10 ticks and lags drift; it's for display. `unit.sh sync` measures all this.
- **The start and end are timed in the engine, not the UI.** `OnButton` runs from `MainLoop`,
  which isn't sample-aligned, so it only posts a command. The looper picks it up at the start of
  the next audio block, and from then on the number of recorded frames *is* the elapsed time
  since the press. Tick times come from the same audio sample clock (`MidiClock.h`).
- **N = floor(elapsed samples ÷ (96 · T)) + 1 at the end press,** not the raw tick count. A press
  exactly on a bar line counts as the start of the next bar (strict rule), and this avoids an
  off-by-one when the press lands on a bar-line tick.

### 2.3 Steps

1. **MIDI clock in.** *(done: `MidiClock.h`)*
   - Re-add MIDI UART + USB init and polling (from WAVE).
   - Add tick counting, source lock, 0.5 s timeout and tick-period averaging.
   - Poll from the audio callback, as WAVE did.
2. **Looper core.** *(done: `Looper.h`)*
   - Add the loop buffer in SDRAM (`DSY_SDRAM_BSS`) and restore `ZeroSDRAM()` at boot.
   - Record and play at 1×, with the loop-point crossfade (5 ms, using a post-roll recorded past
     the loop end, see `Looper.h`), quantized end, clock-loss and 2:45 handling.
   - Wire its output into `wetl`/`wetr` in `passthroughEngine.h`.
3. **Keys.** *(done: `PlayKeys.h`, routed by `NormalPage.h`)* PLAY/LOOP state machine per 1.2, including the quantized
   end and the erase (`PlayKeys.h`). LOOP acts on press; PLAY acts on release and only if LOOP
   wasn't pressed during the hold. The erase mirrors the record: LOOP now, PLAY held + LOOP at
   the loop's end.
4. **Transport.** *(done: `Looper.h`, `NormalPage.h`)* Stepped varispeed, reverse,
   press-to-reset, scrub when paused. The read head is a frame index plus a fraction, read
   with 4-point Hermite interpolation. As TAPE's defaults: speed glides ~0.2 s to each new
   step (tape slew), and scrub speed follows the detents turned per 1/8 s. Play/pause keeps
   the short fades rather than TAPE's tape stop/start.
5. **LEDs and mix jumps.** *(done: `NormalPage.h`)* LEDs per 1.6, the mix jumps per 1.1, and
   the temporary beat flash from step 1 removed.
6. **Docs.** *(done)* `README.md` controls tables (now in `MANUAL.md`), MIDI clock section and file map.

Verification per step: build with GCC 10.3, check the memory table, then test on hardware:
loop length against a DAW's clock, no clicks at the loop point, source switching TRS ↔ USB.
