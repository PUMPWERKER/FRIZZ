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
What FRIZZ sends shows on the TRS jack (wiring A: the CHOMPI's TRS out into the interface's
MIDI in, FRIZZ's MIDI out on TRS: a clock coming in is passed on, a tick within its block) and
over USB, through REAPER's input.

REAPER's configuration is made in a temporary directory and deleted after (TMPDIR), the
license copied in from ~/.config/REAPER: the user's own in ~/.config/REAPER and ~/Apps/REAPER
is never written, and a run that finds a file there newer than its start fails. The audio:
jack (REAPER's JACK through pipewire-jack, a desktop DAW on this computer: the default) or
dummy (no audio device); its MIDI goes to ALSA's raw node either way (hw:FrizZ). The project
is silent, the master muted and REAPER's ports linked to nothing (jack_options=2; a run
finding a link fails): nothing reaches Line 1/2. An evaluation copy shows its notice for a
few seconds at each start, which doesn't hold up the script. REAPER is this tool's own
process: one that runs no script within 20 s, or doesn't quit when asked, has a dialog in
the way, is killed and said so, never tried again.

Each case names the #55 item it covers, and is PASS, FAIL or SKIP (no REAPER, or a wiring it
can't show), as midisuite.py's; the summary in Markdown at the end (--out writes it to FILE
too), with the jitter's table. FRIZZ_REAPER runs another REAPER (the host check's stand-in,
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

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import chompi  # noqa: E402
import midiport  # noqa: E402
import midisuite  # noqa: E402
import remote  # noqa: E402
from midiport import START, CONTINUE, STOP  # noqa: E402
from midisuite import SOURCES, OUTS, WHITE_FX, EMPTY, knob7, fit, count  # noqa: E402

HOME = os.path.expanduser("~")
USER_CONFIG = os.path.join(HOME, ".config", "REAPER")
INSTALL = os.path.join(HOME, "Apps", "REAPER")
LICENSE = "reaper-reginfo2.ini"
RUNNER = os.path.join(HERE, "reaper.lua")
STARTUP = 20.  # s for REAPER to run the script's first line
QUIT = 15.  # s for it to quit when asked
AUDIO_MODES = dict(jack=0, dummy=2)  # linux_audio_mode


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
    """REAPER's JACK ports linked to anything, as pw-link lists them: [] if none, or no pw-link"""
    try:
        text = subprocess.run(["pw-link", "-l"], capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.TimeoutExpired):
        return []
    got, port = [], None
    for line in text.splitlines():
        if not line.startswith(" "):
            port = line.strip()
        elif port and port.startswith("REAPER:") and ("|->" in line or "|<-" in line):
            got.append("%s %s" % (port, line.strip()))
    return got


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

    def __init__(self, audio="jack", node=None, verbose=False, name=None, settings=None):
        """name: a MIDI device by REAPER's name for it instead (virtual: REAPER's own port);
        settings: more of reaper.ini's [reaper] keys, {key: value}"""
        self.audio, self.verbose = audio, verbose
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
            self.outs_named = ["hw:%s" % card, "Midi-Bridge:%s: MIDI 1 (playback)" % card]
            self.ins_named = ["hw:%s" % card, "Midi-Bridge:%s: MIDI 1 (capture)" % card]
        else:
            self.outs_named = self.ins_named = []
        self.dir = tempfile.mkdtemp(prefix="frizz-reaper-")
        license_file = os.path.join(USER_CONFIG, LICENSE)
        if os.path.exists(license_file):
            shutil.copy(license_file, os.path.join(self.dir, LICENSE))
        os.makedirs(os.path.join(self.dir, "Scripts"))
        self.log_path = os.path.join(self.dir, "run.log")
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
            if self.audio == "jack" and jack_links():
                raise ReaperError("REAPER's ports are linked: %s" % "; ".join(jack_links()))
        except BaseException:
            self.close()
            raise

    # ---- REAPER's process ----
    def write_config(self):
        bit = lambda i: 0 if i is None else 1 << i  # noqa: E731
        mask = bit(self.out_index)
        cfg = [
            "[reaper]",
            "linux_audio_mode=%d" % AUDIO_MODES[self.audio],
            "linux_audio_srate=48000", "linux_audio_srateor=1", "linux_audio_bsize=512",
            "linux_audio_bufs=3", "linux_audio_nch_in=2", "linux_audio_nch_out=2",
            "linux_audio_bits=32", "alsa_indev=frizz_none", "alsa_outdev=frizz_none",
            "jack_options=2",  # REAPER links its ports to nothing by itself
            "midiouts=%d" % mask, "midiouts_clock=%d" % mask, "midiouts_noreset=%d" % mask,
            "midiins=%d" % bit(self.in_index),
            "splashlog=0", "lastproject=", "projecttab1=",
        ] + ["%s=%s" % kv for kv in sorted(self.settings.items())] + [
            "",
            "[verchk]", "lastt=%d" % int(time.time()), "",
        ]
        with open(os.path.join(self.dir, "reaper.ini"), "w") as f:
            f.write("\n".join(cfg) + "\n")
        # the device caches (ALSA's raw MIDI, JACK's): its names first, so REAPER numbers them 0
        for i, name in enumerate(("reaper-midihw-alsa.ini", "reaper-midihw-linux.ini")):
            cache = ["[mididevcache]"]
            if self.outs_named:
                cache += ["ot0=1", "on0=" + self.outs_named[min(i, len(self.outs_named) - 1)],
                          "it0=1", "in0=" + self.ins_named[min(i, len(self.ins_named) - 1)]]
            with open(os.path.join(self.dir, name), "w") as f:
                f.write("\n".join(cache) + "\n")
        plan = dict(log=self.log_path, cmds=self.cmds_path, project=os.path.join(self.dir, "run.rpp"),
                    out=self.out_index, out_name=self.out_name, input=self.in_index, in_name=self.in_name)
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


def case(id, item, needs=(), every=False, last=False):
    """every: in each REAPER (audio mode), not only the first; last: only in the last, when it
    isn't the first, which runs with REAPER's preferences as they come"""
    def add(fn):
        c = midisuite.Case(id, item, fn, needs)
        c.every, c.last = every, last
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
        r.tcheck("then holds", max(vals) - min(vals) <= .5 * r.loose, "%.1f..%.1f" % (min(vals), max(vals)),
                 "one whole BPM")
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
    r.check("REAPER plays from the start: the loop from its start", s["looper"] == 2 and abs(s["pos"] - .25) <= .08,
            "%s at %.2f 2 beats later" % (remote.LOOPER[s["looper"]], s["pos"]), "playing at 0.25 +-0.08")
    d.stop_transport()
    time.sleep(.3)
    d.locate(5)
    time.sleep(.3)
    s = pos_after(r, d.play()[0], 2)
    r.check("stopped, the playhead to beat 5 (from 0), play: carries on there",
            s["looper"] == 2 and abs(s["pos"] - 7 / 8.) <= .08,
            "%s at %.2f 2 beats later" % (remote.LOOPER[s["looper"]], s["pos"]), "playing at 0.88 +-0.08")
    time.sleep(1.)
    s = pos_after(r, d.locate(2)[0], 2)
    r.check("playing, the playhead to beat 2: carries on there", s["looper"] == 2 and abs(s["pos"] - .5) <= .08,
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
    r.check("a free loop, REAPER plays from the start", s["looper"] == 2 and .1 <= moved <= .7,
            "%s, %.2f on" % (remote.LOOPER[s["looper"]], moved), "plays on")
    d.stop_transport()


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
        if ch < 15:
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
        if ch < 15 and not latches[remote.FX_NAMES.index(fx)]:
            bad.append("ch %d: not latched" % ch)
    r.check("15 tracks playing: each knob where its lane's last CC, sent straight, puts it; CC 22 latched",
            not bad, "; ".join(bad[:6]) + (" ..." if len(bad) > 6 else "") if bad else "all %d knobs, 14 latches" % n,
            "the same")
    r.check("the lanes moved them", moved >= n * .8, "%d of %d" % (moved, n), "most")
    r.check("REAPER stopped, its CC reset off (CC reset overrides cc0-120=-1): the knobs stay", not kept,
            "; ".join(kept[:4]) + (" ..." if len(kept) > 4 else "") if kept else "all %d" % n, "all stay")


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
        r.tcheck("REAPER (%s) at %d BPM: FRIZZ's tempo over %g s" % (d.audio, bpm, r.args.seconds),
                 max(vals) - min(vals) <= .5 * r.loose,
                 "%.1f..%.1f, %d changes" % (min(vals), max(vals), changes(vals)), "one whole BPM (#83)")


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

def reset(r, d, snap):
    if d.proc.poll() is None:
        d.cmd("stop")
        d.cmd("loop", "off")
        d.cmd("trace", "off")
        d.cmd("clear")
    midisuite.reset(r, snap, False)


def run_case(r, d, c, snap):
    r.checks, r.notes, r.timing_fails = [], [], 0
    why = midisuite.runnable(c, r.wiring)
    if why:
        return "SKIP", why
    print("-- %s: %s" % (c.id, c.item), flush=True)
    status, detail = None, ""
    r.started = time.monotonic()
    try:
        reset(r, d, snap)
        c.run(r, d)
    except midisuite.Skip as e:
        status, detail = "SKIP", str(e)
    except ReaperError as e:
        status, detail = "FAIL", "REAPER: %s" % e
    except (AssertionError, SystemExit) as e:
        status, detail = "FAIL", "stopped: %s" % e
    except Exception:
        status, detail = "FAIL", "error: " + traceback.format_exc().strip().splitlines()[-1]
        if r.args.verbose:
            traceback.print_exc()
    finally:
        try:
            reset(r, d, snap)
        except (Exception, SystemExit) as e:
            print("   (putting things back: %s)" % e, file=sys.stderr)
    if status is None:
        status = "PASS" if r.checks and all(ok for ok, *_ in r.checks) else "FAIL"
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
    lines += ["**%s**" % ", ".join("%d %s" % (counts[s], s) for s in ("PASS", "FAIL", "SKIP") if s in counts), "",
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
    ap.add_argument("--wiring", choices=["auto", "out", "in", "both", "none"], default="auto")
    ap.add_argument("--audio", default="jack,dummy", help="REAPER's audio, each in turn (jack,dummy)")
    ap.add_argument("--cases", help="only these, by id, comma separated")
    ap.add_argument("--seconds", type=float, help="the jitter's measurement per tempo (30 s; --quick 4)")
    ap.add_argument("--quick", action="store_true", help="short (the host check)")
    ap.add_argument("--out", help="the summary into FILE too")
    ap.add_argument("--list", action="store_true", help="the cases, and leave")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    a.long = False
    a.seconds = a.seconds or (4. if a.quick else 30.)
    audios = a.audio.split(",")
    if not audios or any(m not in AUDIO_MODES for m in audios):
        sys.exit("--audio: jack, dummy, or both")
    if a.list:
        for c in CASES:
            print("%-15s %-7s %s" % (c.id, "/".join(c.needs) or "usb", c.item))
        return
    if a.what == "devices":
        devices(audios)
        return
    cases = CASES
    if a.cases:
        wanted = a.cases.split(",")
        unknown = [w for w in wanted if w not in {c.id for c in CASES}]
        if unknown:
            sys.exit("no case %s (--list)" % ", ".join(unknown))
        cases = [c for c in CASES if c.id in wanted]

    device = a.device or chompi.to_frizz(a.slot)
    trs_node = a.trs or (None if a.wiring == "none" else midiport.trs_midi())
    trs_fd = os.open(trs_node, os.O_RDWR | os.O_NONBLOCK) if trs_node else None
    trs = midiport.Tap(trs_fd) if trs_fd is not None else None
    results, sessions, jitter, wiring = [], [], [], set()

    # 1. on the node
    f = remote.Frizz(device)
    r = midisuite.Rig(f, trs_fd, a, trs=trs)
    r.jitter = []
    if not f.ram_only():
        sys.exit("FRIZZ can't keep the card out of it (PERSIST 0): a build from before it; not run")
    print("the card isn't written until FRIZZ restarts (PERSIST 0): everything stays in RAM", flush=True)
    got = f.dev(chompi.IDENT)
    info = chompi.parse_ident(got[1]) if got and got[0] == chompi.OK else None
    ident = "build %08x" % info["crc"] if info else "?"
    if info and info["uptime"] < midisuite.READY:
        time.sleep(midisuite.READY - info["uptime"])
    snap = midisuite.Snapshot(r)
    try:
        midisuite.reset(r, snap, False)
        r.wiring = midisuite.detect_wiring(r) if a.wiring == "auto" else \
            {"out": {"out"}, "in": {"in"}, "both": {"in", "out"}, "none": set()}[a.wiring]
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

        # 2. REAPER, once per audio mode
        for n, audio in enumerate(audios):
            last = n == len(audios) - 1 and n > 0
            todo = [c for c in cases if c.every or (c.last and last) or (n == 0 and not c.last)]
            if n == 0 and len(audios) == 1:
                results += [(c, "SKIP", "needs a second REAPER (--audio jack,dummy)", [], audio)
                            for c in cases if c.last]
            if not todo:
                continue
            if not binary():
                results += [(c, "SKIP", "no REAPER", [], audio) for c in todo]
                continue
            try:
                # the last REAPER with its preferences as they come: its CC reset on
                d = Daw(audio, device, a.verbose, settings=dict(ccresettab="") if last else None)
            except ReaperError as e:
                print("REAPER %s: %s" % (audio, e), file=sys.stderr)
                results += [(c, "FAIL", "REAPER %s: %s" % (audio, str(e).splitlines()[0]), [], audio) for c in todo]
                continue
            sessions.append("%s (%s; its script %.1f s after the start%s)" % (
                audio, d.audio_info, d.started, "" if last else "; CC reset overrides cc0-120=-1"))
            print("REAPER %s" % sessions[-1], flush=True)
            try:
                r = midisuite.Rig(frizz_through(d), trs_fd, a, usb=d.tap, trs=trs, patience=2.)
                r.wiring, r.jitter = wiring, []
                for c in todo:
                    status, detail = run_case(r, d, c, snap)
                    results.append((c, status, detail, r.checks, audio))
                    report(c, status, detail, r)
                    for note in r.notes:
                        print("       " + note, flush=True)
                jitter += r.jitter
            finally:
                d.close()
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
    finally:
        # 3. on the node again
        r = midisuite.Rig(remote.Frizz(device), trs_fd, a, trs=trs)
        try:
            midisuite.reset(r, snap, True)
            snap.restore_settings(r)
        except (Exception, SystemExit) as e:
            print("putting things back: %s" % e, file=sys.stderr)
        r.usb.close()
        if trs:
            trs.close()
    text = summary(results, jitter, wiring, ident, sessions, a)
    print()
    print(text)
    if a.out:
        with open(a.out, "w") as out:
            out.write(text)
    sys.exit(1 if any(s == "FAIL" for _, s, *_ in results) else 0)


__doc__ += "".join("    %-15s %s\n" % (c.id, c.item) for c in CASES)

if __name__ == "__main__":
    main()
