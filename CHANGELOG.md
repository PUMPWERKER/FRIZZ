# Changelog

What changed for players, newest first. Every branch adds its changes under **Unreleased**;
a release renames that section to its version, and its notes on GitHub's Releases page start
from it. Developer-only changes (refactoring, tests, docs) aren't listed unless they change
what FRIZZ does.

## Unreleased

### Added

- **A frequency shifter** (#306), the phaser's second mode: hold SHIFT, tap white key 7 (it's
  selected, as ever), then tap it again: the key turns azure, and every frequency in the sound
  moves up or down by the same number of Hz, so its harmonics no longer line up. A few Hz with
  feedback and the direction to one side make a barber pole that rises (or falls) forever, tens
  to hundreds of Hz a metallic, bell-like sound. Each further tap in the same SHIFT hold switches back and forth. The same
  knobs, read the other way: the shift (free, 0.1 Hz to 1 kHz, or a cycle per tempo division),
  feedback, depth and the direction (down, both, up); page 2's stereo turns the right channel
  the other way. A scene keeps the mode (an older FRIZZ loads it as the phaser), a morph
  switches it at its landing, and CC 23 on the phaser's channel (7) sets it over MIDI. After
  Mutable Instruments' Warps. See [Frequency shifter](MANUAL.md#frequency-shifter).

- **The Companion** (#278): a web page, [frizz.friedrichwagner.eu/companion](https://frizz.friedrichwagner.eu/companion/),
  that manages your loop banks over the USB cable while FRIZZ plays, in Chrome, Edge and other
  Chromium-based browsers and in Firefox. It lists what's on the card, FRIZZ's banks with each
  loop's name and TAPE's cubbis, uploads audio files into FRIZZ's slots (converted to 16-bit
  48 kHz in the browser, their name, and their beats where it knows them, written into the
  file), moves, swaps and deletes them, and copies a cubbi into a FRIZZ slot, as one batch with
  a progress bar. A loop that plays plays on, and an upload cut short leaves the slot as it
  was. Uploads go over MIDI: the longest loop takes about two minutes, so for many loops at
  once the card as a USB drive is faster. FRIZZ now reads a loop's name from its WAV
  (`LIST/INFO/INAM`), which the Companion shows. See [The Companion](MANUAL.md#the-companion).

- **Loops from the SD card** (#70): put your own WAVs in the card's `FRIZZ` folder as `a1.wav`
  to `h15.wav` (bank a-h and the white key, 16-bit 48 kHz, up to 16 bars at 50 BPM) and play
  them from the new Loops page, SHIFT + the lower C#, one on each white key, the lower C# and D#
  for the banks. The recordings you made with TAPE (its cubbis, `cubbi_a1.wav` to
  `cubbi_e15.wav`, in `TAPE` or the card's root) play the same way from the Cubbi page, SHIFT +
  the lower D#, in TAPE's bank colours; FRIZZ only reads them. The lower C# and D# light in the
  colour of the bank they lead to. A white key switches at once with a short crossfade, in
  time: the new loop starts as far into its pass as the old one, so loops at one tempo stay on
  the beat. SHIFT + a white key switches at the end of the loop's pass (tap it more for more
  passes). In one SHIFT hold with a scene change, the first key decides what's counted: a
  scene key first counts bars and the loop lands with the morph or drop, a white key first
  counts passes and the scene lands with the loop at their end. With the crossfader (SHIFT + a
  scene key, the transport turned) a white key puts both loops under the fader, and letting go
  keeps the nearer one. Switching keeps the tape's speed and direction. The loop sets the
  tempo; LOOP, PLAY and the transport knob work on it as on a recording, which is gone once you
  switch. Two quick taps of SHIFT take you back to the play page. Over MIDI, a bank select (CC 0:
  1-8 FRIZZ's banks, 9-13 the cubbis') and a program change pick a loop from any page, CC 54
  says when (at once, at a pass end, with the scene change), CC 81 cancels, and MIDI feedback
  sends the switches you make by hand, so a DAW records them. See
  [Loop loader](MANUAL.md#loop-loader), [Playing loops](MANUAL.md#playing-loops) and
  [Loops and scenes](MANUAL.md#loops-and-scenes).

- **FRIZZ is a USB sound card** (#196): on a computer's USB it's an audio interface called
  FrizZ, next to its MIDI, 2 channels each way at 48 kHz / 24 bit, as USB Audio Class 1, which
  macOS, Windows, Linux and iPadOS know without a driver. FRIZZ sends the master as it is before
  VOLUME, for recording in a DAW; what the computer plays is the input's third source, **USB**:
  the settings page's upper F# now picks AUX, the mic or USB (blue), each press the next, and
  USB has its own input gain, monitoring and effects, saved, as AUX and the mic have; the
  looper records it. A cable going in or out of AUX doesn't switch away from USB. With USB the
  source, SHIFT + upper F# switches USB audio off (and on again); it's on out of the box, and
  saved; switching it, MIDI over USB is gone for about a second. Set the computer to 48 kHz for
  the best quality. USB audio and the card drive (below) build on lost-tty's
  [MIXTAPE](https://github.com/lost-tty/CHOMPI), USB audio for TAPE (MIT). See
  [USB audio](MANUAL.md#usb-audio).

- **The card as a USB drive while FRIZZ plays** (#196): on the settings page, SHIFT + LOOP held
  2 s lends the card to the computer (LOOP blue), so you can copy loops, scenes and bug reports
  without the USB storage firmware; the loop and the effects play on. Eject it on the computer,
  unplug, or hold SHIFT + LOOP again to take it back: FRIZZ finds the loops again, and reads
  `frizz_scenes.txt` and `frizz_master.txt` again if the computer changed them (#291), as at
  power-on: the settings apply at once (the input gains and the compressor glide there), the
  scenes come in for their next recall, and what plays stays. If you changed a scene or a
  setting on FRIZZ while the card was lent, FRIZZ's wins and the computer's file is kept as
  `frizz_scenes.bak` or `frizz_master.bak` (in v1.5.0-beta.1, FRIZZ kept what it had and its
  next save wrote over the computer's changes). See
  [The card as a USB drive](MANUAL.md#the-card-as-a-usb-drive).

- **A crashed FRIZZ restarts by itself** (#105): if FRIZZ crashes, or its sound stops for
  about a second, the CHOMPI restarts instead of staying frozen until you switch it off: into
  the launcher's picker if you use it, or else FRIZZ starts again. A loop in RAM is lost, as
  with the power switch. See [Power and battery](MANUAL.md#power-and-battery).

- **Figures in the manual:** each effect's chapter, the [chaos key](MANUAL.md#chaos-key)'s and
  the [finalizer](MANUAL.md#finalizer)'s show where its white key is and its knobs'
  colours; the panel, with what a chapter plays lit, opens
  [Playing an effect](MANUAL.md#playing-an-effect), [Looper](MANUAL.md#looper),
  [FX scenes](MANUAL.md#fx-scenes), [Scene changes in time](MANUAL.md#scene-changes-in-time),
  [the Loops page](MANUAL.md#the-loops-page) and the [settings page](MANUAL.md#settings-page);
  and the gestures to know are drawn as keys on the
  [Key combinations](MANUAL.md#key-combinations) page, in the [quick start](QUICKSTART.md) and
  on the Loops page.

### Changed

- **Knobs 1-4 and VOLUME speed up when turned fast** (#375): turned slowly, a knob still moves
  1% a detent, so a precise setting is as easy to hit; turned faster than about 8 detents a
  second, its step grows smoothly, up to 5% a detent from about 40 a second, so a quick sweep
  across the whole range takes about one turn instead of three. Slow again, it's back to 1% from
  the next detent, and 0% and 100% are still reached exactly. The effects' knobs on both pages
  and the finalizer's do this; SHIFT + turn (the coarse grid), the stepped parameters, knob
  presses, MIDI and the transport knob are as before. VOLUME speeds up more carefully, at most
  3 times (the gains 6% a detent, from 2%), so a fast turn of the output can't jump the level.
  See [Playing an effect](MANUAL.md#playing-an-effect).

- **Key 15 is now the finalizer, replacing the master compressor** (#345, #347): a master
  section after the effects, in four modules of four knobs. **Key 15's knobs change, and the
  compressor's settings are dropped**: a card from before loads with the finalizer neutral (and
  every other setting as it was), and the old compressor's line is no longer written. This is
  the one change of the controls in 1.5, decided for it. The modules, in the order the sound
  goes through them:
  - **Level** (white): Input, a clean trim of ±12 dB; Drive into a saturation, Boost, Tube or
    Fuzz, at twice the sample rate and level-compensated; Loud, up to +18 dB into a limiter
    with a ceiling of -1 dB that looks 1 ms ahead, for loudness without clipping.
  - **EQ** (yellow): the low, mid and high band's levels, -20 to +8 dB, split at 300 Hz and
    3 kHz.
  - **Comp** (cyan): a compressor on each band, its character picked from six (Light & fast to
    Beat FX, Parallel among them), one Depth for all three. No makeup: level comes back
    through Drive and Loud.
  - **Width** (magenta): each band's stereo, from mono to twice as wide.

  Hold SHIFT and tap key 15 twice for the next module; the key shows its colour. At the
  defaults it leaves the sound untouched. Its knob LEDs are meters while they rest: the input
  level, each band's level, gain reduction or correlation, the limiter's reduction. A press of
  knob 1-3 in EQ, Comp or Width mutes that band, as a DJ mixer's kill. Hold key 15 and press
  SHIFT to bypass it; four presets, kept on the card, are recalled with key 15 held and a scene
  key, and saved, copied or deleted as scenes are, with key 15 held and the upper F#, G# or A#.
  While Loud is up, the outputs and MIDI clock out are 1 ms later, so gear behind stays in
  time. Over MIDI, channel 15 has every parameter at a fixed CC (CC 14-20 Level and EQ, 24-30
  Comp and Width, 21 a preset, 22 on or bypassed, 23 the module, 102-104 the mutes), the global
  channel CC 24-27 Level's knobs and CC 28 Depth. See [Finalizer](MANUAL.md#finalizer).

- **AUX keeps its bass** (#317): the input's DC blocker, TAPE's, cut AUX's lows from about
  76 Hz down (50 Hz 5 dB quieter, 30 Hz 9 dB); it now takes out only DC, from about 8 Hz down,
  so a kick or a bass synth goes into the loop and the effects at full weight. The mic too,
  though its own 150 Hz highpass stays. USB's input never had it.
- **The filter (white key 5) is a state-variable filter now** (#304): one filter a channel,
  lowpass left of the centre and highpass right, where it was WAVE's lowpass and highpass in
  series. The cutoff knob and the resonance keep their ranges: the cutoff sits within about
  half a semitone of where it was at the default resonance, and the resonance goes from none to
  about a 20 dB peak, as before. What you hear differently: at the centre the filter is now
  really flat, where WAVE's lowpass stayed in with the resonance on it, a peak near 8 kHz (+5 dB
  at the default resonance, up to +25 dB with the resonance up), on the whole highpass side too;
  the highpass is 12 dB per octave now, where it was 6 dB at low resonance, so it thins the
  sound faster; and the far right is no longer quieter. The filter takes less of the CPU.
- **MIDI map: CC 0 is the bank select** (#259), where it was left free: 0 (the default) keeps
  program changes on the scenes, 1-8 before a program change pick a loop from FRIZZ's banks
  a-h, 9-13 a cubbi. A host that sends another bank select before its program changes now
  picks loops: send bank 0 for the scenes. See [Program changes](MANUAL.md#program-changes).
- **MIDI map: CC 84 picks the input's source in thirds** (#196): 0-42 AUX, 43-85 the mic, 86-127
  USB (it was 0-63 AUX, 64-127 the mic: a controller sending 127 for the mic now picks USB; send
  64). MIDI feedback sends it as 0 for AUX, **64 for the mic** (it was 127) and 127 for USB. See
  [Controllers](MANUAL.md#controllers).
- **USB is TinyUSB now** (#196): FRIZZ's USB, its MIDI too, runs on a new USB stack. The MIDI
  clock out over USB keeps its time while FRIZZ reads the card (it could come up to 8 ms late
  while a cubbi loaded, #265). FrizZ asks the computer for 100 mA, where it asked for 160. See
  [MIDI out](MANUAL.md#midi-out).
- **The folder keeps the bass** (white key 3): it cut everything below about 76 Hz, even with
  the drive at off, 5 dB at 50 Hz and 9 dB at 30 Hz, so a latched folder took the weight out of
  kicks and bass. Now 50 Hz comes through within half a dB, and turning Symmetry no longer
  thumps on a quiet signal. With Symmetry up and a loud hit at medium drive, the bass a hit
  pushes up rings out a little longer than before. See [Folder](MANUAL.md#folder).
- **Effects that are off take less of the CPU:** the filter, the crusher, the shifter, wow &
  flutter and the flanger no longer keep running unheard while off; they start afresh when
  punched in, without a click: FRIZZ with every effect off takes about 22% of the processing
  time where it took 31%, more headroom when only a few effects are on. A punched-in
  crusher's steps fall at other moments than before, and wow & flutter's wow goes on from
  where it rested.
- **The shifter is brighter** (#305): its taps read between samples more precisely, so the
  shifted sound keeps more of its highs (a 10 kHz tone a fifth up loses 0.3 dB, where it lost
  1.3 dB) and carries less aliasing, most audibly with feedback, where every pass lost highs
  again. The rest of the shifter is as it was.
- **The reverb's tail is smoother, less metallic:** its first allpass is modulated, as in
  Mutable's Clouds and Elements, so the same hit rings a little differently each time instead of
  through the same diffusion. Its knobs, decay, level and freeze are as they were. See
  [Reverb](MANUAL.md#reverb).

### Fixed

- **The knob presses' notes do nothing on the Loops and Cubbi pages** (#360), as knobs 1-4 there:
  notes 36-39 turned the selected effect's page, and with SHIFT's note 45 reset its knob. See
  [Notes: the keys](MANUAL.md#notes-the-keys).
- **A MIDI clock on the TRS jack keeps its time with other MIDI on the same cable** (#322): a
  clock tick followed straight away by other messages (a sequencer's chord, automation for other
  gear on the cable) reached FRIZZ only once they had passed too: about 3 ms late behind a
  chord, 15 ms and more behind a stream of CCs, and a cable that never paused held the clock
  back altogether. MIDI out passed the clock on that late, and a loop started by MIDI's Start
  started that late. Now each tick counts the moment it arrives.
- **Dense MIDI on the TRS jack no longer loses messages** (#356): more than about 80 ms of MIDI
  without the shortest pause and without a clock tick in it (a DAW's automation for many
  knobs at once, written back to back) lost whole stretches of it, so knobs it moved stayed
  where they were. Now every message counts, however long the cable stays busy.
- **The shifter and the flanger no longer click when punched in with the feedback up** (#326):
  the feedback's share of what they had stored came in at once under the key's fade, and
  repeated on every trip. Now it fades in with the key.
- **The phaser's LFO stays on the beat after a long rest** (#327): punched in again after a
  while off, with the tempo moved meanwhile (or a MIDI clock's tempo wavering), its synced LFO
  could start up to a pulse off and jump back at the next one.
- **Opening FRIZZ's USB MIDI port late no longer brings eleven beats of old clock**: with MIDI
  out on USB and no app on the computer reading FRIZZ's port yet, the clock piled up, and an
  app opening the port later (a DAW, at any time after) got about 270 ticks at once, eleven
  beats, and any Start or Stop among them. Now what nobody takes for a tenth of a second is
  dropped: the app gets less than a beat of old clock, then the clock as it runs.
- **Scenes and settings always save into the card's `FRIZZ` folder:** after the card failed to
  answer once (a hiccup FRIZZ recovers from by itself), the scenes and settings saved from then
  on until a restart could land in the card's root, shown as saved but gone at the next
  power-on. Now they go to `FRIZZ` whatever happened before.
- **A restart over MIDI (the launcher's) takes the card back first:** with the card lent to the
  computer as a USB drive, FRIZZ restarted with the computer still on it. Now it takes it back
  as SHIFT + LOOP does, saves what waited for the card, then restarts. And a setting changed just
  before a restart is now always on the card: half the time FRIZZ restarted without waiting for
  it.
- **The card is taken back when the computer ejects it, every time:** an eject that came in
  the same instant FRIZZ looked could be missed, leaving the card lent until SHIFT + LOOP.
- **A card error while FRIZZ looks for your loops no longer hides the ones after it:** the
  slots after the error showed empty until the next scan; now FRIZZ reads the folder again.
- **Switching USB audio waits the 100 ms it should** before FRIZZ leaves the bus, so the
  answer to a tool's request that switched it still reaches the computer.
- **A loop switch at once always crossfades** (#302): now and then a switch by a white key or
  a program change cut over hard, with a click, instead of fading the old loop out over 10 ms,
  most often when two switches came within a few ms of each other (a program change right
  after a key). Now every switch fades; one that comes during another's fade waits for it.
- **A loop switch at a pass end no longer clicks right after another switch** (#318): when the
  loop reached its end within 10 ms of a switch, the switch planned for that end (SHIFT + a
  white key, or CC 54) cut the earlier switch's fade-out off, a click. Now that fade-out ends
  early but smoothly, over 2 ms, and the planned switch stays exactly on the pass end. The same
  holds for a switch while the crossfader still mixes two loops.
- **The resonator rings on its note with the tone dark** (#303): the darker the tone and the
  higher the note, the flatter it rang, 67 cents (two thirds of a semitone) at 440 Hz with the
  tone at the bottom and still 20 cents with it a quarter up. Now its notes are within 5 cents
  at any tone, so the coarse steps' notes at A440 hold, also on the right channel with the
  stereo up. At the default tone it moves only by a few cents.
- **A key of the Loops page is bright only while its slot holds the loop playing** (#295):
  deleting the playing loop's slot in the Companion left its key bright, though the slot was
  empty; replacing it did too. Now the key goes dark (deleted) or dim (replaced) while the
  loop plays on, and moving or swapping it in the Companion takes the bright key along.
- **A loop whose audio starts on a 512-byte boundary lights its key and starts again from the
  top**: such files (Logic, GarageBand and other CoreAudio apps pad their WAVs that way, so do
  some Pro Tools writers) played, but every second switch to one left its key dim instead of
  bright, its key didn't start it again from the top, and FRIZZ didn't tell the Companion it
  played.
- **The free space on a card without a free count is counted right**: on a card a computer left
  without its free count, a scene save, bug report or card lend while FRIZZ was counting could
  leave it with a wrong number, which it then wrote onto the card. Now it counts again from the
  start after any of these.
- **A playing loop the computer replaced while it had the card no longer keeps its key bright**
  (#332): once the card is back, its key is dim and the old loop plays on, as after a replace in
  the Companion; a tap loads the new file. A loop whose file wasn't touched stays bright.
- **FRIZZ tells the Companion which cubbi plays**: it said which of its own slots played and
  was loaded, but never a cubbi.
- **With the card lent to the computer, the loop you played before switches at once** (#292):
  it waited until the card was back, though it was already in memory. A loop that has to be
  loaded from the card still waits.
- **A card error while the Companion replaces, moves or swaps a loop no longer leaves a key lit
  for a loop that's gone** (#294): the Loops page and the Companion's list show what's on the
  card afterwards, and a swap that fails half way is put back at once.
- **The shifter is mono again right after its stereo returns to 0** (#226): turned up and
  back to 0 while the shifter plays, its left and right channel stayed apart (hollow when
  summed to mono) until it was punched in again or the shift passed the centre. Now the right
  channel glides onto the left's within a grain (about 30 ms), without a click. See
  [Shifter](MANUAL.md#shifter).
- **An SD card error no longer freezes the panel for half a minute** (#213): if the card fails
  while FRIZZ saves a scene or the settings, writes a bug report or reads at power-on, the
  LEDs and keys come back within seconds, not after 30 or 60 (the sound never stopped), and
  the save flashes red as when the card can't be written. A card that stops answering
  altogether can take a few seconds longer. See
  [Scenes and settings on the card](MANUAL.md#scenes-and-settings-on-the-card).
- **SHIFT + press of knob 1 on the settings page sets the LEDs to 100 %**: it was knob 2's
  press that did, and knob 1's did nothing. See [LED brightness](MANUAL.md#led-brightness).
- **Save, copy and delete's notes do nothing on the Loops and Cubbi pages**, as the upper F#,
  G# and A# there: notes 66, 68 and 70 started the function, which the next tap of SHIFT could
  confirm. See [Notes: the keys](MANUAL.md#notes-the-keys).
- **A key a MIDI note holds no longer stays held for good when its note off can't come**: when
  the computer goes away (the cable pulled, the computer asleep) or USB audio switches, every
  key MIDI holds lets go, and a note off on any channel lets go of the key its note holds, so
  changing the MIDI channel on the settings page meanwhile no longer leaves an effect punched
  in or SHIFT held. See [Notes: the keys](MANUAL.md#notes-the-keys).
- **A key held by your hand and by a note is pressed once on the Loops and settings pages
  too**: there the note pressed it a second time on the play page, so letting go of note 45
  ended SHIFT while your hand still held it, and a scene key's note recalled its scene again.
  See [Notes: the keys](MANUAL.md#notes-the-keys).
- **SHIFT held from the play page counts for the settings page's 2 s holds**: with SHIFT held
  as you flip the switch up, CHOMPI lit and SHIFT + upper F# worked, but SHIFT + VOLUME or
  SHIFT + LOOP held 2 s did nothing. Now they write the bug report and lend the card. See
  [Settings page](MANUAL.md#settings-page).

## v1.0.1 (2026-10-13)

### Changed

- The quick start shows how the sound flows as a diagram that reads on a phone, and the
  manual notes that the built-in mic hears CHOMPI's own keys and knobs (#245).
- The manual's diagrams show their names at the size they were drawn for (browsers set them
  larger), so the input diagram's last label ("to the phones feed") is no longer cut off at
  its right edge.

## v1.0.0 (2026-10-13)

### Added

- **A hard drop to a scene on a bar line** (#153, #175), after the Neuzeit Instruments
  Drop's drop mode: hold SHIFT, tap a scene key, press the transport knob, let go. The scene
  lands all at once on the downbeat, as a recall would there: what it turns on starts on the
  line (the freezer grabs, the slicer attacks, the tape stops), what it turns off has faded out
  by then. More taps of the scene key put it a bar later. SHIFT + scene key A, then scene key
  B: a build-up, the morph to A, and B dropping on the line where it ends. Its key blinks red
  on the beat while it waits. Over MIDI, CC 83 drops to scene 0-4 (CC 9 sets the bars). See
  [Timed scene changes](MANUAL.md#timed-scene-changes).
- **The built-in microphone as an input** (#113), as on TAPE and TEMPO: with no cable in AUX,
  FRIZZ hears CHOMPI's mic, and records it into the looper like AUX. Plugging a cable into AUX
  switches to AUX, pulling it out back to the mic; at power-on the jack decides. The mic starts
  with its monitoring off (#152, #177): a speaker would feed it back. Record it and play the
  loop, hear it on the headphones' input-only feed (VOLUME's page 3), or switch on its
  monitoring on the settings page (below). It's mono, 5 times the input gain and through a
  highpass at about 150 Hz, as on TAPE (without TAPE's narrow notches, #127), and the mono
  setting doesn't touch it. See [Input: AUX, the mic or USB](MANUAL.md#input-aux-the-mic-or-usb).
- **AUX dry** (#152): AUX can go past the effects and the master compressor, straight to
  VOLUME and the safety limiter, so a drum machine or a finished song plays as it is while you
  play the loop through the effects. The mix knob still sets the balance (its input share is
  the dry AUX), and the mix's automatism stays: a recording finishes on the loop alone, an
  erase goes back to the input alone. See [Input: AUX, the mic or USB](MANUAL.md#input-aux-the-mic-or-usb).
- **The input's three keys: source, monitoring, effects** (#177, #243). On the settings page's
  upper octave, left to right the source, whether you hear it, then how (changed since
  v1.0.0-beta.3: G# is the monitoring, A# the effects; the betas called monitoring "live"): F#
  picks the source, AUX (green) or the mic (magenta), until the next cable goes in or out of
  AUX; G# switches its monitoring, on the outputs (lit) or not (dim), as in a DAW: the looper
  records the source either way; A# sends it through the effects (green) or dry past them
  (white).
  G# and A# are each source's own and saved: the jack or F# brings up the other's. So **the mic
  can be monitored**, through the effects or dry (mind the feedback near a speaker), and **AUX
  can be unmonitored**, recorded without being heard on the master. Out of the box AUX is
  monitored through the effects, the mic through the effects but not monitored. A# is dark
  while the source isn't monitored; pressed then, it blinks red 3 times and changes nothing.
  The looper always records the source as it is. Over MIDI, CC 84 picks the source (0-63 AUX, 64-127 the mic), CC 116 sets its
  effects (64 and up dry), CC 117 its monitoring (64 and up on), without saving them, and MIDI
  feedback sends all three. A beta.1 card's AUX dry is AUX's A#. See [Input: AUX or
  the mic](MANUAL.md#input-aux-the-mic-or-usb).
- **The LEDs' brightness on knob 1, 25-100 %** (#176): on the settings page knob 1 sets it, 5 %
  a detent; SHIFT + turn steps 25, 50, 75, 100 %; SHIFT + press is 100 %. Its LED is white and
  dims with the others. A card's 50, 75 or 100 % stays as it was. See [LED
  brightness](MANUAL.md#led-brightness).
- **An input gain for each input, saved** (#152): AUX and the mic each have their own input
  gain (VOLUME's page 2), kept on the card; switching the input picks its gain. VOLUME works on
  the settings page too: page 1 the output, page 2 the input gain, a press the other page; its
  LED shows the page for 2 s, then the battery again.
- **An input meter** (#152): VOLUME's page 2 LED shows the input as the looper records it, dim
  to green to yellow, red where the loop would clip (its limiter takes it, see Fixed); for a
  second after a turn it shows the gain, blue to red, as before.
- **A scene crossfader by hand** (#66), after the Octatrack's: SHIFT + a scene key, and
  while you still hold SHIFT, turn the transport knob: the morph is in your hand, the scene
  you're in on the left, the one you picked on the right, back and forth as often as you
  like (24 detents from end to end), with or without a loop. Let go all the way right and
  you're in the new scene; all the way left and you're back where you were; in between it
  stays there, as stopping a morph leaves it. The two scene keys light in their mix, the
  transport LEDs show where it is. Over MIDI, CC 80 does the same for any running morph.
  See [Crossfading by hand](MANUAL.md#crossfading-by-hand).
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
  See [Phaser](MANUAL.md#phaser).
- **The chaos key** on the 14th white key (#65), after the Red Panda Tensor's RAND and the
  Chase Bliss Blooper's Scrambler: an effect slot like any other (hold, latch, scenes) that
  makes no sound of its own. Knob 1 lets the effects you've latched drop out and come back on
  random steps (their keys dim while they're out); knob 2 plays random steps of the loop from
  elsewhere in it, while the loop itself runs on; knob 3 sets the grid, 1/16 to a bar, on the
  effects' tempo; knob 4 turns it from every step rolled anew to one bar repeating as a
  pattern. Chance decides only when your effects act, never how they sound. Over MIDI it's
  channel 14, its latch CC 115 on the global channel. See [Chaos key](MANUAL.md#chaos-key).
- **A second page for the FX knobs:** press any of knobs 1-4 and all four turn over to the
  selected effect's page 2, and its number blinks on their LEDs, twice (#117); press again for
  page 1, which blinks once, as VOLUME's pages do. On page 2, knob 4's LED is a VU meter of the
  effect's output, in VOLUME's colours and as bright as its Level, so you see the page whenever
  the effect sounds; on the delay and reverb it shows their return. Every effect but the chaos
  key has one. Pressing or selecting another effect, or the compressor, brings page 1 back;
  pressing the key of the one already on the knobs (an effect's or the compressor's) keeps page
  2. Scenes, recalls and morphs carry page 2 along; scene files from v0.11 load with page 2 on
  its defaults. Over MIDI, page 2 is CC 18-21 on the effect's channel. See [Page 2](MANUAL.md#page-2).
- **Mix, Band and Level on every effect's page 2,** on the same knobs everywhere (#40):
  knob 1 Mix (the dry sound against the effect's: parallel distortion, harmonies under the
  shifter), knob 3 Band (the effect only on the lows or only on the highs, the rest passing
  dry; on the delay and reverb a low or high cut on what goes in) and knob 4 Level (the
  effect's output, off to +12 dB). The flanger has Band and Level; on the delay and reverb,
  knob 1 is Freeze. At the defaults every effect sounds as before. See
  [Page 2](MANUAL.md#page-2).
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
- **Freeze and ducking on the delay and reverb** (page 2's knob 1, page 1's knob 4): freeze
  holds the echoes or the room, by degrees; ducking turns them down while you play.
- **Depth on the tape stop's knob 4:** from a full stop (as before) to slowing down to half
  speed.
- **A page 2 for the master compressor:** Mix, a sidechain highpass (off, or 20 to 500 Hz,
  so the bass doesn't pump the rest) and Makeup (0 to +24 dB), on the knobs an effect has
  its Mix, Band and Level on (#38).
- **A setting the card can't take shows on the settings page** (#81): when a change there
  (channel, transport following, mono, clock factor, clock source, MIDI out, brightness, the
  input's) can't be written, because there's no card or a full one, the key you changed blinks
  red 3 times, and again with each of the 3 retries. Before, only the compressor's key on the
  play page blinked.
- **The safety limiter shows:** the compressor's key turns red while it limits the outputs,
  so you know when to turn something down.
- **Clock source** on the settings page (A# of the lower octave): Auto (the default, as
  before: whichever input ticks first), TRS, USB, or internal (no MIDI clock: the loop, taps
  or the last tempo, and tap tempo works while a clock runs). On TRS or USB, MIDI Start, Stop,
  Continue and Song Position count only from that input too. Saved on the card; `firmware/remote.py source` sets it.
  See [Clock source](MANUAL.md#clock-source).
- **The beat on the settings page:** the clock factor's key (G# of the lower octave) is lit
  for the first half of every beat of the tempo the effects follow, with the factor applied,
  and dim for the second.
- **MIDI out** on the settings page (C# of the upper octave): off (the default), TRS, or TRS
  and USB. FRIZZ sends its clock, always running while it's on, at the tempo it thinks in: the
  loop's, a MIDI clock coming in (passed on with the clock factor), or the last tempo. A new
  loop sends Start, a pause Stop, PLAY Song Position and Continue, so a drum machine follows
  the loop; without a loop, a DAW's Start and Stop are passed on. Nothing goes back where a
  clock comes from. Saved on the card; `firmware/remote.py out` sets it. See [MIDI
  out](MANUAL.md#midi-out) (#60).

- **MIDI channels per effect** (#97): channels 1-15 are the white keys 1-15 (14 the chaos
  key, 15 the master compressor), the same CCs on each: 14-17 page 1's knobs, 18-21 page 2's,
  22 the latch. In a DAW, one MIDI track per effect with the same automation lanes. They count
  over USB always, and over TRS unless the settings page's new D# (lower octave) switches them
  off for a shared TRS chain. The global channel (16 at first) has the rest. See [Channels](MANUAL.md#channels) and "Controllers".
- **On the global channel** (#97): CC 14-22 reach the effect in focus, the one the knobs edit,
  and CC 23 picks it (1-15, by key); CC 82 taps the tempo; CC 90 sets the loop's speed
  outright, in semitones (64 as recorded); CC 102-115 latch the effects by key.
- **The beat on PLAY** (#110): the play page's PLAY LED shows the beat the effects follow, as
  the settings page's G# does: dim white for the first half of each beat without a loop or
  while recording, a short flash on each beat while a loop plays (stronger on each bar's 1),
  and pulsing over the dim position while it's paused. LOOP's red blinks stay as they were.
  See [Looper](MANUAL.md#looper).
- **MIDI feedback** (#104): with the settings page's new upper D# on, what you change on the
  panel goes out as the CCs that would set it, wherever MIDI out goes. An effect's knobs and
  latch go on its channel, and for the effect in focus on the global channel too. The latches
  by key, the focus, the compressor, the gains and the loop's speed go out, and the crossfader
  while you hold it. A recalled scene sends what it changed, a morph where it lands. Nothing
  that came in by MIDI goes back, a value goes at most every 10 ms, and the clock keeps its
  timing. Off at first. See [MIDI feedback](MANUAL.md#midi-feedback).
- **The loop follows a DAW's Start and Song Position** (#97), with transport following on: a
  loop recorded to the clock goes back to its start on Start and plays from the next tick, and
  Song Position while stopped moves it there (by its length in 16ths), so it stays lined up
  with the song; Continue plays on from the next tick. A free loop plays on as before. With
  MIDI out on, the jump back sends Start, so the gear behind starts over with it. See
  [Start and Stop](MANUAL.md#start-and-stop).

### Changed

- **The manual, reordered** (#220): from the panel and diagrams of the sound's way through
  FRIZZ, its input, its effects and its clock, through the looper and the tempo, to a chapter
  per effect, a page of every [key combination](MANUAL.md#key-combinations), and the MIDI
  reference and technical data as appendices. On the website, the tempo's and MIDI's pages
  moved to `/manual/tempo-and-transport/` and `/manual/appendix-midi/`.
- **FRIZZ's own start-up, and a faster one** (#195): instead of the glow and the rainbow, a
  light-blue ripple spreads from the CHOMPI key across the panel, like a stone dropped into
  water, and runs out in half a second; then the play page, sound and keys at once. FRIZZ is
  ready to play about a second after it starts, where the sound came after about 1.75 s and
  the keys after about 6 s, and it answers a computer over USB 0.75 s sooner. Keys pressed or
  held while it starts still do nothing.

- **The scene gestures are one gesture** (#175), changed since v1.0.0-beta.1: SHIFT + a scene
  key, tapped once per bar, then with SHIFT still held: nothing (let go) is a morph, a **press
  of the transport knob** a drop, a turn the crossfader, a second scene key a build-up + drop.
  The bars count from letting go of SHIFT, for a drop too (in beta.1 from PLAY). Whatever
  comes first holds for the SHIFT hold; the crossfader takes over only after 2 detents one way,
  150 ms without a press. One sequence at a time: SHIFT + a blinking scene key in a later hold
  stops it where it is (it was SHIFT + PLAY), SHIFT + another scene key stops it and starts a
  new one from there (it was refused), and a later hold no longer adds bars. **PLAY is out of
  the scene gestures:** SHIFT + PLAY plays and pauses. MIDI follows: CC 3 or CC 83 during a
  sequence stops it and starts the new one (CC 83 no longer joins a CC 3 morph as its drop),
  CC 81 stops it. A control change the user let in before 1.0 as an exception to the feature
  freeze. See [Timed scene changes](MANUAL.md#timed-scene-changes).
- **The transport knob turns on through a standstill into reverse** (#174), as TAPE's does:
  below 1/16× a hold zone of about 4 detents, where the tape glides to a stop (0×) and the
  loop fades out with it; turned on, it plays in reverse from −1/16× up to −2×, and the same
  way back. SHIFT + turn (the ladder) is unchanged, and from a standstill goes to 1/16× the
  way you turn. At a standstill the effects' beat runs on as while paused, at 50 BPM, so a
  scene morph still lands on its bar line; turned out of it, the beat snaps back onto the
  loop. Both transport LEDs are red there. MIDI out treats it as a pause: Stop going in, Song
  Position and Continue coming out. A control change the user let in before 1.0 as an
  exception to the feature freeze. See [Looper](MANUAL.md#looper).
- **The delay's echoes stay on the beat** (#167): its stereo spread (the left echoes 20 ms
  late, the right 10 ms, from TEMPO) went round the feedback too, so every repeat came that
  much later than the one before: at 1/4 and 120 BPM the 5th left echo was 100 ms off the beat.
  Now every echo is just the spread late, and the repeats stay on the beats as the
  [manual](MANUAL.md#delay) always said. The delay sounds tighter, its long tails no longer smear.
- **The scene keys moved two dark keys to the right** (#67): the blank scene is now on F# of
  the lower octave, scenes 1-4 on the lower G# and A# and the upper C# and D#. The lower C#
  and D# do nothing on the play page for now; they're kept free for pages to come. Your
  scenes stay as they were: the same slots on the card, the same program changes (0-4) and
  CCs. **Save, copy and delete are reordered and recoloured:** the upper F# saves (green), G#
  copies (blue), A# deletes (red), where A# saved (blue), G# copied (green) and F# deleted
  (amber), as on TAPE and TEMPO. Notes follow the keys: a controller or DAW mapped to the
  scene keys' notes now finds them on 54 (the blank scene), 56, 58, 61 and 63, save on 66 and
  delete on 70 (copy stays on 68). See [FX scenes](MANUAL.md#fx-scenes).
- **Heavy effect combinations take less of the CPU** (#136): the effects and the master
  compressor work through each block of 24 samples one effect at a time instead of all of
  them sample by sample. They sound the same as before, but many effects at once leave more
  room, so crackling under a heavy combination (#21) sets in later. Page 2 off its defaults
  (Mix, Band, Level) on many effects at once costs less too (#142).
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
  shift …) still switch. See [Morphing to a scene](MANUAL.md#morphing-to-a-scene).
- **Stereo moved from knob 4 to page 2's knob 2** (#117) on the freezer, shifter, flanger,
  resonator, slicer and wow & flutter, where the phaser and the folder's, crusher's and
  filter's new stereo are too: knob 4 is the effect's own setting now, the one you play (see
  Added). Scenes saved before load with every value where its knob went; over MIDI, CC 17 on an
  effect's channel is that setting, CC 19 stereo.
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
  [Clock factor](MANUAL.md#clock-factor).
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

  Program changes, All Notes Off and FRIZZ's SysEx stay as they were, and the notes still
  press the keys from 48 up, so what moved to another key has another note now: the scene
  keys, save and delete, and the resonator to the tape stop (above). A bug report from v0.11
  replays its CCs by the new map.
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
  octave C# MIDI out (was the upper D#), F# the input's source and SHIFT + F# mono (was the
  lower F#), G# and A# the input's monitoring and effects (#177, #243), and the upper D# MIDI feedback
  (new). The LEDs' brightness (was the lower A#) is on knob 1 now (#176). See [Settings page](MANUAL.md#settings-page).

- **The resonator, slicer, wow & flutter and tape stop are one key to the right** (#68): the
  8th, 9th, 10th and 11th white keys, making room for the phaser on the 7th, next to the
  flanger. MIDI notes press the keys, so the notes for them move with the keys, as do their
  MIDI channels (the new MIDI map, above).
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
  quieter until you set its makeup. Over MIDI: CC 24-31 (the new MIDI map, above). The
  safety limiter after VOLUME stays: it is the old one-knob compressor at its lowest setting,
  which was its default.
- **Transport knob in semitones:** while a loop plays, turning it changes the speed a
  semitone every 2 detents, from 2× down to 1/16×, then on through a standstill into reverse
  up to −2× (#174, above), and stops at both ends. SHIFT + turn jumps in fifths and octaves as
  turning did before (from between two of them, to the next one in the turn's direction).
  Over MIDI, CC 90 sets it outright, in semitones. See [Looper](MANUAL.md#looper).
- **Bug reports** moved to the settings page: with the mode switch up, hold SHIFT (CHOMPI)
  and the VOLUME knob pressed for 2 seconds. SHIFT + press the transport knob on the play page
  does nothing now, so a report can't be written by accident while playing. See [Bug reports](MANUAL.md#bug-reports).
- **FRIZZ starts up a quarter of a second faster:** it no longer clears the loop's 32 MB of
  memory at power-on, which a loop never plays before it has recorded it.
- **The delay's and reverb's Level is on page 2's knob 4, as every effect's** (#172), on the
  same scale: off at the bottom, then -36 dB to +12 dB, and 0 dB at first, so the delay and
  reverb are heard from the first press of their key. Ducking moves to page 1's knob 4, where
  the level was. Knob 4's LED on page 2 still shows their return, now as bright as Level
  turns it. Saved scenes come back sounding as saved: their level moves to page 2 at the same
  loudness in dB (below -36 dB off), their ducking to page 1. **Over MIDI**, the CCs follow
  the knobs: on the delay's and reverb's channels (12 and 13) the level is now **CC 21**, as on
  every effect, and CC 17 is ducking; automation written for CC 17 turns ducking now.
- **The freezer's roll is played by hand** (#173): knob 3 divides the length, off, 1/2, 1/4,
  1/8 and 1/16 of it (no shorter than 1/64 bar), each step taking at the start of the next
  repeat so the repeats stay on the grid. It no longer halves the loop by itself after a
  number of repeats: the knob is the roll, and back to off is the whole length again in one
  move. A capture with the roll still on, say, 1/4 repeats a quarter of the length from the
  start. Saved scenes keep the roll's step on the knob.

### Fixed

- **FRIZZ's USB names no longer come up garbled** (#88). Now and then (about 1 start in 20)
  the computer listed FRIZZ as "Љ" by "FrizZ" instead of "FrizZ" by "Chompi Club": each name
  one string late, and a DAW looking for FRIZZ by its name didn't find it until it was switched
  off and on. It happened when the audio started just as the computer asked FRIZZ for its
  names; now FRIZZ answers each question afresh.
- **The headphones in phase with the input** (#209). CHOMPI's headphone out came out
  polarity-inverted against what went into AUX, and against the line out; on its own that's
  inaudible, but mixed with the dry signal (FRIZZ as a send effect beside the DAW's dry
  track, or the headphone out and the line out summed) the lows cancelled. Now the
  headphones and the line out are both in phase with AUX. The stock firmwares do the same as
  FRIZZ did; it comes from CHOMPI's analog stages, and FRIZZ turns the headphones back.
- **MIDI out: a new loop's Start goes with its first tick** (#198), so the gear behind plays
  beat 1 with the loop. When the clock going out had just ticked as the loop started, that
  tick was taken for the loop's first, and the next came a tick (about 20 ms at 120 BPM)
  later, and the gear's beat 1 with it: now and then for a loop on FRIZZ's own clock, every
  time for a quantized loop on a clock passed on to gear that wasn't playing yet. Over USB the
  same.
- **Tap tempo and LOOP's erase lock go by when you pressed LOOP** (#169), not by when FRIZZ got
  round to it: a tap was up to 2 ms off, and while the card was being written (a scene saved)
  tens of ms, so the tapped tempo came out a little off. MIDI taps already were.
- **The compressor key no longer flashes red after the settings page** (#169) from limiting
  that happened while the page was up; it turns red only while the limiter still works.
- **A bug report is no longer cut short by a save** (#169): after a save that had failed, the
  next one (a scene, or the settings 2 s after a change) mounted the card again while a report
  was being written, and the report failed. That save now waits until the report is written.
- **A SysEx cut short no longer deafens MIDI in** (#166): one that broke off (its sender
  unplugged mid-message) swallowed every note, CC, program change and Song Position after it
  until the next SysEx ended, so a note held then hung. Now the next message ends it, as MIDI
  has it; the clock was never affected.
- **MIDI out: a new loop's Start reaches every port whose gear didn't get one** (#166): with
  a Start coming in on the TRS jack and passed on only over USB, a loop recorded then sent the
  jack's gear no Start, as if it already played. Now FRIZZ keeps track port by port.
- **A bug report keeps CC 3 and CC 83 past the scenes** (#166): sent between two morphs to the
  same scene, it lets the second go, but the report left it out, so its replay didn't morph.
- **MIDI out over USB keeps the transport while FRIZZ writes the card** (#166): a scene saved
  while the loop paused or played on could hold USB up long enough that its Stop, or its Song
  Position and Continue, were dropped with the ticks. Now ticks are dropped first.
- **No click when the shifter's Grain jumps** (#167): a scene recall, MIDI or a fast turn of
  Grain while the shifter plays could let one of its two voices play unshifted for up to
  20 ms, a click (worst with a long grain after a short one, shifting up). The new grain now
  comes in as the voices start over, within a few of their lives (a split second).
- **The shifter keeps shifting when its shift jumps from down to up** (#221), since
  v1.0.0-beta.2: a jump while it plays (−7 to +7, by MIDI, a scene or a fast turn) could leave
  it playing its input at its own pitch, one channel or both, until it went off. It depended
  on the moment, so it came now and then. Going the other way, from up to down, its voices
  now also stay within the Grain's longest window.
- **The shifter at stereo 0 is mono on a mono source** (#223): its left and right channel
  chose where to splice each on their own, and the slightest difference between CHOMPI's two
  input channels set them apart, so they read from different places. Summed to mono, that
  was a moving comb filter, a hollow sound. Now the right channel follows the left's choice
  while stereo is at 0, and both play the same; after stereo was turned up and back to 0, from
  the shifter's next punch-in, or once the shift has been at the centre.
- **The crusher's dive after a quick tap** (#167): a tap shorter than the dive's 0.1 s fall,
  then a pause, and the next press started the dive from too high a rate, as if the first
  fall had been cut off where the tap ended. It now picks up where it would have got to.
- **The resonator's env mod after a pause** (#167): punched in again after it had gone quiet,
  with Env Mod up, it rang at the feedback the last loud passage had pushed it to for about
  0.1 s. It now starts from what it hears.
- **No click in the echoes when the delay's damping comes on at once** (#167): a scene recall
  or MIDI turning Damping from the centre to one side could click in the feedback, the filter
  starting from the sound it had when it was last on. It now starts from the echoes as they
  are.
- **With the mix at the loop alone, none of the input is left** (#168): a trace of it, about
  86 dB down, stayed on the master; the headphone feed likewise settles exactly on the master
  or the input now.
- **A settings file with a word where a number belongs is kept, not read as 0** (#168): a
  garbled `gain_aux` silenced AUX and was saved back. Now `frizz_master.txt` is set aside as
  `frizz_master.bak` on the next save, and the settings start from their defaults.
- **A hot input no longer distorts in the loop** (#152). Live it sounded clean, but the loop is
  stored at 16 bits and clipped hard: at the default input gain from about 7 dB below the
  input's full scale (measured: 1 kHz at full scale, the loop 26 % distortion, live under 1 %). Now
  a limiter holds what the looper records just under full scale instead; below that the loop
  is what came in, as before. Both inputs, AUX and the mic.
- **MIDI out over USB sent the Song Position twice** (#157): after a pause, PLAY sent it two
  times over USB (once out of TRS, as it should), so a DAW following FRIZZ got it twice. Now
  once.
- **The pitch shifter's first moment no longer slips through unshifted** (#140): turning
  the shift up out of the centre the first time after power-on (or punching the shifter in
  with the shift already up), or turning it the other way than it last went while it was
  off or at the centre, could play a split second of the unshifted sound under the shifted
  voice, or a stuck one. The shifter now starts its two voices afresh in those cases;
  otherwise it carries on as before.

- **The low-battery countdown shows VOLUME red** (#43): in the 15 s before the CHOMPI
  switches itself off on an empty battery, VOLUME's LED flashes red while the others flash
  yellow, so the battery's red ([Settings page](MANUAL.md#settings-page)) is seen exactly when it matters.
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
- **A long-held tape stop with Depth below the top:** held for more than about 12 s (Depth
  at 0.8) to 22 s (at 0), the slowed tape suddenly played sound from 11 s before at full
  speed. It now jumps back to the live sound in a short crossfade and plays on slowed.
- **A MIDI clock that sends its ticks two at a time** (some DAWs over USB) no longer locks
  FRIZZ to half its tempo when it starts while FRIZZ is already running (#92).
- **A scene file from a newer FRIZZ** (or one whose layout line was edited into something
  that isn't a number) is no longer read as if it were this version's, with knobs in the
  wrong places, and then overwritten by the next save (#168). It's kept: the next save
  moves it to `frizz_scenes.bak`, as for any scene file FRIZZ can't read.

## v0.11 (2026-10-10)

### Added

- **MIDI control** over TRS or USB, on channel 16 (set in `FRIZZ/frizz_master.txt` or with
  `firmware/remote.py`): notes from 48 up play the keys as a keyboard, with CHOMPI, PLAY,
  LOOP and the knob presses below; CCs latch the effects (20-31), set every effect's knobs
  (70-117, in 14 bits over NRPN), the compressor, the gains, the mix and mono (52-60), turn
  the knobs relatively (14-19) and morph to a scene (61-63); program changes 0-4 are the scene
  keys. MIDI Start and Stop can play and pause the loop (off at first). See [MIDI](MANUAL.md#appendix-midi).
- **Remote control over USB:** FRIZZ's SysEx presses keys, turns knobs, and answers the
  state, the LEDs, the processing load and the scenes; `firmware/remote.py` uses it, and plays
  the virtual CHOMPI's scenarios on the device.
- **Settings page** on the mode switch: up, the keys set the MIDI channel (the white keys,
  the last one switching between 15 and 16, or every channel), MIDI transport following, the mono input, a clock factor (follow a
  MIDI clock at half, as sent, or double its tempo) and the LEDs' brightness (100, 75 or
  50 %), each saved on the card; VOLUME's LED shows the battery's level throughout. The loop,
  the effects and MIDI play on while it's up; switch down to play. See [Settings page](MANUAL.md#settings-page).
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

- FRIZZ keeps its files in a folder `FRIZZ` on the card, which it creates at its first start;
  a `frizz_scenes.txt` an earlier build left in the card's top folder is moved into it.
