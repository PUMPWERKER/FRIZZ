// daisy_core.h for usbdevice.cpp: libDaisy's s242f, as it is (code/libs/libDaisy/src/daisy_core.h)
#pragma once
#include <cstdint>
#define S242F_SCALE 1.192092896e-07f
#define S24SIGN 0x800000
inline float s242f(int32_t x)
{
    x = (x ^ S24SIGN) - S24SIGN;
    return (float)x * S242F_SCALE;
}
