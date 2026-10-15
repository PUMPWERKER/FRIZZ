#!/usr/bin/env python3
"""loopxfer.py: the loop transfer's reference (docs/LOOP_PROTOCOL.md, section 8.1): FRIZZ's loop
banks over USB MIDI, as the Companion page (frizz.friedrichwagner.eu/companion/) speaks it.

    tools/loopxfer.py vectors [--out FILE]   writes test/loopxfer/vectors.json, the contract's
                                             test: scripted sessions, the bytes each side sends
    tools/loopxfer.py vectors --check        whether the file is what it would write (the unit
                                             check loopxfer runs it)
    tools/loopxfer.py --selftest [--batches N] [--seed S]
                                             the client against the model over a lossy link

It holds:
  - the codec: N(k), the packed bytes, every request and answer, built and parsed;
  - a model FRIZZ (Frizz): the protocol's state machine over a card in a dict, pass by pass as
    the firmware runs it (code/src/LoopTransfer.h, with LoopLoader.h's scan and header walk),
    so the vectors it writes replay byte for byte against the firmware (test/loopxfer.cpp) and
    against frizz-web's simulator;
  - the client (Client): list, write (go-back-N, 3.7), rename, swap, delete, copy, rescan, over
    any link with send(bytes) and recv(timeout): the model (ModelLink), the twin's pty or the
    CHOMPI's USB node (RawLink; remote.py loops).

The model's pass is one of MainLoop's (2 ms of its clock): the loader's step, then the
transfer's (a request taken and answered, the job's step), then what it sends: an ack, then an
answer. Python 3 without packages, like remote.py.
"""
import argparse
import binascii
import json
import os
import random
import select
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.dirname(HERE)
VECTORS = os.path.join(FIRMWARE, "test", "loopxfer", "vectors.json")

# ---- the protocol's numbers (sections 1-5) ----
HEADER = bytes([0x7D, 0x43, 0x48])
CMD = 0x38
REPLY = CMD | 0x40
MAJOR, MINOR = 1, 0

HELLO, STATUS, BANK, SLOT, NAME = 0x00, 0x01, 0x02, 0x03, 0x04
WRITE_BEGIN, CHUNK, WRITE_END, ABORT = 0x10, 0x11, 0x12, 0x13
RENAME, SWAP, DELETE, COPY, RESCAN = 0x20, 0x21, 0x22, 0x23, 0x30
ARGS = {HELLO: 0, STATUS: 0, RESCAN: 0, BANK: 1, ABORT: 1, SLOT: 2, NAME: 2, DELETE: 2, SWAP: 4,
        RENAME: 5, COPY: 5, WRITE_END: 6, WRITE_BEGIN: 7}
CHANGING = {WRITE_BEGIN, WRITE_END, ABORT, RENAME, SWAP, DELETE, COPY, RESCAN}

STATUSES = ["OK", "BAD", "NOT_HERE", "BUSY_RECORDING", "BUSY_LOADING", "BUSY_SCANNING", "BUSY_CARD",
            "BUSY_JOB", "CARD_LENT", "NO_CARD", "NOT_A_SLOT", "EMPTY", "EXISTS", "NO_SPACE", "TOO_LONG",
            "BAD_WAV", "BAD_CRC", "INCOMPLETE", "NO_TRANSFER", "FRAGMENTED", "CARD_ERROR", "TIMED_OUT",
            "ABORTED"]
for _i, _n in enumerate(STATUSES):
    globals()[_n] = _i
PASSING = range(BUSY_RECORDING, NO_CARD + 1)  # noqa: F821 (from STATUSES)
WORDS = {BUSY_RECORDING: "FRIZZ is recording a loop", BUSY_LOADING: "FRIZZ is loading a loop",  # noqa: F821
         BUSY_SCANNING: "FRIZZ is reading the card", BUSY_CARD: "FRIZZ is writing the card",  # noqa: F821
         BUSY_JOB: "another transfer is running", CARD_LENT: "the card is lent to the computer",  # noqa: F821
         NO_CARD: "no card"}  # noqa: F821

# the loader's results (5.1, LoopLoader.h's Result)
RESULTS = ["empty", "mono", "stereo", "not RIFF/WAVE", "no fmt chunk before the data", "not PCM",
           "not 16-bit", "not 48 kHz", "not mono or stereo", "no data chunk near the start",
           "goes past the file's end", "longer than 16 bars at 50 BPM", "unreadable",
           "in too many fragments", "card error", "timeout", "card lost",
           "its clusters end before its data", "write failed", "write bad"]
EMPTY_R, MONO, STEREO, NOT_WAV, BAD_FMT, NOT_PCM, NOT_16BIT, NOT_48K, CHANNELS, NO_DATA, PAST_END, \
    TOO_LONG_R, UNREADABLE = range(13)

JOB_NONE, JOB_WRITE, JOB_COPY = 0, 1, 2
IDLE, RUNNING, PAUSED, FINISHING, DONE, FAILED = range(6)

MAX_FRAMES = 3686400
MAX_FILE = MAX_FRAMES * 4 + 8192
MAX_CHUNK, WINDOW, MAX_REQUEST, NAME_MAX = 98, 16, 128, 28
FRIZZ_BANKS, CUBBI_BANKS, SLOTS = 8, 5, 15
BANKS = FRIZZ_BANKS + CUBBI_BANKS
REPLACE = 1
BLOCK = 32768
SPARE = 65536
TIMEOUT_MS = 30000
ACK_EVERY_MS = 250
MAX_FRAGMENTS = 255
MAP_CLUSTERS = 256
DIR_ENTRIES = 24
TMP = "/FRIZZ/xfer.tmp"
PASS_MS = 2

# FatFs's numbers (ff.h): CARD_ERROR's DETAIL
FR_OK, FR_INT_ERR, FR_NOT_READY, FR_NO_FILE, FR_NO_PATH, FR_INVALID_OBJECT = 0, 2, 3, 4, 5, 9


# ---- the codec (1.4) ----

def put7(v, groups):
    """N(k): k 7-bit groups, the highest first, saturated at the most they hold"""
    top = (1 << (7 * groups)) - 1 if groups < 5 else 0xFFFFFFFF
    v = min(v, top)
    return [(v >> (7 * g)) & 0x7F if 7 * g < 32 else 0 for g in reversed(range(groups))]


def get7(data):
    v = 0
    for b in data:
        v = (v << 7 | b) & 0xFFFFFFFF
    return v


def pack(data):
    out = []
    for i in range(0, len(data), 7):
        group = data[i:i + 7]
        out.append(sum(((b >> 7) & 1) << j for j, b in enumerate(group)))
        out.extend(b & 0x7F for b in group)
    return out


def unpack(packed, n):
    out = bytearray()
    i = 0
    while len(out) < n:
        top = packed[i]
        i += 1
        for j in range(min(7, n - len(out))):
            out.append(packed[i] | ((top >> j) & 1) << 7)
            i += 1
    return bytes(out)


def sysex(cmd, body):
    return bytes([0xF0]) + HEADER + bytes([cmd]) + bytes(body) + bytes([0xF7])


def request(op, seq, args=()):
    return sysex(CMD, [op, seq] + list(args))


def chunk(tid, offset, data):
    return sysex(CMD, [CHUNK, tid] + put7(offset, 4) + [len(data)] + pack(data))


def hello(seq): return request(HELLO, seq)
def status(seq): return request(STATUS, seq)
def bank(seq, b): return request(BANK, seq, [b])
def slot(seq, b, s): return request(SLOT, seq, [b, s])
def name(seq, b, s): return request(NAME, seq, [b, s])
def write_begin(seq, b, s, flags, size): return request(WRITE_BEGIN, seq, [b, s, flags] + put7(size, 4))
def write_end(seq, tid, crc): return request(WRITE_END, seq, [tid] + put7(crc, 5))
def abort(seq, tid): return request(ABORT, seq, [tid])
def rename(seq, b, s, tb, ts, flags=0): return request(RENAME, seq, [b, s, tb, ts, flags])
def swap(seq, b, s, tb, ts): return request(SWAP, seq, [b, s, tb, ts])
def delete(seq, b, s): return request(DELETE, seq, [b, s])
def copy(seq, cb, s, tb, ts, flags=0): return request(COPY, seq, [cb, s, tb, ts, flags])
def rescan(seq): return request(RESCAN, seq)


class Answer:
    """FRIZZ's message: an answer (op, seq, status, data, detail) or a write's ack (ack True:
    status, tid, next, drops)"""

    def __init__(self, msg):
        body = msg[5:-1]
        self.raw = msg
        self.op, self.seq, self.status = body[0], body[1], body[2]
        self.data = bytes(body[3:])
        self.ack = self.op == CHUNK and self.seq == 0
        self.detail = self.data[0] if self.status and not self.ack and self.data else 0
        if self.ack:
            self.tid, self.next, self.drops = self.data[0], get7(self.data[1:5]), self.data[5]

    def __repr__(self):
        if self.ack:
            return "ack %s tid %d next %d drops %d" % (STATUSES[self.status], self.tid, self.next, self.drops)
        return "%02X seq %d %s %s" % (self.op, self.seq, status_name(self.status), self.data.hex(" "))


def parse(msg):
    """An Answer for FRIZZ's 0x78 message, else None"""
    if len(msg) < 9 or msg[0] != 0xF0 or msg[-1] != 0xF7 or msg[1:4] != HEADER or msg[4] != REPLY:
        return None
    return Answer(msg)


def status_name(st):
    return STATUSES[st] if st < len(STATUSES) else "status %d" % st


def hexes(b):
    return " ".join("%02X" % x for x in b)


# ---- WAV (5.2, 5.3) ----

def le32(v): return v.to_bytes(4, "little")
def le16(v): return v.to_bytes(2, "little")


def riff_chunk(cid, body):
    return cid + le32(len(body)) + body + (b"\0" if len(body) & 1 else b"")


def info(name_bytes):
    """LIST/INFO with an INAM of name_bytes and its NUL (5.3)"""
    return riff_chunk(b"LIST", b"INFO" + riff_chunk(b"INAM", name_bytes + b"\0"))


def acid(beats, seconds):
    body = le32(0) + le16(60) + le16(0x8000) + b"\0\0\0\0" + le32(beats) + le16(4) + le16(4)
    import struct
    body += struct.pack("<f", beats * 60.0 / seconds if seconds else 0.0)
    return riff_chunk(b"acid", body)


def wav(pcm, channels=2, beats=None, name_text=None, rate=48000):
    """The app's WAV (5.2): fmt, acid when beats are known, LIST/INFO/INAM with a name, data"""
    fmt = le16(1) + le16(channels) + le32(rate) + le32(rate * channels * 2) + le16(2 * channels) + le16(16)
    body = b"WAVE" + riff_chunk(b"fmt ", fmt)
    frames = len(pcm) // (2 * channels)
    if beats:
        body += acid(beats, frames / rate)
    if name_text:
        body += info(cut_name(name_text.encode("utf-8")))
    body += riff_chunk(b"data", pcm)
    return b"RIFF" + le32(len(body)) + body


def cut_name(b):
    """At most NAME_MAX bytes, cut at a code point's boundary"""
    if len(b) <= NAME_MAX:
        return b
    k = NAME_MAX
    while k and (b[k] & 0xC0) == 0x80:
        k -= 1
    return b[:k]


def pattern(n, seed):
    x = (seed * 2654435761 + 1) & 0xFFFFFFFF
    out = bytearray(n)
    for i in range(n):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        out[i] = x >> 24
    return bytes(out)


class Walk:
    """LoopLoader.h's WavWalk over a file's bytes in windows of 1 KB, 8 at most, as Open reads
    them: result, frames, beats, name, name_cut"""
    MORE = -1

    def __init__(self, data):
        self.size, self.pos, self.fmt, self.beats = len(data), 0, False, 0
        self.name, self.name_cut, self.frames = b"", False, 0
        self.tag = self.channels = self.bits = self.rate = 0
        r = self.MORE
        for _ in range(8):
            if r != self.MORE:
                break
            at = self.pos & ~511
            win = data[at:at + 1024]
            r = self.step(win, at, len(win))
        self.result = NO_DATA if r == self.MORE else r

    def step(self, win, at, n):
        def has(p, ln):
            return p >= at and p - at <= n and ln <= n - (p - at)
        if self.pos == 0:
            if self.size < 12:
                return NOT_WAV
            if not has(0, 12):
                return self.MORE
            if win[0:4] != b"RIFF" or win[8:12] != b"WAVE":
                return NOT_WAV
            self.pos = 12
        while True:
            if self.size - self.pos < 8:
                return NO_DATA if self.fmt else BAD_FMT
            if not has(self.pos, 8):
                return self.MORE
            c = self.pos - at
            cid, ln = win[c:c + 4], int.from_bytes(win[c + 4:c + 8], "little")
            room = self.size - self.pos - 8
            if cid == b"data" and not self.fmt:
                return BAD_FMT
            if ln > room:
                return PAST_END
            if cid == b"data":
                return self.check(ln)
            if cid == b"fmt ":
                if ln < 16:
                    return BAD_FMT
                need = 16 if ln < 26 else 26
                if not has(self.pos + 8, need):
                    return self.MORE
                f = win[c + 8:]
                self.tag = int.from_bytes(f[0:2], "little")
                self.channels = int.from_bytes(f[2:4], "little")
                self.rate = int.from_bytes(f[4:8], "little")
                self.bits = int.from_bytes(f[14:16], "little")
                if self.tag == 0xFFFE and ln >= 40:
                    self.tag = int.from_bytes(f[24:26], "little")
                self.fmt = True
            if cid == b"acid" and ln >= 16:
                if not has(self.pos + 8, 16):
                    return self.MORE
                beats = int.from_bytes(win[c + 20:c + 24], "little")
                self.beats = beats if beats <= 1024 else 0
            if cid == b"LIST" and ln >= 4 and has(self.pos + 8, ln) and win[c + 8:c + 12] == b"INFO":
                self.read_name(win[c + 12:c + 8 + ln])
            self.pos += 8 + ln
            if ln & 1:
                if self.pos == self.size:
                    return NO_DATA if self.fmt else BAD_FMT
                self.pos += 1

    def read_name(self, p):
        while len(p) >= 8:
            ln = int.from_bytes(p[4:8], "little")
            if ln > len(p) - 8:
                return
            if p[0:4] == b"INAM":
                body = p[8:8 + ln]
                k = body.index(0) if 0 in body else ln
                self.name_cut = k > NAME_MAX
                if self.name_cut:
                    k = NAME_MAX
                    while k and (body[k] & 0xC0) == 0x80:
                        k -= 1
                self.name = bytes(body[:k])
                return
            step = 8 + ln + (ln & 1)
            if step >= len(p):
                return
            p = p[step:]

    def check(self, ln):
        if self.tag != 1:
            return NOT_PCM
        if self.bits != 16:
            return NOT_16BIT
        if self.rate != 48000:
            return NOT_48K
        if self.channels not in (1, 2):
            return CHANNELS
        self.frames = ln // (2 * self.channels)
        if self.frames == 0:
            return NO_DATA
        if self.frames > MAX_FRAMES:
            return TOO_LONG_R
        return MONO if self.channels == 1 else STEREO


def slot_name(b, s):
    return "%c%d" % (ord("a") + b, s + 1)


def slot_of(fname, cubbi):
    """LoopLoader.h's SlotOf: (bank, slot) for a1.wav ... h15.wav or cubbi_a1.wav ...
    cubbi_e15.wav in any case, else None"""
    n = fname.lower()
    if cubbi:
        if not n.startswith("cubbi_"):
            return None
        n = n[6:]
    banks = CUBBI_BANKS if cubbi else FRIZZ_BANKS
    if not n or not "a" <= n[0] < chr(ord("a") + banks) or not n.endswith(".wav"):
        return None
    num = n[1:-4]
    if not num.isdigit() or num[0] == "0" or len(num) > 2 or not 1 <= int(num) <= SLOTS:
        return None
    return ord(n[0]) - ord("a") + (FRIZZ_BANKS if cubbi else 0), int(num) - 1


def slot_at(n):
    """LoopLoader.h's SlotAt: (id, rest) for a FRIZZ slot's name at n's start, else None"""
    if len(n) < 2 or not "a" <= n[0].lower() <= "h" or not "1" <= n[1] <= "9":
        return None
    s, rest = int(n[1]), n[2:]
    if rest[:1].isdigit():
        s, rest = s * 10 + int(rest[0]), rest[1:]
    return ((ord(n[0].lower()) - ord("a")) * SLOTS + s - 1, rest) if s <= SLOTS else None


def swap_of(fname):
    """(from, to) ids for a swap's temp file "a1-b2.swp", else None"""
    a = slot_at(fname)
    if not a or not a[1].startswith("-"):
        return None
    b = slot_at(a[1][1:])
    if not b or b[1].lower() != ".swp" or b[0] == a[0]:
        return None
    return a[0], b[0]


def slot_path(b, s, tape):
    if b < FRIZZ_BANKS:
        return "/FRIZZ/%s.wav" % slot_name(b, s)
    return "%scubbi_%s.wav" % ("/TAPE/" if tape else "/", slot_name(b - FRIZZ_BANKS, s))


def swap_path(a, b):
    return "/FRIZZ/%s-%s.swp" % (slot_name(a // SLOTS, a % SLOTS), slot_name(b // SLOTS, b % SLOTS))


# ---- the model card (test/host/fatfs.h's, which the firmware's unit check runs on) ----

class Fil:
    def __init__(self, path, write, mount):
        self.path, self.write, self.mount, self.pos = path, write, mount, 0


class Card:
    """A card in a dict: path -> bytes, names found in any case. cluster bytes a cluster,
    clusters of them; fsinfo: FSINFO has the free count; frag_clusters: a file's clusters come
    in fragments of that many. Name changes are counted, and a power cut after the cut-th"""

    def __init__(self, files=None, dirs=(), cluster=4096, clusters=1 << 20, fsinfo=True,
                 frag_clusters=1 << 30, space=None):
        self.files = {p: bytes(d) for p, d in (files or {}).items()}
        self.dirs = set(dirs)
        self.cluster, self.clusters, self.fsinfo = cluster, clusters, fsinfo
        self.frag_clusters = frag_clusters
        self.space = space  # bytes writes can still add, None: no limit
        self.present = True
        self.mount_id = 0
        self.synced = {}
        self.changes = 0
        self.cut_after = None
        self.free_clst = None  # FatFs's count, None: not known
        self.fs_type = 0

    def config(self):
        return dict(cluster=self.cluster, clusters=self.clusters, fsinfo=self.fsinfo,
                    frag_clusters=self.frag_clusters)

    def find(self, path):
        if path in self.files:
            return path
        for p in self.files:
            if p.lower() == path.lower():
                return p
        return path

    def has_dir(self, d):
        if d in self.dirs:
            return True
        return any(x.startswith(d + "/") for x in self.dirs) or any(p.startswith(d + "/") for p in self.files)

    def used(self):
        return sum((len(d) + self.cluster - 1) // self.cluster for d in self.files.values())

    def free(self):
        return max(0, self.clusters - self.used())

    def keep_free(self):
        if self.free_clst is not None:
            self.free_clst = self.free()

    def changed(self):
        self.changes += 1
        if self.changes == self.cut_after:
            self.cut()

    def cut(self):
        for p, n in self.synced.items():
            if p in self.files:
                self.files[p] = self.files[p][:n]
        self.synced = {}
        self.present = False
        self.cut_after = None

    # FatFs's calls, as the fake's
    def mount(self, unmount=False):
        for p, n in self.synced.items():
            if p in self.files:
                self.files[p] = self.files[p][:n]
        self.synced = {}
        self.mount_id += 1
        if unmount:
            self.fs_type = 0
            return FR_OK
        self.fs_type = 3 if self.present else 0
        self.free_clst = self.free() if self.fsinfo else None
        return FR_OK if self.present else FR_NOT_READY

    def open(self, path, mode):
        """mode: 'r', 'w' (an existing one) or 'c' (created anew); (FRESULT, Fil)"""
        if not self.present:
            return FR_NOT_READY, None
        p = self.find(path)
        if mode == "c":
            self.files[p] = b""
            self.synced[p] = 0
            self.keep_free()
            self.changed()
            return FR_OK, Fil(p, True, self.mount_id)
        if p not in self.files:
            return FR_NO_FILE, None
        if mode == "w":
            self.synced[p] = len(self.files[p])
        return FR_OK, Fil(p, mode == "w", self.mount_id)

    def valid(self, f):
        if not self.present:
            return FR_NOT_READY
        return FR_OK if f.mount == self.mount_id else FR_INVALID_OBJECT

    def write(self, f, data):
        r = self.valid(f)
        if r:
            return r, 0
        n = len(data) if self.space is None else min(len(data), self.space)
        if self.space is not None:
            self.space -= n
        d = self.files[f.path]
        if len(d) < f.pos:
            d += b"\0" * (f.pos - len(d))
        self.files[f.path] = d[:f.pos] + data[:n] + d[f.pos + n:]
        f.pos += n
        self.keep_free()
        return FR_OK, n

    def read(self, f, n):
        r = self.valid(f)
        if r:
            return r, b""
        d = self.files[f.path][f.pos:f.pos + n]
        f.pos += len(d)
        return FR_OK, d

    def sync(self, f):
        r = self.valid(f)
        if r == FR_OK and f.write and f.path in self.synced:
            self.synced[f.path] = len(self.files[f.path])
        return r

    def close(self, f):
        if f.mount != self.mount_id:
            return FR_INVALID_OBJECT
        if f.write:
            self.synced.pop(f.path, None)
        return FR_OK if self.present else FR_NOT_READY

    def lseek(self, f, pos):
        r = self.valid(f)
        if r == FR_OK:
            f.pos = min(pos, len(self.files[f.path]))
        return r

    def size(self, f):
        return len(self.files.get(f.path, b""))

    def stat(self, path):
        if not self.present:
            return FR_NOT_READY
        return FR_OK if self.find(path) in self.files else FR_NO_FILE

    def unlink(self, path):
        if not self.present:
            return FR_NOT_READY
        p = self.find(path)
        if p not in self.files:
            return FR_NO_FILE
        del self.files[p]
        self.keep_free()
        self.changed()
        return FR_OK

    def rename(self, src, dst):
        if not self.present:
            return FR_NOT_READY
        s = self.find(src)
        if s not in self.files:
            return FR_NO_FILE
        if self.find(dst) in self.files:
            return 8  # FR_EXIST
        self.files[dst] = self.files.pop(s)
        self.changed()
        return FR_OK

    def mkdir(self, path):
        if not self.present:
            return 7  # FR_DENIED
        if self.has_dir(path):
            return 8
        self.dirs.add(path)
        self.changed()
        return FR_OK

    def listdir(self, d):
        """f_opendir's snapshot: (FRESULT, [(name, is_folder)]): its files by path, then its
        folders by name"""
        if not self.present:
            return FR_NOT_READY, None
        if d == "/":
            d = ""
        if d and not self.has_dir(d):
            return FR_NO_PATH, None
        names, folders = [], set()
        for p in sorted(self.files):
            if len(p) <= len(d) + 1 or not p.startswith(d + "/"):
                continue
            rest = p[len(d) + 1:]
            if "/" in rest:
                folders.add(rest.split("/")[0])
            else:
                names.append((rest, False))
        for m in self.dirs:
            if len(m) > len(d) + 1 and m.startswith(d + "/"):
                folders.add(m[len(d) + 1:].split("/")[0])
        return FR_OK, names + [(f, True) for f in sorted(folders)]

    def fragments(self, size):
        clusters = (size + self.cluster - 1) // self.cluster
        return (clusters + self.frag_clusters - 1) // self.frag_clusters


# ---- the model FRIZZ: LoopLoader.h's scan and LoopTransfer.h, pass by pass ----

def empty_slot():
    return dict(result=EMPTY_R, frames=0, beats=0, size=0, name=b"", cut=False)


class Loader:
    """LoopLoader.h's scan (its passes, Repair) and Walk; the rest of the loader isn't modelled"""

    def __init__(self, card):
        self.card = card
        self.slots = [[empty_slot() for _ in range(SLOTS)] for _ in range(BANKS)]
        self.scanning = False
        self.tape = False
        self.found = [0] * BANKS
        self.swaps = []

    def request_scan(self):
        if self.scanning:
            return False
        self.scanning = True
        self.group, self.listed, self.dir, self.entries = 0, False, None, 0
        return True

    def process(self, card_wanted):
        if self.scanning and not card_wanted:
            self.scan_step()

    def walk(self, path):
        p = self.card.find(path)
        if not self.card.present or p not in self.card.files:
            return empty_slot() | dict(result=UNREADABLE)
        data = self.card.files[p]
        w = Walk(data)
        frames = w.frames if w.result in (MONO, STEREO, TOO_LONG_R) else 0
        return dict(result=w.result, frames=frames, beats=w.beats, size=len(data), name=w.name, cut=w.name_cut)

    def scan_step(self):
        if self.group == 2:
            self.scanning = False
            return
        cubbi = self.group == 1
        first, last = (FRIZZ_BANKS, BANKS) if cubbi else (0, FRIZZ_BANKS)
        if not self.listed:
            self.list_step(cubbi, first, last)
            return
        b = next((b for b in range(first, last) if self.found[b]), None)
        if b is None:
            self.group += 1
            self.listed, self.entries = False, 0
            return
        s = (self.found[b] & -self.found[b]).bit_length() - 1
        self.found[b] &= self.found[b] - 1
        self.slots[b][s] = self.walk(slot_path(b, s, self.tape))

    def list_step(self, cubbi, first, last):
        if self.dir is None:
            if not self.entries:
                for b in range(first, last):
                    self.found[b] = 0
                    self.slots[b] = [empty_slot() for _ in range(SLOTS)]
                if cubbi:
                    r, listing = self.card.listdir("/TAPE")
                    self.tape = r == FR_OK
                    opened = listing
            path = "/TAPE" if cubbi and self.tape else "/" if cubbi else "/FRIZZ"
            if not cubbi or self.entries or not self.tape:
                r, opened = self.card.listdir(path)
                if r:
                    self.listed = True
                    return
            self.dir, self.dir_mount, self.dir_at = opened, self.card.mount_id, self.entries
        for _ in range(DIR_ENTRIES):
            if self.dir_mount != self.card.mount_id:
                self.dir = None
                return
            if self.dir_at >= len(self.dir):
                self.dir = None
                self.listed = True
                if not cubbi:
                    self.repair()
                return
            fname, folder = self.dir[self.dir_at]
            self.dir_at += 1
            self.entries += 1
            if folder:
                continue
            at = slot_of(fname, cubbi)
            if at:
                self.found[at[0]] |= 1 << at[1]
            elif not cubbi and len(self.swaps) < 4:
                sw = swap_of(fname)
                if sw:
                    self.swaps.append(sw)

    def repair(self):
        for a, b in self.swaps:
            for i in (b, a):
                bit = 1 << (i % SLOTS)
                if self.found[i // SLOTS] & bit:
                    continue
                if self.card.rename(swap_path(a, b), slot_path(i // SLOTS, i % SLOTS, False)) == FR_OK:
                    self.found[i // SLOTS] |= bit
                break
        self.swaps = []


class Intake:
    """LoopTransfer.h's Intake: the audio callback's half"""

    def __init__(self):
        self.pending = None
        self.open = False
        self.paused = False
        self.tid = self.size = self.taken = self.drops = 0
        self.ring = []
        self.repeat = self.heard = self.stale = False
        self.stale_tid = 0

    def take(self, d):
        if len(d) < 2:
            return
        if d[0] == CHUNK:
            self.chunk(d[1:])
            return
        if self.pending is not None:
            return
        self.pending = (d[0], d[1], bytes(d[2:]))

    def chunk(self, d):
        if len(d) < 6:
            return
        if not self.open or d[0] != self.tid:
            self.stale_tid, self.stale = d[0], True
            return
        self.heard = True
        at, ln = get7(d[1:5]), d[5]
        if at < self.taken:
            self.repeat = True
            return
        if (at > self.taken or self.paused or not ln or ln > MAX_CHUNK or len(d) - 6 != ln + (ln + 6) // 7
                or at + ln > self.size or len(self.ring) >= WINDOW):
            self.drops = (self.drops + 1) & 0x7F
            return
        self.ring.append(unpack(d[6:], ln))
        self.taken += ln

    def open_write(self, tid, size):
        self.tid, self.size, self.taken, self.drops = tid, size, 0, 0
        self.ring = []
        self.paused = self.repeat = self.heard = False
        self.open = True


class Transfer:
    """LoopTransfer.h, statement for statement where the bytes depend on it"""

    def __init__(self, card, loader, intake, clock):
        self.card, self.loader, self.intake, self.clock = card, loader, intake, clock
        self.env = {}
        self.ans = None
        self.remembered = None
        self.job, self.phase, self.tid, self.tid_seq = JOB_NONE, IDLE, 0, 0
        self.bank = self.slot = self.src_bank = self.src_slot = self.result = self.detail = 0
        self.replace = False
        self.total = self.next = self.written = 0
        self.staged = b""
        self.crc = 0
        self.step = 0
        self.file = self.src = None
        self.walked = None
        self.clusters = self.cluster = 0
        self.unlink = False
        self.pause = self.end_ack = self.acked_drops = 0
        self.pause_ms = self.heard_ms = self.stale_ms = self.acked_next = 0
        self.resumed = self.stale_sent = False
        self.count_left = None  # FAT sectors still to count, None: not counting
        self.changed = set()

    @staticmethod
    def boot(card):
        card.unlink(TMP)

    def open_(self):
        return self.phase in (RUNNING, PAUSED, FINISHING)

    def free_known(self):
        return self.card.free_clst is not None

    def scanning(self):
        e = self.env
        return self.loader.scanning or (e["card"] and not e["lent"] and self.card.fs_type and not self.free_known())

    def filled(self, b, s):
        return self.loader.slots[b][s]["result"] != EMPTY_R

    def process(self, e):
        now = self.clock()
        self.env = e
        if self.open_() and not e["usb"]:
            self.end(ABORTED, 0, False)
        if self.phase in (RUNNING, PAUSED):
            why = BUSY_RECORDING if e["recording"] else BUSY_LOADING if e["loading"] else BUSY_CARD if e["card_busy"] else OK
            if why and self.phase == RUNNING:
                self.pause_ms = now - ACK_EVERY_MS
            elif not why and self.phase == PAUSED:
                self.resumed = True
                self.heard_ms = now
            self.pause = why
            self.phase = PAUSED if why else RUNNING
            self.intake.paused = bool(why)
        if self.job == JOB_WRITE and self.phase == RUNNING:
            if self.intake.heard:
                self.intake.heard = False
                self.heard_ms = now
            while len(self.staged) <= BLOCK and self.intake.ring:
                data = self.intake.ring.pop(0)
                self.crc = zlib.crc32(data, self.crc)
                self.staged += data
                self.next += len(data)
        if self.ans is None and self.intake.pending is not None:
            q, self.intake.pending = self.intake.pending, None
            self.handle(q, now)
        card = e["card"] and not e["lent"] and not e["loading"] and not e["card_busy"]
        if card and self.unlink:
            self.card.unlink(TMP)
            self.unlink = False
        elif card and self.open_() and not e["recording"]:
            self.job_step()
        elif card and not self.open_() and self.card.fs_type and not self.free_known():
            self.count_step()
        else:
            self.count_left = None  # a count goes on only over a FAT nobody changed (5.4)
        if self.job == JOB_WRITE and self.phase == RUNNING and now - self.heard_ms >= TIMEOUT_MS:
            self.end(TIMED_OUT, 0, False)

    def send(self):
        """the pass's ack, made as it goes (NEXT and DROPS as they stand), then the answer"""
        out = []
        ack = self.acks(self.clock())
        if ack is not None:
            out.append(sysex(REPLY, ack))
        if self.ans is not None:
            out.append(sysex(REPLY, self.ans))
        self.ans = None
        return out

    def lend(self):
        if self.open_():
            self.end(CARD_LENT, 0, False)
        if self.unlink:
            self.card.unlink(TMP)
        self.unlink = False

    def settled(self):
        job = self.open_() and (self.phase == FINISHING or self.job == JOB_COPY or len(self.staged) >= BLOCK)
        return not job and not self.unlink and (not self.card.fs_type or self.free_known())

    def refusal(self):
        e = self.env
        return (NO_CARD if not e["card"] else CARD_LENT if e["lent"] else BUSY_RECORDING if e["recording"]
                else BUSY_LOADING if e["loading"] else BUSY_SCANNING if self.scanning() else BUSY_JOB
                if self.open_() else BUSY_CARD if e["card_busy"] else OK)

    def handle(self, q, now):
        op, seq, args = q
        changing = op in ARGS and op >= WRITE_BEGIN
        if changing and self.remembered and self.remembered[:2] == (op, seq):
            self.ans = [op, seq] + self.remembered[2]
            return
        self.detail_out = 0
        data = []
        st = BAD if op not in ARGS or len(args) != ARGS[op] else self.do(op, args, data, now)
        ans = [op, seq, st] + (data if st == OK else [self.detail_out])
        self.ans = ans
        if op == HELLO:
            self.remembered = None
        if changing and st not in PASSING:
            self.remembered = (op, seq, ans[2:])

    def do(self, op, a, d, now):
        e = self.env
        ld = self.loader
        if op == HELLO:
            d += [MAJOR, MINOR] + list(FRIZZ_VERSION) + put7(MAX_FRAMES, 4) + put7(MAX_FILE, 4) + \
                [MAX_CHUNK, WINDOW] + put7(MAX_REQUEST, 2) + [NAME_MAX, FRIZZ_BANKS, CUBBI_BANKS, SLOTS]
            return OK
        if op == STATUS:
            lent = e["lent"]
            known = e["card"] and not lent and self.card.fs_type and self.free_known()
            d.append((1 if e["card"] and not lent else 0) | (2 if lent else 0) | (4 if e["recording"] else 0)
                     | (8 if e["loading"] else 0) | (16 if self.scanning() else 0) | (32 if e["card_busy"] else 0)
                     | (64 if known else 0))
            d.append(1 if ld.tape else 0)
            d += put7(self.card.free_clst * self.card.cluster // 1024 if known else 0, 4)
            d += [self.job, self.phase, self.tid, self.bank, self.slot]
            d += put7(self.next, 4) + put7(self.total, 4) + [self.result, self.detail]
            return OK
        if op == BANK:
            if a[0] >= BANKS:
                return BAD
            d += [a[0], 1 if ld.scanning else 0] + [ld.slots[a[0]][s]["result"] for s in range(SLOTS)]
            return OK
        if op in (SLOT, NAME):
            if a[0] >= BANKS or a[1] >= SLOTS:
                return BAD
            s = ld.slots[a[0]][a[1]]
            d += [a[0], a[1]]
            if op == NAME:
                d += [len(s["name"])] + pack(s["name"])
                return OK
            d += [s["result"]] + put7(s["frames"], 4) + put7(s["beats"], 2)
            d += put7(0 if s["result"] == EMPTY_R else s["size"], 4) + [len(s["name"]), 1 if s["cut"] else 0]
            return OK
        if op == WRITE_END:
            if self.job != JOB_WRITE or self.phase not in (RUNNING, PAUSED) or a[0] != self.tid:
                return NO_TRANSFER
            self.heard_ms = now
            if self.next < self.total:
                return INCOMPLETE
            if self.crc != get7(a[1:6]):
                self.end(BAD_CRC, 0, True)
                return BAD_CRC
            self.intake.open = False
            self.phase, self.step = FINISHING, 1
            return OK
        if op == ABORT:
            if not self.open_() or a[0] != self.tid:
                return NO_TRANSFER
            self.end(ABORTED, 0, True)
            return OK
        refused = self.refusal()
        if refused:
            return refused
        if op == RESCAN:
            ld.request_scan()
            return OK
        b, s = a[0], a[1]
        if op in (WRITE_BEGIN, COPY):
            write = op == WRITE_BEGIN
            tb, ts = (b, s) if write else (a[2], a[3])
            flags = a[2] if write else a[4]
            if not (tb < FRIZZ_BANKS and ts < SLOTS) or (not write and not (FRIZZ_BANKS <= b < BANKS and s < SLOTS)):
                return NOT_A_SLOT
            frm = ld.slots[0 if write else b][0 if write else s]
            size = get7(a[3:7]) if write else frm["size"]
            if not write and frm["result"] == EMPTY_R:
                return EMPTY
            if not write and frm["result"] == TOO_LONG_R:
                return TOO_LONG
            if not write and frm["result"] not in (MONO, STEREO):
                self.detail_out = frm["result"]
                return BAD_WAV
            if self.filled(tb, ts) and not flags & REPLACE:
                return EXISTS
            if write and size > MAX_FILE:
                return TOO_LONG
            if write and size < 44:
                return BAD
            if size + SPARE > self.card.free_clst * self.card.cluster // 1024 * 1024:
                return NO_SPACE
            r = FR_OK
            if not write:
                r, self.src = self.card.open(slot_path(b, s, ld.tape), "r")
            if r == FR_OK:
                r, self.file = self.card.open(TMP, "c")
            if r:
                self.close_files()
                self.detail_out = r
                return CARD_ERROR
            self.unlink = False
            self.tid_seq = self.tid_seq % 127 + 1
            self.tid = self.tid_seq
            self.job = JOB_WRITE if write else JOB_COPY
            self.phase = RUNNING
            self.bank, self.slot, self.src_bank, self.src_slot = tb, ts, b, s
            self.replace = bool(flags & REPLACE)
            self.total = size
            self.next = self.written = 0
            self.staged = b""
            self.result = self.detail = 0
            self.crc = 0
            self.heard_ms = now
            self.resumed = False
            self.acked_next = self.acked_drops = 0
            if write:
                self.intake.open_write(self.tid, size)
            d.append(self.tid)
            if write:
                d += [MAX_CHUNK, WINDOW]
            return OK
        tb, ts = (a[2], a[3]) if op != DELETE else (0, 0)
        if not (b < FRIZZ_BANKS and s < SLOTS) or (op != DELETE and not (tb < FRIZZ_BANKS and ts < SLOTS)):
            return NOT_A_SLOT
        if op != DELETE and (b, s) == (tb, ts):
            return BAD
        frm = self.filled(b, s)
        to = op != DELETE and self.filled(tb, ts)
        if not frm and (op != SWAP or not to):
            return EMPTY
        if op == RENAME and to and not a[4] & REPLACE:
            return EXISTS
        pa, pb = slot_path(b, s, False), slot_path(tb, ts, False)
        if op == DELETE:
            r = self.card.unlink(pa)
            r = FR_OK if r == FR_NO_FILE else r
            if r == FR_OK:
                ld.slots[b][s] = empty_slot()
        else:
            if op == SWAP and frm and to:
                tmp = swap_path(b * SLOTS + s, tb * SLOTS + ts)
                r = self.card.rename(pa, tmp)
                if r == FR_OK:
                    r = self.card.rename(pb, pa)
                if r == FR_OK:
                    r = self.card.rename(tmp, pb)
                # one failing: the temp file on to B if that's free, else back to A (5.6)
                if r != FR_OK and self.card.rename(tmp, pb) != FR_OK:
                    self.card.rename(tmp, pa)
            elif not frm:
                r = self.card.rename(pb, pa)
            else:
                r = self.card.unlink(pb) if to else FR_OK
                if r == FR_OK:
                    r = self.card.rename(pa, pb)
            if r == FR_OK:
                ld.slots[b][s], ld.slots[tb][ts] = ld.slots[tb][ts], ld.slots[b][s]
                if op == RENAME:
                    ld.slots[b][s] = empty_slot()
            self.changed.add(tb * SLOTS + ts)
        self.changed.add(b * SLOTS + s)
        if r == FR_OK:
            return OK
        # maybe after other calls worked: the slots as the card holds them (#294)
        self.recheck(b, s)
        if op != DELETE:
            self.recheck(tb, ts)
        self.detail_out = r
        return CARD_ERROR

    def recheck(self, b, s):
        """After a card error: the slot as the card holds it (LoopTransfer::Recheck)"""
        path = slot_path(b, s, False)
        r = self.card.stat(path)
        if r == FR_OK:
            self.loader.slots[b][s] = self.loader.walk(path)
        else:
            self.loader.slots[b][s] = empty_slot() | ({} if r in (FR_NO_FILE, FR_NO_PATH) else dict(result=UNREADABLE))
        self.changed.add(b * SLOTS + s)

    def job_step(self):
        if self.phase == FINISHING:
            self.finish()
        elif self.phase != RUNNING:
            return
        elif self.job == JOB_WRITE and len(self.staged) >= BLOCK:
            if self.write(self.staged[:BLOCK]):
                self.staged = self.staged[BLOCK:]
        elif self.job == JOB_COPY:
            n = min(BLOCK, self.total - self.next)
            r, got = self.card.read(self.src, n)
            if r == FR_INVALID_OBJECT:
                r = self.reopen()
                if r == FR_OK:
                    r, got = self.card.read(self.src, n)
            if r or len(got) != n:
                self.end(CARD_ERROR, r or FR_INT_ERR, False)
                return
            if not self.write(got):
                return
            self.next += n
            if self.next == self.total:
                self.close_files()
                self.phase, self.step = FINISHING, 2

    def write(self, data):
        r, put = self.card.write(self.file, data)
        if r == FR_INVALID_OBJECT:
            r = self.reopen()
            if r == FR_OK:
                r, put = self.card.write(self.file, data)
        if r == FR_OK and put != len(data):
            self.end(NO_SPACE, 0, False)
            return False
        if r == FR_OK:
            r = self.card.sync(self.file)
        if r:
            self.end(CARD_ERROR, r, False)
            return False
        self.written += len(data)
        return True

    def reopen(self):
        self.close_files()
        r, self.file = self.card.open(TMP, "w")
        if r == FR_OK and self.card.size(self.file) != self.written:
            r = FR_INT_ERR
        if r == FR_OK:
            r = self.card.lseek(self.file, self.written)
        if r == FR_OK and self.job == JOB_COPY:
            r, self.src = self.card.open(slot_path(self.src_bank, self.src_slot, self.loader.tape), "r")
            if r == FR_OK:
                r = self.card.lseek(self.src, self.written)
        return r

    def close_files(self):
        for f in (self.file, self.src):
            if f is not None:
                self.card.close(f)
        self.file = self.src = None

    def finish(self):
        if self.step == 1:
            if self.staged and not self.write(self.staged):
                return
            self.staged = b""
            self.close_files()
            self.step = 2
            return
        if self.step == 2:
            self.walked = self.loader.walk(TMP)
            if self.walked["result"] not in (MONO, STEREO):
                self.end(BAD_WAV, self.walked["result"], True)
                return
            r, self.file = self.card.open(TMP, "r")
            if r:
                self.end(CARD_ERROR, r, True)
                return
            self.clusters = (self.walked["size"] + self.card.cluster - 1) // self.card.cluster
            self.cluster = 0
            self.step = 3
            return
        if self.step == 3:
            if not self.card.present:
                self.end(CARD_ERROR, FR_NOT_READY, True)
                return
            if self.file.mount != self.card.mount_id:
                self.card.close(self.file)
                r, self.file = self.card.open(TMP, "r")
                if r:
                    self.end(CARD_ERROR, r, True)
                    return
            self.cluster = min(self.clusters, self.cluster + MAP_CLUSTERS)
            if self.cluster < self.clusters:
                return
            self.close_files()
            if self.card.fragments(self.walked["size"]) > MAX_FRAGMENTS:
                self.end(FRAGMENTED, 0, True)
                return
            self.step = 4
            return
        path = slot_path(self.bank, self.slot, False)
        r = self.card.stat(path)
        there = r == FR_OK
        if there and not self.replace:
            self.end(EXISTS, 0, True)
            return
        # a check that failed is a card error, not "no file" (3.9)
        if there:
            r = self.card.unlink(path)
        elif r == FR_NO_FILE:
            r = FR_OK
        if r == FR_OK:
            r = self.card.rename(TMP, path)
        if r:
            self.end(CARD_ERROR, r, True)
            self.recheck(self.bank, self.slot)  # the old file may be gone (#294)
            return
        self.loader.slots[self.bank][self.slot] = self.walked
        self.changed.add(self.bank * SLOTS + self.slot)
        self.phase = DONE

    def end(self, st, detail, told):
        if self.job == JOB_WRITE and self.phase in (RUNNING, PAUSED) and not told:
            self.end_ack = st
        self.intake.open = False
        self.close_files()
        self.unlink = True
        self.phase, self.result, self.detail = FAILED, st, detail
        self.staged = b""

    def count_step(self):
        # a FAT32 sector a pass, 128 entries (LoopTransfer::CountStep against the fake's FAT)
        if self.count_left is None:
            self.count_left = (self.card.clusters + 2 + 127) // 128
        self.count_left -= 1
        if self.count_left == 0:
            self.card.free_clst = self.card.free()
            self.count_left = None

    def acks(self, now):
        st, tid, drops, nxt = None, self.tid, self.intake.drops, self.next
        if self.end_ack:
            st, self.end_ack = self.end_ack, 0
        elif self.job == JOB_WRITE and self.phase == RUNNING:
            repeat, self.intake.repeat = self.intake.repeat, False
            if repeat or self.resumed or self.next != self.acked_next or drops != self.acked_drops:
                st = OK
            self.resumed = False
        elif self.job == JOB_WRITE and self.phase == PAUSED and now - self.pause_ms >= ACK_EVERY_MS:
            st, self.pause_ms = self.pause, now
        if st is None and self.intake.stale:
            self.intake.stale = False
            tid = self.intake.stale_tid
            if not self.stale_sent or now - self.stale_ms >= ACK_EVERY_MS:
                st, nxt, drops = NO_TRANSFER, 0, 0
                self.stale_sent, self.stale_ms = True, now
        if st is None:
            return None
        if st != NO_TRANSFER:
            self.acked_next, self.acked_drops = nxt, drops
        return [CHUNK, 0, st, tid] + put7(nxt, 4) + [drops]


def frizz_version():
    """kFrizzVersion from code/src/Version.h"""
    import re
    with open(os.path.join(FIRMWARE, "code", "src", "Version.h")) as f:
        m = re.search(r"kFrizzVersion\[4\] = \{(\d+), (\d+), (\d+), (\d+)\}", f.read())
    return [int(x) for x in m.groups()]


FRIZZ_VERSION = frizz_version()


class Bench:
    """FRIZZ-bench.bin: HELLO answered, NOT_HERE to the rest (LoopTransfer.h's BenchAnswer)"""

    def __init__(self, card, boot=True):
        self.card, self.ms, self.intake = card, 0, Intake()
        self.env = dict(usb=True)

    def boot(self):
        self.intake = Intake()

    def deliver(self, msg):
        Frizz.deliver(self, msg)

    def step(self):
        out = []
        if self.intake.pending is not None:
            op, seq, args = self.intake.pending
            self.intake.pending = None
            if op == HELLO and not args:
                out.append(sysex(REPLY, [op, seq, OK, MAJOR, MINOR] + list(FRIZZ_VERSION) + put7(MAX_FRAMES, 4)
                                 + put7(MAX_FILE, 4) + [MAX_CHUNK, WINDOW] + put7(MAX_REQUEST, 2)
                                 + [NAME_MAX, FRIZZ_BANKS, CUBBI_BANKS, SLOTS]))
            else:
                out.append(sysex(REPLY, [op, seq, NOT_HERE, 0]))  # noqa: F821
        self.ms += PASS_MS
        return out

    def settle(self, limit=1):
        return self.step()


class Frizz:
    """The model FRIZZ on a Card: test/loopxfer.cpp's harness around the firmware, the same
    passes. env: card, lent, recording, loading, card_busy, usb"""

    def __init__(self, card, boot=True):
        self.card = card
        self.ms = 0
        self.env = dict(card=True, lent=False, recording=False, loading=False, card_busy=False, usb=True)
        if boot:
            self.boot()

    def boot(self):
        c = self.card
        c.present, c.cut_after = True, None
        c.mount()
        Transfer.boot(c)
        self.loader = Loader(c)
        self.loader.request_scan()
        self.intake = Intake()
        self.xfer = Transfer(c, self.loader, self.intake, lambda: self.ms)
        self.env = dict(card=True, lent=False, recording=False, loading=False, card_busy=False, usb=True)

    def deliver(self, msg):
        """A message from the app: libDaisy's parser keeps the first 128 bytes after F0"""
        if not self.env["usb"] or len(msg) < 2 or msg[0] != 0xF0 or msg[-1] != 0xF7:
            return
        d = msg[1:-1][:MAX_REQUEST]
        if d[:4] == HEADER + bytes([CMD]):
            self.intake.take(d[4:])

    def set(self, **state):
        for k, v in state.items():
            if k == "lent" and v and not self.env["lent"]:
                self.xfer.lend()
                self.card.mount(unmount=True)
            elif k == "lent" and not v and self.env["lent"]:
                self.card.mount()
                self.loader.request_scan()
            self.env[k] = v

    def remount(self):
        self.card.mount()

    def step(self):
        """One MainLoop pass: what FRIZZ sent in it"""
        if not self.env["lent"]:
            self.loader.process(self.env["card_busy"])
        self.xfer.process(dict(self.env))
        out = self.xfer.send()
        self.ms += PASS_MS
        return out if self.env["usb"] else []  # without a computer nothing goes out

    def idle(self):
        return not self.loader.scanning and self.xfer.settled()

    def settle(self, limit=200000):
        out = []
        for _ in range(limit):
            sent = self.step()
            out += sent
            if not sent and self.idle():
                return out
        raise RuntimeError("the model didn't settle")


# ---- links: what the client talks through ----

class ModelLink:
    """The model FRIZZ as a link, in its own time: send() delivers a message (dropped, sent
    twice or cut off as loss says), recv() runs FRIZZ's passes until it sends something (an
    answer dropped as loss says) or the time's up. loss: dict(drop=, repeat=, cut=, lose=)"""

    def __init__(self, frizz, loss=None, rng=None):
        self.frizz = frizz
        self.loss = loss or {}
        self.rng = rng or random.Random(1)
        self.inbox = []
        self.sent = 0

    def now(self):
        return self.frizz.ms / 1000.0

    def _chance(self, what):
        return self.rng.random() < self.loss.get(what, 0)

    def send(self, msg):
        self.sent += 1
        if self._chance("drop"):
            return
        if len(msg) > 10 and msg[5] == CHUNK and self._chance("cut"):
            msg = msg[:self.rng.randint(6, len(msg) - 2)] + b"\xF7"
        self.frizz.deliver(msg)
        if self._chance("repeat"):
            self.frizz.deliver(msg)

    def recv(self, timeout):
        end = self.frizz.ms + max(PASS_MS, int(timeout * 1000))
        while not self.inbox and self.frizz.ms < end:
            for m in self.frizz.step():
                if not self._chance("lose"):
                    self.inbox.append(m)
        return self.inbox.pop(0) if self.inbox else None


class RawLink:
    """A raw MIDI node (ALSA's /dev/snd/midiC*D*, the twin's pty): SysEx messages in and out"""

    def __init__(self, fd):
        self.fd = fd
        self.buf = bytearray()

    def now(self):
        return time.monotonic()

    def send(self, msg):
        os.write(self.fd, msg)

    def recv(self, timeout):
        end = time.monotonic() + timeout
        while True:
            start = self.buf.find(b"\xF0")
            stop = self.buf.find(b"\xF7", start) if start >= 0 else -1
            if start >= 0 and stop >= 0:
                msg = bytes(self.buf[start:stop + 1])
                del self.buf[:stop + 1]
                return msg
            if start > 0:
                del self.buf[:start]
            left = end - time.monotonic()
            if left <= 0:
                return None
            r, _, _ = select.select([self.fd], [], [], left)
            if r:
                try:
                    self.buf.extend(os.read(self.fd, 4096))
                except BlockingIOError:
                    pass


# ---- the client (sections 1.5-3) ----

class LoopError(Exception):
    """A request refused or a job failed: status, detail"""

    def __init__(self, what, st, detail=0):
        self.status, self.detail = st, detail
        words = WORDS.get(st, status_name(st))
        if st == BAD_WAV:  # noqa: F821
            words += ": " + (RESULTS[detail] if detail < len(RESULTS) else str(detail))
        elif st == CARD_ERROR and detail:  # noqa: F821
            words += " (FatFs %d)" % detail
        super().__init__("%s: %s" % (what, words))


class Client:
    """The app's side over a link (1.5-1.6, 3.7). seq starts at random, as the app's does"""

    def __init__(self, link, rng=None, busy_wait=120.0, log=None):
        self.link = link
        self.rng = rng or random.Random()
        self.seq = self.rng.randint(1, 127)
        self.last = None  # the last changing request's (op, seq)
        self.acks = []
        self.busy_wait = busy_wait
        self.log = log or (lambda *a: None)
        self.other = None  # what gets the messages that aren't the loop transfer's
        self.tick = None   # called as a write goes, with the link's time (remote.py's --cpu)
        self.stats = dict(chunks=0, rewinds=0, polls=0, ack_ms=[])

    def next_seq(self, op):
        while True:
            self.seq = self.seq % 127 + 1
            if (op, self.seq) != self.last:
                return self.seq

    def _take(self, msg, op, seq):
        """An answer to op/seq; an ack is kept for the write"""
        a = parse(msg) if msg else None
        if a is None:
            if msg and self.other:
                self.other(msg)
            return None
        if a.ack:
            self.acks.append((a, self.link.now()))
            return None
        return a if (a.op, a.seq) == (op, seq) else None

    def sleep(self, seconds):
        end = self.link.now() + seconds
        while self.link.now() < end:
            self._take(self.link.recv(end - self.link.now()), None, None)

    def ask(self, op, args=(), busy=True):
        """A request answered (resent on silence with its SEQ, 1.6); a BUSY_... asked again
        every 250 ms with a new one while busy, up to busy_wait s"""
        deadline = self.link.now() + self.busy_wait
        while True:
            seq = self.next_seq(op)
            msg = request(op, seq, args)
            if op in CHANGING:
                self.last = (op, seq)
            a = None
            for _ in range(5):
                self.link.send(msg)
                end = self.link.now() + 0.5
                while a is None and self.link.now() < end:
                    a = self._take(self.link.recv(end - self.link.now()), op, seq)
                if a:
                    break
            if a is None:
                raise TimeoutError("FRIZZ doesn't answer: not FRIZZ, a FRIZZ without the loop transfer, "
                                   "or one that hangs")
            if busy and a.status in PASSING and self.link.now() < deadline:
                self.log("  %s, waiting" % WORDS.get(a.status, status_name(a.status)))
                self.sleep(0.25)
                continue
            return a

    def hello(self):
        a = self.ask(HELLO)
        d = a.data
        self.limits = dict(major=d[0], minor=d[1], version=tuple(d[2:6]), max_frames=get7(d[6:10]),
                           max_file=get7(d[10:14]), max_chunk=d[14], window=d[15], max_request=get7(d[16:18]),
                           name_max=d[18], frizz_banks=d[19], cubbi_banks=d[20], slots=d[21])
        return self.limits

    def status(self):
        a = self.ask(STATUS)
        d = a.data
        self.stats["polls"] += 1
        return dict(flags=d[0], tape=bool(d[1] & 1), free_kb=get7(d[2:6]), job=d[6], phase=d[7], tid=d[8],
                    bank=d[9], slot=d[10], done=get7(d[11:15]), total=get7(d[15:19]), result=d[19], detail=d[20])

    def wait_scan(self, timeout=60):
        end = self.link.now() + timeout
        while self.status()["flags"] & 16:
            if self.link.now() > end:
                raise TimeoutError("the card's scan didn't end")
            self.sleep(0.1)

    def listing(self):
        """Every bank's slots: {(bank, slot): dict(result, frames, beats, size, name, cut, ...)}"""
        out = {}
        for b in range(BANKS):
            d = self.ask(BANK, [b]).data
            for s, r in enumerate(d[2:2 + SLOTS]):
                if not r:
                    continue
                i = self.ask(SLOT, [b, s]).data
                n = self.ask(NAME, [b, s]).data
                out[(b, s)] = dict(result=r, frames=get7(i[3:7]), beats=get7(i[7:9]), size=get7(i[9:13]),
                                   cut=bool(i[14] & 1), playing=bool(i[14] & 2), ready=bool(i[14] & 4),
                                   name=unpack(n[3:], n[2]).decode("utf-8", "replace"))
        return out

    def _check(self, what, a):
        if a.status:
            raise LoopError(what, a.status, a.detail)
        return a

    def _job(self, what, tid, timeout=600):
        """STATUS every 250 ms until the job's done or failed"""
        end = self.link.now() + timeout
        while True:
            st = self.status()
            if st["tid"] == tid and st["phase"] == DONE:
                return st
            if st["tid"] == tid and st["phase"] == FAILED:
                raise LoopError(what, st["result"], st["detail"])
            if st["tid"] != tid:
                raise LoopError(what, NO_TRANSFER)  # noqa: F821
            if self.link.now() > end:
                raise TimeoutError("%s: no end in %d s" % (what, timeout))
            self.sleep(0.25)

    def write(self, b, s, data, replace=False, progress=None):
        """data, a whole WAV, into FRIZZ slot (b, s): go-back-N (3.7), then WRITE_END and the
        finishing. Its STATUS at the end"""
        what = "write %s" % slot_name(b, s)
        size = len(data)
        a = self._check(what, self.ask(WRITE_BEGIN, [b, s, REPLACE if replace else 0] + put7(size, 4)))
        tid, max_chunk, window = a.data[0], a.data[1], a.data[2]
        self.acks = []
        acked = sent = drops = 0
        rewound = None
        paused = False
        moved = progress_at = poll_at = self.link.now()
        sent_at = {}
        while acked < size:
            while not paused and sent < size and sent - acked < window * max_chunk:
                n = min(max_chunk, size - sent)
                self.link.send(chunk(tid, sent, data[sent:sent + n]))
                sent_at[sent + n] = self.link.now()
                self.stats["chunks"] += 1
                sent += n
            if not self.acks:
                wait = 0.25 if paused else max(0.002, moved + 0.5 - self.link.now())
                self._take(self.link.recv(wait), None, None)
            now = self.link.now()
            if self.tick:
                self.tick(now)
            while self.acks:
                ack, at = self.acks.pop(0)
                if ack.tid != tid:
                    continue
                if ack.next > acked:
                    if ack.next in sent_at:
                        self.stats["ack_ms"].append((at - sent_at[ack.next]) * 1000)
                    acked = ack.next
                    moved = progress_at = now
                    if progress:
                        progress(acked, size)
                if rewound is not None and acked > rewound:
                    rewound = None
                if ack.status == OK:
                    if paused:
                        paused = False
                        sent = rewound = acked
                        moved = progress_at = now
                    elif ack.drops != drops and rewound is None:
                        sent = rewound = acked
                        self.stats["rewinds"] += 1
                    drops = ack.drops
                elif ack.status in (BUSY_RECORDING, BUSY_LOADING, BUSY_CARD):  # noqa: F821
                    if not paused:
                        self.log("  paused: %s" % WORDS[ack.status])
                    paused = True
                    drops = ack.drops
                else:
                    st = self.status()
                    raise LoopError(what, st["result"] if st["tid"] == tid and st["result"] else ack.status,
                                    st["detail"] if st["tid"] == tid else 0)
            if acked >= size:
                break
            if paused and now - poll_at >= 0.25:
                poll_at = now
                st = self.status()
                if st["tid"] == tid and st["phase"] == RUNNING:
                    paused = False
                    acked = max(acked, st["done"])
                    sent = rewound = acked
                    moved = progress_at = self.link.now()
                elif st["tid"] != tid or st["phase"] in (DONE, FAILED):
                    raise LoopError(what, st["result"] if st["tid"] == tid else NO_TRANSFER, st["detail"])  # noqa: F821
            elif not paused and now - moved >= 0.5:
                # the ack that would have moved it may be the one lost: STATUS first (3.7 step 3)
                st = self.status()
                if st["tid"] != tid or st["phase"] in (DONE, FAILED):
                    raise LoopError(what, st["result"] if st["tid"] == tid else NO_TRANSFER, st["detail"])  # noqa: F821
                if st["phase"] == PAUSED:
                    paused = True
                    continue
                acked = max(acked, st["done"])
                sent = rewound = acked
                moved = self.link.now()
                self.stats["rewinds"] += 1
                if moved - progress_at >= 10:
                    self.ask(ABORT, [tid])
                    raise LoopError(what, TIMED_OUT)  # noqa: F821
        a = self.ask(WRITE_END, [tid] + put7(zlib.crc32(data), 5))
        self._check(what, a)
        return self._job(what, tid)

    def copy(self, cubbi_bank, s, b, ts, replace=False):
        what = "copy cubbi %s to %s" % (slot_name(cubbi_bank - FRIZZ_BANKS, s), slot_name(b, ts))
        a = self._check(what, self.ask(COPY, [cubbi_bank, s, b, ts, REPLACE if replace else 0]))
        return self._job(what, a.data[0])

    def rename(self, b, s, tb, ts, replace=False):
        return self._check("move %s to %s" % (slot_name(b, s), slot_name(tb, ts)),
                           self.ask(RENAME, [b, s, tb, ts, REPLACE if replace else 0]))

    def swap(self, b, s, tb, ts):
        return self._check("swap %s and %s" % (slot_name(b, s), slot_name(tb, ts)), self.ask(SWAP, [b, s, tb, ts]))

    def delete(self, b, s):
        a = self.ask(DELETE, [b, s])
        if a.status not in (OK, EMPTY):  # noqa: F821  (already empty counts as done)
            raise LoopError("delete %s" % slot_name(b, s), a.status, a.detail)
        return a

    def rescan(self):
        self._check("rescan", self.ask(RESCAN))
        self.wait_scan()


# ---- the vectors (8.1) ----

def masked(msg):
    """A message as the vectors write it: hex, HELLO's FRIZZ version as ?? (it changes with
    every release, the contract doesn't)"""
    h = ["%02X" % b for b in msg]
    a = parse(msg)
    if a and not a.ack and a.op == HELLO and a.status == OK:
        h[10:14] = ["??"] * 4
    return " ".join(h)


def card_files(card):
    return {p: {"size": len(d), "crc32": "%08x" % zlib.crc32(d)} for p, d in sorted(card.files.items())}


class Session:
    """A scripted session against the model, recorded as the vectors' steps"""

    def __init__(self, name, about, files=None, dirs=("/FRIZZ",), settle=True, bench=False, **config):
        self.card = Card(files, dirs, **config)
        self.out = dict(name=name, about=about,
                        card=dict(self.card.config(), dirs=sorted(self.card.dirs),
                                  files={p: hexes(d) for p, d in sorted(self.card.files.items())}),
                        steps=[])
        if bench:
            self.out["bench"] = True
        self.frizz = (Bench if bench else Frizz)(self.card, boot=False)
        self.seq = 0
        self.dead = False
        self.boot(settle)

    def step(self, **st):
        self.out["steps"].append(st)

    def boot(self, settle=True):
        self.frizz.boot()
        self.dead = False
        self.step(boot=True)
        if settle:
            self.settle()

    def app(self, msg):
        self.frizz.deliver(msg)
        self.step(app=hexes(msg))

    def run(self, passes=1):
        out = []
        for _ in range(passes):
            out += self.frizz.step()
        self.step(run=passes, frizz=None if self.dead else [masked(m) for m in out])
        return [parse(m) for m in out]

    def settle(self):
        out = self.frizz.settle()
        self.step(settle=True, frizz=None if self.dead else [masked(m) for m in out])
        return [parse(m) for m in out]

    def ask(self, msg, passes=1):
        self.app(msg)
        out = self.run(passes)
        return out[-1] if out else None

    def set(self, **state):
        self.frizz.set(**state)
        self.step(set=state)

    def remount(self):
        self.frizz.remount()
        self.step(remount=True)

    def put(self, path, data):
        """A hand on the card: a file put there directly"""
        self.card.files[path] = data
        self.step(put={path: hexes(data)})

    def cut(self, after):
        """A power cut after the after-th name change from here (a file created, renamed,
        deleted), 0 now: FRIZZ's output isn't checked until the next boot"""
        if after:
            self.card.cut_after = self.card.changes + after
        else:
            self.card.cut()
        self.dead = True
        self.step(cut=after)

    def next_seq(self):
        self.seq = self.seq % 127 + 1
        return self.seq

    def stream(self, tid, data, until=None):
        """A window at a time from the last ack's NEXT, a pass after each, until NEXT is until
        (default the whole): go-back-N as simple as it gets, no loss"""
        until = len(data) if until is None else until
        nxt = self.next
        while nxt < until:
            for o in range(nxt, min(len(data), nxt + MAX_CHUNK * WINDOW), MAX_CHUNK):
                self.app(chunk(tid, o, data[o:o + MAX_CHUNK]))
            acks = [a for a in self.run() if a and a.ack and a.tid == tid]
            if acks:
                nxt = acks[-1].next
        self.next = nxt

    def write(self, b, s, data, flags=0, end=True):
        """A whole write without loss: its TID"""
        a = self.ask(write_begin(self.next_seq(), b, s, flags, len(data)))
        tid = a.data[0]
        self.next = 0
        self.stream(tid, data)
        if end:
            self.ask(write_end(self.next_seq(), tid, zlib.crc32(data)))
            self.settle()
        return tid

    def done(self):
        self.out["after"] = card_files(self.card)
        return self.out


def stereo(frames, seed, **kw):
    return wav(pattern(frames * 4, seed), 2, **kw)


def mono(frames, seed, **kw):
    return wav(pattern(frames * 2, seed), 1, **kw)


def sessions():
    """The vectors: every command, every refusal of sections 3-4, the write's losses, the
    pauses, the timeout, the abort, the power cuts and the boots after them"""
    out = []
    loop_a = stereo(300, 1, beats=4, name_text="Amen break")
    loop_b = mono(200, 2, name_text="Bass été")
    cubbi = stereo(250, 3)
    base = {"/FRIZZ/a1.wav": loop_a, "/FRIZZ/a2.wav": loop_b, "/TAPE/cubbi_a1.wav": cubbi,
            "/TAPE/cubbi_a2.wav": wav(pattern(400, 4), 2, rate=44100), "/FRIZZ/frizz_scenes.txt": b"scenes\n",
            "/cubbi_b1.wav": stereo(10, 5)}

    s = Session("hello", "HELLO, then the listing: BANK, SLOT and NAME of every slot, STATUS", base)
    s.ask(hello(s.next_seq()))
    for b in range(BANKS):
        s.ask(bank(s.next_seq(), b))
    for b, sl in ((0, 0), (0, 1), (8, 0), (8, 1), (0, 5)):
        s.ask(slot(s.next_seq(), b, sl))
        s.ask(name(s.next_seq(), b, sl))
    s.ask(status(s.next_seq()))
    out.append(s.done())

    s = Session("bad", "BAD: unknown OPs, arguments missing or extra, out of range; not remembered", base)
    s.ask(request(0x05, s.next_seq()))
    s.ask(request(0x7F, s.next_seq(), [1, 2]))
    s.ask(request(HELLO, s.next_seq(), [0]))
    s.ask(request(BANK, s.next_seq()))
    s.ask(bank(s.next_seq(), 13))
    s.ask(slot(s.next_seq(), 13, 0))
    s.ask(slot(s.next_seq(), 0, 15))
    s.ask(name(s.next_seq(), 12, 15))
    s.ask(request(DELETE, s.next_seq(), [0]))
    s.ask(write_begin(s.next_seq(), 7, 14, 0, 43))
    s.ask(rename(s.next_seq(), 0, 0, 0, 0))
    s.ask(swap(s.next_seq(), 1, 1, 1, 1))
    s.ask(request(RESCAN, s.next_seq(), [1]))
    s.ask(request(STATUS, s.next_seq(), [0, 0]))
    out.append(s.done())

    s = Session("write", "A write of a stereo loop with beats and a name into an empty slot; the "
                "listing after it", base)
    data = stereo(700, 7, beats=8, name_text="New ☃ loop")
    tid = s.write(1, 2, data, end=False)
    s.ask(status(s.next_seq()))
    s.ask(write_end(s.next_seq(), tid, zlib.crc32(data)))
    s.ask(status(s.next_seq()))
    s.settle()
    s.ask(status(s.next_seq()))
    s.ask(slot(s.next_seq(), 1, 2))
    s.ask(name(s.next_seq(), 1, 2))
    s.ask(bank(s.next_seq(), 1))
    out.append(s.done())

    s = Session("write-replace", "A write onto a filled slot: EXISTS without REPLACE, with it the old "
                "file replaced; a mono loop; HELLO forgets the request remembered", base)
    data = mono(500, 9, name_text="x" * 40)
    seq = s.next_seq()
    s.ask(write_begin(seq, 0, 0, 0, len(data)))
    s.ask(write_begin(seq, 0, 0, 0, len(data)))  # a repeat: the same answer, remembered
    s.write(0, 0, data, REPLACE)
    s.ask(slot(s.next_seq(), 0, 0))
    s.ask(name(s.next_seq(), 0, 0))
    seq = s.next_seq()
    s.ask(delete(seq, 0, 1))
    s.ask(delete(seq, 0, 1))                    # a repeat: its answer remembered, OK
    s.ask(hello(s.next_seq()))
    s.ask(delete(seq, 0, 1))                    # HELLO forgot it: done again, EMPTY now
    out.append(s.done())

    s = Session("write-refusals", "WRITE_BEGIN's refusals: NOT_A_SLOT, EXISTS, TOO_LONG, BAD, NO_SPACE",
                base, clusters=1200)
    s.ask(write_begin(s.next_seq(), 8, 0, 0, 1000))
    s.ask(write_begin(s.next_seq(), 0, 15, 0, 1000))
    s.ask(write_begin(s.next_seq(), 0, 0, 0, 1000))
    s.ask(write_begin(s.next_seq(), 3, 0, 0, MAX_FILE + 1))
    s.ask(write_begin(s.next_seq(), 3, 0, 0, MAX_FILE))
    s.ask(write_begin(s.next_seq(), 3, 0, 0, 43))
    s.ask(write_begin(s.next_seq(), 3, 0, 0, 5 * 1024 * 1024))
    a = s.ask(write_begin(s.next_seq(), 3, 0, 0, 1000))
    s.ask(write_begin(s.next_seq(), 3, 1, 0, 1000))  # one job at a time
    s.ask(status(s.next_seq()))
    s.ask(abort(s.next_seq(), a.data[0]))
    s.settle()
    out.append(s.done())

    s = Session("write-end", "WRITE_END: NO_TRANSFER for another TID, INCOMPLETE before every byte, "
                "BAD_CRC (the temp file deleted, repeated: BAD_CRC again), ABORT after it", base)
    data = stereo(400, 11)
    tid = s.write(2, 0, data, end=False)
    s.ask(write_end(s.next_seq(), tid + 1, zlib.crc32(data)))
    s.ask(write_begin(s.next_seq(), 2, 1, 0, 100))  # BUSY_JOB: one job at a time
    seq = s.next_seq()
    s.ask(write_end(seq, tid, zlib.crc32(data) ^ 1))
    s.ask(write_end(seq, tid, zlib.crc32(data) ^ 1))
    s.ask(abort(s.next_seq(), tid))
    s.ask(status(s.next_seq()))
    s.settle()
    tid = s.write(2, 1, data, end=False)
    s.ask(abort(s.next_seq(), tid))
    s.ask(write_end(s.next_seq(), tid, zlib.crc32(data)))
    s.settle()
    a = s.ask(write_begin(s.next_seq(), 2, 1, 0, len(data)))
    tid = a.data[0]
    s.app(chunk(tid, 0, data[:MAX_CHUNK]))
    s.run()
    s.ask(write_end(s.next_seq(), tid, zlib.crc32(data)))
    s.ask(abort(s.next_seq(), tid))
    s.settle()
    s.ask(status(s.next_seq()))
    out.append(s.done())

    s = Session("chunks", "CHUNKs lost, repeated and cut off: the gap and its DROPS, the rewind, the "
                "stale CHUNKs behind it, a repeat acked, the ring full, a LEN that doesn't match", base)
    data = stereo(1200, 13)
    a = s.ask(write_begin(s.next_seq(), 4, 4, 0, len(data)))
    tid = a.data[0]
    c = [data[o:o + MAX_CHUNK] for o in range(0, len(data), MAX_CHUNK)]
    s.app(chunk(tid, 0, c[0]))
    s.app(chunk(tid, 2 * MAX_CHUNK, c[2]))       # the 2nd lost: a gap
    s.app(chunk(tid, 3 * MAX_CHUNK, c[3]))       # behind it: dropped too
    s.run()                                     # NEXT 98, DROPS 2
    s.app(chunk(tid, MAX_CHUNK, c[1]))           # the rewind
    s.app(chunk(tid, 2 * MAX_CHUNK, c[2]))
    s.app(chunk(tid, 4 * MAX_CHUNK, c[4]))       # a stale one from before the rewind: a gap
    s.run()
    s.app(chunk(tid, 0, c[0]))                   # a repeat: acked, not counted
    s.run()
    s.run()                                     # nothing new: no ack
    cut = chunk(tid, 3 * MAX_CHUNK, c[3])
    s.app(cut[:60] + b"\xF7")                    # cut off by the sender: its LEN doesn't match
    long = chunk(tid, 3 * MAX_CHUNK, c[3] + c[4][:40])  # 138 bytes: libDaisy keeps 128
    s.app(long)
    s.run()
    for k in range(3, 3 + WINDOW + 2):          # past the ring: the last two dropped
        s.app(chunk(tid, k * MAX_CHUNK, c[k]))
    s.run()
    s.app(chunk(tid, 3 * MAX_CHUNK + 1, c[3][1:]))  # an offset inside what was taken: a repeat
    s.run()
    k = 3 + WINDOW
    s.app(sysex(CMD, [CHUNK, tid] + put7(k * MAX_CHUNK, 4) + [MAX_CHUNK + 1]
                + pack(c[k] + c[k + 1][:1])))   # LEN over maxChunk: a gap
    s.app(sysex(CMD, [CHUNK, tid] + put7(k * MAX_CHUNK, 4) + [0]))  # LEN 0: a gap
    s.run()
    for k in range(3 + WINDOW, len(c)):
        s.app(chunk(tid, k * MAX_CHUNK, c[k]))
        if k % WINDOW == 0:
            s.run()
    s.run()
    s.app(chunk(tid, len(data), b"\x01"))       # past SIZE: a gap
    s.run()
    s.ask(write_end(s.next_seq(), tid, zlib.crc32(data)))
    s.settle()
    s.app(chunk(tid, 0, c[0]))                   # the write's done: NO_TRANSFER, every 250 ms at most
    s.app(chunk(tid, 0, c[0]))
    s.run()
    s.app(chunk(tid, 0, c[0]))
    s.run(10)
    s.app(chunk(tid, 0, c[0]))
    s.run(120)
    s.app(chunk(tid, 0, c[0]))
    s.run()
    s.ask(status(s.next_seq()))
    out.append(s.done())

    s = Session("lost-answers", "A lost answer and its repeat across a STATUS poll; a lost ack made good "
                "by STATUS; a passing refusal not remembered", base)
    data = stereo(300, 17)
    seq = s.next_seq()
    s.app(write_begin(seq, 5, 5, 0, len(data)))
    s.run()                                     # the app never saw this answer
    s.ask(status(s.next_seq()))                 # it polls, then asks again with the same SEQ
    a = s.ask(write_begin(seq, 5, 5, 0, len(data)))
    tid = a.data[0]
    for o in range(0, len(data), MAX_CHUNK):
        s.app(chunk(tid, o, data[o:o + MAX_CHUNK]))
    s.run()                                     # its ack lost: STATUS's done says where it is
    s.ask(status(s.next_seq()))
    seq = s.next_seq()
    s.ask(write_end(seq, tid, zlib.crc32(data)))
    s.ask(write_end(seq, tid, zlib.crc32(data)))
    s.settle()
    s.set(recording=True)
    seq = s.next_seq()
    s.ask(delete(seq, 5, 5))                    # BUSY_RECORDING, not remembered:
    s.set(recording=False)
    s.ask(delete(seq, 5, 5))                    # the same SEQ again is done now
    s.ask(delete(seq, 5, 5))                    # and that one remembered
    out.append(s.done())

    s = Session("pauses", "A write paused by a recording, a load and a save, its acks while paused and "
                "the OK ack as each ends; a remount meanwhile (a failed save's) loses nothing", base)
    data = stereo(9000, 19)  # 36 KB: a block written, the rest at the end
    a = s.ask(write_begin(s.next_seq(), 6, 0, 0, len(data)))
    tid = a.data[0]
    s.next = 0
    s.stream(tid, data, WINDOW * MAX_CHUNK)
    s.set(recording=True)
    s.app(chunk(tid, s.next, data[s.next:s.next + MAX_CHUNK]))  # during the pause: a gap
    s.run()
    s.run(124)
    s.run()                                     # 250 ms on: the next paused ack
    s.ask(status(s.next_seq()))
    s.set(recording=False)
    s.run()                                     # the OK ack
    for state in ("loading", "card_busy"):
        s.stream(tid, data, s.next + WINDOW * MAX_CHUNK)
        s.set(**{state: True})
        s.run(3)
        s.set(**{state: False})
        s.run()
    s.stream(tid, data, 34000)
    s.remount()                                 # a block is on the card, synced
    s.stream(tid, data)
    s.set(recording=True)
    s.ask(write_end(s.next_seq(), tid, zlib.crc32(data)))
    s.run(5)                                    # finishing waits for the recording
    s.ask(status(s.next_seq()))
    s.set(recording=False)
    s.settle()
    s.ask(status(s.next_seq()))
    s.ask(slot(s.next_seq(), 6, 0))
    out.append(s.done())

    s = Session("timeout", "No CHUNK for 30 s: TIMED_OUT, its ack, the temp file deleted; paused time "
                "doesn't count; USB gone ends a write (ABORTED)", base)
    data = stereo(300, 23)
    a = s.ask(write_begin(s.next_seq(), 7, 0, 0, len(data)))
    tid = a.data[0]
    s.app(chunk(tid, 0, data[:MAX_CHUNK]))
    s.run()
    s.set(recording=True)
    s.run(10000)                                # 20 s paused: doesn't count
    s.set(recording=False)
    s.run(14990)
    s.ask(status(s.next_seq()))
    s.run(20)                                   # 30 s since the pause ended
    s.ask(status(s.next_seq()))
    s.settle()
    a = s.ask(write_begin(s.next_seq(), 7, 0, 0, len(data)))
    s.set(usb=False)
    s.run()
    s.set(usb=True)
    s.ask(status(s.next_seq()))
    s.settle()
    out.append(s.done())

    s = Session("refusals", "The common refusals in their order: NO_CARD, CARD_LENT, BUSY_RECORDING, "
                "BUSY_LOADING, BUSY_SCANNING, BUSY_JOB, BUSY_CARD; read-only commands answered in every "
                "state; lending ends a write (CARD_LENT)", base)
    for state in ("card_busy", "loading", "recording"):
        s.set(**{state: True})
        s.ask(delete(s.next_seq(), 0, 0))
    s.ask(rescan(s.next_seq()))                 # still recording
    s.set(recording=False, loading=False, card_busy=False)
    s.ask(rescan(s.next_seq()))
    s.ask(delete(s.next_seq(), 0, 0))           # BUSY_SCANNING
    s.ask(bank(s.next_seq(), 0))                # scanning: 1
    s.ask(status(s.next_seq()))
    s.settle()
    data = stereo(300, 29)
    tid = s.write(3, 3, data, end=False)
    s.ask(delete(s.next_seq(), 0, 0))           # BUSY_JOB
    s.set(lent=True)
    s.run()                                     # the write's last ack: CARD_LENT
    s.ask(delete(s.next_seq(), 0, 0))
    s.ask(status(s.next_seq()))
    s.ask(slot(s.next_seq(), 0, 0))
    s.set(lent=False)
    s.ask(delete(s.next_seq(), 0, 0))           # scanning after the card came back
    s.settle()
    s.set(card=False)
    s.ask(delete(s.next_seq(), 0, 0))
    s.ask(status(s.next_seq()))
    s.set(card=True)
    out.append(s.done())

    s = Session("moves", "RENAME, SWAP, DELETE and their refusals; the slots after each, the names going "
                "with the files", base)
    s.ask(rename(s.next_seq(), 0, 0, 8, 0))     # NOT_A_SLOT: a cubbi
    s.ask(rename(s.next_seq(), 0, 9, 1, 0))     # EMPTY
    s.ask(rename(s.next_seq(), 0, 0, 0, 1))     # EXISTS
    s.ask(rename(s.next_seq(), 0, 0, 2, 7))
    s.ask(slot(s.next_seq(), 2, 7))
    s.ask(name(s.next_seq(), 2, 7))
    s.ask(rename(s.next_seq(), 2, 7, 0, 1, REPLACE))
    s.ask(swap(s.next_seq(), 0, 0, 0, 2))       # EMPTY: both
    s.ask(swap(s.next_seq(), 0, 1, 0, 2))       # one empty: a move
    s.put("/FRIZZ/a5.wav", loop_b)
    s.ask(rescan(s.next_seq()))
    s.settle()
    s.ask(swap(s.next_seq(), 0, 2, 0, 4))
    s.ask(bank(s.next_seq(), 0))
    s.ask(name(s.next_seq(), 0, 2))
    s.ask(name(s.next_seq(), 0, 4))
    s.ask(swap(s.next_seq(), 8, 0, 0, 2))       # NOT_A_SLOT
    s.ask(delete(s.next_seq(), 0, 2))
    s.ask(delete(s.next_seq(), 0, 2))           # EMPTY
    s.ask(delete(s.next_seq(), 9, 0))           # NOT_A_SLOT
    s.put("/FRIZZ/A9.WAV", loop_a)              # FAT's names in any case
    s.ask(rescan(s.next_seq()))
    s.settle()
    s.ask(rename(s.next_seq(), 0, 8, 1, 8))
    out.append(s.done())

    s = Session("copy", "COPY of TAPE's cubbis, in /TAPE, into FRIZZ slots, and its refusals", base)
    s.ask(copy(s.next_seq(), 0, 0, 1, 0))       # NOT_A_SLOT: not a cubbi
    s.ask(copy(s.next_seq(), 8, 0, 8, 1))       # NOT_A_SLOT: not a FRIZZ slot
    s.ask(copy(s.next_seq(), 8, 5, 1, 0))       # EMPTY
    s.ask(copy(s.next_seq(), 8, 1, 1, 0))       # BAD_WAV (44.1 kHz)
    s.ask(copy(s.next_seq(), 8, 0, 0, 0))       # EXISTS
    a = s.ask(copy(s.next_seq(), 8, 0, 1, 0))
    s.ask(status(s.next_seq()))
    s.settle()
    s.ask(status(s.next_seq()))
    s.ask(slot(s.next_seq(), 1, 0))
    s.ask(copy(s.next_seq(), 8, 0, 0, 0, REPLACE))
    s.settle()
    a = s.ask(copy(s.next_seq(), 8, 0, 1, 1))
    s.ask(abort(s.next_seq(), a.data[0]))
    s.settle()
    s.ask(status(s.next_seq()))
    out.append(s.done())

    root = {"/cubbi_c3.wav": stereo(120, 31), "/FRIZZ/b1.wav": loop_a}
    s = Session("copy-root", "Stock TAPE's cubbis in the card's root (no /TAPE): STATUS says so, COPY "
                "reads them there", root)
    s.ask(status(s.next_seq()))
    s.ask(bank(s.next_seq(), 10))
    s.ask(copy(s.next_seq(), 10, 2, 7, 14))
    s.settle()
    s.ask(slot(s.next_seq(), 7, 14))
    out.append(s.done())

    s = Session("bad-wav", "Written files FRIZZ won't play: BAD_WAV with the walk's result, the slot as "
                "it was; a file put in the slot meanwhile: EXISTS at the end", base)
    s.write(3, 0, wav(pattern(400, 37), 2, rate=44100))
    s.ask(status(s.next_seq()))
    s.write(3, 0, b"RIFX" + bytes(60))
    s.ask(status(s.next_seq()))
    data = stereo(100, 41)
    tid = s.write(3, 0, data, end=False)
    s.put("/FRIZZ/d1.wav", loop_b)
    s.ask(write_end(s.next_seq(), tid, zlib.crc32(data)))
    s.settle()
    s.ask(status(s.next_seq()))
    s.ask(bank(s.next_seq(), 3))
    out.append(s.done())

    s = Session("no-fsinfo", "A card without FSINFO's free count: counted a FAT sector a pass, "
                "BUSY_SCANNING until then", base, clusters=2000, fsinfo=False)
    out.append(s.done())
    s2 = Session("no-fsinfo-busy", "The count still running: changing commands get BUSY_SCANNING, "
                 "STATUS says the free space isn't known", base, settle=False, clusters=60000, fsinfo=False)
    s2.run(30)
    s2.ask(delete(s2.next_seq(), 0, 0))
    s2.ask(status(s2.next_seq()))
    s2.settle()
    s2.ask(status(s2.next_seq()))
    out.append(s2.done())

    s = Session("power-cut-write", "Power cuts in a replacing write: xfer.tmp written; the old file "
                "deleted, before the rename (the slot empty); the rename done. Each boot leaves whole "
                "files only, xfer.tmp deleted", base)
    data = stereo(300, 43, name_text="New")
    for cut in (0, 1, 1):
        a = s.ask(write_begin(s.next_seq(), 0, 0, REPLACE, len(data)))
        tid = a.data[0]
        for o in range(0, len(data), MAX_CHUNK):
            s.app(chunk(tid, o, data[o:o + MAX_CHUNK]))
        s.run()
        s.cut(cut)
        if cut:
            s.app(write_end(s.next_seq(), tid, zlib.crc32(data)))
            s.run(20)
        s.boot()
        s.ask(slot(s.next_seq(), 0, 0))
    out.append(s.done())

    s = Session("power-cut-swap", "Power cuts in a swap after each of its renames; the scan at boot puts "
                "the .swp right: undone, finished, done", base)
    for cut in (1, 2, 3):
        s.cut(cut)
        s.app(swap(s.next_seq(), 0, 0, 0, 1))
        s.run()
        s.boot()
        s.ask(bank(s.next_seq(), 0))
        s.ask(name(s.next_seq(), 0, 0))
    s.put("/FRIZZ/a3-a4.swp", loop_a)           # both its ends taken: left alone
    s.put("/FRIZZ/a3.wav", loop_b)
    s.put("/FRIZZ/a4.wav", loop_b)
    s.put("/FRIZZ/b2-c3.swp", loop_b)           # to free: the swap finishes
    s.boot()
    s.ask(bank(s.next_seq(), 0))
    s.ask(bank(s.next_seq(), 2))
    out.append(s.done())

    s = Session("boot-tmp", "A temp file a power cut left is deleted at boot", dict(base, **{TMP: b"half"}))
    out.append(s.done())

    s = Session("bench", "FRIZZ-bench.bin: HELLO, NOT_HERE to the rest", base, bench=True)
    s.ask(hello(s.next_seq()))
    s.ask(status(s.next_seq()))
    s.ask(write_begin(s.next_seq(), 0, 0, 0, 1000))
    s.ask(request(HELLO, s.next_seq(), [1]))
    out.append(s.done())

    return out


def vectors_json():
    v = dict(
        format=1, protocol=[MAJOR, MINOR],
        about="docs/LOOP_PROTOCOL.md 8.1: scripted sessions, the bytes each side sends. Written by "
              "firmware/tools/loopxfer.py vectors; don't edit by hand.",
        pass_ms=PASS_MS, sessions=sessions())
    return json.dumps(v, indent=1, ensure_ascii=True) + "\n"


# ---- the selftest: the client against the model over a lossy link ----

def selftest(batches, seed, loss=0.03):
    rng = random.Random(seed)
    loss = dict(drop=loss, repeat=loss, cut=loss * 2 / 3, lose=loss)
    allowed = lambda p: (p == TMP or swap_of(p.split("/")[-1]) is not None
                         or (p.startswith("/FRIZZ/") and slot_of(p[7:], False) is not None))
    failures = 0
    totals = dict(writes=0, bytes=0, chunks=0, rewinds=0, ops=0, ms=0)
    for n in range(batches):
        files = {"/TAPE/cubbi_a1.wav": stereo(rng.randint(10, 400), n), "/FRIZZ/frizz_scenes.txt": b"s",
                 "/notes.txt": b"n"}
        for _ in range(rng.randint(0, 6)):
            files["/FRIZZ/%s.wav" % slot_name(rng.randrange(8), rng.randrange(15))] = \
                stereo(rng.randint(10, 300), rng.randrange(1000))
        card = Card(files, ["/FRIZZ"])
        frizz = Frizz(card)
        frizz.settle()
        client = Client(ModelLink(frizz, loss, random.Random(rng.random())), random.Random(rng.random()))
        before = dict(card.files)
        expect = {}  # what the model's slots should end as: path -> bytes or None
        try:
            client.hello()
            for _ in range(rng.randint(1, 5)):
                listing = client.listing()
                filled = [k for k in listing if k[0] < FRIZZ_BANKS]
                b, s = rng.randrange(8), rng.randrange(15)
                what = rng.choice(["write", "write", "rename", "swap", "delete", "copy"])
                try:
                    if what == "write":
                        data = (stereo if rng.random() < .6 else mono)(rng.randint(10, 1500), rng.randrange(9999),
                                                                       name_text="loop %d" % n)
                        client.write(b, s, data, replace=True)
                        if card.files.get(card.find(slot_path(b, s, False))) != data:
                            raise LoopError("write %s: the card's file isn't what was sent" % slot_name(b, s), BAD)  # noqa: F821
                        expect[(b, s)] = data
                        totals["writes"] += 1
                        totals["bytes"] += len(data)
                    elif what == "rename" and filled:
                        fb, fs = rng.choice(filled)
                        if (fb, fs) != (b, s):
                            client.rename(fb, fs, b, s, replace=True)
                    elif what == "swap" and filled:
                        fb, fs = rng.choice(filled)
                        if (fb, fs) != (b, s):
                            client.swap(fb, fs, b, s)
                    elif what == "delete" and filled:
                        client.delete(*rng.choice(filled))
                    elif what == "copy":
                        client.copy(8, 0, b, s, replace=True)
                except LoopError as e:
                    if e.status not in (EXISTS, EMPTY):  # noqa: F821
                        raise
            client.rescan()
            listing = client.listing()
            totals["chunks"] += client.stats["chunks"]
            totals["rewinds"] += client.stats["rewinds"]
            totals["ms"] += frizz.ms
        except (LoopError, TimeoutError) as e:
            failures += 1
            print("batch %d: %s" % (n, e))
            continue
        # the card: whole files only, only the allowed names changed, the listing as the card is
        changed = {p for p in set(before) | set(card.files) if before.get(p) != card.files.get(p)}
        bad = [p for p in changed if not allowed(p)]
        if TMP in card.files:
            bad.append(TMP + " left")
        for p, d in card.files.items():
            at = slot_of(p[7:], False) if p.startswith("/FRIZZ/") else None
            if at and Walk(d).result not in (MONO, STEREO):
                bad.append(p + " isn't a whole loop")
        for (b, s), info in listing.items():
            p = card.find(slot_path(b, s, True))
            if p not in card.files or info["size"] != len(card.files[p]):
                bad.append("the listing's %s isn't the card's" % slot_name(b, s))
        if bad:
            failures += 1
            print("batch %d: %s" % (n, "; ".join(bad)))
    print("%d batches, %d failed: %d writes (%d KB in %d CHUNKs, %d rewinds), %.0f s of FRIZZ's time" % (
        batches, failures, totals["writes"], totals["bytes"] // 1024, totals["chunks"], totals["rewinds"],
        totals["ms"] / 1000))
    return failures == 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("what", nargs="?", choices=["vectors"])
    ap.add_argument("--out", default=VECTORS)
    ap.add_argument("--check", action="store_true", help="vectors: compare with the file instead")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--batches", type=int, default=300)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--loss", type=float, default=0.03, help="selftest: each message's chance of each loss")
    a = ap.parse_args()
    if a.selftest:
        sys.exit(0 if selftest(a.batches, a.seed, a.loss) else 1)
    if a.what == "vectors":
        text = vectors_json()
        if a.check:
            try:
                with open(a.out) as f:
                    same = f.read() == text
            except OSError:
                same = False
            if not same:
                sys.exit("%s isn't what loopxfer.py vectors writes: run it, and commit the file" % a.out)
            return
        os.makedirs(os.path.dirname(a.out), exist_ok=True)
        with open(a.out, "w") as f:
            f.write(text)
        print("wrote %s" % os.path.relpath(a.out))
        return
    ap.print_help()


if __name__ == "__main__":
    main()
