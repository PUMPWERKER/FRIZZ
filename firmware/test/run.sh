#!/bin/bash
# run.sh <git ref | work> <out.bin>
# Builds the harness against FRIZZ's engine headers at that git ref (or the working tree) and
# runs it, writing every output sample and FX meter to <out.bin>. Needs a host g++.
source "$(dirname "$0")/lib.sh"

# that version's source, FRIZZ's and the platform's (sources.sh), with the host MidiClock in place
# of the real one (it opens MIDI)
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT
firmware_sources "$1" "$D"
# and that version's harness, which drives its engine's interface; the working tree's for refs
# that don't have one
HARNESS=$T/harness.cpp
if [ "$1" != work ]; then
    # FRIZZ lived in firmware/frizz/ before the repo was reorganized
    TEST=firmware/test
    git -C "$REPO" cat-file -e "$1:$TEST" 2>/dev/null || TEST=firmware/frizz/test
    if git -C "$REPO" show "$1:$TEST/harness.cpp" > "$D/harness.cpp" 2>/dev/null; then
        HARNESS=$D/harness.cpp
    fi
fi
cp "$T/host/MidiClock.h" "$D/"

# -ffp-contract=off: no fused multiply-adds, so builds compare bit for bit. Warnings only for
# the working tree: an old ref's are history
WARN=-w
[ "$1" = work ] && WARN="-Wall -Wno-unused-function -Wno-unused-variable"
g++ -O2 -std=gnu++14 -ffp-contract=off -funsigned-char $WARN -I"$D" -I"$T/host" $INC "$HARNESS" \
    "$BUILD/libdaisysp_host.a" -o "$D/harness"
"$D/harness" "$2"
