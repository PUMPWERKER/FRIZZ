#!/bin/bash
# check.sh [ref a] [ref b]: runs the harness on two versions of the engine and compares them.
# Defaults: HEAD against the working tree, the check for a change that shouldn't alter the
# sound. Exits 0 when bit-identical. The two are built and run side by side.
source "$(dirname "$0")/lib.sh"
A=${1:-HEAD}
B=${2:-work}
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT
"$T/run.sh" "$A" "$D/a.bin" & PA=$!
"$T/run.sh" "$B" "$D/b.bin" & PB=$!
RA=0 RB=0
wait $PA || RA=$?
wait $PB || RB=$?
[ $RA -eq 0 ] || { echo "the harness didn't run on $A"; exit 1; }
[ $RB -eq 0 ] || { echo "the harness didn't run on $B"; exit 1; }
echo "$A vs $B:"
python3 "$T/compare.py" "$D/a.bin" "$D/b.bin"
