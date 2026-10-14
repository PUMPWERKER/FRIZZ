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
