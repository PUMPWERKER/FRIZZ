import { LED } from '../lib/drawing.js';

// one card per feature, each in one of the LED colours
export const usage = [
  {
    id: 'transport', title: 'Transport modes', colour: 'purple', keys: ['SHIFT', '+', 'WHEEL', '_click'],
    panel: { hl: ['shift', 'wheel'], labels: { shift: ['hold', 0, -90], wheel: ['click', 0, -90] } },
    diagram: { type: 'flow', steps: [['SPEED'], ['LENGTH', LED.blue], ['STEP', LED.purple]], back: true },
    items: ['<b>Speed</b>: the wheel sets the playback speed, as usual'],
  },
  {
    id: 'length', title: 'Length mode', colour: 'blue', keys: ['WHEEL', '_turn, click or hold'],
    diagram: { type: 'length' },
    items: ['Turn right to <b>double</b> the loop, left to <b>halve</b> it',
      'Doubling a halved loop brings back what was cut off',
      '<b>Click</b>: back to 1×',
      '<b>Hold</b>: the current loop becomes the new 1×'],
  },
  {
    id: 'step', title: 'Step mode', colour: 'teal', keys: ['WHEEL', '_turn or hold'],
    diagram: { type: 'sections' },
    items: ['Turn to jump to the <b>next or previous section</b> of a halved loop',
      '<b>Hold</b>: the current section becomes the new 1×'],
  },
  {
    id: 'bake', title: 'Bake FX', colour: 'magenta', keys: ['SHIFT', '+', 'LOOP', '_hold'],
    panel: { hl: ['shift', 'loop'], leds: { loop: LED.magenta }, labels: { shift: ['hold', 0, -90], loop: ['hold', 0, -90] } },
    diagram: { type: 'flow', steps: [['FX ON'], ['BAKING', LED.magenta], ['FX RESET']] },
    items: ['Records the FX into the loop and resets the FX knobs',
      'Loop LED is <b>magenta</b> while baking, <b>flashes red</b> if it can’t bake',
      'Pausing or changing speed or direction stops it'],
  },
  {
    id: 'undo', title: 'Undo', colour: 'green', keys: ['SHIFT', '+', 'PLAY', '_hold'],
    panel: { hl: ['shift', 'play'], leds: { play: LED.green }, labels: { shift: ['hold', 0, -90], play: ['hold', 0, -90] } },
    diagram: { type: 'flow', steps: [['LOOP'], ['+ OVERDUB'], ['UNDO', LED.green]], back: true },
    items: ['Undoes the last <b>overdub</b> or <b>bake</b>',
      'Hold again to <b>redo</b>',
      'Play LED <b>flashes red</b> if there is nothing to undo'],
  },
  {
    id: 'mono', title: 'Line in mono', colour: 'red', keys: ['SHIFT', '+', 'LINE IN', '_hold'],
    panel: { hl: ['shift', 'line'], leds: { line: LED.red }, labels: { shift: ['hold', 0, -90], line: ['hold', 0, 80] } },
    diagram: { type: 'mono' },
    items: ['Mono cables are detected automatically',
      'Hold the line in key to switch modes'],
  },
  {
    id: 'copy', title: 'Copy onto the loop', colour: 'orange', keys: ['COPY', '→', '_source', '→', 'LOOP'],
    panel: { hl: ['copy', 'loop', 'play'], labels: { copy: ['copy', 0, 80], loop: ['add', 0, -90], play: ['replace', -1, -90] } },
    items: ['A loop copied into the playing looper is <b>queued</b>: playback goes on and switches at the end of the current pass',
      '<b>PLAY</b> replaces the looper’s loop with it',
      '<b>LOOP</b> adds it to the end of the looper’s loop'],
  },
  {
    id: 'threshold', title: 'Recording threshold', colour: 'yellow', keys: ['LEFT KNOB', '_turn', '→', 'CHOMPI', '_tap'],
    panel: { hl: ['knob0', 'shift'], leds: { knob0: LED.red }, labels: { shift: ['tap', -1, -90], knob0: ['turn', 0, -90] } },
    diagram: { type: 'flow', steps: [['WAITING', 'vu'], ['LOUD'], ['RECORDING', LED.red]], blink: true },
    items: ['The <b>left knob</b> sets the threshold, fully down is off',
      'Knob LED turns <b>red</b> when the input is above it',
      'Above zero the CHOMPI key latches, tap again to stop'],
  },
];
