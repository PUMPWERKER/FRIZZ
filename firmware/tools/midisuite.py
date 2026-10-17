#!/usr/bin/env python3
"""midisuite.py: the release test's MIDI clock, sync, MIDI out and MIDI control items (#55,
sections 7 and 8) on the device, unattended, with this computer as an idealised DAW (#114,
step 1).

    ./midisuite.py [--slot 12] [--wiring auto|out|in|both|none] [--cases ID,ID ...]
                   [--quick | --long] [--out FILE] [--log FILE] [-v]
    ./midisuite.py --list

Each case sends what a DAW or a sequencer would (a clock over USB or the TRS jack, Start /
Stop / Continue / Song Position, CCs, notes, program changes, FRIZZ's SysEx for the hand's
keys and knobs), reads what FRIZZ makes of it (remote.py's STATE, GET, the LEDs, and every
byte its MIDI out sends, timed as it comes), and judges it against a tolerance: PASS, FAIL,
or SKIP when the wiring can't show it. It names the #55 item it covers. At the end a summary
in Markdown for the PR (--out writes it to FILE too): the results, the cases still open with
the other wiring, and what stays with the user (feel, ears, eyes, real gear). --log FILE
appends every message that came during each case, with the time it came, to FILE: what a
timing check judged, to look at afterwards (#146).

The wiring: USB to the CHOMPI, and the audio interface's MIDI (tools/midiport.py's
trs_midi(), FRIZZ_TRS_MIDI) on one TRS adapter, one way or the other:
    out   the CHOMPI's TRS out into the interface's MIDI in: what FRIZZ sends out of the jack
          is captured (wiring A, the usual)
    in    the interface's MIDI out into the CHOMPI's TRS in: a clock and CCs go into the jack
          (wiring B)
    both  two adapters
It's found out first (auto): MIDI out on TRS for a moment, and anything arriving at the
interface is the out; a clock out of the interface that FRIZZ's tempo follows is the in.
Cases that need the other way are SKIPs, listed at the end for a second run once the adapter
is turned round; a case whose wiring was found and then sees nothing is a FAIL.

FRIZZ's settings (channel 16, transport following, the clock factor and source, MIDI out and
feedback, the effect channels over TRS) are set by each case, over FRIZZ's SysEx, with the
card kept out of it first (PERSIST 0: refused if FRIZZ can't); the knobs, latches, VOLUME's
levels and scenes 1-2 it changes are put back after each case, the settings at the end, all
in RAM. Before each case and before putting things back it asks FRIZZ (IDENT) whether it still
keeps the card out of it: one that restarted meanwhile would write the card, so the run stops
there, a FAIL, nothing put back (#171). Ctrl-C, SIGTERM or SIGHUP stop it too, things put back
first: it exits 130, 143 or 129 then, 1 for a FAIL or a failed putting back, 0 otherwise.
--quick shortens the long cases (the host check, test/midisuite.cpp), --long runs the
"over a few minutes" ones for 3 minutes. It plays on the FRIZZ on --slot (12, the test slot,
by default), started if it isn't the one running, under the lock (tools/chompi.py).

Linux (ALSA raw MIDI), Python 3 without packages.
"""
import argparse
import os
import signal
import sys
import threading
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import chompi  # noqa: E402
import midiport  # noqa: E402
import remote  # noqa: E402
from midiport import TICK, START, CONTINUE, STOP, SPP, Clock, spp  # noqa: E402

PLAY, LOOP, SHIFT = "KEY_27", "KEY_28", "KEY_26"
KNOB1_ENC, TRANSPORT_ENC, VOLUME_ENC = 4, 5, 6  # kCmdTurn's encoders (SW1-SW6)
TRANSPORT_REV, TRANSPORT_FWD = 5, 6  # the panel's transport LEDs (PanelLeds.h)
FACTOR_LED = 3  # the settings page's lower G#, KEY_19, on the keys' chain (SettingsPage::KeyLedOf)
GLOBAL = 16
READY = 6.5  # s from power-on to the play page taking keys: the boot and the rainbow (test/twintest.h)
# white key k's effect (MANUAL.md "Channels"), as remote.py's FX_NAMES name them
WHITE_FX = [None, "freezer", "shifter", "folder", "crusher", "filter", "flanger", "phaser",
            "resonator", "slicer", "warble", "tapestop", "delay", "reverb", "chaos"]
# the knobs an effect has nothing on (MANUAL.md "Page 2"): (effect, knob 0-7), dark, doing nothing;
# and the finalizer's Level knobs 2 and 3 (MANUAL.md "Level module": the drive's in the 1.5 betas)
EMPTY = {("flanger", 4), ("resonator", 4)} | {("chaos", i) for i in range(4, 8)} | {("fin", 1), ("fin", 2)}
# the settings each case starts from: the global channel 16, the effect channels over TRS on,
# AUX live through the effects and the mic through them, not live (#177), nothing else
BASELINE = dict(midi_channel=16, midi_transport=0, clock_factor=100, clock_source=0, midi_out=0,
                midi_block_trs=1, midi_feedback=0, aux_dry=0, aux_live=1, mic_dry=0, mic_live=0,
                usb_dry=0, usb_live=1)
SOURCES = dict(auto=0, trs=1, usb=2, internal=3)
OUTS = dict(off=0, trs=1, all=2)


def cc(channel, number, value):
    return bytes([0xB0 | (channel - 1), number, value])


def note(channel, number, on):
    return bytes([(0x90 if on else 0x80) | (channel - 1), number, 0x7F if on else 0])


def knob7(v):
    """A CC's value as the knob's 0-1 (MidiToKnob, 7 bits)"""
    return v / 128. if v <= 64 else .5 + .5 * (v - 64) / 63.


def fit(times, timed=None):
    """A line through times (index -> time): (period, worst distance from it in s), of those
    timed says (all if None; None if it says none)"""
    n = len(times)
    if n < 3:
        return None, None
    mx, my = (n - 1) / 2., sum(times) / n
    sxx = sum((i - mx) ** 2 for i in range(n))
    slope = sum((i - mx) * (t - my) for i, t in enumerate(times)) / sxx
    worst = max((abs(t - (my + slope * (i - mx))) for i, t in enumerate(times) if timed is None or timed[i]),
                default=None)
    return slope, worst


def span_offsets(spans):
    """Spans of one clock's ticks, with gaps between (lists of times, 3 or more each): its
    period, the slope within each span pooled (a line through each, one slope), and each span's
    offset from span 0's line at that period, s. Over all of them, this computer's clock
    drifting from FRIZZ's goes into the period, and a wander within one span weighs a third: a
    period from span 0 alone, its wander carried over the seconds to the last span, put that
    one 0.3-0.7 ms off with nothing moved (#386)"""
    p0, _ = fit(spans[0])
    origin = spans[0][0]
    ks = [[round((t - origin) / p0) for t in s] for s in spans]
    num = den = 0.
    for k, s in zip(ks, spans):
        mk, mt = sum(k) / len(k), sum(s) / len(s)
        num += sum((i - mk) * (t - mt) for i, t in zip(k, s))
        den += sum((i - mk) ** 2 for i in k)
    p = num / den
    offs = [sum(t - origin - p * i for i, t in zip(k, s)) / len(s) for k, s in zip(ks, spans)]
    return p, [o - offs[0] for o in offs]


def steps(times, per=48):
    """How far the ticks' position steps, apart from their jitter: the line through them, the
    median of their distances from it every per ticks (two beats), the largest change from one
    such median to the next, in s. A late tick or two (USB, this computer busy) moves no
    median; a jump in the position moves every one after it"""
    p, _ = fit(times)
    if not p:
        return None
    n = len(times)
    mx, my = (n - 1) / 2., sum(times) / n
    res = [t - (my + p * (i - mx)) for i, t in enumerate(times)]
    meds = [sorted(res[i:i + per])[len(res[i:i + per]) // 2] for i in range(0, n - per + 1, per)]
    return max([abs(b - a) for a, b in zip(meds, meds[1:])] or [0.])


def bpm_of(period):
    return 60. / (24. * period) if period else 0.


class Skip(Exception):
    pass


class Stopped(KeyboardInterrupt):
    """Ctrl-C, SIGTERM or SIGHUP: stopped from outside, things put back (a KeyboardInterrupt,
    which run_case lets through), exiting 128 + the signal"""
    def __init__(self, signum):
        super().__init__(signum)
        self.signum = signum


stop_signal = None  # the signal that stopped the run, once one came


def stop_on_signals():
    """Ctrl-C, SIGTERM and SIGHUP raise Stopped; SIGTERM and SIGHUP once: putting things back
    isn't cut short by another"""
    def stop(n, _):
        global stop_signal
        stop_signal = stop_signal or n
        if n != signal.SIGINT:
            for sig in (signal.SIGTERM, signal.SIGHUP):
                signal.signal(sig, signal.SIG_IGN)
        raise Stopped(n)
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, stop)


def stopped_meanwhile():
    """Raises Stopped again if a signal came: where an error is caught, the stop may have
    become another error on its way (raised in a threading.Condition's wait(), the lock isn't
    taken back, and the with's release fails: \"cannot release un-acquired lock\", #183)"""
    if stop_signal:
        raise Stopped(stop_signal)


def exit_code(failed, stopped):
    """128 + the signal that stopped the run (SIGINT for Ctrl-C), else 1 if anything FAILed or
    wasn't put back, else 0"""
    return 128 + stopped if stopped else 1 if failed else 0


# ======== the rig: FRIZZ, its two ports, what comes back ========

class Rig:
    def __init__(self, f, trs_fd, args, usb=None, trs=None, patience=1.):
        """usb, trs: taps already reading (reaper.py's: FRIZZ's USB through REAPER, the jack's
        kept over several rigs), else each on its node; patience: the answers' timeouts times
        this"""
        self.f, self.args = f, args
        self.usb = usb or midiport.Tap(f.link.fd)
        # --quick: the host check's twin, which can fall behind the wall clock under load
        f.link = midiport.TapLink(self.usb, (4. if args.quick else 1.) * patience)
        self.trs_fd = trs_fd
        self.trs = trs or (midiport.Tap(trs_fd) if trs_fd is not None else None)
        self.trs_lock = threading.Lock()
        self.clocks = []
        self.wiring = set()
        self.checks = []
        self.notes = []
        self.timing_fails = 0
        self.lost = None  # why FRIZZ no longer keeps the card out of it (card_lost)
        # --quick, the host check's: the twin's pseudo-terminals, under all.sh's load, jitter
        # by tens of ms, so the tolerances on jitter (slack) and on rates and tempos (loose)
        # widen: there it checks the suite's logic, the device its timing
        self.slack = 5. if args.quick else 1.
        self.loose = 3. if args.quick else 1.

    # ---- sending ----
    def send_usb(self, data):
        self.f.raw(data)

    def send_trs(self, data):
        if self.trs_fd is None:
            raise Skip("no TRS MIDI node")
        with self.trs_lock:
            os.write(self.trs_fd, bytes(data))

    def send(self, port, data):
        (self.send_usb if port == "usb" else self.send_trs)(data)

    def clock(self, port, bpm, **kw):
        c = Clock(self.send_usb if port == "usb" else self.send_trs, bpm, **kw)
        self.clocks.append(c)
        return c

    def stop_clocks(self):
        for c in self.clocks:
            c.close()
        self.clocks = []

    def tap(self, key, ms=60):
        self.f.key(key, True)
        time.sleep(ms / 1000.)
        self.f.key(key, False)

    def turn(self, enc, detents):
        while detents:
            step = max(-64, min(63, detents))
            self.f.send(remote.TURN, [enc, step & 0x7F])
            detents -= step

    # ---- reading ----
    def state(self):
        d = self.f.state()
        return dict(looper=d[0], pos=d[3] / 127., selected=d[4], scene=d[5] - 1, flags=d[6],
                    latched=remote.get14(d[8], d[9]), tempo=remote.tempo(d),
                    morphing=bool(d[6] & 2), page=d[24] if len(d) > 24 else None)

    def tempo(self):
        return self.state()["tempo"]

    def get(self, group):
        return remote.dev_get(self.f, remote.group_of(group) if isinstance(group, str) else group)

    def knob(self, fx, index):
        return remote.knob(self.get(fx)[index])

    def set(self, group, index, value):
        remote.dev_set(self.f, group, str(index), str(value))

    def setting(self, **kw):
        for name, value in kw.items():
            self.set("settings", name, value)

    def captured(self, port):
        tap = self.usb if port == "usb" else self.trs
        if tap is None:
            raise Skip("no TRS MIDI node")
        return tap

    def wait_for(self, what, timeout, every=.05):
        """Polls what() until it's true: the seconds it took, or None"""
        start = time.monotonic()
        while time.monotonic() - start < timeout:
            if what():
                return time.monotonic() - start
            time.sleep(every)
        return None

    def wait_tempo(self, bpm, tol=1., timeout=5.):
        got = [None]

        def near():
            got[0] = self.tempo()
            return abs(got[0] - bpm) <= tol
        return self.wait_for(near, timeout), got[0]

    def tempos(self, seconds, every=.05):
        vals = []
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            vals.append(self.tempo())
            time.sleep(every)
        return vals

    # ---- the looper ----
    def record_loop(self, bars=1, bpm=120., factor=1., quantized=True, seconds=2.):
        """A loop: quantized to the clock (PLAY held + LOOP, LOOP again in its last bar), or
        free (LOOP, seconds, LOOP). Playing when it returns"""
        if quantized:
            self.f.key(PLAY, True)
            time.sleep(.08)
            self.tap(LOOP)
            time.sleep(.02)
            self.f.key(PLAY, False)
        else:
            self.tap(LOOP)
        start = time.monotonic()
        if self.wait_for(lambda: self.state()["looper"] == 1, 2.) is None:
            raise AssertionError("the recording didn't start%s" % (
                " (no clock at the clock source?)" if quantized else ""))
        bar = 240. / (bpm * factor)
        length = (bars - .5) * bar if quantized else seconds
        time.sleep(max(0., length - (time.monotonic() - start)))
        self.tap(LOOP)
        if self.wait_for(lambda: self.state()["looper"] == 2, bar + 2.) is None:
            raise AssertionError("the loop didn't close")
        return time.monotonic()

    def erase(self):
        s = self.state()
        if s["looper"] == 0:
            return
        if s["looper"] == 1:
            self.tap(LOOP)
            self.wait_for(lambda: self.state()["looper"] != 1, 5.)
        time.sleep(.6)  # LOOP doesn't erase for half a second after a recording closed
        for _ in range(3):
            self.tap(LOOP)
            if self.wait_for(lambda: self.state()["looper"] == 0, 1.) is not None:
                return
        raise AssertionError("the loop wasn't erased")

    # ---- judging ----
    def check(self, what, ok, got, want, timing=False):
        self.checks.append((bool(ok), what, got, want))
        if not ok and timing:
            self.timing_fails += 1
        if self.args.verbose:
            print("    %s %s: %s (want %s)%s" % ("ok  " if ok else "FAIL", what, got, want,
                                             " [timing]" if timing else ""), flush=True)
        return ok

    def tcheck(self, what, ok, got, want):
        """A check on timing: a rate, a tempo held, jitter, a delay"""
        return self.check(what, ok, got, want, True)

    def note(self, text):
        self.notes.append(text)
        if self.args.verbose:
            print("    " + text, flush=True)


# ======== the cases ========

CASES = []


class Case:
    def __init__(self, id, item, run, needs=(), literal=None):
        self.id, self.item, self.run, self.needs, self.literal = id, item, run, needs, literal


def case(id, item, needs=(), literal=None):
    """needs: what it can't run without ("in", "out", or "in|out": either). literal: the
    wiring the #55 item is about, when the case stands in for it with the other one"""
    def add(fn):
        CASES.append(Case(id, item, fn, needs, literal))
        return fn
    return add


def ticks_after(log, t0, t1=None):
    return [t for t, m in log if m[0] == TICK and t >= t0 and (t1 is None or t <= t1)]


def ticks_timed(log, t0, t1=None):
    """ticks_after's ticks, and whether each was the last message of its read: a read brings
    all that came since the last, each message with its time, so a tick with bytes behind it in
    its read looks late by their time on the wire (two CCs on TRS: 1.9 ms, #146)"""
    ticks, timed = [], []
    for i, (t, m) in enumerate(log):
        if m[0] == TICK and t >= t0 and (t1 is None or t <= t1):
            ticks.append(t)
            timed.append(not (i + 1 < len(log) and log[i + 1][0] == t))
    return ticks, timed


def count(log, byte, t0, t1=None):
    return sum(1 for t, m in log if m[0] == byte and t >= t0 and (t1 is None or t <= t1))


def first(log, byte, t0):
    for t, m in log:
        if t >= t0 and m[0] == byte:
            return t, m
    return None, None


# ---- 7. MIDI clock, sync, MIDI out ----

@case("clock-trs", "7: MIDI clock in over TRS: the tempo locks as before (#47)", needs=("in",))
def clock_trs(r):
    r.setting(clock_source=SOURCES["trs"])
    r.clock("trs", 120)
    took, got = r.wait_tempo(120, 1, 5)
    r.tcheck("TRS clock 120 BPM, source TRS: FRIZZ's tempo", took is not None, got, "120 +-1 within 5 s")
    vals = r.tempos(3)
    r.tcheck("then over 3 s", max(vals) - min(vals) <= r.loose, "%.1f..%.1f" % (min(vals), max(vals)),
            "steady within %g BPM" % r.loose)


@case("sources-trs", "7: a sequencer on TRS and a DAW over USB at different tempos: TRS follows "
      "the sequencer, USB the DAW, Auto whichever started first (#57)", needs=("in",))
def sources_trs(r):
    for name, first_port, want in (("trs", None, 100), ("usb", None, 140),
                                   ("auto", "trs", 100), ("auto", "usb", 140)):
        r.stop_clocks()
        time.sleep(.7)  # Auto lets go after 0.5 s of silence
        r.setting(clock_source=SOURCES[name])
        if first_port:
            r.clock(first_port, 100 if first_port == "trs" else 140)
            time.sleep(.5)
            r.clock("usb" if first_port == "trs" else "trs", 140 if first_port == "trs" else 100)
        else:
            r.clock("trs", 100)
            r.clock("usb", 140)
        took, got = r.wait_tempo(want, 1, 5)
        if took is not None:
            time.sleep(3.)  # settled: the whole BPM counts the ticks of the last 2-4 s
            got = r.tempo()
        what = "TRS at 100, USB at 140, source %s%s" % (name, ", %s first" % first_port.upper() if first_port else "")
        r.check(what + ": whose", abs(got - want) < abs(got - (240 - want)), got, "%d's, not %d's" % (want, 240 - want))
        r.tcheck(what, took is not None and abs(got - want) <= 1, got, "%d +-1, settled" % want)


@case("sources-usb", "7: ... on internal neither, and tap tempo works (#57)")
def sources_usb(r):
    r.setting(clock_source=SOURCES["usb"])
    r.clock("usb", 140)
    took, got = r.wait_tempo(140, 1, 5)
    r.tcheck("USB clock 140, source USB", took is not None, got, "140 +-1")
    r.setting(clock_source=SOURCES["internal"])
    time.sleep(.3)
    before = r.tempo()
    r.clocks[0].bpm = 100.
    if r.trs_fd is not None:
        r.clock("trs", 90)
    vals = r.tempos(3)
    r.check("source internal, the USB clock moved to 100", all(abs(v - before) <= 1 for v in vals),
            "%.1f..%.1f" % (min(vals), max(vals)), "stays at %.0f, the last tempo" % before)
    tap_tempo(r, 80)  # the clock still running
    took, got = r.wait_tempo(80, 1.5, 2)
    r.check("five taps at 80 BPM (CC 82) on internal, a clock running: taken", abs(got - 80) <= 8, got, "80 +-10 %")
    r.tcheck("five taps at 80 BPM (CC 82) on internal, a clock running", took is not None, got, "80 +-1.5")


@case("transport-trs", "7: transport following on, source TRS: the DAW's play / stop over USB "
      "leaves the loop alone; the sequencer's Start / Stop works (#57)", needs=("in",))
def transport_trs(r):
    r.setting(clock_source=SOURCES["trs"], midi_transport=1)
    trs = r.clock("trs", 120)
    r.wait_tempo(120, 1, 5)
    r.record_loop(1, 120)
    for port, msg, want, what in (("usb", STOP, 2, "USB Stop: still playing"),
                                  ("trs", STOP, 3, "TRS Stop: paused"),
                                  ("usb", CONTINUE, 3, "USB Continue: still paused"),
                                  ("usb", START, 3, "USB Start: still paused"),
                                  ("trs", CONTINUE, 2, "TRS Continue: playing")):
        if port == "trs":
            trs.send([msg])
        else:
            r.send_usb([msg])
        time.sleep(.5)
        got = r.state()["looper"]
        r.check(what, got == want, remote.LOOPER[got], remote.LOOPER[want])


@case("factor", "7: the clock factor's pulse (lower G#) matches the DAW's beat: x1, twice as fast "
      "at x2, half at x1/2 (#57)")
def factor(r):
    r.setting(clock_source=SOURCES["usb"])
    r.clock("usb", 120)
    r.wait_tempo(120, 1, 5)
    r.f.send(remote.SWITCH, [2])  # the settings page, until the hand's switch again
    time.sleep(2.5)  # its LEDs fade in first
    try:
        for value, want in ((100, 120), (200, 240), (50, 60)):
            r.setting(clock_factor=value)
            took, got = r.wait_tempo(want, 1, 4)
            r.tcheck("USB clock 120, factor %s: the tempo" % {100: "x1", 200: "x2", 50: "x1/2"}[value],
                    took is not None, got, "%d +-1" % want)
            time.sleep(1.5)  # the beat's grid onto the new tempo
            period = led_period(r, max(2., 5 * 60. / want))
            r.tcheck("the G# LED's pulse", period and abs(period - 60. / want) <= .1 * 60. / want,
                    "%.3f s" % period if period else "no pulse", "%.3f s +-10 %%" % (60. / want))
    finally:
        r.f.send(remote.SWITCH, [0])


def led_period(r, seconds):
    """The lower G#'s pulse on the settings page: the mean time between its rises, polled"""
    samples = []
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        d = r.f.ask(remote.LEDS, [1])[1:]
        samples.append((time.monotonic(), sum(d[3 * FACTOR_LED:3 * FACTOR_LED + 3])))
    levels = [v for _, v in samples]
    if max(levels) - min(levels) < 2:
        return None
    mid = (max(levels) + min(levels)) / 2.
    rises = [t for (t, v), (_, w) in zip(samples[1:], samples) if v > mid >= w]
    if r.args.verbose:
        r.note("G#: %d readings in %.1f s, levels %d..%d, rises %s" % (
            len(samples), seconds, min(levels), max(levels),
            " ".join("%.3f" % (t - samples[0][0]) for t in rises)))
    if len(rises) < 3:
        return None
    return (rises[-1] - rises[0]) / (len(rises) - 1)


def fast_tempo(r, port, bpms):
    r.setting(clock_source=SOURCES[port])
    for bpm in bpms:
        r.stop_clocks()
        c = r.clock(port, bpm)
        took, got = r.wait_tempo(bpm, 1, 6)
        r.tcheck("%s clock %d BPM: locks" % (port.upper(), bpm), took is not None, got, "+-1 within 6 s")
        time.sleep(4.)  # the whole BPM comes from the ticks of the last 2-4 s (TempoClock.h)
        vals = r.tempos(3. if r.args.quick else 6.)
        r.tcheck("then holds", max(vals) - min(vals) <= .5, "%.1f..%.1f" % (min(vals), max(vals)),
                "one whole BPM, no wobble")
        c.bpm = bpm + 1.
        took, got = r.wait_tempo(bpm + 1, .5, 4)
        r.tcheck("nudged by 1 BPM: follows", took is not None and took <= 2.5,
                "%.1f s" % took if took is not None else "not in 4 s (%.1f)" % got, "within about 2 s")
        c.bpm = bpm + 11.
        took, got = r.wait_tempo(bpm + 11, .5, 3)
        r.tcheck("jumped by 10 BPM: follows", took is not None and took <= .6,
                "%.2f s" % took if took is not None else "not in 3 s (%.1f)" % got, "at once (0.6 s)")


@case("fast-usb", "7: a clock at 170-300 BPM, the delay on, no loop: the echoes stay put; +1 BPM "
      "followed within about 2 s, +10 at once (#74), over USB")
def fast_usb(r):
    fast_tempo(r, "usb", [174] if r.args.quick else [174, 280])


@case("fast-trs", "7: the same over TRS (#74)", needs=("in",))
def fast_trs(r):
    fast_tempo(r, "trs", [174] if r.args.quick else [174, 280])


def tempo_start(r, port):
    """A clock starting, again and again, at 120 and 300 BPM by turns, the delay latched: from
    its first tick on, the FX's tempo (STATE's) is the one before or the clock's (+-1), never
    in between or past it, and turns to the clock's within a quarter second; then +10 BPM is
    followed at once (-10 from 300, FRIZZ's fastest)"""
    r.setting(clock_source=SOURCES[port])
    r.send_usb(cc(GLOBAL, 113, 127))  # the delay latched (white key 12)
    starts = 2 if r.args.quick else 4
    for n in range(2 * starts):
        bpm = (120, 300)[n % 2]
        r.stop_clocks()
        time.sleep(1.)  # the clock lost (0.5 s): the next one is a new lock
        old = r.tempo()
        c = r.clock(port, bpm)
        t0 = time.monotonic()
        vals = []
        while time.monotonic() - t0 < 2.:
            vals.append((time.monotonic() - t0, r.tempo()))
            time.sleep(.01)
        stray = [(t, v) for t, v in vals if abs(v - old) > .5 and abs(v - bpm) > 1.]
        took = next((t for t, v in vals if abs(v - bpm) <= 1.), None)
        back = [(t, v) for t, v in vals if took is not None and t > took and abs(v - bpm) > 1.]
        r.tcheck("%s clock starts at %d BPM (from %.1f): every tempo the FX take" % (port.upper(), bpm, old),
                 not stray and not back,
                 "%d readings in 2 s, %s" % (len(vals), "none astray" if not stray and not back else
                                             "astray: " + ", ".join("%.1f at %.0f ms" % (v, t * 1000)
                                                                    for t, v in (stray + back)[:4])),
                 "%.1f or %d +-1, nothing between" % (old, bpm))
        r.tcheck("... the clock's tempo taken", took is not None and took <= .25 * r.loose,
                 "after %.0f ms" % (took * 1000) if took is not None else "not in 2 s",
                 "within %g ms (and a reading's time)" % (250 * r.loose))
        if n >= 2 * starts - 2:
            to = bpm + (10 if bpm < 300 else -10)
            c.bpm = float(to)
            took, got = r.wait_tempo(to, .5, 3)
            r.tcheck("... then %d BPM: followed" % to, took is not None and took <= .6 * r.loose,
                     "%.2f s" % took if took is not None else "not in 3 s (%.1f)" % got, "at once (0.6 s)")


@case("tempo-start-usb", "7: no loop, the delay latched: a DAW's clock over USB starting at 120 and 300 BPM: "
      "the echoes take its tempo from the start, no wobble; a tempo change followed at once (#417)")
def tempo_start_usb(r):
    tempo_start(r, "usb")


@case("tempo-start-trs", "7: the same with a sequencer's clock into the TRS jack (#417)", needs=("in",))
def tempo_start_trs(r):
    tempo_start(r, "trs")


@case("usb-late", "7: a DAW's clock over USB, started after FRIZZ is on: the tempo follows the "
      "DAW's, not half of it (#93)")
def usb_late(r):
    time.sleep(1.)  # no clock for a while
    for mode in ("single", "pairs"):
        r.stop_clocks()
        time.sleep(.7)
        r.clock("usb", 132, mode=mode)
        took, got = r.wait_tempo(132, 1, 5)
        r.check("USB clock 132 BPM (%s) started: not half" % mode, got > 99, got, "132, not 66")
        r.tcheck("USB clock 132 BPM (%s) started" % mode, took is not None, got, "132 +-1")


# a chord (3 notes, running status) and automation (CC 1, running status) on channel 1, which
# FRIZZ drops with the effect channels off over TRS (midi_block_trs 0): only the UART sees them
def chord(n):
    return bytes([0x90, 60, 100 * (n & 1), 64, 100 * (n & 1), 67, 100 * (n & 1)])


def ccs(count):
    return lambda n: bytes([0xB0] + [b for i in range(count) for b in (1, (n + i) & 0x7F)])


# what follows a tick on the cable: (name, the bytes after tick n, which ticks have them). Not
# more: a Scarlett 18i20 sends a byte every ~0.5 ms, so 29 bytes take 15 of a tick's 20.8 ms at
# 120 BPM, and with 43 its backlog grows, the ticks with it (its delay, not FRIZZ's)
TAILS = [("bare", None, None),
         ("a chord after each tick (7 bytes)", chord, lambda n: True),
         ("a chord on each beat (7 bytes)", lambda n: chord(n // 24) if n % 24 == 0 else b"", lambda n: n % 24 == 0),
         ("14 CCs after each tick (29 bytes)", ccs(14), lambda n: True)]


def pct(vals, q):
    s = sorted(vals)
    return s[min(len(s) - 1, int(q * (len(s) - 1) + .5))]


def probe_clock(r):
    d = remote.dev_data(r.f, chompi.PROBE, [0], "PROBE")
    return chompi.get7(d[10:15]), chompi.get7(d[15:18])


def matched(sent, out, floor):
    """Each tick passed on over USB against the tick sent: (index into sent, delay in s) by
    order, the alignment whose median delay is the least not below floor"""
    best = None
    for k in range(-6, 7):
        pairs = [(i + k, o - sent[i + k]) for i, o in enumerate(out) if 0 <= i + k < len(sent)]
        if len(pairs) < len(out) // 2:
            continue
        med = pct([d for _, d in pairs], .5)
        if med >= floor and (best is None or med < best[0]):
            best = (med, pairs)
    return best[1] if best else []


@case("tick-tail", "7: a TRS clock with other bytes right behind its ticks (a sequencer's chord, "
      "automation on the same cable): each tick counts as it came, and goes out over USB at once; "
      "a quantized loop's Start plays from the tick after it (#322)", needs=("in",))
def tick_tail(r):
    r.setting(clock_source=SOURCES["trs"], midi_out=OUTS["all"], midi_block_trs=0, midi_transport=1)
    c = r.clock("trs", 120)
    took, _ = r.wait_tempo(120, 1, 5)
    if took is None:
        raise AssertionError("FRIZZ didn't take the TRS clock")
    log = r.captured("usb").log
    seconds = 2. if r.args.quick else 8.
    base = None  # the bare clock's: median delay, Start's delay
    before = r.state()
    for name, tail, tailed in TAILS:
        c.tail = tail
        time.sleep(.5)
        ticks0, locks0 = probe_clock(r)
        n0 = c.sent
        t0 = time.perf_counter()
        time.sleep(seconds)
        t1 = time.perf_counter()
        time.sleep(.1)
        ticks1, locks1 = probe_clock(r)
        sent = [t for t in c.ticks if t0 <= t <= t1]
        first = c.ticks.index(sent[0]) if sent else 0
        out = ticks_after(log, t0, t1 + .06)
        pairs = matched(sent, out, base[0] - .001 if base else 0.)
        delays = [d for _, d in pairs]
        r.check("%s: FRIZZ counted every tick, locked on" % name,
                locks1 == locks0 and abs((ticks1 - ticks0) - (c.sent - n0)) <= 2,
                "%d counted of %d sent, %d new locks" % (ticks1 - ticks0, c.sent - n0, locks1 - locks0),
                "all, none")
        if not delays:
            r.check("%s: its ticks passed on over USB" % name, False, "%d ticks" % len(out), "as sent")
            continue
        gaps = [b - a for a, b in zip(out, out[1:])]
        spread = "intervals %.1f..%.1f ms" % (min(gaps) * 1000, max(gaps) * 1000) if gaps else "-"
        vals = r.tempos(1.5)
        if base is None:
            base = [pct(delays, .5), None]
            r.note("%s: the tick out over USB %.2f ms after it was sent (median; 95 %% %.2f, worst "
                   "%.2f); %s; tempo %.2f..%.2f" % (name, base[0] * 1000, pct(delays, .95) * 1000,
                                                   max(delays) * 1000, spread, min(vals), max(vals)))
        else:
            added = [d - base[0] for d in delays]
            on = [d - base[0] for (i, d) in pairs if tailed(first + i)]
            r.note("%s: the tick out over USB later than bare by %.2f ms (median; 95 %% %.2f, worst "
                   "%.2f)%s; %s; tempo %.2f..%.2f" % (
                       name, pct(added, .5) * 1000, pct(added, .95) * 1000, max(added) * 1000,
                       ", the ticks with the chord %.2f (worst %.2f)" % (pct(on, .5) * 1000, max(on) * 1000)
                       if len(on) < len(added) else "", spread, min(vals), max(vals)))
            # USB's frames: a bare tick comes up to a millisecond after the median too
            r.tcheck("%s: the tick out over USB" % name, pct(on, .95) <= .001 * r.slack and max(on) <= .002 * r.slack,
                     "%.2f ms later than bare (95 %%), %.2f at worst" % (pct(on, .95) * 1000, max(on) * 1000),
                     "within %g ms, %g at worst" % (r.slack, 2 * r.slack))
        r.tcheck("%s: FRIZZ's tempo" % name, max(vals) - min(vals) <= .5 * r.loose and abs(pct(vals, .5) - 120) <= .5 * r.loose,
                 "%.2f..%.2f" % (min(vals), max(vals)), "120 +-%g" % (.5 * r.loose))

        # a quantized loop recorded through it: its length (the Looper's line fit), and the
        # Start's cue (Looper::Follow), out over USB with the loop's first tick (MidiOut.h)
        r.record_loop(1, 120)
        d = remote.dev_data(r.f, chompi.PROBE, [1], "PROBE")
        length = chompi.get7(d[2:6])
        cues = []
        for _ in range(2 if r.args.quick else 4):
            time.sleep(.6)
            mark = len(log)
            ts = time.perf_counter()
            c.send([START])
            time.sleep(.3)
            at = next((t for t in c.ticks if t >= ts), None)
            got = next((t for t, m in log[mark:] if m[0] == START), None)
            if at is not None and got is not None:
                cues.append(got - at)
        r.erase()
        if not cues:
            r.check("%s: the loop's Start out over USB" % name, False, "none", "one a Start")
            continue
        if base[1] is None:
            base[1] = pct(cues, .5)
            r.note("%s: a 1-bar loop of %d frames (%+.2f ms); Start's cue out over USB %.2f ms after "
                   "its tick (%s)" % (name, length, (length - 96000) / 48., base[1] * 1000,
                                      " ".join("%.1f" % (q * 1000) for q in cues)))
        else:
            r.note("%s: a 1-bar loop of %d frames (%+.2f ms); Start's cue later than bare by %s ms"
                   % (name, length, (length - 96000) / 48., " ".join("%.1f" % ((q - base[1]) * 1000) for q in cues)))
            r.tcheck("%s: the loop's Start" % name, max(cues) - base[1] <= .001 * r.slack,
                     "%.2f ms later than bare at worst" % ((max(cues) - base[1]) * 1000), "within %g ms" % r.slack)
        r.tcheck("%s: the loop's length" % name, abs(length - 96000) <= 48 * r.slack,
                 "%+.2f ms" % ((length - 96000) / 48.), "1 bar +-%g ms" % r.slack)
    c.tail = None
    after = r.state()
    r.check("the traffic on channel 1 moved nothing", (after["latched"], after["selected"], after["scene"])
            == (before["latched"], before["selected"], before["scene"]),
            "latched %s, selected %s" % (after["latched"], after["selected"]), "as before")


def drift_case(r, port_in, seconds, factor=1., bars=4):
    """A quantized loop of bars to a clock (4: its length taken from 4x the ticks of a 1-bar
    loop's, which scatters by about +-2 ms a minute from one recording to the next, #20), the
    transport a fifth up and back, then FRIZZ's own clock out over USB (the loop's position)
    against the clock sent: the loop's drift"""
    r.setting(clock_source=SOURCES[port_in], midi_out=OUTS["all"],
              clock_factor=int(factor * 100))
    c = r.clock(port_in, 120)
    r.wait_tempo(120 * factor, 1, 5)
    r.record_loop(bars, 120, factor)
    r.turn(TRANSPORT_ENC, 14)
    time.sleep(2.)
    r.turn(TRANSPORT_ENC, -14)
    time.sleep(2.)
    t0 = time.perf_counter()
    r.note("measuring %g s" % seconds)
    time.sleep(seconds)
    out = ticks_after(r.usb.log, t0)
    sent = [t for t in c.ticks if t >= t0]
    p_out, worst = fit(out)
    p_in, _ = fit(sent)
    if not p_out or not p_in:
        r.check("FRIZZ's clock out over USB", False, "%d ticks" % len(out), "a clock")
        return
    drift = (p_out * factor / p_in - 1.) * 60000.  # ms a minute the loop gains on the clock
    # each half on its own: alike, the loop's length is what it is (it scatters from one
    # recording to the next, #20); apart, the glide back to 1x was still settling
    mid = t0 + seconds / 2.
    halves = []
    for a, b in ((t0, mid), (mid, None)):
        po, _ = fit(ticks_after(r.usb.log, a, b))
        pi, _ = fit([t for t in sent if t >= a and (b is None or t < b)])
        halves.append((po * factor / pi - 1.) * 60000. if po and pi else float("nan"))
    r.note("the halves: %+.2f, %+.2f ms a minute" % tuple(halves))
    r.tcheck("the loop (its clock out) against the clock sent, over %g s" % seconds,
            abs(drift) <= 2. * r.slack, "%+.2f ms a minute (halves %+.2f, %+.2f)" % ((drift,) + tuple(halves)), "+-%g ms a minute (before #90: ~30)" % (2 * r.slack))
    step = steps(out)
    _, sent_worst = fit(sent)
    r.tcheck("no jumps in its position", step <= .002 * r.slack,
             "steps %.2f ms; single ticks up to %.1f ms off (the clock sent: %.1f)"
             % (step * 1000, worst * 1000, sent_worst * 1000),
             "%g ms (a late tick is USB's or this computer's jitter)" % (2 * r.slack))


@case("loop-drift", "7: a loop recorded to a MIDI clock, the transport turned a fifth up and back "
      "to 1x: it stays in time with the clock over a few minutes (#90)")
def loop_drift(r):
    drift_case(r, "usb", 8. if r.args.quick else 180. if r.args.long else 40.)


def start_spp(r, factor):
    r.setting(clock_source=SOURCES["usb"], midi_transport=1, clock_factor=int(factor * 100))
    c = r.clock("usb", 120)
    r.wait_tempo(120 * factor, 1, 5)
    r.record_loop(2, 120, factor)
    beat = 60. / (120 * factor)  # FRIZZ's beat; the loop has 8
    sixteenths = int(32 / factor)  # the loop's length in the sender's 16ths
    time.sleep(2.5 * beat)
    c.send([START])
    time.sleep(2 * beat)
    pos = r.state()["pos"]
    r.tcheck("playing, the DAW's Start: back to the loop's start", abs(pos - .25) <= .08 or
            abs(pos - .25) >= .92, "%.2f of the loop 2 beats later" % pos, "0.25 +-0.08")
    c.send([STOP])
    time.sleep(.3)
    r.check("Stop: paused", r.state()["looper"] == 3, remote.LOOPER[r.state()["looper"]], "paused")
    at = sixteenths * 5 // 8 + (sixteenths if factor < 1 else 0)  # past the loop's end: wraps
    c.send(spp(at))
    time.sleep(.3)
    pos = r.state()["pos"]
    want = (at % sixteenths) / sixteenths
    r.check("paused, Song Position %d (16ths)" % at, abs(pos - want) <= .03, "%.2f" % pos, "%.3f +-0.03" % want)
    c.send([CONTINUE])
    time.sleep(2 * beat)
    s = r.state()
    want = (want + .25) % 1.
    r.tcheck("Continue: plays on from there", s["looper"] == 2 and abs(s["pos"] - want) <= .08,
            "%s at %.2f 2 beats later" % (remote.LOOPER[s["looper"]], s["pos"]), "playing at %.2f +-0.08" % want)


@case("start-spp", "7: a DAW's Start / Song Position, transport following on: a loop recorded to "
      "its clock goes back to its start on play-from-start; the playhead moved while stopped "
      "continues there (#108)")
def start_spp_x1(r):
    start_spp(r, 1.)


@case("start-spp-half", "7: the same with the clock factor at 1/2, as on the user's card (#108)")
def start_spp_half(r):
    start_spp(r, .5)


@case("start-free", "7: ... a free loop just plays on (#108)")
def start_free(r):
    r.setting(clock_source=SOURCES["usb"], midi_transport=1)
    r.record_loop(quantized=False, seconds=2.)
    c = r.clock("usb", 120)
    time.sleep(.9)
    before = r.state()["pos"]
    c.send([START])
    time.sleep(.5)
    s = r.state()
    moved = (s["pos"] - before) % 1.
    r.tcheck("a free loop, the DAW's Start", s["looper"] == 2 and .1 <= moved <= .45,
            "%s, %.2f on in 0.5 s" % (remote.LOOPER[s["looper"]], moved), "plays on (~0.25 of a 2 s loop)")


def out_port(r):
    """MIDI out where it can be seen: the jack if its out is wired, else USB"""
    return "trs" if "out" in r.wiring else "usb"


def ticks_bpm(r, port, t0, t1):
    p, worst = fit(ticks_after(r.captured(port).log, t0, t1))
    return bpm_of(p), worst


def tap_tempo(r, bpm, taps=5):
    """Taps over MIDI (CC 82 crossing 64 on the global channel), the last one on a beat"""
    for i in range(taps):
        r.send_usb(cc(GLOBAL, 82, 127))
        time.sleep(.03)
        r.send_usb(cc(GLOBAL, 82, 0))
        if i < taps - 1:
            time.sleep(60. / bpm - .03)


@case("out-idle", "7: MIDI out on TRS, no loop: it takes 120 BPM (or the tapped tempo) without "
      "starting (#73)", literal="out")
def out_idle(r):
    port = out_port(r)
    r.setting(clock_source=SOURCES["internal"], midi_out=OUTS["trs" if port == "trs" else "all"])
    tap_tempo(r, 120)
    took, tempo = r.wait_tempo(120, 1, 2)
    r.tcheck("120 BPM tapped (CC 82)", took is not None, tempo, "120 +-1")
    t0 = time.perf_counter()
    time.sleep(2.)
    bpm, worst = ticks_bpm(r, port, t0 + .3, None)
    r.tcheck("MIDI out (%s), no clock, no loop: its clock" % port.upper(), abs(bpm - tempo) <= tempo * .01 * r.loose,
            "%.2f BPM" % bpm, "%.1f (FRIZZ's tempo) +-%g %%" % (tempo, r.loose))
    r.check("no Start", count(r.captured(port).log, START, t0) == 0,
            count(r.captured(port).log, START, t0), 0)


@case("out-fin-delay", "7: the finalizer's Loud up delays the outputs 1 ms (its limiter's lookahead): "
      "MIDI out's clock comes 1 ms later with them, and back once Loud is at 0 (#345)", literal="out")
def out_fin_delay(r):
    port = out_port(r)
    r.setting(clock_source=SOURCES["internal"], midi_out=OUTS["trs" if port == "trs" else "all"])
    r.set("fin", 3, 0)
    time.sleep(1.)
    spans = []
    for loud in (0, .1, 0):
        r.set("fin", 3, loud)
        time.sleep(.5)
        t0 = time.perf_counter()
        time.sleep(4.)
        spans.append(ticks_after(r.captured(port).log, t0, time.perf_counter()))
    if not all(len(ts) >= 3 for ts in spans):
        r.check("MIDI out (%s): its ticks" % port.upper(), False, "none", "ticks")
        return
    # each span against span 0 at the period of all three (span_offsets); the delay against
    # the mean of before and after too
    _, (a, d, z) = span_offsets(spans)
    delay = (d - (a + z) / 2) * 1000
    r.tcheck("Loud up: MIDI out's clock (%s) later by the limiter's 1 ms" % port.upper(),
             abs(delay - 1.) < .3 * r.loose, "%.2f ms" % delay, "1.00 +-%.1f" % (.3 * r.loose))
    # the capture's wander over the 9 s from span 0 to the last stays in this, up to ~0.3 ms;
    # one not back is 1 ms off
    r.tcheck("Loud at 0: back where it was", abs((z - a) * 1000) < .5 * r.loose, "%.2f ms" % ((z - a) * 1000),
             "0 +-%.1f" % (.5 * r.loose))


@case("out-loop", "7: MIDI out, record a loop: it starts on the loop's beat 1 and stays in time; "
      "pause -> it stops; PLAY -> it goes on in step with the loop (#73)", literal="out")
def out_loop(r):
    port = out_port(r)
    r.setting(clock_source=SOURCES["internal"], midi_out=OUTS["trs" if port == "trs" else "all"])
    log = r.captured(port).log
    t0 = time.perf_counter()
    playing = r.record_loop(quantized=False, seconds=2.)
    t_start, _ = first(log, START, t0)
    r.tcheck("the loop starts playing: Start", t_start is not None and abs(t_start - playing) <= .25,
            "%+.0f ms" % ((t_start - playing) * 1000) if t_start else "none", "with the loop (+-250 ms, polled)")
    if not t_start:
        return
    tempo = r.tempo()
    time.sleep(1.)
    r.tap(PLAY)
    paused = time.perf_counter()
    time.sleep(.5)
    t_stop, _ = first(log, STOP, t_start)
    r.check("PLAY: paused, Stop", t_stop is not None and r.state()["looper"] == 3,
            "Stop %+.0f ms" % ((t_stop - paused) * 1000) if t_stop else "none", "Stop")
    bpm, _ = ticks_bpm(r, port, t_start + .1, t_stop or paused)
    r.tcheck("its clock while playing", abs(bpm - tempo) <= tempo * .01 * r.loose, "%.2f BPM" % bpm,
            "%.1f (the loop's tempo) +-%g %%" % (tempo, r.loose))
    if not t_stop:
        return
    played = len(ticks_after(log, t_start, t_stop))
    time.sleep(.5)
    resumed = time.perf_counter()
    r.tap(PLAY)
    time.sleep(.6)
    t_spp, m = first(log, SPP, resumed)
    t_cont, _ = first(log, CONTINUE, resumed)
    at = m[1] | m[2] << 7 if m else None
    want = -(-played // 6)  # the next 16th from where it paused
    r.check("PLAY: Song Position, where the loop goes on", at is not None and abs(at - want) <= 1,
            at, "%d (+-1), the 16th after the %d ticks played" % (want, played))
    r.tcheck("then Continue", t_cont is not None and t_spp is not None and 0 <= t_cont - t_spp <= .3,
            "%+.0f ms after it" % ((t_cont - t_spp) * 1000) if t_cont and t_spp else "none",
            "within a 16th")


def pass_ports(r):
    """A clock in on one port, passed on out of the other (never back where it came from):
    the DAW over USB and the gear on TRS's out as the item says, or the other way round"""
    if "out" in r.wiring:
        return "usb", "trs"
    return "trs", "usb"


@case("out-pass", "7: a DAW's clock in over USB, MIDI out on TRS: the gear follows the DAW (and its "
      "Start/Stop) while there's no loop; record a quantized loop and it keeps playing without a "
      "jump (#73)", needs=("in|out",), literal="out")
def out_pass(r):
    src, dst = pass_ports(r)
    r.setting(clock_source=SOURCES[src], midi_out=OUTS["trs" if dst == "trs" else "all"])
    c = r.clock(src, 120)
    r.wait_tempo(120, 1, 5)
    log, back = r.captured(dst).log, r.captured(src).log
    t0 = time.perf_counter()
    c.send([START])
    time.sleep(1.5)
    bpm, _ = ticks_bpm(r, dst, t0 + .1, None)
    r.tcheck("%s clock 120 passed on out of %s" % (src.upper(), dst.upper()), abs(bpm - 120) <= 1.2 * r.loose,
            "%.2f BPM" % bpm, "120 +-%g %%" % r.loose)
    r.check("its Start passed on", count(log, START, t0) == 1, count(log, START, t0), 1)
    r.check("nothing back out of %s" % src.upper(), count(back, TICK, t0) == 0,
            "%d ticks" % count(back, TICK, t0), 0)
    t1 = time.perf_counter()
    r.record_loop(1, 120)
    time.sleep(1.5)
    times = ticks_after(log, t1)
    gaps = [b - a for a, b in zip(times, times[1:])]
    period = 60. / (120 * 24)
    r.check("a quantized loop recorded: no second Start", count(log, START, t1) == 0, count(log, START, t1), 0)
    r.tcheck("the ticks through it", gaps and max(gaps) <= 1.6 * period + .002 * r.slack and len(times) > 24,
            "longest gap %.1f ms" % (max(gaps) * 1000) if gaps else "none",
            "no jump (%.1f ms + USB's jitter)" % (1.6 * period * 1000))
    t2 = time.perf_counter()
    r.erase()
    c.send([STOP])
    time.sleep(.5)
    r.check("no loop again: the DAW's Stop passed on", count(log, STOP, t2) >= 1, count(log, STOP, t2), ">= 1")


@case("out-spp", "7: a DAW moves its playhead while stopped and plays on: its Song Position and "
      "Continue, in over USB, go out of TRS in that order, never back, so the gear behind goes on "
      "where the DAW does (#443)", needs=("in|out",), literal="out")
def out_spp(r):
    src, dst = pass_ports(r)
    r.setting(clock_source=SOURCES[src], midi_out=OUTS["trs" if dst == "trs" else "all"])
    c = r.clock(src, 120)
    r.wait_tempo(120, 1, 5)
    log, back = r.captured(dst).log, r.captured(src).log
    c.send([START])
    time.sleep(1.)
    c.send([STOP])
    time.sleep(.5)
    t0 = time.perf_counter()
    c.send(spp(300) + bytes([CONTINUE]))  # together, before the next tick, as a DAW sends them
    time.sleep(.5)
    t_spp, m = first(log, SPP, t0)
    t_cont, _ = first(log, CONTINUE, t0)
    at = m[1] | m[2] << 7 if m else None
    r.check("Song Position 300 passed on out of %s" % dst.upper(), at == 300, at, 300)
    r.tcheck("then Continue", t_spp is not None and t_cont is not None and 0 <= t_cont - t_spp <= .05 * r.loose,
             "%+.1f ms after it" % ((t_cont - t_spp) * 1000) if t_cont and t_spp else "none",
             "right after it (<= %g ms)" % (50 * r.loose))
    sent_back = count(back, SPP, t0) + count(back, CONTINUE, t0)
    r.check("nothing back out of %s" % src.upper(), sent_back == 0, sent_back, 0)
    c.send([STOP])
    time.sleep(.2)


@case("out-spp-factor", "7: the same at the clock factor x2 and x1/2: the Song Position goes out in "
      "the 16ths of the ticks that go out, 2N at x2, N/2 (rounded down) at x1/2 (#469)",
      needs=("in|out",), literal="out")
def out_spp_factor(r):
    src, dst = pass_ports(r)
    r.setting(clock_source=SOURCES[src], midi_out=OUTS["trs" if dst == "trs" else "all"])
    c = r.clock(src, 120)
    log, back = r.captured(dst).log, r.captured(src).log
    for value, name, want in ((200, "x2", 602), (50, "x1/2", 150)):
        r.setting(clock_factor=value)
        r.wait_tempo(120 * value / 100., 1, 5)
        c.send([START])
        time.sleep(1.)
        c.send([STOP])
        time.sleep(.5)
        t0 = time.perf_counter()
        c.send(spp(301) + bytes([CONTINUE]))  # an odd 16th: x1/2 rounds it down
        time.sleep(.5)
        t_spp, m = first(log, SPP, t0)
        t_cont, _ = first(log, CONTINUE, t0)
        at = m[1] | m[2] << 7 if m else None
        r.check("%s: Song Position 301 passed on out of %s" % (name, dst.upper()), at == want, at, want)
        r.tcheck("%s: then Continue" % name,
                 t_spp is not None and t_cont is not None and 0 <= t_cont - t_spp <= .05 * r.loose,
                 "%+.1f ms after it" % ((t_cont - t_spp) * 1000) if t_cont and t_spp else "none",
                 "right after it (<= %g ms)" % (50 * r.loose))
        sent_back = count(back, SPP, t0) + count(back, CONTINUE, t0)
        r.check("%s: nothing back out of %s" % (name, src.upper()), sent_back == 0, sent_back, 0)
        c.send([STOP])
        time.sleep(.3)


@case("out-spp-loop", "7: with a loop and transport following on, the DAW's playhead moved while "
      "stopped: the gear gets FRIZZ's own Song Position, where the loop goes on, then Continue, "
      "not the DAW's (#469)", needs=("in|out",), literal="out")
def out_spp_loop(r):
    src, dst = pass_ports(r)
    r.setting(clock_source=SOURCES[src], midi_transport=1,
              midi_out=OUTS["trs" if dst == "trs" else "all"])
    c = r.clock(src, 120)
    r.wait_tempo(120, 1, 5)
    r.record_loop(1, 120)  # one bar: 16 16ths
    time.sleep(1.)
    log, back = r.captured(dst).log, r.captured(src).log
    c.send([STOP])
    time.sleep(.5)
    r.check("the DAW's Stop: paused", r.state()["looper"] == 3, remote.LOOPER[r.state()["looper"]], "paused")
    t0 = time.perf_counter()
    c.send(spp(301) + bytes([CONTINUE]))  # 16th 301: the loop's 13th
    time.sleep(.5)
    spps = [(t, m[1] | m[2] << 7) for t, m in log if t >= t0 and m[0] == SPP]
    conts = count(log, CONTINUE, t0)
    r.check("one Song Position and one Continue out of %s" % dst.upper(), len(spps) == 1 and conts == 1,
            "%d and %d" % (len(spps), conts), "1 and 1")
    at = spps[0][1] if spps else None
    r.check("the Song Position: where the loop goes on, not the DAW's 301", at in (13, 14), at,
            "13 or 14 (the loop's 13th 16th, or the next)")
    t_cont, _ = first(log, CONTINUE, t0)
    r.tcheck("then Continue", spps and t_cont is not None and 0 <= t_cont - spps[0][0] <= .2,
             "%+.1f ms after it" % ((t_cont - spps[0][0]) * 1000) if spps and t_cont else "none",
             "after it, within a 16th (125 ms) and a block")
    s = r.state()
    r.check("the loop plays on", s["looper"] == 2, remote.LOOPER[s["looper"]], "playing")
    sent_back = count(back, SPP, t0) + count(back, CONTINUE, t0)
    r.check("nothing back out of %s" % src.upper(), sent_back == 0, sent_back, 0)
    c.send([STOP])
    time.sleep(.2)


@case("out-factor", "7: clock factor at double/half with the DAW: the gear runs at FRIZZ's tempo "
      "(#73)", needs=("in|out",), literal="out")
def out_factor(r):
    src, dst = pass_ports(r)
    r.setting(clock_source=SOURCES[src], midi_out=OUTS["trs" if dst == "trs" else "all"])
    r.clock(src, 120)
    for value, want in ((200, 240), (50, 60)):
        r.setting(clock_factor=value)
        r.wait_tempo(want, 1, 4)
        t0 = time.perf_counter()
        time.sleep(2. if want > 100 else 3.)
        bpm, _ = ticks_bpm(r, dst, t0, None)
        r.tcheck("%s clock 120, factor %s: out of %s" % (src.upper(), "x2" if value == 200 else "x1/2", dst.upper()),
                abs(bpm - want) <= want * .01 * r.loose, "%.2f BPM" % bpm, "%d +-%g %%" % (want, r.loose))


@case("out-x2", "7: MIDI out at the clock factor x2 with a DAW's clock over USB: the gear behind "
      "stays in time over a few minutes (#85)", needs=("in|out",), literal="out")
def out_x2(r):
    src, dst = pass_ports(r)
    seconds = 8. if r.args.quick else 180. if r.args.long else 40.
    r.setting(clock_source=SOURCES[src], midi_out=OUTS["trs" if dst == "trs" else "all"], clock_factor=200)
    c = r.clock(src, 120)
    r.wait_tempo(240, 1, 5)
    t0 = time.perf_counter()
    r.note("measuring %g s" % seconds)
    time.sleep(seconds)
    out = ticks_after(r.captured(dst).log, t0)
    p_out, worst = fit(out)
    p_in, sent_worst = fit([t for t in c.ticks if t >= t0])
    drift = (p_out * 2. / p_in - 1.) * 60000. if p_out and p_in else None
    r.tcheck("%s out at x2 against the clock sent, over %g s" % (dst.upper(), seconds),
            drift is not None and abs(drift) <= 2. * r.slack,
            "%+.2f ms a minute" % drift if drift is not None else "no clock", "+-%g ms a minute" % (2 * r.slack))
    step = steps(out)
    r.tcheck("in step: its position", step is not None and step <= .002 * r.slack,
             "steps %.2f ms; single ticks up to %.1f ms off (the clock sent: %.1f)"
             % (step * 1000, worst * 1000, sent_worst * 1000) if step is not None else "-",
             "%g ms (a late tick is USB's or this computer's jitter)" % (2 * r.slack))


@case("out-knobs", "7: a drum machine on the TRS clock while you turn knobs fast (feedback on): it "
      "stays in time (#109)", literal="out")
def out_knobs(r):
    port = out_port(r)
    r.setting(clock_source=SOURCES["internal"], midi_out=OUTS["trs" if port == "trs" else "all"],
              midi_feedback=1)
    log = r.captured(port).log
    time.sleep(.5)
    t0 = time.perf_counter()
    time.sleep(3.)
    t1 = time.perf_counter()
    before = ticks_timed(log, t0, t1)
    p0, w0 = fit(*before)
    ccs = sum(1 for t, m in log if t >= t1 and m[0] & 0xF0 == 0xB0)
    end = time.monotonic() + 4.
    i = 0
    while time.monotonic() < end:
        for enc in (KNOB1_ENC, 1, 2, 3):
            r.turn(enc, 3 if i % 2 else -3)
        i += 1
        time.sleep(.02)
    t2 = time.perf_counter()
    time.sleep(.3)
    during = ticks_timed(log, t1 + .05, t2)
    p1, w1 = fit(*during)  # the ticks timed alone in their reads
    ccs = sum(1 for t, m in log if t >= t1 and m[0] & 0xF0 == 0xB0) - ccs
    r.check("knobs turned fast for 4 s: feedback out of %s" % port.upper(), ccs >= 20, "%d CCs" % ccs, ">= 20")
    r.tcheck("the clock's rate meanwhile", p0 and p1 and abs(p1 / p0 - 1.) <= .001 * r.loose,
            "%.2f -> %.2f BPM" % (bpm_of(p0), bpm_of(p1)) if p0 and p1 else "-", "the same +-%g %%" % (.1 * r.loose))
    timed = [(sum(ts), len(ts)) for _, ts in (before, during)]
    r.tcheck("its ticks' timing", w0 is not None and w1 is not None and w1 <= w0 + .001 * r.slack
             and all(n >= 24 for n, _ in timed),
             ("worst %.2f ms, before %.2f" % (w1 * 1000, w0 * 1000) if w0 is not None and w1 is not None else "-")
             + " (%d of %d ticks timed, before %d of %d)" % (timed[1] + timed[0]),
             "within %g ms of without, a beat's ticks at least alone in their reads" % r.slack)


# ---- 8. MIDI control ----

def moved(r, fx, index, send):
    """A knob set by a CC at 20, then 100: (moved up, the two readings)"""
    send(20)
    time.sleep(.08)
    a = r.knob(fx, index)
    send(100)
    time.sleep(.08)
    b = r.knob(fx, index)
    return b > a + .2, a, b


def until(get, want, limit=.5):
    """Reads get() until it gives want, LIMIT s at most: the s it took, None if it never did.
    A state FRIZZ takes on as MIDI comes in, waited for, not read at a fixed time after it:
    one read too early after a run of cases failed a latch that came (#276)"""
    start = time.monotonic()
    while True:
        if get() == want:
            return time.monotonic() - start
        if time.monotonic() - start >= limit:
            return None
        time.sleep(.01)


def latch_follows(r, fx, send, limit=.5):
    """A latch by CC, 127 then 0, each waited for up to LIMIT s: (the halves that didn't follow,
    "not on" / "not off", the slowest that did in s)"""
    index = remote.FX_NAMES.index(fx)
    missed, slowest = [], 0.
    for value, want in ((127, 1), (0, 0)):
        send(value)
        took = until(lambda: r.get("latch")[index], want, limit)
        if took is None:
            missed.append("not %s within %.0f ms" % ("on" if want else "off", limit * 1000))
        else:
            slowest = max(slowest, took)
    return ", ".join(missed), slowest


@case("cc-channels", "8: DAW automation per effect channel: CC 14-21 on channels 1-14 move that "
      "effect's knobs, on 15 the finalizer's at their fixed CCs (#345), CC 22 latches an effect; "
      "re-sending the same latch value doesn't fight the hand (#108)")
def cc_channels(r):
    bad, n = [], 0
    for ch in (1, 7, 14, 15) if r.args.quick else range(1, 16):
        # an effect's knobs at CC 14-21; the finalizer's parameters 0-6 at 14-20, 8-14 at 24-30
        fx = WHITE_FX[ch] if ch < 15 else "fin"
        knobs = [(14 + i, i) for i in range(8)] if ch < 15 else \
            [(14 + p, p) for p in range(7)] + [(16 + p, p) for p in range(8, 15)]
        for c, i in knobs:
            up, a, b = moved(r, fx, i, lambda v: r.send_usb(cc(ch, c, v)))
            empty = (fx, i) in EMPTY
            n += not empty
            if (b != a) if empty else not up:
                bad.append("ch %d CC %d: %.2f -> %.2f" % (ch, c, a, b))
    r.check("CC 14-21 at 20, then 100, on channels %s: the effect's knob" % ("1, 7, 14, 15" if r.args.quick else "1-15"),
            not bad, "; ".join(bad) or "all %d moved, the empty ones didn't" % n,
            "each moves up; a knob with nothing on it doesn't")
    latched = lambda fx: r.get("latch")[remote.FX_NAMES.index(fx)]  # noqa: E731
    bad, slowest = [], 0.
    for ch in range(1, 15):
        missed, took = latch_follows(r, WHITE_FX[ch], lambda v: r.send_usb(cc(ch, 22, v)))
        slowest = max(slowest, took)
        if missed:
            bad.append("ch %d: %s" % (ch, missed))
    r.check("CC 22 127 / 0 on channels 1-14: the latch", not bad,
            "; ".join(bad) or "all 14, the slowest after %.0f ms" % (slowest * 1000), "on, then off, within 500 ms")
    r.send_usb(cc(5, 22, 127))
    time.sleep(.08)
    r.set("latch", "filter", 0)  # the hand unlatches it
    r.send_usb(cc(5, 22, 127))
    time.sleep(.08)
    r.check("filter latched by CC 22, unlatched by hand, CC 22 127 again", not latched("filter"),
            "latched" if latched("filter") else "unlatched", "stays unlatched (acts on crossing 64)")
    r.send_usb(cc(5, 22, 0))


def global_map(r, port):
    send = lambda d: r.send(port, d)  # noqa: E731
    send(cc(GLOBAL, 23, 5))
    until(lambda: r.state()["selected"], remote.FX_NAMES.index("filter"))
    r.check("CC 23 5 (%s): the focus" % port.upper(), r.state()["selected"] == remote.FX_NAMES.index("filter"),
            remote.FX_NAMES[r.state()["selected"]] if r.state()["selected"] < 14 else "fin", "filter")
    up, a, b = moved(r, "filter", 0, lambda v: send(cc(GLOBAL, 14, v)))
    r.check("then CC 14 on channel 16: the filter's knob 1", up, "%.2f -> %.2f" % (a, b), "moves up")
    bad, slowest = [], 0.
    for k in range(1, 15):
        missed, took = latch_follows(r, WHITE_FX[k], lambda v: send(cc(GLOBAL, 101 + k, v)))
        slowest = max(slowest, took)
        if missed:
            bad.append("CC %d: %s" % (101 + k, missed))
    r.check("CC 102-115 127 / 0: the latches", not bad,
            ", ".join(bad) or "all 14, the slowest after %.0f ms" % (slowest * 1000), "on, then off, within 500 ms")
    bad = []
    for number, index in ((85, 0), (86, 1), (87, 2)):
        up, a, b = moved(r, "volume", index, lambda v: send(cc(GLOBAL, number, v)))
        if not up:
            bad.append("CC %d %.2f -> %.2f" % (number, a, b))
    r.check("CC 85-87: output gain, input gain, mix", not bad, "; ".join(bad) or "all 3 moved", "each moves up")


@case("cc-global", "8: a controller on channel 16: CC 23 picks the focus, CC 14-22 then edit that "
      "effect, CC 102-115 latch the effects, CC 85-87 set the gains and mix (#108), over USB")
def cc_global(r):
    global_map(r, "usb")


@case("cc-global-trs", "8: a hardware controller over TRS on channel 16: the same (#108)", needs=("in",))
def cc_global_trs(r):
    global_map(r, "trs")


CHOMPI_NOTE, FILTER_NOTE = 45, 55  # SHIFT's note, white key 5's (MANUAL.md "Notes: the keys")


def note_latch(r, port):
    send = lambda n, on: (r.send(port, note(GLOBAL, n, on)), time.sleep(.06))
    filt = 1 << 4
    latched = lambda: r.state()["latched"] & filt
    if latched():
        raise AssertionError("the filter latched before the case")

    def press(*order):
        """the notes on in order, then off in reverse (each held while the next goes down),
        as the twin's latch-either-midi"""
        for n in order:
            send(n, True)
        for n in order[::-1]:
            send(n, False)
        time.sleep(.3)
    # SHIFT's note, then the filter's: latched; the filter's alone: still; the filter's, then
    # SHIFT's: unlatched; SHIFT's, then the filter's, twice: latched, unlatched
    press(CHOMPI_NOTE, FILTER_NOTE)
    r.check("%s: note 45, then the filter's note 55: latched" % port.upper(), latched(), bool(latched()), True)
    press(FILTER_NOTE)
    r.check("%s: the filter's note alone: still latched" % port.upper(), latched(), bool(latched()), True)
    for n, on in ((FILTER_NOTE, True), (CHOMPI_NOTE, True), (FILTER_NOTE, False), (CHOMPI_NOTE, False)):
        send(n, on)
    time.sleep(.3)
    r.check("%s: the filter's note, then 45 (the other order): unlatched" % port.upper(), not latched(),
            bool(latched()), False)
    press(CHOMPI_NOTE, FILTER_NOTE)
    a = latched()
    press(CHOMPI_NOTE, FILTER_NOTE)
    r.check("%s: 45 then 55, twice: latched, unlatched" % port.upper(), a and not latched(),
            "%s, %s" % (bool(a), bool(latched())), "True, False")
    # CC 23 picks the focus without playing or latching anything
    before = r.state()
    r.send(port, cc(GLOBAL, 23, 2))  # the shifter (STATE's focus 1: FxId's order, not the keys')
    time.sleep(.2)
    s = r.state()
    r.check("%s: CC 23 = 2: the shifter in focus, nothing latched" % port.upper(),
            s["selected"] == 1 and s["latched"] == before["latched"],
            "focus %d, latched %#x (was %#x)" % (s["selected"], s["latched"], before["latched"]),
            "focus 1, latches as they were")


@case("note-latch", "8: MIDI notes as the hand (#463): note 45 (SHIFT) + an FX key's note in either "
      "order latches or unlatches it, the FX note alone keeps a latch; CC 23 focuses silently, over USB")
def note_latch_usb(r):
    note_latch(r, "usb")


@case("note-latch-trs", "8: the same from a controller on the TRS jack (#463)", needs=("in",))
def note_latch_trs(r):
    note_latch(r, "trs")


@case("focus-latch", "8: the global channel's CC 22 per focus (#444 C): a controller's toggle on CC 22 "
      "latches the freezer, CC 23 to the shifter, the next 127 latches it at once; a focus by hand "
      "likewise; the same focus again keeps the crossing")
def focus_latch(r):
    lat = lambda: r.state()["latched"]
    r.send_usb(cc(GLOBAL, 23, 1) + cc(GLOBAL, 22, 127))
    time.sleep(.15)
    r.check("CC 23 = 1, CC 22 = 127: the freezer latched", lat() & 1, "%#x" % lat(), "bit 0")
    r.send_usb(cc(GLOBAL, 23, 2) + cc(GLOBAL, 22, 127))
    time.sleep(.15)
    r.check("CC 23 = 2, CC 22 = 127 again: the shifter latched straight away", lat() & 3 == 3, "%#x" % lat(),
            "bits 0 and 1")
    r.tap("KEY_5")  # the filter in focus by hand (a plain press: no latch, #456)
    time.sleep(.3)
    r.send_usb(cc(GLOBAL, 22, 127))
    time.sleep(.15)
    r.check("the filter picked by hand, CC 22 = 127: latched", lat() >> 4 & 1, "%#x" % lat(), "bit 4")
    r.f.key(SHIFT, True)  # unlatched by hand
    time.sleep(.1)
    r.tap("KEY_5")
    time.sleep(.1)
    r.f.key(SHIFT, False)
    time.sleep(.3)
    r.check("SHIFT + the filter's key: unlatched by hand", not lat() >> 4 & 1, "%#x" % lat(), "bit 4 clear")
    r.send_usb(cc(GLOBAL, 23, 5) + cc(GLOBAL, 22, 127))
    time.sleep(.15)
    r.check("CC 23 = 5, the same focus, 127 again: no new crossing, stays unlatched", not lat() >> 4 & 1,
            "%#x" % lat(), "bit 4 clear")


@case("block-trs", "8: a TRS chain on channels 1-15, the settings page's lower D# off: that "
      "device's messages don't reach FRIZZ's effects; channel 16 and USB still work (#108)", needs=("in",))
def block_trs(r):
    r.setting(midi_block_trs=0)
    up, a, b = moved(r, "folder", 0, lambda v: r.send_trs(cc(3, 14, v)))
    r.check("effect channels over TRS off: CC 14 on channel 3 over TRS", abs(b - a) < .01,
            "%.2f -> %.2f" % (a, b), "the folder's knob stays")
    r.send_trs(cc(GLOBAL, 23, 3))
    up, a, b = moved(r, "folder", 0, lambda v: r.send_trs(cc(GLOBAL, 14, v)))
    r.check("channel 16 over TRS (focus 3, CC 14)", up, "%.2f -> %.2f" % (a, b), "moves")
    up, a, b = moved(r, "folder", 1, lambda v: r.send_usb(cc(3, 15, v)))
    r.check("channel 3 over USB", up, "%.2f -> %.2f" % (a, b), "moves")
    r.setting(midi_block_trs=1)
    up, a, b = moved(r, "folder", 2, lambda v: r.send_trs(cc(3, 16, v)))
    r.check("lower D# on again: channel 3 over TRS", up, "%.2f -> %.2f" % (a, b), "moves")


def all_knobs(r):
    """The effects' knobs (channels 1-14) as GET reads them: {(channel, knob 0-7): 0-1}"""
    got = {}
    for ch in range(1, 15):
        vals = r.get(WHITE_FX[ch])
        for i in range(8):
            got[(ch, i)] = remote.knob(vals[i])
    return got


@case("cc-burst-trs", "8: automation over TRS that never pauses and has no clock in it (a DAW "
      "writing CCs back to back): every CC counts, however long the line stays busy (#356)", needs=("in",))
def cc_burst_trs(r):
    # each effect's knobs (channels 1-14, CC 14-21) to 0 over USB, then in one write over TRS
    # each to a value of its own, once, 3 CCs FRIZZ leaves alone after each (the mod wheel on
    # channel 16): 12 bytes a knob, 1272 in all, no pause, no tick. A lost lap of libDaisy's
    # 256-byte ring loses the knobs in it
    knobs = [(ch, i) for ch in range(1, 15) for i in range(8) if (WHITE_FX[ch], i) not in EMPTY]
    before = r.state()
    for ch in range(1, 15):
        r.send_usb(b"".join(cc(ch, 14 + i, 0) for c, i in knobs if c == ch))
        time.sleep(.02)
    time.sleep(.3)
    zero = all_knobs(r)
    r.check("before: every knob at 0 (CCs over USB)", all(zero[k] < .05 for k in knobs),
            "%d of %d" % (sum(zero[k] < .05 for k in knobs), len(knobs)), "all")
    want = {k: 80 + n % 48 for n, k in enumerate(knobs)}
    burst = b"".join(cc(ch, 14 + i, want[(ch, i)]) + b"".join(cc(GLOBAL, 1, (3 * n + j) & 0x7F) for j in range(3))
                     for n, (ch, i) in enumerate(knobs))
    t0 = time.monotonic()
    r.send_trs(burst)
    # the first knobs (the freezer's, its first 96 bytes) while the line is still busy: a
    # Scarlett 18i20 sends a byte every ~0.5 ms, 0.7 s for the burst
    time.sleep(.2)
    vals = r.get("freezer")
    early = time.monotonic() - t0
    first = [k for k in knobs if k[0] == 1]
    moved = sum(remote.knob(vals[i]) > .3 for _, i in first)
    r.check("the freezer's knobs, first in the burst, %.0f ms after it was sent, the line still busy" % (early * 1000),
            moved == len(first), "%d of %d set" % (moved, len(first)), "all: handed over as they come")
    time.sleep(max(0., len(burst) * .0005 + 1. - (time.monotonic() - t0)))
    got = all_knobs(r)
    lost = [n for n, k in enumerate(knobs) if got[k] <= .3]
    off = max(abs(got[k] - knob7(want[k])) for k in knobs)
    r.note("%d bytes over TRS in one write; the knobs within %.3f of their CCs' values (stepped ones "
           "take the nearest step)" % (len(burst), off))
    r.check("every knob set by the burst (%d CCs, %d bytes)" % (len(knobs), len(burst)), not lost,
            "%d of %d%s" % (len(knobs) - len(lost), len(knobs), ", not: the knobs at bytes %s" % ", ".join(
                "%d-%d" % (12 * a, 12 * b + 11) for a, b in runs(lost)) if lost else ""), "all")
    after = r.state()
    r.check("the mod wheel on channel 16 moved nothing", (after["latched"], after["selected"], after["scene"])
            == (before["latched"], before["selected"], before["scene"]),
            "latched %s, selected %s" % (after["latched"], after["selected"]), "as before")


UART_ERROR = 5  # kCmdDev TEST's kUartError (MidiControl.h): FRIZZ-bench.bin only


def trs_restarts(r):
    """PROBE 0's count of the jack's receive started again after an error, None before it (#361)"""
    d = remote.dev_data(r.f, chompi.PROBE, [0], "PROBE")
    return chompi.get7(d[29:32]) if len(d) >= 32 else None


@case("trs-relisten", "8: a receive error on the TRS jack (a cable plugged in while the other end "
      "sends, a bad contact): MIDI in over TRS comes back by itself, without a restart (#361). "
      "FRIZZ-bench.bin only (--slot 13)", needs=("in",))
def trs_relisten(r):
    up, a, b = moved(r, "folder", 0, lambda v: r.send_trs(cc(3, 14, v)))
    r.check("before: CC 14 on channel 3 over TRS", up, "%.2f -> %.2f" % (a, b), "moves")
    before = trs_restarts(r)
    # a dense stream (the mod wheel on channel 16, 900 bytes, ~0.45 s from a Scarlett 18i20), and
    # in it the UART at twice its rate for 20 ms: its bytes break their frames meanwhile
    r.send_trs(b"".join(cc(GLOBAL, 1, n & 0x7F) for n in range(300)))
    time.sleep(.1)
    got = r.f.dev(chompi.TEST, [UART_ERROR], retries=1)
    if not got or got[0] != chompi.OK:
        raise Skip("TEST %d: %s (FRIZZ-bench.bin only)" % (
            UART_ERROR, chompi.DEV_STATUS.get(got[0], got[0]) if got else "no answer"))
    time.sleep(1.)
    after = trs_restarts(r)
    if after is None:
        r.note("no restart count in PROBE 0: a FRIZZ from before #361")
    else:
        r.check("the receive ended on an error and started again", after > before,
                "%d times" % (after - before), "at least once")
    up, a, b = moved(r, "folder", 1, lambda v: r.send_trs(cc(3, 15, v)))
    r.check("after: CC 15 on channel 3 over TRS", up, "%.2f -> %.2f" % (a, b), "moves: TRS MIDI in is back")


UART_RESTART = 6  # kCmdDev TEST's kUartRestart (MidiControl.h): FRIZZ-bench.bin only
MARKER = 0xFD  # what it sends out of the jack around each restart (MidiClock::TestRestarts)


def uart_restarts(r, how, n):
    """TEST kUartRestart: n restarts of the jack's receive (how 0 as Listen does, 1 through
    HAL's error callback), 4 ms apart, markers going out meanwhile; the markers sent so far"""
    got = r.f.dev(chompi.TEST, [UART_RESTART, how, n >> 14 & 0x7F, n >> 7 & 0x7F, n & 0x7F], retries=1)
    if not got or got[0] != chompi.OK or len(got[1]) < 3:
        raise Skip("TEST %d: %s (FRIZZ-bench.bin from #442 on)" % (
            UART_RESTART, chompi.DEV_STATUS.get(got[0], got[0]) if got else "no answer"))
    return chompi.get7(got[1][:3])


@case("trs-restart-tx", "8: the jack's receive started again after an error (as trs-relisten's) "
      "leaves MIDI out on TRS alone: a USB clock in pairs passed on out of the jack, the bench's "
      "restarts while it sends, none of its bytes cut (#442). FRIZZ-bench.bin only (--slot 13)",
      needs=("out",))
def trs_restart_tx(r):
    uart_restarts(r, 0, 0)  # a SKIP where there's no TEST
    r.setting(clock_source=SOURCES["usb"], midi_out=OUTS["trs"])
    c = r.clock("usb", 120, mode="pairs")
    r.wait_tempo(120, 1, 5)
    log = r.captured("trs").log
    n = 200
    # how 0: stopped, as most errors end it (Listen starts it at once); 1: the DMA's request
    # off, so the bytes coming in overrun into HAL's error callback (needs the jack's in)
    for how in (0, 1) if "in" in r.wiring else (0,):
        what = ("the restart" if how == 0 else "the restart through HAL's error callback")
        if how == 1:
            # a dense stream into the jack, the mod wheel on channel 16 (moves nothing), ~1.3 s
            r.send_trs(b"".join(cc(GLOBAL, 1, k & 0x7F) for k in range(1300)))
        restarts = trs_restarts(r)
        sent = uart_restarts(r, how, 0)
        t0 = time.perf_counter()
        uart_restarts(r, how, n)
        time.sleep(2.)
        t1 = time.perf_counter()
        sent = uart_restarts(r, how, 0) - sent
        after = trs_restarts(r)
        time.sleep(.2)
        got = count(log, MARKER, t0)
        other = sorted({"%02X" % m[0] for t, m in log if t >= t0 and m[0] not in (TICK, MARKER)})
        if restarts is not None and after is not None:
            r.check("%s: the receive ended and started again" % what, after - restarts >= (n if how == 0 else n // 2),
                    "%d times" % (after - restarts), "%d%s" % (n, "" if how == 0 else " (/2: bytes coming in)"))
        r.check("%s: the markers sent meanwhile all came out of the jack" % what, got == sent and sent > 0,
                "%d of %d" % (got, sent), "all")
        r.check("%s: nothing garbled" % what, not other, ", ".join(other) or "none", "none")
        # the clock passed on: as many ticks out as went in (the edges +-1)
        into = sum(1 for t in c.ticks if t0 + .2 <= t <= t1 - .2)
        out = len(ticks_after(log, t0 + .2 + .002, t1 - .2 + .002))
        r.check("%s: the clock passed on, no tick lost" % what, abs(into - out) <= 2,
                "%d out of %d" % (out, into), "+-2")
    r.stop_clocks()


def runs(ns):
    """Sorted numbers as (first, last) runs"""
    out = []
    for n in ns:
        if out and n == out[-1][1] + 1:
            out[-1] = (out[-1][0], n)
        else:
            out.append((n, n))
    return out


@case("shifter-page","8: CC 18 on channel 2 moves the shifter's mix; note 36 on channel 16 turns "
      "the page (#44)")
def shifter_page(r):
    up, a, b = moved(r, "shifter", 4, lambda v: r.send_usb(cc(2, 18, v)))
    r.check("CC 18 on channel 2: the shifter's page-2 knob 1 (mix)", up, "%.2f -> %.2f" % (a, b), "moves up")
    before = r.state()["page"]
    r.send_usb(note(GLOBAL, 36, True))
    time.sleep(.1)
    r.send_usb(note(GLOBAL, 36, False))
    time.sleep(.2)
    after = r.state()["page"]
    r.check("note 36 on channel 16: the page", before is not None and after != before,
            "%s -> %s" % (before, after), "turns")
    if after != before:
        r.send_usb(note(GLOBAL, 36, True))
        time.sleep(.1)
        r.send_usb(note(GLOBAL, 36, False))
        time.sleep(.2)


@case("chaos-cc", "8: the chaos key from real gear: CC 115 on channel 16 latches it, CC 14-17 on "
      "channel 14 set its knobs (#77)")
def chaos_cc(r):
    r.send_usb(cc(GLOBAL, 115, 127))
    time.sleep(.1)
    on = r.get("latch")[remote.FX_NAMES.index("chaos")]
    r.send_usb(cc(GLOBAL, 115, 0))
    r.check("CC 115 127 on channel 16: the chaos key", on, "latched" if on else "not latched", "latched")
    bad = []
    for i in range(4):
        up, a, b = moved(r, "chaos", i, lambda v: r.send_usb(cc(14, 14 + i, v)))
        if not up:
            bad.append("CC %d %.2f -> %.2f" % (14 + i, a, b))
    r.check("CC 14-17 on channel 14: its knobs", not bad, "; ".join(bad) or "all 4 moved", "each moves up")


# ---- morphs over MIDI: two scenes, the filter's knob 1 at 0.2 in one, 0.8 in the other ----

A, B = .2, .8


def two_scenes(r):
    blank = remote.scene_get(r.f, 0)
    for slot, k in ((1, A), (2, B)):
        scene = dict(blank, used=True, latched=[])
        scene["params"] = dict(blank["params"], filter=[k] + blank["params"]["filter"][1:])
        remote.scene_put(r.f, slot, scene)
    r.send_usb(bytes([0xC0 | (GLOBAL - 1), 1]))  # program change 1: scene 1 recalled
    time.sleep(.2)
    r.send_usb(cc(GLOBAL, 9, 4))  # morphs over 4 bars: time for CC 80 to take one
    r.send_usb(cc(GLOBAL, 3, 127))  # no scene, so the next CC 3 acts
    time.sleep(.1)
    return r.knob("filter", 0)


def fader(r):
    """Where MIDI's crossfader holds a morph, 0-1: the transport LEDs show it in white, the
    start on the reverse one, the scene on the forward one (NormalPage::Draw), at whatever
    brightness the card has; None if they don't. GET shows the scene the morph goes to, not
    where the glide is ("the knobs already show the scene", MANUAL.md)"""
    pth = r.f.leds()[0]
    rev, fwd = sum(pth[TRANSPORT_REV]), sum(pth[TRANSPORT_FWD])
    return fwd / float(rev + fwd) if rev + fwd else None


def fmt(t):
    return "-" if t is None else "%.2f" % t


@case("morph-back", "8: CC 3 to a scene, once it glides CC 3 for the same scene again, then CC 80 up "
      "and back down to 0: at 0 it's the scene you came from, with no jump on arriving there (#89)")
def morph_back(r):
    k = two_scenes(r)
    r.check("scene 1 recalled (program change 1)", abs(k - A) <= .01, "%.3f" % k, A)
    r.send_usb(cc(GLOBAL, 3, 2))
    time.sleep(.4)
    r.send_usb(cc(GLOBAL, 3, 2))
    time.sleep(.2)
    off, down = [], []
    values = list(range(8, 81, 8)) + list(range(72, 0, -8))
    for i, v in enumerate(values):
        r.send_usb(cc(GLOBAL, 80, v))
        time.sleep(.15)
        t = fader(r)
        if t is None or abs(t - v / 127.) > .06:
            off.append("%d: %s" % (v, fmt(t)))
        if i >= values.index(80):
            down.append(t)
    r.check("CC 80 8 .. 80 .. 8: the crossfader (transport LEDs)", not off and r.state()["morphing"],
            "; ".join(off) or "followed, morphing", "CC 80 / 127 +-0.06, the morph held")
    r.check("on the way down", all(a is not None and b is not None and b <= a for a, b in zip(down, down[1:])),
            " ".join(fmt(t) for t in down), "falls step by step, no jump")
    r.send_usb(cc(GLOBAL, 80, 0))
    time.sleep(.3)
    s, k = r.state(), r.knob("filter", 0)
    r.check("CC 80 0: back where it started", abs(k - A) <= .01 and not s["morphing"],
            "%.3f%s" % (k, ", morphing" if s["morphing"] else ", no morph"), "%.1f, scene 1's, no morph" % A)


@case("morph-hold", "8: CC 3 starts a morph, CC 80 at about half holds it; tap CHOMPI: nothing "
      "changes, CC 80 still moves and lands it. With CC 80 at 0, a tap leaves the morph waiting "
      "at its start (#86)")
def morph_hold(r):
    two_scenes(r)
    r.send_usb(cc(GLOBAL, 3, 2))
    time.sleep(.2)
    r.send_usb(cc(GLOBAL, 80, 64))
    time.sleep(.3)
    t1 = fader(r)
    time.sleep(1.)
    t2, s = fader(r), r.state()
    r.check("CC 80 64: halfway, and held", t1 is not None and abs(t1 - .5) <= .06 and t2 == t1 and s["morphing"],
            "%s, %s a second later%s" % (fmt(t1), fmt(t2), "" if s["morphing"] else ", no morph"),
            "0.50 +-0.06, still, morphing")
    r.tap(SHIFT)
    time.sleep(.3)
    t3, s = fader(r), r.state()
    r.check("CHOMPI tapped", t3 == t2 and s["morphing"],
            "%s%s" % (fmt(t3), ", morphing" if s["morphing"] else ", no morph"), "unchanged, morphing")
    r.send_usb(cc(GLOBAL, 80, 127))
    time.sleep(.4)
    s, k = r.state(), r.knob("filter", 0)
    r.check("CC 80 127: lands", abs(k - B) <= .01 and not s["morphing"],
            "%.3f%s" % (k, ", morphing" if s["morphing"] else ", no morph"), "%.1f, scene 2's, no morph" % B)
    two_scenes(r)
    r.send_usb(cc(GLOBAL, 3, 2))
    time.sleep(.2)
    r.send_usb(cc(GLOBAL, 80, 0))
    time.sleep(.3)
    r.tap(SHIFT)
    time.sleep(.6)
    t4, s = fader(r), r.state()
    r.check("a new morph, CC 80 at 0, CHOMPI tapped", s["morphing"] and t4 is not None and t4 <= .02,
            "%s%s" % (fmt(t4), ", morphing" if s["morphing"] else ", no morph"), "waiting at its start (0)")
    r.send_usb(cc(GLOBAL, 81, 0) + cc(GLOBAL, 81, 127) + cc(GLOBAL, 81, 0))  # stopped where it is


def landed(r, seconds=12.):
    """Waits until no morph runs, at most seconds (2 bars at 50 BPM, the slowest, are 9.6 s);
    the state then"""
    end = time.monotonic() + seconds
    s = r.state()
    while s["morphing"] and time.monotonic() < end:
        time.sleep(.1)
        s = r.state()
    return s


@case("morph-sequences", "8: #175: CC 83 + CC 9 a drop; CC 3 during it stops it and morphs to its "
      "scene instead; CC 83 during a morph a drop of its own, not the morph's landing; CC 81 stops "
      "one where it is")
def morph_sequences(r):
    two_scenes(r)
    # over 2 bars: none lands before it's looked at
    r.send_usb(cc(GLOBAL, 9, 2) + cc(GLOBAL, 83, 127) + cc(GLOBAL, 83, 2))
    time.sleep(.2)
    s = r.state()
    r.check("CC 83 2: a drop to scene 2 waits", s["morphing"] and s["scene"] == 2,
            "scene %d%s" % (s["scene"], ", morphing" if s["morphing"] else ""), "scene 2, morphing")
    r.send_usb(cc(GLOBAL, 3, 1))
    time.sleep(.2)
    s = r.state()
    r.check("CC 3 1 during it: a morph to scene 1 instead", s["morphing"] and s["scene"] == 1,
            "scene %d%s" % (s["scene"], ", morphing" if s["morphing"] else ""), "scene 1, morphing")
    s, k = landed(r), r.knob("filter", 0)
    r.check("it lands on scene 1, no drop to 2", abs(k - A) <= .01 and not s["morphing"],
            "%.3f%s" % (k, ", morphing" if s["morphing"] else ""), "%.1f, scene 1's" % A)
    r.send_usb(cc(GLOBAL, 3, 127) + cc(GLOBAL, 3, 2))
    time.sleep(.2)
    r.send_usb(cc(GLOBAL, 83, 127) + cc(GLOBAL, 83, 1))
    time.sleep(.2)
    s = r.state()
    r.check("CC 83 1 during a morph to scene 2: a drop to scene 1", s["morphing"] and s["scene"] == 1,
            "scene %d%s" % (s["scene"], ", morphing" if s["morphing"] else ""), "scene 1, morphing")
    s, k = landed(r), r.knob("filter", 0)
    r.check("it lands on scene 1", abs(k - A) <= .01 and not s["morphing"],
            "%.3f%s" % (k, ", morphing" if s["morphing"] else ""), "%.1f, scene 1's" % A)
    r.send_usb(cc(GLOBAL, 3, 127) + cc(GLOBAL, 3, 2))
    time.sleep(.2)
    r.send_usb(cc(GLOBAL, 81, 0) + cc(GLOBAL, 81, 127) + cc(GLOBAL, 81, 0))
    time.sleep(.2)
    s = r.state()
    r.check("CC 81 up through 64: stopped where it is, scene 2 edited",
            not s["morphing"] and s["scene"] == 2 and s["flags"] & 1,
            "scene %d%s%s" % (s["scene"], ", morphing" if s["morphing"] else "", ", edited" if s["flags"] & 1 else ""),
            "scene 2, edited, no morph")


@case("rpn", "8: a DAW that sets its pitch-bend range (RPN 0, then RPN null) on FRIZZ's channels: "
      "no knob moves (#85)")
def rpn(r):
    groups = remote.FX_NAMES + ["fin", "volume"]
    before = {g: r.get(g) for g in groups}
    for ch in range(1, 17):
        r.send_usb(cc(ch, 101, 0) + cc(ch, 100, 0) + cc(ch, 6, 12) + cc(ch, 38, 0)
                   + cc(ch, 101, 127) + cc(ch, 100, 127))
        r.send_usb(cc(ch, 99, 1) + cc(ch, 98, 8) + cc(ch, 6, 100) + cc(ch, 96, 1) + cc(ch, 97, 1))
    time.sleep(.3)
    changed = ["%s %d" % (g, i) for g in groups for i, (a, b) in enumerate(zip(before[g], r.get(g))) if a != b]
    r.check("RPN 0 = 12 and null, an NRPN and data inc/dec on channels 1-16", not changed,
            ", ".join(changed) or "nothing moved", "nothing moves")


@case("feedback", "8: with MIDI out and feedback on, turning a knob, latching, picking an effect and "
      "VOLUME go out as their CCs; echoed back, nothing loops (#109; the rings and faders are the "
      "user's)")
def feedback(r):
    r.setting(midi_out=OUTS["all"], midi_feedback=1)
    log = r.usb.log
    for key in ("KEY_3", "KEY_5"):  # SHIFT + the folder's key, then the filter's: picked
        if key == "KEY_5":
            time.sleep(.1)
            t0 = time.perf_counter()  # the filter's pick is a change, whatever the focus was
        r.f.key(SHIFT, True)
        time.sleep(.05)
        r.tap(key)
        r.f.key(SHIFT, False)
    time.sleep(.1)
    r.turn(KNOB1_ENC, 10)
    time.sleep(.1)
    r.f.key("KEY_5", True)  # the filter's key, then SHIFT: latched
    time.sleep(.05)
    r.tap(SHIFT)
    r.f.key("KEY_5", False)
    time.sleep(.1)
    r.turn(VOLUME_ENC, 5)
    time.sleep(.3)
    got = {(m[0], m[1]) for t, m in log if t >= t0 and len(m) == 3 and m[0] & 0xF0 == 0xB0}
    for what, want in (("the focus: CC 23 on 16", (0xBF, 23)),
                       ("knob 1: CC 14 on 5", (0xB4, 14)),
                       ("knob 1, in focus: CC 14 on 16", (0xBF, 14)),
                       ("the latch: CC 22 on 5", (0xB4, 22)),
                       ("the latch: CC 106 on 16", (0xBF, 106)),
                       ("VOLUME: CC 85 on 16", (0xBF, 85))):
        r.check(what, want in got, "sent" if want in got else "not sent", "sent")
    # a DAW echoing everything it gets back to FRIZZ: what comes in isn't sent back, so no loop
    seen = len(log)
    echo = threading.Event()

    def echoer():
        i = seen
        while not echo.is_set():
            while i < len(log):
                m = log[i][1]
                if m[0] & 0xF0 == 0xB0:
                    r.send_usb(m)
                i += 1
            time.sleep(.005)
    th = threading.Thread(target=echoer, daemon=True)
    th.start()
    r.turn(KNOB1_ENC, -10)
    time.sleep(1.)
    t1 = time.perf_counter()
    time.sleep(1.)
    echo.set()
    th.join()
    later = sum(1 for t, m in log if t >= t1 and m[0] & 0xF0 == 0xB0)
    r.check("echoed back, a knob turned: CCs out in the second after it settled", later == 0, later, 0)
    r.f.key("KEY_5", True)
    time.sleep(.05)
    r.f.key("KEY_5", False)  # the latch cleared


def input_bits(r):
    """STATE's input byte (#177): the mic 1 (else AUX), its effects dry 2, it live 4, USB 8 (#196)"""
    return r.f.state()[25]


@case("input-cc", "8: CC 84 picks the input's source in thirds (AUX, the mic, USB, #196), CC 116 "
      "its effects, CC 117 it live, in the order they came, each source its own; feedback sends "
      "them, not what came; CC 88 and 91, as a keyboard sends them at power-on, do nothing (#177)")
def input_cc(r):
    r.setting(midi_out=OUTS["all"], midi_feedback=1)
    log = r.usb.log
    r.send_usb(cc(GLOBAL, 84, 0))  # AUX: the cable is in on the device
    time.sleep(.2)
    start = input_bits(r)
    groups = remote.FX_NAMES + ["fin", "volume"]
    before = {g: r.get(g) for g in groups}
    for ch in (1, GLOBAL):
        r.send_usb(cc(ch, 88, 0) + cc(ch, 91, 40) + cc(ch, 91, 0))
        if r.trs_fd is not None and "in" in r.wiring:
            r.send_trs(cc(ch, 88, 0) + cc(ch, 91, 40))
    time.sleep(.3)
    changed = ["%s %d" % (g, i) for g in groups for i, (a, b) in enumerate(zip(before[g], r.get(g))) if a != b]
    r.check("CC 88 and 91 on channels 1 and 16: nothing changes", not changed and input_bits(r) == start,
            ", ".join(changed) or "input %d" % input_bits(r), "nothing, input %d" % start)
    # AUX dry, then off; CC 116 and 84 in one packet: 116 is AUX's, before the switch
    t0 = time.perf_counter()
    r.send_usb(cc(GLOBAL, 116, 127))
    time.sleep(.2)
    aux_dry = input_bits(r)
    r.send_usb(cc(GLOBAL, 117, 0))
    time.sleep(.2)
    aux_off = input_bits(r)
    r.send_usb(cc(GLOBAL, 116, 0) + cc(GLOBAL, 84, 64))
    time.sleep(.2)
    mic = input_bits(r)
    r.send_usb(cc(GLOBAL, 84, 127))
    time.sleep(.2)
    usb = input_bits(r)
    r.send_usb(cc(GLOBAL, 84, 0))
    time.sleep(.2)
    aux = input_bits(r)
    r.check("CC 116 at 127: AUX dry", aux_dry == 2 | (start & 4), aux_dry, 2 | (start & 4))
    r.check("CC 117 at 0: AUX not live", aux_off == 2, aux_off, 2)
    r.check("CC 116 at 0, then CC 84 at 64 in one packet: the 116 AUX's, the mic its own", mic & 1 and aux == 0,
            "the mic %d, AUX then %d" % (mic, aux), "the mic odd, AUX 0")
    r.check("CC 84 at 127: USB, its own, live through the effects (#196)", usb & 8 and usb & 4 and not usb & 3,
            usb, "USB, live: 12")
    time.sleep(.3)
    back = {(m[1], m[2]) for t, m in log if t >= t0 and len(m) == 3 and m[0] == 0xBF and m[1] in (84, 116, 117)}
    sent_back = sorted(k for k in back if k[0] == 84)
    r.check("what came isn't sent back: no CC 84", not sent_back, sent_back or "none", "none")
    # the hand: the settings page's G# and A# go out as CC 117 and 116 (#243)
    t1 = time.perf_counter()
    r.f.send(remote.SWITCH, [2])
    time.sleep(.3)
    r.tap("KEY_24")  # AUX live again
    time.sleep(.2)
    r.tap("KEY_25")  # dry
    time.sleep(.3)
    r.f.send(remote.SWITCH, [0])
    time.sleep(.3)
    out = {(m[1], m[2]) for t, m in log if t >= t1 and len(m) == 3 and m[0] == 0xBF}
    r.check("the settings page's G# and A#: CC 117 at 127 and CC 116 at 127 out", (117, 127) in out and (116, 127) in out,
            sorted(k for k in out if k[0] in (84, 116, 117)) or "none", "(116, 127), (117, 127)")
    r.check("... and AUX live and dry", input_bits(r) == 6, input_bits(r), 6)


@case("no-double", "8: a DAW recording what it also sends (track monitoring on): automation that "
      "plays back into FRIZZ doesn't come back and double up (#109)")
def no_double(r):
    r.setting(midi_out=OUTS["all"], midi_feedback=1)
    log = r.usb.log
    t0 = time.perf_counter()
    for v in range(0, 128, 4):
        r.send_usb(cc(5, 14, v) + cc(GLOBAL, 85, v) + cc(12, 18, v))
        time.sleep(.02)
    time.sleep(.5)
    back = [m.hex() for t, m in log if t >= t0 and m[0] & 0xF0 == 0xB0]
    r.check("CC 14 on 5, CC 85 on 16, CC 18 on 12 swept: CCs back", not back,
            "%d: %s" % (len(back), " ".join(back[:6])) if back else "none", "none")


@case("sysex-thru", "8: a DAW that soft-thrus SysEx back to FRIZZ: remote.py state still answers (#106)")
def sysex_thru(r):
    r.usb.echo = r.send_usb
    try:
        answered = sum(1 for _ in range(20) if r.f.ask(remote.STATE, required=False) is not None)
        got = r.f.dev(chompi.IDENT)
    finally:
        r.usb.echo = None
    r.check("FRIZZ's answers echoed back to it: 20 STATE queries", answered == 20, "%d answered" % answered, 20)
    r.check("and IDENT (0x3E)", got is not None and got[0] == chompi.OK, "answered" if got else "none", "answered")



@case("sysex-cut", "8: a SysEx cut short (its sender unplugged mid-message) doesn't deafen FRIZZ: the "
      "CCs after it are heard (#166)")
def sysex_cut(r):
    latched = lambda: r.get("latch")[remote.FX_NAMES.index("filter")]  # noqa: E731
    r.send_usb(cc(5, 22, 0))
    time.sleep(.1)
    # three bytes: ALSA sends a SysEx's packet once it has three, and the CC after it whole
    r.send_usb(bytes([0xF0, 0x7D, 0x43]) + cc(5, 22, 127))
    time.sleep(.1)
    on = latched()
    r.send_usb(bytes([0xF0, 0x01, 0x02]) + cc(5, 22, 0))
    time.sleep(.1)
    off = not latched()
    r.check("F0 7D 43, cut by CC 22 127 on channel 5: the filter latches", on, "latched" if on else "not",
            "latched")
    r.check("F0 01 02, cut by CC 22 0: it unlatches", off, "unlatched" if off else "still latched", "unlatched")

# what stays with the user: feel, ears, eyes, real gear (#55 sections 7, 8 and Every release)
FOR_THE_USER = [
    "Every release: real MIDI gear, a clock from your DAW or a sequencer over TRS and USB; a "
    "controller's notes and CCs do what MANUAL.md says",
    "7: the clock factor's pulse (lower G#) by eye against the DAW's beat (the suite measures "
    "its period)",
    "7: the echoes by ear at 170-300 BPM (the suite checks the tempo the delay follows)",
    "8: DAW automation in Ableton Live or Bitwig, one track per effect: the knob LEDs follow "
    "(REAPER is #114's step 2)",
    "8: a hardware fader on CC 80 feels usable (#75)",
    "8: a controller with LED rings or motor faders (or a DAW's takeover): the rings follow; "
    "nothing jumps back or loops (#109; the suite checks the CCs and the echo)",
    "8: on a battery, with MIDI out on TRS+USB and feedback on: the UI, LEDs and scene saves "
    "feel as without feedback (#109)",
]


# ======== running them ========

def detect_wiring(r):
    """Which way the TRS adapter goes, by what FRIZZ shows: its MIDI out arriving at the
    interface (out), a clock out of the interface that its tempo follows (in)"""
    found = set()
    if r.trs is None:
        return found
    r.setting(midi_out=OUTS["trs"], clock_source=SOURCES["internal"])
    mark = r.trs.mark()
    time.sleep(1.)
    if any(m[0] == TICK for _, m in r.trs.since(mark)):
        found.add("out")
    r.setting(midi_out=0, clock_source=SOURCES["trs"])
    r.clock("trs", 137)
    if r.wait_tempo(137, 1, 4)[0] is not None:
        found.add("in")
    r.stop_clocks()
    r.setting(clock_source=0)
    return found


WIRINGS = {"out": {"out"}, "in": {"in"}, "both": {"in", "out"}, "none": set()}


def chosen(cases, a):
    """--cases' cases (all without), in CASES' order; exits naming one there's none of"""
    if not a.cases:
        return cases
    wanted = a.cases.split(",")
    unknown = [w for w in wanted if w not in {c.id for c in cases}]
    if unknown:
        sys.exit("no case %s (--list)" % ", ".join(unknown))
    return [c for c in cases if c.id in wanted]


def list_cases(cases):
    for c in cases:
        print("%-15s %-7s %s" % (c.id, "/".join(c.needs) or "usb", c.item))


def keep_card(f):
    """PERSIST 0 before anything (exits for a FRIZZ that can't), then IDENT: 'build X'; waits
    out the boot of a FRIZZ just started"""
    if not f.ram_only():
        sys.exit("FRIZZ can't keep the card out of it (PERSIST 0): a build from before it; not run")
    print("the card isn't written until FRIZZ restarts (PERSIST 0): everything stays in RAM", flush=True)
    got = f.dev(chompi.IDENT)
    info = chompi.parse_ident(got[1]) if got and got[0] == chompi.OK else None
    if info and info["uptime"] < READY:
        time.sleep(READY - info["uptime"])  # just started: the play page isn't ready yet
    return "build %08x" % info["crc"] if info else "?"


def card_lost(r):
    """None while FRIZZ still keeps the card out of it (remote.in_ram); else why not, kept in
    r.lost, which stops the run: a FRIZZ that restarted would write the card with every setting
    and scene put after it (#171)"""
    try:
        why = remote.in_ram(r.f)
    except (Exception, SystemExit) as e:
        stopped_meanwhile()
        why = "IDENT: %s" % e
    r.lost = r.lost or why
    return why


def runnable(c, wiring):
    """None, or why it can't run with this wiring"""
    for need in c.needs:
        if not any(w in wiring for w in need.split("|")):
            return {"in": "needs the interface's MIDI out into the CHOMPI's TRS in",
                    "out": "needs the CHOMPI's TRS out into the interface's MIDI in",
                    "in|out": "needs the TRS adapter one way or the other"}[need]
    return None


class Snapshot:
    """What the cases change, to put back: the knobs, latches, VOLUME's levels, scenes 1-2"""

    GROUPS = remote.FX_NAMES + ["fin", "latch", "volume"]

    def __init__(self, r):
        self.values = {g: r.get(g) for g in self.GROUPS}
        self.scenes = {s: remote.scene_get(r.f, s) for s in (1, 2)}
        self.settings = r.get("settings")

    def restore(self, r, scenes=False):
        for g in self.GROUPS:
            now = r.get(g)
            for i, (want, got) in enumerate(zip(self.values[g], now)):
                if want != got:
                    v = want if g == "latch" else "%.6f" % remote.knob(want)
                    r.set(g, i, v)
        if scenes:
            for slot, scene in self.scenes.items():
                remote.scene_put(r.f, slot, scene)

    def restore_settings(self, r):
        for name, v in zip(remote.SETTING_NAMES, self.settings):
            if name in BASELINE:
                r.set("settings", name, v)


def reset(r, snap, scenes):
    r.stop_clocks()
    r.usb.echo = None
    r.f.release()
    r.f.send(remote.SWITCH, [0])
    # a FRIZZ from before USB audio (#196) has fewer settings: the ones it has
    r.setting(**{k: v for k, v in BASELINE.items() if remote.SETTING_NAMES.index(k) < len(snap.settings)})
    r.erase()
    snap.restore(r, scenes)
    time.sleep(.6)  # the clock let go (0.5 s)


def run_case(r, c, snap):
    r.checks, r.notes, r.timing_fails = [], [], 0
    why = runnable(c, r.wiring)
    if why:
        return "SKIP", why
    print("-- %s: %s" % (c.id, c.item), flush=True)
    status = None
    r.started = time.monotonic()
    if card_lost(r):
        return "FAIL", "not run: %s" % r.lost
    try:
        reset(r, snap, False)
        c.run(r)
    except Skip as e:
        status, detail = "SKIP", str(e)
    except (AssertionError, SystemExit) as e:
        stopped_meanwhile()
        status, detail = "FAIL", "stopped: %s" % e
    except Exception:
        stopped_meanwhile()
        status, detail = "FAIL", "error: " + traceback.format_exc().strip().splitlines()[-1]
        if r.args.verbose:
            traceback.print_exc()
    finally:
        try:
            if not card_lost(r):
                reset(r, snap, c.id.startswith("morph"))
        except (Exception, SystemExit) as e:
            print("   (putting things back: %s)" % e, file=sys.stderr)
    if r.lost:
        return "FAIL", "stopped: %s" % r.lost
    if status is None:
        if not r.checks:
            status, detail = "FAIL", "nothing checked"
        else:
            status = "PASS" if all(ok for ok, *_ in r.checks) else "FAIL"
            detail = ""
    return status, detail


def log_case(path, r, c, status, marks):
    """What came during case c, after the taps' marks: a line a message, "usb" or "trs", the
    time it came (perf_counter, s: one read's messages share it) and its bytes in hex"""
    with open(path, "a") as f:
        f.write("# %s %s %s\n" % (c.id, status, time.strftime("%Y-%m-%d %H:%M:%S")))
        for port, tap, mark in (("usb", r.usb, marks[0]), ("trs", r.trs, marks[1])):
            for t, m in tap.since(mark) if tap else ():
                f.write("%s %.6f %s\n" % (port, t, m.hex(" ")))


def summary(results, wiring, ident, args):
    lines = ["### MIDI suite (`tools/midisuite.py`%s)" % (" --quick" if args.quick else " --long" if args.long else ""),
             "",
             "FRIZZ: %s. Wiring: %s." % (ident, wiring_text(wiring)), ""]
    counts = {}
    for c, status, detail, checks, notes in results:
        counts[status] = counts.get(status, 0) + 1
    lines.append("**%s**" % ", ".join("%d %s" % (counts[s], s) for s in ("PASS", "FAIL", "SKIP") if s in counts))
    lines.append("")
    lines.append("| case | #55 item | result | measured |")
    lines.append("|---|---|---|---|")
    for c, status, detail, checks, notes in results:
        measured = "; ".join(("%s: %s" % (what, got)) + ("" if ok else " (want %s)" % want)
                             for ok, what, got, want in checks)
        if detail:
            measured = (measured + "; " if measured else "") + detail
        if c.literal and c.literal not in wiring and status != "SKIP":
            measured += "; over USB, standing in for the TRS %s" % c.literal
        lines.append("| `%s` | %s | %s | %s |" % (c.id, c.item.replace("|", "/"), status,
                                                  measured.replace("|", "/")))
    other = [c for c, status, *_ in results
             if status == "SKIP" and runnable(c, wiring)
             or status != "SKIP" and c.literal and c.literal not in wiring]
    if other:
        lines += ["", "**Still open with the other wiring** (turn the TRS adapter round and run "
                  "`midisuite.py --cases %s`):" % ",".join(c.id for c in other), ""]
        for c in other:
            lines.append("- `%s`: %s" % (c.id, runnable(c, wiring) or "ran over USB; the TRS %s "
                                                                     "needs it" % c.literal))
    lines += ["", "**For the user** (feel, ears, eyes, real gear):", ""]
    lines += ["- [ ] " + item for item in FOR_THE_USER]
    return "\n".join(lines) + "\n"


def wiring_text(w):
    return {frozenset(): "no TRS MIDI (USB only)",
            frozenset(["out"]): "CHOMPI TRS out -> interface MIDI in (A)",
            frozenset(["in"]): "interface MIDI out -> CHOMPI TRS in (B)",
            frozenset(["in", "out"]): "both ways"}[frozenset(w)]


def put_back(r, snap):
    """The knobs, latches, levels, scenes 1-2 and settings as they were, in RAM; False if that
    failed, or wasn't done: FRIZZ no longer keeps the card out of it"""
    if card_lost(r):
        print("not put back: %s" % r.lost, file=sys.stderr)
        return False
    try:
        reset(r, snap, True)
        snap.restore_settings(r)
        return True
    except (Exception, SystemExit) as e:
        print("putting things back: %s" % e, file=sys.stderr)
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--slot", type=int, choices=chompi.FRIZZ_SLOTS + (chompi.BENCH_SLOT,), default=chompi.TEST_SLOT,
                    help="the FRIZZ slot (12 by default), started unless it runs; the bench's (13) "
                    "for trs-relisten: the one running, once its bench is done (remote.py bench --wait)")
    ap.add_argument("--device", help="FRIZZ's raw MIDI node (as remote.py's; the twin's for the host check)")
    ap.add_argument("--trs", help="the raw MIDI node on the TRS jack (FRIZZ_TRS_MIDI; found by name)")
    ap.add_argument("--wiring", choices=["auto"] + list(WIRINGS), default="auto")
    ap.add_argument("--cases", help="only these, by id, comma separated")
    ap.add_argument("--quick", action="store_true", help="the long cases short (the host check)")
    ap.add_argument("--long", action="store_true", help="the 'over a few minutes' cases for 3 minutes")
    ap.add_argument("--out", help="the summary into FILE too")
    ap.add_argument("--log", help="every message that came during each case into FILE, timed (appended)")
    ap.add_argument("--list", action="store_true", help="the cases, what they need, and leave")
    ap.add_argument("-v", "--verbose", action="store_true", help="every check as it's made")
    a = ap.parse_args()
    if a.list:
        list_cases(CASES)
        return
    cases = chosen(CASES, a)
    stop_on_signals()

    if a.slot == chompi.BENCH_SLOT and not a.device:
        # the bench that runs, its bench done (remote.py bench --wait): not started again,
        # which would run its bench anew
        device = chompi.wait_for("frizz", 30)
        info = chompi.ident(device)
        if not info or info["kind"] != "bench":
            sys.exit("--slot %d: the bench doesn't run (flash.py --bench, then remote.py bench --wait)"
                     % chompi.BENCH_SLOT)
    else:
        device = a.device or chompi.to_frizz(a.slot)
    f = remote.Frizz(device)
    trs_node = a.trs or (None if a.wiring == "none" else midiport.trs_midi())
    chompi.device_guard(trs_node)
    trs_fd = os.open(trs_node, os.O_RDWR | os.O_NONBLOCK) if trs_node else None
    r = Rig(f, trs_fd, a)
    ident = keep_card(f)
    snap = Snapshot(r)
    results, timing_only = [], []
    stopped, restored = None, False
    try:
        reset(r, snap, False)
        r.wiring = detect_wiring(r) if a.wiring == "auto" else WIRINGS[a.wiring]
        print("wiring: %s%s" % (wiring_text(r.wiring), " (found)" if a.wiring == "auto" else ""), flush=True)
        for c in cases:
            marks = (r.usb.mark(), r.trs.mark() if r.trs else 0)
            status, detail = run_case(r, c, snap)
            results.append((c, status, detail, r.checks, r.notes))
            if a.log:
                log_case(a.log, r, c, status, marks)
            print("%-4s %s%s%s" % (status, c.id, " (%.0f s)" % (time.monotonic() - r.started)
                                   if status != "SKIP" else "", ": " + detail if detail else ""), flush=True)
            for ok, what, got_, want in r.checks:
                if not ok:
                    print("       %s: %s, want %s" % (what, got_, want), flush=True)
            if status == "FAIL" and not detail and r.timing_fails == sum(1 for ok, *_ in r.checks if not ok):
                timing_only.append(c.id)
            if r.lost:
                print("stopped: %s" % r.lost, file=sys.stderr)
                break
    except (KeyboardInterrupt, Exception) as e:
        if not isinstance(e, KeyboardInterrupt) and not stop_signal:
            raise
        stopped = getattr(e, "signum", None) or stop_signal or signal.SIGINT
        print("interrupted" if stopped == signal.SIGINT else "stopped (signal %d)" % stopped,
              file=sys.stderr)
    finally:
        restored = put_back(r, snap)
        r.usb.close()
        if r.trs:
            r.trs.close()
    text = summary(results, r.wiring, ident, a)
    print()
    print(text)
    if timing_only:
        print("failed on timing alone: %s" % ",".join(timing_only))
    if midiport.LATE[0]:
        # this computer's part in the timing: a tick sent late, the next ones close behind it
        print("the clock from here: sent up to %.1f ms late" % (midiport.LATE[0] * 1000))
    if a.out:
        with open(a.out, "w") as out:
            out.write(text)
    if not a.device and chompi.frizz_slot not in (None, chompi.TEST_SLOT):
        print("FRIZZ on slot %d keeps what the suite changed in RAM until it restarts: "
              "./flash.py --run %d" % (chompi.frizz_slot, chompi.frizz_slot), file=sys.stderr)
    sys.exit(exit_code(any(s == "FAIL" for _, s, *_ in results) or not restored, stopped))


if __name__ == "__main__":
    main()
