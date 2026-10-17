#!/usr/bin/env python3
"""loopcap.py: the loop loader on the device, heard (#70, #268, #270): test loops whose sound says
which loop plays and on which beat, FRIZZ's master recorded over USB audio while switches go
in, and the capture read back to the sample.

    tools/loopcap.py make DIR          the three loops into DIR (h13.wav, h14.wav, h15.wav)
    tools/loopcap.py analyze CAPTURE.wav [--events EVENTS.json] [--json OUT]
                                       what plays when, the beats, the power, the clicks
    tools/loopcap.py pc OUT            #270: CC 0 = 8 and program changes 12-14 (bank h) on the
                                       global channel, CC 54 at 0, 1, 2, 8 and 127 (with nothing
                                       waiting): where each switch lands; --cubbi: CC 0 = 9 too
    tools/loopcap.py feedback OUT      #270: MIDI feedback on, switches by keys on the Loops page
                                       (at once and at the pass end), FRIZZ's MIDI out recorded,
                                       then played back into FRIZZ: the same switches, the same
                                       moments; then the scenes' program changes
    tools/loopcap.py feedback --planned OUT
                                       #401: the same with switches planned with SHIFT + a key
                                       tapped 1-3 times, and one cancelled (--secs 40)
    tools/loopcap.py fader OUT         #268: the crossfader with two loops (SHIFT + the blank
                                       scene's key, the transport turned, a white key): the two
                                       loops' power at each step, another white key during the
                                       hold, let go at a quarter, the middle and fully right

The loops (16-bit 48 kHz stereo, the right channel silent, each with an acid chunk):
    h13  a 200 Hz bed, 2 s, 4 beats at 120 BPM
    h14  a 300 Hz bed, 4 s, 8 beats at 120 BPM
    h15  a 500 Hz bed, 2.4 s, 4 beats at 100 BPM
on every beat a 10 ms burst whose pitch names the beat, 1000 + 250 x beat Hz, on the bed. In a
10 ms window each bed is a whole number of cycles, so its amplitude there is the projection on
its own sine and cosine, the others' zero: which loop plays, and how loud (the crossfader's
mix), every 10 ms; the bursts' onsets to the sample, and their beat by their pitch.

The device runs (pc, feedback, fader) are for a hold (tools/chompi.py hold) on the test slot,
USB audio on: they put nothing on the card themselves. Put the loops in first, only into
empty slots of bank h:
    tools/loopcap.py make DIR && for s in 13 14 15; do ./remote.py loops put DIR/h$s.wav h $s; done
and take them out after (./remote.py loops rm h 13 ...), the listing (loops ls) before and
after the same. Each run starts from a fresh start of the slot (./flash.py --run 12): it
assumes the play page, an empty looper and nothing latched; feedback's playback starts the slot
again itself. PERSIST 0 first, so the settings it
changes (MIDI out, the feedback) stay in RAM. The capture: hw:FrizZ, PipeWire's profile off
meanwhile; the send times and the capture lined up by a 6 kHz marker of FRIZZ's own test
signal (SIGNAL) at the start. numpy for analyze (measure.py's venv)."""
import argparse
import json
import math
import os
import struct
import subprocess
import sys
import threading
import time
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))

try:
    import numpy as np
except ImportError:  # make and the device runs' sending work without it
    np = None

SR = 48000
WIN = 480  # 10 ms
# slot: (bed Hz, seconds, beats)
LOOPS = {13: (200, 2.0, 4), 14: (300, 4.0, 8), 15: (500, 2.4, 4)}
BED, BURST = 0.25, 0.25
BURST_MS = 10
MARKER_HZ = 6000


def burst_hz(beat):
    return 1000 + 250 * beat


# ======== make ========

def loop_pcm(slot):
    bed_hz, secs, beats = LOOPS[slot]
    n = int(round(secs * SR))
    per = n // beats
    bl = int(BURST_MS * SR / 1000)
    out = bytearray()
    for i in range(n):
        x = BED * math.sin(2 * math.pi * bed_hz * i / SR)
        k = i % per
        if k < bl:
            env = 0.5 - 0.5 * math.cos(2 * math.pi * k / bl)
            x += BURST * env * math.sin(2 * math.pi * burst_hz(i // per) * k / SR)
        out += struct.pack("<hh", int(round(x * 32767)), 0)
    return bytes(out)


def wav_bytes(slot):
    _, secs, beats = LOOPS[slot]
    bpm = 60.0 * beats / secs
    data = loop_pcm(slot)

    def chunk(cid, body):
        return cid + struct.pack("<I", len(body)) + body + (b"\0" if len(body) & 1 else b"")
    fmt = chunk(b"fmt ", struct.pack("<HHIIHH", 1, 2, SR, SR * 4, 4, 16))
    acid = chunk(b"acid", struct.pack("<IHHfIHHf", 0, 60, 0, 0., beats, 4, 4, bpm))
    body = b"WAVE" + fmt + acid + chunk(b"data", data)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def cmd_make(a):
    os.makedirs(a.dir, exist_ok=True)
    for slot in LOOPS:
        path = os.path.join(a.dir, "h%d.wav" % slot)
        with open(path, "wb") as f:
            f.write(wav_bytes(slot))
        bed, secs, beats = LOOPS[slot]
        print("%s: %d Hz bed, %.1f s, %d beats (%.0f BPM)" % (path, bed, secs, beats, 60 * beats / secs))


# ======== analyze ========

def read_left(path):
    with wave.open(path) as w:
        ch, width, n = w.getnchannels(), w.getsampwidth(), w.getnframes()
        raw = w.readframes(n)
        if w.getframerate() != SR:
            sys.exit("%s: not 48 kHz" % path)
    if width == 3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3 * ch)[:, :3].astype(np.int32)
        v = b[:, 0] | b[:, 1] << 8 | b[:, 2] << 16
        v = np.where(v >= 1 << 23, v - (1 << 24), v)
        return v / float(1 << 23)
    if width == 2:
        return np.frombuffer(raw, "<i2").reshape(-1, ch)[:, 0] / 32768.0
    if width == 4:
        return np.frombuffer(raw, "<i4").reshape(-1, ch)[:, 0] / float(1 << 31)
    sys.exit("%s: %d-byte samples" % (path, width))


def bed_levels(x):
    """Per 10 ms window: each loop's bed amplitude, (n_windows, 3) in LOOPS' order; FRIZZ's own
    gain on the master (VOLUME doesn't matter, the take is before it) scales them all alike"""
    nw = len(x) // WIN
    seg = x[:nw * WIN].reshape(nw, WIN)
    t = np.arange(WIN) / SR
    cols = []
    for slot in sorted(LOOPS):
        hz = LOOPS[slot][0]
        s, c = np.sin(2 * np.pi * hz * t), np.cos(2 * np.pi * hz * t)
        # the window's phase doesn't matter: the amplitude from both projections
        a_s, a_c = seg @ s * 2 / WIN, seg @ c * 2 / WIN
        cols.append(np.hypot(a_s, a_c))
    return np.stack(cols, 1)


def bursts(x, rel=0.35):
    """The bursts' onsets (s) and pitches: found on the band 900-3100 Hz's envelope, their
    pitch by FFT (the beat: the nearest 1000 + 250 x beat), then placed to a fraction of a
    sample by correlating the band with the burst itself (its Hann window on its pitch)"""
    n = len(x)
    spec = np.fft.rfft(x)
    fr = np.fft.rfftfreq(n, 1 / SR)
    band = np.fft.irfft(spec * ((fr > 900) & (fr < 3100)), n)
    env = np.abs(band)
    k = int(0.002 * SR)
    env = np.convolve(env, np.ones(k) / k, "same")
    thr = rel * np.percentile(env, 99.9)
    out = []
    i = 0
    bl = int(BURST_MS * SR / 1000)
    win = np.hanning(bl)
    while i < n - 2 * bl:
        if env[i] > thr:
            j0 = max(bl, i - bl // 2)
            piece = band[j0:j0 + bl] * win
            sp = np.abs(np.fft.rfft(piece, 8 * bl))
            f = np.fft.rfftfreq(8 * bl, 1 / SR)[np.argmax(sp)]
            beat = int(round((f - 1000) / 250))
            hz = burst_hz(beat)
            tmpl = (0.5 - 0.5 * np.cos(2 * np.pi * np.arange(bl) / bl)) * np.exp(2j * np.pi * hz * np.arange(bl) / SR)
            seg = band[j0 - bl:j0 + 2 * bl]
            c = np.abs(np.correlate(seg, tmpl, "valid"))
            p = int(np.argmax(c))
            d = 0.0
            if 0 < p < len(c) - 1:
                den = c[p - 1] - 2 * c[p] + c[p + 1]
                d = 0.5 * (c[p - 1] - c[p + 1]) / den if den else 0.0
            onset = j0 - bl + p + d
            out.append((onset / SR, float(hz), beat))
            i = int(onset) + 2 * bl
        else:
            i += 1
    return out


def clicks(x, rel=8.0, skip=None, floor_db=-50.0, bed=None):
    """Transients: the energy above 4 kHz per ms against its median (the loops have none
    there), with the times where it stood out; not within 150 ms of SKIP (the marker, whose
    edges spread) nor the capture's first and last 20 ms"""
    n = len(x)
    spec = np.fft.rfft(x)
    fr = np.fft.rfftfreq(n, 1 / SR)
    hp = np.fft.irfft(spec * (fr > 4000) * ~((fr > MARKER_HZ - 300) & (fr < MARKER_HZ + 300)), n)
    m = SR // 1000
    e = (hp[:n // m * m].reshape(-1, m) ** 2).mean(1)
    med = np.median(e) + 1e-14
    # over the median by REL, and no quieter than FLOOR_DB under the bed's amplitude: a fader's
    # step or a gain glide leaves -60 to -70 dB there, a hard cut -20 to -30
    need = rel * rel * med
    if bed:
        need = max(need, (bed * 10 ** (floor_db / 20)) ** 2 / 2)
    hits = np.nonzero(e > need)[0]
    out = []
    for h in hits:
        if h < 20 or h > len(e) - 20 or (skip is not None and abs(h / 1000.0 - skip) < 0.15):
            continue
        if out and h - out[-1][0] < 20:
            continue
        out.append((int(h), float(10 * np.log10(e[h] * 2 / bed ** 2)) if bed else float(10 * np.log10(e[h] / med))))
    return [(h / 1000.0, db) for h, db in out]


def marker(x):
    """The 6 kHz marker's onset (s), or None: where a 10 ms window first holds half its peak
    amplitude, half a window before that"""
    n = min(len(x), 10 * SR)
    t = np.arange(WIN) / SR
    s, c = np.sin(2 * np.pi * MARKER_HZ * t), np.cos(2 * np.pi * MARKER_HZ * t)
    starts = np.arange(0, n - WIN, 24)
    amps = np.array([math.hypot(x[i:i + WIN] @ s, x[i:i + WIN] @ c) * 2 / WIN for i in starts])
    if not len(amps) or amps.max() < 0.01:
        return None
    i = int(np.nonzero(amps >= amps.max() / 2)[0][0])
    return (starts[i] + WIN / 2) / SR


def segments(levels, floor=0.02):
    """[(start s, end s, slot)] of the loudest bed, where any plays"""
    slots = sorted(LOOPS)
    out = []
    for w, row in enumerate(levels):
        k = int(np.argmax(row))
        slot = slots[k] if row[k] > floor else None
        if out and out[-1][2] == slot:
            out[-1][1] = (w + 1) * WIN / SR
        else:
            out.append([w * WIN / SR, (w + 1) * WIN / SR, slot])
    # a change counts once it held 30 ms: shorter ones are crossfades' middles
    clean = []
    for s in out:
        if clean and s[1] - s[0] < 0.03:
            clean[-1][1] = s[1]
            continue
        if clean and clean[-1][2] == s[2]:
            clean[-1][1] = s[1]
        else:
            clean.append(s)
    return [tuple(s) for s in clean]


def analyze(path, events=None):
    x = read_left(path)
    lv = bed_levels(x)
    single = float(np.percentile(lv.max(1), 50))  # one loop's bed, as FRIZZ plays it
    power = (lv ** 2).sum(1)
    on = power > (0.25 * single) ** 2
    rel_db = 10 * np.log10(power[on] / single ** 2) if on.any() else np.array([0.0])
    mk = marker(x)
    res = dict(file=path, seconds=len(x) / SR, bed=single,
               marker=mk, segments=segments(lv), bursts=bursts(x), clicks=clicks(x, skip=mk, bed=single),
               power_db=dict(min=float(rel_db.min()), max=float(rel_db.max()),
                             p1=float(np.percentile(rel_db, 1)), p99=float(np.percentile(rel_db, 99))))
    res["levels"] = lv.round(5).tolist()
    return res


def beat_grid(bs, t0=None, t1=None):
    """The bursts' intervals in [t0, t1): median, min, max (s)"""
    ts = [b[0] for b in bs if (t0 is None or b[0] >= t0) and (t1 is None or b[0] < t1)]
    d = np.diff(ts) if len(ts) > 1 else np.array([0.0])
    return float(np.median(d)), float(d.min()), float(d.max())


def show(res):
    print("%s: %.1f s, a bed at %.4f; marker at %s s" % (res["file"], res["seconds"], res["bed"], res["marker"]))
    for s, e, slot in res["segments"]:
        print("  %8.3f - %8.3f  %s" % (s, e, "h%d" % slot if slot else "-"))
    print("  bursts: %d; power of the beds against one loop's: %+.2f .. %+.2f dB (1-99 %%: %+.2f .. %+.2f)"
          % (len(res["bursts"]), res["power_db"]["min"], res["power_db"]["max"], res["power_db"]["p1"],
             res["power_db"]["p99"]))
    print("  transients above 4 kHz, within 50 dB of the bed: %d%s" % (len(res["clicks"]), "".join(
        "\n    at %.3f s, %+.1f dB against the bed" % c for c in res["clicks"][:20])))


def cmd_analyze(a):
    if np is None:
        sys.exit("analyze needs numpy (measure.py's venv)")
    res = analyze(a.capture)
    show(res)
    if a.events:
        ev = json.load(open(a.events))
        judge(ev, res)
    if a.json:
        json.dump(res, open(a.json, "w"))


# ======== judging a run ========

def land(res, t, slot, within=(-0.03, 20.0)):
    """The first segment of SLOT starting in [t + within)"""
    for s, e, sl in res["segments"]:
        if sl == slot and t + within[0] <= s < t + within[1]:
            return s
    return None


def pass_ends(res, slot, t, count=8):
    """SLOT's next COUNT pass ends after T, from its last burst heard at or before T while it
    plays: beat b there means its pass began b beats before (an at-once switch starts a loop
    by time, anywhere in its pass), then a pass's length on and on"""
    heard = [b for b in res["bursts"] if b[0] <= t + 0.005
             and any(s - 0.005 <= b[0] < e and sl == slot for s, e, sl in res["segments"])]
    if not heard:
        return []
    _, secs, beats = LOOPS[slot]
    start = heard[-1][0] - heard[-1][2] * secs / beats
    ends = [start + secs * k for k in range(1, count + 3)]
    return [e for e in ends if e > t][:count]


def beat0_near(res, t, within=0.03):
    """The onset of a beat 0 burst within WITHIN of T: where a planned switch landed, to the
    sample (the new loop starts on its beat 0), or None"""
    near = [b[0] for b in res["bursts"] if b[2] == 0 and abs(b[0] - t) <= within]
    return min(near, key=lambda x: abs(x - t)) if near else None


def judge(ev, res):
    off = (res["marker"] - ev["marker_sent"]) if res["marker"] is not None and "marker_sent" in ev else 0.0
    print("send times to the capture's: %+.1f ms (the marker)" % (off * 1000))
    kind = ev.get("kind")
    out = []
    if kind == "pc":
        prev = ev["first"]
        for e in ev["events"]:
            t = e["sent"] + off
            got = land(res, t, e["slot"])
            if got is not None and e["mode"] not in (0, 127):
                got = beat0_near(res, got) or got  # the segment's 10 ms grid, refined
            ends = pass_ends(res, prev, t)
            want = None
            if e["mode"] in (0, 127):
                want = t
            elif len(ends) >= e["mode"]:
                want = ends[e["mode"] - 1]
            ok = got is not None and want is not None and (
                abs(got - want) < (0.12 if e["mode"] in (0, 127) else 0.003))
            line = ("CC 54 = %3d, PC to h%d at %.3f s: heard at %s, %s" % (
                e["mode"], e["slot"], t, "%.4f s" % got if got else "never",
                "at once" if e["mode"] in (0, 127) else "pass end %d at %s" % (
                    e["mode"], "%.4f s" % want if want else "?")))
            if got and want:
                line += " (%+.1f ms)" % ((got - want) * 1000)
            out.append((ok, line))
            prev = e["slot"] if got else prev
        for e in ev.get("cubbi", []):
            t = e["sent"] + off
            lv = np.array(res["levels"])
            w = int(t * SR / WIN)
            gone = next((k for k in range(w, len(lv)) if lv[k].max() < 0.2 * res["bed"]), None)
            out.append((gone is not None and (gone * WIN / SR - t) < 0.3,  # a cubbi loads first
                        "CC 0 = 9, PC %d (cubbi_a%d) at %.3f s: our loops gone at %s" % (
                            e["cubbi"] - 1, e["cubbi"], t, "%.3f s" % (gone * WIN / SR) if gone is not None else "never")))
        last = ev["events"][-1]["sent"] + off + 1.0 if ev["events"] else None
        res["clicks"] = [c for c in res["clicks"] if last is None or c[0] < last]
        med, lo, hi = beat_grid(res["bursts"], None, last)
        out.append((True, "beats: %d, intervals median %.4f s, %.4f .. %.4f" % (len(res["bursts"]), med, lo, hi)))
    elif kind == "feedback":
        for line in ev.get("notes", []):
            out.append((True, line))
    elif kind == "fader":
        lv = np.array(res["levels"])
        single = res["bed"]
        slots = sorted(LOOPS)
        for st in ev["steps"]:
            t0 = st["t0"] + off + 0.15
            t1 = st["t1"] + off - 0.05
            w0, w1 = int(t0 * SR / WIN), int(t1 * SR / WIN)
            if w1 <= w0:
                continue
            m = lv[w0:w1].mean(0)
            p = 10 * math.log10(((lv[w0:w1] ** 2).sum(1)).mean() / single ** 2)
            out.append((abs(p) < 0.5, "%-28s %s; power %+.2f dB" % (
                st["what"], "  ".join("h%d %.4f" % (s, m[i]) for i, s in enumerate(slots)), p)))
    for ok, line in out:
        print(("ok    " if ok else "FAIL  ") + line)
    nclick = len(res["clicks"])
    print("transients above 4 kHz, within 50 dB of the bed: %d" % nclick)
    return all(ok for ok, _ in out) and nclick == 0


# ======== on the device ========

class Run:
    """FRIZZ (remote.Frizz), the capture and the send times of one run"""

    def __init__(self, out, fresh=False):
        import chompi
        import remote
        self.remote, self.chompi = remote, chompi
        self.out = out
        # fresh: the slot started again first, as from power-on (remote.py play --fresh)
        self.f = remote.Frizz(chompi.to_frizz(again=True) if fresh else chompi.find_device())
        if not self.f.ram_only():
            sys.exit("PERSIST 0 refused: nothing sent")
        st = dict(zip(remote.SETTING_NAMES, remote.dev_get(self.f, remote.GROUP_SETTINGS)))
        if not st.get("usb_audio", 0):
            sys.exit("USB audio is off: no capture")
        self.settings = st
        self.ch = (st["midi_channel"] or 16) - 1
        # the marker, FRIZZ's test signal in AUX's place, on the master: AUX live (in RAM)
        if not st.get("aux_live"):
            why = remote.try_set(self.f, "settings", "aux_live", "1")
            if why:
                sys.exit("aux_live: " + why)
        self.card = self._card()
        self.profile = self._profile() if self.card else None
        self.rec = None
        self.t0 = None

    def _pactl(self, *args):
        return subprocess.run(["pactl"] + list(args), capture_output=True, text=True,
                              env=dict(os.environ, LANG="C")).stdout

    def _card(self):
        for line in self._pactl("list", "cards", "short").splitlines():
            if "FrizZ" in line:
                return line.split()[1]
        return None

    def _profile(self):
        for block in self._pactl("list", "cards").split("\nCard #"):
            if "Name: " + self.card in block:
                for line in block.splitlines():
                    if line.strip().startswith("Active Profile:"):
                        return line.split(":", 1)[1].strip()
        return None

    def now(self):
        return time.monotonic() - self.t0

    def start(self, secs):
        r = self.remote
        self.f.send(r.SWITCH, [1])  # the play page
        self.f.dev(self.chompi.SIGNAL, list(r.signal_args("sine", 440, 0)))  # the input silent
        if self.card:
            self._pactl("set-card-profile", self.card, "off")
            time.sleep(0.5)
        self.rec = subprocess.Popen(["arecord", "-q", "-D", "hw:FrizZ", "-f", "S24_3LE", "-c2", "-r48000",
                                     "-d", str(int(secs)), self.out + ".wav"])
        self.t0 = time.monotonic()
        time.sleep(0.5)
        self.marker_sent = self.now()
        self.f.dev(self.chompi.SIGNAL, list(r.signal_args("sine", MARKER_HZ, 0.1)))
        time.sleep(0.05)
        self.f.dev(self.chompi.SIGNAL, list(r.signal_args("sine", 440, 0)))

    def wait(self):
        if self.rec:
            self.rec.wait()

    def end(self):
        r = self.remote
        try:
            self.f.release()
            self.f.raw([0xB0 | self.ch, 0, 0])
            self.f.dev(self.chompi.SIGNAL, [0])
            self.f.send(r.SWITCH, [0])
        except OSError:
            pass
        if self.rec and self.rec.poll() is None:
            self.rec.terminate()
        if self.card:
            self._pactl("set-card-profile", self.card, self.profile or "output:analog-stereo+input:analog-stereo")

    def tap(self, key, hold=0.05):
        self.f.key(key, True)
        time.sleep(hold)
        self.f.key(key, False)

    def loops_page(self, bank_steps=7):
        """SHIFT + the lower C#: the Loops page, then the lower D# BANK_STEPS times (a to h)"""
        self.f.key("KEY_26", True)
        time.sleep(0.06)
        self.tap("KEY_16", 0.06)
        time.sleep(0.06)
        self.f.key("KEY_26", False)
        time.sleep(0.3)
        for _ in range(bank_steps):
            self.tap("KEY_17", 0.05)
            time.sleep(0.15)
        time.sleep(0.3)

    def save(self, kind, events, **more):
        d = dict(kind=kind, marker_sent=self.marker_sent, events=events, **more)
        with open(self.out + ".json", "w") as f:
            json.dump(d, f, indent=1)
        return d


def at(run, t):
    """Sleeps until the run's time T"""
    while True:
        left = t - run.now()
        if left <= 0:
            return
        time.sleep(left - 0.002 if left > 0.003 else 0)


def cmd_pc(a):
    run = Run(a.out)
    ch = run.ch
    plan = [(0, 14), (1, 15), (2, 13), (8, 14), (0, 15), (127, 13)]
    events = []
    try:
        run.start(a.secs)
        run.f.raw([0xB0 | ch, 0, 8, 0xB0 | ch, 54, 0, 0xC0 | ch, 12])  # h13 at once
        first = 13
        t = 2.0
        for mode, slot in plan:
            t += 0.7
            at(run, t)
            run.f.raw([0xB0 | ch, 54, mode])
            time.sleep(0.01)
            e = dict(mode=mode, slot=slot, sent=run.now())
            run.f.raw([0xC0 | ch, slot - 1])
            events.append(e)
            # until it has landed: the n-th pass end of the longest loop (4 s)
            t += 0.6 if mode in (0, 127) else 4.2 * max(1, mode) + 0.5
            print("CC 54 = %d, PC h%d at %.3f s" % (mode, slot, e["sent"]), flush=True)
        if a.cubbi:
            t += 0.5
            at(run, t)
            run.f.raw([0xB0 | ch, 54, 0, 0xB0 | ch, 0, 9, 0xC0 | ch, 0])
            events.append(dict(mode=0, slot=None, cubbi=1, sent=run.now()))
            t += 3
            at(run, t)
            run.f.raw([0xC0 | ch, 1])
            events.append(dict(mode=0, slot=None, cubbi=2, sent=run.now()))
        run.wait()
    finally:
        run.end()
    ev = run.save("pc", [e for e in events if e.get("slot")], first=first, cubbi=[e for e in events if not e.get("slot")])
    if np is not None:
        res = analyze(run.out + ".wav")
        show(res)
        ok = judge(ev, res)
        print("PASS" if ok else "FAIL")


# feedback --planned (#401): switches planned with SHIFT + a key tapped 1-3 times (that many
# pass ends of the loop playing: h13 2 s, h14 4 s, h15 2.4 s), one cancelled in a new SHIFT hold
PLANNED = (("KEY_13", 0, False, 2.3), ("KEY_14", 1, False, 4.5), ("KEY_15", 2, False, 9.5),
           ("KEY_13", 1, True, 4.0), ("KEY_14", 3, False, 9.0))


def cmd_feedback(a):
    run = Run(a.out)
    r = run.remote
    for name, v in (("midi_out", 2), ("midi_feedback", 1)):
        why = r.try_set(run.f, "settings", name, str(v))
        if why:
            sys.exit("%s: %s" % (name, why))
    got = []
    stop = threading.Event()
    fd = run.f.link.fd

    def reader():
        import select
        while not stop.is_set():
            rd, _, _ = select.select([fd], [], [], 0.05)
            if rd:
                try:
                    data = os.read(fd, 4096)
                except BlockingIOError:
                    continue
                got.append((run.now(), list(data)))
    hand = []
    try:
        run.start(a.secs)
        run.loops_page()
        th = threading.Thread(target=reader, daemon=True)
        th.start()
        t = run.now() + 0.5
        # (key, SHIFT + taps: the pass ends it waits for, 0 at once; cancelled after; the gap)
        plan = PLANNED if getattr(a, "planned", False) else (
            ("KEY_13", 0, False, 2.3), ("KEY_14", 0, False, 1.7), ("KEY_15", 1, False, 5.5),
            ("KEY_13", 0, False, 2.9), ("KEY_14", 0, False, 2.1))
        for key, taps, cancel, gap in plan:
            at(run, t)
            if taps:
                run.f.key("KEY_26", True)
                for i in range(taps):
                    time.sleep(0.05 if i == 0 else 0.12)
                    run.tap(key)
                time.sleep(0.05)
                run.f.key("KEY_26", False)
            else:
                run.tap(key)
            hand.append(dict(key=key, planned=taps, sent=run.now()))
            if cancel:  # in a new SHIFT hold, SHIFT + the blinking key (#401: CC 81 goes out)
                time.sleep(0.6)
                run.f.key("KEY_26", True)
                time.sleep(0.05)
                run.tap(key)
                time.sleep(0.05)
                run.f.key("KEY_26", False)
                hand.append(dict(key=key, cancel=True, sent=run.now()))
            t += gap
        at(run, t + 1)
        stop.set()
        th.join()
        # the take: FRIZZ's channel messages (no SysEx, no clock), at their times
        msgs = []
        for tt, data in got:
            for m in r.midi_messages(data):
                if m[0] < 0xF0:
                    msgs.append((tt, list(m)))
        print("FRIZZ sent: " + "; ".join("%.3f %s" % (tt, " ".join("%02X" % b for b in m)) for tt, m in msgs))
        run.wait()
    finally:
        run.end()
    take1 = run.out + ".wav"
    run.f.link.close()  # ALSA's raw MIDI takes one opener: the second run's
    # again: the take played back into FRIZZ, from its first message on, the Loops page shown,
    # as long after the capture's start as by hand; from a fresh start, as the first run: the
    # loop the hand left would play on, the take's first switch would keep its place in the
    # pass (so a later one at the pass end lands elsewhere), and the mix, wet, would hide the
    # marker
    run2 = Run(a.out + "-back", fresh=True)
    try:
        run2.start(a.secs)
        run2.loops_page()
        base = msgs[0][0] if msgs else 0
        start = max(run2.now() + 0.2, base)
        for tt, m in msgs:
            at(run2, start + tt - base)
            run2.f.raw(m)
        # the scenes' program changes after: CC 0 = 0, PC 1 and 0, read back by STATE
        at(run2, start + (msgs[-1][0] - base if msgs else 0) + 2)
        run2.f.raw([0xB0 | run2.ch, 0, 0])
        states = []
        for p in (1, 0):
            run2.f.raw([0xC0 | run2.ch, p])
            time.sleep(1.5)
            states.append((p, list(run2.f.state() or [])))
        run2.wait()
    finally:
        run2.end()
    notes = []
    if np is not None:
        r1, r2 = analyze(take1), analyze(run2.out + ".wav")
        show(r1)
        show(r2)
        c1 = [(s, sl) for s, e, sl in r1["segments"] if sl]
        c2 = [(s, sl) for s, e, sl in r2["segments"] if sl]
        o1 = [sl for _, sl in c1]
        o2 = [sl for _, sl in c2]
        notes.append("the loops in order: by hand %s, played back %s: %s" % (
            o1, o2, "the same" if o1 == o2 else "DIFFERENT"))
        if o1 == o2 and len(c1) > 1:
            d1 = [b[0] - c1[0][0] for b in c1]
            d2 = [b[0] - c2[0][0] for b in c2]
            dev = max(abs(x - y) for x, y in zip(d1, d2))
            notes.append("their moments from the first: %s against %s, at most %.1f ms apart" % (
                ["%.3f" % x for x in d1], ["%.3f" % y for y in d2], dev * 1000))
    notes.append("scene PCs after: %s" % states)
    for n in notes:
        print(n)
    run.save("feedback", hand, take=[(tt, m) for tt, m in msgs], notes=notes)


def cmd_fader(a):
    run = Run(a.out)
    steps = []
    try:
        run.start(a.secs)
        run.loops_page()
        run.tap("KEY_13")  # h13 at once
        time.sleep(2.0)

        def mark(what, secs):
            t0 = run.now()
            time.sleep(secs)
            steps.append(dict(what=what, t0=t0, t1=run.now()))

        def turn(d):
            while d:
                s = max(-64, min(63, d))
                run.f.send(run.remote.TURN, [5, s & 0x7F])
                d -= s
            time.sleep(0.1)
        # 1: h14 joins on the right, 0 -> 24 -> 0 in 4-detent steps; at 16, h15 for h14
        mark("h13 alone", 1.0)
        run.f.key("KEY_26", True)
        time.sleep(0.06)
        run.tap("KEY_18", 0.06)  # the blank scene's key: the crossfader
        time.sleep(0.2)
        run.tap("KEY_14")
        time.sleep(0.3)
        pos = 0
        for d in [4] * 6 + [-4] * 2:
            turn(d)
            pos += d
            mark("fader %d of 24, h13 | h14" % pos, 0.8)
        run.tap("KEY_15")  # another white key: h15 for h14 on the right
        time.sleep(0.1)
        mark("fader 16, h13 | h15 (replaced)", 1.0)
        turn(-12)
        mark("fader 4, h13 | h15", 0.8)
        run.f.key("KEY_26", False)  # let go at a quarter: h13 stays
        time.sleep(0.2)
        mark("let go at 4: the old stays", 1.5)
        # 2: let go in the middle: the new one takes over
        run.f.key("KEY_26", True)
        time.sleep(0.06)
        run.tap("KEY_18", 0.06)
        time.sleep(0.2)
        turn(12)
        run.tap("KEY_14")
        time.sleep(0.3)
        mark("fader 12, h13 | h14", 1.0)
        run.f.key("KEY_26", False)
        time.sleep(0.2)
        mark("let go at 12: the new takes over", 1.5)
        # 3: fully right with h13
        run.f.key("KEY_26", True)
        time.sleep(0.06)
        run.tap("KEY_18", 0.06)
        time.sleep(0.2)
        turn(24)
        run.tap("KEY_13")
        time.sleep(0.3)
        mark("fader 24, h14 | h13", 1.0)
        run.f.key("KEY_26", False)
        time.sleep(0.2)
        mark("let go at 24: the new", 1.5)
        run.wait()
    finally:
        run.end()
    ev = run.save("fader", [], steps=steps)
    if np is not None:
        res = analyze(run.out + ".wav")
        show(res)
        ok = judge(ev, res)
        print("PASS" if ok else "FAIL")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("make")
    p.add_argument("dir")
    p = sub.add_parser("analyze")
    p.add_argument("capture")
    p.add_argument("--events")
    p.add_argument("--json")
    for name, secs in (("pc", 70), ("feedback", 22), ("fader", 30)):
        p = sub.add_parser(name)
        p.add_argument("out", help="the capture's prefix: OUT.wav, OUT.json")
        p.add_argument("--secs", type=int, default=secs, help="the capture's length")
        if name == "feedback":
            p.add_argument("--planned", action="store_true",
                           help="#401: every switch planned with SHIFT (1-3 taps), one cancelled (--secs 40)")
        if name == "pc":
            p.add_argument("--cubbi", action="store_true", help="then CC 0 = 9, the cubbis' bank: PC 0, 1")
    a = ap.parse_args()
    {"make": cmd_make, "analyze": cmd_analyze, "pc": cmd_pc, "feedback": cmd_feedback, "fader": cmd_fader}[a.cmd](a)


if __name__ == "__main__":
    main()
