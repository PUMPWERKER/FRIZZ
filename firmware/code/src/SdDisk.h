/** @file SdDisk.h
 *  @brief FatFs's reads and writes of the card, in place of libDaisy's sd_diskio.c's (#213).
 *  Its SD_read and SD_write wait for the DMA's end and then for the card up to 30 s each
 *  (SD_TIMEOUT), and nothing ends the wait on an error (no HAL_SD_ErrorCallback): an SD error
 *  in a scene's save, the master file, a bug report or the boot's reads froze MainLoop for
 *  30-60 s. These start the same transfer (the same sectors, in the same order, from the same
 *  buffer, its cache lines cleaned before and a read's invalidated after, as sd_diskio.c does),
 *  then poll it as the loop loader's runs do (LoopLoader.h), give each wait kTimeoutMs, and on
 *  an error or a timeout stop the transfer and set the card up again, telling FatFs RES_ERROR.
 *
 *  The logic here, over a port (the card's HAL, SdCard.h's SdHalPort; a card in memory in
 *  test/sddisk.cpp), so the host checks it against sd_diskio.c itself. The rest of FatFs's
 *  driver (initialize, status, ioctl) stays sd_diskio.c's; libDaisy is unchanged.
 */
#pragma once
#include <stdint.h>

namespace sddisk
{
enum Io : uint8_t
{
    BUSY,
    DONE,
    FAILED,
};
// each wait: the HAL ready, the DMA's end, the card back in its transfer state. The SD spec's
// longest write is 250 ms (SDHC), 500 ms (SDXC)
static const uint32_t kTimeoutMs = 1000;

// a fault for a test on the device (LoopLoader's, from the developers' SysEx): on the next read,
// or with kWrites on the next write of more than a sector (FatFs's own data, never the FAT's or
// a folder's, which it writes a sector at a time)
static const uint8_t kCutShort = 1; // given no time: stopped mid-transfer
static const uint8_t kPastEnd = 2;  // past the card's end: the HAL refuses it
static const uint8_t kWrites = 4;
inline uint8_t& Fault()
{
    static uint8_t fault = 0;
    return fault;
}

/** What went wrong since power-on */
struct Stats
{
    uint32_t errors, timeouts, recoveries, reinits;
    uint32_t last_error; // the HAL's (HAL_SD_ERROR_*)
};
inline Stats& GetStats()
{
    static Stats stats = {};
    return stats;
}

/** The cache lines a transfer of bytes at addr touches: from the line addr is in, the size
 *  grown by what's before addr in it (FatFs's buffers needn't be 32-byte aligned), as
 *  sd_diskio.c takes them */
inline void CacheRange(uintptr_t addr, uint32_t bytes, uintptr_t& from, int32_t& size)
{
    from = addr & ~static_cast<uintptr_t>(31);
    size = static_cast<int32_t>(bytes + (addr - from));
}

/** After an error or a timeout: the transfer stopped, the card set up again */
template <class P>
bool Failed(bool timeout)
{
    Stats& s = GetStats();
    s.errors++;
    s.timeouts += timeout;
    s.last_error = P::Error();
    bool reinit = false;
    s.recoveries += P::Recover(reinit);
    s.reinits += reinit;
    return false;
}

/** count sectors read into buf from sector, or written from it; false on an error, after the
 *  card's recovery */
template <class P>
bool Transfer(bool write, uint8_t* buf, uint32_t sector, uint32_t count)
{
    uint8_t fault = Fault();
    if (fault && (fault & kWrites ? write && count > 1 : !write))
        Fault() = 0;
    else
        fault = 0;
    uint32_t t = P::Ms();
    while (!P::Idle())
        if (P::Ms() - t >= kTimeoutMs)
            return Failed<P>(true);
    // the buffer's cache lines cleaned, so none is written back over what the DMA brings or
    // takes; a read's invalidated once it's done
    uintptr_t from;
    int32_t size;
    CacheRange(reinterpret_cast<uintptr_t>(buf), count * 512, from, size);
    P::Clean(from, size);
    if (!P::Start(write, buf, fault & kPastEnd ? 0xF0000000u : sector, count))
        return Failed<P>(false);
    const uint32_t limit = fault & kCutShort ? 0 : kTimeoutMs;
    t = P::Ms();
    Io r;
    while ((r = P::Poll()) == BUSY)
        if (P::Ms() - t >= limit)
            return Failed<P>(true);
    if (r == FAILED)
        return Failed<P>(false);
    if (!write)
        P::Invalidate(from, size);
    // and the card done with it: a write's sectors programmed
    t = P::Ms();
    while (!P::Transferring())
        if (P::Ms() - t >= kTimeoutMs)
            return Failed<P>(true);
    return true;
}
} // namespace sddisk
