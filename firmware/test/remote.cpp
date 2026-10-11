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
    Check(Has(out, "FRIZZ answers on slot 12") && Has(out, "noted 12\n"),
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
              && Has(out, "0 throughout: the bench"),
          "remote: play --cpu counts the load's unanswered asks and samples on; a load of 0 is the bench's");
    if (rc != 0)
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

    rc = Remote({"leds"}, out);
    const std::string leds = LedLine();
    Check(rc == 0 && out == leds + "\n", "remote: leds prints every LED as the twin's LED log does");
    if (out != leds + "\n")
        printf("      remote: %s      twin:   %s\n", out.c_str(), leds.c_str());

    rc = Remote({"load"}, out);
    Check(rc == 0 && Has(out, "max") && Has(out, "mean"), "remote: load reads the load");

    rc = Remote({"load", "--device", slave_path}, out);
    Check(rc == 0 && Has(out, "max"), "remote: --device after the subcommand counts too");
    if (rc != 0)
        printf("%s\n", out.c_str());

    Remote({"channel", "5"}, out);
    Remote({"transport", "on"}, out);
    rc = Remote({"settings"}, out);
    Check(rc == 0 && out == "channel 5, transport on, clock auto, out off\n",
          "remote: channel and transport set, settings reads them");
    Remote({"channel", "0"}, out);
    rc = Remote({"settings"}, out);
    Check(rc == 0 && out == "channel all, transport on, clock auto, out off\n", "remote: channel 0 is every channel");
    Remote({"source", "usb"}, out);
    rc = Remote({"settings"}, out);
    Check(rc == 0 && out == "channel all, transport on, clock usb, out off\n", "remote: source sets the clock source");
    Remote({"out", "trs"}, out);
    rc = Remote({"settings"}, out);
    Check(rc == 0 && out == "channel all, transport on, clock usb, out trs\n", "remote: out sets MIDI out");
    RunMs(2500);
    Check(Has(Card("/FRIZZ/frizz_master.txt"), "midi_channel 0\nmidi_transport 1\n")
              && Has(Card("/FRIZZ/frizz_master.txt"), "clock_source 2\nmidi_out 1\n"),
          "remote: and they go to the card");

    // MIDI out over USB: its clock comes between the answers, and remote.py reads past it
    Remote({"out", "all"}, out);
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
    Remote({"out", "off"}, out);

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
    rc = Remote({"scene", "put", "2", mine}, out);
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
    rc = Remote({"scene", "put", "0", mine}, out);
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
    rc = Remote({"scene", "put", "3", mine}, out);
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
            Press("KEY_20", true);
            RunMs(60);
            Press("KEY_20", false);
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
    Write(script, "booted\ntap KEY_18\nwait 100\n"); // F#: mono
    rc = Remote({"play", script, "--save"}, out);
    const std::string refused = out;
    Remote({"state"}, out);
    Check(rc != 0 && Has(refused, "line 2: tap KEY_18 on the settings page") && Has(refused, "--force")
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
    Write(script, "booted\ntoggle 1\nwait 100\ntap KEY_18\nwait 100\ntoggle 0\nwait 100\n");
    rc = Remote({"play", script, "--save"}, out);
    Check(rc != 0 && Has(out, "line 4: tap KEY_18 on the settings page"),
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
    Write(script, "booted\nmidi B0 34 40\n"); // CC 52: the compressor's knob 1
    rc = Remote({"play", script, "--save"}, out);
    Check(rc != 0 && Has(out, "line 2: the compressor's knob over MIDI (CC 52)"),
          "remote: and its knob over MIDI is refused (#78)");
    Write(script, "booted\ntap KEY_25\nwait 100\ntap KEY_17\nwait 100\ntap KEY_26\nwait 500\n");
    rc = Remote({"play", script, "--save"}, out);
    const std::string refused_save = out;
    Remote({"state"}, out);
    Check(rc != 0 && Has(refused_save, "line 6: SAVE confirmed (tap KEY_26)")
              && Has(refused_save, "/FRIZZ/frizz_scenes.txt") && Has(out, "mode none"),
          "remote: play refuses a scene saved with SAVE, a slot and CHOMPI, naming the scenes file (#78)");
    if (rc == 0 || !Has(refused_save, "frizz_scenes"))
        printf("%s\n", refused_save.c_str());

    // since PERSIST 0, play keeps the card as it was (#58, #78): the settings page's mono key,
    // a channel over SysEx, the compressor's knob and a scene saved change FRIZZ, not its files
    const std::string master_file = CardFiles()["/FRIZZ/frizz_master.txt"];
    const std::string scenes_file = CardFiles()["/FRIZZ/frizz_scenes.txt"];
    Write(script, "booted\ntoggle 1\nwait 100\ntap KEY_18\nwait 100\ntoggle 0\nwait 100\n"
                  "midi F0 7D 43 48 13 00 05 F7\ntap KEY_15\nwait 100\nturn 4 10\nwait 100\n"
                  "tap KEY_25\nwait 100\ntap KEY_18\nwait 100\ntap KEY_26\nwait 2500\n");
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
def case(name, info, slot=None):
    running["info"] = info
    log.clear()
    got = chompi.to_frizz(slot)
    print("%s: %s %s %s" % (name, got, chompi.frizz_slot, log), flush=True)
a = dict(kind="frizz", crc=chompi.image_id("a.bin")[0], length=7)
case("by hand", a)
case("asked", a, 12)
case("other", a, 10)
case("bench", dict(a, kind="bench"))
case("unknown", dict(a, crc=1))
case("old", None)
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
    Check(Has(out, "exit: it runs crc32 00000005, 7 B, NOT the build sent")
              && Has(out, "it runs the build sent: crc32 "),
          "tools: flash.py checks the build running is the one sent");
    if (rc != 0 || !Has(out, "it runs the build sent"))
        printf("%s\n", out.c_str());

    unlink(blank.c_str());
    unlink(mine.c_str());
    unlink(back.c_str());
    unlink(script.c_str());
    rmdir(tmp_dir.c_str());
    return Finish();
}
