// twinlib.cpp: the twin as a shared library, for tools that embed it (a host that loads the
// virtual CHOMPI into a process of its own). ../twin/build.sh builds it position-independent
// and with hidden symbols (TWIN_DEFINES) into ../twin/build/twinlib, lib.sh's daisysp builds
// DaisySP the same way (-fPIC, build/libdaisysp_pic.a), and g++ -shared -Wl,--no-undefined
// links both with twinlib/lib.cpp into build/twinlib/libtwin.so, with nothing left for the
// loader to find. The check loads it with dlopen and runs it: one C function out, the twin's
// own API not, the firmware booting and its main loop running after 2 s, its audio finite.
// Run by unit.sh twinlib.
// unit libs: -ldl
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "check.h"

static const char* const kDefines = "-fPIC -fvisibility=hidden -fvisibility-inlines-hidden";
static const size_t kBlockSize = 24; // twin.h's: 0.5 ms at 48 kHz
static const int kChannels = 4;     // in: mic, X, aux L, aux R; out: headphone L/R, master L/R

typedef int (*RunFn)(int blocks, const float* in, float* out);

/** Runs cmd in the shell, its output into log; shows the log only if it fails */
static bool Sh(const std::string& cmd, const std::string& log, const char* what)
{
    bool ok = system(("(" + cmd + ") > \"" + log + "\" 2>&1").c_str()) == 0;
    if (!ok && system(("cat \"" + log + "\"").c_str()) != 0)
        printf("      its output: %s\n", log.c_str());
    Check(ok, what);
    return ok;
}

int main()
{
    const std::string firmware = std::string(__FILE__).substr(0, std::string(__FILE__).rfind('/')) + "/..";
    const std::string twin = firmware + "/twin/build/twinlib", out = firmware + "/test/build/twinlib";
    const std::string so = out + "/libtwin.so";
    if (system(("mkdir -p \"" + twin + "\" \"" + out + "\"").c_str()) != 0)
        return 1;

    // the twin and DaisySP built for a shared library, then the library itself
    if (!Sh("TWIN_BUILD=\"" + twin + "\" TWIN_DEFINES=\"" + kDefines + "\" flock \"" + twin +
                "/build.lock\" \"" + firmware + "/twin/build.sh\"",
            out + "/twin.log", "twinlib: the twin builds with -fPIC and hidden symbols (build.sh, TWIN_DEFINES)") ||
        !Sh("bash -c '. \"$0\" && daisysp pic -fPIC' \"" + firmware + "/test/lib.sh\"", out + "/daisysp.log",
            "twinlib: DaisySP builds with -fPIC (lib.sh's daisysp pic)") ||
        !Sh("g++ -O2 -g -std=gnu++14 -funsigned-char -Wall " + std::string(kDefines) + " -I\"" + firmware +
                "/twin\" -c \"" + firmware + "/test/twinlib/lib.cpp\" -o \"" + out + "/lib.o\" && g++ -shared " +
                "-Wl,--no-undefined \"" + out + "/lib.o\" \"" + twin + "/libtwin.a\" \"" + firmware +
                "/test/build/libdaisysp_pic.a\" -o \"" + so + "\"",
            out + "/link.log", "twinlib: libtwin.a and DaisySP link into a shared object (-shared, --no-undefined)"))
        return Finish();

    void* lib = dlopen(so.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        printf("      %s\n", dlerror());
    Check(lib != nullptr, "twinlib: dlopen loads it, every symbol resolved (RTLD_NOW)");
    if (!lib)
        return Finish();
    RunFn run = (RunFn)dlsym(lib, "twinlib_run");
    Check(run != nullptr, "twinlib: its C function is exported (twinlib_run)");
    Check(dlsym(lib, "_ZN4twin4BootEv") == nullptr && dlsym(lib, "_ZN4twin3RunEmPKfPf") == nullptr,
          "twinlib: the twin's own API isn't (twin::Boot, twin::Run: -fvisibility=hidden)");
    if (!run)
        return Finish();

    // power-on, then 2 s of a sine into AUX
    const int blocks = 4000;
    std::vector<float> in(blocks * kBlockSize * kChannels), audio(in.size());
    for (size_t i = 0; i < blocks * kBlockSize; i++)
        in[i * kChannels + 2] = in[i * kChannels + 3] = 0.25f * sinf(2.f * float(M_PI) * 440.f * i / 48000.f);
    int early = run(4, in.data(), audio.data());
    Check(early == 0, "twinlib: it boots, main() not in its loop yet after 4 blocks");
    int late = run(blocks - 4, in.data() + 4 * kBlockSize * kChannels, audio.data());
    Check(late != -1, "twinlib: its audio out is finite, 2 s of a sine into AUX");
    Check(late == 1, "twinlib: the firmware's main loop runs after 2 s (4000 blocks)");
    return Finish();
}
