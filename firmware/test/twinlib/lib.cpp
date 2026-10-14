// twinlib/lib.cpp: the shared library twinlib.cpp builds, as a tool that embeds the twin
// would: twin.h's API linked in whole (libtwin.a, DaisySP), behind one C function; of
// twin.h's API itself it exports nothing (the twin is built with -fvisibility=hidden).
#include <cmath>
#include "twin.h"

/** Powers the firmware on at the first call, then runs `blocks` audio blocks (in and out as
 *  twin::Run's). Returns 1 if its main() has entered its loop by then, 0 if not yet, -1 if a
 *  sample out isn't a finite number */
extern "C" __attribute__((visibility("default"))) int twinlib_run(int blocks, const float* in, float* out)
{
    static bool booted = false;
    if (!booted)
        twin::Boot();
    booted = true;
    twin::Run(blocks, in, out);
    for (size_t i = 0; out && i < blocks * twin::kBlockSize * twin::kChannels; i++)
        if (!std::isfinite(out[i]))
            return -1;
    return twin::MainLoopRunning() ? 1 : 0;
}
