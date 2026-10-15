#!/usr/bin/env python3
"""companion.py: the Companion page (the loop transfer in the browser, docs/LOOP_PROTOCOL.md) in a
headless Chromium against the real FRIZZ, over the DevTools protocol (CDP), for the release
test's browser items that need no eyes. The page's own words and its slot buttons are what it
reads; the loop transfer itself is remote.py loops' (and loopxfer.py's model).

    tools/companion.py slots [--url URL]            connect, list the slots the page shows
                                                    ("aria-label"), and which it says plays
    tools/companion.py playing BANK SLOT [--url URL] connect, pick that slot: exit 0 if the
                                                    page says FRIZZ is playing it (a cubbi too:
                                                    cubbi_a 3)
    tools/companion.py refused [--url URL]          Web MIDI refused (the permission denied):
                                                    what the page says
    tools/companion.py ports [--url URL]            the browser's MIDI ports, and FRIZZ's IDENT sent on
                                                    the "frizz" ones: exit 0 if it answers (the
                                                    page's Web MIDI without the page's search)
    tools/companion.py connect [--url URL]          only connect: what the page says (a FRIZZ
                                                    without the loop transfer: "doesn't answer")
    tools/companion.py upload BANK SLOT FILE... [--url URL]
                                                    files put into the slots from BANK SLOT on
                                                    (an empty slot each, or refused), the plan
                                                    written, its progress followed; then the
                                                    page's listing of them. --hide puts another
                                                    tab in front 3 s in (it keeps going)
    tools/companion.py eval JS [--url URL]          connect, then JS's value (the page's
                                                    debugging, no FRIZZ needed with --no-connect)

The browser: the Flatpak Chromium (org.chromium.Chromium, 155 here; Playwright's Chromium 149
crashed on Web MIDI's send) with --headless=new, a profile of its own under ~/.cache, closed
after every run. The Flatpak sees only the home directory: the files to upload live there.
Web MIDI's permission (midi with SysEx) is granted, or refused, to the page's origin through
CDP, so no prompt. While the page has FRIZZ's MIDI port, nothing else reaches FRIZZ: like the
other device tools it takes the CHOMPI's lock (tools/chompi.py), and in a hold it shares it.
It writes the card only with upload, and only into the slots named, which must be empty
(the page refuses nothing there; this does): a session's slots, never a player's loops.

Default URL: the site's /beta/ Companion (the newest pre-release's); FRIZZ_COMPANION_URL or
--url for another (a frizz-web dev server, the stable one).
Python 3 without packages: the WebSocket client is in here."""
import argparse
import base64
import json
import os
import random
import re
import shutil
import socket
import struct
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import chompi  # noqa: E402

URL = os.environ.get("FRIZZ_COMPANION_URL", "https://frizz.friedrichwagner.eu/beta/companion/")
BROWSER = os.environ.get("FRIZZ_CHROMIUM", "flatpak run org.chromium.Chromium").split()
BANKS = list("abcdefgh") + ["cubbi_" + c for c in "abcde"]
CUBBI_BANK = 8  # the page's first cubbi bank (app.js's CUBBI_BANK), as BANKS has it


# ======== a WebSocket client, enough for CDP ========

class WebSocket:
    """RFC 6455's client: text frames out, masked; whole messages in (continuations joined)"""

    def __init__(self, url, timeout=30.0):
        assert url.startswith("ws://")
        host, _, path = url[5:].partition("/")
        h, _, p = host.partition(":")
        self.sock = socket.create_connection((h, int(p or 80)), timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall(("GET /%s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (path, host, key)).encode())
        head = b""
        while b"\r\n\r\n" not in head:
            got = self.sock.recv(1)
            if not got:
                raise ConnectionError("the browser closed the WebSocket")
            head += got
        if b" 101 " not in head.split(b"\r\n")[0]:
            raise ConnectionError("no WebSocket: %r" % head[:80])
        self.buf = b""

    def send(self, text):
        data = text.encode()
        n = len(data)
        head = bytes([0x81])
        if n < 126:
            head += bytes([0x80 | n])
        elif n < 1 << 16:
            head += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            head += bytes([0x80 | 127]) + struct.pack(">Q", n)
        mask = os.urandom(4)
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def _read(self, n):
        while len(self.buf) < n:
            got = self.sock.recv(65536)
            if not got:
                raise ConnectionError("the browser closed the WebSocket")
            self.buf += got
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self):
        """The next whole text message"""
        parts = []
        while True:
            b0, b1 = self._read(2)
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._read(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._read(8))[0]
            mask = self._read(4) if b1 & 0x80 else None
            data = self._read(n)
            if mask:
                data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
            op = b0 & 0x0F
            if op == 8:
                raise ConnectionError("the browser closed the WebSocket")
            if op == 9:  # ping: pong
                self.sock.sendall(bytes([0x8A, 0x80 | len(data)]) + b"\0\0\0\0" + data)
                continue
            if op in (0, 1, 2):
                parts.append(data)
                if b0 & 0x80:
                    return b"".join(parts).decode()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# ======== the browser ========

class Browser:
    """A headless Chromium of its own, one page, driven over CDP; close() ends it"""

    def __init__(self, port=None):
        self.port = port or random.randint(20000, 29999)
        self.profile = os.path.join(os.path.expanduser("~"), ".cache", "frizz-companion", "profile-%d" % self.port)
        os.makedirs(self.profile, exist_ok=True)
        cmd = BROWSER + ["--headless=new", "--remote-debugging-port=%d" % self.port,
                         "--user-data-dir=" + self.profile, "--no-first-run", "--no-default-browser-check",
                         "about:blank"]
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                     start_new_session=True)
        self.ws = None
        self.seq = 0
        self.events = []
        t0 = time.monotonic()
        while time.monotonic() - t0 < 30:
            try:
                with urllib.request.urlopen("http://127.0.0.1:%d/json/list" % self.port, timeout=1) as r:
                    pages = [p for p in json.load(r) if p.get("type") == "page"]
                if pages:
                    self.ws = WebSocket(pages[0]["webSocketDebuggerUrl"])
                    break
            except (OSError, ValueError):
                pass
            time.sleep(0.2)
        if not self.ws:
            self.close()
            raise RuntimeError("Chromium didn't come up (%s)" % " ".join(BROWSER))
        with urllib.request.urlopen("http://127.0.0.1:%d/json/version" % self.port, timeout=2) as r:
            info = json.load(r)
        self.version = info.get("Browser", "?")
        self.browser_ws = WebSocket(info["webSocketDebuggerUrl"])

    def call(self, method, params=None, ws=None, timeout=30.0):
        ws = ws or self.ws
        self.seq += 1
        me = self.seq
        ws.send(json.dumps({"id": me, "method": method, "params": params or {}}))
        ws.sock.settimeout(timeout)
        while True:
            msg = json.loads(ws.recv())
            if msg.get("id") == me:
                if "error" in msg:
                    raise RuntimeError("%s: %s" % (method, msg["error"].get("message")))
                return msg.get("result", {})
            if "method" in msg:
                self.events.append(msg)

    def js(self, expr, timeout=30.0):
        """EXPR's value in the page (awaited if it's a promise)"""
        r = self.call("Runtime.evaluate", {"expression": expr, "awaitPromise": True, "returnByValue": True},
                      timeout=timeout)
        if "exceptionDetails" in r:
            d = r["exceptionDetails"]
            raise RuntimeError("in the page: %s" % (d.get("exception", {}).get("description") or d.get("text")))
        return r.get("result", {}).get("value")

    def permission(self, url, granted):
        """Web MIDI with SysEx for URL's origin: granted, or refused (the prompt answered no)"""
        origin = "/".join(url.split("/")[:3])
        if granted:
            self.call("Browser.grantPermissions", {"origin": origin, "permissions": ["midi", "midiSysex"]},
                      ws=self.browser_ws)
        else:
            for desc in ({"name": "midi", "sysex": True}, {"name": "midi"}):
                self.call("Browser.setPermission", {"origin": origin, "permission": desc, "setting": "denied"},
                          ws=self.browser_ws)

    def open(self, url):
        self.call("Page.enable")
        self.call("Runtime.enable")
        self.call("Page.navigate", {"url": url})
        self.wait("document.readyState === 'complete' && !!document.querySelector('[data-connect]')", 30)

    def wait(self, cond, secs, every=0.1):
        """Until the page's COND is truthy; its value, or None after SECS"""
        t0 = time.monotonic()
        while time.monotonic() - t0 < secs:
            try:
                v = self.js(cond)
            except RuntimeError:
                v = None
            if v:
                return v
            time.sleep(every)
        return None

    def close(self):
        """Browser.close, then the process: Flatpak's sandbox (bwrap) isn't in our process
        group, so the browser ends itself; waits until its port is closed"""
        if getattr(self, "browser_ws", None):
            try:
                self.browser_ws.send(json.dumps({"id": 0, "method": "Browser.close"}))
            except OSError:
                pass
        for ws in (getattr(self, "ws", None), getattr(self, "browser_ws", None)):
            if ws:
                ws.close()
        t0 = time.monotonic()
        while time.monotonic() - t0 < 15:
            try:
                socket.create_connection(("127.0.0.1", self.port), 0.5).close()
            except OSError:
                break
            time.sleep(0.3)
        try:
            os.killpg(self.proc.pid, 15)
        except OSError:
            pass
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(self.proc.pid, 9)
            except OSError:
                pass
        shutil.rmtree(self.profile, ignore_errors=True)


# ======== the page ========

# what the page shows, as one object
STATE_JS = r"""(() => {
  const c = document.querySelector('.connect');
  const t = (s) => (document.querySelector(s)?.textContent || '').trim();
  return {state: c?.dataset.state || '', text: t('[data-conn-text]'), detail: t('[data-conn-detail]'),
          index: t('[data-conn-index]'), empty: t('[data-empty-note]'),
          grid: !(document.querySelector('[data-grid]')?.hidden ?? true),
          slots: [...document.querySelectorAll('[data-grid] .slot')].length};
})()"""

SLOTS_JS = r"""[...document.querySelectorAll('[data-grid] .slot')].map(b => ({
  bank: +b.dataset.bank, slot: +b.dataset.slot, label: b.getAttribute('aria-label') || '',
  cls: b.className}))"""


def connect(br, url, granted=True):
    """Connect pressed: the page's state once it's done, its steps meanwhile in words printed
    (midi.js's search: the ports it opens, skips and asks), and on a failure what the page's
    console said"""
    br.permission(url, granted)
    br.open(url)
    br.js("document.querySelector('[data-connect]').click(), true")
    t0 = time.monotonic()
    seen = []
    st = None
    while time.monotonic() - t0 < 40:
        cur = br.js(STATE_JS)
        line = "%s: %s %s" % (cur["state"], cur["text"], cur["detail"])
        if not seen or seen[-1] != line:
            seen.append(line)
            print("  %5.2f s  %s" % (time.monotonic() - t0, line), flush=True)
        if cur["state"] not in ("busy", ""):
            st = cur
            break
        time.sleep(0.05)
    if st and st["state"] == "connected":
        # the listing read
        st = br.wait("(() => { const s = %s; return s.slots && !/Reading|reading/.test(s.empty) ? s : null })()"
                     % STATE_JS, 120) or st
    elif st:
        for e in br.events:
            if e.get("method") == "Runtime.consoleAPICalled":
                print("  console.%s: %s" % (e["params"].get("type"), " ".join(
                    str(a.get("value", a.get("description", ""))) for a in e["params"].get("args", []))))
            elif e.get("method") == "Runtime.exceptionThrown":
                print("  exception: %s" % e["params"]["exceptionDetails"].get("text"))
    return st or br.js(STATE_JS)


# Web MIDI's ports as the page's browser has them, and FRIZZ's IDENT (the developers' SysEx,
# chompi.py) sent on each "frizz" output, every SysEx that comes back on any input for 2 s
PORTS_JS = r"""(async () => {
  const a = await navigator.requestMIDIAccess({sysex: true});
  const d = (p) => ({type: p.type, name: p.name, maker: p.manufacturer, state: p.state, connection: p.connection});
  const got = [];
  const ins = [...a.inputs.values()];
  for (const i of ins) { try { await i.open(); } catch (e) { got.push('open ' + i.name + ': ' + e); }
    i.onmidimessage = (e) => { if (e.data[0] === 0xF0) got.push(i.name + ': ' + [...e.data].map(b => b.toString(16).padStart(2, '0')).join(' ')); }; }
  for (const o of a.outputs.values()) if (/frizz/i.test(o.name || '')) {
    try { await o.open(); o.send(%s); got.push('sent IDENT on ' + o.name); } catch (e) { got.push('send ' + o.name + ': ' + e); } }
  await new Promise((r) => setTimeout(r, 2000));
  return {ports: [...ins, ...a.outputs.values()].map(d), got};
})()"""


def tab(br, bank):
    """The tab a bank is on: FRIZZ's banks, or TAPE's cubbis"""
    which = "tape" if bank >= CUBBI_BANK else "frizz"
    br.js("document.querySelector('[data-tab=\"%s\"]').click(), true" % which)
    time.sleep(0.3)


def detail_of(br, bank, slot):
    """The detail panel's text once the slot's button is clicked"""
    tab(br, bank)
    ok = br.js("(() => { const b = document.querySelector('[data-grid] [data-bank=\"%d\"][data-slot=\"%d\"]');"
               " if (!b) return false; b.click(); return true })()" % (bank, slot))
    if not ok:
        return None
    time.sleep(0.3)
    return br.js("(document.querySelector('[data-detail]')?.innerText || '').trim()")


def slot_of(bank, slot):
    if bank.lower() not in BANKS or not slot.isdigit() or not 1 <= int(slot) <= 15:
        sys.exit("%s %s: a bank a-h or cubbi_a-cubbi_e and a slot 1-15" % (bank, slot))
    return BANKS.index(bank.lower()), int(slot) - 1


def name_of(b, s):
    return "%s%d" % (BANKS[b], s + 1)


def run(a):
    br = Browser()
    print("browser:", br.version, flush=True)
    try:
        if a.cmd == "ports":
            br.permission(a.url, True)
            br.open(a.url)
            ident = list(chompi.midi_send.HEADER) + [chompi.DEV, chompi.IDENT, 1, 0xF7]
            got = br.js(PORTS_JS % json.dumps(ident), timeout=30)
            for p in got["ports"]:
                print("  %-6s %-30s %-20s %s, %s" % (p["type"], p["name"], p["maker"], p["state"], p["connection"]))
            for line in got["got"]:
                print("  " + line)
            return 0 if any(": f0 7d 43 48" in g for g in got["got"]) else 1
        if a.cmd == "eval" and a.no_connect:
            br.open(a.url)
            print(json.dumps(br.js(a.js), indent=1))
            return 0
        st = connect(br, a.url, a.cmd != "refused")
        print("the page: %s: %s %s %s" % (st.get("state"), st.get("text"), st.get("detail"), st.get("index")))
        if a.cmd in ("refused", "connect"):
            return 0
        if st.get("state") != "connected":
            print("not connected", file=sys.stderr)
            return 1
        if a.cmd == "eval":
            print(json.dumps(br.js(a.js), indent=1))
            return 0
        if a.cmd == "slots":
            for b in (0, CUBBI_BANK):
                tab(br, b)
                for x in br.js(SLOTS_JS):
                    if "empty" not in x["cls"].split():
                        print("%-10s %s" % (name_of(x["bank"], x["slot"]), x["label"]))
            return 0
        if a.cmd == "playing":
            b, s = slot_of(a.bank, a.slot)
            text = detail_of(br, b, s)
            print("%s: %s" % (name_of(b, s), (text or "no such slot on the page").replace("\n", " | ")))
            return 0 if text and "FRIZZ is playing it" in text else 1
        if a.cmd == "upload":
            return upload(br, a)
    finally:
        br.close()
    return 0


def upload(br, a):
    b, s = slot_of(a.bank, a.slot)
    if b >= CUBBI_BANK:
        sys.exit("upload: into FRIZZ's banks a-h only")
    files = [os.path.abspath(p) for p in a.files]
    home = os.path.expanduser("~")
    for p in files:
        if not p.startswith(home + os.sep):
            sys.exit("%s: the Flatpak's Chromium sees only %s" % (p, home))
    tab(br, b)
    slots = {(x["bank"], x["slot"]): x for x in br.js(SLOTS_JS)}
    want = [(b, s + i) for i in range(len(files))]
    for at in want:
        x = slots.get(at)
        if not x or at[1] > 14 or "empty" not in x["cls"].split():
            sys.exit("upload: %s isn't an empty slot on the page: nothing done" % name_of(*at))
    # the slot picked, then the files at once through "Add files" (its input takes several),
    # as a hand adds a batch: the page plans them into that slot and the empty ones after it
    br.call("DOM.enable")
    detail_of(br, b, s)
    doc = br.call("DOM.getDocument", {"depth": 0})
    node = br.call("DOM.querySelector", {"nodeId": doc["root"]["nodeId"],
                                          "selector": "input[data-file-input]"})
    if not node.get("nodeId"):
        sys.exit("upload: no \"Add files\" on the page")
    br.call("DOM.setFileInputFiles", {"nodeId": node["nodeId"], "files": files})
    if not br.wait("document.querySelectorAll('[data-plan-list] > li').length >= %d" % len(files), 120):
        print("not all planned: %s" % br.js("(document.querySelector('[data-notes]')?.innerText || '').trim()"))
    plan = br.js("(document.querySelector('[data-plan-list]')?.innerText || '').trim()")
    print("the plan:\n  " + plan.replace("\n", "\n  "))
    # Review and write, then the dialog's "Write to FRIZZ"
    br.js("document.querySelector('[data-review]').click(), true")
    if not br.wait("document.querySelector('dialog[data-confirm]')?.open", 10):
        sys.exit("upload: no confirmation came up")
    print("confirm: %s" % br.js("document.querySelector('[data-confirm-list]').innerText.trim()").replace("\n", " / "))
    br.js("document.querySelector('dialog[data-confirm] button[value=write]').click(), true")
    t0 = time.monotonic()
    hidden = False
    last = None
    done = None
    while time.monotonic() - t0 < 900:
        prog = br.js("(() => { const t = (s) => (document.querySelector(s)?.innerText || '').trim();"
                     " return {title: t('[data-progress-title]'), count: t('[data-progress-count]'),"
                     " bytes: t('[data-progress-bytes]'), step: t('[data-progress-step]'),"
                     " wait: document.querySelector('[data-progress-wait]')?.hidden ? '' : t('[data-progress-wait]'),"
                     " report: t('[data-progress-report]')} })()")
        line = "%s, %s: %s %s %s" % (prog["title"], prog["count"], prog["step"], prog["bytes"], prog["wait"])
        if line != last:
            print("  %5.1f s  %s" % (time.monotonic() - t0, line), flush=True)
            last = line
        if a.hide and not hidden and time.monotonic() - t0 > 3:
            # another tab in front, as a hand switches tabs: the page's own visibility as the
            # browser has it then, said
            other = br.call("Target.createTarget", {"url": "about:blank"}, ws=br.browser_ws)["targetId"]
            br.call("Target.activateTarget", {"targetId": other}, ws=br.browser_ws)
            time.sleep(0.5)
            print("  another tab in front: the page's visibility %s" % br.js("document.visibilityState"), flush=True)
            hidden = other
        if prog["title"] in ("Done", "Stopped"):
            done = prog
            break
        time.sleep(0.5)
    print("report: %s" % ((done or {}).get("report") or "none (timed out)").replace("\n", " / "))
    if hidden:
        br.call("Target.closeTarget", {"targetId": hidden}, ws=br.browser_ws)
    time.sleep(1)
    tab(br, b)
    for x in br.js(SLOTS_JS):
        if (x["bank"], x["slot"]) in want:
            print("%-6s %s" % (name_of(x["bank"], x["slot"]), x["label"]))
    return 0 if done and done["title"] == "Done" else 1



def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--url", default=URL)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("slots", parents=[common])
    p = sub.add_parser("playing", parents=[common])
    p.add_argument("bank")
    p.add_argument("slot")
    sub.add_parser("refused", parents=[common])
    sub.add_parser("ports", parents=[common])
    sub.add_parser("connect", parents=[common])
    p = sub.add_parser("upload", parents=[common])
    p.add_argument("bank")
    p.add_argument("slot")
    p.add_argument("files", nargs="+")
    p.add_argument("--hide", action="store_true", help="another tab in front 3 s in, until it's done")
    p = sub.add_parser("eval", parents=[common])
    p.add_argument("js")
    p.add_argument("--no-connect", action="store_true")
    a = ap.parse_args()
    if not (a.cmd == "eval" and a.no_connect):
        chompi.claim()
    sys.exit(run(a))


if __name__ == "__main__":
    main()
