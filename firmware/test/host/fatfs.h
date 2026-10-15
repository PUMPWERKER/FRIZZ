// Host stand-in for FatFs: an SD card in memory, just enough for SceneStore.h. The card can be
// taken out (card.present), filled and read back (card.files, keyed by full path), made to
// refuse writes (card.read_only) or run full (card.space: bytes a write can still add), and it
// counts the files open (card.open), so a check sees one left open. As in ff.c, a mount
// (f_mount, also of the volume already mounted) makes the files open before it invalid: their
// reads, writes and close fail with FR_INVALID_OBJECT. For the loop loader (LoopLoader.h) it has
// folders to list (f_opendir, f_readdir), FatFs's fast seek's link map (f_lseek with
// CREATE_LINKMAP) over clusters it hands each file on the first, a seek's cluster (ff.c's
// fp->clust) and the FAT sectors following the chain reads (fat_reads), and the raw sector reads
// SdCard.h makes past FatFs; it counts the FatFs calls made while one of those runs (raw_clash),
// which on the device would put a command on the bus in the middle of its transfer.
// For the loop transfer (LoopTransfer.h): a file opened for writing as it is (FA_OPEN_EXISTING)
// and written where its pointer is; what was written since its last f_sync or f_close lost by a
// remount, as FatFs leaves its directory entry; names found in any case, as FAT's are; the free
// clusters (clusters less the files' own), FSINFO's count at the mount unless fsinfo is false,
// then kept as FatFs keeps it, and the FAT's sectors to count them (FatRead); the name changes
// (a file created, renamed, deleted, a folder made) counted, and a power cut after the cut-th
// (cut_after): the card gone from there, what was open for writing back to its last sync; or
// the one after the fail_after-th failing (FR_DISK_ERR, nothing changed), once, the card staying;
// the same for a file's data: the f_read, f_write or f_sync after the fail_io_after-th (io counts
// them) failing, once. Unmounted (f_mount(nullptr), the card lent as a USB drive), it counts
// every call made until it's mounted again (unmounted_calls), raw reads and the mount itself
// too: FRIZZ leaves the card alone while the computer has it.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

typedef unsigned int UINT;
typedef unsigned char BYTE;
typedef uint32_t DWORD;

// FatFs's numbers (ff.h), as the loop transfer's CARD_ERROR reports them
enum FRESULT
{
    FR_OK = 0,
    FR_DISK_ERR = 1,
    FR_INT_ERR = 2,
    FR_NOT_READY = 3,
    FR_NO_FILE = 4,
    FR_NO_PATH = 5,
    FR_DENIED = 7,
    FR_EXIST = 8,
    FR_INVALID_OBJECT = 9,
    FR_NOT_ENOUGH_CORE = 17,
};

#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_OPEN_EXISTING 0x00
#define FA_CREATE_ALWAYS 0x08
#define FS_FAT16 2
#define FS_FAT32 3
typedef int DRESULT;
#define RES_OK 0
#define RES_ERROR 1
#define CREATE_LINKMAP ((DWORD)0 - 1)

struct FATFS
{
    BYTE fs_type = 0;    // FS_FAT32 once mounted, 0 without a card
    BYTE drv = 0;
    BYTE fsi_flag = 0;
    uint16_t csize = 0;  // sectors a cluster, set by the mount
    DWORD free_clst = 0; // ~0u: not known
    DWORD n_fatent = 0;  // the clusters + 2
    DWORD fatbase = 0;
    DWORD database = 0;  // the first cluster's sector
};
#define AM_DIR 0x10
struct FILINFO
{
    DWORD fsize = 0;
    BYTE fattrib = 0; // AM_DIR for a folder
    char fname[256] = {};
};
struct FIL
{
    std::string path;
    size_t fptr = 0;
    bool write = false;
    uint32_t mount = 0;     // the mount it was opened on (ff.c's obj.id)
    DWORD* cltbl = nullptr; // fast seek's link map
    DWORD clust = 0;        // the cluster the pointer is in, after a seek (ff.c's)
};
struct DIR
{
    std::vector<std::string> names;
    std::vector<DWORD> sizes; // ~0u: a folder
    size_t at = 0;
    uint32_t mount = 0; // the mount it was opened on: a remount makes it invalid, as a FIL
};
#define f_eof(fp) ((int)((fp)->fptr == FakeCard::Get().files[(fp)->path].size()))
#define f_size(fp) ((DWORD)FakeCard::Get().files[(fp)->path].size())

struct FakeCard
{
    bool present = true;
    bool read_only = false;
    size_t space = SIZE_MAX;
    int open = 0;
    uint32_t mount = 0; // counts the mounts (ff.c's fs->id)
    uint32_t dir_reads = 0; // f_readdir's calls
    void (*write_time)(size_t bytes) = nullptr; // a write taking time (the twin's), if set
    std::string cwd = "";
    std::map<std::string, std::string> files;
    std::map<std::string, bool> dirs;
    // the files open for writing, and their size at the last sync (a remount's loss)
    std::map<std::string, size_t> synced;
    // the free space: the volume's clusters, FSINFO's count there at the mount or not
    DWORD clusters = 1u << 20;
    bool fsinfo = true;
    FATFS* fs = nullptr; // the volume mounted
    // the name changes made, and a power cut after the cut_after-th (-1: none)
    int changes = 0;
    int cut_after = -1;
    int fail_after = -1;
    // a file's reads, writes and syncs made, and the one after the fail_io_after-th failing
    int io = 0;
    int fail_io_after = -1;
    // unmounted (lent), and the calls made meanwhile
    bool unmounted = false;
    int unmounted_calls = 0;

    // the volume's geometry, and where each file's clusters are: given out on its first link
    // map, in fragments of frag_clusters with a free cluster between them
    uint16_t csize = 8;
    DWORD database = 1000;
    DWORD next_cluster = 2;
    DWORD frag_clusters = 1u << 30;
    struct Frag
    {
        DWORD first, count; // clusters
        std::string path;
        size_t offset; // the file's bytes there start at
    };
    std::vector<Frag> frags;
    std::map<std::string, std::vector<size_t>> chains; // the file's frags, in order
    // ff.c reads the FAT a sector at a time into its window (FAT32: 128 clusters a sector);
    // the sectors read so, counted
    DWORD fat_win = ~0u;
    int fat_reads = 0;
    // a raw read: done on the raw_polls-th poll; raw_fail fails it (raw_error), raw_dead keeps
    // the card from coming back after it
    int raw_busy = 0;
    int raw_polls = 2;
    bool raw_fail = false;
    bool raw_dead = false;
    uint32_t raw_error = 0;
    int raw_reads = 0;
    int raw_clash = 0; // FatFs calls made while a raw read ran

    static FakeCard& Get()
    {
        static FakeCard card;
        return card;
    }
    std::string Path(const char* name) const
    {
        return name[0] == '/' ? std::string(name) : cwd + "/" + name;
    }
    /** The file there in any case, as FAT finds it; path itself if there's none */
    std::string Find(const std::string& path) const
    {
        if (files.count(path))
            return path;
        for (const auto& file : files)
            if (file.first.size() == path.size()
                && std::equal(file.first.begin(), file.first.end(), path.begin(),
                              [](char a, char b) { return (a | 0x20) == (b | 0x20); }))
                return file.first;
        return path;
    }

    /** The clusters the files take, and the free ones */
    DWORD Used() const
    {
        const size_t bcs = csize * size_t(512);
        size_t used = 0;
        for (const auto& file : files)
            used += (file.second.size() + bcs - 1) / bcs;
        return static_cast<DWORD>(used);
    }
    DWORD Free() const { return Used() < clusters ? clusters - Used() : 0; }
    /** FatFs keeps a count it has up to date */
    void KeepFree()
    {
        if (fs && fs->free_clst <= fs->n_fatent - 2)
            fs->free_clst = Free();
    }
    /** A FAT sector (FAT32's): the clusters the files take first, the rest free */
    void FatRead(DWORD sector, BYTE* buf) const
    {
        const DWORD used = Used();
        for (DWORD i = 0; i < 128; i++)
        {
            const DWORD e = (sector - (fs ? fs->fatbase : 0)) * 128 + i;
            const DWORD v = e < 2 || e - 2 < used ? 0x0FFFFFFF : 0;
            memcpy(buf + 4 * i, &v, 4);
        }
    }
    /** A name changed: the power cut, if this was the last before it */
    void Changed()
    {
        if (++changes == cut_after)
            Cut();
    }
    /** A file's read, write or sync, counted: true for the one after the fail_io_after-th,
     *  which fails (FR_DISK_ERR, nothing read or written), once */
    bool IoFails()
    {
        const bool fails = fail_io_after >= 0 && io == fail_io_after;
        io++;
        if (fails)
            fail_io_after = -1;
        return fails;
    }
    /** A name change that fails: the one after the fail_after-th, once */
    bool Fails()
    {
        if (fail_after < 0 || changes != fail_after)
            return false;
        fail_after = -1;
        return true;
    }
    /** The power gone: the card as it is, what was open for writing as at its last sync */
    void Cut()
    {
        for (const auto& open : synced)
            if (files.count(open.first))
                files[open.first].resize(std::min(files[open.first].size(), open.second));
        synced.clear();
        present = false;
        cut_after = -1;
    }

    /** A folder made, or one a file is in (put on the card as a whole path) */
    bool HasDir(const std::string& dir) const
    {
        if (dirs.count(dir))
            return true;
        for (const auto& d : dirs)
            if (!d.first.compare(0, dir.size() + 1, dir + "/"))
                return true;
        for (const auto& file : files)
            if (!file.first.compare(0, dir.size() + 1, dir + "/"))
                return true;
        return false;
    }

    /** Each FatFs call: none while a raw read runs */
    void Touch()
    {
        raw_clash += raw_busy > 0;
        unmounted_calls += unmounted;
    }

    /** The file's clusters, given out on first asking; a write past them adds more (Grow),
     *  as FatFs allocates them: its last fragment goes on while it's the last given out and has
     *  room, else a new one. A file changed by other means keeps its chain (a broken FAT) */
    const std::vector<size_t>& Chain(const std::string& path)
    {
        return chains.count(path) ? chains[path] : Grow(path);
    }
    const std::vector<size_t>& Grow(const std::string& path)
    {
        std::vector<size_t>& chain = chains[path];
        const size_t cluster = csize * size_t(512);
        const size_t clusters = (files[path].size() + cluster - 1) / cluster;
        size_t at = 0;
        for (size_t f : chain)
            at += frags[f].count;
        if (at < clusters && !chain.empty())
        {
            Frag& last = frags[chain.back()];
            if (last.first + last.count + 1 == next_cluster && last.count < frag_clusters)
            {
                const DWORD n = static_cast<DWORD>(std::min<size_t>(frag_clusters - last.count, clusters - at));
                last.count += n;
                next_cluster += n;
                at += n;
            }
        }
        while (at < clusters)
        {
            const DWORD n = static_cast<DWORD>(std::min<size_t>(frag_clusters, clusters - at));
            chain.push_back(frags.size());
            frags.push_back({next_cluster, n, path, at * cluster});
            next_cluster += n + 1;
            at += n;
        }
        return chain;
    }

    /** The chain's i-th cluster, 0 past its end */
    DWORD Cluster(const std::vector<size_t>& chain, size_t i) const
    {
        for (size_t f : chain)
        {
            if (i < frags[f].count)
                return frags[f].first + static_cast<DWORD>(i);
            i -= frags[f].count;
        }
        return 0;
    }
    /** get_fat's read of the cluster's entry: a sector into the window, unless it's there */
    void FatRead(DWORD cluster)
    {
        if (cluster / 128 != fat_win)
            fat_reads++;
        fat_win = cluster / 128;
    }

    /** As SdRawStart: 1 started, 0 busy, -1 refused (past the card's end, as the HAL does) */
    int RawStart(uint8_t* to, DWORD sector, DWORD count)
    {
        if (raw_busy)
            return 0;
        Touch();
        raw_error = 0;
        if (sector >= 0xF0000000u)
        {
            raw_error = 0x2000000; // HAL_SD_ERROR_ADDR_OUT_OF_RANGE
            return -1;
        }
        for (DWORD s = sector; s < sector + count; s++, to += 512)
        {
            memset(to, 0xEE, 512);
            if (s < database)
                continue;
            const DWORD cl = (s - database) / csize + 2, in = (s - database) % csize;
            for (const Frag& f : frags)
                if (cl >= f.first && cl < f.first + f.count)
                {
                    const std::string& data = files[f.path];
                    const size_t at = f.offset + (size_t(cl - f.first) * csize + in) * 512;
                    if (at < data.size())
                        memcpy(to, data.data() + at, std::min<size_t>(512, data.size() - at));
                }
        }
        raw_busy = raw_polls;
        raw_reads++;
        return 1;
    }
    /** 0 busy, 1 done, -1 failed */
    int RawPoll()
    {
        if (raw_busy > 1)
        {
            raw_busy--;
            return 0;
        }
        raw_busy = 0;
        raw_error = raw_fail ? 0x20 : 0; // HAL_SD_ERROR_RX_OVERRUN
        return raw_fail ? -1 : 1;
    }
    bool RawRecover()
    {
        raw_busy = 0;
        return !raw_dead;
    }
};

inline FRESULT f_mount(FATFS* fs, const char*, BYTE)
{
    FakeCard& c = FakeCard::Get();
    c.Touch();
    // what was open is no more, and what was written to it since its last sync is lost
    for (const auto& open : c.synced)
        if (c.files.count(open.first))
            c.files[open.first].resize(std::min(c.files[open.first].size(), open.second));
    c.synced.clear();
    c.mount++;
    if (!fs)
    {
        // unmounted (the card lent as a USB drive, CardLend.h)
        if (c.fs)
            c.fs->fs_type = 0;
        c.unmounted = true;
        return FR_OK;
    }
    c.unmounted = false;
    c.fs = fs;
    fs->csize = c.csize;
    fs->database = c.database;
    fs->fs_type = c.present ? FS_FAT32 : 0;
    fs->n_fatent = c.clusters + 2;
    fs->fatbase = 32;
    fs->fsi_flag = 0;
    fs->free_clst = c.fsinfo ? c.Free() : ~0u;
    c.cwd = "";
    c.fat_win = ~0u;
    return c.present ? FR_OK : FR_NOT_READY;
}
inline FRESULT f_chdir(const char* path)
{
    FakeCard& c = FakeCard::Get();
    c.Touch();
    if (!c.present || !c.HasDir(path))
        return FR_NO_FILE;
    c.cwd = path;
    return FR_OK;
}
inline FRESULT f_mkdir(const char* path)
{
    FakeCard& c = FakeCard::Get();
    c.Touch();
    if (!c.present || c.read_only)
        return FR_DENIED;
    if (c.HasDir(path))
        return FR_EXIST;
    c.dirs[path] = true;
    c.Changed();
    return FR_OK;
}
inline FRESULT f_stat(const char* name, FILINFO*)
{
    FakeCard& c = FakeCard::Get();
    c.Touch();
    if (!c.present)
        return FR_NOT_READY;
    return c.files.count(c.Find(c.Path(name))) ? FR_OK : FR_NO_FILE;
}
inline FRESULT f_unlink(const char* name)
{
    FakeCard& c = FakeCard::Get();
    c.Touch();
    if (!c.present)
        return FR_NOT_READY;
    if (!c.files.count(c.Find(c.Path(name))))
    {
        // a folder: only an empty one
        const std::string dir = c.Path(name);
        if (!c.HasDir(dir))
            return FR_NO_FILE;
        c.dirs.erase(dir);
        if (c.HasDir(dir))
        {
            c.dirs[dir] = true;
            return FR_DENIED;
        }
        return FR_OK;
    }
    if (c.read_only)
        return FR_DENIED;
    if (c.Fails())
        return FR_DISK_ERR;
    const std::string path = c.Find(c.Path(name));
    c.files.erase(path);
    c.chains.erase(path);
    c.KeepFree();
    c.Changed();
    return FR_OK;
}
inline FRESULT f_rename(const char* from, const char* to)
{
    FakeCard& c = FakeCard::Get();
    c.Touch();
    if (!c.present)
        return FR_NOT_READY;
    const std::string src = c.Find(c.Path(from));
    if (!c.files.count(src))
        return FR_NO_FILE;
    if (c.files.count(c.Find(c.Path(to))))
        return FR_EXIST;
    if (c.read_only)
        return FR_DENIED;
    if (c.Fails())
        return FR_DISK_ERR;
    c.files[c.Path(to)] = c.files[src];
    c.files.erase(src);
    c.chains.erase(c.Path(to));
    if (c.chains.count(src))
    {
        // the clusters go with it
        c.chains[c.Path(to)] = c.chains[src];
        for (size_t f : c.chains[src])
            c.frags[f].path = c.Path(to);
        c.chains.erase(src);
    }
    c.Changed();
    return FR_OK;
}
inline FRESULT f_open(FIL* f, const char* name, BYTE mode)
{
    FakeCard& c = FakeCard::Get();
    c.Touch();
    if (!c.present)
        return FR_NOT_READY;
    f->path = c.Find(c.Path(name));
    f->fptr = 0;
    f->write = mode & FA_WRITE;
    f->mount = c.mount;
    f->cltbl = nullptr;
    if (f->write && (mode & FA_CREATE_ALWAYS))
    {
        if (c.read_only)
            return FR_DENIED;
        c.files[f->path].clear(); // an existing one keeps its name's case
        c.chains.erase(f->path);
        c.synced[f->path] = 0;
        c.open++;
        c.KeepFree();
        c.Changed();
        return FR_OK;
    }
    // FAT's names are found in any case
    if (!c.files.count(f->path))
        return FR_NO_FILE;
    if (f->write && c.read_only)
        return FR_DENIED;
    if (f->write)
        c.synced[f->path] = c.files[f->path].size();
    c.open++;
    return FR_OK;
}
inline FRESULT f_read(FIL* f, void* buf, UINT n, UINT* read)
{
    FakeCard::Get().Touch();
    *read = 0;
    if (!FakeCard::Get().present)
        return FR_NOT_READY;
    if (f->mount != FakeCard::Get().mount)
        return FR_INVALID_OBJECT;
    if (FakeCard::Get().IoFails())
        return FR_DISK_ERR;
    const std::string& data = FakeCard::Get().files[f->path];
    const size_t len = std::min<size_t>(n, data.size() - f->fptr);
    memcpy(buf, data.data() + f->fptr, len);
    f->fptr += len;
    *read = static_cast<UINT>(len);
    return FR_OK;
}
// FatFs passes the whole sectors of a write straight from the caller's buffer to the SD
// card's DMA, which on the device reads from a word-aligned address: from a buffer that isn't
// word-aligned there, the sectors come out shifted, bytes repeated at their start and lost at
// their end (a bug report's file, 2026-10-08). The card here does the same, so the checks
// see it. Only the part before the first sector boundary and after the last goes through
// FatFs's own buffer, untouched
static const size_t kSectorSize = 512;
inline FRESULT f_write(FIL* f, const void* buf, UINT n, UINT* written)
{
    FakeCard& c = FakeCard::Get();
    c.Touch();
    *written = 0;
    if (!c.present)
        return FR_NOT_READY;
    if (f->mount != c.mount)
        return FR_INVALID_OBJECT;
    if (c.IoFails())
        return FR_DISK_ERR;
    if (c.write_time)
        c.write_time(n);
    // a full card: FatFs writes what fits and says so in *written, with FR_OK
    n = static_cast<UINT>(std::min<size_t>(n, c.space));
    c.space -= n;
    const char* src = static_cast<const char*>(buf);
    std::string out;
    const size_t head = std::min<size_t>(n, (kSectorSize - f->fptr % kSectorSize) % kSectorSize);
    const size_t whole = (n - head) / kSectorSize * kSectorSize;
    out.append(src, head);
    const size_t shift = reinterpret_cast<uintptr_t>(src + head) % 4;
    for (size_t i = 0; i < whole; i++)
        out.push_back(head + i >= shift ? src[head + i - shift] : '\0');
    out.append(src + head + whole, n - head - whole);
    // where the pointer is: over what's there, and on past its end
    std::string& file = c.files[f->path];
    if (file.size() < f->fptr)
        file.resize(f->fptr);
    file.replace(f->fptr, std::min(out.size(), file.size() - f->fptr), out);
    f->fptr += n;
    *written = n;
    if (c.chains.count(f->path))
        c.Grow(f->path);
    c.KeepFree();
    return FR_OK;
}
/** What was written so far kept, past a remount */
inline FRESULT f_sync(FIL* f)
{
    FakeCard& c = FakeCard::Get();
    c.Touch();
    if (!c.present)
        return FR_NOT_READY;
    if (f->mount != c.mount)
        return FR_INVALID_OBJECT;
    if (c.IoFails())
        return FR_DISK_ERR;
    if (f->write && c.synced.count(f->path))
        c.synced[f->path] = c.files[f->path].size();
    return FR_OK;
}
inline FRESULT f_close(FIL* f)
{
    FakeCard& c = FakeCard::Get();
    c.Touch();
    c.open -= c.open > 0;
    if (f->mount != c.mount)
        return FR_INVALID_OBJECT;
    if (f->write)
        c.synced.erase(f->path);
    return c.present ? FR_OK : FR_NOT_READY;
}

/** CREATE_LINKMAP fills cltbl as ff.c does: its size, then each fragment's length and first
 *  cluster, then 0; FR_NOT_ENOUGH_CORE, with the size it needs, when it's too small. Any other
 *  offset moves the file's pointer and sets clust to the cluster it's in: by cltbl if set (fast
 *  seek), else following the chain, from the cluster it was in if that's not after it, as ff.c
 *  does: FR_INT_ERR past the chain's end */
inline FRESULT f_lseek(FIL* f, DWORD ofs)
{
    FakeCard& c = FakeCard::Get();
    c.Touch();
    if (!c.present)
        return FR_NOT_READY;
    if (f->mount != c.mount)
        return FR_INVALID_OBJECT;
    if (ofs != CREATE_LINKMAP)
    {
        const size_t to = std::min<size_t>(ofs, c.files[f->path].size()), bcs = c.csize * size_t(512);
        if (to && f->cltbl)
        {
            DWORD cl = static_cast<DWORD>((to - 1) / bcs);
            const DWORD* t = f->cltbl + 1;
            for (; t[0] && cl >= t[0]; t += 2)
                cl -= t[0];
            if (!t[0])
                return FR_INT_ERR;
            f->clust = t[1] + cl;
        }
        else if (to)
        {
            const std::vector<size_t>& chain = c.Chain(f->path);
            const size_t k = (to - 1) / bcs;
            size_t i = f->fptr && k >= (f->fptr - 1) / bcs ? (f->fptr - 1) / bcs : 0;
            for (; i < k; i++)
            {
                c.FatRead(c.Cluster(chain, i));
                if (!c.Cluster(chain, i + 1))
                    return FR_INT_ERR;
            }
            f->clust = c.Cluster(chain, k);
        }
        f->fptr = to;
        return FR_OK;
    }
    if (!f->cltbl)
        return FR_INT_ERR;
    DWORD* tbl = f->cltbl;
    const DWORD tlen = *tbl++;
    DWORD ulen = 2;
    for (size_t i : c.Chain(f->path))
    {
        for (DWORD k = 0; k < c.frags[i].count; k++)
            c.FatRead(c.frags[i].first + k);
        ulen += 2;
        if (ulen <= tlen)
        {
            *tbl++ = c.frags[i].count;
            *tbl++ = c.frags[i].first;
        }
    }
    *f->cltbl = ulen;
    if (ulen > tlen)
        return FR_NOT_ENOUGH_CORE;
    *tbl = 0;
    return FR_OK;
}
inline FRESULT f_opendir(DIR* d, const char* path)
{
    FakeCard& c = FakeCard::Get();
    c.Touch();
    std::string dir = c.Path(path);
    if (dir == "/")
        dir = ""; // the root: every path starts with its "/"
    if (!c.present)
        return FR_NOT_READY;
    if (!dir.empty() && !c.HasDir(dir))
        return FR_NO_PATH;
    d->names.clear();
    d->sizes.clear();
    d->at = 0;
    d->mount = c.mount;
    // its files, then its folders (made, or a file's in them)
    std::map<std::string, bool> folders;
    auto child = [&](const std::string& p, DWORD size) {
        if (p.size() <= dir.size() + 1 || p.compare(0, dir.size() + 1, dir + "/"))
            return;
        const size_t slash = p.find('/', dir.size() + 1);
        if (slash == std::string::npos && size != ~0u)
        {
            d->names.push_back(p.substr(dir.size() + 1));
            d->sizes.push_back(size);
        }
        else
            folders[p.substr(dir.size() + 1, slash == std::string::npos ? std::string::npos : slash - dir.size() - 1)] = true;
    };
    for (const auto& file : c.files)
        child(file.first, static_cast<DWORD>(file.second.size()));
    for (const auto& made : c.dirs)
        child(made.first, ~0u);
    for (const auto& f : folders)
    {
        d->names.push_back(f.first);
        d->sizes.push_back(~0u);
    }
    return FR_OK;
}
inline FRESULT f_readdir(DIR* d, FILINFO* info)
{
    FakeCard& c = FakeCard::Get();
    c.Touch();
    info->fname[0] = '\0';
    if (d->mount != c.mount)
        return FR_INVALID_OBJECT;
    c.dir_reads++;
    if (d->at < d->names.size())
    {
        snprintf(info->fname, sizeof(info->fname), "%s", d->names[d->at].c_str());
        const bool folder = d->sizes[d->at] == ~0u;
        info->fsize = folder ? 0 : d->sizes[d->at];
        info->fattrib = folder ? AM_DIR : 0;
        d->at++;
    }
    return FR_OK;
}
inline FRESULT f_closedir(DIR*)
{
    FakeCard::Get().Touch();
    return FR_OK;
}
