#!/usr/bin/env python3
"""fxchain_tu.py: after the firmware's link, checks that FxChain::Run's code is FxChain.o's.

    tools/fxchain_tu.py MAP OBJ [NM]    MAP the link's map (built with --cref), OBJ FxChain.o,
                                        NM the nm to read it with (arm-none-eabi-nm)

FxChain::Run, the effects' per-sample code, is a translation unit of its own (FxChain.cpp,
code/src/FxChainRun.h, #286), so its machine code follows only its own sources: compiled
with the rest of chompi_main.cpp, it changed with code the audio never runs. What it calls
inline but out of line (Insert<>, Shifter::Process, FxOutput::Slew ...) is emitted by every
object that uses it, and ld keeps the first copy it meets: FxChain.o's, as the Makefile links
it first. If chompi_main.o's copy won (the Makefile's sources reordered), that code would be
chompi_main.cpp's again, so this fails the build, naming each function whose copy came from
elsewhere. The map's cross reference table names first the file a symbol's definition was
taken from. Run by code/src/Makefile after each link; exits 1 on a failure.
"""
import subprocess
import sys


def definers(map_path):
    """Each symbol in the map's cross reference table: the file its definition came from"""
    out = {}
    with open(map_path, errors="replace") as f:
        lines = f.read().split("\n")
    try:
        start = lines.index("Cross Reference Table")
    except ValueError:
        sys.exit("fxchain_tu: %s has no cross reference table (link with --cref)" % map_path)
    for line in lines[start + 1:]:
        if not line or line.startswith(" ") or line.startswith("Symbol "):
            continue
        # "symbol   file", the symbol demangled (it may hold spaces), the file the last word
        sym, _, path = line.rstrip().rpartition(" ")
        out[sym.strip()] = path
    return out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    map_path, obj = sys.argv[1], sys.argv[2]
    nm = sys.argv[3] if len(sys.argv) > 3 else "arm-none-eabi-nm"
    # the functions FxChain.o defines for others to see (T, W): the local ones (t) are its own
    names = subprocess.run([nm, "-C", "--defined-only", obj], check=True, capture_output=True,
                           text=True).stdout.split("\n")
    funcs = [l.split(" ", 2)[2] for l in names if l[9:10] in ("T", "W")]
    if not any(f.startswith("chompi::FxChain::Run(") for f in funcs):
        sys.exit("fxchain_tu: %s doesn't define FxChain::Run" % obj)
    taken = definers(map_path)
    wrong = [(f, taken[f]) for f in funcs if f in taken and taken[f] != obj]
    for f, path in wrong:
        print("fxchain_tu: %s is %s's copy, not %s's (#286: FxChain.cpp first in CPP_SOURCES)"
              % (f, path, obj))
    if wrong:
        sys.exit(1)
    print("fxchain_tu: FxChain::Run and the %d functions beside it, all %s's" % (len(funcs) - 1, obj))


if __name__ == "__main__":
    main()
