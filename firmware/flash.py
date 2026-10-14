#!/usr/bin/env python3
"""flash.py: puts FRIZZ on the CHOMPI over USB, through the multi-firmware launcher.

    ./flash.py --test        builds FRIZZ.bin from the working tree, sends it to slot 12, the
                             test slot, as FRIZZ-TEST: a branch's build, checked by a session
    ./flash.py --bench       builds FRIZZ-bench.bin, sends it to slot 11
    ./flash.py --no-build    sends the build that's there (code/src/build/, build-bench/) instead
    ./flash.py --slot 10|13  a release's (10) or a beta's (13) build, from its v* tag: below
    ./flash.py FILE --slot N sends any .bin to slot N (10 and 13: only the tag's build)
    ./flash.py --run N       starts what's in slot N already (10 FRIZZ, 11 the bench, 12 a test,
                             13 the beta)
    ./flash.py --list        what's on each key: restarts FRIZZ into the launcher (a loop in
                             RAM is lost) and leaves the CHOMPI at its picker

Keys 10 and 13 are the user's: 10 the latest release, 13 the latest beta (FRIZZ-BETA). A build
goes there only when the key is named (--slot 10, --slot 13), and only a release's or a beta's:
this checkout stands on a v* tag, nothing changed in code/, the build is the one make makes
there (a FILE too: the same bytes), and its kFrizzVersion is the tag's (code/src/Version.h, what
the loop transfer's HELLO says: tools/builds.py). So a release goes on from a worktree of its tag
(git worktree add ../FrizZ-vX.Y.Z vX.Y.Z, CLAUDE.md "Releases"). A bare ./flash.py sends
nothing: say which slot. The bench never goes to 10 or 13.

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
FRIZZ_SLOT, BETA_SLOT, BENCH_SLOT and TEST_SLOT set other slots; without --slot, a key that holds another
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
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "tools"))
import builds  # noqa: E402
import chompi  # noqa: E402
import midi_send  # noqa: E402


def tagged(where=HERE):
    """The v* tag the checkout at WHERE stands on, with nothing changed in its code/ (nor a file
    added there); None if it doesn't"""
    def git(*args):
        return subprocess.run(["git", "-C", where] + list(args), capture_output=True,
                              text=True).stdout.strip()
    tag = git("describe", "--exact-match", "--tags", "--match", "v*", "HEAD")
    return tag if tag and not git("status", "--porcelain", "--", "code") else None


def user_slot(slot, args):
    """Exits unless what's asked may go to the user's key SLOT (10, 13): a release's or a beta's
    build, from a checkout of its tag; the tag"""
    what = "the latest release" if slot == chompi.FRIZZ_SLOT else "the latest beta"
    if args.bench:
        sys.exit("key %d is %s, the user's: never the bench" % (slot, what))
    tag = tagged()
    if not tag:
        sys.exit("key %d is %s, the user's: only a release's or a beta's build goes there, made "
                 "in a checkout of its v* tag with nothing changed in code/ (git worktree add "
                 "../FrizZ-vX.Y.Z vX.Y.Z): this one isn't. A branch's build goes to the test "
                 "slot: --test" % (slot, what))
    builds.check_version(tag)
    return tag


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image", nargs="?", help="a .bin to send instead of building one")
    ap.add_argument("--bench", action="store_true", help="the CPU bench (FRIZZ-bench.bin)")
    ap.add_argument("--test", action="store_true",
                    help="to the test slot (12), as FRIZZ-TEST: a branch's build, checked by a session")
    ap.add_argument("--no-build", action="store_true", help="send the build make left in code/src")
    ap.add_argument("--slot", type=int,
                    help="the launcher slot (key) to put it in; 10 and 13, the user's, take only "
                    "a v* tag's build")
    ap.add_argument("--run", type=int, metavar="SLOT", help="start what's in SLOT, sending nothing")
    ap.add_argument("--list", action="store_true",
                    help="show what's on each key, sending nothing: restarts FRIZZ into the "
                    "launcher (a loop in RAM is lost), the CHOMPI left at its picker")
    ap.add_argument("--name", help="its name on the card (default FRIZZ, FRIZZ-BETA, FRIZZ-TEST "
                    "or FRIZZ-BENCH)")
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
    if args.image and not args.slot:
        sys.exit("a file of your own needs --slot: sending replaces what's in it")
    slot = args.slot or (chompi.BENCH_SLOT if args.bench else chompi.TEST_SLOT if args.test else None)
    if slot is None:
        sys.exit("which slot? --test (12) for a branch's build, --bench (11), or --slot N; keys "
                 "10 and 13 take a release's or a beta's build, from its v* tag")
    # the user's keys: refused before anything is built or the CHOMPI restarted
    tag = user_slot(slot, args) if slot in chompi.USER_SLOTS else None
    if args.image:
        image = args.image
    elif args.no_build:
        image = builds.built(args.bench)
    else:
        image = builds.build(args.bench)
    if not os.path.isfile(image):
        sys.exit("%s: no such file" % image)
    if tag:
        made = builds.image(False)
        if not os.path.isfile(made) or builds.stale(made) or builds.md5(made) != builds.md5(image):
            sys.exit("%s isn't the build of %s made here (make in code/src): not sent to key %d"
                     % (image, tag, slot))
    name = args.name or ("FRIZZ-BENCH" if args.bench or slot == chompi.BENCH_SLOT
                         else "FRIZZ-TEST" if slot == chompi.TEST_SLOT
                         else "FRIZZ-BETA" if slot == chompi.BETA_SLOT else "FRIZZ")

    md5 = builds.md5(image)
    print("sending %s to key %d%s" % (builds.describe(image), slot, ", %s's" % tag if tag else ""))

    device = chompi.to_launcher(args.wait, args.device)
    if not args.slot:
        # the default keys are FRIZZ's: never replace another firmware on them unasked
        there = chompi.slot_file(device, slot)
        if there and not there.upper().split("_", 1)[-1].startswith("FRIZZ"):
            sys.exit("key %d holds %s, not FRIZZ: give --slot %d to replace it anyway, "
                     "or set BENCH_SLOT / TEST_SLOT" % (slot, there, slot))
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
