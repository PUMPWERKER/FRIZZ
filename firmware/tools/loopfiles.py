#!/usr/bin/env python3
"""loopfiles.py: test files for the loop loader (code/src/LoopLoader.h, #70), for a test on the
device. Nothing here is for players: the files are patterns, not music. FRIZZ's loops live in
/FRIZZ as a1.wav ... h15.wav; this writes only there, never TAPE's cubbis (/TAPE, the root).

    tools/loopfiles.py make DIR         the set into DIR (DIR/a1.wav ...), checked by
                                        remote.py loops load --check
    tools/loopfiles.py card DIR ROOT    onto the card mounted at ROOT (card.py mount, in a hold):
                                        DIR's files copied into ROOT/FRIZZ, and a2, a12 and a13
                                        written in pieces between a filler's, which is then
                                        deleted, so they lie in fragments; refused if one of
                                        the set's names is there
    tools/loopfiles.py clean ROOT       the files this makes (card, page) deleted from
                                        ROOT/FRIZZ, only where their bytes are this tool's: a
                                        loop of your own by the same name stays
    tools/loopfiles.py page ROOT        the Loops page's set onto the card mounted at ROOT:
                                        the loops twin/scenarios/loops.txt puts on the twin's
                                        card (its `card wav` lines), so it plays on the device
                                        as on the twin (remote.py play); refused if one of
                                        them is there, the rest of /FRIZZ left alone
    tools/loopfiles.py move ROOT        the layout before 1.1's release onto the new one:
                                        ROOT/FRIZZ/LOOPS/A/01.wav to ROOT/FRIZZ/a1.wav and so
                                        on, refused where a name is taken; the emptied folders
                                        removed, anything else in them left there

The set, by slot, and what FRIZZ's scan says (LoopLoader's Result):
    a1   16 bars at 50 BPM (3,686,400 frames), stereo, the most it takes      stereo
    a2   the same data, in 57 fragments on the card (256 KB pieces)            stereo
    a3   44.1 kHz                                                              not 48 kHz
    a4   80 s, longer than 16 bars at 50 BPM                                   too long
    a5   mono, 10 s                                                            mono
    a6   24-bit                                                                not 16-bit
    a7   a LIST chunk before the data (the data at 70)                         stereo
    a8   a 1,500-byte bext chunk before the data (the data at 1,552)           stereo
    a9   WAVE_FORMAT_EXTENSIBLE with PCM as its subformat (the data at 68)     stereo
    a10  its header says 10 s, the file holds 1 s                              past the end
    a11  16 bars at 50 BPM with its data at 504                                stereo
    a12  a1's data in ~450 fragments (32 KB pieces)       stereo at the scan; its load refused
    a13  a1's data in ~113 fragments (128 KB pieces)                           stereo
    b1-b15  0.5 s each                                                         stereo
    c-h  none

The data: little-endian 32-bit words, word j = j * 2654435761 ^ 0x5A5A5A5A (mod 2^32), as
#70's measurement had them; each file's data starts at word 0.
"""
import math
import os
import shutil
import struct
import sys

FRAMES_16_BARS = 3686400  # 16 bars at 50 BPM, 48 kHz
# the files written in pieces on the card, and the pieces' size
PIECES = {"a2.wav": 256 * 1024, "a12.wav": 32 * 1024, "a13.wav": 128 * 1024}
FILLER = "loopfiles-filler.bin"


def pattern(nbytes):
    words = (nbytes + 3) // 4
    # numpy-free, in blocks, fast enough for ~15 MB
    out = bytearray()
    block = 1 << 16
    for start in range(0, words, block):
        n = min(block, words - start)
        out += struct.pack("<%dI" % n, *(((j * 2654435761) ^ 0x5A5A5A5A) & 0xFFFFFFFF
                                          for j in range(start, start + n)))
    return bytes(out[:nbytes])


def chunk(cid, body):
    return cid + struct.pack("<I", len(body)) + body + (b"\0" if len(body) & 1 else b"")


def fmt(channels=2, rate=48000, bits=16, extensible=False):
    align = channels * bits // 8
    body = struct.pack("<HHIIHH", 0xFFFE if extensible else 1, channels, rate, rate * align,
                       align, bits)
    if extensible:
        body += struct.pack("<HHI", 22, bits, 3 if channels == 2 else 4)
        body += bytes.fromhex("0100000000001000800000aa00389b71")  # KSDATAFORMAT_SUBTYPE_PCM
    return chunk(b"fmt ", body)


def wav(data, channels=2, rate=48000, bits=16, extensible=False, before=b"", size=None):
    """A WAV: fmt, the chunks BEFORE the data, the data; SIZE: the data chunk's length as the
    header says it, if not the data's"""
    d = b"data" + struct.pack("<I", len(data) if size is None else size) + data
    body = b"WAVE" + fmt(channels, rate, bits, extensible) + before + d
    return b"RIFF" + struct.pack("<I", len(body)) + body


def the_set():
    """{name: bytes}, the names in /FRIZZ"""
    sec = 48000 * 4
    big = pattern(FRAMES_16_BARS * 4)
    files = {
        "a1.wav": wav(big),
        "a3.wav": wav(pattern(2 * 44100 * 4), rate=44100),
        "a4.wav": wav(pattern(80 * sec)),
        "a5.wav": wav(pattern(10 * 48000 * 2), channels=1),
        "a6.wav": wav(pattern(2 * 48000 * 6), bits=24),
        "a7.wav": wav(pattern(5 * sec), before=chunk(b"LIST", b"INFOISFT" + struct.pack("<I", 6) + b"FRIZZ\0")),
        "a8.wav": wav(pattern(5 * sec), before=chunk(b"bext", b"\0" * 1500)),
        "a9.wav": wav(pattern(5 * sec), extensible=True),
        "a10.wav": wav(pattern(1 * sec), size=10 * sec),
        "a11.wav": wav(big, before=chunk(b"junk", b"\0" * (504 - 44 - 8))),
    }
    for s in range(1, 16):
        files["b%d.wav" % s] = wav(pattern(sec // 2))
    # written in pieces on the card (card): their bytes here are what FRIZZ must load
    files["a2.wav"] = files["a12.wav"] = files["a13.wav"] = files["a1.wav"]
    return files


def make(out):
    os.makedirs(out, exist_ok=True)
    for name, data in the_set().items():
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)
    print("made %s" % out)


def fragmented(dst, data, piece, filler):
    """data into dst, a piece at a time, each followed by as much into filler: on a fresh
    stretch of FAT the two files' clusters alternate"""
    with open(dst, "wb") as f, open(filler, "wb") as g:
        for at in range(0, len(data), piece):
            f.write(data[at:at + piece])
            f.flush()
            os.fsync(f.fileno())
            g.write(b"\0" * piece)
            g.flush()
            os.fsync(g.fileno())


def taken(frizz, names):
    """Those of names /FRIZZ has already, in any case (FAT's names don't tell case apart)"""
    there = {n.lower() for n in os.listdir(frizz)} if os.path.isdir(frizz) else set()
    return [n for n in names if n.lower() in there]


def card(src, root):
    frizz = os.path.join(root, "FRIZZ")
    names = sorted(the_set())
    there = taken(frizz, names + [FILLER])
    if there:
        sys.exit("%s already in %s: not made by this, not touched" % (", ".join(there), frizz))
    os.makedirs(frizz, exist_ok=True)
    for name in names:
        if name not in PIECES:
            shutil.copyfile(os.path.join(src, name), os.path.join(frizz, name))
    with open(os.path.join(src, "a1.wav"), "rb") as f:
        data = f.read()
    filler = os.path.join(frizz, FILLER)
    for name, piece in PIECES.items():
        fragmented(os.path.join(frizz, name), data, piece, filler)
        os.remove(filler)
    os.sync()
    print("put on the card in %s" % frizz)


SCENARIO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "twin", "scenarios", "loops.txt")


def sine(seconds, channels, hz, beats=0):
    """twin/script.cpp's LoopWav: a sine at .3, the right a quarter period on, an acid chunk
    of BEATS before the data"""
    frames = int(seconds * 48000 + .5)
    out = bytearray()
    for i in range(frames):
        for c in range(channels):
            out += struct.pack("<h", int(round(.3 * 32767 * math.sin(2 * math.pi * hz * i / 48000 + c * math.pi / 2))))
    acid = chunk(b"acid", struct.pack("<IHHfIHHf", 0, 60, 0, 0., beats, 4, 4, 120.)) if beats else b""
    return wav(bytes(out), channels=channels, before=acid)


def page_set():
    """{name: bytes}: the scenario's `card wav` lines, its loops in /FRIZZ"""
    with open(SCENARIO) as f:
        lines = [line.split() for line in f if line.split()[:2] == ["card", "wav"]]
    files = {}
    for w in lines:
        if not w[2].startswith("/FRIZZ/") or w[2].count("/") != 2:
            sys.exit("%s: only FRIZZ's own loops, in /FRIZZ" % w[2])
        files[w[2][len("/FRIZZ/"):]] = sine(float(w[3]), int(w[4]), float(w[5]), int(w[6]) if len(w) > 6 else 0)
    return files


def page(root):
    frizz = os.path.join(root, "FRIZZ")
    files = page_set()
    # only where there's none yet: the rest of /FRIZZ is left as it is
    there = taken(frizz, sorted(files))
    if there:
        sys.exit("%s already in %s: not made by this, not touched" % (", ".join(there), frizz))
    os.makedirs(frizz, exist_ok=True)
    for name, data in files.items():
        with open(os.path.join(frizz, name), "wb") as g:
            g.write(data)
        print("  /FRIZZ/%s" % name)
    os.sync()
    print("put on the card in %s" % frizz)


def clean(root):
    """The set's and the page's files deleted from /FRIZZ, only those whose bytes are this
    tool's"""
    frizz = os.path.join(root, "FRIZZ")
    if not os.path.isdir(frizz):
        return
    ours = {}
    for files in (the_set(), page_set()):
        for name, data in files.items():
            ours.setdefault(name, []).append(data)
    gone = []
    for name in sorted(os.listdir(frizz)):
        path = os.path.join(frizz, name)
        if name == FILLER:
            os.remove(path)
            gone.append(name)
            continue
        if name.lower() not in ours or not os.path.isfile(path):
            continue
        with open(path, "rb") as f:
            data = f.read()
        if any(data == d for d in ours[name.lower()]):
            os.remove(path)
            gone.append(name)
    os.sync()
    print("deleted from %s: %s" % (frizz, ", ".join(gone) if gone else "nothing"))


def move(root):
    """/FRIZZ/LOOPS/A/01.wav ... H/15.wav to /FRIZZ/a1.wav ... h15.wav"""
    loops = os.path.join(root, "FRIZZ", "LOOPS")
    frizz = os.path.join(root, "FRIZZ")
    if not os.path.isdir(loops):
        sys.exit("%s isn't there: nothing to move" % loops)
    moves = []
    for bank in sorted(os.listdir(loops)):
        folder = os.path.join(loops, bank)
        if len(bank) != 1 or bank.upper() not in "ABCDEFGH" or not os.path.isdir(folder):
            continue
        for name in sorted(os.listdir(folder)):
            stem, ext = os.path.splitext(name)
            if ext.lower() == ".wav" and len(stem) == 2 and stem.isdigit() and 1 <= int(stem) <= 15:
                moves.append((os.path.join(folder, name), "%s%d.wav" % (bank.lower(), int(stem))))
    there = taken(frizz, [m[1] for m in moves])
    if there:
        sys.exit("%s already in %s: nothing moved" % (", ".join(there), frizz))
    for src, name in moves:
        os.rename(src, os.path.join(frizz, name))
        print("  %s -> /FRIZZ/%s" % (os.path.relpath(src, root), name))
    for bank in sorted(os.listdir(loops)):
        folder = os.path.join(loops, bank)
        if os.path.isdir(folder) and not os.listdir(folder):
            os.rmdir(folder)
    if not os.listdir(loops):
        os.rmdir(loops)
    os.sync()
    print("moved %d; %s" % (len(moves), "/FRIZZ/LOOPS removed" if not os.path.isdir(loops)
                            else "/FRIZZ/LOOPS kept: %s" % ", ".join(os.listdir(loops))))


def main():
    a = sys.argv[1:]
    if a[:1] == ["make"] and len(a) == 2:
        make(a[1])
    elif a[:1] == ["card"] and len(a) == 3:
        card(a[1], a[2])
    elif a[:1] == ["clean"] and len(a) == 2:
        clean(a[1])
    elif a[:1] == ["page"] and len(a) == 2:
        page(a[1])
    elif a[:1] == ["move"] and len(a) == 2:
        move(a[1])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
