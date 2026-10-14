#!/usr/bin/env python3
"""builds.py: FRIZZ's two builds and their md5, the name a build goes by.

    tools/builds.py              builds both (make, make BENCH=1 in code/src, GCC 10.3), prints their md5
    tools/builds.py md5          prints the md5 (and CRC-32) of the builds there are, building nothing
    tools/builds.py cpu [FILE]   files the bench's cpu.txt (default card/cpu.txt, from card.py get)
                                 as bin/cpu.txt, headed by the md5 of the build it measured

The builds aren't in git: they live in code/src/build/FRIZZ.bin and
code/src/build-bench/FRIZZ-bench.bin, and their md5 names them in a pull request, a cpu.txt
and a release's notes. Their CRC-32 is what a running FRIZZ tells of itself (IDENT,
tools/chompi.py), so flash.py and remote.py can say which build runs. That works because GCC 10.3-2021.10 builds the same source to the
same bytes, on any machine (README.md, 1. Toolchain); another compiler gives other bytes, so
builds here refuse it.

A build at a v* tag is a release's or a beta's, and reports its version to the loop transfer's
HELLO from code/src/Version.h's kFrizzVersion (docs/LOOP_PROTOCOL.md 7.1): building refuses a
tag whose version isn't kFrizzVersion (v1.5.0-beta.1 is 1, 5, 0, 1; v1.5.0 is 1, 5, 0, 0), and so
does flash.py --slot 10/13 (check_version).

cpu.txt's first line is the bench's own: the checksum of the source it was built from, which
the bench build also carries. `cpu` files it only if the bench build here carries that line
and the source still has that checksum, so the md5 it adds is that of the build measured.
It warns when a build is older than the source, and flash.py --no-build refuses to send one. Python 3 without packages; it doesn't touch
the CHOMPI (flash.py does).
"""
import argparse
import glob
import hashlib
import os
import re
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.dirname(HERE)
SRC = os.path.join(FIRMWARE, "code", "src")
TOOLCHAIN = os.path.expanduser("~/opt/gcc-arm-none-eabi-10.3-2021.10/bin")
CPU_TXT = os.path.join(FIRMWARE, "bin", "cpu.txt")


def image(bench):
    """where make (BENCH=1) puts the build"""
    return os.path.join(SRC, "build-bench/FRIZZ-bench.bin" if bench else "build/FRIZZ.bin")


def name(bench):
    return "FRIZZ-bench.bin" if bench else "FRIZZ.bin"


def md5(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def short(path):
    rel = os.path.relpath(path)
    return path if rel.startswith("..") else rel


def stale(path):
    """True if make would build the build at PATH again (make -q: by the source it depends on)"""
    path = os.path.abspath(path)
    if path not in (image(False), image(True)):
        return False
    args = ["make", "-q"] + (["BENCH=1"] if path == image(True) else [])
    return subprocess.run(args, cwd=SRC, capture_output=True).returncode == 1


def source_checksum():
    """the Makefile's BENCH_SOURCE: cksum of every *.h, *.c and *.cpp in code/src, sorted by
    name as make's $(sort) sorts (byte by byte)"""
    names = sorted(os.path.basename(p) for ext in ("*.h", "*.c", "*.cpp")
                   for p in glob.glob(os.path.join(SRC, ext)))
    data = b"".join(open(os.path.join(SRC, n), "rb").read() for n in names)
    out = subprocess.run(["cksum"], input=data, capture_output=True, check=True).stdout
    return out.split()[0].decode()


def frizz_version():
    """kFrizzVersion in code/src/Version.h: (major, minor, patch, pre-release)"""
    with open(os.path.join(SRC, "Version.h")) as f:
        m = re.search(r"kFrizzVersion\[4\] = \{(\d+), (\d+), (\d+), (\d+)\};", f.read())
    if not m:
        sys.exit("code/src/Version.h has no kFrizzVersion")
    return tuple(int(x) for x in m.groups())


def tag_version(tag):
    """v1.5.0 -> (1, 5, 0, 0), v1.5.0-beta.2 -> (1, 5, 0, 2); None for another form"""
    m = re.fullmatch(r"v(\d+)\.(\d+)\.(\d+)(?:-beta\.(\d+))?", tag)
    return m and (int(m[1]), int(m[2]), int(m[3]), int(m[4] or 0))


def version_name(v):
    return "v%d.%d.%d%s" % (v[0], v[1], v[2], "-beta.%d" % v[3] if v[3] else "")


def head_tag(where=FIRMWARE):
    """The v* tag on HEAD, None if there's none"""
    out = subprocess.run(["git", "-C", where, "describe", "--exact-match", "--tags", "--match", "v*", "HEAD"],
                         capture_output=True, text=True).stdout.strip()
    return out or None


def check_version(tag):
    """Exits unless kFrizzVersion is tag's version: a release's or a beta's build says which it
    is (the loop transfer's HELLO)"""
    want, have = tag_version(tag), frizz_version()
    if want is None:
        sys.exit("%s isn't a version's tag (vX.Y.Z, vX.Y.Z-beta.N)" % tag)
    if want != have:
        sys.exit("code/src/Version.h's kFrizzVersion is %s, the tag %s: the commit the tag is on sets "
                 "it to %s (docs/LOOP_PROTOCOL.md 7.1)" % (version_name(have), tag, ", ".join(map(str, want))))


def toolchain_env():
    """the environment for make, with GCC 10.3 first on the PATH; exits if it isn't there"""
    env = dict(os.environ)
    if os.path.isdir(TOOLCHAIN):
        env["PATH"] = TOOLCHAIN + os.pathsep + env["PATH"]
    try:
        version = subprocess.run(["arm-none-eabi-gcc", "--version"], env=env,
                                 capture_output=True, text=True).stdout
    except FileNotFoundError:
        version = ""
    if " 10.3." not in version:
        sys.exit("needs GNU Arm Embedded 10.3-2021.10 on the PATH (README.md, 1. Toolchain): "
                 "another compiler builds other bytes")
    return env


def build(bench, env=None):
    """make (BENCH=1) in code/src with GCC 10.3; the image it made"""
    env = env or toolchain_env()
    args = ["make", "-j8"] + (["BENCH=1"] if bench else [])
    print("building", name(bench))
    result = subprocess.run(args, cwd=SRC, env=env, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout[-3000:] + result.stderr[-3000:])
        sys.exit("the build failed")
    return image(bench)


def built(bench, fresh=True):
    """the build make (BENCH=1) left in code/src; exits, saying how to make it, if there's none
    or (FRESH) it's older than the source"""
    path = image(bench)
    how = "make%s in code/src with GCC 10.3, or tools/builds.py" % (" BENCH=1" if bench else "")
    if not os.path.isfile(path):
        sys.exit("no %s in %s: build it first (%s)" % (name(bench), short(os.path.dirname(path)), how))
    if fresh and stale(path):
        sys.exit("%s is older than the source: build it again (%s)" % (short(path), how))
    return path


def crc32(path):
    """zlib's CRC-32 of the build: what FRIZZ says it runs (IDENT, tools/chompi.py)"""
    with open(path, "rb") as f:
        return zlib.crc32(f.read())


def describe(path):
    """'PATH md5 X crc32 Y', with a warning if it's one of the builds here and older than the
    source"""
    line = "%s  md5 %s  crc32 %08x" % (short(path), md5(path), crc32(path))
    if stale(path):
        line += "  (older than the source: rebuild)"
    return line


def file_cpu(src):
    """copies the bench's cpu.txt to bin/cpu.txt with the md5 of the build it measured"""
    with open(src) as f:
        lines = f.read().splitlines(True)
    if not lines or not lines[0].startswith("FRIZZ cpu bench, source "):
        sys.exit("%s isn't a bench's cpu.txt" % src)
    measured = lines[0].split()[-1]
    if measured != source_checksum():
        sys.exit("%s measured source %s, but code/src is %s now: build the bench again and run "
                 "it" % (src, measured, source_checksum()))
    bench = built(True, fresh=False)  # the source checksum it carries says more
    with open(bench, "rb") as f:
        if lines[0].encode() not in f.read():
            sys.exit("%s doesn't carry source %s: rebuild it (make BENCH=1)" % (short(bench), measured))
    head = "# measured on FRIZZ-bench.bin md5 %s" % md5(bench)
    frizz = image(False)
    if os.path.isfile(frizz) and not stale(frizz):
        head += "; FRIZZ.bin from the same source md5 %s" % md5(frizz)
    out = [lines[0], head + "\n"] + [l for l in lines[1:] if not l.startswith("# measured on ")]
    with open(CPU_TXT, "w") as f:
        f.writelines(out)
    print("%s -> %s\n%s" % (short(src), short(CPU_TXT), head))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("what", nargs="?", choices=["md5", "cpu"],
                    help="md5: only print; cpu: file a bench's cpu.txt as bin/cpu.txt")
    ap.add_argument("file", nargs="?", default=os.path.join(FIRMWARE, "card", "cpu.txt"),
                    help="the cpu.txt for cpu (default card/cpu.txt)")
    args = ap.parse_args()

    if args.what == "cpu":
        file_cpu(args.file)
        return
    if args.what is None:
        tag = head_tag()
        if tag:
            check_version(tag)
        env = toolchain_env()
        for bench in (False, True):
            build(bench, env)
    for bench in (False, True):
        path = image(bench)
        print(describe(path) if os.path.isfile(path) else "%s: not built" % short(path))


if __name__ == "__main__":
    main()
