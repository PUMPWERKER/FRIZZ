#!/usr/bin/env python3
"""reaper.py: REAPER as a real DAW against FRIZZ (#114, step 2): its MIDI clock, transport and
automation, for the release test's DAW items (#55, sections 7 and 8), and its clock's jitter
(#83).

    ./reaper.py [--slot 12] [--audio jack,dummy] [--cases ID,ID ...] [--seconds S]
                [--quick] [--out FILE] [-v]
    ./reaper.py --list
    ./reaper.py devices [--audio jack|dummy]

A run has three parts, as FRIZZ's USB MIDI node can't be shared: while REAPER runs it has the
node to itself (remote.py, flash.py and the other tools can't reach FRIZZ then).
  1. This tool on the node, as midisuite.py: FRIZZ kept in RAM (PERSIST 0), what the cases
     change noted to be put back, the TRS wiring found, and the reference for the jitter: a
     clock from this computer (midiport.Clock) at the same tempos.
  2. REAPER, once per audio mode (--audio: jack, then dummy), started on a configuration of
     its own: every case runs in the first, the jitter in each. REAPER takes this tool's
     commands while it runs (reaper.lua: a command file it reads ~30 times a second): its
     transport, tempo, loop range, tracks; and bytes for FRIZZ, so FRIZZ's SysEx (keys,
     knobs, settings, STATE, GET) go out through REAPER, and its answers come back through
     REAPER's MIDI input. midisuite.py's Rig runs on that as on the node, slower (each
     query ~70 ms).
  3. This tool on the node again: everything put back.
The cases marked synced (daw-follows) run in one more REAPER after those, in the first audio
mode, started only when one is: REAPER synchronized to what comes in (SYNC_PROJECT) and
sending no clock of its own (midiouts_clock=0), only the commands' bytes.
The cases marked sound (daw-loop-audio, daw-loop-restart, daw-follows-audio) run in a REAPER
of their own after the first, JACK only, its audio linked by this tool to AUX's sink (outputs
3/4) and from the line out (input 1), nothing else: REAPER plays a song (--material) into AUX
and records the line out, and each bar of its takes is found in the song (measure.find()).
They need numpy (measure.py's venv: ~/.venvs/frizz/bin/python reaper.py ...), else a SKIP.
What FRIZZ sends shows on the TRS jack (wiring A: the CHOMPI's TRS out into the interface's
MIDI in, FRIZZ's MIDI out on TRS: a clock coming in is passed on, a tick within its block) and
over USB, through REAPER's input.

REAPER's configuration is made in a temporary directory and deleted after (TMPDIR), the
license copied in from ~/.config/REAPER: the user's own in ~/.config/REAPER and ~/Apps/REAPER
is never written, and a run that finds a file there newer than its start fails. The audio:
jack (REAPER's JACK through pipewire-jack, a desktop DAW on this computer: the default) or
dummy (no audio device); its MIDI goes to ALSA's raw node either way (hw:FrizZ). The project
is silent, the master muted and REAPER's ports linked to nothing (jack_options=2; a run
finding a link fails), but for the sound cases' links above: nothing reaches Line 1/2. An evaluation copy shows its notice for a
few seconds at each start, which doesn't hold up the script. REAPER is this tool's own
process: one that runs no script within 20 s, or doesn't quit when asked, has a dialog in
the way, is killed and said so, never tried again.

Each case names the #55 item it covers, and is PASS, FAIL or SKIP (no REAPER, or a wiring it
can't show), as midisuite.py's; the summary in Markdown at the end (--out writes it to FILE
too), with the jitter's table. As midisuite.py's, the run stops where FRIZZ no longer keeps the
card out of it (it restarted), and exits 130, 143 or 129 after Ctrl-C, SIGTERM or SIGHUP,
things put back and REAPER closed first; 1 for a FAIL. --quick (the host check) shortens the
waits and skips what it wouldn't judge. FRIZZ_REAPER runs another REAPER (the host check's stand-in,
test/reaper_stub.py). The cases:
"""
import argparse
import atexit
import collections
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import traceback

try:
    import numpy as np
except ImportError:  # the audio cases say so; the others run without it
    np = None

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import chompi  # noqa: E402
import measure  # noqa: E402
import midiport  # noqa: E402
import midisuite  # noqa: E402
import remote  # noqa: E402
from midiport import START, CONTINUE, STOP, SPP  # noqa: E402
from midisuite import SOURCES, OUTS, WHITE_FX, EMPTY, PLAY, knob7, fit, count  # noqa: E402

HOME = os.path.expanduser("~")
USER_CONFIG = os.path.join(HOME, ".config", "REAPER")
INSTALL = os.path.join(HOME, "Apps", "REAPER")
LICENSE = "reaper-reginfo2.ini"
RUNNER = os.path.join(HERE, "reaper.lua")
STARTUP = 20.  # s for REAPER to run the script's first line
QUIT = 15.  # s for it to quit when asked
AUDIO_MODES = dict(jack=0, dummy=2)  # linux_audio_mode
# REAPER's "External timecode synchronization" is the project's (Project Settings, or the
# transport's right click), not reaper.ini's: its line in the project file, the project
# variables projsmptesync, projsmpterate, ... projsmpteinput in that order. 5: on (1), "Start
# playback on valid timecode when stopped" (4); 30 fps; re-synchronize past 100 ms; skip
# frames past 40 ms; seek 1000 ms ahead; freewheel 300 ms; input 4223: MIDI clock and Song
# Position (SPP) from all MIDI inputs (4159 would be MTC; the Ultraschall API's project file
# notes list them); the rest 0. reaper.lua opens it before anything else, and logs what REAPER
# has then ("sync").
SYNC = (5, 4223)
SYNC_PROJECT = """<REAPER_PROJECT 0.1 "7.0" 0 0
  TEMPO 120 4 4 0
  SMPTESYNC %d 30 100 40 1000 300 %d 0 0 0 0
>
""" % SYNC


def binary():
    """REAPER's executable: FRIZZ_REAPER, ~/Apps/REAPER/reaper, or reaper on PATH; None"""
    for path in (os.environ.get("FRIZZ_REAPER"), os.path.join(INSTALL, "reaper"), shutil.which("reaper")):
        if path and os.access(path, os.X_OK):
            return path
    return None


def lua(v):
    """A Python value as a Lua literal: numbers, strings, None, lists, dicts"""
    if v is None:
        return "nil"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(v)
    if isinstance(v, str):
        return '"%s"' % v.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
    if isinstance(v, dict):
        return "{" + ", ".join("%s = %s" % (k, lua(x)) for k, x in v.items() if x is not None) + "}"
    return "{" + ", ".join(lua(x) for x in v) + "}"


def card_id(node):
    """ALSA's id of the card a raw MIDI node is on (FrizZ), which REAPER names it by (hw:FrizZ);
    for the host check's pseudo-terminal, its path"""
    m = re.match(r".*/midiC(\d+)D", node or "")
    if not m:
        return node if node and os.environ.get("FRIZZ_REAPER") else None
    try:
        with open("/proc/asound/card%s/id" % m.group(1)) as f:
            return f.read().strip()
    except OSError:
        return None


def user_files_newer(stamp):
    """Files in the user's REAPER configuration and installation changed since STAMP"""
    got = []
    for top in (USER_CONFIG, INSTALL):
        for root, _, files in os.walk(top):
            for name in files:
                path = os.path.join(root, name)
                try:
                    if os.path.getmtime(path) > stamp:
                        got.append(path)
                except OSError:
                    pass
    return got


def jack_links():
    """REAPER's JACK audio ports' links, as pw-link lists them: [(from, to)], [] if none, or no
    pw-link (its MIDI ports are linked to FRIZZ's, through PipeWire's bridge)"""
    try:
        text = subprocess.run(["pw-link", "-l"], capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.TimeoutExpired):
        return []
    got, port = [], None
    for line in text.splitlines():
        if not line.startswith(" "):
            port = line.strip()
        elif port and port.startswith("REAPER:") and "MIDI" not in port:
            other = line.strip()[3:].strip()
            if line.strip().startswith("|->"):
                got.append((port, other))
            elif line.strip().startswith("|<-"):
                got.append((other, port))
    return got


def scarlett_routing(sink):
    """The audio interface's outputs that AUX's sink plays into, as its mixer has them (amixer,
    read only, never set): None if they're its PCM 3/4 as they should be (or it isn't a
    Scarlett: not checked), else what's wrong"""
    if "Scarlett" not in sink:
        return None
    try:
        with open("/proc/asound/cards") as f:
            card = next(l.split("[")[1].split("]")[0].strip() for l in f if "Scarlett" in l and "[" in l)
    except (OSError, StopIteration):
        return "no Scarlett in /proc/asound/cards"
    for out, want in (("03", "PCM 3"), ("04", "PCM 4")):
        try:
            text = subprocess.run(["amixer", "-c", card, "cget", "name=Analogue Output %s Playback Enum" % out],
                                  capture_output=True, text=True, timeout=5).stdout
        except (OSError, subprocess.TimeoutExpired) as e:
            return "amixer: %s" % e
        m = re.search(r": values=(\d+)", text)
        item = m and re.search(r"Item #%s '([^']*)'" % m.group(1), text)
        if not item or item.group(1) != want:
            return "its Analogue Output %s plays %s, not %s" % (out, item.group(1) if item else "?", want)
    return None


def sink_volume(sink):
    """The sink's volume in %, its channels' highest (pactl); None"""
    try:
        text = subprocess.run(["pactl", "get-sink-volume", sink], capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.TimeoutExpired):
        return None
    vals = [int(v) for v in re.findall(r"(\d+)%", text)]
    return max(vals) if vals else None


def audio_links():
    """REAPER's audio as the audio cases have it, and nothing else: its outputs 3/4 into AUX's
    sink (the interface's line outputs 3/4, never 1/2: the monitors), the line out's left
    (measure.py's source) into its input 1: [(from, to)]; a ReaperError where they aren't found"""
    try:
        sink, source = measure.aux_sink(), measure.line_source()
    except SystemExit as e:
        raise ReaperError(str(e))
    try:
        outs = subprocess.run(["pw-link", "-o"], capture_output=True, text=True, timeout=5).stdout.split()
    except (OSError, subprocess.TimeoutExpired) as e:
        raise ReaperError("pw-link: %s" % e)
    captures = [p for p in outs if p.startswith(source + ":capture")]
    if len(captures) != 1:
        raise ReaperError("%s has %d capture ports, not one" % (source, len(captures)))
    return [("REAPER:out3", sink + ":playback_FL"), ("REAPER:out4", sink + ":playback_FR"),
            (captures[0], "REAPER:in1")]


class ReaperError(Exception):
    pass


class LogTap(midiport.Tap):
    """A midiport.Tap fed from REAPER's log ("in" lines) instead of a node of its own"""

    def __init__(self):
        self.fd = None
        self.log = []
        self.answers = collections.deque()
        self.cond = threading.Condition()
        self.echo = None
        self.status, self.msg = None, []

    def close(self):
        pass


class Daw:
    """REAPER, running on a configuration of its own in a temporary directory, taking
    commands (reaper.lua's) until close(). audio: jack or dummy. node: FRIZZ's raw MIDI node,
    whose card REAPER sends its clock and the commands' bytes to, and whose input it logs
    (tap: what FRIZZ sends over USB, as it's read from the log); None for none"""

    def __init__(self, audio="jack", node=None, verbose=False, name=None, settings=None, sync=False, wired=False):
        """name: a MIDI device by REAPER's name for it instead (virtual: REAPER's own port);
        settings: more of reaper.ini's [reaper] keys, {key: value}; sync: REAPER following the
        clock that comes in (SYNC_PROJECT), its own clock out off; wired: its audio linked as
        audio_links() says (jack only), else to nothing"""
        self.audio, self.verbose, self.sync, self.wired = audio, verbose, sync, wired
        self.links = []
        # REAPER's own "Reset MIDI CC/Pitch on: playback stop / playback loop/skip" (on by
        # default: each CC a track sent goes back to 0) off, through "CC reset overrides"
        self.settings = dict(dict(ccresettab="cc0-120=-1"), **(settings or {}))
        self.exe = binary()
        if not self.exe:
            raise ReaperError("no REAPER (~/Apps/REAPER/reaper, FRIZZ_REAPER)")
        card = card_id(node) if node else None
        if node and not card:
            raise ReaperError("no sound card for %s" % node)
        # REAPER's names for it, out and in: ALSA's raw node (hw:FrizZ), or under JACK the
        # PipeWire bridge's ports, which it takes when it finds them
        if name:
            self.outs_named = self.ins_named = [name]
        elif card:
            # the one REAPER takes in this mode first, so it starts once
            self.outs_named = ["hw:%s" % card, "Midi-Bridge:%s: MIDI 1 (playback)" % card]
            self.ins_named = ["hw:%s" % card, "Midi-Bridge:%s: MIDI 1 (capture)" % card]
            if card.startswith("/"):  # the host check's pseudo-terminal, for its stand-in
                self.outs_named, self.ins_named = self.outs_named[:1], self.ins_named[:1]
            elif audio == "jack":
                self.outs_named.reverse()
                self.ins_named.reverse()
        else:
            self.outs_named = self.ins_named = []
        self.dir = tempfile.mkdtemp(prefix="frizz-reaper-")
        license_file = os.path.join(USER_CONFIG, LICENSE)
        if os.path.exists(license_file):
            shutil.copy(license_file, os.path.join(self.dir, LICENSE))
        os.makedirs(os.path.join(self.dir, "Scripts"))
        self.log_path = os.path.join(self.dir, "run.log")
        self.recdir = os.path.join(self.dir, "rec")
        os.makedirs(self.recdir)
        self.cmds_path = os.path.join(self.dir, "cmds.txt")
        self.tap = LogTap()
        self.lines = []  # (this computer's time when read, REAPER's time, what, rest)
        self.lock = threading.Condition()
        self.next_id = 0
        self.proc = None
        atexit.register(self.kill)  # never left running after this tool
        try:
            # where REAPER numbers the device: the caches seeded with it first, else as it says
            self.out_name, self.in_name = (self.outs_named + [None])[0], (self.ins_named + [None])[0]
            self.out_index = self.in_index = 0 if self.out_name else None
            self.start()
            if self.errors() and self.out_name:
                outs, ins = self.devices()
                self.stop()
                self.out_name = next((n for n in self.outs_named if n in outs), None)
                self.in_name = next((n for n in self.ins_named if n in ins), None)
                if self.out_name is None or self.in_name is None:
                    raise ReaperError("REAPER lists no %s" % " or ".join(
                        self.outs_named if self.out_name is None else self.ins_named))
                self.out_index, self.in_index = outs[self.out_name], ins[self.in_name]
                self.start()
                if self.errors():
                    raise ReaperError("REAPER's devices: %s" % "; ".join(self.errors()))
            self.wire()
        except BaseException:
            self.close()
            raise

    def wire(self):
        """REAPER's audio linked as it should be (wired: audio_links(), else nothing), checked;
        its synchronization as wanted"""
        if self.audio == "jack":
            if self.wired:
                if os.environ.get("FRIZZ_REAPER"):  # the host check's stand-in: no JACK
                    self.links = [("REAPER:out3", "aux"), ("REAPER:out4", "aux"), ("line", "REAPER:in1")]
                else:
                    self.links = audio_links()
                    wrong = scarlett_routing(self.links[0][1])
                    if wrong:
                        raise ReaperError("AUX's outputs: %s (left as it is)" % wrong)
                    for a, b in self.links:
                        if subprocess.run(["pw-link", a, b], capture_output=True, timeout=5).returncode:
                            raise ReaperError("pw-link %s %s failed" % (a, b))
                    self.check_links()
            else:
                self.check_links()
        self.sync_info = next((r for _, _, w, r in reversed(self.lines) if w == "sync"), "?")
        # (REAPER doesn't give midiouts_clock to a script: reaper.ini's, written above, says it)
        if self.sync and self.sync_info.split()[:2] != [str(v) for v in SYNC]:
            raise ReaperError("REAPER's synchronization: %s, not %d %d" % ((self.sync_info,) + SYNC))

    def check_links(self):
        """REAPER's audio linked as self.links, nothing else (nothing to Line 1/2): a ReaperError"""
        if os.environ.get("FRIZZ_REAPER"):
            return
        got = set(jack_links())
        if got != set(self.links):
            raise ReaperError("REAPER's ports are linked: %s%s" % (
                "; ".join("%s -> %s" % l for l in sorted(got)) or "nowhere",
                ", not %s" % "; ".join("%s -> %s" % l for l in self.links) if self.links else ""))

    def restart(self, sync):
        """REAPER quit and started again, synchronized to its MIDI input or not, wired as it
        was: FRIZZ keeps what it has (its loop)"""
        self.stop()
        self.sync = sync
        self.start()
        if self.errors():
            raise ReaperError("REAPER's devices: %s" % "; ".join(self.errors()))
        self.wire()

    # ---- REAPER's process ----
    def write_config(self):
        bit = lambda i: 0 if i is None else 1 << i  # noqa: E731
        mask = bit(self.out_index)
        cfg = [
            "[reaper]",
            "linux_audio_mode=%d" % AUDIO_MODES[self.audio],
            "linux_audio_srate=48000", "linux_audio_srateor=1", "linux_audio_bsize=512",
            # 4 out: a media track goes to 3/4 alone, the master (muted) to 1/2, linked to nothing
            "linux_audio_bufs=3", "linux_audio_nch_in=2", "linux_audio_nch_out=4",
            "linux_audio_bits=32", "alsa_indev=frizz_none", "alsa_outdev=frizz_none",
            "jack_options=2",  # REAPER links its ports to nothing by itself
            # synced, REAPER sends no clock, Start, Stop or Song Position: only the commands' bytes
            "midiouts=%d" % mask, "midiouts_clock=%d" % (0 if self.sync else mask), "midiouts_noreset=%d" % mask,
            "midiins=%d" % bit(self.in_index),
            "splashlog=0", "lastproject=", "projecttab1=",
            "promptendrec=0",  # a recording stopped: no dialog asking what to do with its files
        ] + ["%s=%s" % kv for kv in sorted(self.settings.items())] + [
            "",
            "[verchk]", "lastt=%d" % int(time.time()), "",
        ]
        with open(os.path.join(self.dir, "reaper.ini"), "w") as f:
            f.write("\n".join(cfg) + "\n")
        # the device caches (ALSA's raw MIDI, JACK's): its names first, so REAPER numbers them 0
        for name, bridge in (("reaper-midihw-alsa.ini", False), ("reaper-midihw-linux.ini", True)):
            cache = ["[mididevcache]"]
            if self.outs_named:
                pick = lambda names: next((n for n in names if n.startswith("Midi-Bridge:") == bridge),  # noqa: E731
                                          names[0])
                cache += ["ot0=1", "on0=" + pick(self.outs_named), "it0=1", "in0=" + pick(self.ins_named)]
            with open(os.path.join(self.dir, name), "w") as f:
                f.write("\n".join(cache) + "\n")
        plan = dict(log=self.log_path, cmds=self.cmds_path, project=os.path.join(self.dir, "run.rpp"),
                    out=self.out_index, out_name=self.out_name, input=self.in_index, in_name=self.in_name,
                    recdir=self.recdir)
        if self.sync:
            plan["sync"] = os.path.join(self.dir, "sync.rpp")
            with open(plan["sync"], "w") as f:
                f.write(SYNC_PROJECT)
        with open(RUNNER) as f:
            runner = f.read()
        with open(os.path.join(self.dir, "Scripts", "__startup.lua"), "w") as f:
            f.write("PLAN = " + lua(plan) + "\n" + runner)

    def start(self):
        self.write_config()
        for path in (self.log_path, self.cmds_path):
            if os.path.exists(path):
                os.remove(path)
        open(self.cmds_path, "w").close()
        with self.lock:
            self.lines = []
        self.next_id = 0
        self.first_line = None
        self.stamp = time.time()
        cmd = [self.exe, "-cfgfile", os.path.join(self.dir, "reaper.ini"),
               "-nosplash", "-new", "-newinst", "-ignoreerrors"]
        if self.audio == "jack" and shutil.which("pw-jack") and not os.environ.get("FRIZZ_REAPER"):
            cmd = ["pw-jack"] + cmd
        if shutil.which("setpriv"):
            # killed with this tool, however it ends: REAPER left running would keep FRIZZ's
            # USB node from everyone (pw-jack execs, so REAPER keeps it)
            cmd = ["setpriv", "--pdeathsig", "KILL"] + cmd
        self.stdout = open(os.path.join(self.dir, "stdout.log"), "w")
        launched = time.monotonic()
        self.proc = subprocess.Popen(cmd, stdout=self.stdout, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, env=dict(os.environ, LC_ALL="C"),
                                     start_new_session=True)
        self.reader = threading.Thread(target=self.follow, daemon=True)
        self.reader.start()
        if not self.wait_line(lambda w, r: w in ("listening", "done"), STARTUP):
            self.kill()
            raise ReaperError("REAPER ran no script in %.0f s: a dialog in the way?%s" % (STARTUP, self.tail()))
        self.started = (self.first_line or launched) - launched
        self.audio_info = next((r for _, _, w, r in self.lines if w == "audio"), "?")

    def follow(self):
        """Reads REAPER's log as it grows, into lines; "in" lines into the tap"""
        while not os.path.exists(self.log_path):
            if self.proc.poll() is not None:
                return
            time.sleep(.01)
        self.first_line = time.monotonic()
        with open(self.log_path) as f:
            buf = ""
            while True:
                chunk = f.read()
                if chunk:
                    buf += chunk
                    now = time.perf_counter()
                    *whole, buf = buf.split("\n")
                    for line in whole:
                        self.line(now, line)
                elif self.proc.poll() is not None:
                    return
                else:
                    time.sleep(.005)

    def line(self, now, text):
        parts = text.split(" ", 2)
        if len(parts) < 2:
            return
        t, what, rest = float(parts[0]), parts[1], parts[2] if len(parts) > 2 else ""
        if what == "in":
            try:
                self.tap.feed(bytes.fromhex(rest), now)
            except ValueError:
                pass
        if self.verbose and what not in ("in", "pos"):
            print("    reaper: %s" % text, flush=True)
        with self.lock:
            self.lines.append((now, t, what, rest))
            self.lock.notify_all()

    def wait_line(self, match, timeout, since=0):
        """The first line (now, t, what, rest) from index since on that match(what, rest),
        waiting up to timeout; None"""
        end = time.monotonic() + timeout
        with self.lock:
            i = since
            while True:
                while i < len(self.lines):
                    line = self.lines[i]
                    if match(line[2], line[3]):
                        return line
                    i += 1
                left = end - time.monotonic()
                if left <= 0 or (self.proc.poll() is not None and not self.reader.is_alive()):
                    return None
                self.lock.wait(min(left, .1))

    def errors(self):
        got = [r for _, _, w, r in self.lines if w == "error"]
        try:
            with open(os.path.join(self.dir, "stdout.log")) as f:
                got += [line.strip() for line in f if "error opening" in line]
        except OSError:
            pass
        return got

    def devices(self):
        outs, ins = {}, {}
        for _, _, w, r in self.lines:
            if w == "dev":
                kind, index, name = r.split(" ", 2)
                (outs if kind == "out" else ins)[name] = int(index)
        return outs, ins

    def tail(self, n=12):
        return "".join("\n    %.3f %s %s" % (t, w, r) for _, t, w, r in self.lines[-n:])

    def kill(self):
        if self.proc and self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGTERM)
            try:
                self.proc.wait(3)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait()

    def stop(self):
        """Asks REAPER to quit; kills it if it doesn't: a ReaperError then"""
        if not self.proc or self.proc.poll() is not None:
            return
        self.cmd("quit")
        try:
            self.proc.wait(QUIT)
        except subprocess.TimeoutExpired:
            self.kill()
            raise ReaperError("REAPER didn't quit in %.0f s: a dialog in the way?%s" % (QUIT, self.tail()))
        finally:
            self.reader.join(2)
            self.stdout.close()
            touched = user_files_newer(self.stamp)
            if touched:
                raise ReaperError("REAPER wrote the user's own configuration: %s" % ", ".join(touched))

    def close(self):
        try:
            self.stop()
        finally:
            self.kill()
            shutil.rmtree(self.dir, ignore_errors=True)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # ---- commands ----
    def cmd(self, *words, wait=False, timeout=3.):
        """A command (reaper.lua's) to REAPER; wait: until it's carried out, its (time this
        computer read it, REAPER's position in s, in beats, play state)"""
        self.next_id += 1
        cid = str(self.next_id)
        mark = len(self.lines)
        with open(self.cmds_path, "a") as f:
            f.write(" ".join([cid] + [str(w) for w in words]) + "\n")
        if not wait:
            return None
        got = self.wait_line(lambda w, r: w == "cmd" and r.split(" ", 1)[0] == cid, timeout, mark)
        if got is None:
            raise ReaperError("REAPER didn't carry out %s in %g s%s" % (
                " ".join(map(str, words))[:60], timeout, self.tail()))
        m = re.search(r"pos (\S+) (\S+) state (\d+)", got[3])
        return got[0], float(m.group(1)), float(m.group(2)), int(m.group(3))

    def send(self, data):
        self.cmd("send", bytes(data).hex())

    def play(self):
        return self.cmd("play", wait=True)

    def stop_transport(self):
        return self.cmd("stop", wait=True)

    def locate(self, beat):
        return self.cmd("locate", "%g" % beat, wait=True)

    def tempo(self, bpm):
        return self.cmd("tempo", "%g" % bpm, wait=True)

    def loop(self, a=None, b=None):
        return self.cmd("loop", "off", wait=True) if a is None else self.cmd("loop", "%g" % a, "%g" % b, wait=True)

    def lane(self, length, events):
        """A track with these CCs ((beat, status, data1, data2)) over LENGTH beats from 0"""
        return self.cmd("lane", "%g" % length, *["%g,%d,%d,%d" % e for e in events], wait=True)

    def monitor(self):
        return self.cmd("monitor", wait=True)

    def clear(self):
        return self.cmd("clear", wait=True)

    def trace(self, on):
        return self.cmd("trace", "on" if on else "off", wait=True)

    def media(self, path, beats):
        """A track playing the WAV at path from 0, looped over beats, to outputs 3/4 alone"""
        return self.cmd("media", path, "%g" % beats, wait=True)

    def rec(self):
        """A track recording input 1, armed, monitoring off, sent nowhere"""
        return self.cmd("rec", wait=True)

    def record(self):
        return self.cmd("record", wait=True)

    def takes(self):
        """The takes recorded so far: [(position on the time line s, offset into the file s,
        length s, file)]"""
        mark = len(self.lines)
        self.cmd("takes", wait=True)
        with self.lock:
            got = [r.split(" ", 3) for _, _, w, r in self.lines[mark:] if w == "take"]
        return [(float(a), float(b), float(c), f) for a, b, c, f in got]

    def traced(self, t0, t1=None):
        """The play position traced between t0 and t1: (this computer's time, s, beats, play
        state, play rate, tempo)"""
        got = []
        with self.lock:
            for now, _, w, r in self.lines:
                if w == "pos" and now >= t0 and (t1 is None or now <= t1):
                    v = r.split()
                    got.append((now, float(v[0]), float(v[1]), int(v[3]),
                                float(v[5]) if len(v) > 5 else 1., float(v[7]) if len(v) > 7 else 0.))
        return got

    def positions(self, t0, t1=None):
        """(this computer's time, REAPER's position in beats) traced between t0 and t1"""
        with self.lock:
            return [(now, float(r.split()[1])) for now, _, w, r in self.lines
                    if w == "pos" and now >= t0 and (t1 is None or now <= t1)]


def frizz_through(daw):
    """A remote.Frizz whose bytes go out through REAPER and whose answers come back from its
    input"""
    f = remote.Frizz.__new__(remote.Frizz)
    f.write_lock = threading.Lock()
    f.query_lock = threading.Lock()
    f.held = set()
    f.raw = daw.send
    f.link = midiport.TapLink(daw.tap)
    return f


# ======== the cases ========

CASES = []


def case(id, item, needs=(), every=False, last=False, synced=False, sound=False):
    """every: in each REAPER (audio mode), not only the first; last: only in the last, when it
    isn't the first, which runs with REAPER's preferences as they come; synced: in a REAPER of
    its own after them, following the clock that comes in (Daw's sync); sound: in a REAPER of
    its own with its audio wired (Daw's wired: jack only), after the first"""
    def add(fn):
        c = midisuite.Case(id, item, fn, needs)
        c.every, c.last, c.synced, c.sound = every, last, synced, sound
        CASES.append(c)
        return fn
    return add


def poll_tempo(r, seconds, every=.2):
    """FRIZZ's tempo every 0.2 s for seconds"""
    got = []
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        t = time.monotonic()
        got.append(r.tempo())
        time.sleep(max(0., t + every - time.monotonic()))
    return got


def changes(vals):
    return sum(1 for a, b in zip(vals, vals[1:]) if a != b)


def held(what, ok, got, want, r):
    """A tempo holding still: checked on the device; --quick (the host check) only notes it, as
    the stand-in's clock jitters under all.sh's load enough to flip it (#83), as midisuite.py's
    host check leaves fast-usb out"""
    if r.args.quick:
        r.note("%s: %s (want %s; not judged with --quick)" % (what, got, want))
    else:
        r.tcheck(what, ok, got, want)


def sleep_until(t):
    time.sleep(max(0., t - time.perf_counter()))


@case("daw-tempo", "7: REAPER's clock over USB at 120, 200 and 300 BPM, played after FRIZZ is "
      "on: the tempo follows the DAW's, not half of it, and holds (#93, #74); with MIDI out on "
      "TRS, its clock, Start and Stop passed on (#73)")
def daw_tempo(r, d):
    out = "out" in r.wiring
    r.setting(clock_source=SOURCES["usb"], midi_out=OUTS["trs"] if out else 0)
    for bpm in (120, 200, 300):
        d.locate(0)
        d.tempo(bpm)
        t0 = time.perf_counter()
        d.play()
        took, got = r.wait_tempo(bpm, 1, 6)
        r.tcheck("REAPER at %d BPM: FRIZZ's tempo" % bpm, took is not None, got, "%d +-1 within 6 s" % bpm)
        time.sleep(1. if r.args.quick else 3.)
        vals = poll_tempo(r, 2. if r.args.quick else 4.)
        held("then holds", max(vals) - min(vals) <= .5, "%.1f..%.1f" % (min(vals), max(vals)), "one whole BPM", r)
        t1 = time.perf_counter()
        d.stop_transport()
        time.sleep(.4)
        if out:
            log = r.trs.log
            p, _ = fit(midisuite.ticks_after(log, t0 + 1.5, t1))
            r.tcheck("its clock passed on out of TRS", p and abs(midisuite.bpm_of(p) - bpm) <= bpm * .01 * r.loose,
                     "%.2f BPM" % midisuite.bpm_of(p) if p else "none", "%d +-%g %%" % (bpm, r.loose))
            starts = count(log, START, t0, t1) + count(log, CONTINUE, t0, t1)
            r.check("Start, then Stop passed on", starts == 1 and count(log, STOP, t1) >= 1,
                    "%d Start, %d Continue, %d Stop" % (count(log, START, t0, t1), count(log, CONTINUE, t0, t1),
                                                       count(log, STOP, t1)), "1 Start or Continue, a Stop")


def pos_after(r, t, beats, beat=.5):
    """FRIZZ's loop position (0-1) beats after t"""
    sleep_until(t + beats * beat)
    return r.state()


@case("daw-transport", "7: REAPER's Start / Song Position, transport following on: a loop "
      "recorded to its clock goes back to its start on play-from-start; the playhead moved "
      "while stopped, or while playing, carries on there; a loop range keeps it with the song; "
      "a free loop just plays on (#108)")
def daw_transport(r, d):
    r.setting(clock_source=SOURCES["usb"], midi_transport=1)
    d.tempo(120)
    d.locate(0)
    d.play()
    r.wait_tempo(120, 1, 6)
    r.record_loop(2, 120)  # 8 beats, 4 s, on REAPER's bar line
    d.stop_transport()
    time.sleep(.4)
    s = r.state()
    r.check("REAPER stopped: paused", s["looper"] == 3, remote.LOOPER[s["looper"]], "paused")
    d.locate(0)
    s = pos_after(r, d.play()[0], 2)
    r.tcheck("REAPER plays from the start: the loop from its start", s["looper"] == 2 and abs(s["pos"] - .25) <= .08,
            "%s at %.2f 2 beats later" % (remote.LOOPER[s["looper"]], s["pos"]), "playing at 0.25 +-0.08")
    d.stop_transport()
    time.sleep(.3)
    d.locate(5)
    time.sleep(.3)
    s = pos_after(r, d.play()[0], 2)
    r.tcheck("stopped, the playhead to beat 5 (from 0), play: carries on there",
            s["looper"] == 2 and abs(s["pos"] - 7 / 8.) <= .08,
            "%s at %.2f 2 beats later" % (remote.LOOPER[s["looper"]], s["pos"]), "playing at 0.88 +-0.08")
    time.sleep(1.)
    s = pos_after(r, d.locate(2)[0], 2)
    r.tcheck("playing, the playhead to beat 2: carries on there", s["looper"] == 2 and abs(s["pos"] - .5) <= .08,
            "%s at %.2f 2 beats later" % (remote.LOOPER[s["looper"]], s["pos"]), "playing at 0.50 +-0.08")
    # a loop range of beats 4-8: the second half of FRIZZ's loop, over and over
    d.stop_transport()
    d.loop(4, 8)
    d.locate(4)
    d.trace(True)
    d.play()
    time.sleep(.5)
    t0 = time.perf_counter()
    got = []
    while time.perf_counter() - t0 < (4.5 if r.args.quick else 8.5):
        got.append(r.state()["pos"])
    d.trace(False)
    d.stop_transport()
    d.loop()
    traced = d.positions(t0)
    wraps = sum(1 for (_, a), (_, b) in zip(traced, traced[1:]) if b < a - 1.)
    off = [p for p in got if not (.5 - .06 <= p or p <= .06)]
    r.check("a loop range of beats 4-8 (%d wraps): FRIZZ's loop in its second half" % wraps,
            wraps >= 1 and not off,
            "%d readings, %d outside%s" % (len(got), len(off), ": " + " ".join("%.2f" % p for p in off[:6]) if off else ""),
            "0.44..1.06, a wrap at least")
    # a free loop just plays on
    r.erase()
    r.record_loop(quantized=False, seconds=2.)
    time.sleep(.9)
    before = r.state()["pos"]
    d.locate(0)
    d.play()
    time.sleep(.5)
    s = r.state()
    moved = (s["pos"] - before) % 1.
    r.tcheck("a free loop, REAPER plays from the start", s["looper"] == 2 and .1 <= moved <= .7,
            "%s, %.2f on" % (remote.LOOPER[s["looper"]], moved), "plays on")
    d.stop_transport()


# the channels whose lanes latch their effect too: a few, as every effect at once is the
# heaviest load there is (on the CHOMPI and the twin); midisuite.py's cc-channels latches each
LATCHED = (1, 7, 14)


def automation_value(ch, k):
    """A value of its own for channel ch's CC 14 + k, away from 0, 64 and 127"""
    return 8 + (ch * 7 + k * 13) % 110


@case("daw-automation", "8: REAPER automation per effect channel: a track per effect on channels "
      "1-15, CC 14-21 move that effect's knobs, CC 22 latches it (#108)")
def daw_automation(r, d):
    groups = WHITE_FX[1:] + ["comp"]

    def read():
        return {fx: [remote.knob(v) for v in r.get(fx)] for fx in groups}, r.get("latch")

    def status(ch):
        return 0xB0 | (ch - 1)

    d.clear()
    for ch in range(1, 16):
        events = []
        for k in range(8):
            v = automation_value(ch, k)
            events += [(0., status(ch), 14 + k, 0), (.5, status(ch), 14 + k, v // 2), (1., status(ch), 14 + k, v)]
        if ch in LATCHED:
            events.append((1.25, status(ch), 22, 127))
        d.lane(4, events)
    d.tempo(120)
    d.locate(0)
    before, _ = read()
    d.play()
    time.sleep(1.2)  # the last values at 0.5 s, the latches at 0.63; the lanes go on to 2 s
    played, latches = read()
    d.stop_transport()
    time.sleep(.3)
    stopped, _ = read()
    d.clear()
    # the same values sent as CCs straight away: where each knob should be
    for ch in range(1, 16):
        d.send(b"".join(bytes([status(ch), 14 + k, automation_value(ch, k)]) for k in range(8)))
    time.sleep(.3)
    direct, _ = read()
    bad, kept, n, moved = [], [], 0, 0
    for ch in range(1, 16):
        fx = groups[ch - 1]
        for k in range(8):
            if (fx, k) in EMPTY:
                continue
            n += 1
            moved += abs(played[fx][k] - before[fx][k]) > .01
            if abs(played[fx][k] - direct[fx][k]) > .002:
                bad.append("ch %d CC %d: %.3f, sent: %.3f" % (ch, 14 + k, played[fx][k], direct[fx][k]))
            if abs(stopped[fx][k] - played[fx][k]) > .002:
                kept.append("ch %d CC %d: %.3f -> %.3f" % (ch, 14 + k, played[fx][k], stopped[fx][k]))
        if ch < 15 and (ch in LATCHED) != bool(latches[remote.FX_NAMES.index(fx)]):
            bad.append("ch %d: %slatched" % (ch, "not " if ch in LATCHED else ""))
    r.check("15 tracks playing: each knob where its lane's last CC, sent straight, puts it; CC 22 latched "
            "on channels %s" % ", ".join(map(str, LATCHED)),
            not bad, "; ".join(bad[:6]) + (" ..." if len(bad) > 6 else "") if bad else "all %d knobs, %d latches" % (n, len(LATCHED)),
            "the same")
    r.check("the lanes moved them", moved >= n * .8, "%d of %d" % (moved, n), "most")
    r.check("REAPER stopped, its CC reset off (CC reset overrides cc0-120=-1): the knobs stay", not kept,
            "; ".join(kept[:4]) + (" ..." if len(kept) > 4 else "") if kept else "all %d" % n, "all stay")


@case("daw-scenes", "8: #175 from REAPER's tracks on channel 16: CC 83 + CC 9 a drop; CC 3 during it "
      "stops it and morphs instead; CC 83 during a morph a drop of its own; CC 81 stops one")
def daw_scenes(r, d):
    midisuite.two_scenes(r)
    g = 0xB0 | (midisuite.GLOBAL - 1)
    A = midisuite.A

    def playing(events, seconds):
        d.clear()
        d.lane(4, events)
        d.tempo(120)
        d.locate(0)
        d.play()
        time.sleep(seconds)

    def shown(s):
        return "scene %d%s" % (s["scene"], ", morphing" if s["morphing"] else "")

    # just after beat 0: a drop to scene 2 over 2 bars (CC 9 2, CC 83 2), so none lands before
    # it's looked at; beat 1 (0.5 s): CC 3 1, a morph to scene 1
    playing([(.1, g, 9, 2), (.1, g, 83, 127), (.12, g, 83, 2), (1., g, 3, 127), (1.02, g, 3, 1)], .3)
    s = r.state()
    r.check("beat 0, CC 83 2: a drop to scene 2 waits", s["morphing"] and s["scene"] == 2, shown(s), "scene 2, morphing")
    time.sleep(.5)
    s = r.state()
    r.check("beat 1, CC 3 1: a morph to scene 1 instead", s["morphing"] and s["scene"] == 1, shown(s), "scene 1, morphing")
    d.stop_transport()
    s, k = midisuite.landed(r), r.knob("filter", 0)
    r.check("it lands on scene 1, no drop to 2", abs(k - A) <= .01 and not s["morphing"],
            "%.3f%s" % (k, ", morphing" if s["morphing"] else ""), "%.1f, scene 1's" % A)
    # beat 0: a morph to scene 2; beat 1: CC 83 1, a drop to scene 1
    playing([(.1, g, 3, 127), (.12, g, 3, 2), (1., g, 83, 127), (1.02, g, 83, 1)], .3)
    s = r.state()
    r.check("beat 0, CC 3 2: a morph to scene 2", s["morphing"] and s["scene"] == 2, shown(s), "scene 2, morphing")
    time.sleep(.5)
    s = r.state()
    r.check("beat 1, CC 83 1 during the morph to 2: a drop to scene 1", s["morphing"] and s["scene"] == 1,
            shown(s), "scene 1, morphing")
    d.stop_transport()
    s, k = midisuite.landed(r), r.knob("filter", 0)
    r.check("it lands on scene 1", abs(k - A) <= .01 and not s["morphing"],
            "%.3f%s" % (k, ", morphing" if s["morphing"] else ""), "%.1f, scene 1's" % A)
    # beat 0: a morph to scene 2; beat 1: CC 81 up through 64
    playing([(.1, g, 3, 127), (.12, g, 3, 2), (1., g, 81, 0), (1.02, g, 81, 127), (1.2, g, 81, 0)], .9)
    d.stop_transport()
    s = r.state()
    r.check("beat 1, CC 81: stopped where it is, scene 2 edited",
            not s["morphing"] and s["scene"] == 2 and s["flags"] & 1,
            shown(s) + (", edited" if s["flags"] & 1 else ""), "scene 2, edited, no morph")


@case("daw-ccreset", "8 (for MANUAL): REAPER as it comes sends each CC its tracks used back to 0 "
      "when it stops (\"Reset MIDI CC/Pitch on: playback stop\"), so the knobs automated go to 0; "
      "the other cases have it off", last=True)
def daw_ccreset(r, d):
    d.clear()
    d.lane(4, [(0., 0xB4, 14, 0), (.5, 0xB4, 14, 100)])  # CC 14 on 5: the filter's knob 1
    d.tempo(120)
    d.locate(0)
    d.play()
    time.sleep(1.)
    played = r.knob("filter", 0)
    d.stop_transport()
    time.sleep(.3)
    stopped = r.knob("filter", 0)
    r.check("CC 14 on 5 from a track at 100, REAPER playing: the filter's knob 1", abs(played - knob7(100)) <= .002,
            "%.3f" % played, "%.3f" % knob7(100))
    r.check("REAPER stopped, its preferences as they come: back to 0", stopped <= .002, "%.3f" % stopped, "0")


@case("daw-input", "8: REAPER automation on channel 16 sets the input (#177): CC 116 AUX dry, then "
      "CC 84 the mic and CC 117 it live, in that order; REAPER stopped (CC reset off), it stays; "
      "feedback sends what the hand changes")
def daw_input(r, d):
    GLOBAL = midisuite.GLOBAL
    r.setting(midi_out=OUTS["all"], midi_feedback=1)
    d.clear()
    d.send(midisuite.cc(GLOBAL, 84, 0))  # AUX: the cable is in on the device
    time.sleep(.3)
    start = midisuite.input_bits(r)
    d.lane(4, [(.1, 0xBF, 116, 127), (.3, 0xBF, 84, 64), (.5, 0xBF, 117, 127)])  # 64: the mic's third (#196)
    d.tempo(120)
    d.locate(0)
    d.play()
    time.sleep(1.2)
    played = midisuite.input_bits(r)
    d.stop_transport()
    time.sleep(.5)
    stopped = midisuite.input_bits(r)
    d.clear()
    d.send(midisuite.cc(GLOBAL, 117, 0) + midisuite.cc(GLOBAL, 84, 0))  # the mic off again, AUX
    time.sleep(.3)
    aux = midisuite.input_bits(r)
    r.check("AUX through the effects, live at first", start == 4, start, 4)
    r.check("the lane played: the mic (its own effects), live", played & 1 and played & 4, played, "the mic, live: 5 or 7")
    r.check("REAPER stopped: the input as the lane left it", stopped == played, stopped, played)
    r.check("CC 117 at 0, CC 84 at 0: the mic off, AUX dry as CC 116 left it, live", aux == 6, aux, 6)
    d.clear()


@case("daw-monitor", "8: REAPER recording what it also sends (a track monitoring FRIZZ's input "
      "onto its output), MIDI out and feedback on: automation that plays back into FRIZZ doesn't "
      "come back and double up; a knob turned goes out, nothing loops (#109)")
def daw_monitor(r, d):
    r.setting(midi_out=OUTS["all"], midi_feedback=1)
    d.clear()
    d.monitor()
    d.lane(4, [(i / 8., 0xB4, 14, i * 4) for i in range(32)])  # CC 14 on 5, the filter's, swept
    usb = r.usb.log
    t0 = time.perf_counter()
    d.locate(0)
    d.play()
    time.sleep(2.5)
    d.stop_transport()
    time.sleep(.3)
    back = [m.hex() for t, m in usb if t >= t0 and m[0] & 0xF0 == 0xB0]
    r.check("CC 14 on 5 swept from a track: CCs from FRIZZ over USB", not back,
            "%d: %s" % (len(back), " ".join(back[:6])) if back else "none", "none")
    t1 = time.perf_counter()
    r.turn(midisuite.KNOB1_ENC, -6)  # down: the freezer's knob 1 starts at its top
    time.sleep(1.)
    t2 = time.perf_counter()
    time.sleep(1.)
    sent = sum(1 for t, m in usb if t1 <= t < t2 and m[0] & 0xF0 == 0xB0)
    later = sum(1 for t, m in usb if t >= t2 and m[0] & 0xF0 == 0xB0)
    r.check("a knob turned: its CCs out over USB (and monitored back)", sent >= 1, "%d CCs" % sent, ">= 1")
    r.check("then, in the second after: nothing loops", later == 0, "%d CCs" % later, 0)
    d.clear()


def loop_beats(r, tempo):
    """The loop's length in whole beats at its tempo (PROBE's length: a free loop counts none)"""
    return round(chompi.get7(remote.dev_data(r.f, chompi.PROBE, [1], "PROBE")[2:6]) / 48000. * tempo / 60.)


def line_of(points):
    """The line through (t, y) points: (slope, its y at t = 0, the farthest a point is from it);
    None for fewer than 3"""
    n = len(points)
    if n < 3:
        return None
    mt, my = sum(t for t, _ in points) / n, sum(y for _, y in points) / n
    var = sum((t - mt) ** 2 for t, _ in points)
    if var <= 0:
        return None
    k = sum((t - mt) * (y - my) for t, y in points) / var
    return k, my - k * mt, max(abs(y - (my + k * (t - mt))) for t, y in points)


def followed(d, t0, t1):
    """REAPER's beats while it played between t0 and t1: (its tempo, the line, the readings), the
    line as line_of's over this computer's time; None"""
    pts = [(now, b) for now, _, b, state, _, _ in d.traced(t0, t1) if state & 1]
    line = line_of(pts)
    return (line[0] * 60., line, pts) if line else None


def first_after(log, byte, t0, timeout=0.):
    """The first message starting with byte at or after t0, waiting up to timeout: (time,
    message), or (None, None)"""
    end = time.perf_counter() + timeout
    while True:
        got = next(((t, m) for t, m in list(log) if t >= t0 and m[0] == byte), (None, None))
        if got[0] is not None or time.perf_counter() >= end:
            return got
        time.sleep(.02)


def playing_from(d, t0, timeout=4.):
    """When REAPER's trace first shows it playing (or not, playing=False) from t0 on; None"""
    end = time.perf_counter() + timeout
    while time.perf_counter() < end:
        got = next((now for now, _, _, state, _, _ in d.traced(t0) if state & 1), None)
        if got is not None:
            return got
        time.sleep(.05)
    return None


def moves_from(d, t0, timeout=4.):
    """When REAPER's position, playing, first moves on from t0 on; None"""
    end = time.perf_counter() + timeout
    while time.perf_counter() < end:
        pts = [(now, b) for now, _, b, state, _, _ in d.traced(t0) if state & 1]
        got = next((now for now, b in pts if b > pts[0][1] + .02), None) if pts else None
        if got is not None:
            return got
        time.sleep(.05)
    return None


def stopped_from(d, t0, timeout=4.):
    """When REAPER's trace first shows it not playing (stopped, or paused: synced, a Stop
    pauses it) from t0 on; None"""
    end = time.perf_counter() + timeout
    while time.perf_counter() < end:
        got = next((now for now, _, _, state, _, _ in d.traced(t0) if not state & 1), None)
        if got is not None:
            return got
        time.sleep(.05)
    return None


FOLLOW_BPM = 100  # FRIZZ's tempo, tapped: away from REAPER's own 120
SETTLE = 2.  # s from REAPER playing to judging it: it holds still a second first (seeking ahead)


@case("daw-follows", "7: MIDI out on TRS and USB into a DAW synced to external clock: the DAW "
      "follows the loop (#73): REAPER synced to FRIZZ's clock and Song Position (its own clock "
      "out off) starts with the loop on its beat 1, plays at its tempo and in step with it, "
      "pauses with FRIZZ, goes on where PLAY's Song Position says, follows the clock factor",
      synced=True)
def daw_follows(r, d):
    r.setting(clock_source=SOURCES["internal"], midi_out=OUTS["all"], clock_factor=100)
    usb, trs = r.usb.log, r.trs.log if r.trs else None
    d.tempo(120)
    d.locate(0)
    d.trace(True)
    midisuite.tap_tempo(r, FOLLOW_BPM)
    took, tempo = r.wait_tempo(FOLLOW_BPM, 1, 3)
    r.check("FRIZZ tapped to %d BPM (REAPER's project at 120)" % FOLLOW_BPM, took is not None, tempo,
            "%d +-1" % FOLLOW_BPM)
    time.sleep(1.)
    t0 = time.perf_counter()
    early = playing_from(d, t0, 1.5)
    r.note("FRIZZ's clock out, no loop, no Start: REAPER %s" % (
        "plays %.2f s in" % (early - t0) if early else "waits"))
    if early:
        d.stop_transport()
    # a free loop of 4 beats at FRIZZ's tempo
    t0 = time.perf_counter()
    r.record_loop(quantized=False, seconds=4 * 60. / FOLLOW_BPM)
    tempo = r.tempo()
    beats = loop_beats(r, tempo)
    t_start, _ = first_after(usb, START, t0, 1.)
    r.check("the loop plays: Start over USB", t_start is not None, "after %.2f s" % (t_start - t0) if t_start else "none",
            "Start")
    if t_start is None:
        return
    began = playing_from(d, t_start)
    r.check("REAPER starts playing", began is not None,
            "%.0f ms after FRIZZ's Start" % ((began - t_start) * 1000) if began else "not in 4 s", "it plays")
    if began is None:
        return
    moving = moves_from(d, began)
    r.note("REAPER plays %.0f ms after FRIZZ's Start (its trace's every ~33 ms), its position moving %s" % (
        (began - t_start) * 1000, "%.2f s after (it seeks ahead first)" % (moving - t_start) if moving else "not in 4 s"))
    loop_s = beats * 60. / tempo
    time.sleep(max(0., began + SETTLE + 2.5 * loop_s - time.perf_counter()))
    # FRIZZ's loop position against REAPER's beat, now and then
    phase = []
    for _ in range(6):
        a = time.perf_counter()
        pos = r.state()["pos"]
        b = time.perf_counter()
        phase.append(((a + b) / 2., pos))
        time.sleep(.15)
    t1 = time.perf_counter()
    got = followed(d, began + SETTLE, t1)
    if not got:
        r.check("REAPER's position while it plays", False, "too few readings", "a trace")
        return
    bpm, (k, y0, worst), pts = got
    at_start = y0 + k * t_start
    r.note("REAPER over %.1f s of playing: %.2f BPM, its beat at FRIZZ's Start %.3f, readings up to %.3f "
           "beats off its line, play rate %s, tempo %s" % (
               t1 - began - SETTLE, bpm, at_start, worst,
               sorted({x[4] for x in d.traced(began, t1)}), sorted({x[5] for x in d.traced(began, t1)})))
    r.tcheck("REAPER plays at the loop's tempo (%.1f BPM, %d beats)" % (tempo, beats),
             abs(bpm - tempo) <= tempo * .01 * r.loose, "%.2f BPM" % bpm, "%.1f +-%g %%" % (tempo, r.loose))
    r.tcheck("on its beat 1: REAPER's beat when FRIZZ sent Start", abs(at_start) <= .25,
            "%+.3f beats" % at_start, "0 +-0.25")
    r.tcheck("steadily: no jumps", worst <= .25, "%.3f beats off its line at most" % worst, "0.25")
    offs = []
    for t, pos in phase:
        rb = y0 + k * t
        diff = (rb - pos * beats + beats / 2.) % beats - beats / 2.
        offs.append(diff)
    r.tcheck("in step with the loop over %d wraps: REAPER's beat in the loop against FRIZZ's position"
            % int((t1 - t_start) / loop_s), offs and max(abs(x) for x in offs) <= .3,
            " ".join("%+.2f" % x for x in offs), "0 +-0.3 beats")
    # pause, PLAY
    tp = time.perf_counter()
    r.tap(PLAY)
    t_stop, _ = first_after(usb, STOP, tp, 1.)
    halted = stopped_from(d, t_stop or tp)
    r.check("PLAY: FRIZZ paused, Stop; REAPER pauses", t_stop is not None and halted is not None,
            "Stop %s, REAPER %s" % ("sent" if t_stop else "none",
                                    "paused %.0f ms after" % ((halted - (t_stop or tp)) * 1000) if halted else "plays on"),
            "both")
    time.sleep(.8)
    tr = time.perf_counter()
    r.tap(PLAY)
    t_spp, m = first_after(usb, SPP, tr, 1.)
    t_cont, _ = first_after(usb, CONTINUE, tr, 1.)
    at = (m[1] | m[2] << 7) / 4. if m else None
    resumed = playing_from(d, t_cont or tr)
    r.check("PLAY again: Song Position, Continue; REAPER plays", at is not None and t_cont and resumed,
            "Song Position %s, Continue %s, REAPER %s" % (
                "%g beats" % at if at is not None else "none", "sent" if t_cont else "none",
                "%.0f ms after" % ((resumed - t_cont) * 1000) if resumed and t_cont else "no"), "all")
    if not (at is not None and t_cont and resumed):
        return
    time.sleep(max(0., resumed + SETTLE + 3. - time.perf_counter()))
    got = followed(d, resumed + SETTLE, time.perf_counter())
    if got:
        bpm, (k, y0, worst), _ = got
        there = y0 + k * t_cont
        r.check("REAPER goes on where the Song Position says", abs(there - at) <= .25,
                "beat %.3f at Continue" % there, "%g +-0.25" % at)
        r.tcheck("at the loop's tempo again", abs(bpm - tempo) <= tempo * .01 * r.loose, "%.2f BPM" % bpm,
                 "%.1f" % tempo)
    # the clock factor while it plays: REAPER follows a clock within about half to twice its
    # project's tempo, beyond that in jumps (seeking ahead each time), so x1/2 is judged with
    # its project at 60, and only noted at 120
    project = 120.
    for value, bpm, judged in ((200, 120., True), (50, 120., False), (50, 60., True)):
        if r.args.quick and not judged:
            continue  # noted only, on the device
        if bpm != project:
            d.tempo(bpm)
            project = bpm
        tf = time.perf_counter()
        r.setting(clock_factor=value)
        time.sleep(SETTLE)
        want = tempo * value / 100.
        a = time.perf_counter()
        time.sleep(2. if r.args.quick else 4.)
        got = followed(d, a, time.perf_counter())
        sent = [m.hex() for t, m in usb if t >= tf and m[0] in (START, STOP, CONTINUE, SPP)]
        what = "clock factor %s, REAPER's project at %d (%.1fx)" % (
            "x2" if value == 200 else "x1/2", bpm, want / bpm)
        measured = "%.2f BPM, %.2f beats off its line at most" % (got[0], got[1][2]) if got else "not playing"
        r.note("%s: FRIZZ sent %s; REAPER %s" % (what, " ".join(sent) or "no transport", measured))
        if judged:
            r.tcheck("%s: REAPER at FRIZZ's tempo, steadily" % what,
                     got and abs(got[0] - want) <= want * .01 * r.loose and got[1][2] <= .25,
                     measured, "%.1f +-%g %%, 0.25 beats" % (want, r.loose))
    r.setting(clock_factor=100)
    tp = time.perf_counter()
    r.tap(PLAY)
    t_stop, _ = first_after(usb, STOP, tp, 1.)
    halted = stopped_from(d, t_stop or tp)
    r.check("paused again: REAPER pauses", halted is not None,
            "%.0f ms after the Stop" % ((halted - (t_stop or tp)) * 1000) if halted else "plays on", "it stops")
    if trs is not None:
        def transport(log):
            return [m[0] for t, m in log if t >= t0 and m[0] in (START, STOP, CONTINUE, SPP)]
        r.check("the same Start, Stop, Song Position and Continue out of TRS as over USB (#157)", transport(trs) == transport(usb),
                "TRS %s, USB %s" % (" ".join("%02x" % b for b in transport(trs)),
                                    " ".join("%02x" % b for b in transport(usb))), "the same")
    d.trace(False)


# ======== audio: music through FRIZZ, recorded in REAPER (#55's DAW items by ear) ========

SR = 48000
DAW_BPM = 87.  # the material's tempo, and REAPER's project's
PEAK_DB = -7.  # the material's peak into AUX (under -6 dBFS: AUX doesn't clip)
NO_NUMPY = "needs numpy: ~/.venvs/frizz/bin/python tools/reaper.py ... (measure.py's venv)"


def default_material():
    """FRIZZ_DAW_MATERIAL, else the longest WAV in card/audio: this checkout's, else the main
    checkout's (git's first worktree), where the music for these cases is kept out of git"""
    if os.environ.get("FRIZZ_DAW_MATERIAL"):
        return os.environ["FRIZZ_DAW_MATERIAL"]
    dirs = [os.path.join(HERE, "..", "card", "audio")]
    try:
        text = subprocess.run(["git", "-C", HERE, "worktree", "list", "--porcelain"], capture_output=True,
                              text=True, timeout=5).stdout
        main = text.split("\n")[0].split(" ", 1)[1] if text.startswith("worktree ") else None
        if main:
            dirs.append(os.path.join(main, "firmware", "card", "audio"))
    except (OSError, subprocess.TimeoutExpired):
        pass
    for d in dirs:
        try:
            wavs = [os.path.join(d, n) for n in os.listdir(d) if n.lower().endswith(".wav")]
        except OSError:
            continue
        if wavs:
            return max(wavs, key=os.path.getsize)
    return None


class Material:
    """The music for AUX as REAPER plays it: whole bars at DAW_BPM (cut to them), its peak at
    PEAK_DB, float stereo, written to path; mono: its left, which the line out's left carries
    back. source "generated": measure.py's never repeating notes and noise, 12 bars"""

    def __init__(self, source, path):
        self.bar = 4 * 60. / DAW_BPM * SR  # samples
        if source == "generated":
            x = measure.generated(int(12 * self.bar / SR) + 2)[:int(round(12 * self.bar))]
            x = np.repeat(x[:, None], 2, 1)
        else:
            x = measure.read_wav(source)
        bars = len(x) / self.bar
        self.bars = int(round(bars))
        if self.bars < 4 or abs(bars - self.bars) > .02:
            raise midisuite.Skip("the material is %.2f bars at %g BPM, not 4 or more whole ones" % (bars, DAW_BPM))
        n = int(round(self.bars * self.bar))
        x = np.concatenate([x, np.zeros((max(0, n - len(x)), 2))])[:n]
        peak = float(np.max(np.abs(x)))
        if peak <= 0:
            raise midisuite.Skip("the material is silent")
        self.peak_in = 20 * np.log10(peak)
        self.gain = PEAK_DB - self.peak_in
        self.x = x * 10 ** (self.gain / 20.)
        self.peak = 20 * np.log10(np.max(np.abs(self.x)))
        self.mono = self.x[:, 0]
        self.path = path
        measure.write_wav(path, self.x)

    def beats(self, bars):
        """REAPER's media item over at least bars, in whole passes of the material: beats"""
        return 4 * self.bars * int(np.ceil(bars / float(self.bars)))


def audio_ready(r, d):
    """The material ready, nothing latched (the effects' sound would blur the comparison; put
    back afterwards with the rest): (Material, the loop's length in bars, the bar its recording
    starts on). Skipped without numpy, material or REAPER's audio"""
    if np is None:
        raise midisuite.Skip(NO_NUMPY)
    if not d.links:
        raise midisuite.Skip("REAPER's audio isn't wired (needs --audio jack)")
    src = r.args.material
    if not src or (src != "generated" and not os.path.exists(src)):
        raise midisuite.Skip("no material: --material FILE or FRIZZ_DAW_MATERIAL (a WAV at %g BPM)" % DAW_BPM)
    m = Material(src, os.path.join(d.dir, "material.wav"))
    latched = [n for n, v in zip(remote.FX_NAMES, r.get("latch")) if v]
    for n in latched:
        r.set("latch", n, 0)
    vol = sink_volume(d.links[0][1].split(":")[0]) if not os.environ.get("FRIZZ_REAPER") else 100
    r.note("the material: %s%d bars at %g BPM, its peak %.1f dBFS, into AUX at %+.1f dB: %.1f dBFS%s%s" % (
        "generated, " if src == "generated" else "", m.bars, DAW_BPM, m.peak_in, m.gain, m.peak,
        ", the sink at %d %%" % vol if vol is not None else "", "; unlatched: %s" % " ".join(latched) if latched else ""))
    r.check("the material into AUX at -6 dBFS at most (REAPER's track at 0 dB, the sink at 100 % at most)",
            m.peak <= -6. and vol is not None and vol <= 100,
            "%.1f dBFS, %s" % (m.peak, "the sink at %d %%" % vol if vol is not None else "the sink's volume unknown (pactl)"),
            "<= -6 dBFS, 100 % at most")
    bars = 2 if r.args.quick else 4
    return m, bars, bars


def wait_beat(d, beat, timeout=60.):
    """Until REAPER's trace has it playing at beat or later"""
    end = time.perf_counter() + timeout
    while time.perf_counter() < end:
        got = d.traced(time.perf_counter() - 1.)
        if got and got[-1][3] & 1 and got[-1][2] >= beat:
            return
        time.sleep(.01)
    raise AssertionError("REAPER didn't get to beat %g in %g s" % (beat, timeout))


def recorded(d):
    """The takes REAPER recorded since the last call: [(the time line's s at the first sample,
    the samples)]; with --keep, their files and the material copied there too, for a look
    afterwards (takes.txt: a line a take, as reaper.lua logs it)"""
    files, got = {}, []
    takes = sorted(d.takes())
    keep = getattr(d, "keep", None)
    if keep:
        os.makedirs(keep, exist_ok=True)
        for f in [t[3] for t in takes] + [os.path.join(d.dir, "material.wav")]:
            if os.path.exists(f) and not os.path.exists(os.path.join(keep, os.path.basename(f))):
                shutil.copy(f, keep)
        with open(os.path.join(keep, "takes.txt"), "a") as f:
            for t in takes:
                f.write("%.6f %.6f %.6f %s\n" % (t[0], t[1], t[2], os.path.basename(t[3])))
            f.write("--\n")
    for pos, offs, length, path in takes:
        if path not in files:
            files[path] = measure.read_wav(path)[:, 0]
        a = int(round(offs * SR))
        got.append((pos, files[path][a:a + int(round(length * SR))]))
    return got


def bar_offsets(m, takes, content, bars, center, search):
    """Each project bar k of bars whose WAV bar content(k) (its first 0.8) a take holds: where
    it is against where the time line puts it, looked for within +-search samples of center
    samples after that: [(k, ms, correlation)]"""
    w = int(.8 * m.bar)
    got = []
    for k in bars:
        b = content(k)
        x = m.mono[int(round(b * m.bar)):][:w]
        for pos, y in takes:
            at = k * m.bar - pos * SR + center
            if at < 0 or at + w > len(y):  # (the search may be cut short at a take's ends)
                continue
            f = measure.find(y, x, at, search)
            if f:
                # from the time line's; the correlation either way up (polarity() says which)
                got.append((k, (f[0] - at + center) / SR * 1000., abs(f[1])))
                break
    return got


# the correlation a bar of the material needs to count as found, near where it should be: the
# line out isn't AUX (FRIZZ's compressor and limiter, the codec, the interface's noise), seen
# from 0.4 up on the device
CLEAR = .3


def dry_offset(m, takes, bars):
    """The material through FRIZZ live (before a loop), in REAPER's take: (its offset against
    the time line in ms, which is REAPER's latency compensation's error and FRIZZ's own
    latency; how many bars, the spread in ms, the lowest correlation); None"""
    content = lambda k: k % m.bars  # noqa: E731
    first = None
    for k in bars:
        first = bar_offsets(m, takes, content, [k], 0, int(.5 * SR))
        if first and first[0][2] >= CLEAR:
            break
    if not first or first[0][2] < CLEAR:
        return None
    got = [g for g in bar_offsets(m, takes, content, bars, first[0][1] * SR / 1000., int(.02 * SR))
           if g[2] >= CLEAR]
    ms = sorted(g[1] for g in got)
    return ms[len(ms) // 2], len(ms), ms[-1] - ms[0], min(g[2] for g in got)


def polarity(m, takes, k, dry):
    """The line out against AUX at the project's bar k: "the same way up" or "inverted" (its
    correlation's sign), as the dry signal has it"""
    x = m.mono[int(round((k % m.bars) * m.bar)):][:int(.8 * m.bar)]
    for pos, y in takes:
        at = k * m.bar - pos * SR + dry * SR / 1000.
        if at >= 0 and at + len(x) <= len(y):
            f = measure.find(y, x, at, int(.002 * SR))
            if f:
                return "inverted" if f[1] < 0 else "the same way up"
    return "?"


def loop_content(a, n):
    """The WAV bar a loop of n bars recorded from the project's bar a plays at its bar k, on the
    time line's grid (its start where the song's 0 is, a whole number of loops back from a)"""
    return lambda k: a + (k - a) % n


def loop_offsets(m, takes, content, bars, dry):
    """bar_offsets() of a loop, against the dry signal (ms): [(k, ms, correlation)]"""
    return [(k, ms - dry, c) for k, ms, c in bar_offsets(m, takes, content, bars, dry * SR / 1000., int(.06 * SR))]


def what_plays(m, takes, k, dry):
    """Which WAV bar a take has at the project's bar k, and how far off (within half a bar):
    "bar B at +X ms", for a bar that wasn't where it should be"""
    best = None
    for b in range(m.bars):
        for _, ms, c in bar_offsets(m, takes, lambda _: b, [k], dry * SR / 1000., int(.5 * m.bar)):
            if best is None or c > best[2]:
                best = (b, ms - dry, c)
    return "nothing clear" if not best or best[2] < CLEAR else "WAV bar %d at %+.1f ms (%.2f)" % best


def offsets_text(got):
    ms = [x for _, x, _ in got]
    return "%d bars, %+.2f..%+.2f ms (mean %+.2f), correlation %.2f at least" % (
        len(ms), min(ms), max(ms), sum(ms) / len(ms), min(c for _, _, c in got)) if ms else "none"


def judged(r, what, got, want_bars, tol, m, takes, dry):
    """A check that a loop's bars are all found, within +-tol ms of the dry signal (times
    midisuite's slack: 5 on the twin, --quick); the first bar that isn't, said where it is"""
    tol *= r.slack
    clear = [g for g in got if g[2] >= CLEAR]
    off = [g for g in clear if abs(g[1]) > tol]
    missing = sorted(set(want_bars) - {k for k, _, _ in clear})
    detail = offsets_text(clear)
    if missing or off:
        k = (missing or [off[0][0]])[0]
        detail += "; bar %d: %s" % (k, what_plays(m, takes, k, dry))
    r.tcheck(what, not missing and not off and clear, detail, "all %d bars within +-%g ms" % (len(want_bars), tol))
    return clear


TOL = 5.  # ms: a loop against the dry signal, bar by bar (a 16th at 87 BPM is 172 ms)


def loop_notes(a, n):
    """A track's notes for FRIZZ's keys (channel 16: PLAY 46, LOOP 47), as a DAW user would put
    them: PLAY held and LOOP on the song's bar a, a quantized recording counting from there
    (MANUAL: the press is its bar 1); LOOP again in its last bar, so it ends on the song's bar
    a + n: lane() events"""
    on, off = 0x90 | (midisuite.GLOBAL - 1), 0x80 | (midisuite.GLOBAL - 1)
    b, e = 4. * a, 4. * (a + n) - 2.
    return [(b - .25, on, 46, 100), (b, on, 47, 100), (b + .1, off, 47, 0), (b + .2, off, 46, 0),
            (e, on, 47, 100), (e + .1, off, 47, 0)]


def record_song_loop(r, d, m, n, a, factor=1., until=None):
    """REAPER plays the material from 0 into AUX at DAW_BPM, recording the line out; FRIZZ, its
    clock and transport following REAPER's, records a quantized loop of n bars (the song's) from
    the song's bar a, by notes from a track of REAPER's (loop_notes()); REAPER records on to the
    song's bar until (a + 2 n: the loop once), then stops. The takes, the dry signal's offset
    (dry_offset()), FRIZZ's loop's length (PROBE)"""
    r.setting(clock_source=SOURCES["usb"], midi_transport=1, clock_factor=int(factor * 100), midi_out=0)
    until = until or a + 2 * n
    d.tempo(DAW_BPM)  # first: the media item's length goes by it
    d.media(m.path, m.beats(until + 2))
    d.lane(4 * (a + n) + 1, loop_notes(a, n))
    d.rec()
    d.locate(0)
    d.trace(True)
    d.record()
    want = max(50., DAW_BPM * factor)  # the effects' tempo: 50-300 after the factor (MANUAL)
    took, tempo = r.wait_tempo(want, 1, 6)
    r.check("REAPER at %g BPM, recording: FRIZZ's tempo" % DAW_BPM, took is not None, tempo, "%g +-1" % want)
    wait_beat(d, (a + .3) * 4, 30. + a * m.bar / SR)
    if r.state()["looper"] != 1:
        raise AssertionError("FRIZZ isn't recording after REAPER's notes PLAY + LOOP on bar %d" % a)
    wait_beat(d, (a + n + .3) * 4, 30. + n * m.bar / SR)
    if r.state()["looper"] != 2:
        raise AssertionError("FRIZZ's loop didn't close on bar %d (REAPER's note LOOP in its last bar)" % (a + n))
    length = chompi.get7(remote.dev_data(r.f, chompi.PROBE, [1], "PROBE")[2:6])
    wait_beat(d, until * 4 + .3, 60. + until * m.bar / SR)
    d.stop_transport()
    d.trace(False)
    time.sleep(.3)
    takes = recorded(d)
    # the notes' track gone (played again, PLAY + LOOP would erase the loop), the song and the
    # recording track as they were
    d.clear()
    d.media(m.path, m.beats(5 * n + 2))
    d.rec()
    dry = dry_offset(m, takes, range(1, a + n - 1))
    r.check("REAPER's take before the loop: the material through FRIZZ, live", dry is not None,
            "%+.2f ms against the time line (%d bars, spread %.2f ms, correlation %.2f at least)" % dry if dry else "not found",
            "found")
    if dry:
        r.note("the dry signal %+.2f ms after the time line: REAPER's latency compensation (JACK's latencies "
               "as pipewire-jack reports them) and the CHOMPI's own (the codec, FRIZZ's blocks); the line out "
               "%s against AUX" % (dry[0], polarity(m, takes, 1, dry[0])))
    if dry is None:
        raise AssertionError("the material isn't in REAPER's take: is AUX on %s, the line out on %s?" % (
            d.links[0][1], d.links[2][0]))
    return takes, dry[0], length


@case("daw-loop-audio", "7: music from REAPER into AUX at 87 BPM, its clock and Start over USB; a "
      "quantized loop of 4 bars recorded from it; REAPER plays on, recording FRIZZ's line out: "
      "the loop against the song bar by bar over 2 minutes (drift), its length against 4 bars",
      sound=True)
def daw_loop_audio(r, d):
    m, n, a = audio_ready(r, d)
    minutes = r.args.minutes
    passes = max(2, int(np.ceil(minutes * 60. / (n * m.bar / SR))))
    until = a + n + passes * n
    takes, dry, length = record_song_loop(r, d, m, n, a, until=until)
    content = loop_content(a, n)
    # (a song can repeat its bars: the one wanted near where it should be, not the best of all)
    first = bar_offsets(m, takes, lambda _: a, [a + n], dry * SR / 1000., int(.1 * SR))
    r.check("the loop holds the song's bars %d-%d (recorded from its bar %d)" % (a, a + n - 1, a),
            first and first[0][2] >= CLEAR,
            "at the song's bar %d: WAV bar %d at %+.1f ms (%.2f)" % (a + n, a, first[0][1] - dry, first[0][2]) if first
            else "not found", "WAV bar %d, correlation %g at least" % (a, CLEAR))
    want = list(range(a + n, until))
    got = judged(r, "the loop against the dry signal, bar by bar over %.1f minutes" % ((until - a - n) * m.bar / SR / 60.),
                 loop_offsets(m, takes, content, want, dry), want, TOL, m, takes, dry)
    if len(got) >= 3:
        t = np.array([k * m.bar / SR / 60. for k, _, _ in got])
        ms = np.array([x for _, x, _ in got])
        slope, icpt = np.polyfit(t, ms, 1)
        worst = float(np.max(np.abs(ms - (slope * t + icpt))))
        r.tcheck("its drift against the song (REAPER's time line, the interface's clock)", abs(slope) <= 1. * r.loose,
                 "%+.3f ms a minute over %.1f minutes, bars up to %.2f ms off the line" % (slope, t[-1] - t[0], worst),
                 "+-%g ms a minute" % r.loose)
        per_pass = slope * n * m.bar / SR / 60.
        r.note("the loop in the take: %.1f samples a pass, %d bars at %g BPM: %.1f" % (
            n * m.bar + per_pass * SR / 1000., n, DAW_BPM, n * m.bar))
    r.check("its length (PROBE) against %d bars at %g BPM (%.1f samples)" % (n, DAW_BPM, n * m.bar),
            abs(length - n * m.bar) <= n * m.bar * .001, "%d samples (%+.2f ms)" % (length, (length - n * m.bar) / SR * 1000.),
            "+-0.1 %")


START_TOL = 20.  # ms: a loop's start against the song's bar its LOOP note was on (REAPER's MIDI against its audio)


def audio_block(d):
    """REAPER's audio block in ms, as it says (its audio line: MODE SRATE BSIZE)"""
    try:
        _, rate, size = d.audio_info.split()[:3]
        return float(size) / float(rate) * 1000.
    except ValueError:
        return 0.


def restarts(r, d, factor):
    """A loop recorded from the song (its LOOP note on bar a, a multiple of its length n: so the
    song's 0 is a start of it too), then REAPER restarted three ways, recording each: the loop
    where it should be against the song, judged against where it was played from the start"""
    m, n, a = audio_ready(r, d)
    takes, dry, _ = record_song_loop(r, d, m, n, a, factor)
    content = loop_content(a, n)
    tag = "" if factor == 1 else " (clock factor x1/2)"
    want = list(range(a + n, a + 2 * n))
    judged(r, "playing on%s: the loop against the dry signal" % tag, loop_offsets(m, takes, content, want, dry),
           want, TOL, m, takes, dry)
    span = n + n // 2  # bars played after each restart
    mid = 2 * n + n / 2.  # the playhead into the middle of the loop
    start = None  # the loop against the song after Start: where its LOOP note put its start
    for what, at, rng in (("REAPER stopped, played from the start", 0, None),
                          ("stopped, the playhead to bar %g (the loop's middle), played" % mid, mid, None),
                          ("a loop range of bars %g-%g, three turns" % (n / 2., n / 2. + n), n / 2., (n / 2., n / 2. + n))):
        if rng:
            d.loop(rng[0] * 4, rng[1] * 4)
        d.locate(at * 4)
        time.sleep(.3)
        d.trace(True)
        d.record()
        if rng:
            time.sleep(3 * n * m.bar / SR + .3)
        else:
            wait_beat(d, (at + span) * 4 + .3, 30. + span * m.bar / SR)
        d.stop_transport()
        d.trace(False)
        d.loop()
        time.sleep(.3)
        takes = recorded(d)
        if rng:
            want = [k for k in range(int(np.ceil(rng[0])), int(rng[1]))]
        else:
            want = list(range(int(np.ceil(at)) + 1, int(at) + span))
        if rng:
            # each turn a take of its own: steady within it, and where the turn put it within
            # one of REAPER's blocks (its MIDI at a turn goes out on a block's edge, not on the
            # turn's sample: seen on the device, about 0-16 ms late at 1024 samples)
            block = audio_block(d)
            turns = [[(k, x - start, c) for k, x, c in loop_offsets(m, [t], content, want, dry) if c >= CLEAR]
                     for t in takes]
            turns = [t for t in turns if t]
            means = [sum(x for _, x, _ in t) / len(t) for t in turns]
            spread = max((max(x for _, x, _ in t) - min(x for _, x, _ in t)) for t in turns) if turns else 0.
            r.tcheck("%s%s: each turn steady, where it starts within one of REAPER's blocks (%.1f ms) after "
                     "where Start put it" % (what, tag, block),
                     len(turns) >= 2 and spread <= 1. * r.slack and all(-TOL * r.slack <= x <= block + TOL * r.slack for x in means),
                     "%d turns at %s ms, each within %.2f ms" % (len(turns), ", ".join("%+.1f" % x for x in means), spread),
                     "2 turns at least, within 0..%.1f +-%g ms" % (block, TOL * r.slack))
            continue
        got = [g for t in takes for g in loop_offsets(m, [t], content, want, dry)]
        if start is None:
            clear = [g for g in got if g[2] >= CLEAR]
            start = sorted(x for _, x, _ in clear)[len(clear) // 2] if clear else 0.
            r.note("after REAPER's Start the loop is %+.2f ms off the song's grid: where its LOOP note on bar %d "
                   "started it, as REAPER's MIDI reaches FRIZZ against its audio%s" % (start, a, tag))
            r.tcheck("%s%s: the loop's start on the song's 0, as its LOOP note was on bar %d" % (what, tag, a),
                     clear and len(clear) == len(set(want)) and all(abs(x) <= START_TOL * r.slack for _, x, _ in clear),
                     offsets_text(clear) + ("" if clear else "; bar %d: %s" % (want[0], what_plays(m, takes, want[0], dry))),
                     "all %d bars within +-%g ms" % (len(want), START_TOL * r.slack))
            continue
        judged(r, "%s%s: the loop in step with the song, as after Start" % (what, tag),
               [(k, x - start, c) for k, x, c in got], want, TOL, m, takes, dry + start)


@case("daw-loop-restart", "7: A DAW's Start / Song Position with music: a loop recorded from REAPER's "
      "song (transport following), then REAPER stopped and played from the start, the playhead put "
      "into the loop's middle, a loop range: each time the loop against the song; and again at the "
      "clock factor x1/2 (#108)", sound=True)
def daw_loop_restart(r, d):
    restarts(r, d, 1.)
    reset(r, d, r.snap)
    restarts(r, d, .5)


STANDSTILL = -98  # the transport's detents from 1x down through 1/16x into the standstill (#174)


class LineOut:
    """The line out's left, recorded from now until stop(): pw-record from measure.py's source
    on the device; on the twin, ptytwin.h's out.f32 for the same time (FRIZZ_TWIN_AUDIO)"""

    def __init__(self):
        self.t0 = time.perf_counter()
        self.twin = os.environ.get("FRIZZ_TWIN_AUDIO") if os.environ.get("FRIZZ_REAPER") else None
        if not self.twin:
            fd, self.path = tempfile.mkstemp(suffix=".f32")
            os.close(fd)
            self.proc = subprocess.Popen(["pw-record", "--target", measure.line_source(), "--rate", str(SR),
                                          "--channels", "1", "--format", "f32", "--raw", self.path],
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def stop(self):
        """The samples, and the time this computer had at the first"""
        if self.twin:
            t1 = time.perf_counter()
            with open(os.path.join(self.twin, "start")) as f:
                start = float(f.read())
            n0, n1 = int((self.t0 - start) * SR), int((t1 - start) * SR)
            path = os.path.join(self.twin, "out.f32")
            while os.path.getsize(path) < n1 * 4 and time.perf_counter() < t1 + 3.:
                time.sleep(.01)
            with open(path, "rb") as f:
                f.seek(n0 * 4)
                return np.frombuffer(f.read((n1 - n0) * 4), "<f4").astype(np.float64), self.t0
        self.proc.terminate()
        self.proc.wait()
        y = np.fromfile(self.path, "<f4").astype(np.float64)
        os.remove(self.path)
        return y, self.t0

    def close(self):
        if not self.twin and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
            os.remove(self.path)


def against_song(m, y, a, n, t0, where, every=.25, near=.2):
    """FRIZZ's line out with the mix in the middle: the song as REAPER plays it (into AUX,
    live) and the loop (the song's bars a..a+n-1, a a multiple of n) both. where(t): the song's
    position in s at this computer's time t, from REAPER's trace (None: not playing). For each
    window of about half a bar, every quarter, the song found within +-near s of where REAPER
    is, the loop within as much of where it would be in step, where the two hold different
    bars: (this computer's time, the loop's phase against the song's in ms, mod the loop's
    length, the correlations); and how many windows were left out as the song's bars there were
    too like the loop's at the same place (a song whose bars repeat: drums). REAPER following
    FRIZZ puts its song where FRIZZ's Song Position says, the loop's start on a multiple of its
    length: in step, 0, give or take REAPER's own latency"""
    w = int(.45 * m.bar)
    song = np.concatenate([m.mono, m.mono, m.mono])  # past its end: its start again
    total = m.bars * m.bar
    length = n * m.bar
    got, alike = [], 0
    for i in range(0, len(y) - w, int(every * m.bar)):
        t = t0 + i / float(SR)
        pos = where(t)
        x = y[i:i + w]
        if pos is None or np.sqrt(np.mean(x ** 2)) < 1e-4:
            continue
        at = (pos * SR) % total
        bar = int(at // m.bar)
        if a <= bar < a + n or a <= int(((at + w) % total) // m.bar) < a + n:
            continue  # the song in the loop's bars: the two alike
        sa, la = song[int(at):][:w], song[int(a * m.bar + at % length):][:w]
        if abs(np.dot(sa, la)) > .3 * np.sqrt(np.dot(sa, sa) * np.dot(la, la) + 1e-20):
            alike += 1
            continue  # a song that repeats its bars: here as the loop's, alike too
        so = measure.find(song, x, at + total, near * SR)
        lo = measure.find(song, x, a * m.bar + at % length + total, near * SR)
        if not lo or not so or abs(lo[1]) < CLEAR / 2 or abs(so[1]) < CLEAR / 2:
            continue
        d = ((lo[0] - a * m.bar) - so[0] + length / 2.) % length - length / 2.
        got.append((t, d / SR * 1000., abs(lo[1]), abs(so[1])))
    return got, alike


def where_from(d):
    """The song's position in s at this computer's time, from REAPER's trace: between two
    readings while it plays, as their line; None elsewhere"""
    pts = [(now, sec) for now, sec, _, state, _, _ in d.traced(0.) if state & 1]

    def where(t):
        k = next((i for i, (now, _) in enumerate(pts) if now > t), None)
        if not k or t - pts[k - 1][0] > .2:
            return None
        (u0, s0), (u1, s1) = pts[k - 1], pts[k]
        return s0 + (s1 - s0) * (t - u0) / (u1 - u0) if u1 > u0 else s0
    return where


def stretch(got, t0, t1):
    """against_song()'s readings between t0 and t1: their ms"""
    return [d for t, d, _, _ in got if t0 <= t <= t1]


@case("daw-follows-audio", "7: a DAW synced to FRIZZ, with music (#159), through a standstill (#174): a "
      "loop recorded from REAPER's song, then REAPER synced to FRIZZ's clock and Song Position, "
      "playing the song into AUX, FRIZZ's mix in the middle: the loop against the song in the line out "
      "before the transport is turned into a standstill and after it's pressed back to 1x", sound=True)
def daw_follows_audio(r, d):
    m, n, a = audio_ready(r, d)
    record_song_loop(r, d, m, n, a, until=a + n + 1)
    usb = r.usb.log
    # FRIZZ's own clock (the loop's) out over USB, REAPER following it: its clock out off. (Not
    # recording: REAPER recording runs on by itself, deaf to a Stop; the line out by pw-record)
    r.setting(clock_source=SOURCES["internal"], midi_transport=0, midi_out=OUTS["all"])
    d.send(midisuite.cc(midisuite.GLOBAL, 87, 64))  # the mix in the middle: the song and the loop
    d.restart(sync=True)
    r.note("REAPER restarted synchronized: %s; FRIZZ %s" % (d.sync_info, remote.LOOPER[r.state()["looper"]]))
    d.tempo(DAW_BPM)
    d.media(m.path, m.beats(4 * n + 16))
    d.locate(0)
    d.trace(True)
    if r.state()["looper"] == 2:
        r.tap(PLAY)
        time.sleep(.5)
    rec = LineOut()
    try:
        spans = []
        for what in ("before", "after"):
            t = time.perf_counter()
            r.tap(PLAY if what == "before" else "ENC_5_SW")  # the transport pressed: 1x
            t_cont, _ = first_after(usb, CONTINUE, t, 3.)
            began = playing_from(d, t_cont or t, 4.)
            r.check("%s: FRIZZ's Continue, REAPER plays" % ("PLAY" if what == "before" else "out of the standstill"),
                    t_cont is not None and began is not None,
                    "Continue %s, REAPER %s" % ("sent" if t_cont else "none",
                                               "%.0f ms after" % ((began - t_cont) * 1000) if began and t_cont else "no"), "both")
            if began is None:
                return
            time.sleep(SETTLE)
            t1 = time.perf_counter()
            time.sleep(1.5 * n * m.bar / SR)
            spans.append((what, t1, time.perf_counter()))
            if what == "before":
                tp = time.perf_counter()
                r.turn(midisuite.TRANSPORT_ENC, STANDSTILL)
                t_stop, _ = first_after(usb, STOP, tp, 4.)
                halted = stopped_from(d, t_stop or tp, 6.)
                r.check("the transport turned into a standstill: Stop, REAPER pauses", t_stop is not None and halted is not None,
                        "Stop %s, REAPER %s" % ("sent" if t_stop else "none", "paused" if halted else "plays on"), "both")
                time.sleep(2.)
        y, t0 = rec.stop()
    finally:
        rec.close()
        d.stop_transport()
        d.trace(False)
    if getattr(d, "keep", None):
        os.makedirs(d.keep, exist_ok=True)
        measure.write_wav(os.path.join(d.keep, "line-out.wav"), np.repeat(y[:, None], 2, 1))
        with open(os.path.join(d.keep, "line-out.txt"), "w") as f:
            f.write("t0 %.6f\n" % t0 + "".join("%s %.6f %.6f\n" % s for s in spans))
    got, alike = against_song(m, y, a, n, t0, where_from(d))
    if alike and len(got) < 6:
        r.note("the song's bars too like the loop's to tell the two apart in the line out (%d windows of %d): "
               "the loop against the song not judged; --material generated measures it" % (alike, alike + len(got)))
        return
    res = {}
    for what, t1, t2 in spans:
        ms = stretch(got, t1, t2)
        res[what] = ms
        r.tcheck("%s the standstill: the loop against REAPER's song in the line out, steady" % what,
                 len(ms) >= 3 and max(ms) - min(ms) <= 2. * r.slack,
                 "%d readings: %s ms" % (len(ms), " ".join("%+.1f" % x for x in ms)) if ms else "none",
                 "3 at least, within %g ms" % (2. * r.slack))
    if len(res.get("before", [])) >= 3 and len(res.get("after", [])) >= 3:
        mb, ma = sorted(res["before"])[len(res["before"]) // 2], sorted(res["after"])[len(res["after"]) // 2]
        r.note("REAPER following: its song against FRIZZ's loop in the line out %+.2f ms before the standstill, "
               "%+.2f after (its own latency in it)" % (mb, ma))
        r.tcheck("after the standstill where it was before: REAPER went on where the loop is", abs(ma - mb) <= TOL * r.slack,
                 "%+.2f ms (before %+.2f, after %+.2f)" % (ma - mb, mb, ma), "+-%g ms" % (TOL * r.slack))


def jitter_row(ticks):
    """(ticks, mean, 99 %, max) of their distances from the line through them, ms"""
    p, _ = fit(ticks)
    if not p or len(ticks) < 20:
        return None
    n = len(ticks)
    mx, my = (n - 1) / 2., sum(ticks) / n
    res = sorted(abs(t - (my + p * (i - mx))) * 1000. for i, t in enumerate(ticks))
    return n, sum(res) / n, res[min(n - 1, int(.99 * n))], res[-1]


def measure_clock(r, start, stop, bpm, label):
    """A clock at bpm, start()ed, measured for --seconds after 3 s of settling, stop()ped: its
    ticks passed on out of TRS against their line, FRIZZ's tempo every 0.2 s meanwhile"""
    r.setting(clock_source=SOURCES["usb"], midi_out=OUTS["trs"])
    t0 = time.perf_counter()
    start(bpm)
    r.wait_tempo(bpm, 1, 6)
    sleep_until(t0 + (1.5 if r.args.quick else 3.))
    t1 = time.perf_counter()
    vals = poll_tempo(r, r.args.seconds)
    t2 = time.perf_counter()
    stop()
    row = jitter_row(midisuite.ticks_after(r.trs.log, t1, t2)) if r.trs else None
    r.jitter.append((label, bpm, row, min(vals), max(vals), changes(vals), len(vals)))
    r.check("%s at %d BPM: its ticks passed on out of TRS, measured" % (label, bpm),
            row is not None and row[0] >= bpm * .4 * r.args.seconds * .9,
            "%d ticks" % row[0] if row else "none", "%d" % (bpm * .4 * r.args.seconds))
    r.note("%s, %d BPM: %s; tempo %.1f..%.1f, %d changes in %d readings" % (
        label, bpm, "%d ticks, from their line: mean %.3f, 99 %% %.3f, max %.3f ms" % row if row else "no ticks",
        min(vals), max(vals), changes(vals), len(vals)))
    return vals


@case("daw-jitter", "#83: REAPER's clock over USB at 200 and 300 BPM, in each audio mode: how much "
      "it jitters (passed on out of TRS, against a clock from this computer), and FRIZZ's tempo "
      "holds still", needs=("out",), every=True)
def daw_jitter(r, d):
    for bpm in (200, 300):
        def start(b):
            d.tempo(b)
            d.locate(0)
            d.play()
        vals = measure_clock(r, start, d.stop_transport, bpm, "REAPER, %s (%s)" % (d.audio, d.audio_info))
        what = "REAPER (%s) at %d BPM: FRIZZ's tempo over %g s" % (d.audio, bpm, r.args.seconds)
        r.known.add(what)  # #83: it flips under a jittery USB clock at 200-300 BPM
        held(what, max(vals) - min(vals) <= .5, "%.1f..%.1f, %d changes" % (min(vals), max(vals), changes(vals)),
             "one whole BPM (#83)", r)


def reference(r):
    """The jitter's reference: midiport.Clock over USB from this computer, at REAPER's tempos"""
    def start(b):
        r.clock("usb", b).send([START])

    def stop():
        r.clocks[-1].send([STOP])
        time.sleep(.05)
        r.stop_clocks()
    for bpm in (200, 300):
        measure_clock(r, start, stop, bpm, "this computer (midiport.Clock)")


# ======== running them ========

def reset(r, d, snap, scenes=False):
    if d.proc.poll() is None:
        d.cmd("stop")
        d.cmd("loop", "off")
        d.cmd("trace", "off")
        d.cmd("clear")
    midisuite.reset(r, snap, scenes)


def run_case(r, d, c, snap):
    """PASS, FAIL, SKIP, or KNOWN: failed only on checks of a fault known and not fixed yet
    (r.known, as check.h's Known()), which doesn't fail the run"""
    r.checks, r.notes, r.timing_fails, r.known = [], [], 0, set()
    why = midisuite.runnable(c, r.wiring)
    if why:
        return "SKIP", why
    print("-- %s: %s" % (c.id, c.item), flush=True)
    status, detail = None, ""
    r.started = time.monotonic()
    if midisuite.card_lost(r):
        return "FAIL", "not run: %s" % r.lost
    try:
        reset(r, d, snap)
        c.run(r, d)
    except midisuite.Skip as e:
        status, detail = "SKIP", str(e)
    except ReaperError as e:
        midisuite.stopped_meanwhile()
        status, detail = "FAIL", "REAPER: %s" % e
    except (AssertionError, SystemExit) as e:
        midisuite.stopped_meanwhile()
        status, detail = "FAIL", "stopped: %s" % e
    except Exception:
        midisuite.stopped_meanwhile()
        status, detail = "FAIL", "error: " + traceback.format_exc().strip().splitlines()[-1]
        if r.args.verbose:
            traceback.print_exc()
    finally:
        try:
            if not midisuite.card_lost(r):
                reset(r, d, snap, c.id == "daw-scenes")  # its scenes put back, as midisuite's morphs'
        except (Exception, SystemExit) as e:
            print("   (putting things back: %s)" % e, file=sys.stderr)
    if r.lost:
        return "FAIL", "stopped: %s" % r.lost
    if status is None:
        failed = {what for ok, what, *_ in r.checks if not ok}
        status = "FAIL" if not r.checks else "PASS" if not failed else "KNOWN" if failed <= r.known else "FAIL"
        detail = "" if r.checks else "nothing checked"
    return status, detail


def report(c, status, detail, r):
    print("%-4s %s%s%s" % (status, c.id, " (%.0f s)" % (time.monotonic() - r.started) if status != "SKIP" else "",
                           ": " + detail if detail else ""), flush=True)
    for ok, what, got, want in r.checks:
        if not ok:
            print("       %s: %s, want %s" % (what, got, want), flush=True)


def summary(results, jitter, wiring, ident, sessions, args):
    lines = ["### REAPER (`tools/reaper.py`%s)" % (" --quick" if args.quick else ""), "",
             "FRIZZ: %s. Wiring: %s. REAPER: %s." % (ident, midisuite.wiring_text(wiring),
                                                    "; ".join(sessions) or "not run"), ""]
    counts = collections.Counter(s for _, s, *_ in results)
    lines += ["**%s**" % ", ".join("%d %s" % (counts[s], s) for s in ("PASS", "KNOWN", "FAIL", "SKIP") if s in counts),
              "", "KNOWN: failed only where a fault known and not fixed yet shows (the check names its issue).", "",
              "| case | #55 item | REAPER | result | measured |", "|---|---|---|---|---|"]
    for c, status, detail, checks, audio in results:
        measured = "; ".join(("%s: %s" % (what, got)) + ("" if ok else " (want %s)" % want)
                             for ok, what, got, want in checks)
        if detail:
            measured = (measured + "; " if measured else "") + detail
        lines.append("| `%s` | %s | %s | %s | %s |" % (c.id, c.item.replace("|", "/"), audio, status,
                                                       measured.replace("|", "/")))
    if jitter:
        lines += ["", "**The clock's jitter (#83)**: each tick passed on out of TRS against the line "
                  "through them, over %g s after 3 s of settling. It includes FRIZZ passing it on (in "
                  "its block, 0.5 ms) and the interface's MIDI in, as the clock from this computer's "
                  "does; FRIZZ's tempo read every 0.2 s meanwhile." % args.seconds, "",
                  "| clock | BPM | ticks | mean | 99 % | max | FRIZZ's tempo |", "|---|---|---|---|---|---|---|"]
        for label, bpm, row, lo, hi, n_changes, n in jitter:
            lines.append("| %s | %d | %s | %.1f..%.1f, %d changes in %d readings |" % (
                label, bpm, "%d | %.2f ms | %.2f ms | %.2f ms" % row if row else "- | - | - | -",
                lo, hi, n_changes, n))
    return "\n".join(lines) + "\n"


def devices(audios):
    for audio in audios:
        with Daw(audio) as d:
            print("%s: audio %s, the script %.1f s after the start" % (audio, d.audio_info, d.started))
            for kind, devs in zip(("out", "in"), d.devices()):
                for name, i in sorted(devs.items(), key=lambda x: x[1]):
                    print("  %-3s %2d %s" % (kind, i, name))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("what", nargs="?", choices=["devices"], help="devices: what REAPER sees, and leave")
    ap.add_argument("--slot", type=int, choices=chompi.FRIZZ_SLOTS, default=chompi.TEST_SLOT,
                    help="the FRIZZ slot (12 by default), started unless it runs")
    ap.add_argument("--device", help="FRIZZ's raw MIDI node (the twin's for the host check)")
    ap.add_argument("--trs", help="the raw MIDI node on the TRS jack (FRIZZ_TRS_MIDI; found by name)")
    ap.add_argument("--wiring", choices=["auto"] + list(midisuite.WIRINGS), default="auto")
    ap.add_argument("--audio", default="jack,dummy", help="REAPER's audio, each in turn (jack,dummy)")
    ap.add_argument("--cases", help="only these, by id, comma separated")
    ap.add_argument("--seconds", type=float, help="the jitter's measurement per tempo (30 s; --quick 4)")
    ap.add_argument("--material", default=default_material(),
                    help="the music for the audio cases: a WAV of whole bars at 87 BPM, or 'generated' "
                    "(FRIZZ_DAW_MATERIAL; the longest WAV in card/audio)")
    ap.add_argument("--keep", metavar="DIR", help="the audio cases' takes and material kept in DIR (in card/: "
                    "git ignores it; the material may not be yours to share)")
    ap.add_argument("--minutes", type=float, help="daw-loop-audio's loop played on, recorded (2; --quick 0.1)")
    ap.add_argument("--quick", action="store_true", help="short (the host check)")
    ap.add_argument("--out", help="the summary into FILE too")
    ap.add_argument("--list", action="store_true", help="the cases, and leave")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    a.long = False
    a.seconds = a.seconds or (4. if a.quick else 30.)
    a.minutes = a.minutes or (.1 if a.quick else 2.)
    audios = a.audio.split(",")
    if not audios or any(m not in AUDIO_MODES for m in audios):
        sys.exit("--audio: jack, dummy, or both")
    if a.list:
        midisuite.list_cases(CASES)
        return
    if a.what == "devices":
        devices(audios)
        return
    cases = midisuite.chosen(CASES, a)
    # stopped from outside: put things back and close REAPER as at the end (midisuite.Stopped:
    # not taken for a case's FAIL)
    midisuite.stop_on_signals()

    device = a.device or chompi.to_frizz(a.slot)
    trs_node = a.trs or (None if a.wiring == "none" else midiport.trs_midi())
    chompi.device_guard(trs_node)
    trs_fd = os.open(trs_node, os.O_RDWR | os.O_NONBLOCK) if trs_node else None
    trs = midiport.Tap(trs_fd) if trs_fd is not None else None
    results, sessions, jitter, wiring, timing_only = [], [], [], set(), []

    # 1. on the node
    f = remote.Frizz(device)
    r = midisuite.Rig(f, trs_fd, a, trs=trs)
    r.jitter = []
    ident = midisuite.keep_card(f)
    snap = midisuite.Snapshot(r)
    stopped, restored, lost = None, False, None
    try:
        midisuite.reset(r, snap, False)
        r.wiring = midisuite.detect_wiring(r) if a.wiring == "auto" else midisuite.WIRINGS[a.wiring]
        wiring = r.wiring
        print("wiring: %s%s" % (midisuite.wiring_text(wiring), " (found)" if a.wiring == "auto" else ""), flush=True)
        if binary() and "out" in wiring and any(c.id == "daw-jitter" for c in cases):
            print("-- the jitter's reference: a clock from this computer", flush=True)
            reference(r)
            midisuite.reset(r, snap, False)
            jitter += r.jitter
            for note in r.notes:
                print("       " + note, flush=True)
        r.usb.close()
        os.close(r.usb.fd)

        # 2. REAPER, once per audio mode; then, for the cases that want it, once more in the
        # first, synchronized to what comes in
        plan = []
        plain = [c for c in cases if not c.sound]
        for n, audio in enumerate(audios):
            last = n == len(audios) - 1 and n > 0
            todo = [c for c in plain if not c.synced and (c.every or (c.last and last) or (n == 0 and not c.last))]
            if n == 0 and len(audios) == 1:
                results += [(c, "SKIP", "needs a second REAPER (--audio jack,dummy)", [], audio)
                            for c in plain if c.last]
            plan.append((audio, last, False, False, todo))
            if n == 0:
                # the cases with music in a REAPER of its own, its audio wired
                sound = [c for c in cases if c.sound]
                why = None if audio == "jack" else "needs REAPER's JACK audio (--audio jack first)"
                why = why or (None if np is not None else NO_NUMPY)
                if why:
                    results += [(c, "SKIP", why, [], audio) for c in sound]
                else:
                    plan.append((audio, False, False, True, sound))
        plan.append((audios[0], False, True, False, [c for c in plain if c.synced]))
        for audio, last, sync, wired, todo in plan:
            label = audio + (", synced" if sync else "") + (", wired" if wired else "")
            if not todo or lost:
                continue
            if not binary():
                results += [(c, "SKIP", "no REAPER", [], label) for c in todo]
                continue
            try:
                # the last REAPER with its preferences as they come: its CC reset on
                d = Daw(audio, device, a.verbose, settings=dict(ccresettab="") if last else None, sync=sync,
                        wired=wired)
            except ReaperError as e:
                print("REAPER %s: %s" % (label, e), file=sys.stderr)
                results += [(c, "FAIL", "REAPER %s: %s" % (label, str(e).splitlines()[0]), [], label) for c in todo]
                continue
            sessions.append("%s (%s; its script %.1f s after the start%s%s%s)" % (
                audio, d.audio_info, d.started, "" if last else "; CC reset overrides cc0-120=-1",
                "; synchronized to its MIDI input: projsmptesync %s, projsmpteinput %s; midiouts_clock=0" % tuple(
                    d.sync_info.split()[:2]) if sync else "",
                "; its audio out 3/4 into AUX, in 1 from the line out's left, nothing else linked" if wired else ""))
            print("REAPER %s" % sessions[-1], flush=True)
            try:
                r = midisuite.Rig(frizz_through(d), trs_fd, a, usb=d.tap, trs=trs, patience=2.)
                r.wiring, r.jitter, r.snap = wiring, [], snap
                d.keep = a.keep and os.path.join(a.keep, "%s-%d" % (label.replace(", ", "-"), int(time.time())))
                for c in todo:
                    status, detail = run_case(r, d, c, snap)
                    results.append((c, status, detail, r.checks, label))
                    report(c, status, detail, r)
                    if status == "FAIL" and not detail and r.timing_fails == sum(1 for ok, *_ in r.checks if not ok):
                        timing_only.append(c.id)
                    for note in r.notes:
                        print("       " + note, flush=True)
                    if r.lost:
                        lost = r.lost
                        print("stopped: %s" % lost, file=sys.stderr)
                        break
                jitter += r.jitter
            finally:
                d.close()
    except (KeyboardInterrupt, Exception) as e:
        if not isinstance(e, KeyboardInterrupt) and not midisuite.stop_signal:
            raise
        stopped = getattr(e, "signum", None) or midisuite.stop_signal or signal.SIGINT
        print("interrupted" if stopped == signal.SIGINT else "stopped (signal %d)" % stopped,
              file=sys.stderr)
    finally:
        # 3. on the node again, unless FRIZZ no longer keeps the card out of it
        r = midisuite.Rig(remote.Frizz(device), trs_fd, a, trs=trs)
        r.lost = lost
        restored = midisuite.put_back(r, snap)
        r.usb.close()
        if trs:
            trs.close()
    text = summary(results, jitter, wiring, ident, sessions, a)
    print()
    print(text)
    if timing_only:
        print("failed on timing alone: %s" % ",".join(timing_only))
    if a.out:
        os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
        with open(a.out, "w") as out:
            out.write(text)
    sys.exit(midisuite.exit_code(any(s == "FAIL" for _, s, *_ in results) or not restored, stopped))


__doc__ += "".join("    %-15s %s\n" % (c.id, c.item) for c in CASES)

if __name__ == "__main__":
    main()
