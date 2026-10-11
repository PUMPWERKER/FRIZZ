#!/bin/bash
# ui-at.sh REF [CHECK]: runs a check on the virtual CHOMPI (../test/CHECK.cpp, as it is in the
# working tree; ui, the whole-device checks, unless named: midi, sync, ...) on REF's firmware.
# A new check should fail on the version before its fix and pass after:
#   ./ui-at.sh main          # before this branch's fix: the new check fails
#   ./ui-at.sh work          # with it: everything passes
#   ./ui-at.sh main sync     # the same for the timing checks
set -e -o pipefail
source "$(dirname "$0")/ref.sh"
[ -n "$1" ] || { echo "usage: ui-at.sh REF [CHECK] (REF a commit, tag, branch, or work)"; exit 2; }
DIR=$(twin_dir "$1") || { echo "the twin didn't build on $1"; exit 1; }
TWIN_FIRMWARE=$DIR/src TWIN_BUILD=$DIR exec "$REPO/firmware/test/unit.sh" "${2:-ui}"
