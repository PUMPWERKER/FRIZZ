#!/usr/bin/env python3
"""usbaudio.py: the measurements of usb-audio-poc (#196), FRIZZ as a USB sound card.

    tools/usbaudio.py sine FILE.wav [--freq F]   a recorded sine: level, frequency, glitches
    tools/usbaudio.py rate CARD [--secs N]       the streams' rate as the host sees them:
                                                 hw_ptr against the clock (/proc/asound) and
                                                 the playback's feedback (stream0's Momentary freq)
    tools/usbaudio.py gen FILE.wav [--freq F] [--secs N] [--db D]   a stereo 24-bit sine to play
    tools/usbaudio.py stats [--every S]          FRIZZ's own counts (kCmdDev's USB): blocks to and
                                                 from the host, the stream run dry, the FIFOs, the card
    tools/usbaudio.py card on|off                the card lent to the computer, or taken back

A sine's glitches: a clean sine x satisfies x[n] = 2 cos(w) x[n-1] - x[n-2]; the residual of
that is noise-small, and a dropped or doubled packet, a click or a fade shows as a spike far
above it. Python 3 without packages (no numpy), so it takes some seconds a minute of audio.
An experiment's tool, not merged.
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


def gen(args):
    rate, n = 48000, int(args.secs * 48000)
    amp = 10 ** (args.db / 20) * 8388607
    with wave.open(args.file, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(3)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(n):
            v = int(round(amp * math.sin(2 * math.pi * args.freq * i / rate)))
            b = struct.pack("<i", v)[:3]
            frames += b + b
        w.writeframes(bytes(frames))
    print("%s: %.1f s of %g Hz at %g dBFS, stereo 24 bit" % (args.file, args.secs, args.freq, args.db))


def status(card, dev, sub):
    """(hw_ptr, tstamp as seconds) of a running substream, or None"""
    path = "/proc/asound/card%s/pcm0%s/sub0/status" % (card, "p" if dev == "p" else "c")
    try:
        text = open(path).read()
    except OSError:
        return None
    m = re.search(r"hw_ptr\s*:\s*(\d+)", text)
    t = re.search(r"tstamp\s*:\s*(\d+)\.(\d+)", text)
    if not m or not t:
        return None
    return int(m.group(1)), int(t.group(1)) + int(t.group(2)) / 1e9


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
    return "host %s, streaming in %s, card %s" % ("yes" if f & 1 else "no", "yes" if f & 2 else "no",
                                                  "lent" if f & 4 else "FRIZZ's")


def stats(args):
    chompi, f = frizz()
    while True:
        got = f.dev(USB, [0])
        if not got or got[0] != chompi.OK:
            sys.exit("no answer to USB: not a usb-audio-poc build")
        d = got[1]
        v = [chompi.get7(d[1 + 5 * i:6 + 5 * i]) for i in range(5)]
        rx_fill = d[26] << 7 | d[27] if len(d) >= 30 else 0
        tx_fill = d[28] << 7 | d[29] if len(d) >= 30 else 0
        line = ("%s; from the host %d blocks, ran dry %d, starts %d; to the host %d blocks, IN FIFO full %d; "
                "FIFOs %d / %d bytes" % (flags_text(d[0]), v[0], v[1], v[2], v[3], v[4], rx_fill, tx_fill))
        got = f.dev(USB, [1])
        if got and got[0] == chompi.OK:
            c = [chompi.get7(got[1][1 + 5 * i:6 + 5 * i]) for i in range(6)]
            line += "; card %d reads %d KB, %d writes %d KB, %d errors, %.1f s in the card" % (
                c[0], c[2], c[1], c[3], c[4], c[5] / 1000)
        print(line, flush=True)
        if not args.every:
            break
        time.sleep(args.every)


def card(args):
    chompi, f = frizz()
    got = f.dev(USB, [2, 1 if args.state == "on" else 0])
    if not got or got[0] != chompi.OK:
        sys.exit("no answer to USB: not a usb-audio-poc build")
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
    r = sub.add_parser("rate")
    r.add_argument("card")
    r.add_argument("--secs", type=float, default=60)
    r.add_argument("--every", type=float, default=1)
    t = sub.add_parser("stats")
    t.add_argument("--every", type=float, default=0)
    c = sub.add_parser("card")
    c.add_argument("state", choices=("on", "off"))
    args = ap.parse_args()
    {"sine": sine, "gen": gen, "rate": rate, "stats": stats, "card": card}[args.cmd](args)


if __name__ == "__main__":
    main()
