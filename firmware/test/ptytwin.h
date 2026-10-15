// ptytwin.h: a tool of firmware/tools (midisuite.py, reaper.py) against the virtual CHOMPI
// (twin/twin.h): the twin's USB MIDI and its TRS jack each on a pseudo-terminal, in place of the
// CHOMPI's ALSA node and the audio interface's, the jack wired in, out or both (trs_in, trs_out)
// as the TRS adapter can be. Tool() runs one and pumps the twin until it's done, on the twin's
// clock (lockstep, #388): the tool's time, sleeps, selects, locks and threads wait on the twin's
// time (test/lockstep/sitecustomize.py), and the twin advances only while every thread of the
// tool waits, so its timing comes out the same under any load (ExecLockstep()). With
// FRIZZ_TWIN_CLOCK=0 on the wall clock instead, as before (ExecWall()): there lag says how far
// the twin fell behind the wall clock, and Unjudged() which of its cases failed on timing alone
// that far behind, where under load the twin's answers come late and bunched (#183). With
// audio_dir set, the twin has an audio interface too, through files a tool's stand-in
// (reaper_stub.py) shares with it there: Audio's. Exec() is the same for any command, the ptys
// open (remote.cpp: remote.py on usb alone, no TRS; usb_drop loses messages on the way). For a
// check (NAME.cpp) that includes twin.h first.
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "pytool.h"

using namespace twin;

namespace
{
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

Pty usb, trs; // trs unopened (remote.cpp): no TRS, the twin's MIDI out left to the check
bool trs_in = true, trs_out = true; // the TRS adapter: the interface's out into the jack, the jack's out into it
// A SysEx message the tool sent over USB, F0 to F7: lost on the way if this says so, as a USB
// MIDI packet can be (remote.cpp's lost CHUNKs). Unset, every byte goes through as it comes
std::function<bool(const std::string&)> usb_drop;
// The most the twin's timing fell behind the wall clock, ms: the twin itself, the reader that
// stamps what the tool sends (a stall there takes bytes late and together) or the tool's own
// sender (its line "sent up to N ms late", midiport.py's Clock: a late wake sends ticks
// together), whichever was worst
long lag = 0;
std::atomic<long> reader_stall{0}; // the reader's part of it, ms
long exec_late = 0; // Exec keeps the twin this far behind the wall clock, ms: a load's lag on purpose
// The tools on the twin's clock (lockstep, #388; ExecLockstep()), unless FRIZZ_TWIN_CLOCK=0: on
// the wall clock then, as before (ExecWall(); lag, Twice() and Unjudged() judge only there)
bool lockstep = !getenv("FRIZZ_TWIN_CLOCK") || strcmp(getenv("FRIZZ_TWIN_CLOCK"), "0") != 0;
const double kLockstepBase = 1000.; // s: the twin's ms 0 on the tools' clock (sitecustomize.py's BASE)
int kStuckS = 60;                   // s of the wall clock the twin's clock may stand still
long exec_twin_ms = 0;              // the last Exec's length, the twin's ms
double exec_wall_s = 0;             // and the wall clock's s
// Behind by more than this, the twin's timing isn't the wall clock's: a case that failed on
// timing alone ([timing], "failed on timing alone") isn't judged
const long kLagMs = 5;

/** The cases a tool's run in out failed on timing alone (its "failed on timing alone: ..."
 *  line, of the checks it tags [timing]), if the twin fell too far behind to judge timing */
__attribute__((unused)) std::vector<std::string> Unjudged(const std::string& out)
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

/** attempt() (a tool's run against the twin: true if it passed, false too if a case was left
 *  unjudged), and once more if it didn't with the twin more than kLagMs behind the wall clock:
 *  so far behind (hundreds of ms on a machine whose cores are all busy) its answers can come
 *  after the tool stopped waiting for them, beyond what Unjudged() covers. Said in a line FLAKY
 *  (all.sh lists them); a run that fails twice, or once with the twin on time, fails (#183) */
template <class Attempt>
void Twice(const std::string& name, Attempt attempt)
{
    if (attempt() || lag <= kLagMs)
        return;
    printf("FLAKY  %s: failed with the twin %ld ms behind the wall clock, run again\n", name.c_str(), lag);
    attempt();
}

/** After Twice(): the cases out still leaves unjudged (Unjudged()), each a line to fail by. One
 *  the twin's timing couldn't judge twice running isn't passed: a fault on timing could hide
 *  behind the load for good */
__attribute__((unused)) std::vector<std::string> NotJudged(const std::string& name, const std::string& out)
{
    std::vector<std::string> lines;
    for (const std::string& id : Unjudged(out))
        lines.push_back(name + ": " + id + " failed on timing alone twice, the twin " + std::to_string(lag)
                        + " ms behind: not judged, a fault on timing could hide behind the load");
    return lines;
}

/** A line: how long a tool's run (from t0) took, s of the wall clock, and of the twin's time on
 *  its clock, or how far it fell behind the wall clock's */
__attribute__((unused)) void Took(const std::string& name, std::chrono::steady_clock::time_point t0)
{
    const long s = long(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count());
    if (lockstep)
        printf("%s: %ld s, %ld s of the twin's time\n", name.c_str(), s, exec_twin_ms / 1000);
    else
        printf("%s: %ld s, the twin up to %ld ms behind\n", name.c_str(), s, lag);
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
// The jack's, apart: a byte every 0.32 ms at the most, as the cable carries them (31250 baud),
// so a burst the tool wrote at once comes over a while, not into one block (#356), and the USB
// bytes sent meanwhile don't wait behind it
std::deque<Arrival> trs_arrivals;
const double kTrsByteMs = .32;
std::atomic<bool> reading{false};

/** Reads what the suite sends as it comes, timed: a twin that fell behind (all.sh's load) still
 *  gets each byte at its time, not all that came meanwhile in one block, where a key's press and
 *  release would fall into one debounce */
void Reader(std::chrono::steady_clock::time_point start)
{
    double trs_free = 0.; // when the jack's line is free for the next byte, ms
    long last = 0;        // the last round's time, ms: a round takes about 1 ms (poll's wait)
    while (reading)
    {
        pollfd fds[2] = {{usb.fd, POLLIN, 0}, {trs.fd, POLLIN, 0}}; // one < 0: not polled
        const int ready = poll(fds, 2, 1);
        const long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (ms - last - 1 > reader_stall)
            reader_stall = ms - last - 1;
        last = ms;
        if (ready <= 0)
            continue;
        for (int p = 0; p < 2; p++)
        {
            if (fds[p].fd < 0)
                continue;
            uint8_t buf[256];
            ssize_t n;
            while ((n = read(fds[p].fd, buf, sizeof(buf))) > 0)
            {
                std::lock_guard<std::mutex> lock(arrivals_mutex);
                for (ssize_t i = 0; i < n; i++)
                    if (p == 0)
                        arrivals.push_back({ms, true, buf[i]});
                    else
                    {
                        trs_free = std::max(trs_free, static_cast<double>(ms)) + kTrsByteMs;
                        trs_arrivals.push_back({static_cast<long>(trs_free), false, buf[i]});
                    }
            }
        }
    }
}

/** A byte the tool sent over USB, into the twin: a SysEx message gathered to its F7 first while
 *  usb_drop is set, and lost if it says so */
void ToTwinUsb(uint8_t byte)
{
    static std::string message;
    if (!usb_drop)
    {
        UsbMidi(byte);
        return;
    }
    if (byte == 0xF0 || !message.empty())
        message.push_back(static_cast<char>(byte));
    else
        UsbMidi(byte); // between messages: a clock, a note
    if (byte != 0xF7 || message.empty())
        return;
    if (!usb_drop(message))
        for (char c : message)
            UsbMidi(static_cast<uint8_t>(c));
    message.clear();
}

/** All of data to the pty's far end: a short write's rest written too, waiting while its
 *  buffer is full (the far end reads it) */
void WriteAll(int fd, const std::string& data, const char* what)
{
    size_t done = 0;
    while (done < data.size())
    {
        const ssize_t n = write(fd, data.data() + done, data.size() - done);
        if (n > 0)
            done += size_t(n);
        else if (n < 0 && errno == EAGAIN)
        {
            pollfd p = {fd, POLLOUT, 0};
            if (poll(&p, 1, 1000) <= 0)
            {
                fprintf(stderr, "%s: full for 1 s, %zu bytes lost\n", what, data.size() - done);
                return;
            }
        }
        else
        {
            perror(what);
            return;
        }
    }
}

/** What the twin sent, out (USB; the jack as it's wired: what isn't is lost, as without a
 *  cable; with no TRS pty, the jack's MIDI out stays for the check to take) */
bool FromTwin()
{
    const std::string over_usb = TakeUsbOut();
    WriteAll(usb.fd, over_usb, "usb pty");
    if (trs.fd < 0)
        return !over_usb.empty();
    std::string jack;
    for (const MidiOutByte& b : TakeMidiOut())
        if (!b.usb)
            jack += static_cast<char>(b.byte);
    if (trs_out)
        WriteAll(trs.fd, jack, "trs pty");
    return !over_usb.empty() || (trs_out && !jack.empty());
}

/** What came by the twin's time ms, in; what the twin sent, out: true if it sent anything */
bool Pump(long ms)
{
    {
        std::lock_guard<std::mutex> lock(arrivals_mutex);
        while (!arrivals.empty() && arrivals.front().ms <= ms)
        {
            const Arrival a = arrivals.front();
            arrivals.pop_front();
            if (a.usb)
                ToTwinUsb(a.byte);
            else if (trs_in)
                Midi(a.byte);
        }
        while (!trs_arrivals.empty() && trs_arrivals.front().ms <= ms)
        {
            if (trs_in)
                Midi(trs_arrivals.front().byte);
            trs_arrivals.pop_front();
        }
    }
    return FromTwin();
}

/** Where a tool's stand-in and the twin share its audio (FRIZZ_TWIN_AUDIO), empty for none */
std::string audio_dir;

/** The twin's audio interface, in files in audio_dir, the time the tools' clock in s (Python's
 *  perf_counter: on the twin's clock kLockstepBase at its ms 0, else this computer's monotonic
 *  clock, steady_clock here), the twin's sample n at start + n / 48000:
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

/** The most a tool's sender said it was late in out ("sent up to N ms late", midiport.py's
 *  Clock), ms rounded up; 0 if it said nothing */
long SentLate(const std::string& out)
{
    const std::string tag = "sent up to ";
    long most = 0;
    for (size_t at = out.find(tag); at != std::string::npos; at = out.find(tag, at + 1))
    {
        char* end = nullptr;
        const double ms = strtod(out.c_str() + at + tag.size(), &end);
        if (end && !strncmp(end, " ms late", 8))
            most = std::max(most, long(std::ceil(ms)));
    }
    return most;
}

/** CMD (sh -c) started, with env's NAME=VALUE pairs set and the CHOMPI's lock shared (the twin
 *  isn't the CHOMPI), and those of more after them: its pid (-1 if not); *read gets the end of
 *  a pipe it prints into, stdout and stderr, not blocking */
pid_t Spawn(const std::string& cmd, const std::vector<std::pair<std::string, std::string>>& env,
            const std::vector<std::pair<std::string, std::string>>& more, int* read_fd)
{
    int pipe_fd[2];
    if (pipe(pipe_fd))
        return -1;
    const pid_t pid = fork();
    if (pid == 0)
    {
        dup2(pipe_fd[1], 1);
        dup2(pipe_fd[1], 2);
        close(pipe_fd[0]);
        setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
        setenv("FRIZZ_CHOMPI_HELD", "1", 1); // the twin, not the CHOMPI: no lock (tools/chompi.py)
        setenv("FRIZZ_NO_DEVICE", "1", 1);   // and never the CHOMPI, but on the ptys (pytool.h)
        signal(SIGINT, SIG_DFL); // a shell's background job (all.sh) ignores it, Python then too
        for (const auto& e : env)
            setenv(e.first.c_str(), e.second.c_str(), 1);
        for (const auto& e : more)
            setenv(e.first.c_str(), e.second.c_str(), 1);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(pipe_fd[1]);
    fcntl(pipe_fd[0], F_SETFL, O_NONBLOCK);
    *read_fd = pipe_fd[0];
    return pid;
}

/** Exec() on the wall clock (FRIZZ_TWIN_CLOCK=0): CMD against the twin at the wall clock's
 *  pace from the twin's time now until it's done (limit s at most). lag says how far the twin
 *  fell behind meanwhile; behind as the command ends, the twin catches up then, so everything
 *  it sent has arrived when this returns (#336) */
int ExecWall(const std::string& cmd, std::string& out, int limit,
             const std::vector<std::pair<std::string, std::string>>& env, const std::string& stop_at, int stop_signal)
{
    int pipe_fd[1];
    const pid_t pid = Spawn(cmd, env, {}, &pipe_fd[0]);
    if (pid < 0)
        return -1;
    out.clear();
    lag = 0;
    reader_stall = 0;
    const auto start = std::chrono::steady_clock::now();
    arrivals.clear();
    trs_arrivals.clear();
    reading = true;
    std::thread reader(Reader, start);
    const uint32_t twin_start = NowMs();
    Audio audio;
    if (!audio_dir.empty())
        audio.Open(std::chrono::duration<double>(start.time_since_epoch()).count());
    // one ms of the twin: what came by then in, its answers out
    const auto step = [&]()
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
    };
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
        if (behind > exec_late)
            step();
        else
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    // What the tool sent before it ended reaches the twin all the same: the reader's last bytes,
    // and the twin, as fast as it goes, up to the wall clock's time at the end, as a twin on time
    // was. A twin behind (all.sh's load) left the tool's last messages (play's key releases and
    // SWITCH 0 in its finally) undelivered, and the next Exec dropped them (#336)
    reading = false;
    reader.join();
    const long end = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    for (int p = 0; p < 2; p++)
    {
        const int fd = p == 0 ? usb.fd : trs.fd;
        uint8_t buf[256];
        ssize_t n;
        while (fd >= 0 && (n = read(fd, buf, sizeof(buf))) > 0)
        {
            std::lock_guard<std::mutex> lock(arrivals_mutex);
            for (ssize_t i = 0; i < n; i++)
                arrivals.push_back({end, p == 0, buf[i]});
        }
    }
    long last = end; // and the jack's bytes still on the line
    {
        std::lock_guard<std::mutex> lock(arrivals_mutex);
        if (!trs_arrivals.empty())
            last = std::max(last, trs_arrivals.back().ms);
    }
    while (long(NowMs() - twin_start) <= last)
        step();
    audio.Close();
    char buf[512];
    ssize_t n;
    while ((n = read(pipe_fd[0], buf, sizeof(buf))) > 0)
        out.append(buf, n);
    close(pipe_fd[0]);
    lag = std::max({lag, long(reader_stall), SentLate(out)});
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/** A process on the twin's clock, as its socket says (ExecLockstep()) */
struct Conn
{
    int fd;
    pid_t pid = 0;
    bool idle = false;     // every thread of it waits
    double deadline = -1;  // for the twin's time, ms (-1: none)
    bool io = false;       // a thread waits for the twin's bytes
    std::string buf;
};

/** Sends LINE (and a newline) to a process on the twin's clock */
void Say(int fd, const std::string& line)
{
    const std::string l = line + "\n";
    if (send(fd, l.data(), l.size(), MSG_NOSIGNAL) != ssize_t(l.size()))
        fprintf(stderr, "lockstep: couldn't tell a process %s\n", line.c_str());
}

/** What the tool wrote to the ptys, in, at the twin's time ms: the jack's a byte every
 *  kTrsByteMs. poll() first: it moves what the far end wrote over (the pty's flip buffer, a
 *  kernel worker's otherwise), so all of it is there once every process waits */
void TakeIn(long ms, double& trs_free)
{
    for (int p = 0; p < 2; p++)
    {
        const int fd = p == 0 ? usb.fd : trs.fd;
        if (fd < 0)
            continue;
        pollfd f = {fd, POLLIN, 0};
        poll(&f, 1, 0);
        uint8_t buf[256];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            for (ssize_t i = 0; i < n; i++)
                if (p == 0)
                    arrivals.push_back({ms, true, buf[i]});
                else
                {
                    trs_free = std::max(trs_free, static_cast<double>(ms)) + kTrsByteMs;
                    trs_arrivals.push_back({static_cast<long>(trs_free), false, buf[i]});
                }
    }
}

/** Whether process pid has ended (a zombie, or gone): its socket closes a little before */
bool Ended(pid_t pid)
{
    FILE* f = fopen(("/proc/" + std::to_string(pid) + "/stat").c_str(), "r");
    if (!f)
        return true;
    char state = 0;
    const int got = fscanf(f, "%*d (%*[^)]) %c", &state);
    fclose(f);
    return got != 1 || state == 'Z' || state == 'X';
}

/** Exec() on the twin's clock (lockstep, #388): CMD's Python processes keep their time by the
 *  twin's (test/lockstep/sitecustomize.py, on PYTHONPATH here, the socket in FRIZZ_LOCKSTEP),
 *  and the twin advances only while every thread of theirs waits: to the earliest time one
 *  waits for, or until it sends what one waits for, then wakes them, one process at a time.
 *  What they send comes in at the twin's time it was sent. limit is the twin's time, s; one that
 *  stands still for kStuckS of the wall clock (a thread that waits on something the twin can't
 *  bring) is stopped, said why, every thread's stack printed */
int ExecLockstep(const std::string& cmd, std::string& out, int limit,
                 const std::vector<std::pair<std::string, std::string>>& env, const std::string& stop_at,
                 int stop_signal)
{
    static int count = 0;
    const std::string name = "frizz-lockstep-" + std::to_string(getpid()) + "-" + std::to_string(count++);
    const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path + 1, name.data(), name.size()); // the abstract namespace: no file
    const socklen_t len = socklen_t(offsetof(sockaddr_un, sun_path) + 1 + name.size());
    if (listener < 0 || bind(listener, reinterpret_cast<sockaddr*>(&addr), len) || listen(listener, 16))
    {
        out = "lockstep: no socket";
        return -1;
    }
    const char* path = getenv("PYTHONPATH");
    std::vector<std::pair<std::string, std::string>> more = {
        {"FRIZZ_LOCKSTEP", "@" + name},
        {"FRIZZ_LOCKSTEP_USB", usb.path},
        {"FRIZZ_LOCKSTEP_EPOCH", std::to_string(time(nullptr))},
    };
    if (trs.fd >= 0)
        more.push_back({"FRIZZ_LOCKSTEP_TRS", trs.path});
    std::string python_path = firmware_dir + "/test/lockstep";
    for (const auto& e : env)
        if (e.first == "PYTHONPATH")
            path = e.second.c_str();
    if (path && *path)
        python_path += std::string(":") + path;
    more.push_back({"PYTHONPATH", python_path});
    int pipe_read = -1;
    out.clear();
    lag = 0;
    const pid_t pid = Spawn(cmd, env, more, &pipe_read);
    if (pid < 0)
    {
        close(listener);
        return -1;
    }
    arrivals.clear();
    trs_arrivals.clear();
    double trs_free = 0.;
    const uint32_t twin_start = NowMs();
    const auto ms = [&] { return long(NowMs() - twin_start); };
    Audio audio;
    if (!audio_dir.empty())
        audio.Open(kLockstepBase);
    // one ms of the twin: what came by then in, its answers out; true if it sent anything
    const auto step = [&]() {
        const long now = ms();
        const bool sent = Pump(now);
        if (audio_dir.empty())
            Run(2, nullptr, nullptr);
        else
            audio.Run2(now * long(kSampleRate / 1000));
        return sent;
    };
    const auto read_out = [&] {
        char buf[512];
        ssize_t n;
        while ((n = read(pipe_read, buf, sizeof(buf))) > 0)
            out.append(buf, n);
    };
    const auto wall_start = std::chrono::steady_clock::now();
    auto progress = wall_start;
    const auto wall_s = [](std::chrono::steady_clock::time_point since) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
    };
    std::vector<Conn> conns;
    std::set<pid_t> up;
    std::map<pid_t, int> child_waits; // a child's pid: who waits for its HELLO
    std::deque<int> round;            // the processes still to wake at the twin's time now
    int waking = -1;                  // the one woken, until it waits again
    int signalled = -1;               // the one stop_signal went to, until it says it runs
    auto signalled_at = wall_start;
    int status = -1;
    std::string stuck;
    // FRIZZ_LOCKSTEP_STATS: where the wall clock went, said after what it printed
    double waited_s = 0, advanced_s = 0;
    long advances = 0, wakes = 0;
    const auto find = [&](int fd) -> Conn* {
        for (Conn& c : conns)
            if (c.fd == fd)
                return &c;
        return nullptr;
    };
    const auto message = [&](Conn& c, const std::string& line) {
        std::istringstream words(line);
        std::string what;
        words >> what;
        if (what == "HELLO")
        {
            words >> c.pid;
            Say(c.fd, "TIME " + std::to_string(ms()));
            up.insert(c.pid);
            const auto w = child_waits.find(c.pid);
            if (w != child_waits.end())
            {
                if (find(w->second))
                    Say(w->second, "UP " + std::to_string(c.pid));
                child_waits.erase(w);
            }
        }
        else if (what == "IDLE")
        {
            std::string deadline;
            int io = 0;
            words >> deadline >> io;
            c.idle = true;
            c.deadline = deadline == "-" ? -1 : std::max(0., atof(deadline.c_str()));
            c.io = io != 0;
            if (waking == c.fd)
                waking = -1;
        }
        else if (what == "BUSY")
            c.idle = false;
        else if (what == "CHILD")
        {
            pid_t child = 0;
            words >> child;
            if (up.count(child))
                Say(c.fd, "UP " + std::to_string(child));
            else
                child_waits[child] = c.fd;
        }
        if (signalled == c.fd)
            signalled = -1;
    };
    while (true)
    {
        read_out();
        if (waitpid(pid, &status, WNOHANG) == pid)
            break;
        // the processes: new ones, what they say, the ones that ended
        std::vector<pollfd> fds = {{listener, POLLIN, 0}, {pipe_read, POLLIN, 0}};
        for (const Conn& c : conns)
            fds.push_back({c.fd, POLLIN, 0});
        const auto all_idle = [&] {
            return !conns.empty() && waking < 0 && round.empty() && signalled < 0
                   && std::all_of(conns.begin(), conns.end(), [](const Conn& c) { return c.idle; });
        };
        const auto polled = std::chrono::steady_clock::now();
        poll(fds.data(), fds.size(), all_idle() || (waking < 0 && !round.empty()) ? 0 : 5);
        waited_s += wall_s(polled);
        int fd;
        while ((fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC)) >= 0)
        {
            conns.push_back({fd});
            progress = std::chrono::steady_clock::now();
        }
        for (size_t i = 2; i < fds.size(); i++)
        {
            if (!fds[i].revents)
                continue;
            Conn* c = find(fds[i].fd);
            char buf[512];
            const ssize_t n = recv(fds[i].fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (n < 0 && errno == EAGAIN)
                continue;
            progress = std::chrono::steady_clock::now();
            if (n <= 0)
            {
                // ended: the twin goes on once it has, as what it did last is done then
                for (int k = 0; k < 1000 && c->pid && !Ended(c->pid); k++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (waking == c->fd)
                    waking = -1;
                if (signalled == c->fd)
                    signalled = -1;
                close(c->fd);
                conns.erase(conns.begin() + (c - conns.data()));
                continue;
            }
            c->buf.append(buf, n);
            for (size_t nl; (nl = c->buf.find('\n')) != std::string::npos;)
            {
                const std::string line = c->buf.substr(0, nl);
                c->buf.erase(0, nl + 1);
                message(*c, line);
            }
        }
        if (signalled >= 0 && wall_s(signalled_at) > 2)
            signalled = -1; // it took the signal without a word: as it was
        // the next process woken at the twin's time now, after the one before waits again
        if (waking < 0 && !round.empty())
        {
            Conn* c = find(round.front());
            round.pop_front();
            if (c)
            {
                c->idle = false;
                waking = c->fd;
                wakes++;
                Say(c->fd, "NOW " + std::to_string(ms()));
            }
            continue;
        }
        if (!all_idle())
        {
            if (wall_s(progress) > kStuckS)
            {
                stuck = "(lockstep: stuck for " + std::to_string(kStuckS) + " s of the wall clock at the twin's "
                        + std::to_string(ms()) + " ms:";
                for (const Conn& c : conns)
                    stuck += " pid " + std::to_string(c.pid) + (c.idle ? " waits" : " runs");
                stuck += conns.empty() ? " no process on the twin's clock)" : ")";
                for (const Conn& c : conns)
                    if (c.pid)
                        kill(c.pid, SIGUSR1); // its threads' stacks (faulthandler)
                std::this_thread::sleep_for(std::chrono::seconds(1));
                read_out();
                break;
            }
            continue;
        }
        // every thread waits: what they sent in, then the twin on
        TakeIn(ms(), trs_free);
        read_out();
        if (stop_signal && out.find(stop_at) != std::string::npos)
        {
            kill(pid, stop_signal);
            stop_signal = 0;
            for (const Conn& c : conns)
                if (c.pid == pid)
                    signalled = c.fd;
            signalled_at = std::chrono::steady_clock::now();
            continue;
        }
        if (!audio_dir.empty())
            audio.Look();
        if (ms() >= long(limit) * 1000)
        {
            stuck = "(timed out)";
            break;
        }
        // to the earliest time a thread waits for, or the twin's first byte out if one waits
        // for that; 100 ms at a time if none does either (the limit ends it)
        double next = -1;
        bool io = false;
        for (const Conn& c : conns)
        {
            if (c.deadline >= 0 && (next < 0 || c.deadline < next))
                next = c.deadline;
            io = io || c.io;
        }
        const long goal = next >= 0 ? std::max(ms() + 1, long(std::ceil(next - 1e-6))) : ms() + (io ? long(limit) * 1000 : 100);
        advances++;
        const auto advancing = std::chrono::steady_clock::now();
        while (true)
        {
            const bool sent = step();
            if ((sent && io) || ms() >= goal || ms() >= long(limit) * 1000)
                break;
        }
        progress = std::chrono::steady_clock::now();
        advanced_s += wall_s(advancing);
        for (const Conn& c : conns)
            round.push_back(c.fd);
    }
    if (!stuck.empty())
    {
        kill(pid, SIGKILL);
        for (const Conn& c : conns)
            if (c.pid)
                kill(c.pid, SIGKILL);
        waitpid(pid, &status, 0);
        read_out();
        out += stuck;
    }
    for (const Conn& c : conns)
        close(c.fd); // an orphan still on the twin's clock ends (sitecustomize: the clock is gone)
    close(listener);
    // what it sent last (play's key releases and SWITCH 0 in its finally) reaches the twin, and
    // the jack's bytes still on the line (#336)
    TakeIn(ms(), trs_free);
    long last = ms();
    if (!trs_arrivals.empty())
        last = std::max(last, trs_arrivals.back().ms);
    while (ms() <= last)
        step();
    audio.Close();
    read_out();
    close(pipe_read);
    exec_twin_ms = ms();
    exec_wall_s = wall_s(wall_start);
    if (getenv("FRIZZ_LOCKSTEP_STATS"))
        out += "\n(lockstep: the twin's " + std::to_string(ms()) + " ms in " + std::to_string(advances) + " advances, "
               + std::to_string(advanced_s) + " s; " + std::to_string(wakes) + " wakes; " + std::to_string(waited_s)
               + " s waited for the processes)\n";
    return !stuck.empty() || !WIFEXITED(status) ? -1 : WEXITSTATUS(status);
}

/** CMD (sh -c) against the twin, its ptys open, from the twin's time now until it's done (limit
 *  s at most), with env's NAME=VALUE pairs set and the CHOMPI's lock shared (the twin isn't the
 *  CHOMPI): its exit code; out gets what it printed, "(timed out)" after it if it was stopped
 *  so. stop_signal (SIGINT, SIGTERM) goes to it once it has printed stop_at. On the twin's
 *  clock (ExecLockstep()), or the wall clock's (ExecWall()) with FRIZZ_TWIN_CLOCK=0; either
 *  way, everything it sent has arrived when this returns */
int Exec(const std::string& cmd, std::string& out, int limit,
         const std::vector<std::pair<std::string, std::string>>& env = {}, const std::string& stop_at = "",
         int stop_signal = 0)
{
    return lockstep ? ExecLockstep(cmd, out, limit, env, stop_at, stop_signal)
                    : ExecWall(cmd, out, limit, env, stop_at, stop_signal);
}

/** tools/TOOL ARGS (--device and --trs given) against the twin, at the wall clock's pace until
 *  it's done (limit s at most), with env's NAME=VALUE pairs set: its exit code; out gets what
 *  it printed, and how far the twin fell behind. stop_signal (SIGINT, SIGTERM) goes to it once
 *  it has printed stop_at */
__attribute__((unused)) int Tool(const std::string& tool, const std::string& args, std::string& out, int limit,
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
    const std::string cmd = "exec python3 '" + firmware_dir + "/tools/" + tool + "' --device '" + usb.path
                            + "' --trs '" + trs.path + "' " + args;
    const int rc = Exec(cmd, out, limit, env, stop_at, stop_signal);
    if (lockstep)
        out += "\n(on the twin's clock: " + std::to_string(exec_twin_ms / 1000) + " s of its time in "
               + std::to_string(long(exec_wall_s)) + " s of the wall clock's)\n";
    else
        out += "\n(the twin was " + std::to_string(lag) + " ms behind the wall clock at most)\n";
    return rc;
}
} // namespace
