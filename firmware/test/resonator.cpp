// Resonator check: its ringing on the note (#303). The resonator (FxResonator.h) alone in the
// whole FxChain, feedback .97, an impulse in, and the ringing's pitch by autocorrelation from
// 0.125 to 0.75 s after it, as the research measured it (#303) but with an impulse for its
// noise burst: a burst's random spectrum moves the pitch by up to 10 cents from one seed to the
// next where the partials disagree, an impulse rings them all alike.
//  - on every coarse step of the pitch knob (the notes, F#0 to A5) the ringing is within 5
//    cents of the note at the tone knob's 0, .25, .5, .7 (its default) and 1, wherever it still
//    rings in that window (above -125 dB of the impulse: not the high notes with the tone at
//    the bottom, whose fundamental the lowpass takes away on every trip); until the fix the
//    lowpass's lag made it ring flat, 67 cents at 440 Hz with the tone at the bottom
//  - the right channel with the stereo at the top, the notes an octave up to 1760 Hz, as well
//  - the loop is never shorter than a block, whatever the knobs (Resonator::Delay)
// Resonator::Retune's table is fitted to the first two (FxResonator.h). Run by unit.sh resonator.
#include <cmath>
#include <cstdio>
#include <vector>
#include "check.h"
// the resonator's range and its loop's length
#define private public
#include "FxChain.h"
#undef private
#include "FxParams.h"

using namespace chompi;
using Res = chompi::Resonator;

static const float kSr = 48000.f;
static const float kFeedback = .97f;
static const size_t kImpulseAt = 24000;      // 0.5 s in
static const size_t kFrom = 30000, kLen = 30000; // the window: 0.125 to 0.75 s after it
static const float kImpulse = .25f;
static const double kRings = -125.;  // dB of the impulse, the window's mean square: it rings
static const double kCents = 5.;

/** The chain and its buffers, as the engine gives them (passthroughEngine.h) */
struct Rig
{
    static const size_t kDelayFrames = 480000, kFreezerFrames = 240000, kTapeFrames = 1u << 19;
    std::vector<float> delay = std::vector<float>(kDelayFrames * 2);
    std::vector<float> fz_l = std::vector<float>(kFreezerFrames), fz_r = std::vector<float>(kFreezerFrames);
    std::vector<float> tape_l = std::vector<float>(kTapeFrames), tape_r = std::vector<float>(kTapeFrames);
    daisysp::Reverb reverb;
    FxChain chain;
};

struct Ring
{
    double cents; // off the note
    double level; // the window's mean square, dB of the impulse
};

/** The resonator alone at pitch (the knob), tone and stereo, rung by an impulse: how far off
 *  hz the channel ch rings */
static Ring Measure(float pitch, float tone, float stereo, size_t ch, double hz)
{
    static Rig rig;
    FxChain& c = rig.chain;
    c.Init(kSr, rig.delay.data(), Rig::kDelayFrames, &rig.reverb, rig.fz_l.data(), rig.fz_r.data(),
           Rig::kFreezerFrames, rig.tape_l.data(), rig.tape_r.data(), Rig::kTapeFrames);
    c.SetTempo(120.f, 2000.f);
    c.SetParam(FX_RESONATOR, Res::PITCH, pitch);
    c.SetParam(FX_RESONATOR, Res::FEEDBACK, kFeedback / .98f); // the knob's top is .98
    c.SetParam(FX_RESONATOR, Res::TONE, tone);
    c.SetParam(FX_RESONATOR, Res::STEREO, stereo);
    c.SetOn(FX_RESONATOR, true);
    const size_t B = 24, n = ((kFrom + kLen + 3000) / B + 1) * B;
    std::vector<float> out(n);
    for (size_t i = 0; i < n; i += B)
    {
        float l[B], r[B];
        for (size_t k = 0; k < B; k++)
            l[k] = r[k] = i + k == kImpulseAt ? kImpulse : 0.f;
        float unused;
        c.Block(&unused);
        c.Process(l, r, B);
        for (size_t k = 0; k < B; k++)
            out[i + k] = ch ? r[k] : l[k];
    }
    double e = 0.;
    for (size_t i = kFrom; i < kFrom + kLen; i++)
        e += out[i] * out[i];
    Ring ring;
    ring.level = 10. * log10(e / kLen / (kImpulse * kImpulse) + 1e-30);
    // the autocorrelation's peak within a third of the period, between frames by a parabola
    const double period = kSr / hz;
    const int lo = int(period * .75), hi = int(period * 1.33) + 2;
    std::vector<double> ac(hi + 2, 0.);
    for (int lag = lo - 1; lag <= hi + 1; lag++)
        for (size_t i = kFrom; i < kFrom + kLen; i++)
            ac[lag] += out[i] * out[i + lag];
    int best = lo;
    for (int lag = lo; lag <= hi; lag++)
        if (ac[lag] > ac[best])
            best = lag;
    const double y0 = ac[best - 1], y1 = ac[best], y2 = ac[best + 1];
    const double d = .5 * (y0 - y2) / (y0 - 2. * y1 + y2);
    ring.cents = 1200. * log2(kSr / (best + d) / hz);
    return ring;
}

/** Every coarse step of the pitch at each tone, the left channel or (stereo at the top) the
 *  right an octave up: each within kCents wherever it rings; prints the worst per tone and
 *  each step off */
static void OnTheNote(size_t ch)
{
    const FxGrid& grid = kFxGrids[kFxParams[FX_RESONATOR].coarse[Res::PITCH]];
    const float tones[] = {0.f, .25f, .5f, .7f, 1.f};
    const float stereo = ch ? 1.f : 0.f;
    for (float tone : tones)
    {
        int steps = 0, rung = 0, off = 0;
        double worst = 0., worst_hz = 0., to_hz = 0.;
        for (float k = ceilf(-grid.origin / grid.spacing - .01f);; k++)
        {
            float pitch = grid.origin + k * grid.spacing;
            if (pitch > 1.f + .01f * grid.spacing)
                break;
            pitch = fminf(pitch, 1.f);
            const double hz = Res::kLowestHz * pow(Res::kHighestHz / Res::kLowestHz, pitch) * (ch ? 2. : 1.);
            steps++;
            const Ring ring = Measure(pitch, tone, stereo, ch, hz);
            if (!(ring.level > kRings) || !std::isfinite(ring.cents))
                continue;
            to_hz = hz;
            rung++;
            if (fabs(ring.cents) > kCents)
            {
                printf("      %7.1f Hz: %+.1f cents\n", hz, ring.cents);
                off++;
            }
            if (fabs(ring.cents) > fabs(worst))
            {
                worst = ring.cents;
                worst_hz = hz;
            }
        }
        char what[200];
        snprintf(what, sizeof what,
                 "%s, tone %.2f: %d of the %d notes ring (to %.1f Hz), %d off by more than %g "
                 "cents; the worst %+.1f at %.1f Hz",
                 ch ? "right channel an octave up" : "left channel", tone, rung, steps, to_hz, off,
                 kCents, worst, worst_hz);
        Check(rung > 0 && off == 0, what);
    }
}

/** The loop is longer than a block (Feed reads what earlier blocks wrote) at the top of the
 *  pitch and stereo, whatever the tone: Retune takes the most off with it at the bottom */
static void LongerThanABlock()
{
    static Res res;
    res.Init(kSr);
    res.SetParam(Res::PITCH, 1.f);
    res.SetParam(Res::STEREO, 1.f);
    float shortest = 1e9f;
    for (float tone = 0.f; tone <= 1.f; tone += .125f)
    {
        res.SetParam(Res::TONE, tone);
        shortest = fminf(shortest, fminf(res.delay_[0].target, res.delay_[1].target));
    }
    char what[120];
    snprintf(what, sizeof what, "the loop at 1760 Hz at least a block and a frame long: %.2f frames",
             shortest);
    Check(shortest >= kFxMaxBlock + 1.f, what);
}

int main()
{
    OnTheNote(0);
    OnTheNote(1);
    LongerThanABlock();
    return Finish();
}
