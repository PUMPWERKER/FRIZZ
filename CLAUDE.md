# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

**FRIZZ**, a custom firmware for **CHOMPI** (a discontinued chromatic sampler / tape instrument by
CHOMPI Club / Chase Bliss), forked from CHOMPI's WAVE firmware. Bare-metal C++ for a Daisy Seed2
DFM (STM32H750). It is built on top of CHOMPI's archival open-source release (`upstream` remote),
which will not receive updates.

**[`firmware/README.md`](firmware/README.md) is the developer guide**: toolchain, build, host
checks, the virtual CHOMPI, flashing over USB, the CPU bench, where things are in `code/src`
and the rules the code follows. Read it before changing firmware; this file keeps only the
workflow and what bites. Free memory and how to make room: [`docs/CAPACITY.md`](docs/CAPACITY.md).
The stock firmwares, bootloader and hardware files under `reference/`:
[`docs/STOCK_FIRMWARES.md`](docs/STOCK_FIRMWARES.md).

## Repo layout: user-facing at the root, everything else below it

| Path | What | Audience |
|---|---|---|
| `README.md`, `INSTALL.md`, `QUICKSTART.md`, `MANUAL.md`, `CHANGELOG.md` | what FRIZZ is, install, quick guide, full controls, what changed since the last release | users |
| `firmware/` | FRIZZ source (`code/`, `bin/`, `test/`, `twin/`, `flash.py`, `card.py`, `remote.py`, `tools/`) | developers |
| `docs/` | design notes: `LOOPER.md`, `FX_OVERVIEW.md`, `CAPACITY.md`, `STOCK_FIRMWARES.md` | developers |
| `reference/` | the original CHOMPI release, unchanged except for links | reference only |
| `LICENSE`, `THIRD_PARTY.md`, `TRADEMARKS.md`, `CLAUDE.md` | legal, this file | — |

Keep the root user-facing: anything that only helps development goes in `docs/` or
`firmware/`. When a control changes, update `MANUAL.md` (and `QUICKSTART.md` if it's covered
there). The panel artwork and CHOMPI logos are deliberately absent for copyright reasons, and
the CHOMPI name/marks are excluded from the MIT license (`TRADEMARKS.md`). Don't reintroduce
branding into derived hardware files.

Hand-written source is `firmware/code/src/` (~60 files). Everything under `libs/`,
`cube_dfu/`, `Drivers/`, `Middlewares/` and any `build/` directory is vendored or generated:
exclude it from greps. `reference/` holds three more independent copies of a similar tree, so
scope searches to `firmware/`.

## Git workflow: branches first, checked by the sessions, merged by the user

Never commit development work to `main`. Start every change (feature, fix, refactor, docs) on
its own branch off `main`, named for what it does (e.g. `loop-length`, `fix-crusher-level`),
and commit and push there.

**Testing has three stages** (decided 2026-10-10):

1. **Every branch, on the host and the virtual CHOMPI:** `firmware/test/all.sh`, new cases for
   what changed, `compare.sh` with every difference explained (below).
2. **Every firmware branch, on the device, by the session, without the user:** its build to
   the test slot (12), a bench run, its scenarios played there, and `measure.py` where the
   change is about timing or sound and the audio interface is wired (*The CHOMPI is shared*,
   below). This catches what the twin can't: the CPU load and the memory layout.
3. **Every release, by the user:** ears, hands and real gear on a release candidate, from a
   release-test issue that collects what stages 1 and 2 can't judge (*Releases*, below).

A branch merges into `main` once stages 1 and 2 pass and are in its PR. **The user presses
merge**; sessions never merge into `main`. `main` is therefore checked, not played: what the
user has tested is a release tag.

**One worktree per branch.** Several sessions work on this repo at once, so never switch
branches in the main checkout (`~/git-projects/FrizZ` stays on `main`). Work on a branch in its
own worktree next to it:

```bash
git fetch && git worktree add ../FrizZ-<branch> -b <branch> origin/main
```

and remove it (`git worktree remove`, `git branch -d`, delete the remote branch) once it's
merged. A tool that sends a build (`flash.py --no-build`) sends what that worktree's
`firmware/code/src` built last: run it from the branch's worktree, or name the file.

Branches don't wait for each other: each merges on its own once it passes, and a branch still
open when another reaches `main` merges `main` in and runs its checks again. Stack a branch
on an unmerged one only when it really builds on it (or would conflict heavily without it),
and say so in its PR; its PRs merge bottom-up, and a fix goes on the branch it belongs to,
merged upwards.

**Builds aren't committed; their md5 names them** (decided 2026-10-11). GCC 10.3 builds the
same source to the same bytes on any machine (*Toolchain*, below), so `make` and `make BENCH=1`
in `firmware/code/src` give any session a commit's build, and its md5 says which build it is:
a PR's *Checked on the device*, `firmware/bin/cpu.txt` and a release's notes name builds by
md5. `firmware/tools/builds.py` builds both and prints their md5 (`builds.py md5` only
prints). They stay in `code/src/build/FRIZZ.bin` and `build-bench/FRIZZ-bench.bin`, which git
ignores; `firmware/bin/` keeps CHOMPI's bootloader, its install script and `cpu.txt`. A branch
from before then that merges `main` gets a modify/delete conflict over `firmware/bin/FRIZZ.bin`
and `FRIZZ-bench.bin`: delete them (`git rm`), don't rebuild them into git. Releases for users
are on GitHub's Releases page. `firmware/tools/install-hooks.sh` installs a pre-commit hook
that refuses commits on `main` and warns when `firmware/code/` is staged without
`CHANGELOG.md`.

Every commit that changes `firmware/code/`, `firmware/test/` or `firmware/twin/` first passes
`firmware/test/all.sh`. A check that fails is fixed, or, when the change is meant to alter
what it checks, updated in the same commit, saying so in the commit message and in the PR. A
refactor must leave `firmware/test/check.sh` and `firmware/twin/compare.sh` at
`bit-identical`.

**Open topics are GitHub issues** (`gh issue`): a bug, a fault found by a check, a decision
the user still has to make, a test gap, an idea for later. A new finding becomes an issue (the
repo is public: no private details), with the labels `known fault`, `timing`, `cpu`,
`test gap`, `decision` or `parked` where they fit; a PR that settles one says `Fixes #N`, and
a `Known()` check names its issue in its message. Don't keep open topics only in PR texts or
notes.

Two artifacts track every branch; keep both current with each change, and read them before
working on a branch:

- **`CHANGELOG.md`** (Keep a Changelog style): every change a player notices goes under
  **Unreleased** (Added / Changed / Fixed / Removed), in the commit that makes it, worded for
  users like `MANUAL.md`. Credit outside contributors with their PR. Refactoring, tests and
  developer docs stay out unless they change behaviour. A release renames Unreleased to the
  version and its GitHub release notes start from it.
- **A pull request per branch into `main`**, opened (as a draft is fine) once the branch is
  pushed. Its description lists what changed and three sections, kept current
  (`gh pr edit`) with every commit:
  - **Checked on the twin** (stage 1, below).
  - **Checked on the device** (stage 2): which build ran (the md5 `flash.py` prints for
    what it sends, the bench build's too), the bench's `cpu.txt` against
    `main`'s (`firmware/bin/cpu.txt`), the scenarios played with `remote.py play --cpu` and
    their worst load, `measure.py` results where they apply. A docs- or tooling-only branch
    says it has none.
  - **For the release test** (stage 3): `- [ ]` items, one per thing only the user can
    judge on the CHOMPI (feel, sound by ear, real MIDI gear), with what to expect. Merging
    doesn't wait for them: when the user merges, the session copies them into the open
    release-test issue (*Releases*).

  The PR is ready when stages 1 and 2 pass: mark it ready (`gh pr ready`) and tell the user,
  with a short summary; merging it is the user's approval. A branch built on another unmerged
  branch either gets a PR covering both or a stacked PR based on that branch (retarget the
  upper PR to `main` before deleting the lower branch, or GitHub closes it). Merge an outside
  contributor's commit unchanged (no squash, rebase or cherry-pick) so GitHub credits them.
- **What the virtual CHOMPI can show, it checks, not the user** (`firmware/twin/`). For each
  thing to test that is about keys, LEDs, the card, levels, clicks or MIDI timing, the branch
  adds a case to `firmware/test/ui.cpp` (or `sync.cpp`/`midi.cpp`), and
  `firmware/twin/ui-at.sh origin/main` shows it failing without the change. `git fetch`
  first: the local `main` can lag. `firmware/twin/compare.sh origin/main HEAD` goes into the
  PR with every difference explained. The PR lists those under **Checked on the twin** (no
  boxes); what only the device shows goes to stage 2, what only the user can judge to the
  release test.

### Releases and the release test

The open **release-test issue** (label `release test`, one at a time, titled for the next
version) collects every merged PR's *For the release test* items, under the PR's number, plus
the standing ones (a bench run on the candidate, a session of playing). When the user wants
to prepare a release, the session cuts a release candidate from `main` as a test build
(below) and links it in the issue; the user plays it and ticks the items. A problem found
becomes an issue and a fix branch (stages 1 and 2 as always), then the next candidate. The
release is made from the candidate the user signed off, once every box is ticked or struck
through with why, and the issue is closed with it; the next one opens.

A build handed out for testing goes on GitHub as a **pre-release**, never as Latest:

- Each release candidate (or other test round the user asks for) gets a numbered one,
  `v<next>-beta.N` (next: `v0.12-beta.1`), tagged on the pushed commit, with that commit's
  build attached: `make` in a clean worktree at the tag (`git worktree add ../FrizZ-<tag>
  <tag>`, GCC 10.3), and its `firmware/code/src/build/FRIZZ.bin` uploaded under that name. The
  notes name its md5 and are taken from `CHANGELOG.md`'s Unreleased section, plus a link to
  the release-test issue (or the PR). The number never moves, so feedback can name the build.
- The pre-release **`beta`** always carries the newest numbered one (until v0.12-beta.1: v0.11
  itself), and moves only with a new one, at a fixed link
  (`https://github.com/ZUHcYa/FrizZ/releases/download/beta/FRIZZ.bin`): `git tag -f beta
  <commit> && git push -f origin beta`, `gh release upload beta <the numbered one's FRIZZ.bin>
  --clobber`, and `gh release edit beta` with a title and notes naming the numbered build and
  its md5.
- Cut one only when the user asks for a test build; a release for everyone is a normal
  release on `main`, its `FRIZZ.bin` built the same way at its tag, its md5 in the notes.

## The CHOMPI is shared: the lock and the slots

There is one CHOMPI, and other sessions and the user use it too. The device tools
(`flash.py`, `card.py`, `remote.py`, `tools/measure.py`) take a lock through
`tools/chompi.py` and wait while someone else has it, saying who. A session's stage-2 run
holds it for the whole sequence, so nothing slips in between:

```bash
cd firmware && tools/builds.py && tools/chompi.py hold sh -c '
  ./flash.py --bench --no-build && sleep 150 && ./card.py --then none get &&
  ./flash.py --test --no-build && ./remote.py play twin/scenarios/fx-each.txt --cpu
  ./flash.py --run 10' && tools/builds.py cpu
```

(it builds before taking the CHOMPI; the bench runs ~100 s by itself; `card.py get` fetches
its `cpu.txt` into `card/`, and `builds.py cpu` files that as `bin/cpu.txt`), and leaves FRIZZ
on key 10 running at the end. Stage 2 plays no scenario that uses the settings
page (`settings.txt`): what it changes is saved to the card key 10 shares, so `remote.py play`
refuses it without `--force` (#58). The user takes the CHOMPI for playing with
`tools/chompi.py hold` (until Ctrl-C), or by telling a session, which then runs it for them.
Within that, stage 2 needs no asking. Ask the user first only for what needs their hands or
changes what they play: sending to key 10, anything when the lock's holder is the user, a
cable to plug.

The slots: FRIZZ on key 10 is the user's and gets only `main`'s or a release's build, when
they ask; a branch's build goes to the **test slot, key 12** (`flash.py --test`, as
`FRIZZ-TEST`); the bench on 11, USB storage on 15. Never send to another slot. The tools
reach the device over USB through the multi-firmware launcher (`firmware/README.md`, *Put it
on the CHOMPI*); while it is switched off, they can't.
`firmware/tools/measure.py` measures timing and sound there (a MIDI clock in pairs, a loop's
drift against a TRS clock, an effect's A/B between builds): `firmware/README.md`, *Measure
timing and sound on the device*.

## Toolchain: GCC 10.3, and the pin matters

FRIZZ builds with **GNU Arm Embedded 10.3-2021.10** (GCC 10.3.1): newer GCC intermittently
breaks SD-card communication. On this machine it is at `~/opt/gcc-arm-none-eabi-10.3-2021.10/`,
on `PATH` via `~/.bashrc`; non-interactive shells may not read it, so prepend it explicitly:
`PATH=~/opt/gcc-arm-none-eabi-10.3-2021.10/bin:$PATH make`. Run `make` in
`firmware/code/src`, never from the repo root, and read the memory table it prints.

The pin is also what makes a build's md5 its name: GCC 10.3 builds the same source to the same
bytes on every machine (checked 2026-10-11: main rebuilt elsewhere came out byte-identical),
so the builds needn't be committed. Another compiler builds other bytes, and its build isn't
the commit's: `flash.py` and `tools/builds.py` refuse to build with one. The host
checks need only `g++` and `python3`; the browser twin needs Emscripten in `~/opt/emsdk`.

## Tests: the host checks and the virtual CHOMPI

`firmware/test/all.sh` runs everything, side by side (about 75 s): `check.sh` (the engine against HEAD;
a refactor must come out `bit-identical`) and every unit check, `./unit.sh NAME` for one:
`pitch`, `tape`, `crusher`, `freezer`, `scenes`, `store`, `clicks`, `delay`, `controls`, `keys`, `looper`, `tempo`,
`comp`, `level`, `sleep`, `inserts`, `ui`, `remote`, `bench`, `midi`, `sync`, `midiout`, `chaos`, `page2`. A new check is just a new
`NAME.cpp`; [`firmware/test/README.md`](firmware/test/README.md) says what each covers.
`check.h`'s `Known()` marks a fault found and not yet fixed: it reports, doesn't fail.

`firmware/twin/` is the **virtual CHOMPI**: the whole firmware compiled unchanged for the host
on a simulated board, deterministic and 13-20x real time. `./run.sh -o out.wav -l - SCRIPT`
plays a script of keys, knobs, MIDI and audio from power-on; `web/serve.sh` plays it in the
browser. A bug the user hits comes as such a script (SHIFT + VOLUME held 2 s on the
settings page writes `/FRIZZ/bug-N.txt`, `EventLog.h`); once it shows the bug, it becomes a case in `ui.cpp`. The
twin can't show the CPU load, the codec, races between the audio interrupt and `main()`, or
anything else about the chip ([`firmware/twin/README.md`](firmware/twin/README.md)).

## The audio callback is CPU-bound

The callback has 0.5 ms per 24-sample block. Idle it uses ~45 %, a loop with ~11 effects
reaches 100 % and crackles (a known limit, parked by the user). Effects that are off must stay
cheap (`FxGate::Asleep`). A change that only shifts the memory layout has made it crackle on
the device (b5c658c) while the host stayed bit-identical, so suspect the CPU when crackles
appear that the twin can't reproduce. The host can't measure the load; the device can:
`make BENCH=1` builds `FRIZZ-bench.bin` (`Bench.h`, compiled in only then), which runs 23
segments by itself and writes `/FRIZZ/cpu.txt`; `firmware/remote.py load` and `remote.py play
SCRIPT --cpu` read `FRIZZ.bin`'s own load. Every firmware branch runs the bench in stage 2,
on its final build: its `cpu.txt` goes into `firmware/bin/cpu.txt` through `tools/builds.py
cpu`, which heads it with the md5 of the build it measured (so `main` always has its own load),
and into the PR, compared with `main`'s. A segment
noticeably higher is explained or fixed before the PR is ready. Details:
`firmware/README.md`, *Measure the CPU load*.

## Firmware architecture

`chompi_main.cpp` documents the skeleton. Two execution contexts:

1. **`AudioCallback()`**, the audio ISR, once per 24-sample block at 48 kHz: polls controls,
   reads MIDI (`MidiClock.h`), generates UI events, runs the engine.
2. **`MainLoop()`**: UI event dispatch, LEDs, the card, USB answers, battery.

**No file I/O and no blocking call from the audio ISR.** FRIZZ reads the card once at boot and
writes it only from `MainLoop()`: when an FX scene is saved, copied or deleted, 2 s after the
master compressor's knobs, the mono input or the MIDI settings rest (`SceneStore.h`,
`MasterSettings.h`), or when a bug report is asked for (`EventLog.h`). Its files live in
`/FRIZZ` (`frizz_scenes.txt`, `frizz_master.txt`, `.bak` copies, `bug-N.txt`), which
`EnterFrizzDir()` creates at boot on a card without it. MIDI: clock in over TRS and USB
(`MidiClock.h` → `TempoClock.h`, `TapTempo.h` as the fallback), notes, CCs, program changes and
FRIZZ's SysEx (`MidiControl.h`), answered over USB from `MainLoop()`. MIDI out
(`MidiOut.h`, off unless the settings page asks): the clock and the loop's transport, out of
the jack from the audio callback without waiting, over USB from `MainLoop()`.

The play page is `NormalPage.h`; the engine is `passthroughEngine.h` → `Looper.h` +
`FxMorph.h` → `FxChain.h` → `MasterComp.h` → output gain → `limiter.h`. Large buffers live in
SDRAM (`DSY_SDRAM_BSS`), cleared by `ZeroSDRAM()` at boot because startup code doesn't. Card
buffers live in internal RAM, 32-byte aligned, written in whole sectors (`EventLog::Flush`).

**Memory**: the firmware runs from SRAM, loaded by CHOMPI's bootloader (`APP_TYPE=BOOT_SRAM`).
Code has about 60 KB left (`SRAM_EXEC`), the bench build ~43 KB, and its data only ~7 KB; `docs/CAPACITY.md` lists every
region and the ways to make room. Never use `make program-boot`, and never swap in upstream
libDaisy: the vendored copy is patched (`THIRD_PARTY.md`).
`__attribute__((optimize("-O0")))` and similar per-function overrides (`ProcessAllControls`)
are deliberate workarounds, not leftovers.
