#!/bin/bash
# unit.sh NAME: builds NAME.cpp against the working tree's headers and runs it (see README.md
# for what each check covers). Exits 0 when it passes.
# a single check takes no host lock, nor does what it starts (its twin's build: hostlock.sh)
export FRIZZ_HOST_LOCKED=${FRIZZ_HOST_LOCKED:-unit.sh}
source "$(dirname "$0")/lib.sh"
[ -f "$T/$1.cpp" ] || { echo "usage: unit.sh NAME, NAME.cpp one of: $(units | xargs)"; exit 2; }
unit_test "$1"
