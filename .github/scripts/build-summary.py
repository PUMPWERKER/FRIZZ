#!/usr/bin/env python3
"""build-summary.py COMMIT < builds.py's output: the job summary's Markdown for the firmware
build (ci.yml, release.yml): each build's md5, the name stage 2 and a release's notes give it,
and its memory table, headed by the room left in code (SRAM_EXEC) and data (SRAM)."""
import re
import sys

UNITS = {"B": 1, "KB": 1024, "MB": 1024 ** 2, "GB": 1024 ** 3}


def size(n, unit):
    return int(n) * UNITS[unit]


commit = sys.argv[1]
builds, md5s, current = [], {}, None
for line in sys.stdin.read().splitlines():
    m = re.match(r"building (\S+)", line)
    if m:
        current = {"name": m[1], "table": [], "free": {}}
        builds.append(current)
    elif current and (line.startswith("Memory region") or re.match(r"\s+\S+:\s", line)):
        current["table"].append(line)
        m = re.match(r"\s+(\S+):\s+(\d+) (\w+)\s+(\d+) (\w+)", line)
        if m:
            current["free"][m[1]] = size(m[4], m[5]) - size(m[2], m[3])
    m = re.match(r"\S*/(FRIZZ(?:-bench)?\.bin)\s+md5 (\w+)\s+crc32 (\w+)", line)
    if m:
        md5s[m[1]] = (m[2], m[3])

print("### Firmware build of %s\n" % commit)
print("| Build | md5 | CRC-32 | code left (SRAM_EXEC) | data left (SRAM) |")
print("|---|---|---|---|---|")
for b in builds:
    md5, crc = md5s.get(b["name"], ("?", "?"))
    left = ["%.1f KB" % (b["free"][r] / 1024) if r in b["free"] else "?" for r in ("SRAM_EXEC", "SRAM")]
    print("| %s | `%s` | `%s` | %s | %s |" % (b["name"], md5, crc, left[0], left[1]))
for b in builds:
    print("\n<details><summary>%s's memory table</summary>\n\n```\n%s\n```\n</details>"
          % (b["name"], "\n".join(b["table"])))
