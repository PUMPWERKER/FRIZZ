// sddisk.cpp: checks SdDisk.h, FRIZZ's reads and writes for FatFs (#213), against what it
// replaces, libDaisy's sd_diskio.c itself, both under FatFs itself (libDaisy's ff.c), on a card in
// memory (FAT32, 4 KB clusters, as the user's).
//   - The same: a scene's save, the master file's, a deletion, a bug report (EventLog's writes,
//     replayed) and writes and reads from buffers that aren't 32-byte aligned make the same
//     transfers (sectors, counts, the data, in the same order), clean and invalidate the same
//     cache lines, and leave the same card, through either.
//   - An error doesn't hold MainLoop: a transfer that never ends, a card that stays busy after
//     a write, a read error, a transfer the HAL refuses, a card that doesn't come back. Each ends
//     within kTimeoutMs and recovers the card; sd_diskio.c waited 30 s for the first two.
//   - The faults for a test on the device (LOOPS: remote.py loops load/write --fault).
// Exits 0 when everything passes. Run by unit.sh sddisk.
//
// unit includes: host/sddisk ../platform/daisy/libs/libDaisy/Middlewares/Third_Party/FatFs/src
// unit sources: ../platform/daisy/libs/libDaisy/Middlewares/Third_Party/FatFs/src/ff.c ../platform/daisy/libs/libDaisy/Middlewares/Third_Party/FatFs/src/diskio.c ../platform/daisy/libs/libDaisy/Middlewares/Third_Party/FatFs/src/ff_gen_drv.c ../platform/daisy/libs/libDaisy/Middlewares/Third_Party/FatFs/src/option/ccsbcs.c ../platform/daisy/libs/libDaisy/src/util/sd_diskio.c

#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "check.h"
#include "fatfs.h"
#include "ff_gen_drv.h"
#include "util/sd_diskio.h"
#include "SdDisk.h"
#include "SceneStore.h"

extern "C" Disk_drvTypeDef disk;

using namespace chompi;

/** The card: its sectors (all zero but those kept), what was asked of it, and its faults */
struct Card
{
    static const uint32_t kSectors = 600000; // ~290 MB: FAT32 at 4 KB clusters
    std::map<uint32_t, std::array<uint8_t, 512>> sectors;
    struct Op
    {
        bool write;
        uint32_t sector, count, crc, align;
        bool operator==(const Op& o) const
        {
            return write == o.write && sector == o.sector && count == o.count && crc == o.crc && align == o.align;
        }
    };
    std::vector<Op> ops;
    std::vector<std::pair<uint32_t, int32_t>> cache; // lines cleaned (size > 0), invalidated (< 0)
    // faults: the next transfer never ends (hang) or fails (fail), the card busy after it
    bool hang = false, fail = false, busy_after = false, dead = false, reinit = false;
    bool stuck = false; // and a recovery doesn't bring it back
    // a write stopped: the card programs what it got until then (ms on the clock), and set up
    // again meanwhile it stays at the setup's 400 kHz and 1 bit (slow), as it did on the device
    uint32_t programming_until = 0;
    uint32_t programs_ms = 300;
    bool writing = false, slow = false;
    int reinits = 0;
    int polls = 2; // a transfer is done on its polls-th poll (the new driver's)
    int left = 0;
    uint32_t error = 0;
    int recoveries = 0;

    static uint32_t Crc(const uint8_t* p, size_t n)
    {
        uint32_t c = 0xFFFFFFFFu;
        for (size_t i = 0; i < n; i++)
        {
            c ^= p[i];
            for (int k = 0; k < 8; k++)
                c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
        }
        return ~c;
    }
    bool Transfer(bool write, uint8_t* buf, uint32_t sector, uint32_t count)
    {
        if (sector >= kSectors || count > kSectors - sector)
        {
            error = 0x2000000; // HAL_SD_ERROR_ADDR_OUT_OF_RANGE
            return false;
        }
        for (uint32_t s = 0; s < count; s++)
        {
            uint8_t* b = buf + s * 512;
            if (write)
            {
                bool zero = true;
                for (int i = 0; i < 512 && zero; i++)
                    zero = !b[i];
                if (zero)
                    sectors.erase(sector + s);
                else
                    memcpy(sectors[sector + s].data(), b, 512);
            }
            else if (sectors.count(sector + s))
                memcpy(b, sectors[sector + s].data(), 512);
            else
                memset(b, 0, 512);
        }
        ops.push_back({write, sector, count, Crc(buf, count * 512), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(buf) % 32)});
        return true;
    }
};
static Card card;
static uint32_t now_ms = 0; // the tick clock: a ms each time it's read

// sd_diskio.c's side: the BSP and HAL calls it makes
extern "C" {
uint32_t HAL_GetTick(void) { return now_ms++; }
void SCB_CleanDCache_by_Addr(uint32_t* addr, int32_t size)
{
    card.cache.push_back({static_cast<uint32_t>(reinterpret_cast<uintptr_t>(addr)), size});
}
void SCB_InvalidateDCache_by_Addr(uint32_t* addr, int32_t size)
{
    card.cache.push_back({static_cast<uint32_t>(reinterpret_cast<uintptr_t>(addr)), -size});
}
uint8_t BSP_SD_Init(void) { return MSD_OK; }
uint8_t BSP_SD_GetCardState(void) { return card.busy_after ? SD_TRANSFER_BUSY : SD_TRANSFER_OK; }
void BSP_SD_GetCardInfo(BSP_SD_CardInfo* info)
{
    memset(info, 0, sizeof(*info));
    info->BlockNbr = info->LogBlockNbr = Card::kSectors;
    info->BlockSize = info->LogBlockSize = 512;
}
static uint8_t OldTransfer(bool write, uint32_t* p, uint32_t sector, uint32_t n)
{
    if (!card.Transfer(write, reinterpret_cast<uint8_t*>(p), sector, n))
        return MSD_ERROR;
    // the DMA's end, as the interrupt calls it; never, if it hangs or fails
    if (!card.hang && !card.fail)
        write ? BSP_SD_WriteCpltCallback() : BSP_SD_ReadCpltCallback();
    return MSD_OK;
}
uint8_t BSP_SD_ReadBlocks_DMA(uint32_t* p, uint32_t sector, uint32_t n) { return OldTransfer(false, p, sector, n); }
uint8_t BSP_SD_WriteBlocks_DMA(uint32_t* p, uint32_t sector, uint32_t n) { return OldTransfer(true, p, sector, n); }
}

/** SdDisk.h's port on the same card */
struct TestPort
{
    static uint32_t Ms() { return now_ms++; }
    static bool Idle() { return true; }
    static bool Transferring() { return !card.busy_after && !card.stuck && !card.dead && now_ms >= card.programming_until; }
    static uint32_t Error() { return card.error; }
    static void Abort()
    {
        card.recoveries++;
        if (card.writing && (card.hang || card.left > 0))
            card.programming_until = now_ms + card.programs_ms;
        if (!card.stuck)
            card.hang = card.fail = false;
    }
    static bool Reinit()
    {
        card.reinits++;
        if (card.dead || card.stuck)
            return false;
        if (now_ms < card.programming_until)
        {
            card.slow = true; // HAL_SD_Init failed on the busy card
            return false;
        }
        card.busy_after = false;
        return true;
    }
    static void Clean(uintptr_t from, int32_t size) { card.cache.push_back({static_cast<uint32_t>(from), size}); }
    static void Invalidate(uintptr_t from, int32_t size) { card.cache.push_back({static_cast<uint32_t>(from), -size}); }
    static bool Start(bool write, uint8_t* buf, uint32_t sector, uint32_t count)
    {
        // refused, nothing goes to the card
        const bool ok = card.Transfer(write, buf, sector, count);
        card.left = ok ? card.polls : 0;
        card.writing = write && ok;
        return ok;
    }
    static sddisk::Io Poll()
    {
        if (card.hang || --card.left > 0)
            return sddisk::BUSY;
        if (card.fail)
            card.error = 0x2; // HAL_SD_ERROR_DATA_CRC_FAIL
        return card.fail ? sddisk::FAILED : sddisk::DONE;
    }
};
static DRESULT NewRead(BYTE, BYTE* buf, DWORD sector, UINT count)
{
    return sddisk::Transfer<TestPort>(false, buf, sector, count) ? RES_OK : RES_ERROR;
}
static DRESULT NewWrite(BYTE, const BYTE* buf, DWORD sector, UINT count)
{
    return sddisk::Transfer<TestPort>(true, const_cast<BYTE*>(buf), sector, count) ? RES_OK : RES_ERROR;
}
static Diskio_drvTypeDef new_driver;

static char path[4];
static FATFS fs;
static std::map<uint32_t, std::array<uint8_t, 512>> formatted;

static void FormatCard()
{
    FATFS_LinkDriver(&SD_Driver, path);
    new_driver = {SD_Driver.disk_initialize, SD_Driver.disk_status, NewRead, NewWrite, SD_Driver.disk_ioctl};
    static BYTE work[4096];
    const FRESULT r = f_mkfs(path, FM_FAT32, 4096, work, sizeof(work));
    Check(r == FR_OK, "card: formatted FAT32, 4 KB clusters");
    formatted = card.sectors;
}

/** A scene saved as a confirm leaves it */
static void SaveInRam(SceneStore& store, size_t slot, float value)
{
    FxScene& scene = store.scenes[slot];
    scene.used = true;
    scene.latched = 0;
    for (size_t fx = 0; fx < kNumFx; fx++)
        for (size_t p = 0; p < kNumFxParams; p++)
            scene.params[fx][p] = kFxParams[fx].defaults[p];
    scene.params[0][0] = value;
}

/** EventLog's writes of a bug report, replayed: the remount, /FRIZZ, the next bug-N.txt free,
 *  its text from a 32-byte aligned buffer in whole sectors as it fills, the rest at the end */
static bool BugReport(size_t bytes)
{
    alignas(32) static char text[4096 + 256];
    if (f_mount(&fs, path, 1) != FR_OK)
        return false;
    EnterFrizzDir();
    char name[24];
    FILINFO info;
    int n = 1;
    for (; n < 1000; n++)
    {
        snprintf(name, sizeof(name), "bug-%d.txt", n);
        if (f_stat(name, &info) != FR_OK)
            break;
    }
    FIL file;
    if (f_open(&file, name, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
        return false;
    size_t pos = 0, done = 0;
    bool ok = true;
    for (int line = 0; done + pos < bytes && ok; line++)
    {
        pos += snprintf(text + pos, 200, "%d key %d down\n", line * 7, line % 15);
        if (pos >= 4096)
        {
            UINT w = 0;
            const size_t chunk = pos - pos % 512;
            ok = f_write(&file, text, chunk, &w) == FR_OK && w == chunk;
            memmove(text, text + chunk, pos - chunk);
            pos -= chunk;
            done += chunk;
        }
    }
    UINT w = 0;
    ok = ok && f_write(&file, text, pos, &w) == FR_OK && w == pos;
    return f_close(&file) == FR_OK && ok;
}

/** A file written from a buffer at an odd address and read back into another */
static bool Unaligned()
{
    static uint8_t out[9000], in[9000];
    for (size_t i = 0; i < sizeof(out); i++)
        out[i] = static_cast<uint8_t>(i * 31 + 7);
    FIL file;
    UINT n = 0;
    bool ok = f_open(&file, "/FRIZZ/u.bin", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK
              && f_write(&file, out + 5, 5000, &n) == FR_OK && n == 5000
              && f_write(&file, out + 5005, 3001, &n) == FR_OK && f_close(&file) == FR_OK;
    ok = ok && f_open(&file, "/FRIZZ/u.bin", FA_READ) == FR_OK && f_read(&file, in + 3, 8001, &n) == FR_OK
         && n == 8001 && f_close(&file) == FR_OK;
    return ok && !memcmp(in + 3, out + 5, 8001);
}

struct Session
{
    std::vector<Card::Op> ops;
    std::vector<std::pair<uint32_t, int32_t>> cache;
    std::map<uint32_t, std::array<uint8_t, 512>> sectors;
    bool ok = false;
};

/** The same session through a driver, from the formatted card */
static Session Play(const Diskio_drvTypeDef* driver)
{
    card.sectors = formatted;
    card.ops.clear();
    card.cache.clear();
    disk.drv[0] = driver;
    static SceneStore store;
    store = SceneStore();
    store.Init(&fs, path); // mounts, makes /FRIZZ
    SaveInRam(store, 1, 0.25f);
    SaveInRam(store, 3, 0.75f);
    store.RequestSave();
    store.Process(false);
    bool ok = store.GetSaveState() == SceneStore::SaveState::OK;
    store.RequestMasterSave();
    store.Process(false);
    ok = ok && !store.TakeMasterFailed();
    store.scenes[3].used = false; // deleted
    store.RequestSave();
    store.Process(false);
    ok = ok && store.GetSaveState() == SceneStore::SaveState::OK;
    ok = ok && BugReport(300 * 1024) && Unaligned();
    // and read again at the next boot
    store = SceneStore();
    store.Init(&fs, path);
    ok = ok && store.scenes[1].used && !store.scenes[3].used;
    return {card.ops, card.cache, card.sectors, ok};
}

static void TestSame()
{
    const Session before = Play(&SD_Driver);
    const Session after = Play(&new_driver);
    Check(before.ok && after.ok, "same: scenes saved, deleted, the master file, a bug report, odd buffers: all through either");
    size_t writes = 0, multi = 0, odd = 0;
    for (const Card::Op& op : after.ops)
    {
        writes += op.write;
        multi += op.write && op.count > 1;
        odd += op.align != 0;
    }
    Check(writes > 100 && multi > 50 && odd > 0, "same: the session writes sectors one at a time and in runs, some from odd addresses");
    Check(before.ops == after.ops, "same: the same transfers, sectors, counts and data, in the same order");
    if (!(before.ops == after.ops))
        printf("      %zu transfers against %zu\n", before.ops.size(), after.ops.size());
    Check(before.cache == after.cache, "same: the same cache lines cleaned before and invalidated after");
    Check(before.sectors == after.sectors, "same: the same card after it");
}

static void TestRange()
{
    uintptr_t from;
    int32_t size;
    sddisk::CacheRange(0x24001005, 512, from, size);
    Check(from == 0x24001000 && size == 517, "cache: from an odd address, its line and the bytes before it in it");
    sddisk::CacheRange(0x24002000, 4096, from, size);
    Check(from == 0x24002000 && size == 4096, "cache: from a line's start, as it is");
}

/** One transfer through a driver from the card as it is: its result and the ms it took */
static bool Through(const Diskio_drvTypeDef* driver, bool write, uint32_t& ms, uint32_t sector = 1000, UINT count = 8)
{
    alignas(32) static BYTE buf[8 * 512];
    const uint32_t t = now_ms;
    const DRESULT r = write ? driver->disk_write(0, buf, sector, count) : driver->disk_read(0, buf, sector, count);
    ms = now_ms - t;
    return r == RES_OK;
}

static void TestErrors()
{
    uint32_t ms = 0;
    const sddisk::Stats s0 = sddisk::GetStats();

    card.hang = true;
    bool ok = Through(&SD_Driver, false, ms);
    card.hang = false;
    Check(!ok && ms >= 30000, "before: a read that never ends held sd_diskio.c 30 s");
    card.hang = true;
    card.recoveries = 0;
    ok = Through(&new_driver, false, ms);
    Check(!ok && ms >= sddisk::kTimeoutMs && ms < sddisk::kTimeoutMs + 10 && card.recoveries == 1,
          "timeout: a read that never ends: RES_ERROR after 1 s, the card recovered");
    Check(Through(&new_driver, false, ms) && ms < 10, "timeout: and the next read goes through");

    card.busy_after = true;
    ok = Through(&SD_Driver, true, ms);
    card.busy_after = false;
    Check(!ok && ms >= 30000, "before: a card busy after a write held sd_diskio.c 30 s");
    card.busy_after = true;
    card.reinits = 0;
    ok = Through(&new_driver, true, ms);
    Check(!ok && ms >= 2 * sddisk::kTimeoutMs && ms < 2 * sddisk::kTimeoutMs + 20 && card.recoveries == 2
              && card.reinits == 1 && !card.slow,
          "timeout: a card busy after a write: RES_ERROR after 1 s, a second's wait, then set up again");

    card.fail = true;
    ok = Through(&new_driver, false, ms);
    Check(!ok && ms < 10 && card.recoveries == 3 && sddisk::GetStats().last_error == 0x2,
          "error: a read error ends it at once, the HAL's error kept, recovered");
    ok = Through(&new_driver, true, ms, Card::kSectors - 2);
    Check(!ok && ms < 10 && card.recoveries == 4, "error: a write the HAL refuses (past the card's end): at once, recovered");

    card.dead = true;
    card.hang = true;
    ok = Through(&new_driver, false, ms);
    card.dead = false;
    const sddisk::Stats& s = sddisk::GetStats();
    Check(!ok && ms < 2 * sddisk::kTimeoutMs + 20 && s.errors - s0.errors == 5 && s.timeouts - s0.timeouts == 3
              && s.recoveries - s0.recoveries == 4,
          "error: a card that doesn't come back: RES_ERROR, not counted recovered; 5 errors, 3 timeouts");
    Check(Through(&new_driver, false, ms), "error: a card back again reads");

    // through FatFs: a save whose write never ends fails in a second, the next one goes through
    card.sectors = formatted;
    disk.drv[0] = &new_driver;
    static SceneStore store;
    store = SceneStore();
    store.Init(&fs, path);
    SaveInRam(store, 2, 0.5f);
    card.hang = true;
    uint32_t t = now_ms;
    store.RequestSave();
    store.Process(false);
    Check(store.GetSaveState() == SceneStore::SaveState::FAILED && now_ms - t < 3 * sddisk::kTimeoutMs,
          "FatFs: a scene's save with a write that never ends fails within seconds, not 30");
    t = now_ms;
    store.RequestSave();
    store.Process(false);
    Check(store.GetSaveState() == SceneStore::SaveState::OK && now_ms - t < 100, "FatFs: the next save goes through");
    store = SceneStore();
    store.Init(&fs, path);
    Check(store.scenes[2].used && store.scenes[2].params[0][0] == 0.5f, "FatFs: and it's there at the next boot");

    // a card that stays gone: every transfer waits its second, and FatFs gives up at the first
    card.hang = card.stuck = true;
    SaveInRam(store, 4, 0.5f);
    t = now_ms;
    store.RequestSave();
    store.Process(false);
    const uint32_t gone = now_ms - t;
    card.hang = card.stuck = false;
    Check(store.GetSaveState() == SceneStore::SaveState::FAILED && gone < 5 * sddisk::kTimeoutMs,
          "FatFs: a save on a card that stays gone fails within seconds");
    if (gone >= 5 * sddisk::kTimeoutMs)
        printf("      %u ms\n", gone);
}

/** A write stopped mid-transfer: the card programs what it got, and set up again meanwhile it
 *  would stay at 400 kHz and 1 bit (found on the device, #213): it's waited for instead */
static void TestWriteStopped()
{
    uint32_t ms = 0;
    card.reinits = 0;
    card.slow = false;
    card.programs_ms = 300;
    sddisk::Fault() = sddisk::kCutShort | sddisk::kWrites;
    bool ok = Through(&new_driver, true, ms, 1000, 8);
    Check(!ok && card.reinits == 0 && !card.slow && ms >= 300 && ms < 320,
          "write stopped: the card waited for while it programs (300 ms), not set up again, not slowed");
    Check(Through(&new_driver, true, ms, 1000, 8) && ms < 10, "write stopped: the next write goes through at once");
    card.hang = true;
    ok = Through(&new_driver, true, ms, 1000, 8);
    Check(!ok && card.reinits == 0 && !card.slow && ms >= sddisk::kTimeoutMs + 300 && ms < sddisk::kTimeoutMs + 320,
          "write stopped: one that never ended too, after its second");
    // one that programs longer than the wait (the SD spec's longest is 500 ms): set up again,
    // which fails while it's busy; said so, not counted recovered
    card.programs_ms = 3 * sddisk::kTimeoutMs;
    const sddisk::Stats before = sddisk::GetStats();
    sddisk::Fault() = sddisk::kCutShort | sddisk::kWrites;
    ok = Through(&new_driver, true, ms, 1000, 8);
    const sddisk::Stats& after = sddisk::GetStats();
    Check(!ok && card.reinits == 2 && after.recoveries == before.recoveries && after.reinits == before.reinits + 1,
          "write stopped: programming past the wait: set up again twice, failed, not counted recovered");
    card.programs_ms = 300;
    card.programming_until = 0;
    card.slow = false;
}

static void TestFaults()
{
    uint32_t ms = 0;
    card.recoveries = 0;
    sddisk::Fault() = sddisk::kCutShort;
    bool ok = Through(&new_driver, false, ms);
    Check(!ok && sddisk::Fault() == 0 && card.recoveries == 1, "fault: a read cut short: stopped, recovered, used up");
    Check(Through(&new_driver, false, ms), "fault: the next read goes through");
    sddisk::Fault() = sddisk::kPastEnd;
    size_t ops = card.ops.size();
    Check(!Through(&new_driver, false, ms) && card.recoveries == 2 && card.ops.size() == ops,
          "fault: a read past the card's end: refused, recovered");
    sddisk::Fault() = sddisk::kCutShort | sddisk::kWrites;
    Check(Through(&new_driver, false, ms) && Through(&new_driver, true, ms, 1000, 1) && sddisk::Fault() != 0,
          "fault: a write's waits for a write of more than a sector, past reads and one sector's");
    Check(!Through(&new_driver, true, ms, 1000, 8) && sddisk::Fault() == 0 && card.recoveries == 3,
          "fault: and stops that one, recovered");
    sddisk::Fault() = sddisk::kPastEnd | sddisk::kWrites;
    ops = card.ops.size();
    Check(!Through(&new_driver, true, ms, 1000, 8) && card.ops.size() == ops && card.recoveries == 4,
          "fault: a write past the card's end: refused, nothing written");
}

int main()
{
    TestRange();
    FormatCard();
    TestSame();
    TestErrors();
    TestWriteStopped();
    TestFaults();
    return Finish();
}
