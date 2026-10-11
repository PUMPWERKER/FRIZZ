// midisuite.cpp: firmware/tools/midisuite.py against the virtual CHOMPI (twin/twin.h), as
// remote.cpp does remote.py's: the twin's USB MIDI and its TRS jack each on a pseudo-terminal,
// in place of the CHOMPI's ALSA node and the audio interface's, the twin at the wall clock's
// pace. The jack is wired one way, the other or both, as the TRS adapter can be: the suite has
// to find that out, run what the wiring shows, and pass on the firmware as it is; what the
// wiring can't show is a SKIP, never a FAIL. It checks the suite's logic, not the firmware's
// timing (sync.cpp, midiout.cpp do that, the device the rest): --quick runs the long cases
// short and widens the tolerances on timing, as the pseudo-terminals under all.sh's load
// jitter by tens of ms, and fast-usb / fast-trs, which judge only how still the tempo holds,
// aren't run.
//
// MIDISUITE="ARGS" runs the suite once with those arguments (--wiring both if none is given)
// and prints what it printed: the live twin to work on midisuite.py with, e.g.
//     MIDISUITE="--cases out-loop -v" ./unit.sh midisuite
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "cases.h"
#include "check.h"
#include "twin.h"

using namespace twin;

namespace
{
std::string firmware_dir;

/** A pseudo-terminal, raw, so every byte goes through as it is; its far end's path */
struct Pty
{
    int fd = -1;
    std::string path;
    bool Open()
    {
        fd = posix_openpt(O_RDWR | O_NOCTTY);
        if (fd < 0 || grantpt(fd) || unlockpt(fd))
            return false;
        path = ptsname(fd);
        // kept open, so the terminal stays up whoever has it
        const int far = open(path.c_str(), O_RDWR | O_NOCTTY);
        termios t;
        if (far < 0 || tcgetattr(far, &t))
            return false;
        cfmakeraw(&t);
        tcsetattr(far, TCSANOW, &t);
        fcntl(fd, F_SETFL, O_NONBLOCK);
        return true;
    }
};

Pty usb, trs;
bool trs_in = true, trs_out = true; // the TRS adapter: the interface's out into the jack, the jack's out into it
long lag = 0; // the most the twin fell behind the wall clock, ms
// Behind by more than this, the twin's timing isn't the wall clock's: a case that failed on
// timing alone ([timing], "failed on timing alone") isn't judged
const long kLagMs = 5;

/** A byte the suite sent, and when it came, in ms of the wall clock since the run started */
struct Arrival
{
    long ms;
    bool usb;
    uint8_t byte;
};
std::mutex arrivals_mutex;
std::deque<Arrival> arrivals;
std::atomic<bool> reading{false};

/** Reads what the suite sends as it comes, timed: a twin that fell behind (all.sh's load) still
 *  gets each byte at its time, not all that came meanwhile in one block, where a key's press and
 *  release would fall into one debounce */
void Reader(std::chrono::steady_clock::time_point start)
{
    while (reading)
    {
        pollfd fds[2] = {{usb.fd, POLLIN, 0}, {trs.fd, POLLIN, 0}};
        if (poll(fds, 2, 1) <= 0)
            continue;
        const long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        for (int p = 0; p < 2; p++)
        {
            uint8_t buf[256];
            ssize_t n;
            while ((n = read(fds[p].fd, buf, sizeof(buf))) > 0)
            {
                std::lock_guard<std::mutex> lock(arrivals_mutex);
                for (ssize_t i = 0; i < n; i++)
                    arrivals.push_back({ms, p == 0, buf[i]});
            }
        }
    }
}

/** What came by the twin's time ms, in; what the twin sent, out (USB both ways; the jack as
 *  it's wired: what isn't is lost, as without a cable) */
void Pump(long ms)
{
    {
        std::lock_guard<std::mutex> lock(arrivals_mutex);
        while (!arrivals.empty() && arrivals.front().ms <= ms)
        {
            const Arrival a = arrivals.front();
            arrivals.pop_front();
            if (a.usb)
                UsbMidi(a.byte);
            else if (trs_in)
                Midi(a.byte);
        }
    }
    const std::string out = TakeUsbOut();
    if (!out.empty() && write(usb.fd, out.data(), out.size()) < 0)
        perror("usb pty");
    std::string jack;
    for (const MidiOutByte& b : TakeMidiOut())
        if (!b.usb)
            jack += static_cast<char>(b.byte);
    if (trs_out && !jack.empty() && write(trs.fd, jack.data(), jack.size()) < 0)
        perror("trs pty");
}

/** midisuite.py ARGS against the twin, at the wall clock's pace until it's done (limit s at
 *  most): its exit code; out gets what it printed */
int Suite(const std::string& args, std::string& out, int limit)
{
    if (!usb.Open() || !trs.Open())
    {
        out = "no pseudo-terminal";
        return -1;
    }
    // the boot and the rainbow, as fast as the twin goes: the play page takes keys from here
    // (midisuite.py waits for it on a FRIZZ just started)
    while (NowMs() < 7000)
        Run(2, nullptr, nullptr);
    int pipe_fd[2];
    if (pipe(pipe_fd))
        return -1;
    const std::string cmd = "exec python3 '" + firmware_dir + "/tools/midisuite.py' --device '" + usb.path
                            + "' --trs '" + trs.path + "' " + args;
    const pid_t pid = fork();
    if (pid == 0)
    {
        dup2(pipe_fd[1], 1);
        dup2(pipe_fd[1], 2);
        close(pipe_fd[0]);
        setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
        setenv("FRIZZ_CHOMPI_HELD", "1", 1); // the twin, not the CHOMPI: no lock (tools/chompi.py)
        execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(pipe_fd[1]);
    fcntl(pipe_fd[0], F_SETFL, O_NONBLOCK);
    out.clear();
    lag = 0;
    const auto start = std::chrono::steady_clock::now();
    arrivals.clear();
    reading = true;
    std::thread reader(Reader, start);
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
        if (wall > std::chrono::seconds(limit))
        {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            out += "(timed out)";
            break;
        }
        // one ms of the twin per ms of the wall clock
        const long behind = std::chrono::duration_cast<std::chrono::milliseconds>(wall).count()
                            - long(NowMs() - twin_start);
        lag = std::max(lag, behind);
        if (behind > 0)
        {
            Pump(long(NowMs() - twin_start));
            Run(2, nullptr, nullptr);
        }
        else
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    reading = false;
    reader.join();
    char buf[512];
    ssize_t n;
    while ((n = read(pipe_fd[0], buf, sizeof(buf))) > 0)
        out.append(buf, n);
    close(pipe_fd[0]);
    out += "\n(the twin was " + std::to_string(lag) + " ms behind the wall clock at most)\n";
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool Has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

/** The cases that failed on timing alone, if the twin fell too far behind to judge timing */
std::vector<std::string> Unjudged(const std::string& out)
{
    std::vector<std::string> ids;
    const std::string tag = "failed on timing alone: ";
    const size_t at = out.find(tag);
    if (lag <= kLagMs || at == std::string::npos)
        return ids;
    std::istringstream list(out.substr(at + tag.size(), out.find('\n', at) - at - tag.size()));
    std::string id;
    while (std::getline(list, id, ','))
        ids.push_back(id);
    return ids;
}

/** A case's line in out: "PASS out-loop (7 s)", "SKIP clock-trs: ..." */
bool Line(const std::string& out, const std::string& result, const std::string& id)
{
    for (const char* end : {" (", ":", "\n"})
        if (Has(out, "\n" + result + " " + id + end) || out.compare(0, result.size() + id.size() + 2, result + " " + id + end) == 0)
            return true;
    return false;
}

/** Each of ids has the result want in out's lines, or failed on timing the twin couldn't keep */
void Expect(const std::string& out, const std::vector<std::string>& ids, const std::string& want,
            const std::string& what)
{
    const std::vector<std::string> unjudged = Unjudged(out);
    std::string missing;
    for (const std::string& id : ids)
        if (!Line(out, want, id)
            && !(want == "PASS" && std::find(unjudged.begin(), unjudged.end(), id) != unjudged.end()))
            missing += " " + id;
    Check(missing.empty(), (what + (missing.empty() ? "" : ":" + missing + " not " + want)).c_str());
}

/** The suite on CASES with the jack wired IN / OUT, found by itself: each case's result want,
 *  the summary's lines in it */
void Run(const std::string& name, bool in, bool out_wired, const std::vector<std::string>& ids,
         const std::string& want, const std::vector<std::string>& lines, int exit_code)
{
    trs_in = in;
    trs_out = out_wired;
    std::string list;
    for (const std::string& id : ids)
        list += (list.empty() ? "" : ",") + id;
    std::string out;
    const auto t0 = std::chrono::steady_clock::now();
    const int rc = Suite("--quick -v --cases " + list, out, 55);
    printf("%s: %ld s\n", name.c_str(),
           long(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count()));
    const std::vector<std::string> unjudged = Unjudged(out);
    for (const std::string& id : unjudged)
        printf("%s: %s not judged, the twin %ld ms behind the wall clock\n", name.c_str(), id.c_str(), lag);
    bool ok = rc == exit_code || (rc == 1 && !unjudged.empty() && !Has(out, "error:") && !Has(out, "stopped:"));
    Check(ok, (name + ": exit " + std::to_string(rc) + ", not " + std::to_string(exit_code)).c_str());
    Expect(out, ids, want, name);
    for (const std::string& line : lines)
    {
        Check(Has(out, line), (name + ": says \"" + line + "\"").c_str());
        ok = ok && Has(out, line);
    }
    if (!ok || failures)
        printf("%s\n", out.c_str());
}
} // namespace

int main()
{
    firmware_dir = std::string(__FILE__).substr(0, std::string(__FILE__).rfind('/')) + "/..";
    signal(SIGPIPE, SIG_IGN);
    if (const char* args = getenv("MIDISUITE"))
    {
        Boot();
        std::string a = args, out;
        if (!Has(a, "--wiring"))
            a += " --wiring both";
        trs_in = !Has(a, "--wiring out") && !Has(a, "--wiring none");
        trs_out = !Has(a, "--wiring in") && !Has(a, "--wiring none");
        const int rc = Suite(a, out, 3600);
        printf("%s\nexit %d\n", out.c_str(), rc);
        return rc;
    }

    // wired both ways: every case runs, and passes on the firmware as it is (a few to a process,
    // each under about 25 s; start-spp-half and fast-usb / fast-trs only on the device, the
    // effect channels four of them)
    const std::vector<std::vector<std::string>> both = {
        {"clock-trs", "usb-late", "sources-usb"}, {"sources-trs"}, {"transport-trs", "start-free"},
        {"start-spp"}, {"factor"}, {"out-idle", "out-loop", "out-pass"}, {"out-factor", "out-knobs"},
        {"loop-drift"}, {"out-x2"}, {"cc-channels", "rpn"},
        {"cc-global", "cc-global-trs", "block-trs", "shifter-page", "chaos-cc"}, {"morph-back", "morph-hold"},
        {"feedback", "no-double", "sysex-thru"}};
    for (const auto& ids : both)
        cases.push_back({ids[0], [ids] { Run("both ways, " + ids[0], true, true, ids, "PASS", {"wiring: both ways (found)"}, 0); }});
    // wiring A, the jack's out into the interface: what needs the jack's in is a SKIP, listed
    // for the other wiring; MIDI out is seen on TRS, the clock passed on from USB
    cases.push_back({"wiring A", [] {
        Run("wiring A", false, true, {"clock-trs", "block-trs"}, "SKIP",
            {"wiring: CHOMPI TRS out -> interface MIDI in (A) (found)", "Still open with the other wiring",
             "--cases clock-trs,block-trs"},
            0);
        trs_in = false;
        std::string out;
        Suite("--quick --cases out-pass,cc-global", out, 55);
        Expect(out, {"out-pass", "cc-global"}, "PASS", "wiring A: a clock from USB passed on out of TRS");
        Check(Has(out, "USB clock 120 passed on out of TRS"), "wiring A: says so");
    }});
    // wiring B, the interface's out into the jack: MIDI out is seen over USB instead, the
    // clock passed on from TRS, and the TRS-out items listed for the other wiring
    cases.push_back({"wiring B", [] {
        Run("wiring B", true, false, {"out-idle", "out-pass"}, "PASS",
            {"wiring: interface MIDI out -> CHOMPI TRS in (B) (found)", "TRS clock 120 passed on out of USB",
             "Still open with the other wiring", "`out-idle`: ran over USB; the TRS out needs it"},
            0);
    }});
    // no TRS MIDI at all: USB only
    cases.push_back({"usb only", [] {
        Run("no TRS", false, false, {"sources-trs", "out-factor"}, "SKIP", {"wiring: no TRS MIDI (USB only)"}, 0);
    }});
    // the processes mostly wait on the wall clock: all at once, not a core each
    setenv("CASE_JOBS", "16", 0);
    RunAllCases();
    return Finish();
}
