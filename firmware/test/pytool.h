// pytool.h: what the checks that run firmware's Python tools share (remote.cpp, measure.cpp, and
// ptytwin.h's midisuite.cpp and reaper.cpp): where firmware/ is, a temp folder, Has(), and
// Python(), which runs a piece of Python against the tools with the CHOMPI's lock a file in
// that folder, so a host check never waits for, or takes, the real CHOMPI's, and with
// FRIZZ_NO_DEVICE set: a piece that reaches for a real device (a stub missing) is stopped
// there (tools/chompi.py's device_guard), never touching the CHOMPI.
#pragma once
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <fstream>
#include <string>

namespace
{
std::string firmware_dir; // the check's main() sets both
std::string tmp_dir;

bool Has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

/** python3 on CODE with tools/ and firmware/ on its path, the lock (tools/chompi.py) a file in
 *  the temp folder and not held, 20 s at most: never the CHOMPI's own, nor the CHOMPI (exit
 *  70 if it reaches for it). It runs in the temp folder. Its exit code (-1 for a signal); out
 *  gets what it printed, stdout and stderr */
__attribute__((unused)) int Python(const std::string& code, std::string& out)
{
    const std::string file = tmp_dir + "/tools.py";
    std::ofstream(file) << code;
    const std::string cmd = "cd '" + tmp_dir + "' && PYTHONDONTWRITEBYTECODE=1 PYTHONPATH='" + firmware_dir + "/tools:"
                            + firmware_dir + "' FRIZZ_CHOMPI_LOCK='" + tmp_dir
                            + "/lock' FRIZZ_CHOMPI_HELD= FRIZZ_NO_DEVICE=1 timeout -s KILL 20 python3 '" + file
                            + "' 2>&1";
    out.clear();
    FILE* p = popen(cmd.c_str(), "r");
    char buf[512];
    size_t n;
    while (p && (n = fread(buf, 1, sizeof(buf), p)) > 0)
        out.append(buf, n);
    const int status = p ? pclose(p) : -1;
    unlink(file.c_str());
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
} // namespace
