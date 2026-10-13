#!/usr/bin/env python3
"""Checks the user docs the website is built from (CLAUDE.md, "Docs that feed the website"),
without the website: every link to a heading resolves, the headings are plain and unique, and the
formats the site parses hold.

    firmware/tools/doclinks.py          # from anywhere in the repo; exit 1 on an error

Checked:
- MANUAL.md, QUICKSTART.md, INSTALL.md: exactly one H1 each.
- MANUAL.md: no two headings with the same slug, no link, code or emphasis in a heading.
- Every link in MANUAL, QUICKSTART, INSTALL, README and CHANGELOG to `#anchor` or to
  `FILE.md#anchor` points at a heading there, by GitHub's slugs (github-slugger, which the site
  uses too: "Wow & flutter" is `wow--flutter`); a link to another repo path points at a file
  that exists.
- QUICKSTART.md's `| Effect |` row: 15 cells, each `–` or a name whose heading is `Name`,
  `Name key` or `Master Name` in MANUAL.md.
- An image that isn't in the repo (yet) is a warning, not an error.
"""
import os
import re
import subprocess
import sys

ROOT = subprocess.run(['git', 'rev-parse', '--show-toplevel'], capture_output=True, text=True,
                      cwd=os.path.dirname(os.path.abspath(__file__))).stdout.strip()
SITE = ['MANUAL.md', 'QUICKSTART.md', 'INSTALL.md', 'README.md']
LINKED = SITE + ['CHANGELOG.md']
errors, warnings = [], []


def slugger():
    """github-slugger: lower case, everything but letters, digits, '_', ' ' and '-' dropped,
    spaces to '-', numbered from a heading's second time."""
    seen = {}

    def slug(text):
        base = re.sub(r'[^\w \-]', '', text.strip().lower()).replace(' ', '-')
        n = seen.get(base, 0)
        seen[base] = n + 1
        return f'{base}-{n}' if n else base
    return slug


def lines(path):
    """(line number, text) outside code fences."""
    fence = False
    for n, text in enumerate(open(os.path.join(ROOT, path), encoding='utf-8').read().split('\n'), 1):
        if re.match(r'\s*```', text):
            fence = not fence
            continue
        if not fence:
            yield n, text


def headings(path):
    return [(n, len(m.group(1)), m.group(2)) for n, t in lines(path)
            for m in [re.match(r'^(#{1,6})\s+(.*?)\s*#*\s*$', t)] if m]


anchors = {}
for path in LINKED + ['firmware/README.md']:
    if not os.path.exists(os.path.join(ROOT, path)):
        continue
    slug = slugger()
    hs = headings(path)
    anchors[path] = {}
    for n, level, text in hs:
        s = slug(text)
        if path == 'MANUAL.md':
            if s in anchors[path]:
                errors.append(f'{path}:{n}: heading "{text}" has the slug of line {anchors[path][s]}')
            if re.search(r'[\[\]`*_]', text):
                errors.append(f'{path}:{n}: heading "{text}" isn\'t plain text')
        anchors[path].setdefault(s, n)
    if path in ('MANUAL.md', 'QUICKSTART.md', 'INSTALL.md'):
        h1 = [n for n, level, _ in hs if level == 1]
        if len(h1) != 1:
            errors.append(f'{path}: {len(h1)} H1 headings, expected 1')

LINK = re.compile(r'(!?)\[(?:[^\[\]]|\[[^\]]*\])*\]\(([^)\s]+)\)')
checked = 0
for path in LINKED:
    # a link may wrap over two lines: join each paragraph's lines before looking
    text = '\n'.join(t for _, t in lines(path))
    numbered = list(lines(path))
    for m in LINK.finditer(text.replace('\n', ' ')):
        image, target = m.group(1), m.group(2)
        line = numbered[text[:m.start()].count('\n')][0]
        if re.match(r'^[a-z][a-z0-9+.-]*:', target, re.I):
            continue
        checked += 1
        file, _, frag = target.partition('#')
        dest = os.path.normpath(os.path.join(os.path.dirname(path), file)) if file else path
        if not os.path.exists(os.path.join(ROOT, dest)):
            (warnings if image else errors).append(f'{path}:{line}: {target}: no such file')
            continue
        if frag:
            if dest not in anchors:
                errors.append(f'{path}:{line}: {target}: anchors not checked in {dest}')
            elif frag not in anchors[dest]:
                errors.append(f'{path}:{line}: {target}: no such heading in {dest}')

row = next((t for _, t in lines('QUICKSTART.md') if t.startswith('| Effect |')), None)
if row is None:
    errors.append('QUICKSTART.md: no "| Effect |" row')
else:
    cells = [c.strip() for c in row.split('|')[2:-1]]
    if len(cells) != 15:
        errors.append(f'QUICKSTART.md: the Effect row has {len(cells)} cells, expected 15')
    for c in cells:
        if c in ('–', '-', ''):
            continue
        if not any(slugger()(h) in anchors['MANUAL.md'] for h in (c, f'{c} key', f'Master {c}')):
            errors.append(f'QUICKSTART.md: no MANUAL.md heading for the white key "{c}"')
for fallback in ('punch-in-fx', 'master-compressor'):
    if fallback not in anchors['MANUAL.md']:
        errors.append(f'MANUAL.md: no heading "{fallback}", the site\'s fallback for a white key')

for w in warnings:
    print('warning:', w)
for e in errors:
    print('error:', e)
print(f'{checked} links checked, {len(errors)} errors, {len(warnings)} warnings')
sys.exit(1 if errors else 0)
