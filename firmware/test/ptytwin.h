// ptytwin.h: a tool of firmware/tools (midisuite.py, reaper.py) against the virtual CHOMPI
// (twin/twin.h) at the wall clock's pace: the twin's USB MIDI and its TRS jack each on a
// pseudo-terminal, in place of the CHOMPI's ALSA node and the audio interface's, the jack wired
// in, out or both (trs_in, trs_out) as the TRS adapter can be. Tool() runs one and pumps the
// twin until it's done; lag says how far the twin fell behind the wall clock meanwhile. For a
// check (NAME.cpp) that includes twin.h first.
#pragma once
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
#include <string>
#include <thread>
#include <vector>

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

/** tools/TOOL ARGS (--device and --trs given) against the twin, at the wall clock's pace until
 *  it's done (limit s at most), with env's NAME=VALUE pairs set: its exit code; out gets what
 *  it printed. stop_signal (SIGINT, SIGTERM) goes to it once it has printed stop_at */
int Tool(const std::string& tool, const std::string& args, std::string& out, int limit,
         const std::vector<std::pair<std::string, std::string>>& env = {}, const std::string& stop_at = "",
         int stop_signal = 0)
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
    const std::string cmd = "exec python3 '" + firmware_dir + "/tools/" + tool + "' --device '" + usb.path
                            + "' --trs '" + trs.path + "' " + args;
    const pid_t pid = fork();
    if (pid == 0)
    {
        dup2(pipe_fd[1], 1);
        dup2(pipe_fd[1], 2);
        close(pipe_fd[0]);
        setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
        setenv("FRIZZ_CHOMPI_HELD", "1", 1); // the twin, not the CHOMPI: no lock (tools/chompi.py)
        signal(SIGINT, SIG_DFL); // a shell's background job (all.sh) ignores it, Python then too
        for (const auto& e : env)
            setenv(e.first.c_str(), e.second.c_str(), 1);
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
        if (stop_signal && out.find(stop_at) != std::string::npos)
        {
            kill(pid, stop_signal);
            stop_signal = 0;
        }
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
} // namespace
