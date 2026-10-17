#!/usr/bin/env python3
"""release-check.py: release-check.yml's steps (#440). It reads the repository's releases, and
writes nothing: no release, no asset, no tag, never the rolling `beta`. A mismatch fails
loudly (::error::, exit 1), saying it in the job summary too.

    release-check.py plan [TAG]        what to check: TAG's release (a release event's, or the
                                       one asked for by hand) and the rolling beta; with no TAG
                                       (the schedule) the latest stable release and the newest
                                       numbered pre-release after it (what the website serves),
                                       and beta. Writes tags=JSON and beta=true|false to
                                       GITHUB_OUTPUT
    release-check.py release TAG MD5   TAG's release's FRIZZ.bin against CI's build of the tag
                                       (MD5): the same bytes, or it fails; a published release
                                       without one fails, a draft without one is left alone
    release-check.py beta              the rolling beta's FRIZZ.bin (the fixed link testers
                                       download) against the numbered pre-release (vX.Y.Z-beta.N)
                                       whose tag is on beta's commit: the same md5, which beta's
                                       notes name; beta behind the newest numbered one is a
                                       warning (it moves only with a new one, CLAUDE.md)

GH_REPO names the repository; the GitHub CLI (gh api, GET only) does the reading, with
GH_TOKEN. Python 3 without packages.
"""
import hashlib
import json
import os
import re
import subprocess
import sys

VERSION = re.compile(r"v(\d+)\.(\d+)\.(\d+)(?:-beta\.([1-9]\d*))?")
BIN = "FRIZZ.bin"


def fail(text, error):
    say("**%s**" % text)
    print("::error::%s" % error)
    sys.exit(1)


def say(text):
    print(text)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as f:
            f.write(text + "\n\n")


def warn(text):
    say(text)
    print("::warning::%s" % text)


def gh(path, raw=False, paginate=False):
    """gh api PATH (a GET): its JSON (a paginated listing's pages joined), or RAW bytes"""
    args = ["gh", "api"] + (["-H", "Accept: application/octet-stream"] if raw else []) \
        + (["--paginate"] if paginate else []) + [path]
    r = subprocess.run(args, capture_output=True)
    if r.returncode != 0:
        sys.exit("::error::gh api %s: %s" % (path, r.stderr.decode(errors="replace").strip()))
    if raw:
        return r.stdout
    # --paginate prints each page's JSON one after another
    text, at, pages = r.stdout.decode(), 0, []
    dec = json.JSONDecoder()
    while True:
        while at < len(text) and text[at].isspace():
            at += 1
        if at == len(text):
            break
        page, at = dec.raw_decode(text, at)
        pages.append(page)
    if paginate:
        return [x for page in pages for x in (page if isinstance(page, list) else [page])]
    return pages[0] if pages else None


def repo():
    r = os.environ.get("GH_REPO")
    if not r:
        sys.exit("GH_REPO names the repository (owner/name)")
    return r


def releases():
    return gh("repos/%s/releases" % repo(), paginate=True)


def version(tag):
    """(major, minor, patch, 1 for a release / 0 for a beta, N): sorts a release after its
    betas; None for another tag"""
    m = VERSION.fullmatch(tag or "")
    return m and (int(m[1]), int(m[2]), int(m[3]), 0 if m[4] else 1, int(m[4] or 0))


def asset(rel):
    return next((a for a in rel.get("assets", []) if a["name"] == BIN), None)


def asset_md5(a):
    return hashlib.md5(gh("repos/%s/releases/assets/%s" % (repo(), a["id"]), raw=True)).hexdigest()


def commit(tag):
    """The commit TAG is on (an annotated tag's too)"""
    obj = gh("repos/%s/git/ref/tags/%s" % (repo(), tag))["object"]
    while obj["type"] == "tag":
        obj = gh("repos/%s/git/tags/%s" % (repo(), obj["sha"]))["object"]
    return obj["sha"]


def plan(tag):
    beta = True
    if tag == "beta":
        tags = []
    elif tag:
        if not version(tag):
            say("%s isn't a version's tag nor beta: nothing to check" % tag)
            tags, beta = [], False
        else:
            tags = [tag]
    else:
        published = [r for r in releases() if not r["draft"] and version(r["tag_name"])]
        stable = max((r for r in published if not r["prerelease"] and version(r["tag_name"])[3]),
                     key=lambda r: version(r["tag_name"]), default=None)
        pre = max((r for r in published if r["prerelease"] and not version(r["tag_name"])[3]),
                  key=lambda r: version(r["tag_name"]), default=None)
        tags = [r["tag_name"] for r in (stable, pre) if r]
        if stable and pre and version(pre["tag_name"]) < version(stable["tag_name"]):
            tags = [stable["tag_name"]]
    say("checking: %s%s" % (", ".join(tags) or "no release's build", ", and beta" if beta else ""))
    out = os.environ.get("GITHUB_OUTPUT")
    if out:
        with open(out, "a") as f:
            f.write("tags=%s\nbeta=%s\n" % (json.dumps(tags), "true" if beta else "false"))
    else:
        print("tags=%s\nbeta=%s" % (json.dumps(tags), "true" if beta else "false"))


def release(tag, md5):
    found = [r for r in releases() if r["tag_name"] == tag]
    if not found:
        say("No release for %s: nothing to check" % tag)
        return
    if len(found) > 1:
        fail("%s has %d releases" % (tag, len(found)), "%s has %d releases (%s)" % (
            tag, len(found), ", ".join(str(r["id"]) for r in found)))
    rel = found[0]
    kind = "draft release" if rel["draft"] else "release"
    a = asset(rel)
    if a is None:
        if rel["draft"]:
            say("%s's draft release has no %s yet: nothing to check (release.yml attaches CI's)" % (tag, BIN))
            return
        fail("%s's release is published without a %s: the fixed link and the website have none" % (tag, BIN),
             "%s's published release has no %s" % (tag, BIN))
    have = asset_md5(a)
    if have != md5:
        fail("%s's %s carries a %s that isn't the tag's build: its md5 `%s`, CI's `%s` (GCC 10.3-2021.10 at "
             "%s). Build it again in a clean worktree of the tag (CLAUDE.md, *Releases*) before anyone installs it"
             % (tag, kind, BIN, have, md5, tag),
             "%s's %s (md5 %s) isn't CI's build of the tag (md5 %s)" % (tag, BIN, have, md5))
    say("%s's %s carries %s md5 `%s`: the same bytes CI built from the tag." % (tag, kind, BIN, md5))


def beta():
    rels = releases()
    b = next((r for r in rels if r["tag_name"] == "beta"), None)
    if b is None:
        say("No rolling beta release: nothing to check")
        return
    a = asset(b)
    if a is None:
        fail("The rolling beta carries no %s: its fixed link is dead" % BIN, "beta has no %s" % BIN)
    have = asset_md5(a)
    at = commit("beta")
    numbered = sorted((r for r in rels if not r["draft"] and r["prerelease"] and version(r["tag_name"])
                       and not version(r["tag_name"])[3]), key=lambda r: version(r["tag_name"]), reverse=True)
    # the newest ten are enough: beta carries the newest one
    on = [r for r in numbered[:10] if commit(r["tag_name"]) == at]
    if not on:
        fail("The rolling beta is on %s, where no numbered pre-release (vX.Y.Z-beta.N) is: it moves only with "
             "a new one (CLAUDE.md, *Releases*)" % at[:12],
             "beta's commit %s carries no numbered pre-release" % at[:12])
    n = on[0]
    na = asset(n)
    if na is None:
        fail("%s, the numbered pre-release on beta's commit, has no %s" % (n["tag_name"], BIN),
             "%s has no %s" % (n["tag_name"], BIN))
    theirs = asset_md5(na)
    if have != theirs:
        fail("The rolling beta's %s isn't %s's: its md5 `%s`, %s's `%s`. Upload %s's again (`gh release upload "
             "beta … --clobber`, CLAUDE.md) before anyone downloads it" % (BIN, n["tag_name"], have, n["tag_name"],
                                                                           theirs, n["tag_name"]),
             "beta's %s (md5 %s) isn't %s's (md5 %s)" % (BIN, have, n["tag_name"], theirs))
    named = set(re.findall(r"\b[0-9a-f]{32}\b", b.get("body") or ""))
    if have not in named:
        fail("The rolling beta's notes don't name its build's md5 `%s` (%s)" % (
            have, "they name %s" % ", ".join("`%s`" % m for m in sorted(named)) if named else "they name none"),
            "beta's notes don't name md5 %s" % have)
    if n["tag_name"] not in (b.get("body") or "") + (b.get("name") or ""):
        warn("The rolling beta's title and notes don't name %s, the build it carries" % n["tag_name"])
    if numbered and numbered[0]["tag_name"] != n["tag_name"]:
        warn("The rolling beta carries %s, but %s is newer: move beta to it (CLAUDE.md, *Releases*)" % (
            n["tag_name"], numbered[0]["tag_name"]))
    say("The rolling beta carries %s md5 `%s`: %s's build, on its commit %s, the md5 its notes name." % (
        BIN, have, n["tag_name"], at[:12]))


def main():
    a = sys.argv[1:]
    if a[:1] == ["plan"] and len(a) <= 2:
        plan(a[1] if len(a) == 2 else "")
    elif a[:1] == ["release"] and len(a) == 3:
        release(a[1], a[2])
    elif a == ["beta"]:
        beta()
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
