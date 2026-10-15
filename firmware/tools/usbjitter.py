#!/usr/bin/env python3
"""usbjitter.py: USB MIDI out's clock while FRIZZ loads loops from the card (#265, #196).

    tools/usbjitter.py [--slot 12] [--bank cubbi_a] [--slots 1,2] [--loads 10] [--idle 5]

MIDI out goes to the jack and USB (the settings' "out", kept in RAM: PERSIST 0), FRIZZ's own
tempo (120 BPM without a clock coming in), and every clock tick that arrives over USB is
timed here as it's read. First --idle seconds alone, then --loads loads of the bank's slots in
turn (remote.py loops load, each one's f_open a MainLoop stall), then --idle seconds alone again.
Prints the ticks' intervals against the tempo's, alone and around the loads. On main, USB's
bytes went out from MainLoop (before TinyUSB, #196), so a stall there held them up; TinyUSB's
task sends them now. Linux (ALSA raw MIDI), Python 3 without packages.
"""
import argparse
import os
import select
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import chompi  # noqa: E402
import midi_send  # noqa: E402
import remote  # noqa: E402


class TapLink(midi_send.Link):
    """The link's bytes read by a thread: the clock's ticks timed, the rest kept for recv()"""

    def __init__(self, link):
        self.fd = link.fd
        self.buf = bytearray()
        self.verbose = False
        self.cv = threading.Condition()
        self.ticks = []
        self.running = True
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        while self.running:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if not r:
                continue
            try:
                data = os.read(self.fd, 4096)
            except BlockingIOError:
                continue
            t = time.monotonic()
            rest = bytearray()
            for b in data:
                if b == 0xF8:
                    self.ticks.append(t)
                elif b < 0xF8:  # other real-time bytes dropped too: they'd split an answer
                    rest.append(b)
            with self.cv:
                self.buf.extend(rest)
                self.cv.notify_all()

    def recv(self, cmd, timeout, match=None):
        deadline = time.monotonic() + timeout
        with self.cv:
            while True:
                start = self.buf.find(b"\xF0")
                end = self.buf.find(b"\xF7", start) if start >= 0 else -1
                if start >= 0 and end >= 0:
                    msg = bytes(self.buf[start:end + 1])
                    del self.buf[:end + 1]
                    if msg[:4] == midi_send.HEADER and len(msg) > 5 and msg[4] == cmd | midi_send.REPLY:
                        if match is None or match(msg[5:-1]):
                            return msg[5:-1]
                    continue
                left = deadline - time.monotonic()
                if left <= 0:
                    return None
                self.cv.wait(left)


def stats(intervals, period):
    if not intervals:
        return "no ticks"
    devs = sorted(abs(i - period) for i in intervals)
    return "%d intervals, mean %.3f ms, |deviation| median %.2f, 99%% %.2f, max %.2f ms" % (
        len(intervals), 1000 * sum(intervals) / len(intervals), 1000 * devs[len(devs) // 2],
        1000 * devs[min(len(devs) - 1, int(len(devs) * 0.99))], 1000 * devs[-1])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--slot", type=int, choices=chompi.FRIZZ_SLOTS, default=chompi.TEST_SLOT,
                    help="the FRIZZ to measure (default the test slot's); another than the test "
                    "slot's is started again at the end, from the card, so it saves again")
    ap.add_argument("--bank", default="cubbi_a")
    ap.add_argument("--slots", default="1,2", help="the bank's slots loaded in turn")
    ap.add_argument("--loads", type=int, default=10)
    ap.add_argument("--idle", type=float, default=5)
    a = ap.parse_args()

    f = remote.Frizz(chompi.to_frizz(a.slot))
    f.link = TapLink(f.link)
    if not f.ram_only():
        sys.exit("FRIZZ can't keep the card out of it (PERSIST 0): not changing its MIDI out")
    before = f.ask(remote.SETTINGS)
    f.send(remote.SETTING, [3, remote.OUTS.index("all")])
    bank = remote.BANKS.index(a.bank)
    slots = [int(s) for s in a.slots.split(",")]
    windows = []
    try:
        time.sleep(1)
        t_idle = time.monotonic()
        time.sleep(a.idle)
        t_loads = time.monotonic()
        for k in range(a.loads):
            t0 = time.monotonic()
            try:
                remote.loops_load(f, bank, slots[k % len(slots)], None, None)
            except SystemExit as e:  # refused or failed: the card was still asked
                print("load %d: %s" % (k + 1, e))
            windows.append((t0, time.monotonic()))
        t_end = time.monotonic()
        time.sleep(a.idle)
        t_after = time.monotonic()
    finally:
        if len(before) > 3:
            f.send(remote.SETTING, [3, before[3]])
        f.link.running = False
        # PERSIST 0 stays until FRIZZ restarts: the user's key 10 or 13 would keep their saves
        # in RAM, unsaid (remote.py's restore, as after a script)
        a.found = True
        remote.restore(f, a)

    ticks = f.link.ticks
    iv = [(ticks[i], ticks[i] - ticks[i - 1]) for i in range(1, len(ticks))]
    if not iv:
        sys.exit("no clock ticks over USB: MIDI out not on USB?")
    # the tempo's own period, by the idle stretches' mean
    idle = [d for t, d in iv if t_idle <= t < t_loads or t_end <= t < t_after]
    period = sum(idle) / len(idle)
    during = [d for t, d in iv if t_loads <= t < t_end]
    near = [d for t, d in iv if any(w0 - 0.05 <= t <= w1 + 0.05 for w0, w1 in windows)]
    print("USB MIDI out's clock: %.2f BPM by its idle ticks" % (60 / (24 * period)))
    print("  idle:          " + stats(idle, period))
    print("  during loads:  " + stats(during, period))
    print("  in the loads:  " + stats(near, period))
    late = sorted(((d - period), t) for t, d in iv if t_loads <= t < t_end)[-5:]
    print("  latest ticks during loads: " + ", ".join("%+.1f ms" % (1000 * d) for d, _ in reversed(late)))


if __name__ == "__main__":
    main()
