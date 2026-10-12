#!/usr/bin/env python3
"""reaper_stub.py: a stand-in for REAPER in the host check (reaper.cpp), for tools/reaper.py
(FRIZZ_REAPER). Started as reaper.py starts REAPER (-cfgfile DIR/reaper.ini ...), it does what
tools/reaper.lua does inside REAPER: reads PLAN from DIR/Scripts/__startup.lua, writes its log,
carries out the command file's lines (one "send" a time round, every 30 ms) and logs what
comes in on its MIDI device, which here is the twin's USB pseudo-terminal (the device's name,
"hw:" and its path). Its MIDI is REAPER's as seen on the device (REAPER 7, its virtual port):
  - a clock, 24 ticks a beat, while playing; Start when it plays from 0, else Continue; Stop
    and Song Position (the edit cursor's) when it stops; Song Position when it's located
    while stopped; Stop, Song Position, Continue when it's located while playing, and at each
    turn of the loop range;
  - a lane's CCs at their beats while playing, and each CC a lane used back to 0 on stop
    and at a turn of the loop range (REAPER's "Reset MIDI CC/Pitch on: playback stop /
    playback loop/skip", on by default), unless reaper.ini's ccresettab has "=-1";
  - a monitored track sends back what comes in, but the clock.
It checks reaper.py's logic, not REAPER.
"""
import os
import re
import select
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import midiport  # noqa: E402

TICK, START, CONTINUE, STOP = 0xF8, 0xFA, 0xFB, 0xFC


def plan_of(path):
    """PLAN's strings and numbers, from the startup script's first line"""
    with open(path) as f:
        line = f.readline()
    plan = {}
    for key, s, n in re.findall(r'(\w+) = (?:"((?:[^"\\]|\\.)*)"|(-?[\d.]+))', line):
        plan[key] = s if not n else float(n)
    return plan


class Stub:
    def __init__(self, plan, resets):
        self.plan, self.resets = plan, resets
        self.t0 = time.perf_counter()
        self.out = open(plan["log"], "w")
        self.lock = threading.Lock()
        self.fd = None
        name = plan.get("out_name")
        if name:
            self.fd = os.open(name[3:], os.O_RDWR | os.O_NONBLOCK)
        self.bpm = 120.
        self.playing = False
        self.cursor = 0.  # beats: where play starts, where it goes back to on stop
        self.at_beat, self.at_time = 0., 0.  # the play position: the beat at the time
        self.loop = None
        self.lanes = []  # [(beat, bytes)]
        self.monitor = False
        self.tracing = False
        self.stop = False
        self.tap = midiport.Tap.__new__(midiport.Tap)
        self.tap.log, self.tap.status, self.tap.msg = [], None, []
        self.tap.put = lambda t, msg: self.came(msg)
        self.clock = threading.Thread(target=self.run_clock, daemon=True)

    def log(self, fmt, *args):
        with self.lock:
            self.out.write("%.4f %s\n" % (time.perf_counter() - self.t0, fmt % args))
            self.out.flush()

    def write(self, data):
        if self.fd is not None:
            with self.lock:
                os.write(self.fd, bytes(data))

    def came(self, msg):
        self.log("in %s", msg.hex())
        if self.monitor and msg[0] < 0xF8:
            self.write(msg)

    # ---- the transport ----
    def pos(self, now=None):
        """The play position in beats"""
        if not self.playing:
            return self.cursor
        now = time.perf_counter() if now is None else now
        return self.at_beat + (now - self.at_time) * self.bpm / 60.

    def spp(self, beat):
        sixteenths = int(beat * 4 + .5)
        return bytes([0xF2, sixteenths & 0x7F, sixteenths >> 7 & 0x7F])

    def play(self):
        if self.playing:
            return
        self.at_beat, self.at_time = self.cursor, time.perf_counter()
        self.playing = True
        self.write([START] if self.cursor == 0 else [CONTINUE])

    def halt(self):
        if not self.playing:
            return
        self.playing = False
        self.write(bytes([STOP]) + self.spp(self.cursor))
        self.reset()

    def reset(self):
        if self.resets:
            for cc in sorted({(m[0], m[1]) for _, m in self.lanes}):
                self.write([cc[0], cc[1], 0])

    def locate(self, beat):
        self.cursor = beat
        if self.playing:
            self.at_beat, self.at_time = beat, time.perf_counter()
            self.write(bytes([STOP]) + self.spp(beat) + bytes([CONTINUE]))
        else:
            self.write(self.spp(beat))

    def run_clock(self):
        """The clock and the lanes while playing, the loop range's turns"""
        last = None
        while not self.stop:
            time.sleep(.0005)
            if not self.playing:
                last = None
                continue
            now = time.perf_counter()
            beat = self.pos(now)
            if self.loop and beat >= self.loop[1]:
                self.at_beat, self.at_time = self.loop[0], now
                self.write(bytes([STOP]) + self.spp(self.loop[0]) + bytes([CONTINUE]))
                self.reset()
                last = None
                continue
            if last is not None:
                for b, msg in self.lanes:
                    if last < b <= beat or b == 0 and last <= 0 < beat:
                        self.write(msg)
                ticks = int(beat * 24) - int(last * 24)
                if ticks > 0:
                    self.write([TICK] * ticks)
            else:
                for b, msg in self.lanes:
                    if b == beat:
                        self.write(msg)
            last = beat

    # ---- the commands ----
    def run(self, w, sent):
        cid, name = w[0], w[1]
        if name == "send":
            if sent:
                return False
            self.write(bytes.fromhex(w[2]))
        elif name == "play":
            self.play()
        elif name == "stop":
            self.halt()
        elif name == "pause":
            self.halt()
        elif name == "locate":
            self.locate(float(w[2]))
        elif name == "tempo":
            beat = self.pos()
            self.bpm = float(w[2])
            self.at_beat, self.at_time = beat, time.perf_counter()
        elif name == "loop":
            self.loop = None if w[2] == "off" else (float(w[2]), float(w[3]))
        elif name == "lane":
            for e in w[3:]:
                b, s, d1, d2 = e.split(",")
                self.lanes.append((float(b), bytes([int(s), int(d1), int(d2)])))
            self.lanes.sort(key=lambda x: x[0])
        elif name == "monitor":
            self.monitor = True
        elif name == "clear":
            self.lanes, self.monitor = [], False
        elif name == "trace":
            self.tracing = w[2] == "on"
        p = self.pos()
        self.log("cmd %s %s pos %.4f %.4f state %d", cid, name, p * 60. / self.bpm, p, 1 if self.playing else 0)
        return None if name == "quit" else True

    def main(self):
        self.log("ready")
        if self.fd is not None:
            self.log("dev out 0 %s", self.plan["out_name"])
            self.log("dev in 0 %s", self.plan["out_name"])
        self.log("audio Stub 48000 512")
        self.log("listening")
        self.clock.start()
        read_at, pending = 0, []
        while True:
            if self.fd is not None:
                r, _, _ = select.select([self.fd], [], [], 0)
                if r:
                    try:
                        self.tap.feed(os.read(self.fd, 4096), time.perf_counter())
                    except OSError:
                        pass
            with open(self.plan["cmds"]) as f:
                f.seek(read_at)
                text = f.read()
            whole = text[:text.rfind("\n") + 1]
            read_at += len(whole)
            pending += [line.split() for line in whole.splitlines() if line.strip()]
            sent = False
            while pending:
                done = self.run(pending[0], sent)
                if done is None:
                    self.halt()
                    self.log("done")
                    self.stop = True
                    return
                if not done:
                    break
                sent = sent or pending[0][1] == "send"
                pending.pop(0)
            if self.tracing:
                p = self.pos()
                self.log("pos %.4f %.4f state %d", p * 60. / self.bpm, p, 1 if self.playing else 0)
            time.sleep(.03)


def main():
    cfg = sys.argv[sys.argv.index("-cfgfile") + 1]
    plan = plan_of(os.path.join(os.path.dirname(cfg), "Scripts", "__startup.lua"))
    with open(cfg) as f:
        resets = not re.search(r"^ccresettab=.*=-1", f.read(), re.M)
    Stub(plan, resets).main()


if __name__ == "__main__":
    main()
