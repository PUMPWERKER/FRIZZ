// lockstep.cpp: the tools on the virtual CHOMPI's clock (ptytwin.h's ExecLockstep() and
// test/lockstep/sitecustomize.py, #388), which remote, midisuite and reaper run them on: a
// stall of the computer doesn't move the twin's time, so a tool's timing comes out the same
// under any load; its threads, locks, conditions, joins and Python children all wait on that
// clock; the pseudo-terminals hand bytes over at once (what makes it exact); a scheduler that
// can't go on stops loudly, saying where; and off the twin (on the CHOMPI) nothing of it is
// there: the tools keep the wall clock.
#include "cases.h"
#include "check.h"
#include "twin.h"
#include "ptytwin.h"

namespace
{
/** Python CODE (a heredoc) against the twin on its clock, tools/ on its path: its exit code */
int Piece(const std::string& code, std::string& out, int limit = 20)
{
    return Exec("python3 - <<'EOF'\n" + code + "\nEOF\n", out, limit, {{"PYTHONPATH", firmware_dir + "/tools"}});
}

/** The lines of out that start with what, without it */
std::vector<std::string> Lines(const std::string& out, const std::string& what)
{
    std::vector<std::string> got;
    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line))
        if (line.compare(0, what.size(), what) == 0)
            got.push_back(line.substr(what.size()));
    return got;
}

// a clock at 120 from the tool to the twin over USB, its 4th tick's write held up by a real
// stall of STALL s (select on nothing: the wall clock's, as a busy computer's): when each
// tick went, ms of the twin's time, and how late the clock was at worst
const char* kClock = R"(
import os, select, time, midiport
fd = os.open(os.environ["FRIZZ_LOCKSTEP_USB"], os.O_RDWR | os.O_NONBLOCK)
n = [0]
def write(data):
    n[0] += 1
    if n[0] == 4 and STALL:
        select.select([], [], [], STALL)
    os.write(fd, data)
t0 = time.perf_counter()
c = midiport.Clock(write, 120)
time.sleep(.3)
t1 = time.perf_counter()
c.close()
print("ticks", " ".join("%.1f" % ((t - t0) * 1000) for t in c.ticks))
print("late %.3f" % (midiport.LATE[0] * 1000))
print("took %.1f" % ((t1 - t0) * 1000))
)";
} // namespace

int main()
{
    firmware_dir = std::string(__FILE__).substr(0, std::string(__FILE__).rfind('/')) + "/..";
    signal(SIGPIPE, SIG_IGN);
    lockstep = true;

    // the pseudo-terminal hands bytes over at once: what one end wrote, the other's poll()
    // finds, both ways, every time (it moves them over itself, n_tty's poll; a kernel worker
    // would otherwise, some time later). Every process waiting, all it wrote is there
    cases.push_back({"pty", [] {
        Pty p;
        Check(p.Open(), "pty: a pseudo-terminal");
        const int far = open(p.path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        int missed[2] = {0, 0};
        for (int i = 0; i < 2000; i++)
            for (int way = 0; way < 2; way++)
            {
                const int from = way ? far : p.fd, to = way ? p.fd : far;
                const uint8_t b = uint8_t(i);
                if (write(from, &b, 1) != 1)
                    missed[way]++;
                pollfd f = {to, POLLIN, 0};
                uint8_t got = 0;
                if (poll(&f, 1, 0) != 1 || read(to, &got, 1) != 1 || got != b)
                    missed[way]++;
                while (read(to, &got, 1) > 0)
                {
                }
            }
        close(far);
        Check(missed[0] == 0 && missed[1] == 0, "pty: a byte written is there at once, 2000 times each way");
        if (missed[0] || missed[1])
            printf("missed %d to the far end, %d back\n", missed[0], missed[1]);
    }});
    // a real stall in the tool's clock thread (80 ms of the wall clock) moves nothing: the
    // ticks go at the same times as without it, and the clock is never late (the twin's ms:
    // under 1 ms, a wake at the next whole ms)
    cases.push_back({"stall", [] {
        usb.Open();
        std::string calm, stalled;
        const int rc = Piece(std::string("STALL = 0") + kClock, calm) | Piece(std::string("STALL = .08") + kClock, stalled);
        const std::vector<std::string> a = Lines(calm, "ticks "), b = Lines(stalled, "ticks ");
        const std::vector<std::string> late = Lines(stalled, "late "), took = Lines(stalled, "took ");
        Check(rc == 0 && a.size() == 1 && a == b, "stall: a stall of the computer doesn't move the tool's clock: the same ticks");
        Check(late.size() == 1 && atof(late[0].c_str()) <= 1., "stall: its clock never late on the twin's time (a wake at the next whole ms)");
        Check(took.size() == 1 && atof(took[0].c_str()) >= 300. && atof(took[0].c_str()) < 302.,
              "stall: its sleep of 300 ms took 300 ms of the twin's time");
        if (failures)
            printf("calm:\n%s\nstalled:\n%s\n", calm.c_str(), stalled.c_str());
    }});
    // threads on the twin's clock: an Event's timeout, a notify, a lock held across a sleep, a
    // join, each at its time to the ms
    cases.push_back({"threads", [] {
        usb.Open();
        std::string out;
        const int rc = Piece(R"(
import threading, time
t0 = time.monotonic()
def at(what):
    print("%s %.1f" % (what, (time.monotonic() - t0) * 1000))
e = threading.Event()
at("event-timeout %s" % e.wait(.05))
cond = threading.Condition()
box = []
def producer():
    time.sleep(.1)
    with cond:
        box.append(1)
        cond.notify()
p = threading.Thread(target=producer, daemon=True)
p.start()
with cond:
    cond.wait_for(lambda: box, timeout=1)
at("notified")
lock = threading.Lock()
def holder():
    with lock:
        time.sleep(.2)
h = threading.Thread(target=holder)
h.start()
time.sleep(.01)
with lock:
    at("lock")
s = threading.Thread(target=time.sleep, args=(.3,))
s.start()
s.join()
at("joined")
h2 = threading.Thread(target=time.sleep, args=(5,), daemon=True)
h2.start()
h2.join(.1)
at("join-timeout %s" % h2.is_alive())
)", out);
        const std::string want = "event-timeout False 50.0\nnotified 150.0\nlock 350.0\njoined 650.0\njoin-timeout True 750.0\n";
        Check(rc == 0 && Has(out, want), "threads: Event, Condition, Lock, join and their timeouts on the twin's time, to the ms");
        if (rc != 0 || !Has(out, want))
            printf("%s\n", out.c_str());
    }});
    // a Python child takes the same clock: it starts at the parent's time, its sleep is the
    // twin's, and the parent's wait for it too
    cases.push_back({"child", [] {
        usb.Open();
        std::string out;
        const int rc = Piece(R"PY(
import subprocess, sys, time
t0 = time.monotonic()
p = subprocess.Popen([sys.executable, "-c", "import time; t = time.monotonic(); time.sleep(.25); print('child %.1f %.1f' % (t * 1000, time.monotonic() * 1000), flush=True)"])
p.wait(5)
print("parent %.1f %.1f" % (t0 * 1000, time.monotonic() * 1000))
)PY", out);
        const std::vector<std::string> child = Lines(out, "child "), parent = Lines(out, "parent ");
        double c0 = 0, c1 = 0, p0 = 0, p1 = 0;
        const bool read = child.size() == 1 && parent.size() == 1 && sscanf(child[0].c_str(), "%lf %lf", &c0, &c1) == 2
                          && sscanf(parent[0].c_str(), "%lf %lf", &p0, &p1) == 2;
        Check(rc == 0 && read && c0 == p0 && c1 - c0 == 250. && p1 >= c1 && p1 <= c1 + 2.,
              "child: a Python child on the parent's clock, its sleep and the parent's wait on the twin's time");
        if (rc != 0 || !read || c0 != p0 || c1 - c0 != 250. || p1 < c1 || p1 > c1 + 2.)
            printf("%s\n", out.c_str());
    }});
    // a thread that waits on what the twin can't bring (a pipe nobody writes) stands the clock
    // still: stopped after kStuckS of the wall clock, said so, its stack printed, not hung
    cases.push_back({"stuck", [] {
        usb.Open();
        kStuckS = 2;
        std::string out;
        const int rc = Piece(R"(
import os, time
r, w = os.pipe()
time.sleep(.1)
os.read(r, 1)
)", out);
        Check(rc == -1 && Has(out, "(lockstep: stuck for 2 s") && Has(out, " runs") && Has(out, "line 5"),
              "stuck: a clock that stands still stops the tool, says so and where it was");
        if (rc != -1 || !Has(out, "(lockstep: stuck"))
            printf("exit %d\n%s\n", rc, out.c_str());
    }});
    // off the twin, as on the CHOMPI, the tools keep the wall clock: without FRIZZ_LOCKSTEP, or
    // without FRIZZ_NO_DEVICE (only the host checks set it), sitecustomize changes nothing, and
    // midiport's clock spins its last ms as it always did
    cases.push_back({"device", [] {
        const std::string code = "import sitecustomize, select, threading, time, midiport, subprocess\n"
                                 "print('builtin', all(type(f).__name__ == 'builtin_function_or_method' for f in "
                                 "(time.sleep, time.monotonic, time.perf_counter, time.time, select.select)))\n"
                                 "print('threading', threading.Lock.__module__ != 'sitecustomize' and "
                                 "threading.Condition.__module__ == 'threading' and "
                                 "threading.Thread.join.__module__ == 'threading' and "
                                 "subprocess.Popen.wait.__module__ == 'subprocess')\n"
                                 "print('spins', not midiport.LOCKSTEP)\n";
        for (const std::string& env : {std::string("env -u FRIZZ_LOCKSTEP"), std::string("env -u FRIZZ_NO_DEVICE FRIZZ_LOCKSTEP=@nowhere")})
        {
            const std::string cmd = env + " PYTHONDONTWRITEBYTECODE=1 PYTHONPATH='" + firmware_dir + "/test/lockstep:"
                                    + firmware_dir + "/tools' python3 -c \"" + code + "\" 2>&1";
            std::string out;
            FILE* p = popen(cmd.c_str(), "r");
            char buf[512];
            size_t n;
            while (p && (n = fread(buf, 1, sizeof(buf), p)) > 0)
                out.append(buf, n);
            const int rc = p ? pclose(p) : -1;
            const bool off = rc == 0 && Has(out, "builtin True") && Has(out, "threading True") && Has(out, "spins True");
            Check(off, ("device: " + env.substr(4) + ": the wall clock's time, sleep, select, threading and Popen, as on the CHOMPI").c_str());
            if (!off)
                printf("%s\n", out.c_str());
        }
    }});
    RunAllCases();
    return Finish();
}
