// reaper.cpp: firmware/tools/reaper.py against the virtual CHOMPI (ptytwin.h, as midisuite.cpp
// does midisuite.py), with test/reaper_stub.py in REAPER's place (FRIZZ_REAPER): the stand-in
// does what tools/reaper.lua does in REAPER, and sends REAPER's clock, transport and lanes as
// REAPER does on the device. It checks the tool's logic: the three parts (on the node, through
// "REAPER" once per audio mode, on the node again), the commands, FRIZZ's answers through
// REAPER's input, the cases on the firmware as it is, and the summary with the jitter's table;
// not REAPER, nor the firmware's timing (--quick widens the tolerances, as midisuite.cpp's).
//
// REAPERCHECK="ARGS" runs the tool once with those arguments and prints what it printed, e.g.
//     REAPERCHECK="--cases daw-transport -v" ./unit.sh reaper
#include "cases.h"
#include "check.h"
#include "twin.h"
#include "ptytwin.h"

namespace
{
bool Has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

int Reaper(const std::string& args, std::string& out, int limit)
{
    return Tool("reaper.py", "--quick " + args, out, limit,
                {{"FRIZZ_REAPER", firmware_dir + "/test/reaper_stub.py"}});
}

/** The tool on ids with the jack wired out (A) or not at all: each case's line, the summary's */
void Run(const std::string& name, bool out_wired, const std::string& ids, const std::vector<std::string>& lines,
         int exit_code)
{
    trs_in = false;
    trs_out = out_wired;
    std::string out;
    const auto t0 = std::chrono::steady_clock::now();
    const int rc = Reaper("-v --cases " + ids, out, 170);
    printf("%s: %ld s\n", name.c_str(),
           long(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count()));
    bool ok = rc == exit_code;
    Check(ok, (name + ": exit " + std::to_string(rc) + ", not " + std::to_string(exit_code)).c_str());
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
    if (const char* args = getenv("REAPERCHECK"))
    {
        Boot();
        std::string out;
        trs_in = false;
        trs_out = true;
        const int rc = Reaper(args, out, 3600);
        printf("%s\nexit %d\n", out.c_str(), rc);
        return rc;
    }
    // wiring A, as on the device: every case, the jitter in both audio modes, REAPER's CC reset
    // as it comes in the second
    cases.push_back({"tempo", [] {
        Run("tempo", true, "daw-tempo",
            {"wiring: CHOMPI TRS out -> interface MIDI in (A) (found)", "PASS daw-tempo",
             "REAPER jack (Stub 48000 512"},
            0);
    }});
    cases.push_back({"automation", [] {
        Run("automation", true, "daw-automation", {"PASS daw-automation", "| `daw-automation` |", "the knobs stay: all"}, 0);
    }});
    cases.push_back({"transport", [] {
        Run("transport", true, "daw-transport", {"PASS daw-transport", "wraps)"}, 0);
    }});
    cases.push_back({"monitor", [] {
        Run("monitor", true, "daw-monitor", {"PASS daw-monitor"}, 0);
    }});
    cases.push_back({"jitter", [] {
        Run("jitter", true, "daw-jitter,daw-ccreset --seconds 2",
            {"PASS daw-jitter", "PASS daw-ccreset", "dummy (Stub 48000 512; its script", "REAPER dummy (Stub", "**The clock's jitter (#83)**",
             "| this computer (midiport.Clock) | 200 |", "| REAPER, jack (Stub 48000 512) | 300 |",
             "| REAPER, dummy (Stub 48000 512) | 300 |"},
            0);
    }});
    // no TRS: the jitter can't be seen, a SKIP
    cases.push_back({"no-trs", [] {
        Run("no TRS", false, "daw-jitter", {"SKIP daw-jitter", "needs the CHOMPI's TRS out"}, 0);
    }});
    setenv("CASE_JOBS", "8", 0);
    RunAllCases();
    return Finish();
}
