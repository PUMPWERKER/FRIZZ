#!/usr/bin/env python3
"""chompi.py: where the CHOMPI on USB is, and getting it from there to where it's wanted.

It is in one of four states, as the computer sees it:

    frizz      FRIZZ, which answers its own SysEx (code/src/MidiControl.h)
    launcher   the multi-firmware launcher's picker, which answers its PING
    storage    the USB storage firmware: the card is a drive labelled CHOMPI-SD
    other      a CHOMPI MIDI device that is neither (an older FRIZZ, TAPE, ...)

and None for nothing on USB. Every way between them goes through the launcher: FRIZZ restarts
into it on its SysEx F0 7D 43 48 10 F7 (MidiClock.h), the storage firmware on an eject, and
the launcher starts a slot on its RUN (05). Launchers and storage firmwares from before those
two need a hand instead, and this says which.

Which slot runs, no firmware says: FRIZZ on 10 and FRIZZ-TEST on 12 answer alike, and so does
the bench (FRIZZ-bench.bin, 11), whose load reads 0. So the tools note the slot they last
started (RUNNING, beside the lock: run(), flash.py) and to_frizz() goes by it: the FRIZZ
running if no slot is asked for, else the one asked for, started unless it's the one noted;
the bench is never taken for FRIZZ. A slot started by hand isn't noted: `hold` without a
command (playing by hand) forgets the note.

One process at a time has the CHOMPI: the first time a tool reaches for it (state(), a link
to it, a restart), it takes a lock (LOCK, flock) for the process's life, and waits, saying who
has it, while another holds it; what doesn't touch the CHOMPI (measure.py compare) never
waits. A tool started by one that holds it (FRIZZ_CHOMPI_HELD set) shares it; one that talks
to the twin, not the CHOMPI (test/remote.cpp), sets it too. To keep the CHOMPI over several
tools, or for playing it by hand:

    tools/chompi.py hold                 holds it until Ctrl-C
    tools/chompi.py hold CMD ARGS...     holds it while CMD runs (sh -c for a sequence)

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

import midi_send

FRIZZ_SLOT = int(os.environ.get("FRIZZ_SLOT", 10))
BENCH_SLOT = int(os.environ.get("BENCH_SLOT", 11))
TEST_SLOT = int(os.environ.get("TEST_SLOT", 12))  # a branch's build, tested by a session
STORAGE_SLOT = int(os.environ.get("STORAGE_SLOT", 15))
FRIZZ_SLOTS = (FRIZZ_SLOT, TEST_SLOT)  # the slots with a FRIZZ that answers its SysEx
STORAGE_LABEL = "CHOMPI-SD"

RESTART = midi_send.HEADER + bytes([0x10, 0xF7])  # FRIZZ's (MidiClock.h)
SETTINGS = 0x24  # FRIZZ's kCmdSettings: two bytes back; the launcher's BAD_MESSAGE is one
RUN = 0x05
BAD_MESSAGE, BAD_SLOT = 1, 8


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


# ---- MIDI ---------------------------------------------------------------------------------

ASOUND, SND = "/proc/asound", "/dev/snd"
DAISY_USB_ID = "0483:5740"  # the Daisy Seed's, which FRIZZ and the launcher keep


def find_device():
    """The CHOMPI's raw MIDI node: as midi_send finds it (by "chompi" in the card's names), or
    else the first card with the Daisy's USB id, for a FRIZZ whose names came garbled (seen
    once: product "Љ", maker "FrizZ"), saying what it sees there"""
    node = midi_send.find_device()
    if node:
        return node
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
            say("sound card %s has the Daisy's USB id (%s) but no CHOMPI in its names (%s): "
                "taken for the CHOMPI" % (card, DAISY_USB_ID, card_names(card)))
            return nodes[0]
    return None


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


def to_launcher(timeout=120, device=None):
    """The launcher's raw MIDI node, from wherever the CHOMPI is"""
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


def run(slot, wanted, timeout=120, device=None):
    """Starts slot SLOT from wherever the CHOMPI is and waits for state WANTED (None: don't)"""
    node = to_launcher(timeout, device)
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
    started(slot)
    if wanted:
        return wait_for(wanted, timeout, device)
    return None


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


def to_frizz(slot=None, timeout=120, device=None):
    """FRIZZ's raw MIDI node: on SLOT, started unless it's the one noted as running; without
    one the FRIZZ running, or else the slot last started in this hold, or FRIZZ_SLOT. Waits
    SETTLE s first for a firmware that is still starting, and says which slot it is"""
    now, where = state(device)
    settle = time.monotonic() + SETTLE
    while now in (None, "other") and time.monotonic() < settle:
        time.sleep(0.5)
        now, where = state(device)
    last, this_hold, what = last_started()
    if now == "frizz":
        if last == BENCH_SLOT:
            say("the bench runs (slot %d, %s), which answers as FRIZZ does" % (last, what))
        elif last in FRIZZ_SLOTS and slot in (None, last):
            say("FRIZZ on slot %d (%s)" % (last, what))
            return where
        elif slot is None:
            say("FRIZZ runs, on a slot the tools didn't start: --slot N makes sure")
            return where
    if slot is None:
        slot = last if this_hold and last in FRIZZ_SLOTS else FRIZZ_SLOT
    return start(slot, timeout, device)


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
    if sys.argv[1:2] != ["hold"]:
        sys.exit(__doc__)
    hold(sys.argv[2:])
