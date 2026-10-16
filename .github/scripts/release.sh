#!/bin/bash
# release.sh FRIZZ.bin: release.yml's last step. With TAG (a v* tag), MD5 (the CI build's) and
# GH_REPO/GH_TOKEN set, finds the tag's GitHub release, drafts included (the by-tag lookup
# skips drafts, so it lists them), and
# - checks its FRIZZ.bin against the CI's build: the same md5, or this fails loudly;
# - attaches the CI's build to a draft without a FRIZZ.bin; a published one without fails
#   loudly instead, as attaching would put it live (the website takes a published release with
#   its .bin) without a person;
# - makes a draft one (a pre-release for a -beta.N tag) with FRIZZ.bin and its md5 if there's
#   none. Never publishes one, never marks one Latest, never touches the rolling `beta`.
# Writes what it did to the job summary.
set -euo pipefail
BIN=$1
SUMMARY=${GITHUB_STEP_SUMMARY:-/dev/stdout}
[[ $TAG =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-beta\.[1-9][0-9]*)?$ ]] || { echo "::error::$TAG isn't a version's tag"; exit 1; }
[ "$(md5sum "$BIN" | cut -d' ' -f1)" = "$MD5" ] || { echo "::error::$BIN isn't the build job's ($MD5)"; exit 1; }
say() { echo "$1"; echo "$1" >> "$SUMMARY"; }

mapfile -t ids < <(gh api "repos/$GH_REPO/releases" --paginate --jq ".[] | select(.tag_name == \"$TAG\") | .id")
if [ ${#ids[@]} -gt 1 ]; then
    echo "::error::$TAG has ${#ids[@]} releases (${ids[*]}): delete the extra drafts, then run this again"
    exit 1
fi

if [ ${#ids[@]} = 0 ]; then
    pre=()
    [[ $TAG == *-beta.* ]] && pre=(--prerelease)
    notes="FRIZZ.bin md5 \`$MD5\`, built by CI (release.yml) from $TAG with GCC 10.3-2021.10.

Draft: the notes start from CHANGELOG.md's *Unreleased* section (CLAUDE.md, *Releases*); publishing it is a person's step."
    gh release create "$TAG" --verify-tag --draft "${pre[@]}" --title "$TAG" --notes "$notes" "$BIN"
    say "No release for $TAG: made a draft${pre:+ pre-release} with FRIZZ.bin md5 \`$MD5\`. Publishing it is a person's step."
    exit 0
fi

id=${ids[0]}
read -r draft asset < <(gh api "repos/$GH_REPO/releases/$id" \
    --jq '[(.draft | tostring), ([.assets[] | select(.name == "FRIZZ.bin") | .id][0] // "none" | tostring)] | join(" ")')
kind=release
[ "$draft" = true ] && kind="draft release"
if [ "$asset" = none ] && [ "$draft" != true ]; then
    say "**$TAG's release is published without a FRIZZ.bin:** CI attaches nothing to a published release (that would put it live without a person). Attach the build of a clean worktree of the tag yourself (CLAUDE.md, *Releases*), md5 \`$MD5\` as CI built it, then run this again."
    echo "::error::$TAG's published release has no FRIZZ.bin; CI attaches only to drafts"
    exit 1
fi
if [ "$asset" = none ]; then
    gh api --method POST -H "Content-Type: application/octet-stream" \
        "https://uploads.github.com/repos/$GH_REPO/releases/$id/assets?name=FRIZZ.bin" --input "$BIN" > /dev/null
    say "$TAG's $kind had no FRIZZ.bin: attached CI's, md5 \`$MD5\`."
    exit 0
fi

theirs=$(mktemp)
trap 'rm -f "$theirs"' EXIT
gh api -H "Accept: application/octet-stream" "repos/$GH_REPO/releases/assets/$asset" > "$theirs"
have=$(md5sum "$theirs" | cut -d' ' -f1)
if [ "$have" != "$MD5" ]; then
    say "**$TAG's $kind carries a FRIZZ.bin that isn't the tag's build:** its md5 \`$have\`, CI's \`$MD5\` (GCC 10.3-2021.10 at $TAG). Build it again in a clean worktree of the tag (CLAUDE.md, *Releases*) before anyone installs it."
    echo "::error::$TAG's FRIZZ.bin (md5 $have) isn't CI's build of the tag (md5 $MD5)"
    exit 1
fi
say "$TAG's $kind carries FRIZZ.bin md5 \`$MD5\`: the same bytes CI built from the tag."
