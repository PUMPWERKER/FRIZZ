// compare.cpp: what ../twin/compare.sh relies on to say a scenario is bit-identical or not.
// compare.py's own check of its LED comparison (--selftest: logs of different lengths and a line
// cut short are a difference, said as such, never a crash), and frizz-twin failing a run whose
// WAV or LED log it couldn't write whole (exit 4: a full /tmp, where compare.sh's runs go), so a
// log cut short never reaches the comparison. Run by unit.sh compare; needs python3.
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

    Run("rm -rf '" + tmp + "'", out);
    return Finish();
}
