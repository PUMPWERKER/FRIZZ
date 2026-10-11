#!/bin/bash
# lib.sh: what the test scripts share; sourced, not run. Sets T (this folder), REPO, INC (the
# DaisySP include flags) and BUILD, builds DaisySP for the host when needed, and provides
# units (the unit checks' names), twin_for NAME (below) and unit_test NAME: builds NAME.cpp
# against the working tree's headers, with the host MidiClock in place of the real one (it
# opens MIDI), and runs it. FRIZZ's code is built with -funsigned-char, as on the CHOMPI, where
# char is unsigned (on x86 it's signed); DaisySP needn't be, it doesn't depend on it.
set -e -o pipefail
T=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(git -C "$T" rev-parse --show-toplevel)
DAISYSP=$REPO/firmware/code/libs/DaisySP/Source
BUILD=$T/build
INC=$(find "$DAISYSP" -type d | sed 's/^/-I/' | tr '\n' ' ')
mkdir -p "$BUILD"

# DaisySP for the host, rebuilt when its sources or the compiler change
STAMP=$( (g++ --version; find "$DAISYSP" -name '*.cpp' -o -name '*.h' | sort | xargs cat) | md5sum | cut -d' ' -f1)
if [ ! -f "$BUILD/libdaisysp_host.a" ] || [ "$(cat "$BUILD/daisysp.stamp" 2>/dev/null)" != "$STAMP" ]; then
    echo "building DaisySP for the host"
    rm -rf "$BUILD/daisysp" "$BUILD/libdaisysp_host.a"
    mkdir -p "$BUILD/daisysp"
    for f in $(find "$DAISYSP" -name '*.cpp'); do
        g++ -O2 -std=gnu++14 -w -c "$f" $INC -o "$BUILD/daisysp/$(basename "$f" .cpp).o"
    done
    ar rcs "$BUILD/libdaisysp_host.a" "$BUILD"/daisysp/*.o
    echo "$STAMP" > "$BUILD/daisysp.stamp"
fi

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
    log=$dir/build.log
    if ! g++ -O2 -std=gnu++14 -funsigned-char -Wall -Wno-unused-function -Wno-unused-variable -I"$dir" \
        -I"$T/host" $INC "$T/$name.cpp" "$BUILD/libdaisysp_host.a" -o "$dir/$name" 2> "$log"; then
        cat "$log"
        echo "$name.cpp didn't build"
        exit 1
    fi
    grep -A3 "$name.cpp" "$log" || true
    "$dir/$name"
}
