#!/usr/bin/env python3
"""platform-sync.py: FRIZZ's copy of the PUMPWERKER platform, firmware/platform/ (#477).

    tools/platform-sync.py COMMIT   replaces firmware/platform/ with COMMIT's files, writes its
                                    hash to firmware/platform/PIN and stages both (git add): then
                                    commit them
    tools/platform-sync.py --check  compares firmware/platform/ with PIN's commit, file by file,
                                    content and mode, both ways: exits 0 if it's that commit, 1 if
                                    a file is missing, changed or extra (each named), 2 if it
                                    couldn't compare

The platform is a repository of its own, github.com/PUMPWERKER/platform (README.md there). FRIZZ
doesn't take it as a submodule but as real files, so a FRIZZ commit builds from FRIZZ alone
(git worktree add, git archive, test/run.sh, twin/ref.sh, CI's base, flash.py's clean tag),
and PIN says which platform commit they are. A change to the platform goes there first, then
comes in here by this script; nobody edits firmware/platform/ by hand, as --check (the host
check `platform`, all.sh) fails until it's the pinned commit again.

Where the commit comes from: a clone of the platform next to FRIZZ's checkout (../platform,
beside the folder that holds FRIZZ's .git; PLATFORM_REPO=DIR names another), if it has the
commit, else fetched from GitHub (PLATFORM_URL) into a cache (test/build/platform.git,
PLATFORM_CACHE). A commit to copy in must be on GitHub, where CI fetches it: one that isn't in
a branch or tag there is refused (--unpushed copies it anyway, for a try that isn't committed).
--check without the network and without the commit in a clone says "SKIP: local only" and
passes; on GitHub Actions it fails instead. Python 3 and git, nothing else; --dir DIR works on
another copy than firmware/platform/ (the host check's own cases).
"""
import argparse
import fcntl
import hashlib
import os
import shutil
import stat
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.dirname(HERE)
URL = os.environ.get("PLATFORM_URL", "https://github.com/PUMPWERKER/platform.git")
CACHE = os.environ.get("PLATFORM_CACHE", os.path.join(FIRMWARE, "test", "build", "platform.git"))
PIN = "PIN"


def git(*args, cwd=None, check=True, data=None):
    """git's stdout (bytes); exits with its message if it fails and check"""
    # its messages in English, whatever the machine's language: they're quoted in ours
    r = subprocess.run(["git"] + list(args), cwd=cwd, input=data, capture_output=True,
                       env=dict(os.environ, LC_ALL="C"))
    if check and r.returncode:
        sys.exit("git %s: %s" % (" ".join(args), r.stderr.decode(errors="replace").strip()))
    return r.stdout if check else r


def clone():
    """the platform's clone next to FRIZZ's checkout, or PLATFORM_REPO's; None without one"""
    path = os.environ.get("PLATFORM_REPO")
    if not path:
        common = git("rev-parse", "--path-format=absolute", "--git-common-dir", cwd=FIRMWARE, check=False)
        if common.returncode:
            return None
        path = os.path.join(os.path.dirname(os.path.dirname(common.stdout.decode().strip())), "platform")
    return path if os.path.isdir(path) and not git("rev-parse", "--git-dir", cwd=path, check=False).returncode else None


def has(repo, commit):
    return not git("cat-file", "-e", commit + "^{commit}", cwd=repo, check=False).returncode


def full(repo, commit):
    return git("rev-parse", "--verify", commit + "^{commit}", cwd=repo).decode().strip()


def fetch(commit):
    """the cache with commit fetched from URL: (cache, None), or (None, why) when URL couldn't be
    reached, or (cache, why) when it was but hasn't the commit"""
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    with open(CACHE + ".lock", "w") as lock:
        # checks run side by side (all.sh): one fetch at a time
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not os.path.isdir(CACHE):
            git("init", "-q", "--bare", CACHE)
        if has(CACHE, commit):
            return CACHE, None
        r = git("fetch", "-q", URL, "+refs/heads/*:refs/remotes/origin/*", "+refs/tags/*:refs/tags/*",
                cwd=CACHE, check=False)
        if r.returncode:
            err = r.stderr.decode(errors="replace").strip().splitlines()
            return None, "couldn't fetch %s%s" % (URL, ": " + err[-1] if err else "")
        if not has(CACHE, commit):
            # a commit in no branch or tag (a pull request's): GitHub hands it out by its hash
            git("fetch", "-q", URL, commit, cwd=CACHE, check=False)
        if not has(CACHE, commit):
            return CACHE, "%s has no commit %s" % (URL, commit)
        return CACHE, None


def source(commit):
    """a repository with commit: the clone if it has it, else the cache, fetched (see fetch)"""
    repo = clone()
    if repo and has(repo, commit):
        return repo, None
    return fetch(commit)


def pushed(repo, commit):
    """True if commit is in a branch or tag of the platform on GitHub: in the cache, anything it
    has came from there; in a clone, a remote branch or a tag must hold it"""
    if repo == CACHE:
        return True
    return bool(git("for-each-ref", "--contains", commit, "refs/remotes", "refs/tags", cwd=repo).strip())


def tree(repo, commit):
    """{path: (mode, blob)} of commit's files"""
    files = {}
    for entry in git("ls-tree", "-r", "-z", "--full-tree", commit, cwd=repo).split(b"\0"):
        if not entry:
            continue
        meta, path = entry.split(b"\t", 1)
        mode, kind, blob = meta.decode().split()
        if kind != "blob":
            sys.exit("%s in %s is a %s, not a file: the copy holds files only" % (path.decode(), commit, kind))
        files[path.decode()] = (mode, blob)
    return files


def blob_of(path):
    """(mode, blob) of a file on disk, as git would store it"""
    st = os.lstat(path)
    if stat.S_ISLNK(st.st_mode):
        mode, data = "120000", os.readlink(path).encode()
    else:
        mode = "100755" if st.st_mode & 0o111 else "100644"
        with open(path, "rb") as f:
            data = f.read()
    return mode, hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def on_disk(folder):
    """the files of the copy, relative to it, PIN left out: git's view where the copy is in a
    work tree (tracked files, and untracked ones it doesn't ignore), else every file"""
    r = git("ls-files", "-z", "--cached", "--others", "--exclude-standard", "--full-name", "--", ".",
            cwd=folder, check=False)
    if r.returncode:
        names = [os.path.relpath(os.path.join(d, f), folder) for d, _, fs in os.walk(folder) for f in fs]
    else:
        top = git("rev-parse", "--show-prefix", cwd=folder).decode().strip()
        names = [n.decode()[len(top):] for n in r.stdout.split(b"\0") if n]
        names = [n for n in names if os.path.lexists(os.path.join(folder, n))]
    return sorted(set(n for n in names if n != PIN))


def differences(folder, files):
    """what the copy has other than files: lines naming each path"""
    out = []
    have = on_disk(folder)
    for path in have:
        if path not in files:
            out.append("extra:   %s (not in the platform's commit)" % path)
        elif blob_of(os.path.join(folder, path)) != files[path]:
            mode, blob = blob_of(os.path.join(folder, path))
            out.append("changed: %s%s" % (path, " (mode %s, the commit's %s)" % (mode, files[path][0])
                                           if blob == files[path][1] else ""))
    for path in sorted(set(files) - set(have)):
        out.append("missing: %s" % path)
    return out


def read_pin(folder):
    try:
        with open(os.path.join(folder, PIN)) as f:
            pin = f.read().strip()
    except OSError:
        sys.exit("%s: no %s: copy a platform commit in first (tools/platform-sync.py COMMIT)" % (folder, PIN))
    if len(pin) != 40 or any(c not in "0123456789abcdef" for c in pin):
        sys.exit("%s/%s: %r isn't a commit's hash" % (folder, PIN, pin))
    return pin


def check(folder):
    pin = read_pin(folder)
    repo, why = source(pin)
    if repo is None:
        if os.environ.get("GITHUB_ACTIONS") == "true":
            print("FAIL  the copy against PIN %s: %s" % (pin[:12], why))
            return 2
        print("SKIP: local only (the network, or PIN's commit %s in a clone of the platform: %s)" % (pin[:12], why))
        return 0
    if why:
        print("FAIL  PIN %s: %s" % (pin[:12], why))
        return 1
    diff = differences(folder, tree(repo, pin))
    for line in diff:
        print(line)
    rel = os.path.relpath(folder)
    if diff:
        print("FAIL  %s isn't the platform's commit %s: %d file(s) differ (above)" % (rel, pin[:12], len(diff)))
        return 1
    if not pushed(repo, pin):
        print("note: PIN's commit %s isn't in a branch or tag of the platform on GitHub (%s): CI won't find it"
              % (pin[:12], repo))
    print("%s is the platform's commit %s, file for file" % (rel, pin[:12]))
    return 0


def sync(folder, commit, unpushed):
    repo = clone()
    if not (repo and has(repo, commit)):
        repo, why = fetch(commit)
        if why:
            sys.exit(why)
    commit = full(repo, commit)
    if not unpushed and not pushed(repo, commit):
        sys.exit("%s isn't in a branch or tag of the platform on GitHub (%s), where CI fetches it: push it "
                 "first (--unpushed copies it anyway, not to be committed)" % (commit[:12], repo))
    files = tree(repo, commit)
    # the copy is the commit's, nothing else: the folder emptied, every file written from its blob
    # as it is, no attribute applied (git archive would: export-ignore)
    if os.path.lexists(folder):
        shutil.rmtree(folder)
    paths = sorted(files)
    data = git("cat-file", "--batch", cwd=repo, data="".join(files[p][1] + "\n" for p in paths).encode())
    at = 0
    for p in paths:
        mode, blob = files[p]
        head_end = data.index(b"\n", at)
        size = int(data[at:head_end].split()[2])
        body = data[head_end + 1:head_end + 1 + size]
        at = head_end + 1 + size + 1
        path = os.path.join(folder, p)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if mode == "120000":
            os.symlink(body.decode(), path)
            continue
        with open(path, "wb") as f:
            f.write(body)
        os.chmod(path, 0o755 if mode == "100755" else 0o644)
    with open(os.path.join(folder, PIN), "w") as f:
        f.write(commit + "\n")
    staged = not git("rev-parse", "--git-dir", cwd=folder, check=False).returncode
    if staged:
        # by name and forced: a .gitignore (FRIZZ's, or one in the copy) never drops one of its files
        git("add", "-A", "--", ".", cwd=folder)
        git("add", "-f", "--pathspec-from-file=-", "--pathspec-file-nul", cwd=folder,
            data=b"\0".join(p.encode() for p in paths + [PIN]))
    diff = differences(folder, files)
    if diff:
        sys.exit("the copy came out other than the commit:\n" + "\n".join(diff))
    what = "%s: the platform's commit %s, %d files" % (os.path.relpath(folder), commit[:12], len(files))
    if staged:
        changed = git("diff", "--cached", "--shortstat", "--", ".", cwd=folder).decode().strip()
        what += ", staged (%s): commit it" % (changed or "no change")
    print(what)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("commit", nargs="?", help="the platform commit (hash, branch or tag) to copy in")
    ap.add_argument("--check", action="store_true", help="compare the copy with PIN's commit")
    ap.add_argument("--unpushed", action="store_true", help="copy in a commit that isn't on GitHub")
    ap.add_argument("--dir", default=os.path.join(FIRMWARE, "platform"), help="the copy (firmware/platform)")
    a = ap.parse_args()
    folder = os.path.abspath(a.dir)
    if a.check == bool(a.commit):
        ap.error("a COMMIT to copy in, or --check")
    return check(folder) if a.check else sync(folder, a.commit, a.unpushed)


if __name__ == "__main__":
    sys.exit(main())
