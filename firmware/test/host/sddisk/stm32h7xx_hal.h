// The part of the STM32H7 HAL that libDaisy's sd_diskio.c uses, for sddisk.cpp, which runs it
// on the host against a card in memory. The functions are sddisk.cpp's
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define BLOCKSIZE ((uint32_t)512U)
uint32_t HAL_GetTick(void);
void SCB_CleanDCache_by_Addr(uint32_t* addr, int32_t dsize);
void SCB_InvalidateDCache_by_Addr(uint32_t* addr, int32_t dsize);
#ifdef __cplusplus
}
#endif
