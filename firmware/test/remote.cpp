// remote.cpp: firmware/remote.py against the virtual CHOMPI (twin/twin.h). The twin's USB MIDI
// sits on a pseudo-terminal in place of the CHOMPI's ALSA node, and remote.py runs as itself
// (--device), on the twin's clock (ptytwin.h's lockstep): remote.py's SysEx, its parsing of
// the answers and its script player are checked, end to end, against the firmware's.
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "../code/src/Crash.h"
#include "check.h"
#include "script.h"
#include "twin.h"
#include "ptytwin.h"

using namespace twin;

// the firmware's crash record (chompi_main.cpp), an ordinary static on the twin
extern chompi::crash::Backup crash_mem;

static const int kFilterKeyLed = 20;
static const int kPlayLed = 7; // PanelLeds.h

static std::string remote_py;

// the loop transfer's CHUNKs (0x38 0x11) the pty loses on the way while usb_drop is DropChunk
// (ptytwin.h): each kDropEvery-th offset (98 bytes a CHUNK, kMaxChunk), on its first sending
// only. By the offset, not by the count of CHUNKs seen: go-back-N sends the same CHUNKs again
// after a gap, and every n-th one seen can be the same offset each round, the file never whole
// (#315)
static const uint32_t kDropEvery = 7;
static int chunks_dropped = 0;
static std::set<uint32_t> dropped;

static bool DropChunk(const std::string& m)
{
    if (m.size() < 12 || m.compare(0, 6, "\xF0\x7D\x43\x48\x38\x11"))
        return false;
    uint32_t offset = 0;
    for (int i = 7; i < 11; i++)
        offset = offset << 7 | (static_cast<uint8_t>(m[i]) & 0x7F);
    if ((offset / 98) % kDropEvery != kDropEvery - 1 || !dropped.insert(offset).second)
        return false;
    chunks_dropped++;
    return true;
}

// the twin's time each TURN of knob 4 came in (F0 7D 43 48 12 04 DIR F7), seen on the way
// while usb_drop is SeeTurn (nothing lost): the gaps the firmware measures (ui.h's Gap)
static std::vector<uint32_t> turns_at;

static bool SeeTurn(const std::string& m)
{
    if (m.size() == 8 && !m.compare(0, 6, "\xF0\x7D\x43\x48\x12\x04"))
        turns_at.push_back(NowMs());
    return false;
}

/** Runs ms of silence, as fast as the twin goes: what remote.py left on the pty in, the twin's
 *  answers out */
static void RunMs(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t++)
    {
        uint8_t buf[256];
        ssize_t n;
        while ((n = read(usb.fd, buf, sizeof(buf))) > 0)
            for (ssize_t i = 0; i < n; i++)
                ToTwinUsb(buf[i]);
        FromTwin();
        Run(2, nullptr, nullptr);
    }
}
#include "twintest.h" // after RunMs, which its Tap runs

/** In an LED line (LedLine, remote.py leds), where PLAY's colour is: after "pth " and the LEDs
 *  before it, 7 characters each. npos in another line */
static size_t PlayAt(const std::string& line)
{
    const size_t at = 4 + 7 * kPlayLed;
    return line.compare(0, 4, "pth ") == 0 && line.size() >= at + 6 ? at : std::string::npos;
}
static std::string PlayLed(const std::string& line)
{
    const size_t at = PlayAt(line);
    return at == std::string::npos ? "" : line.substr(at, 6);
}
static std::string WithoutPlay(std::string line)
{
    const size_t at = PlayAt(line);
    if (at != std::string::npos)
        line.replace(at, 6, "------");
    return line;
}

/** ARG quoted for sh */
static std::string Quoted(const std::string& arg)
{
    std::string q = "'";
    for (char c : arg)
        q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}

/** How often WHAT is in S */
static size_t Count(const std::string& s, const std::string& what)
{
    size_t n = 0;
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1))
        n++;
    return n;
}

/** remote.py ARGS against the twin until it's done (30 s at most, ptytwin.h's Exec),
 *  interrupted (SIGINT, Ctrl-C) once it has printed interrupt_at, if given. Its exit code; out
 *  gets what it printed, stdout and stderr */
static int Remote(const std::vector<std::string>& args, std::string& out,
                  const std::string& interrupt_at = "")
{
    std::string cmd = "exec python3 " + Quoted(remote_py) + " --device " + Quoted(usb.path);
    for (const std::string& a : args)
        cmd += " " + Quoted(a);
    return Exec(cmd, out, 30, {}, interrupt_at, interrupt_at.empty() ? 0 : SIGINT);
}

static void Write(const std::string& path, const std::string& text)
{
    std::ofstream(path) << text;
}

/** A WAV of 16-bit PCM: frames of a pattern, at rate, in channels */
static std::string LoopWav(uint32_t frames, uint32_t rate, uint16_t channels)
{
    auto u32 = [](std::string& w, uint32_t v) {
        for (int i = 0; i < 4; i++)
            w.push_back(static_cast<char>(v >> (8 * i)));
    };
    const uint32_t bytes = frames * channels * 2;
    std::string w = "RIFF";
    u32(w, 36 + bytes);
    w += "WAVEfmt ";
    u32(w, 16);
    u32(w, 1 | channels << 16);
    u32(w, rate);
    u32(w, rate * channels * 2);
    u32(w, channels * 2 | 16 << 16);
    w += "data";
    u32(w, bytes);
    uint32_t x = 12345;
    for (uint32_t i = 0; i < bytes; i++)
    {
        x = x * 1103515245u + 12345u;
        w.push_back(static_cast<char>(x >> 16));
    }
    return w;
}

static std::string Read(const std::string& path)
{
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

/** An effect's knob in remote.py's scene JSON ("filter": [a, b, c, d]), the index'th */
static float JsonParam(const std::string& json, const char* fx, int index)
{
    const size_t at = json.find("\"" + std::string(fx) + "\": [");
    if (at == std::string::npos)
        return -1.f;
    size_t p = json.find('[', at) + 1;
    for (int i = 0; i < index; i++)
        p = json.find(',', p) + 1;
    return strtof(json.c_str() + p, nullptr);
}

int main()
{
    firmware_dir = std::string(__FILE__).substr(0, std::string(__FILE__).rfind('/')) + "/..";
    remote_py = firmware_dir + "/remote.py";
    char dir[] = "/tmp/frizz-remote-XXXXXX";
    tmp_dir = mkdtemp(dir);
    std::string out;

    // the tools without the CHOMPI: the lock is taken only when one reaches for it
    {
        const int lock = open((tmp_dir + "/lock").c_str(), O_RDWR | O_CREAT, 0644);
        flock(lock, LOCK_EX);
        int rc = Python("import chompi, remote, card, flash\nprint('imported')\n", out);
        Check(rc == 0 && Has(out, "imported"),
              "tools: importing them doesn't wait for the CHOMPI another holds (measure.py compare)");
        if (rc != 0)
            printf("%s\n", out.c_str());
        rc = Python("import signal, chompi\nsignal.alarm(1)\nchompi.claim()\nprint('claimed')\n", out);
        Check(rc != 0 && Has(out, "the CHOMPI is in use") && !Has(out, "claimed"),
              "tools: reaching for it does, saying so");
        // card.py with the card a drive already (another's card.py mount, the storage firmware
        // started by hand): it waits for the lock all the same, touching nothing
        rc = Python(R"(
import signal, sys, chompi, card
signal.alarm(1)
chompi.storage_partition = lambda: "/dev/sdx1"
chompi.mount = lambda part: print("mounted", part) or "/nowhere"
chompi.to_launcher = chompi.start = lambda *rest: print("restarted")
sys.argv = ["card.py", "ls"]
card.main()
)", out);
        Check(rc != 0 && Has(out, "the CHOMPI is in use") && !Has(out, "mounted") && !Has(out, "restarted"),
              "tools: card.py waits for the lock with the card a drive already");
        if (!Has(out, "the CHOMPI is in use"))
            printf("%s\n", out.c_str());
        close(lock);
        unlink((tmp_dir + "/lock").c_str());
    }
    // where the slot started afterwards is waited for: a FRIZZ until it answers (all three)
    int rc = Python("import chompi, card\ncalls = []\n"
                    "chompi.run = lambda slot, wanted, *rest: calls.append((slot, wanted))\n"
                    "card.finish(12)\ncard.finish(None)\ncard.finish(11)\ncard.finish(13)\nprint(calls)\n", out);
    Check(rc == 0 && Has(out, "[(12, 'frizz'), (10, 'frizz'), (11, 'frizz'), (13, None)]"),
          "tools: card.py --then 12 or 11 waits for FRIZZ-TEST or FRIZZ-BETA as for FRIZZ, not for the bench");
    if (rc != 0)
        printf("%s\n", out.c_str());
    // a FRIZZ whose USB names came garbled (product "Љ", maker "FrizZ": no "chompi" in them)
    // is found by the Daisy's USB id, saying so; another card with MIDI isn't taken for it
    rc = Python(R"(
import os, shutil, chompi, midi_send
midi_send.find_device = lambda: None
for d in ("asound/card1", "asound/card2", "snd"):
    os.makedirs(d)
open("asound/cards", "w").write(
    " 1 [Default        ]: USB-Audio - Љ\n                      FrizZ Љ at usb-1, full speed\n"
    " 2 [USB            ]: USB-Audio - Scarlett\n                      Focusrite Scarlett at usb-2\n")
open("asound/card1/usbid", "w").write("0483:5740\n")
open("asound/card2/usbid", "w").write("1235:8215\n")
for n in ("midiC1D0", "midiC2D0"):
    open("snd/" + n, "w").close()
chompi.ASOUND, chompi.SND = "asound", "snd"
print("found", chompi.find_device(), "garbled", chompi.garbled())
open("asound/card1/usbid", "w").write("1235:0001\n")
print("found", chompi.find_device(), "garbled", chompi.garbled())
shutil.rmtree("asound")
shutil.rmtree("snd")
)", out);
    Check(rc == 0 && Has(out, "found snd/midiC1D0 garbled True\n") && Has(out, "no CHOMPI in its names (Default")
              && Has(out, "found None garbled False\n"),
          "tools: a FRIZZ with garbled USB names is found by its USB id, and said so");
    if (rc != 0 || !Has(out, "found snd/midiC1D0"))
        printf("%s\n", out.c_str());

    // a FRIZZ slot started that comes up with garbled names is started again from the launcher
    // (to_launcher restarts it), twice at most, then the power switch asked for; never sent
    // again; storage and other slots, or a node given (--device), aren't. chompi.py fix-name
    // starts the slot noted as running again, names right or not (#88)
    rc = Python(R"(
import os, sys, chompi, card, flash, midi_send
names, log = [], []
chompi.garbled = lambda: names[0] if len(names) == 1 else names.pop(0)
chompi.to_launcher = lambda *rest: log.append("launcher") or "l"
chompi.send_run = lambda node, slot: log.append("run %d" % slot)
chompi.wait_for = lambda wanted, *rest: log.append("wait " + str(wanted)) or "node"
def case(name, garbled, call):
    names[:] = garbled
    log.clear()
    try:
        got = call()
    except SystemExit as e:
        got = "exit: %s" % e
    print("%s: %s %s" % (name, got, log), flush=True)
case("fine", [False], lambda: chompi.start(12))
case("once", [True, False], lambda: chompi.start(12))
case("stays", [True], lambda: chompi.start(10))
case("storage", [True], lambda: chompi.run(chompi.STORAGE_SLOT, "storage"))
case("device", [True], lambda: chompi.start(12, device="/dev/x"))
case("then", [True, False], lambda: card.finish(12))
open("image.bin", "wb").write(b"x")
midi_send.main = lambda: log.append("sent")
chompi.slot_file = lambda *rest: None
chompi.features = lambda *rest: 0  # a launcher before 1.6: no colour
def flash_to(*argv):
    sys.argv = ["flash.py", "image.bin"] + list(argv)
    flash.main()
case("flash", [True, False], lambda: flash_to("--slot", "12"))
case("flash 5", [True], lambda: flash_to("--slot", "5", "--other-slot", "--name", "FRIZZ-X"))
os.remove("image.bin")
chompi.forget()
case("fix-name, none noted", [False], lambda: chompi.fix_name())
chompi.started(12)
case("fix-name", [False], lambda: chompi.fix_name())
case("fix-name 15", [False], lambda: chompi.fix_name(15))
chompi.forget()
)", out);
    Check(rc == 0 && Has(out, "fine: node ['launcher', 'run 12', 'wait frizz']\n")
              && Has(out, "once: node ['launcher', 'run 12', 'wait frizz', 'launcher', 'run 12', 'wait frizz']\n")
              && Has(out, "slot 12 came up with garbled USB names (#88): starting it again (1 of 2)"),
          "tools: a FRIZZ started with garbled USB names is started again from the launcher (#88)");
    Check(Has(out, "stays: node ['launcher', 'run 10', 'wait frizz', 'launcher', 'run 10', 'wait frizz', "
                   "'launcher', 'run 10', 'wait frizz']\n")
              && Has(out, "still garbled after 2 restarts (#88): switch the CHOMPI off and on"),
          "tools: twice at most, then the power switch asked for");
    Check(Has(out, "storage: node ['launcher', 'run 15', 'wait storage']\n")
              && Has(out, "device: node ['launcher', 'run 12', 'wait frizz']\n")
              && Has(out, "flash 5: None ['launcher', 'sent']\n"),
          "tools: not the storage firmware's, another slot's, or with a node given");
    Check(Has(out, "then: None ['launcher', 'run 12', 'wait frizz', 'launcher', 'run 12', 'wait frizz']\n")
              && Has(out, "flash: None ['launcher', 'sent', 'wait frizz', 'launcher', 'run 12', 'wait frizz']\n"),
          "tools: card.py --then and flash.py do it too, flash.py starting it again, not sending it again");
    Check(Has(out, "fix-name, none noted: exit: no slot is noted as running")
              && Has(out, "fix-name: node ['launcher', 'run 12', 'wait frizz']\n")
              && Has(out, "slot 12 answers, its USB names right")
              && Has(out, "fix-name 15: exit: slot 15 isn't FRIZZ's"),
          "tools: chompi.py fix-name starts the slot noted as running again; FRIZZ's slots and the bench's only");
    if (rc != 0 || !Has(out, "fix-name 15"))
        printf("%s\n", out.c_str());

    // RUN sent, and the launcher gone from USB before its reply is read (it started the slot,
    // reset and enumerated again): said, not a traceback, the caller waits as usual; another
    // error still stops it (#203)
    rc = Python(R"(
import contextlib, errno, chompi
class Gone:
    def __init__(self, no): self.no = no
    def call(self, *rest, **kw): raise OSError(self.no, "gone")
for no in (errno.ENODEV, errno.EACCES):
    chompi.link = contextlib.contextmanager(lambda node, no=no: (yield Gone(no)))
    try:
        chompi.send_run("n", 12)
        print("run %d: carried on" % no, flush=True)
    except OSError as e:
        print("run %d: raised %d" % (no, e.errno), flush=True)
)", out);
    Check(rc == 0 && Has(out, "the launcher left USB before it answered RUN: it is starting slot 12 (#203)")
              && Has(out, "starting slot 12 ...") && Has(out, "run 19: carried on\n")
              && Has(out, "run 13: raised 13\n"),
          "tools: a launcher gone from USB before RUN's reply is taken as starting the slot (#203)");
    if (rc != 0 || !Has(out, "run 19: carried on"))
        printf("%s\n", out.c_str());

    // card.py: a mount that fails still ejects the card and starts FRIZZ; mount alone needs a
    // hold, as the card stays mounted after it
    rc = Python(R"(
import os, sys, chompi, card
chompi.to_storage = lambda *rest: "/dev/sdx1"
def mount(part):
    sys.exit("udisksctl mount: refused")
chompi.mount = mount
card.finish = lambda then: print("finished", then)
sys.argv = ["card.py", "ls"]
try:
    card.main()
except SystemExit as e:
    print("exit:", e)
os.environ.pop(chompi.HELD, None)  # the ls above took the lock, as its own process would
sys.argv = ["card.py", "mount"]
try:
    card.main()
except SystemExit as e:
    print("exit:", e)
)", out);
    Check(rc == 0 && Has(out, "finished None\nexit: udisksctl mount: refused"),
          "tools: card.py ejects the card and starts FRIZZ again when the mount fails");
    Check(Has(out, "exit: card.py mount leaves the card mounted"),
          "tools: card.py mount needs a hold");
    if (rc != 0 || !Has(out, "finished None"))
        printf("%s\n", out.c_str());

    // card.py put writes into /FRIZZ and /USBTEST only, never over the settings and scenes or a
    // file there (the user's loops) unasked; --dangerous elsewhere, but never the root, /FIRMWARE
    // or /TAPE, however the name is spelt (FAT: any case, trailing dots and spaces dropped);
    // refused before the CHOMPI goes anywhere when the path says so
    rc = Python(R"(
import os, sys, chompi, card
for d in ("card/FRIZZ", "card/USBTEST", "card/FIRMWARE", "card/TAPE", "card/mine"):
    os.makedirs(d)
open("card/FRIZZ/a1.wav", "w").write("the user's")
open("card/USBTEST/t.txt", "w").write("old")
open("x.txt", "w").write("new")
root = os.path.abspath("card")
def put(path, *flags):
    try:
        card.put(root, "x.txt", path, "--dangerous" in flags, "--replace" in flags)
    except SystemExit as e:
        print("refused %s %s: %s" % (path, " ".join(flags), e), flush=True)
for path, flags in (("FRIZZ/", ()), ("FRIZZ/a1.wav", ()), ("FRIZZ/a1.wav", ("--replace",)),
                    ("USBTEST/t.txt", ()), ("frizz/FRIZZ_master.txt", ()), ("FRIZZ./frizz_scenes.txt.bak", ()),
                    ("FRIZZ/FRIZZ_~1.TXT", ()), ("FRIZZ/frizz_master.txt", ("--dangerous",)),
                    ("mine/", ()), ("mine/", ("--dangerous",)), ("CHOMPI.bin", ("--dangerous",)),
                    ("/", ("--dangerous",)), ("FIRMWARE/10_FRIZZ.bin", ("--dangerous",)),
                    ("firmware. /13_x.bin", ("--dangerous",)), ("TAPE/", ("--dangerous",)), ("../x", ())):
    put(path, *flags)
print(sorted(os.path.relpath(os.path.join(d, f), root) for d, _, fs in os.walk(root) for f in fs))
print(open("card/FRIZZ/a1.wav").read(), open("card/USBTEST/t.txt").read())
chompi.to_storage = lambda *rest: print("to the drive") or "/dev/sdx1"
chompi.mount = lambda part: print("mounted") or root
card.finish = lambda then: print("finished")
for path in ("FIRMWARE/", "TAPE/x.wav", "frizz/frizz_master.txt"):
    sys.argv = ["card.py", "put", "x.txt", path]
    try:
        card.main()
    except SystemExit as e:
        print("main %s: %s" % (path, e), flush=True)
)", out);
    Check(rc == 0 && Has(out, "['FRIZZ/a1.wav', 'FRIZZ/frizz_master.txt', 'FRIZZ/x.txt', 'USBTEST/t.txt', 'mine/x.txt']\n")
              && Has(out, "\nnew new\n") && Has(out, "refused FRIZZ/a1.wav : not written to FRIZZ/a1.wav: there already")
              && !Has(out, "refused FRIZZ/ :") && !Has(out, "refused USBTEST/t.txt"),
          "tools: card.py put writes into /FRIZZ and /USBTEST, over a file in /FRIZZ only with --replace");
    Check(Has(out, "refused frizz/FRIZZ_master.txt : not written to frizz/FRIZZ_master.txt: the user's settings")
              && Has(out, "refused FRIZZ./frizz_scenes.txt.bak : ") && Has(out, "refused FRIZZ/FRIZZ_~1.TXT : ")
              && Has(out, "refused mine/ : not written to mine/x.txt: only /FRIZZ and /USBTEST")
              && Has(out, "refused CHOMPI.bin --dangerous: not written to CHOMPI.bin: the card's root")
              && Has(out, "refused / --dangerous: not written to x.txt: the card's root")
              && Has(out, "refused FIRMWARE/10_FRIZZ.bin --dangerous: not written to FIRMWARE/10_FRIZZ.bin: /FIRMWARE is")
              && Has(out, "refused firmware. /13_x.bin --dangerous: ") && Has(out, "refused TAPE/ --dangerous: ")
              && Has(out, "refused ../x : ../x is not on the card"),
          "tools: never the settings and scenes unasked, never the root, /FIRMWARE or /TAPE, however spelt");
    Check(Has(out, "main FIRMWARE/: not written to FIRMWARE/: /FIRMWARE is")
              && Has(out, "main TAPE/x.wav: not written to TAPE/x.wav: /TAPE is TAPE's")
              && Has(out, "main frizz/frizz_master.txt: not written to frizz/frizz_master.txt: the user's")
              && !Has(out, "to the drive") && !Has(out, "finished"),
          "tools: card.py put refuses those before the CHOMPI goes anywhere");
    if (rc != 0 || !Has(out, "main frizz/"))
        printf("%s\n", out.c_str());
    system(("rm -rf '" + tmp_dir + "/card' '" + tmp_dir + "/x.txt'").c_str());

    // a host check that reaches for a real device, a stub missing, is stopped there, finally
    // blocks and all: Python() sets FRIZZ_NO_DEVICE (pytool.h). A pty (the twin's) still goes
    for (const char* reach : {"chompi.link('/dev/snd/midiC9D0').__enter__()", "chompi.udisks('info', '-b', '/dev/sdx1')",
                              "remote.Frizz('/dev/snd/midiC9D0')"})
    {
        const std::string code = std::string("import chompi, remote, card\ntry:\n    ") + reach
                                 + "\nfinally:\n    print('finally ran')\nprint('carried on')\n";
        rc = Python(code, out);
        Check(rc == 70 && Has(out, "a host check reached for a real device (") && !Has(out, "carried on")
                  && !Has(out, "finally ran"),
              (std::string("tools: the host checks' net stops ") + reach).c_str());
        if (rc != 70)
            printf("%s\n", out.c_str());
    }
    rc = Python("import os, pty, chompi\nm, s = pty.openpty()\nwith chompi.link(os.ttyname(s)) as to:\n"
                "    print('pty', to.fd > 0)\n",
                out);
    Check(rc == 0 && Has(out, "pty True\n"), "tools: but lets a pty through, the twin's");
    if (rc != 0)
        printf("%s\n", out.c_str());

    // the user's keys stay guarded whatever FRIZZ_SLOT and BETA_SLOT say; a slot is checked
    // before anything is built or the CHOMPI restarted, another key than FRIZZ's and the
    // bench's only with --other-slot; FRIZZ's keys keep another firmware on them unasked, and
    // the test slot and the bench the user's FRIZZ and FRIZZ-BETA (the keys moved, #171)
    rc = Python(R"(
import os, sys
os.environ.update(FRIZZ_SLOT="5", BETA_SLOT="14", TEST_SLOT="10")
import chompi, flash, midi_send
log = []
chompi.to_launcher = lambda *rest: log.append("launcher") or "l"
chompi.wait_for = lambda *rest: "node"
chompi.renamed = lambda slot, node, *rest: node
chompi.ident = lambda node: None
chompi.started = chompi.noted_image = lambda *rest: None
chompi.features = lambda *rest: 0
midi_send.main = lambda: log.append("sent %s" % sys.argv[3])
there = {}
chompi.slot_file = lambda device, slot: there.get(slot)
open("image.bin", "wb").write(b"x")
flash.builds.build = flash.builds.built = lambda bench: "image.bin"
def flash_to(*argv):
    log.clear()
    sys.argv = ["flash.py"] + list(argv)
    try:
        flash.main()
        got = "ok"
    except SystemExit as e:
        got = "exit: %s" % e
    print("%s: %s %s" % (" ".join(argv), got, log), flush=True)
print("user slots", chompi.USER_SLOTS)
for argv in (("image.bin", "--slot", "11"), ("image.bin", "--slot", "10"), ("--test", "--no-build"),
             ("image.bin", "--slot", "5"), ("image.bin", "--slot", "14")):
    flash_to(*argv)
print("-- the defaults", flush=True)
os.environ.update(FRIZZ_SLOT="10", BETA_SLOT="11", TEST_SLOT="12")
import importlib
importlib.reload(chompi)
flash.chompi = chompi
chompi.to_launcher = lambda *rest: log.append("launcher") or "l"
chompi.wait_for = lambda *rest: "node"
chompi.renamed = lambda slot, node, *rest: node
chompi.ident = lambda node: None
chompi.started = chompi.noted_image = lambda *rest: None
chompi.slot_file = lambda device, slot: there.get(slot)
chompi.features = lambda *rest: 0
for argv in (("image.bin", "--slot", "15"), ("image.bin", "--slot", "0"), ("image.bin", "--slot", "16"),
             ("image.bin", "--slot", "15", "--other-slot"), ("image.bin", "--slot", "12")):
    flash_to(*argv)
there[12] = "12_TAPE.bin"
flash_to("image.bin", "--slot", "12")
flash_to("--test", "--no-build")
flash_to("image.bin", "--slot", "12", "--other-slot")
there[13] = "13_FRIZZ-BETA.bin"
flash_to("--bench", "--no-build")
there[12] = "12_FRIZZ.bin"
flash_to("--test", "--no-build")
there[12] = "12_FRIZZ-TEST.bin"
flash_to("--test", "--no-build")
os.remove("image.bin")
)", out);
    Check(rc == 0 && Has(out, "user slots (5, 10, 11, 14)\n")
              && Has(out, "image.bin --slot 11: exit: key 11 is the latest beta, the user's: only a release's")
              && Has(out, "image.bin --slot 10: exit: key 10 is ") && Has(out, "--test --no-build: exit: key 10 is ")
              && Has(out, "image.bin --slot 5: exit: key 5 is ") && Has(out, "image.bin --slot 14: exit: key 14 is ")
              && Has(out, "-- the defaults") && !Has(out.substr(0, out.find("-- the defaults")), "'sent"),
          "tools: flash.py keeps keys 10 and 11 the user's whatever FRIZZ_SLOT, BETA_SLOT and TEST_SLOT say");
    Check(Has(out, "image.bin --slot 15: exit: key 15 isn't FRIZZ's or the bench's (10, 11, 12, 13): ")
              && Has(out, "image.bin --slot 0: exit: no key 0: the launcher's are 1-15 []\n")
              && Has(out, "image.bin --slot 16: exit: no key 16") && Has(out, "image.bin --slot 15: exit: key 15 isn't FRIZZ's or the bench's (10, 11, 12, 13): sending replaces what's there, so --other-slot says it's meant (never in a session's run) []\n")
              && Has(out, "image.bin --slot 15 --other-slot: ok ['launcher', 'sent 15']\n")
              && Has(out, "image.bin --slot 12: ok ['launcher', 'sent 12']\n"),
          "tools: flash.py --slot takes FRIZZ's keys and the bench's, another only with --other-slot, before any restart");
    Check(Has(out, "image.bin --slot 12: exit: key 12 holds 12_TAPE.bin, not FRIZZ: give --other-slot")
              && Has(out, "--test --no-build: exit: key 12 holds 12_TAPE.bin")
              && Has(out, "image.bin --slot 12 --other-slot: ok ['launcher', 'sent 12']\n"),
          "tools: flash.py replaces another firmware on FRIZZ's keys only with --other-slot, --slot or not");
    Check(Has(out, "--bench --no-build: exit: key 13 holds 13_FRIZZ-BETA.bin, the user's: a test or a bench "
                   "build never replaces it ['launcher']\n")
              && Has(out, "--test --no-build: exit: key 12 holds 12_FRIZZ.bin, the user's")
              && Has(out, "--test --no-build: ok ['launcher', 'sent 12']\n"),
          "tools: flash.py --test and --bench never replace the user's FRIZZ or FRIZZ-BETA on their keys (#171)");
    if (rc != 0 || !Has(out, "--other-slot: ok ['launcher', 'sent 12']"))
        printf("%s\n", out.c_str());

    // what's on a key only the launcher lists, so flash.py refuses for that after restarting
    // FRIZZ into it: the FRIZZ that ran is started again, not left at the picker. Which one,
    // to_launcher() finds before the restart (came_from): by IDENT and the builds sent, or else
    // the slot started in this hold; 10 for the bench (never started again unasked) or one it
    // can't tell; nothing when the CHOMPI was at the launcher already
    rc = Python(R"(
import os, sys, chompi, flash, midi_send
log = []
states = []
chompi.state = lambda device=None: states.pop(0) if states else ("launcher", "l")
chompi.restart = lambda node, hard=False: log.append("restart")
chompi.reached = lambda *rest: True
chompi.wait_for = lambda wanted, *rest, **kw: "l"
chompi.start = lambda slot, *rest: log.append("start %d" % slot)
chompi.slot_file = lambda device, slot: "12_FRIZZ.bin"
midi_send.main = lambda: log.append("sent")
open("image.bin", "wb").write(b"x")
flash.builds.built = lambda bench: "image.bin"
idents = {"frizz": {"kind": "frizz", "crc": 1, "length": 1}, "bench": {"kind": "bench", "crc": 1, "length": 1}}
def case(name, now, info=None, sent=(), last=None):
    log.clear()
    states[:] = [now]
    chompi.ident = lambda node: idents.get(info)
    chompi.slots_of = lambda i: list(sent)
    chompi.last_started = lambda: (last, last is not None, "a tool")
    sys.argv = ["flash.py", "--test", "--no-build"]
    try:
        flash.main()
    except SystemExit as e:
        print("exit:", e)
    print("%s: %s, came from %s" % (name, log, chompi.came_from), flush=True)
case("the beta, by IDENT", ("frizz", "n"), "frizz", sent=(11,))
case("one sent to two keys, the one started", ("frizz", "n"), "frizz", sent=(10, 12), last=12)
case("a build not sent", ("frizz", "n"), "frizz", last=11)
case("the bench", ("frizz", "n"), "bench", sent=(13,), last=13)
case("no IDENT, started in this hold", ("frizz", "n"), last=11)
case("at the launcher", ("launcher", "l"))
os.remove("image.bin")
)", out);
    Check(rc == 0 && Has(out, "refused, nothing sent: starting FRIZZ on slot 11, not leaving the CHOMPI at the launcher\nexit: key 12 holds 12_FRIZZ.bin, the user's")
              && Has(out, "the beta, by IDENT: ['restart', 'start 11'], came from 11\n")
              && Has(out, "one sent to two keys, the one started: ['restart', 'start 12'], came from 12\n")
              && Has(out, "a build not sent: ['restart', 'start 10'], came from 10\n")
              && Has(out, "the bench: ['restart', 'start 10'], came from 10\n")
              && Has(out, "no IDENT, started in this hold: ['restart', 'start 11'], came from 11\n")
              && Has(out, "at the launcher: [], came from None\n"),
          "tools: flash.py refused at the launcher starts the FRIZZ that ran again, the bench never");
    if (rc != 0 || failures)
        printf("%s\n", out.c_str());

    // nothing restarts FRIZZ while it lends the card to the computer: the drive would go from
    // under it (#196). Its drive seen, or FRIZZ's own word (USB's flags) before the drive shows;
    // sent as before when neither
    rc = Python(R"(
import contextlib, os, sys, chompi
r, w = os.pipe()
os.set_blocking(r, False)
class To:
    fd = w
chompi.link = contextlib.contextmanager(lambda node: (yield To()))
flags = [0]
chompi.dev_call = lambda to, sub, args=b"", **kw: (chompi.OK, bytes([flags[0]])) if sub == chompi.USB_SUB else None
def restart(drive, f, hard=False):
    chompi.lent_partition = lambda: drive
    flags[0] = f
    try:
        chompi.restart("node", hard)
        got = "sent"
    except SystemExit as e:
        got = "exit: %s" % e
    try:
        got += " " + os.read(r, 100).hex()
    except BlockingIOError:
        got += " nothing"
    print(got, flush=True)
restart("/dev/sdz1", 0)
restart(None, chompi.CARD_LENT | 1)
restart(None, 1)
restart(None, 0, hard=True)
restart("/dev/sdz1", 0, hard=True)
chompi.dev_call = lambda *rest, **kw: None
restart(None, 0)
)", out);
    Check(rc == 0 && Has(out, "exit: the firmware running lends the card to the computer (CHOMPI-SD at /dev/sdz1): not restarted, the drive would go from under it. Eject it there (or take it back on the CHOMPI) and run this again nothing\n")
              && Has(out, "exit: the firmware running lends the card to the computer (FRIZZ says so): not restarted")
              && Has(out, "\nsent f07d434810f7\nsent f07d43483e0a00525354f7\nexit: ")
              && Has(out, "\nsent f07d434810f7\n"),
          "tools: chompi.restart refuses while FRIZZ lends the card, by the drive or by FRIZZ's word");
    if (rc != 0 || !Has(out, "FRIZZ says so"))
        printf("%s\n", out.c_str());

    // remote.py set: VOLUME's input gain is saved however its index is written ("01", " 1",
    // "+1", as try_set reads it), so it keeps the card out of it as for "in" (PERSIST 0)
    rc = Python(R"(
import remote
print([remote.set_saves("volume", i) for i in ("in", "1", "01", " 1", "+1", "out", "0", "2", "x", "1.0")],
      remote.set_saves("comp", "0"), remote.set_saves("settings", "mono"), remote.set_saves("filter", "1"))
)", out);
    Check(rc == 0 && Has(out, "[True, True, True, True, True, False, False, False, False, False] True True False\n"),
          "tools: remote.py set volume's input gain counts as saved however its index is written");
    if (rc != 0)
        printf("%s\n", out.c_str());

    // remote.py's developers' SysEx asked again keeps its SEQ, so a late answer to the first ask
    // counts: a job's start asked again is refused, but the first ask's OK comes first (FIFO)
    rc = Python(R"(
import os, threading, remote, chompi
r, w = os.pipe()
class Link:
    fd = w
    def __init__(self, answers): self.answers = answers
    def recv(self, cmd, timeout, match=None):
        a = self.answers.pop(0) if self.answers else None
        return a if a is None or match(a) else None
f = object.__new__(remote.Frizz)
f.write_lock, f.query_lock = threading.Lock(), threading.Lock()
def asks():
    data = os.read(r, 1000)
    return [m[6] for m in data.split(b"\xf7") if m]
chompi._seq[0] = 40
f.link = Link([None, None, bytes([remote.chompi.IDENT, 41, 0])])
print("ident", f.dev(chompi.IDENT), asks(), flush=True)
f.link = Link([None, bytes([chompi.LOOPS, 42, 0]), bytes([chompi.LOOPS, 42, 1])])
print("load", f.dev(chompi.LOOPS, [4, 0, 0, 0], wait=0.01), asks(), flush=True)
)", out);
    Check(rc == 0 && Has(out, "ident (0, b'') [41, 41, 41]\n") && Has(out, "load (0, b'') [42, 42]\n"),
          "tools: remote.py asks again with the same SEQ, and takes the first answer, a job's start's OK");
    if (rc != 0)
        printf("%s\n", out.c_str());

    // the TRS interface found is never FRIZZ's own node (the Daisy's USB id, its card called
    // FrizZ or Default), not while FRIZZ restarts either, and said which
    rc = Python(R"(
import os, chompi, midiport
chompi.find_device = lambda: None  # FRIZZ restarting: not found as the CHOMPI
for d in ("asound/card1", "asound/card2", "snd"):
    os.makedirs(d)
open("asound/cards", "w").write(" 1 [Default        ]: USB-Audio - FrizZ\n                      Electrosmith FrizZ at usb-1\n"
                                " 2 [USB            ]: USB-Audio - Scarlett\n                      Focusrite Scarlett at usb-2\n")
open("asound/card1/usbid", "w").write("0483:5740\n")
open("asound/card2/usbid", "w").write("1235:8215\n")
for n in ("midiC1D0", "midiC2D0"):
    open("snd/" + n, "w").close()
chompi.ASOUND, chompi.SND = "asound", "snd"
print("trs", midiport.trs_midi(), flush=True)
)", out);
    Check(rc == 0 && Has(out, "trs snd/midiC2D0\n") && Has(out, "the TRS interface: snd/midiC2D0 (USB / "),
          "tools: the TRS interface is never FRIZZ's own USB node, and said which");
    if (rc != 0 || !Has(out, "trs snd/midiC2D0"))
        printf("%s\n", out.c_str());

    // tools/midi_send.py run by hand takes the lock and leaves the user's keys (sending, clearing
    // or colouring them), the storage firmware's and the launcher to flash.py; another key goes
    // on as before
    rc = Python(R"(
import subprocess, sys
open("x.bin", "wb").write(b"x")
by_hand = "import runpy, sys\nsys.argv[0] = 'midi_send.py'\nrunpy.run_module('midi_send', run_name='__main__')\n"
for args in (["--slot", "11"], ["--sl", "10"], ["--slot=15"], ["--clear", "10"], ["--launcher"], ["--slot", "12"],
             ["--color", "11", "8A4DFF"], ["--color", "12", "8A4DFF"], ["--slot", "13"]):
    p = subprocess.run([sys.executable, "-c", by_hand, "x.bin", "--device", "nowhere"] + args,
                       capture_output=True, text=True)
    print(" ".join(args), p.returncode, (p.stderr.strip().splitlines() or [""])[-1], flush=True)
)", out);
    Check(rc == 0 && Has(out, "--slot 11 1 midi_send.py by hand leaves keys 10, 11, 15 and the launcher alone")
              && Has(out, "--sl 10 1 midi_send.py by hand") && Has(out, "--slot=15 1 midi_send.py by hand")
              && Has(out, "--clear 10 1 midi_send.py by hand") && Has(out, "--launcher 1 midi_send.py by hand")
              && Has(out, "--slot 12 1 FileNotFoundError") && Has(out, "--slot 13 1 FileNotFoundError")
              && Has(out, "--color 11 8A4DFF 1 midi_send.py by hand")
              && Has(out, "--color 12 8A4DFF 2 midi_send.py: error: --slot is needed"),
          "tools: midi_send.py by hand leaves keys 10, 11, 15 (their colour too) and the launcher to flash.py");
    if (rc != 0 || !Has(out, "--slot 12 1 FileNotFoundError"))
        printf("%s\n", out.c_str());

    // the launcher 1.5 and 1.6, stubbed (their PING, LIST, COLOR and RUN): the tools tell the
    // launcher from FRIZZ, read a key's file and list the keys with either; 1.6's LIST adds the
    // key's colour (7 bits a channel, so #8A4DFF lists as #8B4CFF). flash.py stores without
    // starting, colours the key, then starts it (a colour the card fails is said, the build
    // started all the same): in its slot's colour (#171: 12 and 13 red), in --color's when given,
    // another key only with --color; with 1.5 it sends without, said only for a --color given
    rc = Python(R"(
import contextlib, os, sys, chompi, flash, midi_send
log = []
class Launcher:
    def __init__(self, minor):
        self.minor = minor
    def close(self):
        pass
    def call(self, cmd, payload=b"", timeout=0.3, retries=10, required=True, match=None):
        if cmd == midi_send.PING:
            features = 0x3F if self.minor >= 6 else 0x1F
            return bytes([0, 1]) + midi_send.put7(256, 4) + bytes([15, features, 1, self.minor, 0])
        if cmd == midi_send.LIST:
            name = {12: b"12_FRIZZ-TEST.bin", 3: b"03_WAVE.bin"}.get(payload[0], b"")
            r = bytes([0, payload[0], len(name)]) + name + midi_send.put7(248344 if name else 0, 4)
            return r + (bytes([0x45, 0x26, 0x7F] if name else [0, 0, 0]) if self.minor >= 6 else b"")
        if cmd == midi_send.COLOR:
            log.append("color %d %s" % (payload[0], payload[1:].hex()))
            return bytes([0, payload[0]])
        if cmd == chompi.RUN:
            log.append("run %d" % payload[0])
            return bytes([0, payload[0]])
        return bytes([1])  # BAD_MESSAGE, to FRIZZ's SETTINGS among others
chompi.to_launcher = lambda *rest: log.append("launcher") or "l"
chompi.wait_for = lambda *rest: "node"
chompi.renamed = lambda slot, node, *rest: node
chompi.ident = lambda node: None
chompi.started = chompi.noted_image = lambda *rest: None
sent = midi_send.main
open("image.bin", "wb").write(b"x")
flash.builds.build = flash.builds.built = lambda bench: "image.bin"
def run(what, main, *argv):
    log.clear()
    sys.argv = list(argv)
    try:
        main()
        got = "ok"
    except SystemExit as e:
        got = "exit: %s" % e
    print("%s: %s %s" % (what, got, log), flush=True)
for minor in (5, 6):
    there = Launcher(minor)
    chompi.link = contextlib.contextmanager(lambda node: (yield there))
    midi_send.Link = lambda path, verbose=False: there
    print("1.%d state %s, key 12 %r, key 1 %r, features %#x" % (minor, chompi.ask_midi("l"),
          chompi.slot_file("l", 12), chompi.slot_file("l", 1), chompi.features("l")), flush=True)
    midi_send.main = sent
    run("1.%d list" % minor, flash.main, "flash.py", "--list")
    run("1.%d color by hand" % minor, midi_send.main, "midi_send.py", "--color", "12", "8A4DFF", "--device", "l")
    midi_send.main = lambda: log.append(" ".join(sys.argv[1:]))
    run("1.%d test" % minor, flash.main, "flash.py", "--test", "--no-build")
    run("1.%d test colour" % minor, flash.main, "flash.py", "--test", "--no-build", "--color", "8A4DFF")
    run("1.%d bench" % minor, flash.main, "flash.py", "--bench", "--no-build")
    run("1.%d another key" % minor, flash.main, "flash.py", "image.bin", "--slot", "3", "--other-slot")
    run("1.%d another key, coloured" % minor, flash.main, "flash.py", "image.bin", "--slot", "3", "--other-slot",
        "--color", "8A4DFF")
def fails():
    if "--color" in sys.argv:
        sys.exit("color failed: writing to the card failed")
    log.append(" ".join(sys.argv[1:]))
midi_send.main = fails
run("1.6 colour fails", flash.main, "flash.py", "--test", "--no-build", "--color", "8A4DFF")
run("bad colour", flash.main, "flash.py", "--test", "--color", "violet")
run("colour, nothing sent", flash.main, "flash.py", "--run", "12", "--color", "8A4DFF")
os.remove("image.bin")
)", out);
    Check(rc == 0 && Has(out, "1.5 state launcher, key 12 '12_FRIZZ-TEST.bin', key 1 '', features 0x1f\n")
              && Has(out, "1.6 state launcher, key 12 '12_FRIZZ-TEST.bin', key 1 '', features 0x3f\n"),
          "tools: the launcher 1.5 and 1.6 are told from FRIZZ, and their keys read, alike");
    Check(Has(out, "  key 12: 12_FRIZZ-TEST.bin (248344 bytes)\n")
              && Has(out, "  key 12: 12_FRIZZ-TEST.bin (248344 bytes)  #8B4CFF\n")
              && Has(out, "  key  3: 03_WAVE.bin (248344 bytes)  #8B4CFF\n") && Has(out, "  key  1: -\n")
              && Has(out, "1.5 list: ok") && Has(out, "1.6 list: ok"),
          "tools: flash.py --list lists the keys from either launcher, 1.6's with their colour");
    Check(Has(out, "1.5 color by hand: exit: this launcher cannot colour its keys")
              && Has(out, "1.6 color by hand: ok ['color 12 45267f']"),
          "tools: midi_send.py --color sends 1.6's COLOR, 7 bits a channel; 1.5 can't");
    Check(Has(out, "1.5 test: ok ['launcher', 'image.bin --slot 12 --name FRIZZ-TEST --device l']\n")
              && Has(out, "1.5 test colour: ok ['launcher', 'image.bin --slot 12 --name FRIZZ-TEST --device l']\n")
              && Has(out, "1.5 bench: ok ['launcher', 'image.bin --slot 13 --name FRIZZ-BENCH --device l']\n")
              && Has(out, "this launcher can't colour its keys (1.6 can): sent without --color")
              && Count(out, "this launcher can't colour its keys") == 2
              && Has(out, "1.6 test colour: ok ['launcher', 'image.bin --slot 12 --name FRIZZ-TEST --device l "
                          "--stay', '--color 12 8A4DFF --device l', 'run 12']\n"),
          "tools: flash.py --color stores, colours, then starts with 1.6; 1.5 sends without, said for a --color given");
    Check(Has(out, "1.6 test: ok ['launcher', 'image.bin --slot 12 --name FRIZZ-TEST --device l --stay', "
                   "'--color 12 FF0000 --device l', 'run 12']\n")
              && Has(out, "1.6 bench: ok ['launcher', 'image.bin --slot 13 --name FRIZZ-BENCH --device l --stay', "
                          "'--color 13 FF0000 --device l', 'run 13']\n")
              && Has(out, "1.5 another key: ok ['launcher', 'image.bin --slot 3 --name FRIZZ --device l']\n")
              && Has(out, "1.6 another key: ok ['launcher', 'image.bin --slot 3 --name FRIZZ --device l']\n")
              && Has(out, "1.6 another key, coloured: ok ['launcher', 'image.bin --slot 3 --name FRIZZ --device l "
                          "--stay', '--color 3 8A4DFF --device l', 'run 3']\n"),
          "tools: flash.py gives the test slot and the bench their red unasked (#171), another key a colour only with --color");
    Check(Has(out, "key 12 not coloured: color failed: writing to the card failed\n")
              && Has(out, "1.6 colour fails: ok ['launcher', 'image.bin --slot 12 --name FRIZZ-TEST --device l --stay', 'run 12']\n"),
          "tools: flash.py --color whose COLOR the card fails says so and starts the build all the same");
    Check(Has(out, "bad colour: exit: --color takes a colour like 8A4DFF") && Has(out, "colour, nothing sent: exit: --color takes"),
          "tools: flash.py --color takes a colour, with a build sent only");
    if (rc != 0 || !Has(out, "colour, nothing sent: exit"))
        printf("%s\n", out.c_str());

    // the launcher's node as ALSA's raw MIDI has it: one opener at a time (EBUSY). flash.py's
    // midi_send.main() for the build, then again for COLOR (and chompi's links between), each
    // must close it: the first left it open, COLOR's open failed uncaught and nothing was
    // started. A COLOR that fails so anyway is said, and the build started all the same
    rc = Python(R"(
import contextlib, errno, os, sys, chompi, flash, midi_send
log = []
class Node:
    """The launcher 1.6 behind an exclusive node"""
    users = 0
    def __init__(self, path, verbose=False):
        if Node.users:
            raise OSError(errno.EBUSY, "Device or resource busy")
        Node.users += 1
        self.fd = 0
        self.length = 0
    def close(self):
        if self.fd >= 0:
            Node.users -= 1
            self.fd = -1
    def call(self, cmd, payload=b"", timeout=0.3, retries=10, required=True, match=None):
        if cmd == midi_send.PING:
            return bytes([0, 1]) + midi_send.put7(256, 4) + bytes([15, 0x3F, 1, 6, 0])
        if cmd == midi_send.LIST:
            name = b"12_FRIZZ-TEST.bin" if payload[0] == 12 else b""
            return bytes([0, payload[0], len(name)]) + name + midi_send.put7(1, 4) + bytes([0, 0, 0])
        if cmd == midi_send.BEGIN:
            self.length = midi_send.get7(payload[:4])
            return bytes([0])
        if cmd == midi_send.DATA:
            return bytes([0]) + midi_send.put7(self.length, 4)
        if cmd == midi_send.END:
            log.append('stored%s' % (' (stay)' if payload[5:] else ''))
            return bytes([0])
        if cmd == midi_send.COLOR:
            if colour_fails:
                raise OSError(errno.EBUSY, "Device or resource busy")
            log.append("color %d" % payload[0])
            return bytes([0, payload[0]])
        if cmd == chompi.RUN:
            log.append("run %d" % payload[0])
            return bytes([0, payload[0]])
        return bytes([1])
midi_send.Link = Node
chompi.to_launcher = lambda *rest: "l"
chompi.wait_for = lambda *rest: "node"
chompi.renamed = lambda slot, node, *rest: node
chompi.ident = lambda node: None
chompi.started = chompi.noted_image = lambda *rest: None
open("image.bin", "wb").write(b"x")
flash.builds.build = flash.builds.built = lambda bench: "image.bin"
for colour_fails in (False, True):
    log.clear()
    sys.argv = ["flash.py", "--test", "--no-build"]
    try:
        flash.main()
        got = "ok"
    except BaseException as e:
        got = "%s: %s" % (type(e).__name__, e)
    print("exclusive%s: %s %s, open %d" % (", COLOR fails" if colour_fails else "", got, log, Node.users), flush=True)
    Node.users = 0
os.remove("image.bin")
)", out);
    Check(rc == 0 && Has(out, "exclusive: ok ['stored (stay)', 'color 12', 'run 12'], open 0\n"),
          "tools: flash.py with the launcher 1.6 on a node one opener at a time: stored, coloured, started, the node closed");
    Check(Has(out, "key 12 not coloured: [Errno 16] Device or resource busy\n")
              && Has(out, "exclusive, COLOR fails: ok ['stored (stay)', 'run 12'], open 0\n"),
          "tools: and a COLOR the node refuses (EBUSY) is said, the build started all the same");
    if (rc != 0 || !Has(out, "exclusive: ok"))
        printf("%s\n", out.c_str());

    // usbjitter.py turns PERSIST 0 on: the user's FRIZZ it measured is started again at the end,
    // from the card, so it saves again (the test slot's needn't); usbaudio.py card on needs a
    // hold, the card staying lent after it (as card.py mount)
    rc = Python(R"(
import sys, chompi, remote, usbjitter, usbaudio
usbjitter.time.sleep = lambda s: None
class F:
    def __init__(self): self.link = type("L", (), {"fd": -1})()
    def ram_only(self): return True
    def ask(self, cmd): return [0, 0, 0, 0]
    def send(self, *rest): pass
usbjitter.TapLink = lambda link: type("T", (), {"fd": -1, "running": True, "ticks": []})()
remote.Frizz = lambda node: F()
remote.loops_load = lambda *rest: None
remote.os.close = lambda fd: None
for slot in (10, 12):
    def to_frizz(s, slot=slot):
        chompi.frizz_slot = slot
        return "node"
    chompi.to_frizz = to_frizz
    chompi.start = lambda s: print("started", s, flush=True)
    sys.argv = ["usbjitter.py", "--slot", str(slot)]
    try:
        usbjitter.main()
    except SystemExit as e:
        print("slot %d: %s" % (slot, e), flush=True)
sys.argv = ["usbaudio.py", "card", "on"]
try:
    usbaudio.main()
except SystemExit as e:
    print("card on:", e)
)", out);
    Check(rc == 0 && Has(out, "started 10\nslot 10: no clock ticks") && Has(out, "\nslot 12: no clock ticks")
              && !Has(out, "started 12"),
          "tools: usbjitter.py starts the user's FRIZZ it measured again, its PERSIST 0 gone");
    Check(Has(out, "card on: card on leaves the card lent until card off, which no lock of its own covers"),
          "tools: usbaudio.py card on needs a hold");
    if (rc != 0 || !Has(out, "card on:"))
        printf("%s\n", out.c_str());

    // the smaller holes: a hold that's gone (SIGTERMed, its child running on) holds nothing, so
    // its child takes the lock; make failing (2) isn't a fresh build; key 10 takes no
    // pre-release's tag; an IDENT cut short is no IDENT, said so
    rc = Python(R"(
import os, signal, subprocess, sys, chompi, builds, flash, remote
p = subprocess.Popen(["true"])
p.wait()
os.environ[chompi.HELD] = str(p.pid)  # its hold is gone
chompi.claim()
print("dead hold: lock taken", chompi._lock is not None, flush=True)
codes = []
builds.subprocess.run = lambda *a, **kw: subprocess.CompletedProcess(a, codes.pop(0))
for code in (0, 1, 2):
    codes.append(code)
    print("make -q %d: stale %s" % (code, builds.stale(builds.image(False))), flush=True)
flash.tagged = lambda where=flash.HERE: "v1.5.0-beta.1"
class A:
    bench = False
for slot in (10, 11):
    flash.builds.check_version = lambda tag: print("checked", tag)
    try:
        print("beta tag, key %d:" % slot, flash.user_slot(slot, A()), flush=True)
    except SystemExit as e:
        print("beta tag, key %d: exit: %s" % (slot, e), flush=True)
print("short ident", chompi.parse_ident(bytes(25)), chompi.parse_ident(bytes(26)) is not None)
class F:
    def dev(self, sub, *rest, **kw): return (chompi.OK, bytes(10))
try:
    remote.show_ident(F())
except SystemExit as e:
    print("show_ident:", e)
print("in_ram:", remote.in_ram(F()))
)", out);
    Check(rc == 0 && Has(out, "dead hold: lock taken True\n"), "tools: a hold that's gone holds nothing: its child takes the lock");
    Check(Has(out, "make -q 0: stale False\nmake -q 1: stale True\nmake -q 2: stale True\n"),
          "tools: builds.py takes a build for fresh only when make -q says so (0), not when it fails");
    Check(Has(out, "beta tag, key 10: exit: key 10 is the latest release: v1.5.0-beta.1 is a pre-release's tag")
              && Has(out, "whose build goes to key 11 (--slot 11)")
              && Has(out, "checked v1.5.0-beta.1\nbeta tag, key 11: v1.5.0-beta.1\n"),
          "tools: flash.py sends a pre-release's build to key 11, never to key 10");
    Check(Has(out, "short ident None True\n") && Has(out, "show_ident: IDENT's answer is cut short (10 bytes)")
              && Has(out, "in_ram: no answer to IDENT"),
          "tools: an IDENT cut short is no IDENT, said so, never an IndexError");
    if (rc != 0 || !Has(out, "in_ram:"))
        printf("%s\n", out.c_str());

    // chompi.py hold CMD keeps the lock until what it runs has ended: a SIGTERM (a closed
    // terminal's SIGHUP, Ctrl-C's SIGINT) goes on to CMD's process group, and the lock goes only
    // once nothing in it runs, a child that outlives sh too; a second signal kills the group.
    // Before, the hold died at once and its sh chain ran on at the CHOMPI without the lock
    // (2026-10-15). Each sh writes its group's id first, which the finally kills whatever
    // happened
    rc = Python(R"(
import fcntl, os, signal, subprocess, sys, time, chompi
def free():
    with open(chompi.LOCK, "a+") as f:
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return True
        except BlockingIOError:
            return False
def until(what, s=5):
    end = time.monotonic() + s
    while not what() and time.monotonic() < end:
        time.sleep(0.02)
    return what()
def case(name, script, signals=1):
    for f in ("started", "done", "pgid"):
        if os.path.exists(f):
            os.remove(f)
    with open("hold.err", "w") as err:
        p = subprocess.Popen([sys.executable, chompi.__file__, "hold", "sh", "-c", "echo $$ > pgid; " + script],
                             stderr=err, process_group=0)
    try:
        ok = until(lambda: os.path.exists("started")) and not free()
        for _ in range(signals):
            p.send_signal(signal.SIGTERM)
            time.sleep(0.2)
        held = True
        while p.poll() is None:
            if free():  # a hold exiting closes the lock a moment before it can be waited for
                time.sleep(0.2)
                held = held and p.poll() is not None
            time.sleep(0.05)
        done = os.path.exists("done")
        print("%s: started %s, held while it ran %s, done when it let go %s, exit %d, free after %s"
              % (name, ok, held, done, p.returncode, free()), flush=True)
    finally:
        for group in (open("pgid").read().strip() if os.path.exists("pgid") else "", p.pid):
            try:
                os.killpg(int(group), signal.SIGKILL)
            except (OSError, ValueError):
                pass
        p.wait()
    print(open("hold.err").read(), end="")
case("trap", "trap 'sleep 1; touch done; sleep 0.3; exit 3' TERM; touch started; while :; do sleep 0.1; done")
case("grandchild", "(trap '' TERM; touch started; sleep 1.5; touch done; sleep 0.3) & sleep 30")
case("twice", "trap '' TERM; touch started; sleep 30", signals=2)
case("plain", "touch started; sleep 0.5; exit 5", signals=0)
)", out);
    Check(rc == 0 && Has(out, "trap: started True, held while it ran True, done when it let go True, exit 3, free after True\n")
              && Has(out, "SIGTERM: passed on to what the hold runs; the CHOMPI stays held until it has ended"),
          "tools: chompi.py hold passes SIGTERM on to what it runs and keeps the lock until it has ended");
    Check(Has(out, "grandchild: started True, held while it ran True, done when it let go True, exit 143, free after True\n"),
          "tools: chompi.py hold keeps the lock while a child sh started runs on after sh");
    Check(Has(out, "twice: started True, held while it ran True, done when it let go False, exit 137, free after True\n")
              && Has(out, "killing what the hold runs (SIGKILL)"),
          "tools: a second signal to chompi.py hold kills what it runs, then lets the lock go");
    Check(Has(out, "plain: started True, held while it ran True, done when it let go False, exit 5, free after True\n"),
          "tools: chompi.py hold gives its command's exit code");
    if (rc != 0 || failures)
        printf("%s\n", out.c_str());

    // what a hold passes on reaches tools: one that takes the CHOMPI (claim) and left SIGTERM and
    // SIGHUP as they come exits 128+N on them, its finally run (a lent card taken back), not
    // ended on the spot; one with a handler of its own keeps it
    rc = Python(R"(
import os, signal, subprocess, sys, time
tool = """
import signal, sys, time, chompi
if sys.argv[1] == "own":
    signal.signal(signal.SIGTERM, lambda *_: (print("its own handler", flush=True), sys.exit(3)))
chompi.claim()
try:
    print("claimed", flush=True)
    time.sleep(10)
finally:
    print("finally ran", flush=True)
"""
for kind, sig in (("plain", signal.SIGTERM), ("plain", signal.SIGHUP), ("own", signal.SIGTERM)):
    p = subprocess.Popen([sys.executable, "-c", tool, kind], stdout=subprocess.PIPE, text=True)
    try:
        first = p.stdout.readline().strip()
        p.send_signal(sig)
        rest = p.stdout.read().strip().replace("\n", ", ")
        print("%s, %s: %s, %s, exit %d" % (kind, signal.Signals(sig).name, first, rest, p.wait(5)), flush=True)
    finally:
        if p.poll() is None:
            p.kill()
)", out);
    Check(rc == 0 && Has(out, "plain, SIGTERM: claimed, finally ran, exit 143\n")
              && Has(out, "plain, SIGHUP: claimed, finally ran, exit 129\n")
              && Has(out, "own, SIGTERM: claimed, its own handler, finally ran, exit 3\n"),
          "tools: a tool that took the CHOMPI runs its finally on SIGTERM or SIGHUP (a hold's, passed on), "
          "its own handler kept");
    if (rc != 0 || failures)
        printf("%s\n", out.c_str());

    // at a prompt, the hold gives the terminal to its command's group, and takes it back after:
    // an interactive one (tools/chompi.py hold bash, README.md) reads what's typed, where in a
    // group without the terminal it would stop (SIGTTIN). Under a pty of its own, as a shell
    // runs it: sh, the hold, a command that says whether it has the terminal and reads a line
    rc = Python(R"PY(
import os, pty, select, signal, sys, time, chompi
held = ("import os, sys; print('group', os.getpgrp(), 'has the terminal', os.tcgetpgrp(0) == os.getpgrp(), flush=True); "
        "print('read', sys.stdin.readline().strip(), flush=True)")
after = "import os; print('terminal back', os.tcgetpgrp(0) == os.getpgrp(), flush=True)"
pid, fd = pty.fork()
if pid == 0:
    os.execvp("sh", ["sh", "-c", '"$0" "$1" hold "$0" -c "$2"; "$0" -c "$3"', sys.executable, chompi.__file__, held, after])
out, typed, end = b"", False, time.monotonic() + 6
while time.monotonic() < end:
    if not select.select([fd], [], [], 0.1)[0]:
        continue
    try:
        data = os.read(fd, 4096)
    except OSError:
        break
    if not data:
        break
    out += data
    if not typed and b"has the terminal" in out:
        os.write(fd, b"typed\n")
        typed = True
text = out.decode(errors="replace").replace("\r", "")
for line in text.splitlines():
    if line.startswith("group "):
        try:
            os.killpg(int(line.split()[1]), signal.SIGKILL)
        except (OSError, ValueError):
            pass
try:
    os.killpg(pid, signal.SIGKILL)
except OSError:
    pass
os.waitpid(pid, 0)
print("has the terminal True" in text, "read typed" in text, "terminal back True" in text)
)PY", out);
    Check(rc == 0 && Has(out, "True True True\n"),
          "tools: chompi.py hold gives its command the terminal (hold bash), and takes it back after");
    if (rc != 0 || failures)
        printf("%s\n", out.c_str());

    // builds.py hands make only PATH, HOME, TMPDIR and the locale: the Makefiles take what they
    // don't set from the environment, so GCC_PATH would build with another compiler than the
    // GCC 10.3 checked on the PATH, USB_SAFETY or ITCM into another directory than the build
    // flash.py sends, BENCH, DEBUG, C_DEFS or MAKEFLAGS other bytes. make stubbed: its calls
    // and the environment each got
    rc = Python(R"(
import os, subprocess, builds
os.environ.update(GCC_PATH="/nowhere", USB_SAFETY="1", ITCM="1", BENCH="1", DEBUG="1", C_DEFS="-DX",
                  LDFLAGS="-s", MAKEFLAGS="-e", CPATH="/nowhere", LANG="C.UTF-8", LC_ALL="C", TMPDIR="/tmp")
calls = []
def run(args, env=None, **kw):
    calls.append((args, env))
    return subprocess.CompletedProcess(args, 0, "arm-none-eabi-gcc 10.3.1 20210824 (release)\n"
                                       if args[-1] == "--version" else "", "")
builds.subprocess.run = run
builds.build(False)
builds.stale(builds.image(False))
given = ("GCC_PATH", "USB_SAFETY", "ITCM", "BENCH", "DEBUG", "C_DEFS", "LDFLAGS", "MAKEFLAGS", "CPATH",
         "PATH", "HOME", "TMPDIR", "LANG", "LC_ALL")
for args, env in calls:
    env = os.environ if env is None else env
    print("%s: %s" % (" ".join(args), " ".join(k for k in given if k in env)))
)", out);
    Check(rc == 0 && Has(out, "arm-none-eabi-gcc --version: PATH HOME TMPDIR LANG LC_ALL\n")
              && Has(out, "make -j8: PATH HOME TMPDIR LANG LC_ALL\n") && Has(out, "make -q: PATH HOME TMPDIR LANG LC_ALL\n"),
          "tools: builds.py's make gets no GCC_PATH, USB_SAFETY, ITCM, BENCH, DEBUG, C_DEFS, MAKEFLAGS from the "
          "environment: the GCC 10.3 checked builds the commit's build, where flash.py takes it");
    if (rc != 0 || failures)
        printf("%s\n", out.c_str());

    // loopxfer.py's model answers HELLO with builds.py's kFrizzVersion, read when asked: a
    // Version.h it can't read fails that HELLO, saying so, not every import of loopxfer (remote.py)
    rc = Python(R"(
import os, builds
real = builds.SRC
os.makedirs("src", exist_ok=True)
open("src/Version.h", "w").write("constexpr uint8_t kFrizzVersion[] = {1, 5, 0, 4};\n")
builds.SRC = os.path.abspath("src")
import loopxfer
print("imported, Version.h reshaped", flush=True)
c = loopxfer.Client(loopxfer.ModelLink(loopxfer.Frizz(loopxfer.Card())))
try:
    c.hello()
except SystemExit as e:
    print("hello:", e, flush=True)
builds.SRC = real
print("hello's version is kFrizzVersion:", c.hello()["version"] == builds.frizz_version(), flush=True)
builds.frizz_version = lambda: (9, 8, 7, 6)
print("hello's version:", c.hello()["version"], flush=True)
)", out);
    Check(rc == 0 && Has(out, "imported, Version.h reshaped\nhello: code/src/Version.h has no kFrizzVersion\n")
              && Has(out, "hello's version is kFrizzVersion: True\nhello's version: (9, 8, 7, 6)\n"),
          "tools: loopxfer.py's HELLO says builds.py's kFrizzVersion, read when asked, not at import");
    if (rc != 0 || failures)
        printf("%s\n", out.c_str());

    // which slot runs, no firmware says (the bench answers as FRIZZ does): the tools note the
    // one they started, and go by it. chompi's way to the CHOMPI stubbed: states in turn
    const char* const kSlots = R"(
import os, sys, chompi, flash, midi_send
chompi.time.sleep = lambda s: None
chompi.garbled = lambda: False
os.environ[chompi.HELD] = "7"
states, starts = [], []
def state(device=None):
    return states.pop(0) if len(states) > 1 else states[0]
chompi.state = state
chompi.start = lambda slot, *rest: starts.append(slot) or "started"
def case(name, now, slot, noted=None, hold="7"):
    states[:] = now
    starts.clear()
    chompi.forget()
    if noted:
        os.environ[chompi.HELD] = hold
        chompi.started(noted)
        os.environ[chompi.HELD] = "7"
    got = chompi.to_frizz(slot)
    print("%s: %s %s" % (name, got, starts), flush=True)
frizz = [("frizz", "node")]
case("test runs", frizz, None, 12)
case("test asked", frizz, 12, 12)
case("other asked", frizz, 10, 12)
case("unknown asked", frizz, 12)
case("unknown", frizz, None)
case("bench", frizz, None, 13)
case("booting", [(None, None), ("other", "x"), ("frizz", "node")], None, 12)
case("launcher, this hold", [("launcher", "l")], None, 12)
case("launcher, another", [("launcher", "l")], None, 12, hold="8")
# flash.py: once sent, the launcher for a moment, then nothing, then FRIZZ-TEST
open("image.bin", "wb").write(b"x")
midi_send.main = lambda: None
chompi.to_launcher = lambda *rest: "l"
chompi.slot_file = lambda *rest: None
chompi.features = lambda *rest: 0
states[:] = [("launcher", "l"), (None, None), ("frizz", "node")]
sys.argv = ["flash.py", "image.bin", "--slot", "12"]
flash.main()
print("noted", chompi.last_started()[0])
# and the bench, which answers as FRIZZ does
states[:] = [("launcher", "l"), (None, None), ("frizz", "node")]
sys.argv = ["flash.py", "image.bin", "--slot", "13", "--name", "FRIZZ-BENCH"]
flash.main()
print("left", states)
chompi.forget()
os.remove("image.bin")
)";
    rc = Python(kSlots, out);
    Check(rc == 0 && Has(out, "FRIZZ on slot 12 (") && Has(out, "test runs: node []\n")
              && Has(out, "test asked: node []\n"),
          "tools: FRIZZ-TEST running is told apart, and used");
    Check(Has(out, "other asked: started [10]\n") && Has(out, "unknown asked: started [12]\n")
              && Has(out, "unknown: node []\n") && Has(out, "a slot the tools didn't start"),
          "tools: --slot starts the slot asked for, unless noted as running; without, an unknown one is said");
    Check(Has(out, "the bench runs (slot 13") && Has(out, "bench: started [10]\n"),
          "tools: the bench isn't taken for FRIZZ");
    Check(Has(out, "booting: node []\n"), "tools: a firmware still starting is waited for, not restarted");
    Check(Has(out, "launcher, this hold: started [12]\n")
              && Has(out, "launcher, another: started [10]\n"),
          "tools: from the launcher, the FRIZZ last started in this hold, else FRIZZ's");
    Check(Has(out, "FRIZZ-TEST answers on slot 12") && Has(out, "noted 12\n"),
          "tools: flash.py waits until the FRIZZ it sent answers, and notes its slot");
    Check(Has(out, "FRIZZ-BENCH answers on slot 13") && Has(out, "left [('frizz', 'node')]\n"),
          "tools: and until the bench answers, so a tool run right after finds it");
    if (rc != 0 || !Has(out, "noted 12"))
        printf("%s\n", out.c_str());

    // play --cpu on a FRIZZ that leaves every other ask for the load unanswered: counted, and the
    // sampling goes on; and one whose load reads 0, the bench
    const char* const kLoad = R"(
import remote
class Stub(remote.Frizz):
    def __init__(self, zero):
        self.held, self.zero, self.n = set(), zero, 0
    def send(self, cmd, payload=b""):
        pass
    def raw(self, data):
        pass
    def ask(self, cmd, payload=b"", required=True):
        self.n += 1
        if self.n % 2 and self.n > 1 and not self.zero:
            return None
        return [0, 0, 0, 0] if self.zero else [0, 50 + self.n, 0, 40]
open("wait.txt", "w").write("wait 1500\n")
f = Stub(False)
remote.play(f, "wait.txt", True)
print("asked", f.n >= 6)
remote.play(Stub(True), "wait.txt", True)
)";
    rc = Python(kLoad, out);
    Check(rc == 0 && Has(out, "readings unanswered, their time not in the worst") && Has(out, "asked True")
              && Has(out, "0 throughout: the bench") && Has(out, "load: still 0 after 3 s"),
          "remote: play --cpu counts the load's unanswered asks and samples on; a load of 0 is the bench's");
    if (rc != 0)
        printf("%s\n", out.c_str());

    // play's paced turn after a late wake (a busy computer: here its first send takes 300 ms):
    // the next detent still MS after it, not sooner, which FRIZZ would take as a fast turn
    const char* const kPaced = R"(
import time, remote
class Late(remote.Frizz):
    def __init__(self):
        self.held, self.at = set(), []
    def send(self, cmd, payload=b""):
        if cmd == remote.TURN:
            self.at.append(time.monotonic())
            if len(self.at) == 1:
                time.sleep(0.3)
    def raw(self, data):
        pass
    def ask(self, cmd, payload=b"", required=True):
        return None
open("paced.txt", "w").write("turn 4 -4 200\nwait 1500\n")
f = Late()
remote.play(f, "paced.txt", False)
gaps = [round((b - a) * 1000) for a, b in zip(f.at, f.at[1:])]
print("gaps", gaps, "slow", len(f.at) == 4 and min(gaps) >= 200)
)";
    rc = Python(kPaced, out);
    Check(rc == 0 && Has(out, "slow True"), "remote: play's paced turn keeps MS after a late detent, never less (#375)");
    if (rc != 0 || !Has(out, "slow True"))
        printf("%s\n", out.c_str());

    // play --cpu right after a start: FRIZZ answers 0 / 0 until its first block, then two
    // readings over its start (90 / 48.1, 49.3 / 49.1), then the scenario's (60 / 43.5); the
    // start isn't the worst (#147)
    const char* const kStarting = R"(
import remote
class Starting(remote.Frizz):
    def __init__(self):
        self.held, self.n = set(), 0
    def send(self, cmd, payload=b""):
        pass
    def raw(self, data):
        pass
    def ask(self, cmd, payload=b"", required=True):
        self.n += 1
        return ([0, 0, 0, 0], [0, 0, 0, 0], [7, 4, 3, 97], [3, 109, 3, 107], [4, 88, 3, 51])[min(self.n, 5) - 1]
remote.play(Starting(), "wait.txt", True)
)";
    rc = Python(kStarting, out);
    Check(rc == 0 && Has(out, "load: worst max 60.0%, worst mean 43.5% of the block\n") && !Has(out, "still 0"),
          "remote: play --cpu right after a start opens its window once the start is over, not in the worst");
    if (rc != 0 || !Has(out, "worst mean 43.5%"))
        printf("%s\n", out.c_str());
    unlink((tmp_dir + "/wait.txt").c_str());

    // input sine as SIGNAL (#219): on at its line, input off and input wav turn it off, the
    // script's end, an error or Ctrl-C turn it off; a FRIZZ without SIGNAL skips the lines
    const char* const kSignal = R"(
import remote
class Stub(remote.Frizz):
    def __init__(self, answer=0, stop_at=None):
        self.held, self.sent, self.answer, self.stop_at = set(), [], answer, stop_at
    def send(self, cmd, payload=b""):
        pass
    def raw(self, data):
        if self.stop_at:
            raise self.stop_at()
    def dev(self, sub, args=(), retries=3):
        self.sent.append((sub, list(args)))
        return None if self.answer is None else (self.answer, b"")
def run(name, text, **kw):
    open("signal.txt", "w").write(text)
    f = Stub(**kw)
    try:
        got = remote.play(f, "signal.txt", False)
    except BaseException as e:
        got = type(e).__name__
    print(name, got, [a for sub, a in f.sent if sub == remote.chompi.SIGNAL], flush=True)
run("on", "input sine 220 0.3\nwait 10\n")
run("off", "input sine 330 0.8\nwait 10\ninput off\nwait 10\n")
run("wav", "input sine 110 1\ninput wav loop.wav\nwait 10\n")
run("only-wav", "input wav loop.wav\ninput off\n")
run("none", "wait 10\n")
run("interrupted", "input sine 220 0.02\nmidi 90 3C 64\n", stop_at=KeyboardInterrupt)
run("broken", "input sine 220 0.3\nwait x\n")
run("zero", "input sine 0 0.3\n")
run("old", "input sine 220 0.3\ninput sine 220 0.3\n", answer=None)
)";
    rc = Python(kSignal, out);
    Check(rc == 0 && Has(out, "on 0 [[1, 1, 92, 38, 51], [0]]\n")
              && Has(out, "off 0 [[1, 2, 74, 102, 50], [0]]\n")
              && Has(out, "input: the test signal (SIGNAL) in AUX's place, and the mic's"),
          "remote: play sends input sine as SIGNAL, and SIGNAL off at input off or the script's end (#219)");
    Check(Has(out, "wav 0 [[1, 0, 110, 127, 127], [0]]\n")
              && Has(out, "skipped (twin only): input wav (the device's test signal plays a sine): the signal off")
              && Has(out, "only-wav 0 []\n") && Has(out, "none 0 []\n"),
          "remote: input wav turns the signal off, skipped; a script without input sends no SIGNAL");
    Check(Has(out, "interrupted KeyboardInterrupt [[1, 1, 92, 2, 72], [0]]\n")
              && Has(out, "broken ValueError [[1, 1, 92, 38, 51], [0]]\n"),
          "remote: a script interrupted (Ctrl-C) or broken off turns its signal off");
    Check(Has(out, "zero 1 []\n") && Has(out, "line 1: input sine 0 0.3: 1-16383 Hz")
              && Has(out, "old 0 [[1, 1, 92, 38, 51]]\n")
              && Has(out, "skipped (twin only): input, a FRIZZ without SIGNAL"),
          "remote: a frequency out of range fails its line; a FRIZZ without SIGNAL skips input, asked once");
    if (rc != 0 || !Has(out, "on 0 "))
        printf("%s\n", out.c_str());
    unlink((tmp_dir + "/signal.txt").c_str());

    // expect led at the brightness that fits every line so far: one that fits only 100 %
    // after one that fits only 50 % fails, saying at which it looked (#94)
    const char* const kLevels = R"(
import remote
class Stub(remote.Frizz):
    def __init__(self, shown):
        self.held, self.shown = set(), shown
    def send(self, cmd, payload=b""):
        pass
    def leds(self):
        return [(0, 0, 0)] * 10, [self.shown.pop(0)] + [(0, 0, 0)] * 24
open("levels.txt", "w").write("expect led smt 0 fc0000\nexpect led smt 0 fc0000\n")
print("full", remote.play(Stub([(63, 0, 0), (63, 0, 0)]), "levels.txt", False), flush=True)
print("75", remote.play(Stub([(47, 0, 0), (47, 0, 0)]), "levels.txt", False), flush=True)
print("mixed", remote.play(Stub([(31, 0, 0), (63, 0, 0)]), "levels.txt", False), flush=True)
print("dark", remote.play(Stub([(0, 0, 0), (0, 0, 0)]), "levels.txt", False), flush=True)
)";
    rc = Python(kLevels, out);
    Check(rc == 0 && Has(out, "full 0\n") && Has(out, "the LEDs at 75 % brightness") && Has(out, "75 0\n"),
          "remote: expect led holds at 100 % and at 75 %, saying which when it isn't 100 %");
    Check(Has(out, "line 2: smt 0 is fc0000, not fc0000, ~7c0000 at 50 %\n") && Has(out, "mixed 1\n")
              && Has(out, "dark 2\n"),
          "remote: but the level must fit every line, saying what it would show, and dark is none of them");
    if (rc != 0 || !Has(out, "mixed 1"))
        printf("%s\n", out.c_str());
    unlink((tmp_dir + "/levels.txt").c_str());

    if (!usb.Open())
    {
        Check(false, "remote: a pseudo-terminal for the twin's USB");
        return Finish();
    }
    Boot();
    RunMs(kReadyMs);

    rc = Remote({"state"}, out);
    Check(rc == 0 && Has(out, "looper     empty") && Has(out, "knobs on   freezer")
              && Has(out, "tempo      120.0 BPM") && Has(out, "out 0.75, in 0.75"),
          "remote: state reads the play page: no loop, the freezer's knobs, 120 BPM, the gains");
    if (rc != 0)
        printf("%s\n", out.c_str());

    // PLAY blinks the boot tempo's beat (kPlayLed), which may turn between remote.py's read and
    // the twin's (#124): compared apart, as one of the colours it shows over a beat
    std::vector<std::string> play_beat;
    for (int t = 0; t < 600; t += 10)
    {
        const std::string now = PlayLed(LedLine());
        if (std::find(play_beat.begin(), play_beat.end(), now) == play_beat.end())
            play_beat.push_back(now);
        RunMs(10);
    }
    const auto on_beat = [&](const std::string& led)
    { return std::find(play_beat.begin(), play_beat.end(), led) != play_beat.end(); };
    rc = Remote({"leds"}, out);
    const std::string leds = LedLine();
    const std::string play_remote = PlayLed(out), play_twin = PlayLed(leds);
    Check(rc == 0 && WithoutPlay(out) == WithoutPlay(leds) + "\n",
          "remote: leds prints every LED as the twin's LED log does");
    Check(play_beat.size() == 2 && on_beat(play_remote) && on_beat(play_twin),
          "remote: and PLAY in one of the two colours the twin's beat shows");
    if (WithoutPlay(out) != WithoutPlay(leds) + "\n" || !on_beat(play_remote))
        printf("      remote: %s      twin:   %s\n", out.c_str(), leds.c_str());

    rc = Remote({"load"}, out);
    Check(rc == 0 && Has(out, "max") && Has(out, "mean"), "remote: load reads the load");

    rc = Remote({"load", "--device", usb.path}, out);
    Check(rc == 0 && Has(out, "max"), "remote: --device after the subcommand counts too");
    if (rc != 0)
        printf("%s\n", out.c_str());

    rc = Remote({"play", "nothing.txt", "--fresh"}, out);
    Check(rc != 0 && Has(out, "--fresh starts the slot again through the launcher: not with --device"),
          "remote: play --fresh, which starts the slot again, is refused with --device (#123)");

    Remote({"channel", "5", "--save"}, out);
    Remote({"transport", "on", "--save"}, out);
    rc = Remote({"settings"}, out);
    Check(rc == 0 && out == "channel 5, transport on, clock auto, out off\n",
          "remote: channel and transport set, settings reads them");
    Remote({"channel", "0", "--save"}, out);
    rc = Remote({"settings"}, out);
    Check(rc == 0 && out == "channel all, transport on, clock auto, out off\n", "remote: channel 0 is every channel");
    Remote({"source", "usb", "--save"}, out);
    rc = Remote({"settings"}, out);
    Check(rc == 0 && out == "channel all, transport on, clock usb, out off\n", "remote: source sets the clock source");
    Remote({"out", "trs", "--save"}, out);
    rc = Remote({"settings"}, out);
    Check(rc == 0 && out == "channel all, transport on, clock usb, out trs\n", "remote: out sets MIDI out");
    RunMs(2500);
    Check(Has(Card("/FRIZZ/frizz_master.txt"), "midi_channel 0\nmidi_transport 1\n")
              && Has(Card("/FRIZZ/frizz_master.txt"), "clock_source 2\nmidi_out 1\n"),
          "remote: and they go to the card");

    // MIDI out over USB: its ticks come in with the answers, and remote.py reads past them: the
    // ticks of 200 ms queued on the pty ahead of the answers (on the twin's clock remote.py's
    // start takes none of its time, and a query a few ms, between two ticks)
    Remote({"out", "all", "--save"}, out);
    TakeMidiOut();
    RunMs(200);
    size_t usb_ticks = 0;
    for (const MidiOutByte& b : TakeMidiOut())
        usb_ticks += b.usb && b.byte == 0xF8;
    rc = Remote({"state"}, out);
    Check(rc == 0 && Has(out, "looper     empty") && usb_ticks > 0,
          "remote: with MIDI out over USB, its ticks queued ahead of the answers, state still reads");
    if (rc != 0)
        printf("%s\n", out.c_str());
    Remote({"out", "off", "--save"}, out);

    // a scene: the blank one read, the filter closed and latched in it, sent to slot 2
    const std::string blank = tmp_dir + "/blank.json", mine = tmp_dir + "/mine.json",
                      back = tmp_dir + "/back.json";
    rc = Remote({"scene", "get", "0", blank}, out);
    std::string json = Read(blank);
    Check(rc == 0 && Has(json, "\"used\": true") && fabsf(JsonParam(json, "filter", 0) - .5f) < 1e-4f,
          "remote: scene get reads the blank scene, the filter's cutoff at its centre");
    const size_t cut = json.find("\"filter\": [") + 11;
    json.replace(cut, json.find(',', cut) - cut, "0.25");
    const size_t latched = json.find("\"latched\": [") + 12;
    json.insert(latched, "\"filter\"");
    // and the phaser as the frequency shifter (#306)
    const size_t modes = json.find("\"modes\": [") + 10;
    Check(modes > 10 && json.compare(modes, 1, "]") == 0, "remote: scene get says no effect is in its second mode");
    json.insert(modes, "\"phaser\"");
    Write(mine, json);
    rc = Remote({"scene", "put", "2", mine, "--save"}, out);
    RunMs(2500);
    const std::string scenes = Card("/FRIZZ/frizz_scenes.txt");
    const size_t phaser = scenes.find("phaser ", scenes.find("scene 2\n"));
    Check(rc == 0 && Has(scenes, "scene 2\n") && Has(scenes, "filter 1 250") && phaser != std::string::npos
              && scenes.compare(scenes.find('\n', phaser) - 2, 2, " 1") == 0,
          "remote: scene put stores it in slot 2, on the card, the phaser's mode its ninth value");
    if (rc != 0)
        printf("%s\n", out.c_str());
    rc = Remote({"scene", "get", "2", back}, out);
    json = Read(back);
    Check(rc == 0 && fabsf(JsonParam(json, "filter", 0) - .25f) < 1e-4f && Has(json, "\"filter\"\n")
              && Has(json, "\"modes\": [\n  \"phaser\"\n ]"),
          "remote: and scene get reads it back, the same within 14 bits, the phaser's mode too");
    rc = Remote({"scene", "put", "0", mine, "--save"}, out);
    Check(rc != 0 && Has(out, "refused"), "remote: never into the blank scene");
    Check(fabsf(JsonParam(json, "shifter", 4) - 1.f) < 1e-4f,
          "remote: a scene has both pages, the shifter's mix 5th, on its default");
    // a scene from before page 2, four knobs each: page 2 comes from the blank scene
    std::string old = "{\"used\": true, \"latched\": [], \"params\": {";
    for (const char* fx : {"freezer", "shifter", "folder", "crusher", "filter", "flanger",
                           "resonator", "slicer", "warble", "tapestop", "delay", "reverb"})
        old += std::string(fx[0] == 'f' && fx[1] == 'r' ? "" : ", ") + "\"" + fx
               + "\": [0.5, 0.5, 0.5, 0.5]";
    Write(mine, old + "}}");
    rc = Remote({"scene", "put", "3", mine, "--save"}, out);
    rc |= Remote({"scene", "get", "3", back}, out);
    json = Read(back);
    Check(rc == 0 && fabsf(JsonParam(json, "shifter", 0) - .5f) < 1e-4f
              && fabsf(JsonParam(json, "shifter", 4) - 1.f) < 1e-4f,
          "remote: a scene of four knobs each puts page 2 on its defaults");

    // which build runs (kCmdDev's IDENT): FRIZZ, the twin's image 0 B; the card written, as no
    // script has played yet
    rc = Remote({"ident"}, out);
    Check(rc == 0 && Has(out, "FRIZZ, crc32 00000000, 0 B") && Has(out, "MainLoop ")
              && Has(out, "the card: written as FRIZZ writes it"),
          "remote: ident says FRIZZ runs, its uptime, and that it writes the card");
    if (rc != 0)
        printf("%s\n", out.c_str());

    // the developers' probes (#105): the clock and the looper, a test signal the levels show,
    // the random numbers reseeded, the battery
    rc = Remote({"probe"}, out);
    Check(rc == 0 && Has(out, ", counting none, factor x1")
              && Has(out, "MIDI       0.00 BPM") && Has(out, "looper     empty")
              && Has(out, "loop       0 frames") && Has(out, "TRS in     restarted 0 times after a receive error"),
          "remote: probe says the clock, the jack's receive restarts (#361) and the looper");
    if (rc != 0)
        printf("%s\n", out.c_str());
    rc = Remote({"signal", "sine", "1000", "0.5"}, out);
    rc |= Remote({"levels"}, out);
    Check(rc == 0 && Has(out, "AUX L         0.500") && Has(out, "-6.0 dBFS") && Has(out, "mic           0.500")
              && !Has(out, "line L        0.0000") && Has(out, "effects    freezer 0.00"),
          "remote: signal plays a sine at half scale where the input was, levels shows it at -6 dBFS");
    Check(rc == 0 && Has(out, "finalizer  input ") && Has(out, "; limiter "),
          "remote: levels shows the finalizer's meters too (LEVELS 2, #408)");
    if (rc != 0 || !Has(out, "-6.0 dBFS"))
        printf("%s\n", out.c_str());
    rc = Remote({"signal", "off"}, out);
    rc |= Remote({"levels"}, out);
    Check(rc == 0 && Has(out, "AUX L         0.0000     silent"), "remote: signal off, the input again");
    rc = Remote({"signal", "sine", "0"}, out);
    Check(rc != 0 && Has(out, "1-16383 Hz"), "remote: signal wants a frequency");
    rc = Remote({"reseed", "12345"}, out);
    Check(rc == 0 && Has(out, "seed       12345\nchaos      ") && Has(out, "slicer     ")
              && Has(out, "delay      ") && Has(out, "warble     00003038, 0 draws since"),
          "remote: reseed sets the random numbers from a seed and counts their draws since");
    rc = Remote({"battery"}, out);
    Check(rc == 0 && Has(out, "battery    high (above 3.3 V)") && Has(out, "cable 00000000")
              && Has(out, "MP2722     status 0x11-0x16: "),
          "remote: battery says the level and the charger's status, as last read");
    if (rc != 0)
        printf("%s\n", out.c_str());

    // the loop loader (#70): slots put on the card and scanned again, one loaded and checked
    // against its file, a fault provoked, the write test, which leaves nothing behind
    {
        const std::string a01 = LoopWav(96000, 48000, 2), wav = tmp_dir + "/a01.wav";
        std::ofstream(wav, std::ios::binary) << a01;
        CardFiles()["/FRIZZ/a1.wav"] = a01;
        CardFiles()["/FRIZZ/c15.WAV"] = LoopWav(1000, 44100, 2);
        CardFiles()["/cubbi_b2.wav"] = LoopWav(1000, 48000, 2);
        rc = Remote({"loops", "--scan"}, out);
        Check(rc == 0 && Has(out, "a1  stereo, 96000 frames (2.00 s), its data at 44\n")
                  && Has(out, "c15  not 48 kHz\n") && Has(out, "cubbi_b2  stereo") && Has(out, "3 slots filled"),
              "remote: loops --scan lists the slots on the card, a refused one with why");
        if (rc != 0)
            printf("%s\n", out.c_str());
        rc = Remote({"loops", "load", "a", "1", "--check", wav}, out);
        Check(rc == 0 && Has(out, "a1 loaded: 0.38 of 0.38 MB in ") && Has(out, ", 3 runs, the file in 1 fragment\n")
                  && Has(out, "the audio callback meanwhile: max ") && Has(out, "a01.wav's data 0x")
                  && Has(out, ": the same"),
              "remote: loops load reads a slot in runs, its CRC-32 the file's data's");
        if (rc != 0)
            printf("%s\n", out.c_str());
        rc = Remote({"loops", "load", "A", "1", "--fault", "past-end"}, out);
        Check(rc == 1 && Has(out, "a1 failed, card error: 0.26 of 0.38 MB") && Has(out, "SD error 0x2000000")
                  && Has(out, "card recovered 1 times"),
              "remote: a load's fault provoked: it stops at once, why, the card recovered");
        if (rc != 1)
            printf("%s\n", out.c_str());
        CardFiles().erase("/FRIZZ/a1.wav");
        CardFiles().erase("/FRIZZ/c15.WAV");
        CardFiles().erase("/cubbi_b2.wav");
        const size_t files = CardFiles().size();
        rc = Remote({"loops", "write", "16", "300"}, out);
        bool left = false;
        for (const auto& f : CardFiles())
            left = left || f.first.find("/FRIZZ/T") != std::string::npos;
        Check(rc == 0 && Has(out, "write test, 16 KB in 300-byte writes: passed") && !left
                  && CardFiles().size() == files,
              "remote: loops write writes, renames, reads back and deletes a file in /FRIZZ/T");
        if (rc != 0)
            printf("%s\n", out.c_str());
        unlink(wav.c_str());
    }
    // the loop transfer (#278, tools/loopxfer.py's client): an upload, loaded back and its CRC
    // checked, the names in the listing; the pty dropping CHUNKs, go-back-N on the firmware;
    // moved, swapped, deleted, a cubbi copied; only FRIZZ's slots changed on the card
    {
        CardFiles()["/TAPE/cubbi_c3.wav"] = LoopWav(3000, 48000, 2);
        const auto before = CardFiles();
        const std::string wav = tmp_dir + "/h1.wav", other = tmp_dir + "/h2.wav";
        std::string named = LoopWav(10000, 48000, 2);
        // a LIST/INFO/INAM before the data: "Loop ünë" (UTF-8), its NUL, a pad byte
        const std::string name = "Loop \xC3\xBCn\xC3\xAB";
        std::string list = "LIST";
        const uint32_t inam = static_cast<uint32_t>(name.size() + 1), size = 4 + 8 + inam + (inam & 1);
        list.append(reinterpret_cast<const char*>(&size), 4);
        list += "INFOINAM";
        list.append(reinterpret_cast<const char*>(&inam), 4);
        list += name + std::string(1 + (inam & 1), '\0');
        named.insert(36, list);
        const uint32_t riff = static_cast<uint32_t>(named.size() - 8);
        named.replace(4, 4, reinterpret_cast<const char*>(&riff), 4);
        std::ofstream(wav, std::ios::binary) << named;
        std::ofstream(other, std::ios::binary) << LoopWav(7000, 48000, 1);
        rc = Remote({"loops", "rescan"}, out); // what the loader's check above left
        rc = Remote({"loops", "put", wav, "h", "1", "--cpu"}, out);
        Check(rc == 0 && Has(out, "h1.wav -> h1: " + std::to_string(named.size()) + " bytes in ")
                  && Has(out, "KB/s; 409 CHUNKs, 0 rewinds") && Has(out, "the audio callback meanwhile: max ")
                  && Card("/FRIZZ/h1.wav") == named,
              "remote: loops put uploads a WAV into h1, byte for byte, its rate and the load meanwhile");
        if (rc != 0)
            printf("%s\n", out.c_str());
        rc = Remote({"loops", "load", "h", "1", "--check", wav}, out);
        Check(rc == 0 && Has(out, ": the same"), "remote: loops load of the upload: its CRC-32 the file's data's");
        usb_drop = DropChunk;
        rc = Remote({"loops", "put", other, "h", "2"}, out);
        usb_drop = nullptr;
        const bool whole = Card("/FRIZZ/h2.wav") == Read(other);
        Check(rc == 0 && chunks_dropped == 20 && !Has(out, ", 0 rewinds") && whole,
              "remote: loops put with every 7th CHUNK lost on its first sending: rewound, the file whole");
        const size_t line = out.rfind("h2.wav ->");
        printf("      %d CHUNKs dropped: %s", chunks_dropped, line == std::string::npos ? "\n" : out.substr(line).c_str());
        if (rc != 0 || !whole)
        {
            printf("%s\n      (the twin was %ld ms behind the wall clock at most)\n", out.c_str(), lag);
            // the cases after it from h2 as it should be, not failing with this one
            CardFiles()["/FRIZZ/h2.wav"] = Read(other);
            Remote({"loops", "rescan"}, out);
        }
        rc = Remote({"loops", "ls"}, out);
        Check(rc == 0 && Has(out, "h1  stereo, 10000 frames (0.21 s), " + std::to_string(named.size()) + " B, \"" + name + "\"\n")
                  && Has(out, "h2  mono, 7000 frames") && Has(out, "cubbi_c3  stereo")
                  && Has(out, "the cubbis in /TAPE"),
              "remote: loops ls lists the slots with their names");
        if (rc != 0)
            printf("%s\n", out.c_str());
        rc = Remote({"loops", "mv", "h", "1", "h", "2"}, out);
        Check(rc == 1 && Has(out, "move h1 to h2: EXISTS"), "remote: loops mv onto a loop: refused without --replace");
        rc = Remote({"loops", "mv", "h", "1", "h", "3"}, out);
        const int mv = rc;
        rc = Remote({"loops", "swap", "h", "2", "h", "3"}, out);
        const int sw = rc;
        Check(mv == 0 && sw == 0 && Card("/FRIZZ/h2.wav") == named && Card("/FRIZZ/h3.wav") == Read(other)
                  && !CardFiles().count("/FRIZZ/h1.wav"),
              "remote: loops mv, then swap: the files where they were sent");
        rc = Remote({"loops", "cp", "cubbi_c", "3", "h", "4"}, out);
        Check(rc == 0 && Card("/FRIZZ/h4.wav") == Card("/TAPE/cubbi_c3.wav"),
              "remote: loops cp copies TAPE's cubbi into h4 on the card, the cubbi as it was");
        if (rc != 0)
            printf("%s\n", out.c_str());
        for (const char* s : {"2", "3", "4"})
            Remote({"loops", "rm", "h", s}, out);
        rc = Remote({"loops", "rescan"}, out);
        Check(rc == 0 && CardFiles() == before, "remote: loops rm and rescan: the card as it was, nothing else touched");
        CardFiles().erase("/TAPE/cubbi_c3.wav");
        unlink(wav.c_str());
        unlink(other.c_str());
    }
    // the crash record (#105): none on the twin; one as the fault handler writes it, read back
    // and cleared; a watchdog's, as the next start records it
    rc = Remote({"crash"}, out);
    Check(rc == 0 && Has(out, "crash      none recorded") && Has(out, "no option bytes (the twin)"),
          "remote: crash says none is recorded");
    {
        using namespace chompi;
        const uint32_t frame[8] = {0, 0, 0, 0, 0, 0x24001235, 0x24004214, 0x01000003};
        const uint32_t fault[4] = {0x8200, 0x40000000, 0, 0x2FFF0000};
        crash::Write(crash_mem.record, crash::HARD_FAULT, crash_mem.running, 12345, 0x00A00000, frame, fault);
    }
    rc = Remote({"crash"}, out);
    Check(rc == 0 && Has(out, "crash      HardFault, 1 since cleared\nbuild      FRIZZ, crc32 00000000, 0 B")
              && Has(out, "when       12.345 s up (that start's RCC_RSR 00a00000: brown-out, power-on)")
              && Has(out, "PC         24004214\nLR         24001235\nxPSR       01000003 (exception 3)")
              && Has(out, "CFSR       00008200 PRECISERR BFARVALID\nHFSR       40000000 FORCED\nBFAR       2fff0000")
              && !Has(out, "MMFAR") && Has(out, " -f -C -i -e FRIZZ.elf (make in a checkout") && Has(out, "24004214 24001234"),
          "remote: crash reads a fault's record: the build, when, the registers, the faults' bits, addr2line");
    if (rc != 0 || !Has(out, "24004214 24001234"))
        printf("%s\n", out.c_str());
    rc = Remote({"crash", "--clear"}, out);
    std::string after;
    rc |= Remote({"crash"}, after);
    Check(rc == 0 && Has(out, "HardFault") && Has(out, "\ncleared") && Has(after, "none recorded"),
          "remote: crash --clear clears it");
    {
        using namespace chompi;
        const crash::Running running = crash_mem.running;
        const uint32_t alive = running.alive_ms;
        crash::Boot(crash_mem, crash::kIwdgResetFlag | 1u << 24, 0x1234, 99);
        rc = Remote({"crash"}, out);
        char when[96];
        snprintf(when, sizeof(when), "MainLoop last ran at %.3f s up; RCC_RSR 05000000 (software, watchdog)",
                 alive / 1000.);
        Check(rc == 0 && Has(out, "crash      the watchdog: the audio callback stopped for 1 s, 1 since cleared")
                  && Has(out, when) && !Has(out, "PC ") && crash_mem.running.crc == 0x1234,
              "remote: a watchdog reset is recorded at the next start, with when MainLoop last ran");
        if (rc != 0 || !Has(out, when))
            printf("%s\n%s\n", out.c_str(), when);
        crash::Boot(crash_mem, 1u << 24, 0, 0);
        Check(crash::Valid(crash_mem.record) && crash_mem.record.kind == crash::WATCHDOG,
              "crash: a start without the watchdog's flag keeps the record");
        crash_mem.running = running;
        crash::Clear(crash_mem.record);
    }
    {
        // a stacking fault, or an SP where no stack is: the handler doesn't read the frame (a
        // second fault there would lock the core up), it keeps the SP and the fault's bits
        using namespace chompi;
        Check(crash::FrameReadable(0x2001ff00u, 0x8200) && crash::FrameReadable(0x24070000u, 0)
                  && !crash::FrameReadable(0x2001ff00u, 0x1000) && !crash::FrameReadable(0x2001ff00u, 0x10)
                  && !crash::FrameReadable(0x2001fff0u, 0) && !crash::FrameReadable(0x60000000u, 0)
                  && !crash::FrameReadable(0x2001ff02u, 0),
              "crash: the stacked frame is read only if stacked, in DTCM or the AXI SRAM");
        crash::Record r = {};
        const uint32_t frame[8] = {0, 0, 0, 0, 0, 0x24001235, 0x24004214, 0x01000003};
        const uint32_t fault[4] = {0x1000, 0x40000000, 0, 0};
        crash::Write(r, crash::HARD_FAULT, crash_mem.running, 1, 0, frame, fault, false);
        Check(crash::Valid(r) && r.pc == 0 && r.lr == 0 && r.xpsr == 0 && r.cfsr == 0x1000
                  && r.sp == (reinterpret_cast<uintptr_t>(frame) & 0xFFFFFFFFu),
              "crash: a frame not stacked isn't read, its SP and the fault's bits recorded");
    }
    rc = Remote({"crash-test", "fault-main"}, out);
    Check(rc != 0 && Has(out, "TEST: not in this build"), "remote: crash-test, on FRIZZ: not in this build");

    // RESET, taken in the audio callback as it comes: with the wrong key nothing, with
    // remote.py's the chip's reset (the twin notes it)
    for (uint8_t b : {0xF0, 0x7D, 0x43, 0x48, 0x3E, 0x0A, 0x01, 0x52, 0x53, 0x53, 0xF7})
        UsbMidi(b);
    RunMs(20);
    const bool kept_running = !Restarted();
    rc = Remote({"reset", "--no-wait"}, out);
    RunMs(20);
    Check(rc == 0 && kept_running && Restarted() && Has(out, "RESET sent"),
          "remote: reset resets the chip from the audio callback; not without its key");

    // a script played on the device: the filter's key held, its LED expected as the twin
    // shows it held and let go, with the load
    const std::string off = Hex(SmtLedFull(kFilterKeyLed));
    Press("KEY_5", true);
    RunMs(300);
    const std::string on = Hex(SmtLedFull(kFilterKeyLed));
    Press("KEY_5", false);
    RunMs(300);
    const std::string script = tmp_dir + "/script.txt";
    // (the filter key's LED shows the input's level, here none: the test signal only after, #219)
    Write(script, "booted\nat 100\ndown KEY_5\nwait 300\nexpect led smt 20 " + on +
                      "\nup KEY_5\nwait 300\nexpect led smt 20 " + off + "\ninput sine 220 0.3\nwait 300\n");
    rc = Remote({"play", script, "--cpu"}, out);
    Check(rc == 0 && on != off && Has(out, "input: the test signal (SIGNAL) in AUX's place")
              && !Has(out, "skipped (twin only): input") && Has(out, "load: worst max"),
          "remote: play holds a key over SysEx, its LED as expected, and reports the load");
    if (rc != 0)
        printf("%s\n", out.c_str());
    rc = Remote({"levels"}, out);
    Check(rc == 0 && Has(out, "AUX L         0.0000     silent"),
          "remote: play's input sine went as the test signal, off again at the script's end (#219)");
    if (rc != 0 || !Has(out, "AUX L         0.0000"))
        printf("%s\n", out.c_str());
    Write(script, "booted\ndown KEY_5\nwait 300\nexpect led smt 20 " + off + "\nup KEY_5\n");
    rc = Remote({"play", script}, out);
    Check(rc == 1 && Has(out, "line 4: smt 20 is " + on + ", not " + off),
          "remote: and an LED that isn't as expected fails the run, telling which");
    rc = Remote({"state"}, out);
    Check(rc == 0 && Has(out, "knobs on   filter"), "remote: the script's key selected the filter");

    // the LEDs dimmed to 50 % on the settings page (knob 1 ten detents down, led_brightness 50
    // on the card, #176): the same expectations, at full scale, hold at the level that fits
    // them, said (#94)
    auto brightness_knob = [](int detents) {
        SetToggle(true);
        RunMs(300);
        Turn(4, detents); // knob 1, SW4
        RunMs(300);
        SetToggle(false);
        RunMs(300);
    };
    brightness_knob(-10);
    const std::string dim_off = Hex(SmtLedFull(kFilterKeyLed));
    Write(script, "booted\ndown KEY_5\nwait 300\nexpect led smt 20 " + on + "\nup KEY_5\nwait 300\n"
                  "expect led smt 20 " + off + "\n");
    rc = Remote({"play", script}, out);
    Check(rc == 0 && dim_off != off && Has(out, "the LEDs at 50 % brightness"),
          "remote: play's expect led holds at the card's 50 % brightness, and says it's 50 % (#94)");
    if (rc != 0 || !Has(out, "50 %"))
        printf("%s\n", out.c_str());
    Write(script, "booted\ndown KEY_5\nwait 300\nexpect led smt 20 " + off + "\nup KEY_5\n");
    rc = Remote({"play", script}, out);
    Check(rc == 1 && Has(out, "line 4: smt 20 is "),
          "remote: and an LED that isn't as expected at any brightness still fails");
    brightness_knob(10); // and back to 100 %
    Check(Hex(SmtLedFull(kFilterKeyLed)) == off, "remote: the LEDs at full again");

    // whatever ends a script, the keys it holds are let go, the switch is the hand's again
    // (a key goes down on the page shown when the next block reads it: a wait before toggling)
    Write(script, "booted\ndown KEY_5\nwait 100\ntoggle 1\nwait 200\nwait x\n");
    rc = Remote({"play", script}, out);
    const std::string broke = out;
    RunMs(100);
    const std::string after_error = Hex(SmtLedFull(kFilterKeyLed));
    Remote({"state"}, out);
    Check(rc != 0 && Has(broke, "could not convert") && after_error == off
              && Has(out, "page       play; the mode switch stands down\n"),
          "remote: a script that breaks off lets go of its key and gives the switch back");
    if (after_error != off || !Has(out, "page       play"))
        printf("%s\n%s\n", broke.c_str(), out.c_str());
    // (stderr's "skipped" says when it's there, in the wait)
    Write(script, "booted\ninput sine 1000 0.5\ndown KEY_5\nwait 100\ntoggle 1\nbattery 3.0\n"
                  "wait 5000\nup KEY_5\n");
    rc = Remote({"play", script, "--cpu"}, out, "skipped (twin only): battery");
    const std::string interrupted = out;
    RunMs(100);
    const std::string after_interrupt = Hex(SmtLedFull(kFilterKeyLed));
    Remote({"state"}, out);
    Check(rc != 0 && Has(interrupted, "interrupted: the keys let go") && after_interrupt == off
              && Has(out, "page       play; the mode switch stands down\n"),
          "remote: and so does one interrupted (Ctrl-C)");
    if (after_interrupt != off || !Has(out, "page       play"))
        printf("%s\n%s\n", interrupted.c_str(), out.c_str());
    rc = Remote({"levels"}, out);
    Check(rc == 0 && Has(interrupted, "input: the test signal") && Has(out, "AUX L         0.0000     silent"),
          "remote: and turns its test signal off (#219)");
    if (rc != 0 || !Has(out, "AUX L         0.0000"))
        printf("%s\n%s\n", interrupted.c_str(), out.c_str());

    // the mode switch over SysEx (kCmdSwitch): up shows the settings page, and SysEx keys
    // reach it as the hand's do; `switch hand` gives the real switch back
    Remote({"switch", "up"}, out);
    rc = Remote({"state"}, out);
    Check(rc == 0 && Has(out, "page       settings; the mode switch stands down, SysEx holds it up"),
          "remote: switch up shows the settings page, and state says so");
    // play starts with the switch down, as the twin starts, whatever the device shows (#253):
    // the script's keys reach the play page, and the switch is the hand's again after
    Write(script, "booted\ndown KEY_5\nwait 300\nexpect led smt 20 " + on + "\nup KEY_5\n");
    rc = Remote({"play", script}, out);
    const std::string held_up = out;
    Remote({"state"}, out);
    Check(rc == 0 && Has(out, "page       play; the mode switch stands down\n"),
          "remote: play puts a switch SysEx holds up down, as the twin starts, then the hand's (#253)");
    if (rc != 0 || !Has(out, "page       play"))
        printf("%s\n%s\n", held_up.c_str(), out.c_str());
    SetToggle(true);
    RunMs(300);
    rc = Remote({"play", script}, out);
    const std::string hand_up = out;
    Remote({"state"}, out);
    Check(rc == 0 && Has(out, "page       settings; the mode switch stands up"),
          "remote: and the hand's switch up: the keys still reach the play page, the hand's after (#253)");
    if (rc != 0 || !Has(out, "page       settings"))
        printf("%s\n%s\n", hand_up.c_str(), out.c_str());
    SetToggle(false);
    RunMs(300);
    // the guard of a FRIZZ from before PERSIST (card_use), as --save keeps it: a key on the
    // settings page the script toggles up to
    Write(script, "booted\ntoggle 1\nwait 100\ndown KEY_26\ntap KEY_23\nup KEY_26\nwait 100\n"); // SHIFT + the upper F#: mono
    rc = Remote({"play", script, "--save"}, out);
    const std::string refused = out;
    Remote({"state"}, out);
    Check(rc != 0 && Has(refused, "line 4: down KEY_26 on the settings page") && Has(refused, "--force")
              && !Has(out, ", mono"),
          "remote: play refuses a key on the settings page, saying why, and sends none (#58)");
    if (rc == 0 || !Has(refused, "--force"))
        printf("%s\n", refused.c_str());
    Remote({"play", script, "--save", "--force"}, out);
    rc = Remote({"state"}, out);
    Check(rc == 0 && Has(out, ", mono") && Has(out, "page       play; the mode switch stands down\n"),
          "remote: a SysEx key there sets mono; the play page again after the script");
    if (!Has(out, ", mono") || !Has(out, "page       play"))
        printf("%s\n", out.c_str());
    // a script's toggle: up and down over SysEx, the real switch again at its end
    Write(script, "booted\ntoggle 1\nwait 100\ndown KEY_26\ntap KEY_23\nup KEY_26\nwait 100\n"
                  "toggle 0\nwait 100\n");
    rc = Remote({"play", script, "--save"}, out);
    Check(rc != 0 && Has(out, "line 4: down KEY_26 on the settings page"),
          "remote: and one the script toggles up to, by the line (#58)");
    rc = Remote({"play", script, "--save", "--force"}, out);
    const int played = rc;
    rc = Remote({"state"}, out);
    Check(played == 0 && rc == 0 && !Has(out, ", mono") && Has(out, "page       play; the mode switch stands down\n"),
          "remote: play's toggle 1 / toggle 0 reach the settings page (mono off again), the real switch after");
    if (!Has(out, "page       play"))
        printf("%s\n", out.c_str());
    // what remote.py sent last, the keys let go and SWITCH 0 in play's finally, still reaches the
    // twin as it ends (#336): on the twin's clock the bytes it wrote after its last wait; on the
    // wall clock (FRIZZ_TWIN_CLOCK=0) with the twin 400 ms behind as it ends (all.sh's load)
    Write(script, "booted\ntoggle 1\nwait 100\ndown KEY_26\ntap KEY_23\nup KEY_26\nwait 100\n");
    exec_late = 400;
    Remote({"play", script, "--save", "--force"}, out);
    exec_late = 0;
    rc = Remote({"state"}, out);
    Check(rc == 0 && Has(out, ", mono") && Has(out, "page       play; the mode switch stands down\n"),
          "remote: the twin 400 ms behind as play ends: its last messages still reach it, mono and the play page (#336)");
    if (!Has(out, ", mono") || !Has(out, "page       play"))
        printf("%s\n", out.c_str());
    Remote({"play", script, "--save", "--force"}, out); // mono off again
    rc = Remote({"state"}, out);
    Check(rc == 0 && !Has(out, ", mono") && Has(out, "page       play; the mode switch stands down\n"),
          "remote: and mono off again, on time");
    // the settings page only looked at plays; a setting over SysEx doesn't
    Write(script, "booted\ntoggle 1\nwait 100\ntoggle 0\nwait 100\ntap KEY_18\n");
    rc = Remote({"play", script, "--save"}, out);
    Check(rc == 0, "remote: a script that toggles up and down again, no key there, plays");
    if (rc != 0)
        printf("%s\n", out.c_str());
    // the input's key up there (the upper G#, live, #243) saves AUX's variant now (#152): refused too
    Write(script, "booted\ntoggle 1\nwait 100\ntap KEY_24\nwait 100\ntap KEY_24\nwait 100\ntoggle 0\nwait 100\n");
    rc = Remote({"play", script, "--save"}, out);
    Check(rc != 0 && Has(out, "line 4: tap KEY_24 on the settings page"),
          "remote: and one that taps G# (live) there is refused: each source's is saved (#177)");
    // VOLUME's page 2 on the play page, the input gain, is saved too (#152); page 1 isn't
    Write(script, "booted\nturn 6 -2\nwait 100\ntap ENC_6_SW\nwait 100\nturn 6 2\nwait 100\n");
    rc = Remote({"play", script, "--save"}, out);
    Check(rc != 0 && Has(out, "line 6: turn 6 on VOLUME's page 2, the input gain"),
          "remote: VOLUME turned on its page 2 is refused, on page 1 not (#152)");
    Write(script, "booted\ndown KEY_26\ntap ENC_6_SW\nup KEY_26\nturn 6 2\nwait 100\n");
    rc = Remote({"play", script, "--save"}, out);
    Check(rc == 0, "remote: SHIFT + VOLUME's press resets the mix, no page: VOLUME's turn then plays");
    if (rc != 0)
        printf("%s\n", out.c_str());
    Write(script, "booted\nmidi F0 7D 43 48 13 00 05 F7\n");
    rc = Remote({"play", script, "--save"}, out);
    const std::string refused_sysex = out;
    Remote({"settings"}, out);
    Check(rc != 0 && Has(refused_sysex, "line 2: a setting over SysEx") && Has(out, "channel all,"),
          "remote: and a script that sets the channel over SysEx is refused, the channel kept");

    // the play page writes the card too (#78): the finalizer's knobs to frizz_master.txt,
    // SAVE / COPY / DELETE to frizz_scenes.txt. Refused without --force, nothing sent (with
    // --save, as from a FRIZZ before PERSIST)
    Write(script, "booted\ntap KEY_15\nwait 100\nturn 4 10\nwait 300\n");
    rc = Remote({"play", script, "--save"}, out);
    const std::string refused_comp = out;
    Remote({"state"}, out);
    Check(rc != 0 && Has(refused_comp, "line 4: turn 4 with the finalizer selected")
              && Has(refused_comp, "/FRIZZ/frizz_master.txt") && Has(refused_comp, "--force")
              && !Has(out, "knobs on   finalizer"),
          "remote: play refuses the finalizer's key then a knob, naming the master file, and sends none (#78)");
    if (rc == 0 || !Has(refused_comp, "frizz_master"))
        printf("%s\n", refused_comp.c_str());
    // the finalizer selected already: where the device stands counts, as the switch does
    Write(script, "booted\ntap KEY_15\nwait 200\n");
    const int selected = Remote({"play", script, "--save"}, out);
    Remote({"state"}, out);
    const bool comp_shown = Has(out, "knobs on   finalizer");
    Write(script, "booted\nturn 1 3\nwait 300\n");
    rc = Remote({"play", script, "--save"}, out);
    Check(selected == 0 && comp_shown && rc != 0 && Has(out, "line 2: turn 1 with the finalizer selected"),
          "remote: and a knob alone with the finalizer already selected on the device (#78)");
    Write(script, "booted\ntap KEY_5\nwait 100\nturn 4 2\nwait 300\n");
    rc = Remote({"play", script, "--save"}, out);
    Check(rc == 0, "remote: an effect selected first, its knob turns, played");
    if (rc != 0)
        printf("%s\n", out.c_str());
    Write(script, "booted\nmidi BF 18 40 BE 0E 40\n"); // CC 24 and channel 15's 14: the finalizer's knob 1
    rc = Remote({"play", script, "--save"}, out);
    Check(rc == 0, "remote: its knob over MIDI is played: CCs never reach the card (#97)");
    if (rc != 0)
        printf("%s\n", out.c_str());
    Write(script, "booted\ntap KEY_5\nwait 100\nmidi BF 17 0F\nturn 1 2\nwait 300\n");
    rc = Remote({"play", script, "--save"}, out);
    Check(rc != 0 && Has(out, "line 5: turn 1 with the finalizer selected"),
          "remote: a knob turned once CC 23 put the finalizer in focus is refused (#78)");
    Write(script, "booted\ntap KEY_23\nwait 100\ntap KEY_19\nwait 100\ntap KEY_26\nwait 500\n");
    rc = Remote({"play", script, "--save"}, out);
    const std::string refused_save = out;
    Remote({"state"}, out);
    Check(rc != 0 && Has(refused_save, "line 6: SAVE confirmed (tap KEY_26)")
              && Has(refused_save, "/FRIZZ/frizz_scenes.txt") && Has(out, "mode none"),
          "remote: play refuses a scene saved with SAVE, a slot and CHOMPI, naming the scenes file (#78)");
    if (rc == 0 || !Has(refused_save, "frizz_scenes"))
        printf("%s\n", refused_save.c_str());
    // the finalizer's presets (#345): key 15 held + SAVE starts their mode, the confirm writes
    // the master file; key 15 held + a scene key recalls one, which the master file keeps too
    Write(script, "booted\ndown KEY_15\nwait 100\ntap KEY_23\nwait 100\nup KEY_15\nwait 100\ntap KEY_20\n"
                  "wait 100\ntap KEY_26\nwait 500\n");
    rc = Remote({"play", script, "--save"}, out);
    Check(rc != 0 && Has(out, "line 10: SAVE confirmed (tap KEY_26)") && Has(out, "/FRIZZ/frizz_master.txt"),
          "remote: play refuses a finalizer preset saved (key 15 + SAVE, a preset, CHOMPI), naming the master file");
    if (rc == 0)
        printf("%s\n", out.c_str());
    Write(script, "booted\ndown KEY_15\nwait 100\ntap KEY_19\nwait 100\nup KEY_15\nwait 500\n");
    rc = Remote({"play", script, "--save"}, out);
    Check(rc != 0 && Has(out, "line 4: a finalizer preset recalled (tap KEY_19)") && Has(out, "/FRIZZ/frizz_master.txt"),
          "remote: and one recalled by hand (key 15 held + a scene key)");
    Write(script, "booted\ndown KEY_15\nwait 100\nup KEY_15\nwait 100\ntap KEY_23\nwait 100\ntap KEY_19\n"
                  "wait 100\ntap KEY_26\nwait 500\n");
    rc = Remote({"play", script, "--save"}, out);
    Check(rc != 0 && Has(out, "SAVE confirmed (tap KEY_26)") && Has(out, "/FRIZZ/frizz_scenes.txt"),
          "remote: SAVE after key 15 is let go is the scenes' again");

    // a script's set (twin/README.md) goes as the developers' SET, in RAM as the rest (#214);
    // one FRIZZ refuses, or that can't be sent, fails its line. The old guard (--save) refuses
    // a set FRIZZ would save: the finalizer's knobs, a setting, the input gain; not an effect's
    Write(script, "booted\nset fin 0 0.3\nset settings led_brightness 100\nwait 100\n");
    rc = Remote({"play", script}, out);
    const std::string set_played = out;
    Remote({"get", "fin"}, out);
    Check(rc == 0 && Has(out, "level   0.3000  0.0000"),
          "remote: play's set sets the finalizer's knob 1 to 0.3 (#214)");
    if (rc != 0 || !Has(out, "0.3000"))
        printf("%s\n%s\n", set_played.c_str(), out.c_str());
    Write(script, "booted\nset comp 0 0\nset comp 0 2\nset settings 99 1\nset nothing 0 0\nwait 100\n");
    rc = Remote({"play", script}, out);
    const std::string set_failed = out;
    Remote({"get", "comp"}, out);
    Check(rc == 1 && Has(set_failed, "line 3: set comp 0 2: a knob's value is 0-1")
              && Has(set_failed, "line 4: set settings 99 1: refused") && Has(set_failed, "line 5: set nothing 0 0: no group")
              && Has(out, "level   0.0000  0.0000"),
          "remote: a set out of range, refused or of no group fails its line; the others are set");
    if (rc != 1)
        printf("%s\n", set_failed.c_str());
    Write(script, "booted\nset filter 0 0.5\nset comp 1 0.5\nwait 100\n");
    rc = Remote({"play", script, "--save"}, out);
    Check(rc != 0 && Has(out, "line 3: set comp 1 0.5") && Has(out, "/FRIZZ/frizz_master.txt"),
          "remote: and with --save a set of the finalizer is refused, naming the master file; an effect's isn't");
    if (rc == 0)
        printf("%s\n", out.c_str());

    // a setter FRIZZ saves keeps the card out of it the same way, unless --save (#171): the
    // channel changes in RAM, the master file stays as it was; --save after that can't write
    {
        const std::string master = CardFiles()["/FRIZZ/frizz_master.txt"];
        rc = Remote({"channel", "7"}, out);
        const std::string kept = out;
        RunMs(2500);
        Remote({"settings"}, out);
        Check(rc == 0 && Has(kept, "kept in RAM; --save writes it") && Has(out, "channel 7,")
                  && CardFiles()["/FRIZZ/frizz_master.txt"] == master && !master.empty(),
              "remote: a setter keeps the change in RAM (PERSIST 0), the card's master file as it was (#171)");
        if (rc != 0)
            printf("%s\n", kept.c_str());
        rc = Remote({"channel", "0", "--save"}, out);
        Check(rc == 0 && Has(out, "--save writes nothing until it restarts"),
              "remote: --save then says it writes nothing until FRIZZ restarts");
    }

    // since PERSIST 0, play keeps the card as it was (#58, #78): the settings page's mono key,
    // a channel over SysEx, the finalizer's knob and a scene saved change FRIZZ, not its files
    const std::string master_file = CardFiles()["/FRIZZ/frizz_master.txt"];
    const std::string scenes_file = CardFiles()["/FRIZZ/frizz_scenes.txt"];
    Write(script, "booted\ntoggle 1\nwait 100\ndown KEY_26\ntap KEY_23\nup KEY_26\nwait 100\n"
                  "toggle 0\nwait 100\nmidi F0 7D 43 48 13 00 05 F7\ntap KEY_15\nwait 100\nturn 4 10\nwait 100\n"
                  "tap KEY_23\nwait 100\ntap KEY_20\nwait 100\ntap KEY_26\nwait 2500\n");
    rc = Remote({"play", script}, out);
    const std::string in_ram = out;
    Remote({"settings"}, out);
    const std::string settings = out;
    Remote({"state"}, out);
    Check(rc == 0 && Has(in_ram, "what the script changes stays in RAM") && Has(settings, "channel 5,")
              && Has(out, ", mono") && CardFiles()["/FRIZZ/frizz_master.txt"] == master_file
              && CardFiles()["/FRIZZ/frizz_scenes.txt"] == scenes_file && !master_file.empty(),
          "remote: play keeps what a script changes in RAM (PERSIST 0), the card's files as they were");
    if (rc != 0)
        printf("%s\n", in_ram.c_str());
    rc = Remote({"ident"}, out);
    Check(rc == 0 && Has(out, "the card: not written until it restarts (RAM only)"),
          "remote: and ident says so");

    // GET and SET by (group, index): an effect's knob, a setting, one refused
    rc = Remote({"set", "filter", "1", "0.25"}, out);
    rc |= Remote({"get", "filter"}, out);
    Check(rc == 0 && out.find("0.2500") == out.find("page 1  ") + 16, "remote: set an effect's knob, get reads it");
    if (rc != 0)
        printf("%s\n", out.c_str());
    // play's turn paced (#375): a detent every MS ms is a slow turn, 1% each; a plain turn
    // goes at once, one a block, a fast one. The slow one is judged by the gaps the firmware
    // measured (ui.h's Gap), the twin's time each TURN came in: on the twin's clock exactly MS
    // apart. On the wall clock (FRIZZ_TWIN_CLOCK=0) on a busy computer the pty's reader can
    // stall and take two detents late and together, a fast turn to the firmware whatever
    // remote.py sent: paced far from kFxAccelSlowMs, and a turn that still came in faster is
    // played again, said in a line FLAKY, as Twice() does (#183): it fails only if it came in
    // fast twice, or slow and the value is off
    const uint32_t kPaceMs = 400, kSlowMs = 125; // FxControls.h's kFxAccelSlowMs
    std::string turns;
    uint32_t shortest = 0;
    bool slow = false;
    usb_drop = SeeTurn;
    for (int attempt = 0; attempt < 2 && !slow; attempt++)
    {
        if (attempt)
            printf("FLAKY  remote: play's slow turn came in %u ms apart (%s), the twin's reader behind: "
                   "run again\n", shortest, turns.c_str());
        turns_at.clear();
        rc = Remote({"set", "filter", "0", "0.5"}, out);
        Write(script, "booted\ntap KEY_5\nwait 100\nturn 4 -5 " + std::to_string(kPaceMs) + "\nwait 2400\n");
        rc |= Remote({"play", script}, out);
        turns.clear();
        shortest = kPaceMs;
        for (size_t i = 0; i < turns_at.size(); i++)
        {
            turns += (i ? " " : "") + std::to_string(turns_at[i] - turns_at[0]);
            if (i)
                shortest = std::min(shortest, turns_at[i] - turns_at[i - 1]);
        }
        slow = turns_at.size() == 5 && shortest >= kSlowMs;
    }
    usb_drop = nullptr;
    rc |= Remote({"get", "filter"}, out);
    Check(rc == 0 && slow && out.find("0.4500") == out.find("page 1  ") + 8,
          "remote: play's turn ENC N MS turns slowly, 1% a detent");
    if (rc != 0 || !slow || out.find("0.4500") != out.find("page 1  ") + 8)
        printf("the detents came in at %s ms\n%s\n", turns.c_str(), out.c_str());
    // from 0.45 set, not the slow turn's: one SysEx of 5 detents, one a block, the first turned
    // back (slow), the others 0 ms apart
    rc = Remote({"set", "filter", "0", "0.45"}, out);
    Write(script, "booted\ntap KEY_5\nwait 100\nturn 4 5\nwait 300\n");
    rc |= Remote({"play", script}, out);
    rc |= Remote({"get", "filter"}, out);
    Check(rc == 0 && out.find("0.6600") == out.find("page 1  ") + 8,
          "remote: and its plain turn fast: 1%, then 5% a detent");
    if (rc != 0 || out.find("0.6600") != out.find("page 1  ") + 8)
        printf("%s\n", out.c_str());
    rc = Remote({"set", "settings", "clock_factor", "50"}, out);
    rc |= Remote({"get", "settings"}, out);
    Check(rc == 0 && Has(out, "clock_factor    50\n") && Has(out, "midi_channel    5\n"),
          "remote: set the clock factor, get reads every setting");
    // the settings' 19 values come in an answer of 49 bytes, two USB transfers (#196)
    Check(rc == 0 && Has(out, "usb_live        1\n") && Has(out, "usb_audio       1\n") && !Has(out, "?"),
          "remote: and the last ones, USB's, from an answer longer than a USB transfer");
    rc = Remote({"set", "settings", "led_brightness", "20"}, out);
    Check(rc != 0 && Has(out, "refused"), "remote: a value a setting doesn't take is refused");
    rc = Remote({"set", "volume", "mix", "1"}, out);
    rc |= Remote({"get", "volume"}, out);
    Check(rc == 0 && Has(out, "mix 1.0000"), "remote: VOLUME's by name");
    rc = Remote({"bench"}, out);
    Check(rc != 0 && Has(out, "not in this build"), "remote: bench, on FRIZZ: not in this build");
    rc = Remote({"load"}, out);
    Check(rc == 0 && Has(out, "over 100 %: 0 blocks"), "remote: load counts the blocks over 100 %");

    // a FRIZZ from before PERSIST (no answer): the old guard, refusing. The page the device
    // shows doesn't count (play puts the switch down, #253): the script's toggle does
    rc = Python(R"(
import sys, remote
class Old:
    def ram_only(self):
        return None
    def state(self):
        return bytes([0, 0, 0, 0, 0, 0, 64, 0]) + bytes(20)
remote.Frizz = lambda device: Old()
open("s.txt", "w").write("booted\ntoggle 1\ntap KEY_18\n")
sys.argv = ["remote.py", "--device", "x", "play", "s.txt"]
try:
    remote.main()
except SystemExit as e:
    print("exit:", e)
)", out);
    Check(Has(out, "a FRIZZ from before PERSIST") && Has(out, "exit: line 3: tap KEY_18 on the settings page"),
          "remote: a FRIZZ from before PERSIST is guarded as before (#58)");
    if (!Has(out, "exit: line 2"))
        printf("%s\n", out.c_str());

    // which slot runs, by IDENT and the builds flash.py noted: one started by hand is found,
    // the bench never taken for FRIZZ, another slot asked for started; one without IDENT goes
    // by the note as before. flash.py fails when the build running isn't the one sent
    rc = Python(R"(
import sys, chompi, flash, midi_send
open("a.bin", "wb").write(b"build a")
open("b.bin", "wb").write(b"build b")
chompi.noted_image(12, "a.bin", "aaaa")
chompi.noted_image(10, "b.bin", "bbbb")
chompi.forget()
log = []
chompi.state = lambda device=None: ("frizz", "node")
chompi.start = lambda slot, *rest: log.append("start %d" % slot) or "started"
running = {}
def ident(node):
    return running.get("info")
chompi.ident = ident
def case(name, info, slot=None, again=False):
    running["info"] = info
    log.clear()
    try:
        got = chompi.to_frizz(slot, again=again)
    except SystemExit as e:
        got = "exit: %s" % e
    print("%s: %s %s %s" % (name, got, chompi.frizz_slot, log), flush=True)
a = dict(kind="frizz", crc=chompi.image_id("a.bin")[0], length=7)
case("by hand", a)
case("asked", a, 12)
case("other", a, 10)
case("bench", dict(a, kind="bench"))
case("unknown", dict(a, crc=1))
case("old", None)
# remote.py play --fresh: the one running started again, the one asked for once (#123)
case("fresh", a, None, True)
case("fresh asked", a, 12, True)
case("fresh other", a, 10, True)
case("fresh unknown", dict(a, crc=1), None, True)
case("fresh bench", dict(a, kind="bench"), None, True)
chompi.started(12)
case("old noted", None)
sys.argv = ["flash.py", "a.bin", "--slot", "12"]
chompi.to_launcher = lambda *rest: "l"
chompi.slot_file = lambda *rest: None  # key 12 holds FRIZZ-TEST
chompi.features = lambda *rest: 0
chompi.wait_for = lambda *rest: "node"
chompi.renamed = lambda slot, node, *rest: node
midi_send.main = lambda: None
running["info"] = dict(a, crc=5)
try:
    flash.main()
except SystemExit as e:
    print("exit:", e)
running["info"] = a
flash.main()
)", out);
    Check(rc == 0 && Has(out, "by hand: node 12 []\n") && Has(out, "asked: node 12 []\n")
              && Has(out, "other: started 10 ['start 10']\n") && Has(out, "FRIZZ on slot 12: FRIZZ, crc32 ")
              && Has(out, "slot 12's: md5 aaaa, a.bin at "),
          "tools: IDENT finds the slot a build went to, started by hand too; another asked for is started");
    Check(Has(out, "bench: started 10 ['start 10']\n") && Has(out, "the bench runs:")
              && Has(out, "unknown: node None []\n") && Has(out, "not a build the tools sent")
              && Has(out, "old: node None []\n") && Has(out, "old noted: node 12 []\n"),
          "tools: the bench isn't FRIZZ; a build the tools didn't send, or one without IDENT, as before");
    Check(Has(out, "fresh: started 12 ['start 12']\n") && Has(out, "fresh asked: started 12 ['start 12']\n")
              && Has(out, "fresh other: started 10 ['start 10']\n") && Has(out, "starting slot 12 again"),
          "tools: again (play --fresh) starts the FRIZZ running again, through the launcher (#123)");
    Check(Has(out, "fresh unknown: exit: FRIZZ runs on a slot the tools can't tell") && Has(out, "fresh bench: started 10 ['start 10']\n"),
          "tools: again, a FRIZZ on a slot it can't tell is refused, not guessed; the bench is replaced as before");
    Check(Has(out, "exit: it runs crc32 00000005, 7 B, NOT the build sent")
              && Has(out, "it runs the build sent: crc32 "),
          "tools: flash.py checks the build running is the one sent");
    if (rc != 0 || !Has(out, "it runs the build sent"))
        printf("%s\n", out.c_str());

    // the bench followed over USB (remote.py bench --wait): a segment over 100 % starves
    // MainLoop, whose silence is waited out; cpu.txt then fetched in chunks. And a FRIZZ that
    // doesn't answer for a while isn't taken for another firmware: it's restarted once it does
    rc = Python(R"(
import sys, time, chompi, remote
text = b"FRIZZ cpu bench, source 1\n" + b"x" * 70 + b"\n"
states = [bytes([1, 0, 3, 25, 0, 0, 0, 0, 0]), None, None,
          bytes([0, 0, 25, 25, 1, 1, 0, len(text) >> 7, len(text) & 0x7F])]
class F:
    def dev(self, sub, args=()):
        if args[0] == 0:
            got = states.pop(0) if len(states) > 1 else states[0]
            return got and (0, got)
        at = args[1] << 14 | args[2] << 7 | args[3]
        return 0, text[at:at + 32]
time.sleep = lambda s: None
remote.bench(F(), True, "card/cpu.txt")
print("same" if open("card/cpu.txt", "rb").read() == text else "differs")
log = []
seq = [("other", "x"), ("other", "x"), ("frizz", "node")]
chompi.state = lambda device=None: seq.pop(0) if len(seq) > 1 else seq[0]
chompi.restart = lambda node: log.append("restart " + node)
chompi.reached = lambda *rest: True
chompi.wait_for = lambda wanted, *rest, **kw: log.append("wait " + wanted) or "l"
print(chompi.to_launcher(), log)
)", out);
    Check(rc == 0 && Has(out, "segment 4 of 25\n") && Has(out, "segment 4 of 25\n  no answer: over 100 %, MainLoop starved; waiting\n")
              && Has(out, "done; cpu.txt on the card, 97 B") && Has(out, "same\n"),
          "tools: remote.py bench --wait waits out a starved MainLoop, then fetches cpu.txt as sent");
    Check(Has(out, "l ['restart node', 'wait launcher']"),
          "tools: a FRIZZ silent for a while is restarted once it answers, not taken for another firmware");
    if (rc != 0 || !Has(out, "same") || !Has(out, "waiting\n"))
        printf("%s\n", out.c_str());

    // the rescue (#105): a FRIZZ that took the restart but doesn't get to the launcher, or
    // one silent for BUSY s by its names, is RESET from its audio callback before the power
    // switch is asked for; another firmware never is. FRIZZ is told by its USB names
    rc = Python(R"(
import os, time, chompi
time.sleep = lambda s: None
chompi.BUSY = 0
log = []
def case(name, states, names, reaches=False):
    seq = list(states)
    chompi.state = lambda device=None: seq.pop(0) if len(seq) > 1 else seq[0]
    chompi.restart = lambda node, hard=False: log.append(("RESET " if hard else "restart ") + node)
    chompi.frizz_node = lambda node: names
    chompi.reached = lambda wanted, *rest: log.append("reached? " + wanted) or reaches
    chompi.wait_for = lambda wanted, timeout, device=None, hint=None, **kw: log.append("wait %s: %s" % (wanted, hint)) or "l"
    log.clear()
    chompi.to_launcher()
    print("%s: %s" % (name, log), flush=True)
case("restarts", [("frizz", "node")], True, True)
case("restart hangs", [("frizz", "node")], True)
case("silent", [("other", "node")], True)
case("another", [("other", "node")], False)
os.makedirs("asound", exist_ok=True)
open("asound/cards", "w").write(" 3 [FrizZ          ]: USB-Audio - FrizZ\n                      Electrosmith FrizZ at usb-0000:00:14.0-2, full speed\n"
                                " 4 [Seed           ]: USB-Audio - Daisy Seed Built In\n                      Electrosmith Daisy Seed Built In at usb-0000:00:14.0-3\n")
chompi.ASOUND = "asound"
import importlib
frizz_node = importlib.reload(chompi).frizz_node
chompi.ASOUND = "asound"
print("names:", frizz_node("/dev/snd/midiC3D0"), frizz_node("/dev/snd/midiC4D0"), frizz_node(None))
)", out);
    Check(rc == 0 && Has(out, "restarts: ['restart node', 'reached? launcher', 'wait launcher: no launcher yet")
              && Has(out, "restart hangs: ['restart node', 'reached? launcher', 'RESET node', 'wait launcher: no launcher yet")
              && Has(out, "FRIZZ hasn't restarted: resetting it from its audio callback (RESET)"),
          "tools: a FRIZZ that doesn't get to the launcher after its restart is RESET from its audio callback (#105)");
    Check(Has(out, "silent: ['RESET node', \"wait launcher: still no launcher: FRIZZ's audio callback hangs too")
              && Has(out, "another: ['wait launcher: None']") && Has(out, "names: True False False"),
          "tools: a silent FRIZZ, by its USB names, is RESET before the power switch is asked for; another firmware isn't");
    if (rc != 0 || !Has(out, "names: True False"))
        printf("%s\n", out.c_str());

    // another firmware on FRIZZ's platform (#368): by its USB names (Chompi Club) it's asked to
    // restart into the launcher before the power switch is; IDENT's kinds past the bench are
    // another firmware's, never taken for FRIZZ; a card it lends isn't the storage firmware's,
    // and nothing restarts it meanwhile
    rc = Python(R"(
import os, sys, time, chompi, midi_send
time.sleep = lambda s: None
chompi.BUSY = 0
log = []
def case(name, names, reaches):
    chompi.state = lambda device=None: ("other", "node")
    chompi.restart = lambda node, hard=False: log.append(("RESET " if hard else "restart ") + node)
    chompi.frizz_node = lambda node: False
    chompi.platform_node = lambda node: names
    chompi.reached = lambda wanted, *rest: log.append("reached? " + wanted) or reaches
    chompi.wait_for = lambda wanted, timeout, device=None, hint=None, **kw: log.append("wait %s: %s" % (wanted, hint)) or "l"
    log.clear()
    chompi.to_launcher()
    print("%s: %s" % (name, log), flush=True)
case("platform", True, True)
case("platform, no restart", True, False)
case("stock", False, True)
os.makedirs("asound", exist_ok=True)
open("asound/cards", "w").write(" 3 [OTHERFW        ]: USB-Audio - OTHERFW\n                      Chompi Club OTHERFW at usb-0000:00:14.0-2, full speed\n"
                                " 4 [Seed           ]: USB-Audio - Daisy Seed Built In\n                      Electrosmith Daisy Seed Built In at usb-0000:00:14.0-3\n")
import importlib
chompi = importlib.reload(chompi)
chompi.ASOUND = "asound"
print("names:", chompi.platform_node("/dev/snd/midiC3D0"), chompi.platform_node("/dev/snd/midiC4D0"),
      chompi.platform_node(None), chompi.frizz_node("/dev/snd/midiC3D0"), flush=True)
# garbled names (#88, #371): one string off, the maker the product's name, found by the Daisy's
# USB id; a Daisy with its own names isn't the CHOMPI's platform
with open("asound/cards", "a") as cards:
    cards.write(" 5 [Default        ]: USB-Audio - \u0409\n                      OTHERFW \u0409 at usb-0000:00:14.0-2, full speed\n"
                " 6 [Default_1      ]: USB-Audio - \u0409\n                      FrizZ \u0409 at usb-0000:00:14.0-2, full speed\n")
for card in (4, 5, 6):
    os.makedirs("asound/card%d" % card, exist_ok=True)
    open("asound/card%d/usbid" % card, "w").write("0483:5740\n")
for what, card in (("garbled", 5), ("garbled FRIZZ", 6), ("Daisy", 4)):
    node = "/dev/snd/midiC%dD0" % card
    print("%s: %s %s %s" % (what, chompi.platform_node(node), chompi.frizz_node(node), chompi.other_firmware(node)), flush=True)
import shutil
for card in (4, 5, 6):
    shutil.rmtree("asound/card%d" % card)
# IDENT's kinds
for kind in (0, 1, 2, 9):
    info = chompi.parse_ident(bytes([1, kind]) + bytes(24))
    print("kind %d: %s / %s" % (kind, info["kind"], chompi.describe_ident(info, [])), flush=True)
chompi.state = lambda device=None: ("frizz", "node")
chompi.ident = lambda node: chompi.parse_ident(bytes([1, 2]) + bytes(24))
chompi.last_started = lambda: (12, True, "this hold")
chompi.start = lambda slot, *rest: print("start", slot, flush=True) or "n"
print("to_frizz:", chompi.to_frizz(), flush=True)
# the card: the storage firmware's, or lent by a firmware
chompi.card_partition = lambda: "/dev/sdx1"
for vendor, midi in (("CHOMPI", None), (None, None), ("FrizZ", "/dev/snd/midiC3D0"), ("FrizZ", None),
                     ("OTHERFW", "/dev/snd/midiC3D0"), ("OTHERFW", None)):
    chompi.drive_vendor = lambda part: vendor
    midi_send.find_device = lambda: midi
    chompi.by_usb_id = lambda: (None, None)
    print("drive %s, MIDI %s: storage %s, lent %s, FRIZZ's %s" % (vendor, "yes" if midi else "no",
          chompi.storage_partition(), chompi.lent_partition(), chompi.lent_by_frizz("/dev/sdx1")), flush=True)
chompi.drive_vendor = lambda part: "OTHERFW"
midi_send.find_device = lambda: "/dev/snd/midiC3D0"
try:
    chompi.restart("node")
    print("restarted while another firmware lends it", flush=True)
except SystemExit as e:
    print("exit:", e, flush=True)
)", out);
    Check(rc == 0 && Has(out, "platform: ['restart node', 'reached? launcher', 'wait launcher: None']\n")
              && Has(out, "another firmware on the CHOMPI: asking it to restart into the launcher")
              && Has(out, "platform, no restart: ['restart node', 'reached? launcher', 'RESET node', 'wait launcher: still no launcher: switch the CHOMPI off and on']\n")
              && Has(out, "it didn't restart: resetting it from its audio callback (RESET)")
              && Has(out, "stock: ['wait launcher: None']\n") && Has(out, "names: True False False False\n"),
          "tools: another firmware on FRIZZ's platform, by its USB names, is asked to restart into the launcher first (#368), "
          "RESET when it doesn't (#371)");
    Check(Has(out, "garbled: True False True\n") && Has(out, "garbled FRIZZ: True True False\n")
              && Has(out, "Daisy: False False False\n"),
          "tools: another firmware with garbled USB names is found by the Daisy's USB id as FRIZZ is, not a Daisy that says so (#371)");
    Check(Has(out, "kind 0: frizz / FRIZZ, crc32") && Has(out, "kind 1: bench / the bench, crc32")
              && Has(out, "kind 2: other 2 / another firmware (IDENT kind 2), crc32")
              && Has(out, "kind 9: other 9 / another firmware (IDENT kind 9), crc32")
              && Has(out, "not FRIZZ: another firmware (IDENT kind 2)") && Has(out, "start 12\nto_frizz: n\n"),
          "tools: IDENT's kinds past the bench are another firmware's, never taken for FRIZZ: its slot is started (#368)");
    Check(Has(out, "drive CHOMPI, MIDI no: storage /dev/sdx1, lent None, FRIZZ's False\n")
              && Has(out, "drive None, MIDI no: storage /dev/sdx1, lent None, FRIZZ's False\n")
              && Has(out, "drive FrizZ, MIDI yes: storage None, lent /dev/sdx1, FRIZZ's True\n")
              && Has(out, "drive FrizZ, MIDI no: storage None, lent /dev/sdx1, FRIZZ's True\n")
              && Has(out, "drive OTHERFW, MIDI yes: storage None, lent /dev/sdx1, FRIZZ's False\n")
              && Has(out, "drive OTHERFW, MIDI no: storage /dev/sdx1, lent None, FRIZZ's False\n")
              && Has(out, "exit: the firmware running lends the card to the computer (CHOMPI-SD at /dev/sdx1): not restarted"),
          "tools: a card another firmware lends (its vendor, or MIDI beside the drive) isn't the storage firmware's, and isn't restarted from under (#368)");
    if (rc != 0 || !Has(out, "to_frizz: n"))
        printf("%s\n", out.c_str());

    // #371: another firmware by its USB names never answers as FRIZZ, so it isn't waited for:
    // neither BUSY s before its restart (to_launcher) nor SETTLE s before its slot is started
    // (to_frizz); FRIZZ silent by its names still is. A clock the sleeps move on
    rc = Python(R"(
import time, chompi
clock = [0.]
time.monotonic = lambda: clock[0]
time.sleep = lambda s: clock.__setitem__(0, clock[0] + s)
chompi.BUSY, chompi.SETTLE = 20, 10
chompi.state = lambda device=None: ("other", "node")
chompi.reached = lambda wanted, *rest: True
chompi.wait_for = lambda *a, **kw: "l"
who = [""]
chompi.restart = lambda node, hard=False: print("%s: %s at %g s" % (who[0], "RESET" if hard else "restart", clock[0]), flush=True)
chompi.last_started = lambda: (None, False, "")
chompi.start = lambda slot, *rest: print("%s: start %d at %g s" % (who[0], slot, clock[0]), flush=True) or "n"
for name, frizz in (("another", False), ("FRIZZ", True)):
    chompi.platform_node = lambda node: True
    chompi.frizz_node = lambda node, frizz=frizz: frizz
    clock[0] = 0.
    who[0] = name + " to_launcher"
    chompi.to_launcher()
    clock[0] = 0.
    who[0] = name + " to_frizz"
    chompi.to_frizz()
)", out);
    Check(rc == 0 && Has(out, "another to_launcher: restart at 0 s\n") && Has(out, "another to_frizz: start 10 at 0 s\n"),
          "tools: another firmware on FRIZZ's platform is restarted at once, not after BUSY or SETTLE s (#371)");
    Check(Has(out, "FRIZZ to_launcher: RESET at 20 s\n") && Has(out, "FRIZZ to_frizz: start 10 at 10 s\n"),
          "tools: a FRIZZ silent by its names is still waited for (BUSY, SETTLE)");
    if (rc != 0 || !Has(out, "restart at 0 s"))
        printf("%s\n", out.c_str());

    // after a script played RAM only, a FRIZZ slot other than the test slot is started again
    // (from the card, so it saves again); the test slot isn't; one it can't tell, it says.
    // Through run(), which gets the node from to_frizz (#134). A failed expect led says --fresh
    rc = Python(R"(
import argparse, sys, chompi, remote
sys.stdout.reconfigure(line_buffering=True)
sys.stderr = sys.stdout
chompi.start = lambda slot, *rest: print("start", slot)
running = [None]
def to_frizz(slot=None, timeout=120, device=None, again=False):
    chompi.frizz_slot = running[0]
    return "node"
chompi.to_frizz = to_frizz
persist = [True]  # PERSIST 0 answered (None: a FRIZZ from before it)
kept = [True]  # IDENT's RAM only flag: False once FRIZZ restarted
refuse = [False]  # SET refused
class F:
    class link:
        fd = None
    def __init__(self, device):
        pass
    def ram_only(self):
        return persist[0]
    def dev(self, sub, args=()):
        if sub == chompi.SET and refuse[0]:
            return 1, b""
        return (0, bytes(25) + bytes([1 if kept[0] else 0])) if persist[0] else None
    def send(self, cmd, payload=b""):
        print("sent 0x%02X" % cmd)
remote.Frizz = F
remote.os.close = lambda fd: None
def case(slot, failed, **given):
    running[0] = chompi.frizz_slot = slot
    remote.play = lambda f, path, cpu: failed
    a = argparse.Namespace(cmd="play", script="x.txt", cpu=False, fresh=False, save=False,
                           force=False, slot=None, device=None, no_start=False)
    vars(a).update(given)
    print("slot", slot)
    try:
        remote.run(a)
    except SystemExit as e:
        print("exit", e.code)
case(10, 0)
case(12, 0)
case(None, 0)
case(12, 2)
case(10, 0, device="/dev/x")
case(12, 2, fresh=True)
# the setters FRIZZ saves (#171): PERSIST 0 first, key 10's started again afterwards, the
# change gone; the test slot's kept; a knob isn't saved, no PERSIST; --save as before
print("-- setters")
case(10, 0, cmd="channel", channel=7)
case(12, 0, cmd="transport", on="on")
case(10, 0, cmd="set", group="filter", index="1", value="0.5")
case(12, 0, cmd="set", group="volume", index="in", value="0.5")
case(10, 0, cmd="channel", channel=7, save=True)
persist[0] = None
case(12, 0, cmd="out", ports="trs")
case(12, 0, cmd="out", ports="trs", save=True)
persist[0] = True
# however a setter ends, key 10's FRIZZ is started again: a value refused, no file
print("-- refused")
refuse[0] = True
case(10, 0, cmd="set", group="settings", index="led_brightness", value="60")
refuse[0] = False
case(10, 0, cmd="scene", action="put", scene_slot=2, file=None)
print("-- lost")
# FRIZZ restarted meanwhile (IDENT: not RAM only): said, failed, before it's started again
kept[0] = False
case(12, 0)
case(10, 0, cmd="channel", channel=7)
kept[0] = True
)", out);
    Check(rc == 0 && Has(out, "slot 10\nthe card isn't written until FRIZZ restarts (PERSIST 0): what the script changes "
                             "stays in RAM\nstarting slot 10 again, from the card, so it saves again\nstart 10\nexit 0\n")
              && Has(out, "RAM\nexit 0\nslot None\n") && Has(out, "FRIZZ keeps what it changes in RAM until it restarts"),
          "tools: after play RAM only, key 10's FRIZZ is started again from the card; the test slot isn't (#134)");
    Check(Has(out, "RAM\nexpect led judged after what played since FRIZZ started: --fresh plays the script from "
                   "power-on (#123)\nexit 1\n")
              && Has(out, "slot 10\nthe card isn't written until FRIZZ restarts (PERSIST 0): what the script changes "
                          "stays in RAM\nFRIZZ on slot 10 keeps what it changes in RAM")
              && Has(out, "slot 12\nthe card isn't written until FRIZZ restarts (PERSIST 0): what the script changes "
                          "stays in RAM\nexit 1\n"),
          "tools: a failed expect led says --fresh, not after it; with --device, key 10's FRIZZ isn't started, but said");
    {
        const std::string setters = out.substr(std::min(out.size(), out.find("-- setters")));
        const std::string lost = out.substr(std::min(out.size(), out.find("-- lost")));
        const std::string refused = out.substr(std::min(out.size(), out.find("-- refused")));
        Check(Has(refused, "slot 10\nthe card isn't written until FRIZZ restarts (PERSIST 0): kept in RAM; --save writes "
                           "it to the card\nstarting slot 10 again, from the card, so it saves again: what was set is "
                           "gone (--save keeps it)\nstart 10\nexit SET settings led_brightness 60: refused\n")
                  && Has(refused, "is gone (--save keeps it)\nstart 10\nexit scene put SLOT FILE\n"),
              "tools: however a setter ends (a value refused, no file), key 10's FRIZZ is started again from the card (#171)");
        Check(Has(setters, "slot 10\nthe card isn't written until FRIZZ restarts (PERSIST 0): kept in RAM; --save writes "
                           "it to the card\nsent 0x13\nstarting slot 10 again, from the card, so it saves again: what "
                           "was set is gone (--save keeps it)\nstart 10\nslot 12\nthe card isn't written until FRIZZ "
                           "restarts (PERSIST 0): kept in RAM; --save writes it to the card\nsent 0x13\nslot 10\n"),
              "tools: a setter FRIZZ saves keeps it in RAM (PERSIST 0), key 10's started again after, the test slot's not (#171)");
        Check(Has(setters, "slot 12\nthe card isn't written until FRIZZ restarts (PERSIST 0): kept in RAM; --save "
                           "writes it to the card\nslot 10\nFRIZZ keeps the card out of it already (PERSIST 0): --save "
                           "writes nothing until it restarts (./flash.py --run N)\nsent 0x13\nslot 12\nexit a FRIZZ "
                           "from before PERSIST")
                  && Has(setters, "--save sends it anyway\nslot 12\nsent 0x13\n"),
              "tools: VOLUME's input gain too, an effect's knob not, --save as before; a FRIZZ from before PERSIST "
              "refused without --save, nothing sent");
        Check(Has(lost, "slot 12\nthe card isn't written until FRIZZ restarts (PERSIST 0): what the script changes "
                        "stays in RAM\nexit FRIZZ restarted (up 0.0 s, reset: none): it writes the card again: what "
                        "the script changed after that may be on the card\n")
                  && Has(lost, "starting slot 10 again, from the card, so it saves again: what was set is gone (--save "
                               "keeps it)\nstart 10\nexit FRIZZ restarted"),
              "tools: a FRIZZ that restarted during play or a setter (IDENT: no longer RAM only) fails, saying so (#171)");
    }
    if (rc != 0 || !Has(out, "start 10") || failures)
        printf("%s\n", out.c_str());

    // play --fresh: the slot started again first, then the switch down (a restart drops what
    // SysEx held, #253), the hand's again at the end
    rc = Python(R"(
import argparse, sys, chompi, remote
sys.stdout.reconfigure(line_buffering=True)
sys.stderr = sys.stdout
def to_frizz(slot=None, timeout=120, device=None, again=False):
    chompi.frizz_slot = 12
    print("to_frizz again", again)
    return "node"
chompi.to_frizz = to_frizz
class F:
    class link:
        fd = None
    def __init__(self, device):
        pass
    def ram_only(self):
        return True
    def dev(self, sub, args=()):
        return 0, bytes(25) + bytes([1])
    def send(self, cmd, payload=b""):
        print("sent 0x%02X %s" % (cmd, list(payload)))
    def release(self):
        print("released")
remote.Frizz = F
remote.os.close = lambda fd: None
open("x.txt", "w").write("booted\nwait 10\n")
remote.run(argparse.Namespace(cmd="play", script="x.txt", cpu=False, fresh=True, save=False,
                              force=False, slot=None, device=None, no_start=False))
)", out);
    Check(rc == 0 && Has(out, "to_frizz again True\n") && Has(out, "sent 0x14 [1]\nreleased\nsent 0x14 [0]\n")
              && out.find("to_frizz again True") < out.find("sent 0x14 [1]"),
          "tools: play --fresh starts the slot again first, then puts the switch down; the hand's at the end (#253)");
    if (rc != 0 || !Has(out, "sent 0x14 [1]"))
        printf("%s\n", out.c_str());

    // keys 10 and 11 are the user's (#171): flash.py sends there only when the key is named, and
    // only the build of a clean v* tag's checkout; refused before anything is built or the
    // CHOMPI restarted (to_launcher). A bare ./flash.py sends nothing. Each key gets its colour
    // (a launcher 1.6): 10 green, 11 orange, 12 and 13 red
    rc = Python(R"(
import os, subprocess, sys, builds, chompi, flash, midi_send
sys.stdout.reconfigure(line_buffering=True)
sys.stderr = sys.stdout
def git(*args):
    subprocess.run(["git", "-C", "repo", "-c", "user.name=t", "-c", "user.email=t@t", "-c",
                    "commit.gpgsign=false", "-c", "tag.gpgsign=false"] + list(args), check=True,
                   capture_output=True)
os.makedirs("repo/code")
open("repo/code/a.h", "w").write("1\n")
open("repo/notes.md", "w").write("1\n")
git("init", "-q")
git("add", ".")
git("commit", "-qm", "one")
git("tag", "beta")
print("tagged beta:", flash.tagged("repo"))
git("tag", "v9.9.9")
print("tagged v9.9.9:", flash.tagged("repo"))
open("repo/code/a.h", "w").write("2\n")
print("tagged, code changed:", flash.tagged("repo"))
git("checkout", "code/a.h")
open("repo/code/b.h", "w").write("new\n")
print("tagged, a file added in code:", flash.tagged("repo"))
os.remove("repo/code/b.h")
open("repo/notes.md", "w").write("2\n")
print("tagged, outside code changed:", flash.tagged("repo"))
git("commit", "-qam", "two")
print("a commit after:", flash.tagged("repo"))

log = []
chompi.to_launcher = lambda *rest: log.append("launcher") or "l"
chompi.slot_file = lambda *rest: None
chompi.wait_for = lambda *rest: "node"
chompi.renamed = lambda slot, node, *rest: node
chompi.ident = lambda node: None
chompi.features = lambda *rest: midi_send.FEATURE_COLOR
chompi.send_run = lambda node, slot: log.append("run %d" % slot)
midi_send.main = lambda: log.append(" ".join(sys.argv[1:]))
open("tag.bin", "wb").write(b"the tag's build")
open("other.bin", "wb").write(b"another")
builds.build = lambda bench: log.append("build") or "tag.bin"
builds.built = lambda bench: "tag.bin"
builds.image = lambda bench: "tag.bin"
builds.stale = lambda path: False
builds.describe = lambda path: path
builds.frizz_version = lambda: (1, 0, 0, 0)
tag = [None]
flash.tagged = lambda where=None: tag[0]
def case(name, *argv):
    log.clear()
    sys.argv = ["flash.py"] + list(argv)
    try:
        flash.main()
    except SystemExit as e:
        print("exit:", e)
    print("%s: %s" % (name, log))
case("bare")
case("10 untagged", "--slot", "10")
case("11 untagged", "--no-build", "--slot", "11")
case("file to 10 untagged", "other.bin", "--slot", "10")
tag[0] = "v1.0.0"
case("11 tagged", "--slot", "11")
case("10 tagged", "--no-build", "--slot", "10")
case("its file to 10", "tag.bin", "--slot", "10")
case("another file to 10", "other.bin", "--slot", "10")
case("the bench to 11", "--bench", "--slot", "11")
builds.frizz_version = lambda: (1, 5, 0, 1)
case("10 tagged, another version", "--slot", "10")
tag[0] = "v1.5.0-beta.1"
case("11 its beta", "--no-build", "--slot", "11")
tag[0] = "v1.5.0"
case("10 not its release", "--no-build", "--slot", "10")
print("versions:", builds.tag_version("v1.5.0"), builds.tag_version("v1.5.0-beta.12"), builds.tag_version("v1.0"),
      builds.version_name((1, 5, 0, 1)))
print("beta 0:", builds.tag_version("v1.5.0-beta.0"), builds.tag_version("v1.5.0-beta.01"))
tag[0] = "v1.5.0-beta.0"
builds.frizz_version = lambda: (1, 5, 0, 0)
case("11 beta.0", "--no-build", "--slot", "11")
# release.sh (release.yml's last step) takes the same tags: a gh on the PATH that only says so
os.makedirs("bin", exist_ok=True)
open("bin/gh", "w").write("#!/bin/sh\necho gh called >&2\nexit 1\n")
os.chmod("bin/gh", 0o755)
release = os.path.join(os.path.dirname(builds.FIRMWARE), ".github", "scripts", "release.sh")
for t in ("v1.5.0-beta.0", "v1.5.0-beta.01", "v1.5.0-beta.1", "v1.5.0"):
    r = subprocess.run(["bash", release, "/dev/null"], capture_output=True, text=True,
                       env=dict(os.environ, TAG=t, MD5="d41d8cd98f00b204e9800998ecf8427e", GH_REPO="x/y",
                                PATH=os.path.abspath("bin") + os.pathsep + os.environ["PATH"]))
    print("release.sh %s: %s" % (t, "refused" if "isn't a version's tag" in r.stdout + r.stderr
                                 else "gh called" if "gh called" in r.stderr else "?"))
tag[0] = None
case("test", "--test")
case("file to 12", "other.bin", "--slot", "12")
case("file to 13", "other.bin", "--slot", "13")
case("bench", "--bench", "--no-build")
case("13 without --bench", "--no-build", "--slot", "13")
case("the bench to 12", "--bench", "--no-build", "--slot", "12")
case("--test --bench", "--test", "--bench", "--no-build")
case("--test to 13", "--test", "--slot", "13")
)", out);
    Check(rc == 0 && Has(out, "tagged beta: None\n") && Has(out, "tagged v9.9.9: v9.9.9\n")
              && Has(out, "tagged, code changed: None\n") && Has(out, "tagged, a file added in code: None\n")
              && Has(out, "tagged, outside code changed: v9.9.9\n") && Has(out, "a commit after: None\n"),
          "tools: flash.py's tagged(): a v* tag on HEAD, nothing changed or added in code/ (#171)");
    Check(Has(out, "exit: which slot?") && Has(out, "bare: []\n")
              && Has(out, "exit: key 10 is the latest release, the user's: only a release's or a beta's build")
              && Has(out, "10 untagged: []\n") && Has(out, "exit: key 11 is the latest beta, the user's")
              && Has(out, "11 untagged: []\n") && Has(out, "file to 10 untagged: []\n"),
          "tools: flash.py sends nothing without a slot, nor an untagged build to 10 or 11, before restarting "
          "the CHOMPI (#171)");
    Check(Has(out, "sending tag.bin to key 11, v1.0.0's\n")
              && Has(out, "11 tagged: ['build', 'launcher', 'tag.bin --slot 11 --name FRIZZ-BETA --device l --stay', "
                          "'--color 11 FF7000 --device l', 'run 11']\n")
              && Has(out, "10 tagged: ['launcher', 'tag.bin --slot 10 --name FRIZZ --device l --stay', "
                          "'--color 10 00C000 --device l', 'run 10']\n")
              && Has(out, "its file to 10: ['launcher', 'tag.bin --slot 10 --name FRIZZ --device l --stay', "
                          "'--color 10 00C000 --device l', 'run 10']\n"),
          "tools: a v* tag's build goes to 11 (FRIZZ-BETA, orange) or 10 (green) when the key is named, its file too");
    Check(Has(out, "exit: other.bin isn't the build of v1.0.0 made here") && Has(out, "another file to 10: []\n")
              && Has(out, "exit: key 11 is the latest beta, the user's: never the bench")
              && Has(out, "the bench to 11: []\n"),
          "tools: another file isn't, nor the bench, on a tag too");
    Check(Has(out, "exit: code/src/Version.h's kFrizzVersion is v1.5.0-beta.1, the tag v1.0.0")
              && Has(out, "10 tagged, another version: []\n")
              && Has(out, "11 its beta: ['launcher', 'tag.bin --slot 11 --name FRIZZ-BETA --device l --stay', "
                          "'--color 11 FF7000 --device l', 'run 11']\n")
              && Has(out, "10 not its release: []\n")
              && Has(out, "versions: (1, 5, 0, 0) (1, 5, 0, 12) None v1.5.0-beta.1\n"),
          "tools: 10 and 11 take a tag's build only where kFrizzVersion is the tag's (the loop transfer's HELLO)");
    Check(Has(out, "beta 0: None None\n")
              && Has(out, "exit: v1.5.0-beta.0 isn't a version's tag (vX.Y.Z, vX.Y.Z-beta.N, N from 1)\n11 beta.0: []\n")
              && Has(out, "release.sh v1.5.0-beta.0: refused\nrelease.sh v1.5.0-beta.01: refused\n"
                          "release.sh v1.5.0-beta.1: gh called\nrelease.sh v1.5.0: gh called\n"),
          "tools: a beta's number starts at 1: -beta.0 isn't taken for the release, in builds.py and release.sh");
    Check(Has(out, "test: ['build', 'launcher', 'tag.bin --slot 12 --name FRIZZ-TEST --device l --stay', "
                   "'--color 12 FF0000 --device l', 'run 12']\n")
              && Has(out, "file to 12: ['launcher', 'other.bin --slot 12 --name FRIZZ-TEST --device l --stay', "
                          "'--color 12 FF0000 --device l', 'run 12']\n")
              && Has(out, "file to 13: ['launcher', 'other.bin --slot 13 --name FRIZZ-BENCH --device l --stay', "
                          "'--color 13 FF0000 --device l', 'run 13']\n")
              && Has(out, "bench: ['launcher', 'tag.bin --slot 13 --name FRIZZ-BENCH --device l --stay', "
                          "'--color 13 FF0000 --device l', 'run 13']\n"),
          "tools: a branch's build goes to the test slot as FRIZZ-TEST, the bench and a file to 13 as FRIZZ-BENCH, "
          "both red (#171)");
    Check(Has(out, "exit: key 13 is the bench's: --bench sends FRIZZ-bench.bin there (FRIZZ.bin goes to the test slot, --test)\n"
                   "13 without --bench: []\n")
              && Has(out, "exit: --bench sends the bench (FRIZZ-bench.bin) to its key, 13: not to key 12\nthe bench to 12: []\n")
              && Has(out, "exit: --bench or --test: the bench goes to key 13, a branch's build to key 12\n--test --bench: []\n")
              && Has(out, "exit: key 13 is the bench's: --bench sends FRIZZ-bench.bin there (FRIZZ.bin goes to the test slot, --test)\n"
                          "--test to 13: []\n"),
          "tools: flash.py sends the bench only to its key and FRIZZ.bin never there, before restarting the CHOMPI: "
          "neither goes out under the other's name");
    if (rc != 0 || failures)
        printf("%s\n", out.c_str());

    // midisuite.py and reaper.py (#171): FRIZZ asked before each case and before putting things
    // back whether it still keeps the card out of it; one that restarted stops the run, nothing
    // put back. SIGTERM isn't taken for a case's FAIL but stops the run as Ctrl-C does; the
    // exit codes
    rc = Python(R"(
import os, signal, sys, time, types, midisuite
sys.stdout.reconfigure(line_buffering=True)
sys.stderr = sys.stdout
kept = [True]
class F:
    def dev(self, sub, args=()):
        return 0, bytes(25) + bytes([1 if kept[0] else 0])
log = []
midisuite.reset = lambda r, snap, scenes: log.append("reset")
class Snap:
    def restore_settings(self, r):
        log.append("settings")
def rig():
    return types.SimpleNamespace(f=F(), lost=None, wiring=set(), args=types.SimpleNamespace(verbose=False))
def checked(r):
    r.checks.append((True, "x", 1, 1))
def restarts(r):
    checked(r)
    kept[0] = False
def term(r):
    os.kill(os.getpid(), signal.SIGTERM)
    time.sleep(2)
def run(name, fn):
    r = rig()
    log.clear()
    try:
        got = midisuite.run_case(r, midisuite.Case(name, "item", fn), Snap())
    except KeyboardInterrupt as e:
        got = "interrupted %s" % getattr(e, "signum", "")
    print("%s: %s %s%s" % (name, got, log, ", lost" if r.lost else ""))
    return r
run("fine", checked)
r = run("restarts", restarts)
run("restarted before", checked)
log.clear()
print("put back, lost:", midisuite.put_back(r, Snap()), log)
kept[0] = True
log.clear()
print("put back:", midisuite.put_back(rig(), Snap()), log)
midisuite.stop_on_signals()
run("sigterm", term)
print("then ignored:", signal.getsignal(signal.SIGTERM) == signal.SIG_IGN)
print("exit codes:", midisuite.exit_code(False, None), midisuite.exit_code(True, None),
      midisuite.exit_code(False, signal.SIGTERM), midisuite.exit_code(True, signal.SIGINT))
)", out);
    Check(rc == 0 && Has(out, "fine: ('PASS', '') ['reset', 'reset']\n")
              && Has(out, "restarts: ('FAIL', 'stopped: FRIZZ restarted (up 0.0 s, reset: none): it writes the card "
                          "again') ['reset'], lost\n")
              && Has(out, "restarted before: ('FAIL', 'not run: FRIZZ restarted") && Has(out, "') [], lost\n"),
          "tools: midisuite's cases ask IDENT first and after: one that restarted stops the run, its settings not "
          "put back (#171)");
    Check(Has(out, "not put back: FRIZZ restarted") && Has(out, "put back, lost: False []\n")
              && Has(out, "put back: True ['reset', 'settings']\n"),
          "tools: and nothing put back at the end then, which fails the run");
    Check(Has(out, "sigterm: interrupted 15 ['reset', 'reset']\n") && Has(out, "then ignored: True\n")
              && Has(out, "exit codes: 0 1 143 130\n"),
          "tools: SIGTERM in a case isn't its FAIL: things put back, the run stopped, exit 128 + 15 (#171)");
    if (rc != 0 || failures)
        printf("%s\n", out.c_str());

    // the folder whole: the cases leave more in it than these (the loops' WAVs, the card), and
    // rmdir alone left ~200 KB in /tmp each run
    if (system(("rm -rf '" + tmp_dir + "'").c_str()) != 0)
        printf("could not remove %s\n", tmp_dir.c_str());
    return Finish();
}
