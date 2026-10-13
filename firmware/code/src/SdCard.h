/** @file SdCard.h
 *  @brief The SDMMC's settings that libDaisy's SdmmcHandler::Config has no field for, and
 *  reads of whole runs of sectors past FatFs, for the loop loader (LoopLoader.h). libDaisy's
 *  sdmmc.cpp and sd_diskio.c are vendored and stay as they are (THIRD_PARTY.md); their handle,
 *  hsd1, is used here. On the twin the card is test/host/fatfs.h's, raw reads too.
 */
#pragma once
#ifdef __arm__
#include "daisy_seed.h"

extern "C" SD_HandleTypeDef hsd1;
extern "C" uint8_t BSP_SD_Init(void);
#else
#include "fatfs.h"
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

/** Sectors read straight into memory by the SDMMC's DMA, without waiting: SdRawStart starts
 *  count sectors from sector (the card's, as FatFs numbers them) into to (32-byte aligned,
 *  whole sectors), SdRawPoll says how it went. Unlike FatFs's reads (sd_diskio.c), nothing
 *  waits 30 s on an error: the DMA's end or error shows at the next poll. Nothing else may
 *  use the card while one runs (LoopLoader::Process) */
enum class SdRaw : uint8_t
{
    BUSY,
    DONE,
    FAILED,
};

#ifdef __arm__
/** 1 started; 0 the card isn't ready for it yet (still busy with the last); -1 the HAL
 *  refused it (its error in SdRawError) */
inline int SdRawStart(uint8_t* to, uint32_t sector, uint32_t count)
{
    if (hsd1.State != HAL_SD_STATE_READY || HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER)
        return 0;
    // the cache's lines there dropped, so none is written back over what the DMA brings
    SCB_InvalidateDCache_by_Addr(reinterpret_cast<uint32_t*>(to), static_cast<int32_t>(count * 512));
    return HAL_SD_ReadBlocks_DMA(&hsd1, to, sector, count) == HAL_OK ? 1 : -1;
}

/** to, count: as started */
inline SdRaw SdRawPoll(uint8_t* to, uint32_t count)
{
    if (hsd1.State == HAL_SD_STATE_BUSY)
        return SdRaw::BUSY;
    // the interrupt sets the error before the state
    if (hsd1.ErrorCode != HAL_SD_ERROR_NONE)
        return SdRaw::FAILED;
    // and again, for lines read ahead into the cache while the DMA ran
    SCB_InvalidateDCache_by_Addr(reinterpret_cast<uint32_t*>(to), static_cast<int32_t>(count * 512));
    return SdRaw::DONE;
}

/** HAL_SD_ERROR_*: the last transfer's, 0 if none */
inline uint32_t SdRawError() { return hsd1.ErrorCode; }

/** After a failed or overdue transfer: stops it, and sets the card up again from scratch when
 *  it isn't back in its transfer state, or always with force (a test's), saying so in reinit.
 *  A card that doesn't answer at all makes each command wait the HAL's 5 s. True once the card
 *  is ready again */
inline bool SdRawRecover(bool& reinit, bool force = false)
{
    // as the HAL's interrupt does on a data error: the FIFO emptied (with flow control a full
    // one holds the card's clock), the data path aborted with CMD12, the DMA off. HAL_SD_Abort
    // stops neither the data path nor the FIFO
    HAL_NVIC_DisableIRQ(SDMMC1_IRQn);
    __HAL_SD_DISABLE_IT(&hsd1, SDMMC_IT_DATAEND | SDMMC_IT_DCRCFAIL | SDMMC_IT_DTIMEOUT
                                   | SDMMC_IT_TXUNDERR | SDMMC_IT_RXOVERR);
    __SDMMC_CMDTRANS_DISABLE(SDMMC1);
    SDMMC1->DCTRL |= SDMMC_DCTRL_FIFORST;
    SDMMC1->CMD |= SDMMC_CMD_CMDSTOP;
    SDMMC_CmdStopTransfer(SDMMC1); // the card refuses it when it isn't sending: no matter
    SDMMC1->CMD &= ~SDMMC_CMD_CMDSTOP;
    SDMMC1->IDMACTRL = SDMMC_DISABLE_IDMA;
    __HAL_SD_CLEAR_FLAG(&hsd1, SDMMC_STATIC_FLAGS | SDMMC_FLAG_DABORT);
    hsd1.Context = SD_CONTEXT_NONE;
    hsd1.State = HAL_SD_STATE_READY;
    NVIC_ClearPendingIRQ(SDMMC1_IRQn);
    HAL_NVIC_EnableIRQ(SDMMC1_IRQn);
    reinit = false;
    for (int i = 0; i < 3 && !force; i++)
        if (HAL_SD_GetCardState(&hsd1) == HAL_SD_CARD_TRANSFER)
            return true;
    reinit = true;
    HAL_SD_DeInit(&hsd1);
    return BSP_SD_Init() == 0 && HAL_SD_GetCardState(&hsd1) == HAL_SD_CARD_TRANSFER;
}

/** For a test on the device (LoopLoader's faults): flow control off, or on again. Only while
 *  no transfer runs */
inline void SdRawFlowControl(bool on)
{
    if (on)
        SDMMC1->CLKCR |= SDMMC_CLKCR_HWFC_EN;
    else
        SDMMC1->CLKCR &= ~SDMMC_CLKCR_HWFC_EN;
}
inline uint32_t SdRawClkcr() { return SDMMC1->CLKCR; }
#else
inline int SdRawStart(uint8_t* to, uint32_t sector, uint32_t count)
{
    return FakeCard::Get().RawStart(to, sector, count);
}
inline SdRaw SdRawPoll(uint8_t*, uint32_t)
{
    const int r = FakeCard::Get().RawPoll();
    return r == 0 ? SdRaw::BUSY : (r > 0 ? SdRaw::DONE : SdRaw::FAILED);
}
inline uint32_t SdRawError() { return FakeCard::Get().raw_error; }
inline bool SdRawRecover(bool& reinit, bool force = false)
{
    reinit = force;
    return FakeCard::Get().RawRecover();
}
inline void SdRawFlowControl(bool) {}
inline uint32_t SdRawClkcr() { return 0; }
#endif
