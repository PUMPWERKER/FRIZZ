// compare.cpp: what ../twin/compare.sh relies on to say a scenario is bit-identical or not.
// compare.py's own check of its LED comparison (--selftest: logs of different lengths and a line
// cut short are a difference, said as such, never a crash), and frizz-twin failing a run whose
// WAV or LED log it couldn't write whole (exit 4: a full /tmp, where compare.sh's runs go), so a
// log cut short never reaches the comparison; that both compare.py exit 2 on what they can't
// compare (the twin's saying the scenario and comparing the next all the same), not the 1 of a
// difference; and CI's guard on them (.github/scripts/against-base.sh). Run by unit.sh compare;
// needs python3.
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include "check.h"
#include "twin.h" // builds the twin: frizz-twin is what this checks, beside libtwin.a

/** Runs cmd in the shell, stdout and stderr into out; its exit code (-1 for a signal) */
static int Run(const std::string& cmd, std::string& out)
{
    out.clear();
    FILE* p = popen((cmd + " 2>&1").c_str(), "r");
    char buf[512];
    size_t n;
    while (p && (n = fread(buf, 1, sizeof(buf), p)) > 0)
        out.append(buf, n);
    const int status = p ? pclose(p) : -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static bool Has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

int main()
{
    const std::string firmware = std::string(__FILE__).substr(0, std::string(__FILE__).rfind('/')) + "/..";
    const char* build = getenv("TWIN_BUILD");
    const std::string twin = (build && *build ? std::string(build) : firmware + "/twin/build") + "/frizz-twin";
    char dir[] = "/tmp/frizz-compare-XXXXXX";
    if (!mkdtemp(dir))
    {
        Check(false, "compare: a temp folder");
        return Finish();
    }
    const std::string tmp = dir;

    std::string out;
    int rc = Run("PYTHONDONTWRITEBYTECODE=1 python3 '" + firmware + "/twin/compare.py' --selftest", out);
    printf("%s", out.c_str());
    Check(rc == 0, "compare: compare.py --selftest: LED logs the same, changed, of different lengths and cut short");

    const std::string script = tmp + "/s.txt";
    std::ofstream(script) << "wait 200\nturn 4 10\nwait 200\n";
    rc = Run("'" + twin + "' -q -o '" + tmp + "/s.wav' -l '" + tmp + "/s.leds' '" + script + "'", out);
    std::ifstream leds(tmp + "/s.leds");
    std::string line;
    int lines = 0;
    while (std::getline(leds, line))
        lines++;
    printf("      written: exit %d, %d LED lines\n", rc, lines);
    Check(rc == 0 && lines > 0, "compare: frizz-twin writes its WAV and LED log, exit 0");

    struct
    {
        const char* what;
        std::string args, bad;
    } cases[] = {
        {"its LED log on a full disk", "-o '" + tmp + "/s.wav' -l /dev/full", "/dev/full"},
        {"its WAV on a full disk", "-o /dev/full -l '" + tmp + "/s.leds'", "/dev/full"},
        {"its LED log in a folder that isn't there", "-l '" + tmp + "/none/s.leds'", tmp + "/none/s.leds"},
    };
    for (const auto& c : cases)
    {
        rc = Run("'" + twin + "' -q " + c.args + " '" + script + "'", out);
        printf("      %s: exit %d, %s", c.what, rc, out.empty() ? "nothing said\n" : out.c_str());
        Check(rc == 4 && Has(out, "can't write " + c.bad),
              (std::string("compare: frizz-twin fails (exit 4) when it can't write ") + c.what).c_str());
    }

    // compare.py on a scenario it can't compare (B's WAV missing) and one after it: the first
    // said as such, the second compared all the same, and exit 2, never the 1 of a difference
    const std::string a = tmp + "/a", b = tmp + "/b";
    Run("mkdir -p '" + a + "' '" + b + "' && for d in '" + a + "' '" + b + "'; do cp '" + tmp + "/s.wav' \"$d/good.wav\" && cp '" + tmp +
            "/s.leds' \"$d/good.leds\" && cp '" + tmp + "/s.leds' \"$d/bad.leds\"; done && cp '" + tmp + "/s.wav' '" + a + "/bad.wav'",
        out);
    rc = Run("PYTHONDONTWRITEBYTECODE=1 python3 '" + firmware + "/twin/compare.py' '" + a + "' '" + b + "' bad good", out);
    printf("      a scenario it can't compare, then one it can: exit %d\n%s", rc, out.c_str());
    Check(rc == 2 && out.rfind("bad          couldn't compare: FileNotFoundError", 0) == 0 && Has(out, "\ngood         bit-identical\n") && Has(out, "\ncouldn't compare: bad\n"),
          "compare: twin/compare.py says a scenario it can't compare, compares the next all the same, exits 2");

    // the engine's compare.py: runs it can't read are exit 2, not a difference
    Run("head -c 400 /dev/zero > '" + a + "/x.bin' && head -c 404 /dev/zero > '" + b + "/x.bin'", out);
    rc = Run("PYTHONDONTWRITEBYTECODE=1 python3 '" + firmware + "/test/compare.py' '" + a + "/x.bin' '" + b + "/x.bin'", out);
    printf("      the engine's compare.py on runs it can't read: exit %d, %s", rc, out.c_str());
    Check(rc == 2 && Has(out, "couldn't compare: ") && Has(out, "unexpected run size"),
          "compare: test/compare.py exits 2 on runs it can't read, not 1 as for a difference");

    // ci.yml's guard (.github/scripts/against-base.sh): 0 and 1 (a difference) pass, a fault
    // fails, an exit 2 or a fault printed under an exit 1
    struct
    {
        const char* what;
        std::string cmd;
        int rc;
        const char* summary;
    } guard[] = {
        {"bit-identical", "echo same", 0, "Bit-identical."},
        {"a difference", "echo DIFFERENT; exit 1", 0, "Differences: each one is explained in the PR"},
        {"a fault (exit 2)", "echo boom; exit 2", 1, "**Didn't get through** (exit 2)"},
        {"a crash said under exit 1", "echo scenario x crashed on HEAD; exit 1", 1, "**Didn't get through** (exit 2)"},
        {"a traceback's exit 1 that compare.sh caught", "echo scenario x: compare.py gave no verdict; exit 1", 1, "**Didn't get through**"},
    };
    for (const auto& g : guard)
    {
        const std::string summary = tmp + "/summary.md";
        rc = Run("rm -f '" + summary + "'; GITHUB_STEP_SUMMARY='" + summary + "' '" + firmware + "/../.github/scripts/against-base.sh' 'T' sh -c \"" + g.cmd +
                     "\" > /dev/null; rc=$?; cat '" + summary + "'; exit $rc",
                 out);
        printf("      guard, %s: exit %d\n", g.what, rc);
        Check(rc == g.rc && Has(out, "### T\n") && Has(out, g.summary),
              (std::string("compare: CI's guard against the base on ") + g.what + (g.rc ? ": fails" : ": passes")).c_str());
    }

    Run("rm -rf '" + tmp + "'", out);
    return Finish();
}
