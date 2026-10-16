#!/bin/bash
# against-base.sh TITLE CMD...: a comparison of a PR with its base (ci.yml's engine and compare
# jobs: test/check.sh, twin/compare.sh), its output in the log and, under TITLE, in the job
# summary. Both exit 0 when bit-identical and 1 when something differs, which doesn't fail the
# step: the PR explains each difference (CLAUDE.md, *Checked on the twin*). Any other exit is a
# comparison that didn't get through (a build, a run or the comparison itself that failed), and
# fails it.
set -uo pipefail
TITLE=$1
shift
SUMMARY=${GITHUB_STEP_SUMMARY:-/dev/stdout}
OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT
"$@" 2>&1 | tee "$OUT"
rc=${PIPESTATUS[0]}
# and a fault they printed counts as one whatever their exit code says
if [ "$rc" = 1 ] && grep -E "crashed on|didn't run on|didn't build|couldn't compare|gave no verdict|line [0-9]+ failed" "$OUT"; then
    rc=2
fi
{
    echo "### $TITLE"
    echo
    case $rc in
        0) echo "Bit-identical." ;;
        1) echo "Differences: each one is explained in the PR (*Checked on the twin*)." ;;
        *) echo "**Didn't get through** (exit $rc): a fault, not a difference (below)." ;;
    esac
    echo
    echo '```'
    cat "$OUT"
    echo '```'
} >> "$SUMMARY"
if [ "$rc" -gt 1 ]; then
    echo "::error::$(basename "$1") didn't get through (exit $rc): the comparison is above"
    exit 1
fi
exit 0
