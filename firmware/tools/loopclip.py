#!/usr/bin/env python3
"""loopclip.py: does the loop clip where the live input is clean? (#152)

A 1 kHz sine at rising levels into the input, either from the audio interface into AUX
(measure.py's wiring, --source aux) or from FRIZZ's own test signal in the input's place
(--source signal, remote.py signal). At each level: the input's peak as FRIZZ takes it (LEVELS),
the line out with the input alone (no loop: the mix is the input's), then a loop recorded from
it (LOOP, LOOP), the input silenced and the line out with the loop alone (the mix goes to the
loop on its own as the recording closes), the loop erased. The two recordings' distortion
(THD, the harmonics against the fundamental) side by side: both went through the same
compressor, VOLUME and limiter, so a loop distorting where the live input doesn't is the loop's
int16 clipping (Looper.h).

    tools/loopclip.py [--source aux|signal] [--levels DB,DB,...] [--slot N]

Run it on an empty looper, under the lock (tools/chompi.py hold), with numpy (measure.py).
"""
import argparse
import os
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import chompi  # noqa: E402
import measure  # noqa: E402
import remote  # noqa: E402
import numpy as np  # noqa: E402

SR = measure.SR
HZ = 1000


def thd(y):
    """The harmonics 2-10 against the fundamental, in dB, and the fundamental's peak"""
    y = y[len(y) // 4:]  # past the start
    n = len(y) // 48 * 48
    y = y[:n] * np.hanning(n)
    p = np.abs(np.fft.rfft(y)) ** 2
    fr = np.fft.rfftfreq(n, 1. / SR)

    def band(f):
        return p[(fr > f - 30) & (fr < f + 30)].sum()
    fund = band(HZ)
    harm = sum(band(HZ * k) for k in range(2, 11))
    return 10 * np.log10(harm / fund + 1e-20), np.sqrt(fund * 2) / (np.sum(np.hanning(n)) / 2)


def peaks(f):
    g = chompi.get7
    remote.dev_data(f, chompi.LEVELS, [1], "LEVELS")
    time.sleep(.3)
    d = remote.dev_data(f, chompi.LEVELS, [1], "LEVELS")
    return [g(d[5 + 2 * i:7 + 2 * i]) / 8192 for i in range(len(remote.PEAKS))]


def tap(f, name):
    f.key(name, True)
    time.sleep(.05)
    f.key(name, False)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", choices=["aux", "signal"], default="aux")
    ap.add_argument("--levels", default="-24,-12,-9,-7,-6,-5,-4,-3,-2,-1,0")
    ap.add_argument("--slot", type=int, choices=chompi.FRIZZ_SLOTS,
                    help="the FRIZZ slot (10, 11, 12), started unless it runs; default the one running")
    ap.add_argument("--settle", type=float, default=10., help="seconds to wait first")
    ap.add_argument("--out", help="keep the recordings: NAME-<dB>-live.f32 / -loop.f32")
    a = ap.parse_args()
    f = remote.Frizz(chompi.to_frizz(a.slot))
    time.sleep(a.settle)  # past the boot animation, which takes the keys (just started?)
    if f.state()[0] != 0:
        sys.exit("the looper isn't empty")
    tmp = tempfile.mkdtemp()
    try:
        run(a, f, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def run(a, f, tmp):
    print("source %s; input peak as FRIZZ takes it (AUX L), line out L: THD live / loop, "
          "the fundamental's peak live / loop" % a.source)
    for db in (float(x) for x in a.levels.split(",")):
        amp = 10 ** (db / 20)
        player = None
        if a.source == "aux":
            t = np.arange(40 * SR) / SR
            wav = os.path.join(tmp, "sine.wav")
            measure.write_wav(wav, np.repeat((amp * np.sin(2 * np.pi * HZ * t))[:, None], 2, 1))
            player = measure.play(wav)
        else:
            remote.signal(f, "sine", HZ, amp)
        time.sleep(1.5)
        pk = peaks(f)
        live = measure.record(1.5)
        tap(f, "KEY_28")
        time.sleep(1.2)
        tap(f, "KEY_28")
        time.sleep(.3)
        if player:
            player.terminate()
            player.wait()
        else:
            remote.signal(f, "off", 0, 0)
        time.sleep(.5)
        st = f.state()[0]
        loop = measure.record(1.5)
        if a.out:
            live.astype("<f4").tofile("%s%+g-live.f32" % (a.out, db))
            loop.astype("<f4").tofile("%s%+g-loop.f32" % (a.out, db))
        tl, al = thd(live)
        tp, ap_ = thd(loop)
        print("%+5.1f dB: input %.3f (%6.1f dBFS)  THD live %6.1f dB, loop %6.1f dB  peak %.3f / %.3f%s"
              % (db, pk[1], 20 * np.log10(pk[1] + 1e-9), tl, tp, al, ap_,
                 "" if st == 2 else "  (looper %s!)" % remote.LOOPER[st]), flush=True)
        time.sleep(.6)  # past the erase lock
        tap(f, "KEY_28")  # erase
        time.sleep(.8)
        if f.state()[0] != 0:
            sys.exit("the loop didn't erase")


if __name__ == "__main__":
    main()
