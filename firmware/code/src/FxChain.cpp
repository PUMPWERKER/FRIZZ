/** @file FxChain.cpp
 *  @brief FxChain::Run, the effects' per-sample code, as a translation unit of its own
 *  (FxChainRun.h, #286). Linked first (Makefile), so the inline functions it shares with
 *  chompi_main.cpp are its own copies, which the build checks (tools/fxchain_tu.py).
 */
#define FRIZZ_FXCHAIN_TU 1
// the effects' buffers defined in headers, here only declared: chompi_main.cpp's
#define FRIZZ_FXCHAIN_CPP 1
#include "FxChainRun.h"
