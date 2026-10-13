#!/usr/bin/env python3
"""chompi.py: where the CHOMPI on USB is, and getting it from there to where it's wanted.

It is in one of four states, as the computer sees it:

    frizz      FRIZZ, which answers its own SysEx (code/src/MidiControl.h)
    launcher   the multi-firmware launcher's picker, which answers its PING
    storage    the USB storage firmware: the card is a drive labelled CHOMPI-SD
    other      a CHOMPI MIDI device that is neither (an older FRIZZ, TAPE, ...)

and None for nothing on USB. FRIZZ's keys: 10 the latest release, 13 the latest beta (the
user's both), 12 a branch's build under test, 11 the bench, 15 the USB storage firmware. Every way between them goes through the launcher: FRIZZ restarts
into it on its SysEx F0 7D 43 48 10 F7 (MidiClock.h), the storage firmware on an eject, and
the launcher starts a slot on its RUN (05). Launchers and storage firmwares from before those
two need a hand instead, and this says which.

Which build runs, FRIZZ says itself since its developers' SysEx (ident(): kCmdDev's IDENT,
code/src/MidiControl.h): the CRC-32 and length of the image it runs, which the host works out
from a .bin (zlib.crc32), and whether it's the bench. flash.py notes the build it sends to a
slot (IMAGES, beside the lock: noted_image()), so a build found running is its slot's, even one
started by hand; to_frizz() goes by that: the FRIZZ running if no slot is asked for, else the
one asked for, started unless it's the one running; the bench is never taken for FRIZZ.

A FRIZZ from before IDENT (or a build the tools didn't send) can't say, and answers as FRIZZ on
10, FRIZZ-BETA on 13, FRIZZ-TEST on 12 and the bench (FRIZZ-bench.bin, 11, whose load reads 0) all do. For those
the tools note the slot they last started (RUNNING: run(), flash.py) and go by that note
instead. A slot started by hand isn't noted: `hold` without a command (playing by hand)
forgets the note.

One process at a time has the CHOMPI: the first time a tool reaches for it (state(), a link
to it, a restart), it takes a lock (LOCK, flock) for the process's life, and waits, saying who
has it, while another holds it; what doesn't touch the CHOMPI (measure.py compare) never
waits. A tool started by one that holds it (FRIZZ_CHOMPI_HELD set) shares it; one that talks
to the twin, not the CHOMPI (test/remote.cpp), sets it too. To keep the CHOMPI over several
tools, or for playing it by hand:

    tools/chompi.py hold                 holds it until Ctrl-C
    tools/chompi.py hold CMD ARGS...     holds it while CMD runs (sh -c for a sequence)

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
import fcntl
import glob
import os
import re
import subprocess
import sys
import time
import zlib

import midi_send

FRIZZ_SLOT = int(os.environ.get("FRIZZ_SLOT", 10))  # the latest release, the user's
BENCH_SLOT = int(os.environ.get("BENCH_SLOT", 11))
TEST_SLOT = int(os.environ.get("TEST_SLOT", 12))  # a branch's build, tested by a session
BETA_SLOT = int(os.environ.get("BETA_SLOT", 13))  # the latest beta, the user's too
STORAGE_SLOT = int(os.environ.get("STORAGE_SLOT", 15))
FRIZZ_SLOTS = (FRIZZ_SLOT, TEST_SLOT, BETA_SLOT)  # the slots with a FRIZZ that answers its SysEx
# the user's: flash.py sends there only when it's named (--slot), and only a release's or a
# beta's build, from its v* tag (flash.py's tagged())
USER_SLOTS = (FRIZZ_SLOT, BETA_SLOT)
STORAGE_LABEL = "CHOMPI-SD"

RESTART = midi_send.HEADER + bytes([0x10, 0xF7])  # FRIZZ's (MidiClock.h)
SETTINGS = 0x24  # FRIZZ's kCmdSettings: two bytes back; the launcher's BAD_MESSAGE is one
RUN = 0x05
BAD_MESSAGE, BAD_SLOT = 1, 8
# FRIZZ's developers' SysEx (MidiControl.h's namespace dev): F0 7D 43 48 3E SUB SEQ ARGS F7,
# answered 7E SUB SEQ STATUS DATA
DEV = 0x3E
IDENT, PERSIST, GET, SET, BENCH, PROBE, LEVELS, SIGNAL, RESEED, BATTERY = range(10)
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


def claim():
    """Takes the CHOMPI for this process, waiting while another has it"""
    global _lock
    if _lock or os.environ.get(HELD):
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
        sys.exit(subprocess.run(cmd).returncode)
    forget()  # what's started by hand, the tools can't know
    say("holding the CHOMPI; Ctrl-C lets it go")
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass



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


def parse_ident(data):
    """IDENT's answer as a dict"""
    return {"version": data[0], "kind": "bench" if data[1] == 1 else "frizz",
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
    if not got or got[0] != OK or len(got[1]) < 26:
        return None
    return parse_ident(got[1])


def reset_cause(flags):
    return ", ".join(n for bit, n in sorted(RESET_FLAGS.items()) if flags >> bit & 1) or "none"


def describe_ident(info, slots=None):
    """'FRIZZ, crc32 X, N B (slot 12's: md5 M, ...)', for the tools to say"""
    slots = slots_of(info) if slots is None else slots
    images = noted_images()
    what = "%s, crc32 %08x, %d B" % ("the bench" if info["kind"] == "bench" else "FRIZZ",
                                     info["crc"], info["length"])
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
        try:
            with open(usbid) as f:
                if f.read().strip() != DAISY_USB_ID:
                    continue
        except OSError:
            continue
        card = re.search(r"card(\d+)", usbid).group(1)
        nodes = sorted(glob.glob(os.path.join(SND, "midiC%sD*" % card)))
        if nodes:
            return card, nodes[0]
    return None, None


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


@contextlib.contextmanager
def link(node):
    """A midi_send.Link to the raw MIDI node, closed afterwards; takes the CHOMPI first"""
    claim()
    to = midi_send.Link(node)
    try:
        yield to
    finally:
        os.close(to.fd)


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


def restart(device):
    """Asks a running FRIZZ to restart into the launcher; the launcher ignores it"""
    try:
        with link(device) as to:
            os.write(to.fd, RESTART)
    except OSError:
        pass


# ---- the card, as a drive -----------------------------------------------------------------

def storage_partition():
    """/dev/sdXN of the CHOMPI-SD file system, if it is there"""
    link = "/dev/disk/by-label/" + STORAGE_LABEL
    return os.path.realpath(link) if os.path.exists(link) else None


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


def to_launcher(timeout=120, device=None):
    """The launcher's raw MIDI node, from wherever the CHOMPI is"""
    now, where = state(device)
    # a FRIZZ that doesn't answer for a while looks like another firmware: the bench, its
    # audio callback over 100 % in a segment, leaves MainLoop no time (BUSY s at most)
    busy = time.monotonic() + BUSY
    while now == "other" and time.monotonic() < busy:
        time.sleep(1)
        now, where = state(device)
    if now == "launcher":
        return where
    if now == "frizz":
        say("restarting FRIZZ into the launcher ...")
        restart(where)
        hint = "no launcher yet: switch the CHOMPI off and on"
    elif now == "storage":
        say("ejecting the card, which restarts the CHOMPI into the launcher ...")
        eject(where)
        hint = ("still no launcher: this USB storage firmware doesn't restart on an eject; "
                "press overdub, then the CHOMPI key")
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
    """The launcher's RUN for slot SLOT, exiting if it can't"""
    with link(node) as to:
        reply = to.call(RUN, bytes([slot]), timeout=1.0, retries=2, required=False)
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
    garbled; FRIZZ's on keys 10 and 13 is only started, never written"""
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


def slot_file(node, slot):
    """The file on key SLOT ("" for none), as the launcher at NODE lists it; None from a
    launcher before 1.5, which can't say"""
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
    while now in (None, "other") and time.monotonic() < settle:
        time.sleep(0.5)
        now, where = state(device)
    last, this_hold, what = last_started()
    info = ident(where) if now == "frizz" else None
    if info:
        slots = slots_of(info)
        running = slots[0] if len(slots) == 1 else (last if last in slots else None)
        if info["kind"] == "bench":
            say("the bench runs: %s" % describe_ident(info, slots))
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
