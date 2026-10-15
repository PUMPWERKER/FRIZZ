// reaper.cpp: firmware/tools/reaper.py against the virtual CHOMPI (ptytwin.h, as midisuite.cpp
// does midisuite.py), with test/reaper_stub.py in REAPER's place (FRIZZ_REAPER): the stand-in
// does what tools/reaper.lua does in REAPER, and sends REAPER's clock, transport and lanes as
// REAPER does on the device. It checks the tool's logic: the three parts (on the node, through
// "REAPER" once per audio mode, on the node again), the commands, FRIZZ's answers through
// REAPER's input, the cases on the firmware as it is, REAPER synchronized to FRIZZ's clock (in a
// REAPER of its own), and the summary with the jitter's table; and the cases with music: the
// stand-in plays a generated song into the twin's AUX and records its line out (ptytwin.h's
// Audio), so the tool's analysis runs on the firmware's own loop. Those need numpy, as the tool
// does: with ~/.venvs/frizz (measure.py's), else they're a SKIP, said so.
// Not REAPER, nor the firmware's timing (--quick widens the tolerances, as midisuite.cpp's).
//
// REAPERCHECK="ARGS" runs the tool once with those arguments and prints what it printed, e.g.
//     REAPERCHECK="--cases daw-transport -v" ./unit.sh reaper
#include "cases.h"
#include "check.h"
#include "twin.h"
#include "ptytwin.h"

namespace
{

// measure.py's venv, with numpy: the audio cases' python
const std::string kVenv = std::string(getenv("HOME") ? getenv("HOME") : "") + "/.venvs/frizz/bin";

bool HasVenv() { return access((kVenv + "/python3").c_str(), X_OK) == 0; }

int Reaper(const std::string& args, std::string& out, int limit,
           std::vector<std::pair<std::string, std::string>> env = {})
{
    env.push_back({"FRIZZ_REAPER", firmware_dir + "/test/reaper_stub.py"});
    return Tool("reaper.py", "--quick " + args, out, limit, env);
}

/** The tool on ids with the jack wired out (A) or not at all: each case's line, the summary's.
 *  A run that failed with the twin behind, or left a case unjudged (failed on timing alone so
 *  behind, ptytwin.h's Unjudged()), runs once more (Twice()); unjudged again, it fails
 *  (NotJudged()) */
void Run(const std::string& name, bool out_wired, const std::string& ids, const std::vector<std::string>& lines,
         int exit_code, const std::vector<std::pair<std::string, std::string>>& env = {}, int limit = 170)
{
    trs_in = false;
    trs_out = out_wired;
    std::string out;
    int rc = 0;
    std::vector<std::string> unjudged;
    // the exit code as wanted, or 1 for cases only failed on timing the twin couldn't keep
    auto exited = [&] {
        bool judged_fail = false;
        for (size_t at = out.find("\nFAIL "); at != std::string::npos; at = out.find("\nFAIL ", at + 1))
        {
            const std::string id = out.substr(at + 6, out.find_first_of(" :\n", at + 6) - at - 6);
            judged_fail = judged_fail || std::find(unjudged.begin(), unjudged.end(), id) == unjudged.end();
        }
        return rc == exit_code
               || (rc == 1 && !unjudged.empty() && !judged_fail && !Has(out, "not put back")
                   && !Has(out, "putting things back:"));
    };
    // a line wanted: one that says a case not judged passed isn't (a summary's row names none)
    auto wanted = [&](const std::string& line) {
        bool of_unjudged = !Has(line, "daw-");
        for (const std::string& id : unjudged)
            of_unjudged = of_unjudged || Has(line, id);
        return unjudged.empty() || !Has(line, "PASS") || !of_unjudged;
    };
    auto says = [&] {
        for (const std::string& line : lines)
            if (wanted(line) && !Has(out, line))
                return false;
        return true;
    };
    Twice(name, [&] {
        const auto t0 = std::chrono::steady_clock::now();
        rc = Reaper("-v --cases " + ids, out, limit, env);
        printf("%s: %ld s, the twin up to %ld ms behind\n", name.c_str(),
               long(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count()), lag);
        unjudged = Unjudged(out);
        return exited() && says() && unjudged.empty();
    });
    for (const std::string& line : NotJudged(name, out))
        Check(false, line.c_str());
    Check(exited(), (name + ": exit " + std::to_string(rc) + ", not " + std::to_string(exit_code)).c_str());
    for (const std::string& line : lines)
        if (wanted(line))
            Check(Has(out, line), (name + ": says \"" + line + "\"").c_str());
    if (!exited() || !says() || failures)
        printf("%s\n", out.c_str());
}
/** A case with music (sound=True) on the twin's audio, a generated song: with the venv the
 *  lines wanted, else its SKIP for numpy */
void RunAudio(const std::string& name, const std::string& id, const std::vector<std::string>& lines)
{
    char dir[] = "/tmp/frizz-twin-audio-XXXXXX";
    if (!mkdtemp(dir))
    {
        Check(false, (name + ": a temporary directory").c_str());
        return;
    }
    audio_dir = dir;
    std::vector<std::pair<std::string, std::string>> env = {{"FRIZZ_TWIN_AUDIO", audio_dir}};
    if (HasVenv())
    {
        env.push_back({"PATH", kVenv + ":" + std::string(getenv("PATH") ? getenv("PATH") : "")});
        Run(name, true, id + " --material generated", lines, 0, env, 300);
    }
    else
    {
        // a SKIP line all.sh lists under the check: stage 1b, for a machine with the venv
        printf("SKIP: local only (numpy in %s): %s checks its SKIP instead\n", kVenv.c_str(), name.c_str());
        Run(name, true, id + " --material generated", {"SKIP " + id, "needs numpy"}, 0, env);
    }
    if (system(("rm -rf '" + audio_dir + "'").c_str()) != 0)
        printf("%s: %s left behind\n", name.c_str(), dir);
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
        // the twin's audio too, and numpy where the venv is, for the cases with music
        char dir[] = "/tmp/frizz-twin-audio-XXXXXX";
        std::vector<std::pair<std::string, std::string>> env;
        if (mkdtemp(dir))
        {
            audio_dir = dir;
            env.push_back({"FRIZZ_TWIN_AUDIO", audio_dir});
        }
        if (HasVenv())
            env.push_back({"PATH", kVenv + ":" + std::string(getenv("PATH") ? getenv("PATH") : "")});
        const int rc = Reaper(args, out, 3600, env);
        printf("%s\nexit %d\n", out.c_str(), rc);
        if (!audio_dir.empty() && system(("rm -rf '" + audio_dir + "'").c_str()) != 0)
            printf("%s left behind\n", dir);
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
    cases.push_back({"scenes", [] {
        Run("scenes", true, "daw-scenes", {"PASS daw-scenes", "| `daw-scenes` |"}, 0);
    }});
    cases.push_back({"transport", [] {
        Run("transport", true, "daw-transport", {"PASS daw-transport", "wraps)"}, 0);
    }});
    cases.push_back({"monitor", [] {
        Run("monitor", true, "daw-monitor", {"PASS daw-monitor"}, 0);
    }});
    cases.push_back({"input", [] {
        Run("input", true, "daw-input", {"PASS daw-input"}, 0);
    }});
    cases.push_back({"jitter", [] {
        Run("jitter", true, "daw-jitter,daw-ccreset --seconds 2",
            {"PASS daw-jitter", "PASS daw-ccreset", "dummy (Stub 48000 512; its script", "REAPER dummy (Stub", "**The clock's jitter (#83)**",
             "| this computer (midiport.Clock) | 200 |", "| REAPER, jack (Stub 48000 512) | 300 |",
             "| REAPER, dummy (Stub 48000 512) | 300 |"},
            0);
    }});
    // REAPER synchronized to FRIZZ's clock out, in a REAPER of its own (#150)
    cases.push_back({"follows", [] {
        Run("follows", true, "daw-follows",
            {"PASS daw-follows", "REAPER jack (Stub 48000 512", "synchronized to its MIDI input: projsmptesync 5, "
             "projsmpteinput 4223; midiouts_clock=0", "| jack, synced | PASS |", "on its beat 1"},
            0);
    }});
    // music from "REAPER" into the twin's AUX, its line out recorded (ptytwin.h's Audio): a loop
    // recorded from the song, against it bar by bar; restarted by Start, Song Position and a loop
    // range, at x1 and x1/2; REAPER synchronized to it through a standstill (#174)
    cases.push_back({"loop-audio", [] {
        RunAudio("loop-audio", "daw-loop-audio",
                 {"PASS daw-loop-audio", "REAPER jack (Stub 48000 512", "its audio out 3/4 into AUX",
                  "the loop holds the song's bars 2-3 (recorded from its bar 2): at the song's bar 4: WAV bar 2 at",
                  "| `daw-loop-audio` |"});
    }});
    cases.push_back({"loop-restart", [] {
        RunAudio("loop-restart", "daw-loop-restart",
                 {"PASS daw-loop-restart", "a loop range of bars 1-3, three turns (clock factor x1/2): each turn steady"});
    }});
    cases.push_back({"follows-audio", [] {
        RunAudio("follows-audio", "daw-follows-audio",
                 {"PASS daw-follows-audio", "after the standstill where it was before"});
    }});
    // stopped from outside (SIGTERM) while REAPER runs a case: not taken for the case's FAIL,
    // REAPER closed, things put back, exit 143 (#171)
    cases.push_back({"sigterm", [] {
        trs_in = false;
        trs_out = true;
        std::string out;
        const auto card = CardFiles();
        const int rc = Tool("reaper.py", "--quick -v --cases daw-tempo,daw-transport", out, 170,
                            {{"FRIZZ_REAPER", firmware_dir + "/test/reaper_stub.py"}}, "-- daw-tempo", SIGTERM);
        Check(rc == 143 && Has(out, "stopped (signal 15)") && !Has(out, "FAIL daw-tempo") && !Has(out, "-- daw-transport")
                  && !Has(out, "putting things back:") && !Has(out, "not put back") && Has(out, "### REAPER"),
              "sigterm: SIGTERM in a case stops the run, things put back, exit 143");
        Check(CardFiles() == card, "sigterm: the card as it was (PERSIST 0)");
        if (rc != 143)
            printf("exit %d\n%s\n", rc, out.c_str());
    }});
    // no TRS: the jitter can't be seen, a SKIP
    cases.push_back({"no-trs", [] {
        Run("no TRS", false, "daw-jitter", {"SKIP daw-jitter", "needs the CHOMPI's TRS out"}, 0);
    }});
    // the processes mostly wait on the wall clock: all at once
    setenv("CASE_JOBS", "6", 0);
    RunAllCases();
    return Finish();
}
