#!/usr/bin/env python3
"""Draws the docs' diagrams into images/: signal-flow, input, fx-chain, clock-midi, panel and
the quick start's quickstart-flow; and the chapters' figures: a key locator for each white key
(key-*), the panel with what a chapter plays lit (panel-*) and gestures as keycaps (keys-*).

Plain block diagrams, by hand in coordinates of a 720 px wide picture (the quick start's and the
chapters' figures 480, so they read on a phone): boxes, arrows, lettered
switches and a legend. What they show is the code's (firmware/code/src: passthroughEngine.h,
FxChain.h, FxOutput.h, Finalizer.h, TempoClock.h, MidiClock.h, MidiOut.h, SettingsPage.h) and
MANUAL.md's; the panel's positions come from firmware/twin/web/layout.json (CHOMPI's board
file) and the enclosure's drawings in reference/hardware. The chapters' figures read their
colours from the firmware as the website does (LedColors.h, FxSlots.h, NormalPage.h's banks) and
the knobs' names from MANUAL.md's knob tables, so run it again after changing either.

Run it from anywhere, standard library only; it writes the same bytes every time:

    firmware/tools/diagrams/diagrams.py
    firmware/tools/diagrams/diagrams.py --check   # writes nothing; exit 1 if a picture is stale

The look is the website's, frizz-web's PUMPWERKER system (design/system/tokens.json): soot,
chalk and the greys, square, flat, 2 px lines, 1.5 px for controls and LED rings, no coloured
text. Each picture has its own soot ground, so it reads the same on the site and on GitHub,
light or dark. The LED colours are the firmware's, each in a soot ring. The text is IBM Plex
Mono, named with fallbacks rather than drawn as paths: an <img> loads no webfonts, every
monospace font sets 0.6 em a character, so the layout holds with any of them, and the files
stay small and their text real.
"""
import contextlib
import io
import json
import math
import os
import re
import sys
from xml.sax.saxutils import escape

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
OUT = os.path.join(ROOT, 'images')

BG = '#1B1B19'      # soot: the ground
RAISED = '#26261F'  # soot-raised: a surface on it, the usual way's boxes, a dark key
FG = '#EEEBE3'      # chalk: names, the usual way, the badges
ACC = FG            # the usual way through, the switches: chalk, as the names
LINE = '#A9A49A'    # ash: the other ways, the other boxes
MUTED = LINE        # ash: notes, as glosses on soot
DIMMED = '#77736B'  # ash-dark: control lines, what a figure doesn't use
KEY = RAISED        # a dark key's fill
FONT = "'IBM Plex Mono', ui-monospace, Menlo, monospace"  # names, values, keys
SANS = "'IBM Plex Sans', system-ui, sans-serif"            # notes and running text
BOLD = '600'        # Plex's semibold, the system's values
W = 720


def num(v):
    """A coordinate without trailing zeros, so the files stay small and stable"""
    return ('%.1f' % v).rstrip('0').rstrip('.')


def px(size, sans=False):
    """The size a text is set at, from the size the pictures were laid out with (for Arial):
    sans as laid out, at least 12 px; mono one px under it, as it sets wider, at least 11 px, so
    nothing gets too small on a phone, where the site shows a picture at down to 3/4"""
    return max(12, size) if sans else max(11, size - 1)


def width(s, size, bold=False, sans=False):
    """About how wide s is set at px(size): monospace at 0.6 em a character in any font,
    Plex Sans by its letters' widths; for checking that a label fits its box"""
    if not sans:
        return len(s) * .6 * px(size)
    narrow = 'iljtfrI.,:;|!()[]\' '
    w = 0.0
    for c in s:
        if c in narrow:
            w += .3
        elif c.isupper() or c in 'mw#&×↔–':
            w += .7
        else:
            w += .55
    return w * px(size, True) * 1.1 * (1.05 if bold else 1)


class Svg:
    def __init__(self, h, title, desc, w=W):
        self.w = w
        self.h = h
        self.title = title
        self.desc = desc
        self.items = []
        self.style = ''  # a figure's own classes, after the shared ones
        self.defs = []   # its own definitions

    def add(self, s):
        self.items.append(s)

    def text(self, x, y, s, size=13, anchor='middle', cls='t', bold=False, rotate=None, sans=None):
        """sans: Plex Sans, for notes (cls 'm', the default) and running text; else Plex Mono"""
        if sans is None:
            sans = cls == 'm'
        if sans and cls != 'm':
            cls += ' sn'
        elif not sans and cls == 'm':
            cls += ' mo'
        # the anchor as a class: the style sheet's text-anchor would win over an attribute
        if anchor != 'middle':
            cls += ' ' + {'start': 'as', 'end': 'ae'}[anchor]
        attrs = 'x="%s" y="%s" class="%s"' % (num(x), num(y), cls)
        if px(size, sans) != 12:
            attrs += ' font-size="%d"' % px(size, sans)
        if bold:
            attrs += ' font-weight="%s"' % BOLD
        if rotate is not None:
            attrs += ' transform="rotate(%s %s %s)"' % (num(rotate), num(x), num(y))
        else:
            w = width(s, size, bold, sans)
            left = x - {'start': 0, 'middle': .5, 'end': 1}[anchor] * w
            if left < 0 or left + w > self.w:
                print('  past the edge: %r' % s)
        self.add('<text %s>%s</text>' % (attrs, escape(s)))

    def box(self, x, y, w, h, lines, accent=False, check=True):
        """A box with lines of text, centred: (text, 'h' a name | 'm' a note); a plain string
        is a name. A note that only names keys or a number (SHIFT + VOLUME, 12) is set in mono,
        as the keys are, the other notes in sans"""
        self.add('<rect x="%s" y="%s" width="%s" height="%s" class="%s"/>'
                 % (num(x), num(y), num(w), num(h), 'ba' if accent else 'bx'))
        lines = [(l, 'h') if isinstance(l, str) else l for l in lines]
        step = 16
        top = y + h / 2 - step * (len(lines) - 1) / 2 + 4.5
        for i, (s, kind) in enumerate(lines):
            size = 13 if kind == 'h' else 12
            sans = kind == 'm' and not re.fullmatch(r'[A-Z0-9#+ ]+', s)
            if check and width(s, size, kind == 'h', sans) > w - 6:
                print('  tight: %r in %d px' % (s, w))
            self.text(x + w / 2, top + i * step, s, size, cls='t' if kind == 'h' else 'm',
                      bold=kind == 'h', sans=sans)

    def line(self, pts, kind='s', arrow=True):
        """kind: 'a' the usual way, 's' audio, 'c' control (dashed)"""
        d = ' '.join('%s,%s' % (num(x), num(y)) for x, y in pts)
        m = ' marker-end="url(#m%s)"' % kind if arrow else ''
        self.add('<polyline points="%s" class="l%s"%s/>' % (d, kind, m))

    def dot(self, x, y, kind='s'):
        self.add('<circle cx="%s" cy="%s" r="3" class="d%s"/>' % (num(x), num(y), kind))

    def sum(self, x, y):
        self.add('<circle cx="%s" cy="%s" r="9" class="b"/>' % (num(x), num(y)))
        self.add('<path d="M%s,%sh10M%s,%sv10" class="ls"/>' % (num(x - 5), num(y), num(x), num(y - 5)))

    def jack(self, x, y):
        self.add('<circle cx="%s" cy="%s" r="5" class="b"/>' % (num(x), num(y)))

    def badge(self, x, y, letter):
        self.add('<rect x="%s" y="%s" width="18" height="18" class="g"/>' % (num(x - 9), num(y - 9)))
        self.text(x, y + 4.5, letter, 12, cls='gt', bold=True)

    def switch(self, x, y, letter, kind='s'):
        """An on/off switch on a line from x to x + 30, open, its letter above"""
        self.add('<circle cx="%s" cy="%s" r="2.5" class="b"/>' % (num(x + 2.5), num(y)))
        self.add('<circle cx="%s" cy="%s" r="2.5" class="b"/>' % (num(x + 27.5), num(y)))
        self.add('<path d="M%s,%sL%s,%s" class="sw"/>' % (num(x + 4), num(y - 1), num(x + 26), num(y - 11)))
        self.badge(x + 15, y - 24, letter)

    def selector(self, cx, cy, ends, on, letter, badge_at):
        """A switch between ends (contacts) and its common pole at cx, cy, set to ends[on]"""
        self.add('<circle cx="%s" cy="%s" r="2.5" class="b"/>' % (num(cx), num(cy)))
        for ex, ey in ends:
            self.add('<circle cx="%s" cy="%s" r="2.5" class="b"/>' % (num(ex), num(ey)))
        ex, ey = ends[on]
        # the lever stops short of the contact, so it reads as a switch
        dx, dy = ex - cx, ey - cy
        k = 1 - 6 / (dx * dx + dy * dy) ** .5
        self.add('<path d="M%s,%sL%s,%s" class="sw"/>' % (num(cx), num(cy), num(cx + dx * k), num(cy + dy * k + 4)))
        self.badge(badge_at[0], badge_at[1], letter)

    def legend(self, x, y, rows, wrap=None):
        """rows: (letter or line kind, text)"""
        for i, (mark, s) in enumerate(rows):
            yy = y + i * 22
            if mark in ('a', 's', 'c'):
                self.line([(x - 4, yy - 4), (x + 22, yy - 4)], mark, arrow=False)
            elif mark:
                self.badge(x + 9, yy - 4, mark)
            self.text(x + 32, yy, s, 12, anchor='start', cls='t', sans=True)
            if width(s, 12, sans=True) + x + 32 > self.w - 8:
                print('  legend too wide: %r' % s)

    def render(self):
        defs = ''.join(
            '<marker id="m%s" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="9" '
            'markerHeight="9" markerUnits="userSpaceOnUse" orient="auto">'
            '<path d="M0,0L10,5L0,10z" fill="%s"/></marker>' % (k, c)
            for k, c in (('a', ACC), ('s', LINE), ('c', DIMMED)))
        style = (
            # px(13) on the picture, inherited: a text's own font-size wins over it, where a
            # text{} rule's would win over the attribute
            'svg{font-size:12px}text{font-family:%s;fill:%s;text-anchor:middle}'
            '.m,.sn{font-family:%s}.m.mo{font-family:%s}'
            '.as{text-anchor:start}.ae{text-anchor:end}.m{fill:%s}.gt{fill:%s}'
            '.b{fill:none;stroke:%s;stroke-width:1.5}'
            '.bx{fill:none;stroke:%s;stroke-width:2}'
            '.ba{fill:%s;stroke:%s;stroke-width:2}'
            '.la{fill:none;stroke:%s;stroke-width:2}'
            '.ls{fill:none;stroke:%s;stroke-width:2}'
            '.lc{fill:none;stroke:%s;stroke-width:1.5;stroke-dasharray:5 4}'
            '.da{fill:%s}.ds{fill:%s}'
            '.sw{stroke:%s;stroke-width:2}'
            '.g{fill:%s}.j{fill:%s}'
            '.wk{fill:%s;stroke:%s;stroke-width:1.5}'
            '.dk{fill:%s;stroke:%s;stroke-width:1.5}'
            '.sk{fill:%s;stroke:%s;stroke-width:1.5}'
            '.led{fill:none;stroke:%s;stroke-width:1.5}'
            % (FONT, FG, SANS, FONT, MUTED, BG, LINE, LINE, RAISED, ACC, ACC, LINE, DIMMED, ACC, LINE, ACC,
               FG, LINE, FG, BG, KEY, LINE, LINE, BG, LINE))
        out = [
            '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d" '
            'role="img" aria-labelledby="title desc">' % (self.w, self.h, self.w, self.h),
            '<title id="title">%s</title>' % escape(self.title),
            '<desc id="desc">%s</desc>' % escape(self.desc),
            '<style>%s</style>' % (style + self.style),
            '<defs>%s</defs>' % (defs + ''.join(self.defs)),
            '<rect width="%d" height="%d" fill="%s"/>' % (self.w, self.h, BG),
        ] + self.items + ['</svg>']
        return '\n'.join(out) + '\n'


# ---------- 1. the whole way at a glance ----------
def signal_flow():
    s = Svg(440, 'FRIZZ signal flow',
            'The whole way of the sound. The input, AUX, the built-in mic or USB after its input gain, '
            'goes to the looper and to the input/loop mix; the looper\'s playback is the input/loop '
            'mix\'s other side. The input/loop mix goes through the effects of the white keys and '
            'the finalizer. A source set past the effects: the input/loop mix\'s input '
            'share skips the effects and the finalizer and joins after them. From there the master '
            'goes to the computer over USB, and through VOLUME, the output gain, and a safety limiter on each output: the line '
            'out, and the headphones, whose feed can blend in the input alone.')
    # row 1: input, looper, mix, effects, finalizer, the branch past them joining
    s.box(20, 42, 110, 56, ['INPUT', ('AUX, mic or USB', 'm'), ('input gain', 'm')], accent=True)
    s.line([(130, 70), (295, 70)], 'a')
    s.dot(150, 70, 'a')
    s.text(225, 62, 'input', 12, cls='m')
    s.line([(150, 70), (150, 150), (175, 150)])
    s.box(175, 126, 120, 48, ['LOOPER', ('records the input', 'm')])
    s.line([(295, 150), (347, 150), (347, 98)])
    s.text(321, 143, 'loop', 12, cls='m')
    s.box(295, 42, 105, 56, ['MIX', ('input ↔ loop', 'm'), ('SHIFT + VOLUME', 'm')])
    s.line([(400, 70), (420, 70)], 'a')
    s.box(420, 42, 120, 56, ['EFFECTS', ('white keys 1–14', 'm')], accent=True)
    s.line([(540, 70), (560, 70)], 'a')
    s.box(560, 42, 110, 56, ['FINALIZER', ('white key 15', 'm')], accent=True)
    s.line([(670, 70), (679, 70)], 'a')
    s.sum(688, 70)
    s.line([(385, 98), (385, 205), (688, 205), (688, 79)])
    s.text(525, 223, 'the input\'s share, past the effects and the finalizer', 12, cls='m')
    # on to row 2
    s.line([(697, 70), (708, 70), (708, 240), (75, 240), (75, 268)], 'a')
    # the master before VOLUME to the computer
    s.dot(600, 240)
    s.line([(600, 240), (600, 300)])
    s.jack(600, 307)
    s.text(600, 332, 'USB OUT', 13, bold=True)
    s.text(600, 348, 'to the computer,', 12, cls='m')
    s.text(600, 364, 'before VOLUME', 12, cls='m')
    s.box(20, 268, 110, 56, ['VOLUME', ('output gain', 'm')], accent=True)
    s.line([(130, 296), (150, 296), (150, 280), (330, 280)], 'a')
    s.dot(150, 296, 'a')
    s.line([(150, 296), (150, 340), (175, 340)])
    s.box(175, 316, 120, 48, ['PHONES FEED', ('VOLUME page 3', 'm')])
    s.line([(295, 340), (330, 340)])
    s.line([(235, 400), (235, 364)])
    s.text(235, 416, 'the input alone, at VOLUME', 12, cls='m')
    for y, kind, name in ((280, 'a', 'LINE OUT'), (340, 's', 'PHONES')):
        s.box(330, y - 22, 100, 44, ['LIMITER', ('safety', 'm')], accent=kind == 'a')
        s.line([(430, y), (465, y)], kind)
        s.jack(472, y)
        s.text(484, y + 4.5, name, 13, anchor='start', bold=True)
    s.legend(360, 400, [('a', 'the usual way'), ('s', 'other ways')])
    return s


# ---------- 2. the input ----------
def input_diagram():
    D = 70  # the USB row's: everything below the sources moves down by it
    s = Svg(650 + D, 'FRIZZ input',
            'The input. AUX (stereo, or mono: its left to both sides), the built-in mic and USB (a '
            'computer\'s audio) each have their own input gain; AUX is then amplified 3× and its DC '
            'taken out, the mic 5×, DC blocked, highpassed at about 150 Hz and the same on both '
            'sides, USB 3× as AUX. Switch A (upper F#, or the AUX jack between AUX and the mic) '
            'picks the source, with a 5 ms crossfade. The source goes as it is to the looper and '
            'to the headphone feed. Switch B (upper G#), its monitoring, has it on the outputs or '
            'not, switch C (upper A#) sends it through the effects or past them. The input/loop '
            'mix sets the input\'s share against the loop\'s; the share past the effects joins '
            'after the finalizer, before VOLUME.')
    # AUX
    s.jack(28, 90)
    s.text(20, 72, 'AUX IN', 13, anchor='start', bold=True)
    s.line([(33, 90), (70, 90)], 'a')
    s.box(70, 66, 90, 48, ['STEREO', ('or mono', 'm')])
    s.badge(160, 66, 'M')
    s.line([(160, 90), (185, 90)], 'a')
    s.box(185, 66, 100, 48, ['INPUT GAIN', ('AUX\'s own', 'm')])
    s.line([(285, 90), (310, 90)], 'a')
    s.box(310, 66, 100, 48, ['× 3', ('DC block', 'm')])
    s.line([(410, 90), (550, 90), (550, 125), (562, 125)], 'a', arrow=False)
    # the mic
    s.jack(28, 190)
    s.text(20, 172, 'MIC (built in)', 13, anchor='start', bold=True)
    s.line([(33, 190), (185, 190)])
    s.box(185, 166, 100, 48, ['INPUT GAIN', ('the mic\'s own', 'm')])
    s.line([(285, 190), (310, 190)])
    s.box(310, 166, 100, 48, ['× 5', ('DC block', 'm')])
    s.line([(410, 190), (435, 190)])
    s.box(435, 166, 100, 48, ['HIGHPASS', ('150 Hz, mono', 'm')])
    s.line([(535, 190), (550, 190), (550, 155), (562, 155)], arrow=False)
    # USB
    s.jack(28, 290)
    s.text(20, 272, 'USB (a computer)', 13, anchor='start', bold=True)
    s.line([(33, 290), (185, 290)])
    s.box(185, 266, 100, 48, ['INPUT GAIN', ('USB\'s own', 'm')])
    s.line([(285, 290), (310, 290)])
    s.box(310, 266, 100, 48, ['× 3', ('as AUX', 'm')])
    s.line([(410, 290), (556, 290), (556, 185), (562, 185)], arrow=False)
    # A: the source
    s.selector(600, 155, [(565, 125), (565, 155), (565, 185)], 0, 'A', (600, 104))
    s.text(612, 210, '5 ms crossfade', 12, cls='m')
    s.text(560, 34, 'the AUX jack: a cable in picks AUX, out the mic', 12, cls='m')
    s.line([(583, 42), (583, 128)], 'c')
    s.line([(602, 155), (705, 155), (705, 262 + D), (40, 262 + D), (40, 330 + D)], 'a', arrow=False)
    s.line([(40, 330 + D), (40, 490 + D)], arrow=False)
    s.dot(40, 330 + D, 'a')
    # B: the monitoring, whether you hear it; C: then how, through the effects or past them
    s.line([(40, 330 + D), (75, 330 + D)], 'a', arrow=False)
    s.switch(75, 330 + D, 'B')
    s.line([(105, 330 + D), (157, 330 + D)], 'a', arrow=False)
    s.selector(160, 330 + D, [(195, 305 + D), (195, 355 + D)], 0, 'C', (170, 298 + D))
    s.line([(197, 305 + D), (330, 305 + D)], 'a')
    s.text(263, 298 + D, 'through the effects', 12, cls='m')
    s.line([(197, 355 + D), (330, 355 + D)])
    s.text(263, 348 + D, 'past the effects', 12, cls='m')
    # the looper and the phones take the source as it is
    s.line([(40, 420 + D), (200, 420 + D)])
    s.dot(40, 420 + D)
    s.text(120, 412 + D, 'meter: VOLUME page 2', 12, cls='m')
    s.box(200, 398 + D, 100, 44, ['LOOPER', ('records it', 'm')])
    s.line([(300, 420 + D), (330, 420 + D)])
    s.text(315, 413 + D, 'loop', 11, cls='m')
    s.line([(40, 490 + D), (470, 490 + D)])
    s.text(250, 483 + D, 'the source alone, whatever B and C say', 12, cls='m')
    s.text(478, 494.5 + D, 'to the phones feed (VOLUME p. 3)', 13, anchor='start', sans=True)
    s.box(330, 285 + D, 140, 157, ['MIX', ('SHIFT + VOLUME', 'm'), ('', 'm'), ('the input\'s share', 'm'),
                                   ('against the loop\'s,', 'm'), ('on both inputs', 'm')], check=False)
    s.line([(470, 315 + D), (505, 315 + D)], 'a')
    s.text(512, 319.5 + D, 'to the effects', 13, anchor='start', bold=True)
    s.text(512, 336 + D, 'and the finalizer', 12, anchor='start', cls='m')
    s.line([(470, 385 + D), (505, 385 + D)])
    s.text(512, 389.5 + D, 'past the effects:', 13, anchor='start', sans=True)
    s.text(512, 406 + D, 'joins after the finalizer', 12, anchor='start', cls='m')
    s.legend(28, 540 + D, [
        ('A', 'upper F#: the source, AUX, the mic or USB. The AUX jack picks AUX or the mic'),
        ('B', 'upper G#: monitoring, on the outputs or not'),
        ('C', 'upper A#: through the effects, or past them and the finalizer'),
        ('M', 'SHIFT + upper F#: AUX mono, its left channel to both sides'),
    ])
    s.text(28, 634 + D, 'On the settings page (mode switch up). Each source keeps its own gain, B and C.',
           12, anchor='start', cls='m')
    return s


# ---------- 3. the effects, the finalizer and the outputs ----------
def fx_chain():
    s = Svg(830, 'FRIZZ effects chain',
            'The order FRIZZ computes the effects in. From the input/loop mix: the freezer (key 1), then the '
            'shifter, folder, crusher, filter, flanger and phaser (keys 2-7), which sit inside the '
            'resonator\'s feedback loop (key 8): it taps the signal after the phaser and feeds its '
            'ringing back in after the freezer. Then the slicer, wow & flutter and the tape stop '
            '(keys 9-11). The delay (key 12) and the reverb (key 13) are sends: the key fades what '
            'goes in, page 2\'s Band filters it, Level sets the return, which is added to the '
            'signal; the reverb gets the delay\'s echoes too. The keys and knobs come from the '
            'panel, MIDI and scene morphs; the chaos key (14) drops latched effects and scrambles '
            'the loop. After the effects the finalizer (key 15); then the input past the effects joins, '
            'then VOLUME and a safety limiter on the line out and on the headphones, whose feed blends '
            'towards the input alone. Below: every insert\'s page 2, Band, Level and Mix.')
    # what plays the effects
    for x, w, lines in ((20, 170, ['PANEL · MIDI', ('keys and knobs', 'm')]),
                        (210, 190, ['SCENES · MORPHS', ('the dark keys', 'm')]),
                        (420, 180, ['CHAOS  14', ('drops latched effects,', 'm'), ('scrambles the loop', 'm')])):
        s.box(x, 14, w, 56 if len(lines) == 3 else 46, lines)
        s.line([(x + w / 2, 70 if len(lines) == 3 else 60), (x + w / 2, 84)], 'c', arrow=False)
    s.line([(40, 84), (690, 84)], 'c', arrow=False)
    s.text(608, 36, 'to every effect,', 12, anchor='start', cls='m')
    s.text(608, 52, 'in both rows', 12, anchor='start', cls='m')
    # row 1
    y = 130
    s.line([(14, y), (50, y)], 'a')
    s.text(30, y - 8, 'mix', 12, cls='m')
    s.box(46, y - 22, 80, 44, ['FREEZER', ('1', 'm')], accent=True)
    s.line([(86, 84), (86, y - 23)], 'c')
    s.line([(126, y), (137, y)], 'a')
    s.sum(146, y)
    s.line([(155, y), (170, y)], 'a')
    names = ['SHIFTER', 'FOLDER', 'CRUSHER', 'FILTER', 'FLANGER', 'PHASER']
    for i, name in enumerate(names):
        x = 168 + i * 86
        s.box(x, y - 22, 78, 44, [name, (str(i + 2), 'm')], accent=True)
        s.line([(x + 39, 84), (x + 39, y - 23)], 'c')
        if i < len(names) - 1:
            s.line([(x + 78, y), (x + 86, y)], 'a')
    s.line([(598 + 78, y), (708, y), (708, 250), (30, 250), (30, 290), (50, 290)], 'a')
    s.dot(690, y, 'a')
    # the resonator's loop
    s.line([(690, y), (690, 200), (520, 200)])
    s.box(380, 178, 140, 44, ['RESONATOR  8', ('Band in, Level out', 'm')])
    s.line([(380, 200), (146, 200), (146, y + 9)])
    s.text(263, 216, 'its ringing, back in', 12, cls='m')
    s.text(606, 216, 'the loop from the phaser', 12, cls='m')
    # row 2
    y = 290
    s.box(50, y - 30, 74, 60, ['SLICER', ('9', 'm')], accent=True)
    s.line([(124, y), (138, y)], 'a')
    s.box(138, y - 30, 90, 60, ['WOW &', 'FLUTTER', ('10', 'm')], accent=True)
    s.line([(228, y), (242, y)], 'a')
    s.box(242, y - 30, 80, 60, ['TAPE', 'STOP', ('11', 'm')], accent=True)
    s.line([(322, y), (479, y)], 'a')
    s.line([(479 + 18, y), (643, y)], 'a')
    for x, sx, name in ((356, 488, 'DELAY  12'), (518, 652, 'REVERB  13')):
        tap = x - 10
        s.dot(tap, y, 'a')
        s.line([(tap, y), (tap, 370), (x, 370)])
        s.box(x, 330, 120, 80, [name, ('in: the key, Band', 'm'), ('out: Level', 'm'),
                                ('knob 4: ducking', 'm')])
        s.line([(x + 120, 370), (sx, 370), (sx, y + 9)])
        s.sum(sx, y)
    s.text(414, 283, 'sends', 12, cls='m')
    s.text(574, 283, 'echoes into the reverb', 11, cls='m')
    # row 3: the finalizer, the input past the effects, VOLUME, the outputs
    s.line([(661, y), (708, y), (708, 440), (24, 440), (24, 480), (40, 480)], 'a')
    y = 480
    s.box(40, y - 24, 120, 48, ['FINALIZER  15', ('neutral at first', 'm')], accent=True)
    s.line([(160, y), (176, y)], 'a')
    s.sum(185, y)
    s.line([(185, 535), (185, y + 9)])
    s.text(185, 552, 'the input past the effects', 12, cls='m')
    s.line([(194, y), (220, y)], 'a')
    s.box(220, y - 24, 100, 48, ['VOLUME', ('output gain', 'm')], accent=True)
    s.line([(320, y), (340, y), (340, 460), (520, 460)], 'a')
    s.dot(340, y, 'a')
    s.line([(340, y), (340, 525), (390, 525)])
    s.box(390, 503, 110, 44, ['PHONES FEED', ('VOLUME page 3', 'm')])
    s.line([(500, 525), (520, 525)])
    s.line([(445, 580), (445, 547)])
    s.text(445, 596, 'the input alone, at VOLUME', 12, cls='m')
    for yy, kind, name in ((460, 'a', 'LINE OUT'), (525, 's', 'PHONES')):
        s.box(520, yy - 20, 90, 40, ['LIMITER'], accent=kind == 'a')
        s.line([(610, yy), (632, yy)], kind)
        s.jack(639, yy)
        s.text(650, yy + 4.5, name, 12, anchor='start', bold=True)
    # every insert's page 2 (FxOutput.h): Band splits, the effect, Level, Mix, the rest added
    s.text(20, 640, 'Every insert (keys 1–7 and 9–11), with its page 2:', 13, anchor='start', bold=True)
    y = 700
    s.line([(20, y), (80, y)])
    s.text(40, y - 8, 'in', 12, cls='m')
    s.box(80, y - 22, 90, 44, ['BAND', ('splits off', 'm')])
    s.line([(125, y - 22), (125, 664), (540, 664), (540, y - 9)])
    s.text(330, 657, 'the rest of the sound passes by', 12, cls='m')
    s.line([(170, y), (200, y)])
    s.dot(185, y)
    s.box(200, y - 22, 90, 44, ['EFFECT', ('page 1', 'm')])
    s.line([(290, y), (310, y)])
    s.box(310, y - 22, 80, 44, ['LEVEL', ('knob 4', 'm')])
    s.line([(390, y), (420, y)])
    s.box(420, y - 22, 80, 44, ['MIX', ('knob 1', 'm')])
    s.line([(185, y), (185, 740), (460, 740), (460, y + 22)])
    s.text(320, 754, 'the band, dry', 12, cls='m')
    s.line([(500, y), (531, y)])
    s.sum(540, y)
    s.line([(549, y), (590, y)])
    s.text(598, y + 4.5, 'out', 12, anchor='start', cls='m')
    s.text(20, 782, 'The key fades Level and Mix with the effect: off, the sound passes as it is.',
           12, anchor='start', cls='m')
    s.legend(28, 812, [('a', 'the usual way')])
    s.legend(220, 812, [('s', 'other ways')])
    s.legend(400, 812, [('c', 'control')])
    return s


# ---------- 4. clock and MIDI ----------
def clock_midi():
    s = Svg(690, 'FRIZZ clock and MIDI',
            'Where the tempo comes from and where MIDI goes. MIDI comes in over TRS and USB. The '
            'clock source (A) picks whose clock counts, the clock factor (B) follows it at half or '
            'double. That clock quantizes recordings in the looper. Start, Stop, Continue and Song '
            'Position from the chosen input move the loop when transport following (C) is on. The '
            'tempo clock takes the first of: the loop, the MIDI clock, the tapped or last tempo; '
            'it drives the effects that follow a tempo, the scene morphs\' bar lines, the beat '
            'LEDs and MIDI out. MIDI out (D) sends the clock, generated or passed on, the loop\'s '
            'transport and, with feedback (E), the panel\'s changes as CCs, over TRS or TRS and USB.')
    # in
    s.jack(28, 140)
    s.text(14, 122, 'TRS MIDI IN', 13, anchor='start', bold=True)
    s.jack(28, 190)
    s.text(14, 172, 'USB MIDI', 13, anchor='start', bold=True)
    s.line([(33, 140), (110, 140)])
    s.line([(33, 190), (80, 190), (80, 170), (110, 170)])
    s.box(110, 120, 150, 70, ['MIDI IN', ('clock source', 'm'), ('clock factor', 'm')])
    s.badge(110, 120, 'A')
    s.badge(260, 120, 'B')
    # transport, to the looper
    s.line([(170, 120), (170, 68), (215, 68)], arrow=False)
    s.switch(215, 68, 'C')
    s.line([(245, 68), (300, 68)])
    s.text(272, 84, 'transport', 12, cls='m')
    s.box(300, 40, 120, 56, ['LOOPER', ('quantized: whole', 'm'), ('bars of the clock', 'm')])
    # the clock
    s.line([(260, 155), (470, 155)], 'a')
    s.dot(385, 155, 'a')
    s.line([(385, 155), (385, 96)], 'a')
    s.text(325, 148, '24 PPQN', 12, cls='m')
    s.box(300, 185, 120, 44, ['TAP TEMPO', ('SHIFT + LOOP', 'm')])
    s.line([(420, 68), (445, 68), (445, 120), (470, 120)], 'a')
    s.line([(420, 207), (470, 207)])
    s.add('<rect x="470" y="80" width="140" height="160" class="ba"/>')
    s.text(540, 99, 'TEMPO CLOCK', 13, bold=True)
    s.text(478, 124.5, '1  the loop', 11, anchor='start', sans=True)
    s.text(478, 159.5, '2  MIDI clock', 11, anchor='start', sans=True)
    s.text(478, 203, '3  tapped, or the', 11, anchor='start', sans=True)
    s.text(493, 218, 'last (120 at first)', 11, anchor='start', cls='m')
    s.text(540, 182, 'the first that applies', 11, cls='m')
    s.text(552, 260, '× factor, 50–300 BPM', 12, anchor='start', cls='m')
    # what follows it
    s.line([(540, 240), (540, 248)], 'a', arrow=False)
    s.line([(290, 275), (690, 275)], 'a', arrow=False)
    s.line([(540, 248), (540, 275)], 'a', arrow=False)
    s.dot(540, 275, 'a')
    for x, w, lines in ((300, 190, ['EFFECTS\' TEMPO', ('delay, filter LFO, phaser,', 'm'),
                                    ('freezer, slicer, tape stop,', 'm'), ('chaos grid', 'm')]),
                        (505, 95, ['SCENE', 'MORPHS', ('bar lines', 'm')]),
                        (615, 95, ['BEAT', ('PLAY,', 'm'), ('lower G#', 'm')])):
        s.line([(x + w / 2, 275), (x + w / 2, 295)], 'a')
        s.box(x, 295, w, 80, lines)
    # MIDI out
    s.line([(290, 275), (290, 425), (300, 425)], 'a')
    s.text(282, 402, 'generated: a loop,', 12, anchor='end', cls='m')
    s.text(282, 417, 'or no clock coming in', 12, anchor='end', cls='m')
    s.line([(130, 190), (130, 450), (300, 450)])
    s.text(122, 300, 'passed on:', 12, anchor='end', cls='m')
    s.text(122, 315, 'no loop, a clock', 12, anchor='end', cls='m')
    s.text(122, 330, 'coming in', 12, anchor='end', cls='m')
    s.text(20, 479.5, 'panel changes', 12, anchor='start', cls='m')
    s.line([(110, 475), (200, 475)], arrow=False)
    s.switch(200, 475, 'E')
    s.line([(230, 475), (300, 475)])
    s.text(262, 468, 'CCs', 12, cls='m')
    s.box(300, 405, 190, 90, ['MIDI OUT', ('clock 24 PPQN; the loop\'s', 'm'), ('Start, Stop, Song', 'm'),
                              ('Position, Continue', 'm')])
    for yy, name in ((430, 'TRS MIDI OUT'), (470, 'USB MIDI')):
        s.line([(490, yy), (530, yy)], arrow=False)
        s.switch(530, yy, 'D')
        s.line([(560, yy), (635, yy)])
        s.jack(642, yy)
        s.text(654, yy + 4.5, name.split()[0], 13, anchor='start', bold=True)
        s.text(654, yy + 20, ' '.join(name.split()[1:]), 12, anchor='start', cls='m')
    s.text(300, 518, 'Never back out of the port a passed-on clock comes in on.', 12, anchor='start',
           cls='m')
    s.legend(28, 556, [
        ('A', 'lower A#: the clock source, Auto, TRS, USB or internal'),
        ('B', 'lower G#: the clock factor, as sent, double or half'),
        ('C', 'lower F#: transport following'),
        ('D', 'upper C#: MIDI out, off, TRS, TRS and USB'),
        ('E', 'upper D#: MIDI feedback'),
    ])
    s.text(28, 674, 'All on the settings page.', 12, anchor='start', cls='m')
    return s


# ---------- 5. the panel ----------
EFFECTS = ['Freezer', 'Shifter', 'Folder', 'Crusher', 'Filter', 'Flanger', 'Phaser', 'Resonator',
           'Slicer', 'Wow & flutter', 'Tape stop', 'Delay', 'Reverb', 'Chaos', 'Finalizer']
# the dark keys on the play page, left to right (KEY16-25); '' does nothing there (the lower C#
# and D# only glow when the card holds loops, #434)
DARK = ['', '', 'blank', '1', '2', '3', '4', 'save', 'copy', 'delete']
# the dark keys as MANUAL.md names them, left to right
DARK_NAMES = ['lower C#', 'lower D#', 'lower F#', 'lower G#', 'lower A#', 'upper C#', 'upper D#',
              'upper F#', 'upper G#', 'upper A#']
DEPTH = 104  # the top board's play side, from the front edge (y 0) back


def board():
    """Every control on the top board: name -> (x, y) in mm, y from the front edge back, from
    firmware/twin/web/layout.json. White keys 'w1'-'w15', the dark keys by DARK_NAMES, 'SHIFT'
    (the CHOMPI key), 'PLAY', 'LOOP', knobs 'k1'-'k4', 'VOLUME', 'transport' and 'mode'"""
    layout = json.load(open(os.path.join(ROOT, 'firmware', 'twin', 'web', 'layout.json')))
    keys = {k['name']: k for k in layout['keys']}
    at = {}
    for n in range(1, 16):
        at['w%d' % n] = (keys['KEY%d' % n]['x'], keys['KEY%d' % n]['y'])
    for i, name in enumerate(DARK_NAMES):
        at[name] = (keys['KEY%d' % (16 + i)]['x'], keys['KEY%d' % (16 + i)]['y'])
    for n, name in ((26, 'SHIFT'), (27, 'PLAY'), (28, 'LOOP')):
        at[name] = (keys['KEY%d' % n]['x'], keys['KEY%d' % n]['y'])
    # knobs (SWn: EncoderId's, app.js); the transport wheel is on the lower board and comes up
    # through the panel's big hole, in the knobs' row (the enclosure's drawing: 212.8, 71.36
    # less the panel's offset)
    for e in layout['encoders']:
        name = {'SW4': 'k1', 'SW1': 'k2', 'SW2': 'k3', 'SW3': 'k4', 'SW5': 'transport',
                'SW6': 'VOLUME'}[e['name']]
        at[name] = (e['x'], 68.46 if name == 'transport' else e['y'])
    at['mode'] = (layout['toggle']['x'], layout['toggle']['y'])
    return at, layout


def panel():
    at, layout = board()
    sc = 1.8
    x0, top = 72, 46
    depth = DEPTH
    X = lambda x: x0 + x * sc
    Y = lambda y: top + (depth - y) * sc
    s = Svg(410, 'FRIZZ panel',
            'A schematic of the panel, not its artwork. Front row: 15 white keys, the effects '
            'freezer, shifter, folder, crusher, filter, flanger, phaser, resonator, slicer, '
            'wow & flutter, tape stop, delay, reverb, the chaos key and the finalizer. '
            'Above them 10 dark keys: the blank scene, scenes 1-4, save, copy and delete. Behind '
            'them the mode switch, the CHOMPI key (SHIFT), knobs 1-4, the transport wheel, PLAY, '
            'LOOP and VOLUME. Jacks: MIDI in and out on the left, AUX in and the headphones on '
            'the right, USB-C, the microSD card and the line out at the back.')
    s.add('<rect x="%s" y="%s" width="%s" height="%s" class="b"/>'
          % (num(X(0)), num(Y(depth)), num(320 * sc), num(depth * sc)))
    # white keys, their effects below the panel
    for n in range(1, 16):
        kx, ky = at['w%d' % n]
        w, h = 17.5 * sc, 17 * sc
        s.add('<rect x="%s" y="%s" width="%s" height="%s" class="wk"/>'
              % (num(X(kx) - w / 2), num(Y(ky) - h / 2), num(w), num(h)))
        s.text(X(kx), Y(ky) + 5, str(n), 12, cls='gt', bold=True)
        lx, ly = X(kx) - 3, Y(0) + 14
        s.text(lx, ly, EFFECTS[n - 1], 12, anchor='start', rotate=40)
    # dark keys: the scenes in the middle, their functions above them
    for i, name in enumerate(DARK_NAMES):
        kx, ky = at[name]
        w, h = 17.5 * sc, 15 * sc
        cls = 'sk' if 2 <= i <= 6 else 'dk'
        s.add('<rect x="%s" y="%s" width="%s" height="%s" class="%s"/>'
              % (num(X(kx) - w / 2), num(Y(ky) - h / 2), num(w), num(h), cls))
        if DARK[i]:
            s.text(X(kx), Y(ky) - h / 2 - 6, DARK[i], 11)
    # the keys behind: CHOMPI (SHIFT), PLAY, LOOP; PLAY's and LOOP's names above them, as
    # the dark keys' names take the line below
    for name, label, above in (('SHIFT', 'CHOMPI = SHIFT', False), ('PLAY', 'PLAY', True),
                               ('LOOP', 'LOOP', True)):
        kx, ky = at[name]
        w = 14 * sc
        s.add('<rect x="%s" y="%s" width="%s" height="%s" class="ba"/>'
              % (num(X(kx) - w / 2), num(Y(ky) - w / 2), num(w), num(w)))
        s.text(X(kx), Y(ky) - w / 2 - 6 if above else Y(ky) + w / 2 + 15, label, 12,
               bold=True)
    # knobs, their numbers in them, and the transport wheel
    for name in ('k2', 'k3', 'k4', 'k1', 'transport', 'VOLUME'):  # layout.json's order
        x, y = X(at[name][0]), Y(at[name][1])
        if name == 'transport':
            s.add('<circle cx="%s" cy="%s" r="%s" class="ba"/>' % (num(x), num(y), num(15.5 * sc)))
            # two lines: it doesn't fit across the wheel at 11 px
            s.text(x, y - 2, 'trans-', 12, bold=True)
            s.text(x, y + 11, 'port', 12, bold=True)
            continue
        s.add('<circle cx="%s" cy="%s" r="%s" class="b"/>' % (num(x), num(y), num(7 * sc)))
        s.text(x, y + 4.5, name[1] if name[0] == 'k' else '', 12, bold=True)
    s.text(X(119.7), Y(68.46) - 7 * sc - 6, 'knobs 1–4', 12, cls='m', sans=False)
    s.text(X(300.65), Y(68.46) - 7 * sc - 6, 'VOLUME', 12, bold=True)
    # the LEDs above the knobs and the transport
    for l in layout['pth']:
        s.add('<circle cx="%s" cy="%s" r="3" class="led"/>' % (num(X(l['x'])), num(Y(l['y']))))
    # the mode switch
    x, y = X(at['mode'][0]), Y(at['mode'][1])
    s.add('<rect x="%s" y="%s" width="10" height="24" class="b"/>' % (num(x - 5), num(y - 12)))
    s.add('<circle cx="%s" cy="%s" r="3.5" class="g"/>' % (num(x), num(y + 6)))
    s.text(x, y - 18, 'mode', 12, cls='m', sans=False)
    # jacks: the sides and the back of the lower board, in the top board's coordinates: the
    # lower board's y + 99.29, which puts SW5 under its hole (y 68.46) and the side jacks
    # where the side panels' holes are; the back's x from the enclosure's back panel
    for y, name in ((60.2, 'MIDI OUT'), (38.7, 'MIDI IN')):
        s.add('<rect x="%s" y="%s" width="6" height="14" class="j"/>' % (num(X(0) - 6), num(Y(y) - 7)))
        s.text(X(0) - 12, Y(y) + 4.5, name, 12, anchor='end', bold=True)
    for y, name in ((60.2, 'AUX IN'), (38.7, 'PHONES')):
        s.add('<rect x="%s" y="%s" width="6" height="14" class="j"/>' % (num(X(320)), num(Y(y) - 7)))
        s.text(X(320) + 12, Y(y) + 4.5, name, 12, anchor='start', bold=True)
    for x, name in ((22.4, 'USB-C'), (250.4, 'microSD'), (296.8, 'LINE OUT')):
        s.add('<rect x="%s" y="%s" width="14" height="6" class="j"/>' % (num(X(x) - 7), num(Y(depth) - 6)))
        s.text(X(x), Y(depth) - 14, name, 12, bold=name.isupper() or name == 'USB-C')
    s.text(X(0) - 12, Y(depth) + 4, 'back', 12, anchor='end', cls='m', sans=False)
    s.text(X(0) - 12, Y(0) + 2, 'front', 12, anchor='end', cls='m', sans=False)
    for i, line in enumerate((
            'The play page (mode switch down; up is the settings page). Dark keys: the blank',
            'scene, scenes 1–4 (grey), save, copy, delete; the lower C# and D# show the card\'s loops.',
            'A schematic to scale, from the board files: not the panel\'s artwork.')):
        s.text(20, 352 + i * 18, line, 12, anchor='start', cls='m')
    return s


# ---------- 6. the quick start's way of the sound, narrow for a phone ----------
def quickstart_flow():
    s = Svg(450, 'How the sound flows',
            'The input, AUX, the built-in mic or USB after its input gain, goes two ways: to the '
            'looper, which records it before the effects, so nothing is printed into the loop; '
            'and, if monitored (the mic starts off), on to the input/loop mix, SHIFT + VOLUME, '
            'where the loop joins it. From there input and loop alike go through the effects '
            '(white keys 1-14), the finalizer (white key 15) and VOLUME, the output gain, to '
            'the line out and the headphones.', w=480)
    s.box(50, 20, 200, 60, ['INPUT', ('AUX, the mic or USB', 'm'), ('input gain', 'm')],
          accent=True)
    # the looper takes the input before the monitoring, the mix and the effects
    s.line([(150, 80), (150, 212)], 'a')
    s.dot(150, 100, 'a')
    s.line([(150, 100), (375, 100), (375, 116)])
    s.text(262, 92, 'the input', 12, cls='m')
    s.box(290, 116, 170, 80, ['LOOPER', ('records here, before', 'm'), ('the effects: nothing is', 'm'),
                              ('printed into the loop', 'm')])
    s.text(142, 150, 'if monitored', 12, anchor='end', sans=True)
    s.text(142, 166, 'the mic: off at first', 11, anchor='end', cls='m')
    s.box(50, 212, 200, 60, ['MIX', ('input ↔ loop', 'm'), ('SHIFT + VOLUME', 'm')])
    s.line([(375, 196), (375, 242), (250, 242)])
    s.text(312, 235, 'loop', 12, cls='m')
    s.line([(150, 272), (150, 292)], 'a')
    s.box(50, 292, 200, 60, ['EFFECTS', ('white keys 1–14,', 'm'), ('on input and loop alike', 'm')],
          accent=True)
    s.line([(150, 352), (150, 372)], 'a')
    s.box(80, 372, 140, 48, ['FINALIZER', ('white key 15', 'm')], accent=True)
    s.line([(220, 396), (240, 396)], 'a')
    s.box(240, 372, 110, 48, ['VOLUME', ('output gain', 'm')], accent=True)
    s.line([(350, 396), (385, 396)], 'a')
    s.jack(392, 396)
    s.text(404, 400.5, 'OUT', 13, anchor='start', bold=True)
    s.text(404, 416, 'line out,', 12, anchor='start', cls='m')
    s.text(404, 431, 'phones', 12, anchor='start', cls='m')
    return s


# ---------- 7. the chapters' figures: what they read ----------
SRC = os.path.join(ROOT, 'firmware', 'code', 'src')
DIM = RAISED       # a white key the figure doesn't use, in a DIM_LINE outline
DIM_DARK = RAISED  # a dark key it doesn't use, the same
DIM_LINE = DIMMED  # the outline of a control it doesn't use, the panel's edge
RING = ' stroke="%s" stroke-width="1.5"' % BG  # an LED colour's soot ring


def source(name):
    return open(os.path.join(SRC, name), encoding='utf-8').read()


def led_colours():
    """The LED colours by name, as hex: LedColors.h's (`static const float name[3] = {r, g, b};`,
    the shape the site reads) and the ones NormalPage.h keeps for the Cubbi page, rounded as the
    site rounds them"""
    pat = re.compile(r'static\s+(?:const|constexpr const)\s+float\s+(\w+)\[3\]\s*=\s*\{([^}]*)\}')
    out = {}
    for text in (source('LedColors.h'), source('NormalPage.h')):
        for name, rgb in pat.findall(text):
            vals = [float(v.strip().rstrip('f')) for v in rgb.split(',')]
            out[name] = '#' + ''.join('%02x' % math.floor(v * 255 + .5) for v in vals)
    return out


def key_colours():
    """White key n -> (its colour, its knobs' colours at 0 and at 1), from FxSlots.h's kFxSlots
    (`{Hardware::SwId::KEY_<n>, <colour>, {<at 0>, <unused>, <at 1>}, …`) and kCompKey, white
    with kCompKnobColors, as the site reads them"""
    c = led_colours()
    code = re.sub(r'//.*', '', source('FxSlots.h'))
    out = {}
    for n, key, k0, k1 in re.findall(
            r'\{\s*Hardware::SwId::KEY_(\d+)\s*,\s*(\w+)\s*,\s*\{\s*(\w+)\s*,\s*\w+\s*,\s*(\w+)\s*\}', code):
        out[int(n)] = (c[key], c[k0], c[k1])
    comp = int(re.search(r'kCompKey\s*=\s*Hardware::SwId::KEY_(\d+)', code).group(1))
    k0, k1 = re.search(r'kCompKnobColors\[3\]\s*=\s*\{\s*(\w+)\s*,\s*\w+\s*,\s*(\w+)\s*\}', code).groups()
    out[comp] = (c['white'], c[k0], c[k1])
    return out


def bank_colours(name):
    """A page's bank colours, NormalPage.h's kBankColors (the Loops page) or kCubbiColors"""
    c = led_colours()
    names = re.search(name + r'\[\d+\]\s*=\s*\{([^}]*)\}', source('NormalPage.h')).group(1)
    return [c[n.strip()] for n in names.split(',')]


def knob_names(chapter):
    """A chapter's knobs on page 1, from MANUAL.md's knob table: each Page 1 cell up to the first
    ":", "," or " (", as the website's landing page reads them"""
    text = open(os.path.join(ROOT, 'MANUAL.md'), encoding='utf-8').read()
    body = text.split('\n## ' + chapter + '\n', 1)[1].split('\n## ', 1)[0]
    names = {}
    for n, cell in re.findall(r'^\| ([1-4]) \| ([^|]*)\|', body, re.M):
        names.setdefault(int(n), re.split(r':|,| \(', cell.strip(), maxsplit=1)[0])
    return [names[n] for n in (1, 2, 3, 4)]


def ink(colour):
    """The text colour that reads on a fill: soot on a light one, chalk on a dark one"""
    r, g, b = (int(colour[i:i + 2], 16) / 255 for i in (1, 3, 5))
    return BG if .2126 * r + .7152 * g + .0722 * b > .4 else FG


def slug(name):
    return re.sub(r'[^a-z]+', '-', name.lower()).strip('-')


# ---------- 8. a key locator for each effect's chapter ----------
# The chapters' own names in MANUAL.md, where they differ from EFFECTS'
CHAPTER = {'Chaos': 'Chaos key'}
LW = 480  # the chapters' figures: as wide as the quick start's, so they read on a phone


def locator(n):
    """White key n among the 15 and the dark keys above them, in its colour, and its knobs' LED
    colours"""
    name = EFFECTS[n - 1]
    chapter = CHAPTER.get(name, name)
    colour, k0, k1 = key_colours()[n]
    knobs = knob_names(chapter)
    at, _ = board()
    sc, x0 = 1.38, (LW - 320 * 1.38) / 2
    X = lambda x: x0 + x * sc
    s = Svg(84, '%s: white key %d' % (chapter, n),
            'The keys from above: the %s is white key %d of 15, counted from the left, lit in its '
            'colour. Its knobs 1 to 4 are %s; their LEDs go from one colour to the other.'
            % (chapter.lower(), n,
               ', '.join(knobs)), w=LW)
    for name_ in DARK_NAMES:
        kx = X(at[name_][0])
        s.add('<rect x="%s" y="10" width="%s" height="9" fill="%s" stroke="%s" stroke-width="1.5"/>'
              % (num(kx - 17.5 * sc / 2), num(17.5 * sc), DIM_DARK, DIM_LINE))
    for k in range(1, 16):
        kx = X(at['w%d' % k][0])
        on = k == n
        s.add('<rect x="%s" y="24" width="%s" height="%s" fill="%s"%s/>'
              % (num(kx - 17.5 * sc / 2), num(17.5 * sc), num(17 * sc), colour if on else DIM,
                 RING if on else ' stroke="%s" stroke-width="1.5"' % DIM_LINE))
        s.add('<text x="%s" y="%s" font-size="%d"%s style="fill:%s">%d</text>'
              % (num(kx), num(24 + 17 * sc / 2 + 4.5 if on else 24 + 17 * sc / 2 + 4), px(13 if on else 11),
                 ' font-weight="%s"' % BOLD if on else '', ink(colour) if on else MUTED, k))
    # the key's name under it, kept within the picture; the knobs' colours on the other side
    kx = X(at['w%d' % n][0])
    label = chapter
    lw = width(label, 13, True)
    lx = min(max(kx, 8 + lw / 2), LW - 8 - lw / 2)
    s.text(lx, 74, label, 13, bold=True)
    s.add('<path d="M%s,%sv6" stroke="%s" stroke-width="1.5"/>' % (num(kx), num(24 + 17 * sc + 3), FG))
    # the knobs' LEDs, from the colour at 0 to the one at 1
    bx = 24 if kx > LW / 2 else LW - 24 - 150
    s.text(bx, 74, 'knob LEDs', 12, anchor='start', cls='m')
    for x, c in ((bx + 84, k0), (bx + 136, k1)):
        s.add('<circle cx="%s" cy="70" r="6" fill="%s"%s/>' % (num(x), c, RING))
    s.line([(bx + 94, 70), (bx + 126, 70)], 's')
    return s


# ---------- 9. the panel in a chapter: what that chapter plays, lit ----------
class Mini:
    """The panel at the chapters' width, every control dim but the ones a chapter is about, each
    lit in its colour; chalk badges mark them for the legend under it, as the other diagrams'
    letters do"""
    SC = 1.38

    def __init__(self, s, top, lit, mode='down'):
        """lit: control (board()'s names) -> its colour, or (colour, 'dim') for an LED that glows"""
        self.s, self.top = s, top
        self.at, _ = board()
        self.x0 = (s.w - 320 * self.SC) / 2
        sc = self.SC
        s.add('<rect x="%s" y="%s" width="%s" height="%s" fill="none" stroke="%s" '
              'stroke-width="1.5"/>' % (num(self.X(0)), num(top), num(320 * sc), num(DEPTH * sc), DIM_LINE))
        for name, (x, y) in sorted(self.at.items(), key=lambda kv: (kv[1][1], kv[1][0])):
            c = lit.get(name)
            dim = isinstance(c, tuple)
            c = c[0] if dim else c
            fill = c if c else (DIM if name[0] == 'w' else DIM_DARK if name in DARK_NAMES else 'none')
            stroke = RING if c else ' stroke="%s" stroke-width="1.5"' % DIM_LINE
            op = ' fill-opacity=".4"' if dim or (c and name == 'transport') else ''
            cx, cy = self.X(x), self.Y(y)
            if name[0] == 'w' or name in DARK_NAMES or name in ('SHIFT', 'PLAY', 'LOOP'):
                w, h = ((17.5, 17) if name[0] == 'w' else (17.5, 15) if name in DARK_NAMES
                        else (14, 14))
                s.add('<rect x="%s" y="%s" width="%s" height="%s" fill="%s"%s%s/>'
                      % (num(cx - w * sc / 2), num(cy - h * sc / 2), num(w * sc), num(h * sc), fill, op, stroke))
            elif name == 'mode':
                s.add('<rect x="%s" y="%s" width="8" height="18" fill="none" stroke="%s" '
                      'stroke-width="1.5"/>' % (num(cx - 4), num(cy - 9), c or DIM_LINE))
                s.add('<circle cx="%s" cy="%s" r="2.6" fill="%s"/>'
                      % (num(cx), num(cy + (4.5 if mode == 'down' else -4.5)), c or DIM_LINE))
            else:
                r = 15.5 if name == 'transport' else 7
                s.add('<circle cx="%s" cy="%s" r="%s" fill="%s"%s%s/>'
                      % (num(cx), num(cy), num(r * sc), fill, op, stroke))
            if c and name == 'transport':
                # the wheel's LED ring, just inside its soot ring
                s.add('<circle cx="%s" cy="%s" r="%s" fill="none" stroke="%s" stroke-width="1.5"/>'
                      % (num(cx), num(cy), num(15.5 * sc - 1.5), c))

    def X(self, x):
        return self.x0 + x * self.SC

    def Y(self, y):
        return self.top + (DEPTH - y) * self.SC

    def pos(self, name):
        return self.X(self.at[name][0]), self.Y(self.at[name][1])

    def label(self, name, text, dy):
        """A control's name or number, centred on it or dy below its centre"""
        x, y = self.pos(name)
        self.s.text(x, y + dy, text, 11, cls='m')

    def badge(self, name, letter, dx=0, dy=0):
        x, y = self.pos(name)
        self.s.badge(x + dx, y + dy, letter)

    def bracket(self, first, last, letter, y):
        """A badge for a group of controls: a line from first to last at y, the badge on it"""
        x1, x2 = self.pos(first)[0], self.pos(last)[0]
        self.s.add('<path d="M%s,%sh%s" stroke="%s" stroke-width="2"/>'
                   % (num(x1), num(y), num(x2 - x1), ACC))
        self.s.badge((x1 + x2) / 2, y, letter)

    def bottom(self):
        return self.Y(0)


def chapter_svg(h, title, desc):
    return Svg(h, title, desc, w=LW)


def legend_rows(s, y, rows, x=20):
    """The legend under a panel: (badge, text) rows; a row whose badge is '' goes on with the one
    above it"""
    for i, (mark, text) in enumerate(rows):
        yy = y + i * 20
        if mark:
            s.badge(x + 9, yy - 4, mark)
        s.text(x + 26, yy, text, 12, anchor='start', sans=True)
        if width(text, 12, sans=True) + x + 26 > s.w - 6:
            print('  legend too wide: %r' % text)
    return y + len(rows) * 20


def fit(s, y):
    """The picture's height from where its legend left off"""
    s.h = int(y - 20 + 14)
    return s


def fig_punch_in():
    c = key_colours()
    rows = [('A', 'Hold a white key: its effect is on while you hold it.'),
            ('B', 'Hold the key, then SHIFT: latched on, or off again.'),
            ('', 'SHIFT first, then the key: selected for the knobs, unheard.'),
            ('C', 'Turn: the effect\'s sound. SHIFT + turn: in big steps.'),
            ('', 'Press: its page 2 and back. SHIFT + press: the default.')]
    s = chapter_svg(170 + 20 * len(rows) + 28, 'Playing an effect',
                    'The panel with what plays an effect lit: the white keys 1 to 14 in their '
                    'effects\' colours, the CHOMPI key (SHIFT) and knobs 1 to 4. ' +
                    ' '.join(t for _, t in rows))
    lit = {'w%d' % n: c[n][0] for n in range(1, 15)}
    lit.update({'SHIFT': FG, 'k1': FG, 'k2': FG, 'k3': FG, 'k4': FG})
    m = Mini(s, 30, lit)
    m.bracket('w1', 'w14', 'A', m.bottom() + 12)
    m.badge('SHIFT', 'B', 0, -22)
    m.bracket('k1', 'k4', 'C', m.top - 14)
    return fit(s, legend_rows(s, m.bottom() + 46, rows))


def fig_looper():
    c = led_colours()
    rows = [('A', 'LOOP: record; again, stop and play; again, erase.'),
            ('B', 'PLAY: play or pause, when you let go.'),
            ('', 'Hold PLAY, press LOOP: record in time, or erase at the end.'),
            ('C', 'The transport knob: turn for speed, press for 1×.'),
            ('D', 'SHIFT + LOOP: tap the tempo.')]
    s = chapter_svg(170 + 20 * len(rows) + 10, 'The looper',
                    'The panel with the looper\'s controls lit: LOOP, PLAY, the transport knob '
                    'and the CHOMPI key (SHIFT). ' + ' '.join(t for _, t in rows))
    m = Mini(s, 30, {'LOOP': c['red'], 'PLAY': FG, 'transport': c['purple'], 'SHIFT': FG})
    m.badge('LOOP', 'A', 0, -24)
    m.badge('PLAY', 'B', 0, -24)
    m.badge('transport', 'C', 0, -32)
    m.badge('SHIFT', 'D', 0, -22)
    return fit(s, legend_rows(s, m.bottom() + 26, rows))


def fig_scenes():
    c = led_colours()
    rows = [('A', 'A scene key: recall it. The first is the blank scene, then 1–4.'),
            ('1', 'Save, copy or delete (green, blue, red): tap one,'),
            ('2', 'then the slot (for copy, the source, then the destination),'),
            ('3', 'then CHOMPI to confirm. White: done.')]
    s = chapter_svg(170 + 20 * len(rows) + 10, 'FX scenes',
                    'The panel with the scene keys lit: the five dark keys in the middle, the '
                    'blank scene and scenes 1 to 4; the last three dark keys, save in green, copy '
                    'in blue, delete in red; and the CHOMPI key. ' + ' '.join(t for _, t in rows))
    lit = {n: FG for n in DARK_NAMES[2:7]}
    lit.update({'upper F#': c['green'], 'upper G#': c['blue'], 'upper A#': c['red'], 'SHIFT': FG})
    m = Mini(s, 30, lit)
    for i, n in enumerate(DARK_NAMES[3:7]):
        x, y = m.pos(n)
        s.add('<text x="%s" y="%s" font-size="%d" font-weight="%s" style="fill:%s">%d</text>'
              % (num(x), num(y + 4), px(11), BOLD, BG, i + 1))
    m.bracket('lower F#', 'upper D#', 'A', m.Y(36.56 + 7.5) - 9)
    m.bracket('upper F#', 'upper A#', '1', m.Y(36.56 + 7.5) - 9)
    m.badge('SHIFT', '3', 0, -22)
    return fit(s, legend_rows(s, m.bottom() + 26, rows))


def fig_scene_changes():
    c = led_colours()
    rows = [('1', 'Hold SHIFT,'),
            ('2', 'tap a scene key, once per bar (1 to 8),'),
            ('3', 'press the transport knob for a drop, turn it for the'),
            ('', 'crossfader, or leave it for a morph,'),
            ('4', 'let go of SHIFT: the bars count from there.')]
    s = chapter_svg(170 + 20 * len(rows) + 10, 'Scene changes in time',
                    'The panel with one SHIFT hold\'s controls lit: the CHOMPI key (SHIFT), the '
                    'scene keys 1 to 4 and the transport knob. ' + ' '.join(t for _, t in rows))
    lit = {n: FG for n in DARK_NAMES[3:7]}
    lit.update({'SHIFT': FG, 'transport': c['purple']})
    m = Mini(s, 30, lit)
    m.badge('SHIFT', '1', -12, -22)
    m.badge('SHIFT', '4', 12, -22)
    m.bracket('lower G#', 'upper D#', '2', m.Y(36.56 + 7.5) - 9)
    m.badge('transport', '3', 0, -32)
    return fit(s, legend_rows(s, m.bottom() + 26, rows))


def fig_loops_page():
    banks = bank_colours('kBankColors')
    rows = [('A', 'CHOMPI glows in the bank\'s colour; hold it as SHIFT for the'),
            ('', 'gestures: SHIFT + lower C# opens and closes the page.'),
            ('B', 'Lower C#, lower D#: the bank before, after, in its colour.'),
            ('C', 'A white key: its loop, at once. SHIFT + the key: at the end'),
            ('', 'of the pass, tapped N times at the end of the N-th.')]
    s = chapter_svg(170 + 20 * len(rows) + 10, 'The Loops page',
                    'The panel on the Loops page, in bank B: the CHOMPI key glows yellow, bank '
                    'B\'s colour; the lower C# is orange, leading to bank A, the lower D# green, '
                    'leading to bank C; white keys 1 to 9 hold loops, dim yellow, key 3 bright, '
                    'the loop playing. ' + ' '.join(t for _, t in rows))
    lit = {'w%d' % n: (banks[1], 'dim') for n in range(1, 10)}
    lit.update({'w3': banks[1], 'SHIFT': (banks[1], 'dim'), 'lower C#': banks[0],
                'lower D#': banks[2]})
    m = Mini(s, 30, lit)
    m.badge('SHIFT', 'A', 0, -22)
    m.bracket('lower C#', 'lower D#', 'B', m.Y(36.56 + 7.5) - 9)
    m.bracket('w1', 'w15', 'C', m.bottom() + 12)
    return fit(s, legend_rows(s, m.bottom() + 46, rows))


def fig_settings():
    c = led_colours()
    left = [('A', 'Lower C#: every channel'), ('B', 'Lower D#: effects over TRS'),
            ('C', 'Lower F#: transport'), ('D', 'Lower G#: clock factor, beat'),
            ('E', 'Lower A#: clock source')]
    right = [('F', 'Upper C#: MIDI out'), ('G', 'Upper D#: MIDI feedback'),
             ('H', 'Upper F#: AUX, the mic or USB'), ('I', 'Upper G#: monitoring'),
             ('J', 'Upper A#: through the effects')]
    rows = [('K', 'White keys: the MIDI channel, 1 to 15; key 15 again: 16.'),
            ('L', 'Knob 1: the LED brightness.'),
            ('M', 'The mode switch: up for this page, down to play.')]
    s = chapter_svg(170 + 20 * 7 + 30, 'The settings page',
                    'The panel on the settings page, the mode switch up: the white keys pick the '
                    'MIDI channel, dim light blue, key 15 white for channel 16; the ten dark keys set the rest, each lit in its '
                    'own colour; knob 1 sets the LED brightness. ' +
                    ' '.join('%s.' % t[0].upper() + t[1:] for _, t in left + right) + ' ' +
                    ' '.join(t for _, t in rows))
    lit = {'w%d' % n: (c['med_blue'], 'dim') for n in range(1, 16)}
    lit.update({'w15': FG, 'lower C#': (c['med_blue'], 'dim'),
                'lower D#': c['med_blue'], 'lower F#': c['green'], 'lower G#': c['yellow'],
                'lower A#': FG, 'upper C#': (FG, 'dim'), 'upper D#': (c['med_blue'], 'dim'),
                'upper F#': c['green'], 'upper G#': FG, 'upper A#': c['green'], 'k1': FG,
                'mode': ACC})
    m = Mini(s, 30, lit, mode='up')
    for i, n in enumerate(DARK_NAMES):
        m.badge(n, 'ABCDEFGHIJ'[i], 0, -21)
    m.badge('w8', 'K', 0, 30)
    m.badge('k1', 'L', 0, -22)
    m.badge('mode', 'M', 0, -22)
    y = legend_rows(s, m.bottom() + 46, left)
    legend_rows(s, m.bottom() + 46, right, x=250)
    return fit(s, legend_rows(s, y + 6, rows))


# ---------- 10. key combinations: keycaps in a row, each shaped as on the panel ----------
# a cap's kind: 'w' a white key, 'd' a dark key, 'k' CHOMPI, PLAY or LOOP, 'o' a knob; '+' with,
# '>' then, and a string in brackets a note
def caps(s, x, y, parts):
    """One gesture from x, its caps centred on y; returns where it ends"""
    for p in parts:
        if p in ('+', '>'):
            if p == '+':
                s.text(x + 9, y + 5, '+', 15, bold=True)
            else:
                s.line([(x + 3, y), (x + 21, y)], 's')
            x += 24
        elif p.startswith('('):
            s.text(x + 4, y + 4.5, p[1:-1], 12, anchor='start', cls='m')
            x += width(p[1:-1], 12, sans=True) + 12
        else:
            kind, name = p[0], p[2:]
            w = width(name, 12, True) + 18
            attrs = {'w': 'class="wk"', 'd': 'class="dk"', 'k': 'class="ba"',
                     'o': 'rx="13" class="b"'}[kind]
            s.add('<rect x="%s" y="%s" width="%s" height="26" %s/>' % (num(x), num(y - 13), num(w), attrs))
            s.text(x + w / 2, y + 4.5, name, 12, cls='gt' if kind == 'w' else 't', bold=True)
            x += w + 4
    if x > s.w - 8:
        print('  past the edge: %r' % parts)
    return x


def card(title, desc, rows):
    """rows: (the caps, what it does): the gesture, its meaning under it"""
    s = Svg(16 + len(rows) * 58, title, desc, w=LW)
    for i, (parts, does) in enumerate(rows):
        y = 28 + i * 58
        caps(s, 20, y, parts)
        s.text(20, y + 30, does, 12, anchor='start', cls='m')
    return s


def describe(rows):
    """The cards' description: each gesture in words"""
    out = []
    for parts, does in rows:
        words = ' '.join({'+': '+', '>': 'then'}.get(p, p[1:-1] if p.startswith('(') else p[2:])
                         for p in parts)
        out.append('%s: %s' % (words, does[0].lower() + does[1:].rstrip('.')))
    return '. '.join(out) + '.'


KEYS_PLAY = [
    (['(hold)', 'w:FX key', '>', 'k:SHIFT'], 'Latch the effect on, or off again.'),
    (['k:SHIFT', '>', 'w:FX key'], 'Select it for the knobs, without hearing it.'),
    (['k:SHIFT', '+', 'k:LOOP', '(3 taps or more)'], 'Tap the tempo.'),
    (['(hold)', 'k:PLAY', '+', 'k:LOOP'], 'Record in time with the clock; with a loop, erase at its end.'),
    (['k:SHIFT', '+', 'o:VOLUME', '(turn)'], 'The input/loop mix.'),
    (['k:SHIFT', '+', 'd:scene key', '(let go)'], 'Morph to the scene on the bar line.'),
    (['k:SHIFT', '+', 'o:VOLUME', '(hold 2 s)'], 'On the settings page: write a bug report.'),
    (['k:SHIFT', '+', 'k:LOOP', '(hold 2 s)'], 'On the settings page: lend the card to the computer.'),
]
KEYS_START = [
    (['(hold)', 'w:FX key', '>', 'k:SHIFT'], 'Latch the effect on, or off again.'),
    (['k:SHIFT', '>', 'w:FX key'], 'Pick it for the knobs, without hearing it.'),
    (['k:SHIFT', '+', 'o:knob', '(turn)'], 'Jump in big steps: notes, octaves, intervals.'),
    (['o:knob', '(press)'], 'Its page 2: Mix, stereo, band and level; again, back.'),
]
KEYS_LOOPS = [
    (['k:SHIFT', '+', 'd:lower C#'], 'Open the Loops page; again, back to the play page.'),
    (['k:SHIFT', '+', 'd:lower D#'], 'Open the Cubbi page; again, back to the play page.'),
    (['k:SHIFT', '(tap)', 'k:SHIFT', '(tap)'], 'Back to the play page, from either.'),
    (['k:SHIFT', '+', 'w:white key', '(N taps)'], 'Its loop at the end of the N-th pass.'),
]


def keys_play():
    return card('Key combinations', 'The gestures to know, as keys. ' + describe(KEYS_PLAY), KEYS_PLAY)


def keys_start():
    return card('First gestures', 'The quick start\'s gestures, as keys. ' + describe(KEYS_START),
                KEYS_START)


def keys_loops():
    return card('The loop pages\' keys', 'The Loops and Cubbi pages\' keys. ' + describe(KEYS_LOOPS),
                KEYS_LOOPS)


def main():
    """Writes every picture; with --check, writes nothing and names each one that isn't what this
    draws (doclinks.py runs it so: a knob renamed in MANUAL.md or a colour changed in the firmware
    leaves its figure behind)"""
    check = sys.argv[1:] == ['--check']
    figures = [('signal-flow', signal_flow), ('input', input_diagram), ('fx-chain', fx_chain),
               ('clock-midi', clock_midi), ('panel', panel), ('quickstart-flow', quickstart_flow)]
    figures += [('key-' + slug(EFFECTS[n - 1]), lambda n=n: locator(n)) for n in range(1, 16)]
    figures += [('panel-punch-in', fig_punch_in), ('panel-looper', fig_looper),
                ('panel-scenes', fig_scenes), ('panel-scene-changes', fig_scene_changes),
                ('panel-loops', fig_loops_page), ('panel-settings', fig_settings),
                ('keys-play', keys_play), ('keys-quickstart', keys_start), ('keys-loops', keys_loops)]
    os.makedirs(OUT, exist_ok=True)
    stale = 0
    for name, make in figures:
        path = os.path.join(OUT, name + '.svg')
        if check:
            with contextlib.redirect_stdout(io.StringIO()):
                svg = make().render()
            try:
                same = open(path, encoding='utf-8').read() == svg
            except OSError:
                same = False
            if not same:
                print('images/%s.svg: not what diagrams.py draws: run it' % name)
                stale += 1
            continue
        print(name)
        svg = make().render()
        with open(path, 'w', encoding='utf-8') as f:
            f.write(svg)
    return 1 if stale else 0

if __name__ == '__main__':
    sys.exit(main())
