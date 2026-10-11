#!/bin/bash
# compare.sh [A [B]]: the whole device, two versions side by side. Builds the twin on the
# firmware of A and of B (commits, tags, branches, or "work" for the working tree; default HEAD
# against work), plays every scenario in scenarios/ on both, and compares the master out and
# the LEDs: bit-identical, or from when they part, by how much, and which LEDs.
#
#   ./compare.sh                 # HEAD vs the working tree: a refactor must be bit-identical
#   ./compare.sh v0.10 HEAD      # what changed for a player since v0.10
#   SCENARIOS="looper scenes" ./compare.sh main HEAD
#
# Each version's twin is built once into build/compare/<commit> and kept. Exits 0 when every
# scenario is bit-identical; names a scenario the twin crashed in (or couldn't read), and fails.
# Uses the twin of the working tree on both firmwares.
set -e -o pipefail
source "$(dirname "$0")/ref.sh"
A=${1:-HEAD}
B=${2:-work}
TA=$(twin_dir "$A")/frizz-twin || { echo "the twin didn't build on $A"; exit 1; }
TB=$(twin_dir "$B")/frizz-twin || { echo "the twin didn't build on $B"; exit 1; }
NAMES=${SCENARIOS:-$(cd "$TW/scenarios" && ls *.txt | sed 's/\.txt$//')}
RUNS=$(mktemp -d)
trap "rm -rf '$RUNS'" EXIT
mkdir -p "$RUNS/a" "$RUNS/b"
PIDS=()
for n in $NAMES; do
    "$TA" -q -o "$RUNS/a/$n.wav" -l "$RUNS/a/$n.leds" "$TW/scenarios/$n.txt" > /dev/null &
    PIDS+=("$!:$A:$n")
    "$TB" -q -o "$RUNS/b/$n.wav" -l "$RUNS/b/$n.leds" "$TW/scenarios/$n.txt" > /dev/null &
    PIDS+=("$!:$B:$n")
done
# a scenario's failed expectations (1) are what the comparison shows; anything else is the twin
# not getting through it: a crash (a signal) or a script it couldn't read (2)
BAD=
for p in "${PIDS[@]}"; do
    rc=0
    wait "${p%%:*}" || rc=$?
    IFS=: read -r _ ref n <<< "$p"
    if [ $rc -gt 128 ]; then
        echo "scenario $n crashed on $ref ($(kill -l $((rc - 128))))"
        BAD="$BAD $n"
    elif [ $rc -gt 1 ]; then
        echo "scenario $n didn't run on $ref (exit $rc)"
        BAD="$BAD $n"
    fi
done
RAN=$(for n in $NAMES; do [[ " $BAD " == *" $n "* ]] || echo "$n"; done)
echo "$A against $B:"
[ -z "$RAN" ] || python3 "$TW/compare.py" "$RUNS/a" "$RUNS/b" $RAN
[ -z "$BAD" ]
