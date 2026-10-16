#!/usr/bin/env python3
"""chompi.py: where the CHOMPI on USB is, and getting it from there to where it's wanted.

It is in one of four states, as the computer sees it:

    frizz      FRIZZ, which answers its own SysEx (code/src/MidiControl.h)
    launcher   the multi-firmware launcher's picker, which answers its PING
    storage    the USB storage firmware: the card is a drive labelled CHOMPI-SD
    other      a CHOMPI MIDI device that is neither (an older FRIZZ, TAPE, ...)

and None for nothing on USB. FRIZZ's keys: 10 the latest release, 11 the latest beta (the
user's both), 12 a branch's build under test, 13 the bench, 15 the USB storage firmware. Every way between them goes through the launcher: FRIZZ restarts
into it on its SysEx F0 7D 43 48 10 F7 (MidiClock.h), and one that doesn't get there (its
MainLoop hung) on its developers' RESET, taken in the audio callback (to_launcher, #105); the
storage firmware on an eject, and the launcher starts a slot on its RUN (05). Launchers and storage firmwares from before those
two need a hand instead, and this says which.

Which build runs, FRIZZ says itself since its developers' SysEx (ident(): kCmdDev's IDENT,
code/src/MidiControl.h): the CRC-32 and length of the image it runs, which the host works out
from a .bin (zlib.crc32), and whether it's the bench. flash.py notes the build it sends to a
slot (IMAGES, beside the lock: noted_image()), so a build found running is its slot's, even one
started by hand; to_frizz() goes by that: the FRIZZ running if no slot is asked for, else the
one asked for, started unless it's the one running; the bench is never taken for FRIZZ.

A FRIZZ from before IDENT (or a build the tools didn't send) can't say, and answers as FRIZZ on
10, FRIZZ-BETA on 11, FRIZZ-TEST on 12 and the bench (FRIZZ-bench.bin, 13, whose load reads 0) all do. For those
the tools note the slot they last started (RUNNING: run(), flash.py) and go by that note
instead. A slot started by hand isn't noted: `hold` without a command (playing by hand)
forgets the note.

One process at a time has the CHOMPI: the first time a tool reaches for it (state(), a link
to it, a restart), it takes a lock (LOCK, flock) for the process's life, and waits, saying who
has it, while another holds it; what doesn't touch the CHOMPI (measure.py compare) never
waits. A tool started by one that holds it (FRIZZ_CHOMPI_HELD set) shares it while that one
runs. A hold runs its command in a process group of its own and keeps the lock until that has
ended: SIGTERM, SIGINT and SIGHUP go on to the group, and the lock goes only once nothing in it
runs, so what a tool does when stopped (a lent card taken back) is still done under it; a
second signal kills the group. Only a hold killed outright (SIGKILL) frees the lock while
what it started runs on, which takes it again at its next reach for the CHOMPI. One that
talks to the twin, not the CHOMPI (test/remote.cpp), sets it too, and FRIZZ_NO_DEVICE, which
stops a host check that reaches for a real device (device_guard). To keep the CHOMPI over several
tools, or for playing it by hand:

    tools/chompi.py hold                 holds it until Ctrl-C
    tools/chompi.py hold CMD ARGS...     holds it while CMD runs (sh -c for a sequence, or
                                         bash to type commands: the terminal is CMD's)

Now and then (#88, 2 in ~310 starts) FRIZZ comes up with its USB names garbled, one string
off (product "Љ", maker "FrizZ"): the tools find it by the Daisy's USB id then, and say so.
A tool that started a FRIZZ slot (or the bench, where it waits for it) and finds it so starts
it again, twice at most, and then asks for the power switch. For one found running so:

    tools/chompi.py fix-name [SLOT]      starts the slot noted as running (or SLOT) again:
                                         10, 11, 12 or 13, only started, nothing written

Linux only: ALSA's raw MIDI and udisks, Python 3 without packages.
"""
import argparse
import contextlib
import errno
import fcntl
import glob
import os
import re
import signal
import subprocess
import sys
import time
import zlib

import midi_send

FRIZZ_SLOT = int(os.environ.get("FRIZZ_SLOT", 10))  # the latest release, the user's
BETA_SLOT = int(os.environ.get("BETA_SLOT", 11))  # the latest beta, the user's too
TEST_SLOT = int(os.environ.get("TEST_SLOT", 12))  # a branch's build, tested by a session
BENCH_SLOT = int(os.environ.get("BENCH_SLOT", 13))
STORAGE_SLOT = int(os.environ.get("STORAGE_SLOT", 15))
FRIZZ_SLOTS = (FRIZZ_SLOT, TEST_SLOT, BETA_SLOT)  # the slots with a FRIZZ that answers its SysEx
# the user's: flash.py sends there only when it's named (--slot), and only a release's or a
# beta's build, from its v* tag (flash.py's tagged()). Keys 10 and 11 whatever FRIZZ_SLOT and
# BETA_SLOT say: they can add keys to guard (another CHOMPI's), never take these away
USER_SLOTS = tuple(sorted({10, 11, FRIZZ_SLOT, BETA_SLOT}))
STORAGE_LABEL = "CHOMPI-SD"

RESTART = midi_send.HEADER + bytes([0x10, 0xF7])  # FRIZZ's (MidiClock.h)
SETTINGS = 0x24  # FRIZZ's kCmdSettings: two bytes back; the launcher's BAD_MESSAGE is one
RUN = 0x05
BAD_MESSAGE, BAD_SLOT = 1, 8
# FRIZZ's developers' SysEx (MidiControl.h's namespace dev): F0 7D 43 48 3E SUB SEQ ARGS F7,
# answered 7E SUB SEQ STATUS DATA
DEV = 0x3E
IDENT, PERSIST, GET, SET, BENCH, PROBE, LEVELS, SIGNAL, RESEED, BATTERY, RESET, CRASH, TEST = range(13)
# RESET, handled in FRIZZ's audio callback, so it reaches one whose MainLoop hangs; sent once,
# never asked again (a FRIZZ started meanwhile would take it too), and unanswered
RESET_SYSEX = midi_send.HEADER + bytes([DEV, RESET, 0]) + b"RST" + b"\xF7"
LOOPS = 0x0D  # the loop loader's (LoopLoader.h): remote.py loops
DEV_STATUS = {0: "ok", 1: "refused", 2: "not in this build", 3: "not yet"}
OK, REFUSED, NOT_HERE, NOT_YET = range(4)
RAM_ONLY = 1  # IDENT's and PERSIST's flags: the card isn't written until a restart
# RCC_RSR's flags (the STM32H750's reference manual, RM0433): why the chip last reset
RESET_FLAGS = {17: "cpu", 19: "d1", 20: "d2", 21: "brown-out", 22: "pin", 23: "power-on",
               24: "software", 26: "watchdog", 28: "window watchdog", 30: "low power"}


def say(*args):
    print(*args, file=sys.stderr, flush=True)


# ---- the lock: one process at a time ------------------------------------------------------

def _runtime_dir():
    """The user's /run/user/UID, as the desktop's XDG_RUNTIME_DIR is, whatever a shell set: one
    lock for every tool of the user; ~/.cache where there's none"""
    run = "/run/user/%d" % os.getuid()
    if os.path.isdir(run) and os.access(run, os.W_OK):
        return run
    cache = os.path.join(os.path.expanduser("~"), ".cache")
    os.makedirs(cache, exist_ok=True)
    return cache


LOCK = os.environ.get("FRIZZ_CHOMPI_LOCK") or os.path.join(_runtime_dir(), "frizz-chompi.lock")
HELD = "FRIZZ_CHOMPI_HELD"
_lock = None


def _holding(held):
    """Whether HELD (FRIZZ_CHOMPI_HELD: the pid of the process that took the lock) still runs:
    a hold killed outright (SIGKILL) frees the lock while what it started runs on. Not a pid (a
    check's "1" is init's): taken as held"""
    try:
        os.kill(int(held), 0)
    except ValueError:
        return True
    except PermissionError:
        return True
    except OSError:
        return False
    return True


def claim():
    """Takes the CHOMPI for this process, waiting while another has it; shared with the hold
    that started it (HELD) while that runs"""
    global _lock
    held = os.environ.get(HELD)
    if _lock or held and _holding(held):
        return
    f = open(LOCK, "a+")
    try:
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        f.seek(0)
        say("the CHOMPI is in use (%s): waiting" % (f.read().strip() or "?"))
        fcntl.flock(f, fcntl.LOCK_EX)
    f.seek(0)
    f.truncate()
    f.write("pid %d in %s: %s\n" % (os.getpid(), os.getcwd(), " ".join(sys.argv)))
    f.flush()
    _lock = f
    os.environ[HELD] = str(os.getpid())  # the tools this one starts share it, and know the hold


def hold(cmd):
    """Holds the CHOMPI while cmd runs, or until Ctrl-C without one"""
    claim()
    if cmd:
        sys.exit(_run_held(cmd))
    forget()  # what's started by hand, the tools can't know
    say("holding the CHOMPI; Ctrl-C lets it go")
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass


HOLD_SIGNALS = (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)


def _run_held(cmd):
    """Runs CMD in a process group of its own, under the lock, until it has ended: SIGTERM,
    SIGINT and SIGHUP go on to that group, and once one came, the lock goes only when nothing in
    the group runs any more (a tool's child, or what sh -c started in the background, may
    outlive sh); a second signal kills the group (SIGKILL). Its exit code, 128+N for a
    signal N"""
    child, got = [], []

    def forward(sig, frame):
        got.append(sig)
        if not child:
            return  # not started yet: passed on below
        try:
            if len(got) == 1:
                say("%s: passed on to what the hold runs; the CHOMPI stays held until it has "
                    "ended (a second signal kills it)" % signal.Signals(sig).name)
                os.killpg(child[0].pid, sig)
            else:
                say("killing what the hold runs (SIGKILL)")
                os.killpg(child[0].pid, signal.SIGKILL)
        except ProcessLookupError:
            pass

    for sig in HOLD_SIGNALS:
        signal.signal(sig, forward)
    tty = _foreground_tty()
    p = subprocess.Popen(cmd, process_group=0)
    child.append(p)
    try:
        if tty is not None:
            # the terminal's to the group (an interactive one, a shell: hold bash), which its
            # Ctrl-C then reaches itself; started again if it stopped reading it before
            _give_tty(tty, p.pid)
            os.killpg(p.pid, signal.SIGCONT)
        if got:
            os.killpg(p.pid, got[0])
    except ProcessLookupError:
        pass  # ended already
    code = p.wait()
    while got:
        try:
            os.killpg(p.pid, 0)
        except ProcessLookupError:
            break
        except PermissionError:
            pass
        time.sleep(0.2)
    if tty is not None:
        _give_tty(tty, os.getpgrp())
    return 128 - code if code < 0 else code


def _foreground_tty():
    """stdin's terminal if this process's group has it (a hold run at a prompt), else None"""
    try:
        fd = sys.stdin.fileno()
        return fd if os.isatty(fd) and os.tcgetpgrp(fd) == os.getpgrp() else None
    except (OSError, ValueError):
        return None


def _give_tty(fd, group):
    """Makes GROUP the terminal's foreground, from a group that may no longer be (SIGTTOU held
    off)"""
    old = signal.signal(signal.SIGTTOU, signal.SIG_IGN)
    try:
        os.tcsetpgrp(fd, group)
    except OSError:
        pass
    finally:
        signal.signal(signal.SIGTTOU, old)


# ---- which slot runs: noted by the tools that start one ----------------------------------

RUNNING = os.path.join(os.path.dirname(LOCK), "frizz-chompi.slot")


def started(slot):
    """Notes that slot SLOT was started, by what and in which hold"""
    what = " ".join([os.path.basename(sys.argv[0])] + sys.argv[1:])
    try:
        with open(RUNNING, "w") as f:
            f.write("%d %s %s %s\n" % (slot, os.environ.get(HELD) or "-",
                                       time.strftime("%H:%M:%S"), what))
    except OSError:
        pass


def forget():
    try:
        os.remove(RUNNING)
    except OSError:
        pass


def last_started():
    """(the slot last started, whether in this hold, 'what at when'), or (None, False, None)"""
    try:
        with open(RUNNING) as f:
            slot, holder, at, what = f.read().strip().split(" ", 3)
        return int(slot), holder == os.environ.get(HELD), "%s at %s" % (what, at)
    except (OSError, ValueError):
        return None, False, None


# ---- which build runs: IDENT, and the builds flash.py sent ------------------------------

IMAGES = os.path.join(os.path.dirname(LOCK), "frizz-chompi.images")


def image_id(path):
    """(CRC-32, length) of a .bin, as IDENT tells them for the image running"""
    with open(path, "rb") as f:
        data = f.read()
    return zlib.crc32(data), len(data)


def noted_images():
    """{slot: (crc, length, 'md5 M, what at when')} of the builds flash.py sent"""
    found = {}
    try:
        with open(IMAGES) as f:
            for line in f:
                slot, crc, length, rest = line.rstrip("\n").split(" ", 3)
                found[int(slot)] = (int(crc, 16), int(length), rest)
    except (OSError, ValueError):
        pass
    return found


def noted_image(slot, path, md5):
    """Notes that the build at PATH (md5 MD5) went to slot SLOT"""
    images = noted_images()
    crc, length = image_id(path)
    images[slot] = (crc, length, "md5 %s, %s at %s" % (md5, os.path.basename(path),
                                                         time.strftime("%H:%M:%S")))
    try:
        with open(IMAGES, "w") as f:
            for s in sorted(images):
                f.write("%d %08x %d %s\n" % ((s,) + images[s]))
    except OSError:
        pass


def slots_of(info):
    """The slots flash.py sent the build IDENT (INFO) tells of to"""
    return [s for s, (crc, length, _) in sorted(noted_images().items())
            if (crc, length) == (info["crc"], info["length"])]


_seq = [0]


def dev_call(to, sub, args=b"", timeout=0.5, retries=3):
    """kCmdDev's SUB on the Link TO, asked again while unanswered: (status, data), or None
    when nothing answers (a firmware without it)"""
    _seq[0] = _seq[0] % 127 + 1
    seq = _seq[0]
    reply = to.call(DEV, bytes([sub, seq]) + bytes(args), timeout=timeout, retries=retries,
                    required=False, match=lambda r: len(r) >= 3 and r[0] == sub and r[1] == seq)
    return None if reply is None else (reply[2], bytes(reply[3:]))


def get7(data):
    """A number in 7-bit groups, the highest first"""
    v = 0
    for b in data:
        v = v << 7 | b
    return v


IDENT_LEN = 26  # IDENT's data, its flags the last byte


# IDENT's kinds (MidiControl.h): FRIZZ.bin, its bench; any other is another firmware built on
# FRIZZ's platform, never taken for FRIZZ (#368)
IDENT_KINDS = {0: "frizz", 1: "bench"}


def parse_ident(data):
    """IDENT's answer as a dict; None for one too short (cut off, or not IDENT's). Its kind:
    "frizz", "bench", or "other N" for another firmware's"""
    if len(data) < IDENT_LEN:
        return None
    return {"version": data[0], "kind": IDENT_KINDS.get(data[1], "other %d" % data[1]),
            "crc": get7(data[2:7]) & 0xFFFFFFFF, "length": get7(data[7:10]),
            "uptime": get7(data[10:15]) / 1000, "passes": get7(data[15:20]),
            "reset": get7(data[20:25]) & 0xFFFFFFFF, "ram_only": bool(data[25] & RAM_ONLY)}


def ident(device):
    """What the FRIZZ at DEVICE runs (parse_ident), or None from one without IDENT"""
    try:
        with link(device) as to:
            got = dev_call(to, IDENT, retries=2)
    except OSError:
        return None
    if not got or got[0] != OK:
        return None
    return parse_ident(got[1])


def reset_cause(flags):
    return ", ".join(n for bit, n in sorted(RESET_FLAGS.items()) if flags >> bit & 1) or "none"


def describe_ident(info, slots=None):
    """'FRIZZ, crc32 X, N B (slot 12's: md5 M, ...)', for the tools to say"""
    slots = slots_of(info) if slots is None else slots
    images = noted_images()
    name = {"frizz": "FRIZZ", "bench": "the bench"}.get(info["kind"])
    name = name or "another firmware (IDENT kind %s)" % info["kind"].split()[-1]
    what = "%s, crc32 %08x, %d B" % (name, info["crc"], info["length"])
    if slots:
        what += " (%s)" % "; ".join("slot %d's: %s" % (s, images[s][2]) for s in slots)
    elif info["length"]:
        what += " (not a build the tools sent)"
    return what


# ---- MIDI ---------------------------------------------------------------------------------

ASOUND, SND = "/proc/asound", "/dev/snd"
DAISY_USB_ID = "0483:5740"  # the Daisy Seed's, which FRIZZ and the launcher keep


def find_device():
    """The CHOMPI's raw MIDI node: as midi_send finds it (by "chompi" in the card's names), or
    else the first card with the Daisy's USB id, for a FRIZZ whose names came garbled (#88:
    product "Љ", maker "FrizZ"), saying what it sees there"""
    node = midi_send.find_device()
    if node:
        return node
    card, node = by_usb_id()
    if node:
        say("sound card %s has the Daisy's USB id (%s) but no CHOMPI in its names (%s): "
            "taken for the CHOMPI" % (card, DAISY_USB_ID, card_names(card)))
    return node


def by_usb_id():
    """(the card, its raw MIDI node) of the first sound card with the Daisy's USB id, or
    (None, None)"""
    for usbid in sorted(glob.glob(os.path.join(ASOUND, "card*", "usbid"))):
        card = re.search(r"card(\d+)", usbid).group(1)
        if usb_id(card) != DAISY_USB_ID:
            continue
        nodes = sorted(glob.glob(os.path.join(SND, "midiC%sD*" % card)))
        if nodes:
            return card, nodes[0]
    return None, None


def usb_id(card):
    """Sound card CARD's USB id ("0483:5740"), None for one without (not on USB)"""
    try:
        with open(os.path.join(ASOUND, "card%s" % card, "usbid")) as f:
            return f.read().strip()
    except OSError:
        return None


def garbled():
    """Whether the CHOMPI is on USB with its names garbled (#88): found by the Daisy's USB id
    only, not by its names"""
    return not midi_send.find_device() and by_usb_id()[1] is not None


def card_names(card):
    """Sound card CARD's id and names, as /proc/asound/cards has them"""
    try:
        with open(os.path.join(ASOUND, "cards")) as f:
            text = f.read()
    except OSError:
        return "?"
    m = re.search(r"^\s*%s \[([^\]]*)\]: (.*)\n\s*(.*)$" % card, text, re.M)
    return " / ".join(g.strip() for g in m.groups()) if m else "?"


# Set by the host checks' Python() (test/pytool.h): nothing reaches a real device from there,
# however a stub falls short. A pseudo-terminal (the twin's) still goes
NO_DEVICE = "FRIZZ_NO_DEVICE"


def device_guard(what):
    """Stops the process at once, finally blocks and all (they'd reach for the device too), when
    a host check has reached for a real device: WHAT, a node under /dev (not a pty's), or
    "udisks"; the twin's ptys and a stub's made-up names pass"""
    if not os.environ.get(NO_DEVICE):
        return
    if what == "udisks" or (str(what).startswith("/dev/") and not str(what).startswith("/dev/pts/")):
        say("a host check reached for a real device (%s): stub it. Stopped" % what)
        os._exit(70)


@contextlib.contextmanager
def link(node):
    """A midi_send.Link to the raw MIDI node, closed afterwards; takes the CHOMPI first"""
    device_guard(node)
    claim()
    to = midi_send.Link(node)
    try:
        yield to
    finally:
        to.close()


def ask_midi(device):
    """frizz, launcher or other, by one query to the device"""
    try:
        with link(device) as to:
            reply = to.call(SETTINGS, timeout=0.3, retries=2, required=False)
    except OSError:
        return None  # not there, or it went away mid-way
    if reply is None:
        return "other"
    return "frizz" if len(reply) >= 2 else "launcher"


def restart(device, hard=False):
    """Asks a running FRIZZ to restart into the launcher; the launcher ignores it. HARD: its
    developers' RESET, from the audio callback, for one whose MainLoop doesn't answer: at once,
    not waiting for the card (a scene or the settings being written are cut short). Exits,
    sending nothing, while FRIZZ lends the card to the computer: the drive would go from under
    it, its cache unwritten (lent())"""
    why = lent(device)
    if why:
        sys.exit("the firmware running lends the card to the computer (%s): not restarted, the drive "
                 "would go from under it. Eject it there (or take it back on the CHOMPI) and run this "
                 "again" % why)
    try:
        with link(device) as to:
            os.write(to.fd, RESET_SYSEX if hard else RESTART)
    except OSError:
        pass


def frizz_node(node):
    """Whether NODE is FRIZZ's by its USB names (product "FrizZ", or maker when garbled, #88):
    FRIZZ silent for BUSY s is RESET at once, another firmware only once it didn't take the
    restart (#371)"""
    m = re.search(r"midiC(\d+)D", node or "")
    return bool(m) and "frizz" in card_names(m.group(1)).lower()


def platform_node(node):
    """Whether NODE is a CHOMPI's by its USB names ("Chompi Club"), or by the Daisy's USB id
    when they came garbled (#88: one string off, the maker the product's name, so no "Chompi
    Club"; FRIZZ's are found so too, #371), not a Daisy that says it is one: another firmware
    on FRIZZ's platform takes FRIZZ's restart SysEx too (#368); one that doesn't ignores it"""
    m = re.search(r"midiC(\d+)D", node or "")
    names = card_names(m.group(1)).lower() if m else ""
    return bool(m) and ("chompi" in names
                        or usb_id(m.group(1)) == DAISY_USB_ID and "daisy" not in names)


def other_firmware(node):
    """Whether NODE is another firmware's on FRIZZ's platform, not FRIZZ's, by its USB names:
    it never answers as FRIZZ, so waiting for it to (BUSY, SETTLE) only costs time (#371)"""
    return platform_node(node) and not frizz_node(node)


# ---- the card, as a drive -----------------------------------------------------------------

def card_partition():
    """/dev/sdXN of the CHOMPI-SD file system, if it is there, whoever offers it"""
    link = "/dev/disk/by-label/" + STORAGE_LABEL
    if os.environ.get(NO_DEVICE):
        return None  # a host check: this computer's own drives aren't its business
    return os.path.realpath(link) if os.path.exists(link) else None


# the vendor (SCSI INQUIRY) of a card FRIZZ lends (#196); another firmware built on FRIZZ's
# platform is told by its MIDI beside the drive (lent_by_firmware, #368)
LENDERS = ("FrizZ",)


def drive_vendor(part):
    """The vendor PART's drive gives (INQUIRY), None if it can't be read"""
    disk = os.path.basename(storage_disk(part))
    try:
        with open("/sys/block/%s/device/vendor" % disk) as f:
            return f.read().strip()
    except OSError:
        return None


def lent_by_frizz(part):
    """Whether PART is the card a running FRIZZ lends as a USB drive (#196: its drive's vendor
    is "FrizZ"), not the USB storage firmware's"""
    return drive_vendor(part) == "FrizZ"


def lent_by_firmware(part):
    """Whether PART is a card a running firmware lends (FRIZZ, or another on its platform), not
    the USB storage firmware's: its vendor is a lender's, or the CHOMPI answers MIDI beside the
    drive, which the storage firmware, a drive alone, never does (#368)"""
    return (drive_vendor(part) in LENDERS or midi_send.find_device() is not None
            or by_usb_id()[1] is not None)


def storage_partition():
    """/dev/sdXN of the CHOMPI-SD file system, if the USB storage firmware offers it (not a
    firmware lending the card: FRIZZ, or another, answers MIDI meanwhile)"""
    part = card_partition()
    return part if part and not lent_by_firmware(part) else None


def lent_partition():
    """/dev/sdXN of the card a running firmware lends (FRIZZ, or another on its platform), if
    it is there"""
    part = card_partition()
    return part if part and lent_by_firmware(part) else None


USB_SUB = 0x0E  # kCmdDev's USB (MidiControl.h): its first data byte's 4 is "the card lent"
CARD_LENT = 4


def lent(device):
    """Why the card counts as lent to the computer by the FRIZZ at DEVICE ("CHOMPI-SD at
    /dev/sdX1", or what USB's flags say), None if it isn't: the drive FRIZZ offers (#196), or
    FRIZZ's own word, which a lend the computer hasn't shown as a drive yet has too. A FRIZZ
    that doesn't answer USB (one from before TinyUSB, or hung) has only the drive to go by"""
    part = lent_partition()
    if part:
        return "%s at %s" % (STORAGE_LABEL, part)
    try:
        with link(device) as to:
            reply = dev_call(to, USB_SUB, [0], timeout=0.3, retries=1)
    except OSError:
        return None
    if reply and reply[0] == OK and reply[1] and reply[1][0] & CARD_LENT:
        return "FRIZZ says so"
    return None


def storage_disk(part):
    """The whole disk the file system is on: /dev/sdX (a card without partitions: itself)"""
    parent = subprocess.run(["lsblk", "-no", "PKNAME", part], capture_output=True,
                            text=True).stdout.strip()
    return "/dev/" + parent if parent else part


def mountpoint(part):
    with open("/proc/mounts") as mounts:
        for line in mounts:
            fields = line.split()
            if os.path.realpath(fields[0]) == part:
                return fields[1].replace("\\040", " ")
    return None


def udisks(*args, check=True):
    """udisksctl's output; None if it failed and check is off"""
    device_guard("udisks")
    quiet = [] if args[0] == "info" else ["--no-user-interaction"]  # info takes no such
    result = subprocess.run(["udisksctl", *args, *quiet], capture_output=True, text=True)
    if result.returncode == 0:
        return result.stdout
    if check:
        sys.exit("udisksctl %s: %s" % (" ".join(args), result.stderr.strip()))
    return None


def mount(part, tries=20):
    """Where the card is mounted, mounting it if needed. The drive appears a moment before
    its file system can be mounted, so this tries for a while"""
    for attempt in range(tries):
        point = mountpoint(part)
        if point:
            return point
        out = udisks("mount", "-b", part, check=attempt == tries - 1)
        if out:
            m = re.search(r" at (.+?)\.?$", out.strip())
            return m.group(1) if m else mountpoint(part)
        time.sleep(0.5)


def eject(part):
    """Unmounts the card and ejects the drive, which restarts the storage firmware"""
    os.sync()
    # the desktop looks into a freshly mounted card for a moment (busy): try for a while
    for attempt in range(20):
        if not mountpoint(part) or udisks("unmount", "-b", part, check=attempt == 19):
            break
        time.sleep(0.5)
    disk = storage_disk(part)
    # udisks' Eject runs eject(1) as root: START STOP UNIT with LoEj, what the firmware waits for
    info = udisks("info", "-b", disk)
    m = re.search(r"Drive:\s+'(/org/freedesktop/UDisks2/drives/[^']+)'", info)
    if not m:
        sys.exit("udisks doesn't know the drive behind %s" % disk)
    result = subprocess.run(["gdbus", "call", "--system", "--dest", "org.freedesktop.UDisks2",
                             "--object-path", m.group(1), "--method",
                             "org.freedesktop.UDisks2.Drive.Eject", "{}"],
                            capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit("ejecting %s: %s" % (disk, result.stderr.strip()))


# ---- the state, and the ways between ------------------------------------------------------

def state(device=None):
    """(state, the raw MIDI node or the drive's partition), or (None, None)"""
    claim()
    part = storage_partition()
    if part:
        return "storage", part
    device = device or find_device()
    if device:
        found = ask_midi(device)
        if found:
            return found, device
    return None, None


def wait_for(wanted, timeout, device=None, hint=None, hint_after=8):
    """The node (or partition) once the CHOMPI is in the wanted state; exits after timeout"""
    deadline = time.monotonic() + timeout
    hint_at = time.monotonic() + hint_after
    while time.monotonic() < deadline:
        now, where = state(device)
        if now == wanted:
            return where
        if hint and time.monotonic() > hint_at:
            say(hint)
            hint = None
        time.sleep(0.5)
    sys.exit("the CHOMPI didn't get to %s within %d s" % (wanted, timeout))


BUSY = 20  # s a FRIZZ (or the bench) may leave unanswered before it's taken for another firmware
RESCUE = 15  # s a FRIZZ asked to restart has before it's RESET from its audio callback


def reached(wanted, timeout, device=None):
    """Whether the CHOMPI gets to state WANTED within TIMEOUT s"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if state(device)[0] == wanted:
            return True
        time.sleep(0.5)
    return False


# the slot of the FRIZZ to_launcher() last restarted into the launcher (running_slot()), None
# when it found the CHOMPI elsewhere: what a tool that then sends nothing (flash.py refusing a
# key only the launcher can list) starts again
came_from = None


def running_slot(node):
    """The slot the FRIZZ at NODE runs from: by IDENT and the builds flash.py sent, or else the
    slot noted as started in this hold; FRIZZ_SLOT when neither tells, and for the bench, which
    answers as FRIZZ does but is never started again unasked (it measures ~100 s)"""
    info = ident(node)
    last, this_hold, _ = last_started()
    if info:
        slots = [s for s in slots_of(info) if s in FRIZZ_SLOTS] if info["kind"] == "frizz" else []
        return slots[0] if len(slots) == 1 else last if last in slots else FRIZZ_SLOT
    return last if this_hold and last in FRIZZ_SLOTS else FRIZZ_SLOT


def to_launcher(timeout=120, device=None):
    """The launcher's raw MIDI node, from wherever the CHOMPI is; came_from says which FRIZZ it
    restarted"""
    global came_from
    came_from = None
    now, where = state(device)
    # a FRIZZ that doesn't answer for a while looks like another firmware: the bench, its
    # audio callback over 100 % in a segment, leaves MainLoop no time (BUSY s at most)
    # (another firmware never answers as FRIZZ: no use waiting for it, #371)
    busy = time.monotonic() + BUSY
    while now == "other" and not other_firmware(where) and time.monotonic() < busy:
        time.sleep(1)
        now, where = state(device)
    if now == "launcher":
        return where
    if now == "frizz":
        came_from = running_slot(where)
        say("restarting FRIZZ into the launcher ...")
        restart(where)
        hint = "no launcher yet: switch the CHOMPI off and on"
        # one that took the restart but whose MainLoop hangs before it gets there (#105)
        if not reached("launcher", RESCUE, device):
            say("FRIZZ hasn't restarted: resetting it from its audio callback (RESET) ...")
            restart(where, hard=True)
    elif now == "other" and frizz_node(where):
        # FRIZZ by its names, its MainLoop silent for BUSY s: hung, while its audio callback
        # may still take RESET (#105)
        say("FRIZZ doesn't answer: resetting it from its audio callback (RESET) ...")
        restart(where, hard=True)
        hint = "still no launcher: FRIZZ's audio callback hangs too; switch the CHOMPI off and on"
    elif now == "storage":
        say("ejecting the card, which restarts the CHOMPI into the launcher ...")
        eject(where)
        hint = ("still no launcher: this USB storage firmware doesn't restart on an eject; "
                "press overdub, then the CHOMPI key")
    elif now == "other" and platform_node(where):
        # another firmware on FRIZZ's platform: it doesn't answer as FRIZZ, but takes its
        # restart into the launcher (#368)
        say("another firmware on the CHOMPI: asking it to restart into the launcher ...")
        restart(where)
        hint = None
        # its MainLoop hung, while its audio callback may still take the platform's RESET (#371)
        if not reached("launcher", RESCUE, device):
            say("it didn't restart: resetting it from its audio callback (RESET) ...")
            restart(where, hard=True)
            hint = "still no launcher: switch the CHOMPI off and on"
    elif now == "other":
        say("a CHOMPI that can't be restarted from here: switch it off and on")
        hint = None
    else:
        say("no CHOMPI on USB: switch it on (and connect it)")
        hint = None
    return wait_for("launcher", timeout, device, hint, hint_after=10)


def run(slot, wanted, timeout=120, device=None, rename=True):
    """Starts slot SLOT from wherever the CHOMPI is and waits for state WANTED (None: don't);
    a FRIZZ whose USB names came garbled is started again (renamed())"""
    node = to_launcher(timeout, device)
    send_run(node, slot)
    started(slot)
    if not wanted:
        return None
    where = wait_for(wanted, timeout, device)
    return renamed(slot, where, timeout, device) if wanted == "frizz" and rename else where


def send_run(node, slot):
    """The launcher's RUN for slot SLOT, exiting if it can't. A launcher that went away from
    USB before its reply was read has started the slot (#203): the caller's wait finds it"""
    try:
        with link(node) as to:
            reply = to.call(RUN, bytes([slot]), timeout=1.0, retries=2, required=False)
    except OSError as e:
        if e.errno not in (errno.ENODEV, errno.EIO):
            raise
        say("the launcher left USB before it answered RUN: it is starting slot %d (#203)" % slot)
        reply = None
    if reply and reply[0] == BAD_SLOT:
        sys.exit("the launcher has nothing in slot %d" % slot)
    if reply and reply[0] == BAD_MESSAGE:
        say("this launcher can't start a slot from here (no RUN): press key %d" % slot)
    elif reply and reply[0] != 0:
        sys.exit("the launcher refused to start slot %d: status %d" % (slot, reply[0]))
    else:
        say("starting slot %d ..." % slot)


NAME_TRIES = 2  # restarts for USB names that came garbled
RENAME_SLOTS = (FRIZZ_SLOT, BENCH_SLOT, TEST_SLOT, BETA_SLOT)


def renamed(slot, node, timeout=120, device=None):
    """NODE, FRIZZ's on slot SLOT just started, once its USB names are right: now and then
    (#88, 2 in ~310 starts) they come garbled, found by the USB id only, and FRIZZ is
    restarted into the launcher and the slot started again, NAME_TRIES times at most, sending
    nothing to it. Only FRIZZ's slots and the bench's; not with a node given (--device)"""
    if device or slot not in RENAME_SLOTS:
        return node
    for attempt in range(NAME_TRIES):
        if not garbled():
            return node
        say("slot %d came up with garbled USB names (#88): starting it again (%d of %d)"
            % (slot, attempt + 1, NAME_TRIES))
        node = run(slot, "frizz", timeout, device, rename=False)
    if garbled():
        say("the USB names are still garbled after %d restarts (#88): switch the CHOMPI off "
            "and on. The tools find it anyway, by its USB id" % NAME_TRIES)
    return node


def fix_name(slot=None, timeout=120):
    """Starts SLOT again (default: the one noted as running), for USB names that came
    garbled; FRIZZ's on keys 10 and 11 is only started, never written"""
    last, _, what = last_started()
    if slot is None and last is None:
        sys.exit("no slot is noted as running (one started by hand isn't): fix-name SLOT")
    slot = last if slot is None else slot
    if slot not in RENAME_SLOTS:
        sys.exit("slot %d isn't FRIZZ's, FRIZZ-BETA's, FRIZZ-TEST's or the bench's (%s)"
                 % (slot, ", ".join(str(s) for s in RENAME_SLOTS)))
    say("starting slot %d again%s" % (slot, " (%s)" % what if slot == last else ""))
    node = run(slot, "frizz", timeout)
    if not garbled():
        say("slot %d answers, its USB names right" % slot)
    return node


def start(slot, timeout=120, device=None):
    """Starts slot SLOT from wherever the CHOMPI is; a FRIZZ slot's raw MIDI node once it
    answers, None for another"""
    return run(slot, "frizz" if slot in FRIZZ_SLOTS else None, timeout, device)


def features(node):
    """The launcher's at NODE: its PING's features (midi_send.FEATURE_*), 0 from one before 1.5
    (no such byte) or none answering"""
    with link(node) as to:
        reply = to.call(midi_send.PING, timeout=0.3, retries=3, required=False)
    return reply[7] if reply and len(reply) >= 8 else 0


def slot_file(node, slot):
    """The file on key SLOT ("" for none), as the launcher at NODE lists it; None from a
    launcher before 1.5, which can't say. 1.6's LIST adds the key's colour after the size,
    which this doesn't read"""
    with link(node) as to:
        reply = to.call(midi_send.PING, timeout=0.3, retries=3, required=False)
        if not reply or len(reply) < 8 or not reply[7] & midi_send.FEATURE_LIST:
            return None
        reply = to.call(midi_send.LIST, bytes([slot]), timeout=2.0, retries=2,
                        required=False, match=midi_send.for_slot(slot))
    if not reply or reply[0] != 0 or len(reply) < 3:
        return None
    return reply[3:3 + reply[2]].decode("ascii", "replace")


SETTLE = 10  # s for a firmware just started to answer: USB back, FRIZZ booted


# the slot to_frizz() found or started FRIZZ on, None if it couldn't tell
frizz_slot = None


def to_frizz(slot=None, timeout=120, device=None, again=False):
    """FRIZZ's raw MIDI node: on SLOT, started unless it's the one running; without one the
    FRIZZ running, or else the slot last started in this hold, or FRIZZ_SLOT. Which runs, it
    asks FRIZZ (ident(): the build, the bench or not), or else goes by the slot noted as
    started. Waits SETTLE s first for a firmware that is still starting, and says which slot
    it is (frizz_slot). AGAIN starts the one running again too, so it comes up as from
    power-on (remote.py play --fresh, #123): through the launcher, the card not written"""
    global frizz_slot

    def found(node, which):
        global frizz_slot
        frizz_slot = which
        if not again:
            return kept(node)
        if which is None:
            sys.exit("FRIZZ runs on a slot the tools can't tell, to start it again: --slot N")
        say("starting slot %d again, for a fresh start" % which)
        return start(which, timeout, device)

    now, where = state(device)
    settle = time.monotonic() + SETTLE
    while (now is None or now == "other" and not other_firmware(where)) and time.monotonic() < settle:
        time.sleep(0.5)
        now, where = state(device)
    last, this_hold, what = last_started()
    info = ident(where) if now == "frizz" else None
    if info:
        slots = slots_of(info)
        running = slots[0] if len(slots) == 1 else (last if last in slots else None)
        if info["kind"] == "bench":
            say("the bench runs: %s" % describe_ident(info, slots))
        elif info["kind"] != "frizz":
            say("not FRIZZ: %s" % describe_ident(info, slots))  # started below (#368)
        elif running is not None and slot in (None, running):
            say("FRIZZ on slot %d: %s" % (running, describe_ident(info, slots)))
            return found(where, running)
        elif running is None and slot is None:
            say("FRIZZ runs: %s; --slot N makes sure" % describe_ident(info, slots))
            return found(where, last if this_hold and last in FRIZZ_SLOTS else None)
        elif running is None and slot == last and this_hold:
            say("FRIZZ on slot %d (%s): %s" % (last, what, describe_ident(info, slots)))
            return found(where, last)
    elif now == "frizz":
        if last == BENCH_SLOT:
            say("the bench runs (slot %d, %s), which answers as FRIZZ does" % (last, what))
        elif last in FRIZZ_SLOTS and slot in (None, last):
            say("FRIZZ on slot %d (%s)" % (last, what))
            return found(where, last)
        elif slot is None:
            say("FRIZZ runs, on a slot the tools didn't start: --slot N makes sure")
            return found(where, None)
    if slot is None:
        slot = last if this_hold and last in FRIZZ_SLOTS else FRIZZ_SLOT
    frizz_slot = slot
    return start(slot, timeout, device)


def kept(node):
    """NODE, of a FRIZZ found running: with garbled USB names, saying what restarts it"""
    if node and garbled():
        say("its USB names are garbled (#88): tools/chompi.py fix-name starts it again")
    return node


def to_storage(timeout=120):
    """The card's partition, with the CHOMPI in its USB storage firmware (mount() mounts it)"""
    return storage_partition() or run(STORAGE_SLOT, "storage", timeout)


# ---- the tools' command lines --------------------------------------------------------------

def before_and_after(ap, *options):
    """Options taken before the subcommand and after it, each (flags, add_argument's keywords):
    added to AP, and to the parser returned, the parent for the subcommands, where a default of
    SUPPRESS keeps one given before from being overwritten"""
    common = argparse.ArgumentParser(add_help=False)
    for flags, kw in options:
        ap.add_argument(*flags, **kw)
        common.add_argument(*flags, **dict(kw, default=argparse.SUPPRESS))
    return common


if __name__ == "__main__":
    if sys.argv[1:2] == ["hold"]:
        hold(sys.argv[2:])
    elif sys.argv[1:2] == ["fix-name"] and len(sys.argv) <= 3:
        try:
            fix_name(int(sys.argv[2]) if len(sys.argv) > 2 else None)
        except ValueError:
            sys.exit("fix-name [SLOT]: a slot number")
    else:
        sys.exit(__doc__)
