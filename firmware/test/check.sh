#!/bin/bash
# check.sh [ref a] [ref b]: runs the harness on two versions of the engine and compares them.
# Defaults: HEAD against the working tree, the check for a change that shouldn't alter the
# sound before it's committed; once it is, the working tree is HEAD and that compares nothing,
# which the last line says. CI runs it against the PR's base (ci.yml's engine job). The two are
# built and run side by side. The last line names the two versions and the verdict (all.sh
# shows it). Exits 0 when bit-identical, 1 when they differ; anything else is 2: a harness that
# didn't build or run, or a comparison that didn't get through (CI fails on 2, not on 1).
source "$(dirname "$0")/lib.sh"
trap 'echo "check.sh: line $LINENO failed (exit $?)"; exit 2' ERR
A=${1:-HEAD}
B=${2:-work}
label() { [ "$1" = work ] && echo "the working tree" || echo "$1"; }
WHAT="$(label "$A") against $(label "$B")"
# what run.sh reads of the working tree, the same as HEAD's: "HEAD against work" compared HEAD
# with itself (its host/ headers go into both sides alike, so they don't count)
unchanged()
{
    [ -z "$(git -C "$REPO" status --porcelain -- ':(glob)firmware/code/src/*.h' firmware/test/harness.cpp)" ]
}
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT
"$T/run.sh" "$A" "$D/a.bin" & PA=$!
"$T/run.sh" "$B" "$D/b.bin" & PB=$!
RA=0 RB=0
wait $PA || RA=$?
wait $PB || RB=$?
[ $RA -eq 0 ] || { echo "$WHAT: the harness didn't run on $(label "$A")"; exit 2; }
[ $RB -eq 0 ] || { echo "$WHAT: the harness didn't run on $(label "$B")"; exit 2; }
echo "$WHAT:"
rc=0
python3 "$T/compare.py" "$D/a.bin" "$D/b.bin" | tee "$D/compared" || rc=$?
# a difference is the table under DIFFERENT; an exit 1 without it (a crash before main()) isn't
[ $rc != 1 ] || grep -q "^DIFFERENT" "$D/compared" || rc=2
case $rc in
    0)
        if [ "$A" = HEAD ] && [ "$B" = work ] && unchanged; then
            echo "$WHAT: bit-identical, but nothing compared: no uncommitted change to the engine or the harness"
        else
            echo "$WHAT: bit-identical"
        fi ;;
    1) echo "$WHAT: DIFFERENT (above)" ;;
    *) echo "$WHAT: couldn't compare (compare.py exit $rc, above)"; exit 2 ;;
esac
exit $rc
