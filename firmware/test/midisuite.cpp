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
#include <sstream>
#include "cases.h"
#include "check.h"
#include "twin.h"
#include "ptytwin.h"

namespace
{
/** midisuite.py ARGS against the twin (ptytwin.h) */
int Suite(const std::string& args, std::string& out, int limit, const std::string& stop_at = "", int stop_signal = 0)
{
    return Tool("midisuite.py", args, out, limit, {}, stop_at, stop_signal);
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
    const auto card = CardFiles();
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
    Check(CardFiles() == card, (name + ": the card as it was (PERSIST 0)").c_str());
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
    // stopped from outside (Ctrl-C) in a case: things put back, the summary, exit 130, not 0
    // (#171)
    cases.push_back({"interrupted", [] {
        trs_in = trs_out = true;
        std::string out;
        const int rc = Suite("--quick --wiring both --cases out-idle,out-pass", out, 55, "-- out-idle", SIGINT);
        Check(rc == 130 && Has(out, "interrupted") && !Has(out, "putting things back:") && !Has(out, "not put back")
                  && Has(out, "### MIDI suite") && !Has(out, "-- out-pass"),
              "interrupted: Ctrl-C in a case stops the run, things put back, exit 130");
        if (rc != 130)
            printf("exit %d\n%s\n", rc, out.c_str());
    }});
    // the processes mostly wait on the wall clock: all at once, not a core each
    setenv("CASE_JOBS", "16", 0);
    RunAllCases();
    return Finish();
}
