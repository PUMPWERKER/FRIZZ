// The page: the panel drawn from layout.json (CHOMPI's own board file), the controls, and the
// plumbing between the twin's worker (worker.js) and the audio thread (worklet.js).
'use strict';

const SVG = 'http://www.w3.org/2000/svg';
const panel = document.getElementById('panel');
const $ = (id) => document.getElementById(id);

// what each control is, for the labels (MANUAL.md)
const WHITE = ['Freezer', 'Shifter', 'Folder', 'Crusher', 'Filter', 'Flanger', 'Resonator', 'Slicer',
  'Wow', 'Tape stop', '', 'Delay', 'Reverb', '', 'Comp'];
const DARK = ['', '', 'Scene', 'Scene 1', 'Scene 2', 'Scene 3', 'Scene 4', 'Save', 'Copy', 'Delete'];
const TOP = { KEY26: 'SHIFT', KEY27: 'PLAY', KEY28: 'LOOP' };
const KNOBS = { SW4: 'knob 1', SW1: 'knob 2', SW2: 'knob 3', SW3: 'knob 4', SW5: 'transport', SW6: 'VOLUME' };
// keyboard: white keys 1-10 Q..P, 11-15 A..G; dark keys 1..0; SHIFT, PLAY, LOOP
const KEYBOARD = {};
'QWERTYUIOP'.split('').forEach((c, i) => KEYBOARD['Key' + c] = 'KEY_' + (i + 1));
'ASDFG'.split('').forEach((c, i) => KEYBOARD['Key' + c] = 'KEY_' + (i + 11));
'1234567890'.split('').forEach((c, i) => KEYBOARD['Digit' + c] = 'KEY_' + (i + 16));
KEYBOARD.ShiftLeft = KEYBOARD.ShiftRight = 'KEY_26';
KEYBOARD.Space = 'KEY_27';
KEYBOARD.Enter = 'KEY_28';
const HINT = { KEY_26: '⇧', KEY_27: '␣', KEY_28: '⏎' };
for (const [code, key] of Object.entries(KEYBOARD))
  if (!HINT[key]) HINT[key] = code.replace(/^Key|^Digit/, '');

let layout = null;
let worker = null, ctx = null, node = null, liveSource = null;
const held = new Set();     // keys down (by name, KEY_n)
const latched = new Set();  // keys held by a right-click
let toggleLevel = 0; // the mode switch: 0 down (play), 1 up (settings), as the 4021 reads it
let fileAudio = null;
const ledEls = [];          // 35: the panel's 10, then the keys' 25
const keyEls = {};
let card = {};
try { card = JSON.parse(localStorage.getItem('frizz-twin-card') || '{}'); } catch (e) { card = {}; }

// ---------- the panel ----------
function el(tag, attrs, parent)
{
  const e = document.createElementNS(SVG, tag);
  for (const [k, v] of Object.entries(attrs || {})) e.setAttribute(k, v);
  if (parent) parent.appendChild(e);
  return e;
}

async function DrawPanel()
{
  layout = await (await fetch('layout.json')).json();
  // the play side of the board: y from the keys up to the LEDs; EAGLE's y runs up
  const top = 104, bottom = 4, left = 4, right = 316;
  const Y = (y) => top - y;
  panel.setAttribute('viewBox', `${left} 0 ${right - left} ${top - bottom}`);
  const defs = el('defs', {}, panel);
  const blur = el('filter', { id: 'glow', x: '-100%', y: '-100%', width: '300%', height: '300%' }, defs);
  el('feGaussianBlur', { stdDeviation: '1.2' }, blur);

  // keys: KEYn is Hardware::SwId's KEY_n
  for (const k of layout.keys)
  {
    const n = +k.name.slice(3);
    const name = 'KEY_' + n;
    const white = n <= 15, top3 = n >= 26;
    const g = el('g', { class: 'key ' + (white ? 'white' : 'dark') }, panel);
    const w = top3 ? 14 : 17.5, h = white ? 17 : 15;
    el('rect', { x: k.x - w / 2, y: Y(k.y) - h / 2, width: w, height: h, rx: 1.6 }, g);
    // the light of the key's LED, if it has one
    g.tint = el('rect', { x: k.x - w / 2, y: Y(k.y) - h / 2, width: w, height: h, rx: 1.6, class: 'tint', opacity: 0 }, g);
    const label = white ? WHITE[n - 1] : top3 ? TOP[k.name] : DARK[n - 16];
    el('text', { x: k.x, y: Y(k.y) + h / 2 - 1.6 }, g).textContent = label || '';
    el('text', { x: k.x, y: Y(k.y) - h / 2 + 3.2, class: 'hint' }, g).textContent = HINT[name] || '';
    keyEls[name] = g;
    g.addEventListener('pointerdown', (e) => {
      if (e.button === 2 || e.ctrlKey) { ToggleHold(name); e.preventDefault(); return; }
      if (e.button !== 0) return;
      g.setPointerCapture(e.pointerId);
      KeyDown(name);
    });
    g.addEventListener('pointerup', (e) => { if (e.button === 0 && !latched.has(name)) KeyUp(name); });
    g.addEventListener('contextmenu', (e) => e.preventDefault());
  }

  // encoders: SWn is EncoderId's SWn; the transport (SW5) sits on the board's other half, so
  // it's drawn between its two LEDs
  for (const s of layout.encoders)
  {
    const n = +s.name.slice(2);
    const x = n === 5 ? 210.2 : s.x, y = n === 5 ? 70 : s.y;
    const r = n === 5 ? 10 : 7.5;
    const g = el('g', { class: 'knob' }, panel);
    el('circle', { cx: x, cy: Y(y), r, class: 'cap' }, g);
    const mark = el('line', { x1: x, y1: Y(y) - r + 1.2, x2: x, y2: Y(y) - r + 4, class: 'mark' }, g);
    const push = el('circle', { cx: x, cy: Y(y), r: r * 0.38, class: 'push' }, g);
    el('text', { x, y: Y(y) + r + 3.6, class: 'label' }, panel).textContent = KNOBS[s.name];
    let angle = 0;
    const turn = (d) => {
      if (!d) return;
      angle += d * 15;
      mark.setAttribute('transform', `rotate(${angle} ${x} ${Y(y)})`);
      Send('turn', [n, d]);
    };
    let wheel = 0;
    g.addEventListener('wheel', (e) => {
      e.preventDefault();
      // with Shift held (SHIFT + turn), browsers scroll sideways: take whichever moved
      const delta = e.deltaY || e.deltaX;
      wheel += e.deltaMode === 1 ? delta * 33 : delta;
      const d = Math.trunc(wheel / 50);
      wheel -= d * 50;
      turn(-d);
    }, { passive: false });
    let dragY = null;
    g.addEventListener('pointerdown', (e) => {
      if (e.target === push) return;
      g.setPointerCapture(e.pointerId);
      dragY = e.clientY;
    });
    g.addEventListener('pointermove', (e) => {
      if (dragY === null) return;
      const d = Math.trunc((dragY - e.clientY) / 8);
      if (d) { dragY -= d * 8; turn(d); }
    });
    g.addEventListener('pointerup', () => { dragY = null; });
    const sw = 'ENC_' + n + '_SW';
    push.addEventListener('pointerdown', (e) => {
      e.stopPropagation();
      push.setPointerCapture(e.pointerId);
      g.classList.add('pressed');
      Send('down', [sw]);
    });
    push.addEventListener('pointerup', () => { g.classList.remove('pressed'); Send('up', [sw]); });
  }

  // the mode switch
  const t = layout.toggle;
  const tg = el('g', { class: 'toggle' }, panel);
  el('rect', { x: t.x - 3, y: Y(t.y) - 6, width: 6, height: 12, rx: 3, fill: '#111317', stroke: '#555a66', 'stroke-width': .5 }, tg);
  const lever = el('circle', { cx: t.x, cy: Y(t.y) + (toggleLevel ? -3 : 3), r: 2.4, fill: '#bbb' }, tg);
  el('text', { x: t.x, y: Y(t.y) + 9.5, class: 'label' }, panel).textContent = 'mode';
  tg.addEventListener('click', () => {
    toggleLevel = toggleLevel ? 0 : 1;
    lever.setAttribute('cy', Y(t.y) + (toggleLevel ? -3 : 3));
    Send('toggle', [toggleLevel]);
  });

  // LEDs, in the firmware's order: the panel's chain, then the keys'
  for (const l of layout.pth.concat(layout.smt))
  {
    const smt = l.name.startsWith('LED_K');
    const g = el('g', {}, panel);
    const glow = el('circle', { cx: l.x, cy: Y(l.y), r: smt ? 3.2 : 3.4, fill: '#000', filter: 'url(#glow)', opacity: 0 }, g);
    const dot = el('circle', { cx: l.x, cy: Y(l.y), r: smt ? 1.5 : 2, fill: '#0b0c0e', stroke: '#000', 'stroke-width': .3 }, g);
    // a key LED lights the key it's under: the nearest one
    let key = null;
    if (smt)
      key = layout.keys.reduce((a, k) => Math.hypot(k.x - l.x, k.y - l.y) < Math.hypot(a.x - l.x, a.y - l.y) ? k : a);
    ledEls.push({ glow, dot, tint: key ? keyEls['KEY_' + key.name.slice(3)].tint : null });
  }
}

// an LED's byte to what it looks like: LEDs look bright long before full PWM
function Shade(v) { return Math.round(255 * Math.sqrt(v / 255)); }

function ShowLeds(leds)
{
  for (let i = 0; i < ledEls.length; i++)
  {
    const r = leds[i * 3], g = leds[i * 3 + 1], b = leds[i * 3 + 2];
    const lit = r + g + b > 0;
    const c = `rgb(${Shade(r)},${Shade(g)},${Shade(b)})`;
    ledEls[i].dot.setAttribute('fill', lit ? c : '#0b0c0e');
    ledEls[i].glow.setAttribute('fill', c);
    ledEls[i].glow.setAttribute('opacity', lit ? Math.min(1, Math.max(r, g, b) / 120) : 0);
    if (ledEls[i].tint)
    {
      ledEls[i].tint.setAttribute('fill', c);
      ledEls[i].tint.setAttribute('opacity', lit ? Math.min(.75, Math.sqrt(Math.max(r, g, b) / 255)) : 0);
    }
  }
}

// ---------- keys ----------
function Send(cmd, args) { if (worker) worker.postMessage({ cmd, args }); }

function KeyDown(name)
{
  if (held.has(name)) return;
  held.add(name);
  keyEls[name]?.classList.add('down');
  Send('down', [name]);
}

function KeyUp(name)
{
  if (!held.has(name)) return;
  held.delete(name);
  latched.delete(name);
  keyEls[name]?.classList.remove('down', 'held');
  Send('up', [name]);
}

function ToggleHold(name)
{
  if (latched.has(name)) { KeyUp(name); return; }
  latched.add(name);
  keyEls[name]?.classList.add('held');
  KeyDown(name);
}

addEventListener('keydown', (e) => {
  const k = KEYBOARD[e.code];
  if (!k || e.repeat || e.target.tagName === 'INPUT' || e.target.tagName === 'SELECT') return;
  e.preventDefault();
  KeyDown(k);
});
addEventListener('keyup', (e) => {
  const k = KEYBOARD[e.code];
  if (!k || latched.has(k)) return;
  e.preventDefault();
  KeyUp(k);
});
// leaving the page with a key down: let go of it, as fingers would
addEventListener('blur', () => { for (const k of [...held]) if (!latched.has(k)) KeyUp(k); });

// ---------- power ----------
async function PowerOn(script)
{
  PowerOff();
  if (!ctx)
  {
    ctx = new AudioContext({ sampleRate: 48000 });
    await ctx.audioWorklet.addModule('worklet.js');
  }
  await ctx.resume();
  node = new AudioWorkletNode(ctx, 'twin-output', { numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2] });
  node.connect(ctx.destination);
  if (liveSource) liveSource.connect(node);
  const channel = new MessageChannel();
  node.port.postMessage({ port: channel.port1 }, [channel.port1]);

  worker = new Worker('worker.js');
  worker.onmessage = OnWorker;
  worker.postMessage({
    start: true, port: channel.port2, card, script,
    held: script ? [] : [...held], toggle: toggleLevel,
    battery: { volts: +$('volts').value, plugged: $('plugged').checked },
    source: Source(), tone: { freq: +$('freq').value }, file: fileAudio, output: $('output').value,
  }, [channel.port2]);
  if (script) { for (const k of [...held]) KeyUp(k); }
  $('status').textContent = 'on';
  $('status').classList.remove('off');
  $('power').textContent = 'Switch off and on';
}

function PowerOff()
{
  if (worker) worker.terminate();
  worker = null;
  if (node) node.disconnect();
  node = null;
  ShowLeds(new Uint8Array(35 * 3));
  $('status').textContent = 'off';
  $('status').classList.add('off');
  $('clockbtn').textContent = 'Start';
  $('rec').textContent = 'Record audio';
  $('rec').classList.remove('on');
}

function OnWorker(e)
{
  const d = e.data;
  if (d.version) $('version').textContent = d.version;
  if (d.leds) ShowLeds(d.leds);
  if (d.now !== undefined) $('clock').textContent = (d.now / 1000).toFixed(1) + ' s';
  if (d.underruns !== undefined) $('underruns').textContent = d.underruns;
  if (d.powered === false) { $('status').textContent = 'off (by itself)'; $('status').classList.add('off'); }
  if (d.card) { card = d.card; SaveCard(); }
  if (d.wav) Download(new Blob([d.wav], { type: 'audio/wav' }), 'frizz-twin.wav');
  if (d.log) Download(new Blob([d.log], { type: 'text/plain' }), 'frizz-twin.txt');
}

function Download(blob, name)
{
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

function SaveCard()
{
  try { localStorage.setItem('frizz-twin-card', JSON.stringify(card)); } catch (e) { /* private window */ }
  const names = Object.keys(card);
  $('cardinfo').textContent = names.length ? names.join(', ') : 'empty';
}

// ---------- controls ----------
function Source() { return document.querySelector('input[name=src]:checked').value; }

$('power').onclick = () => PowerOn();
$('off').onclick = () => { PowerOff(); $('power').textContent = 'Switch on'; };

for (const r of document.querySelectorAll('input[name=src]'))
  r.onchange = async () => {
    const s = Source();
    if (s === 'live' && !liveSource && ctx)
    {
      const stream = await navigator.mediaDevices.getUserMedia({
        audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false } });
      liveSource = ctx.createMediaStreamSource(stream);
      if (node) liveSource.connect(node);
    }
    worker?.postMessage({ source: s });
    if (s === 'tone') Send('input', ['sine', $('freq').value, '0.3']);
    if (s === 'off') Send('input', ['off']);
  };
$('freq').onchange = () => worker?.postMessage({ tone: { freq: +$('freq').value } });
$('output').onchange = () => worker?.postMessage({ output: $('output').value });
$('file').onchange = async () => {
  const f = $('file').files[0];
  if (!f) return;
  const c = ctx || new AudioContext({ sampleRate: 48000 });
  const buf = await c.decodeAudioData(await f.arrayBuffer());
  const l = buf.getChannelData(0), r = buf.numberOfChannels > 1 ? buf.getChannelData(1) : l;
  fileAudio = new Float32Array(l.length * 2);
  for (let i = 0; i < l.length; i++) { fileAudio[i * 2] = l[i]; fileAudio[i * 2 + 1] = r[i]; }
  document.querySelector('input[name=src][value=file]').checked = true;
  worker?.postMessage({ file: fileAudio, source: 'file' });
};

$('clockbtn').onclick = () => {
  const on = $('clockbtn').textContent === 'Start';
  Send('clock', [on ? $('bpm').value : 0]);
  $('clockbtn').textContent = on ? 'Stop' : 'Start';
};
$('bpm').onchange = () => { if ($('clockbtn').textContent === 'Stop') Send('clock', [$('bpm').value]); };

function SendBattery()
{
  $('voltsval').textContent = (+$('volts').value).toFixed(2);
  Send('battery', [$('volts').value].concat($('plugged').checked ? ['plugged'] : []));
}
$('volts').oninput = SendBattery;
$('plugged').onchange = SendBattery;

$('rec').onclick = () => {
  if (!worker) return;
  const on = !$('rec').classList.contains('on');
  worker.postMessage({ record: on });
  $('rec').classList.toggle('on', on);
  $('rec').textContent = on ? 'Stop and download' : 'Record audio';
};
$('logbtn').onclick = () => worker?.postMessage({ getLog: true });
$('script').onchange = async () => {
  const f = $('script').files[0];
  if (f) PowerOn(await f.text());
  $('script').value = '';
};

$('carddl').onclick = () => {
  let text = '';
  for (const [path, body] of Object.entries(card)) text += `--- ${path}\n${body}\n`;
  Download(new Blob([text || '(empty)\n'], { type: 'text/plain' }), 'frizz-twin-card.txt');
};
$('cardclear').onclick = () => {
  if (!confirm('Empty the twin\'s SD card? It takes effect at the next power-on.')) return;
  card = {};
  SaveCard();
};

// a real MIDI device's messages into the TRS jack
if (navigator.requestMIDIAccess)
  navigator.requestMIDIAccess().then((access) => {
    const sel = $('midiin');
    const fill = () => {
      const cur = sel.value;
      sel.replaceChildren(new Option('none', ''));
      for (const i of access.inputs.values()) sel.add(new Option(i.name, i.id));
      sel.value = cur;
    };
    fill();
    access.onstatechange = fill;
    let input = null;
    sel.onchange = () => {
      if (input) input.onmidimessage = null;
      input = access.inputs.get(sel.value) || null;
      if (input) input.onmidimessage = (m) => Send('midi', [...m.data].map((b) => b.toString(16)));
    };
  }).catch(() => {});

SaveCard();
DrawPanel();
