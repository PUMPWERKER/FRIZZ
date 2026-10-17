// The virtual CHOMPI itself: FRIZZ's firmware (build/frizz-twin.wasm) in a worker. One worker
// is one power cycle: the page starts a new one to switch the device off and on.
//
// It makes audio in chunks of 32 blocks (768 frames, 16 ms) whenever less than kAhead frames
// are on their way to the speakers (sent, not yet played), applies the page's key presses and knob turns as they come, and
// sends back the LEDs and, now and then, the SD card's files.
importScripts('build/frizz-twin.js');

const kBlock = 24, kChannels = 4, kChunkBlocks = 32, kChunk = kBlock * kChunkBlocks;
const kAhead = 2048; // frames queued in the audio thread: about 43 ms

let m = null;           // the module
let audio = null;       // MessagePort to the audio thread
let sent = 0;           // frames sent to the audio thread, in all
let underruns = 0;
let output = 'master';  // which pair goes to the speakers: master or headphones

// the audio into AUX
let source = 'tone';    // tone, file, live, off
let tone = { freq: 220, amp: 0.3, phase: 0 };
let file = null, filePos = 0; // Float32Array, interleaved L R at 48 kHz
const live = [];               // chunks from the audio thread
let liveOffset = 0;

// a MIDI clock of our own, 24 ticks a beat
let clockBpm = 0, clockNext = 0;

// recording: the output as a WAV, and every input as a script line (README.md's commands)
let recording = null;
let log = [];
let logStart = 0;

// a script being played back: lines with the time they're due, from power-on or, after its
// `booted` line (a bug report, EventLog.h), from when the firmware's main loop started
let script = [];
let bootedAt = -1;

let lastLeds = '';
let lastCard = '';
let cardTimer = 0;

function str(s) { return m.stringToNewUTF8(s); }

function Log(line)
{
    log.push({ t: m._twin_now_ms(), line });
}

function Apply(cmd, args, fromScript)
{
    switch (cmd)
    {
    case 'down':
    case 'up':
        m._twin_press(str(args[0]), cmd === 'down' ? 1 : 0);
        break;
    case 'turn':
        m._twin_turn(+args[0], +args[1]);
        break;
    case 'toggle':
        m._twin_toggle(+args[0]);
        break;
    case 'midi':
        for (const b of args) m._twin_midi(parseInt(b, 16));
        break;
    case 'clock':
        clockBpm = +args[0];
        clockNext = m._twin_now_ms();
        break;
    case 'card': // a script's file, its text after the path
        if (args[0] === 'file') m._twin_card_put(str(args[1]), str(args[2].text));
        return;
    case 'usbaudio': // the computer ejects the card lent to it
        if (args[0] !== 'eject') return;
        m._twin_eject_card();
        break;
    case 'battery':
        m._twin_battery(+args[0], args.includes('plugged') ? 1 : 0, args.includes('full') ? 1 : 0);
        break;
    case 'input':
        if (args[0] === 'sine')
        {
            source = 'tone';
            tone.freq = +args[1] || 220;
            tone.amp = args[2] === undefined ? 0.3 : +args[2];
        }
        else if (args[0] === 'off') source = 'off';
        break;
    default:
        return;
    }
    // MIDI clock ticks aren't logged one by one: the clock command is
    if (!(cmd === 'midi' && args.length === 1 && args[0].toLowerCase() === 'f8'))
        Log([cmd, ...args].join(' '));
}

/** A script (the command line twin's format): its timed commands, from power-on, and the
 *  files its `card file` blocks put on the card; a block after `booted` (a bug report's lend,
 *  EventLog.h) goes there at its time, as a command */
function ParseScript(text)
{
    const out = [], card = {};
    let t = 0, booted = false, file = null;
    for (const raw of text.split('\n'))
    {
        if (file !== null && raw.startsWith('|'))
        {
            file.text += raw.slice(1) + '\n';
            continue;
        }
        file = null;
        const words = raw.replace(/#.*/, '').trim().split(/\s+/).filter(Boolean);
        if (words.length === 0) continue;
        const [cmd, ...args] = words;
        if (cmd === 'card' && args[0] === 'file')
        {
            file = { text: '' };
            if (booted) out.push({ t, cmd, args: [args[0], args[1], file], booted });
            else card[args[1]] = file;
        }
        else if (cmd === 'booted') { booted = true; t = 0; }
        else if (cmd === 'wait') t += +args[0];
        else if (cmd === 'at') t = Math.max(t, +args[0]);
        else if (cmd === 'tap')
        {
            out.push({ t, cmd: 'down', args: [args[0]], booted });
            t += args[1] ? +args[1] : 60;
            out.push({ t, cmd: 'up', args: [args[0]], booted });
        }
        else out.push({ t, cmd, args, booted });
    }
    for (const path in card) card[path] = card[path].text;
    return { items: out, card };
}

function FillInput(inp)
{
    for (let i = 0; i < kChunk; i++)
    {
        let l = 0, r = 0;
        if (source === 'tone')
        {
            l = r = tone.amp * Math.sin(tone.phase);
            tone.phase += 2 * Math.PI * tone.freq / 48000;
            if (tone.phase > 2 * Math.PI) tone.phase -= 2 * Math.PI;
        }
        else if (source === 'file' && file)
        {
            l = file[filePos];
            r = file[filePos + 1];
            filePos = (filePos + 2) % file.length;
        }
        else if (source === 'live' && live.length)
        {
            l = live[0][liveOffset * 2];
            r = live[0][liveOffset * 2 + 1];
            if (++liveOffset * 2 >= live[0].length)
            {
                live.shift();
                liveOffset = 0;
            }
        }
        m.HEAPF32[inp + i * kChannels + 2] = l;
        m.HEAPF32[inp + i * kChannels + 3] = r;
    }
    // more than a few chunks behind: the live input is ahead of us, drop the oldest
    while (live.length > 8) live.shift();
}

function Produce()
{
    const inp = m._twin_in() >> 2, outp = m._twin_out() >> 2;
    const now = m._twin_now_ms();
    if (bootedAt < 0 && m._twin_main_loop_running()) bootedAt = now;
    while (script.length && script[0].t + (script[0].booted ? bootedAt : 0) <= now
           && !(script[0].booted && bootedAt < 0))
    {
        const s = script.shift();
        Apply(s.cmd, s.args, true);
    }
    while (clockBpm > 0 && clockNext <= now + 16)
    {
        m._twin_midi(0xF8);
        clockNext += 60000 / (clockBpm * 24);
    }
    FillInput(inp);
    m._twin_run(kChunkBlocks);

    const pair = output === 'master' ? 2 : 0;
    const chunk = new Float32Array(kChunk * 2);
    for (let i = 0; i < kChunk; i++)
    {
        chunk[i * 2] = m.HEAPF32[outp + i * kChannels + pair];
        chunk[i * 2 + 1] = m.HEAPF32[outp + i * kChannels + pair + 1];
    }
    if (recording) recording.push(chunk.slice());
    audio.postMessage(chunk, [chunk.buffer]);
    sent += kChunk;

    const p = m._twin_leds();
    const leds = m.HEAPU8.slice(p, p + 35 * 3);
    const key = leds.join(',');
    if (key !== lastLeds)
    {
        lastLeds = key;
        postMessage({ leds, now: m._twin_now_ms(), powered: !!m._twin_powered() });
    }

    if (++cardTimer >= 60) // about once a second
    {
        cardTimer = 0;
        const card = m.UTF8ToString(m._twin_card_files());
        if (card !== lastCard)
        {
            lastCard = card;
            postMessage({ card: ParseCard(card) });
        }
        postMessage({ now: m._twin_now_ms(), underruns });
    }
}

function ParseCard(text)
{
    const files = {};
    let p = 0;
    while (p < text.length)
    {
        const a = text.indexOf('\n', p);
        const b = text.indexOf('\n', a + 1);
        const path = text.slice(p, a), len = +text.slice(a + 1, b);
        files[path] = text.slice(b + 1, b + 1 + len);
        p = b + 1 + len;
    }
    return files;
}

function Wav(chunks)
{
    const frames = chunks.reduce((n, c) => n + c.length / 2, 0);
    const buf = new ArrayBuffer(44 + frames * 8);
    const v = new DataView(buf);
    const w = (o, s) => { for (let i = 0; i < s.length; i++) v.setUint8(o + i, s.charCodeAt(i)); };
    w(0, 'RIFF'); v.setUint32(4, 36 + frames * 8, true); w(8, 'WAVEfmt ');
    v.setUint32(16, 16, true); v.setUint16(20, 3, true); v.setUint16(22, 2, true);
    v.setUint32(24, 48000, true); v.setUint32(28, 48000 * 8, true); v.setUint16(32, 8, true);
    v.setUint16(34, 32, true); w(36, 'data'); v.setUint32(40, frames * 8, true);
    let o = 44;
    for (const c of chunks)
        for (let i = 0; i < c.length; i++, o += 4) v.setFloat32(o, c[i], true);
    return buf;
}

function ScriptText()
{
    // the time lines as the command line twin reads them, so a log replays there too
    let out = '# FRIZZ twin ' + m.UTF8ToString(m._twin_version()) + '\n';
    let t = -1;
    for (const e of log)
    {
        if (e.t !== t) out += 'at ' + e.t + '\n';
        t = e.t;
        out += e.line + '\n';
    }
    return out;
}

onmessage = async (e) => {
    const d = e.data;
    if (d.start)
    {
        m = await FrizzTwin({ locateFile: (f) => 'build/' + f });
        const parsed = d.script ? ParseScript(d.script) : { items: [], card: {} };
        // a script's own card files over the page's
        for (const [path, text] of Object.entries({ ...(d.card || {}), ...parsed.card }))
            m._twin_card_put(str(path), str(text));
        script = parsed.items;
        // a script plays from power-on: its own keys and inputs only
        for (const k of d.script ? [] : d.held || []) m._twin_press(str(k), 1);
        if (d.toggle !== undefined) m._twin_toggle(d.toggle);
        if (d.battery) m._twin_battery(d.battery.volts, d.battery.plugged ? 1 : 0, 0);
        if (d.source) source = d.source;
        if (d.tone) Object.assign(tone, d.tone);
        if (d.file) file = d.file;
        if (d.output) output = d.output;
        // the log starts with what's plugged in, so a replay starts from the same
        if (source === 'tone') Log(`input sine ${tone.freq} ${tone.amp}`);
        else if (source === 'off') Log('input off');
        else Log(`# input: ${source === 'file' ? 'an audio file' : 'mic / line in'}, not in the script`);
        if (d.battery) Log(`battery ${d.battery.volts}${d.battery.plugged ? ' plugged' : ''}`);
        if (d.toggle !== undefined) Log('toggle ' + d.toggle);
        for (const k of d.script ? [] : d.held || []) Log('down ' + k);
        m._twin_boot();
        postMessage({ version: m.UTF8ToString(m._twin_version()) });
        audio = d.port;
        audio.onmessage = (a) => {
            if (a.data.input) live.push(a.data.input);
            if (a.data.played !== undefined)
            {
                underruns = a.data.underruns;
                while (sent - a.data.played < kAhead) Produce();
            }
        };
        return;
    }
    if (!m) return;
    if (d.cmd) Apply(d.cmd, d.args || []);
    if (d.source) source = d.source;
    if (d.file) { file = d.file; filePos = 0; }
    if (d.tone) Object.assign(tone, d.tone);
    if (d.output) output = d.output;
    if (d.record === true) recording = [];
    if (d.record === false && recording)
    {
        const wav = Wav(recording);
        recording = null;
        postMessage({ wav }, [wav]);
    }
    if (d.getLog) postMessage({ log: ScriptText() });
};
