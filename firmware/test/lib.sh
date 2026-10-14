#!/bin/bash
# lib.sh: what the test scripts share; sourced, not run. Sets T (this folder), REPO, INC (the
# DaisySP include flags) and BUILD, builds DaisySP for the host when needed, and provides
# daisysp NAME (another build of it, below), units (the unit checks' names), twin_for NAME
# (below) and unit_test NAME: builds NAME.cpp
# against the working tree's headers, with the host MidiClock in place of the real one (it
# opens MIDI), and runs it (with C sources of its own, if it names them: unit_test). FRIZZ's code is built with -funsigned-char, as on the CHOMPI, where
# char is unsigned (on x86 it's signed); DaisySP needn't be, it doesn't depend on it.
set -e -o pipefail
T=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(git -C "$T" rev-parse --show-toplevel)
DAISYSP=$REPO/firmware/code/libs/DaisySP/Source
BUILD=$T/build
INC=$(find "$DAISYSP" -type d | sed 's/^/-I/' | tr '\n' ' ')
mkdir -p "$BUILD"

# DaisySP for the host, rebuilt when its sources or the compiler change: daisysp NAME [FLAGS]
# builds build/libdaisysp_NAME.a with FLAGS added. The checks link host's (no flags);
# twinlib.cpp builds pic's (-fPIC), for a shared library
daisysp()
{
    local lib=$BUILD/libdaisysp_$1.a dir=$BUILD/daisysp stamp f
    [ "$1" = host ] || dir=$BUILD/daisysp-$1
    stamp=$( (g++ --version; find "$DAISYSP" -name '*.cpp' -o -name '*.h' | sort | xargs cat) | md5sum | cut -d' ' -f1)
    [ -f "$lib" ] && [ "$(cat "$dir.stamp" 2>/dev/null)" = "$stamp" ] && return
    echo "building DaisySP for the host${2:+ ($2)}"
    rm -rf "${dir:?}" "$lib"
    mkdir -p "$dir"
    for f in $(find "$DAISYSP" -name '*.cpp'); do
        g++ -O2 -std=gnu++14 -w $2 -c "$f" $INC -o "$dir/$(basename "$f" .cpp).o"
    done
    ar rcs "$lib" "$dir"/*.o
    echo "$stamp" > "$dir.stamp"
}
daisysp host

# the unit checks: every NAME.cpp but the harness
units()
{
    for f in "$T"/*.cpp; do
        f=$(basename "$f" .cpp)
        [ "$f" = harness ] || echo "$f"
    done
}

# A check that includes twin.h runs on the virtual CHOMPI instead: the whole firmware, built by
# ../twin/build.sh (TWIN_FIRMWARE and TWIN_BUILD pick another firmware, as there). A line
# "// twin defines: FLAGS" in NAME.cpp builds a twin of its own with them, in ../twin/build/NAME
# (bench.cpp: the CPU bench's firmware). twin_for NAME builds NAME's twin (none for a check
# without one), one build.sh at a time per folder, as checks run side by side (all.sh), and
# prints its folder; fails if it didn't build (the folder's twin would be another firmware's)
twin_for()
{
    local name=$1 defines twin
    grep -q '#include "twin.h"' "$T/$name.cpp" || return 0
    defines=$(sed -n 's|^// twin defines: *||p' "$T/$name.cpp" | head -1)
    twin=${TWIN_BUILD:-$REPO/firmware/twin/build}
    [ -z "$defines" ] || twin=$REPO/firmware/twin/build/$name
    mkdir -p "$twin"
    TWIN_DEFINES="$defines" TWIN_BUILD="$twin" flock "$twin/build.lock" "$REPO/firmware/twin/build.sh" >&2 \
        || return 1
    echo "$twin"
}

# NAME.cpp's warnings are shown, the headers' only if it doesn't build: then every message is.
unit_test()
{
    local name=$1 dir log
    dir=$(mktemp -d)
    trap "rm -rf '$dir'" EXIT
    if grep -q '#include "twin.h"' "$T/$name.cpp"; then
        local twin
        twin=$(twin_for "$name") || { echo "$name: its twin didn't build"; exit 1; }
        log=$dir/build.log
        if ! g++ -O2 -std=gnu++14 -funsigned-char -Wall -I"$REPO/firmware/twin" "$T/$name.cpp" \
            "$twin/libtwin.a" "$BUILD/libdaisysp_host.a" -o "$dir/$name" 2> "$log"; then
            cat "$log"
            echo "$name.cpp didn't build"
            exit 1
        fi
        cat "$log"
        "$dir/$name"
        return
    fi
    cp "$REPO/firmware/code/src"/*.h "$dir/"
    cp "$T/host/MidiClock.h" "$dir/"
    # a line "// unit includes: DIR ..." in NAME.cpp adds those folders, from here, first
    local extra=() d
    for d in $(sed -n 's|^// unit includes: *||p' "$T/$name.cpp" | head -1); do
        extra+=(-I"$T/$d")
    done
    log=$dir/build.log
    # a line "// unit sources: FILE.c ..." in NAME.cpp compiles those C files, from here, with the
    # same folders, and links them (sddisk.cpp: FatFs and libDaisy's sd_diskio.c themselves)
    local objs=() c
    for c in $(sed -n 's|^// unit sources: *||p' "$T/$name.cpp" | head -1); do
        if ! gcc -O1 -w -funsigned-char "${extra[@]}" -I"$T/host" -c "$T/$c" -o "$dir/$(basename "$c" .c).o" 2> "$log"; then
            cat "$log"
            echo "$c didn't build"
            exit 1
        fi
        objs+=("$dir/$(basename "$c" .c).o")
    done
    # a line "// unit libs: -lNAME ..." in NAME.cpp links those system libraries (twinlib.cpp: dlopen)
    local libs
    libs=$(sed -n 's|^// unit libs: *||p' "$T/$name.cpp" | head -1)
    if ! g++ -O2 -std=gnu++14 -funsigned-char -Wall -Wno-unused-function -Wno-unused-variable "${extra[@]}" -I"$dir" \
        -I"$T/host" $INC "$T/$name.cpp" "${objs[@]}" "$BUILD/libdaisysp_host.a" $libs -o "$dir/$name" 2> "$log"; then
        cat "$log"
        echo "$name.cpp didn't build"
        exit 1
    fi
    grep -A3 "$name.cpp" "$log" || true
    "$dir/$name"
}
