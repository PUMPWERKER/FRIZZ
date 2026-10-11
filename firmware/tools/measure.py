#!/usr/bin/env python3
"""measure.py: measurements on the real CHOMPI, with no hands at the panel.

    ./measure.py clock [--mode single|pairs|catchup|late] [--bpm 120] [--seconds 20]
        a MIDI clock over USB, paced by this computer, and FRIZZ's tempo read every 50 ms:
        how steady it holds. pairs: two ticks in one write every 2 ticks' time (a host that
        batches); catchup: every other tick held back and sent 1 ms before the next; late:
        one tick in 48 (every other beat) 10 ms late, the rest on time
    ./measure.py drift [--bpm 120] [--bars 1] [--seconds 180] [--mode M] [--out NAME]
        a quantized loop against a MIDI clock on the TRS jack (its ticks as `clock --mode`
        sends them): generated material into AUX,
        PLAY + LOOP over SysEx, the line out recorded, then the loop's length against the
        clock's bars and its drift in ms a minute
    ./measure.py fx KEY [--level CC] [--seconds 30] [--material FILE] --out NAME
        an effect latched (KEY: its key, KEY_1 .. KEY_13), its level knob set over CC (the
        sends start at 0: 113 the delay, 117 the reverb), material into AUX, the line out
        recorded to NAME.f32
    ./measure.py compare A.f32 B.f32
        two recordings' levels and octave bands, B against A, in dB
    ./measure.py material [--music FILE ...] --out FILE.wav
        40 s of generated, never repeating notes and noise (for drift: every pass of a loop
        is told apart), or a mix of the WAV files given, looped (for listening tests)

The audio: AUX gets an output of the audio interface, the CHOMPI's line out goes into one of
its inputs, the TRS jack gets its MIDI out. Found by name, or set them:
    FRIZZ_AUX_SINK      a part of the PipeWire sink's name    (default: Line3)
    FRIZZ_LINE_SOURCE   a part of the PipeWire source's name  (default: Mic1, input 1 of the
                        interface AUX's sink is on)
    FRIZZ_TRS_MIDI      the raw MIDI node for the jack        (default: the first sound
                        card's with MIDI that isn't the CHOMPI, e.g. /dev/snd/midiC1D0)

Before measuring: is anyone else testing on the CHOMPI? (CLAUDE.md). It measures the FRIZZ that
runs, saying which slot, or the one --slot names, and starts FRIZZ first if the CHOMPI is
elsewhere, as remote.py does (chompi.py). Linux (ALSA raw MIDI, PipeWire's pw-play and
pw-record), Python 3 with numpy: `python3 -m venv ~/.venvs/frizz && ~/.venvs/frizz/bin/pip
install numpy`, then run this with that python.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import chompi  # noqa: E402
import remote  # noqa: E402

try:
    import numpy as np
except ImportError:
    sys.exit("measure.py needs numpy: python3 -m venv ~/.venvs/frizz && "
             "~/.venvs/frizz/bin/pip install numpy")

SR = 48000
CC = 0xBF  # channel 16, FRIZZ's at first


# ======== the devices ========

def pw_node(kind, part):
    """The PipeWire sink or source whose name has part in it"""
    out = subprocess.run(["pactl", "list", "short", kind], capture_output=True, text=True).stdout
    names = [l.split("\t")[1] for l in out.splitlines() if part in l and ".monitor" not in l]
    if not names:
        sys.exit("no PipeWire %s with %r in its name (FRIZZ_%s)" % (
            kind[:-1], part, "AUX_SINK" if kind == "sinks" else "LINE_SOURCE"))
    return names[0]


def aux_sink():
    return pw_node("sinks", os.environ.get("FRIZZ_AUX_SINK", "Line3"))


def line_source():
    """On the same interface as the AUX sink, unless set: a laptop has a Mic1 too"""
    part = os.environ.get("FRIZZ_LINE_SOURCE", "Mic1")
    if "FRIZZ_LINE_SOURCE" not in os.environ:
        device = aux_sink().replace("alsa_output.", "alsa_input.").split(".HiFi")[0]
        return pw_node("sources", device + ".HiFi__" + part)
    return pw_node("sources", part)


def trs_midi():
    """The first sound card's raw MIDI node that isn't the CHOMPI's: FRIZZ's card isn't called
    CHOMPI (its id is FrizZ, or Default), so the CHOMPI's is skipped as the tools find it"""
    if os.environ.get("FRIZZ_TRS_MIDI"):
        return os.environ["FRIZZ_TRS_MIDI"]
    own = re.match(r".*/midiC(\d+)D", chompi.find_device() or "")
    cards = open("/proc/asound/cards").read()
    for m in re.finditer(r"^\s*(\d+) \[([^\]]*)\]", cards, re.M):
        node = "/dev/snd/midiC%sD0" % m.group(1)
        if own and m.group(1) == own.group(1) or "CHOMPI" in m.group(2):
            continue
        if os.path.exists(node):
            return node
    sys.exit("no MIDI out for the TRS jack (FRIZZ_TRS_MIDI)")


def frizz(a):
    return remote.Frizz(chompi.to_frizz(a.slot))


def latch(f, name):
    """An FX key held, SHIFT tapped: latched (MANUAL.md)"""
    f.key(name, True)
    time.sleep(.08)
    f.key("KEY_26", True)
    time.sleep(.05)
    f.key("KEY_26", False)
    time.sleep(.05)
    f.key(name, False)


# ======== a clock paced by this computer ========

def wait_until(t):
    """Sleeps most of the way, then spins: ticks to about 0.1 ms"""
    while True:
        d = t - time.perf_counter()
        if d <= 0:
            return
        if d > .002:
            time.sleep(d - .0015)


class Clock:
    """24 ticks a beat at bpm from a thread, through write(bytes); mode as `clock` says"""

    def __init__(self, write, bpm, mode="single"):
        self.write, self.period, self.mode = write, 60. / (bpm * 24.), mode
        self.stop = False
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        t0 = time.perf_counter() + .05
        n = 0
        while not self.stop:
            if self.mode == "pairs":
                wait_until(t0 + (n + 1) * self.period)
                self.write(b"\xF8\xF8")
                n += 2
            elif self.mode == "late":
                wait_until(t0 + n * self.period + (.010 if n % 48 == 47 else 0.))
                self.write(b"\xF8")
                n += 1
            elif self.mode == "catchup":
                wait_until(t0 + (n + 1) * self.period - .001)
                self.write(b"\xF8")
                wait_until(t0 + (n + 1) * self.period)
                self.write(b"\xF8")
                n += 2
            else:
                wait_until(t0 + n * self.period)
                self.write(b"\xF8")
                n += 1

    def close(self):
        self.stop = True
        self.thread.join()


# ======== audio ========

def write_wav(path, stereo):
    data = np.asarray(stereo, dtype="<f4").tobytes()
    with open(path, "wb") as w:
        w.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt "
                + struct.pack("<IHHIIHH", 16, 3, 2, SR, SR * 8, 8, 32)
                + b"data" + struct.pack("<I", len(data)) + data)


def read_wav(path):
    """Any PCM 16/24/32-bit or float WAV, as float stereo at 48 kHz (linear resampling, fine
    for test material)"""
    d = open(path, "rb").read()
    if d[:4] != b"RIFF" or d[8:12] != b"WAVE":
        sys.exit("%s: not a WAV" % path)
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(d):
        cid, size = d[pos:pos + 4], struct.unpack("<I", d[pos + 4:pos + 8])[0]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", d[pos + 8:pos + 24])
        elif cid == b"data":
            data = d[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    tag, ch, rate, _, _, bits = fmt
    if tag == 3:  # float (an extensible header with 32 bits is read as integers)
        x = np.frombuffer(data, "<f4").astype(np.float64)
    elif bits == 16:
        x = np.frombuffer(data, "<i2") / 32768.
    elif bits == 24:
        b = np.frombuffer(data[:len(data) // 3 * 3], np.uint8).reshape(-1, 3)
        x = ((b[:, 0].astype(np.int32) | b[:, 1].astype(np.int32) << 8 | b[:, 2].astype(np.int32) << 16)
             << 8 >> 8) / 8388608.
    elif bits == 32:
        x = np.frombuffer(data, "<i4") / 2147483648.
    else:
        sys.exit("%s: %d-bit WAVs aren't read" % (path, bits))
    x = x[:len(x) // ch * ch].reshape(-1, ch)
    x = np.repeat(x, 2, axis=1) if ch == 1 else x[:, :2]
    if rate != SR:
        t = np.arange(int(len(x) * SR / rate)) * rate / SR
        x = np.stack([np.interp(t, np.arange(len(x)), x[:, c]) for c in range(2)], 1)
    return x


def generated(seconds=60, seed=7):
    """Never repeating notes and noise bursts, mono, peaks about -6 dBFS"""
    rng = np.random.default_rng(seed)
    x = np.zeros(seconds * SR)
    t = 0
    while t < len(x) - SR:
        n = int(rng.integers(2400, 12000))
        f = 110 * 2 ** (rng.integers(0, 36) / 12)
        tone = np.sin(2 * np.pi * f * np.arange(n) / SR)
        if rng.random() < .3:
            tone += .3 * rng.standard_normal(n)
        x[t:t + n] += .25 * np.exp(-np.arange(n) / (n / 4)) * tone
        t += int(rng.integers(1200, 9600))
    return np.clip(x, -.9, .9)


def play(path):
    return subprocess.Popen(["pw-play", "--target", aux_sink(), path])


def record(seconds, path=None, times=None):
    """The line out, mono float, for seconds: into path (raw float32); with times, a list of
    (samples so far, this computer's time) per packet, for matching the two clocks"""
    rec = subprocess.Popen(["pw-record", "--target", line_source(), "--rate", str(SR),
                            "--channels", "1", "--format", "f32", "--raw", "-"],
                           stdout=subprocess.PIPE)
    chunks, samples = [], 0
    end = time.perf_counter() + seconds
    while time.perf_counter() < end:
        b = rec.stdout.read1(65536)
        now = time.perf_counter()
        if not b:
            break
        chunks.append(b)
        samples += len(b) // 4
        if times is not None:
            times.append((samples, now))
    rec.terminate()
    y = np.frombuffer(b"".join(chunks), "<f4").astype(np.float64)
    if path:
        y.astype("<f4").tofile(path)
    return y


# ======== the commands ========

def cmd_clock(a):
    f = frizz(a)
    clock = Clock(lambda b: f.raw(b), a.bpm, a.mode)
    time.sleep(3)  # locked and settled
    vals = []
    end = time.time() + a.seconds
    while time.time() < end:
        vals.append(remote.tempo(f.state()))
        time.sleep(.05)
    clock.close()
    changes = sum(1 for x, y in zip(vals, vals[1:]) if x != y)
    print("%-8s %5.1f BPM over USB: FRIZZ's tempo %.1f..%.1f, mostly %.1f, %d changes in %d readings over %g s"
          % (a.mode, a.bpm, min(vals), max(vals), max(set(vals), key=vals.count), changes, len(vals), a.seconds))


def loop_length(y, times, bpm, bars):
    """The loop's length in the recording, against the clock's bars in this computer's time:
    (passes, loop, bar, drift in ms a minute, the passes' jumps)"""
    t = np.array(times, dtype=np.float64)
    t = t[t[:, 1] > t[0, 1] + 3.]  # past the start, whose packets come in a burst
    # samples per second of this computer: a line through the packets' arrivals, then again
    # through the half that came soonest after it (the least delayed)
    a, b = np.polyfit(t[:, 1], t[:, 0], 1)
    for _ in range(3):
        keep = t[:, 0] - (a * t[:, 1] + b) >= np.percentile(t[:, 0] - (a * t[:, 1] + b), 50)
        a, b = np.polyfit(t[keep, 1], t[keep, 0], 1)
    bar = bars * 4 * 60. / bpm * a
    w = int(min(bar, SR) * .8)
    s0 = int(bar * .5)  # a window out of the first whole pass
    tmpl = y[s0:s0 + w]
    ks, offs, cs = [], [], []
    k = 1
    while s0 + k * bar + w + 400 < len(y):
        c0 = int(round(s0 + k * bar))
        seg = y[c0 - 300:c0 + 300 + w]
        corr = np.correlate(seg, tmpl, "valid")
        i = int(np.argmax(corr))
        p = 0.
        if 0 < i < len(corr) - 1:  # to a fraction of a sample, by a parabola
            p = .5 * (corr[i - 1] - corr[i + 1]) / (corr[i - 1] - 2 * corr[i] + corr[i + 1])
        cs.append(corr[i] / (np.sqrt(np.sum(tmpl ** 2) * np.sum(seg[i:i + w] ** 2)) + 1e-12))
        ks.append(k)
        offs.append(c0 - 300 + i + p - (s0 + k * bar))
        k += 1
    ks, offs, cs = map(np.array, (ks, offs, cs))
    good = cs > .9
    if good.sum() < 3:
        sys.exit("the passes don't match (best %.2f): is the line out on %s, is the loop playing?"
                 % (cs.max() if len(cs) else 0, line_source()))
    slope = np.polyfit(ks[good], offs[good], 1)[0]
    jumps = [(ks[good][i + 1], j - slope) for i, j in enumerate(np.diff(offs[good])) if abs(j - slope) > 8]
    return len(ks), good.sum(), bar + slope, bar, slope / bar * 60000., jumps, (a / SR - 1) * 1e6


def cmd_drift(a):
    f = frizz(a)
    if f.state()[0] != 0:
        sys.exit("the looper isn't empty: erase it, or restart FRIZZ (flash.py --run 10)")
    midi = os.open(trs_midi(), os.O_WRONLY)
    clock = Clock(lambda b: os.write(midi, b), a.bpm, a.mode)
    wav = (a.out or "drift") + ".material.wav"
    write_wav(wav, np.repeat(generated()[:, None], 2, 1))
    p = play(wav)
    time.sleep(4)
    # PLAY held, LOOP: a quantized recording; LOOP again half a bar before its last bar ends
    f.key("KEY_27", True); time.sleep(.08); f.key("KEY_28", True); time.sleep(.06)
    f.key("KEY_28", False); time.sleep(.02); f.key("KEY_27", False)
    start, bar = time.time(), 4 * 60. / a.bpm
    while f.state()[0] != 1 and time.time() - start < 2:
        time.sleep(.01)
    time.sleep(max(0., (a.bars - .5) * bar - (time.time() - start)))
    f.key("KEY_28", True); time.sleep(.05); f.key("KEY_28", False)
    t1 = time.time()
    while f.state()[0] != 2 and time.time() - t1 < bar + 2:
        time.sleep(.02)
    if f.state()[0] != 2:
        sys.exit("the loop didn't close: is the clock reaching the TRS jack?")
    time.sleep(.3)
    p.terminate()
    times = []
    y = record(a.seconds, a.out and a.out + ".f32", times)
    clock.close()
    n, good, loop, bar_s, drift, jumps, ppm = loop_length(y, times, a.bpm, a.bars)
    print("%g BPM, %d bar%s, TRS (%s): %d passes (%d clear), the loop %.1f samples for the bars' %.1f "
          "(%+.2f a pass): drift %+.2f ms a minute; the interface %+.0f ppm against this computer"
          % (a.bpm, a.bars, "s" if a.bars > 1 else "", a.mode, n, good, loop, bar_s, loop - bar_s, drift, ppm))
    if jumps:
        print("the loop's position jumped: " + ", ".join("%+.0f samples at pass %d" % (j, k) for k, j in jumps))


def cmd_fx(a):
    f = frizz(a)
    material = a.material
    if not material:
        material = a.out + ".material.wav"
        write_wav(material, np.repeat(generated()[:, None], 2, 1))
    p = play(material)
    time.sleep(1)
    latch(f, a.key)
    if a.level:
        f.raw([CC, a.level, 100])
    time.sleep(2)
    y = record(a.seconds, a.out + ".f32")
    p.terminate()
    print("%s: %.1f s, RMS %.4f" % (a.out + ".f32", len(y) / SR, np.sqrt(np.mean(y ** 2))))


def bands(y):
    """Octave bands from 40 Hz, in dB, by averaged FFTs"""
    n = 8192
    win = np.hanning(n)
    segs = [y[i:i + n] * win for i in range(0, len(y) - n, n // 2)]
    p = np.mean([np.abs(np.fft.rfft(s)) ** 2 for s in segs], axis=0)
    fr = np.fft.rfftfreq(n, 1. / SR)
    edges = [40, 80, 160, 315, 630, 1250, 2500, 5000, 10000, 20000]
    return edges, [10 * np.log10(p[(fr >= lo) & (fr < hi)].sum() + 1e-30) for lo, hi in zip(edges, edges[1:])]


def cmd_compare(a):
    ya, yb = (np.fromfile(p, "<f4").astype(np.float64) for p in (a.a, a.b))
    ra, rb = np.sqrt(np.mean(ya ** 2)), np.sqrt(np.mean(yb ** 2))
    edges, ba = bands(ya)
    _, bb = bands(yb)
    print("level: %.4f -> %.4f (%+.2f dB)" % (ra, rb, 20 * np.log10(rb / ra)))
    print("bands: " + "  ".join("%g: %+.1f" % (lo, y - x) for lo, x, y in zip(edges, ba, bb)) + " dB")


def cmd_material(a):
    if not a.music:
        write_wav(a.out, np.repeat(generated(40)[:, None], 2, 1))
    else:
        n = 40 * SR
        mix = np.zeros((n, 2))
        for path in a.music:
            x = read_wav(path)
            mix += np.tile(x, (n // len(x) + 1, 1))[:n]
        write_wav(a.out, mix * (.5 / np.max(np.abs(mix))))
    print("%s: 40 s" % a.out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    on_frizz = argparse.ArgumentParser(add_help=False)
    on_frizz.add_argument("--slot", type=int, choices=chompi.FRIZZ_SLOTS,
                          help="the FRIZZ slot to measure (10, 12), started unless it runs; "
                          "default the one running (remote.py)")
    p = sub.add_parser("clock", parents=[on_frizz])
    p.add_argument("--mode", choices=["single", "pairs", "catchup", "late"], default="single")
    p.add_argument("--bpm", type=float, default=120.)
    p.add_argument("--seconds", type=float, default=20.)
    p = sub.add_parser("drift", parents=[on_frizz])
    p.add_argument("--bpm", type=float, default=120.)
    p.add_argument("--bars", type=int, default=1)
    p.add_argument("--mode", choices=["single", "pairs", "catchup", "late"], default="single")
    p.add_argument("--seconds", type=float, default=180.)
    p.add_argument("--out", help="keep the recording as OUT.f32 and the material as OUT.material.wav")
    p = sub.add_parser("fx", parents=[on_frizz])
    p.add_argument("key", choices=["KEY_%d" % i for i in range(1, 14)])
    p.add_argument("--level", type=int, help="CC of the effect's level knob, set to 100")
    p.add_argument("--seconds", type=float, default=30.)
    p.add_argument("--material", help="a WAV into AUX (measure.py material); generated if not given")
    p.add_argument("--out", required=True)
    p = sub.add_parser("compare")
    p.add_argument("a")
    p.add_argument("b")
    p = sub.add_parser("material")
    p.add_argument("--music", nargs="*")
    p.add_argument("--out", required=True)
    a = ap.parse_args()
    {"clock": cmd_clock, "drift": cmd_drift, "fx": cmd_fx, "compare": cmd_compare,
     "material": cmd_material}[a.cmd](a)


if __name__ == "__main__":
    main()
