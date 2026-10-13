# Installing FRIZZ

<!-- summary: Put FRIZZ on your CHOMPI, update it, and go back to stock firmware. -->

FRIZZ installs from the SD card, the same way as any CHOMPI firmware update. You don't need any
tools or drivers, and the CHOMPI's bootloader is never touched.

## What you need

- A CHOMPI. It doesn't matter which firmware it runs now (TAPE, TEMPO or WAVE).
- Its microSD card, or any FAT32-formatted microSD card.
- A computer with a card reader.
- `FRIZZ.bin`. Download it from the [Releases page](https://github.com/ZUHcYa/FrizZ/releases),
  or build it yourself as described in [`firmware/README.md`](firmware/README.md).

## Install

1. **Back up the card.** Copy everything on it to your computer: your samples, `presets.json`
   and `options.json`. FRIZZ doesn't touch them, but you'll want them if you go back.
2. **Remove the old firmware.** Delete every `.bin` file in the top folder of the card, for
   example `CHOMPI_TAPEv2_0.bin`. There must be exactly one `.bin` on the card. Everything else
   can stay.
3. **Copy `FRIZZ.bin`** to the top folder of the card, not into a subfolder.
4. **Eject the card** properly, put it in the CHOMPI and switch it on.
5. **Wait.** A slow rainbow LED pattern means the CHOMPI is writing FRIZZ into its memory.
   Don't switch it off while that runs. FRIZZ starts by itself when it's done: a light-blue
   ripple spreads from the CHOMPI key, then the FX keys glow in their colours.

Leave the card in: FRIZZ keeps your scenes and settings on it, in a folder `FRIZZ`
([Card and files](MANUAL.md#card-and-files)). Without a card FRIZZ runs, but nothing you save
lasts past power-off.

## Check that it works

1. Make sure the mode switch is **down** (the play page). Plug a stereo source into the
   **AUX input**, and headphones or a mixer into the outputs. (Without a cable in AUX, FRIZZ
   listens to the built-in microphone, but doesn't put it on the outputs at first, so a
   speaker can't feed it back: see [Input: AUX or the mic](MANUAL.md#input-aux-or-the-mic).)
2. Turn the **VOLUME** knob. You should hear your source, and the VOLUME LED moves with the level.

If you hear nothing, press VOLUME once and turn it to raise the input gain. Press it twice more
to get back to output volume. The [Quick start](QUICKSTART.md) takes it from here.

## Updating FRIZZ

Same as installing: replace `FRIZZ.bin` on the card with the new one and switch on. Your
scenes and settings stay, as long as the `FRIZZ` folder stays on the card.

## With the multi-firmware launcher

The [multi-firmware launcher](https://github.com/sfaber02/CHOMPI-MULTI-FIRMWARE/releases)
(a community project by hiwatts, chomplex music theory and lnetzel) keeps several firmwares on
one card: at power-on each white key with a firmware lights up, and pressing one starts it. FRIZZ
works as one of them, next to TAPE, TEMPO and WAVE.

1. **Set up the card with the launcher**, as its
   [v1.5 release](https://github.com/sfaber02/CHOMPI-MULTI-FIRMWARE/releases/tag/launcher-v1.5)
   describes: copy the contents of `chompi-multi-firmware-card/` to an empty FAT32 card and
   switch on (a rainbow once, then the picker). On a card with an older launcher (v1.1 or
   later), the setup page below updates it.
2. **Put FRIZZ on a key with the [setup page](https://ugrossek.github.io/CHOMPI/setup/).**
   With the picker showing, connect the CHOMPI to a computer over USB and open the page in
   Chrome or Edge. Pick FRIZZ for a free key; the page fetches the latest release and writes
   it to the card.

   **Or by hand:** copy `FRIZZ.bin` into the card's `FIRMWARE` folder, named after the key you
   want it on, for example `FIRMWARE/04_FRIZZ.bin` for white key 4.
3. Copy your `FRIZZ` folder over too, if you have scenes from another card. Otherwise FRIZZ
   creates it on its first start and keeps its scenes there, apart from the other firmwares'
   files.

With the launcher, the CHOMPI goes back to its picker every time you switch it on. To update
FRIZZ, pick it again on the setup page, or copy the new `FRIZZ.bin` over the old
`FIRMWARE/04_FRIZZ.bin`; no rainbow pattern, since the launcher stays installed. The setup
page offers releases only: a test build (a pre-release on the Releases page) goes on by hand.

### USB storage mode

The launcher's white key 15 is a USB storage mode: connect the CHOMPI to a computer over USB
and the card shows up there as a drive named `CHOMPI-SD`, so you can copy files (bug reports,
scenes) without taking the card out. To leave, eject the drive on the computer first. With
[USB storage v1.6](https://github.com/lnetzel/CHOMPI-lnetzel/releases/tag/usb-storage-v1.6),
the CHOMPI then restarts into the launcher's picker by itself. With an older one (a card set
up from the launcher's v1.5 release has v1.5), press the LOOP key, which glows red: it blinks
and the CHOMPI key turns red; tap the CHOMPI key to confirm, and the CHOMPI restarts into the
picker. Pressing the blinking LOOP again cancels.

## Going back to stock firmware

1. Delete `FRIZZ.bin` from the card.
2. Copy back your backup, or a factory card from
   [`reference/firmware/card-profiles/`](reference/firmware/card-profiles/) (also in
   [CHOMPI Club's repo](https://github.com/CHOMPI-Club/CHOMPI/tree/main/firmware/card-profiles)): `tape-2.0/`,
   `tempo-1.0/` or `wave-1.0/`. Copy the folder's *contents* to the top folder of the card.
   Each one includes its firmware `.bin`.
3. Switch on and wait for the rainbow pattern to finish.

## Troubleshooting

| Problem | Fix |
|---|---|
| No rainbow, the old firmware starts | Check that `FRIZZ.bin` is in the top folder, that it's the only `.bin` there, and that the card is FAT32 |
| Rainbow, but then nothing works | Copy `FRIZZ.bin` to the card again (the copy may be damaged) and switch on again |
| The keys don't play | The mode switch is up, on the settings page: flip it down |
| Silence | Check the input gain (press VOLUME, then turn) and the input/loop mix (SHIFT + turn VOLUME, turn towards green). Is the cable in AUX all the way? Without one, the input is the mic, which isn't live at first. On the settings page (mode switch up), the upper G# puts the input you hear live (lit) |
| The CHOMPI doesn't respond at all | The bootloader may be damaged. Reinstall CHOMPI's own bootloader (`firmware/bin/CHOMPI_Bootloader_V6_2_0.bin`) as described in [`firmware/README.md`](firmware/README.md#4-put-it-on-the-chompi), then install FRIZZ again. Don't install the generic Daisy bootloader instead |
