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
| `README.md`, `INSTALL.md`, `QUICKSTART.md`, `MANUAL.md`, `CHANGELOG.md` | what FRIZZ is, install, quick start, full controls, what changed since the last release | users |
| `firmware/` | FRIZZ source (`code/`, `bin/`, `test/`, `twin/`, `flash.py`, `card.py`, `remote.py`, `tools/`) | developers |
| `docs/` | design notes: `LOOPER.md`, `FX_OVERVIEW.md`, `CAPACITY.md`, `STOCK_FIRMWARES.md`, `LOOP_PROTOCOL.md` | developers |
| `reference/` | the original CHOMPI release, unchanged except for links | reference only |
| `images/` | the manual's diagrams (SVG, drawn by `firmware/tools/diagrams/`) | users |
| `site/` | the website's front-page text (`landing.md`) and what the release's firmware can do (`features.json`, the Companion page's), both read by the site at the tag | users |
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

## Docs that feed the website

https://pumpwerker.de/frizz/ is built from `MANUAL.md`, `QUICKSTART.md`, `INSTALL.md`,
`images/`, `site/landing.md`, `site/features.json` (what the Companion page offers:
`loopTransfer`, the loop transfer's protocol major, [`docs/LOOP_PROTOCOL.md`](docs/LOOP_PROTOCOL.md)
2.2) and the key colours in `LedColors.h`/`FxSlots.h` (repo
`friedrich/frizz-web` on the user's Gitea), fetched at the **latest stable release** tag (and
`/beta/` at the newest pre-release), so a docs change on `main` goes live with the next tag.
FrizZ holds every fact the site shows; frizz-web only renders it (design, layout, the trademark
note). `README.md` is for GitHub only, not a site source. Its build fails on any internal link
or anchor that doesn't resolve, and on any format below it can't read. Every PR that touches
these files is checked against this before it merges:

1. **Headings are URLs.** Each `##` of `MANUAL.md` becomes its own page (`/manual/<slug>/`),
   each `###` an anchor on it (GitHub's slugs: "Wow & flutter" → `wow--flutter`). Renaming a
   heading breaks links to it from outside: keep headings stable, unique within the manual,
   and plain text (no links, code or emphasis in them).
2. **Cross-references are Markdown links**, `[Settings page](MANUAL.md#settings-page)`, not
   prose ("MANUAL.md, *Settings page*"). Links between the four files and to their anchors
   become site URLs, links to other repo paths go to GitHub at the release tag; an anchor
   must match an existing heading.
3. **A `##` chapter is one web page:** keep it readable as one; split a chapter that grows very
   long (Punch-in FX) into several `##` chapters, e.g. one per effect.
4. **Formats the site parses:**
   - `QUICKSTART.md`: the white keys' table keeps its row starting `| Effect |`, then exactly
     15 cells, one per white key (`–` for an unused one); the landing page builds its key row
     from it and links each name to the manual heading `Name`, `Name key` or `Master Name`.
   - `MANUAL.md`, `QUICKSTART.md` and `INSTALL.md` keep exactly one H1 each (the page title).
   - `MANUAL.md`'s parts: a line `<!-- part: Name -->` on its own (a blank line before and
     after, not in a code block) right before a `##` starts a part, a folding group in the
     site's sidebar; chapters before the first marker go to "Getting started". Invisible on
     GitHub, headings and anchors unchanged. A new chapter goes into the part it belongs to.
   - `MANUAL.md`'s summaries: a line `<!-- summary: ... -->` right under a `##` is that
     chapter's one-line description on the site's Contents page and its meta description (a
     sentence, under ~12 words; optional, a chapter without one gets none). A white key's
     chapter must have one, and it is its card on the landing page: 1 to 2 sentences, at most
     160 characters, written for players. `INSTALL.md` and `QUICKSTART.md` each have one
     under their H1, their description on the site.
   - Each chapter of a white key's effect (the chaos key and the finalizer too) keeps
     its knob table, `| Knob | Page 1 | …` (the finalizer's first module's `| Knob | Function |`), rows 1 to 4: the landing page's knob labels are
     each Page 1 cell's text up to the first ":", "," or " (", so that start names the knob.
   - `site/landing.md` is the landing page's text (hero, the moves, the steps, the section
     hints, credits) in frizz-web's fixed format: its sections in order, `- Name: value` fields
     with length limits, one paragraph each, links only to the docs, `images/` or `https://`.
     The format is spelled out in the file's head comment and checked by `doclinks.py`.
   - The key colours: the site parses `LedColors.h` (`static const float name[3] = {r, g, b};`)
     and `FxSlots.h` (`kFxSlots` entries `{Hardware::SwId::KEY_<n>, <colour>, …`, `kCompKey`);
     keep those shapes when touching them.
5. **No version history in the user docs:** "until v0.11 …" belongs in `CHANGELOG.md`, not in
   `MANUAL.md`, `QUICKSTART.md` or `INSTALL.md`.
6. **To see it:** in a frizz-web worktree of your own (not the user's checkout),
   `FRIZZ_DIR=/abs/path/to/FrizZ-worktree npm run build` builds the site from that checkout and
   fails on a broken link, anchor or missing image (`FRIZZ_REF=<release tag>` builds a
   release). Images live in `images/`, linked relatively (`![…](images/name.svg)`) with real alt
   text. Without the site, `firmware/tools/doclinks.py` checks every rule above (links,
   anchors, headings, summaries, parts, knob tables, the key row, the key colours,
   `site/landing.md`); it runs in `all.sh` as the check `doclinks`, and `--selftest` shows each
   rule firing. What the manual's tables say (knobs used, steps, coarse grids, CCs, colours) is
   checked against the code by the unit check `docs`: a change to a control fails `all.sh`
   until `MANUAL.md` follows.
   The diagrams in `images/` are drawn by `firmware/tools/diagrams/diagrams.py` from data
   (`layout.json`, the colours in `LedColors.h`/`FxSlots.h`, `kBankColors`/`kCubbiColors`, the
   knobs' page-1 names in `MANUAL.md`), and `doclinks` runs `diagrams.py --check`: a branch
   that changes any of those runs `diagrams.py` and commits the SVGs, or `all.sh` fails.
7. **The site updates itself:** frizz-web checks every 20 minutes for a new release and rebuilds
   when there is one, so it is live within about 20 minutes; nothing to trigger. New means a
   published GitHub release (not a draft) with its `.bin` attached; a bare tag isn't picked up.
   The rolling `beta` pre-release is ignored: the newest numbered pre-release after the latest
   stable one becomes `/beta/`. `curl https://pumpwerker.de/build.json` shows which
   tags the site has (`{"stable": …, "prerelease": …}`): check it after cutting a release.

## Git workflow: branches first, checked by the sessions, merged by the user

Never commit development work to `main`. Start every change (feature, fix, refactor, docs) on
its own branch off `main`, named for what it does (e.g. `loop-length`, `fix-crusher-level`),
and commit and push there.

**Testing has three stages** (decided 2026-10-10):

1. **Every branch, on the host and the virtual CHOMPI:** `firmware/test/all.sh`, new cases for
   what changed, `compare.sh` with every difference explained (below). Since 2026-10-15 (the
   user's decision) in two parts, with CI on GitHub Actions (`.github/workflows/ci.yml`):
   - **1a, on Actions, at each push:** `all.sh`, `doclinks.py`, and `check.sh` and
     `compare.sh` against the PR's base run there, and both builds with ARM's GCC 10.3, their
     md5 and memory tables in the job summary (the build of the branch's head, the md5 its
     stage 2 names; CI's and a local build's are byte for byte the same). Before the merge the
     run on the PR's last push is green, and the PR explains every difference in its
     `check.sh` and `compare.sh` output under
     *Checked on the twin*, linking the run. A difference doesn't fail the run; a scenario
     the twin couldn't play, or a comparison that didn't get through, does.
   - **1b, locally:** what a check can't run on Actions, which `all.sh` lists under it as
     `SKIP: local only (what's missing)`, and the firmware's build for the device (stage 2).
   - **While working**, `./unit.sh NAME` for the checks of the part changed: cheap, no host
     lock, alongside other sessions.
   - **Without Actions** (down, or no run on the head) everything runs locally as before,
     under the host lock (*Tests*, below). No check is required on GitHub: merging and
     releases never wait for Actions.
2. **Every firmware branch, on the device, by the session, without the user:** its build to
   the test slot (12), a bench run, its scenarios played there, and `measure.py` where the
   change is about timing or sound and the audio interface is wired (*The CHOMPI is shared*,
   below). This catches what the twin can't: the CPU load and the memory layout.
3. **Every release, by the user:** ears, hands and real gear on a release candidate, from a
   release-test issue that collects what stages 1 and 2 can't judge (*Releases*, below).

A branch merges into `main` once stages 1 and 2 pass and are in its PR. **The user presses
merge**; sessions never merge into `main`. `main` is therefore checked, not played: what the
user has tested is a release tag.

**Broken checks come first** (decided 2026-10-15): when CI or a check of any stage fails
because of the check itself (the workflows, `all.sh` and the host checks, the twin, the device
tools), its fix goes before other work, and the supervising session may merge it without the
user. A firmware bug that a check finds is an ordinary fix branch.

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
open when another reaches `main` merges `main` in and runs its host checks again (`all.sh`,
`compare.sh`); its bench only if its numbers stood noticeably apart from `main`'s (*The audio
callback*, below). Stack a branch
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

Every PR that changes `firmware/code/`, `firmware/test/` or `firmware/twin/` passes
`firmware/test/all.sh` before it merges, on Actions or locally (stage 1 above, changed
2026-10-15 from every commit). A check that fails is fixed, or, when the change is meant to
alter what it checks, updated in the same commit, saying so in the commit message and in the
PR. A refactor must leave `firmware/test/check.sh` and `firmware/twin/compare.sh` at
`bit-identical`, the Action's runs of them or local ones.

**Open topics are GitHub issues** (`gh issue`): a bug, a fault found by a check, a decision
the user still has to make, a test gap, an idea for later. A new finding becomes an issue (the
repo is public: no private details), with the labels `known fault`, `timing`, `cpu`,
`test gap`, `decision` or `parked` where they fit; a PR that settles one says `Fixes #N`, and
a `Known()` check names its issue in its message. Each issue also gets a **milestone** for when it's planned (decided 2026-10-15): the
next release (`v1.5.0` now), `v1.6.0`, `2.0` (a change to the controls), or `später` (parked, no
version yet); a PR gets the milestone of what it settles. Labels say what kind it is, the
milestone when. Don't keep open topics only in PR texts or
notes.

Two artifacts track every branch; keep both current with each change, and read them before
working on a branch:

- **`CHANGELOG.md`** (Keep a Changelog style): every change a player notices goes under
  **Unreleased** (Added / Changed / Fixed / Removed), in the commit that makes it, worded for
  users like `MANUAL.md`. Credit outside contributors with their PR; a merged outside PR
  also adds its author to `README.md`'s *Credits and license* (Contributors) and
  `site/landing.md`'s *Credits*, if they're not there yet (the user's decision 2026-10-15:
  everyone whose code is in FRIZZ, across the project; tips and ideas don't count).
  Refactoring, tests and developer docs stay out unless they change behaviour. A release renames Unreleased to the
  version and its GitHub release notes start from it.
- **A pull request per branch into `main`**, opened (as a draft is fine) once the branch is
  pushed. Its description lists what changed and three sections, kept current
  (`gh pr edit`) with every commit:
  - **Checked on the twin** (stage 1, below), with a link to the Actions run.
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
  PR with every difference explained (the Action's run of it, in its job summary, or a local
  one). The PR lists those under **Checked on the twin** (no
  boxes); what only the device shows goes to stage 2, what only the user can judge to the
  release test.

### Releases and the release test

**1.0 is out** (`v1.0.0`, 2026-10-13; the feature freeze before it, decided 2026-10-12, is
over). From 1.0 on the controls, the MIDI map and the scene files stay as they are: a feature
goes into 1.x, a fix into 1.0.x, and only a break of the controls would be 2.0. A change to
how a control behaves goes to the user as a `decision` issue first, even within 1.x. One
exception: 1.5 replaces the master compressor with the finalizer (#345), decided by the user on
2026-10-15, key 15's knobs and its settings changing.

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
  `v<next>-beta.N` (e.g. `v1.5.0-beta.4`, or `v1.0.2-beta.1` for a fix release), tagged on
  the pushed commit, with that commit's build attached. First the commit to tag sets
  `kFrizzVersion` in `firmware/code/src/Version.h` to the tag's version (`{1, 5, 0, 4}` for
  `v1.5.0-beta.4`, the last 0 for a release), on a branch merged before tagging: the loop
  transfer's HELLO reports it, and `tools/builds.py` and `flash.py --slot 10/11` refuse a tag's
  build whose constant isn't its tag. After the tag, main's next commit may carry the next
  version's. Then the build: `make` in a clean worktree at the tag
  (`git worktree add ../FrizZ-<tag> <tag>`, GCC 10.3), and its `firmware/code/src/build/FRIZZ.bin` uploaded under that name. The
  notes name its md5 and are taken from `CHANGELOG.md`'s Unreleased section, plus a link to
  the release-test issue (or the PR). The number never moves, so feedback can name the build.
- The pre-release **`beta`** always carries the newest numbered one (now v1.5.0-beta.4), and
  moves only with a new one, at a fixed link
  (`https://github.com/PUMPWERKER/FRIZZ/releases/download/beta/FRIZZ.bin`): `git tag -f beta
  <commit> && git push -f origin beta`, `gh release upload beta <the numbered one's FRIZZ.bin>
  --clobber`, and `gh release edit beta` with a title and notes naming the numbered build and
  its md5.
- Cut one only when the user asks for a test build; a release for everyone is a normal
  release on `main`, its `FRIZZ.bin` built the same way at its tag, its md5 in the notes.
  Its notes start from `CHANGELOG.md` but are written for a CHOMPI owner new to FRIZZ, as
  v1.0.0's are: what FRIZZ is, trying it beside the stock firmwares, what you can do with it,
  then what moved for those updating, and a link to the CHANGELOG for every entry.
- On the CHOMPI (decided 2026-10-13, #171; the keys reordered 2026-10-15): a beta goes to
  **key 11** (`FRIZZ-BETA`), a release to **key 10** (`FRIZZ`), each on the user's word once
  it's cut, from the clean worktree of its tag: `./flash.py --slot 11` (or `--slot 10`) there.
  `flash.py` sends to 10 or 11 nothing else: only when the key is named, and only the build of
  a clean `v*` tag's checkout.
- **CI checks the release's build** (`.github/workflows/release.yml`): a pushed `v*` tag is
  built there too, refused unless it is `kFrizzVersion`'s. If the tag's release (or a draft of
  it) carries a `FRIZZ.bin`, CI's must have the same md5, or the run fails loudly; a draft
  without one gets CI's attached, a published release without one fails the run (CI never
  puts anything live); with no release yet, CI makes a draft (a pre-release for `-beta.N`) with
  `FRIZZ.bin` and its md5, never published, never Latest, and `beta` is never touched. A
  release never waits for it: make it as above, right after pushing the tag (`gh release
  create <tag> --verify-tag …`); should CI's draft be there first, finish that one (`gh release
  edit`) instead of making a second, and once your `FRIZZ.bin` is on it, run the tag's
  *Release build* again (`gh run rerun <its run>`) so it checks that md5. After the push,
  **Release check** (`.github/workflows/release-check.yml`, #440, read-only) checks a v*
  release's `FRIZZ.bin` again against a fresh build of its tag when it is published or edited,
  and the rolling `beta`'s against the numbered pre-release on its commit (and the md5 its notes
  name) when it is edited, and both nightly (the latest stable and newest numbered pre-release):
  a mismatch fails loudly; it never uploads, publishes or moves anything.

## The CHOMPI is shared: the lock and the slots

There is one CHOMPI, and other sessions and the user use it too. The device tools
(`flash.py`, `card.py`, `remote.py`, `tools/measure.py`) take a lock through
`tools/chompi.py` and wait while someone else has it, saying who. A session's stage-2 run
holds it for the whole sequence, so nothing slips in between:

```bash
cd firmware && tools/builds.py && tools/chompi.py hold sh -c '
  ./flash.py --bench --no-build && ./remote.py bench --wait --out card/cpu.txt &&
  ./flash.py --test --no-build && {
    ./remote.py play twin/scenarios/fx-each.txt --cpu; echo "play: $?"
    ./remote.py play twin/scenarios/phaser.txt --cpu --fresh; echo "play: $?"; }
  ./flash.py --run 10'
```

(it builds before taking the CHOMPI; `flash.py` checks that the build it started is the one
it sent (FRIZZ's IDENT, the developers' SysEx: `firmware/README.md`); the bench runs ~100 s
by itself, and `remote.py bench --wait` follows it over USB and fetches its `cpu.txt` into
`card/`, byte for byte what it wrote to the card, for the PR to compare with `main`'s
`bin/cpu.txt`), and leaves FRIZZ on key 10 running at the end. A bench build from before that
(a branch that hasn't merged `main` since) gets `sleep 150 && ./card.py --then none get`
instead. Each `play` (several go in the `{ }`, the `--test` build's)
ends in `;`, not `&&`: a failed `expect led` (exit 1) is judged from its output, and stops
neither the next scenario nor `--run 10`. Stop a hold with SIGTERM (a second forces it), never
SIGKILL: it passes the signal to the whole chain, whose `; ./flash.py --run 10` then doesn't
run, so run `tools/chompi.py hold ./flash.py --run 10` afterwards. Ctrl-Z at a prompt with
`hold sh -c` hangs the terminal; `hold bash` is fine. Every `play` after the first gets `--fresh` (the
second line shows how; play the branch's own scenarios there), which starts the slot again
first (~10 s, the card untouched): a scenario starts from power-on, as on the twin, and
without it the RAM the ones before left can fail its `expect led` (#123). `play` keeps the
card as it was: it first turns PERSIST 0 on (nothing written until FRIZZ restarts), so the
scenarios that change what key 10 shares, the settings page (`settings.txt`, #58), the
compressor's knobs (`comp.txt`), the scenes (`scenes.txt`, #78) and the input (`mic.txt`, `volume.txt`, #152), play in stage 2 too. Never `--save` or `--force` on the device: a
FRIZZ without PERSIST (key 10's from before it) is still refused, as before. The card holds
the user's own settings and loops, which a session never changes. PERSIST 0 doesn't cover the
loop transfer: `remote.py loops put`, `mv`, `swap`, `rm` and `cp` write the card at once, so on
the device a session moves only loops it put there itself, in slots the user's loops aren't in
(`remote.py` keeps a ledger of the slots the tools filled and refuses `rm`, `mv`, `swap` and
`--replace` on any other loop unless `--users-loop`, which a session never gives, #438); `expect led` lines are judged at the
card's LED brightness, and on the device `settings.txt` fails the battery's line (VOLUME white
on USB, green on the twin). The user takes
the CHOMPI for playing with `tools/chompi.py hold` (until Ctrl-C),
or by telling a session, which then runs it for them.
A scenario that depends on what the card keeps sets it first (`set GROUP INDEX VALUE`, #218):
`ui.cpp` also plays every scenario on a player's card. `input sine` goes to the device as the
test signal (SIGNAL, #225), so `play --cpu` with it reads ~1 point above runs before that. A
turn in the same instant as a key's edge lands differently on the twin and the device: leave
`wait 100` between them (#227). The DAW cases' music lives in `firmware/card/audio/`
(git-ignored, the user's own samples: never commit or name it).
Within that, stage 2 needs no asking. Ask the user first only for what needs their hands or
changes what they play: sending to key 10 or 11, `flash.py --list` (it restarts FRIZZ into
the launcher: a loop in RAM is lost), anything when the lock's holder is the user, a cable to
plug.

The slots (#171, reordered 2026-10-15): keys 10 and 11 are the user's, **10 the latest
release, 11 the latest beta** (*Releases*); sessions never send there unasked, and `flash.py`
refuses anything but a clean `v*` tag's build there, and only with the key named (`--slot 10`,
`--slot 11`; a bare `./flash.py` sends nothing). A branch's build goes to the **test slot, key
12** (`flash.py --test`, as `FRIZZ-TEST`); the bench on 13 (`flash.py --bench`, as
`FRIZZ-BENCH`), USB storage on 15. Never send to another slot. On the launcher 1.6 `flash.py`
colours each key with the build it sends: 10 green, 11 orange, 12 and 13 red (the builds
sessions make).
`remote.py`'s setters that FRIZZ saves (`channel`, `transport`, `source`, `out`, `scene put`,
`set` for settings, the compressor and the input gain) keep the card out of it as `play` does
(PERSIST 0) unless `--save`, which a session never gives. The tools
reach the device over USB through the multi-firmware launcher (`firmware/README.md`, *Put it
on the CHOMPI*); while it is switched off, they can't.
`firmware/tools/measure.py` measures timing and sound there (a MIDI clock in pairs, a loop's
drift against a TRS clock, an effect's A/B between builds): `firmware/README.md`, *Measure
timing and sound on the device*.
`firmware/tools/midisuite.py` (this computer as an idealised DAW) and `firmware/tools/reaper.py`
(REAPER 7 itself, on a configuration of its own, never the user's) play the release test's
MIDI and DAW items there, under the lock like the other tools: `tools/reaper.py --out
card/reaper.md` between `./flash.py --test --no-build &&` and `./flash.py --run 10`. While
REAPER runs it has FRIZZ's USB node to itself (the tool talks to FRIZZ through REAPER), so
nothing else reaches FRIZZ then; `firmware/README.md`, *REAPER: a real DAW against FRIZZ*.

## Toolchain: GCC 10.3, and the pin matters

FRIZZ builds with **GNU Arm Embedded 10.3-2021.10** (GCC 10.3.1): newer GCC intermittently
breaks SD-card communication. It goes in `~/opt/gcc-arm-none-eabi-10.3-2021.10/`
(`firmware/README.md`, *0. Set up a development machine*), on `PATH` via `~/.bashrc`;
non-interactive shells may not read it, so prepend it explicitly:
`PATH=~/opt/gcc-arm-none-eabi-10.3-2021.10/bin:$PATH make`. Run `make` in
`firmware/code/src`, never from the repo root, and read the memory table it prints.

The pin is also what makes a build's md5 its name: GCC 10.3 builds the same source to the same
bytes on every machine (checked 2026-10-11: main rebuilt elsewhere came out byte-identical),
so the builds needn't be committed. Another compiler builds other bytes, and its build isn't
the commit's: `flash.py` and `tools/builds.py` refuse to build with one. The host
checks need only `g++` and `python3`; the browser twin needs Emscripten in `~/opt/emsdk`.

## Tests: the host checks and the virtual CHOMPI

`firmware/test/all.sh` runs everything, side by side (about a minute; the first run ~80 s; on
Actions ~4 minutes, stage 1a): `check.sh` (the engine in the working tree against HEAD, before
a commit; after it there's nothing to compare, and its line says so: on Actions it runs against
the PR's base on its own; a refactor must come out `bit-identical`) and every unit check, `./unit.sh NAME` for one:
`pitch`, `tape`, `block`, `crusher`, `freezer`, `scenes`, `store`, `clicks`, `delay`, `controls`, `keys`, `looper`, `tempo`,
`comp`, `level`, `sleep`, `inserts`, `resonator`, `ui`, `remote`, `bench`, `midi`, `sync`, `midiout`, `chaos`, `page2`, `phaser`, `finalizer`, `midisuite`, `reaper`, `lockstep`, `splice`, `usbmidi`, `routing`, `measure`, `loopcap`, `companion`, `loops`, `loopxfer`, `sddisk`, `twinlib`, `webtwin`, `hostlock`, `compare`, `rtalloc`, `usbtransport`, `usbdevice`, `uartlisten`, `docs` (and `doclinks`, the docs' format, rule 6 above). A new check is just a new
`NAME.cpp`; [`firmware/test/README.md`](firmware/test/README.md) says what each covers.
`check.h`'s `Known()` marks a fault found and not yet fixed: it reports, doesn't fail. A check
that needs something only a developer's machine has prints `SKIP: local only (what's
missing)` and checks what it can without; `all.sh` lists those lines under it (stage 1b).

**Several sessions on one machine:** heavy host runs (`all.sh`, `compare.sh`, `check.sh`,
`ui-at.sh`, `tools/builds.py`, the twin's builds by hand) go one at a time, each under a memory
cap: parallel ones have run the machine out of memory. They take the host lock themselves
(`firmware/test/hostlock.sh`, #439: `flock ~/.cache/frizz-host.lock`, then a `systemd-run --user
--scope -p MemoryMax=4G -p MemorySwapMax=0`), wait while another run holds it, saying which, and
what they start runs under theirs; nothing to wrap by hand (a wrapper by hand still works). A
single `./unit.sh NAME` takes none and may run alongside.

`firmware/twin/` is the **virtual CHOMPI**: the whole firmware compiled unchanged for the host
on a simulated board, deterministic and 13-20x real time. `./run.sh -o out.wav -l - SCRIPT`
plays a script of keys, knobs, MIDI and audio from power-on; `web/serve.sh` plays it in the
browser. A bug the user hits comes as such a script (SHIFT + VOLUME held 2 s on the
settings page writes `/FRIZZ/bug-N.txt`, `EventLog.h`); once it shows the bug, it becomes a case in `ui.cpp`. The
twin can't show the CPU load, the codec, races between the audio interrupt and `main()`, or
anything else about the chip ([`firmware/twin/README.md`](firmware/twin/README.md)).

## The audio callback is CPU-bound

The callback has 0.5 ms per 24-sample block. Idle it uses ~29 %, a loop with every effect
~73 % on average and ~80 % at worst (~81 % and ~88 % with every finalizer module working); only the bench's page-2 extreme (all 13 effects, page 2
off its defaults) goes past 100 % and crackles (#21, settled by the user for 1.0). Effects that are off must stay
cheap (`FxGate::Asleep`). A change that only shifts the memory layout has made it crackle on
the device (b5c658c) while the host stayed bit-identical, so suspect the CPU when crackles
appear that the twin can't reproduce. The host can't measure the load; the device can:
`make BENCH=1` builds `FRIZZ-bench.bin` (`Bench.h`, compiled in only then), which runs 31
segments by itself and writes `/FRIZZ/cpu.txt`; `firmware/remote.py load` and `remote.py play
SCRIPT --cpu` read `FRIZZ.bin`'s own load. Every firmware branch runs the bench in stage 2,
on its final build, and its PR compares it with `main`'s `firmware/bin/cpu.txt`; a segment
noticeably higher is explained or fixed before the PR is ready. Compare in one hold (A/B against main's build): the same build benched on another day reads up to ~0.5 points apart in single columns (#261), so `cpu.txt` files of different days only compare in their totals. **A branch doesn't commit
`bin/cpu.txt`** (decided 2026-10-11), so firmware PRs don't queue behind each other's bench:
after a round of merges, and before every test build or release, a session benches `main`'s
own build, files it with `tools/builds.py cpu` (which heads it with the md5 of the build it
measured) and opens a PR changing only `bin/cpu.txt` (tooling, it merges any time). So `main`
has its own load, measured after the merges, and layout effects between merged branches show
there. Details:
`firmware/README.md`, *Measure the CPU load*. The bench build checksums its source, so a
comment changed under `firmware/code/` changes its md5 too: a docs-only branch leaves code
comments alone (or benches again).

## Firmware architecture

`chompi_main.cpp` documents the skeleton. Two execution contexts, and USB's task between them:

1. **`AudioCallback()`**, the audio ISR, once per 24-sample block at 48 kHz: polls controls,
   reads MIDI (`MidiClock.h`), generates UI events, runs the engine, USB audio in and out.
2. **`MainLoop()`**: UI event dispatch, LEDs, the card (and lending it as a USB drive), USB
   answers, battery.

USB runs on TinyUSB (`UsbDevice.h`, #196): a UAC1 sound card, MIDI and the card as a drive, its
task on TIM7 at 4 kHz (priority 2, below the audio callback). TinyUSB is called only from that
task and the USB interrupt; `MainLoop()` posts what it wants. A change to USB gets `test/unit.sh
usbtransport` (and `usbdevice`) and a first flash of `make USB_SAFETY=1` before anything else.

**No file I/O and no blocking call from the audio ISR.** FRIZZ reads and writes the card only
from `MainLoop()`, and leaves it alone while it lends it to the computer (`CardLend.h`). It
reads the scenes and settings at boot and again, where the computer changed them, when it takes
the card back (#314), and loops when one is loaded (`LoopLoader.h`, the slots scanned at boot,
after a lend and on RESCAN). It writes when an FX scene is saved, copied or deleted, 2 s after
the finalizer's knobs, the mono input or the MIDI settings rest (`SceneStore.h`,
`MasterSettings.h`), when the loop transfer writes, moves or deletes a slot (`LoopTransfer.h`,
through `/FRIZZ/xfer.tmp`), or when a bug report is asked for (`EventLog.h`). Its files live in
`/FRIZZ` (`frizz_scenes.txt`, `frizz_master.txt`, `.bak` copies, `bug-N.txt`, the loops
`aN.wav`), which `EnterFrizzDir()` creates at boot on a card without it. MIDI: clock in over TRS and USB
(`MidiClock.h` → `TempoClock.h`, `TapTempo.h` as the fallback), notes, CCs, program changes and
FRIZZ's SysEx (`MidiControl.h`), answered over USB from `MainLoop()`. MIDI out
(`MidiOut.h`, off unless the settings page asks): the clock and the loop's transport, out of
the jack from the audio callback without waiting, over USB from TinyUSB's task (#265).

The play page is `NormalPage.h`; the engine is `passthroughEngine.h` → `Looper.h` +
`FxMorph.h` → `FxChain.h` → `Finalizer.h` → output gain → `limiter.h`. Large buffers live in
SDRAM (`DSY_SDRAM_BSS`), cleared by `ZeroSDRAM()` at boot because startup code doesn't; the
loop and the event log (`SDRAM_NOINIT`) aren't, as they're always written before read. Card
buffers live in internal RAM, 32-byte aligned, written in whole sectors (`EventLog::Flush`).

**Memory**: the firmware runs from SRAM, loaded by CHOMPI's bootloader (`APP_TYPE=BOOT_SRAM`).
Code has about **5.2 KB** left (`SRAM_EXEC`), the bench build **~6.0 KB** (2026-10-16, v1.5.0-beta.4; after the finalizer, #345: 7 KB, and libdaisy.a `-Os` where no interrupt runs it, #323: −4.5 KB), and data (`SRAM`) ~24.5 KB, the bench build's ~16.6 KB (after the code-room round, #320: FatFs `-Os`, no SPI driver); `docs/CAPACITY.md` lists every
region and the ways to make room. Never use `make program-boot`, and never swap in upstream
libDaisy: the vendored copy is patched (`THIRD_PARTY.md`).
`__attribute__((optimize("-O0")))` and similar per-function overrides (`ProcessAllControls`)
are deliberate workarounds, not leftovers.

## A new development machine

The repo carries everything the code and its checks need: `firmware/README.md`, *0. Set up a
development machine*. What lives outside it and moves by hand: Claude Code's project memory
(`~/.claude/projects/<the clone's path, slashes as dashes>/memory/`: keep the clone at the same
path, or rename that folder), the user's own Claude skills under `~/.claude/skills/`, and the
songs in `firmware/card/audio/`.
