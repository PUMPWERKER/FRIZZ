// measure.cpp: firmware/tools/measure.py's fx without the CHOMPI (#187). FRIZZ is a stand-in
// that answers GET and SET as MidiControl.h does (a latch that takes, or one that doesn't),
// the audio interface is stubbed: which effect is latched while it records, the CCs on the
// effect's channel, the latches and knobs put back afterwards, and the measurement refused
// when the latch didn't take; AUX set live and through the effects in RAM only where the
// settings have it otherwise, and put back (#391). Python 3 only: importing measure.py works without numpy.
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include "check.h"
#include "pytool.h"

// The stand-in and a run of `measure.py fx ARGS`: prints what was latched while it recorded,
// the CCs sent, the latches and the knobs afterwards, and the exit
static const char* kStub = R"(
import math, sys, time
sys.modules["numpy"] = None  # as on a computer without it: measure.py imports all the same
import chompi, remote, measure
time.sleep = lambda s: None


class Frizz:
    """GET and SET as MidiControl.h answers them; a latch SET that doesn't take with TAKES False;
    the settings with AUX as AUX has it (aux_live, aux_dry), PERSIST 0 taken unless PERSIST False"""
    def __init__(self, latches=(), takes=True, aux=(1, 0), persist=True):
        self.latch = [int(n in latches) for n in remote.FX_NAMES]
        self.knobs = [[8192] * 8 for _ in remote.FX_NAMES]
        self.settings = [0] * len(remote.SETTING_NAMES)
        self.set_aux(*aux)
        self.takes, self.ccs, self.persist, self.ram = takes, [], persist, False

    def set_aux(self, live, dry):
        self.settings[remote.SETTING_NAMES.index("aux_live")] = live
        self.settings[remote.SETTING_NAMES.index("aux_dry")] = dry

    def aux(self):
        return "live %d dry %d" % tuple(self.settings[remote.SETTING_NAMES.index(n)] for n in ("aux_live", "aux_dry"))

    def ram_only(self):
        self.ram = self.ram or self.persist
        return self.persist or None

    def dev(self, sub, args=(), retries=3):
        if sub == chompi.GET:
            g = args[0]
            v = {remote.GROUP_LATCH: self.latch, remote.GROUP_SETTINGS: self.settings}.get(g) or self.knobs[g]
            return chompi.OK, bytes([g, len(v)] + [b for x in v for b in (x >> 7 & 0x7F, x & 0x7F)])
        if sub == chompi.SET:
            g, i, v = args[0], args[1], args[2] << 7 | args[3]
            if g == remote.GROUP_SETTINGS:
                self.settings[i] = v
            elif g != remote.GROUP_LATCH:
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


def fx(args, latches=(), takes=True, level=1e-4, fail=False, aux=(1, 0), persist=True):
    f = Frizz(latches, takes, aux, persist)
    seen, heard = [], []
    def record(seconds, path):
        seen.append(" ".join(n for n, v in zip(remote.FX_NAMES, f.latch) if v) or "-")
        heard.append("AUX %s%s" % (f.aux(), ", RAM only" if f.ram else ""))
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
    if aux != (1, 0):
        print("AUX %s: while recording %s; after %s; exit %s" % (aux, heard or "never", f.aux(), code), flush=True)
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
fx("reverb", aux=(0, 1))
fx("none", aux=(1, 1))
fx("reverb", aux=(0, 1), fail=True)
fx("reverb", aux=(0, 0), persist=False)
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
    // #391: a card with AUX unmonitored or past the effects recorded silence or the dry line
    Check(Has(out, "AUX set to play through for the measurement, in RAM only, the card untouched: aux_live 1 (was 0), aux_dry 0 (was 1) (put back after)")
              && Has(out, "AUX (0, 1): while recording ['AUX live 1 dry 0, RAM only']; after live 0 dry 1; exit 0"),
          "measure: AUX off and past the effects is set through for fx in RAM only (PERSIST 0), said so, and put back after (#391)");
    Check(Has(out, "untouched: aux_dry 0 (was 1) (put back after)") && Has(out, "AUX (1, 1): while recording ['AUX live 1 dry 0, RAM only']; after live 1 dry 1; exit 0"),
          "measure: fx none too, only what differs set");
    Check(Has(out, "AUX (0, 1): while recording ['AUX live 1 dry 0, RAM only']; after live 0 dry 1; exit raised"),
          "measure: a recording that breaks off still puts AUX's settings back");
    Check(Has(out, "this FRIZZ can't set them in RAM only (no PERSIST)") && Has(out, "AUX (0, 0): while recording never; after live 0 dry 0; exit 1"),
          "measure: a FRIZZ that can't keep them out of the card is left as it is, the measurement refused, saying why");
    Check(!Has(out, "AUX set to play through for the measurement, in RAM only, the card untouched: aux_live 1 (was 1)"),
          "measure: AUX already live and through: nothing set");
    if (failures)
        printf("%s\n", out.c_str());
    std::string cmd = "rm -rf '" + tmp_dir + "'";
    if (system(cmd.c_str()) != 0)
        printf("couldn't remove %s\n", tmp_dir.c_str());
    return Finish();
}
