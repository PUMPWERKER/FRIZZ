import { LED } from '../lib/drawing.js';

// A walkthrough: sample a clap, build a loop on it, keep it. One part per heading, steps numbered across all of them.
export const tutorial = [
  {
    part: 'Sample a clap',
    steps: [
      {
        title: 'Set the threshold', colour: 'yellow', keys: ['LEFT KNOB', '_turn'],
        panel: { hl: ['knob0'], leds: { knob0: LED.red }, labels: { knob0: ['turn', 0, -90] } },
        text: 'Flip the switch to CHOMPI mode. Clap and turn the left knob up until its LED turns <b>red</b> only on the claps.',
      },
      {
        title: 'Record the clap', colour: 'orange', keys: ['CHOMPI', '_tap', '→', 'CHOMPI', '_tap'],
        diagram: { type: 'flow', steps: [['WAITING', 'vu'], ['CLAP'], ['RECORDING', LED.red]], blink: true },
        text: 'Tap CHOMPI, clap, tap CHOMPI again. The clap is now on every key. Flip the switch out of CHOMPI mode.',
      },
    ],
  },
  {
    part: 'Loop a beat',
    steps: [
      {
        title: 'Start the metronome', colour: 'green', keys: ['WHEEL', '_click or turn'],
        panel: { hl: ['wheel'], labels: { wheel: ['click / turn', 0, -90] } },
        text: 'Click the wheel. Turn it to set the tempo.',
      },
      {
        title: 'Play the beat', colour: 'teal', keys: ['PLAY', '_hold', '+', 'LOOP', '→', 'KEY', '→', 'PLAY'],
        panel: { hl: ['loop', 'play'], leds: { loop: LED.red }, labels: { play: ['hold', -1, -90], loop: ['press', 0, -90] } },
        diagram: { type: 'beats' },
        text: 'Hold PLAY, then press LOOP, and let go when the loop key blinks red. Play the clap on any key, a simple beat is fine. Press PLAY when it’s done.',
        notes: ['Start and end snap to the nearest beat'],
      },
      {
        title: 'Listen', colour: 'blue', keys: ['LOOP', '+', 'PLAY', '_hold 2s'],
        text: 'Let it go round. To start over, hold LOOP and PLAY for two seconds, then click the wheel to start the metronome again.',
      },
    ],
  },
  {
    part: 'Build it up',
    steps: [
      {
        title: 'Make room', colour: 'purple', keys: ['SHIFT', '+', 'WHEEL', '_click', '→', 'WHEEL', '_turn right'],
        panel: { hl: ['shift', 'wheel'], labels: { shift: ['hold', 0, -90], wheel: ['click, then turn', 0, -90] } },
        diagram: { type: 'length' },
        text: 'Hold SHIFT and click the wheel until its LEDs are <b>light blue</b>. Let go and turn it right until the right LED is <b>yellow</b>. The beat now plays four times.',
      },
      {
        title: 'Add a bass', colour: 'magenta', keys: ['SHIFT', '+', 'KEY', '→', 'LOOP', '→', 'PLAY'],
        panel: { hl: ['loop'], leds: { loop: LED.yellow }, labels: { loop: ['press', 0, -90] } },
        text: 'Hold SHIFT and press a lit key to pick a bass. Press LOOP, play along, press PLAY when you’re done.',
      },
      {
        title: 'Try again if you like', colour: 'red', keys: ['SHIFT', '+', 'PLAY', '_hold'],
        panel: { hl: ['shift', 'play'], leds: { play: LED.green }, labels: { shift: ['hold', 0, -90], play: ['hold', 0, -90] } },
        text: 'SHIFT + hold PLAY undoes the take. Hold again to bring it back.',
      },
      {
        title: 'More room', colour: 'yellow', keys: ['WHEEL', '_turn right'],
        text: 'Turn the wheel right until the right LED is <b>orange</b>.',
      },
      {
        title: 'Add a melody', colour: 'green', keys: ['SHIFT', '+', 'KEY', '→', 'LOOP', '→', 'PLAY'],
        text: 'Hold SHIFT and press another lit key, then LOOP, play, PLAY.',
      },
    ],
  },
  {
    part: 'Keep it',
    steps: [
      {
        title: 'Save it', colour: 'teal', keys: ['SHIFT', '+', 'CUBBI', '→', 'COPY', '→', 'LOOP', '→', '_slot', '→', 'CHOMPI'],
        panel: { hl: ['copy', 'loop'], labels: { copy: ['copy', 0, 80], loop: ['source', 0, -90] } },
        text: 'Hold SHIFT and press CUBBI, then COPY. Press LOOP, then a dim blinking key, then CHOMPI. It’s on the SD card now.',
      },
    ],
  },
];
