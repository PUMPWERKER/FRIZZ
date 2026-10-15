// Clicks check: knob moves and key presses that used to jump the sound, on a 220Hz sine.
// Built and run by unit.sh clicks.
//  - the flanger's stereo knob turned back to 0 (as a recall or morph does): the right
//    channel's sweep glides back onto the left one instead of jumping there
//  - the same as a scene recall does it, at the recall's fast slew, after 200 drift times:
//    however far apart the LFOs have drifted, the glide moves the right delay no faster than
//    a small pitch bend (#130)
//  - the freezer pressed again while its release fades the loop out: the loop plays on and
//    hands over to the new capture, instead of dropping to the live signal at once
//  - the filter's LFO division changed at full depth and high resonance: the cutoff jumps,
//    the output steps no further than the sweep does (a guard; it never clicked)
//  - the crusher and the filter punched in after they've gone to sleep (FxGate::Asleep), at
//    their harshest: no step and no peak in the first 100ms beyond what they make settled
//  - the finalizer (#345) switching what changes its phase or latency: the bands (an EQ
//    turned off its centre and back), the drive on and off, Loud's 1 ms delay in and out, its
//    bypass, a band muted and unmuted: each fades, no step beyond the sine's own (a cut
//    straight to the 1 ms later signal would step by up to .6)
//  - every insert and the resonator punched in from asleep through the chain, in the
//    device's runs of 24 samples (FxChain::Insert), on a 220Hz and a 50Hz sine: the same;
//    the shifter with its feedback at .8 and .95 on the 50Hz too, building up from the
//    punch-in (#249)
//  - the filter, crusher, shifter, wow & flutter and flanger punched in again and again
//    through the chain, at a hand's speed and inside the key's ~5ms fade: no step or peak
//    beyond one punch-in's (#249; the flanger inside the fade is #392)
#include <cmath>
#include <vector>
#include "Finalizer.h"
#include "FxChain.h"
#include "check.h"

using namespace chompi;

static const float kSr = 48000.f;
// a 220Hz sine at .5 moves at most .0144 a sample; a flanger's sweep adds little to that
static const float kMaxStep = .05f;

static float Sine(size_t i, float hz = 220.f)
{
    return .5f * sinf(2.f * static_cast<float>(M_PI) * hz * static_cast<float>(i) / kSr);
}

// what the FX's clock pulses: 12 PPQN at 120 BPM
static const size_t kPulseSamples = 2000;

static void NoPulse(size_t) {}

// the last output, so a step across two Runs (a press between them) counts too
static float last_l = 0.f, last_r = 0.f;

/** Runs n samples of the sine through fx from sample i on, with pulse(position) on every
 *  clock pulse; the largest step between two output samples, either channel, the one from
 *  the last Run's end included unless i is 0 */
template <class Fx, class Pulse = void (*)(size_t)>
static float Run(Fx& fx, size_t& i, size_t n, Pulse pulse = NoPulse)
{
    float step = 0.f;
    for (size_t k = 0; k < n; k++, i++)
    {
        if (i % kPulseSamples == 0)
            pulse(i / kPulseSamples);
        float l = Sine(i), r = Sine(i);
        fx.Process(&l, &r);
        if (i > 0)
            step = fmaxf(step, fmaxf(fabsf(l - last_l), fabsf(r - last_r)));
        last_l = l;
        last_r = r;
    }
    return step;
}

static void TestFlanger()
{
    static chompi::Flanger fx;
    fx.Init(kSr);
    fx.SetParam(chompi::Flanger::RATE, .5f);
    fx.SetParam(chompi::Flanger::AMOUNT, .6f);
    fx.SetParam(chompi::Flanger::STEREO, .5f); // the right LFO free and detuned
    fx.SetOn(true);
    size_t i = 0;
    const float before = Run(fx, i, 3 * static_cast<size_t>(kSr)); // the LFOs drift apart
    fx.SetParam(chompi::Flanger::STEREO, 0.f);
    const float after = Run(fx, i, static_cast<size_t>(kSr));
    printf("      flanger: largest step %.4f in stereo, %.4f turning it to 0\n", before, after);
    Check(before < kMaxStep, "flanger: a stereo sweep is smooth");
    Check(after < kMaxStep, "flanger: stereo back to 0 glides, no jump");
}

/** A recall taking stereo back to 0 (FxChain::FastSlew's coefficient for its 2400 samples),
 *  after 1 to 11 s of drift in 50 ms steps: the largest step never much beyond the flanger's
 *  own with stereo at 0 */
static void TestFlangerRecall()
{
    static chompi::Flanger fx;
    float mono = 0.f, worst = 0.f;
    size_t worst_at = 0;
    for (size_t d = 0; d < 200; d++)
    {
        fx.Init(kSr);
        fx.SetParam(chompi::Flanger::AMOUNT, 1.f);
        fx.SetParam(chompi::Flanger::RATE, .5f);
        fx.SetOn(true);
        size_t i = 0;
        Run(fx, i, 24000);
        if (d == 0)
            mono = Run(fx, i, 48000);
        fx.SetParam(chompi::Flanger::STEREO, .5f);
        Run(fx, i, 48000 + d * 2400);
        FxSlew::coeff = kFxRecallCoeff;
        fx.SetParam(chompi::Flanger::STEREO, 0.f);
        float step = Run(fx, i, 2400);
        FxSlew::coeff = kFxParamCoeff;
        step = fmaxf(step, Run(fx, i, 24000));
        if (step > worst)
        {
            worst = step;
            worst_at = 1000 + d * 50;
        }
    }
    printf("      flanger recall: largest step %.4f (after %zu ms of drift), %.4f at stereo 0\n", worst,
           worst_at, mono);
    Check(worst < mono * 1.5f, "flanger: a recall to stereo 0 glides without a click, however far the LFOs drifted");
}

static void TestFreezer()
{
    static const size_t kFrames = 240000;
    static std::vector<float> buf_l(kFrames), buf_r(kFrames);
    static chompi::Freezer fx;
    fx.Init(kSr, buf_l.data(), buf_r.data(), kFrames);
    fx.SetParam(chompi::Freezer::LENGTH, 1.f / 7.f); // 1/8T, which a 220Hz cycle doesn't fit
    fx.SetTempo(120.f);
    auto pulse = [](size_t pos) { fx.ClockPulse(static_cast<uint32_t>(pos)); };
    size_t i = 1; // off the 16th, so the press waits for one
    last_l = last_r = Sine(0);
    fx.SetOn(true);
    // 1.1 s: the loop isn't in phase with the live sine (220Hz fits 1 s exactly)
    const float held = Run(fx, i, static_cast<size_t>(1.1f * kSr), pulse);
    fx.SetOn(false);
    Run(fx, i, 16, pulse); // into the release, where the loop and the live sine differ
    fx.SetOn(true);
    const float again = Run(fx, i, static_cast<size_t>(kSr / 2), pulse);
    printf("      freezer: largest step %.4f repeating, %.4f pressed again in the release\n", held,
           again);
    Check(held < kMaxStep, "freezer: repeating is smooth");
    Check(again < kMaxStep, "freezer: pressed again in the release, no drop to the live signal");
}

static void TestFilter()
{
    static chompi::Filter fx;
    fx.Init(kSr);
    fx.SetParam(chompi::Filter::CUTOFF, .5f);
    fx.SetParam(chompi::Filter::RESONANCE, .9f);
    fx.SetParam(chompi::Filter::LFO_DEPTH, 1.f);
    fx.SetParam(chompi::Filter::LFO_DIVISION, 4.f / 6.f); // 1 bar
    fx.SetPulseSamples(static_cast<float>(kPulseSamples));
    auto pulse = [](size_t pos) { fx.ClockPulse(static_cast<uint32_t>(pos % 192)); };
    size_t i = 0;
    last_l = last_r = 0.f;
    fx.SetOn(true);
    // a quarter bar in, where the 1 bar LFO is at its top and the 1/16's at its centre
    const float sweep = Run(fx, i, 48000 / 4 * 2 + 3000, pulse);
    fx.SetParam(chompi::Filter::LFO_DIVISION, 0.f); // 1/16
    const float turned = Run(fx, i, 4800, pulse);
    printf("      filter: largest step %.4f sweeping, %.4f on a division change\n", sweep, turned);
    Check(turned < 2.f * fmaxf(sweep, kMaxStep), "filter: a division change at full LFO depth doesn't jump");
}

/** On for 1s, off for 1s (asleep by then), on again: the first 100ms against 100ms once
 *  settled, 0.5s later, or the dry input if that's more, by the largest step between two
 *  samples and the peak */
template <class Fx, class Pulse = void (*)(size_t)>
static void PunchIn(Fx& fx, const char* name, float amp, Pulse pulse = NoPulse)
{
    size_t i = 0;
    float step = 0.f, peak = 0.f, last = 0.f;
    const auto Play = [&](size_t n) {
        step = peak = 0.f;
        for (size_t k = 0; k < n; k++, i++)
        {
            if (i % kPulseSamples == 0)
                pulse(i / kPulseSamples);
            float l = amp * Sine(i), r = amp * Sine(i);
            fx.Process(&l, &r);
            step = fmaxf(step, fabsf(l - last));
            peak = fmaxf(peak, fabsf(l));
            last = l;
        }
    };
    fx.SetOn(true);
    Play(48000);
    fx.SetOn(false);
    Play(48000);
    const bool slept = fx.Idle();
    fx.SetOn(true);
    Play(4800);
    const float first_step = step, first_peak = peak;
    Play(24000);
    Play(4800);
    printf("      %s: punched in, largest step %.4f, peak %.4f; settled %.4f, %.4f\n", name,
           first_step, first_peak, step, peak);
    char what[96];
    snprintf(what, sizeof what, "%s: a punch-in from asleep doesn't click or burst", name);
    // the fade-in mixes the dry input out, so that's allowed too: the sine's peak and step
    const float dry_peak = .5f * amp, dry_step = dry_peak * 2.f * static_cast<float>(M_PI) * 220.f / kSr;
    Check(slept && first_step <= 1.5f * fmaxf(step, dry_step) + 1e-4f
              && first_peak <= 1.25f * fmaxf(peak, dry_peak) + 1e-4f,
          what);
}

static void TestPunchIns()
{
    // on a quiet input: the coarsest bits, the lowest rate, the XOR at the top
    static chompi::Crusher crusher;
    crusher.Init(kSr);
    for (size_t p = 0; p < chompi::kNumFxParams; p++)
        crusher.SetParam(p, 1.f);
    PunchIn(crusher, "crusher", .04f);

    // low, at full resonance and LFO depth
    static chompi::Filter filter;
    filter.Init(kSr);
    filter.SetParam(chompi::Filter::CUTOFF, .2f);
    filter.SetParam(chompi::Filter::RESONANCE, 1.f);
    filter.SetParam(chompi::Filter::LFO_DEPTH, 1.f);
    filter.SetParam(chompi::Filter::LFO_DIVISION, 2.f / 6.f);
    filter.SetPulseSamples(static_cast<float>(kPulseSamples));
    PunchIn(filter, "filter", 1.f,
            [](size_t pos) { filter.ClockPulse(static_cast<uint32_t>(pos % 192)); });
}

// ======== punch-ins through the chain ========

static const size_t kDelayFrames = 480000, kFzFrames = 240000, kTapeFrames = 1u << 19;
static float delay_mem[kDelayFrames * 2];
static float fz_l[kFzFrames], fz_r[kFzFrames], tape_l[kTapeFrames], tape_r[kTapeFrames];
static daisysp::Reverb reverb;
static chompi::FxChain chain;

/** One effect through the chain in blocks of 24, as the audio callback runs it: its page-1
 *  knobs and stereo at vals, on for 1s, off for 2s (asleep by then), on again; the first
 *  100ms against the 2s after, as PunchIn (2s: its LFO's whole sweep, a press restarts some) */
static void ChainPunchIn(size_t fx, const char* name, const float (&vals)[5], float amp, float hz)
{
    chain.Init(kSr, delay_mem, kDelayFrames, &reverb, fz_l, fz_r, kFzFrames, tape_l, tape_r,
               kTapeFrames);
    chain.SetTempo(120.f, static_cast<float>(kPulseSamples));
    for (size_t p = 0; p < 4; p++)
        chain.SetParam(fx, p, vals[p]);
    chain.SetParam(fx, 5, vals[4]);
    size_t i = 0;
    float step = 0.f, peak = 0.f, last = 0.f;
    const auto Play = [&](size_t n) {
        step = peak = 0.f;
        for (size_t k = 0; k < n; k += 24)
        {
            float jump;
            if (i % kPulseSamples < 24)
                chain.ClockPulse(static_cast<uint32_t>(i / kPulseSamples % 192));
            chain.Block(&jump);
            float l[24], r[24];
            for (size_t j = 0; j < 24; j++)
                l[j] = r[j] = amp * Sine(i + j, hz);
            chain.Process(l, r, 24);
            for (size_t j = 0; j < 24; j++)
            {
                step = fmaxf(step, fabsf(l[j] - last));
                peak = fmaxf(peak, fabsf(l[j]));
                last = l[j];
            }
            i += 24;
        }
    };
    chain.SetOn(fx, true);
    Play(48000);
    chain.SetOn(fx, false);
    Play(96000);
    const bool slept = chain.Resting(fx);
    chain.SetOn(fx, true);
    Play(4800);
    const float first_step = step, first_peak = peak;
    Play(96000);
    printf("      %s at %.0fHz: punched in, largest step %.4f, peak %.4f; settled %.4f, %.4f%s\n",
           name, hz, first_step, first_peak, step, peak, slept ? "" : " (NOT asleep before)");
    char what[112];
    snprintf(what, sizeof what, "%s: a punch-in from asleep at %.0fHz, in the chain's runs, doesn't click", name, hz);
    const float dry_peak = .5f * amp, dry_step = dry_peak * 2.f * static_cast<float>(M_PI) * hz / kSr;
    Check(slept && first_step <= 1.5f * fmaxf(step, dry_step) + 1e-4f
              && first_peak <= 1.25f * fmaxf(peak, dry_peak) + 1e-4f,
          what);
}

struct ChainCase
{
    size_t fx;
    const char* name;
    float vals[5]; // knobs 1-4, stereo
    float amp;
};
static const ChainCase kChainCases[] = {
    {chompi::FX_SHIFTER, "shifter", {.8f, .5f, .5f, .3f, .5f}, 1.f},
    {chompi::FX_FOLDER, "folder", {.6f, .5f, .7f, .6f, .5f}, 1.f},
    {chompi::FX_CRUSHER, "crusher", {1.f, 1.f, 1.f, 1.f, 1.f}, .08f},
    {chompi::FX_FILTER, "filter", {.2f, 1.f, 1.f, 2.f / 6.f, .5f}, 1.f},
    {chompi::FX_FLANGER, "flanger", {.5f, 1.f, .6f, 0.f, .5f}, 1.f},
    {chompi::FX_PHASER, "phaser", {.3f, .8f, .8f, .5f, .5f}, 1.f},
    {chompi::FX_RESONATOR, "resonator", {.3f, .9f, .5f, .5f, .5f}, .5f},
    {chompi::FX_WARBLE, "warble", {.7f, .5f, .7f, 0.f, .5f}, 1.f},
};

static void TestChainPunchIns()
{
    for (const ChainCase& c : kChainCases)
        for (float hz : {220.f, 50.f})
            ChainPunchIn(c.fx, c.name, c.vals, c.amp, hz);
    // the shifter's feedback past half on a bass (#249): it builds up from the punch-in, no
    // click as it comes in. Held or latched, the chain sees the same press: the latch keeps
    // the key on through its release (FxControls: on is held or latched)
    for (float fb : {.8f, .95f})
    {
        const float vals[5] = {.8f, fb, .5f, .3f, .5f};
        char name[48];
        snprintf(name, sizeof name, "shifter, feedback %.2f", fb);
        ChainPunchIn(chompi::FX_SHIFTER, name, vals, 1.f, 50.f);
    }
}

/** One effect through the chain in runs of 24, its knobs at vals, punched in once as
 *  ChainPunchIn does; then its key off and on again every interval samples, 12 times, as a
 *  hand punching in quickly and over and over, or each press landing while the last one still
 *  fades (~5ms): the largest step and peak through that against the single punch-in's 2.1s,
 *  or the dry input if that's more */
static void ChainRepunch(const ChainCase& c, float hz, size_t interval, float* step_over, float* peak_over)
{
    chain.Init(kSr, delay_mem, kDelayFrames, &reverb, fz_l, fz_r, kFzFrames, tape_l, tape_r,
               kTapeFrames);
    chain.SetTempo(120.f, static_cast<float>(kPulseSamples));
    for (size_t p = 0; p < 4; p++)
        chain.SetParam(c.fx, p, c.vals[p]);
    chain.SetParam(c.fx, 5, c.vals[4]);
    size_t i = 0;
    float step = 0.f, peak = 0.f, last = 0.f;
    const auto Play = [&](size_t n) {
        step = peak = 0.f;
        for (size_t k = 0; k < n; k += 24)
        {
            float jump;
            if (i % kPulseSamples < 24)
                chain.ClockPulse(static_cast<uint32_t>(i / kPulseSamples % 192));
            chain.Block(&jump);
            float l[24], r[24];
            for (size_t j = 0; j < 24; j++)
                l[j] = r[j] = c.amp * Sine(i + j, hz);
            chain.Process(l, r, 24);
            for (size_t j = 0; j < 24; j++)
            {
                step = fmaxf(step, fabsf(l[j] - last));
                peak = fmaxf(peak, fabsf(l[j]));
                last = l[j];
            }
            i += 24;
        }
    };
    chain.SetOn(c.fx, true);
    Play(48000);
    chain.SetOn(c.fx, false);
    Play(96000);
    chain.SetOn(c.fx, true);
    Play(4800);
    float on_step = step, on_peak = peak;
    Play(96000);
    on_step = fmaxf(on_step, step);
    on_peak = fmaxf(on_peak, peak);
    float worst_step = 0.f, worst_peak = 0.f;
    for (int n = 0; n < 12; n++)
    {
        chain.SetOn(c.fx, false);
        Play(interval);
        worst_step = fmaxf(worst_step, step);
        worst_peak = fmaxf(worst_peak, peak);
        chain.SetOn(c.fx, true);
        Play(interval);
        worst_step = fmaxf(worst_step, step);
        worst_peak = fmaxf(worst_peak, peak);
    }
    Play(4800);
    worst_step = fmaxf(worst_step, step);
    worst_peak = fmaxf(worst_peak, peak);
    const float dry_peak = .5f * c.amp, dry_step = dry_peak * 2.f * static_cast<float>(M_PI) * hz / kSr;
    // the key's fade crossing from the dry input to the wet, which an effect that delays it
    // (wow & flutter, the flanger) has out of phase: at most kFxGateCoeff a sample of the two
    const float cross = kFxGateCoeff * (on_peak + dry_peak);
    *step_over = worst_step / (1.5f * fmaxf(on_step, dry_step) + cross + 1e-4f);
    *peak_over = worst_peak / (1.25f * fmaxf(on_peak, dry_peak) + 1e-4f);
}

/** #249: filter, crusher, shifter, wow & flutter and flanger punched in again and again, at a
 *  hand's speed (20 to 150ms) and inside the key's fade (0.5 to 5ms: a release bouncing
 *  back, MIDI; the key reaches the effect at a run's start): no step or peak beyond what one
 *  punch-in makes, with ChainPunchIn's margins and the fade's own crossing */
static void TestRepunch()
{
    struct Speed
    {
        const char* what;
        size_t intervals[4];
    } speeds[] = {{"at a hand's speed", {960, 1440, 2400, 7200}}, {"inside the key's fade", {24, 48, 120, 240}}};
    for (const ChainCase& c : kChainCases)
    {
        if (c.fx != chompi::FX_FILTER && c.fx != chompi::FX_CRUSHER && c.fx != chompi::FX_SHIFTER
            && c.fx != chompi::FX_WARBLE && c.fx != chompi::FX_FLANGER)
            continue;
        for (const Speed& sp : speeds)
            for (float hz : {220.f, 50.f})
            {
                float worst_step = 0.f, worst_peak = 0.f;
                size_t step_at = 0;
                for (size_t interval : sp.intervals)
                {
                    float st, pk;
                    ChainRepunch(c, hz, interval, &st, &pk);
                    if (st > worst_step)
                    {
                        worst_step = st;
                        step_at = interval;
                    }
                    worst_peak = fmaxf(worst_peak, pk);
                }
                char what[180];
                snprintf(what, sizeof what,
                         "%s: punched in again and again %s at %.0fHz, no click (step %.2f of its bound, every %zu "
                         "samples; peak %.2f)%s",
                         c.name, sp.what, hz, worst_step, step_at, worst_peak,
                         c.fx == chompi::FX_FLANGER && sp.intervals[0] == 24 ? " (#392)" : "");
                const bool ok = worst_step <= 1.f && worst_peak <= 1.f;
                if (c.fx == chompi::FX_FLANGER && sp.intervals[0] == 24)
                    Known(ok, what); // #392: a press restarts the sweep, also while still heard
                else
                    Check(ok, what);
            }
    }
}

/** The finalizer on the 220Hz sine in runs of 24, a change made at 0.5 s, back at 1.5 s: the
 *  largest step of its output over the 3 s */
static float FinStep(void (*change)(Finalizer&, bool))
{
    Finalizer f;
    f.Init(kSr);
    float step = 0.f, last = 0.f;
    float l[24], r[24];
    for (size_t b = 0; b < 3 * 48000 / 24; b++)
    {
        if (b == 1000)
            change(f, true);
        if (b == 3000)
            change(f, false);
        for (size_t i = 0; i < 24; i++)
            l[i] = r[i] = Sine(b * 24 + i);
        f.Process(l, r, 24);
        for (size_t i = 0; i < 24; i++)
        {
            step = fmaxf(step, fabsf(l[i] - last));
            last = l[i];
        }
    }
    return step;
}

static void TestFinalizer()
{
    struct Case
    {
        const char* what;
        void (*change)(Finalizer&, bool);
    };
    const Case cases[] = {
        {"the bands woken and put to sleep (EQ low)", [](Finalizer& f, bool on) { f.SetParam(Finalizer::EQ_LOW, on ? .55f : .5f); }},
        {"the drive on and off", [](Finalizer& f, bool on) { f.SetParam(Finalizer::DRIVE, on ? .05f : 0.f); }},
        {"Loud's 1 ms delay in and out", [](Finalizer& f, bool on) { f.SetParam(Finalizer::LOUD, on ? .01f : 0.f); }},
        {"the bypass, everything on", [](Finalizer& f, bool on) {
             if (on)
             {
                 f.SetParam(Finalizer::DRIVE, .05f);
                 f.SetParam(Finalizer::LOUD, .01f);
                 f.SetParam(Finalizer::WIDTH_LOW + 1, .6f);
                 return;
             }
             f.SetBypass(true);
         }},
        {"a band muted and unmuted (the low band, a 220Hz sine in it)", [](Finalizer& f, bool on) { f.SetMute(0, on); }},
    };
    for (const Case& c : cases)
    {
        const float step = FinStep(c.change);
        char what[200];
        snprintf(what, sizeof(what), "finalizer: %s: no step beyond the sine's (%.4f, at most %.2f)", c.what, step, kMaxStep);
        Check(step < kMaxStep, what);
    }
}

int main()
{
    TestFinalizer();
    TestFlanger();
    TestFlangerRecall();
    TestFreezer();
    TestFilter();
    TestPunchIns();
    TestChainPunchIns();
    TestRepunch();
    return Finish();
}
