#!/usr/bin/env python3
"""midiport.py: MIDI on the CHOMPI's two ports for the tools that measure it (measure.py,
midisuite.py), without numpy: a clock paced by this computer, the interface's raw MIDI node for
the TRS jack, and a tap that timestamps every byte that comes back.

    Clock(write, bpm, mode)    24 ticks a beat from a thread; Start / Stop / Continue / Song
                               Position sent with the next tick; the tempo changed on the go
    trs_midi()                 the interface's raw MIDI node: its out into the CHOMPI's TRS in,
                               its in from the CHOMPI's TRS out (FRIZZ_TRS_MIDI sets it)
    Tap(fd)                    reads an fd from a thread: every MIDI message with the time it
                               came, FRIZZ's SysEx answers apart, for TapLink
    TapLink(tap)               midi_send.Link's recv() on a tap, so remote.Frizz's queries and
                               a capture share the CHOMPI's one USB node
"""
import collections
import os
import random
import re
import select
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import chompi  # noqa: E402
import midi_send  # noqa: E402

TICK, START, CONTINUE, STOP, SPP = 0xF8, 0xFA, 0xFB, 0xFC, 0xF2


def wait_until(t):
    """Sleeps most of the way, then spins: ticks to about 0.1 ms"""
    while True:
        d = t - time.perf_counter()
        if d <= 0:
            return
        if d > .002:
            time.sleep(d - .0015)


def spp(sixteenths):
    """Song Position: where to go on, in the sender's 16ths"""
    return bytes([SPP, sixteenths & 0x7F, sixteenths >> 7 & 0x7F])


class Clock:
    """24 ticks a beat at bpm from a thread, through write(bytes). mode: single, or as a host
    that batches (pairs: two ticks in one write every 2 ticks' time), catches up (catchup:
    every other tick held back and sent 1 ms before the next) or is late now and then (late:
    one tick in 48 10 ms late). jitter: each tick up to that many ms early or late, at random.
    send() puts bytes (Start, Song Position ...) in front of the next tick, in the same write,
    as a DAW does; bpm can be changed on the go. ticks keeps when each tick went"""

    def __init__(self, write, bpm, mode="single", jitter=0., seed=1):
        self.write, self.bpm, self.mode, self.jitter = write, float(bpm), mode, jitter
        self.rng = random.Random(seed)
        self.stop = False
        self.ticks = []
        self.lock = threading.Lock()
        self.pending = b""
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def send(self, data):
        with self.lock:
            self.pending += bytes(data)

    def _out(self, n):
        with self.lock:
            data, self.pending = self.pending + b"\xF8" * n, b""
        now = time.perf_counter()
        self.ticks.extend([now] * n)
        self.write(data)

    def run(self):
        t = time.perf_counter() + .05
        n = 0
        while not self.stop:
            period = 60. / (self.bpm * 24.)
            if self.mode == "pairs":
                wait_until(t + period)
                self._out(2)
                t += 2 * period
                n += 2
            elif self.mode == "catchup":
                wait_until(t + period - .001)
                self._out(1)
                wait_until(t + period)
                self._out(1)
                t += 2 * period
                n += 2
            else:
                late = .010 if self.mode == "late" and n % 48 == 47 else 0.
                wobble = self.rng.uniform(-self.jitter, self.jitter) / 1000. if self.jitter else 0.
                wait_until(t + late + wobble)
                self._out(1)
                t += period
                n += 1

    def close(self):
        self.stop = True
        self.thread.join()


def trs_midi():
    """The first sound card's raw MIDI node that isn't the CHOMPI's, said which: not one with
    the Daisy's USB id (FRIZZ's, the launcher's: FRIZZ's card is called FrizZ or Default, not
    CHOMPI, and while it restarts it isn't found as the CHOMPI), nor one called CHOMPI"""
    if os.environ.get("FRIZZ_TRS_MIDI"):
        return os.environ["FRIZZ_TRS_MIDI"]
    try:
        cards = open(os.path.join(chompi.ASOUND, "cards")).read()
    except OSError:
        cards = ""
    for m in re.finditer(r"^\s*(\d+) \[([^\]]*)\]", cards, re.M):
        card = m.group(1)
        node = os.path.join(chompi.SND, "midiC%sD0" % card)
        if "CHOMPI" in m.group(2) or chompi.usb_id(card) == chompi.DAISY_USB_ID:
            continue
        if os.path.exists(node):
            chompi.say("the TRS interface: %s (%s)" % (node, chompi.card_names(card)))
            return node
    return None


def message_length(status):
    """A channel or system message's length with its status byte; 0 for SysEx (to F7)"""
    if status < 0xF0:
        return 2 if status & 0xF0 in (0xC0, 0xD0) else 3
    return {0xF0: 0, 0xF1: 2, 0xF2: 3, 0xF3: 2}.get(status, 1)


class Tap:
    """Reads fd from a thread until close(): every message that comes, as (time, bytes), the
    time this computer's (perf_counter) at the read that brought its last byte. Real-time
    bytes are messages of their own, also between another's bytes; running status is
    filled in. FRIZZ's SysEx answers (F0 7D 43 48 ...) go to answers, for TapLink; everything
    else to log. echo, if set, gets each answer's bytes too (a soft thru)"""

    def __init__(self, fd):
        self.fd = fd
        self.log = []
        self.answers = collections.deque()
        self.cond = threading.Condition()
        self.echo = None
        self.status, self.msg = None, []
        self.stop = False
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while not self.stop:
            try:
                r, _, _ = select.select([self.fd], [], [], .05)
                data = os.read(self.fd, 4096) if r else b""
            except (OSError, ValueError):
                return
            if data:
                self.feed(data, time.perf_counter())

    def feed(self, data, t):
        for b in data:
            if b >= 0xF8:
                self.put(t, bytes([b]))
            elif b == 0xF0:
                self.status, self.msg = 0xF0, [b]
            elif b == 0xF7:
                if self.status == 0xF0:
                    self.put(t, bytes(self.msg + [b]))
                self.status, self.msg = None, []
            elif b >= 0x80:
                self.status, self.msg = b, [b]
                if message_length(b) == 1:
                    self.put(t, bytes(self.msg))
                    self.status, self.msg = None, []
            elif self.status is not None:
                if not self.msg:
                    self.msg = [self.status]
                self.msg.append(b)
                if self.status != 0xF0 and len(self.msg) == message_length(self.status):
                    self.put(t, bytes(self.msg))
                    self.msg = []
                    if self.status >= 0xF0:
                        self.status = None

    def put(self, t, msg):
        if msg[:4] == midi_send.HEADER:
            with self.cond:
                self.answers.append(msg)
                self.cond.notify_all()
            if self.echo:
                self.echo(msg)
        else:
            self.log.append((t, msg))

    def mark(self):
        """Where the log stands: since(mark) gives what came after"""
        return len(self.log)

    def since(self, mark=0):
        return self.log[mark:]

    def close(self):
        self.stop = True
        self.thread.join(1)


class TapLink:
    """midi_send.Link on a Tap: recv() takes FRIZZ's answers from it, so the tap's log keeps
    every other byte that came over USB"""

    def __init__(self, tap, patience=1.):
        """patience: the answers' timeouts times this (a twin behind the wall clock)"""
        self.tap, self.fd, self.buf, self.patience = tap, tap.fd, bytearray(), patience

    def recv(self, cmd, timeout, match=None):
        deadline = time.monotonic() + timeout * self.patience
        with self.tap.cond:
            while True:
                while self.tap.answers:
                    msg = self.tap.answers.popleft()
                    if len(msg) > 5 and msg[4] == cmd | midi_send.REPLY:
                        if match is None or match(msg[5:-1]):
                            return msg[5:-1]
                left = deadline - time.monotonic()
                if left <= 0:
                    return None
                self.tap.cond.wait(left)
