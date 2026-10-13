#!/usr/bin/env python3
"""Draws the docs' diagrams into images/: signal-flow, input, fx-chain, clock-midi, panel and
the quick start's quickstart-flow.

Plain block diagrams, by hand in coordinates of a 720 px wide picture (the quick start's 480,
so it reads on a phone): boxes, arrows, lettered
switches and a legend. What they show is the code's (firmware/code/src: passthroughEngine.h,
FxChain.h, FxOutput.h, MasterComp.h, TempoClock.h, MidiClock.h, MidiOut.h, SettingsPage.h) and
MANUAL.md's; the panel's positions come from firmware/twin/web/layout.json (CHOMPI's board
file) and the enclosure's drawings in reference/hardware.

Run it from anywhere, standard library only; it writes the same bytes every time:

    firmware/tools/diagrams/diagrams.py

The colours are frizz-web's (design/system/tokens.json): the website is dark, so each picture
has its own dark ground and reads the same on the site and on GitHub.
"""
import json
import os
from xml.sax.saxutils import escape

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
OUT = os.path.join(ROOT, 'images')

BG = '#131314'      # surface
FG = '#e2e2e2'      # secondary-fixed: lines and text
ACC = '#c4708a'     # rose-fuzz: the usual way through, the switches
MUTED = '#999999'   # steel-muted: notes, control lines
KEY = '#39393a'     # surface-bright: a dark key's fill
FONT = "'Hanken Grotesk', Helvetica, Arial, sans-serif"
W = 720


def num(v):
    """A coordinate without trailing zeros, so the files stay small and stable"""
    return ('%.1f' % v).rstrip('0').rstrip('.')


def width(s, size, bold=False):
    """About how wide Arial sets s: for checking that a label fits its box"""
    narrow = 'iljtfrI.,:;|!()[]\' '
    w = 0.0
    for c in s:
        if c in narrow:
            w += .3
        elif c.isupper() or c in 'mw#&×↔–':
            w += .7
        else:
            w += .55
    return w * size * (1.07 if bold else 1)


class Svg:
    def __init__(self, h, title, desc, w=W):
        self.w = w
        self.h = h
        self.title = title
        self.desc = desc
        self.items = []

    def add(self, s):
        self.items.append(s)

    def text(self, x, y, s, size=13, anchor='middle', cls='t', bold=False, rotate=None):
        # the anchor as a class: the style sheet's text-anchor would win over an attribute
        if anchor != 'middle':
            cls += ' ' + {'start': 'as', 'end': 'ae'}[anchor]
        attrs = 'x="%s" y="%s" class="%s"' % (num(x), num(y), cls)
        if size != 13:
            attrs += ' font-size="%d"' % size
        if bold:
            attrs += ' font-weight="bold"'
        if rotate is not None:
            attrs += ' transform="rotate(%s %s %s)"' % (num(rotate), num(x), num(y))
        else:
            w = width(s, size, bold)
            left = x - {'start': 0, 'middle': .5, 'end': 1}[anchor] * w
            if left < 0 or left + w > self.w:
                print('  past the edge: %r' % s)
        self.add('<text %s>%s</text>' % (attrs, escape(s)))

    def box(self, x, y, w, h, lines, accent=False, check=True):
        """A box with lines of text, centred: (text, 'h' a name | 'm' a note); a plain string
        is a name"""
        self.add('<rect x="%s" y="%s" width="%s" height="%s" class="%s"/>'
                 % (num(x), num(y), num(w), num(h), 'ba' if accent else 'b'))
        lines = [(l, 'h') if isinstance(l, str) else l for l in lines]
        step = 16
        top = y + h / 2 - step * (len(lines) - 1) / 2 + 4.5
        for i, (s, kind) in enumerate(lines):
            size = 13 if kind == 'h' else 12
            if check and width(s, size, kind == 'h') > w - 6:
                print('  tight: %r in %d px' % (s, w))
            self.text(x + w / 2, top + i * step, s, size, cls='t' if kind == 'h' else 'm',
                      bold=kind == 'h')

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
        self.add('<circle cx="%s" cy="%s" r="9" class="g"/>' % (num(x), num(y)))
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
            self.text(x + 32, yy, s, 12, anchor='start', cls='t')
            if width(s, 12) + x + 32 > self.w - 8:
                print('  legend too wide: %r' % s)

    def render(self):
        defs = ''.join(
            '<marker id="m%s" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="9" '
            'markerHeight="9" markerUnits="userSpaceOnUse" orient="auto">'
            '<path d="M0,0L10,5L0,10z" fill="%s"/></marker>' % (k, c)
            for k, c in (('a', ACC), ('s', FG), ('c', MUTED)))
        style = (
            # 13 px on the picture, inherited: a text's own font-size wins over it, where a
            # text{} rule's would win over the attribute
            'svg{font-size:13px}text{font-family:%s;fill:%s;text-anchor:middle}'
            '.as{text-anchor:start}.ae{text-anchor:end}.m{fill:%s}.gt{fill:%s}'
            '.b{fill:none;stroke:%s;stroke-width:1.5}'
            '.ba{fill:none;stroke:%s;stroke-width:2}'
            '.la{fill:none;stroke:%s;stroke-width:2.5}'
            '.ls{fill:none;stroke:%s;stroke-width:1.5}'
            '.lc{fill:none;stroke:%s;stroke-width:1.5;stroke-dasharray:5 4}'
            '.da{fill:%s}.ds{fill:%s}'
            '.sw{stroke:%s;stroke-width:2.5;stroke-linecap:round}'
            '.g{fill:%s}'
            '.wk{fill:%s;stroke:%s;stroke-width:1}'
            '.dk{fill:%s;stroke:%s;stroke-width:1.2}'
            '.sk{fill:%s;stroke:%s;stroke-width:1.2}'
            '.led{fill:none;stroke:%s;stroke-width:1}'
            % (FONT, FG, MUTED, BG, FG, ACC, ACC, FG, MUTED, ACC, FG, ACC, ACC,
               FG, BG, KEY, FG, ACC, FG, MUTED))
        out = [
            '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d" '
            'role="img" aria-labelledby="title desc">' % (self.w, self.h, self.w, self.h),
            '<title id="title">%s</title>' % escape(self.title),
            '<desc id="desc">%s</desc>' % escape(self.desc),
            '<style>%s</style>' % style,
            '<defs>%s</defs>' % defs,
            '<rect width="%d" height="%d" fill="%s"/>' % (self.w, self.h, BG),
        ] + self.items + ['</svg>']
        return '\n'.join(out) + '\n'


# ---------- 1. the whole way at a glance ----------
def signal_flow():
    s = Svg(440, 'FRIZZ signal flow',
            'The whole way of the sound. The input, AUX or the built-in mic after its input gain, '
            'goes to the looper and to the input/loop mix; the looper\'s playback is the input/loop '
            'mix\'s other side. The input/loop mix goes through the effects of the white keys and '
            'the master compressor. A source set past the effects: the input/loop mix\'s input '
            'share skips the effects and the compressor and joins after them. Then VOLUME, the output gain, and a safety limiter on each output: the line '
            'out, and the headphones, whose feed can blend in the input alone.')
    # row 1: input, looper, mix, effects, compressor, the branch past them joining
    s.box(20, 42, 110, 56, ['INPUT', ('AUX or mic', 'm'), ('input gain', 'm')], accent=True)
    s.line([(130, 70), (295, 70)], 'a')
    s.dot(150, 70, 'a')
    s.text(225, 62, 'input', 12, cls='m')
    s.line([(150, 70), (150, 150), (175, 150)])
    s.box(175, 126, 100, 48, ['LOOPER', ('records the input', 'm')])
    s.line([(275, 150), (347, 150), (347, 98)])
    s.text(310, 143, 'loop', 12, cls='m')
    s.box(295, 42, 105, 56, ['MIX', ('input ↔ loop', 'm'), ('SHIFT + VOLUME', 'm')])
    s.line([(400, 70), (420, 70)], 'a')
    s.box(420, 42, 120, 56, ['EFFECTS', ('white keys 1–14', 'm')], accent=True)
    s.line([(540, 70), (560, 70)], 'a')
    s.box(560, 42, 110, 56, ['COMPRESSOR', ('white key 15', 'm')], accent=True)
    s.line([(670, 70), (679, 70)], 'a')
    s.sum(688, 70)
    s.line([(385, 98), (385, 205), (688, 205), (688, 79)])
    s.text(537, 223, 'the input\'s share, past the effects and the compressor', 12, cls='m')
    # on to row 2
    s.line([(697, 70), (708, 70), (708, 240), (75, 240), (75, 268)], 'a')
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
    s.legend(560, 290, [('a', 'the usual way'), ('s', 'other ways')])
    return s


# ---------- 2. the input ----------
def input_diagram():
    s = Svg(650, 'FRIZZ input',
            'The input. AUX (stereo, or mono: its left to both sides) and the built-in mic each '
            'have their own input gain; AUX is then amplified 3× and its DC taken out, the mic '
            '5×, DC blocked, highpassed at about 150 Hz and the same on both sides. Switch A '
            '(upper F#, or the AUX jack) picks the source, with a 5 ms crossfade. The source goes '
            'as it is to the looper and to the headphone feed. Switch B (upper G#), its '
            'monitoring, has it on the outputs or not, switch C (upper A#) sends it through the '
            'effects or past them. The input/loop mix sets the input\'s share against the '
            'loop\'s; the share past the effects joins after the compressor, before VOLUME.')
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
    # A: the source
    s.selector(600, 140, [(565, 125), (565, 155)], 0, 'A', (600, 104))
    s.text(612, 180, '5 ms crossfade', 12, cls='m')
    s.text(560, 34, 'the AUX jack: a cable in picks AUX, out the mic', 12, cls='m')
    s.line([(583, 42), (583, 128)], 'c')
    s.line([(602, 140), (705, 140), (705, 262), (40, 262), (40, 330)], 'a', arrow=False)
    s.line([(40, 330), (40, 490)], arrow=False)
    s.dot(40, 330, 'a')
    # B: the monitoring, whether you hear it; C: then how, through the effects or past them
    s.line([(40, 330), (75, 330)], 'a', arrow=False)
    s.switch(75, 330, 'B')
    s.line([(105, 330), (157, 330)], 'a', arrow=False)
    s.selector(160, 330, [(195, 305), (195, 355)], 0, 'C', (170, 298))
    s.line([(197, 305), (330, 305)], 'a')
    s.text(263, 298, 'through the effects', 12, cls='m')
    s.line([(197, 355), (330, 355)])
    s.text(263, 348, 'past the effects', 12, cls='m')
    # the looper and the phones take the source as it is
    s.line([(40, 420), (200, 420)])
    s.dot(40, 420)
    s.text(120, 412, 'meter: VOLUME page 2', 12, cls='m')
    s.box(200, 398, 100, 44, ['LOOPER', ('records it', 'm')])
    s.line([(300, 420), (330, 420)])
    s.text(315, 413, 'loop', 12, cls='m')
    s.line([(40, 490), (470, 490)])
    s.text(250, 483, 'the source alone, whatever B and C say', 12, cls='m')
    s.text(478, 494.5, 'to the phones feed (VOLUME p. 3)', 13, anchor='start')
    s.box(330, 285, 140, 157, ['MIX', ('SHIFT + VOLUME', 'm'), ('', 'm'), ('the input\'s share', 'm'),
                               ('against the loop\'s,', 'm'), ('on both inputs', 'm')], check=False)
    s.line([(470, 315), (505, 315)], 'a')
    s.text(512, 319.5, 'to the effects', 13, anchor='start', bold=True)
    s.text(512, 336, 'and the compressor', 12, anchor='start', cls='m')
    s.line([(470, 385), (505, 385)])
    s.text(512, 389.5, 'past the effects:', 13, anchor='start')
    s.text(512, 406, 'joins after the compressor', 12, anchor='start', cls='m')
    s.legend(28, 540, [
        ('A', 'upper F#: the source, AUX or the mic. The AUX jack picks it; F# the other one'),
        ('B', 'upper G#: monitoring, on the outputs or not'),
        ('C', 'upper A#: through the effects, or past them and the compressor'),
        ('M', 'SHIFT + upper F#: AUX mono, its left channel to both sides'),
    ])
    s.text(28, 634, 'On the settings page (mode switch up). Each source keeps its own gain, B and C.',
           12, anchor='start', cls='m')
    return s


# ---------- 3. the effects, the compressor and the outputs ----------
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
            'the loop. After the effects the master compressor (key 15); then the input past the effects joins, '
            'then VOLUME and a safety limiter on the line out and on the headphones, whose feed blends '
            'towards the input alone. Below: every insert\'s page 2, Band, Level and Mix.')
    # what plays the effects
    for x, w, lines in ((20, 170, ['PANEL · MIDI', ('keys and knobs', 'm')]),
                        (210, 190, ['SCENES · MORPHS', ('the dark keys', 'm')]),
                        (420, 180, ['CHAOS  14', ('drops latched effects,', 'm'), ('scrambles the loop', 'm')])):
        s.box(x, 14, w, 56 if len(lines) == 3 else 46, lines)
        s.line([(x + w / 2, 70 if len(lines) == 3 else 60), (x + w / 2, 84)], 'c', arrow=False)
    s.line([(40, 84), (690, 84)], 'c', arrow=False)
    s.text(612, 36, 'to every effect,', 12, anchor='start', cls='m')
    s.text(612, 52, 'in both rows', 12, anchor='start', cls='m')
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
    s.text(600, 216, 'the loop from the phaser', 12, cls='m')
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
    s.text(568, 283, 'echoes into the reverb', 12, cls='m')
    # row 3: the compressor, the input past the effects, VOLUME, the outputs
    s.line([(661, y), (708, y), (708, 440), (24, 440), (24, 480), (40, 480)], 'a')
    y = 480
    s.box(40, y - 24, 120, 48, ['COMPRESSOR', ('15, always on', 'm')], accent=True)
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
    s = Svg(640, 'FRIZZ clock and MIDI',
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
    s.text(20, 122, 'TRS MIDI IN', 13, anchor='start', bold=True)
    s.jack(28, 190)
    s.text(20, 172, 'USB MIDI', 13, anchor='start', bold=True)
    s.line([(33, 140), (110, 140)])
    s.line([(33, 190), (80, 190), (80, 170), (110, 170)])
    s.box(110, 120, 150, 70, ['MIDI IN', ('clock source', 'm'), ('clock factor', 'm')])
    s.badge(110, 120, 'A')
    s.badge(260, 120, 'B')
    # transport, to the looper
    s.line([(170, 120), (170, 68), (215, 68)], arrow=False)
    s.switch(215, 68, 'C')
    s.line([(245, 68), (300, 68)])
    s.text(272, 60, 'transport', 12, cls='m')
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
    s.text(478, 124.5, '1  the loop', 12, anchor='start')
    s.text(478, 159.5, '2  MIDI clock', 12, anchor='start')
    s.text(478, 203, '3  tapped, or the', 12, anchor='start')
    s.text(493, 218, 'last (120 at first)', 12, anchor='start', cls='m')
    s.text(540, 182, 'the first that applies', 12, cls='m')
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
    ])
    s.legend(400, 556, [
        ('D', 'upper C#: MIDI out, off, TRS, TRS and USB'),
        ('E', 'upper D#: MIDI feedback'),
    ])
    s.text(432, 600, 'All on the settings page.', 12, anchor='start', cls='m')
    return s


# ---------- 5. the panel ----------
EFFECTS = ['Freezer', 'Shifter', 'Folder', 'Crusher', 'Filter', 'Flanger', 'Phaser', 'Resonator',
           'Slicer', 'Wow & flutter', 'Tape stop', 'Delay', 'Reverb', 'Chaos', 'Compressor']
# the dark keys on the play page, left to right (KEY16-25); '' does nothing there
DARK = ['', '', 'blank', '1', '2', '3', '4', 'save', 'copy', 'delete']


def panel():
    layout = json.load(open(os.path.join(ROOT, 'firmware', 'twin', 'web', 'layout.json')))
    sc = 1.8
    x0, top = 72, 46
    depth = 104  # the top board's play side, from the front edge (y 0) back
    X = lambda x: x0 + x * sc
    Y = lambda y: top + (depth - y) * sc
    s = Svg(410, 'FRIZZ panel',
            'A schematic of the panel, not its artwork. Front row: 15 white keys, the effects '
            'freezer, shifter, folder, crusher, filter, flanger, phaser, resonator, slicer, '
            'wow & flutter, tape stop, delay, reverb, the chaos key and the master compressor. '
            'Above them 10 dark keys: the blank scene, scenes 1-4, save, copy and delete. Behind '
            'them the mode switch, the CHOMPI key (SHIFT), knobs 1-4, the transport wheel, PLAY, '
            'LOOP and VOLUME. Jacks: MIDI in and out on the left, AUX in and the headphones on '
            'the right, USB-C, the microSD card and the line out at the back.')
    s.add('<rect x="%s" y="%s" width="%s" height="%s" rx="10" class="b"/>'
          % (num(X(0)), num(Y(depth)), num(320 * sc), num(depth * sc)))
    keys = {k['name']: k for k in layout['keys']}
    # white keys, their effects below the panel
    for n in range(1, 16):
        k = keys['KEY%d' % n]
        w, h = 17.5 * sc, 17 * sc
        s.add('<rect x="%s" y="%s" width="%s" height="%s" rx="3" class="wk"/>'
              % (num(X(k['x']) - w / 2), num(Y(k['y']) - h / 2), num(w), num(h)))
        s.text(X(k['x']), Y(k['y']) + 5, str(n), 12, cls='gt', bold=True)
        lx, ly = X(k['x']) - 3, Y(0) + 14
        s.text(lx, ly, EFFECTS[n - 1], 12, anchor='start', rotate=40)
    # dark keys: the scenes in the middle, their functions above them
    for i, n in enumerate(range(16, 26)):
        k = keys['KEY%d' % n]
        w, h = 17.5 * sc, 15 * sc
        cls = 'sk' if 2 <= i <= 6 else 'dk'
        s.add('<rect x="%s" y="%s" width="%s" height="%s" rx="3" class="%s"/>'
              % (num(X(k['x']) - w / 2), num(Y(k['y']) - h / 2), num(w), num(h), cls))
        if DARK[i]:
            s.text(X(k['x']), Y(k['y']) - h / 2 - 6, DARK[i], 12)
    # the keys behind: CHOMPI (SHIFT), PLAY, LOOP; PLAY's and LOOP's names above them, as
    # the dark keys' names take the line below
    for name, label, above in (('KEY26', 'CHOMPI = SHIFT', False), ('KEY27', 'PLAY', True),
                               ('KEY28', 'LOOP', True)):
        k = keys[name]
        w = 14 * sc
        s.add('<rect x="%s" y="%s" width="%s" height="%s" rx="3" class="ba"/>'
              % (num(X(k['x']) - w / 2), num(Y(k['y']) - w / 2), num(w), num(w)))
        s.text(X(k['x']), Y(k['y']) - w / 2 - 6 if above else Y(k['y']) + w / 2 + 15, label, 12,
               bold=True)
    # knobs, their numbers in them, and the transport wheel (SWn: EncoderId's, app.js)
    knobs = {'SW4': '1', 'SW1': '2', 'SW2': '3', 'SW3': '4', 'SW6': ''}
    for e in layout['encoders']:
        x, y = X(e['x']), Y(e['y'])
        if e['name'] == 'SW5':
            # on the lower board; its wheel comes up through the panel's big hole, in the
            # knobs' row (the enclosure's drawing: 212.8, 71.36 less the panel's offset)
            y = Y(68.46)
            s.add('<circle cx="%s" cy="%s" r="%s" class="ba"/>' % (num(x), num(y), num(15.5 * sc)))
            s.text(x, y + 4.5, 'transport', 12, bold=True)
            continue
        s.add('<circle cx="%s" cy="%s" r="%s" class="b"/>' % (num(x), num(y), num(7 * sc)))
        s.text(x, y + 4.5, knobs[e['name']], 12, bold=True)
    s.text(X(119.7), Y(68.46) - 7 * sc - 6, 'knobs 1–4', 12, cls='m')
    s.text(X(300.65), Y(68.46) - 7 * sc - 6, 'VOLUME', 12, bold=True)
    # the LEDs above the knobs and the transport
    for l in layout['pth']:
        s.add('<circle cx="%s" cy="%s" r="3" class="led"/>' % (num(X(l['x'])), num(Y(l['y']))))
    # the mode switch
    t = layout['toggle']
    x, y = X(t['x']), Y(t['y'])
    s.add('<rect x="%s" y="%s" width="10" height="24" rx="5" class="b"/>' % (num(x - 5), num(y - 12)))
    s.add('<circle cx="%s" cy="%s" r="3.5" class="g"/>' % (num(x), num(y + 6)))
    s.text(x, y - 18, 'mode', 12, cls='m')
    # jacks: the sides and the back of the lower board, in the top board's coordinates: the
    # lower board's y + 99.29, which puts SW5 under its hole (y 68.46) and the side jacks
    # where the side panels' holes are; the back's x from the enclosure's back panel
    for y, name in ((60.2, 'MIDI OUT'), (38.7, 'MIDI IN')):
        s.add('<rect x="%s" y="%s" width="6" height="14" class="g"/>' % (num(X(0) - 6), num(Y(y) - 7)))
        s.text(X(0) - 12, Y(y) + 4.5, name, 12, anchor='end', bold=True)
    for y, name in ((60.2, 'AUX IN'), (38.7, 'PHONES')):
        s.add('<rect x="%s" y="%s" width="6" height="14" class="g"/>' % (num(X(320)), num(Y(y) - 7)))
        s.text(X(320) + 12, Y(y) + 4.5, name, 12, anchor='start', bold=True)
    for x, name in ((22.4, 'USB-C'), (250.4, 'microSD'), (296.8, 'LINE OUT')):
        s.add('<rect x="%s" y="%s" width="14" height="6" class="g"/>' % (num(X(x) - 7), num(Y(depth) - 6)))
        s.text(X(x), Y(depth) - 14, name, 12, bold=name.isupper() or name == 'USB-C')
    s.text(X(0) - 12, Y(depth) + 4, 'back', 12, anchor='end', cls='m')
    s.text(X(0) - 12, Y(0) + 2, 'front', 12, anchor='end', cls='m')
    for i, line in enumerate((
            'The play page (mode switch down; up is the settings page). Dark keys: the blank',
            'scene, scenes 1–4 (rose), save, copy, delete; the lower C# and D# do nothing here.',
            'A schematic to scale, from the board files: not the panel\'s artwork.')):
        s.text(20, 352 + i * 18, line, 12, anchor='start', cls='m')
    return s


# ---------- 6. the quick start's way of the sound, narrow for a phone ----------
def quickstart_flow():
    s = Svg(450, 'How the sound flows',
            'The input, AUX or the built-in mic after its input gain, goes two ways: to the '
            'looper, which records it before the effects, so nothing is printed into the loop; '
            'and, if monitored (the mic starts off), on to the input/loop mix, SHIFT + VOLUME, '
            'where the loop joins it. From there input and loop alike go through the effects '
            '(white keys 1-14), the compressor (white key 15) and VOLUME, the output gain, to '
            'the line out and the headphones.', w=480)
    s.box(50, 20, 200, 60, ['INPUT', ('AUX in, or the built-in mic', 'm'), ('input gain', 'm')],
          accent=True)
    # the looper takes the input before the monitoring, the mix and the effects
    s.line([(150, 80), (150, 212)], 'a')
    s.dot(150, 100, 'a')
    s.line([(150, 100), (375, 100), (375, 116)])
    s.text(262, 92, 'the input', 12, cls='m')
    s.box(290, 116, 170, 80, ['LOOPER', ('records here, before', 'm'), ('the effects: nothing is', 'm'),
                              ('printed into the loop', 'm')])
    s.text(142, 150, 'if monitored', 12, anchor='end')
    s.text(142, 166, 'the mic: off at first', 12, anchor='end', cls='m')
    s.box(50, 212, 200, 60, ['MIX', ('input ↔ loop', 'm'), ('SHIFT + VOLUME', 'm')])
    s.line([(375, 196), (375, 242), (250, 242)])
    s.text(312, 235, 'loop', 12, cls='m')
    s.line([(150, 272), (150, 292)], 'a')
    s.box(50, 292, 200, 60, ['EFFECTS', ('white keys 1–14,', 'm'), ('on input and loop alike', 'm')],
          accent=True)
    s.line([(150, 352), (150, 372)], 'a')
    s.box(80, 372, 140, 48, ['COMPRESSOR', ('white key 15', 'm')], accent=True)
    s.line([(220, 396), (240, 396)], 'a')
    s.box(240, 372, 110, 48, ['VOLUME', ('output gain', 'm')], accent=True)
    s.line([(350, 396), (385, 396)], 'a')
    s.jack(392, 396)
    s.text(404, 400.5, 'OUT', 13, anchor='start', bold=True)
    s.text(404, 416, 'line out,', 12, anchor='start', cls='m')
    s.text(404, 431, 'phones', 12, anchor='start', cls='m')
    return s


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, make in (('signal-flow', signal_flow), ('input', input_diagram),
                       ('fx-chain', fx_chain), ('clock-midi', clock_midi), ('panel', panel),
                       ('quickstart-flow', quickstart_flow)):
        print(name)
        svg = make().render()
        with open(os.path.join(OUT, name + '.svg'), 'w', encoding='utf-8') as f:
            f.write(svg)


if __name__ == '__main__':
    main()
