#!/usr/bin/env python3
"""compare.py DIR_A DIR_B NAME...: compares two twins' runs of the same scenarios (compare.sh).

Each DIR holds NAME.wav (the master out) and NAME.leds (the LEDs at every change) per scenario.
Prints one line per scenario: bit-identical, or where the sound and the LEDs first part, by how
much, and which LEDs. Exits 0 when every scenario is bit-identical. LED logs of different lengths,
or a line without all 35 LEDs (a log cut short), are a difference, said as such.

compare.py --selftest: checks compare_leds on made-up logs, the ones cut short too.
"""
import json
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# the LEDs by what they show: the panel's in NormalPage.h's numbering, the keys' by the key above
# which they sit on the board (web/layout.json)
PTH = ["CHOMPI", "knob 1", "knob 2", "knob 3", "knob 4", "transport rev", "transport fwd",
       "PLAY", "LOOP", "VOLUME"]
SMT = []
try:
    layout = json.load(open(os.path.join(HERE, "web", "layout.json")))
    for l in layout["smt"]:
        k = min(layout["keys"], key=lambda k: math.hypot(k["x"] - l["x"], k["y"] - l["y"]))
        SMT.append("key %s" % k["name"][3:])
except (OSError, ValueError, KeyError):
    SMT = ["smt %d" % i for i in range(25)]
NAMES = PTH + SMT


def samples(path):
    data = open(path, "rb").read()[44:]
    return struct.unpack("<%df" % (len(data) // 4), data)


def leds(path):
    """[(ms, [35 colours])]"""
    out = []
    for line in open(path):
        parts = line.split()
        if len(parts) < 2:
            continue
        cols = [p for p in parts[2:] if p not in ("|", "smt")]
        out.append((int(parts[0]), cols))
    return out


def compare_audio(a, b):
    if a == b:
        return None
    n = min(len(a), len(b))
    first = next((i for i in range(n) if a[i] != b[i]), n)
    diff = [a[i] - b[i] for i in range(first, n)]
    peak = max((abs(d) for d in diff), default=0.)
    rms = math.sqrt(sum(d * d for d in diff) / max(1, len(diff)))
    ref = math.sqrt(sum(x * x for x in a[first:n]) / max(1, n - first))
    db = 20 * math.log10(rms / ref) if rms > 0 and ref > 0 else float("-inf")
    return "sound from %.3f s (difference peak %.4f, %.0f dB below the signal)" % (
        first / 2 / 48000, peak, -db)


def compare_leds(a, b):
    """Walks both logs in time order: the first time their LEDs differ, and how often after. A
    log with more or fewer lines, or a line short of LEDs, says so"""
    if a == b:
        return None
    notes = []
    if len(a) != len(b):
        notes.append("LED trace lengths differ: %d vs %d" % (len(a), len(b)))
    for side, log in (("A", a), ("B", b)):
        short = next((i for i, (_, cols) in enumerate(log) if len(cols) != len(NAMES)), None)
        if short is not None:
            notes.append("%s's LED trace line %d has %d of %d LEDs (cut short?)" % (
                side, short + 1, len(log[short][1]), len(NAMES)))
    ia = ib = 0
    sa = sb = None
    first, differing = None, 0
    per_led = [0] * len(NAMES)
    while ia < len(a) or ib < len(b):
        t = min(a[ia][0] if ia < len(a) else 1 << 62, b[ib][0] if ib < len(b) else 1 << 62)
        while ia < len(a) and a[ia][0] == t:
            sa = a[ia][1]
            ia += 1
        while ib < len(b) and b[ib][0] == t:
            sb = b[ib][1]
            ib += 1
        if sa != sb:
            differing += 1
            if first is None:
                first = t
            for i in range(len(NAMES)):
                if not sa or not sb or i >= len(sa) or i >= len(sb) or sa[i] != sb[i]:
                    per_led[i] += 1
    if first is not None:
        # the LEDs that differ, most often first
        which = sorted((n, NAMES[i]) for i, n in enumerate(per_led) if n)[::-1]
        notes.insert(0, "LEDs from %.3f s, %d times: %s" % (
            first / 1000, differing, ", ".join("%s %dx" % (name, n) for n, name in which[:6])
            + (" ..." if len(which) > 6 else "")))
    return "; ".join(notes) or None


def selftest():
    """compare_leds on made-up logs: the same, one LED changed, one log longer, the last line
    cut short (as a twin whose writes failed leaves it), the first line empty"""
    off, on = ["000000"] * len(NAMES), ["000000"] * len(NAMES)
    on[7] = "ffffff"  # PLAY
    log = [(0, off), (100, on), (200, off)]
    cases = [
        ("the same", log, list(log), lambda r: r is None),
        ("PLAY changed", log, [(0, off), (150, on), (200, off)],
         lambda r: r.startswith("LEDs from 0.100 s, 1 times: PLAY 1x") and "lengths" not in r),
        ("B longer", log, log + [(300, on)],
         lambda r: r.startswith("LEDs from 0.300 s") and "LED trace lengths differ: 3 vs 4" in r),
        ("B cut short", log, log[:2] + [(200, off[:4])],
         lambda r: "B's LED trace line 3 has 4 of %d LEDs (cut short?)" % len(NAMES) in r
         and "lengths" not in r),
        ("A empty at first", [(0, [])] + log[1:], log,
         lambda r: "A's LED trace line 1 has 0 of" in r),
    ]
    failed = 0
    for what, a, b, ok in cases:
        try:
            r = compare_leds(a, b)
            good = ok(r)
        except Exception as e:  # noqa: BLE001: a crash is what this checks against
            r, good = "%s: %s" % (type(e).__name__, e), False
        failed += not good
        print("%s  %s: %s" % ("ok  " if good else "FAIL", what, r))
    return failed == 0


def main():
    if sys.argv[1:] == ["--selftest"]:
        sys.exit(0 if selftest() else 1)
    da, db, names = sys.argv[1], sys.argv[2], sys.argv[3:]
    same = True
    for name in names:
        notes = [compare_audio(samples(os.path.join(da, name + ".wav")),
                               samples(os.path.join(db, name + ".wav"))),
                 compare_leds(leds(os.path.join(da, name + ".leds")),
                              leds(os.path.join(db, name + ".leds")))]
        notes = [n for n in notes if n]
        same &= not notes
        print("%-12s %s" % (name, "bit-identical" if not notes else notes[0]))
        for n in notes[1:]:
            print("%-12s %s" % ("", n))
    sys.exit(0 if same else 1)


if __name__ == "__main__":
    main()
