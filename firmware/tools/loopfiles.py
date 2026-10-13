#!/usr/bin/env python3
"""loopfiles.py: test files for the loop loader (code/src/LoopLoader.h, #70), for a test on the
device. Nothing here is for players: the files are patterns, not music.

    tools/loopfiles.py make DIR         the set into DIR (DIR/A/01.wav ...), checked by
                                        remote.py loops load --check
    tools/loopfiles.py card DIR ROOT    onto the card mounted at ROOT (card.py mount, in a hold):
                                        ROOT/FRIZZ/LOOPS made, DIR's files copied there, and A/02,
                                        A/12 and A/13 written in pieces between a filler's, which
                                        is then deleted, so they lie in fragments
    tools/loopfiles.py clean ROOT       ROOT/FRIZZ/LOOPS deleted, all of it
    tools/loopfiles.py page ROOT        the Loops page's set onto the card mounted at ROOT:
                                        the loops twin/scenarios/loops.txt puts on the twin's
                                        card (its `card wav` lines), so it plays on the device
                                        as on the twin (remote.py play)

The set, by slot, and what FRIZZ's scan says (LoopLoader's Result):
    A/01  16 bars at 50 BPM (3,686,400 frames), stereo, the most it takes     stereo
    A/02  the same data, in 57 fragments on the card (256 KB pieces)            stereo
    A/03  44.1 kHz                                                              not 48 kHz
    A/04  80 s, longer than 16 bars at 50 BPM                                   too long
    A/05  mono, 10 s                                                            mono
    A/06  24-bit                                                                not 16-bit
    A/07  a LIST chunk before the data (the data at 70)                         stereo
    A/08  a 1,500-byte bext chunk before the data (the data at 1,552)           stereo
    A/09  WAVE_FORMAT_EXTENSIBLE with PCM as its subformat (the data at 68)     stereo
    A/10  its header says 10 s, the file holds 1 s                              past the end
    A/11  16 bars at 50 BPM with its data at 504                                stereo
    A/12  A/01's data in ~450 fragments (32 KB pieces)   stereo at the scan; its load refused
    A/13  A/01's data in ~113 fragments (128 KB pieces)                         stereo
    B/01-15  0.5 s each                                                         stereo
    C     an empty folder; D-H none

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
PIECES = {"02.wav": 256 * 1024, "12.wav": 32 * 1024, "13.wav": 128 * 1024}


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
    """{path: bytes}, paths relative to /FRIZZ/LOOPS"""
    sec = 48000 * 4
    big = pattern(FRAMES_16_BARS * 4)
    files = {
        "A/01.wav": wav(big),
        "A/03.wav": wav(pattern(2 * 44100 * 4), rate=44100),
        "A/04.wav": wav(pattern(80 * sec)),
        "A/05.wav": wav(pattern(10 * 48000 * 2), channels=1),
        "A/06.wav": wav(pattern(2 * 48000 * 6), bits=24),
        "A/07.wav": wav(pattern(5 * sec), before=chunk(b"LIST", b"INFOISFT" + struct.pack("<I", 6) + b"FRIZZ\0")),
        "A/08.wav": wav(pattern(5 * sec), before=chunk(b"bext", b"\0" * 1500)),
        "A/09.wav": wav(pattern(5 * sec), extensible=True),
        "A/10.wav": wav(pattern(1 * sec), size=10 * sec),
        "A/11.wav": wav(big, before=chunk(b"junk", b"\0" * (504 - 44 - 8))),
    }
    for s in range(1, 16):
        files["B/%02d.wav" % s] = wav(pattern(sec // 2))
    # written in pieces on the card (card): their bytes here are what FRIZZ must load
    files["A/02.wav"] = files["A/12.wav"] = files["A/13.wav"] = files["A/01.wav"]
    return files


def make(out):
    for path, data in the_set().items():
        dst = os.path.join(out, path)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(data)
    os.makedirs(os.path.join(out, "C"), exist_ok=True)
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


def card(src, root):
    loops = os.path.join(root, "FRIZZ", "LOOPS")
    if os.path.exists(loops):
        sys.exit("%s is there already: not made by this, not touched" % loops)
    for bank in "ABC":
        os.makedirs(os.path.join(loops, bank))
    for bank in "AB":
        for name in sorted(os.listdir(os.path.join(src, bank))):
            if bank == "A" and name in PIECES:
                continue
            shutil.copyfile(os.path.join(src, bank, name), os.path.join(loops, bank, name))
    with open(os.path.join(src, "A", "01.wav"), "rb") as f:
        data = f.read()
    filler = os.path.join(loops, "filler.bin")
    for name, piece in PIECES.items():
        fragmented(os.path.join(loops, "A", name), data, piece, filler)
        os.remove(filler)
    os.sync()
    print("put on the card under %s" % loops)


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


def page(root):
    loops = os.path.join(root, "FRIZZ", "LOOPS")
    if os.path.exists(loops):
        sys.exit("%s is there already: not made by this, not touched" % loops)
    with open(SCENARIO) as f:
        for line in f:
            w = line.split()
            if w[:2] != ["card", "wav"]:
                continue
            path = os.path.join(root, w[2].lstrip("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as g:
                g.write(sine(float(w[3]), int(w[4]), float(w[5]), int(w[6]) if len(w) > 6 else 0))
            print("  %s" % w[2])
    os.sync()
    print("put on the card under %s" % loops)


def clean(root):
    loops = os.path.join(root, "FRIZZ", "LOOPS")
    if os.path.exists(loops):
        shutil.rmtree(loops)
        os.sync()
        print("deleted %s" % loops)


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
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
