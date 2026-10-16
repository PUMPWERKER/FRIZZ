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
# Each version's twin is built once into build/compare/<commit> and kept. Uses the twin of the
# working tree on both firmwares. Exits 0 when every scenario is bit-identical and 1 when one
# differs, every scenario compared; anything else is 2, saying what: a twin that didn't build,
# a scenario the twin crashed in or couldn't read, one compare.py couldn't compare or gave no
# line for, or this script failing on its way (CI's compare step fails on 2, not on 1)
set -e -o pipefail
trap 'echo "compare.sh: line $LINENO failed (exit $?)"; exit 2' ERR
source "$(dirname "$0")/ref.sh"
A=${1:-HEAD}
B=${2:-work}
TA=$(twin_dir "$A")/frizz-twin || { echo "the twin didn't build on $A"; exit 2; }
TB=$(twin_dir "$B")/frizz-twin || { echo "the twin didn't build on $B"; exit 2; }
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
# not getting through it: a crash (a signal), a script it couldn't read (2), its queues
# overflowing (3) or a log or WAV it couldn't write whole (4: a full /tmp, where they go)
BAD=
for p in "${PIDS[@]}"; do
    rc=0
    wait "${p%%:*}" || rc=$?
    IFS=: read -r _ ref n <<< "$p"
    if [ $rc -gt 128 ]; then
        echo "scenario $n crashed on $ref ($(kill -l $((rc - 128))))"
        BAD="$BAD $n"
    elif [ $rc -eq 4 ]; then
        echo "scenario $n didn't run on $ref: its log or WAV couldn't be written ($RUNS full?)"
        BAD="$BAD $n"
    elif [ $rc -gt 1 ]; then
        echo "scenario $n didn't run on $ref (exit $rc)"
        BAD="$BAD $n"
    fi
done
RAN=$(for n in $NAMES; do [[ " $BAD " == *" $n "* ]] || echo "$n"; done)
echo "$A against $B:"
rc=0
[ -z "$RAN" ] || python3 "$TW/compare.py" "$RUNS/a" "$RUNS/b" $RAN | tee "$RUNS/compared" || rc=$?
# every scenario compared has its line, whatever compare.py's exit code: one that died before
# it got there (a traceback outside its scenarios exits 1, as a difference does) isn't one
for n in $RAN; do
    grep -q "^$n " "$RUNS/compared" 2> /dev/null || { echo "scenario $n: compare.py gave no verdict (exit $rc)"; BAD="$BAD $n"; }
done
[ -z "$BAD" ] || exit 2
[ $rc -le 1 ] || exit 2
exit $rc
