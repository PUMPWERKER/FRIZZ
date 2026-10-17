// platform.cpp: FRIZZ's copy of the PUMPWERKER platform (../platform/, #477). It is the commit its
// PIN names, file for file (tools/platform-sync.py --check: from a clone of the platform next to
// FRIZZ's checkout, else fetched from GitHub; offline it says SKIP, on Actions it fails); a
// source file's name is in one folder only, of code/src, the platform's layers and twin/ (the
// Makefile names objects by it, test/sources.sh and twin/build.sh copy them into one folder);
// and the layers keep their include rule (core/ includes nothing of daisy/ or boards/, daisy/
// nothing of boards/). Then platform-sync.py itself, on repositories of its own in a temp folder,
// never the network: a commit copied in (its modes, a file a .gitignore would drop, PIN, staged),
// --check telling a changed, extra, missing or re-moded file, refusing a commit that isn't
// pushed, fetching one the clone hasn't, and offline: SKIP, or a failure on Actions.
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "check.h"

static std::string firmware_dir;
static std::string tmp_dir;

static bool Has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

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

/** The files in dir (not its subfolders) whose names end in one of exts */
static std::vector<std::string> Files(const std::string& dir, const std::vector<std::string>& exts)
{
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str()))
    {
        while (dirent* e = readdir(d))
        {
            const std::string name = e->d_name;
            struct stat st;
            if (stat((dir + "/" + name).c_str(), &st) || !S_ISREG(st.st_mode))
                continue;
            for (const std::string& ext : exts)
                if (name.size() > ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0)
                    out.push_back(name);
        }
        closedir(d);
    }
    return out;
}

static std::string Read(const std::string& path)
{
    std::ifstream in(path);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

static const std::vector<std::string> kSources = {".h", ".c", ".cpp"};

static void Pin()
{
    std::string out;
    const int rc = Run("timeout -s KILL 600 python3 '" + firmware_dir + "/tools/platform-sync.py' --check", out);
    // its own lines, a SKIP among them (all.sh lists it under the check)
    printf("%s", out.c_str());
    Check(rc == 0, "firmware/platform is the platform commit its PIN names (tools/platform-sync.py --check)");
}

static void Basenames()
{
    std::map<std::string, std::vector<std::string>> where;
    for (const char* dir : {"code/src", "platform/core", "platform/daisy", "platform/boards/chompi", "twin"})
        for (const std::string& name : Files(firmware_dir + "/" + dir, kSources))
            where[name].push_back(dir);
    std::string twice;
    for (const auto& w : where)
        if (w.second.size() > 1)
            twice += " " + w.first + " (" + w.second[0] + ", " + w.second[1] + ")";
    Check(where.count("chompi_main.cpp") && where.count("MidiClock.h") && twice.empty(),
          ("a source file's name is in one folder only, of code/src, the platform's layers and twin/" + twice).c_str());
}

static void Layers()
{
    const std::string p = firmware_dir + "/platform/";
    std::set<std::string> daisy, boards;
    for (const std::string& f : Files(p + "daisy", kSources))
        daisy.insert(f);
    for (const std::string& f : Files(p + "boards/chompi", kSources))
        boards.insert(f);
    const std::regex include("^\\s*#\\s*include\\s*\"([^\"]+)\"");
    std::string bad;
    for (const char* layer : {"core", "daisy"})
        for (const std::string& f : Files(p + layer, kSources))
        {
            std::istringstream text(Read(p + layer + "/" + f));
            std::string line;
            std::smatch m;
            while (std::getline(text, line))
                if (std::regex_search(line, m, include))
                {
                    const std::string name = m[1];
                    if (boards.count(name) || (std::string(layer) == "core" && daisy.count(name)))
                        bad += " " + std::string(layer) + "/" + f + " includes " + name;
                }
        }
    Check(bad.empty(), ("core/ includes nothing of daisy/ or boards/, daisy/ nothing of boards/" + bad).c_str());
}

// --- platform-sync.py on repositories of its own ---

/** The environment of each run: git without the user's configuration, commits by a test author */
static const std::string kGit = "GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1 GIT_AUTHOR_NAME=t "
                                "GIT_AUTHOR_EMAIL=t@t GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@t ";

/** Check(), with what the runs printed only if it fails (their FAIL and SKIP lines are the cases') */
static void Says(bool ok, const std::string& what, const std::string& out)
{
    Check(ok, (ok ? what : what + ": " + out).c_str());
}

static int Sh(const std::string& cmd, std::string& out) { return Run("cd '" + tmp_dir + "' && " + kGit + cmd, out); }

static std::string Sh(const std::string& cmd)
{
    std::string out;
    if (Sh(cmd, out))
        printf("(setup failed: %s: %s)\n", cmd.c_str(), out.c_str());
    while (!out.empty() && out.back() == '\n')
        out.pop_back();
    return out;
}

/** platform-sync.py ARGS on the copy fw/platform, the clone CLONE, GitHub at HUB, the cache CACHE */
static int Sync(const std::string& args, std::string& out, const std::string& clone = "clone",
                const std::string& hub = "hub.git", const std::string& cache = "cache.git",
                const std::string& actions = "")
{
    return Sh("PLATFORM_REPO='" + tmp_dir + "/" + clone + "' PLATFORM_URL='file://" + tmp_dir + "/" + hub
                  + "' PLATFORM_CACHE='" + tmp_dir + "/" + cache + "' GITHUB_ACTIONS=" + actions + " python3 '"
                  + firmware_dir + "/tools/platform-sync.py' --dir fw/platform " + args,
              out);
}

static void SyncCases()
{
    // the platform: a header, a script, a library its .gitignore would drop; pushed to hub.git, a
    // clone of it with a commit it never pushed
    Sh("git init -q -b main src && mkdir -p src/core src/daisy/libs/x/build src/tools"
       " && printf 'int a;\\n' > src/core/a.h && printf 'lib\\0\\1' > src/daisy/libs/x/build/x.a"
       " && printf '#!/bin/sh\\n' > src/tools/run.sh && chmod +x src/tools/run.sh && printf 'build/\\n' > src/.gitignore"
       " && git -C src add -f . && git -C src commit -q -m one && git clone -q --bare src hub.git"
       " && git clone -q hub.git clone && printf 'int a2;\\n' > clone/core/a.h && git -C clone commit -q -am two"
       " && git init -q -b main fw && printf '*.a\\n' > fw/.gitignore && git -C fw add . && git -C fw commit -q -m fw");
    const std::string one = Sh("git -C hub.git rev-parse main"), two = Sh("git -C clone rev-parse HEAD");
    std::string out;
    int rc = Sync(one.substr(0, 10), out);
    Says(rc == 0 && Read(tmp_dir + "/fw/platform/PIN") == one + "\n" && Read(tmp_dir + "/fw/platform/core/a.h") == "int a;\n"
              && Has(out, "staged") && Sh("git -C fw ls-files platform") == "platform/.gitignore\nplatform/PIN\n"
                                                                            "platform/core/a.h\nplatform/daisy/libs/x/build/x.a\n"
                                                                            "platform/tools/run.sh"
              && Sh("git -C fw ls-files -s platform/tools/run.sh").substr(0, 6) == "100755",
          "a commit copied in from the clone: its files with their modes, a library the .gitignores would drop, PIN, "
          "all staged", out);
    rc = Sync("--check", out);
    Says(rc == 0 && Has(out, "file for file") && !Has(out, "note"), "--check: the copy is PIN's commit", out);
    Sh("git -C fw commit -q -m sync");

    Sh("printf 'int b;\\n' > fw/platform/core/a.h");
    rc = Sync("--check", out);
    Says(rc == 1 && Has(out, "changed: core/a.h\n") && Has(out, "FAIL"), "--check: a file changed", out);
    Sh("git -C fw checkout -q platform && chmod -x fw/platform/tools/run.sh");
    rc = Sync("--check", out);
    Says(rc == 1 && Has(out, "changed: tools/run.sh (mode 100644, the commit's 100755)"), "--check: a file's mode changed", out);
    Sh("chmod +x fw/platform/tools/run.sh && printf 'x' > fw/platform/core/b.h && printf 'x' > fw/platform/core/junk.a");
    rc = Sync("--check", out);
    Says(rc == 1 && Has(out, "extra:   core/b.h") && !Has(out, "junk.a"), "--check: a file added (one the .gitignore ignores isn't the copy's)", out);
    Sh("rm fw/platform/core/b.h fw/platform/core/junk.a && rm fw/platform/daisy/libs/x/build/x.a");
    rc = Sync("--check", out);
    Says(rc == 1 && Has(out, "missing: daisy/libs/x/build/x.a"), "--check: a file gone", out);
    Sh("git -C fw checkout -q platform");

    rc = Sync(two, out);
    Says(rc != 0 && Has(out, "isn't in a branch or tag") && Read(tmp_dir + "/fw/platform/PIN") == one + "\n", "a commit that isn't pushed isn't copied in", out);
    rc = Sync(two + " --unpushed", out);
    std::string check;
    const int rc2 = Sync("--check", check);
    Says(rc == 0 && Read(tmp_dir + "/fw/platform/core/a.h") == "int a2;\n" && rc2 == 0 && Has(check, "note: PIN's commit"),
         "--unpushed copies it, and --check says CI won't find it", out + check);

    // the clone without the commit: fetched from GitHub into the cache
    Sh("git -C src rm -q tools/run.sh && git -C src commit -q -m three && git -C src push -q '" + tmp_dir + "/hub.git' main");
    const std::string three = Sh("git -C src rev-parse HEAD");
    rc = Sync(three, out);
    rc = rc || Sync("--check", check, "clone", "hub.git", "cache.git");
    Says(rc == 0 && Read(tmp_dir + "/fw/platform/PIN") == three + "\n" && Has(check, "file for file")
              && Sh("git -C fw status --porcelain platform/tools") == "D  platform/tools/run.sh", "a commit the clone hasn't, fetched from GitHub, a file it removed removed and staged", out + check);

    // offline: no clone with PIN's commit, GitHub out of reach
    rc = Sync("--check", out, "none", "nohub.git", "cache2.git");
    Says(rc == 0 && Has(out, "SKIP: local only") && !Has(out, "FAIL"), "--check offline: SKIP", out);
    rc = Sync("--check", out, "none", "nohub.git", "cache2.git", "true");
    Says(rc == 2 && Has(out, "FAIL"), "--check offline on GitHub Actions fails", out);
    Sh("echo " + two + " > fw/platform/PIN");
    rc = Sync("--check", out, "none", "hub.git", "cache3.git");
    Says(rc == 1 && Has(out, "has no commit"), "--check: PIN's commit isn't on GitHub", out);
}

int main()
{
    firmware_dir = std::string(__FILE__).substr(0, std::string(__FILE__).rfind('/')) + "/..";
    char tmpl[] = "/tmp/frizz-platform-XXXXXX";
    tmp_dir = mkdtemp(tmpl);
    Pin();
    Basenames();
    Layers();
    SyncCases();
    std::string out;
    Run("rm -rf '" + tmp_dir + "'", out);
    return Finish();
}
