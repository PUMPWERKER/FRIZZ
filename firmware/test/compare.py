#!/usr/bin/env python3
"""compare.py <a.bin> <b.bin>: whether two harness runs are bit-identical, and if not, the
loudness of each segment in both (master out RMS), the peak and any NaNs. Exits 0 when
bit-identical, 1 when they differ, 2 when it couldn't compare them (a run it can't read, or its
own fault: check.sh's CI step fails on that, not on a difference)."""
import math, os, struct, sys, traceback

BLOCK = 24
SEG = int(3 * 48000 / BLOCK)   # blocks per 3s segment
TAIL = ["all inserts", "everything", "everything 2", "tails"]
# For harnesses that don't write <run>.names: their FX in kAll order, by meter count
LEGACY = {9: ["filter", "crusher", "freezer", "slicer", "flanger", "shifter", "resonator"]}
LEGACY[10] = LEGACY[9] + ["folder"]
LEGACY[12] = LEGACY[10] + ["warble", "tapestop"]
for m in LEGACY:
    LEGACY[m] = LEGACY[m] + ["delay", "reverb"]

def fail(why):
    print(f"couldn't compare: {why}")
    sys.exit(2)

def layout(path, n_floats):
    """A run's floats per block and its segment names: from <run>.names when the harness
    wrote one, otherwise guessed from the run's size"""
    if os.path.exists(path + ".names"):
        fx = open(path + ".names").read().split()
        meters = len(fx)
    else:
        for meters, fx in LEGACY.items():
            if n_floats == (meters + 4) * SEG * (4 * BLOCK + meters):
                break
        else:
            fail(f"{path}: unexpected run size: {n_floats} floats")
    rec = 4 * BLOCK + meters
    if n_floats != (meters + 4) * SEG * rec:
        fail(f"{path}: {n_floats} floats don't match {meters} FX")
    return rec, fx + TAIL

def load(path):
    data = open(path, "rb").read()
    return struct.unpack(f"{len(data) // 4}f", data)

def main():
    a_path, b_path = sys.argv[1], sys.argv[2]
    if open(a_path, "rb").read() == open(b_path, "rb").read():
        print("bit-identical: outputs and meters")
        return 0

    a, b = load(a_path), load(b_path)
    rec_a, names_a = layout(a_path, len(a))
    rec_b, names_b = layout(b_path, len(b))
    print("DIFFERENT" + ("" if names_a == names_b else " (different FX: segments matched by name)"))
    print("NaNs:", sum(1 for x in b if x != x))
    print(f"{'segment':12s} {'RMS a':>8s} {'RMS b':>8s} {'peak b':>8s}")

    def master(v, rec, s):  # master out L/R of segment s
        lo, hi = s * SEG * rec, (s + 1) * SEG * rec
        return [v[i] for i in range(lo, hi) if 2 * BLOCK <= (i - lo) % rec < 4 * BLOCK]

    rms = lambda v: math.sqrt(sum(x * x for x in v) / len(v)) if v else 0.0
    for s, name in enumerate(names_b):
        vb = master(b, rec_b, s)
        ra = f"{rms(master(a, rec_a, names_a.index(name))):8.4f}" if name in names_a else f"{'-':>8s}"
        print(f"{name:12s} {ra} {rms(vb):8.4f} {max(abs(x) for x in vb):8.4f}")
    return 1

try:
    sys.exit(main())
except Exception as e:  # noqa: BLE001: a fault of its own isn't a difference
    traceback.print_exc(file=sys.stdout)
    fail(f"{type(e).__name__}: {e}")
