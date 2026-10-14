#!/usr/bin/env python3
"""Checks the files the website is built from (CLAUDE.md, "Docs that feed the website"),
without the website: every link to a heading resolves, the headings are plain and unique, and
every format frizz-web's scripts/fetch-docs.mjs parses holds, by the same rules it parses them.

    firmware/tools/doclinks.py             # from anywhere in the repo; exit 1 on an error
    firmware/tools/doclinks.py --selftest  # breaks a copy of the docs on purpose, case by case,
                                           # and checks that each break is reported
    firmware/tools/doclinks.py --all       # both, the selftest's failures only (all.sh)

Checked:
- MANUAL.md, QUICKSTART.md, INSTALL.md: exactly one H1 each.
- MANUAL.md: no two headings with the same slug, no link, code or emphasis in a heading.
- Every link in MANUAL, QUICKSTART, INSTALL, README and CHANGELOG to `#anchor` or to
  `FILE.md#anchor` points at a heading there, by GitHub's slugs (github-slugger, which the site
  uses too: "Wow & flutter" is `wow--flutter`); a link to another repo path points at a file
  that exists.
- QUICKSTART.md's `| Effect |` row: 15 cells, each `–` or a name whose heading is `Name`,
  `Name key` or `Master Name` in MANUAL.md.
- Each of those effects' chapters (the `##` the heading is, or is in): a summary, the text of
  its key's card on the landing page, and a knob table, `| Knob | Page 1 |` (or `| Knob | Function |`), its first
  in the chapter, with rows 1 to 4, each cell a dash (an unused knob) or text that leaves a name
  before the first ":", "," or "(" (the landing page's knob labels).
- `<!-- summary: ... -->`: right under a `##` of MANUAL.md, or under the H1 of INSTALL.md and
  QUICKSTART.md (blank lines between are fine, nothing else), at most one there, on one line,
  one sentence ending in "."; INSTALL.md and QUICKSTART.md must have theirs. Over 12 words is a
  warning. An effect's chapter's (its key's card on the landing page): 1 to 2 sentences, at
  most 160 characters.
- `<!-- part: Name -->` in MANUAL.md: on its own line, a blank line before and after, right
  before a `##`, not in a code block.
- A comment that looks like either marker but doesn't match it exactly (the site would drop it
  without a word) is an error.
- The white keys' colours, where the site reads them: every `static const float <name>[3] =
  {r, g, b};` of LedColors.h in float literals, every kFxSlots entry of FxSlots.h starting
  `{Hardware::SwId::KEY_<n>, <colour>,` with a colour LedColors.h has, `kCompKey =
  Hardware::SwId::KEY_<n>`, PanelKeys.h's kWhiteKeys KEY_1 to KEY_15 in order (KEY_<n> is the
  n-th white key), and a slot or kCompKey for every key the `| Effect |` row names. Read only.
- site/landing.md, the landing page's text, against frizz-web's schema: one H1, the sections
  Meta, Hero, Keys, Session, Get started, Credits in this order, each one's `- Name: value`
  fields (all of them, none other, none twice, each within its length), one paragraph where it
  has one, 3 to 6 moves (### under Session) with 1 to 4 `Controls`, no HTML, images or other
  lists, `code` only in Controls, links only to the docs (anchors resolving), images/ or https.
- An image that isn't in the repo (yet) is a warning, not an error.
- Every picture under images/ is what firmware/tools/diagrams/diagrams.py draws now
  (`diagrams.py --check`): its figures read MANUAL.md's knob names and the firmware's colours.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

SITE = ['MANUAL.md', 'QUICKSTART.md', 'INSTALL.md', 'README.md']
LINKED = SITE + ['CHANGELOG.md']
GUIDES = ['INSTALL.md', 'QUICKSTART.md']
# the pages the site is built from (README.md no longer, frizz-web #17)
SITE_PAGES = ['MANUAL.md', 'QUICKSTART.md', 'INSTALL.md']
# fetch-docs.mjs's patterns, as it has them
PART = re.compile(r'^<!--\s*part:\s*(.+?)\s*-->\s*$')
SUMMARY = re.compile(r'^<!--\s*summary:\s*(.+?)\s*-->\s*$')
HEADING = re.compile(r'^(#{1,6})\s+(.*?)\s*#*\s*$')
LINK = re.compile(r'(!?)\[(?:[^\[\]]|\[[^\]]*\])*\]\(([^)\s]+)\)')
DASH = re.compile(r'^[—–-]?$')
SRC = 'firmware/code/src'
LANDING = 'site/landing.md'
FLOAT = r'(\d+\.?\d*|\.\d+)[fF]?'
EFFECT_SUMMARY = 160  # characters: an effect's summary is its key's card on the landing page


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


def knob_name(cell):
    """fetch-docs' knobNames(): the cell up to the first ':', ',' or '(', without a trailing
    ' in …' / ' on …'; '' for an unused knob (a dash)."""
    if DASH.match(cell):
        return ''
    return re.sub(r' (in|on) .*', '', re.split(r'[:,(]', re.sub(r'[`*]', '', cell))[0]).strip()


def check(root):
    """Checks the docs under root; returns (errors, warnings, links checked)."""
    errors, warnings = [], []
    texts = {}

    def text(path):
        if path not in texts:
            full = os.path.join(root, path)
            texts[path] = open(full, encoding='utf-8').read() if os.path.exists(full) else None
        return texts[path]

    def all_lines(path):
        """(line number, text, in a code fence) for every line, the fences themselves too."""
        fence = False
        for n, t in enumerate(text(path).replace('\r\n', '\n').split('\n'), 1):
            is_fence = bool(re.match(r'\s*```', t))
            if is_fence:
                fence = not fence
            yield n, t, fence or is_fence

    def lines(path):
        """(line number, text) outside code fences."""
        return [(n, t) for n, t, code in all_lines(path) if not code]

    def headings(path):
        return [(n, len(m.group(1)), m.group(2)) for n, t in lines(path)
                for m in [HEADING.match(t)] if m]

    # Headings: anchors, plain and unique in the manual, one H1 per page
    anchors = {}
    for path in LINKED + ['firmware/README.md']:
        if text(path) is None:
            if path in SITE:
                errors.append(f'{path}: missing')
            continue
        slug = slugger()
        hs = headings(path)
        anchors[path] = {}
        for n, level, t in hs:
            s = slug(t)
            if path == 'MANUAL.md':
                if s in anchors[path]:
                    errors.append(f'{path}:{n}: heading "{t}" has the slug of line {anchors[path][s]}')
                if re.search(r'[\[\]`*_]', t):
                    errors.append(f'{path}:{n}: heading "{t}" isn\'t plain text')
            anchors[path].setdefault(s, n)
        if path in ('MANUAL.md', 'QUICKSTART.md', 'INSTALL.md'):
            h1 = [n for n, level, _ in hs if level == 1]
            if len(h1) != 1:
                errors.append(f'{path}: {len(h1)} H1 headings ({", ".join(map(str, h1)) or "none"}), '
                              'expected exactly 1 (the page title)')
    if text('MANUAL.md') is None or text('QUICKSTART.md') is None:
        return errors, warnings, 0

    # Links
    checked = 0
    for path in LINKED:
        if text(path) is None:
            continue
        # a link may wrap over two lines: join each paragraph's lines before looking
        numbered = lines(path)
        joined = '\n'.join(t for _, t in numbered)
        for m in LINK.finditer(joined.replace('\n', ' ')):
            image, target = m.group(1), m.group(2)
            line = numbered[joined[:m.start()].count('\n')][0]
            if re.match(r'^[a-z][a-z0-9+.-]*:', target, re.I):
                continue
            checked += 1
            file, _, frag = target.partition('#')
            dest = os.path.normpath(os.path.join(os.path.dirname(path), file)) if file else path
            if path in SITE_PAGES and dest == 'README.md':
                errors.append(f'{path}:{line}: {target}: the site has no page for the README '
                              '(frizz-web #17): link to the manual or to GitHub instead')
                continue
            if not os.path.exists(os.path.join(root, dest)):
                (warnings if image else errors).append(f'{path}:{line}: {target}: no such file')
                continue
            if frag:
                if dest not in anchors:
                    errors.append(f'{path}:{line}: {target}: anchors not checked in {dest}')
                elif frag not in anchors[dest]:
                    errors.append(f'{path}:{line}: {target}: no such heading in {dest}')

    # Markers: a comment in a site page that looks like one must be one, where the site reads it
    for path in SITE_PAGES:
        if text(path) is None:
            continue
        for n, t, code in all_lines(path):
            if '<!--' not in t:
                continue
            if code:
                if path == 'MANUAL.md' and PART.match(t):
                    errors.append(f'{path}:{n}: a part marker in a code block (the site ignores it)')
                continue
            if PART.match(t) or SUMMARY.match(t):
                if PART.match(t) and path != 'MANUAL.md':
                    errors.append(f'{path}:{n}: a part marker outside MANUAL.md (the site ignores it)')
            elif re.search(r'<!--\s*[\w-]+\s*:|part|summ?a?ry', t, re.I):
                errors.append(f'{path}:{n}: "{t.strip()}" isn\'t a marker the site reads: '
                              'it must be `<!-- part: Name -->` or `<!-- summary: ... -->`, '
                              'alone on one line')

    # Summaries: right under a ## of the manual or a guide's H1, before anything else
    def summaries(path, level):
        """{heading line: (summary line, text)}, reporting misplaced and second summaries"""
        found = {}
        under = None      # the heading line a summary may still follow
        seen_text = False
        for n, t in lines(path):
            h = HEADING.match(t)
            if h:
                under = n if len(h.group(1)) == level else None
                seen_text = False
                continue
            m = SUMMARY.match(t)
            if m:
                if under is None:
                    errors.append(f'{path}:{n}: a summary not under a {"#" * level} heading '
                                  '(the site ignores it)')
                elif under in found:
                    errors.append(f'{path}:{n}: a second summary for the heading on line {under} '
                                  f'(the first is on line {found[under][0]})')
                elif seen_text:
                    errors.append(f'{path}:{n}: the summary must come right under its heading, '
                                  'before any other text (the site ignores it here)')
                else:
                    found[under] = (n, m.group(1))
                continue
            if t.strip() and not PART.match(t):
                seen_text = True
        return found

    def sentences(path, n, s, most):
        """s is 1 to `most` sentences, each ending in '.'"""
        count = len(re.findall(r'[.!?](\s|$)', s))
        if not s.endswith('.') or count > most:
            errors.append(f'{path}:{n}: the summary must be {"one sentence" if most == 1 else f"1 to {most} sentences"}'
                          ' ending in "."')

    manual_summaries = summaries('MANUAL.md', 2)
    for path in GUIDES:
        if text(path) is None:
            continue
        found = summaries(path, 1)
        if not found:
            errors.append(f'{path}: no `<!-- summary: ... -->` under the H1 (the site\'s description '
                          'of the page)')
        for n, s in found.values():
            sentences(path, n, s, 1)

    # Parts: own line, blank before and after, right before a ##
    numbered = list(all_lines('MANUAL.md'))
    for i, (n, t, code) in enumerate(numbered):
        if code or not PART.match(t):
            continue
        before = numbered[i - 1][1] if i else ''
        after = numbered[i + 1][1] if i + 1 < len(numbered) else None
        nxt = next((u for _, u, _ in numbered[i + 1:] if u.strip()), None)
        if before.strip() or after is None or after.strip():
            errors.append(f'MANUAL.md:{n}: a part marker needs a blank line before and after it')
        if not (nxt and re.match(r'^##\s', nxt)):
            errors.append(f'MANUAL.md:{n}: a part marker must come right before a ## heading')

    # The manual's chapters (##), each with its lines, for the knob tables
    chapters = []   # [heading line, slug, title, [(n, text)]]
    slug = slugger()
    for n, t in lines('MANUAL.md'):
        h = HEADING.match(t)
        s = slug(h.group(2)) if h and len(h.group(1)) > 1 else None
        if h and len(h.group(1)) == 2:
            chapters.append([n, s, h.group(2), []])
        elif chapters:
            chapters[-1][3].append((n, t, s))

    def chapter_of(anchor):
        for c in chapters:
            if c[1] == anchor or any(s == anchor for _, _, s in c[3]):
                return c
        return None

    # The QUICKSTART key row, and each effect's chapter
    effect_chapters = set()
    row = next(((n, t) for n, t in lines('QUICKSTART.md') if t.startswith('| Effect |')), None)
    if row is None:
        errors.append('QUICKSTART.md: no "| Effect |" row (the landing page\'s white keys)')
        cells = []
    else:
        cells = [c.strip() for c in row[1].split('|')[2:-1]]
        if len(cells) != 15:
            errors.append(f'QUICKSTART.md:{row[0]}: the Effect row has {len(cells)} cells, expected 15')
    for c in cells:
        if c in ('–', '-', ''):
            continue
        anchor = next((a for a in (slugger()(h) for h in (c, f'{c} key', f'Master {c}'))
                       if a in anchors['MANUAL.md']), None)
        if anchor is None:
            errors.append(f'QUICKSTART.md:{row[0]}: no MANUAL.md heading for the white key "{c}" '
                          f'("{c}", "{c} key" or "Master {c}")')
            continue
        chapter = chapter_of(anchor)
        if chapter is None:
            errors.append(f'MANUAL.md:{anchors["MANUAL.md"][anchor]}: the white key "{c}" has no '
                          'chapter (##) around its heading')
            continue
        n0, _, title = chapter[0], chapter[1], chapter[2]
        effect_chapters.add(n0)
        if n0 not in manual_summaries:
            errors.append(f'MANUAL.md:{n0}: "{title}" (white key "{c}") has no `<!-- summary: ... -->`: '
                          'it is the text of its key on the landing page')
        else:
            n, s = manual_summaries[n0]
            sentences('MANUAL.md', n, s, 2)
            if len(s) > EFFECT_SUMMARY:
                errors.append(f'MANUAL.md:{n}: the summary of "{title}" has {len(s)} characters, at most '
                              f'{EFFECT_SUMMARY} (its key\'s card on the landing page)')
        knobs, table, start = {}, None, None
        for n, t, _ in chapter[3]:
            if not t.startswith('|'):
                if table:
                    break
                continue
            r = [x.strip() for x in t.split('|')[1:-1]]
            if table is None and len(r) > 1 and r[0] == 'Knob' and r[1] in ('Page 1', 'Function'):
                table, start = True, n
            elif table and re.fullmatch(r'[1-4]', r[0]):
                if r[0] in knobs:
                    errors.append(f'MANUAL.md:{n}: knob {r[0]} twice in the knob table of "{title}"')
                knobs[r[0]] = (n, r[1] if len(r) > 1 else '')
        if not table:
            errors.append(f'MANUAL.md:{n0}: "{title}" (white key "{c}") has no knob table '
                          '`| Knob | Page 1 |` (the landing page\'s knob labels)')
            continue
        for k in '1234':
            if k not in knobs:
                errors.append(f'MANUAL.md:{start}: the knob table of "{title}" has no row {k} '
                              '(it needs rows 1 to 4)')
            elif not DASH.match(knobs[k][1]) and not knob_name(knobs[k][1]):
                errors.append(f'MANUAL.md:{knobs[k][0]}: knob {k} of "{title}" has no name before '
                              'its first ":", "," or "(" (the landing page\'s label); "–" for unused')
    for n0, (n, s) in manual_summaries.items():
        if n0 in effect_chapters:
            continue
        sentences('MANUAL.md', n, s, 1)
        if len(s.split()) > 12:
            warnings.append(f'MANUAL.md:{n}: the summary has {len(s.split())} words, keep it under ~12')
    for fallback in ('punch-in-fx', 'master-compressor'):
        if fallback not in anchors['MANUAL.md']:
            errors.append(f'MANUAL.md: no heading "{fallback}", the site\'s fallback for a white key')

    key_colours(root, cells, row[0] if row else 0, errors)
    if os.path.exists(os.path.join(root, LANDING)):
        checked += landing(root, anchors, errors)
    else:
        errors.append(f'{LANDING}: missing (the landing page\'s text)')
    return errors, warnings, checked


def read(root, path):
    full = os.path.join(root, path)
    return open(full, encoding='utf-8').read() if os.path.exists(full) else None


def key_colours(root, cells, row_line, errors):
    """The white keys' colours, as fetch-docs reads them: LedColors.h's
    `static const float <name>[3] = {<r>, <g>, <b>};`, FxSlots.h's kFxSlots entries
    `{Hardware::SwId::KEY_<n>, <colour>,` and kCompKey (white), KEY_<n> the n-th white key."""
    leds, slots, panel = (read(root, f'{SRC}/{f}') for f in ('LedColors.h', 'FxSlots.h', 'PanelKeys.h'))
    for f, t in (('LedColors.h', leds), ('FxSlots.h', slots), ('PanelKeys.h', panel)):
        if t is None:
            errors.append(f'{SRC}/{f}: missing (the site reads the keys\' colours from it)')
    if None in (leds, slots, panel):
        return
    colours = {}
    for m in re.finditer(r'static\s+const\s+float\s+(\w+)\[3\]\s*=\s*\{([^}]*)\};', leds):
        values = [v.strip() for v in m.group(2).split(',')]
        if len(values) != 3 or not all(re.fullmatch(FLOAT, v) for v in values):
            errors.append(f'{SRC}/LedColors.h:{leds[:m.start()].count(chr(10)) + 1}: {m.group(1)}: '
                          f'"{m.group(2)}" isn\'t 3 float literals (like 1.f, .95f, 0.05f)')
        elif not all(0 <= float(v.rstrip('fF')) <= 1 for v in values):
            errors.append(f'{SRC}/LedColors.h: {m.group(1)}: a channel outside 0..1')
        colours[m.group(1)] = values
    if 'white' not in colours:
        errors.append(f'{SRC}/LedColors.h: no white (the compressor\'s key)')
    table = re.search(r'kFxSlots\[\]\s*=\s*\{(.*?)\n\};', slots, re.S)
    keys = {}
    if not table:
        errors.append(f'{SRC}/FxSlots.h: no `kFxSlots[] = {{ ... }};` (the keys\' colours)')
    else:
        base = slots[:table.start(1)].count('\n') + 1
        for i, entry in enumerate(table.group(1).split('\n')):
            if not entry.strip().startswith('{'):
                continue
            m = re.match(r'\s*\{Hardware::SwId::KEY_(\d+),\s*(?:\d+,\s*)?(\w+),', entry)
            if not m:
                errors.append(f'{SRC}/FxSlots.h:{base + i}: an entry the site can\'t read: it must start '
                              '`{Hardware::SwId::KEY_<n>, <colour>,`')
                continue
            if m.group(2) not in colours:
                errors.append(f'{SRC}/FxSlots.h:{base + i}: colour "{m.group(2)}" isn\'t in LedColors.h')
            keys[int(m.group(1))] = m.group(2)
    comp = re.search(r'kCompKey\s*=\s*Hardware::SwId::KEY_(\d+)\s*;', slots)
    if not comp:
        errors.append(f'{SRC}/FxSlots.h: no `kCompKey = Hardware::SwId::KEY_<n>;` (the compressor\'s key)')
    elif int(comp.group(1)) in keys:
        errors.append(f'{SRC}/FxSlots.h: kCompKey KEY_{comp.group(1)} has an effect slot too')
    white = re.search(r'kWhiteKeys\[\w*\]\s*=\s*\{([^}]*)\}', panel)
    if not white or re.findall(r'KEY_(\d+)', white.group(1)) != [str(n) for n in range(1, 16)]:
        errors.append(f'{SRC}/PanelKeys.h: kWhiteKeys isn\'t KEY_1 to KEY_15 in order, but the site '
                      'takes KEY_<n> for the n-th white key')
    for i, c in enumerate(cells, 1):
        if c in ('–', '-', ''):
            continue
        if i not in keys and not (comp and int(comp.group(1)) == i):
            errors.append(f'QUICKSTART.md:{row_line}: white key {i} ("{c}") has no slot in FxSlots.h '
                          f'(KEY_{i}) and isn\'t kCompKey: the site has no colour for it')


def plain(s):
    """A landing.md text without its markup, for its length."""
    s = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', s)
    return re.sub(r'\*\*|\*|`', '', s).strip()


# site/landing.md: its sections in order, each one's fields (name: most characters) and whether
# it has a paragraph (its most characters), as frizz-web's fetch-docs reads them
LANDING_SECTIONS = [
    ('Meta', {'Title': 70, 'Description': 170}, None),
    ('Hero', {'Eyebrow': 40, 'Title': 32, 'Assure': 180}, 360),
    ('Keys', {'Title': 40}, 360),
    ('Session', {'Eyebrow': 40, 'Title': 48}, None),
    ('Get started', {'Eyebrow': 40, 'Title': 48, 'Install': 160, 'Quick start': 160,
                     'Manual link': 60}, 300),
    ('Credits', {}, 420),
]
MOVE = ({'Controls': None}, 320)  # a ### of Session: its title at most 40, 3 to 6 of them
EMPHASIS_ONCE = {('Hero', 'Title'), ('Session', 'Title')}


def landing(root, anchors, errors):
    """Checks site/landing.md against frizz-web's schema; returns the links checked."""
    path = LANDING
    src = read(root, path)
    # comments go, anywhere, keeping the line numbers
    src = re.sub(r'<!--.*?-->', lambda m: '\n' * m.group(0).count('\n'), src, flags=re.S)
    lines = src.split('\n')
    err = lambda n, msg: errors.append(f'{path}:{n}: {msg}')
    blocks = []   # [level, title, line, [(n, text)]]
    h1 = 0
    for n, t in enumerate(lines, 1):
        h = re.match(r'^(#{1,6})\s+(.*?)\s*$', t)
        if h and len(h.group(1)) == 1:
            h1 += 1
            if blocks:
                err(n, 'the H1 must come first')
            continue
        if h and len(h.group(1)) in (2, 3):
            blocks.append([len(h.group(1)), h.group(2), n, []])
        elif h:
            err(n, f'a {h.group(1)} heading: only ## sections and the moves (### under Session)')
        elif blocks:
            blocks[-1][3].append((n, t))
        elif t.strip():
            err(n, 'text before the first ## section')
    if h1 != 1:
        errors.append(f'{path}: {h1} H1 headings, expected exactly 1')

    names = [b[1] for b in blocks if b[0] == 2]
    want = [s[0] for s in LANDING_SECTIONS]
    if names != want:
        errors.append(f'{path}: the sections are {names}, expected exactly {want} in this order')
    spec = {s[0]: (s[1], s[2]) for s in LANDING_SECTIONS}
    links = 0
    section = None
    moves = 0
    for level, title, n0, body in blocks:
        if level == 2:
            section = title
            if title not in spec:
                err(n0, f'unknown section "{title}"')
                continue
            fields, para = spec[title]
            where = title
        else:
            if section != 'Session':
                err(n0, f'### "{title}": only Session has ### (its moves)')
                continue
            moves += 1
            if len(plain(title)) > 40:
                err(n0, f'the move "{title}" is {len(plain(title))} characters, at most 40')
            if '`' in title:
                err(n0, f'`code` in the move "{title}": only in Controls')
            links += inline(path, n0, title, anchors, root, errors)
            fields, para = MOVE
            where = f'Session, move "{title}"'
        # the field list: right under the heading, then a blank line, then the paragraph
        i = 0
        while i < len(body) and not body[i][1].strip():
            i += 1
        seen = {}
        while i < len(body) and body[i][1].startswith('- '):
            n, t = body[i]
            f = re.match(r'^- ([^:]+):\s*(.*)$', t)
            if not f:
                err(n, f'"{t}" isn\'t a field (`- Name: value`)')
            elif f.group(1) not in fields:
                err(n, f'unknown field "{f.group(1)}" in {where} (it has {list(fields) or "none"})')
            elif f.group(1) in seen:
                err(n, f'the field "{f.group(1)}" twice in {where}')
            else:
                seen[f.group(1)] = (n, f.group(2))
            i += 1
        for f in fields:
            if f not in seen:
                err(n0, f'{where} has no field "{f}"')
        for f, (n, v) in seen.items():
            if not v.strip():
                err(n, f'the field "{f}" of {where} is empty')
            if f == 'Controls':
                spans = re.findall(r'`([^`]*)`', v)
                if not 1 <= len(spans) <= 4 or re.sub(r'`[^`]*`', '', v).strip():
                    err(n, f'Controls must be 1 to 4 `code` spans and nothing else, not "{v}"')
                for s in spans:
                    if len(s) > 20:
                        err(n, f'the control "{s}" is {len(s)} characters, at most 20')
                continue
            if '`' in v:
                err(n, f'`code` in the field "{f}" of {where}: only in Controls')
            if section == 'Meta' and re.search(r'[*\[<`]', v):
                err(n, f'the field "{f}" of Meta is plain text: no **bold**, *emphasis*, links or HTML')
            if len(plain(v)) > fields[f]:
                err(n, f'the field "{f}" of {where} is {len(plain(v))} characters, at most {fields[f]}')
            if (section, f) in EMPHASIS_ONCE and level == 2 and len(re.findall(r'(?<!\*)\*[^*]+\*(?!\*)', v)) > 1:
                err(n, f'the field "{f}" of {where} may have one *emphasis* at most')
            links += inline(path, n, v, anchors, root, errors)
        # the paragraphs after it
        paras, cur = [], None
        for n, t in body[i:]:
            if not t.strip():
                cur = None
                continue
            if re.match(r'^\s*([-*+>|]|\d+[.)])\s', t):
                err(n, f'a list, quote or table in {where}: only the field list right under the heading')
            if cur is None:
                cur = [n, t]
                paras.append(cur)
            else:
                cur[1] += ' ' + t.strip()
        if seen and i < len(body) and body[i][1].strip():
            err(body[i][0], f'a blank line must separate the field list of {where} from its paragraph')
        if para is None:
            for n, _ in paras:
                err(n, f'{where} has no paragraph of its own')
        elif len(paras) != 1:
            err(n0, f'{where} must have exactly one paragraph, it has {len(paras)}')
        for n, p in paras:
            if para is not None and len(plain(p)) > para:
                err(n, f'the paragraph of {where} is {len(plain(p))} characters, at most {para}')
            if '`' in p:
                err(n, f'`code` in {where}: only in Controls')
            links += inline(path, n, p, anchors, root, errors)
    if not 3 <= moves <= 6:
        errors.append(f'{path}: {moves} moves (### under Session), expected 3 to 6')
    return links


def inline(path, n, s, anchors, root, errors):
    """No HTML, no images; links to the docs, images/ or https:// that resolve."""
    if '<' in s:
        errors.append(f'{path}:{n}: raw HTML ("<"): only **bold**, *emphasis* and [links](...)')
    if '![' in s:
        errors.append(f'{path}:{n}: an image: not on the landing page\'s text')
    if re.search(r'[\[\]]', plain(s)):
        errors.append(f'{path}:{n}: a [ or ] that isn\'t a [link](target)')
    count = 0
    for m in re.finditer(r'\[[^\]]*\]\(([^)\s]+)\)', s):
        target = m.group(1)
        count += 1
        if re.match(r'^https://', target):
            continue
        file, _, frag = target.partition('#')
        if file not in ('MANUAL.md', 'INSTALL.md', 'QUICKSTART.md') and not file.startswith('images/'):
            errors.append(f'{path}:{n}: {target}: a link goes to MANUAL.md, INSTALL.md, QUICKSTART.md '
                          '(with an #anchor), images/ or https://')
        elif not os.path.exists(os.path.join(root, file)):
            errors.append(f'{path}:{n}: {target}: no such file')
        elif frag and frag not in anchors.get(file, {}):
            errors.append(f'{path}:{n}: {target}: no such heading in {file}')
    return count


# A valid site/landing.md for the selftest, so its breaks don't depend on the real one's words
LANDING_FIXTURE = """# Front page

<!-- a comment -->

## Meta

- Title: A title
- Description: A description.

## Hero

- Eyebrow: An eyebrow
- Title: Loop it, then *mangle* it
- Assure: Free.

**FRIZZ** turns it into a box, with the
[looper](MANUAL.md#looper) in it.

## Keys

- Title: Hold a key.

The hint.

## Session

- Eyebrow: How it plays
- Title: To the *drop*

### One

- Controls: `LOOP`

Move one.

### Two

- Controls: `A` `B`

Move two.

### Three

- Controls: `C`

Move three.

## Get started

- Eyebrow: Get started
- Title: From the card
- Install: Swap the file.
- Quick start: Ten minutes.
- Manual link: The manual

The hint.

## Credits

Builds on [the release](https://example.com/).
"""


def selftest(root, quiet=False):
    """Breaks a copy of the docs one way at a time; each break must be reported. The breaks find
    their place by structure (the first knob table, the first effect's summary …), not by words."""
    def edit(path, fn):
        def apply(dir):
            p = os.path.join(dir, path)
            assert not os.path.islink(p), p
            t = open(p, encoding='utf-8').read()
            t2 = fn(t)
            if t2 is None or t2 == t:
                raise LookupError(f'nothing to break in {path}')
            open(p, 'w', encoding='utf-8').write(t2)
        return apply

    def sub(path, pattern, new):
        """the first match of pattern replaced"""
        return edit(path, lambda t: re.sub(pattern, new, t, count=1, flags=re.M))

    def after_table(pattern, new):
        """the first match of pattern after MANUAL.md's first `| Knob | Page 1 |` table head"""
        def fn(t):
            i = t.find('| Knob | Page 1 |')
            return None if i < 0 else t[:i] + re.sub(pattern, new, t[i:], count=1, flags=re.M)
        return edit('MANUAL.md', fn)

    def effect_summary(new):
        """the summary of the first chapter with a `| Knob | Page 1 |` table, its line replaced
        by new(line)"""
        def fn(t):
            i = t.find('| Knob | Page 1 |')
            starts = [m for m in re.finditer(r'^<!-- summary:.*$', t[:i], re.M)]
            if i < 0 or not starts:
                return None
            m = starts[-1]
            return t[:m.start()] + new(m.group(0)) + t[m.end():]
        return edit('MANUAL.md', fn)

    M, Q, I, S = 'MANUAL.md', 'QUICKSTART.md', 'INSTALL.md', LANDING
    LED, FX, PK = (f'{SRC}/{f}' for f in ('LedColors.h', 'FxSlots.h', 'PanelKeys.h'))
    cases = [
        # the knob tables
        ('a knob table without row 4', after_table(r'^\| 4 \|.*\n', ''), 'has no row 4'),
        ('a knob table without row 1', after_table(r'^\| 1 \|.*\n', ''), 'has no row 1'),
        ('a knob row without a name', after_table(r'^\| 2 \| ', '| 2 | (no name): '), 'knob 2 of'),
        ('an effect without its knob table', sub(M, r'^\| Knob \| Page 1 \|', '| Knob | Page one |'),
         'has no knob table'),
        # summaries
        ('a second summary', effect_summary(lambda s: s + '\n<!-- summary: Again. -->'), 'a second summary'),
        ('a summary after text', effect_summary(lambda s: 'Text first.\n\n' + s), 'right under its heading'),
        ('a summary under a ###', sub(M, r'^(### .*\n)', r'\1<!-- summary: A section. -->\n'),
         'not under a ## heading'),
        ('an effect without a summary', effect_summary(lambda s: ''), 'has no `<!-- summary'),
        ('an effect summary over 160 characters',
         effect_summary(lambda s: '<!-- summary: ' + 'A fold, ' * 20 + 'folding. -->'), 'at most 160'),
        ('an effect summary of 3 sentences',
         effect_summary(lambda s: '<!-- summary: Folds. Bright. Gritty. -->'), '1 to 2 sentences'),
        ('a chapter summary of 2 sentences',
         sub(M, r'^<!-- summary:.*-->', '<!-- summary: What it does. How to read it. -->'), 'one sentence'),
        ('a typo in a marker', effect_summary(lambda s: s.replace('summary:', 'sumary:')),
         "isn't a marker the site reads"),
        ('a marker over 2 lines', effect_summary(lambda s: s.replace(' -->', '\n-->')),
         "isn't a marker the site reads"),
        ('a guide without its summary', sub(I, r'^<!-- summary:.*\n', ''), 'INSTALL.md: no `<!-- summary'),
        ('a guide summary of 2 sentences', sub(Q, r'^<!-- summary:.*-->', '<!-- summary: Levels. A loop. -->'),
         'one sentence'),
        # parts
        ('a part marker without a blank line after', sub(M, r'^(<!-- part: .*-->)\n\n', r'\1\n'),
         'blank line before and after'),
        ('a part marker away from a ##', sub(M, r'^(<!-- part: .*-->)\n\n', r'\1\n\nText.\n\n'),
         'right before a ## heading'),
        ('a part marker in a code block', sub(M, r'^(## .*\n)', r'\1\n```\n<!-- part: X -->\n```\n'),
         'in a code block'),
        # headings, the key row, links
        ('a second H1', sub(M, r'^## ', '# '), 'H1 headings'),
        ('a key row of 14 cells', sub(Q, r'^(\| Effect \|.*\|)[^|\n]*\|$', r'\1'), 'has 14 cells'),
        ('a link to no heading', sub(Q, r'MANUAL\.md#[\w-]+', 'MANUAL.md#no-such'), 'no such heading'),
        ('a link to the README', sub(Q, r'\]\(MANUAL\.md\)', '](README.md)'), 'the README'),
        # the keys' colours
        ('a slot colour not in LedColors.h', sub(FX, r'(\{Hardware::SwId::KEY_\d+, )\w+,', r'\1rosa,'),
         'colour "rosa"'),
        ('a colour that is no float literal', sub(LED, r'(\[3\] = \{)[^,]*', r'\1ONE'), "isn't 3 float literals"),
        ("a slot entry the site can't read", sub(FX, r'\{Hardware::SwId::KEY_(\d+), ', r'{kKey\1, '),
         "an entry the site can't read"),
        ('a key without a slot', sub(FX, r'^\s*\{Hardware::SwId::KEY_\d+,.*\n', ''), 'has no slot'),
        ('no compressor key', sub(FX, r'kCompKey\s*=', 'kCompressorKey ='), 'no `kCompKey'),
        ('the white keys out of order', sub(PK, r'(kWhiteKeys\[\w*\]\s*=\s*\{\s*Hardware::SwId::)KEY_1,', r'\1KEY_25,'),
         "kWhiteKeys isn't KEY_1 to KEY_15"),
        # site/landing.md (LANDING_FIXTURE)
        ('a landing field missing', sub(S, r'^- Assure: .*\n', ''), 'Hero has no field "Assure"'),
        ('a landing field unknown', sub(S, r'^(- Assure: .*\n)', r'\1- Subtitle: More.\n'), 'unknown field "Subtitle"'),
        ('a landing field twice', sub(S, r'^(- Title: Hold a key.*\n)', r'\1\1'), 'twice'),
        ('a landing field too long', sub(S, r'^- Eyebrow: An eyebrow$', '- Eyebrow: ' + 'x' * 41), 'at most 40'),
        ('a landing field empty', sub(S, r'^- Manual link: .*$', '- Manual link:'), 'is empty'),
        ('a landing section missing', sub(S, r'^## Keys\n(?:.*\n)*?(?=## Session)', ''), 'the sections are'),
        ('the last landing section missing', sub(S, r'^## Credits\n(?:.*\n)*', ''), 'the sections are'),
        ('a landing section unknown', sub(S, r'^## Keys$', '## Keyboard'), 'unknown section "Keyboard"'),
        ('a landing paragraph too many', sub(S, r'^(Move one\.\n)', r'\1\nAnd more.\n'), 'exactly one paragraph'),
        ('a landing paragraph missing', sub(S, r'^Move two\.\n', ''), 'exactly one paragraph'),
        ('a paragraph in Meta', sub(S, r'^(- Description: .*\n)', r'\1\nA word.\n'), 'no paragraph of its own'),
        ('no blank line after the fields', sub(S, r'^(- Title: Hold a key.*)\n\n', r'\1\n'), 'blank line must separate'),
        ('too many controls', sub(S, r'^- Controls: `LOOP`$', '- Controls: `A` `B` `C` `D` `E`'), '1 to 4'),
        ('a control too long', sub(S, r'^- Controls: `LOOP`$', '- Controls: `' + 'x' * 21 + '`'), 'at most 20'),
        ('2 moves', sub(S, r'^### Two\n(?:.*\n)+?(?=## Get)', ''), 'expected 3 to 6'),
        ('raw HTML in landing.md', sub(S, r'^\*\*FRIZZ\*\*', '<b>FRIZZ</b>'), 'raw HTML'),
        ('an image in landing.md', sub(S, r'^Move one\.', '![A key](images/key.svg)'), 'an image'),
        ('a list in landing.md', sub(S, r'^Move one\.', '- Move one.'), 'a list, quote or table'),
        ('a quote in landing.md', sub(S, r'^Move one\.', '> Move one.'), 'a list, quote or table'),
        ('a table in landing.md', sub(S, r'^Move one\.', '| Move | one |'), 'a list, quote or table'),
        ('a link without its target', sub(S, r'^Move one\.', 'See [the manual].'), "isn't a [link](target)"),
        ('emphasis in Meta', sub(S, r'^- Title: A title$', '- Title: A *title*'), 'plain text'),
        ('HTML in a move title', sub(S, r'^### One$', '### One <br>'), 'raw HTML'),
        ('code in a move title', sub(S, r'^### One$', '### `One`'), 'only in Controls'),
        ('code in a paragraph', sub(S, r'^Move one\.', 'Press `LOOP`.'), 'only in Controls'),
        ('code in a field', sub(S, r'^- Install: .*$', '- Install: Swap `FRIZZ.bin`.'), 'only in Controls'),
        ('a landing link to no heading', sub(S, r'MANUAL\.md#looper', 'MANUAL.md#no-such-heading'),
         'no such heading in MANUAL.md'),
        ('a landing link into the repo', sub(S, r'MANUAL\.md#looper', 'firmware/README.md'),
         'a link goes to MANUAL.md'),
        ('two emphases in a title', sub(S, r'^- Title: Loop it, then \*mangle\* it$', '- Title: *Loop* it, *mangle*'),
         'one *emphasis*'),
    ]
    files = LINKED + ['firmware/README.md', LANDING] + [LED, FX, PK]
    failed = 0
    if check(root)[0]:
        print('FAIL selftest: the docs as they are already have errors; fix those first')
        return 1
    for name, apply, expect in cases:
        with tempfile.TemporaryDirectory(prefix='doclinks-') as dir:
            for f in files:
                if os.path.exists(os.path.join(root, f)):
                    os.makedirs(os.path.dirname(os.path.join(dir, f)), exist_ok=True)
                    shutil.copy(os.path.join(root, f), os.path.join(dir, f))
            # the rest of the repo, for the links, linked in beside the copies (never written)
            for d in {''} | {os.path.dirname(f) for f in files} | {
                    os.path.dirname(f)[:i] for f in files for i, c in enumerate(os.path.dirname(f)) if c == '/'}:
                for entry in os.listdir(os.path.join(root, d)):
                    if not os.path.lexists(os.path.join(dir, d, entry)):
                        os.symlink(os.path.join(root, d, entry), os.path.join(dir, d, entry))
            open(os.path.join(dir, LANDING), 'w', encoding='utf-8').write(LANDING_FIXTURE)
            try:
                if check(dir)[0]:
                    raise LookupError(f'the copy has errors before the break: {check(dir)[0]}')
                apply(dir)
            except LookupError as e:
                failed += 1
                print(f'FAIL  {name}: the case no longer applies ({e}): update it in doclinks.py')
                continue
            errors = check(dir)[0]
        hit = [e for e in errors if expect in e]
        if hit:
            if not quiet:
                print(f'ok    {name}: {hit[0]}')
        else:
            failed += 1
            print(f'FAIL  {name}: expected an error with "{expect}", got: {errors or "none"}')
    if not quiet or failed:
        print(f'{"FAIL selftest: " if failed else ""}{len(cases) - failed} of {len(cases)} breaks reported')
    return 1 if failed else 0


def figures(root):
    """The pictures under images/ as diagrams.py draws them now: the chapters' figures read
    MANUAL.md's knob names and the firmware's colours, so a change to either leaves them stale"""
    run = subprocess.run([sys.executable, os.path.join(root, 'firmware/tools/diagrams/diagrams.py'),
                          '--check'], capture_output=True, text=True)
    if run.returncode == 0:
        return []
    return run.stdout.split('\n')[:-1] + run.stderr.split('\n')[:-1] or ['diagrams.py --check failed']


def main():
    root = subprocess.run(['git', 'rev-parse', '--show-toplevel'], capture_output=True, text=True,
                          cwd=os.path.dirname(os.path.abspath(__file__))).stdout.strip()
    if sys.argv[1:] == ['--selftest']:
        return selftest(root)
    if sys.argv[1:] not in ([], ['--all']):
        print(__doc__)
        return 2
    failed = selftest(root, quiet=True) if sys.argv[1:] == ['--all'] else 0
    errors, warnings, checked = check(root)
    errors += figures(root)
    for w in warnings:
        print('warning:', w)
    for e in errors:
        print('error:', e)
    print(f'{"FAIL: " if errors else ""}{checked} links checked, {len(errors)} errors, '
          f'{len(warnings)} warnings')
    return 1 if errors or failed else 0


if __name__ == '__main__':
    sys.exit(main())
