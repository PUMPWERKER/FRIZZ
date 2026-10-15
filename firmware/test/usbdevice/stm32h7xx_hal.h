// stm32h7xx_hal.h for usbdevice.cpp: what UsbDevice.cpp's Init() touches, as plain memory. TIM7's
// registers are kept (the task's rate); APB1 at 120 MHz, divided, as FRIZZ's clock tree has it
#pragma once
#include <cstdint>

struct GPIO_InitTypeDef
{
    uint32_t Pin, Mode, Pull, Speed, Alternate;
};
#define GPIO_PIN_14 (1u << 14)
#define GPIO_PIN_15 (1u << 15)
#define GPIO_MODE_AF_PP 2u
#define GPIO_NOPULL 0u
#define GPIO_SPEED_FREQ_HIGH 2u
#define GPIO_AF12_OTG2_FS 12u
#define GPIOB nullptr
inline void HAL_GPIO_Init(void*, GPIO_InitTypeDef*) {}
#define __HAL_RCC_GPIOB_CLK_ENABLE() ((void)0)
#define __HAL_RCC_USB_OTG_HS_CLK_ENABLE() ((void)0)
#define __HAL_RCC_CRS_CLK_ENABLE() ((void)0)
#define __HAL_RCC_TIM7_CLK_ENABLE() ((void)0)
inline void HAL_PWREx_EnableUSBVoltageDetector() {}

struct RCC_CRSInitTypeDef
{
    uint32_t Prescaler, Source, Polarity, ReloadValue, ErrorLimitValue, HSI48CalibrationValue;
};
#define RCC_CRS_SYNC_DIV1 0u
#define RCC_CRS_SYNC_SOURCE_USB1 (2u << 28)
#define RCC_CRS_SYNC_POLARITY_RISING 0u
#define __HAL_RCC_CRS_RELOADVALUE_CALCULATE(f, s) (((f) / (s)) - 1u)
#define RCC_CRS_ERRORLIMIT_DEFAULT 34u
#define RCC_CRS_HSI48CALIBRATION_DEFAULT 32u
inline void HAL_RCCEx_CRSConfig(RCC_CRSInitTypeDef*) {}

enum IRQn_Type
{
    TIM7_IRQn = 55,
    OTG_HS_IRQn = 77,
};
inline void HAL_NVIC_SetPriority(IRQn_Type, uint32_t, uint32_t) {}
inline void HAL_NVIC_EnableIRQ(IRQn_Type) {}
inline uint32_t HAL_RCC_GetPCLK1Freq() { return 120000000u; }

struct RccStub
{
    uint32_t D2CFGR;
};
#define RCC_D2CFGR_D2PPRE1 (7u << 4)
#define RCC_D2CFGR_D2PPRE1_DIV1 0u
#define RCC_D2CFGR_D2PPRE1_DIV2 (4u << 4)
static RccStub rcc_stub = {RCC_D2CFGR_D2PPRE1_DIV2};
#define RCC (&rcc_stub)

struct TimStub
{
    uint32_t PSC, ARR, EGR, SR, DIER, CR1;
};
#define TIM_EGR_UG 1u
#define TIM_DIER_UIE 1u
#define TIM_CR1_CEN 1u
static TimStub tim7_stub = {};
#define TIM7 (&tim7_stub)
