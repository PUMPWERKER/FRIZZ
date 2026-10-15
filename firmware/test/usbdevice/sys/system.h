// sys/system.h for usbdevice.cpp: the clock CardTask times the card's transfers by, a ms a call
#pragma once
#include <cstdint>
namespace daisy
{
struct System
{
    static uint32_t GetUs()
    {
        static uint32_t us = 0;
        return us += 1000;
    }
};
} // namespace daisy
