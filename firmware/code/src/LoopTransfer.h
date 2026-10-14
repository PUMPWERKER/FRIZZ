/** @file LoopTransfer.h
 *  @brief FRIZZ's loop banks over USB MIDI (#278): the protocol the Companion page speaks
 *  (frizz.friedrichwagner.eu/companion/), SysEx 0x38 over USB only. docs/LOOP_PROTOCOL.md is
 *  the contract; its sections are named here as "(3.7)". tools/loopxfer.py is its reference,
 *  and test/loopxfer.cpp replays that one's vectors against this code byte for byte.
 *
 *  Two halves:
 *   - Intake, in the audio callback (MidiControl.h): a CHUNK's packed bytes into a ring of
 *     kWindow in SDRAM if it's the next one of the open write (3.7), else counted as a gap or
 *     a repeat; any other request into a slot of its own, one at a time.
 *   - LoopTransfer, in MainLoop beside the loader (LoopLoader.h): the requests, answered as
 *     they come, and the job, a write or a cubbi's copy, a step a pass: the ring unpacked into a
 *     staging block in SDRAM (the CRC-32 as it comes), written to /FRIZZ/xfer.tmp 32 KB at a
 *     time and synced, so a remount (a save's, a bug report's) loses nothing (the file opened
 *     again at its end); then its header walked (WavWalk), its clusters followed as a load
 *     does, the slot's old file deleted and the new one renamed into place in one pass (3.9).
 *     It pauses while LOOP records, a loop loads or the card is being written (3.8), and ends
 *     before the card is lent (Lend). The free space is counted a FAT sector a pass where the
 *     card's FSINFO doesn't have it (5.4). A swap's temp file a power cut left is put right by
 *     the loader's scan (5.6, LoopLoader::Repair), xfer.tmp deleted at boot (Boot).
 *  FRIZZ-bench.bin has the intake and answers HELLO, and NOT_HERE to the rest (BenchAnswer).
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include "util/scopedirqblocker.h"
#include "Crash.h"
#include "LoopLoader.h"
#include "Version.h"

// built for size, all of it (SRAM_EXEC, docs/CAPACITY.md): none of it is in a hurry
#pragma GCC push_options
#pragma GCC optimize("Os")

namespace loopxfer
{
// the protocol's version (HELLO; site/features.json's loopTransfer is the major)
static const uint8_t kLoopsProtocol = 1, kLoopsProtocolMinor = 0;

enum Op : uint8_t
{
    HELLO = 0x00,
    STATUS = 0x01,
    BANK = 0x02,
    SLOT = 0x03,
    NAME = 0x04,
    WRITE_BEGIN = 0x10,
    CHUNK = 0x11,
    WRITE_END = 0x12,
    ABORT = 0x13,
    RENAME = 0x20,
    SWAP = 0x21,
    DELETE = 0x22,
    COPY = 0x23,
    RESCAN = 0x30,
};

/** 4.1: fixed from v1 on, only appended */
enum Status : uint8_t
{
    OK,
    BAD,
    NOT_HERE,
    BUSY_RECORDING,
    BUSY_LOADING,
    BUSY_SCANNING,
    BUSY_CARD,
    BUSY_JOB,
    CARD_LENT,
    NO_CARD,
    NOT_A_SLOT,
    EMPTY,
    EXISTS,
    NO_SPACE,
    TOO_LONG,
    BAD_WAV,
    BAD_CRC,
    INCOMPLETE,
    NO_TRANSFER,
    FRAGMENTED,
    CARD_ERROR,
    TIMED_OUT,
    ABORTED,
};

enum Job : uint8_t
{
    NONE,
    WRITE,
    COPY_JOB,
};
enum Phase : uint8_t
{
    IDLE,
    RUNNING,
    PAUSED,    // by FRIZZ (3.8)
    FINISHING, // 3.9's steps
    DONE,
    FAILED,
};

static const uint8_t kMaxChunk = 98;                               // HELLO's maxChunk
static const uint8_t kWindow = 16;                                 // HELLO's window
static const size_t kPacked = kMaxChunk + (kMaxChunk + 6) / 7;     // 112
static const uint32_t kMaxFile = loops::kMaxFrames * 4 + 8192;     // 14,753,792
static const uint8_t kMaxRequest = 128;                            // libDaisy's SYSEX_BUFFER_LEN
static const uint8_t kReplace = 1;                                 // FLAGS' bit 0
static const uint32_t kBlock = 32768;                              // xfer.tmp's writes, a copy's
static const uint32_t kSpare = 65536;                              // NO_SPACE's margin (3.6)
static const uint32_t kTimeoutMs = 30000;                          // 1.6
static const uint32_t kAckEveryMs = 250;                           // paused, NO_TRANSFER's
static const size_t kMaxFragments = (loops::kMapSize - 2) / 2;     // 255: the loader's map
static const char kTmp[] = "/FRIZZ/xfer.tmp";

/** In SDRAM (chompi_main.cpp): the staging block (the card's DMA reads it), the ring, the
 *  job's files (xfer.tmp, a copy's cubbi), as the loader keeps its own */
struct Mem
{
    alignas(32) uint8_t stage[kBlock + kMaxChunk];
    uint8_t ring[kWindow][kPacked];
    FIL file, src;
};

/** A request that isn't a CHUNK: OP SEQ, len argument bytes (the first 8 kept) */
struct Request
{
    uint8_t op, seq, len;
    uint8_t arg[8];
};

/** N(k): k 7-bit groups, the highest first (1.4); saturated at the most they hold */
inline uint32_t Get7(const uint8_t* p, size_t groups)
{
    uint32_t v = 0;
    for (size_t g = 0; g < groups; g++)
        v = v << 7 | p[g];
    return v;
}
inline size_t Put7(uint8_t* d, size_t n, uint32_t v, size_t groups)
{
    const uint32_t top = groups >= 5 ? 0xFFFFFFFFu : (1u << (7 * groups)) - 1;
    v = v > top ? top : v;
    for (size_t g = groups; g-- > 0;)
        d[n++] = static_cast<uint8_t>(g * 7 < 32 ? (v >> (g * 7)) & 0x7F : 0);
    return n;
}
/** 8-bit bytes in groups of 7, each after a byte of their top bits (1.4): n bytes from s into
 *  d, the packed length */
inline size_t Pack(const uint8_t* s, size_t n, uint8_t* d)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i += 7)
    {
        uint8_t& top = d[k++];
        top = 0;
        for (size_t j = 0; j < 7 && i + j < n; j++)
        {
            top = static_cast<uint8_t>(top | (s[i + j] >> 7) << j);
            d[k++] = s[i + j] & 0x7F;
        }
    }
    return k;
}
/** And back: n bytes into d */
inline void Unpack(const uint8_t* s, size_t n, uint8_t* d)
{
    for (size_t i = 0; i < n; i += 7)
    {
        const uint8_t top = *s++;
        for (size_t j = 0; j < 7 && i + j < n; j++)
            d[i + j] = static_cast<uint8_t>(*s++ | ((top >> j) & 1) << 7);
    }
}

/** HELLO's data (2.1) after d[n]: the length */
inline size_t Hello(uint8_t* d, size_t n)
{
    d[n++] = kLoopsProtocol;
    d[n++] = kLoopsProtocolMinor;
    for (uint8_t v : kFrizzVersion)
        d[n++] = v;
    n = Put7(d, n, loops::kMaxFrames, 4);
    n = Put7(d, n, kMaxFile, 4);
    d[n++] = kMaxChunk;
    d[n++] = kWindow;
    n = Put7(d, n, kMaxRequest, 2);
    d[n++] = loops::kNameMax;
    d[n++] = loops::kFrizzBanks;
    d[n++] = loops::kCubbiBanks;
    d[n++] = loops::kSlots;
    return n;
}

/** The audio callback's half (6.4): what comes over USB for 0x38, for MainLoop. The open
 *  write's state is MainLoop's, set with the interrupt blocked */
class Intake
{
public:
    /** Its memory may not be cleared at boot (chompi_main.cpp: in the DTCM): all of it set here */
    void Init(Mem* mem)
    {
        memset(static_cast<void*>(this), 0, sizeof(*this));
        mem_ = mem;
    }

    /** From the audio callback (MidiControl::SysEx): the bytes after 0x38 */
    void Take(const uint8_t* d, size_t n)
    {
        if (n < 2)
            return;
        if (d[0] == CHUNK)
        {
            Chunk(d + 1, n - 1);
            return;
        }
        // one at a time: the app waits for its answer (1.5), so another is a repeat
        if (pending_)
            return;
        req_.op = d[0];
        req_.seq = d[1];
        n -= 2;
        req_.len = static_cast<uint8_t>(n > 255 ? 255 : n);
        memcpy(req_.arg, d + 2, n < sizeof(req_.arg) ? n : sizeof(req_.arg));
        pending_ = true;
    }

    // ---- MainLoop's ----
    bool TakeRequest(Request& r)
    {
        daisy::ScopedIrqBlocker irq;
        if (!pending_)
            return false;
        r = req_;
        pending_ = false;
        return true;
    }
    /** A write opened: tid's, size bytes; nothing taken yet */
    void Open(uint8_t tid, uint32_t size)
    {
        daisy::ScopedIrqBlocker irq;
        tid_ = tid;
        size_ = size;
        taken_ = 0;
        drops_ = 0;
        w_ = r_ = 0;
        paused_ = repeat_ = heard_ = false;
        open_ = true;
    }
    inline void Close() { open_ = false; }
    inline void Pause(bool paused) { paused_ = paused; }
    /** The ring's oldest CHUNK: its packed bytes, its LEN; false when it's empty */
    bool Peek(const uint8_t*& packed, uint8_t& len) const
    {
        if (r_ == w_)
            return false;
        packed = mem_->ring[r_ % kWindow];
        len = lens_[r_ % kWindow];
        return true;
    }
    inline void Pop() { r_ = static_cast<uint8_t>(r_ + 1); }
    inline uint8_t Drops() const { return drops_; }
    bool TakeRepeat() { return Take(repeat_); }
    /** A CHUNK of the open write came since the last ask (the timeout's) */
    bool TakeHeard() { return Take(heard_); }
    /** A CHUNK whose TID isn't the open write's came since the last ask: its TID */
    bool TakeStale(uint8_t& tid)
    {
        tid = stale_tid_;
        return Take(stale_);
    }

private:
    static bool Take(volatile bool& flag)
    {
        daisy::ScopedIrqBlocker irq;
        const bool was = flag;
        flag = false;
        return was;
    }

    /** TID OFFSET(4) LEN DATA (3.7) */
    void Chunk(const uint8_t* d, size_t n)
    {
        if (n < 6)
            return;
        if (!open_ || d[0] != tid_)
        {
            stale_tid_ = d[0];
            stale_ = true;
            return;
        }
        heard_ = true;
        const uint32_t at = Get7(d + 1, 4);
        const uint8_t len = d[5];
        if (at < taken_)
        {
            repeat_ = true;
            return;
        }
        if (at > taken_ || paused_ || !len || len > kMaxChunk || n - 6 != len + (len + 6u) / 7u
            || at + len > size_ || static_cast<uint8_t>(w_ - r_) >= kWindow)
        {
            drops_ = (drops_ + 1) & 0x7F;
            return;
        }
        memcpy(mem_->ring[w_ % kWindow], d + 6, n - 6);
        lens_[w_ % kWindow] = len;
        taken_ += len;
        w_ = static_cast<uint8_t>(w_ + 1);
    }

    // set by Init, so no code sets them at boot
    Mem* mem_;
    Request req_;
    volatile bool pending_;
    // the open write's
    volatile bool open_, paused_;
    uint8_t tid_;
    uint32_t size_, taken_; // taken_: the end of what the ring took
    uint8_t lens_[kWindow];
    volatile uint8_t w_, r_, drops_;
    volatile bool repeat_, heard_, stale_;
    volatile uint8_t stale_tid_;
};

/** FRIZZ-bench.bin's answer (7.2): HELLO, NOT_HERE to the rest; into d, its length */
inline size_t BenchAnswer(const Request& q, uint8_t* d)
{
    d[0] = q.op;
    d[1] = q.seq;
    d[2] = q.op == HELLO && !q.len ? OK : NOT_HERE;
    if (d[2] == OK)
        return Hello(d, 3);
    d[3] = 0;
    return 4;
}

/** Where FRIZZ stands for a request, MainLoop's (chompi_main.cpp) */
struct Env
{
    bool card;      // mounted: else NO_CARD
    bool lent;      // to the computer as a drive (CardLend.h)
    bool recording; // LOOP records
    bool loading;   // the loader loads or checks a loop (or the developers' write test)
    bool card_busy; // a scene save, the master settings or a bug report writes the card
    bool usb;       // a computer has USB configured
};
} // namespace loopxfer

class LoopTransfer
{
public:
    /** send: an answer's bytes after 0x78 (MidiControl::Reply's); log: a job's begin (what, the
     *  slot's id) and end (0x80 | its status, DETAIL), or null; flags: a FRIZZ slot's SLOT flags
     *  1 and 2 (it plays, it's ready, LoopControls'), or null */
    void Init(loopxfer::Intake* intake, LoopLoader* loader, loopxfer::Mem* mem, FATFS* fs, uint32_t (*ms)(),
              void (*send)(const uint8_t* d, size_t n), void (*log)(uint8_t what, uint16_t value),
              uint8_t (*flags)(int id))
    {
        in_ = intake;
        loader_ = loader;
        mem_ = mem;
        fs_ = fs;
        ms_ = ms;
        send_ = send;
        log_ = log;
        flags_ = flags;
        in_->Init(mem);
    }

    /** At boot, before the loader's scan: the temp file a power cut left (5.6) */
    static void Boot() { f_unlink(loopxfer::kTmp); }

    /** From MainLoop, after the loader: a request, the job's step */
    LOOP_LOADER_ONCE void Process(const loopxfer::Env& e)
    {
        using namespace loopxfer;
        const uint32_t now = ms_();
        env_ = e;
        if (Open() && !e.usb)
            End(ABORTED, 0, false); // 1.6
        // 3.8: a running job pauses, and goes on once it's over
        if (phase_ == RUNNING || phase_ == PAUSED)
        {
            const uint8_t why = e.recording ? BUSY_RECORDING : e.loading ? BUSY_LOADING : e.card_busy ? BUSY_CARD : OK;
            if (why && phase_ == RUNNING)
                pause_ms_ = now - kAckEveryMs; // its first ack at once
            else if (!why && phase_ == PAUSED)
            {
                resumed_ = true;
                heard_ms_ = now;
            }
            pause_ = why;
            phase_ = why ? PAUSED : RUNNING;
            in_->Pause(why);
        }
        // the CHUNKs out of the ring (NEXT), while the block has room
        if (job_ == WRITE && phase_ == RUNNING)
        {
            if (in_->TakeHeard())
                heard_ms_ = now;
            const uint8_t* packed;
            uint8_t len;
            while (staged_ <= kBlock && in_->Peek(packed, len))
            {
                uint8_t* to = mem_->stage + staged_;
                Unpack(packed, len, to);
                crc_ = chompi::crash::Crc32More(crc_, to, len);
                staged_ += len;
                next_ += len;
                in_->Pop();
            }
        }
        Request q;
        if (!ans_len_ && in_->TakeRequest(q))
            Handle(q, now);
        const bool card = e.card && !e.lent && !e.loading && !e.card_busy;
        if (card && unlink_)
        {
            f_unlink(kTmp);
            unlink_ = false;
        }
        else if (card && Open() && !e.recording)
            JobStep();
        else if (card && !Open() && fs_->fs_type && !FreeKnown())
            CountStep();
        if (job_ == WRITE && phase_ == RUNNING && now - heard_ms_ >= kTimeoutMs)
            End(TIMED_OUT, 0, false);
        Acks(now);
    }

    /** The pass's messages, from the play page's one answer a pass (NormalPage::Remote): an
     *  ack, which takes no answer's place, then the answer; true if one went */
    bool Send()
    {
        if (ack_len_)
            send_(ack_, ack_len_);
        ack_len_ = 0;
        const bool answer = ans_len_ != 0;
        if (answer)
            send_(ans_, ans_len_);
        ans_len_ = 0;
        return answer;
    }

    /** Before the card is lent (chompi_main.cpp's CardLendProcess): a job ends with CARD_LENT,
     *  its temp file deleted now (3.8) */
    LOOP_LOADER_ONCE void Lend()
    {
        if (Open())
            End(loopxfer::CARD_LENT, 0, false);
        if (unlink_)
            f_unlink(loopxfer::kTmp);
        unlink_ = false;
    }

    /** A FRIZZ slot the transfer changed since the last ask (LoopControls::Forget): its id,
     *  bank * 15 + slot; false for none */
    bool TakeChanged(int& id)
    {
        for (size_t b = 0; b < loops::kFrizzBanks; b++)
            if (changed_[b])
            {
                const int s = __builtin_ctz(changed_[b]);
                changed_[b] &= static_cast<uint16_t>(changed_[b] - 1);
                id = static_cast<int>(b * loops::kSlots) + s;
                return true;
            }
        return false;
    }

    /** A RENAME or SWAP done since the last ask, before its slots' TakeChanged: from's file is
     *  to's now, and with swap to's from's; false for none */
    bool TakeMoved(int& from, int& to, bool& swap)
    {
        if (!moved_[0])
            return false;
        from = moved_[0] - 1;
        to = moved_[1] - 1;
        swap = moved_swap_;
        moved_[0] = 0;
        return true;
    }

    /** No card work waiting: no job's step, no temp file to delete, the free space known
     *  (the unit check's settle) */
    bool Settled() const
    {
        using namespace loopxfer;
        const bool job = Open() && (phase_ == FINISHING || job_ == COPY_JOB || staged_ >= kBlock);
        return !job && !unlink_ && (!fs_->fs_type || FreeKnown());
    }

private:
    inline bool Open() const
    {
        return phase_ == loopxfer::RUNNING || phase_ == loopxfer::PAUSED || phase_ == loopxfer::FINISHING;
    }
    inline bool FreeKnown() const { return fs_->free_clst <= fs_->n_fatent - 2; }
    inline uint32_t FreeKb() const { return fs_->free_clst * fs_->csize / 2u; }
    inline bool Scanning() const
    {
        return loader_->GetState() == LoopLoader::SCANNING
               || (env_.card && !env_.lent && fs_->fs_type && !FreeKnown());
    }
    inline const loops::Slot& Slot(size_t bank, size_t slot) const { return loader_->GetSlot(bank, slot); }
    inline bool Filled(size_t bank, size_t slot) const { return Slot(bank, slot).result != loops::EMPTY; }
    static inline bool Frizz(size_t bank, size_t slot) { return bank < loops::kFrizzBanks && slot < loops::kSlots; }
    inline void Changed(size_t bank, size_t slot) { changed_[bank] |= static_cast<uint16_t>(1u << slot); }

    /** After a card error: bank's slot as the card holds it now, no file empty, one that
     *  won't say unreadable (#294) */
    LOOP_LOADER_ONCE void Recheck(size_t bank, size_t slot)
    {
        char path[LoopLoader::kPathLen];
        loader_->SlotPath(path, bank, slot);
        FILINFO info;
        loops::Slot& s = loader_->SetSlot(bank, slot);
        const FRESULT r = f_stat(path, &info);
        if (r == FR_OK)
            loader_->Walk(path, s);
        else
            s = r == FR_NO_FILE || r == FR_NO_PATH ? loops::Slot{} : loops::Slot{loops::UNREADABLE};
        Changed(bank, slot);
    }

    /** A request's arguments, by its OP; -1 for an OP there's none of */
    static int Args(uint8_t op)
    {
        using namespace loopxfer;
        switch (op)
        {
        case HELLO:
        case STATUS:
        case RESCAN: return 0;
        case BANK:
        case ABORT: return 1;
        case SLOT:
        case NAME:
        case DELETE: return 2;
        case SWAP: return 4;
        case RENAME:
        case COPY: return 5;
        case WRITE_END: return 6;
        case WRITE_BEGIN: return 7;
        default: return -1;
        }
    }

    /** The refusals every changing command but WRITE_END and ABORT gets, in their order (3) */
    uint8_t Refusal() const
    {
        using namespace loopxfer;
        return !env_.card ? NO_CARD : env_.lent ? CARD_LENT : env_.recording ? BUSY_RECORDING
               : env_.loading ? BUSY_LOADING : Scanning() ? BUSY_SCANNING : Open() ? BUSY_JOB
               : env_.card_busy ? BUSY_CARD : OK;
    }

    /** A request answered: its answer into ans_, a changing one's remembered (1.5) */
    LOOP_LOADER_ONCE void Handle(const loopxfer::Request& q, uint32_t now)
    {
        using namespace loopxfer;
        uint8_t* d = ans_;
        d[0] = q.op;
        d[1] = q.seq;
        const int args = Args(q.op);
        const bool changing = args >= 0 && q.op >= WRITE_BEGIN;
        if (changing && remembered_ && q.op == mem_op_ && q.seq == mem_seq_)
        {
            memcpy(d + 2, mem_ans_, mem_len_);
            ans_len_ = static_cast<uint8_t>(2 + mem_len_);
            return;
        }
        detail_out_ = 0;
        size_t n = 3;
        uint8_t st = args < 0 || q.len != args ? BAD : Do(q, d, n, now);
        if (st != OK)
        {
            d[3] = detail_out_;
            n = 4;
        }
        d[2] = st;
        ans_len_ = static_cast<uint8_t>(n);
        if (q.op == HELLO)
            remembered_ = false;
        // the passing refusals aren't remembered: they're tried again
        if (changing && (st < BUSY_RECORDING || st > NO_CARD))
        {
            remembered_ = true;
            mem_op_ = q.op;
            mem_seq_ = q.seq;
            mem_len_ = static_cast<uint8_t>(n - 2);
            memcpy(mem_ans_, d + 2, mem_len_);
        }
    }

    /** The request done: its status, its data after d[n] */
    LOOP_LOADER_ONCE uint8_t Do(const loopxfer::Request& q, uint8_t* d, size_t& n, uint32_t now)
    {
        using namespace loopxfer;
        const uint8_t* a = q.arg;
        switch (q.op)
        {
        case HELLO:
            n = Hello(d, n);
            return OK;
        case STATUS:
        {
            const bool lent = env_.lent, known = env_.card && !lent && fs_->fs_type && FreeKnown();
            d[n++] = static_cast<uint8_t>((env_.card && !lent ? 1 : 0) | (lent ? 2 : 0) | (env_.recording ? 4 : 0)
                                          | (env_.loading ? 8 : 0) | (Scanning() ? 16 : 0) | (env_.card_busy ? 32 : 0)
                                          | (known ? 64 : 0));
            d[n++] = loader_->Tape() ? 1 : 0;
            n = Put7(d, n, known ? FreeKb() : 0, 4);
            d[n++] = job_;
            d[n++] = phase_;
            d[n++] = tid_;
            d[n++] = bank_;
            d[n++] = slot_;
            n = Put7(d, n, next_, 4);
            n = Put7(d, n, total_, 4);
            d[n++] = result_;
            d[n++] = detail_;
            return OK;
        }
        case BANK:
            if (a[0] >= loops::kBanks)
                return BAD;
            d[n++] = a[0];
            d[n++] = loader_->GetState() == LoopLoader::SCANNING;
            for (size_t s = 0; s < loops::kSlots; s++)
                d[n++] = Slot(a[0], s).result;
            return OK;
        case SLOT:
        case NAME:
        {
            if (a[0] >= loops::kBanks || a[1] >= loops::kSlots)
                return BAD;
            const loops::Slot& s = Slot(a[0], a[1]);
            d[n++] = a[0];
            d[n++] = a[1];
            if (q.op == NAME)
            {
                d[n++] = s.name_len;
                n += Pack(reinterpret_cast<const uint8_t*>(s.name), s.name_len, d + n);
                return OK;
            }
            const bool frames = s.result == loops::MONO || s.result == loops::STEREO || s.result == loops::TOO_LONG;
            d[n++] = s.result;
            n = Put7(d, n, frames ? s.frames : 0, 4);
            n = Put7(d, n, s.beats, 2);
            n = Put7(d, n, s.result == loops::EMPTY ? 0 : s.size, 4);
            d[n++] = s.name_len;
            const uint8_t more = flags_ && Frizz(a[0], a[1]) ? flags_(a[0] * loops::kSlots + a[1]) : 0;
            d[n++] = static_cast<uint8_t>((s.name_cut ? 1 : 0) | more << 1);
            return OK;
        }
        case WRITE_END:
        {
            if (job_ != WRITE || (phase_ != RUNNING && phase_ != PAUSED) || a[0] != tid_)
                return NO_TRANSFER;
            heard_ms_ = now;
            if (next_ < total_)
                return INCOMPLETE;
            if (~crc_ != Get7(a + 1, 5))
            {
                End(BAD_CRC, 0, true);
                return BAD_CRC;
            }
            in_->Close();
            phase_ = FINISHING;
            step_ = 1;
            return OK;
        }
        case ABORT:
            if (!Open() || a[0] != tid_)
                return NO_TRANSFER;
            End(ABORTED, 0, true);
            return OK;
        default:
            break;
        }
        // the changing commands
        const uint8_t refused = Refusal();
        if (refused)
            return refused;
        if (q.op == RESCAN)
        {
            loader_->Request(LoopLoader::SCANNING);
            return OK;
        }
        const size_t bank = a[0], slot = a[1];
        if (q.op == WRITE_BEGIN || q.op == COPY)
        {
            const bool write = q.op == WRITE_BEGIN;
            const size_t to_bank = write ? bank : a[2], to_slot = write ? slot : a[3];
            const uint8_t flags = write ? a[2] : a[4];
            if (!Frizz(to_bank, to_slot) || (!write && (bank < loops::kCubbiBank || bank >= loops::kBanks || slot >= loops::kSlots)))
                return NOT_A_SLOT;
            const loops::Slot& from = Slot(write ? 0 : bank, write ? 0 : slot);
            const uint32_t size = write ? Get7(a + 3, 4) : from.size;
            if (!write && from.result == loops::EMPTY)
                return EMPTY;
            if (!write && from.result == loops::TOO_LONG)
                return TOO_LONG;
            if (!write && from.result != loops::MONO && from.result != loops::STEREO)
            {
                detail_out_ = from.result;
                return BAD_WAV;
            }
            if (Filled(to_bank, to_slot) && !(flags & kReplace))
                return EXISTS;
            if (write && size > kMaxFile)
                return TOO_LONG;
            if (write && size < 44)
                return BAD;
            if (static_cast<uint64_t>(size) + kSpare > static_cast<uint64_t>(FreeKb()) * 1024u)
                return NO_SPACE;
            FRESULT r = FR_OK;
            if (!write)
            {
                char path[LoopLoader::kPathLen];
                loader_->SlotPath(path, bank, slot);
                r = f_open(&mem_->src, path, FA_READ);
                src_open_ = r == FR_OK;
            }
            if (r == FR_OK)
            {
                r = f_open(&mem_->file, kTmp, FA_CREATE_ALWAYS | FA_WRITE);
                if (r == FR_NO_PATH && f_mkdir("/FRIZZ") == FR_OK)
                    r = f_open(&mem_->file, kTmp, FA_CREATE_ALWAYS | FA_WRITE);
                file_open_ = r == FR_OK;
            }
            if (r != FR_OK)
            {
                CloseFiles();
                detail_out_ = static_cast<uint8_t>(r);
                return CARD_ERROR;
            }
            unlink_ = false;
            tid_seq_ = static_cast<uint8_t>(tid_seq_ % 127 + 1);
            tid_ = tid_seq_;
            job_ = write ? WRITE : COPY_JOB;
            phase_ = RUNNING;
            bank_ = static_cast<uint8_t>(to_bank);
            slot_ = static_cast<uint8_t>(to_slot);
            src_bank_ = static_cast<uint8_t>(bank);
            src_slot_ = static_cast<uint8_t>(slot);
            replace_ = flags & kReplace;
            total_ = size;
            next_ = written_ = staged_ = 0;
            result_ = detail_ = 0;
            crc_ = 0xFFFFFFFFu;
            heard_ms_ = now;
            resumed_ = false;
            acked_next_ = 0;
            acked_drops_ = 0;
            if (write)
                in_->Open(tid_, size);
            if (log_)
                log_(q.op, static_cast<uint16_t>(to_bank * loops::kSlots + to_slot));
            d[n++] = tid_;
            if (write)
            {
                d[n++] = kMaxChunk;
                d[n++] = kWindow;
            }
            return OK;
        }
        // RENAME, SWAP, DELETE: card renames and deletes in /FRIZZ (3.11)
        const size_t to_bank = a[2], to_slot = a[3];
        if (!Frizz(bank, slot) || (q.op != DELETE && !Frizz(to_bank, to_slot)))
            return NOT_A_SLOT;
        if (q.op != DELETE && bank == to_bank && slot == to_slot)
            return BAD;
        const bool from = Filled(bank, slot), to = q.op != DELETE && Filled(to_bank, to_slot);
        if (!from && (q.op != SWAP || !to))
            return EMPTY;
        if (q.op == RENAME && to && !(a[4] & kReplace))
            return EXISTS;
        const int id = static_cast<int>(bank * loops::kSlots + slot);
        const int to_id = static_cast<int>(to_bank * loops::kSlots + to_slot);
        char pa[LoopLoader::kPathLen], pb[LoopLoader::kPathLen];
        loader_->SlotPath(pa, bank, slot);
        loader_->SlotPath(pb, to_bank, to_slot);
        FRESULT r;
        char tmp[20];
        loops::SwapPath(tmp, bank * loops::kSlots + slot, to_bank * loops::kSlots + to_slot);
        loops::Slot& sa = loader_->SetSlot(bank, slot);
        if (q.op == DELETE)
        {
            r = f_unlink(pa);
            r = r == FR_NO_FILE ? FR_OK : r;
        }
        else
        {
            loops::Slot& sb = loader_->SetSlot(to_bank, to_slot);
            if (q.op == SWAP && from && to)
            {
                // A -> A-B.swp, B -> A, A-B.swp -> B: the temp name says where it belongs (5.6);
                // one failing, the temp file goes on to B if that's free, else back to A, as a
                // scan puts it right
                r = f_rename(pa, tmp);
                if (r == FR_OK)
                    r = f_rename(pb, pa);
                if (r == FR_OK)
                    r = f_rename(tmp, pb);
                if (r != FR_OK && f_rename(tmp, pb) != FR_OK)
                    f_rename(tmp, pa);
            }
            else
            {
                // a move: RENAME, or a SWAP with one side empty
                if (!from)
                {
                    r = f_rename(pb, pa);
                }
                else
                {
                    r = to ? f_unlink(pb) : FR_OK;
                    if (r == FR_OK)
                        r = f_rename(pa, pb);
                }
            }
            if (r == FR_OK)
            {
                const loops::Slot t = sa;
                sa = sb;
                sb = t;
                if (q.op == RENAME)
                    sa = loops::Slot{};
                // the file moved: a loop playing from it goes with it (LoopControls::Moved)
                moved_[0] = static_cast<uint8_t>((from ? id : to_id) + 1);
                moved_[1] = static_cast<uint8_t>((from ? to_id : id) + 1);
                moved_swap_ = q.op == SWAP && from && to;
            }
            Changed(to_bank, to_slot);
        }
        if (r == FR_OK && q.op == DELETE)
            sa = loops::Slot{};
        Changed(bank, slot);
        if (r == FR_OK)
            return OK;
        // a FatFs call failed, maybe after others worked: the slots as the card holds them (#294)
        Recheck(bank, slot);
        if (q.op != DELETE)
            Recheck(to_bank, to_slot);
        detail_out_ = static_cast<uint8_t>(r);
        return CARD_ERROR;
    }

    /** A step of the job: a block written, a block copied, one of 3.9's */
    LOOP_LOADER_ONCE void JobStep()
    {
        using namespace loopxfer;
        if (phase_ == FINISHING)
            Finish();
        else if (phase_ != RUNNING)
            return;
        else if (job_ == WRITE && staged_ >= kBlock && Write(kBlock))
        {
            staged_ -= kBlock;
            memmove(mem_->stage, mem_->stage + kBlock, staged_);
        }
        else if (job_ == COPY_JOB)
        {
            // 32 KB of the cubbi read and written
            const uint32_t n = total_ - next_ < kBlock ? total_ - next_ : kBlock;
            UINT got = 0;
            FRESULT r = f_read(&mem_->src, mem_->stage, n, &got);
            if (r == FR_INVALID_OBJECT && (r = Reopen()) == FR_OK)
                r = f_read(&mem_->src, mem_->stage, n, &got);
            if (r != FR_OK || got != n)
            {
                End(CARD_ERROR, static_cast<uint8_t>(r ? r : FR_INT_ERR), false);
                return;
            }
            if (!Write(n))
                return;
            next_ += n;
            if (next_ == total_)
            {
                CloseFiles();
                phase_ = FINISHING;
                step_ = 2;
            }
        }
    }

    /** n bytes of the block onto xfer.tmp, synced: a remount meanwhile loses nothing */
    LOOP_LOADER_ONCE bool Write(uint32_t n)
    {
        using namespace loopxfer;
        UINT put = 0;
        FRESULT r = f_write(&mem_->file, mem_->stage, n, &put);
        if (r == FR_INVALID_OBJECT && (r = Reopen()) == FR_OK)
            r = f_write(&mem_->file, mem_->stage, n, &put);
        if (r == FR_OK && put != n)
        {
            End(NO_SPACE, 0, false);
            return false;
        }
        if (r == FR_OK)
            r = f_sync(&mem_->file);
        if (r != FR_OK)
        {
            End(CARD_ERROR, static_cast<uint8_t>(r), false);
            return false;
        }
        written_ += n;
        return true;
    }

    /** After a remount (a save's, a bug report's): xfer.tmp opened again at its end, a copy's
     *  cubbi where it was */
    LOOP_LOADER_ONCE FRESULT Reopen()
    {
        CloseFiles();
        FRESULT r = f_open(&mem_->file, loopxfer::kTmp, FA_WRITE);
        file_open_ = r == FR_OK;
        if (r == FR_OK && f_size(&mem_->file) != written_)
            r = FR_INT_ERR;
        if (r == FR_OK)
            r = f_lseek(&mem_->file, written_);
        if (r == FR_OK && job_ == loopxfer::COPY_JOB)
        {
            char path[LoopLoader::kPathLen];
            loader_->SlotPath(path, src_bank_, src_slot_);
            r = f_open(&mem_->src, path, FA_READ);
            src_open_ = r == FR_OK;
            if (r == FR_OK)
                r = f_lseek(&mem_->src, written_);
        }
        return r;
    }

    void CloseFiles()
    {
        if (file_open_)
            f_close(&mem_->file);
        if (src_open_)
            f_close(&mem_->src);
        file_open_ = src_open_ = false;
    }

    /** 3.9's steps 1-3 a pass each (the clusters over passes), 4-6 in one: once the old file is
     *  deleted, the new one takes its place, no ABORT between */
    LOOP_LOADER_ONCE void Finish()
    {
        using namespace loopxfer;
        if (step_ == 1)
        {
            if (staged_ && !Write(staged_))
                return;
            staged_ = 0;
            CloseFiles();
            step_ = 2;
            return;
        }
        if (step_ == 2)
        {
            loader_->Walk(kTmp, walked_);
            if (walked_.result != loops::MONO && walked_.result != loops::STEREO)
            {
                End(BAD_WAV, walked_.result, true);
                return;
            }
            const FRESULT r = f_open(&mem_->file, kTmp, FA_READ);
            file_open_ = r == FR_OK;
            if (r != FR_OK)
            {
                End(CARD_ERROR, static_cast<uint8_t>(r), true);
                return;
            }
            const uint32_t bcs = fs_->csize * loops::kSector;
            clusters_ = (walked_.size + bcs - 1) / bcs;
            cluster_ = frags_ = 0;
            step_ = 3;
            return;
        }
        if (step_ == 3)
        {
            // the clusters followed as the loader's MapStep does, counting the fragments
            const uint32_t bcs = fs_->csize * loops::kSector, size = walked_.size;
            for (uint32_t k = 0; k < loops::kMapClusters && cluster_ < clusters_; k++, cluster_++)
            {
                const uint32_t end = (cluster_ + 1) * bcs < size ? (cluster_ + 1) * bcs : size;
                FRESULT r = f_lseek(&mem_->file, end);
                if (r == FR_INVALID_OBJECT)
                {
                    f_close(&mem_->file);
                    r = f_open(&mem_->file, kTmp, FA_READ);
                    if (r == FR_OK)
                        r = f_lseek(&mem_->file, end);
                }
                if (r != FR_OK)
                {
                    End(CARD_ERROR, static_cast<uint8_t>(r), true);
                    return;
                }
                const DWORD c = mem_->file.clust;
                frags_ += !frags_ || c != last_ + 1;
                last_ = c;
            }
            if (cluster_ < clusters_)
                return;
            CloseFiles();
            if (frags_ > kMaxFragments)
            {
                End(FRAGMENTED, 0, true);
                return;
            }
            step_ = 4;
            return;
        }
        // 4: the slot again (a file put there meanwhile), 5: renamed into place, 6: the slot
        char path[LoopLoader::kPathLen];
        loader_->SlotPath(path, bank_, slot_);
        FILINFO info;
        const bool there = f_stat(path, &info) == FR_OK;
        if (there && !replace_)
        {
            End(EXISTS, 0, true);
            return;
        }
        FRESULT r = there ? f_unlink(path) : FR_OK;
        if (r == FR_OK)
            r = f_rename(kTmp, path);
        if (r != FR_OK)
        {
            End(CARD_ERROR, static_cast<uint8_t>(r), true);
            Recheck(bank_, slot_); // the old file may be gone (#294)
            return;
        }
        loader_->SetSlot(bank_, slot_) = walked_;
        Changed(bank_, slot_);
        phase_ = DONE;
        if (log_)
            log_(0x80, 0);
    }

    /** The job ends with status: its temp file deleted once the card is free; told: the app
     *  had it in an answer, else a write's last ack says it (3.7) */
    LOOP_LOADER_ONCE void End(uint8_t status, uint8_t detail, bool told)
    {
        using namespace loopxfer;
        if (job_ == WRITE && (phase_ == RUNNING || phase_ == PAUSED) && !told)
            end_ack_ = status;
        in_->Close();
        CloseFiles();
        unlink_ = true;
        phase_ = FAILED;
        result_ = status;
        detail_ = detail;
        staged_ = 0;
        if (log_)
            log_(static_cast<uint8_t>(0x80 | status), detail);
    }

    /** The free clusters counted a FAT sector a pass where FSINFO hadn't them (5.4); FatFs
     *  keeps the count from there */
    LOOP_LOADER_ONCE void CountStep()
    {
        const bool fat16 = fs_->fs_type == FS_FAT16;
        const uint32_t per = fat16 ? 256 : 128, first = count_sector_ * per;
        uint8_t* s = mem_->stage;
        if (SdDiskRead(fs_->drv, s, fs_->fatbase + count_sector_, 1) != RES_OK)
            return; // the next pass tries it again
        for (uint32_t i = 0; i < per && first + i < fs_->n_fatent; i++)
        {
            const uint32_t v = fat16 ? loops::Le16(s + 2 * i) : loops::Le32(s + 4 * i) & 0x0FFFFFFF;
            count_free_ += first + i >= 2 && !v;
        }
        count_sector_++;
        if (first + per < fs_->n_fatent)
            return;
        fs_->free_clst = count_free_;
        fs_->fsi_flag |= 1;
        count_sector_ = count_free_ = 0;
    }

    /** The write's ack for this pass, if one is due (3.7): what it took (NEXT) or dropped
     *  moved, a repeat came, a pause began (and every kAckEveryMs of it) or ended, FRIZZ ended
     *  it; else a stale app's NO_TRANSFER, every kAckEveryMs at most */
    LOOP_LOADER_ONCE void Acks(uint32_t now)
    {
        using namespace loopxfer;
        if (ack_len_)
            return;
        uint8_t st = 0xFF, tid = tid_, drops = in_->Drops();
        uint32_t next = next_;
        if (end_ack_)
        {
            st = end_ack_;
            end_ack_ = 0;
        }
        else if (job_ == WRITE && phase_ == RUNNING)
        {
            if (in_->TakeRepeat() || resumed_ || next_ != acked_next_ || drops != acked_drops_)
                st = OK;
            resumed_ = false;
        }
        else if (job_ == WRITE && phase_ == PAUSED && now - pause_ms_ >= kAckEveryMs)
        {
            st = pause_;
            pause_ms_ = now;
        }
        if (st == 0xFF && in_->TakeStale(tid) && (!stale_sent_ || now - stale_ms_ >= kAckEveryMs))
        {
            st = NO_TRANSFER;
            next = drops = 0;
            stale_sent_ = true;
            stale_ms_ = now;
        }
        if (st == 0xFF)
            return;
        if (st != NO_TRANSFER)
        {
            acked_next_ = next;
            acked_drops_ = drops;
        }
        uint8_t* d = ack_;
        d[0] = CHUNK;
        d[1] = 0;
        d[2] = st;
        d[3] = tid;
        Put7(d, 4, next, 4);
        d[8] = drops;
        ack_len_ = 9;
    }

    loopxfer::Intake* in_ = nullptr;
    LoopLoader* loader_ = nullptr;
    loopxfer::Mem* mem_ = nullptr;
    FATFS* fs_ = nullptr;
    uint32_t (*ms_)() = nullptr;
    void (*send_)(const uint8_t*, size_t) = nullptr;
    void (*log_)(uint8_t, uint16_t) = nullptr;
    uint8_t (*flags_)(int) = nullptr;
    loopxfer::Env env_ = {};

    // the answer waiting, the ack; the request remembered (1.5)
    uint8_t ans_[40] = {}, ans_len_ = 0;
    uint8_t ack_[9] = {}, ack_len_ = 0;
    bool remembered_ = false;
    uint8_t mem_op_ = 0, mem_seq_ = 0, mem_len_ = 0, mem_ans_[8] = {};
    uint8_t detail_out_ = 0;

    // the job (STATUS's)
    uint8_t job_ = loopxfer::NONE, phase_ = loopxfer::IDLE, tid_ = 0, tid_seq_ = 0;
    uint8_t bank_ = 0, slot_ = 0, src_bank_ = 0, src_slot_ = 0, result_ = 0, detail_ = 0;
    bool replace_ = false;
    // TakeMoved's: the two slots' ids + 1 (0 none), in replace_'s padding, so the object stays
    // as it was in .bss (#286)
    uint8_t moved_[2] = {};
    bool moved_swap_ = false;
    uint32_t total_ = 0, next_ = 0;  // a write's NEXT, a copy's bytes copied
    uint32_t staged_ = 0, written_ = 0, crc_ = 0;
    uint8_t step_ = 0;               // 3.9's
    bool file_open_ = false, src_open_ = false;
    loops::Slot walked_ = {};        // xfer.tmp's header
    uint32_t clusters_ = 0, cluster_ = 0, frags_ = 0;
    DWORD last_ = 0;
    bool unlink_ = false;            // xfer.tmp to delete once the card's free
    // the pauses, the timeout, the acks
    uint8_t pause_ = 0, end_ack_ = 0, acked_drops_ = 0;
    uint32_t pause_ms_ = 0, heard_ms_ = 0, stale_ms_ = 0, acked_next_ = 0;
    bool resumed_ = false, stale_sent_ = false;
    // the free space's count
    uint32_t count_sector_ = 0, count_free_ = 0;
    uint16_t changed_[loops::kFrizzBanks] = {};
};

#pragma GCC pop_options
