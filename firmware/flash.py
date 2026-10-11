#!/usr/bin/env python3
"""flash.py: puts FRIZZ on the CHOMPI over USB, through the multi-firmware launcher.

    ./flash.py               builds FRIZZ.bin from the working tree, sends it to slot 10
    ./flash.py --bench       builds FRIZZ-bench.bin, sends it to slot 11
    ./flash.py --test        a branch's build for a session's own checks: to slot 12
    ./flash.py --no-build    sends the build that's there (code/src/build/, build-bench/) instead
    ./flash.py FILE --slot N sends any .bin to slot N
    ./flash.py --run N       starts what's in slot N already (10 FRIZZ, 11 the bench, 12 a test)
    ./flash.py --list        what's on each key (the CHOMPI stays at the launcher's picker)

The launcher (github.com/sfaber02/CHOMPI-MULTI-FIRMWARE, firmware/chompi-launcher, its
releases) is CHOMPI.bin in the card's root, and keeps the firmwares in /FIRMWARE/NN_NAME.bin,
NN being the key that starts it. While its picker shows, it takes a firmware over USB MIDI,
writes it to its slot, replacing whatever is there, and starts it (tools/midi_send.py, the
launcher's own client). The CHOMPI is first brought to the launcher from wherever it is: a running
FRIZZ restarts into it over USB MIDI (MidiClock.h), the USB storage firmware on an eject
(tools/chompi.py); a FRIZZ from before that, or another firmware, needs the power switch, and
this says so. It then sends, and FRIZZ starts; it waits until FRIZZ (or the bench) answers, so a tool run
right after finds it, and notes the slot and the build sent there, so that tool knows which it
is (tools/chompi.py). It then asks FRIZZ which build it runs (IDENT: the image's CRC-32 and
length) and fails if that isn't the one sent; --run says which build it started.
One that came up with its USB names garbled (#88) is started again, not sent again.
FRIZZ_SLOT, BENCH_SLOT and TEST_SLOT set other slots; without --slot, a key that holds another
firmware than FRIZZ is refused (launcher 1.5 lists its keys; an older one can't, and isn't
asked). It waits while another tool has the CHOMPI (tools/chompi.py, the lock).

It prints the md5 of what it sends: that names the build (tools/builds.py). Builds aren't in
git, so --no-build needs one made first (make, make BENCH=1 in code/src), and refuses one older
than the source.

Linux only (ALSA's raw MIDI), Python 3 without packages; building needs the ARM toolchain
(README.md).
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "tools"))
import builds  # noqa: E402
import chompi  # noqa: E402
import midi_send  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image", nargs="?", help="a .bin to send instead of building one")
    ap.add_argument("--bench", action="store_true", help="the CPU bench (FRIZZ-bench.bin)")
    ap.add_argument("--test", action="store_true",
                    help="to the test slot (12), as FRIZZ-TEST: a branch's build, checked by a session")
    ap.add_argument("--no-build", action="store_true", help="send the build make left in code/src")
    ap.add_argument("--slot", type=int, help="the launcher slot (key) to put it in")
    ap.add_argument("--run", type=int, metavar="SLOT", help="start what's in SLOT, sending nothing")
    ap.add_argument("--list", action="store_true", help="show what's on each key, sending nothing")
    ap.add_argument("--name", help="its name on the card (default FRIZZ or FRIZZ-BENCH)")
    ap.add_argument("--wait", type=int, default=120, help="seconds to wait for the launcher")
    ap.add_argument("--device", help="its raw MIDI node, e.g. /dev/snd/midiC1D0 (default: found)")
    args = ap.parse_args()

    if args.run:
        node = chompi.start(args.run, args.wait, args.device)
        info = node and chompi.ident(node)
        if info:
            print("slot %d runs %s" % (args.run, chompi.describe_ident(info)))
        return
    if args.list:
        device = chompi.to_launcher(args.wait, args.device)
        sys.argv = ["midi_send.py", "--list", "--device", device]
        midi_send.main()
        return
    if args.image:
        image = args.image
    elif args.no_build:
        image = builds.built(args.bench)
    else:
        image = builds.build(args.bench)
    if not os.path.isfile(image):
        sys.exit("%s: no such file" % image)
    slot = args.slot or (chompi.BENCH_SLOT if args.bench
                         else chompi.TEST_SLOT if args.test else chompi.FRIZZ_SLOT)
    name = args.name or ("FRIZZ-BENCH" if args.bench else "FRIZZ-TEST" if args.test else "FRIZZ")
    if args.image and not args.slot:
        sys.exit("a file of your own needs --slot: sending replaces what's in it")

    md5 = builds.md5(image)
    print("sending", builds.describe(image), "to key", slot)

    device = chompi.to_launcher(args.wait, args.device)
    if not args.slot:
        # the default keys are FRIZZ's: never replace another firmware on them unasked
        there = chompi.slot_file(device, slot)
        if there and not there.upper().split("_", 1)[-1].startswith("FRIZZ"):
            sys.exit("key %d holds %s, not FRIZZ: give --slot %d to replace it anyway, "
                     "or set FRIZZ_SLOT / BENCH_SLOT / TEST_SLOT" % (slot, there, slot))
    sys.argv = ["midi_send.py", image, "--slot", str(slot), "--name", name, "--device", device]
    midi_send.main()
    chompi.started(slot)
    chompi.noted_image(slot, image, md5)
    if slot in chompi.FRIZZ_SLOTS + (chompi.BENCH_SLOT,) and name.upper().startswith("FRIZZ"):
        # until it answers (the bench does as FRIZZ): the launcher may for a moment, then
        # nothing while USB comes back
        node = chompi.wait_for("frizz", args.wait, args.device)
        node = chompi.renamed(slot, node, args.wait, args.device)  # started again, not sent again
        print("%s answers on slot %d" % (name, slot))
        check_running(node, image)


def check_running(node, image):
    """Exits unless FRIZZ at NODE runs IMAGE, by IDENT; a build without IDENT can't say"""
    info = node and chompi.ident(node)
    if not info:
        print("(it can't say which build it runs: one from before IDENT)")
        return
    crc, length = chompi.image_id(image)
    if (info["crc"], info["length"]) != (crc, length):
        sys.exit("it runs crc32 %08x, %d B, NOT the build sent (crc32 %08x, %d B)"
                 % (info["crc"], info["length"], crc, length))
    print("it runs the build sent: crc32 %08x, %d B" % (crc, length))


if __name__ == "__main__":
    main()
