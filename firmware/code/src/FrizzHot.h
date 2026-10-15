/** @file FrizzHot.h
 *  @brief FRIZZ_HOT: the per-sample code, to run from ITCM (chompi_sram.lds), the core's
 *  fastest code memory, which has no cache to miss: as the code grew past the 16KB I-cache,
 *  everything in the audio callback slowed (docs/CAPACITY.md). main() copies it there before
 *  the audio starts (CopyItcm, chompi_main.cpp).
 *
 *  Off unless built with make ITCM=1: the first device run with it hung (#51), so it stays an
 *  experiment until that's understood. Off, FRIZZ_HOT is nothing and the code is as before.
 */
#pragma once

#if defined(__arm__) && defined(FRIZZ_ITCM)
#define FRIZZ_HOT __attribute__((section(".itcmram"), noinline))
// AudioCallback's: in a section of its own, as the inline ones are COMDAT and it isn't
#define FRIZZ_HOT_CALLBACK __attribute__((section(".itcmram.callback")))
// FxChain::Run's, in FxChain.cpp, which isn't inline either (FxChainRun.h, #286)
#define FRIZZ_HOT_RUN __attribute__((section(".itcmram.fxchain"), noinline))
#else
#define FRIZZ_HOT
#define FRIZZ_HOT_CALLBACK
#define FRIZZ_HOT_RUN
#endif

// FRIZZ_AFTER_RUN: an effect's out-of-line per-sample code, linked at the end of the code
// (.frizz_after_run, chompi_sram.lds) instead of before FxChain::Run: its size then doesn't
// move Run against the code it calls. The shifter's Hermite reads (#305) moved Run 796 B,
// which cost the delay a point of the block by the I-cache alone (#308). Off with ITCM=1,
// where FRIZZ_HOT puts the same code in .itcmram
#if defined(__arm__) && !defined(FRIZZ_ITCM)
#define FRIZZ_AFTER_RUN __attribute__((section(".frizz_after_run")))
#else
#define FRIZZ_AFTER_RUN
#endif
