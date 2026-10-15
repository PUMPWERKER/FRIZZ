/** @file BenchProfile.h
 *  @brief Where the audio callback's time goes, only in FRIZZ-bench.bin (Bench.h).
 *
 *  BENCH_MARK(part) gives the cycles since the last mark to that part. The marks sit in
 *  AudioCallback and PassthroughEngine::Process; in FRIZZ.bin and on the host's tests they are
 *  nothing. The cycles come from the core's own counter (DWT->CYCCNT), which costs a few
 *  cycles to read, unlike the system timer; on the host there is none and they stay 0.
 */
#pragma once
#include <stdint.h>

#if FRIZZ_BENCH
#define BENCH_MARK(part) chompi::bench_profile.Mark(chompi::BenchProfile::part)
#define BENCH_MARK_FX(fx)                                                                  \
    chompi::bench_profile.Mark(static_cast<chompi::BenchProfile::Part>(chompi::BenchProfile::FX0 + (fx)))
#else
#define BENCH_MARK(part)
#define BENCH_MARK_FX(fx)
#endif

#if FRIZZ_BENCH
#ifdef __arm__
#include "stm32h7xx.h"
#endif

namespace chompi
{

struct BenchProfile
{
    enum Part
    {
        MIDI,     // the MIDI clock
        CONTROLS, // the keys' and knobs' shift registers, the encoders
        EVENTS,   // the UI's events from them
        INPUT,    // the AUX input's gain and DC block
        LOOPER,
        TEMPO,    // the FX's tempo and clock, the scene morph
        FX0,      // the FX chain, one part per effect in FxId's order (FxChain.h), with its meter
        COMP = FX0 + 13, // the finalizer (Finalizer.h; "comp" in cpu.txt, the master compressor until #345)
        OUTPUT,   // the mix, the gains, the meter, the headphones' cue, the four limiters
        kNumParts
    };

    static inline uint32_t Now()
    {
#ifdef __arm__
        return DWT->CYCCNT;
#else
        return 0;
#endif
    }

    /** Once, before the first block: the counter runs only once switched on */
    static void Enable()
    {
#ifdef __arm__
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->LAR = 0xC5ACCE55; // the H7 locks the DWT's registers until this is written
        DWT->CYCCNT = 0;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
#endif
    }

    inline void Start()
    {
        for (int p = 0; p < kNumParts; p++)
            cycles[p] = 0;
        last = start = Now();
    }
    inline void Mark(Part part)
    {
        const uint32_t now = Now();
        cycles[part] += now - last;
        last = now;
    }
    /** The whole callback's cycles, read at its end */
    inline uint32_t Total() const { return Now() - start; }

    uint32_t start = 0, last = 0;
    uint32_t cycles[kNumParts] = {};
};

extern BenchProfile bench_profile;

} // namespace chompi
#endif
