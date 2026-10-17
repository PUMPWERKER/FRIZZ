// midisuite.cpp: firmware/tools/midisuite.py against the virtual CHOMPI (twin/twin.h), as
// remote.cpp does remote.py's: the twin's USB MIDI and its TRS jack each on a pseudo-terminal,
// in place of the CHOMPI's ALSA node and the audio interface's, the suite on the twin's clock
// (ptytwin.h's lockstep). The jack is wired one way, the other or both, as the TRS adapter can be: the suite has
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


/** A case's line in out: "PASS out-loop (7 s)", "SKIP clock-trs: ..." */
bool Line(const std::string& out, const std::string& result, const std::string& id)
{
    for (const char* end : {" (", ":", "\n"})
        if (Has(out, "\n" + result + " " + id + end) || out.compare(0, result.size() + id.size() + 2, result + " " + id + end) == 0)
            return true;
    return false;
}

/** Those of ids without the result want in out's lines that didn't fail on timing the twin
 *  couldn't keep, each after a space */
std::string Missing(const std::string& out, const std::vector<std::string>& ids, const std::string& want)
{
    const std::vector<std::string> unjudged = Unjudged(out);
    std::string missing;
    for (const std::string& id : ids)
        if (!Line(out, want, id)
            && !(want == "PASS" && std::find(unjudged.begin(), unjudged.end(), id) != unjudged.end()))
            missing += " " + id;
    return missing;
}

/** Each of ids has the result want in out's lines, or failed on timing the twin couldn't keep */
void Expect(const std::string& out, const std::vector<std::string>& ids, const std::string& want,
            const std::string& what)
{
    const std::string missing = Missing(out, ids, want);
    Check(missing.empty(), (what + (missing.empty() ? "" : ":" + missing + " not " + want)).c_str());
}

/** The suite on CASES with the jack wired IN / OUT, found by itself: each case's result want,
 *  the summary's lines in it. Run once more if that failed with the twin behind, or left a case
 *  unjudged (ptytwin.h's Twice()); unjudged again, it fails (NotJudged()) */
void Run(const std::string& name, bool in, bool out_wired, const std::vector<std::string>& ids,
         const std::string& want, const std::vector<std::string>& lines, int exit_code)
{
    trs_in = in;
    trs_out = out_wired;
    std::string list;
    for (const std::string& id : ids)
        list += (list.empty() ? "" : ",") + id;
    std::string out;
    int rc = 0;
    const auto card = CardFiles();
    // the exit code as wanted, or 1 for cases only failed on timing the twin couldn't keep
    auto exited = [&] {
        return rc == exit_code || (rc == 1 && !Unjudged(out).empty() && !Has(out, "error:") && !Has(out, "stopped:"));
    };
    auto says = [&] {
        for (const std::string& line : lines)
            if (!Has(out, line))
                return false;
        return true;
    };
    Twice(name, [&] {
        const auto t0 = std::chrono::steady_clock::now();
        rc = Suite("--quick -v --cases " + list, out, 55);
        Took(name, t0);
        return exited() && Missing(out, ids, want).empty() && says() && Unjudged(out).empty();
    });
    for (const std::string& line : NotJudged(name, out))
        Check(false, line.c_str());
    Check(exited(), (name + ": exit " + std::to_string(rc) + ", not " + std::to_string(exit_code)).c_str());
    Expect(out, ids, want, name);
    Check(CardFiles() == card, (name + ": the card as it was (PERSIST 0)").c_str());
    for (const std::string& line : lines)
        Check(Has(out, line), (name + ": says \"" + line + "\"").c_str());
    if (!exited() || !says() || failures)
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
        {"start-spp"}, {"factor"}, {"tick-tail"}, {"out-idle", "out-loop", "out-pass"}, {"out-factor", "out-knobs", "out-spp"}, {"out-fin-delay"},
        {"loop-drift"}, {"out-x2"}, {"cc-channels", "rpn"},
        {"cc-global", "cc-global-trs", "block-trs", "cc-burst-trs", "shifter-page", "chaos-cc"}, {"morph-back", "morph-hold"}, {"morph-sequences"},
        {"feedback", "no-double", "sysex-thru", "sysex-cut"}, {"input-cc"}};
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
        const std::vector<std::string> ids = {"out-pass", "cc-global"};
        const std::string says = "USB clock 120 passed on out of TRS";
        Twice("wiring A, out-pass", [&] {
            Suite("--quick -v --cases out-pass,cc-global", out, 55);
            return Missing(out, ids, "PASS").empty() && Has(out, says) && Unjudged(out).empty();
        });
        Expect(out, ids, "PASS", "wiring A: a clock from USB passed on out of TRS");
        Check(Has(out, says), "wiring A: says so");
        for (const std::string& line : NotJudged("wiring A, out-pass", out))
            Check(false, line.c_str());
        if (!Missing(out, ids, "PASS").empty() || !Has(out, says) || !Unjudged(out).empty())
            printf("%s\n", out.c_str());
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
    // trs-relisten needs the bench build's TEST (kUartError): on FRIZZ.bin a SKIP that says so
    // (#361)
    cases.push_back({"trs-relisten", [] {
        Run("trs-relisten", true, true, {"trs-relisten"}, "SKIP", {"FRIZZ-bench.bin only"}, 0);
    }});
    // trs-restart-tx too, its TEST kUartRestart (#442)
    cases.push_back({"trs-restart-tx", [] {
        Run("trs-restart-tx", true, true, {"trs-restart-tx"}, "SKIP", {"FRIZZ-bench.bin from #442 on"}, 0);
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
    // out-fin-delay's spans (span_offsets, #386): FRIZZ's clock 200 ppm off this computer's,
    // jitter, and the capture's own trend within the first span. With Loud back at 0 the last
    // span is where the first was, within the case's 0.5 ms (a period from span 0 alone put it
    // 0.3-0.5 ms off); one left 1 ms late is caught; the delay reads 1 ms
    cases.push_back({"fin-delay-fit", [] {
        char dir[] = "/tmp/frizz-fin-delay-XXXXXX";
        if (!mkdtemp(dir))
        {
            Check(false, "fin-delay-fit: a temporary directory");
            return;
        }
        tmp_dir = dir;
        std::string out;
        const int rc = Python(R"(
import random, midisuite
P = 60. / (120 * 24) * (1 + 200e-6)
def spans(delays, seed):
    rng = random.Random(seed)
    at = []
    for n in range(700):
        t = n * P
        at.append(t + delays[0 if t < 5 else 1 if t < 10 else 2] + (.0001 - .00005 * t if t < 5 else 0.)
                  + rng.gauss(0, .0002))
    return [[t for t in at if a <= t < b] for a, b in ((.5, 4.5), (5.5, 9.5), (10.5, 14.))]
for seed in range(5):
    _, (a, d, z) = midisuite.span_offsets(spans((0, .001, 0), seed))
    _, (_, _, stuck) = midisuite.span_offsets(spans((0, .001, .001), seed))
    print("delay %.2f back %.2f stuck %.2f" % ((d - (a + z) / 2) * 1000, z * 1000, stuck * 1000),
          abs(d - (a + z) / 2 - .001) < .0003 and abs(z) < .0005 and stuck > .0005)
)", out);
        std::system(("rm -rf '" + tmp_dir + "'").c_str());
        Check(rc == 0 && Has(out, "True") && !Has(out, "False"),
              "fin-delay-fit: Loud back at 0 is where it was through drift and a wander, one left 1 ms late isn't");
        if (rc != 0 || Has(out, "False"))
            printf("%s\n", out.c_str());
    }});
    // judging under load on the wall clock (FRIZZ_TWIN_CLOCK=0, ptytwin.h's ExecWall()): lag
    // counts the tool's own clock sent late too (midiport.py's Clock, a late wake: the ticks
    // behind it close), as its line says; a case left unjudged twice fails, naming it
    cases.push_back({"judging", [] {
        std::string out;
        lockstep = false;
        usb.Open();
        Exec("python3 - <<'EOF'\n"
             "import time, midiport\n"
             "n = [0]\n"
             "def write(data):\n"
             "    n[0] += 1\n"
             "    if n[0] == 3:\n"
             "        time.sleep(.05)  # the 4th tick ~29 ms late\n"
             "c = midiport.Clock(write, 120)\n"
             "time.sleep(.3)\n"
             "c.close()\n"
             "print('the clock from here: sent up to %.1f ms late' % (midiport.LATE[0] * 1000))\n"
             "EOF\n",
             out, 10, {{"PYTHONPATH", firmware_dir + "/tools"}});
        Check(lag >= 25 && lag < 100 && Has(out, "sent up to "), "judging: a tick the tool's clock sent late counts as the twin behind");
        if (lag < 25 || lag >= 100)
            printf("lag %ld ms\n%s\n", lag, out.c_str());
        lag = kLagMs + 1;
        const std::vector<std::string> lines = NotJudged("both ways, tick-tail", "failed on timing alone: tick-tail\n");
        Check(lines.size() == 1 && Has(lines[0], "both ways, tick-tail: tick-tail failed on timing alone twice"),
              "judging: a case left unjudged after its second run fails, by name");
        lag = kLagMs;
        Check(NotJudged("both ways, tick-tail", "failed on timing alone: tick-tail\n").empty(),
              "judging: with the twin on time it was judged (its FAIL is the case's)");
    }});
    // on the wall clock the processes mostly wait: all at once; on the twin's, a core each
    if (!lockstep)
        setenv("CASE_JOBS", "16", 0);
    RunAllCases();
    return Finish();
}
