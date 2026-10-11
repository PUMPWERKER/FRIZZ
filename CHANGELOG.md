# Changelog

What changed for players, newest first. Every branch adds its changes under **Unreleased**;
a release renames that section to its version, and its notes on GitHub's Releases page start
from it. Developer-only changes (refactoring, tests, docs) aren't listed unless they change
what FRIZZ does.

## Unreleased

### Added

- **A scene crossfader by hand** (#66), after the Octatrack's: SHIFT + a scene key, and
  while you still hold SHIFT, turn the transport knob: the morph is in your hand, the scene
  you're in on the left, the one you picked on the right, back and forth as often as you
  like (24 detents from end to end), with or without a loop. Let go all the way right and
  you're in the new scene; all the way left and you're back where you were; in between it
  stays there, as SHIFT + PLAY leaves a morph. The two scene keys light in their mix, the
  transport LEDs show where it is. Over MIDI, CC 118 does the same for any running morph.
  See MANUAL.md, "Crossfading by hand".
- **The chaos key** on the 11th white key (#65), after the Red Panda Tensor's RAND and the
  Chase Bliss Blooper's Scrambler: an effect slot like any other (hold, latch, scenes) that
  makes no sound of its own. Knob 1 lets the effects you've latched drop out and come back on
  random steps (their keys dim while they're out); knob 2 plays random steps of the loop from
  elsewhere in it, while the loop itself runs on; knob 3 sets the grid, 1/16 to a bar, on the
  effects' tempo; knob 4 turns it from every step rolled anew to one bar repeating as a
  pattern. Chance decides only when your effects act, never how they sound. Over MIDI, CC 119
  latches it and NRPN 0/122-125 set its knobs. See MANUAL.md, "Chaos key".
- **A second page for the FX knobs:** press any of knobs 1-4 and all four turn over to the
  selected effect's page 2, with their LEDs pulsing; press again for page 1. Every effect but
  the chaos key has one. Pressing or
  selecting another effect, or the compressor, brings page 1 back. Scenes, recalls and morphs
  carry page 2 along; scene files from v0.11 load with page 2 on its defaults. Over MIDI,
  page 2 is NRPN MSB 1 with the page-1 CC as LSB. See MANUAL.md, "Page 2".
- **Mix, Band and Level on every effect's page 2,** on the same knobs everywhere (#40):
  knob 1 Mix (the dry sound against the effect's: parallel distortion, harmonies under the
  shifter), knob 3 Band (the effect only on the lows or only on the highs, the rest passing
  dry; on the delay and reverb a low or high cut on what goes in) and knob 4 Level (the
  effect's output, off to +12 dB). The flanger has Band and Level; on the delay and reverb,
  knob 1 is Freeze and knob 4 Ducking. At the defaults every effect sounds as before. See
  MANUAL.md, "Page 2".
- **Each effect's own setting on page 2's knob 2** (#40), after the SP-404MK2 and the
  Elektron Tonverk: the freezer's gate (a stutter), the shifter's grain, the flanger's
  polarity, the resonator's env mod (its own Band and Level too), the slicer's shuffle, wow
  & flutter's age (dropouts), the tape stop's darken, the delay's damping (left thinner,
  right darker repeats) and the reverb's pre-delay (up to 250 ms).
- **Freeze and ducking on the delay and reverb** (page 2's knobs 1 and 4): freeze holds the
  echoes or the room, by degrees; ducking turns them down while you play.
- **Stereo on page 1's knob 4 of the folder, crusher and filter**, so knob 4 is stereo on
  every insert but the tape stop (its depth): the folder drives the right channel harder,
  the crusher's right rate runs lower, the filter's right LFO lags. Their old knob 4 (symmetry, XOR, LFO division) moved
  to page 2's knob 2; saved scenes are moved along.
- **Depth on the tape stop's knob 4:** from a full stop (as before) to slowing down to half
  speed.
- **A page 2 for the master compressor:** Mix, a sidechain highpass (off, or 20 to 500 Hz,
  so the bass doesn't pump the rest) and Makeup (0 to +24 dB), on the knobs an effect has
  its Mix, Band and Level on (#38).
- **The safety limiter shows:** the compressor's key turns red while it limits the outputs,
  so you know when to turn something down.
- **Clock source** on the settings page (G# of the upper octave): Auto (the default, as
  before: whichever input ticks first), TRS, USB, or internal (no MIDI clock: the loop, taps
  or the last tempo, and tap tempo works while a clock runs). On TRS or USB, MIDI Start and
  Stop count only from that input too. Saved on the card; `firmware/remote.py source` sets it.
  See MANUAL.md, "Clock source".
- **The beat on the settings page:** the clock factor's key (G# of the lower octave) is lit
  for the first half of every beat of the tempo the effects follow, with the factor applied,
  and dim for the second.
- **MIDI out** on the settings page (D# of the upper octave): off (the default), TRS, or TRS
  and USB. FRIZZ sends its clock, always running while it's on, at the tempo it thinks in: the
  loop's, a MIDI clock coming in (passed on with the clock factor), or the last tempo. A new
  loop sends Start, a pause Stop, PLAY Song Position and Continue, so a drum machine follows
  the loop; without a loop, a DAW's Start and Stop are passed on. Nothing goes back where a
  clock comes from. Saved on the card; `firmware/remote.py out` sets it. See MANUAL.md, "MIDI
  out" (#60).

### Changed

- **`remote.py` and FRIZZ's SysEx** count the chaos key as effect 12, so the compressor's
  knobs are asked for as 13, and a scene travels with 4 effects to a part instead of 3. A
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
  quieter until you set its makeup. Over MIDI, CC 54 is now the attack and CC 55 the release;
  the mix and the makeup are NRPN 1/52 and 1/55.
- **Transport knob in semitones:** while a loop plays, turning it changes the speed a
  semitone every 2 detents, from 2× down to 1/16×, and stops at both ends. SHIFT + turn
  jumps in fifths and octaves as turning did before (from between two of them, to the next
  one in the turn's direction), and is now the only way on past 1/16× into reverse. Over
  MIDI, CC 18 turns it by detents as the hand does, 2 to a semitone; with the CHOMPI key's
  note (45) held, 4 to a step of fifths and octaves. See MANUAL.md, "Looper".
- **Bug reports** moved to the settings page: with the mode switch up, hold SHIFT (CHOMPI)
  and the VOLUME knob pressed for 2 seconds. SHIFT + press the transport knob on the play page
  does nothing now, so a report can't be written by accident while playing. See MANUAL.md,
  "Bug reports".

### Fixed

- **The effects' tempo holds still on a MIDI clock** (#19): at fast tempos (around 174 and
  300 BPM) and between two whole BPM (say 120.4), it used to flip between neighbouring
  BPM many times a second, moving the delay time with it. It's now measured over the last
  few seconds; a jump in tempo is still followed at once.

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
