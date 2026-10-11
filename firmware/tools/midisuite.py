#!/usr/bin/env python3
"""midisuite.py: the release test's MIDI clock, sync, MIDI out and MIDI control items (#55,
sections 7 and 8) on the device, unattended, with this computer as an idealised DAW (#114,
step 1).

    ./midisuite.py [--slot 12] [--wiring auto|out|in|both|none] [--cases ID,ID ...]
                   [--quick | --long] [--out FILE] [-v]
    ./midisuite.py --list

Each case sends what a DAW or a sequencer would (a clock over USB or the TRS jack, Start /
Stop / Continue / Song Position, CCs, notes, program changes, FRIZZ's SysEx for the hand's
keys and knobs), reads what FRIZZ makes of it (remote.py's STATE, GET, the LEDs, and every
byte its MIDI out sends, timed as it comes), and judges it against a tolerance: PASS, FAIL,
or SKIP when the wiring can't show it. It names the #55 item it covers. At the end a summary
in Markdown for the PR (--out writes it to FILE too): the results, the cases still open with
the other wiring, and what stays with the user (feel, ears, eyes, real gear).

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
in RAM. --quick shortens the long cases (the host check, test/midisuite.cpp), --long runs the
"over a few minutes" ones for 3 minutes. It plays on the FRIZZ on --slot (12, the test slot,
by default), started if it isn't the one running, under the lock (tools/chompi.py).

Linux (ALSA raw MIDI), Python 3 without packages.
"""
import argparse
import os
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
# the knobs an effect has nothing on (MANUAL.md "Page 2"): (effect, knob 0-7), dark, doing nothing
EMPTY = {("flanger", 4), ("resonator", 4), ("comp", 5)} | {("chaos", i) for i in range(4, 8)}
# the settings each case starts from: the global channel 16, the effect channels over TRS on,
# nothing else
BASELINE = dict(midi_channel=16, midi_transport=0, clock_factor=100, clock_source=0, midi_out=0,
                midi_block_trs=1, midi_feedback=0)
SOURCES = dict(auto=0, trs=1, usb=2, internal=3)
OUTS = dict(off=0, trs=1, all=2)


def cc(channel, number, value):
    return bytes([0xB0 | (channel - 1), number, value])


def note(channel, number, on):
    return bytes([(0x90 if on else 0x80) | (channel - 1), number, 0x7F if on else 0])


def knob7(v):
    """A CC's value as the knob's 0-1 (MidiToKnob, 7 bits)"""
    return v / 128. if v <= 64 else .5 + .5 * (v - 64) / 63.


def fit(times):
    """A line through times (index -> time): (period, worst distance from it in s)"""
    n = len(times)
    if n < 3:
        return None, None
    mx, my = (n - 1) / 2., sum(times) / n
    sxx = sum((i - mx) ** 2 for i in range(n))
    slope = sum((i - mx) * (t - my) for i, t in enumerate(times)) / sxx
    worst = max(abs(t - (my + slope * (i - mx))) for i, t in enumerate(times))
    return slope, worst


def bpm_of(period):
    return 60. / (24. * period) if period else 0.


class Skip(Exception):
    pass


# ======== the rig: FRIZZ, its two ports, what comes back ========

class Rig:
    def __init__(self, f, trs_fd, args):
        self.f, self.args = f, args
        self.usb = midiport.Tap(f.link.fd)
        # --quick: the host check's twin, which can fall behind the wall clock under load
        f.link = midiport.TapLink(self.usb, 4. if args.quick else 1.)
        self.trs_fd = trs_fd
        self.trs = midiport.Tap(trs_fd) if trs_fd is not None else None
        self.trs_lock = threading.Lock()
        self.clocks = []
        self.wiring = set()
        self.checks = []
        self.notes = []
        self.timing_fails = 0
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


def drift_case(r, port_in, seconds, factor=1.):
    """A quantized loop to a clock, the transport a fifth up and back, then FRIZZ's own clock
    out over USB (the loop's position) against the clock sent: the loop's drift"""
    r.setting(clock_source=SOURCES[port_in], midi_out=OUTS["all"],
              clock_factor=int(factor * 100))
    c = r.clock(port_in, 120)
    r.wait_tempo(120 * factor, 1, 5)
    r.record_loop(1, 120, factor)
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
    r.tcheck("no jumps in its position", worst <= .004 * r.slack, "worst %.1f ms off the line" % (worst * 1000),
            "%g ms (USB's own jitter ~1)" % (4 * r.slack))


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
    r.check("playing, the DAW's Start: back to the loop's start", abs(pos - .25) <= .08 or
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
    r.check("Continue: plays on from there", s["looper"] == 2 and abs(s["pos"] - want) <= .08,
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
    r.check("a free loop, the DAW's Start", s["looper"] == 2 and .1 <= moved <= .45,
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
    p_out, worst = fit(ticks_after(r.captured(dst).log, t0))
    p_in, _ = fit([t for t in c.ticks if t >= t0])
    drift = (p_out * 2. / p_in - 1.) * 60000. if p_out and p_in else None
    r.tcheck("%s out at x2 against the clock sent, over %g s" % (dst.upper(), seconds),
            drift is not None and abs(drift) <= 2. * r.slack,
            "%+.2f ms a minute" % drift if drift is not None else "no clock", "+-%g ms a minute" % (2 * r.slack))
    r.tcheck("each tick on its line", worst is not None and worst <= .004 * r.slack,
            "worst %.1f ms" % (worst * 1000) if worst is not None else "-", "%g ms" % (4 * r.slack))


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
    p0, w0 = fit(ticks_after(log, t0, t1))
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
    p1, w1 = fit(ticks_after(log, t1 + .05, t2))
    ccs = sum(1 for t, m in log if t >= t1 and m[0] & 0xF0 == 0xB0) - ccs
    r.check("knobs turned fast for 4 s: feedback out of %s" % port.upper(), ccs >= 20, "%d CCs" % ccs, ">= 20")
    r.tcheck("the clock's rate meanwhile", p0 and p1 and abs(p1 / p0 - 1.) <= .001 * r.loose,
            "%.2f -> %.2f BPM" % (bpm_of(p0), bpm_of(p1)) if p0 and p1 else "-", "the same +-%g %%" % (.1 * r.loose))
    r.tcheck("its ticks' timing", w0 is not None and w1 is not None and w1 <= w0 + .001 * r.slack,
            "worst %.2f ms, before %.2f" % (w1 * 1000, w0 * 1000) if w0 is not None and w1 is not None else "-",
            "within %g ms of without" % r.slack)


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


@case("cc-channels", "8: DAW automation per effect channel: CC 14-21 on channels 1-15 move that "
      "effect's knobs, CC 22 latches it; re-sending the same latch value doesn't fight the hand (#108)")
def cc_channels(r):
    bad, n = [], 0
    for ch in (1, 7, 14, 15) if r.args.quick else range(1, 16):
        fx = WHITE_FX[ch] if ch < 15 else "comp"
        for i in range(8):
            up, a, b = moved(r, fx, i, lambda v: r.send_usb(cc(ch, 14 + i, v)))
            empty = (fx, i) in EMPTY
            n += not empty
            if (b != a) if empty else not up:
                bad.append("ch %d CC %d: %.2f -> %.2f" % (ch, 14 + i, a, b))
    r.check("CC 14-21 at 20, then 100, on channels %s: the effect's knob" % ("1, 7, 14, 15" if r.args.quick else "1-15"),
            not bad, "; ".join(bad) or "all %d moved, the empty ones didn't" % n,
            "each moves up; a knob with nothing on it doesn't")
    latched = lambda fx: r.get("latch")[remote.FX_NAMES.index(fx)]  # noqa: E731
    bad = []
    for ch in range(1, 15):
        fx = WHITE_FX[ch]
        r.send_usb(cc(ch, 22, 127))
        time.sleep(.08)
        on = latched(fx)
        r.send_usb(cc(ch, 22, 0))
        time.sleep(.08)
        if not on or latched(fx):
            bad.append("ch %d: %s" % (ch, "not on" if not on else "not off"))
    r.check("CC 22 127 / 0 on channels 1-14: the latch", not bad, "; ".join(bad) or "all 14", "on, then off")
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
    time.sleep(.1)
    r.check("CC 23 5 (%s): the focus" % port.upper(), r.state()["selected"] == remote.FX_NAMES.index("filter"),
            remote.FX_NAMES[r.state()["selected"]] if r.state()["selected"] < 14 else "comp", "filter")
    up, a, b = moved(r, "filter", 0, lambda v: send(cc(GLOBAL, 14, v)))
    r.check("then CC 14 on channel 16: the filter's knob 1", up, "%.2f -> %.2f" % (a, b), "moves up")
    bad = []
    for k in range(1, 15):
        send(cc(GLOBAL, 101 + k, 127))
        time.sleep(.06)
        on = r.get("latch")[remote.FX_NAMES.index(WHITE_FX[k])]
        send(cc(GLOBAL, 101 + k, 0))
        time.sleep(.06)
        if not on or r.get("latch")[remote.FX_NAMES.index(WHITE_FX[k])]:
            bad.append("CC %d" % (101 + k))
    r.check("CC 102-115 127 / 0: the latches", not bad, ", ".join(bad) or "all 14", "on, then off")
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


@case("shifter-page", "8: CC 18 on channel 2 moves the shifter's mix; note 36 on channel 16 turns "
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


@case("rpn", "8: a DAW that sets its pitch-bend range (RPN 0, then RPN null) on FRIZZ's channels: "
      "no knob moves (#85)")
def rpn(r):
    groups = remote.FX_NAMES + ["comp", "volume"]
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

    GROUPS = remote.FX_NAMES + ["comp", "latch", "volume"]

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
    r.setting(**BASELINE)
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
    try:
        reset(r, snap, False)
        c.run(r)
    except Skip as e:
        status, detail = "SKIP", str(e)
    except (AssertionError, SystemExit) as e:
        status, detail = "FAIL", "stopped: %s" % e
    except Exception:
        status, detail = "FAIL", "error: " + traceback.format_exc().strip().splitlines()[-1]
        if r.args.verbose:
            traceback.print_exc()
    finally:
        try:
            reset(r, snap, c.id.startswith("morph"))
        except (Exception, SystemExit) as e:
            print("   (putting things back: %s)" % e, file=sys.stderr)
    if status is None:
        if not r.checks:
            status, detail = "FAIL", "nothing checked"
        else:
            status = "PASS" if all(ok for ok, *_ in r.checks) else "FAIL"
            detail = ""
    return status, detail


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


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--slot", type=int, choices=chompi.FRIZZ_SLOTS, default=chompi.TEST_SLOT,
                    help="the FRIZZ slot (12 by default), started unless it runs")
    ap.add_argument("--device", help="FRIZZ's raw MIDI node (as remote.py's; the twin's for the host check)")
    ap.add_argument("--trs", help="the raw MIDI node on the TRS jack (FRIZZ_TRS_MIDI; found by name)")
    ap.add_argument("--wiring", choices=["auto", "out", "in", "both", "none"], default="auto")
    ap.add_argument("--cases", help="only these, by id, comma separated")
    ap.add_argument("--quick", action="store_true", help="the long cases short (the host check)")
    ap.add_argument("--long", action="store_true", help="the 'over a few minutes' cases for 3 minutes")
    ap.add_argument("--out", help="the summary into FILE too")
    ap.add_argument("--list", action="store_true", help="the cases, what they need, and leave")
    ap.add_argument("-v", "--verbose", action="store_true", help="every check as it's made")
    a = ap.parse_args()
    if a.list:
        for c in CASES:
            print("%-15s %-7s %s" % (c.id, "/".join(c.needs) or "usb", c.item))
        return
    cases = CASES
    if a.cases:
        wanted = a.cases.split(",")
        unknown = [w for w in wanted if w not in {c.id for c in CASES}]
        if unknown:
            sys.exit("no case %s (--list)" % ", ".join(unknown))
        cases = [c for c in CASES if c.id in wanted]

    device = a.device or chompi.to_frizz(a.slot)
    f = remote.Frizz(device)
    trs_node = a.trs or (None if a.wiring == "none" else midiport.trs_midi())
    trs_fd = os.open(trs_node, os.O_RDWR | os.O_NONBLOCK) if trs_node else None
    r = Rig(f, trs_fd, a)
    if not f.ram_only():
        sys.exit("FRIZZ can't keep the card out of it (PERSIST 0): a build from before it; not run")
    print("the card isn't written until FRIZZ restarts (PERSIST 0): everything stays in RAM", flush=True)
    got = f.dev(chompi.IDENT)
    info = chompi.parse_ident(got[1]) if got and got[0] == chompi.OK else None
    ident = "build %08x" % info["crc"] if info else "?"
    if info and info["uptime"] < READY:
        time.sleep(READY - info["uptime"])  # just started: the play page isn't ready yet
    snap = Snapshot(r)
    results, timing_only = [], []
    try:
        reset(r, snap, False)
        if a.wiring == "auto":
            r.wiring = detect_wiring(r)
        else:
            r.wiring = {"out": {"out"}, "in": {"in"}, "both": {"in", "out"}, "none": set()}[a.wiring]
        print("wiring: %s%s" % (wiring_text(r.wiring), " (found)" if a.wiring == "auto" else ""), flush=True)
        for c in cases:
            status, detail = run_case(r, c, snap)
            results.append((c, status, detail, r.checks, r.notes))
            print("%-4s %s%s%s" % (status, c.id, " (%.0f s)" % (time.monotonic() - r.started)
                                   if status != "SKIP" else "", ": " + detail if detail else ""), flush=True)
            for ok, what, got_, want in r.checks:
                if not ok:
                    print("       %s: %s, want %s" % (what, got_, want), flush=True)
            if status == "FAIL" and not detail and r.timing_fails == sum(1 for ok, *_ in r.checks if not ok):
                timing_only.append(c.id)
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
    finally:
        try:
            reset(r, snap, True)
            snap.restore_settings(r)
        except (Exception, SystemExit) as e:
            print("putting things back: %s" % e, file=sys.stderr)
        r.usb.close()
        if r.trs:
            r.trs.close()
    text = summary(results, r.wiring, ident, a)
    print()
    print(text)
    if timing_only:
        print("failed on timing alone: %s" % ",".join(timing_only))
    if a.out:
        with open(a.out, "w") as out:
            out.write(text)
    if not a.device and chompi.frizz_slot not in (None, chompi.TEST_SLOT):
        print("FRIZZ on slot %d keeps what the suite changed in RAM until it restarts: "
              "./flash.py --run %d" % (chompi.frizz_slot, chompi.frizz_slot), file=sys.stderr)
    sys.exit(1 if any(s == "FAIL" for _, s, *_ in results) else 0)


if __name__ == "__main__":
    main()
