# Changelog

What changed for players, newest first. Every branch adds its changes under **Unreleased**;
a release renames that section to its version, and its notes on GitHub's Releases page start
from it. Developer-only changes (refactoring, tests, docs) aren't listed unless they change
what FRIZZ does.

## Unreleased

### Added

- **The built-in microphone as an input** (#113), as on TAPE and TEMPO: with no cable in AUX,
  FRIZZ hears CHOMPI's mic, and records it into the looper like AUX. Plugging a cable into AUX
  switches to AUX, pulling it out back to the mic; at power-on the jack decides. On the
  settings page, A# of the upper octave picks the other input by hand (green: AUX, magenta:
  the mic) until the next cable goes in or out; it isn't saved. The input gain, the headphone
  feed and the effects take the mic as they take AUX; it's mono, 5 times the input gain and
  through a highpass at about 150 Hz, as on TAPE (without TAPE's narrow notches, #127), and the
  mono setting doesn't touch it. See MANUAL.md, "Input: AUX or the mic".
- **A scene crossfader by hand** (#66), after the Octatrack's: SHIFT + a scene key, and
  while you still hold SHIFT, turn the transport knob: the morph is in your hand, the scene
  you're in on the left, the one you picked on the right, back and forth as often as you
  like (24 detents from end to end), with or without a loop. Let go all the way right and
  you're in the new scene; all the way left and you're back where you were; in between it
  stays there, as SHIFT + PLAY leaves a morph. The two scene keys light in their mix, the
  transport LEDs show where it is. Over MIDI, CC 80 does the same for any running morph.
  See MANUAL.md, "Crossfading by hand".
- **A phaser** on the 7th white key, next to the flanger (#68), after the SP-404MK2's and the
  Elektron Tonverk's: six allpass stages in a row with feedback, sweeping three notches
  through the sound, inside the resonator's loop, so with the resonator on the sweep moves
  through its ringing. Knob 1 is its rate, in two halves: turned
  left of the centre it runs free, .02 to 50 Hz (its LED in the phaser's colours, violet to
  orange); right of the centre it follows the effects' tempo, 1/16 at the centre, then 1/8,
  1/4, 1/2, a bar, 2 bars and 4 bars at the top (its LED green to yellow), a step every 3
  detents, and with SHIFT a step every detent. Knob 2 is feedback (the resonance), knob 3
  depth (at 0 the phaser is dry; turning it up brings the notches in and widens the sweep, up
  to 2.5 octaves either way), knob 4 the centre the sweep moves around (100 Hz to 6.4 kHz,
  800 Hz in the middle). Page 2: Mix, stereo (the right channel's sweep up to half a cycle
  behind), Band and Level. Its key is violet. Over MIDI it's channel 7, its latch CC 108 on
  the global channel. Scene files from before it load with the phaser off, on its defaults.
  See MANUAL.md, "Phaser".
- **The chaos key** on the 14th white key (#65), after the Red Panda Tensor's RAND and the
  Chase Bliss Blooper's Scrambler: an effect slot like any other (hold, latch, scenes) that
  makes no sound of its own. Knob 1 lets the effects you've latched drop out and come back on
  random steps (their keys dim while they're out); knob 2 plays random steps of the loop from
  elsewhere in it, while the loop itself runs on; knob 3 sets the grid, 1/16 to a bar, on the
  effects' tempo; knob 4 turns it from every step rolled anew to one bar repeating as a
  pattern. Chance decides only when your effects act, never how they sound. Over MIDI it's
  channel 14, its latch CC 115 on the global channel. See MANUAL.md, "Chaos key".
- **A second page for the FX knobs:** press any of knobs 1-4 and all four turn over to the
  selected effect's page 2, and its number blinks on their LEDs, twice (#117); press again
  for page 1, which blinks once, as VOLUME's pages do. On page 2, knob 4's LED is a VU meter
  of the effect's output, in VOLUME's colours and as bright as its Level, so you see the page
  whenever the effect sounds; on the delay and reverb (knob 4 their ducking) it shows their
  return. Every effect but the chaos key has one. Pressing or
  selecting another effect, or the compressor, brings page 1 back; pressing the key of the
  one already on the knobs (an effect's or the compressor's) keeps page 2. Scenes, recalls and morphs
  carry page 2 along; scene files from v0.11 load with page 2 on its defaults. Over MIDI,
  page 2 is CC 18-21 on the effect's channel. See MANUAL.md, "Page 2".
- **Mix, Band and Level on every effect's page 2,** on the same knobs everywhere (#40):
  knob 1 Mix (the dry sound against the effect's: parallel distortion, harmonies under the
  shifter), knob 3 Band (the effect only on the lows or only on the highs, the rest passing
  dry; on the delay and reverb a low or high cut on what goes in) and knob 4 Level (the
  effect's output, off to +12 dB). The flanger has Band and Level; on the delay and reverb,
  knob 1 is Freeze and knob 4 Ducking. At the defaults every effect sounds as before. See
  MANUAL.md, "Page 2".
- **Each effect's own setting on knob 4, stereo on page 2's knob 2** (#40, #117), after the
  SP-404MK2 and the Elektron Tonverk: knob 4 is what you play, the freezer's gate (a
  stutter), the shifter's grain, the flanger's polarity, the resonator's env mod, the slicer's
  shuffle, wow & flutter's age (dropouts); the folder's symmetry, the crusher's XOR and the
  filter's LFO division stay there. Stereo, which you set and leave, moved from knob 4 to page
  2's knob 2, and the folder (the right channel driven harder), the crusher (its right rate
  lower) and the filter (its right LFO lagging) have it too now. The tape stop (its darken),
  the delay (its damping: left thinner, right darker repeats) and the reverb (its pre-delay,
  up to 250 ms) have their own setting on page 2's knob 2; the resonator its own Band and
  Level too. Saved scenes are moved along, and so is MIDI: CC 17 is knob 4, CC 19 page 2's
  knob 2.
- **Freeze and ducking on the delay and reverb** (page 2's knobs 1 and 4): freeze holds the
  echoes or the room, by degrees; ducking turns them down while you play.
- **Depth on the tape stop's knob 4:** from a full stop (as before) to slowing down to half
  speed.
- **A page 2 for the master compressor:** Mix, a sidechain highpass (off, or 20 to 500 Hz,
  so the bass doesn't pump the rest) and Makeup (0 to +24 dB), on the knobs an effect has
  its Mix, Band and Level on (#38).
- **A setting the card can't take shows on the settings page** (#81): when a change there
  (channel, transport following, mono, clock factor, clock source, MIDI out, brightness)
  can't be written, because there's no card or a full one, the key you changed blinks red 3
  times, and again with each of the 3 retries. Before, only the compressor's key on the play
  page blinked.
- **The safety limiter shows:** the compressor's key turns red while it limits the outputs,
  so you know when to turn something down.
- **Clock source** on the settings page (A# of the lower octave): Auto (the default, as
  before: whichever input ticks first), TRS, USB, or internal (no MIDI clock: the loop, taps
  or the last tempo, and tap tempo works while a clock runs). On TRS or USB, MIDI Start, Stop,
  Continue and Song Position count only from that input too. Saved on the card; `firmware/remote.py source` sets it.
  See MANUAL.md, "Clock source".
- **The beat on the settings page:** the clock factor's key (G# of the lower octave) is lit
  for the first half of every beat of the tempo the effects follow, with the factor applied,
  and dim for the second.
- **MIDI out** on the settings page (C# of the upper octave): off (the default), TRS, or TRS
  and USB. FRIZZ sends its clock, always running while it's on, at the tempo it thinks in: the
  loop's, a MIDI clock coming in (passed on with the clock factor), or the last tempo. A new
  loop sends Start, a pause Stop, PLAY Song Position and Continue, so a drum machine follows
  the loop; without a loop, a DAW's Start and Stop are passed on. Nothing goes back where a
  clock comes from. Saved on the card; `firmware/remote.py out` sets it. See MANUAL.md, "MIDI
  out" (#60).

- **MIDI channels per effect** (#97): channels 1-15 are the white keys 1-15 (14 the chaos
  key, 15 the master compressor), the same CCs on each: 14-17 page 1's knobs, 18-21 page 2's,
  22 the latch. In a DAW, one MIDI track per effect with the same automation lanes. They count
  over USB always, and over TRS unless the settings page's new D# (lower octave) switches them
  off for a shared TRS chain. The global channel (16 at first) has the rest. See MANUAL.md,
  "Channels" and "Controllers".
- **On the global channel** (#97): CC 14-22 reach the effect in focus, the one the knobs edit,
  and CC 23 picks it (1-15, by key); CC 82 taps the tempo; CC 90 sets the loop's speed
  outright, in semitones (64 as recorded); CC 102-115 latch the effects by key.
- **The beat on PLAY** (#110): the play page's PLAY LED shows the beat the effects follow, as
  the settings page's G# does: dim white for the first half of each beat without a loop or
  while recording, a short flash on each beat while a loop plays (stronger on each bar's 1),
  and pulsing over the dim position while it's paused. LOOP's red blinks stay as they were.
  See MANUAL.md, "Looper".
- **MIDI feedback** (#104): with the settings page's new upper D# on, what you change on the
  panel goes out as the CCs that would set it, wherever MIDI out goes. An effect's knobs and
  latch go on its channel, and for the effect in focus on the global channel too. The latches
  by key, the focus, the compressor, the gains and the loop's speed go out, and the crossfader
  while you hold it. A recalled scene sends what it changed, a morph where it lands. Nothing
  that came in by MIDI goes back, a value goes at most every 10 ms, and the clock keeps its
  timing. Off at first. See MANUAL.md, "MIDI feedback".
- **The loop follows a DAW's Start and Song Position** (#97), with transport following on: a
  loop recorded to the clock goes back to its start on Start and plays from the next tick, and
  Song Position while stopped moves it there (by its length in 16ths), so it stays lined up
  with the song; Continue plays on from the next tick. A free loop plays on as before. With
  MIDI out on, the jump back sends Start, so the gear behind starts over with it. See
  MANUAL.md, "Start and Stop".

### Changed

- **Heavy effect combinations take less of the CPU** (#136): the effects and the master
  compressor work through each block of 24 samples one effect at a time instead of all of
  them sample by sample. They sound the same as before, but many effects at once leave more
  room, so crackling under a heavy combination (#21) sets in later.
- **The pitch shifter's peaks take less of the CPU** (#137): where a tap starts over, the
  shifter searches for the best match, which took almost a tenth of an audio block at once,
  twice at stereo 0. That search is now cheaper and spread over two blocks, so the busiest
  moments with the shifter on take about 4 to 12 percent less of the CPU, and crackling
  under a heavy combination sets in later still. The shifter sounds as before: each
  crossing of its two taps rests half a millisecond longer, and shifting up its voice comes
  a fraction of a millisecond later (up to 2 ms at the swoop's top).
- **The freezer, shifter and slicer fade in a morph** (#118), with their page 2 Mix, instead
  of switching on the bar line. One the new scene turns off glides to dry over the morph and
  goes off on the bar line, as the other effects do; the shifter or slicer the new scene
  turns on comes on when the glide starts, dry, and its Mix glides up to the scene's. The
  freezer coming on still grabs on the bar line: a grab in the middle of a bar sounds worse.
  The crossfader does the same by hand: the shifter and slicer fade in from the left end,
  the three fade out up to the right end, the freezer coming on switches in the middle as
  before. Stepped knobs of an effect on in both scenes (delay division, slicer pattern,
  shift …) still switch. See MANUAL.md, "Morphing to a scene".
- **Stereo moved from knob 4 to page 2's knob 2** (#117) on the freezer, shifter, flanger,
  resonator, slicer and wow & flutter (and the phaser and, since v0.12-beta.1, the folder,
  crusher and filter): knob 4 is the effect's own setting now, the one you play (see Added).
  Scenes saved before load with every value where its knob went; over MIDI, CC 17 on an
  effect's channel is that setting, CC 19 stereo.
- **Page 2's knob LEDs no longer pulse** (since v0.12-beta.1, #117): turning the page blinks
  its number instead, and on page 2 knob 4's LED is the effect's output VU (see Added).
- **The clock factor works on every tempo** (#116), not only on MIDI clock: the settings
  page's lower G# (as sent, double, half) now also doubles or halves the effects' tempo from
  a loop, from your taps and from the last tempo. Tap the tempo of a song on AUX (SHIFT +
  LOOP), then put the delay, the slicer and the rest at double or half of it. The loop
  itself stays as it is (its length, its beats, the bars it was recorded to), and a loop
  recorded to a clock at double has the effects where they were. Changed while a loop plays,
  the effects' beat 1 stays on the loop's start; a tapped beat stays on your taps. The beat
  on G# and PLAY and the clock MIDI out sends follow it, and the 50-300 BPM limit applies
  after it. At half, a loop of an odd number of beats holds a beat and a half a pass: the
  effects' beats run on evenly, and beat 1 lands on the loop's start every other pass. See
  MANUAL.md, "Clock factor".
- **The MIDI map is new** (#97): everything a DAW automates is a plain CC (DAWs automate NRPN
  poorly, Ableton Live not at all), effects have channels of their own, and nothing collides
  with what hosts and keyboards send by themselves. A setup made for v0.11 needs its numbers
  moved:

  | What | v0.11 (channel 16) | Now |
  |---|---|---|
  | An effect's knobs, page 1 | CC 70-117 (70 + 4 × the effect + the knob − 1) | CC 14-17 on the effect's channel (its key: 1 freezer … 13 reverb) |
  | An effect's knobs, page 2 (new since v0.11) | — | CC 18-21 on its channel |
  | An effect's latch | CC 20-31 | CC 22 on its channel, or CC 102-115 by key on channel 16 |
  | The effect the knobs edit | — | CC 14-22 on channel 16, CC 23 picks it |
  | Master compressor | CC 52-55 | CC 24-31 on channel 16 (28-31 its page 2), or CC 14-21 on channel 15 |
  | Output gain, input gain, mix | CC 56, 57, 58 | CC 85, 86, 87 |
  | Headphone feed | CC 59, 0 the master | CC 89, as the knob: 127 the master |
  | Morph to a scene, its bars, stop | CC 62, 61, 63 | CC 3, 9, 81 |
  | Knobs 1-4, transport, VOLUME turned (relative) | CC 14-19 | FRIZZ's SysEx (`remote.py`) |
  | Loop speed | CC 18, relative | CC 90, outright |
  | Mono input | CC 60 | the settings page, FRIZZ's SysEx |
  | 14 bits | NRPN, the CC's number as LSB | none (CC 46-53 kept free for later) |

  Notes, program changes, All Notes Off and FRIZZ's SysEx stay as they were. A bug report
  from v0.11 replays its CCs by the new map.
- **Switches act as they change** (#97): a latch acts as its CC crosses 64, a morph (CC 3) as
  its value changes, stop and tap as they cross 64 upward. An automation lane sending the same
  value again no longer fires them again, nor takes back a latch you switched by hand.
- **Nothing that comes in by CC is saved** (#97): the compressor's CCs (and v0.11's mono CC)
  went to the card 2 s after they rested, so a DAW's automation wrote it again and again.
  After a restart the compressor is as you left it by hand, its knobs, the settings page or
  FRIZZ's SysEx.
- **The settings page's dark keys are grouped by topic** (#97): the lower octave is MIDI in,
  C# every channel (as before), D# the effect channels over TRS (new), F# transport following
  (was D#), G# the clock factor (as before), A# the clock source (was the upper G#); the upper
  octave C# MIDI out (was the upper D#), F# mono (was the lower F#), G# the LEDs' brightness
  (was the lower A#), and the upper D# MIDI feedback (new). Each keeps its colours. See
  MANUAL.md, "Settings page".

- **The resonator, slicer, wow & flutter and tape stop are one key to the right** (#68): the
  8th, 9th, 10th and 11th white keys, making room for the phaser on the 7th, next to the
  flanger. MIDI notes press the keys, so the notes for them move with the keys, as do their
  MIDI channels (the new MIDI map, below).
- **`remote.py` and FRIZZ's SysEx** count the phaser as effect 12 and the chaos key as 13,
  so the compressor's knobs are asked for as 14, and a scene travels with 4 effects to a part
  instead of 3. A
  `remote.py` from before can't send or fetch scenes from this version; scene files saved by
  it load, with the chaos key off.
- **The knob LEDs use white only for a neutral point:** a knob with a centre (shift, cutoff,
  random, damping, grain, every Band) and Level (at 0 dB) are white there, blue below and
  orange above, on every effect. Other knobs go between the effect's two colours.
- **Nothing sets a level by itself any more but the safety limiter.** The folder is no longer
  matched to its input's level, and the crusher no longer held to it: driven, they get
  louder, the folder much louder on quiet sounds (at full drive a sound 30 dB down comes out
  about 27 dB louder), and page 2's Level sets how loud. Scenes with the folder or the
  crusher come back at a different level than they were saved at.
- **The master compressor is a plain one** (#38): its makeup is no longer automatic (it added
  up to +14 dB, to everything) but a knob on page 2, at 0 dB at first. Speed is split into
  Attack (knob 3) and Release (knob 4), and Mix moves to page 2's knob 1. Saved settings
  come back with Speed as both and Makeup at 0 dB, so a compressor you had turned up is
  quieter until you set its makeup. Over MIDI: CC 24-31 (the new MIDI map, below).
- **Transport knob in semitones:** while a loop plays, turning it changes the speed a
  semitone every 2 detents, from 2× down to 1/16×, and stops at both ends. SHIFT + turn
  jumps in fifths and octaves as turning did before (from between two of them, to the next
  one in the turn's direction), and is now the only way on past 1/16× into reverse. Over
  MIDI, CC 90 sets it outright, in semitones. See MANUAL.md, "Looper".
- **Bug reports** moved to the settings page: with the mode switch up, hold SHIFT (CHOMPI)
  and the VOLUME knob pressed for 2 seconds. SHIFT + press the transport knob on the play page
  does nothing now, so a report can't be written by accident while playing. See MANUAL.md,
  "Bug reports".
- **FRIZZ starts up a quarter of a second faster:** it no longer clears the loop's 32 MB of
  memory at power-on, which a loop never plays before it has recorded it.

### Fixed

- **The pitch shifter's first moment no longer slips through unshifted** (#140): the first
  punch-in after power-on with the shift up, or a punch-in after the shift was turned from
  down to up (or up to down) while the shifter was off, could play a split second of the
  unshifted sound, or a stuck one, under the shifted voice. The shifter now starts its two
  voices afresh in those cases; otherwise it carries on as before.

- **The low-battery countdown shows VOLUME red** (#43): in the 15 s before the CHOMPI
  switches itself off on an empty battery, VOLUME's LED flashes red while the others flash
  yellow, so the battery's red (MANUAL.md, Settings page) is seen exactly when it matters.
  Before, VOLUME flashed yellow with the rest and its red was never shown.
- **No click when a scene recall takes the flanger's stereo back to 0** (#130): depending on
  how far its two sweeps had drifted apart, the right channel's sweep raced back onto the
  left one in a few milliseconds, a click. It now glides there as a short bend, in under a
  tenth of a second; stereo at rest sounds as before.
- **Song Position on a loop recorded to a clock** reads the sender's 16ths by the clock
  factor the loop was recorded at (#116): with the factor changed since, it moved the loop
  to the wrong place.
- **Selecting a latched effect keeps it latched** (#115): SHIFT + an FX key played as a
  quick roll, letting go of SHIFT just as the key went down, or the key landing a moment
  before SHIFT, could count as a plain press or as the latch gesture, and the effect dropped
  out or latched when you let go. SHIFT now counts as first when it goes down up to about
  15 ms after the key, or is let go up to about 15 ms before the key registers; a key that
  touches again just after you let go of it doesn't count as a new press either. The latch
  gesture (hold the key, then SHIFT) works as before; it just needs a little more than
  15 ms between them, as it always gets. The same goes for the compressor's key.
- **What hosts and keyboards send by themselves turns no knob** (#97): a DAW's pitch-bend
  range (RPN 0, then the null RPN) turned the slicer's knobs to 0 or the top, data increment
  and decrement (CC 96/97) moved the resonator, keyboards' CC 71 and 74 the freezer and the
  shifter, GM's CC 91 and 93 the flanger, and a 14-bit controller on the latches (CC 20-31)
  also wrote their LSBs, CC 52-63: the compressor, the gains, mono and the morph. None of
  these numbers is used now. Reset All Controllers (CC 121) does nothing.
- **A note tapped faster than FRIZZ reads its keys** (two in one USB packet) is a tap: notes
  are taken in the order they came. Before, a note on and off within half a millisecond was
  lost.
- **A clock tick inside a SysEx counts** (#18), and the SysEx around it still arrives whole:
  a DAW sending SysEx while its clock runs cost ticks, and the tempo with them.
- **A Song Position after a program change** is read whole: its second byte became a program
  change, recalling a scene.

- **The CHOMPI key lights white while you hold it on the settings page** (#81), as SHIFT on
  the play page and as the manual says, also through the 2 s SHIFT + VOLUME hold for a bug
  report. It stayed dark there.
- **The effects' tempo holds still on a MIDI clock** (#19): at fast tempos (around 174 and
  300 BPM) and between two whole BPM (say 120.4), it used to flip between neighbouring
  BPM many times a second, moving the delay time with it. It's now measured over the last
  few seconds; a jump in tempo is still followed at once.
- **A loop runs at exactly its speed again after the speed has been changed:** after a
  glide (back to 1× or to any other step) it used to stay a hair beside it, about 0.06 %,
  so a loop recorded to a MIDI clock drifted away from it by about half a millisecond a
  second, and the effects' tempo and MIDI out, which follow the loop, with it.
- **A scene file FRIZZ can't read is kept as `frizz_scenes.bak`** also when a save cut
  short by a power cut sits next to it: the boot used to finish that save by deleting the
  unreadable file.
- **A CHOMPI tap no longer ends a morph that MIDI's crossfader (CC 80) holds.** It used to
  freeze it where it was, so the CC had no effect after, or, at 0, take it back to the scene
  it started from. Now only your own crossfader (the transport knob with SHIFT held) ends
  where you let go of SHIFT; the CC's stays the CC's.
- **VOLUME held through the boot animation** no longer switches VOLUME's page when it's let
  go on the play page.
- **A scene sent over SysEx into the active slot** marks it as edited (its key pulses), as
  COPY onto it does, since what plays is no longer what the slot holds.
- **MIDI out over USB no longer drops a tick** that came while USB was still sending something
  else (an answer to a query, or MIDI feedback's CCs): it goes at the next chance, at most 5 ms
  late, instead of not at all (#104). Without a computer on USB (on a battery), nothing is
  tried there any more, where every tick held FRIZZ's other work up a moment (#84).
- **MIDI out at double tempo keeps every tick** of a clock whose ticks come in bunches (a DAW
  over USB sending two at once): one in four went missing, so the gear behind fell behind.
- **MIDI out no longer sends a stale Start after power-on:** a Start that came while FRIZZ
  was starting up went out once it was up, seconds late. A clock running then no longer
  sends a few ticks at once either.
- **Band on the delay, reverb and resonator after a pause:** with page 2's Band turned, an
  effect switched on again could start with a short thump from what the Band filter held
  when it last went off, which the delay then repeated. The filter now starts afresh.
- **An effect's key LED when it comes on again** no longer flashes up for a moment at the
  level it showed when the effect last went off.
- **The crossfader over MIDI after a morph got another bar:** when CC 62 asked again for
  the scene a morph was already gliding to (another bar) and CC 118 then took it, the
  fader's far end was where the morph had got to at that moment, not the scene you came
  from, so arriving there jumped. It's now always where the morph started.
- **A long-held tape stop with Depth below the top:** held for more than about 12 s (Depth
  at 0.8) to 22 s (at 0), the slowed tape suddenly played sound from 11 s before at full
  speed. It now jumps back to the live sound in a short crossfade and plays on slowed.
- **A MIDI clock that sends its ticks two at a time** (some DAWs over USB) no longer locks
  FRIZZ to half its tempo when it starts while FRIZZ is already running (#92).

## v0.11 (2026-10-10)

### Added

- **MIDI control** over TRS or USB, on channel 16 (set in `FRIZZ/frizz_master.txt` or with
  `firmware/remote.py`): notes from 48 up play the keys as a keyboard, with CHOMPI, PLAY,
  LOOP and the knob presses below; CCs latch the effects (20-31), set every effect's knobs
  (70-117, in 14 bits over NRPN), the compressor, the gains, the mix and mono (52-60), turn
  the knobs relatively (14-19) and morph to a scene (61-63); program changes 0-4 are the scene
  keys. MIDI Start and Stop can play and pause the loop (off at first). See MANUAL.md, "MIDI".
- **Remote control over USB:** FRIZZ's SysEx presses keys, turns knobs, and answers the
  state, the LEDs, the processing load and the scenes; `firmware/remote.py` uses it, and plays
  the virtual CHOMPI's scenarios on the device.
- **Settings page** on the mode switch: up, the keys set the MIDI channel (the white keys,
  the last one switching between 15 and 16, or every channel), MIDI transport following, the mono input, a clock factor (follow a
  MIDI clock at half, as sent, or double its tempo) and the LEDs' brightness (100, 75 or
  50 %), each saved on the card; VOLUME's LED shows the battery's level throughout. The loop,
  the effects and MIDI play on while it's up; switch down to play. See MANUAL.md, "Settings page".
- **Bug reports:** SHIFT + press the transport knob writes what you did since switching on
  (every key, knob, the mode switch, MIDI clock and the MIDI it acted on, with the card's scenes as they were at
  power-on) to `/FRIZZ/bug-1.txt` (then `bug-2.txt`, ...). Send it with a bug report: the
  virtual CHOMPI plays it back and shows what happened. The transport LEDs blink white while
  it's written, then 3 times white when it's on the card, red when it isn't.

### Changed

- **The mode switch now matters:** down is the play page, up the settings page. If the keys
  don't play after updating, flip it down.
- The mono input moved from VOLUME's page 3 to the settings page (F# of the lower octave), and
  the battery check from holding VOLUME to the settings page's VOLUME LED. VOLUME has three
  pages now: output, input, headphones.
- The crusher, the filter and the resonator take far less processing time while they're
  off, which leaves more room for the effects that are on.
- Turning the master compressor's knobs takes far less processing time while they glide,
  so it no longer adds to crackles on busy scenes.
- The reverb and the delay take less processing time while they play: the reverb's slow
  modulation no longer computes two cosines a sample, the delay no longer divides by the
  tempo every sample. The delay sounds the same; the reverb's modulation is within a
  thousandth of what it was.
- A flash on a key that's already lit nearly white (a select on a loud effect or on the
  compressor working hard, a tap on LOOP near the loop's end) goes dark instead of white, so
  it's seen.
- LOOP's blink while a quantized recording closes or an erase waits for the loop's end is
  slower, the same as a picked scene slot's, so it doesn't look like a refusal's 3 quick blinks.
- Switching on is a little quicker: FRIZZ clears only the part of its sample memory it
  uses (43 of 64 MB), not all of it.

### Fixed

- A quantized loop is now as long as its bars to a fraction of a millisecond, so it stays with
  the MIDI clock much longer: from a DAW over USB it used to slip up to 50 ms a minute against
  it, now a few ms at most (a short loop at a fast tempo; longer loops far less). It measures
  the clock over the whole recording, up to where it closes, and averages out the ticks'
  jitter.
- Holding an FX key and tapping CHOMPI twice in a save, copy or delete mode confirmed the
  scene and dropped the latch; it now latches and doesn't confirm, as with one tap.
- The delay's pitch-up events (random knob left of centre) read from the wrong place after
  switching on, until the division knob or the tempo first changed.
- The freezer clicked when pressed again right after a release: the repeats now play on until
  the next 16th and hand over to the new capture.
- The flanger clicked when its stereo knob went back to 0, as a scene recall or morph does.
- A card FRIZZ couldn't read at power-on lost its saved scenes and master settings to the
  first save that got through. A card not read at power-on is now never written until the
  next power-on: saves flash red, and the card's files stay as they are.
- A failed write of the compressor's settings or the mono switch is tried again 3 times,
  as the manual says (it was twice, and not at all for a mono change after earlier failures).
- LEDs at full brightness no longer flicker dark: a value a hair above full wrapped to off
  (the transport's colours at full speed, the boot animation).
- Holding an FX key, then SHIFT with a control that does nothing (the transport, or a knob the
  effect doesn't use) no longer cancels the latch or a pending confirm.
- The effects' tempo from a MIDI clock whose ticks come in pairs (two at once, or a late one
  catching up just before the next, as a busy computer can send them) was off by up to 5 %
  and flapped. A pair now counts as two ticks of the right length.
- A quantized loop recorded against a MIDI clock that delivers ticks late and then catches up
  (as a busy computer can) came out up to 56 samples short for 1 bar, so it drifted up to
  26 ms a minute against the clock. The late ticks no longer count for its length: it's as
  close to its bars as with any other clock.
- A bug report that couldn't be written (the card full) left its file open on the card; it's
  now closed, so the card stays sound and the next report is written.
- A restart over MIDI (FRIZZ's own SysEx, which `flash.py`, `card.py` and `remote.py` send to
  get back to the multi-firmware launcher) right after a setting changed lost the setting; it
  now goes to the card first (waiting a second at most).
- The battery colour didn't follow the charging cable: it stayed green while charging and
  stayed white for 20 minutes after a full charge was unplugged. VOLUME's LED on the settings
  page is now white while the cable is in, and shows the battery within seconds of pulling it.
- A single MIDI clock tick that came a little late (a few ms, a sender's hiccup) moved the
  effects' tempo for a moment: 10 ms late at 120 BPM showed 114. A late tick and the next one
  now count together, so the tempo holds.

## v0.10 (2026-10-08)

### Added

- **Mono input** on VOLUME's page 3: turn 3 detents left for a mono (TS) cable, such as one
  from a Lyra-8 or a pedal, and the left channel feeds both sides; 3 right for stereo. Saved
  on the card. By sfaber02 ([#1](https://github.com/ZUHcYa/FrizZ/pull/1)).
- **Headphone feed** on VOLUME's page 4: turned left, blends steplessly from the master out
  to the dry input on its own; back on the master at power-on.
- SHIFT + press VOLUME resets the input/loop mix.
- Picking a VOLUME page blinks its number in white.
- A failed write of the compressor's settings blinks its key red
  and is tried again, up to 3 times.

### Changed

- **New key layout:** the delay and reverb moved one key right (12th and 13th) and the master
  compressor to the last key (15th). The 11th and 14th keys are free, setting the inserts,
  the sends and the compressor apart.
- The headphone feed moved from the mode switch to VOLUME; the mode switch does nothing in
  play mode.
- A morph started with SHIFT + scene key waits while SHIFT is held, however long, and on
  release glides the whole way to the next bar line (one more per tap). The effects it
  fades in come on when the glide starts.
- The DELETE scene mode is amber, so red only means refused.
- VOLUME turns faster: the gains move 2% per detent, the mix (SHIFT + turn) 4%.
- The battery check shows red below 3 V and reads correctly from a second after boot.
- The crusher is held to its input's level.
- The reverb's modulation runs at its intended rate (it was 1/32 too fast); the delay's
  feedback and the filter's resonance glide like every other knob. The sound changes slightly.
- The filter's LFO follows a loop slowed below 50 BPM instead of running ahead.
- The CHOMPI shows up as FrizZ over USB.
- The delay's random events only start while its key is on: after you release it, or a
  scene turns it off, its tail rings out as plain echoes instead of firing new grains.

### Fixed

- Crackles while a loop plays with the delay on, since v0.10-beta.2: the audio processing ran
  at the edge of its time, partly because effects that were switched off kept working in the
  background. They now rest once faded out (the delay and reverb only once their tails have
  rung out), which roughly halves the work with a loop and the delay running. Many effects on
  at once cost as much as before.
- Letting go of CHOMPI before VOLUME no longer also changes the page, and SHIFT + VOLUME press
  doesn't start the battery check.
- Keys without a function no longer cancel a pending confirm or latch; SHIFT + transport
  press does nothing.
- An FX key pressed while a morph switched another effect could lose its on/off.
- The shifter keeps its swoop when a knob turns during it.
- A quantized recording closes when MIDI clock drops out and comes back within a block.
- A delay event on a slowed-down loop no longer runs off the buffer.
- On a loop whose tempo isn't a whole BPM, the delay's echoes, the freezer's length and the
  tape stop follow the loop's exact tempo instead of the nearest whole BPM. The echoes used
  to drift off the loop's beats a little more with every repeat (up to 4 ms a repeat on a
  1/4 delay), which smeared and flammed against the loop while it played.
- The delay no longer zips when a loop closes on a tempo of its own (or the tempo jumps
  otherwise): it crossfades to the new delay time instead of scrubbing through its buffer.
- The master compressor, turned off under a hot signal, releases before bypassing.
- The master compressor no longer crackles at extreme settings: its detector holds a peak
  for 10 ms, and its fastest attack is 1 ms (was 0.5 ms). It sounds smoother on bass and
  reduces slightly more at the same settings (about 1 dB with the amount at 30%).

### Removed

- **The randomizer.** Its line in `FRIZZ/frizz_master.txt` is skipped and dropped at the next
  save; the compressor's settings and the mono input stay as they were.
- The factory self-test page (TAPE or WAVE run the same test).

### Known issues

- Some combinations of effects, especially the shifter and the crusher with a loop playing
  and the master compressor working hard, can need more processing time than there is, which
  is heard as crackles. Switching one of them off clears it. Effects that are off cost (almost)
  nothing.

## v0.9 (2026-10-06)

The first public release. See its
[release notes](https://github.com/ZUHcYa/FrizZ/releases/tag/v0.9).
