#!/bin/bash
# all.sh: runs every host check on the working tree: the engine against HEAD (check.sh), then
# each unit check (every NAME.cpp but the harness). Prints one line per check and exits 0 when
# all pass, with the faults the checks know of (Known() in check.h) listed under theirs. A
# changed sound (check.sh not bit-identical) counts as a failure: run ./check.sh to
# see what changed.
#
# The checks run side by side, one per core (JOBS=N sets how many), once DaisySP and the twins
# they share are built; their lines come in the order above. The ones on the twin go first,
# the longest; ui, sync, midi and midiout each run their cases side by side too (cases.h,
# CASE_JOBS). remote.cpp and midisuite.cpp, which play the twin at the wall clock's pace
# against remote.py's and midisuite.py's timeouts, and bench.cpp, the longest single process,
# start first and at the normal priority, the others behind them (nice). CASES (for working
# on a few cases) is ignored: all.sh runs every case.
source "$(dirname "$0")/lib.sh"
set +e
unset CASES
JOBS=${JOBS:-$(nproc)}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
for t in $(units); do
    twin_for "$t" > /dev/null || { echo "FAIL  $t: its twin didn't build"; exit 1; }
done

on_twin=$(for t in $(units); do grep -q '#include "twin.h"' "$T/$t.cpp" && echo "$t"; done)
others=$(for t in check $(units); do echo "$on_twin" | grep -qx "$t" || echo "$t"; done)
run()
{
    local t=$1 nice=
    echo "$first" | grep -qx "$t" || nice="nice -n 10"
    if [ "$t" = check ]; then $nice "$T/check.sh"; else $nice "$T/unit.sh" "$t"; fi > "$OUT/$t.out" 2>&1
    echo $? > "$OUT/$t.rc"
}
first=$(printf '%s\n' remote midisuite bench | grep -x -f <(units))
for t in $first $(echo "$on_twin" | grep -vx -f <(echo "$first")) $others; do
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n; done
    run "$t" &
done
wait

fail=0
for t in check $(units); do
    out=$(cat "$OUT/$t.out")
    if [ "$(cat "$OUT/$t.rc")" -eq 0 ]; then
        echo "ok    $t: $(echo "$out" | tail -1)$(echo "$out" | grep -m1 -E '^[0-9]+ known$' | sed 's/^/, /')"
        echo "$out" | grep "^KNOWN" | sed 's/^/      /'
    else
        echo "FAIL  $t:"
        echo "$out" | grep -v "^ok" | sed 's/^/      /'
        fail=1
    fi
done
exit $fail
