#pragma once

#include <stdint.h>
#include <string.h>
#include <algorithm>

namespace chompi
{
    // the data chunk among a WAV's first n bytes: where it starts, its bytes in whole stereo frames
    inline bool WavData(const uint8_t *head, uint32_t n, uint32_t file_size, uint32_t &at, uint32_t &size)
    {
        for(uint32_t pos = 0; pos + 8 <= n; pos++)
            if(!memcmp(head + pos, "data", 4))
            {
                at = pos + 8;
                size = head[pos + 4] | head[pos + 5] << 8 | head[pos + 6] << 16 | uint32_t(head[pos + 7]) << 24;
                const uint32_t rest = file_size > at ? file_size - at : 0;
                if(!size || size > rest) // 0: a header that was never finished
                    size = rest;
                size -= size % 4;
                return true;
            }
        return false;
    }
}
