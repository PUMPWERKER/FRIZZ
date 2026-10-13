/** @file SdCard.h
 *  @brief The SDMMC's settings that libDaisy's SdmmcHandler::Config has no field for, reads
 *  of whole runs of sectors past FatFs, for the loop loader (LoopLoader.h), and FatFs's own
 *  reads and writes (SdDisk.h, SdDiskLink). libDaisy's sdmmc.cpp and sd_diskio.c are vendored
 *  and stay as they are (THIRD_PARTY.md); their handle, hsd1, is used here. On the twin the
 *  card is test/host/fatfs.h's, raw reads too.
 */
#pragma once
#include "SdDisk.h"
#ifdef __arm__
#include "daisy_seed.h"
#include "ff_gen_drv.h"
#include "util/sd_diskio.h"

extern "C" SD_HandleTypeDef hsd1;
extern "C" uint8_t BSP_SD_Init(void);
extern "C" Disk_drvTypeDef disk;
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

/** SdDisk.h's port: the card's HAL, as sd_diskio.c drives it */
struct SdHalPort
{
    static uint32_t Ms() { return HAL_GetTick(); }
    static bool Idle() { return hsd1.State == HAL_SD_STATE_READY; }
    static bool Transferring() { return HAL_SD_GetCardState(&hsd1) == HAL_SD_CARD_TRANSFER; }
    static uint32_t Error() { return hsd1.ErrorCode; }
    static void Clean(uintptr_t from, int32_t size) { SCB_CleanDCache_by_Addr(reinterpret_cast<uint32_t*>(from), size); }
    static void Invalidate(uintptr_t from, int32_t size)
    {
        SCB_InvalidateDCache_by_Addr(reinterpret_cast<uint32_t*>(from), size);
    }
    static bool Start(bool write, uint8_t* buf, uint32_t sector, uint32_t count)
    {
        return (write ? HAL_SD_WriteBlocks_DMA(&hsd1, buf, sector, count)
                      : HAL_SD_ReadBlocks_DMA(&hsd1, buf, sector, count))
               == HAL_OK;
    }
    static sddisk::Io Poll()
    {
        if (hsd1.State == HAL_SD_STATE_BUSY)
            return sddisk::BUSY;
        // the interrupt sets the error before the state
        return hsd1.ErrorCode == HAL_SD_ERROR_NONE ? sddisk::DONE : sddisk::FAILED;
    }
    /** A transfer stopped, as the HAL's interrupt does on a data error: the FIFO emptied (with
     *  flow control a full one holds the card's clock), the data path aborted with CMD12, the
     *  DMA off. HAL_SD_Abort stops neither the data path nor the FIFO */
    static void Abort()
    {
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
    }
    /** The card set up from scratch, as at boot: flow control on again with it (SdFlowControl) */
    static bool Reinit()
    {
        HAL_SD_DeInit(&hsd1);
        return BSP_SD_Init() == 0 && HAL_SD_GetCardState(&hsd1) == HAL_SD_CARD_TRANSFER;
    }
};

/** After a failed or overdue transfer: stopped, and the card back (sddisk::Recover: waited for,
 *  else set up again from scratch, or always with force, a test's), saying so in reinit. A card
 *  that doesn't answer at all makes each command wait the HAL's 5 s. True once it's ready */
inline bool SdRawRecover(bool& reinit, bool force = false) { return sddisk::Recover<SdHalPort>(reinit, force); }

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

inline DRESULT SdDiskRead(BYTE, BYTE* buf, DWORD sector, UINT count)
{
    return sddisk::Transfer<SdHalPort>(false, buf, sector, count) ? RES_OK : RES_ERROR;
}
inline DRESULT SdDiskWrite(BYTE, const BYTE* buf, DWORD sector, UINT count)
{
    return sddisk::Transfer<SdHalPort>(true, const_cast<BYTE*>(buf), sector, count) ? RES_OK : RES_ERROR;
}

/** After fsi.Init: FatFs's driver for the card with SdDisk.h's reads and writes, the rest
 *  sd_diskio.c's (SD_Driver) */
inline void SdDiskLink(const char* path)
{
    static const Diskio_drvTypeDef driver = {SD_Driver.disk_initialize, SD_Driver.disk_status, SdDiskRead,
                                             SdDiskWrite, SD_Driver.disk_ioctl};
    disk.drv[path[0] - '0'] = &driver;
}
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
inline void SdDiskLink(const char*) {}
#endif
