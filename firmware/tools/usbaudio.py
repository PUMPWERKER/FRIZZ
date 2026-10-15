#!/usr/bin/env python3
"""usbaudio.py: measuring FRIZZ as a USB sound card (#196, UsbDevice.h).

    tools/usbaudio.py sine FILE.wav [--freq F]   a recorded sine: level, frequency, glitches
    tools/usbaudio.py rate CARD [--secs N]       the streams' rate as the host sees them:
                                                 hw_ptr against the clock (/proc/asound) and
                                                 the playback's feedback (stream0's Momentary freq)
    tools/usbaudio.py gen FILE.wav [--freq F] [--secs N] [--db D]   a stereo 24-bit sine to play
    tools/usbaudio.py ramp FILE.wav              a recorded saw: rising or falling, per channel (the
                                                 polarity against the jacks)
    tools/usbaudio.py stats [--every S]          FRIZZ's own counts (kCmdDev's USB): blocks to and
                                                 from the host, the stream run dry, the FIFOs, the card
    tools/usbaudio.py card on|off                the card lent to the computer, or taken back
                                                 (on: in a hold, as card.py mount)

A sine's glitches: a clean sine x satisfies x[n] = 2 cos(w) x[n-1] - x[n-2]; the residual of
that is noise-small, and a dropped or doubled packet, a click or a fade shows as a spike far
above it. Python 3 without packages (no numpy), so it takes some seconds a minute of audio.
From the PoC (#275); firmware/README.md, *USB audio: the sound card and the drive*, says how it's used.
"""
import argparse
import math
import os
import re
import struct
import sys
import time
import wave


def read_wav(path):
    """(rate, channels as lists of floats, full scale 1)"""
    with wave.open(path, "rb") as w:
        rate, ch, width, n = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
        raw = w.readframes(n)
    full = float(1 << (8 * width - 1))
    chans = [[] for _ in range(ch)]
    step = width * ch
    for c in range(ch):
        out = chans[c]
        for i in range(c * width, len(raw), step):
            b = raw[i:i + width]
            if width == 3:
                v = b[0] | b[1] << 8 | b[2] << 16
                v -= (v & 0x800000) << 1
            elif width == 2:
                v = struct.unpack("<h", b)[0]
            else:
                v = struct.unpack("<i", b)[0]
            out.append(v / full)
    return rate, chans


def frequency(x, rate):
    """by the rising zero crossings, interpolated, fitted over the whole: Hz"""
    t = []
    for i in range(1, len(x)):
        if x[i - 1] < 0 <= x[i]:
            t.append(i - 1 + x[i - 1] / (x[i - 1] - x[i]))
    if len(t) < 3:
        return 0.0, t
    n = len(t)
    mk, mt = (n - 1) / 2, sum(t) / n
    slope = sum((k - mk) * (tk - mt) for k, tk in enumerate(t)) / sum((k - mk) ** 2 for k in range(n))
    return rate / slope, t


def sine(args):
    rate, chans = read_wav(args.file)
    print("%s: %d Hz, %d channels, %.1f s" % (args.file, rate, len(chans), len(chans[0]) / rate))
    for c, x in enumerate(chans):
        n = len(x)
        rms = math.sqrt(sum(v * v for v in x) / n) if n else 0
        peak = max((abs(v) for v in x), default=0)
        f, _ = frequency(x, rate)
        line = "ch%d: rms %.2f dBFS, peak %.2f dBFS, %.4f Hz" % (
            c + 1, 20 * math.log10(rms or 1e-12), 20 * math.log10(peak or 1e-12), f)
        if f > 0 and rms > 1e-4:
            k = 2 * math.cos(2 * math.pi * f / rate)
            res = [abs(x[i] - k * x[i - 1] + x[i - 2]) for i in range(2, n)]
            srt = sorted(res)
            floor = srt[len(srt) // 2] or 1e-9
            # a glitch: far above both the floor and the sine's own step at that level
            limit = max(args.factor * floor, args.min * peak)
            spikes, last = [], -10 * rate
            for i, r in enumerate(res):
                if r > limit and i - last > rate // 100:
                    spikes.append(i + 2)
                    last = i
            line += ", residual median %.1e max %.1e, glitches %d" % (floor, srt[-1], len(spikes))
            if spikes:
                line += " at " + ", ".join("%.3f s" % (s / rate) for s in spikes[:12])
                if len(spikes) > 12:
                    line += " ..."
        print(line)
        if f > 0 and rms > 1e-4:
            print("     " + phase_jumps(x, rate, f))


def phase_jumps(x, rate, f, window=0.25):
    """a lost or doubled packet moves the sine's phase by whole samples: each window's rising
    zero crossings against the whole's frequency, the largest step between windows"""
    _, t = frequency(x, rate)
    per = rate / f
    wins, cur, edge = [], [], window * rate
    for tk in t:
        if tk > edge:
            if cur:
                wins.append((edge, cur))
            cur, edge = [], edge + window * rate
        cur.append(tk)
    phases = []
    for e, ts in wins:
        # the crossings' offset from the grid of periods, unwrapped against the first
        ref = ts[0] % per
        ph = sum(((tk % per) - ref + per / 2) % per - per / 2 for tk in ts) / len(ts) + ref
        phases.append((e / rate, ph))
    steps = []
    for (t0, p0), (t1, p1) in zip(phases, phases[1:]):
        d = (p1 - p0 + per / 2) % per - per / 2
        steps.append((abs(d), t1))
    steps.sort(reverse=True)
    big = [st for st in steps if st[0] > 0.5]
    return "phase: largest step between %.2f s windows %.2f samples%s" % (
        window, steps[0][0] if steps else 0,
        "; jumps over half a sample at " + ", ".join("%.2f s (%.1f)" % (t, d) for d, t in big[:10]) if big else ", none over half a sample")


def gen(args):
    rate, n = 48000, int(args.secs * 48000)
    amp = 10 ** (args.db / 20) * 8388607
    with wave.open(args.file, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(3)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(n):
            if args.wave == "saw":
                ph = (args.freq * i / rate) % 1.0
                v = int(round(amp * (2 * ph - 1)))
            else:
                v = int(round(amp * math.sin(2 * math.pi * args.freq * i / rate)))
            b = struct.pack("<i", v)[:3]
            frames += b + b
        w.writeframes(bytes(frames))
    print("%s: %.1f s of %g Hz at %g dBFS, stereo 24 bit" % (args.file, args.secs, args.freq, args.db))


def ramp(args):
    """a saw's slow ramp and its jump go opposite ways: the ramp's direction is the sign of the
    median step"""
    rate, chans = read_wav(args.file)
    for c, x in enumerate(chans):
        steps = sorted(x[i] - x[i - 1] for i in range(1, len(x)))
        med = steps[len(steps) // 2]
        print("ch%d: %s (median step %+.2e, largest jumps %+.3f / %+.3f)" % (
            c + 1, "rising" if med > 0 else "falling" if med < 0 else "flat", med, steps[0], steps[-1]))


def status(card, dev, sub):
    """(hw_ptr, the time read) of a running substream, or None"""
    path = "/proc/asound/card%s/pcm0%s/sub0/status" % (card, "p" if dev == "p" else "c")
    try:
        text = open(path).read()
    except OSError:
        return None
    m = re.search(r"hw_ptr\s*:\s*(\d+)", text)
    if not m or "RUNNING" not in text:
        return None
    # the kernel's tstamp moves only when the application asks: the time of this read instead
    return int(m.group(1)), time.monotonic()


def rate(args):
    samples = {"p": [], "c": []}
    fb = []
    end = time.time() + args.secs
    while time.time() < end:
        for d in "pc":
            s = status(args.card, d, 0)
            if s:
                samples[d].append(s)
        try:
            text = open("/proc/asound/card%s/stream0" % args.card).read()
            m = re.search(r"Momentary freq = (\d+) Hz", text)
            if m:
                fb.append(int(m.group(1)))
        except OSError:
            pass
        time.sleep(args.every)
    for d, name in (("p", "playback (host -> FRIZZ)"), ("c", "capture (FRIZZ -> host)")):
        s = samples[d]
        if len(s) < 2:
            print("%s: not running" % name)
            continue
        (p0, t0), (p1, t1) = s[0], s[-1]
        r = (p1 - p0) / (t1 - t0)
        print("%s: %.2f Hz over %.1f s (%+.0f ppm against 48000)" % (name, r, t1 - t0, (r / 48000 - 1) * 1e6))
    if fb:
        print("playback's Momentary freq: min %d, max %d, last %d Hz (%d reads)" % (min(fb), max(fb), fb[-1], len(fb)))


USB = 0x0E  # kCmdDev's USB (MidiControl.h)


def frizz():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    import chompi
    import remote
    return chompi, remote.Frizz(chompi.to_frizz(None))


def flags_text(f):
    return "host %s, USB audio %s, it plays %s, records %s, card %s" % (
        "yes" if f & 1 else "no", "on" if f & 8 else "off", "yes" if f & 2 else "no",
        "yes" if f & 16 else "no", "lent" if f & 4 else "FRIZZ's")


def stats(args):
    chompi, f = frizz()
    while True:
        got = f.dev(USB, [0])
        if not got or got[0] != chompi.OK:
            sys.exit("no answer to USB: a FRIZZ from before TinyUSB")
        d = got[1]
        v = [chompi.get7(d[1 + 5 * i:6 + 5 * i]) for i in range(5)]
        rx_fill = d[26] << 7 | d[27] if len(d) >= 30 else 0
        tx_fill = d[28] << 7 | d[29] if len(d) >= 30 else 0
        line = ("%s; from the host %d blocks, ran dry %d, starts %d; to the host %d blocks, IN FIFO full %d; "
                "FIFOs %d / %d bytes" % (flags_text(d[0]), v[0], v[1], v[2], v[3], v[4], rx_fill, tx_fill))
        if len(d) >= 32:
            line += "; on the bus %d times" % (d[30] << 7 | d[31])
        got = f.dev(USB, [0, 1])
        if got and got[0] == chompi.OK and len(got[1]) >= 25:
            p = [chompi.get7(got[1][1 + 5 * i:6 + 5 * i]) for i in range(4)]
            line += "; host's packets %d (short %d, long %d), OUT FIFO full %d, fill %d..%d bytes" % (
                p[0], p[1], p[2], p[3], got[1][21] << 7 | got[1][22], got[1][23] << 7 | got[1][24])
        got = f.dev(USB, [1])
        if got and got[0] == chompi.OK:
            c = [chompi.get7(got[1][1 + 5 * i:6 + 5 * i]) for i in range(6)]
            line += "; card %d reads %d KB, %d writes %d KB, %d errors, %.1f s in the card" % (
                c[0], c[2], c[1], c[3], c[4], c[5] / 1000)
            if len(got[1]) >= 35:
                line += "; the host's TEST UNIT READY %d, READ CAPACITY %d" % (
                    got[1][31] << 7 | got[1][32], got[1][33] << 7 | got[1][34])
        print(line, flush=True)
        if not args.every:
            break
        time.sleep(args.every)


def card(args):
    if args.state == "on" and not os.environ.get("FRIZZ_CHOMPI_HELD"):
        # the card stays lent after this ends, and a tool's restart then would take the drive
        # from under the computer: the CHOMPI stays held until card off (as card.py mount)
        sys.exit("card on leaves the card lent until card off, which no lock of its own covers: "
                 "run both in a hold, e.g. tools/chompi.py hold bash")
    chompi, f = frizz()
    got = f.dev(USB, [2, {"on": 1, "off": 0}[args.state]])
    if not got or got[0] != chompi.OK:
        sys.exit("no answer to USB: a FRIZZ from before TinyUSB")
    time.sleep(0.3)
    got = f.dev(USB, [1])
    print(flags_text(got[1][0]) if got and got[0] == chompi.OK else "?")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("sine")
    s.add_argument("file")
    s.add_argument("--factor", type=float, default=50, help="a glitch: this many times the residual's median")
    s.add_argument("--min", type=float, default=0.02, help="and at least this share of the peak")
    g = sub.add_parser("gen")
    g.add_argument("file")
    g.add_argument("--freq", type=float, default=997)
    g.add_argument("--secs", type=float, default=60)
    g.add_argument("--db", type=float, default=-12)
    g.add_argument("--wave", choices=("sine", "saw"), default="sine")
    m = sub.add_parser("ramp")
    m.add_argument("file")
    r = sub.add_parser("rate")
    r.add_argument("card")
    r.add_argument("--secs", type=float, default=60)
    r.add_argument("--every", type=float, default=1)
    t = sub.add_parser("stats")
    t.add_argument("--every", type=float, default=0)
    c = sub.add_parser("card")
    c.add_argument("state", choices=("on", "off"))
    args = ap.parse_args()
    {"sine": sine, "gen": gen, "rate": rate, "stats": stats, "card": card, "ramp": ramp}[args.cmd](args)


if __name__ == "__main__":
    main()
