import { LED } from '../lib/drawing.js';

// one card per feature, each in one of the LED colours
export const usage = [
  {
    id: 'transport', title: 'Transport modes', colour: 'purple', keys: ['SHIFT', '+', 'WHEEL', '_click'],
    panel: { hl: ['shift', 'wheel'], labels: { shift: ['hold', 0, -90], wheel: ['click', 0, -90] } },
    diagram: { type: 'flow', steps: [['SPEED', LED.green], ['LENGTH', LED.blue], ['STEP', LED.purple]], back: true },
    items: ['<b>SHIFT + click</b>: speed → length → step',
      '<b>Speed</b>: as on TAPE'],
  },
  {
    id: 'length', title: 'Length mode', colour: 'blue', keys: ['WHEEL', '_turn, click or hold'],
    diagram: { type: 'length' },
    items: ['<b>Turn right</b>: double, <b>left</b>: halve',
      'Doubling a halved loop brings the cut part back',
      '<b>Click</b>: back to 1×',
      '<b>Hold</b>: this length becomes 1×'],
  },
  {
    id: 'step', title: 'Step mode', colour: 'teal', keys: ['WHEEL', '_turn or hold'],
    diagram: { type: 'sections' },
    items: ['<b>Turn</b>: next or previous section of a halved loop',
      '<b>Hold</b>: this section becomes 1×'],
  },
  {
    id: 'bake', title: 'Bake FX', colour: 'magenta', keys: ['SHIFT', '+', 'LOOP', '_hold'],
    panel: { hl: ['shift', 'loop'], leds: { loop: LED.magenta }, labels: { shift: ['hold', 0, -90], loop: ['hold', 0, -90] } },
    diagram: { type: 'flow', steps: [['FX ON'], ['BAKING', LED.magenta], ['FX RESET']] },
    items: ['Records the FX into the loop, then resets the FX knobs',
      'Loop LED <b>magenta</b>: baking, <b>red</b> flash: can’t bake now',
      'Pausing or changing speed or direction stops it'],
  },
  {
    id: 'undo', title: 'Undo', colour: 'green', keys: ['SHIFT', '+', 'PLAY', '_hold'],
    panel: { hl: ['shift', 'play'], leds: { play: LED.green }, labels: { shift: ['hold', 0, -90], play: ['hold', 0, -90] } },
    diagram: { type: 'flow', steps: [['LOOP'], ['+ OVERDUB'], ['UNDO', LED.green]], back: true },
    items: ['Undoes the last <b>overdub</b> or <b>bake</b>',
      '<b>Hold again</b>: redo',
      'Play LED <b>red</b> flash: nothing to undo'],
  },
  {
    id: 'mono', title: 'Line in mono', colour: 'red', keys: ['SHIFT', '+', 'LINE IN', '_hold'],
    panel: { hl: ['shift', 'line'], leds: { line: LED.red }, labels: { shift: ['hold', 0, -90], line: ['hold', 0, 80] } },
    diagram: { type: 'mono' },
    items: ['Mono cables are detected automatically',
      '<b>Hold</b> the line in key: next mode'],
  },
  {
    id: 'copy', title: 'Copy onto the loop', colour: 'orange', keys: ['COPY', '→', '_source', '→', 'LOOP'],
    panel: { hl: ['copy', 'loop', 'play'], labels: { copy: ['copy', 0, 80], loop: ['add', 0, -90], play: ['replace', -1, -90] } },
    items: ['Copied into a playing looper, a loop is <b>queued</b> and starts at the end of the pass',
      '<b>PLAY</b>: replace the loop',
      '<b>LOOP</b>: add to its end'],
  },
  {
    id: 'tempo', title: 'Metronome and MIDI clock', colour: 'pink', keys: ['WHEEL', '_click, hold or turn'],
    panel: { hl: ['wheel'], labels: { wheel: ['click / turn', 0, -90] } },
    diagram: { type: 'beats' },
    items: ['<b>Click</b>: metronome on/off, before the first loop',
      '<b>Hold</b>: time signature, 4/4, 3/4, 6/8, 5/4, 7/4',
      '<b>Turn</b>: tempo',
      'Syncs to MIDI clock (TRS or USB)',
      'First loop snaps to the beat',
      'Delay syncs to the beat once either is on',
      '<b>SHIFT + FX knob</b>: note values, 1/16 to 1/2'],
  },
  {
    id: 'grains', title: 'Grains', colour: 'magenta', keys: ['FX KNOB', '_turn or hold'],
    panel: { hl: ['knob3'], leds: { knob3: LED.magenta }, labels: { knob3: ['turn / hold', 1, -90] } },
    text: 'Keep turning the knob past full reverb and grains take over: small pieces of the last seconds, '
      + 'played back as a cloud. The further you turn, the denser and wider it gets, with octaves and fifths '
      + 'mixed in. The reverb fades to a soft tail and the LED turns from blue to magenta. '
      + 'The delay time sets the grain length, and when the delay is synced, the grains follow the beat. '
      + 'Hold the knob to freeze the cloud, hold again to let it go. '
      + 'If the FX go into the loop, the freeze ends when a recording or overdub ends.',
  },
  {
    id: 'threshold', title: 'Recording threshold', colour: 'yellow', keys: ['LEFT KNOB', '_turn', '→', 'CHOMPI', '_tap'],
    panel: { hl: ['knob0', 'shift'], leds: { knob0: LED.red }, labels: { shift: ['tap', -1, -90], knob0: ['turn', 0, -90] } },
    diagram: { type: 'flow', steps: [['WAITING', 'vu'], ['LOUD'], ['RECORDING', LED.red]], blink: true },
    items: ['<b>Left knob</b>: threshold, fully down is off',
      'Knob LED <b>red</b>: input above it',
      'Above zero, CHOMPI latches: <b>tap again</b> to stop'],
  },
  {
    id: 'kits', title: 'OP-1 drum kits', colour: 'teal', keys: ['START', '_/', 'END', '_turn'],
    panel: { hl: ['knob1', 'knob2'], labels: { knob1: ['start', -1, -90], knob2: ['end', 1, -90] } },
    items: ['Copy an OP-1 drum kit <code>.aif</code> onto the card as <code>jammi_a1.aif</code>, … (no <code>.wav</code> with the same name)',
      'Each key plays one slice, from the lowest key up',
      '<b>START / END</b>: trim the last played slice',
      'Keys blink while loading, red if the file can\'t play'],
  },
];

// the sections, as in the overview
const card = Object.fromEntries(usage.map((c) => [c.id, c]));
export const groups = [
  ['Looper', ['transport', 'length', 'step', 'undo', 'bake', 'copy', 'tempo']],
  ['Sampling', ['threshold', 'mono']],
  ['Effects', ['grains']],
  ['Drum kits', ['kits']],
].map(([title, ids]) => ({ title, cards: ids.map((id) => card[id]) }));

// USB, a section of its own
export const usb = [
  {
    id: 'usb-audio', title: 'Audio interface', colour: 'blue', keys: ['USB', '_to a computer'],
    diagram: { type: 'legend', rows: [['Main L / R', 'blue', 'the mix, as the speakers get it'], ['Input L / R', 'red', 'the input, as CHOMPI records it'],
      ['Stereo in', 'pink', 'what the computer plays']] },
    items: ['48 kHz, 24 bit, in and out at once',
      'Shows up as <b>CHOMPI</b>, no driver needed'],
  },
  {
    id: 'usb-input', title: 'USB as input', colour: 'blue', keys: ['MIC', '_hold'],
    panel: { hl: ['mic'], leds: { mic: LED.blue }, labels: { mic: ['hold', 0, 80] } },
    items: ['Audio from the computer becomes the input as soon as it plays, mic LED <b>blue</b>',
      'Same level as line in: sample it, loop it, monitor it',
      'Picking mic, line in or resample overrides it until the computer reconnects',
      '<b>Hold MIC</b>: mic ↔ USB',
      'Unplugged: back to line in or mic'],
  },
  {
    id: 'usb-midi', title: 'MIDI', colour: 'blue', keys: ['USB'],
    items: ['MIDI in and out, same as the TRS jack',
      'Clock in syncs the tempo and the delay'],
  },
];

// the arpeggiator, a section of its own
export const arp = [
  {
    id: 'arp', title: 'Speed', colour: 'green', keys: ['LEFT KNOB', '_turn'],
    diagram: { type: 'rate' },
    items: ['Colours: synced to the beat',
      'White: free running, slower to the right'],
  },
  {
    id: 'arp-pattern', title: 'Patterns', colour: 'green', keys: ['LEFT KNOB', '_hold + turn'],
    diagram: { type: 'legend', rows: [['Up', 'white', 'low to high'], ['Down', 'deep', 'high to low'],
      ['Up-down', 'blue', 'up, then back down'], ['As played', 'green', 'in the order you pressed'],
      ['Random', 'magenta', 'in random order'], ['Bounce', 'purple', 'each note, then an octave up'],
      ['Tresillo', 'lime', 'the 3 + 3 + 2 rhythm'], ['Swing', 'teal', 'every second note late']] },
    items: [],
  },
  {
    id: 'arp-octaves', title: 'Octaves', colour: 'green', keys: ['SHIFT', '+', 'LEFT KNOB', '_turn'],
    items: ['1, 2 or 3 octaves'],
  },
  {
    id: 'arp-latch', title: 'Hold', colour: 'green', keys: ['LEFT KNOB', '_hold'],
    text: 'Hold the knob and the arpeggio keeps playing after you release the keys. A new chord replaces it. '
      + 'With the arp off, the notes are sustained.',
  },
];
