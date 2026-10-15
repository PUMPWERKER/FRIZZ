// loopcap.cpp: firmware/tools/loopcap.py without the CHOMPI: its test loops (whole cycles, the
// acid chunk's beats, the bursts on the beats) and its analysis of a capture made here from them
// as FRIZZ would play them: at once with a 10 ms crossfade by time, a hard cut, the crossfader's
// equal-power mix, the 6 kHz marker; and its judging of a program change's landing, at once and
// at the n-th pass end, right and wrong. The analysis needs numpy, from ~/.venvs/frizz
// (measure.py's; CI makes one): without it, SKIP: local only.
#include <unistd.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "check.h"
#include "pytool.h"

static const char* kCode = R"(
import io, json, sys, contextlib
import numpy as np
import loopcap as L

SR = L.SR
L.cmd_make(type("A", (), {"dir": "loops"})())
import wave, struct
for slot, (bed, secs, beats) in L.LOOPS.items():
    raw = open("loops/h%d.wav" % slot, "rb").read()
    acid = raw.find(b"acid")
    got = struct.unpack("<IHHfIHHf", raw[acid + 8:acid + 32])
    with wave.open("loops/h%d.wav" % slot) as w:
        n = w.getnframes()
        x = np.frombuffer(w.readframes(n), "<i2").reshape(-1, 2)
    # the step from the last sample to the first, against the bed's largest sample to sample
    wrap = abs(int(x[0, 0]) - int(x[-1, 0]))
    step = int(np.abs(np.diff(x[:2000, 0].astype(int))).max())
    print("LOOP h%d frames %d beats %d bpm %.1f right %d wrap %d" % (
        slot, n, got[4], got[7], int(np.abs(x[:, 1]).max()), int(wrap > step + 2)))

def pcm(slot):
    return np.frombuffer(L.loop_pcm(slot), "<i2").reshape(-1, 2)[:, 0] / 32768.0

a, b, c = pcm(13), pcm(14), pcm(15)
n = 16 * SR
A, B, C = (np.tile(v, n // len(v) + 1)[:n] for v in (a, b, c))
x = np.zeros(n)
fl = 480
r = np.linspace(0, 1, fl)
sw1 = 4 * SR               # h13's pass end (2 s): h14 by time from its own beat 0
x[:sw1] = A[:sw1]
x[sw1:sw1 + fl] = A[sw1:sw1 + fl] * np.cos(r * np.pi / 2) + B[sw1:sw1 + fl] * np.sin(r * np.pi / 2)
mix0, mix1 = int(6.5 * SR), int(8.5 * SR)  # the crossfader at the middle: each at -3 dB
x[sw1 + fl:mix0] = B[sw1 + fl:mix0]
x[mix0:mix1] = (B[mix0:mix1] + C[mix0:mix1]) * np.sqrt(0.5)
x[mix1:int(10 * SR)] = B[mix1:int(10 * SR)]
cut = int(12.3 * SR)       # a hard cut to h15
x[int(10 * SR):cut] = B[int(10 * SR):cut]
x[cut:] = C[:n - cut]
m = int(0.5 * SR)
x[m:m + 2400] += 0.1 * np.sin(2 * np.pi * L.MARKER_HZ * np.arange(2400) / SR)
x *= 0.6
v = (np.clip(x, -1, 1) * (2 ** 23 - 1)).astype(np.int32)
raw = np.zeros((n, 6), np.uint8)
for k in range(3):
    raw[:, k] = (v >> (8 * k)) & 255
with wave.open("cap.wav", "wb") as w:
    w.setnchannels(2); w.setsampwidth(3); w.setframerate(SR); w.writeframes(raw.tobytes())

res = L.analyze("cap.wav")
print("MARKER %.4f" % res["marker"])
print("SEGMENTS " + " ".join("%.2f-%.2f:%s" % (s, e, sl) for s, e, sl in res["segments"]))
print("CLICKS " + " ".join("%.3f" % t for t, _ in res["clicks"]))
bs = res["bursts"]
print("BEATS13 " + " ".join(str(b[2]) for b in bs if b[0] < 3.99))
print("BEATS14 " + " ".join(str(b[2]) for b in bs if 3.99 <= b[0] < 6.4))
on = [b[0] for b in bs if b[0] < 6.4]
print("GRID %.5f %.5f" % (min(np.diff(on)), max(np.diff(on))))
lv = np.array(res["levels"])
w0, w1 = int(6.7 * SR / L.WIN), int(8.3 * SR / L.WIN)
p = 10 * np.log10(((lv[w0:w1] ** 2).sum(1)).mean() / res["bed"] ** 2)
print("MIXPOWER %.3f h14 %.4f h15 %.4f bed %.4f" % (p, lv[w0:w1, 1].mean(), lv[w0:w1, 2].mean(), res["bed"]))

def judged(events):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        ok = L.judge(events, res)
    return ok, out.getvalue()

# sent 0.5 s: the marker; the PC to h14 at 3.0 s with CC 54 = 1 lands at h13's pass end (4.0 s)
ok, text = judged(dict(kind="pc", marker_sent=0.5, first=13, events=[dict(mode=1, slot=14, sent=3.0)]))
print("JUDGE-PASSEND %s" % ok)
ok, text = judged(dict(kind="pc", marker_sent=0.5, first=13, events=[dict(mode=2, slot=14, sent=3.0)]))
print("JUDGE-WRONGPASS %s" % ok)
ok, text = judged(dict(kind="pc", marker_sent=0.5, first=13, events=[dict(mode=0, slot=14, sent=3.95)]))
print("JUDGE-ATONCE %s" % ok)
ok, text = judged(dict(kind="pc", marker_sent=0.5, first=13, events=[dict(mode=0, slot=14, sent=3.0)]))
print("JUDGE-LATE %s" % ok)
)";

int main()
{
    firmware_dir = std::string(__FILE__).substr(0, std::string(__FILE__).rfind('/')) + "/..";
    const std::string venv = std::string(getenv("HOME") ? getenv("HOME") : "") + "/.venvs/frizz/bin";
    if (access((venv + "/python3").c_str(), X_OK) != 0)
    {
        printf("SKIP: local only (numpy in %s): loopcap's analysis\n", venv.c_str());
        return Finish();
    }
    setenv("PATH", (venv + ":" + std::string(getenv("PATH") ? getenv("PATH") : "")).c_str(), 1);
    char dir[] = "/tmp/frizz-loopcap-XXXXXX";
    tmp_dir = mkdtemp(dir);
    std::string out;
    const int rc = Python(kCode, out);
    printf("%s%s", out.c_str(), rc ? "\n(the Python above exited with an error)\n" : "");
    auto line = [&](const std::string& key) {
        const size_t at = out.find(key + " ");
        return at == std::string::npos ? std::string() : out.substr(at + key.size() + 1, out.find('\n', at) - at - key.size() - 1);
    };

    Check(Has(out, "LOOP h13 frames 96000 beats 4 bpm 120.0 right 0 wrap") && Has(out, "LOOP h14 frames 192000 beats 8 bpm 120.0 right 0")
              && Has(out, "LOOP h15 frames 115200 beats 4 bpm 100.0 right 0"),
          "loopcap: the loops' lengths, their acid chunk's beats and tempo, the right channel silent");
    bool wraps = true;
    for (const char* s : {"h13", "h14", "h15"})
    {
        const std::string l = line(std::string("LOOP ") + s);
        wraps &= !l.empty() && l.substr(l.rfind(' ') + 1) == "0";
    }
    Check(wraps, "loopcap: each loop's end meets its start (a whole number of the bed's cycles)");
    const double marker = atof(line("MARKER").c_str());
    Check(marker > .498 && marker < .503, "loopcap: the marker's onset within 2 ms");
    Check(line("SEGMENTS").rfind("0.00-", 0) == 0 && Has(line("SEGMENTS"), ":13 ") && Has(line("SEGMENTS"), ":14 ")
              && Has(line("SEGMENTS"), ":15"),
          "loopcap: what plays when: h13, h14 (the crossfader's middle in it), h15");
    Check(Has(line("SEGMENTS"), "4.0") && Has(line("SEGMENTS"), "12.30-"),
          "loopcap: the switch at the pass end and the hard cut, each where it was, to 10 ms");
    const std::string clicks = line("CLICKS");
    Check(Has(clicks, "12.29") || Has(clicks, "12.30"), "loopcap: the hard cut is a transient");
    Check(!Has(clicks, "4.0") && !Has(clicks, "6.5") && !Has(clicks, "8.5") && !Has(clicks, "0.5"),
          "loopcap: the 10 ms crossfade, the fader's mix and the marker aren't");
    Check(line("BEATS13") == "0 1 2 3 0 1 2 3" && line("BEATS14").rfind("0 1 2 3", 0) == 0,
          "loopcap: the bursts' beats by their pitch, h14 from its own beat 0 after the pass end");
    const std::string grid = line("GRID");
    Check(atof(grid.c_str()) > .4995 && atof(grid.substr(grid.find(' ') + 1).c_str()) < .5005,
          "loopcap: the beat grid to a twentieth of a ms (0.5 s at 120 BPM)");
    const std::string mix = line("MIXPOWER");
    Check(fabs(atof(mix.c_str())) < .1, "loopcap: two loops at -3 dB each sum to one loop's power, within 0.1 dB");
    Check(Has(out, "JUDGE-PASSEND True") && Has(out, "JUDGE-WRONGPASS False"),
          "loopcap: CC 54 = 1 judged at the pass end it landed on; 2 isn't");
    Check(Has(out, "JUDGE-ATONCE True") && Has(out, "JUDGE-LATE False"),
          "loopcap: at once judged against the send time: 50 ms after it passes, 1 s after it doesn't");
    return Finish();
}
