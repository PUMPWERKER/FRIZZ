// measure.cpp: firmware/tools/measure.py's fx without the CHOMPI (#187). FRIZZ is a stand-in
// that answers GET and SET as MidiControl.h does (a latch that takes, or one that doesn't),
// the audio interface is stubbed: which effect is latched while it records, the CCs on the
// effect's channel, the latches and knobs put back afterwards, and the measurement refused
// when the latch didn't take. Python 3 only: importing measure.py works without numpy.
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include "check.h"

static std::string firmware_dir, tmp_dir;

static bool Has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

/** python3 on CODE with tools/ and firmware/ on its path, the lock (tools/chompi.py) a file in
 *  the temp folder and not held, 20 s at most: never the CHOMPI's own (as remote.cpp's). Its
 *  exit code; out gets what it printed, stdout and stderr */
static int Python(const std::string& code, std::string& out)
{
    const std::string file = tmp_dir + "/tools.py";
    std::ofstream(file) << code;
    const std::string cmd = "cd '" + tmp_dir + "' && PYTHONDONTWRITEBYTECODE=1 PYTHONPATH='" + firmware_dir + "/tools:"
                            + firmware_dir + "' FRIZZ_CHOMPI_LOCK='" + tmp_dir
                            + "/lock' FRIZZ_CHOMPI_HELD= timeout -s KILL 20 python3 '" + file
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

// The stand-in and a run of `measure.py fx ARGS`: prints what was latched while it recorded,
// the CCs sent, the latches and the knobs afterwards, and the exit
static const char* kStub = R"(
import math, sys, time
sys.modules["numpy"] = None  # as on a computer without it: measure.py imports all the same
import chompi, remote, measure
time.sleep = lambda s: None


class Frizz:
    """GET and SET as MidiControl.h answers them; a latch SET that doesn't take with TAKES False"""
    def __init__(self, latches=(), takes=True):
        self.latch = [int(n in latches) for n in remote.FX_NAMES]
        self.knobs = [[8192] * 8 for _ in remote.FX_NAMES]
        self.takes, self.ccs = takes, []

    def dev(self, sub, args=(), retries=3):
        if sub == chompi.GET:
            g = args[0]
            v = self.latch if g == remote.GROUP_LATCH else self.knobs[g]
            return chompi.OK, bytes([g, len(v)] + [b for x in v for b in (x >> 7 & 0x7F, x & 0x7F)])
        if sub == chompi.SET:
            g, i, v = args[0], args[1], args[2] << 7 | args[3]
            if g != remote.GROUP_LATCH:
                self.knobs[g][i] = v
            elif self.takes:
                self.latch[i] = v
            return chompi.OK, bytes([g, i])

    def raw(self, b):
        self.ccs.append(" ".join("%02X" % x for x in b))
        fx = remote.FX_NAMES.index(measure.KEY_FX["KEY_%d" % ((b[0] & 15) + 1)])
        self.knobs[fx][b[1] - 14] = b[2] << 7


class Recording(list):
    def __pow__(self, n):
        return self


class Np:
    sqrt = staticmethod(math.sqrt)
    mean = staticmethod(lambda y: sum(y) / len(y))


def fx(args, latches=(), takes=True, level=1e-4, fail=False):
    f = Frizz(latches, takes)
    seen = []
    def record(seconds, path):
        seen.append(" ".join(n for n, v in zip(remote.FX_NAMES, f.latch) if v) or "-")
        if fail:
            raise RuntimeError("pw-record died")
        return Recording([level] * 10)
    measure.frizz = lambda a: f
    measure.play = lambda path: type("P", (), {"terminate": lambda self: None})()
    measure.record = record
    measure.np = Np
    measure.aux_sink = measure.line_source = lambda: "stub"
    sys.argv = ["measure.py", "fx"] + args.split() + ["--material", "m.wav", "--out", "o"]
    try:
        measure.main()
        code = 0
    except SystemExit as e:
        code = e.code if isinstance(e.code, int) else 1
        if not isinstance(e.code, int):
            print(e.code)
    except RuntimeError:
        code = "raised"
    print("RESULT %s: recorded %s; ccs [%s]; after %s; knobs %s; exit %s" % (
        args, seen or "never", ", ".join(f.ccs),
        " ".join(n for n, v in zip(remote.FX_NAMES, f.latch) if v) or "-",
        "moved" if any(v != 8192 for k in f.knobs for v in k) else "as before", code), flush=True)
)";

int main()
{
    firmware_dir = std::string(__FILE__).substr(0, std::string(__FILE__).rfind('/')) + "/..";
    char dir[] = "/tmp/frizz-measure-XXXXXX";
    tmp_dir = mkdtemp(dir);
    std::string out;
    const int rc = Python(std::string(kStub) + R"(
fx("KEY_12 --cc 21=80", latches=["reverb"])
fx("KEY_7 --cc 14=101")
fx("reverb")
fx("shifter --cc 14=101", latches=["shifter"])
fx("none", latches=["delay", "chaos"])
fx("KEY_12", takes=False)
fx("KEY_12 --cc 21=80", latches=["reverb"], fail=True)
fx("KEY_12", level=1e-10)
fx("none --cc 21=80")
fx("KEY_14")
)", out);
    if (rc != 0)
        printf("%s\n", out.c_str());

    Check(Has(out, "RESULT KEY_12 --cc 21=80: recorded ['delay']; ccs [BB 15 50]; after reverb; knobs as before; exit 0"),
          "measure: fx KEY_12 records with the delay latched alone (the reverb unlatched), its CC on channel 12, both put back after");
    Check(Has(out, "delay latched alone, its knobs: 0.5000  0.5000  0.5000  0.5000  0.5000  0.5000  0.5000  0.6250"),
          "measure: it says what was latched and the knobs as FRIZZ has them, the CC's included");
    Check(Has(out, "RESULT KEY_7 --cc 14=101: recorded ['phaser']; ccs [B6 0E 65]; after -; knobs as before; exit 0"),
          "measure: KEY_7 is the phaser (FxSlots.h), not FxId 6, its CC on channel 7");
    Check(Has(out, "RESULT reverb: recorded ['reverb']; ccs []; after -; knobs as before; exit 0"),
          "measure: an effect by its name");
    Check(Has(out, "RESULT shifter --cc 14=101: recorded ['shifter']; ccs [B1 0E 65]; after shifter; knobs as before; exit 0"),
          "measure: an effect latched already stays latched, its knobs put back");
    Check(Has(out, "nothing latched") && Has(out, "RESULT none: recorded ['-']; ccs []; after delay chaos; knobs as before; exit 0"),
          "measure: fx none records with nothing latched (the dry line), the latches put back after");
    Check(Has(out, "latched: nothing, not delay: FRIZZ didn't take the latch")
              && Has(out, "RESULT KEY_12: recorded never; ccs []; after -; knobs as before; exit 1"),
          "measure: a latch that doesn't take ends it before anything is played or recorded (#187)");
    Check(Has(out, "RESULT KEY_12 --cc 21=80: recorded ['delay']; ccs [BB 15 50]; after reverb; knobs as before; exit raised"),
          "measure: a recording that breaks off still puts the latches and knobs back");
    Check(Has(out, "nothing on the line in") && Has(out, "RESULT KEY_12: recorded ['delay']; ccs []; after -; knobs as before; exit 1"),
          "measure: a silent line in fails, saying so");
    Check(Has(out, "fx none: no effect for the CCs") && Has(out, "RESULT none --cc 21=80: recorded never; ccs []; after -; knobs as before; exit 1"),
          "measure: CCs with none are refused");
    Check(Has(out, "fx KEY_14: a key KEY_1 .. KEY_13") && Has(out, "RESULT KEY_14: recorded never; ccs []; after -; knobs as before; exit 1"),
          "measure: a key without an effect of its own (the chaos key) is refused");
    if (failures)
        printf("%s\n", out.c_str());
    std::string cmd = "rm -rf '" + tmp_dir + "'";
    if (system(cmd.c_str()) != 0)
        printf("couldn't remove %s\n", tmp_dir.c_str());
    return Finish();
}
