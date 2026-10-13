/** @file Crash.h
 *  @brief What a crash leaves for the next start, and the watchdog that wakes on a dead
 *  audio callback (#105). For the developers' SysEx (MidiControl.h's dev::CRASH), not players.
 *
 *  A fault (HardFault, MemManage, BusFault, UsageFault) is caught by chompi_main.cpp's handler,
 *  which writes a Record of the stacked registers and the fault status, then resets the chip
 *  as the launcher hands over: the bootloader starts the launcher. Nothing is written to the
 *  card from there. The Record lives in the backup SRAM (chompi_sram.lds's .backup_sram, after
 *  libDaisy's boot_info), which a reset leaves alone and which nothing else uses: the
 *  bootloader, the launcher and USB storage have only boot_info there, the stock firmwares
 *  don't map it. The AXI SRAM won't do (the launcher copies an image of up to 512K over it on
 *  every start), nor the SDRAM (unrefreshed through the reset, the launcher's staging area).
 *  After a power-off it holds noise: a magic word and a check sum tell a Record from that.
 *
 *  The watchdog (IWDG1) is fed only by the audio callback, at its top, also while booting: a
 *  MainLoop that hangs or blocks (a card failing for 30 s, a bug report, f_mount) never sets
 *  it off, only an audio callback that has stopped for kWatchdogMs. It starts once audio runs
 *  (chompi_main.cpp), after the boot's card reads and battery checks, and a reset stops it (the
 *  option bytes' IWDG1_SW: started by software, DevInfo's option_bytes says), so the launcher
 *  and the other slots never run with it. The IWDG can't tell the next start anything, so
 *  FRIZZ clears RCC_RSR at boot once read (Boot): its watchdog flag at the next boot means a
 *  watchdog reset since this FRIZZ started, recorded then, with when MainLoop last ran.
 *
 *  The IWDG runs on in Stop (the option bytes' FZ_IWDG_STOP, as CHOMPI's are): the low-battery
 *  sleep on a weak charger (hardware.h's LowBatteryLockoutCheck) gets one watchdog reset, then
 *  sleeps for good, in the launcher (which has its own) or in FRIZZ's boot-time checks, which
 *  come before the watchdog starts. That reset is recorded as the watchdog's.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace chompi
{
namespace crash
{
/** Why: 1 the watchdog, else the fault's exception number, as IPSR has it. MemManage,
 *  BusFault and UsageFault aren't enabled (SHCSR), so every fault comes as a HardFault, and
 *  CFSR says which it was */
enum Kind : uint8_t
{
    NONE = 0,
    WATCHDOG = 1,
    HARD_FAULT = 3,
    MEM_MANAGE = 4,
    BUS_FAULT = 5,
    USAGE_FAULT = 6,
};

static const uint32_t kMagic = 0x465A4352u; // "FZCR"
static const uint32_t kWatchdogMs = 1000;
static const uint32_t kIwdgResetFlag = 1u << 26; // RCC_RSR's IWDG1RSTF

/** The fault's registers and what ran. Plain words, no constructor: what's in the backup SRAM
 *  is left as the last start left it */
struct Record
{
    uint32_t magic;
    uint32_t kind;
    uint32_t count; // crashes since cleared
    uint32_t ms;    // uptime at the fault (the watchdog: MainLoop's last pass)
    uint32_t crc;   // the image's CRC-32 and length (IDENT's), which build crashed
    uint32_t len;
    uint32_t rsr; // RCC_RSR at the start that recorded it (the watchdog's)
    uint32_t sp;  // the stacked frame's address
    uint32_t pc, lr, xpsr;
    uint32_t cfsr, hfsr, mmfar, bfar;
    uint32_t check;
};

/** The build running, for the watchdog's Record at the next start, and MainLoop's last pass */
struct Running
{
    uint32_t magic;
    uint32_t crc, len;
    uint32_t alive_ms;
};

/** What lives in the backup SRAM */
struct Backup
{
    Running running;
    Record record;
};

inline uint32_t Check(const Record& r)
{
    const uint32_t* w = &r.magic;
    uint32_t sum = 0x5A5A5A5Au;
    for (size_t i = 0; i < offsetof(Record, check) / 4; i++)
        sum = (sum << 5 | sum >> 27) ^ w[i];
    return sum;
}

inline bool Valid(const Record& r) { return r.magic == kMagic && r.check == Check(r); }

/** Clears the Record: no crash since */
inline void Clear(Record& r)
{
    r.magic = 0;
    r.count = 0;
}

/** A Record of KIND, the registers as stacked in FRAME (r0-r3, r12, lr, pc, xPSR), or none.
 *  Its count goes on from a valid one's */
inline void Write(Record& r, uint32_t kind, const Running& running, uint32_t ms, uint32_t rsr,
                  const uint32_t* frame, const uint32_t* fault)
{
    const uint32_t count = Valid(r) ? r.count + 1 : 1;
    r.magic = kMagic;
    r.kind = kind;
    r.count = count;
    r.ms = ms;
    r.crc = running.magic == kMagic ? running.crc : 0;
    r.len = running.magic == kMagic ? running.len : 0;
    r.rsr = rsr;
    r.sp = reinterpret_cast<uintptr_t>(frame) & 0xFFFFFFFFu;
    r.lr = frame ? frame[5] : 0;
    r.pc = frame ? frame[6] : 0;
    r.xpsr = frame ? frame[7] : 0;
    r.cfsr = fault ? fault[0] : 0;
    r.hfsr = fault ? fault[1] : 0;
    r.mmfar = fault ? fault[2] : 0;
    r.bfar = fault ? fault[3] : 0;
    r.check = Check(r);
}

/** At boot, RSR as read (then cleared): a watchdog reset since FRIZZ last started is recorded,
 *  with that start's build and MainLoop's last pass; then this build is the one running */
inline void Boot(Backup& b, uint32_t rsr, uint32_t crc, uint32_t len)
{
    if ((rsr & kIwdgResetFlag) && b.running.magic == kMagic)
        Write(b.record, WATCHDOG, b.running, b.running.alive_ms, rsr, nullptr, nullptr);
    b.running.magic = kMagic;
    b.running.crc = crc;
    b.running.len = len;
    b.running.alive_ms = 0;
}

/** zlib's CRC-32 of the image as loaded, which the host works out from the .bin
 *  (tools/builds.py): a nibble at a time, from a table of 16 */
inline uint32_t Crc32(const uint8_t* p, size_t len)
{
    static const uint32_t kNibble[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
        0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
        0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= p[i];
        crc = (crc >> 4) ^ kNibble[crc & 15];
        crc = (crc >> 4) ^ kNibble[crc & 15];
    }
    return ~crc;
}
} // namespace crash
} // namespace chompi
