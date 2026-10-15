/** @file LoopLoader.h
 *  @brief Loops from the card (#70), the card's side: which slots hold a loop, and one loaded
 *  into a second SDRAM buffer while FRIZZ plays. How a player picks one and when it takes over
 *  from the recorded loop isn't here yet: for now the developers' SysEx drives it (LOOPS,
 *  MidiControl.h's namespace dev; remote.py loops).
 *
 *  The card: FRIZZ's own loops in /FRIZZ, a1.wav ... h15.wav (banks a-h, the white key from
 *  the left; any case, no leading zero), and TAPE's cubbis, cubbi_a1.wav ... cubbi_e15.wav
 *  (TAPE's per-key recordings; its _double and jammi files aren't loops), in /TAPE if there's
 *  one (the multi-firmware launcher's TAPE keeps its files there, an empty /TAPE too), else in
 *  the card's root (stock TAPE). The name sets the bank and slot, a missing one is empty; here
 *  they're banks 0-7 and kCubbiBank + 0-4, one row of ids. A loop is a WAV of 16-bit PCM at
 *  48 kHz, mono or stereo, its data in the file, at most 16 bars at 50 BPM (3,686,400
 *  frames); anything else is refused, with its reason (Result). An `acid` chunk before the
 *  data (ACID's, Ableton's, many sample packs') gives its beats, which then set the tempo
 *  (#70).
 *
 *  Everything runs from MainLoop, a step a pass, as the card's other users do (SceneStore.h,
 *  EventLog.h); a FatFs object kept open from one pass to the next (the scan's folder, the
 *  map's file) is opened again where it was after a remount by one of them (a bug report's, a
 *  save's), which makes it invalid.
 *   - At boot it scans /FRIZZ, then TAPE's folder: a pass reads kDirEntries of its entries
 *     (the folder open from pass to pass, opened again after a remount), then a pass a file
 *     found there, checking its header (WavWalk) from its first sectors.
 *   - A load opens the file, checks its header again, takes the file's clusters into a link
 *     map as FatFs's fast seek does (CREATE_LINKMAP), kMapClusters a pass (MapStep), and
 *     closes it; then it reads whole runs of the file's sectors, up to 128 KB each, straight
 *     into the buffer with the SDMMC's DMA (SdCard.h), from the data's first sector on. A run goes on while MainLoop does its other
 *     work, and its end or its error shows at the next pass: an error stops the load at once
 *     and sets the card up again (SdRawRecover), instead of FatFs's 30 s wait (sd_diskio.c).
 *     A run's DMA must have the card to itself: while a save or a bug report wants it
 *     (Process's card_wanted), the run in flight is waited for and no new one starts.
 *  Measured on the device (#70): 16 bars at 50 BPM, 14.75 MB, load in ~0.7 s; the audio
 *  callback doesn't notice, with the SDMMC's hardware flow control on.
 *
 *  For a test on the device, also: a CRC-32 of the data loaded (zlib's, so the computer checks
 *  it against the file), faults to provoke (the flow control off, a read past the card's end,
 *  a run cut short), and a write test, which writes, renames, reads back and deletes a file in
 *  /FRIZZ/T, a folder of its own, in the chunks of FRIZZ's own writes.
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include "fatfs.h"
#include "SdCard.h"

#define LOOP_LOADER_ONCE __attribute__((noinline, optimize("Os")))

namespace loops
{
static const size_t kFrizzBanks = 8;   // /FRIZZ/a1.wav ... h15.wav
static const size_t kCubbiBanks = 5;   // TAPE's cubbi_a1.wav ... cubbi_e15.wav
static const size_t kCubbiBank = kFrizzBanks; // the cubbis' first bank here
static const size_t kBanks = kFrizzBanks + kCubbiBanks;
static const size_t kSlots = 15;
static const size_t kDirEntries = 24; // a folder's entries a scan's pass reads (~1-3 ms)
static const uint32_t kMaxFrames = 3686400; // 16 bars at 50 BPM: 76.8 s at 48 kHz
static const uint32_t kPostRoll = 240;      // the looper's frames after the loop (Looper.h)
static const uint32_t kSector = 512;
// the data's first sector (up to 510 bytes before the data), 16 bars in stereo, the post-roll
static const size_t kBufBytes = (kSector + kMaxFrames * 4 + kPostRoll * 4 + kSector - 1) / kSector * kSector;
static const uint32_t kRunSectors = 256; // 128 KB a run: ~6 ms at the card's ~21 MB/s
static const uint32_t kRunTimeoutUs = 250000;
static const size_t kMapSize = 512; // the link map (2 KB, in SDRAM): 255 fragments
// the file's clusters a pass follows into the map: FatFs reads the FAT a sector at a time
// (~0.35-0.7 ms each; 128 clusters a sector on FAT32), so a pass reads 2-5 of them (#212)
static const uint32_t kMapClusters = 256;
static const size_t kWindow = 1024; // bytes of a file a header step reads
static const size_t kWindows = 8;   // a header's chunks in those first windows
static const size_t kNameMax = 28;  // a name's bytes kept (LIST/INFO/INAM; docs/LOOP_PROTOCOL.md 5.3)

/** A slot's state after a scan, or why a load stopped */
enum Result : uint8_t
{
    EMPTY,
    MONO,       // a loop, its data
    STEREO,     //
    NOT_WAV,    // no RIFF/WAVE
    BAD_FMT,    // no fmt chunk before the data, or a short one
    NOT_PCM,    // compressed, float
    NOT_16BIT,  //
    NOT_48K,    //
    CHANNELS,   // not mono or stereo
    NO_DATA,    // no data chunk in the first windows, or not a frame in it
    PAST_END,   // the data goes past the file's end, or a chunk does
    TOO_LONG,   // more than kMaxFrames
    UNREADABLE, // the card wouldn't give it
    FRAGMENTED, // in more pieces than the link map holds
    CARD_ERROR, // a read failed (SdRawError says how); the card is set up again
    TIMEOUT,    // a read didn't end in time; stopped, the card set up again
    CARD_LOST,  // and it didn't come back
    SHORT,      // the file's clusters ended before its data did
    WRITE_FAILED, // the write test: a FatFs call failed
    WRITE_BAD,    // the write test: what came back isn't what was written
};

inline uint32_t Le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24);
}
inline uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

/** A WAV's header, walked chunk by chunk over windows of the file (each of its bytes from a
 *  position on): Start with the file's size, then Step with a window at or before Want(),
 *  until it says more than MORE. Every length is checked against the file's size, so a
 *  broken one can't make it loop or run past the window */
class WavWalk
{
public:
    static const int MORE = -1;

    void Start(uint32_t size)
    {
        size_ = size;
        pos_ = 0;
        fmt_ = false;
        beats_ = 0;
        name_len = 0;
        name_cut = false;
    }
    /** The beats an acid chunk before the data gave, 0 for none */
    inline uint32_t Beats() const { return beats_; }
    /** The name a LIST/INFO chunk's INAM before the data gave (UTF-8, name_len bytes, not
     *  terminated), its first kNameMax bytes cut back to a character's start (name_cut) */
    char name[kNameMax];
    uint8_t name_len = 0;
    bool name_cut = false;
    /** Where the next window has to include */
    inline uint32_t Want() const { return pos_; }

    /** win: the file's n bytes from at. MORE, or a Result: MONO or STEREO with data (the
     *  data's offset in the file) and frames */
    LOOP_LOADER_ONCE int Step(const uint8_t* win, uint32_t at, uint32_t n, uint32_t& data, uint32_t& frames)
    {
        auto has = [&](uint32_t p, uint32_t len) { return p >= at && p - at <= n && len <= n - (p - at); };
        if (pos_ == 0)
        {
            if (size_ < 12)
                return NOT_WAV;
            if (!has(0, 12))
                return MORE;
            if (memcmp(win, "RIFF", 4) || memcmp(win + 8, "WAVE", 4))
                return NOT_WAV;
            pos_ = 12;
        }
        while (true)
        {
            if (size_ - pos_ < 8)
                return fmt_ ? NO_DATA : BAD_FMT;
            if (!has(pos_, 8))
                return MORE;
            const uint8_t* c = win + (pos_ - at);
            const uint32_t len = Le32(c + 4), room = size_ - pos_ - 8;
            if (!memcmp(c, "data", 4) && !fmt_)
                return BAD_FMT;
            if (len > room)
                return PAST_END;
            if (!memcmp(c, "data", 4))
            {
                data = pos_ + 8;
                return Check(len, frames);
            }
            if (!memcmp(c, "fmt ", 4))
            {
                if (len < 16)
                    return BAD_FMT;
                const uint32_t need = len < 26 ? 16 : 26;
                if (!has(pos_ + 8, need))
                    return MORE;
                tag_ = Le16(c + 8);
                channels_ = Le16(c + 10);
                rate_ = Le32(c + 12);
                bits_ = Le16(c + 22);
                // WAVE_FORMAT_EXTENSIBLE: the format is its subformat's first two bytes
                if (tag_ == 0xFFFE && len >= 40)
                    tag_ = Le16(c + 8 + 24);
                fmt_ = true;
            }
            // acid: flags (4), root note (2), 2, a float (4), then the beats (4)
            if (!memcmp(c, "acid", 4) && len >= 16)
            {
                if (!has(pos_ + 8, 16))
                    return MORE;
                const uint32_t beats = Le32(c + 8 + 12);
                beats_ = beats <= 1024 ? beats : 0;
            }
            // LIST/INFO: its INAM, the name, read only where the whole chunk is in the window
            if (!memcmp(c, "LIST", 4) && len >= 4 && has(pos_ + 8, len) && !memcmp(c + 8, "INFO", 4))
                Name(c + 12, len - 4);
            pos_ += 8 + len;
            if (len & 1)
            {
                if (pos_ == size_)
                    return fmt_ ? NO_DATA : BAD_FMT;
                pos_++;
            }
        }
    }

private:
    /** INFO's chunks, n bytes at p: INAM's bytes up to a NUL or its end. One whose length runs
     *  past the LIST gives none */
    LOOP_LOADER_ONCE void Name(const uint8_t* p, uint32_t n)
    {
        while (n >= 8)
        {
            const uint32_t len = Le32(p + 4);
            if (len > n - 8)
                return;
            if (!memcmp(p, "INAM", 4))
            {
                uint32_t k = 0;
                while (k < len && p[8 + k])
                    k++;
                name_cut = k > kNameMax;
                if (name_cut)
                    for (k = kNameMax; k && (p[8 + k] & 0xC0) == 0x80;)
                        k--; // not inside a character: its continuation bytes go with it
                memcpy(name, p + 8, k);
                name_len = static_cast<uint8_t>(k);
                return;
            }
            const uint32_t step = 8 + len + (len & 1);
            if (step >= n)
                return;
            p += step;
            n -= step;
        }
    }

    int Check(uint32_t len, uint32_t& frames) const
    {
        if (tag_ != 1)
            return NOT_PCM;
        if (bits_ != 16)
            return NOT_16BIT;
        if (rate_ != 48000)
            return NOT_48K;
        if (channels_ != 1 && channels_ != 2)
            return CHANNELS;
        frames = len / (2u * channels_);
        if (frames == 0)
            return NO_DATA; // not a frame: nothing to loop
        if (frames > kMaxFrames)
            return TOO_LONG;
        return channels_ == 1 ? MONO : STEREO;
    }

    uint32_t size_ = 0, pos_ = 0, rate_ = 0, beats_ = 0;
    uint16_t tag_ = 0, channels_ = 0, bits_ = 0;
    bool fmt_ = false;
};

/** A slot as the scan found it */
struct Slot
{
    uint8_t result; // Result
    uint32_t frames; // MONO's, STEREO's, TOO_LONG's
    uint32_t data; // where its data starts in the file
    uint16_t beats; // an acid chunk's, 0 for none
    uint32_t size;  // the file's bytes, 0 for none
    uint8_t name_len; // its name (WavWalk's), name_cut if it was longer
    bool name_cut;
    char name[kNameMax];
};

/** A file's slot, 1-15, by its name in any case: a1.wav ... h15.wav (FRIZZ's, cubbi false),
 *  cubbi_a1.wav ... cubbi_e15.wav (TAPE's, cubbi true), the number without a leading zero;
 *  bank its bank here (cubbis from kCubbiBank). 0 for anything else */
inline int SlotOf(const char* name, bool cubbi, size_t& bank)
{
    auto lower = [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c | 0x20 : c); };
    if (cubbi)
    {
        for (int i = 0; i < 6; i++)
            if (lower(name[i]) != "cubbi_"[i])
                return 0;
        name += 6;
    }
    const int b = lower(name[0]) - 'a';
    if (b < 0 || b >= static_cast<int>(cubbi ? kCubbiBanks : kFrizzBanks))
        return 0;
    int s = 0, i = 1;
    for (; i < 3 && name[i] >= '0' && name[i] <= '9'; i++)
        s = s * 10 + name[i] - '0';
    if (name[1] == '0' || s < 1 || s > static_cast<int>(kSlots))
        return 0;
    for (int k = 0; k < 4; k++)
        if (lower(name[i + k]) != ".wav"[k])
            return 0;
    if (name[i + 4] != '\0')
        return 0;
    bank = static_cast<size_t>(b) + (cubbi ? kCubbiBank : 0);
    return s;
}

/** "a1" ... "h15", a FRIZZ slot's name, from p: the end */
inline char* SlotName(char* p, size_t bank, size_t slot)
{
    *p++ = static_cast<char>('a' + bank);
    if (slot >= 9)
        *p++ = '1';
    *p++ = static_cast<char>('0' + (slot + 1) % 10);
    return p;
}
/** v in `groups` 7-bit groups, the highest first, from d + n: the new n. Only v's low bits,
 *  cut where they don't fit (the developers' SysEx: the loader's LOOPS, USB's;
 *  loopxfer::Put7 saturates instead) */
LOOP_LOADER_ONCE inline size_t Put7Low(uint8_t* d, size_t n, uint32_t v, size_t groups)
{
    for (size_t g = groups; g-- > 0;)
        d[n++] = static_cast<uint8_t>(g * 7 < 32 ? (v >> (g * 7)) & 0x7F : 0);
    return n;
}
/** A swap's temp file, "/FRIZZ/a1-b2.swp": a1's file on its way to b2 (LoopTransfer.h,
 *  docs/LOOP_PROTOCOL.md 5.6), into path (20 bytes) */
LOOP_LOADER_ONCE inline void SwapPath(char* path, size_t from, size_t to)
{
    memcpy(path, "/FRIZZ/", 7);
    char* p = SlotName(path + 7, from / kSlots, from % kSlots);
    *p++ = '-';
    p = SlotName(p, to / kSlots, to % kSlots);
    memcpy(p, ".swp", 5);
}
/** A FRIZZ slot's name at p in any case ("a1" ... "H15", no leading zero): its id (bank * 15 +
 *  slot), p past it; -1 for none */
inline int SlotAt(const char*& p)
{
    const int b = (p[0] | 0x20) - 'a';
    if (b < 0 || b >= static_cast<int>(kFrizzBanks) || p[1] < '1' || p[1] > '9')
        return -1;
    int s = p[1] - '0';
    p += 2;
    if (*p >= '0' && *p <= '9')
        s = s * 10 + *p++ - '0';
    return s <= static_cast<int>(kSlots) ? b * static_cast<int>(kSlots) + s - 1 : -1;
}
/** Whether name is a swap's temp file ("a1-b2.swp", any case): from, to its slots' ids */
LOOP_LOADER_ONCE inline bool SwapOf(const char* name, int& from, int& to)
{
    from = SlotAt(name);
    if (from < 0 || *name++ != '-')
        return false;
    to = SlotAt(name);
    for (int k = 0; k < 5; k++)
        if ((name[k] | (k < 4 ? 0x20 : 0)) != ".swp"[k])
            return false;
    return to >= 0 && to != from;
}

/** The loader's memory beside the buffer, in SDRAM (chompi_main.cpp): the header's window
 *  (the SDMMC's DMA lands there, so 32-byte aligned), the slots, FatFs's objects */
struct Mem
{
    alignas(32) uint8_t window[kWindow];
    Slot slots[kBanks][kSlots];
    DWORD map[kMapSize];
    FIL file;
    DIR dir;
    FILINFO info;
};
} // namespace loops

class LoopLoader
{
public:
    enum State : uint8_t
    {
        IDLE,
        SCANNING,
        LOADING,
        LOADED,
        FAILED,   // the load: Error() says why
        CHECKING, // the CRC of what was loaded
        WRITING,  // the write test
        WRITTEN,  // and its result in Error() (0 when it passed)
    };
    // a load's faults to provoke, for a test on the device
    static const uint8_t kFaultNoFlowControl = 1; // the SDMMC's flow control off for it
    static const uint8_t kFaultPastEnd = 2;       // its third run reads past the card's end
    static const uint8_t kFaultCutShort = 4;      // its third run is given no time
    static const uint8_t kFaultReinit = 8;        // a recovery sets the card up from scratch
    // FatFs's own (SdDisk.h): a load's next read as it opens the file, the write test's next
    // write of more than a sector
    static const uint8_t kFaultDiskCutShort = 16; // given no time: stopped mid-transfer
    static const uint8_t kFaultDiskPastEnd = 32;  // past the card's end

    /** buf: kBufBytes, 32-byte aligned; ms: a clock in ms; ticks: a finer one, tick_freq a
     *  second (System::GetTick's, which wraps every ~18 s: only differences are taken).
     *  Starts the scan */
    void Init(uint8_t* buf, loops::Mem* mem, FATFS* fs, uint32_t (*ms)(), uint32_t (*ticks)(),
              uint32_t tick_freq)
    {
        home_ = buf_ = buf;
        mem_ = mem;
        fs_ = fs;
        ms_clock_ = ms;
        ticks_ = ticks;
        ticks_us_ = tick_freq / 1000000u;
        // its memory isn't cleared at boot (SDRAM_NOINIT_LATER): no slot read before its scan
        memset(mem_->slots, 0, sizeof(mem_->slots));
        Request(SCANNING);
    }

    /** From MainLoop (the developers' SysEx, the play page): starts a scan, a load of bank's
     *  slot (from 0) with arg its faults into a buffer of kBufBytes or more (Init's without
     *  one), the CRC of what's loaded, or the write test (arg: KB << 16 | the bytes a write;
     *  bank its faults), in Init's buffer. False while one runs */
    bool Request(State what, uint8_t bank = 0, uint8_t slot = 0, uint32_t arg = 0,
                 uint8_t* into = nullptr)
    {
        if (Busy() || (what == CHECKING && state_ != LOADED))
            return false;
        if (what == LOADING || what == WRITING)
            buf_ = into ? into : home_;
        Begin(what, bank, slot, arg);
        return true;
    }
    /** A load stops at the next pass, after the run in flight (LoopControls.h: another loop
     *  wanted). Then IDLE */
    inline void Cancel()
    {
        if (state_ == LOADING)
            cancel_ = true;
    }
    /** Whether the developers' SysEx may use Init's buffer, which the looper may be playing
     *  from (LoopControls.h): its loads and the write test are refused otherwise */
    inline void DevBuffer(bool free) { dev_taken_ = !free; }
    inline const uint8_t* Buffer() const { return buf_; }
    inline const uint8_t* Home() const { return home_; }
    /** The slot loaded last, and its acid chunk's beats (0 for none) */
    inline uint8_t Bank() const { return bank_; }
    inline uint8_t SlotLoaded() const { return slot_; }
    inline uint32_t Beats() const { return beats_; }

    /** From MainLoop, before the card's other users (SceneStore, EventLog): a step of what's
     *  asked. card_wanted: one of them is about to use the card; a run in flight is waited
     *  for, and nothing new starts */
    LOOP_LOADER_ONCE void Process(bool card_wanted)
    {
        const uint32_t t0 = ticks_();
        if (inflight_)
        {
            SdRaw r = SdRawPoll(buf_ + done_, run_);
            while (r == SdRaw::BUSY && card_wanted && ticks_() - run_t_ < timeout_)
                r = SdRawPoll(buf_ + done_, run_);
            if (r == SdRaw::DONE)
                RunDone();
            else if (r == SdRaw::FAILED)
                Fail(loops::CARD_ERROR);
            else if (ticks_() - run_t_ >= timeout_)
                Fail(loops::TIMEOUT);
        }
        if (!inflight_ && !card_wanted)
            Step();
        const uint32_t dt = (ticks_() - t0) / ticks_us_;
        worst_us_ = dt > worst_us_ ? dt : worst_us_;
    }

    inline State GetState() const { return state_; }
    inline bool Busy() const { return state_ == SCANNING || state_ == LOADING || state_ == CHECKING || state_ == WRITING; }
    /** A loaded loop: its frames of int16, interleaved when stereo */
    inline bool Ready() const { return state_ == LOADED; }
    inline const int16_t* Data() const { return reinterpret_cast<const int16_t*>(buf_ + skip_); }
    inline uint32_t Frames() const { return frames_; }
    inline uint8_t Channels() const { return result_ == loops::MONO ? 1 : 2; }
    inline const loops::Slot& GetSlot(size_t bank, size_t slot) const { return mem_->slots[bank][slot]; }
    /** The loop transfer's (LoopTransfer.h): a slot as it changed on the card, and whether
     *  TAPE's cubbis are in /TAPE (else the root) */
    inline loops::Slot& SetSlot(size_t bank, size_t slot) { return mem_->slots[bank][slot]; }
    inline bool Tape() const { return tape_; }

    /** The file at path's header walked into s: its result, frames, data, beats, size and
     *  name. The scan's, and the loop transfer's check of a file it wrote */
    LOOP_LOADER_ONCE void Walk(const char* path, loops::Slot& s)
    {
        s.result = Open(path, s.frames, s.data, false, &s);
        s.beats = static_cast<uint16_t>(beats_);
        s.size = size_;
    }

    /** A slot's file: "/FRIZZ/a1.wav", or "/TAPE/cubbi_a1.wav" ("/cubbi_a1.wav" without /TAPE) */
    void SlotPath(char* path, size_t bank, size_t slot) const
    {
        const bool cubbi = bank >= loops::kCubbiBank;
        const char* dir = !cubbi ? "/FRIZZ/" : tape_ ? "/TAPE/cubbi_" : "/cubbi_";
        const size_t n = strlen(dir);
        memcpy(path, dir, n);
        memcpy(loops::SlotName(path + n, bank - (cubbi ? loops::kCubbiBank : 0), slot), ".wav", 5);
    }
    static const size_t kPathLen = 24; // "/TAPE/cubbi_a15.wav"

    /** The developers' SysEx's LOOPS (MidiControl.h): arg[0] the part, the reply into d (its
     *  length, -1 for a request it doesn't take). Numbers in 7-bit groups, the highest first
     *   0           -> state, bank, slot, result, the SD error (5), bytes loaded (4) of (4),
     *                  ms (3), runs (3), MainLoop's longest step in us (3), CLKCR (5),
     *                  recoveries, of them set up again, the file's fragments (2)
     *   1 BANK      -> the bank's scan: SCANNING, then its 15 slots' results
     *   2 BANK SLOT -> its result, frames (4), its data's offset in the file (5)
     *   3           scans again
     *   4 BANK SLOT FAULTS  loads it (slots from 0); FAULTS the kFault bits
     *   5 [1]       -> state, the CRC-32 (5) of the data loaded once it's LOADED again; with
     *               1 it starts
     *   6 KB(2) CHUNK(2)    the write test: KB written in CHUNK-byte writes; KB's high group
     *               can carry kFaultDiskCutShort or kFaultDiskPastEnd (KB is at most 1024,
     *               the developers' SysEx 5 bytes)
     *  3-6 are refused (-1) while something runs, and while the card is lent (chompi_main.cpp) */
    LOOP_LOADER_ONCE int Dev(const uint8_t* arg, size_t len, uint8_t* d)
    {
        const uint8_t part = len ? arg[0] : 0xFF;
        const uint8_t bank = len > 1 ? arg[1] : 0, slot = len > 2 ? arg[2] : 0;
        size_t n = 0;
        if (part == 0)
        {
            d[n++] = state_;
            d[n++] = bank_;
            d[n++] = slot_;
            d[n++] = error_;
            n = loops::Put7Low(d, n, sd_error_, 5);
            n = loops::Put7Low(d, n, done_, 4);
            n = loops::Put7Low(d, n, total_, 4);
            n = loops::Put7Low(d, n, ms_, 3);
            n = loops::Put7Low(d, n, runs_, 3);
            n = loops::Put7Low(d, n, worst_us_, 3);
            n = loops::Put7Low(d, n, SdRawClkcr(), 5);
            d[n++] = recoveries_ & 0x7F;
            d[n++] = reinits_ & 0x7F;
            n = loops::Put7Low(d, n, frags_, 2);
            worst_us_ = 0;
            return static_cast<int>(n);
        }
        const bool banked = part == 1 || part == 2 || part == 4;
        if ((banked && bank >= loops::kBanks) || ((part == 2 || part == 4) && slot >= loops::kSlots)
            || (part == 4 && len < 4))
            return -1;
        if (part == 1)
        {
            d[n++] = state_ == SCANNING;
            for (size_t s = 0; s < loops::kSlots; s++)
                d[n++] = mem_->slots[bank][s].result;
        }
        else if (part == 2)
        {
            const loops::Slot& s = mem_->slots[bank][slot];
            d[n++] = s.result;
            n = loops::Put7Low(d, n, s.frames, 4);
            n = loops::Put7Low(d, n, s.data, 5);
        }
        else if (part == 3)
            return Request(SCANNING) ? 0 : -1;
        else if (part == 4)
            return !dev_taken_ && Request(LOADING, bank, slot, arg[3]) ? 0 : -1;
        else if (part == 5)
        {
            if (len > 1 && !Request(CHECKING))
                return -1;
            d[n++] = state_;
            n = loops::Put7Low(d, n, crc_, 5);
        }
        else if (part == 6 && len >= 5)
            return !dev_taken_ && Request(WRITING, arg[1] & 0x30, 0, ((arg[1] & 0x0F) << 7 | arg[2]) << 16 | (arg[3] << 7 | arg[4])) ? 0 : -1;
        else
            return -1;
        return static_cast<int>(n);
    }

private:
    /** The step of what runs */
    LOOP_LOADER_ONCE void Step()
    {
        if (state_ == SCANNING)
            ScanStep();
        else if (state_ == LOADING && cancel_)
        {
            if (faults_ & kFaultNoFlowControl)
                SdRawFlowControl(true);
            End(IDLE, 0);
        }
        else if (state_ == LOADING)
            opened_ ? RunStep() : (mapping_ ? MapStep() : OpenStep());
        else if (state_ == CHECKING)
            CrcStep();
        else if (state_ == WRITING)
            WriteStep();
    }

    LOOP_LOADER_ONCE void Begin(State what, uint8_t bank, uint8_t slot, uint32_t arg)
    {
        state_ = what;
        error_ = 0;
        sd_error_ = 0;
        if (what == SCANNING)
        {
            if (dir_open_)
                f_closedir(&mem_->dir);
            group_ = 0;
            listed_ = dir_open_ = relisted_ = false;
            entries_ = 0;
            ms_ = 0;
            start_ms_ = ms_clock_();
        }
        else if (what == LOADING)
        {
            cancel_ = false;
            bank_ = bank;
            slot_ = slot;
            faults_ = static_cast<uint8_t>(arg);
            opened_ = mapping_ = false;
            done_ = total_ = runs_ = ms_ = frags_ = 0;
            start_ms_ = ms_clock_();
        }
        else if (what == CHECKING)
        {
            crc_ = 0xFFFFFFFFu;
            at_ = 0;
        }
        else if (what == WRITING)
        {
            total_ = (arg >> 16) * 1024u;
            chunk_ = arg & 0xFFFF;
            faults_ = bank;
            phase_ = 0;
            at_ = 0;
            done_ = 0;
            bad_ = 0;
            if (!total_ || !chunk_ || total_ > kWriteMax)
                End(WRITTEN, loops::WRITE_FAILED);
            else
                DiskFaultArm(true);
        }
    }

    void End(State state, uint8_t error)
    {
        state_ = state;
        error_ = error;
    }

    // ---- the scan ----

    /** A pass: kDirEntries of a folder's entries listed (/FRIZZ, then TAPE's), or one file
     *  found there checked. The folder stays open from pass to pass; a remount meanwhile (a
     *  save's, a bug report's) makes it invalid, and it's opened again past what was read */
    LOOP_LOADER_ONCE void ScanStep()
    {
        if (group_ == 2)
        {
            ms_ = ms_clock_() - start_ms_;
            End(IDLE, 0);
            return;
        }
        const bool cubbi = group_ == 1;
        const size_t first = cubbi ? loops::kCubbiBank : 0;
        const size_t last = cubbi ? loops::kBanks : loops::kFrizzBanks;
        if (!listed_)
        {
            ListStep(cubbi, first, last);
            return;
        }
        size_t bank = first;
        while (bank < last && !found_[bank])
            bank++;
        if (bank == last)
        {
            group_++;
            listed_ = false;
            entries_ = 0;
            return;
        }
        const size_t slot = static_cast<size_t>(__builtin_ctz(found_[bank]));
        found_[bank] &= found_[bank] - 1;
        char path[kPathLen];
        SlotPath(path, bank, slot);
        Walk(path, mem_->slots[bank][slot]);
    }

    /** A pass of the listing: the folder opened (TAPE's: /TAPE if it's there, else the root),
     *  kDirEntries read */
    LOOP_LOADER_ONCE void ListStep(bool cubbi, size_t first, size_t last)
    {
        DIR& dir = mem_->dir;
        if (!dir_open_)
        {
            if (!entries_)
            {
                for (size_t b = first; b < last; b++)
                {
                    found_[b] = 0;
                    for (size_t s = 0; s < loops::kSlots; s++)
                        mem_->slots[b][s] = {loops::EMPTY, 0, 0, 0};
                }
                if (cubbi)
                    tape_ = f_opendir(&dir, "/TAPE") == FR_OK;
            }
            const char* path = !cubbi ? "/FRIZZ" : tape_ ? "/TAPE" : "/";
            if ((!cubbi || entries_ || !tape_) && f_opendir(&dir, path) != FR_OK)
            {
                listed_ = true; // no folder: nothing there
                return;
            }
            dir_open_ = true;
            // opened again after a remount: past the entries read before it
            for (uint32_t n = 0; n < entries_; n++)
                if (f_readdir(&dir, &mem_->info) != FR_OK || !mem_->info.fname[0])
                    break;
        }
        for (size_t n = 0; n < loops::kDirEntries; n++)
        {
            const FRESULT r = f_readdir(&dir, &mem_->info);
            // a remount: opened again at the next pass, past what was read; a card error too,
            // once a scan, rather than the slots after it read as empty
            if (r == FR_INVALID_OBJECT || (r != FR_OK && !relisted_))
            {
                relisted_ |= r != FR_INVALID_OBJECT;
                dir_open_ = false;
                return;
            }
            if (r != FR_OK || !mem_->info.fname[0])
            {
                f_closedir(&dir);
                dir_open_ = false;
                listed_ = true;
                if (!cubbi)
                    Repair();
                return;
            }
            entries_++;
            size_t bank = 0;
            int from, to;
            if (mem_->info.fattrib & AM_DIR)
                ;
            else if (const int s = loops::SlotOf(mem_->info.fname, cubbi, bank))
                found_[bank] |= static_cast<uint16_t>(1u << (s - 1));
            else if (!cubbi && swaps_ < kSwaps && loops::SwapOf(mem_->info.fname, from, to))
            {
                swap_[swaps_][0] = static_cast<uint8_t>(from);
                swap_[swaps_++][1] = static_cast<uint8_t>(to);
            }
        }
    }

    /** A swap a power cut interrupted (5.6), found in /FRIZZ's listing: its temp file to where
     *  it was going if that's free (the swap finished), else back where it came from if that
     *  is (undone), else left; never deleted, it holds a loop */
    LOOP_LOADER_ONCE void Repair()
    {
        for (size_t i = 0; i < swaps_; i++)
            for (const uint8_t id : {swap_[i][1], swap_[i][0]})
            {
                uint16_t& found = found_[id / loops::kSlots];
                const uint16_t bit = static_cast<uint16_t>(1u << (id % loops::kSlots));
                if (found & bit)
                    continue;
                char tmp[20], path[kPathLen];
                loops::SwapPath(tmp, swap_[i][0], swap_[i][1]);
                SlotPath(path, id / loops::kSlots, id % loops::kSlots);
                if (f_rename(tmp, path) == FR_OK)
                    found |= bit;
                break;
            }
        swaps_ = 0;
    }

    /** Opens the slot's file and checks its header. The file is closed again, unless keep and
     *  it's a loop */
    LOOP_LOADER_ONCE uint8_t Open(size_t bank, size_t slot, uint32_t& frames, uint32_t& data, bool keep)
    {
        char path[kPathLen];
        SlotPath(path, bank, slot);
        return Open(path, frames, data, keep);
    }
    /** name: its name into it too */
    LOOP_LOADER_ONCE uint8_t Open(const char* path, uint32_t& frames, uint32_t& data, bool keep,
                                  loops::Slot* name = nullptr)
    {
        FIL& file = mem_->file;
        loops::WavWalk walk;
        walk.Start(0);
        frames = beats_ = size_ = 0;
        if (name)
            name->name_len = 0;
        if (f_open(&file, path, FA_READ) != FR_OK)
            return loops::UNREADABLE;
        walk.Start(f_size(&file));
        int r = loops::WavWalk::MORE;
        for (size_t w = 0; w < loops::kWindows && r == loops::WavWalk::MORE; w++)
        {
            const uint32_t at = walk.Want() & ~(loops::kSector - 1);
            UINT got = 0;
            if (f_lseek(&file, at) != FR_OK || f_read(&file, mem_->window, loops::kWindow, &got) != FR_OK)
                r = loops::UNREADABLE;
            else
                r = walk.Step(mem_->window, at, got, data, frames);
        }
        beats_ = walk.Beats();
        size_ = f_size(&file);
        if (name)
        {
            name->name_len = walk.name_len;
            name->name_cut = walk.name_cut;
            memcpy(name->name, walk.name, loops::kNameMax);
        }
        if (r == loops::WavWalk::MORE)
            r = loops::NO_DATA;
        if (!keep || (r != loops::MONO && r != loops::STEREO))
            f_close(&file);
        return static_cast<uint8_t>(r);
    }

    // ---- the load ----
    /** The load's first pass: the file opened, its header checked again, the map begun */
    LOOP_LOADER_ONCE void OpenStep()
    {
        DiskFaultArm(false);
        result_ = Open(bank_, slot_, frames_, data_, true);
        DiskFaultEnd();
        if (result_ != loops::MONO && result_ != loops::STEREO)
        {
            End(FAILED, result_);
            return;
        }
        const uint32_t bcs = fs_->csize * loops::kSector;
        clusters_ = (f_size(&mem_->file) + bcs - 1) / bcs;
        cluster_ = 0;
        frags_ = 0;
        mapping_ = true;
    }

    /** A pass of the map: the file's next kMapClusters clusters followed, as FatFs's
     *  CREATE_LINKMAP does all in one: the map's size, then each fragment's length and first
     *  cluster, then 0. A seek to a cluster's end leaves FatFs in it (its fp->clust), reading
     *  only the FAT (the file's last, past its end, also a sector of it). The file stays open
     *  from pass to pass; a remount meanwhile (a save's, a bug report's) makes it invalid, and
     *  it's opened again where it was (Reopen) */
    LOOP_LOADER_ONCE void MapStep()
    {
        FIL& file = mem_->file;
        DWORD* map = mem_->map;
        const uint32_t bcs = fs_->csize * loops::kSector, size = f_size(&file);
        for (uint32_t n = 0; n < loops::kMapClusters && cluster_ < clusters_; n++, cluster_++)
        {
            const uint32_t end = (cluster_ + 1) * bcs;
            FRESULT r = f_lseek(&file, end < size ? end : size);
            if (r == FR_INVALID_OBJECT && (r = Reopen()) == FR_OK)
                r = f_lseek(&file, end < size ? end : size);
            if (r != FR_OK)
            {
                f_close(&file);
                mapping_ = false;
                End(FAILED, r == FR_INT_ERR ? loops::SHORT
                            : (r == FR_NOT_ENOUGH_CORE ? loops::FRAGMENTED : loops::UNREADABLE));
                return;
            }
            const DWORD c = file.clust;
            // the map: 2 items, then 2 a fragment, stored while they fit
            if (frags_ && c == last_ + 1)
            {
                if (2 + 2 * frags_ <= loops::kMapSize)
                    map[2 * frags_ - 1]++;
            }
            else if (2 + 2 * ++frags_ <= loops::kMapSize)
            {
                map[2 * frags_ - 1] = 1;
                map[2 * frags_] = c;
            }
            last_ = c;
        }
        if (cluster_ < clusters_)
            return;
        f_close(&file);
        mapping_ = false;
        map[0] = 2 + 2 * frags_;
        if (map[0] > loops::kMapSize)
        {
            End(FAILED, loops::FRAGMENTED);
            return;
        }
        map[map[0] - 1] = 0;
        Mapped();
    }

    /** The file opened again after a remount, FatFs put back in the map's last cluster by the
     *  map so far (fast seek: no FAT read) */
    FRESULT Reopen()
    {
        FIL& file = mem_->file;
        f_close(&file);
        char path[kPathLen];
        SlotPath(path, bank_, slot_);
        FRESULT r = f_open(&file, path, FA_READ);
        if (r != FR_OK || !cluster_)
            return r;
        if (2 + 2 * frags_ > loops::kMapSize)
            return FR_NOT_ENOUGH_CORE; // too many fragments already
        mem_->map[0] = loops::kMapSize;
        mem_->map[2 * frags_ + 1] = 0;
        file.cltbl = mem_->map;
        r = f_lseek(&file, cluster_ * fs_->csize * loops::kSector);
        file.cltbl = nullptr;
        return r;
    }

    /** The map done: where the runs start */
    void Mapped()
    {
        // the volume's, kept: a remount (a save's, a bug report's) leaves the card as it is
        csize_ = fs_->csize;
        database_ = fs_->database;
        skip_ = data_ % loops::kSector;
        const uint32_t bytes = frames_ * (result_ == loops::MONO ? 2u : 4u);
        total_ = (skip_ + bytes + loops::kSector - 1) & ~(loops::kSector - 1);
        // the run from the data's first sector: past the fragments before it
        frag_ = 1;
        frag_at_ = data_ / loops::kSector;
        while (mem_->map[frag_] && frag_at_ >= mem_->map[frag_] * csize_)
        {
            frag_at_ -= mem_->map[frag_] * csize_;
            frag_ += 2;
        }
        if (faults_ & kFaultNoFlowControl)
            SdRawFlowControl(false);
        opened_ = true;
        run_t_ = ticks_(); // the first run's wait for the card starts here
    }

    /** Starts the next run, or ends the load */
    LOOP_LOADER_ONCE void RunStep()
    {
        if (done_ == total_)
        {
            ms_ = ms_clock_() - start_ms_;
            SdRawFlowControl(true);
            End(LOADED, 0);
            return;
        }
        const uint32_t len = frag_ + 1 < loops::kMapSize ? mem_->map[frag_] * csize_ : 0;
        if (!len)
        {
            Fail(loops::SHORT);
            return;
        }
        uint32_t n = (total_ - done_) / loops::kSector;
        n = n < loops::kRunSectors ? n : loops::kRunSectors;
        n = n < len - frag_at_ ? n : len - frag_at_;
        uint32_t sector = database_ + (mem_->map[frag_ + 1] - 2) * csize_ + frag_at_;
        const bool third = runs_ == 2;
        if (third && (faults_ & kFaultPastEnd))
            sector = 0xF0000000u; // the HAL refuses it, beyond any card's last sector
        const int started = SdRawStart(buf_ + done_, sector, n);
        if (started <= 0)
        {
            // refused, or the card still busy with the last run for too long
            if (started < 0)
                Fail(loops::CARD_ERROR);
            else if (ticks_() - run_t_ >= loops::kRunTimeoutUs * ticks_us_)
                Fail(loops::TIMEOUT);
            return;
        }
        run_ = n;
        run_t_ = ticks_();
        timeout_ = third && (faults_ & kFaultCutShort) ? 0 : loops::kRunTimeoutUs * ticks_us_;
        inflight_ = true;
    }

    void RunDone()
    {
        inflight_ = false;
        done_ += run_ * loops::kSector;
        frag_at_ += run_;
        runs_++;
        run_t_ = ticks_(); // the next run's wait for the card starts here
        if (frag_at_ == mem_->map[frag_] * csize_)
        {
            frag_ += 2;
            frag_at_ = 0;
        }
    }

    /** A run failed or overran its time: the transfer stopped, the card set up again */
    LOOP_LOADER_ONCE void Fail(uint8_t why)
    {
        sd_error_ = SdRawError();
        const bool card = inflight_ || why == loops::CARD_ERROR;
        inflight_ = false;
        if (card)
        {
            bool reinit = false;
            if (!SdRawRecover(reinit, faults_ & kFaultReinit))
                why = loops::CARD_LOST;
            recoveries_++;
            reinits_ += reinit;
        }
        SdRawFlowControl(true);
        ms_ = ms_clock_() - start_ms_;
        End(FAILED, why);
    }

    /** FatFs's fault for a test armed (SdDisk.h), from faults_; and what its recoveries are
     *  counted from, so LOOPS 0 counts FatFs's in this load's open or this write test too */
    void DiskFaultArm(bool writes)
    {
        const uint8_t f = (faults_ & kFaultDiskCutShort ? sddisk::kCutShort : 0)
                          | (faults_ & kFaultDiskPastEnd ? sddisk::kPastEnd : 0);
        sddisk::Fault() = f ? static_cast<uint8_t>(f | (writes ? sddisk::kWrites : 0)) : 0;
        disk_ = sddisk::GetStats();
    }
    void DiskFaultEnd()
    {
        sddisk::Fault() = 0;
        const sddisk::Stats& s = sddisk::GetStats();
        recoveries_ += s.recoveries - disk_.recoveries;
        reinits_ += s.reinits - disk_.reinits;
        if (s.errors != disk_.errors)
            sd_error_ = s.last_error;
    }

    // ---- the test's CRC-32 (zlib's), 16 KB a pass (~3 ms) ----
    LOOP_LOADER_ONCE void CrcStep()
    {
        const uint32_t bytes = frames_ * Channels() * 2u;
        const uint8_t* p = buf_ + skip_;
        const uint32_t end = at_ + 16384 < bytes ? at_ + 16384 : bytes;
        uint32_t c = crc_;
        for (; at_ < end; at_++)
        {
            c ^= p[at_];
            for (int k = 0; k < 8; k++)
                c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
        }
        crc_ = c;
        if (at_ == bytes)
        {
            crc_ = ~c;
            End(LOADED, 0);
        }
    }

    // ---- the write test, in /FRIZZ/T: a chunk a pass, written then read back ----
    static const uint32_t kWriteMax = 1u << 20;
    static uint8_t Pattern(uint32_t i) { return static_cast<uint8_t>((i * 2654435761u) >> 24); }

    LOOP_LOADER_ONCE void WriteStep()
    {
        static const char* const kDir = "/FRIZZ/T";
        static const char* const kTmp = "/FRIZZ/T/w.tmp";
        static const char* const kName = "/FRIZZ/T/w.bin";
        FIL& file = mem_->file;
        uint8_t* back = buf_ + kWriteMax;
        UINT got = 0;
        bool ok = true;
        if (phase_ == 0)
        {
            const FRESULT d = f_mkdir(kDir);
            ok = (d == FR_OK || d == FR_EXIST) && f_open(&file, kTmp, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK;
            phase_ = 1;
        }
        else if (phase_ == 1 || phase_ == 3)
        {
            // a chunk's bytes made, written; or read back and compared (a loaded loop goes)
            const uint32_t n = total_ - at_ < chunk_ ? total_ - at_ : chunk_;
            if (phase_ == 1)
            {
                for (uint32_t i = at_; i < at_ + n; i++)
                    buf_[i] = Pattern(i);
                ok = f_write(&file, buf_ + at_, n, &got) == FR_OK;
            }
            else
            {
                ok = f_read(&file, back + at_, n, &got) == FR_OK;
                for (uint32_t i = at_; i < at_ + n; i++)
                    bad_ += back[i] != Pattern(i);
            }
            ok = ok && got == n;
            at_ += n;
            done_ = phase_ == 1 ? at_ : total_ + at_;
            if (at_ == total_)
            {
                // as SceneStore saves: the .tmp written, the file it replaces deleted, renamed
                ok = ok && f_close(&file) == FR_OK;
                if (phase_ == 1)
                {
                    const FRESULT del = f_unlink(kName);
                    ok = ok && (del == FR_OK || del == FR_NO_FILE) && f_rename(kTmp, kName) == FR_OK
                         && f_open(&file, kName, FA_READ) == FR_OK;
                }
                at_ = 0;
                phase_ += 2; // to reading it back (3), then to the comparison (5)
            }
        }
        else
        {
            WriteEnd(bad_ ? loops::WRITE_BAD : 0);
            return;
        }
        if (!ok)
        {
            f_close(&file);
            WriteEnd(loops::WRITE_FAILED);
        }
    }

    /** Whatever the test left, deleted: its files, its folder */
    void WriteEnd(uint8_t error)
    {
        DiskFaultEnd();
        f_unlink("/FRIZZ/T/w.tmp");
        f_unlink("/FRIZZ/T/w.bin");
        f_unlink("/FRIZZ/T");
        End(WRITTEN, error);
    }

    uint8_t* buf_ = nullptr;  // the buffer of the last load or write test
    uint8_t* home_ = nullptr; // Init's
    // every member's default 0, so the loader sits in .bss, not in .data (whose values take
    // room in the image): Init sets the clock's ticks, a load its fragment
    bool cancel_ = false, dev_taken_ = false;
    uint32_t beats_ = 0, size_ = 0; // Open's file's: its acid chunk's beats, its size
    loops::Mem* mem_ = nullptr;
    FATFS* fs_ = nullptr;
    uint32_t (*ms_clock_)() = nullptr;
    uint32_t (*ticks_)() = nullptr;
    uint32_t ticks_us_ = 0;

    State state_ = IDLE;
    uint8_t bank_ = 0, slot_ = 0, error_ = 0, faults_ = 0;
    uint32_t sd_error_ = 0;
    // the scan: the folder (0 /FRIZZ, 1 TAPE's, 2 done), its entries read, the slots found
    uint8_t group_ = 0;
    bool listed_ = false, dir_open_ = false, tape_ = false;
    uint32_t entries_ = 0;
    uint16_t found_[loops::kBanks] = {};
    static const size_t kSwaps = 4; // swaps' temp files a scan puts right
    uint8_t swap_[kSwaps][2] = {}, swaps_ = 0;
    bool relisted_ = false; // the listing read again after a card error
    // the load
    bool opened_ = false, mapping_ = false, inflight_ = false;
    uint8_t result_ = loops::EMPTY;
    uint32_t frames_ = 0, data_ = 0, skip_ = 0;
    uint32_t clusters_ = 0, cluster_ = 0; // the map's: the file's clusters, the next one
    DWORD last_ = 0;
    uint16_t csize_ = 0;
    DWORD database_ = 0;
    size_t frag_ = 0;
    uint32_t frag_at_ = 0, run_ = 0, done_ = 0, total_ = 0, runs_ = 0;
    uint32_t run_t_ = 0, timeout_ = 0, start_ms_ = 0, ms_ = 0, worst_us_ = 0; // ticks, ms, us
    uint8_t recoveries_ = 0, reinits_ = 0;
    sddisk::Stats disk_ = {};
    uint32_t frags_ = 0;
    // the CRC, the write test
    uint32_t crc_ = 0, at_ = 0, chunk_ = 0, bad_ = 0;
    uint8_t phase_ = 0;
};
