#!/bin/bash
# gcc-arm.sh: puts GNU Arm Embedded 10.3-2021.10, FRIZZ's pinned compiler (CLAUDE.md,
# "Toolchain"), in ~/opt/gcc-arm-none-eabi-10.3-2021.10, where tools/builds.py looks for it:
# ARM's own tarball, checked against the MD5 ARM publishes with it and its SHA-256, before
# anything of it is unpacked. For CI (ci.yml, release.yml); a machine of your own does the same
# by hand (firmware/README.md, 1. Toolchain).
set -euo pipefail
URL=https://developer.arm.com/-/media/Files/downloads/gnu-rm/10.3-2021.10/gcc-arm-none-eabi-10.3-2021.10-x86_64-linux.tar.bz2
MD5=2383e4eb4ea23f248d33adc70dc3227e
SHA256=97dbb4f019ad1650b732faffcc881689cedc14e2b7ee863d390e0a41ef16c9a3
DL=$(mktemp -d)
trap 'rm -rf "$DL"' EXIT
curl -fsSL --retry 3 -o "$DL/gcc.tar.bz2" "$URL"
echo "$MD5  $DL/gcc.tar.bz2" | md5sum -c --quiet || { echo "::error::ARM's tarball doesn't have ARM's MD5"; exit 1; }
echo "$SHA256  $DL/gcc.tar.bz2" | sha256sum -c --quiet || { echo "::error::ARM's tarball doesn't have its SHA-256"; exit 1; }
mkdir -p ~/opt
tar -xjf "$DL/gcc.tar.bz2" -C ~/opt
~/opt/gcc-arm-none-eabi-10.3-2021.10/bin/arm-none-eabi-gcc --version | head -1
