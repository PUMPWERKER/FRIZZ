// ptytwin.h: a tool of firmware/tools (midisuite.py, reaper.py) against the virtual CHOMPI
// (twin/twin.h) at the wall clock's pace: the twin's USB MIDI and its TRS jack each on a
// pseudo-terminal, in place of the CHOMPI's ALSA node and the audio interface's, the jack wired
// in, out or both (trs_in, trs_out) as the TRS adapter can be. Tool() runs one and pumps the
// twin until it's done; lag says how far the twin fell behind the wall clock meanwhile, and
// Unjudged() which of its cases failed on timing alone that far behind: under load (all.sh, or
// several side by side) the twin's answers come late and bunched, where a tool's timing can't
// be judged (#183). With audio_dir set, the twin has an audio interface too, through files a
// tool's stand-in (reaper_stub.py) shares with it there: Audio's. For a check (NAME.cpp) that
// includes twin.h first.
#pragma once
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
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

/** The cases a tool's run in out failed on timing alone (its "failed on timing alone: ..."
 *  line, of the checks it tags [timing]), if the twin fell too far behind to judge timing */
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

/** attempt() (a tool's run against the twin: true if it passed), and once more if it didn't
 *  with the twin more than kLagMs behind the wall clock: so far behind (hundreds of ms on a
 *  machine whose cores are all busy) its answers can come after the tool stopped waiting for
 *  them, beyond what Unjudged() covers. Said in a line FLAKY (all.sh lists them); a run that
 *  fails twice, or once with the twin on time, fails (#183) */
template <class Attempt>
void Twice(const std::string& name, Attempt attempt)
{
    if (attempt() || lag <= kLagMs)
        return;
    printf("FLAKY  %s: failed with the twin %ld ms behind the wall clock, run again\n", name.c_str(), lag);
    attempt();
}

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

/** Where a tool's stand-in and the twin share its audio (FRIZZ_TWIN_AUDIO), empty for none */
std::string audio_dir;

/** The twin's audio interface, in files in audio_dir, the time this computer's monotonic clock
 *  (Python's perf_counter, steady_clock here) in s, the twin's sample n at start + n / 48000:
 *    start         that start, written here
 *    timeline.f32  a song as REAPER plays it from 0 (float, mono, 48 kHz), the stand-in's
 *    aux.txt       where it plays from when: lines "TIME SONG_S RATE" (1 playing, 0 stopped)
 *    out.f32       the line out's left, every sample from the start, written here
 *  AUX gets the song where aux.txt has it at each sample's time, both sides */
struct Audio
{
    std::vector<float> timeline;
    std::vector<double> moves; // TIME, SONG_S, RATE each
    off_t timeline_size = -1, aux_size = -1;
    double start = 0;
    FILE* out = nullptr;

    static off_t Size(const std::string& path)
    {
        struct stat st;
        return stat(path.c_str(), &st) ? -1 : st.st_size;
    }

    void Open(double start_s)
    {
        start = start_s;
        FILE* f = fopen((audio_dir + "/start").c_str(), "w");
        if (f)
        {
            fprintf(f, "%.9f\n", start);
            fclose(f);
        }
        out = fopen((audio_dir + "/out.f32").c_str(), "w");
    }

    /** What the stand-in wrote since, read again */
    void Look()
    {
        const off_t t = Size(audio_dir + "/timeline.f32");
        if (t >= 0 && t != timeline_size)
        {
            timeline_size = t;
            timeline.assign(t / sizeof(float), 0.f);
            FILE* f = fopen((audio_dir + "/timeline.f32").c_str(), "rb");
            if (f)
            {
                timeline.resize(fread(timeline.data(), sizeof(float), timeline.size(), f));
                fclose(f);
            }
        }
        const off_t a = Size(audio_dir + "/aux.txt");
        if (a >= 0 && a != aux_size)
        {
            aux_size = a;
            moves.clear();
            std::string text;
            FILE* f = fopen((audio_dir + "/aux.txt").c_str(), "r");
            if (f)
            {
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                    text.append(buf, n);
                fclose(f);
            }
            std::istringstream lines(text.substr(0, text.rfind('\n') + 1));
            double time, song, rate;
            while (lines >> time >> song >> rate)
                moves.insert(moves.end(), {time, song, rate});
        }
    }

    float At(double time) const
    {
        size_t i = moves.size();
        while (i >= 3 && moves[i - 3] > time)
            i -= 3;
        if (i < 3)
            return 0.f;
        const double song = moves[i - 2] + (time - moves[i - 3]) * moves[i - 1];
        const long at = std::lround(song * kSampleRate);
        return at >= 0 && at < long(timeline.size()) ? timeline[at] : 0.f;
    }

    /** 2 blocks (1 ms) from sample n on: AUX in, the line out written */
    void Run2(long n)
    {
        static const size_t kFrames = 2 * kBlockSize;
        float in[kFrames * kChannels] = {}, o[kFrames * kChannels];
        for (size_t k = 0; k < kFrames; k++)
            in[k * kChannels + 2] = in[k * kChannels + 3] = At(start + (n + long(k)) / double(kSampleRate));
        twin::Run(2, in, o);
        float left[kFrames];
        for (size_t k = 0; k < kFrames; k++)
            left[k] = o[k * kChannels + 2];
        if (out)
        {
            fwrite(left, sizeof(float), kFrames, out);
            fflush(out);
        }
    }

    void Close()
    {
        if (out)
            fclose(out);
        out = nullptr;
    }
};

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
    Audio audio;
    if (!audio_dir.empty())
        audio.Open(std::chrono::duration<double>(start.time_since_epoch()).count());
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
            const long ms = long(NowMs() - twin_start);
            Pump(ms);
            if (audio_dir.empty())
                Run(2, nullptr, nullptr);
            else
            {
                if (ms % 10 == 0)
                    audio.Look();
                audio.Run2(ms * long(kSampleRate / 1000));
            }
        }
        else
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    reading = false;
    reader.join();
    audio.Close();
    char buf[512];
    ssize_t n;
    while ((n = read(pipe_fd[0], buf, sizeof(buf))) > 0)
        out.append(buf, n);
    close(pipe_fd[0]);
    out += "\n(the twin was " + std::to_string(lag) + " ms behind the wall clock at most)\n";
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
} // namespace
