#!/bin/bash
# sources.sh: sourced by lib.sh and ../twin/ref.sh, which set REPO. firmware_sources REF DIR copies
# the firmware's source at REF (a commit; "work" for the working tree) into DIR, flat: FRIZZ's
# own (code/src's *.h, *.c, *.cpp) and the PUMPWERKER platform's (../platform/, #477: its
# layers' own files, PLATFORM_LAYERS, not their libraries). A file's name is in one folder only
# (the check `platform`), so one folder holds them all, as code/src did before the platform
# moved out; a commit from before that has code/src only (or firmware/frizz/code/src, before the
# repository was reorganized).
PLATFORM_LAYERS="core daisy boards/chompi"

firmware_sources()
{
    local ref=$1 dir=$2 path layer files
    mkdir -p "$dir"
    if [ "$ref" = work ]; then
        for path in "$REPO/firmware/code/src" $(for layer in $PLATFORM_LAYERS; do echo "$REPO/firmware/platform/$layer"; done); do
            [ -d "$path" ] || continue
            find "$path" -maxdepth 1 -type f \( -name '*.h' -o -name '*.c' -o -name '*.cpp' \) -exec cp {} "$dir/" \;
        done
        return
    fi
    path=firmware/code/src
    git -C "$REPO" cat-file -e "$ref:$path" 2> /dev/null || path=firmware/frizz/code/src
    for path in $path $(for layer in $PLATFORM_LAYERS; do echo "firmware/platform/$layer"; done); do
        git -C "$REPO" cat-file -e "$ref:$path" 2> /dev/null || continue
        # the folder's own files, not its subfolders (daisy/libs)
        files=$(git -C "$REPO" ls-tree "$ref" "$path/" | awk -F'\t' '$1 ~ / blob / {print $2}' \
            | grep -E '\.(h|c|cpp)$') || continue
        # shellcheck disable=SC2086
        git -C "$REPO" archive "$ref" -- $files | tar -x -C "$dir" --strip-components=$(($(echo "$path" | tr -cd / | wc -c) + 1))
    done
}
