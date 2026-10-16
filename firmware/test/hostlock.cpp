// hostlock.cpp: the host lock the heavy host runs take themselves (hostlock.sh, #439), on a lock
// file of its own (FRIZZ_HOST_LOCK), never the machine's, and mostly without the memory cap
// (FRIZZ_HOST_MEMORY=none; once with it where systemd-run --user works): a run takes it and
// says so in the file, its arguments as they were; what it starts goes on under it (nested, no
// wait); a run held by another waits, naming the holder, by its line or, for one by hand, its
// command; a caller that holds it by hand (`flock FILE ...`, the way before) isn't waited for;
// CI and FRIZZ_HOST_LOCKED take nothing; builds.py runs itself again under it to build, not for
// md5; the heavy scripts take it before anything else, build.sh only when it has something to
// build. Run by unit.sh hostlock; needs flock(1).
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "check.h"
#include "twin.h" // the twin built first (lib.sh's twin_for): build.sh has nothing to build then

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

static std::string Read(const std::string& path)
{
    std::ifstream f(path);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

static double Now()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main()
{
    // the folder as lib.sh names it: build.sh's flags carry the twin's path, and its stamp them
    char* real = realpath((std::string(__FILE__).substr(0, std::string(__FILE__).rfind('/')) + "/..").c_str(), nullptr);
    const std::string firmware = real ? real : "?";
    free(real);
    const std::string hostlock = firmware + "/test/hostlock.sh";
    char dir[] = "/tmp/frizz-hostlock-XXXXXX";
    if (!mkdtemp(dir))
    {
        Check(false, "hostlock: a temp folder");
        return Finish();
    }
    const std::string tmp = dir, lock = tmp + "/host.lock";
    // the lock of its own, no cap, nothing of all.sh's lock or CI's
    const std::string env = "env -u CI -u FRIZZ_HOST_LOCKED FRIZZ_HOST_LOCK='" + lock + "' FRIZZ_HOST_MEMORY=none ";
    // a heavy script: takes the lock, says what it got, and runs its argument (a nested one)
    const std::string heavy = tmp + "/heavy.sh";
    std::ofstream(heavy) << "#!/bin/bash\nsource '" << hostlock << "'\n"
                         << "echo \"$1: locked=$FRIZZ_HOST_LOCKED args=$#:$*\"\n"
                         << "[ -z \"$2\" ] || \"$2\" inner\n";
    std::string out;
    if (Run("chmod +x '" + heavy + "'", out) != 0)
        Check(false, "hostlock: the script");

    int rc = Run(env + "'" + heavy + "' outer '" + heavy + "'", out);
    const std::string line = Read(lock);
    printf("      %s", line.c_str());
    Check(rc == 0 && Has(out, "outer: locked=") && !Has(out, "outer: locked=\n") && !Has(out, "outer: locked=above")
              && Has(line, "pid ") && Has(line, " since ") && Has(line, heavy + " outer " + heavy),
          "hostlock: a heavy run takes the lock, its line in the file (pid, where, since when, the command)");
    const size_t at = out.find("outer: locked=");
    const std::string pid = at == std::string::npos ? "?" : out.substr(at + 14, out.find(' ', at + 14) - at - 14);
    Check(Has(out, "inner: locked=" + pid + " "), "hostlock: what it starts runs under it, not waiting for it (all.sh's check.sh)");

    rc = Run(env + "timeout 10 '" + heavy + "' 'a b' '' '$HOME' '*'", out);
    Check(rc == 0 && Has(out, "a b: locked=") && Has(out, "args=4:a b  $HOME *"),
          "hostlock: the arguments run again as they were (spaces, $, *)");

    // held by another run: it waits, naming it, and runs once that ended
    double t0 = Now();
    rc = Run("(" + env + "bash '" + hostlock + "' sleep 2 > /dev/null 2>&1 &) ; sleep 0.5; " + env + "timeout 10 '" + heavy + "' second",
             out);
    double waited = Now() - t0;
    printf("      waited %.1f s: %s", waited, out.c_str());
    Check(rc == 0 && Has(out, "the host lock is held (pid ") && Has(out, ": sleep 2): waiting") && Has(out, "second: locked=")
              && waited > 1.5,
          "hostlock: a run held by another waits, saying who holds it, then runs");

    // held by hand around it: no wait; held by hand beside it: it waits, naming the command
    rc = Run(env + "timeout 10 flock '" + lock + "' '" + heavy + "' hand", out);
    Check(rc == 0 && Has(out, "hand: locked=above"), "hostlock: a caller holding it by hand (flock FILE CMD) isn't waited for");
    t0 = Now();
    rc = Run("(flock '" + lock + "' sleep 2 > /dev/null 2>&1 &) ; sleep 0.5; " + env + "timeout 10 '" + heavy + "' beside", out);
    waited = Now() - t0;
    Check(rc == 0 && Has(out, "the host lock is held (pid ") && Has(out, ": flock " + lock + " sleep 2 ): waiting")
              && Has(out, "beside: locked=") && waited > 1.5,
          "hostlock: one held by hand beside it is waited for, named by its command");
    if (rc != 0 || !Has(out, "flock " + lock))
        printf("%s\n", out.c_str());

    // CI and FRIZZ_HOST_LOCKED: nothing taken, even with the lock held
    rc = Run("(flock '" + lock + "' sleep 2 > /dev/null 2>&1 &) ; sleep 0.3; " + env + "CI=true timeout 1 '" + heavy + "' ci; " + env
                 + "FRIZZ_HOST_LOCKED=7 timeout 1 '" + heavy + "' set",
             out);
    Check(rc == 0 && Has(out, "ci: locked= ") && Has(out, "set: locked=7 "), "hostlock: CI and FRIZZ_HOST_LOCKED take nothing");
    Run("flock '" + lock + "' true", out); // the one by hand ended

    // the memory cap, where systemd-run --user works here (not on Actions: no user session)
    if (Run("systemd-run --user --scope --quiet -p MemoryMax=1G true", out) == 0)
    {
        rc = Run("env -u CI -u FRIZZ_HOST_LOCKED FRIZZ_HOST_LOCK='" + lock + "' FRIZZ_HOST_MEMORY=1G timeout 10 bash '"
                     + hostlock + "' sh -c 'cat /proc/self/cgroup; systemctl --user show -p MemoryMax \"$(basename "
                                  "$(cut -d: -f3 /proc/self/cgroup))\"'",
                 out);
        Check(rc == 0 && Has(out, ".scope") && Has(out, "MemoryMax=1073741824"), "hostlock: in a scope of its own, with the cap");
        if (rc != 0 || !Has(out, "MemoryMax=1073741824"))
            printf("%s\n", out.c_str());
    }
    else
        printf("      no systemd-run --user here: the cap not checked\n");

    // builds.py: to build, it runs itself again through it; md5 doesn't
    const std::string py = "PYTHONDONTWRITEBYTECODE=1 PYTHONPATH='" + firmware + "/tools' timeout 10 python3 -c 'import sys, builds; "
                           "sys.argv = [\"builds.py\", \"--no-such\"]; builds.host_lock(); print(\"not again\")'";
    rc = Run(env + py, out);
    Check(!Has(out, "not again") && Has(Read(lock), "/tools/builds.py --no-such"),
          "hostlock: builds.py runs itself again under it to build");
    rc = Run(env + "FRIZZ_HOST_LOCKED=1 " + py, out);
    Check(rc == 0 && Has(out, "not again"), "hostlock: and not when it's taken care of");
    const std::string builds = Read(firmware + "/tools/builds.py");
    const size_t md5 = builds.find("if args.what == \"cpu\":"), take = builds.find("host_lock()\n", md5);
    Check(md5 != std::string::npos && take != std::string::npos && builds.rfind("if args.what is None:", take) > md5,
          "hostlock: builds.py takes it only to build (no argument), not for md5 or cpu");

    // the heavy scripts take it before anything else they source; build.sh before it builds
    for (const char* s : {"test/all.sh", "test/check.sh", "twin/compare.sh", "twin/ui-at.sh"})
    {
        const std::string text = Read(firmware + "/" + s);
        const size_t first = text.find("\nsource ");
        Check(first != std::string::npos && text.find("hostlock.sh\"", first) < text.find('\n', first + 1),
              (std::string("hostlock: ") + s + " takes the lock first").c_str());
    }
    const std::string build = Read(firmware + "/twin/build.sh");
    size_t heavy_at = build.find("\n    heavy\n"), host_at = build.find("\nheavy\n");
    Check(heavy_at != std::string::npos && heavy_at < build.find("building DaisySP for the browser")
              && host_at != std::string::npos && host_at < build.find("echo \"building the twin\"\n"),
          "hostlock: build.sh takes it before it builds, the browser's twin and the host's");
    const char* tb = getenv("TWIN_BUILD");
    const std::string twin = tb && *tb ? tb : firmware + "/twin/build";
    t0 = Now();
    rc = Run("(" + env + "bash '" + hostlock + "' sleep 3 > /dev/null 2>&1 &) ; sleep 0.3; " + env + "TWIN_BUILD='" + twin
                 + "' timeout 10 flock '" + twin + "/build.lock' '" + firmware + "/twin/build.sh'",
             out);
    waited = Now() - t0;
    Check(rc == 0 && !Has(out, "waiting") && waited < 2.5, "hostlock: build.sh with nothing to build doesn't wait for it (run.sh, serve.py)");
    if (rc != 0 || Has(out, "waiting"))
        printf("%s\n", out.c_str());
    Run("flock '" + lock + "' true", out);

    if (system(("rm -rf '" + tmp + "'").c_str()) != 0)
        printf("could not remove %s\n", tmp.c_str());
    return Finish();
}
