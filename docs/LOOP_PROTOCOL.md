# Loop transfer: FRIZZ's loop banks over USB MIDI

Status: **specification, version 1.0** (#278, 2026-10-14), built in the firmware for v1.5
(`firmware/code/src/LoopTransfer.h`, PR for #278) and in frizz-web's Companion page. Both
sides follow this document. Where the code and this document disagree, one of them has a bug.
Where this document is unclear, settle it here first, then build.

The Companion page (`/companion/` on the website: frizz-web, Web MIDI in Chrome- and
Firefox-based browsers) manages FRIZZ's loop
banks directly over USB. It lists what's on the card, uploads loops, moves, swaps and
deletes them, and copies TAPE's cubbis into FRIZZ's slots. FRIZZ answers over a small SysEx
protocol of its own, which this document defines. The protocol is for players, through the
Companion, so it is stable and versioned. It is separate from the developers' SysEx (`0x3E`,
`firmware/README.md`), which may change with any build.

The Companion is for arranging loops and for single loops. A 16-bar loop takes a minute or more
over MIDI (section 6). For many loops at once, the card reader (SHIFT + LOOP, #196) is the
faster way.

Words used here:

- **app**: the side that asks. The Companion page, `remote.py`, or the reference client
  (section 8).
- **slot**: a place for a loop, numbered as the loop loader numbers them (`LoopLoader.h`):
  bank 0-7 are FRIZZ's banks a-h (`/FRIZZ/a1.wav` … `/FRIZZ/h15.wav`), bank 8-12 are TAPE's
  cubbi banks a-e (`cubbi_a1.wav` … `cubbi_e15.wav`, in `/TAPE` or else the card's root), and
  slot 0-14 is the white key 1-15. A **FRIZZ slot** is in bank 0-7, a **cubbi** is in bank 8-12.
- **job**: a write or a cubbi copy, the two operations that move a file's bytes. One runs at a
  time.

---

## 1. Transport

### 1.1 Where

USB MIDI only, cable 0. FRIZZ ignores this command when it arrives on the TRS jack. It works
the same on libDaisy's USB stack and on TinyUSB (#196): both feed the same MIDI parser
(libDaisy's `MidiHandler`), and every limit below comes from that parser or from FRIZZ's
own code, never from the stack.

**Finding the port.** FRIZZ's USB product string is "FrizZ" (`usbd_desc.c`), and its MIDI
port is named after it: `FrizZ MIDI 1` in ALSA (`amidi -l`). Each system dresses the name
differently, for example with a prefix "FrizZ:" in Chrome on Linux. "CHOMPI" appears only in
the name of the ALSA card (`/proc/asound/cards`, where `tools/midi_send.py` looks), and Web
MIDI doesn't show that. So the app:

1. tries the ports whose name contains "frizz" in any case first: their input and output
   together, with HELLO (2.1);
2. if none matches or answers, asks every input and output port with HELLO.

Only a port that answers HELLO is FRIZZ. A port named "CHOMPI" somewhere is found by step 2
only. A port that doesn't answer HELLO is not a FRIZZ that has this protocol.

### 1.2 Framing

```
request  F0 7D 43 48 38 OP SEQ ARGS… F7
answer   F0 7D 43 48 78 OP SEQ STATUS DATA… F7
```

- `7D 43 48` is the header of the multi-firmware launcher and of every FRIZZ SysEx (`7D`, the
  non-commercial manufacturer ID, then "CH"; `MidiControl.h`'s `kMidiHeader`).
- **`38` is this protocol's command** (`kCmdLoops` in the firmware, new). It is not `10` (the
  restart), not `11`-`14`, `20`-`24`, `30`-`31` (FRIZZ's others), and not `3E` (the developers').
  The answer is the command `| 0x40`, as for every FRIZZ command: `78`.
- `OP` says what is asked (section 3). The answer repeats it.
- `SEQ` 1-127 is chosen by the app, a new value for every new request (counting up, 127 wraps
  to 1). The answer repeats it, so the app can match its answer. `SEQ` 0 marks what FRIZZ sends
  without being asked: the acks of a write (3.7).
- `STATUS` is one byte (section 4). An answer with a status other than `OK` (0) carries
  exactly one more byte, `DETAIL` (4.2), and nothing else. The data listed for each command
  below is the data of an `OK` answer. The one exception is a write's ack (3.7), which always
  carries its `TID`, `NEXT` and `DROPS`.

### 1.3 Sizes

| What | v1 limit | Why |
|---|---|---|
| a request, the bytes between `F0` and `F7` | **128** | libDaisy's parser keeps 128 SysEx bytes (`SYSEX_BUFFER_LEN`, `MidiEvent.h`). A longer message is cut off without notice and still delivered at `F7`. |
| an answer, `F0` to `F7` | **45** | one USB transfer (15 USB-MIDI packets). That leaves 39 bytes after the `78`: `OP SEQ STATUS` plus up to 36 bytes of data. (`MidiControl::Reply` itself sends up to 51 bytes, 17 packets, for FRIZZ's other answers; this protocol's stay within 45.) |
| a CHUNK's data | **98 bytes** (HELLO's `maxChunk`) | 14 packed groups of 7 (112 bytes). The largest request is then 123 bytes: header 11 + 112. |
| CHUNKs in flight | **16** (HELLO's `window`) | decided with the supervisor; section 6 |

The app must take `maxChunk`, `window` and `maxRequest` from HELLO and never use more. A
later FRIZZ may raise them in a minor version, for example through a raw SysEx path for this
one command or after the MIDI queues shrink (`docs/CAPACITY.md`, lever 4), and the app then
goes faster without changing.

A CHUNK carries its own length (`LEN`). A message that the parser cut off doesn't match its
`LEN`, so FRIZZ drops it as a gap (3.7) instead of writing broken data.

### 1.4 Encoding

Every byte after `F0` is 7-bit (`00`-`7F`).

- **N(k)**: an unsigned number in k 7-bit groups, the highest first, as the developers' SysEx
  writes them. `N(4)` holds up to 268,435,455, `N(5)` 32 bits.
- **Packed bytes** (8-bit data: the file's bytes, a name): in groups of up to 7 bytes. Each
  group is one byte holding the top bits, then the group's bytes with their top bits cleared.
  Bit i of the top-bit byte is bit 7 of the group's byte i. n bytes take
  `n + ceil(n / 7)` bytes packed. For example, `FF 01 80` packs to `05 7F 01 00`.
- **Names**: UTF-8, packed. FRIZZ never shows a name, it only stores and reports it, so it
  treats the bytes as opaque (section 5.3).
- **Bank, slot, flags, counts**: one byte each, 0-127.

### 1.5 One request at a time

The app sends a request and waits for its answer before the next one. CHUNKs are the
exception: they stream within the window (3.7) and get no answer each. Two apps at once (two
browser tabs, a tab and `remote.py`) are not supported. The transfer ID (`TID`, 3.6) keeps a
second app's CHUNKs out of the first one's job. The second app's `WRITE_BEGIN` gets
`BUSY_JOB`.

**Repeats are safe.** FRIZZ remembers the last **changing** request it answered (`10`, `12`,
`13`, `20`-`23`, `30`: its `OP`, `SEQ` and the answer it gave). The same `OP` and `SEQ` again
get that answer again, and nothing is done twice. Read-only requests (`00`-`04`) and CHUNKs
don't touch that memory, so a `STATUS` poll between a lost answer and its repeat doesn't make
FRIZZ forget it. The passing refusals (`BUSY_…`, `CARD_LENT`, `NO_CARD`) aren't remembered:
they are tried again when they come again. A final refusal is remembered like an `OK`, so a
repeated `WRITE_END` after `BAD_CRC` gets `BAD_CRC` again, not `NO_TRANSFER`. So the app answers
a lost answer by sending the same request with the same `SEQ`, and a `BUSY_…` by sending it
again later with a new `SEQ`.

**HELLO clears that memory**, so a new visit can't get an earlier visit's answer by hitting the
same `OP` and `SEQ`. The app starts each connection with HELLO (2.1), begins its `SEQ` at a
random value, and never sends a new changing request with the `OP` and `SEQ` of its own last
one (after 127 requests the counter comes round again: it skips that value).

### 1.6 Timeouts

| Who | Waits for | How long | Then |
|---|---|---|---|
| app | an answer to a request | 500 ms | sends it again, same `SEQ`, up to 4 times |
| app | 5 tries without an answer | — | "FRIZZ doesn't answer": not FRIZZ, a FRIZZ without this protocol, or one that hangs |
| app | an ack that moves `NEXT` during a write | 500 ms | sends again from `NEXT` (3.7) |
| app | progress during a write, while no ack says paused | 10 s | `ABORT`, and reports the failure |
| app | a `BUSY_…` answer | 250 ms | asks again, new `SEQ` |
| FRIZZ | a CHUNK or `WRITE_END`, while a write is running (phase 1: not paused, not finishing) | **30 s** | aborts the write and deletes the temp file; the job's result is `TIMED_OUT` |
| FRIZZ | USB unconfigured (cable out, computer asleep) during a job | — | aborts it at once; result `ABORTED` |

A paused write (3.8) and one finishing don't count towards FRIZZ's 30 s. Nothing else keeps
a write alive: `STATUS` polls don't, so an app that is stuck can't hold a write open forever.

### 1.7 Web MIDI

- Access: `navigator.requestMIDIAccess({ sysex: true })`, which the browser asks the player to
  allow. Firefox asks through a site permission add-on that Mozilla generates for the site.
  Both browsers need a secure context (https).
- Send each message as one `output.send([...])` of one complete SysEx, `F0` to `F7`.
- The protocol's messages are small (at most 128 bytes) and at most 16 are in flight, far below
  every browser limit: Chrome's 10 MB cap on bytes in flight before it drops messages silently,
  Firefox's crash with SysEx over 64 KB on macOS (bug 1945967, since fixed), and Windows drivers
  that split long SysEx.
- Drive the write from the MIDI input's acks (`onmidimessage`), not from timers. Browsers
  slow timers in a hidden tab, down to once a minute after 5 minutes in Chrome. Input events
  still come at once, so an ack-driven pipeline keeps its pace. The app should still ask the
  player to keep the tab open, and can hold a Wake Lock.

---

## 2. Version and capabilities

### 2.1 HELLO

`OP 00`, no arguments. FRIZZ answers it in any state, and forgets the last changing request
it remembered (1.5). An open job stays open.

| Data | Bytes | v1 value |
|---|---|---|
| protocol major | 1 | 1 |
| protocol minor | 1 | 0 |
| FRIZZ version: major, minor, patch, pre-release | 4 | e.g. `01 05 00 01` for v1.5.0-beta.1; pre-release 0 for a release (`kFrizzVersion`, 7.1) |
| max frames of a loop | N(4) | 3,686,400 (16 bars at 50 BPM, 76.8 s at 48 kHz) |
| max file size, bytes | N(4) | 14,753,792 (the data's most, 14,745,600, plus the 8 KB the header walk reaches) |
| `maxChunk`: a CHUNK's most data bytes | 1 | 98 |
| `window`: CHUNKs in flight beyond `NEXT` | 1 | 16 |
| `maxRequest`: the most bytes between `F0` and `F7` | N(2) | 128 |
| `nameMax`: a name's most bytes | 1 | 28 |
| FRIZZ banks, cubbi banks, slots a bank | 3 | 8, 5, 15 |

That's 22 bytes of data.

**Versions.** A new minor version adds commands, or bytes at the end of an answer. The app
ignores bytes it doesn't know, and FRIZZ answers an unknown `OP` with `BAD`. A new major version
breaks something. The app only talks to the major version it was built for. Against another
one it tells the player which FRIZZ it needs, or that the page is older than the firmware.

### 2.2 `site/features.json`

FrizZ tells frizz-web what the firmware at a tag can do, so the site shows the Companion page only
where the release supports it. The file comes with the firmware, not before.

```json
{
  "loopTransfer": 1
}
```

- **Format:** one JSON object. Keys are feature names in camelCase, values are integers
  ≥ 0. A missing file, a missing key and 0 all mean "not in this release". `loopTransfer` is
  the protocol's major version that this release's FRIZZ speaks. Future features add keys.
  Nothing else goes in the file: no comments, no other types.
- **Where frizz-web reads it:** `scripts/fetch-docs.mjs`, as it reads `site/landing.md`, with
  `optionalSource(tag, 'site/features.json')` at the tag it builds from. That is the latest
  stable release for `/` and the pre-release for `/beta/`, or `FRIZZ_DIR` for a local checkout.
  A 404 means `{}`, anything else that fails fails the build, as for the other files. It writes
  the result to `src/data/firmware.json` (not `features.json`: `src/data/features.md` is the
  landing page's copy). The Companion page and its link in the navigation are built only where
  `loopTransfer` is a major version the app supports.
- **Checked by:** `doclinks.py`, once the firmware's PR adds the file: valid JSON, an object
  of non-negative integers, and `loopTransfer` equal to the protocol major in the firmware
  (`kLoopsProtocol`, `LoopTransfer.h`).
- The site's page and the player's FRIZZ can still differ: the player may run an older build.
  The site's flag only says the release has the feature. HELLO says whether the CHOMPI on the
  cable has it.

---

## 3. Commands

| OP | Name | Arguments | Changes the card |
|---|---|---|---|
| `00` | HELLO | — | no |
| `01` | STATUS | — | no |
| `02` | BANK | BANK | no |
| `03` | SLOT | BANK SLOT | no |
| `04` | NAME | BANK SLOT | no |
| `10` | WRITE_BEGIN | BANK SLOT FLAGS SIZE(4) | yes |
| `11` | CHUNK | TID OFFSET(4) LEN DATA… | yes |
| `12` | WRITE_END | TID CRC(5) | yes |
| `13` | ABORT | TID | yes |
| `20` | RENAME | BANK SLOT TOBANK TOSLOT FLAGS | yes |
| `21` | SWAP | BANK SLOT TOBANK TOSLOT | yes |
| `22` | DELETE | BANK SLOT | yes |
| `23` | COPY | CUBBIBANK SLOT TOBANK TOSLOT FLAGS | yes |
| `30` | RESCAN | — | no |

`FLAGS` bit 0, **`REPLACE`**: a file already in the destination slot is replaced. Without it,
the request is refused with `EXISTS`. The app sets it only once the player has confirmed the
replacement.

**What every changing command is refused with** (`10`, `20`-`23`, `30`; their own refusals
follow each), checked in this order:

| Status | When |
|---|---|
| `NO_CARD` | no card, or it doesn't mount |
| `CARD_LENT` | the card is lent to the computer as a drive (SHIFT + LOOP, #196) |
| `BUSY_RECORDING` | LOOP is recording |
| `BUSY_LOADING` | the loop loader is loading or checking a loop (a key picked, MIDI's program change) |
| `BUSY_SCANNING` | the loader is scanning the card, or counting its free space (5.4) |
| `BUSY_JOB` | a write or a copy is open, its own or another app's |
| `BUSY_CARD` | a scene save, the master settings or a bug report is writing the card right now (`SceneStore`, `EventLog`) |

Read-only commands (`00`-`04`) are answered in every state, from what FRIZZ holds in RAM.

### 3.1 STATUS

`OP 01`. Where FRIZZ stands, and the job's progress and result. The app polls it during a job
(every 250 ms) and after a timeout.

| Data | Bytes | |
|---|---|---|
| flags | 1 | bit 0 card mounted, 1 card lent, 2 LOOP recording, 3 loader loading or checking, 4 scanning, 5 card busy (a save, a report), 6 free space known |
| more flags | 1 | bit 0 the cubbis are in `/TAPE` (else the root) |
| free space, KB | N(4) | 0 while unknown |
| job | 1 | 0 none, 1 write, 2 copy |
| phase | 1 | 0 none, 1 running, 2 paused by FRIZZ, 3 finishing, 4 done, 5 failed |
| TID | 1 | the job's |
| bank, slot | 2 | its destination |
| done | N(4) | bytes received (a write: `NEXT`) or copied |
| total | N(4) | the file's size |
| result | 1 | when done or failed: its status (`OK` when done) |
| detail | 1 | its `DETAIL` |

That's 21 bytes of data. A job's result stays until the next `WRITE_BEGIN` or `COPY`, so an
app that lost the answer to `WRITE_END` reads the outcome here.

### 3.2 BANK

`OP 02 BANK`, BANK 0-12. The loader's results for the bank's 15 slots, as the last scan
or a later change left them.

| Data | Bytes |
|---|---|
| BANK | 1 |
| scanning (1 while a scan runs: the results may still change) | 1 |
| the 15 slots' results (5.1) | 15 |

Refused: `BAD` for a bank over 12.

### 3.3 SLOT

`OP 03 BANK SLOT`. One slot.

| Data | Bytes | |
|---|---|---|
| BANK, SLOT | 2 | |
| result | 1 | 5.1 |
| frames | N(4) | 0 unless mono or stereo; TOO_LONG's own count too |
| beats | N(2) | the `acid` chunk's, 0 without one |
| file size, bytes | N(4) | 0 for an empty slot |
| name length, bytes | 1 | 0 without a name |
| flags | 1 | bit 0 the name was cut (the file's is longer than `nameMax`), 1 the looper plays this slot's loop, 2 it's loaded into a buffer and ready |

Refused: `BAD` for a bank over 12 or a slot over 14.

A listing is BANK for each of the 13 banks, then SLOT and NAME for every slot that isn't
empty, then STATUS for the free space. At ~2 ms an answer, a full card takes about half a
second.

### 3.4 NAME

`OP 04 BANK SLOT`. The slot's name, read from the file's `LIST/INFO/INAM` chunk (5.3).

| Data | Bytes |
|---|---|
| BANK, SLOT | 2 |
| LEN, the name's bytes (0-28) | 1 |
| the name, packed | LEN + ceil(LEN / 7), at most 32 |

That's at most 35 bytes of data. Cubbis have a name only if someone wrote one into the file:
TAPE doesn't.

### 3.5 RESCAN

`OP 30`. FRIZZ scans the card again, as at boot: both folders, every slot's header and name,
and the free space if FSINFO doesn't have it (5.4). It also repairs a swap that a power cut
interrupted (5.6). The answer comes at once. `STATUS` flag 4 and `BANK`'s scanning byte say
when the scan is done: the app waits for that rather than for a fixed time. The app sends it at the end of a
batch: the user decided in #278 that FRIZZ rescans after a batch. Every changing command
already updates the slots it touched (3.7, 3.9-3.11), so the listing is right without it.
The rescan also picks up anything changed on the card by other means.

### 3.6 WRITE_BEGIN

`OP 10 BANK SLOT FLAGS SIZE(4)`. Opens a write of SIZE bytes, a whole WAV file (section 5),
into FRIZZ slot BANK SLOT.

FRIZZ checks, after the common refusals:

| Status | When |
|---|---|
| `NOT_A_SLOT` | BANK over 7 (a cubbi: TAPE's files are never written) or SLOT over 14 |
| `EXISTS` | the slot holds a file and `REPLACE` isn't set |
| `TOO_LONG` | SIZE over HELLO's max file size |
| `BAD` | SIZE under 44 (no WAV is that short) |
| `NO_SPACE` | SIZE plus 64 KB more than the card's free space (the old file stays until the end, so it doesn't count as free) |

`OK` answers with:

| Data | Bytes |
|---|---|
| TID: this job's ID, 1-127, a new one each time | 1 |
| `maxChunk`, `window` for this job (the same as HELLO's in v1) | 2 |

FRIZZ deletes a leftover `/FRIZZ/xfer.tmp` and creates it anew (`FA_CREATE_ALWAYS`). `NEXT`,
the next byte FRIZZ expects, is 0.

### 3.7 CHUNK and the acks

```
CHUNK  F0 7D 43 48 38 11 TID OFFSET(4) LEN DATA(packed) F7
ACK    F0 7D 43 48 78 11 00 STATUS TID NEXT(4) DROPS F7
```

A CHUNK has no `SEQ` (its `OFFSET` does that job) and gets no answer of its own. LEN is
1-`maxChunk` and DATA is the file's bytes OFFSET … OFFSET + LEN − 1, packed. Every CHUNK but
the file's last carries `maxChunk` bytes.

**FRIZZ takes a CHUNK** in the audio callback, into a ring that holds `window` CHUNKs, only if
it all fits: the TID is the open write's, OFFSET is the end of what it has taken so far, the
packed length matches LEN, LEN ≤ `maxChunk`, OFFSET + LEN ≤ SIZE, the ring has room, and the
write isn't paused. MainLoop takes the CHUNKs out of the ring, in order, into its staging
buffer. **`NEXT` is the end of what MainLoop has taken out of the ring.** Bytes below `NEXT`
are FRIZZ's to write: they stay in its RAM, in order, until they're on the card, and are lost
only if the whole write fails. Since `NEXT` never counts what still sits in the ring, an app
that keeps to the window never overfills it. Otherwise:

- OFFSET below the end of what was taken: a repeat. Not taken again and not counted, but it
  is acked (at most once a pass), so a lost ack is made good by the app's next repeat.
- OFFSET beyond it, a length that doesn't match, a full ring, a paused write: a **gap**. The
  CHUNK is dropped and `DROPS` counts it.
- an unknown TID: ignored, with an ack of `NO_TRANSFER` at most every 250 ms, so a stale app
  stops. That ack carries the CHUNK's TID, `NEXT` 0 and `DROPS` 0. The app then reads `STATUS`
  for how its job ended (it may have ended while an answer was lost, e.g. `WRITE_BEGIN`'s) and
  acts on that result.
- a CHUNK too short to have its `LEN` (fewer than 6 bytes after `11`): ignored.

FRIZZ doesn't check that every CHUNK but the last carries `maxChunk` bytes: a shorter one at the
right `OFFSET` is taken. The app keeps to it, so its offsets stay `maxChunk` apart.

**FRIZZ acks** (`SEQ` 0, `NEXT` and `DROPS` as they stand) once in every pass of its UI
(~2 ms) in which `NEXT` or `DROPS` changed or a repeat came, every 250 ms while paused (the
first as the pause begins), once in the pass in which a pause ends (an `OK` ack, so the app
starts sending again without waiting for its `STATUS` poll; the app also takes `STATUS` phase 1
as the pause's end, in case that ack is lost), and once when FRIZZ ends the write itself (its
status, below). At most one ack goes in a pass. It doesn't take the place of FRIZZ's one
answer a pass: the ack goes first, then the pass's answer (this protocol's, or another of
FRIZZ's), both in the same pass, so a `STATUS` poll is answered while the acks stream. `DROPS` (0-127,
wrapping, 0 at `WRITE_BEGIN`) goes up by one for every CHUNK dropped as a gap. Because it is a
count and not a flag, a lost ack loses nothing: the next one still shows that it changed. An
ack's `STATUS`:

- `OK`: running.
- `BUSY_RECORDING`, `BUSY_LOADING`, `BUSY_CARD`: paused (3.8). The app stops sending and
  waits for an `OK` ack, polling `STATUS` meanwhile. While paused FRIZZ takes nothing out of
  its ring either, so `NEXT` stands still.
- anything else ends the write: `NO_TRANSFER`, `NO_SPACE`, `CARD_ERROR`, `CARD_LENT`,
  `TIMED_OUT`, `ABORTED`. FRIZZ has deleted the temp file, and `STATUS` keeps the result.

**The app sends go-back-N.** It keeps:

- `acked`, the highest `NEXT` an ack gave;
- `sent`, the end of what it has sent;
- `drops`, the last `DROPS` it saw (0 at `WRITE_BEGIN`);
- `rewound`, the offset it last went back to, or none.

It sends while `sent < SIZE` and `sent − acked < window × maxChunk`. On each ack:

1. `acked = max(acked, NEXT)`. Once `acked` has moved past `rewound`, `rewound` is none again.
2. If `DROPS` ≠ `drops`: `drops = DROPS`, and if `rewound` is none, the app sets
   `sent = rewound = acked` and sends again from there. If `rewound` is set, the new drops are
   the CHUNKs sent before that rewind, still arriving behind the gap, and they don't call for
   another rewind.
3. No ack moving `acked` for 500 ms while no ack says paused: the app first asks `STATUS`
   and sets `acked = max(acked, done)`, since the ack that would have moved it may be the one
   lost (the window's last, or the file's end). Then, and on an `OK` ack after a pause,
   `sent = rewound = acked`, and the app sends again from there (if `acked` is SIZE, it sends
   `WRITE_END`). That is also how it starts after a pause, since FRIZZ dropped what came
   during it.

The progress bar follows `acked`, in bytes.

The CRC-32 comes from the bytes MainLoop took, in order, as they come, so `WRITE_END` needn't
read the file back.

### 3.8 Pauses during a job

A job is never refused halfway because of something the player does. FRIZZ pauses it:

- **LOOP starts recording**: paused until the recording closes.
- **The player picks a loop**, which the loader loads: paused until the load ends. A load's
  DMA must have the card to itself (`LoopLoader.h`). The player's loads go first.
- **A scene save, the master settings or a bug report** writes the card: paused for that
  write, usually milliseconds. These remount the card. FRIZZ then opens `xfer.tmp` again and
  goes on at its end, as the loader does after a remount (`Reopen`).
- **The card is lent** (SHIFT + LOOP): the player's gesture wins. The job ends with
  `CARD_LENT`, the temp file deleted, before the card goes to the computer. A job already at
  step 5 of 3.9 (the rename, which `ABORT` can't stop either, 3.10) finishes first, within
  milliseconds, and the card is lent after it; the job's result is then `OK`. The same holds
  when USB goes away (1.6).

While paused, FRIZZ takes no CHUNKs and its 30 s timeout stands still. The app waits as long
as the pause lasts. A recording can take 77 s or more, so the app tells the player why it
waits.

### 3.9 WRITE_END

`OP 12 TID CRC(5)`. CRC is zlib's CRC-32 of the whole file, all SIZE bytes.

FRIZZ checks right away:

| Status | When |
|---|---|
| `NO_TRANSFER` | TID isn't the open write (it timed out, was aborted, or never opened) |
| `INCOMPLETE` | `NEXT` < SIZE: the app sends the rest, then `WRITE_END` again (new `SEQ`) |
| `BAD_CRC` | the CRC of what came differs. The write ends, the temp file is deleted, and the app sends the file again from `WRITE_BEGIN` |

`OK` means FRIZZ is finishing (`STATUS` phase 3). It finishes a step a pass, in MainLoop:

1. writes what's left in RAM, closes `xfer.tmp`;
2. walks its header as the scan does (`WavWalk`). It must be a loop FRIZZ plays (mono or
   stereo), else `BAD_WAV` with the walk's result as `DETAIL`;
3. follows its clusters as a load does (`MapStep`). More fragments than the loader's map holds
   (255) is `FRAGMENTED`: the card is too fragmented for that loop. FRIZZ could never load it,
   so it isn't kept;
4. checks the slot again. A file there now (put there by RENAME, a hand on the card) without
   `REPLACE` is `EXISTS`. With `REPLACE`, the old file is deleted;
5. renames `xfer.tmp` to the slot's name, `/FRIZZ/<bank><key>.wav` in lower case;
6. updates the slot (result, frames, beats, size, name), forgets every copy of that slot in
   RAM (5.5), and the phase becomes done (4).

Steps 1, 2 and 3 take a pass each (step 3 one per 256 clusters), steps 4 to 6 one pass
together: once the old file is deleted, nothing (no `ABORT`) comes between it and the rename.

A failure in steps 2-5 deletes `xfer.tmp`, leaves the slot as it was, and sets phase 5 with
the status. The app polls `STATUS` until the phase is 4 or 5. A FatFs call that fails at any
point is `CARD_ERROR` with FatFs's `FRESULT` as `DETAIL`.

### 3.10 ABORT

`OP 13 TID`. Ends the job (a write or a copy) at once and deletes `xfer.tmp`. The slot stays as
it was. Its result is `ABORTED`. `NO_TRANSFER` if TID isn't the open job. The app sends it when
the player cancels or when it gives up. Once a job reaches step 5 of 3.9, it can no longer be
aborted, and `ABORT` gets `NO_TRANSFER` once the job is done.

### 3.11 RENAME, SWAP, DELETE

Card renames and deletes in `/FRIZZ`, no data over USB. FRIZZ carries each out in the pass that
takes it, a few FatFs calls, and answers when it's done. Each updates the slots it touched and
forgets their copies in RAM (5.5).

- **RENAME** `OP 20 BANK SLOT TOBANK TOSLOT FLAGS`: moves a FRIZZ slot's file to another FRIZZ
  slot. Refused: `NOT_A_SLOT` (either side not a FRIZZ slot), `BAD` (the same slot twice),
  `EMPTY` (nothing to move), `EXISTS` (the target is taken, no `REPLACE`; with it, the target's
  file is deleted first).
- **SWAP** `OP 21 BANK SLOT TOBANK TOSLOT`: two FRIZZ slots trade files. One of them may be
  empty, in which case it's a move. Refused: `NOT_A_SLOT`, `BAD` (the same slot twice), `EMPTY`
  (both empty). Three renames through a temp name that says where its file belongs (5.6).
- **DELETE** `OP 22 BANK SLOT`: deletes a FRIZZ slot's file. Refused: `NOT_A_SLOT`, `EMPTY`
  (already empty; the app counts that as done).

### 3.12 COPY

`OP 23 CUBBIBANK SLOT TOBANK TOSLOT FLAGS`: copies TAPE's cubbi (CUBBIBANK 8-12) byte for byte
into a FRIZZ slot, on the card itself. TAPE's file is only read, never changed.

Refused, after the common refusals:

| Status | When |
|---|---|
| `NOT_A_SLOT` | the source isn't a cubbi, or the target isn't a FRIZZ slot |
| `EMPTY` | no such cubbi |
| `TOO_LONG` | the cubbi is longer than 16 bars at 50 BPM (76.8 s; TAPE's cubbis go to 2:45, #263) |
| `BAD_WAV` | the scan found no loop FRIZZ plays (`DETAIL`: its result) |
| `EXISTS` | the target is taken, no `REPLACE` |
| `NO_SPACE` | as for a write |

`OK` answers with the job's TID. FRIZZ copies through `/FRIZZ/xfer.tmp`, 32 KB a pass. It
pauses as a write does (3.8), and finishes with steps 2-6 of 3.9. `ABORT` stops it. `STATUS`
shows its progress (job 2) and its result. A 14.7 MB cubbi takes a few seconds.

---

## 4. Status codes

### 4.1 STATUS

Numbers are fixed from v1 on. New ones are only appended.

| # | Name | Meaning |
|---|---|---|
| `00` | OK | done, or running |
| `01` | BAD | a malformed request: unknown OP, missing or extra arguments, out of range |
| `02` | NOT_HERE | not in this build (FRIZZ-bench.bin) |
| `03` | BUSY_RECORDING | LOOP is recording |
| `04` | BUSY_LOADING | the loader is loading or checking a loop |
| `05` | BUSY_SCANNING | a scan, or the free space being counted |
| `06` | BUSY_CARD | a save or a bug report is writing the card |
| `07` | BUSY_JOB | a write or a copy is open |
| `08` | CARD_LENT | the card is lent to the computer |
| `09` | NO_CARD | no card, or it doesn't mount |
| `0A` | NOT_A_SLOT | not a FRIZZ slot (or, for COPY's source, not a cubbi) |
| `0B` | EMPTY | nothing in that slot |
| `0C` | EXISTS | the target slot holds a file, and `REPLACE` isn't set |
| `0D` | NO_SPACE | not enough room on the card |
| `0E` | TOO_LONG | over the max file size or the max frames |
| `0F` | BAD_WAV | not a loop FRIZZ plays (`DETAIL`: the loader's result, 5.1) |
| `10` | BAD_CRC | the CRC-32 differs |
| `11` | INCOMPLETE | `WRITE_END` before every byte came |
| `12` | NO_TRANSFER | no job with that TID (it ended, timed out or never began) |
| `13` | FRAGMENTED | the file lies in more pieces than the loader can load |
| `14` | CARD_ERROR | a FatFs call failed (`DETAIL`: its `FRESULT`) |
| `15` | TIMED_OUT | FRIZZ ended the job: no CHUNK for 30 s |
| `16` | ABORTED | ended by ABORT, or USB went away |

The `BUSY_…` statuses, `CARD_LENT` and `NO_CARD` are passing states (as in 1.5): ask again
later. The app shows them
in words ("FRIZZ is recording a loop"). The rest are final for that request.

### 4.2 DETAIL

One byte after a status other than OK, 0 if there's nothing more to say: the loader's result
(5.1) for `BAD_WAV`, FatFs's `FRESULT` for `CARD_ERROR`.

---

## 5. Files

### 5.1 The loader's results

`BANK`, `SLOT` and `BAD_WAV`'s `DETAIL` give a slot's state as `LoopLoader.h`'s `Result`. These
numbers become part of the protocol here. The firmware only appends to them.

| # | Result | | # | Result |
|---|---|---|---|---|
| 0 | empty | | 10 | goes past the file's end |
| 1 | mono, a loop | | 11 | longer than 16 bars at 50 BPM |
| 2 | stereo, a loop | | 12 | unreadable |
| 3 | not RIFF/WAVE | | 13 | in too many fragments |
| 4 | no fmt chunk before the data | | 14 | card error |
| 5 | not PCM | | 15 | timeout |
| 6 | not 16-bit | | 16 | card lost |
| 7 | not 48 kHz | | 17 | its clusters end before its data |
| 8 | not mono or stereo | | 18, 19 | the developers' write test's |
| 9 | no data chunk near the start | | | |

### 5.2 The WAV the app writes

The app converts any file in the browser (WAV, AIFF, MP3 …) to exactly this layout. FRIZZ's
loader reads any WAV within its limits, but the app writes only this one:

| Offset | Chunk | Bytes | Content |
|---|---|---|---|
| 0 | `RIFF` | 12 | `"RIFF"`, file size − 8 (LE32), `"WAVE"` |
| 12 | `fmt ` | 8 + 16 | format 1 (PCM), channels 1 or 2, 48,000 Hz, bytes a second, block align (2 × channels), 16 bits |
| 36 | `acid` | 8 + 24 | the beats (below); left out when the app doesn't know them |
| 68 | `LIST` | 8 + 4 + 8 + name | `"INFO"`, then the `INAM` chunk (5.3); left out without a name |
| … | `data` | 8 + frames × 2 × channels | 16-bit signed little-endian PCM, interleaved L R in stereo |

- **48 kHz, 16-bit, PCM, mono or stereo**, at most **3,686,400 frames** (HELLO's max frames:
  16 bars at 50 BPM, 76.8 s). The app resamples, dithers to 16 bits, and keeps a mono source
  mono (half the bytes, half the time on the cable). FRIZZ spreads it to stereo when it loads.
- **Every chunk before `data`**, all within the first 1 KB (the app's whole header is under
  150 bytes). FRIZZ's header walk reads at most 8 windows of 1 KB, never past `data`. A name
  or beats after the data are never seen.
- **`acid`**, 24 bytes, ACID's layout (Sony ACID, also written by Ableton and many sample
  packs):

  | Offset in the chunk's data | Field | The app writes |
  |---|---|---|
  | 0 | flags, LE32 | 0 (a loop: not one-shot, no root note, no stretch) |
  | 4 | root note, LE16 | 60 |
  | 6 | unknown, LE16 | 0x8000 |
  | 8 | unknown, float32 | 0 |
  | 12 | **beats**, LE32 | the loop's length in beats, 1-1024 |
  | 16 | meter denominator, LE16 | 4 |
  | 18 | meter numerator, LE16 | 4 |
  | 20 | tempo, float32 BPM | beats × 60 / seconds |

  FRIZZ reads only the beats (`WavWalk`: a chunk of 16 bytes or more, a value over 1024 is
  ignored). With beats, FRIZZ knows the loop's tempo. Without them, it takes the power of two
  of beats nearest the running tempo (#70). The other fields are there for the other software
  that reads the file.
- **Sizes** in the chunk headers are exact, and odd-length chunks get a pad byte (RIFF's rule).
  Only `INAM` can have an odd length, and the app pads it. FRIZZ checks every length against
  the file's size (`WavWalk`), and a file that lies about one is refused, never read past its
  end.

### 5.3 The name: `LIST/INFO/INAM`

- **Encoding:** UTF-8, at most **28 bytes** (HELLO's `nameMax`) plus a terminating NUL. The
  app cuts a longer name at a code point's boundary, never inside a character. The name is
  the source file's name without its extension, as the player sees it in the app. The app
  may let the player edit it before the upload.
- **The bytes** for a name of n bytes (1-28):

  | Offset in `LIST` | Bytes | Content |
  |---|---|---|
  | 0 | 4 | `"LIST"` |
  | 4 | 4 | LE32: 4 + 8 + (n + 1) + pad |
  | 8 | 4 | `"INFO"` |
  | 12 | 4 | `"INAM"` |
  | 16 | 4 | LE32: n + 1 (the name and its NUL; the pad isn't counted) |
  | 20 | n + 1 | the name, then `00` |
  | 21 + n | pad | one `00` when n + 1 is odd, else nothing |

  So `INAM`'s size is the name's bytes plus 1, a pad byte follows when that's odd, and `LIST`'s
  size counts the pad. For "Amen" (n = 4): `INAM` size 5, one pad byte, `LIST` size 18, 26
  bytes in all.
- **Why 28:** NAME's answer has to fit one reply: 28 bytes pack into 32.
- **What FRIZZ learns** (new in `WavWalk`): a `LIST` chunk whose first 4 bytes are `"INFO"`,
  before `data`, is walked for `INAM`. Its bytes up to the first NUL or the chunk's end are
  the name. Over 28 bytes, FRIZZ keeps the first 28, cut back to a UTF-8 boundary, and sets
  SLOT's flag 0. FRIZZ reads the name only where the whole `LIST` chunk lies within the window
  the walk has. A file from elsewhere with a huge `LIST` then shows no name, but it isn't
  refused for it. Any other chunk, or a `LIST` of another type (`adtl`), is skipped as it is
  today.
- FRIZZ keeps the names with the scan's results, in SDRAM (`loops::Mem`): 13 × 15 × 29 bytes,
  ~5.7 KB. The cubbis get theirs the same way, though TAPE doesn't write any.
- Renaming a loop's name without uploading it again isn't part of v1. It would mean
  rewriting the file's header in place.

### 5.4 Free space

`STATUS` reports the card's free space in KB: free clusters × cluster size. FatFs takes the
count from the card's FSINFO sector (`_FS_NOFSINFO` 0). Where FSINFO has none (a card a
computer left without it), `f_getfree` would scan the whole FAT in one call: seconds on a
large card, MainLoop held up. FRIZZ instead counts the free clusters during the scan, a FAT
sector a pass, and reports "unknown" (flag 6 clear, 0 KB) until then. Changing commands get
`BUSY_SCANNING` meanwhile.

### 5.5 What the player hears

**A playing loop plays on.** The looper plays from SDRAM, never from the card. Writing,
replacing, renaming or deleting its file changes nothing that plays.

- **The loops in RAM:** the loader keeps the loop it last loaded, and remembers which slot each
  buffer holds, so going back needs no load (`LoopControls.h`). After any change to a slot,
  FRIZZ forgets that it holds that slot, except for the loop playing, which plays on as it
  was. Picking that key again loads what the slot holds now.
- **A planned switch** (SHIFT + a key, MIDI's program change) to a slot that changes loads the
  slot's new file when its time comes. If the slot became empty, the plan is cancelled as if
  the player had cancelled it.
- **The Loops page's keys** show the slots as the scan and the changes leave them: a slot that
  becomes empty goes dark, a new one lights in its bank's colour.
- Nothing about the protocol is in the audio callback beyond copying a CHUNK's bytes into a
  ring (6.3). All card work runs in MainLoop, a step a pass, as the loader's and SceneStore's
  do.

### 5.6 Safety

- **Numbers, never paths.** Every request names slots by bank and slot number. FRIZZ builds the
  path itself (`SlotPath`), so no request can name another file.
- **The only names FRIZZ creates, writes, renames or deletes** through this protocol:
  - `/FRIZZ/a1.wav` … `/FRIZZ/h15.wav`, the FRIZZ slots (`[a-h]` and `1`-`15` without a leading
    zero; FAT names ignore case, so an existing `A1.WAV` is the same file);
  - `/FRIZZ/xfer.tmp`, the job's temp file;
  - `/FRIZZ/<from>-<to>.swp`, a swap's temp file, e.g. `a1-b2.swp`: a1's file on its way to b2.

  None of these temp names matches `SlotOf` (it needs a name that ends exactly in `.wav`), so
  a scan never takes them for loops. TAPE's files (`/TAPE`, the root) are only ever opened for
  reading. So are `/FRIZZ`'s other files: the scenes, the settings, the bug reports. A missing
  `/FRIZZ` is created, as at boot (`EnterFrizzDir`).
- **No half loop.** A slot only ever holds a complete file that passed the CRC, the header walk
  and the fragment check, renamed into place in one step. A broken connection, a closed tab, a
  timeout or an abort leaves `xfer.tmp`, which FRIZZ deletes:
  - when the job fails or is aborted;
  - at the next `WRITE_BEGIN` or `COPY`;
  - **at boot** (after a power cut mid-job), before the first scan.
- **Replacing** needs the old file deleted before the rename (FatFs's `f_rename` doesn't
  replace). A power cut in the milliseconds between the two leaves the slot empty and the new
  loop complete in `xfer.tmp`, which the boot then deletes. The player loses that slot's old
  loop and the new one, and uploads it again. The app still has the source. No slot ever holds
  half a file.
- **A swap** is three renames: A → `A-B.swp`, B → A, `A-B.swp` → B. A power cut between them
  leaves a `.swp` file, which holds a loop, so FRIZZ never deletes one. Each scan (at boot,
  RESCAN) finishes or undoes it, by the name: a `/FRIZZ/A-B.swp` goes to B if B is free (the
  swap finishes), else back to A if A is free (the swap is undone, before its second step).
  If both are taken, it stays, and the scan ignores it.
- **The card stays FRIZZ's.** A job never runs while the card is lent, and lending ends a job
  first (3.8). The loader's rule holds: no FatFs call while a load's DMA run is in flight.

---

## 6. Throughput

### 6.1 What a CHUNK costs

A CHUNK of 98 bytes is 125 bytes of SysEx (`F0`, 11 bytes of header, 112 packed, `F7`). USB
MIDI carries SysEx 3 bytes per 4-byte packet: 42 packets, 168 bytes on the bus, 2.6 of USB's
64-byte transfers. That's **37 bytes of the file per 64 bytes on the bus**.

### 6.2 The estimate

Three limits; the smallest wins.

1. **The host's USB MIDI driver**: how many 64-byte transfers it sends a millisecond. Full
   speed allows up to ~19, but MIDI class drivers send far fewer, and it differs by system.
   This is the unknown that stage 2 measures:

   | Transfers a ms | File bytes a second | 4 MB | 14.7 MB (16 bars at 50 BPM, stereo) |
   |---|---|---|---|
   | 1 | 37 KB/s | 1 min 47 s | 6 min 35 s |
   | 2 | 75 KB/s | 54 s | 3 min 17 s |
   | 4 | 149 KB/s | 27 s | 1 min 39 s |
   | 8 | 299 KB/s | 13 s | 49 s |

2. **The window**: at most 16 × 98 = 1,568 bytes per round trip. The round trip is the app's
   send, FRIZZ's pass (~2 ms) and the ack back through the browser: ~5 ms on a quick system
   gives ~310 KB/s, ~10 ms gives ~155 KB/s.
3. **FRIZZ**: the TinyUSB build reads its MIDI FIFO (64 bytes) in a task at 4 kHz, at most
   256 KB/s of USB MIDI, ~150 KB/s of file (#196's PoC, `tusb_config.h`). libDaisy's stack
   takes every packet in its interrupt, with no limit of its own short of its queues. The
   card writes several MB/s, and the CRC costs ~1 % of MainLoop at these rates.

**Expected: 4 MB in 30 s to 2 min, a full 14.7 MB loop in 1.5 to 7 min**, the faster end on
Linux, the slower one on a driver that sends one transfer a millisecond. A mono loop takes half.
The progress bar goes by bytes.

### 6.3 Tuning the rate

The user decided this order (2026-10-14). Neither step changes the protocol, because HELLO
reports `maxChunk`, `window` and `maxRequest` and the app follows them.

1. **Measure first.** The firmware's stage 2 measures the real rate on the device:
   - through Web MIDI in Chrome and in Firefox;
   - through ALSA directly (`loopxfer.py` on the raw MIDI node), which shows what the browser
     adds.

   For each round trip it records three times: the ack's round trip (a CHUNK sent to the ack
   that covers it, on the app's side), the card's write (MainLoop's time per 32 KB block), and
   the parse (the audio callback's time per CHUNK). The PR gives the rates for 4 MB and
   14.7 MB next to 6.2's estimate.
2. **If the rate is at the low end of the estimate, the window first**: `window` from 16 up to
   64. That helps wherever the round trip is the limit. It costs 128 bytes of ring per CHUNK
   (64 × 128 = 8 KB, outside `SRAM`) and stays far below libDaisy's queue of 256 events. On
   TinyUSB, the MIDI receive FIFO grows with it (`CFG_TUD_MIDI_RX_BUFSIZE`, 64 → 256 bytes,
   ~200 B more of RAM), or a task pass can't take more than one USB transfer.
3. **Then, if still needed, a bigger SysEx buffer** for bigger CHUNKs (`maxChunk`,
   `maxRequest`). Its RAM cost:
   - **`SYSEX_BUFFER_LEN` from 128 to 256** costs 128 bytes per queued `MidiEvent`. With
     libDaisy's queues as they are (256 events, two transports) that's +64 KB of `SRAM`, which
     isn't there (~19 KB free, `CAPACITY.md`). It's affordable only together with lever 4
     there: queues of 32 events with 256-byte buffers take ~18 KB for both transports instead
     of today's 74 KB, 56 KB less than now.
   - **A raw path for command `38` alone**, which takes its bytes from the USB stream before
     libDaisy's parser, costs one buffer of the chunk's size (~0.3 KB) plus its code. It is a
     change to vendored code (`THIRD_PARTY.md`).

   Bigger CHUNKs only shrink the header's share (11 of 123 bytes today) and the number of
   acks, so this comes last.

**The Companion is for arranging loops and for single loops.** For many loops at once, the card
reader (SHIFT + LOOP, #196) is the faster way: the computer writes `/FRIZZ` directly at
~700-790 KB/s, 5 to 20 times this protocol's rate. The app says so when a batch is large
(over ~50 MB) and RESCANs once the card is back. The MANUAL will say the same when the feature
ships.

No compression: audio barely compresses losslessly, and FRIZZ's CPU budget belongs to the
audio callback.

### 6.4 The device's side

- **The audio callback** (where MIDI is parsed, `MidiClock::Process`) only checks a CHUNK's
  header and copies its packed bytes, at most 123 of them, into a ring of `window` slots
  (16 × 128 bytes = 2 KB, outside the tight `SRAM`: `RAM_D2CACHE`, `RAM_D3` or SDRAM). A full
  ring is a gap (3.7). No EventLog line per CHUNK: the log keeps a job's begin, end and result
  only.
- **MainLoop** unpacks the ring into a staging buffer in SDRAM, updates the CRC, and writes
  whole 32 KB blocks to `xfer.tmp` (the last one shorter), as the loader's and SceneStore's
  card work does, a step a pass, giving the card to the loader's DMA when it wants it.
- **Acks** go in the one-a-pass answer's pass (`NormalPage::Remote`), before it, without taking
  its place: one ack and one answer at most a pass.

---

## 7. Firmware notes

For the worker who builds it. These aren't part of the contract, but they follow from it.

### 7.1 `kFrizzVersion`

HELLO reports FRIZZ's version from a constant in the source, `kFrizzVersion` (major, minor,
patch, pre-release: 0 for a release, N for `-beta.N`). It can't come from the git tag at build
time: the same commit would build to another md5 before and after it's tagged, and a build's
md5 names it (CLAUDE.md). So the commit before a tag sets it, when a beta or a release is cut.

A check holds the two together: `tools/builds.py` (and `flash.py --slot 10`/`--slot 13`, which
send only a clean `v*` tag's build) compares the tag at `HEAD` with `kFrizzVersion` and refuses a
build whose constant doesn't match its tag. A commit without a tag (a branch, `main`) may carry
any value, normally the next version's. Whether CLAUDE.md's release steps mention it is the
supervisor's decision.

### 7.2 Where it plugs in

As built (`LoopTransfer.h`):

- `MidiControl::SysEx`: a `case kCmdLoops` (0x38), USB only, not logged. It hands the bytes to
  `loopxfer::Intake`, the audio callback's half: a CHUNK into the ring (6.4, in SDRAM), any
  other request into a one-request slot of its own, apart from the developers' queue and the
  queries' slot.
- `LoopTransfer` in MainLoop after the loader, while the card is lent too (it refuses then):
  the requests, the job's state, the temp file, the steps of 3.9 and 3.12, the free space's
  count (5.4), the acks. It reuses the loader's `WavWalk` (`LoopLoader::Walk`) and `SlotPath`,
  follows the clusters as `MapStep` does, and waits for the loader (a load pauses it). Its
  messages go out from the play page's one answer a pass (`MidiControl::xfer_send`).
  `LoopTransfer::Boot` deletes the temp file before the loader's scan; the scan puts a swap's
  temp file right (`LoopLoader::Repair`). `CardLendProcess` calls `LoopTransfer::Lend` before
  FatFs lets go of the card.
- `WavWalk` learns `LIST/INFO/INAM` (5.3). `loops::Slot` gets the name, its length, the file's
  size.
- `LoopControls::Forget` (5.5), `LoopControls::Flags` for SLOT's flags 1 and 2.
- `crash::Crc32More`: the CRC carried on between steps (the same table).
- The protocol's version: `loopxfer::kLoopsProtocol` (major) and `kLoopsProtocolMinor`;
  FRIZZ's: `kFrizzVersion` in `Version.h` (7.1).
- Code room: the header is built `-Os`. `FRIZZ.bin` took ~7 KB of `SRAM_EXEC` (~3.2 KB left
  after it, `docs/CAPACITY.md`) and 0.3 KB of `SRAM`; the ring, the staging block, the job's
  files and the names are in SDRAM.
- `FRIZZ-bench.bin` answers `NOT_HERE` to every OP but HELLO (`loopxfer::BenchAnswer`).

---

## 8. Tests

### 8.1 The reference: `firmware/tools/loopxfer.py`

The protocol's executable specification, in Python 3 with no packages, like `remote.py`:

- **the codec**: N(k), packing, every request and answer, built and parsed;
- **a model FRIZZ**: the protocol's state machine (sections 1.5-5.6) over a card in a dict
  (`{"/FRIZZ/a1.wav": bytes, …}`), with states to set (recording, loading, card lent, a save
  writing, a power cut after any FatFs step) and a link that can drop, repeat or cut off
  messages;
- **the client**: list, write (go-back-N, 3.7), rename, swap, delete, copy, rescan, against
  any link with `send(bytes)` and `recv(timeout)`: the model, the twin's pty, or the CHOMPI's
  USB node (`remote.py`'s);
- **`loopxfer.py vectors`**: writes `firmware/test/loopxfer/vectors.json`, scripted sessions as
  the bytes each side sends and the card before and after. Covered: every command, every
  refusal in sections 3-4, a write with lost, repeated and cut-off CHUNKs (a gap, its `DROPS`,
  the rewind and the stale CHUNKs behind it), a lost ack, a lost answer and its repeat across a
  `STATUS` poll, a pause, a timeout, an abort, a power cut mid-write and mid-swap with the boot
  after it;
- **`loopxfer.py --selftest`**: the client against the model, with a lossy link, over a few
  hundred random batches. The card must hold only whole files, and only the allowed names may
  ever change.

frizz-web's simulator (JavaScript) is built from this document and checked against
`vectors.json` at the release tag it builds from, as it reads the docs. The firmware's unit
check is checked against the same vectors. The vectors are the contract's test, and a change to
the protocol changes them in the same commit.

**The vectors' format** (`vectors.json`, `format` 1). One JSON object: `format` 1, `protocol`
`[major, minor]`, `pass_ms` 2, `about`, and `sessions`, a list. A session has a `name`, an
`about`, its `card` before, its `steps` and the card `after`; `"bench": true` for one of
`FRIZZ-bench.bin` (HELLO answered in the pass after it came, `NOT_HERE` with `DETAIL` 0 to the
rest).

- **`card`**: `cluster`, its bytes (4096); `clusters`, the volume's; `fsinfo`, whether the
  card's FSINFO has the free count at a mount (false: FRIZZ counts it, 5.4); `frag_clusters`,
  the most clusters of a file in one piece (a new file's next piece starts after a free
  cluster; a file of n clusters is in ceil(n / `frag_clusters`) fragments); `dirs`, the
  folders; `files`, each path's bytes in hex. Names are found in any case, as FAT finds them. The
  free clusters are `clusters` less each file's ceil(size / `cluster`).
- **`steps`**, in order:
  - `{"boot": true}`: FRIZZ starts on the card as it is: it mounts it, deletes
    `/FRIZZ/xfer.tmp` and starts its scan. The job, the remembered request, the TID count (the
    first is 1) and the states below start afresh.
  - `{"app": "F0 … F7"}`: a message from the app, taken by the audio callback at once: the
    bytes between `F0` and `F7`, the first 128 of them (1.3). A CHUNK goes into the ring, a
    request into its slot; a request while another waits there is dropped. Without USB
    (`usb` false) nothing arrives.
  - `{"run": N, "frizz": […]}`: N passes. `frizz` is every message FRIZZ sent in them, in
    order, in hex, `??` any byte; `null` isn't checked (after a power cut).
  - `{"settle": true, "frizz": […]}`: passes until one sends nothing while FRIZZ has no card
    work left: no scan, no count of the free space, no job step waiting (a finishing write, a
    copy, a full staging block), no temp file to delete.
  - `{"set": {"name": true|false, …}}`: FRIZZ's state: `card` (mounted; false: `NO_CARD`),
    `lent` (true: a job ends with `CARD_LENT` and its temp file is deleted, then the card is
    let go; false: the card mounted again and scanned), `recording`, `loading`, `card_busy` (a
    save or a bug report writes the card), `usb` (false: nothing goes out, a job ends
    `ABORTED`). After boot `card` and `usb` are true, the rest false.
  - `{"remount": true}`: the card mounted again, as a save does after a failed one: open files
    become invalid, what was written to one since its last sync is lost, FSINFO is read again.
  - `{"put": {path: "hex"}}`: a file put on the card by hand.
  - `{"cut": K}`: a power cut after the K-th name change from here (a file created, renamed or
    deleted, a folder made), 0 at once: the card stays as it is then, a file open for writing
    as at its last sync. FRIZZ's messages aren't checked until the next `boot`.
- **`after`**: every file on the card at the end, path → `{"size": n, "crc32": "8 hex
  digits"}` (zlib's).
- **Masked**: in HELLO's `OK` answer, FRIZZ's version (the message's bytes 10-13) is `??`. It
  changes with every release, the contract doesn't.

**A pass** is one of MainLoop's, 2 ms of FRIZZ's clock (it moves after the pass). In it:

1. the loader's scan step, unless the card is lent or `card_busy`: `/FRIZZ`'s listing 24
   entries a pass (the end of the folder found in the pass that reads past its last), then the
   swap repair (5.6) in that pass; a pass for each slot's file found there (lowest bank and key
   first), its header walked; a pass to go on to TAPE's folder (`/TAPE` if it's there, else
   the root), listed and walked the same way; a pass to end. The listing is a folder's files in
   their names' byte order, then its folders. A slot's result is the walk's (section 5.1, the
   walk of `LoopLoader.h`: 1 KB windows, 8 at most);
2. the transfer's step, in this order: a job ends `ABORTED` without USB; a running or paused
   job pauses or goes on (3.8: recording, then loading, then `card_busy`); a running write takes
   CHUNKs out of its ring while its staging block holds 32 KB or less; the request waiting is
   answered (unless the last answer hasn't gone yet); one card step if the card is free (no
   `card`, `lent`, `loading` or `card_busy`): the temp file deleted, else a job's step (not while
   recording: a 32 KB block written and synced, 32 KB of a copy, or one of 3.9's steps), else
   one FAT sector (128 entries) of the free count while no job runs; then the 30 s timeout;
   then the ack, if one is due (3.7: at most one, an end of the write's first, then a running
   or paused write's, then a stale TID's `NO_TRANSFER`);
3. what it sends: the ack, then the answer.

`loopxfer.py`'s `Frizz`, `Loader` and `Transfer` are this, statement for statement where the
bytes depend on it: where this summary leaves a doubt, they settle it.

### 8.2 Host checks (stage 1)

- **Unit check `loopxfer`** (`firmware/test/loopxfer.cpp`): the firmware's protocol code
  against `test/host/fatfs.h`'s card in memory, as `loops.cpp` checks the loader:
  - `vectors.json` replayed byte for byte;
  - every refusal;
  - the temp file and its rename;
  - remount mid-write (SceneStore's save), the loader's DMA in flight, the free space counted
    a step a pass;
  - the fragment check, a file in 300 fragments;
  - a power cut after every FatFs call: a fresh boot on the card as it was, then the card holds
    whole files only;
  - the swap's repair;
  - a diff of the card's file list after each case: only the allowed names changed, `/TAPE`
    and the root never.
- **`loops.cpp`** gets `WavWalk`'s `INAM` cases: a name, one over 28 bytes cut at a UTF-8
  boundary, a `LIST` after the data (no name), a `LIST` of another type, a huge `LIST`, a
  length that lies.
- **`remote.py`** gets `loops put FILE BANK SLOT`, `loops mv`, `loops swap`, `loops rm`,
  `loops cp CUBBI BANK SLOT` and `loops ls` (with names), through `loopxfer.py`'s client.
  `test/remote.cpp` runs them against the twin over its pty: an upload, then
  `loops load --check FILE` (the CRC of what loads), names in the listing, the pty dropping
  CHUNKs (go-back-N on the real code).
- **The twin** (`ui.cpp`, a scenario): a loop playing while its own slot is replaced, swapped
  and deleted. The audio must be identical to the same run without the transfer (the loop plays
  on). Then picking the slot plays the new file, and the Loops page's LEDs follow a batch and
  its RESCAN. `twin/ui-at.sh origin/main` shows these failing without the firmware.

### 8.3 On the device (stage 2)

Under the lock, on slot 12, as always (CLAUDE.md). Rules for the user's card, decided with the
supervisor:

- write only into **bank h**, and only into slots that LIST shows empty: the user keeps no
  loops there;
- `ls` of `/FRIZZ` before and after (`remote.py loops ls`), and every slot written is deleted
  again at the end;
- never touch `/TAPE`, the root, the settings or the scenes. A COPY copies a cubbi into
  bank h, then deletes that copy;
- PERSIST 0 (`remote.py play`) doesn't block a transfer: a transfer is asked for, not a side
  effect.

What it measures: the time for 4 MB and for 14.7 MB through Chrome, Firefox and ALSA directly,
with each round trip's ack time, card write time and parse time (6.3, step 1), `remote.py play --cpu`
during a write (the audio callback's load must not rise beyond the scatter), a write paused by
a recording and by a load, the card pulled out of the job's reach (lend), and `loops load
--check` of each upload.

### 8.4 For the release test (stage 3)

The web app on the CHOMPI, by the user: Chrome and Firefox, on Linux, and on Windows and
macOS where possible. That covers 6.2's slow end, a full batch with a progress bar, a tab
closed mid-upload (nothing half on the card, the next batch works), and drag and drop of a
cubbi.
