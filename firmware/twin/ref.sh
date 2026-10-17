#!/bin/bash
# ref.sh: sourced by compare.sh and ui-at.sh. twin_dir REF builds the twin on REF's firmware (a
# commit, tag or branch; "work" for the working tree) with the working tree's twin around it,
# into build/compare/<commit>, kept for next time, and prints that folder; its firmware is in
# src there, FRIZZ's and the platform's flat (../test/sources.sh; for work, the working tree's,
# copied again each time). Fails if it didn't build.
TW=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(git -C "$TW" rev-parse --show-toplevel)
OUT=$TW/build/compare
source "$TW/../test/sources.sh"

twin_dir()
{
    local ref=$1 dir src
    mkdir -p "$OUT"
    if [ "$ref" = work ]; then
        dir=$OUT/work
        src=$dir/src
        # the working tree's firmware, where the others have theirs
        rm -rf "$src"
        firmware_sources work "$src"
    else
        local hash
        hash=$(git -C "$REPO" rev-parse --short "$ref^{commit}")
        dir=$OUT/$hash
        src=$dir/src
        if [ ! -d "$src" ]; then
            # unpacked beside it, then moved: one that was cut short isn't kept as the firmware
            rm -rf "$src.tmp"
            mkdir -p "$src.tmp"
            firmware_sources "$hash" "$src.tmp"
            mv "$src.tmp" "$src"
        fi
    fi
    TWIN_FIRMWARE=$src TWIN_BUILD=$dir "$TW/build.sh" >&2 || return 1
    echo "$dir"
}
