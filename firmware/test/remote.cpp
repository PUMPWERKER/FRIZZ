// remote.cpp: firmware/remote.py against the virtual CHOMPI (twin/twin.h). The twin's USB MIDI
// sits on a pseudo-terminal in place of the CHOMPI's ALSA node, and remote.py runs as itself
// (--device), while the twin plays at the wall clock's pace: remote.py's SysEx, its parsing of
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
#include <string>
#include <thread>
#include <vector>
#include "check.h"
#include "script.h"
#include "twin.h"

using namespace twin;

static const int kFilterKeyLed = 20;
static const int kPlayLed = 7; // PanelLeds.h

static int master_fd = -1;
static std::string slave_path;
static std::string remote_py;
static std::string tmp_dir;

/** The pseudo-terminal: raw, so every byte goes through as it is */
static bool OpenPty()
{
    master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd < 0 || grantpt(master_fd) || unlockpt(master_fd))
        return false;
    slave_path = ptsname(master_fd);
    // kept open, so the terminal stays up between remote.py's runs
    const int slave = open(slave_path.c_str(), O_RDWR | O_NOCTTY);
    termios t;
    if (slave < 0 || tcgetattr(slave, &t))
        return false;
    cfmakeraw(&t);
    tcsetattr(slave, TCSANOW, &t);
    fcntl(master_fd, F_SETFL, O_NONBLOCK);
    return true;
}

/** What remote.py wrote, into the twin's USB; what the twin sent, back to it */
static void Pump()
{
    uint8_t buf[256];
    ssize_t n;
    while ((n = read(master_fd, buf, sizeof(buf))) > 0)
        for (ssize_t i = 0; i < n; i++)
            UsbMidi(buf[i]);
    const std::string out = TakeUsbOut();
    if (!out.empty() && write(master_fd, out.data(), out.size()) < 0)
        perror("pty");
}

/** Runs ms of silence, as fast as the twin goes */
static void RunMs(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t++)
    {
        Pump();
        Run(2, nullptr, nullptr);
    }
}
#include "twintest.h" // after RunMs, which its Tap runs

static bool Has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

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

/** remote.py ARGS, the twin running at the wall clock's pace until it's done (30 s at most),
 *  interrupted (SIGINT, Ctrl-C) once it has printed interrupt_at, if given. Its exit code; out
 *  gets what it printed, stdout and stderr */
static int Remote(const std::vector<std::string>& args, std::string& out,
                  const std::string& interrupt_at = "")
{
    bool interrupt = !interrupt_at.empty();
    int pipe_fd[2];
    if (pipe(pipe_fd))
        return -1;
    const pid_t pid = fork();
    if (pid == 0)
    {
        dup2(pipe_fd[1], 1);
        dup2(pipe_fd[1], 2);
        close(pipe_fd[0]);
        setenv("PYTHONDONTWRITEBYTECODE", "1", 1); // no __pycache__ in the repo
        setenv("FRIZZ_CHOMPI_HELD", "1", 1); // the twin, not the CHOMPI: no lock (tools/chompi.py)
        signal(SIGINT, SIG_DFL); // a shell's background job (all.sh) ignores it, Python then too
        std::vector<const char*> argv = {"python3", remote_py.c_str(), "--device",
                                         slave_path.c_str()};
        for (const std::string& a : args)
            argv.push_back(a.c_str());
        argv.push_back(nullptr);
        execvp("python3", const_cast<char* const*>(argv.data()));
        _exit(127);
    }
    close(pipe_fd[1]);
    fcntl(pipe_fd[0], F_SETFL, O_NONBLOCK);
    out.clear();
    const auto start = std::chrono::steady_clock::now();
    const uint32_t twin_start = NowMs();
    int status = -1;
    while (true)
    {
        char buf[512];
        ssize_t n;
        while ((n = read(pipe_fd[0], buf, sizeof(buf))) > 0)
            out.append(buf, n);
        if (waitpid(pid, &status, WNOHANG) == pid)
            break;
        const auto wall = std::chrono::steady_clock::now() - start;
        if (interrupt && Has(out, interrupt_at))
        {
            kill(pid, SIGINT);
            interrupt = false;
        }
        if (wall > std::chrono::seconds(30))
        {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            out += "(timed out)";
            break;
        }
        // one ms of the twin per ms of the wall clock
        if (NowMs() - twin_start < std::chrono::duration_cast<std::chrono::milliseconds>(wall).count())
            RunMs(1);
        else
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    char buf[512];
    ssize_t n;
    while ((n = read(pipe_fd[0], buf, sizeof(buf))) > 0)
        out.append(buf, n);
    close(pipe_fd[0]);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}


static std::string firmware_dir;

/** python3 on CODE with tools/ and firmware/ on its path, the lock (tools/chompi.py) a file in
 *  the temp folder and not held, 20 s at most: never the CHOMPI's own. It runs in the temp
 *  folder. Its exit code (-1 for a signal); out gets what it printed, stdout and stderr */
static int Python(const std::string& code, std::string& out)
{
    const std::string file = tmp_dir + "/tools.py";
    std::ofstream(file) << code;
    const std::string cmd = "cd '" + tmp_dir + "' && PYTHONDONTWRITEBYTECODE=1 PYTHONPATH='" + firmware_dir + "/tools:"
                            + firmware_dir + "' FRIZZ_CHOMPI_LOCK='" + tmp_dir
                            + "/lock' FRIZZ_CHOMPI_HELD= timeout -s KILL 20 python3 '" + file
                            + "' 2>&1";
    out.clear();
    FILE* p = popen(cmd.c_str(), "r");
    char buf[512];
    size_t n;
    while (p && (n = fread(buf, 1, sizeof(buf), p)) > 0)
        out.append(buf, n);
    const int status = p ? pclose(p) : -1;
    unlink(file.c_str());
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void Write(const std::string& path, const std::string& text)
{
    std::ofstream(path) << text;
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
        close(lock);
        unlink((tmp_dir + "/lock").c_str());
    }
    // where the slot started afterwards is waited for: a FRIZZ until it answers (both of them)
    int rc = Python("import chompi, card\ncalls = []\n"
                    "chompi.run = lambda slot, wanted, *rest: calls.append((slot, wanted))\n"
                    "card.finish(12)\ncard.finish(None)\ncard.finish(11)\nprint(calls)\n", out);
    Check(rc == 0 && Has(out, "[(12, 'frizz'), (10, 'frizz'), (11, None)]"),
          "tools: card.py --then 12 waits for FRIZZ-TEST as for FRIZZ, not for the bench");
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
def flash_to(*argv):
    sys.argv = ["flash.py", "image.bin"] + list(argv)
    flash.main()
case("flash", [True, False], lambda: flash_to("--slot", "12"))
case("flash 5", [True], lambda: flash_to("--slot", "5", "--name", "FRIZZ-X"))
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
case("bench", frizz, None, 11)
case("booting", [(None, None), ("other", "x"), ("frizz", "node")], None, 12)
case("launcher, this hold", [("launcher", "l")], None, 12)
case("launcher, another", [("launcher", "l")], None, 12, hold="8")
# flash.py: once sent, the launcher for a moment, then nothing, then FRIZZ-TEST
open("image.bin", "wb").write(b"x")
midi_send.main = lambda: None
chompi.to_launcher = lambda *rest: "l"
chompi.slot_file = lambda *rest: None
states[:] = [("launcher", "l"), (None, None), ("frizz", "node")]
sys.argv = ["flash.py", "image.bin", "--slot", "12"]
flash.main()
print("noted", chompi.last_started()[0])
# and the bench, which answers as FRIZZ does
states[:] = [("launcher", "l"), (None, None), ("frizz", "node")]
sys.argv = ["flash.py", "image.bin", "--slot", "11", "--name", "FRIZZ-BENCH"]
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
    Check(Has(out, "the bench runs (slot 11") && Has(out, "bench: started [10]\n"),
          "tools: the bench isn't taken for FRIZZ");
    Check(Has(out, "booting: node []\n"), "tools: a firmware still starting is waited for, not restarted");
    Check(Has(out, "launcher, this hold: started [12]\n")
              && Has(out, "launcher, another: started [10]\n"),
          "tools: from the launcher, the FRIZZ last started in this hold, else FRIZZ's");
    Check(Has(out, "FRIZZ-TEST answers on slot 12") && Has(out, "noted 12\n"),
          "tools: flash.py waits until the FRIZZ it sent answers, and notes its slot");
    Check(Has(out, "FRIZZ-BENCH answers on slot 11") && Has(out, "left [('frizz', 'node')]\n"),
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

    if (!OpenPty())
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

    rc = Remote({"load", "--device", slave_path}, out);
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

    // MIDI out over USB: its clock comes between the answers, and remote.py reads past it
    Remote({"out", "all", "--save"}, out);
    RunMs(200);
    TakeMidiOut();
    rc = Remote({"state"}, out);
    size_t usb_ticks = 0;
    for (const MidiOutByte& b : TakeMidiOut())
        usb_ticks += b.usb && b.byte == 0xF8;
    Check(rc == 0 && Has(out, "looper     empty") && usb_ticks > 0,
          "remote: with MIDI out over USB, its clock comes in between, and state still reads");
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
    Write(mine, json);
    rc = Remote({"scene", "put", "2", mine, "--save"}, out);
    RunMs(2500);
    const std::string scenes = Card("/FRIZZ/frizz_scenes.txt");
    Check(rc == 0 && Has(scenes, "scene 2\n") && Has(scenes, "filter 1 250"),
          "remote: scene put stores it in slot 2, on the card");
    if (rc != 0)
        printf("%s\n", out.c_str());
    rc = Remote({"scene", "get", "2", back}, out);
    json = Read(back);
    Check(rc == 0 && fabsf(JsonParam(json, "filter", 0) - .25f) < 1e-4f && Has(json, "\"filter\"\n"),
          "remote: and scene get reads it back, the same within 14 bits");
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
              && Has(out, "loop       0 frames"),
          "remote: probe says the clock and the looper");
    if (rc != 0)
        printf("%s\n", out.c_str());
    rc = Remote({"signal", "sine", "1000", "0.5"}, out);
    rc |= Remote({"levels"}, out);
    Check(rc == 0 && Has(out, "AUX L         0.500") && Has(out, "-6.0 dBFS") && Has(out, "mic           0.500")
              && !Has(out, "line L        0.0000") && Has(out, "effects    freezer 0.00"),
          "remote: signal plays a sine at half scale where the input was, levels shows it at -6 dBFS");
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

    // a script played on the device: the filter's key held, its LED expected as the twin
    // shows it held and let go, with the load
    const std::string off = Hex(SmtLedFull(kFilterKeyLed));
    Press("KEY_5", true);
    RunMs(300);
    const std::string on = Hex(SmtLedFull(kFilterKeyLed));
    Press("KEY_5", false);
    RunMs(300);
    const std::string script = tmp_dir + "/script.txt";
    Write(script, "input sine 220 0.3\nbooted\nat 100\ndown KEY_5\nwait 300\nexpect led smt 20 " + on +
                      "\nup KEY_5\nwait 300\nexpect led smt 20 " + off + "\n");
    rc = Remote({"play", script, "--cpu"}, out);
    Check(rc == 0 && on != off && Has(out, "skipped (twin only): input") && Has(out, "load: worst max"),
          "remote: play holds a key over SysEx, its LED as expected, and reports the load");
    if (rc != 0)
        printf("%s\n", out.c_str());
    Write(script, "booted\ndown KEY_5\nwait 300\nexpect led smt 20 " + off + "\nup KEY_5\n");
    rc = Remote({"play", script}, out);
    Check(rc == 1 && Has(out, "line 4: smt 20 is " + on + ", not " + off),
          "remote: and an LED that isn't as expected fails the run, telling which");
    rc = Remote({"state"}, out);
    Check(rc == 0 && Has(out, "knobs on   filter"), "remote: the script's key selected the filter");

    // the LEDs dimmed to 50 % on the settings page (A# twice, led_brightness 50 on the card):
    // the same expectations, at full scale, hold at the level that fits them, said (#94)
    auto brightness_key = [](int presses) {
        SetToggle(true);
        RunMs(300);
        for (int i = 0; i < presses; i++)
        {
            Press("KEY_24", true);
            RunMs(60);
            Press("KEY_24", false);
            RunMs(100);
        }
        SetToggle(false);
        RunMs(300);
    };
    brightness_key(2);
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
    brightness_key(1); // and round to 100 %
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
    Write(script, "booted\ndown KEY_5\nwait 100\ntoggle 1\nbattery 3.0\nwait 5000\nup KEY_5\n");
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

    // the mode switch over SysEx (kCmdSwitch): up shows the settings page, and SysEx keys
    // reach it as the hand's do; `switch hand` gives the real switch back. The guard of a FRIZZ
    // from before PERSIST (card_use), as --save keeps it
    Remote({"switch", "up"}, out);
    rc = Remote({"state"}, out);
    Check(rc == 0 && Has(out, "page       settings; the mode switch stands down, SysEx holds it up"),
          "remote: switch up shows the settings page, and state says so");
    Write(script, "booted\ntap KEY_23\nwait 100\n"); // the upper F#: mono
    rc = Remote({"play", script, "--save"}, out);
    const std::string refused = out;
    Remote({"state"}, out);
    Check(rc != 0 && Has(refused, "line 2: tap KEY_23 on the settings page") && Has(refused, "--force")
              && !Has(out, ", mono"),
          "remote: play refuses a key on the settings page the device shows, saying why, and sends none (#58)");
    if (rc == 0 || !Has(refused, "--force"))
        printf("%s\n", refused.c_str());
    Remote({"play", script, "--save", "--force"}, out);
    Remote({"switch", "hand"}, out);
    rc = Remote({"state"}, out);
    Check(rc == 0 && Has(out, ", mono") && Has(out, "page       play; the mode switch stands down\n"),
          "remote: a SysEx key there sets mono; switch hand, the play page again");
    // a script's toggle: up and down over SysEx, the real switch again at its end
    Write(script, "booted\ntoggle 1\nwait 100\ntap KEY_23\nwait 100\ntoggle 0\nwait 100\n");
    rc = Remote({"play", script, "--save"}, out);
    Check(rc != 0 && Has(out, "line 4: tap KEY_23 on the settings page"),
          "remote: and one the script toggles up to, by the line (#58)");
    rc = Remote({"play", script, "--save", "--force"}, out);
    const int played = rc;
    rc = Remote({"state"}, out);
    Check(played == 0 && rc == 0 && !Has(out, ", mono") && Has(out, "page       play; the mode switch stands down\n"),
          "remote: play's toggle 1 / toggle 0 reach the settings page (mono off again), the real switch after");
    if (!Has(out, "page       play"))
        printf("%s\n", out.c_str());
    // the settings page only looked at plays; a setting over SysEx doesn't
    Write(script, "booted\ntoggle 1\nwait 100\ntoggle 0\nwait 100\ntap KEY_18\n");
    rc = Remote({"play", script, "--save"}, out);
    Check(rc == 0, "remote: a script that toggles up and down again, no key there, plays");
    if (rc != 0)
        printf("%s\n", out.c_str());
    // the input's key up there (the upper A#) saves AUX's variant now (#152): refused too
    Write(script, "booted\ntoggle 1\nwait 100\ntap KEY_25\nwait 100\ntap KEY_25\nwait 100\ntoggle 0\nwait 100\n");
    rc = Remote({"play", script, "--save"}, out);
    Check(rc != 0 && Has(out, "line 4: tap KEY_25 on the settings page"),
          "remote: and one that taps the input's key there is refused: AUX's variant is saved (#152)");
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

    // the play page writes the card too (#78): the compressor's knobs to frizz_master.txt,
    // SAVE / COPY / DELETE to frizz_scenes.txt. Refused without --force, nothing sent (with
    // --save, as from a FRIZZ before PERSIST)
    Write(script, "booted\ntap KEY_15\nwait 100\nturn 4 10\nwait 300\n");
    rc = Remote({"play", script, "--save"}, out);
    const std::string refused_comp = out;
    Remote({"state"}, out);
    Check(rc != 0 && Has(refused_comp, "line 4: turn 4 with the compressor selected")
              && Has(refused_comp, "/FRIZZ/frizz_master.txt") && Has(refused_comp, "--force")
              && !Has(out, "knobs on   compressor"),
          "remote: play refuses the compressor's key then a knob, naming the master file, and sends none (#78)");
    if (rc == 0 || !Has(refused_comp, "frizz_master"))
        printf("%s\n", refused_comp.c_str());
    // the compressor selected already: where the device stands counts, as the switch does
    Write(script, "booted\ntap KEY_15\nwait 200\n");
    const int selected = Remote({"play", script, "--save"}, out);
    Remote({"state"}, out);
    const bool comp_shown = Has(out, "knobs on   compressor");
    Write(script, "booted\nturn 1 3\nwait 300\n");
    rc = Remote({"play", script, "--save"}, out);
    Check(selected == 0 && comp_shown && rc != 0 && Has(out, "line 2: turn 1 with the compressor selected"),
          "remote: and a knob alone with the compressor already selected on the device (#78)");
    Write(script, "booted\ntap KEY_5\nwait 100\nturn 4 2\nwait 300\n");
    rc = Remote({"play", script, "--save"}, out);
    Check(rc == 0, "remote: an effect selected first, its knob turns, played");
    if (rc != 0)
        printf("%s\n", out.c_str());
    Write(script, "booted\nmidi BF 18 40 BE 0E 40\n"); // CC 24 and channel 15's 14: the compressor's knob 1
    rc = Remote({"play", script, "--save"}, out);
    Check(rc == 0, "remote: its knob over MIDI is played: CCs never reach the card (#97)");
    if (rc != 0)
        printf("%s\n", out.c_str());
    Write(script, "booted\ntap KEY_5\nwait 100\nmidi BF 17 0F\nturn 1 2\nwait 300\n");
    rc = Remote({"play", script, "--save"}, out);
    Check(rc != 0 && Has(out, "line 5: turn 1 with the compressor selected"),
          "remote: a knob turned once CC 23 put the compressor in focus is refused (#78)");
    Write(script, "booted\ntap KEY_23\nwait 100\ntap KEY_19\nwait 100\ntap KEY_26\nwait 500\n");
    rc = Remote({"play", script, "--save"}, out);
    const std::string refused_save = out;
    Remote({"state"}, out);
    Check(rc != 0 && Has(refused_save, "line 6: SAVE confirmed (tap KEY_26)")
              && Has(refused_save, "/FRIZZ/frizz_scenes.txt") && Has(out, "mode none"),
          "remote: play refuses a scene saved with SAVE, a slot and CHOMPI, naming the scenes file (#78)");
    if (rc == 0 || !Has(refused_save, "frizz_scenes"))
        printf("%s\n", refused_save.c_str());

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
    // a channel over SysEx, the compressor's knob and a scene saved change FRIZZ, not its files
    const std::string master_file = CardFiles()["/FRIZZ/frizz_master.txt"];
    const std::string scenes_file = CardFiles()["/FRIZZ/frizz_scenes.txt"];
    Write(script, "booted\ntoggle 1\nwait 100\ntap KEY_23\nwait 100\ntoggle 0\nwait 100\n"
                  "midi F0 7D 43 48 13 00 05 F7\ntap KEY_15\nwait 100\nturn 4 10\nwait 100\n"
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
    rc = Remote({"set", "settings", "clock_factor", "50"}, out);
    rc |= Remote({"get", "settings"}, out);
    Check(rc == 0 && Has(out, "clock_factor    50\n") && Has(out, "midi_channel    5\n"),
          "remote: set the clock factor, get reads every setting");
    rc = Remote({"set", "settings", "led_brightness", "60"}, out);
    Check(rc != 0 && Has(out, "refused"), "remote: a value a setting doesn't take is refused");
    rc = Remote({"set", "volume", "mix", "1"}, out);
    rc |= Remote({"get", "volume"}, out);
    Check(rc == 0 && Has(out, "mix 1.0000"), "remote: VOLUME's by name");
    rc = Remote({"bench"}, out);
    Check(rc != 0 && Has(out, "not in this build"), "remote: bench, on FRIZZ: not in this build");
    rc = Remote({"load"}, out);
    Check(rc == 0 && Has(out, "over 100 %: 0 blocks"), "remote: load counts the blocks over 100 %");

    // a FRIZZ from before PERSIST (no answer): the old guard, refusing
    rc = Python(R"(
import sys, remote
class Old:
    def ram_only(self):
        return None
    def state(self):
        return bytes([0, 0, 0, 0, 0, 0, 64, 0]) + bytes(20)
remote.Frizz = lambda device: Old()
open("s.txt", "w").write("booted\ntap KEY_18\n")
sys.argv = ["remote.py", "--device", "x", "play", "s.txt"]
try:
    remote.main()
except SystemExit as e:
    print("exit:", e)
)", out);
    Check(Has(out, "a FRIZZ from before PERSIST") && Has(out, "exit: line 2: tap KEY_18 on the settings page"),
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
class F:
    class link:
        fd = None
    def __init__(self, device):
        pass
    def ram_only(self):
        return persist[0]
    def dev(self, sub, args=()):
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
# FRIZZ restarted meanwhile (IDENT: not RAM only): said, failed, before it's started again
print("-- lost")
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

    // keys 10 and 13 are the user's (#171): flash.py sends there only when the key is named, and
    // only the build of a clean v* tag's checkout; refused before anything is built or the
    // CHOMPI restarted (to_launcher). A bare ./flash.py sends nothing
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
midi_send.main = lambda: log.append(" ".join(sys.argv[1:]))
open("tag.bin", "wb").write(b"the tag's build")
open("other.bin", "wb").write(b"another")
builds.build = lambda bench: log.append("build") or "tag.bin"
builds.built = lambda bench: "tag.bin"
builds.image = lambda bench: "tag.bin"
builds.stale = lambda path: False
builds.describe = lambda path: path
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
case("13 untagged", "--no-build", "--slot", "13")
case("file to 10 untagged", "other.bin", "--slot", "10")
tag[0] = "v1.0.0"
case("13 tagged", "--slot", "13")
case("10 tagged", "--no-build", "--slot", "10")
case("its file to 10", "tag.bin", "--slot", "10")
case("another file to 10", "other.bin", "--slot", "10")
case("the bench to 13", "--bench", "--slot", "13")
tag[0] = None
case("test", "--test")
case("file to 12", "other.bin", "--slot", "12")
)", out);
    Check(rc == 0 && Has(out, "tagged beta: None\n") && Has(out, "tagged v9.9.9: v9.9.9\n")
              && Has(out, "tagged, code changed: None\n") && Has(out, "tagged, a file added in code: None\n")
              && Has(out, "tagged, outside code changed: v9.9.9\n") && Has(out, "a commit after: None\n"),
          "tools: flash.py's tagged(): a v* tag on HEAD, nothing changed or added in code/ (#171)");
    Check(Has(out, "exit: which slot?") && Has(out, "bare: []\n")
              && Has(out, "exit: key 10 is the latest release, the user's: only a release's or a beta's build")
              && Has(out, "10 untagged: []\n") && Has(out, "exit: key 13 is the latest beta, the user's")
              && Has(out, "13 untagged: []\n") && Has(out, "file to 10 untagged: []\n"),
          "tools: flash.py sends nothing without a slot, nor an untagged build to 10 or 13, before restarting "
          "the CHOMPI (#171)");
    Check(Has(out, "sending tag.bin to key 13, v1.0.0's\n")
              && Has(out, "13 tagged: ['build', 'launcher', 'tag.bin --slot 13 --name FRIZZ-BETA --device l']\n")
              && Has(out, "10 tagged: ['launcher', 'tag.bin --slot 10 --name FRIZZ --device l']\n")
              && Has(out, "its file to 10: ['launcher', 'tag.bin --slot 10 --name FRIZZ --device l']\n"),
          "tools: a v* tag's build goes to 13 (FRIZZ-BETA) or 10 when the key is named, its file too");
    Check(Has(out, "exit: other.bin isn't the build of v1.0.0 made here") && Has(out, "another file to 10: []\n")
              && Has(out, "exit: key 13 is the latest beta, the user's: never the bench")
              && Has(out, "the bench to 13: []\n"),
          "tools: another file isn't, nor the bench, on a tag too");
    Check(Has(out, "test: ['build', 'launcher', 'tag.bin --slot 12 --name FRIZZ-TEST --device l']\n")
              && Has(out, "file to 12: ['launcher', 'other.bin --slot 12 --name FRIZZ-TEST --device l']\n"),
          "tools: a branch's build goes to the test slot as before, as FRIZZ-TEST");
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

    unlink(blank.c_str());
    unlink(mine.c_str());
    unlink(back.c_str());
    unlink(script.c_str());
    rmdir(tmp_dir.c_str());
    return Finish();
}
