// The virtual board: what the host stand-ins for libDaisy's peripherals talk to. The real
// firmware (chompi_main.cpp, hardware.h, encoder.cpp, libDaisy's UI, Switch and 4021 driver)
// is compiled unchanged on top; only the pins, the I2C bus, the LED DMA, the audio driver and
// the clock end up here. twin.cpp implements it.
#pragma once
#include <cstddef>
#include <cstdint>

namespace twin
{
// --- clock ---------------------------------------------------------------------------------
// The audio callback runs at the start of each 0.5 ms block; main() runs as a coroutine
// between them, its own time moving only on System::Delay*, GetNow and DelayTicks
uint64_t NowNs();
/** NowNs() as the firmware's clock reads it: from SetClockStartMs() on */
uint64_t FirmwareNs();
void DelayNs(uint64_t ns);
bool InMain();

// --- pins: port * 16 + pin -----------------------------------------------------------------
void PinWrite(int id, bool level);
bool PinRead(int id);

// --- I2C (the MP2722 charger) --------------------------------------------------------------
void I2cWrite(uint16_t addr, const uint8_t* data, size_t size);
void I2cRead(uint16_t addr, uint8_t* data, size_t size);

// --- LED DMA: one chain per timer channel (2 = the keys' SMT LEDs, 4 = the panel's PTH LEDs)
typedef void (*DmaDone)(void* context);
void LedDmaStart(int channel, const uint32_t* data, size_t size, DmaDone done, void* context);

// --- audio: libDaisy's AudioHandle::AudioCallback ------------------------------------------
typedef void (*AudioCallback)(const float* const* in, float** out, size_t size);
void StartAudio(AudioCallback cb);

// --- MIDI in and out (the TRS jacks' UART) ------------------------------------------------
typedef void (*UartRx)(uint8_t* data, size_t size, void* context);
void UartListen(UartRx rx, void* context);
/** A byte into the UART's transmit register, if it's free: as on the chip it holds one byte
 *  while the one before it goes out, 0.32 ms each at 31250 baud */
bool UartTx(uint8_t byte);

// --- USB MIDI, as raw MIDI bytes ------------------------------------------------------------
void UsbListen(UartRx rx, void* context);
void UsbTx(const uint8_t* data, size_t size);
/** A host has USB configured (a cable to a computer that knows FRIZZ); SetUsbHost (twin.h) */
bool UsbHost();

// --- power ---------------------------------------------------------------------------------
void Stop(); // HAL_PWR_EnterSTOPMode
} // namespace twin
