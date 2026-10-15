// for usbdevice.cpp: no interrupts on the host
#pragma once
namespace daisy
{
class ScopedIrqBlocker
{
};
} // namespace daisy
