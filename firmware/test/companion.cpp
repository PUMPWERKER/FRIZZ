// companion.cpp: firmware/tools/companion.py without a browser and without the CHOMPI: its
// WebSocket client against a stand-in server here (the handshake, a masked frame out, frames in
// of each length's encoding, a message in two fragments, a ping answered, the close), its CDP
// call (events kept while it waits for its answer, an error raised), and what it refuses before
// anything opens: a slot that isn't one, an upload into TAPE's banks, a file the Flatpak's
// browser can't see. Python 3 only.
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "check.h"
#include "pytool.h"

static const char* kCode = R"(
import json, os, socket, struct, sys, threading, types
import companion as C

srv = socket.socket()
srv.bind(("127.0.0.1", 0))
srv.listen(1)
port = srv.getsockname()[1]
seen = {}

def frame(op, data, fin=True):
    n = len(data)
    head = bytes([(0x80 if fin else 0) | op])
    if n < 126:
        head += bytes([n])
    elif n < 1 << 16:
        head += bytes([126]) + struct.pack(">H", n)
    else:
        head += bytes([127]) + struct.pack(">Q", n)
    return head + data

def server():
    c, _ = srv.accept()
    req = b""
    while b"\r\n\r\n" not in req:
        req += c.recv(1)
    seen["path"] = req.split(b" ")[1].decode()
    c.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n")
    b0, b1 = c.recv(2)
    n = b1 & 0x7F
    if n == 126:
        n = struct.unpack(">H", c.recv(2))[0]
    mask = c.recv(4)
    data = b""
    while len(data) < n:
        data += c.recv(n - len(data))
    seen["masked"] = bool(b1 & 0x80)
    msg = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    c.sendall(frame(1, msg))                       # the echo, 126's length
    c.sendall(frame(1, b"x" * 70000))              # 127's
    c.sendall(frame(1, b'{"part": ', fin=False) + frame(0, b'"two"}'))
    c.sendall(frame(9, b"hi") + frame(1, b"after the ping"))
    pong = c.recv(2 + 4 + 2)
    seen["pong"] = pong[0] == 0x8A
    c.sendall(frame(8, b""))
    c.close()

th = threading.Thread(target=server, daemon=True)
th.start()
ws = C.WebSocket("ws://127.0.0.1:%d/devtools/page/X" % port)
text = "hello, " * 30
ws.send(text)
print("ECHO", ws.recv() == text, len(text))
print("BIG", len(ws.recv()))
print("FRAGMENTS", json.loads(ws.recv())["part"])
print("PING", ws.recv())
try:
    ws.recv()
    print("CLOSE no")
except ConnectionError:
    print("CLOSE raised")
th.join(5)
print("PATH", seen.get("path"), "MASKED", seen.get("masked"), "PONG", seen.get("pong"))

class FakeWs:
    def __init__(self, replies):
        self.out, self.replies = [], replies
        self.sock = types.SimpleNamespace(settimeout=lambda t: None)
    def send(self, t):
        self.out.append(json.loads(t))
    def recv(self):
        return json.dumps(self.replies.pop(0)(self.out[-1]))

br = object.__new__(C.Browser)
br.seq, br.events = 0, []
br.ws = FakeWs([lambda m: {"method": "Page.loadEventFired", "params": {}},
                lambda m: {"id": m["id"], "result": {"result": {"value": 42}}},
                lambda m: {"id": m["id"], "error": {"message": "no such node"}}])
print("JS", br.js("6 * 7"), "EVENTS", [e["method"] for e in br.events], br.ws.out[0]["method"],
      br.ws.out[0]["params"]["awaitPromise"])
try:
    br.call("DOM.querySelector", {})
    print("ERROR none")
except RuntimeError as e:
    print("ERROR", e)

print("SLOT", C.slot_of("cubbi_a", "3"), C.slot_of("H", "15"), C.name_of(8, 2), C.name_of(7, 12))
for bank, slot in (("i", "1"), ("a", "16"), ("a", "x")):
    try:
        C.slot_of(bank, slot)
        print("REFUSED no", bank, slot)
    except SystemExit as e:
        print("REFUSED", bank, slot, e)

def upload(bank, files):
    a = types.SimpleNamespace(bank=bank, slot="13", files=files, hide=None)
    try:
        C.upload(None, a)
        print("UPLOAD ran")
    except SystemExit as e:
        print("UPLOAD", e)
    except AttributeError:
        print("UPLOAD reached the browser")
upload("cubbi_a", [os.path.expanduser("~/x.wav")])
upload("h", ["/tmp/x.wav"])
upload("h", [os.path.expanduser("~/x.wav")])
)";

int main()
{
    firmware_dir = std::string(__FILE__).substr(0, std::string(__FILE__).rfind('/')) + "/..";
    char dir[] = "/tmp/frizz-companion-XXXXXX";
    tmp_dir = mkdtemp(dir);
    std::string out;
    const int rc = Python(kCode, out);
    printf("%s%s", out.c_str(), rc ? "\n(the Python above exited with an error)\n" : "");

    Check(Has(out, "ECHO True 210") && Has(out, "PATH /devtools/page/X MASKED True"),
          "companion: the WebSocket's handshake to the page's path, a masked frame out (126's length) and its echo back");
    Check(Has(out, "BIG 70000") && Has(out, "FRAGMENTS two"),
          "companion: a frame in with 127's length, a message in two fragments joined");
    Check(Has(out, "PING after the ping") && Has(out, "PONG True") && Has(out, "CLOSE raised"),
          "companion: a ping answered with a pong, the close frame ends it");
    Check(Has(out, "JS 42 EVENTS ['Page.loadEventFired'] Runtime.evaluate True"),
          "companion: a CDP call's answer by its id, the events before it kept; JS awaited");
    Check(Has(out, "ERROR DOM.querySelector: no such node"), "companion: a CDP error raised with its method");
    Check(Has(out, "SLOT (8, 2) (7, 14) cubbi_a3 h13"), "companion: slots by bank and number, as the page numbers them");
    Check(Has(out, "REFUSED i 1") && Has(out, "REFUSED a 16") && Has(out, "REFUSED a x") && !Has(out, "REFUSED no"),
          "companion: no bank i, no slot 16, no slot x");
    Check(Has(out, "UPLOAD upload: into FRIZZ's banks a-h only"), "companion: no upload into TAPE's cubbis");
    Check(Has(out, "UPLOAD /tmp/x.wav: the Flatpak's Chromium sees only"),
          "companion: a file outside the home directory refused before the browser is asked");
    Check(Has(out, "UPLOAD reached the browser"), "companion: one in the home directory goes on to the page");
    return Finish();
}
