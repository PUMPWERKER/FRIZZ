#!/usr/bin/env python3
"""remote.py: plays and inspects a running FRIZZ over USB MIDI (code/src/MidiControl.h).

    ./remote.py state                 what the play page shows
    ./remote.py leds                  every LED, as the twin's LED log prints them
    ./remote.py load [--every S]      the audio callback's load (max, mean), once or every S s
    ./remote.py settings              the MIDI channel, transport following, the clock source
    ./remote.py channel N             listen on channel N (1-16), 0 for all
    ./remote.py transport on|off      MIDI Start / Continue / Stop play and pause the loop
    ./remote.py source auto|trs|usb|internal
                                      whose MIDI clock counts (the settings page's lower A#)
    ./remote.py out off|trs|all       MIDI out: off, the jack, the jack and USB (upper C#)
    ./remote.py switch up|down|hand   the mode switch: up (settings page), down (play page),
                                      or the real one again; until power-off
    ./remote.py scene get SLOT [FILE] a scene (1-4, 0 the blank one) as JSON
    ./remote.py scene put SLOT FILE   one into slot 1-4, and onto the card
    ./remote.py play SCRIPT [--cpu] [--fresh] [--force] [--save]
                                      a twin script (twin/README.md) on the device

and the developers' (kCmdDev, code/src/MidiControl.h; firmware/README.md):

    ./remote.py ident                 which build runs (its CRC-32, and which slot's that is),
                                      the bench or FRIZZ, uptime, why it last reset, RAM only
    ./remote.py ram-only              the card isn't written until FRIZZ restarts (PERSIST 0)
    ./remote.py get GROUP             an effect's knobs (by name, both pages), comp, latch,
                                      volume or settings
    ./remote.py set GROUP INDEX VALUE one of them: a knob's 0-1, a latch's 0/1, a setting's
                                      value as frizz_master.txt has it (INDEX a number or name)
    ./remote.py bench [--wait] [--out FILE]
                                      the bench's progress; --wait until it's done, --out
                                      writes its cpu.txt, as it went to the card, to FILE
    ./remote.py probe                 the clock as FRIZZ counts it, the effects' tempo, the looper
    ./remote.py levels                the effects' meters, the VU meter, the compressor's
                                      reduction, the inputs' and outputs' peaks over 0.25 s
    ./remote.py signal sine|saw|noise HZ LEVEL | off
                                      a test signal in the input's place (LEVEL 0-1 of full
                                      scale), in RAM until FRIZZ restarts; off the input again
    ./remote.py reseed [SEED]         the effects' random numbers (chaos, the slicer, the
                                      delay, wow & flutter) from SEED (0 as at power-on);
                                      without, how many each has drawn since the last
    ./remote.py battery               the battery's level and the charger's status, as FRIZZ
                                      last read them

`play` presses the keys and turns the knobs of a script over FRIZZ's SysEx, at the script's
times, sends its `midi` and `clock` to the device, and checks its `expect led` lines against the
LEDs the device shows, dimmed as the card's led_brightness (the settings page) has them: at
100, 75 or 50 %, the one level that fits every line, said when it isn't 100 %; its
`toggle 0|1` sets the mode switch (down, up) over SysEx, back to the real one when the script
ends. What only the twin has (the card, the input, the mic, AUX's jack, the battery, power-on)
is skipped, and `booted` is when the script starts. With --cpu it asks for the load every 250 ms
and prints the worst, the way to try a scenario for crackles on FRIZZ.bin itself rather than on
the bench's build; right after a start it waits (3 s at most) until FRIZZ counts its load, so the
start isn't in the worst. A bug report (/FRIZZ/bug-N.txt) plays too, from where the device is.

A script plays on FRIZZ as it stands, with what the scripts before it left in RAM since it
started (the effect selected, its knobs, the page, the loop): a twin script starts from
power-on, so its `expect led` lines can fail after another one (#123). --fresh starts the slot
again first, through the launcher (tools/chompi.py), as `flash.py --run N` does: FRIZZ boots
from the card, which a start doesn't write, and with nothing left from before. Every `play`
after the first one since a start wants it. From
the computer everything goes over USB, `midi` lines too: with the device's clock source on TRS,
a script's clock and Start / Stop are ignored there (MANUAL.md, "Clock source").

`play` first stops FRIZZ writing the card until it restarts (PERSIST 0, ram-only above), so a
script may change the settings, the compressor or the scenes: they stay in RAM, the card keeps
what the FRIZZ on key 10 shares. It says so, and, on a slot other than the test slot (12),
starts that slot again at the end, from the card, so it saves again. --save leaves the card
written as FRIZZ writes it. A FRIZZ from before PERSIST, or --save, gets the old guard:

`play` refuses a script that would change what FRIZZ keeps on the card the FRIZZ on key 10
shares, naming the line and the file (card_use). /FRIZZ/frizz_master.txt, saved 2 s after the
last change, with no .bak to go back to: the settings page (twin/scenarios/settings.txt, most
bug reports, which end with SHIFT + VOLUME held there), a key or knob while the mode switch is
up (the upper A# too: AUX's variant is saved, #152, so mic.txt), or a settings SysEx (#58);
VOLUME turned on its page 2, the input gain (#152, volume.txt); knobs 1-4 turned with the
compressor selected (comp.txt, or in focus by CC 23); MIDI's CCs never (#97). /FRIZZ/frizz_scenes.txt, whose .bak keeps only the one before: SAVE /
COPY / DELETE confirmed with CHOMPI once a slot is picked (scenes.txt), or a scene put over
SysEx (#78). It reads where the device stands before it decides (the page, what the knobs edit,
the scene mode); --force plays it anyway.

It plays on the FRIZZ that runs, and says which slot that is (10 FRIZZ, 12 FRIZZ-TEST): by
the build it runs (IDENT) and the builds flash.py sent, or, from a FRIZZ before IDENT, as the
tools that started it noted (tools/chompi.py); --slot N plays on that one, started if it
isn't the one running. FRIZZ is started first if the CHOMPI is elsewhere: at the launcher's
picker, in its USB storage firmware, or in the bench, which answers as FRIZZ does but measures
no load: the slot last started in this hold (tools/chompi.py hold), or FRIZZ's. --no-start
leaves it be, as --device does.

Linux only (ALSA's raw MIDI), Python 3 without packages, like flash.py.
"""
import argparse
import json
import math
import os
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "tools"))
import chompi  # noqa: E402
import midi_send  # noqa: E402

KEY, TURN, SETTING, SWITCH = 0x11, 0x12, 0x13, 0x14
STATE, LEDS, LOAD, SETTINGS = 0x20, 0x22, 0x23, chompi.SETTINGS
SOURCES = ["auto", "trs", "usb", "internal"]  # MidiClock.h's ClockSource, as SETTING 2 takes it
OUTS = ["off", "trs", "all"]  # MidiClock.h's MidiOutPorts, as SETTING 3 takes it
SCENE_GET, SCENE_PUT = 0x30, 0x31
# a scene's parts: 4 of 4 effects' page-1 knobs (the last has the phaser, the chaos key and
# zeros), the
# same for page 2, then the latches
FX_PARTS, FX_PER_PART = 4, 4
SCENE_PARTS = 2 * FX_PARTS + 1

# Hardware::SwId, in order (code/src/EventLog.h's kSwNames)
SW_NAMES = [
    "ENC_1_SW", "ENC_2_SW", "ENC_3_SW", "ENC_4_SW", "ENC_5_SW", "KEY_26", "SW_TOG", "KEY_16",
    "KEY_2", "KEY_3", "KEY_4", "KEY_5", "KEY_17", "KEY_18", "KEY_19", "KEY_1",
    "KEY_6", "KEY_7", "KEY_8", "KEY_9", "KEY_10", "KEY_20", "KEY_21", "KEY_22",
    "KEY_11", "KEY_12", "KEY_13", "KEY_14", "KEY_15", "KEY_23", "KEY_24", "KEY_25",
    "ENC_6_SW", "KEY_27", "KEY_28",
]
FX_NAMES = ["freezer", "shifter", "folder", "crusher", "filter", "flanger", "resonator",
            "slicer", "warble", "tapestop", "delay", "reverb", "phaser", "chaos"]
# kCmdDev's GET / SET groups (MidiControl.h's namespace dev): the effects by FxId, then these;
# the settings as masterfile::kLines lists them (MasterSettings.h), VOLUME's as NormalPage has them
GROUP_COMP, GROUP_LATCH, GROUP_VOLUME, GROUP_SETTINGS = 0x20, 0x21, 0x22, 0x23
GROUPS = dict({n: i for i, n in enumerate(FX_NAMES)}, comp=GROUP_COMP, latch=GROUP_LATCH,
              volume=GROUP_VOLUME, settings=GROUP_SETTINGS)
SETTING_NAMES = ["mono", "midi_channel", "midi_transport", "clock_factor", "clock_source",
                 "midi_out", "led_brightness", "midi_block_trs", "midi_feedback", "aux_dry",
                 "gain_aux", "gain_mic", "mic_dry", "aux_live", "mic_live"]
VOLUME_NAMES = ["out", "in", "mix", "headphones"]
LOOPER = ["empty", "recording", "playing", "paused"]
MODES = ["none", "save", "copy", "delete"]

# What a script can change on the card (#58, #78), and what keeps the one before
MASTER = "frizz_master.txt", "with no .bak"
SCENES = "frizz_scenes.txt", "whose .bak keeps only the one before"
# the play page's keys that matter there (NormalPage.h, FxSlots.h): the compressor's, the
# effects', the scene slots and SAVE / COPY / DELETE (MODES' order)
COMP_KEY = "KEY_15"
FX_KEYS = {"KEY_%d" % k for k in range(1, 15)}
SLOT_KEYS = {"KEY_%d" % k for k in range(18, 23)}
MODE_KEYS = {"KEY_23": 1, "KEY_24": 2, "KEY_25": 3}
VOLUME_KNOB, VOLUME_KEY, SHIFT_KEY = 6, "ENC_6_SW", "KEY_26"
INPUT_GAIN_PAGE = 1  # VOLUME's page 2 (PAGES), the input gain, saved (#152)
# MidiControl.h's midimap: the morph to a scene, the focus (15 the compressor), the notes
MORPH_CC, FOCUS_CC, COMP_FOCUS = 3, 23, 15
BASE_NOTE, CHOMPI_NOTE = 48, 45

PAGES = ["output gain", "input gain", "headphone feed"]

# the LED parts kCmdLeds answers: (panel?, count)
LED_PARTS = [(True, 10), (False, 9), (False, 8), (False, 8)]


def get14(hi, lo):
    return (hi << 7) | lo


def knob(v):
    """14 bits as a knob's 0-1, 8192 the centre (MidiToKnob)"""
    return v / 16384 if v <= 8192 else 0.5 + 0.5 * (v - 8192) / 8191


def to14(k):
    k = min(1.0, max(0.0, k))
    v = k * 2 * 8192 if k <= 0.5 else 8192 + (k - 0.5) * 2 * 8191
    return int(v + 0.5)


class Frizz:
    def __init__(self, device=None):
        device = device or chompi.find_device()
        if not device:
            sys.exit("no CHOMPI on USB MIDI")
        chompi.claim()
        self.link = midi_send.Link(device)
        # one write at a time, so messages don't interleave; one query at a time, whose wait
        # for the answer doesn't hold up the clock's ticks or a script's keys
        self.write_lock = threading.Lock()
        self.query_lock = threading.Lock()
        self.held = set()

    def send(self, cmd, payload=b""):
        self.raw(midi_send.HEADER + bytes([cmd]) + bytes(payload) + b"\xF7")

    def raw(self, data):
        with self.write_lock:
            os.write(self.link.fd, bytes(data))

    def dev(self, sub, args=(), retries=3):
        """kCmdDev's SUB: (status, data), asked again while unanswered (its queue full, or the
        USB lost it); None if nothing answers, a FRIZZ from before it"""
        with self.query_lock:
            for _ in range(retries):
                chompi._seq[0] = chompi._seq[0] % 127 + 1
                seq = chompi._seq[0]
                self.send(chompi.DEV, [sub, seq] + list(args))
                reply = self.link.recv(chompi.DEV, 0.5,
                                       match=lambda r: len(r) >= 3 and r[0] == sub and r[1] == seq)
                if reply is not None:
                    return reply[2], bytes(reply[3:])
        return None

    def ram_only(self):
        """PERSIST 0: True once FRIZZ keeps everything in RAM until it restarts, None from a
        FRIZZ without it"""
        got = self.dev(chompi.PERSIST, [0])
        return bool(got and got[0] == chompi.OK and got[1][:1] == bytes([chompi.RAM_ONLY])) or None

    def ask(self, cmd, payload=b"", required=True):
        """The answer's payload; None if there's none and it isn't required"""
        with self.query_lock:
            self.send(cmd, payload)
            reply = self.link.recv(cmd, 0.5)
        if reply is None and required:
            sys.exit("no answer to 0x%02X: is FRIZZ running, a build newer than v0.10?" % cmd)
        return reply

    def key(self, name, down):
        """A key (SW_NAMES) pressed or let go over SysEx; the ones held are kept, for release()"""
        if name not in SW_NAMES:
            print("unknown key %s" % name, file=sys.stderr)
            return
        self.send(KEY, [SW_NAMES.index(name), 1 if down else 0])
        (self.held.add if down else self.held.discard)(name)

    def release(self):
        """Lets go of every key held"""
        for name in sorted(self.held):
            self.key(name, False)

    def state(self):
        """STATE's answer: what the play page shows (show_state)"""
        return self.ask(STATE)

    def leds(self):
        """[(r, g, b)] * 10 for the panel, * 25 for the keys, as the LEDs get them"""
        pth, smt = [], []
        for part, (panel, count) in enumerate(LED_PARTS):
            d = self.ask(LEDS, [part])[1:]
            rgb = [tuple(d[3 * i:3 * i + 3]) for i in range(count)]
            (pth if panel else smt).extend(rgb)
        return pth, smt


def led_line(pth, smt):
    """As the twin's LED log: scaled back to 0-255 (PTH x11, SMT x4)"""
    def hexes(leds, by):
        return " ".join("%02x%02x%02x" % tuple(min(255, c * by) for c in rgb) for rgb in leds)
    return "pth " + hexes(pth, 11) + " | smt " + hexes(smt, 4)


# the LEDs' brightness the settings page sets (led_brightness on the card), in percent of
# full as LedDim takes them (code/src/temp_led_stuff.h): 25-100 by 5 (#176)
LED_PERCENTS = tuple(range(100, 20, -5))


def led_dim(v, div, percent):
    """A colour byte on the chain, as LedDim makes it"""
    d = v * percent // (100 * div)
    return 1 if d == 0 and v >= div else d


def led_levels(want, chain, rgb, levels=LED_PERCENTS):
    """The brightnesses (percent, of LEVELS) at which the device's chain bytes RGB show the
    colour WANT ("RRGGBB" at full scale, as the twin's LED log and expect led have it): any
    colour byte that the log shows as WANT's, dimmed as LedDim does"""
    div = 11 if chain == "pth" else 4
    want = [int(want[i:i + 2], 16) for i in (0, 2, 4)]
    return [q for q in levels
            if all(got in {led_dim(v, div, q) for v in range(256) if min(255, v // div * div) == w}
                   for w, got in zip(want, rgb))]


def led_full(want, chain, percent):
    """WANT ("RRGGBB" at full scale) dimmed to PERCENT, at full scale again: about what the
    device's LED shows at that brightness"""
    div = 11 if chain == "pth" else 4
    return "%02x%02x%02x" % tuple(min(255, led_dim(int(want[i:i + 2], 16), div, percent) * div)
                                  for i in (0, 2, 4))


def tempo(d):
    """The tempo in BPM, from STATE's answer"""
    return get14(d[21], d[22]) / 10


def show_state(f):
    d = f.state()
    sel, active, flags = d[4], d[5], d[6]
    latched, on = get14(d[8], d[9]), get14(d[10], d[11])
    print("looper     %s, speed %+.3f, at %d%%" % (
        LOOPER[d[0]] if d[0] < 4 else d[0], knob(get14(d[1], d[2])) * 4 - 2,
        round(d[3] / 1.27)))
    print("knobs on   %s%s" % ("compressor" if sel == len(FX_NAMES) else FX_NAMES[sel],
                               ", page 2" if len(d) > 24 and d[24] else ""))
    print("scene      %s%s%s, mode %s" % (active - 1 if active else "none",
                                         ", edited" if flags & 1 else "",
                                         ", morphing" if flags & 2 else "", MODES[d[7]]))
    print("on         %s" % " ".join(n for i, n in enumerate(FX_NAMES) if on >> i & 1))
    print("latched    %s" % " ".join(n for i, n in enumerate(FX_NAMES) if latched >> i & 1))
    print("VOLUME     page %s, mix %.2f, out %.2f, in %.2f, headphones %.2f%s" % (
        PAGES[d[12]] if d[12] < len(PAGES) else d[12], knob(get14(d[13], d[14])),
        knob(get14(d[15], d[16])), knob(get14(d[17], d[18])), knob(get14(d[19], d[20])),
        ", mono" if flags & 32 else ""))
    print("tempo      %.1f BPM%s%s" % (tempo(d),
                                     ", SHIFT held" if flags & 4 else "",
                                     ", erase waiting" if flags & 8 else ""))
    if len(d) > 23:
        print("page       %s; the mode switch stands %s%s" % (
            "settings" if flags & 64 else "play", "up" if d[23] & 1 else "down",
            {0: "", 2: ", SysEx holds it down", 4: ", SysEx holds it up"}.get(d[23] & 6, "")))
    if len(d) > 25:
        # the source, its effects and live (#177)
        print("input      %s, %s, %s" % ("the mic" if d[25] & 1 else "AUX",
                                         "dry" if d[25] & 2 else "through the effects",
                                         "live" if d[25] & 4 else "not live"))


def load(f, required=True):
    """(max, mean) in % of the block since the last ask, and the blocks that took longer than
    a block (crackles; None from a FRIZZ that doesn't count them); None if unanswered and not
    required"""
    d = f.ask(LOAD, required=required)
    return d and (get14(d[0], d[1]) / 10, get14(d[2], d[3]) / 10,
                  chompi.get7(d[4:7]) if len(d) >= 10 else None)


def show_ident(f):
    got = f.dev(chompi.IDENT)
    if not got or got[0] != chompi.OK:
        sys.exit("no answer to IDENT: a FRIZZ from before it")
    info = chompi.parse_ident(got[1])
    print(chompi.describe_ident(info))
    for path in (os.path.join(HERE, "code/src/build/FRIZZ.bin"),
                 os.path.join(HERE, "code/src/build-bench/FRIZZ-bench.bin")):
        if os.path.isfile(path) and chompi.image_id(path) == (info["crc"], info["length"]):
            print("the build in %s" % os.path.relpath(path))
    print("up %.1f s, MainLoop %d passes; reset: %s (RCC_RSR %08x)" % (
        info["uptime"], info["passes"], chompi.reset_cause(info["reset"]), info["reset"]))
    print("the card: %s" % ("not written until it restarts (RAM only)" if info["ram_only"]
                            else "written as FRIZZ writes it"))


def group_of(name):
    if name in GROUPS:
        return GROUPS[name]
    sys.exit("no group %s: %s" % (name, ", ".join(GROUPS)))


def dev_get(f, group):
    """GET's values for GROUP (a number)"""
    got = f.dev(chompi.GET, [group])
    if not got:
        sys.exit("no answer to GET: a FRIZZ from before it")
    if got[0] != chompi.OK:
        sys.exit("GET %d: %s" % (group, chompi.DEV_STATUS.get(got[0], got[0])))
    d = got[1]
    return [get14(d[2 + 2 * i], d[3 + 2 * i]) for i in range(d[1])]


def show_get(f, name):
    group = group_of(name)
    values = dev_get(f, group)
    if group == GROUP_SETTINGS:
        for n, v in zip(SETTING_NAMES + ["?"] * len(values), values):
            print("%-15s %d" % (n, v))
    elif group == GROUP_LATCH:
        print("latched    %s" % " ".join(n for n, v in zip(FX_NAMES, values) if v))
    elif group == GROUP_VOLUME:
        print("  ".join("%s %.4f" % (n, knob(v)) for n, v in zip(VOLUME_NAMES, values)))
    else:
        print("page 1  %s\npage 2  %s" % tuple("  ".join("%.4f" % knob(v) for v in values[p:p + 4])
                                              for p in (0, 4)))


def dev_set(f, name, index, value):
    group = group_of(name)
    names = {GROUP_SETTINGS: SETTING_NAMES, GROUP_VOLUME: VOLUME_NAMES,
             GROUP_LATCH: FX_NAMES}.get(group, [])
    i = names.index(index) if index in names else int(index)
    if group in (GROUP_SETTINGS, GROUP_LATCH):
        v = {"on": 1, "off": 0}.get(value)
        v = int(value) if v is None else v
    else:
        v = to14(float(value))
    got = f.dev(chompi.SET, [group, i, v >> 7 & 0x7F, v & 0x7F])
    if not got:
        sys.exit("no answer to SET: a FRIZZ from before it")
    if got[0] != chompi.OK:
        sys.exit("SET %s %s %s: %s" % (name, index, value, chompi.DEV_STATUS.get(got[0], got[0])))


def bench_state(f, missing_ok=False):
    """The bench's state (Bench::Report), or exits; None if unanswered and MISSING_OK"""
    got = f.dev(chompi.BENCH, [0])
    if not got and missing_ok:
        return None
    if not got:
        sys.exit("no answer to BENCH: a FRIZZ from before it")
    if got[0] != chompi.OK:
        sys.exit("BENCH: %s (FRIZZ-bench.bin only)" % chompi.DEV_STATUS.get(got[0], got[0]))
    d = got[1]
    return {"running": bool(d[0]), "settling": bool(d[1]), "segment": d[2], "segments": d[3],
            "written": bool(d[4]), "stored": bool(d[5]), "length": chompi.get7(d[6:9])}


def bench_text(f, length):
    """cpu.txt, as the bench sent it to the card, a chunk at a time"""
    text = b""
    while len(text) < length:
        at = len(text)
        got = f.dev(chompi.BENCH, [1, at >> 14 & 0x7F, at >> 7 & 0x7F, at & 0x7F])
        if not got or got[0] != chompi.OK or not got[1]:
            sys.exit("BENCH: no cpu.txt from %d on" % at)
        text += got[1]
    return text.decode("ascii")


BENCH_SILENT = 60  # s the bench may leave unanswered: a segment over 100 % starves MainLoop


def bench(f, wait, out):
    st = bench_state(f)
    said = silent = None
    answered = time.monotonic()
    while wait and (st["running"] or not st["written"]):
        line = "segment %d of %d%s" % (st["segment"] + 1, st["segments"],
                                       ", waiting for the effects to rest" if st["settling"] else "")
        if line != said:
            print(line, flush=True)
            said = line
        time.sleep(2)
        got = bench_state(f, missing_ok=True)
        if got:
            st, answered = got, time.monotonic()
        elif time.monotonic() - answered > BENCH_SILENT:
            sys.exit("the bench hasn't answered for %d s" % BENCH_SILENT)
        elif silent != said:
            print("  no answer: over 100 %, MainLoop starved; waiting", flush=True)
            silent = said
    if st["running"] or not st["written"]:
        print("running: segment %d of %d" % (st["segment"] + 1, st["segments"]))
        return
    print("done; cpu.txt %s the card, %d B" % ("on" if st["stored"] else "NOT on", st["length"]))
    if out:
        os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
        with open(out, "w") as dst:
            dst.write(bench_text(f, st["length"]))
        print("written to", out)


LOCKED = ["none", "trs", "usb"]  # MidiClock::Source, the clock counted
FACTORS = ["x1/2", "x1", "x2"]
WAVES = ["off", "sine", "saw", "noise"]  # dev::Audio::Wave
# LEVELS 1's peaks (dev::Audio), full scale 8192
PEAKS = ["mic", "AUX L", "AUX R", "headphones L", "headphones R", "line L", "line R"]
# RESEED's random numbers and their power-on seeds (FxChaos.h, FxSlicer.h, granularDelay.h:
# xorshift32; FxWarble.h: an LCG)
RANDOMS = [("chaos", 0x6A09E667), ("slicer", 0x2545F491), ("delay", 0x6C8E9CF5), ("warble", 1)]
BATTERY = ["full (on the cable)", "high (above 3.3 V)", "medium (3-3.3 V)", "low (below 3 V)"]
MAX_DRAWS = 1 << 20


def dev_data(f, sub, args, what):
    """kCmdDev's answer data, or exits"""
    got = f.dev(sub, args)
    if not got:
        sys.exit("no answer to %s: a FRIZZ from before it" % what)
    if got[0] != chompi.OK:
        sys.exit("%s: %s" % (what, chompi.DEV_STATUS.get(got[0], got[0])))
    return got[1]


def probe(f):
    g = chompi.get7
    d = dev_data(f, chompi.PROBE, [0], "PROBE")
    flags = d[3]
    print("clock      source %s, counting %s, factor %s%s" % (
        SOURCES[d[0]], LOCKED[d[1]] if flags & 1 else "none", FACTORS[d[2]],
        ", transport followed" if flags & 16 else ""))
    print("MIDI       %.2f BPM (%.2f as sent), %d ticks, locked %d times" % (
        g(d[4:7]) / 100, g(d[7:10]) / 100, g(d[10:15]), g(d[15:18])))
    print("effects    %.2f BPM (%d), %s; pulse %d%s%s" % (
        g(d[18:21]) / 100, g(d[21:23]),
        "the loop's" if flags & 2 else "set by a clock or taps" if flags & 4 else "the default",
        g(d[23:26]), ", of %d round the loop" % g(d[26:29]) if flags & 2 else "",
        ", backwards" if flags & 8 else ""))
    d = dev_data(f, chompi.PROBE, [1], "PROBE")
    length = g(d[2:6])
    print("looper     %s%s%s%s%s" % (
        LOOPER[d[0]] if d[0] < len(LOOPER) else d[0],
        ", closing" if d[1] & 1 else "", ", erase waiting" if d[1] & 2 else "",
        ", erasing" if d[1] & 4 else "", ", can record quantized" if d[1] & 8 else ""))
    print("loop       %d frames (%.3f s), at %d, %d beats, %+d semitones, speed %+.3f (now %+.3f), "
          "rewound %d times" % (length, length / 48000, g(d[6:10]), g(d[10:13]), d[13] - 64,
                                g(d[14:16]) / 1000 - 2, g(d[16:18]) / 1000 - 2, g(d[18:21])))


def dbfs(v):
    return "%6.1f dBFS" % (20 * math.log10(v)) if v > 0 else "    silent"


LEVELS_S = .25  # the peaks levels prints are over the last


def levels(f):
    g = chompi.get7
    dev_data(f, chompi.LEVELS, [1], "LEVELS")  # the peaks from now on (and the meters started)
    time.sleep(LEVELS_S)
    d = dev_data(f, chompi.LEVELS, [1], "LEVELS")
    fx = dev_data(f, chompi.LEVELS, [0], "LEVELS")
    print("effects    %s" % "  ".join("%s %.2f" % (n, g(fx[2 * i:2 * i + 2]) / 16383)
                                      for i, n in enumerate(FX_NAMES)))
    print("VU meter   %.3f; the compressor reducing %.2f dB" % (g(d[1:3]) / 16383, g(d[3:5]) / 100))
    print("peaks over %.2f s:" % LEVELS_S)
    for i, n in enumerate(PEAKS):
        v = g(d[5 + 2 * i:7 + 2 * i]) / 8192
        print("  %-13s %.4f %s" % (n, v, dbfs(v)))


def signal(f, wave, hz, level):
    w = WAVES.index(wave)
    v = max(0, min(16383, int(level * 16383 + .5))) if w else 0
    hz = int(hz) if w else 0
    if w and not 1 <= hz <= 16383:
        sys.exit("signal: 1-16383 Hz")
    dev_data(f, chompi.SIGNAL, [w, hz >> 7 & 0x7F, hz & 0x7F, v >> 7 & 0x7F, v & 0x7F], "SIGNAL")


def draws(start, state, lcg=False):
    """The steps from start to state, None beyond MAX_DRAWS: of xorshift32 (FxCommon.h's Rng),
    or wow & flutter's LCG (FxWarble.h)"""
    x = start & 0x7FFFFFFF if lcg else start or 1
    for n in range(MAX_DRAWS):
        if x == state:
            return n
        if lcg:
            x = (1103515245 * x + 12345) & 0x7FFFFFFF
        else:
            x ^= x << 13 & 0xFFFFFFFF
            x ^= x >> 17
            x ^= x << 5 & 0xFFFFFFFF
    return None


def reseed(f, seed):
    args = [] if seed is None else [seed >> (7 * g) & 0x7F for g in range(4, -1, -1)]
    d = dev_data(f, chompi.RESEED, args, "RESEED")
    seed = chompi.get7(d[0:5])
    print("seed       %d" % seed)
    counts = {}
    for i, (name, own) in enumerate(RANDOMS):
        state = chompi.get7(d[5 + 5 * i:10 + 5 * i])
        n = counts[name] = draws(seed ^ own, state, name == "warble")
        print("%-10s %08x, %s since" % (name, state,
                                        "%d draws" % n if n is not None else "over %d draws" % MAX_DRAWS))
    return counts


def battery(f):
    d = dev_data(f, chompi.BATTERY, [], "BATTERY")
    g = chompi.get7
    bounce = [g(d[4 + 2 * i:6 + 2 * i]) for i in range(4)]
    raw = [g(d[12 + 2 * i:14 + 2 * i]) for i in range(6)]
    print("battery    %s%s" % (BATTERY[d[0]] if d[0] < len(BATTERY) else d[0],
                               "" if d[0] == d[1] else "; the level check last read %s" % BATTERY[d[1]].split(" ")[0]))
    print("charger    cable %s, below the threshold %s, legacy cable %s, input current limited %s "
          "(the last 8 reads, the newest last)" % tuple("{:08b}".format(b) for b in bounce))
    print("           the level check %s%s" % (
        "resting" if d[2] == 0 else "reading at 3.3 V (step %d)" % d[2],
        "; the last read failed" if d[3] else ""))
    print("MP2722     status 0x11-0x16: %s" % " ".join("%02x" % b for b in raw))


def scene_get(f, slot):
    parts = [f.ask(SCENE_GET, [slot, p])[2:] for p in range(SCENE_PARTS)]
    params = {n: [] for n in FX_NAMES}
    for p in range(SCENE_PARTS - 1):  # page 1's parts, then page 2's
        for i in range(FX_PER_PART):
            fx = p % FX_PARTS * FX_PER_PART + i
            if fx >= len(FX_NAMES):
                break
            d = parts[p][8 * i:8 * i + 8]
            params[FX_NAMES[fx]] += [
                round(knob(get14(d[2 * k], d[2 * k + 1])), 6) for k in range(4)]
    last = parts[-1]
    latched = get14(last[1], last[2])
    return {"used": bool(last[0]),
            "latched": [n for i, n in enumerate(FX_NAMES) if latched >> i & 1],
            "params": params}


def scene_put(f, slot, scene):
    params = scene["params"]
    if any(len(v) < 8 for v in params.values()) or any(n not in params for n in FX_NAMES):
        # four knobs each, from before page 2, or an effect missing (the chaos key or the
        # phaser, before they were there): the rest on its defaults, the blank scene's
        blank = scene_get(f, 0)["params"]
        params = {n: list(params.get(n, [])[:8]) + blank[n][len(params.get(n, [])[:8]):]
                  for n in FX_NAMES}
    for p in range(SCENE_PARTS - 1):
        data = []
        for i in range(FX_PER_PART):
            page = p // FX_PARTS
            fx = p % FX_PARTS * FX_PER_PART + i
            knobs = params[FX_NAMES[fx]][4 * page:4 * page + 4] if fx < len(FX_NAMES) else [0] * 4
            for k in knobs:
                v = to14(k)
                data += [v >> 7, v & 0x7F]
        if f.ask(SCENE_PUT, [slot, p] + data)[2] != 0:
            sys.exit("part %d refused" % p)
    latched = sum(1 << FX_NAMES.index(n) for n in scene.get("latched", []))
    if f.ask(SCENE_PUT, [slot, SCENE_PARTS - 1, int(scene.get("used", True)),
                         latched >> 7, latched & 0x7F])[2] != 0:
        sys.exit("refused: slots 1-4 only")


def script_lines(path):
    """(line number, command, its arguments) for each line of a twin script that says
    something: comments and the LED log's lines (|) left out"""
    with open(path) as src:
        for line_no, line in enumerate(src, 1):
            words = line.split("#", 1)[0].split()
            if words and not line.startswith("|"):
                yield line_no, words[0], words[1:]


def note_key(note):
    """The key a MIDI note presses (MidiControl::NoteKey), or None"""
    if CHOMPI_NOTE <= note < CHOMPI_NOTE + 3:
        return ("KEY_26", "KEY_27", "KEY_28")[note - CHOMPI_NOTE]
    if not BASE_NOTE <= note <= BASE_NOTE + 24:
        return None
    octave, semi = divmod(note - BASE_NOTE, 12)
    white = [0, None, 1, None, 2, 3, None, 4, None, 5, None, 6][semi]
    if white is not None:
        return "KEY_%d" % (1 + octave * 7 + white)
    return "KEY_%d" % (16 + octave * 5 + [None, 0, None, 1, None, None, 2, None, 3, None, 4][semi])


def midi_messages(data):
    """A script's midi / usb bytes, one message at a time, running status kept"""
    status, msg = None, []
    for b in data:
        if b >= 0xF8:  # real time, between anything
            yield [b]
        elif b == 0xF0:
            status, msg = 0xF0, [b]
        elif b == 0xF7 and status == 0xF0:
            yield msg + [b]
            status, msg = None, []
        elif b >= 0x80:
            status, msg = b, [b]
            if b >= 0xF4:
                yield msg
        elif status is not None:
            if not msg:
                msg = [status]
            msg.append(b)
            if status != 0xF0 and len(msg) == (2 if status & 0xF0 in (0xC0, 0xD0) else 3):
                yield msg
                msg = []


def card_use(path, up, comp, mode, page=0):
    """Where a script would change what FRIZZ keeps on the card, (line, what, (file, its
    .bak)), or None. Where the device stands when the script starts: up, the settings page
    shown; comp, the compressor's knobs selected; mode, a scene mode (MODES), its slot taken
    as picked; page, VOLUME's (PAGES).

    The settings page: a key or a knob while the mode switch is up, or a setting over SysEx
    (frizz_master.txt, #58). The play page: VOLUME turned on its page 2 without SHIFT, the
    input gain (#152), knobs 1-4 turned with the compressor selected, by
    its key or MIDI's focus (frizz_master.txt), SAVE / COPY / DELETE confirmed with the CHOMPI
    key once a slot is picked, or a scene put over SysEx (frizz_scenes.txt, #78). The hand's
    and SysEx's keys and knobs reach the page the switch shows, MIDI's notes and controllers
    the play page always"""
    by_toggle = up
    picked = mode != 0
    shift = vol_shift = False

    def volume(name, down):
        """VOLUME's page as its press steps it on the play page: on the release of one
        without SHIFT"""
        nonlocal page, shift, vol_shift
        if name == SHIFT_KEY:
            shift = down
        elif name == VOLUME_KEY and down:
            vol_shift = shift
        elif name == VOLUME_KEY and not vol_shift:
            page = (page + 1) % len(PAGES)

    def key(name, what):
        nonlocal comp, mode, picked
        if name == COMP_KEY:
            comp = True
        elif name in FX_KEYS:
            comp = False
        elif name in MODE_KEYS:
            mode = 0 if mode == MODE_KEYS[name] else MODE_KEYS[name]
            picked = False
        elif name in SLOT_KEYS and mode:
            picked = True
        elif name == "KEY_26" and mode and picked:
            return "%s confirmed (%s)" % (MODES[mode].upper(), what), SCENES
        return None

    for line_no, cmd, args in script_lines(path):
        used = None
        if cmd == "toggle":
            up = by_toggle = bool(int(args[0]))
            if up:
                mode = 0  # the settings page shown leaves the scene mode (NormalPage::Draw)
        elif cmd in ("down", "tap", "turn") and up:
            used = "%s %s on the settings page" % (cmd, args[0]), MASTER
        elif cmd in ("down", "tap", "up"):
            volume(args[0], cmd != "up")
            if cmd == "tap":
                volume(args[0], False)
            if cmd != "up":
                used = key(args[0], "%s %s" % (cmd, args[0]))
        elif cmd == "turn" and int(args[0]) == VOLUME_KNOB and page == INPUT_GAIN_PAGE and not shift:
            used = "turn %s on VOLUME's page 2, the input gain" % args[0], MASTER
        elif cmd == "turn" and comp and 1 <= int(args[0]) <= 4:
            used = "turn %s with the compressor selected" % args[0], MASTER
        elif cmd in ("midi", "usb"):
            for m in midi_messages([int(b, 16) for b in args]):
                kind = m[0] & 0xF0
                if m[0] == 0xF0 and bytes(m[1:4]) == midi_send.HEADER[1:] and len(m) > 5:
                    if m[4] == SETTING:
                        used = "a setting over SysEx", MASTER
                    elif m[4] == SCENE_PUT:
                        used = "a scene put over SysEx", SCENES
                    elif m[4] == SWITCH:
                        up = {0: by_toggle, 1: False, 2: True}.get(m[5], up)
                        if up:
                            mode = 0
                    elif m[4] == KEY and up:
                        used = "a SysEx key on the settings page", MASTER
                    elif m[4] == KEY and m[5] < len(SW_NAMES):
                        volume(SW_NAMES[m[5]], m[6:7] != [0])
                        if m[6:7] != [0]:
                            used = key(SW_NAMES[m[5]], "a SysEx key")
                    elif m[4] == TURN and up and m[5] in (4, VOLUME_KNOB):  # SW4: knob 1, the brightness
                        used = "a SysEx turn on the settings page", MASTER
                    elif (m[4] == TURN and m[5] == VOLUME_KNOB and page == INPUT_GAIN_PAGE
                          and not shift):
                        used = "a SysEx turn on VOLUME's page 2, the input gain", MASTER
                    elif m[4] == TURN and not up and comp and 1 <= m[5] <= 4:
                        used = "a SysEx turn with the compressor selected", MASTER
                elif kind == 0x90 and len(m) == 3 and m[2] and note_key(m[1]):
                    used = key(note_key(m[1]), "note %d" % m[1])
                elif kind == 0xC0 and mode:
                    picked = True  # a program change is its scene key, pressed
                elif kind == 0xB0 and len(m) == 3:
                    # MIDI's CCs never reach the card (#97); CC 3 picks a slot in a mode, as
                    # its scene key, and CC 23 what the hand's knobs edit
                    cc, v = m[1], m[2]
                    if cc == MORPH_CC and mode and v < 5:
                        picked = True
                    elif cc == FOCUS_CC and 1 <= v <= COMP_FOCUS:
                        comp = v == COMP_FOCUS
                if used:
                    break
        if used:
            return (line_no,) + used
    return None


def play(f, path, cpu):
    """A twin script on the device; returns how many expectations failed. Whatever ends it
    (an error, Ctrl-C, no answer), the keys it holds are let go and the mode switch it set is
    given back to the hand"""
    start = time.monotonic()
    clock = {"bpm": 0.0}
    stop = threading.Event()
    worst = [0.0, 0.0]
    over = [None]  # the blocks over 100 %, from a FRIZZ that counts them
    readings = {"asked": 0, "missed": 0, "zero": 0}
    failed = 0

    def clock_thread():
        nxt = time.monotonic()
        while not stop.is_set():
            bpm = clock["bpm"]
            if bpm <= 0:
                time.sleep(0.005)
                nxt = time.monotonic()
                continue
            nxt += 60.0 / (bpm * 24)
            time.sleep(max(0.0, nxt - time.monotonic()))
            f.raw([0xF8])

    def cpu_thread():
        # an unanswered ask is counted, not the thread's end: the worst covers the whole run
        while not stop.is_set():
            try:
                got = load(f, required=False)
            except OSError:
                got = None
            readings["asked"] += 1
            if got is None:
                readings["missed"] += 1
            else:
                worst[0], worst[1] = max(worst[0], got[0]), max(worst[1], got[1])
                readings["zero"] += got[:2] == (0.0, 0.0)
                if got[2] is not None:
                    over[0] = (over[0] or 0) + got[2]
            time.sleep(0.25)

    threads = [threading.Thread(target=clock_thread, daemon=True)]
    if cpu:
        # from here on, once FRIZZ's start is over: right after one it answers 0 / 0 (no block
        # yet), and its first blocks cost ~6 points more for ~0.3 s, which the worst would
        # count (#147)
        until = time.monotonic() + 3
        starting = False
        while load(f)[:2] == (0.0, 0.0):
            starting = time.monotonic() < until
            if not starting:
                print("load: still 0 after 3 s (the bench, or FRIZZ starting): "
                      "the worst may count its start")
                break
            time.sleep(0.1)
        if starting:
            time.sleep(0.5)
            load(f)
        start = time.monotonic()
        threads.append(threading.Thread(target=cpu_thread, daemon=True))
    for t in threads:
        t.start()

    def wait_until(t):
        left = start + t / 1000 - time.monotonic()
        if left > 0:
            time.sleep(left)

    skipped = set()
    at_base = 0.0
    toggled = False
    levels = list(LED_PERCENTS)  # the brightnesses that fit every expect led so far
    try:
        for line_no, cmd, args in script_lines(path):
            now_ms = (time.monotonic() - start) * 1000
            if cmd == "wait":
                wait_until(now_ms + float(args[0]))
            elif cmd == "at":
                wait_until(at_base + float(args[0]))
            elif cmd == "booted":
                at_base = now_ms
            elif cmd in ("down", "up"):
                f.key(args[0], cmd == "down")
            elif cmd == "tap":
                f.key(args[0], True)
                time.sleep((float(args[1]) if len(args) > 1 else 60) / 1000)
                f.key(args[0], False)
            elif cmd == "turn":
                detents = int(args[1])
                while detents:
                    step = max(-64, min(63, detents))
                    f.send(TURN, [int(args[0]), step & 0x7F])
                    detents -= step
            elif cmd in ("midi", "usb"):
                f.raw(int(b, 16) for b in args)
            elif cmd == "clock":
                clock["bpm"] = float(args[0])
            elif cmd == "toggle":
                toggled = True
                f.send(SWITCH, [2 if int(args[0]) else 1])
            elif cmd == "leds":
                print("%7d %s" % (now_ms, led_line(*f.leds())))
            elif cmd == "expect" and args[0] == "led":
                pth, smt = f.leds()
                chain, index, want = args[1], int(args[2]), args[3].lower()
                rgb = (pth if chain == "pth" else smt)[index]
                fits = led_levels(want, chain, rgb, levels)
                if fits:
                    levels = fits
                else:
                    got = "%02x%02x%02x" % tuple(min(255, c * (11 if chain == "pth" else 4))
                                                 for c in rgb)
                    print("line %d: %s %d is %s, not %s%s" % (
                        line_no, chain, index, got, want, "".join(
                            ", ~%s at %d %%" % (led_full(want, chain, q), q)
                            for q in levels if q != 100)), file=sys.stderr)
                    failed += 1
            else:
                if cmd not in skipped:
                    print("skipped (twin only): %s" % cmd, file=sys.stderr)
                skipped.add(cmd)
    finally:
        stop.set()
        for t in threads:
            t.join(1)
        try:
            f.release()
            if toggled:
                f.send(SWITCH, [0])  # the real switch again
        except OSError:
            pass  # the device went away: what ended the run says more
    if levels != [100] and len(levels) < len(LED_PERCENTS):
        print("expect led: the LEDs at %s %% brightness (led_brightness on the card)"
              % " or ".join(str(q) for q in levels))
    if cpu:
        last = load(f, required=False)
        if last:
            worst = [max(worst[0], last[0]), max(worst[1], last[1])]
            if last[2] is not None:
                over[0] = (over[0] or 0) + last[2]
        print("load: worst max %.1f%%, worst mean %.1f%% of the block%s" % (
            worst[0], worst[1], "" if over[0] is None else ", %d blocks over 100 %%" % over[0]))
        if readings["missed"]:
            print("load: %d of %d readings unanswered, their time not in the worst"
                  % (readings["missed"], readings["asked"]))
        answered = readings["asked"] - readings["missed"]
        if answered and readings["zero"] == answered:
            print("load: 0 throughout: the bench (FRIZZ-bench.bin) answers as FRIZZ does, but "
                  "doesn't count its load")
    return failed


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    common = chompi.before_and_after(
        ap, (["--device"], dict(help="the raw MIDI node, if not the first CHOMPI")),
        (["--no-start"], dict(action="store_true",
                              help="don't start FRIZZ if the CHOMPI is elsewhere")),
        (["--slot"], dict(type=int, choices=chompi.FRIZZ_SLOTS,
                          help="the FRIZZ slot to play on (10, 12), started unless it runs; "
                          "default the one running")))
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("state", parents=[common])
    sub.add_parser("leds", parents=[common])
    p = sub.add_parser("load", parents=[common])
    p.add_argument("--every", type=float)
    sub.add_parser("settings", parents=[common])
    p = sub.add_parser("channel", parents=[common])
    p.add_argument("channel", type=int, choices=range(17))
    p = sub.add_parser("transport", parents=[common])
    p.add_argument("on", choices=["on", "off"])
    p = sub.add_parser("source", parents=[common])
    p.add_argument("source", choices=SOURCES)
    p = sub.add_parser("out", parents=[common])
    p.add_argument("ports", choices=OUTS)
    p = sub.add_parser("switch", parents=[common])
    p.add_argument("pos", choices=["up", "down", "hand"])
    p = sub.add_parser("scene", parents=[common])
    p.add_argument("action", choices=["get", "put"])
    p.add_argument("slot", type=int, choices=range(5))
    p.add_argument("file", nargs="?")
    p = sub.add_parser("play", parents=[common])
    p.add_argument("script")
    p.add_argument("--cpu", action="store_true")
    p.add_argument("--fresh", action="store_true",
                   help="start the slot again first, so the script plays from power-on as on "
                        "the twin, not after what played before (#123)")
    p.add_argument("--force", action="store_true",
                   help="with --save (or a FRIZZ from before PERSIST): play a script that changes "
                        "the card: the settings page, the compressor's knobs, a scene saved, "
                        "copied or deleted")
    p.add_argument("--save", action="store_true",
                   help="don't keep it in RAM (PERSIST 0): what the script changes goes to the card")
    sub.add_parser("ident", parents=[common])
    sub.add_parser("ram-only", parents=[common])
    p = sub.add_parser("get", parents=[common])
    p.add_argument("group")
    p = sub.add_parser("set", parents=[common])
    p.add_argument("group")
    p.add_argument("index")
    p.add_argument("value")
    p = sub.add_parser("bench", parents=[common])
    p.add_argument("--wait", action="store_true", help="until it's done")
    p.add_argument("--out", help="write its cpu.txt there, once it's done")
    sub.add_parser("probe", parents=[common])
    sub.add_parser("levels", parents=[common])
    p = sub.add_parser("signal", parents=[common])
    p.add_argument("wave", choices=WAVES)
    p.add_argument("hz", type=float, nargs="?", default=0)
    p.add_argument("level", type=float, nargs="?", default=.5, help="0-1 of full scale")
    p = sub.add_parser("reseed", parents=[common])
    p.add_argument("seed", type=int, nargs="?")
    sub.add_parser("battery", parents=[common])
    a = ap.parse_args()

    try:
        run(a)
    except OSError as e:  # the node went away under it, e.g. the CHOMPI restarting
        sys.exit("the CHOMPI went away: %s" % e)


def run(a):
    if a.cmd in ("bench", "ident") and not a.device and not a.slot:
        # whatever runs, the bench too, which answers as FRIZZ does
        a.device = chompi.wait_for("frizz", 30)
    # FRIZZ found or started by the tools, which can start it again (play --fresh, restore);
    # a.device is its node from here on either way
    a.found = not a.device and not a.no_start
    fresh = a.cmd == "play" and a.fresh
    if fresh and not a.found:
        sys.exit("--fresh starts the slot again through the launcher: not with --device or "
                 "--no-start")
    if a.found:
        a.device = chompi.to_frizz(a.slot, again=fresh)
    f = Frizz(a.device)
    if a.cmd == "state":
        show_state(f)
    elif a.cmd == "leds":
        print(led_line(*f.leds()))
    elif a.cmd == "load":
        while True:
            mx, mean, over = load(f)
            print("max %5.1f%%  mean %5.1f%%%s" % (
                mx, mean, "" if over is None else "  over 100 %%: %d blocks" % over), flush=True)
            if not a.every:
                break
            time.sleep(a.every)
    elif a.cmd == "settings":
        d = f.ask(SETTINGS)
        line = "channel %s, transport %s" % (d[0] or "all", "on" if d[1] else "off")
        if len(d) > 2:  # FRIZZ from before the clock source answers two
            line += ", clock %s" % (SOURCES[d[2]] if d[2] < len(SOURCES) else d[2])
        if len(d) > 3:  # and from before MIDI out three
            line += ", out %s" % (OUTS[d[3]] if d[3] < len(OUTS) else d[3])
        print(line)
    elif a.cmd == "channel":
        f.send(SETTING, [0, a.channel])
    elif a.cmd == "transport":
        f.send(SETTING, [1, 1 if a.on == "on" else 0])
    elif a.cmd == "source":
        f.send(SETTING, [2, SOURCES.index(a.source)])
    elif a.cmd == "out":
        f.send(SETTING, [3, OUTS.index(a.ports)])
    elif a.cmd == "switch":
        f.send(SWITCH, [{"hand": 0, "down": 1, "up": 2}[a.pos]])
    elif a.cmd == "scene" and a.action == "get":
        text = json.dumps(scene_get(f, a.slot), indent=1)
        if a.file:
            with open(a.file, "w") as out:
                out.write(text + "\n")
        else:
            print(text)
    elif a.cmd == "scene":
        if not a.file:
            sys.exit("scene put SLOT FILE")
        with open(a.file) as src:
            scene_put(f, a.slot, json.load(src))
    elif a.cmd == "ident":
        show_ident(f)
    elif a.cmd == "ram-only":
        if not f.ram_only():
            sys.exit("no answer to PERSIST: a FRIZZ from before it")
        print("FRIZZ writes nothing to the card until it restarts")
    elif a.cmd == "get":
        show_get(f, a.group)
    elif a.cmd == "set":
        dev_set(f, a.group, a.index, a.value)
    elif a.cmd == "bench":
        bench(f, a.wait, a.out)
    elif a.cmd == "probe":
        probe(f)
    elif a.cmd == "levels":
        levels(f)
    elif a.cmd == "signal":
        signal(f, a.wave, a.hz, a.level)
    elif a.cmd == "reseed":
        if a.seed is not None and not 0 <= a.seed < 1 << 32:
            sys.exit("reseed: a seed of 32 bits")
        reseed(f, a.seed)
    elif a.cmd == "battery":
        battery(f)
    elif a.cmd == "play":
        ram = None if a.save else f.ram_only()
        if ram:
            print("the card isn't written until FRIZZ restarts (PERSIST 0): what the script "
                  "changes stays in RAM")
        elif not a.save:
            print("a FRIZZ from before PERSIST: the card is guarded by refusing", file=sys.stderr)
        if not a.force and not ram:
            # where the device stands: the page it shows (STATE's flags, bit 6), where the
            # script's keys land until it toggles; what the knobs edit; the scene mode
            d = f.state()
            used = card_use(a.script, bool(d[6] & 64), d[4] == len(FX_NAMES), d[7], d[12])
            if used:
                line_no, what, (name, bak) = used
                sys.exit("line %d: %s. FRIZZ would save it to /FRIZZ/%s, on the card the FRIZZ on "
                         "key 10 shares, %s (#58, #78): not played. --force plays it anyway"
                         % (line_no, what, name, bak))
        try:
            failed = play(f, a.script, a.cpu)
        except KeyboardInterrupt:
            failed = "interrupted: the keys let go, the mode switch back with the hand"
        finally:
            if ram:
                restore(f, a)
        if failed and not isinstance(failed, str) and a.found and not fresh:
            print("expect led judged after what played since FRIZZ started: --fresh plays the "
                  "script from power-on (#123)", file=sys.stderr)
        sys.exit(failed if isinstance(failed, str) else 1 if failed else 0)


def restore(f, a):
    """After a script played RAM only: FRIZZ on a slot other than the test slot is started
    again, from the card, so it saves again"""
    slot = chompi.frizz_slot
    if slot == chompi.TEST_SLOT:
        return
    if not a.found or slot is None:
        print("FRIZZ%s keeps what it changes in RAM until it restarts: ./flash.py --run N"
              % ("" if slot is None else " on slot %d" % slot), file=sys.stderr)
        return
    os.close(f.link.fd)
    print("starting slot %d again, from the card, so it saves again" % slot, file=sys.stderr)
    chompi.start(slot)


if __name__ == "__main__":
    main()
