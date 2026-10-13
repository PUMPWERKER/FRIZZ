/** @file SdCard.h
 *  @brief The SDMMC's settings that libDaisy's SdmmcHandler::Config has no field for. Its
 *  sdmmc.cpp is vendored and stays as it is (THIRD_PARTY.md); its handle, hsd1, is set here
 *  between SdmmcHandler::Init and the first mount. Nothing of it on the twin, whose card is
 *  test/host/fatfs.h.
 */
#pragma once
#ifdef __arm__
#include "daisy_seed.h"

extern "C" SD_HandleTypeDef hsd1;
#endif

/** Hardware flow control: the card's clock stops while the SDMMC's FIFO is full (reading) or
 *  empty (writing), so its DMA can't overrun or underrun while the AXI bus and the SDRAM are
 *  busy. Without it, a read into SDRAM under a loop and 13 effects overran (#70). libDaisy
 *  sets it off; set here before the mount, HAL_SD_InitCard still identifies the card without
 *  it, and HAL_SD_ConfigWideBusOperation (the mount's BSP_SD_Init) copies it into CLKCR for
 *  every transfer after */
inline void SdFlowControl()
{
#ifdef __arm__
    hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_ENABLE;
#endif
}
