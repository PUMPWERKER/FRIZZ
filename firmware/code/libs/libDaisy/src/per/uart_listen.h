// FRIZZ: the MIDI jack's listening, its USART's part (uart.cpp's DmaListenStart), on its own
// for the host check (test/uartlisten.cpp, against registers of its own). After stm32h7xx_hal.h,
// which has the register bits
#pragma once
#include <stdint.h>

namespace daisy
{
/** A MIDI clock tick (0xF8) hands over what came so far at once, as IDLE does (#322): the
 *  character match. ADD is written with the USART off, and only the first time: CR2 keeps it
 *  (HAL_UART_Init's UART_SetConfig only writes STOP there), and turning UE off stops the
 *  transmitter too, cutting the byte MIDI out is sending, which a restart after a receive
 *  error did (#442). The match's flag cleared, and the receive errors' too, as turning UE
 *  off did: bytes that came in while the reception was ended leave an overrun behind, which
 *  HAL_UART_Receive_DMA doesn't clear, and would end the new one at once. Its interrupt on */
template <typename Usart>
inline void UartListenMatchTick(Usart* usart)
{
    const uint32_t add = (0xF8U << USART_CR2_ADD_Pos) | USART_CR2_ADDM7;
    if((usart->CR2 & (USART_CR2_ADD | USART_CR2_ADDM7)) != add)
    {
        const uint32_t cr1 = usart->CR1;
        usart->CR1         = cr1 & ~USART_CR1_UE;
        usart->CR2 = (usart->CR2 & ~(USART_CR2_ADD | USART_CR2_ADDM7)) | add;
        usart->CR1 = cr1;
    }
    usart->ICR = USART_ICR_CMCF | USART_ICR_ORECF | USART_ICR_NECF | USART_ICR_FECF
                 | USART_ICR_PECF;
    usart->CR1 = usart->CR1 | USART_CR1_CMIE;
}
} // namespace daisy
