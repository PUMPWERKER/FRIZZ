#!/bin/bash
# ref.sh: sourced by compare.sh and ui-at.sh. twin_dir REF builds the twin on REF's firmware (a
# commit, tag or branch; "work" for the working tree) with the working tree's twin around it,
# into build/compare/<commit>, kept for next time, and prints that folder; its firmware is in
# src there (for work, a link to code/src). Fails if it didn't build.
TW=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(git -C "$TW" rev-parse --show-toplevel)
OUT=$TW/build/compare

twin_dir()
{
    local ref=$1 dir src
    mkdir -p "$OUT"
    if [ "$ref" = work ]; then
        dir=$OUT/work
        src=$dir/src
        # the working tree's firmware, where the others have theirs
        mkdir -p "$dir"
        ln -sfn "$REPO/firmware/code/src" "$src"
    else
        local hash path
        hash=$(git -C "$REPO" rev-parse --short "$ref^{commit}")
        dir=$OUT/$hash
        src=$dir/src
        if [ ! -d "$src" ]; then
            # unpacked beside it, then moved: one that was cut short isn't kept as the firmware
            rm -rf "$src.tmp"
            mkdir -p "$src.tmp"
            # FRIZZ moved from firmware/frizz/ to firmware/ (CLAUDE.md)
            path=firmware/code/src
            git -C "$REPO" cat-file -e "$hash:$path" 2> /dev/null || path=firmware/frizz/code/src
            git -C "$REPO" archive "$hash" "$path" \
                | tar -x -C "$src.tmp" --strip-components=$(($(echo "$path" | tr -cd / | wc -c) + 1))
            mv "$src.tmp" "$src"
        fi
    fi
    TWIN_FIRMWARE=$src TWIN_BUILD=$dir "$TW/build.sh" >&2 || return 1
    echo "$dir"
}
