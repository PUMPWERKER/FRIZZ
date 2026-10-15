#!/usr/bin/env python3
"""flash.py: puts FRIZZ on the CHOMPI over USB, through the multi-firmware launcher.

    ./flash.py --test        builds FRIZZ.bin from the working tree, sends it to slot 12, the
                             test slot, as FRIZZ-TEST: a branch's build, checked by a session
    ./flash.py --bench       builds FRIZZ-bench.bin, sends it to slot 13, as FRIZZ-BENCH
    ./flash.py --no-build    sends the build that's there (code/src/build/, build-bench/) instead
    ./flash.py --slot 10|11  a release's (10) or a beta's (11) build, from its v* tag: below
    ./flash.py FILE --slot N sends any .bin to slot N: 12 or 13 (10 and 11: only the tag's
                             build); another key (a stock firmware, the storage firmware's 15)
                             only with --other-slot, which a session never gives
    ./flash.py --run N       starts what's in slot N already (10 FRIZZ, 11 the beta, 12 a test,
                             13 the bench)
    ./flash.py --list        what's on each key (and its colour, launcher 1.6 on): restarts
                             FRIZZ into the launcher (a loop in RAM is lost) and leaves the
                             CHOMPI at its picker
    --color RRGGBB           with a build sent: the colour its key breathes in at the picker
                             instead of its slot's (launcher 1.6 on; an older one sends without)

Keys 10 and 11 are the user's: 10 the latest release, 11 the latest beta (FRIZZ-BETA). A build
goes there only when the key is named (--slot 10, --slot 11), and only a release's or a beta's:
this checkout stands on a v* tag, nothing changed in code/, the build is the one make makes
there (a FILE too: the same bytes), and its kFrizzVersion is the tag's (code/src/Version.h, what
the loop transfer's HELLO says: tools/builds.py). So a release goes on from a worktree of its tag
(git worktree add ../FrizZ-vX.Y.Z vX.Y.Z, CLAUDE.md "Releases"). A bare ./flash.py sends
nothing: say which slot. The bench never goes to 10 or 11, and a build for the test slot or the
bench never over the user's FRIZZ or FRIZZ-BETA (a key the launcher lists so).

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
FRIZZ_SLOT, BETA_SLOT, BENCH_SLOT and TEST_SLOT set other slots (FRIZZ_SLOT and BETA_SLOT add
keys to guard as the user's: 10 and 11 stay guarded whatever they say); a key that holds another
firmware than FRIZZ is refused but with --other-slot (launcher 1.5 and later list their keys; an
older one can't, and isn't asked). Every slot is checked before anything is built or the CHOMPI restarted. It waits while another tool has the CHOMPI (tools/chompi.py, the lock).

The launcher 1.6 colours each key: the colour chosen for the file on it (its COLOR, kept on the
card in /FIRMWARE/key_colors.txt with the file's name), or one made from that name. flash.py
gives FRIZZ's keys theirs with every build sent there (KEY_COLORS, decided 2026-10-15, #171):
10 green, the release; 11 orange, the beta; 12 and 13 red, the builds sessions make. --color
chooses another: the build is stored without starting (--stay), the key coloured, then started
(RUN; a colour the card fails to keep is said, the build started all the same). Another key
(--other-slot) gets a colour only from --color; a launcher before 1.6 sends without.

It prints the md5 of what it sends: that names the build (tools/builds.py). Builds aren't in
git, so --no-build needs one made first (make, make BENCH=1 in code/src), and refuses one older
than the source.

Linux only (ALSA's raw MIDI), Python 3 without packages; building needs the ARM toolchain
(README.md).
"""
import argparse
import os
import re
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
    return tag if tag and not git("status", "--porcelain", "-unormal", "--", "code") else None


def user_slot(slot, args):
    """Exits unless what's asked may go to the user's key SLOT (10, 11): a release's or a beta's
    build, from a checkout of its tag; the tag"""
    what = "the latest release" if slot in (10, chompi.FRIZZ_SLOT) else "the latest beta"
    if args.bench:
        sys.exit("key %d is %s, the user's: never the bench" % (slot, what))
    tag = tagged()
    if not tag:
        sys.exit("key %d is %s, the user's: only a release's or a beta's build goes there, made "
                 "in a checkout of its v* tag with nothing changed in code/ (git worktree add "
                 "../FrizZ-vX.Y.Z vX.Y.Z): this one isn't. A branch's build goes to the test "
                 "slot: --test" % (slot, what))
    if slot == 10 and "-" in tag:
        sys.exit("key 10 is the latest release: %s is a pre-release's tag, whose build goes to "
                 "key %d (--slot %d)" % (tag, chompi.BETA_SLOT, chompi.BETA_SLOT))
    builds.check_version(tag)
    return tag


SLOTS = range(1, 16)  # the launcher's keys
# the colour each of FRIZZ's keys breathes in at the launcher 1.6's picker, given with every
# build sent there (decided 2026-10-15, #171): the release green, the beta orange, the builds
# sessions make red
KEY_COLORS = {chompi.FRIZZ_SLOT: "00C000", chompi.BETA_SLOT: "FF7000",
              chompi.TEST_SLOT: "FF0000", chompi.BENCH_SLOT: "FF0000"}
USER_NAMES = ("FRIZZ", "FRIZZ-BETA")  # the user's builds, as their keys' files are named


def check_slot(slot, args):
    """Exits unless SLOT may be sent to as asked: FRIZZ's keys (the user's behind user_slot's
    tag check) and the bench's, another key of the launcher's only with --other-slot"""
    if slot not in SLOTS:
        sys.exit("no key %d: the launcher's are %d-%d" % (slot, SLOTS[0], SLOTS[-1]))
    # never the storage firmware's, whatever BENCH_SLOT or TEST_SLOT say: card.py needs it
    ours = tuple(s for s in chompi.USER_SLOTS + (chompi.BENCH_SLOT, chompi.TEST_SLOT)
                 if s not in (15, chompi.STORAGE_SLOT))
    if slot not in ours and not args.other_slot:
        sys.exit("key %d isn't FRIZZ's or the bench's (%s): sending replaces what's there, so "
                 "--other-slot says it's meant (never in a session's run)"
                 % (slot, ", ".join(str(s) for s in sorted(set(ours)))))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image", nargs="?", help="a .bin to send instead of building one")
    ap.add_argument("--bench", action="store_true", help="the CPU bench (FRIZZ-bench.bin)")
    ap.add_argument("--test", action="store_true",
                    help="to the test slot (12), as FRIZZ-TEST: a branch's build, checked by a session")
    ap.add_argument("--no-build", action="store_true", help="send the build make left in code/src")
    ap.add_argument("--slot", type=int,
                    help="the launcher slot (key) to put it in: 12, 13; 10 and 11, the user's, take "
                    "only a v* tag's build; another only with --other-slot")
    ap.add_argument("--other-slot", action="store_true",
                    help="let --slot be another key than FRIZZ's and the bench's (1-15: a stock "
                    "firmware, the storage firmware), or one holding another firmware: replaces "
                    "what's there. Never a session's")
    ap.add_argument("--run", type=int, metavar="SLOT", help="start what's in SLOT, sending nothing")
    ap.add_argument("--list", action="store_true",
                    help="show what's on each key, sending nothing: restarts FRIZZ into the "
                    "launcher (a loop in RAM is lost), the CHOMPI left at its picker")
    ap.add_argument("--name", help="its name on the card (default FRIZZ, FRIZZ-BETA, FRIZZ-TEST "
                    "or FRIZZ-BENCH)")
    ap.add_argument("--color", metavar="RRGGBB",
                    help="the colour for its key instead of the slot's own (10 green, 11 orange, "
                    "12 and 13 red), e.g. 8A4DFF (launcher 1.6 on; 000000 the launcher's own)")
    ap.add_argument("--wait", type=int, default=120, help="seconds to wait for the launcher")
    ap.add_argument("--device", help="its raw MIDI node, e.g. /dev/snd/midiC1D0 (default: found)")
    args = ap.parse_args()

    if args.color is not None and (args.run is not None or args.list
                                   or not re.fullmatch(r"[0-9A-Fa-f]{6}", args.color)):
        sys.exit("--color takes a colour like 8A4DFF, and goes with a build sent")
    if args.run is not None:
        node = chompi.start(args.run, args.wait, args.device)
        info = node and chompi.ident(node)
        if info:
            print("slot %d runs %s" % (args.run, chompi.describe_ident(info)))
        return
    if args.list:
        device = chompi.to_launcher(args.wait, args.device)
        chompi.device_guard(device)
        sys.argv = ["midi_send.py", "--list", "--device", device]
        midi_send.main()
        return
    if args.image and args.slot is None:
        sys.exit("a file of your own needs --slot: sending replaces what's in it")
    slot = args.slot if args.slot is not None else (
        chompi.BENCH_SLOT if args.bench else chompi.TEST_SLOT if args.test else None)
    if slot is None:
        sys.exit("which slot? --test (12) for a branch's build, --bench (13), or --slot N; keys "
                 "10 and 11 take a release's or a beta's build, from its v* tag")
    # the slot and the user's keys: refused before anything is built or the CHOMPI restarted
    check_slot(slot, args)
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
    if not args.other_slot and not tag:
        # FRIZZ's keys: never replace another firmware on them unasked
        there = chompi.slot_file(device, slot)
        if there and not there.upper().split("_", 1)[-1].startswith("FRIZZ"):
            sys.exit("key %d holds %s, not FRIZZ: give --other-slot to replace it anyway, "
                     "or set BENCH_SLOT / TEST_SLOT" % (slot, there))
        # nor the user's release or beta (keys moved since a tool sent: 2026-10-15, #171)
        if there and there.upper().split("_", 1)[-1].rsplit(".", 1)[0] in USER_NAMES:
            sys.exit("key %d holds %s, the user's: a test or a bench build never replaces it"
                     % (slot, there))
    chompi.device_guard(device)
    sys.argv = ["midi_send.py", image, "--slot", str(slot), "--name", name, "--device", device]
    color = args.color if args.color is not None else (
        None if args.other_slot else KEY_COLORS.get(slot))
    colour = color is not None and chompi.features(device) & midi_send.FEATURE_COLOR
    if args.color is not None and not colour:
        print("this launcher can't colour its keys (1.6 can): sent without --color")
    if colour:
        # stored, coloured (the key must hold it first), then started: a colour that fails
        # (the card) is said, and the build started all the same
        sys.argv.append("--stay")
    midi_send.main()
    if colour:
        sys.argv = ["midi_send.py", "--color", str(slot), color, "--device", device]
        try:
            midi_send.main()
        except SystemExit as e:
            print("key %d not coloured: %s" % (slot, e))
        chompi.send_run(device, slot)
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
