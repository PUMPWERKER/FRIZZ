// Host stand-in for FatFs: an SD card in memory, just enough for SceneStore.h. The card can be
// taken out (card.present), filled and read back (card.files, keyed by full path), made to
// refuse writes (card.read_only) or run full (card.space: bytes a write can still add), and it
// counts the files open (card.open), so a check sees one left open. As in ff.c, a mount
// (f_mount, also of the volume already mounted) makes the files open before it invalid: their
// reads, writes and close fail with FR_INVALID_OBJECT.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>

typedef unsigned int UINT;
typedef unsigned char BYTE;

enum FRESULT
{
    FR_OK = 0,
    FR_DISK_ERR,
    FR_NOT_READY,
    FR_NO_FILE,
    FR_EXIST,
    FR_DENIED,
    FR_INVALID_OBJECT,
};

#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_CREATE_ALWAYS 0x08

struct FATFS
{
};
struct FILINFO
{
};
struct FIL
{
    std::string path;
    size_t fptr = 0;
    bool write = false;
    uint32_t mount = 0; // the mount it was opened on (ff.c's obj.id)
};
#define f_eof(fp) ((int)((fp)->fptr == FakeCard::Get().files[(fp)->path].size()))

struct FakeCard
{
    bool present = true;
    bool read_only = false;
    size_t space = SIZE_MAX;
    int open = 0;
    uint32_t mount = 0; // counts the mounts (ff.c's fs->id)
    void (*write_time)(size_t bytes) = nullptr; // a write taking time (the twin's), if set
    std::string cwd = "";
    std::map<std::string, std::string> files;
    std::map<std::string, bool> dirs;

    static FakeCard& Get()
    {
        static FakeCard card;
        return card;
    }
    std::string Path(const char* name) const
    {
        return name[0] == '/' ? std::string(name) : cwd + "/" + name;
    }
};

inline FRESULT f_mount(FATFS*, const char*, BYTE)
{
    FakeCard& c = FakeCard::Get();
    c.cwd = "";
    c.mount++;
    return c.present ? FR_OK : FR_NOT_READY;
}
inline FRESULT f_chdir(const char* path)
{
    FakeCard& c = FakeCard::Get();
    if (!c.present || !c.dirs.count(path))
        return FR_NO_FILE;
    c.cwd = path;
    return FR_OK;
}
inline FRESULT f_mkdir(const char* path)
{
    FakeCard& c = FakeCard::Get();
    if (!c.present || c.read_only)
        return FR_DENIED;
    c.dirs[path] = true;
    return FR_OK;
}
inline FRESULT f_stat(const char* name, FILINFO*)
{
    FakeCard& c = FakeCard::Get();
    return c.present && c.files.count(c.Path(name)) ? FR_OK : FR_NO_FILE;
}
inline FRESULT f_unlink(const char* name)
{
    FakeCard& c = FakeCard::Get();
    if (!c.present)
        return FR_NOT_READY;
    if (!c.files.count(c.Path(name)))
        return FR_NO_FILE;
    if (c.read_only)
        return FR_DENIED;
    c.files.erase(c.Path(name));
    return FR_OK;
}
inline FRESULT f_rename(const char* from, const char* to)
{
    FakeCard& c = FakeCard::Get();
    if (!c.present)
        return FR_NOT_READY;
    if (!c.files.count(c.Path(from)))
        return FR_NO_FILE;
    if (c.files.count(c.Path(to)))
        return FR_EXIST;
    if (c.read_only)
        return FR_DENIED;
    c.files[c.Path(to)] = c.files[c.Path(from)];
    c.files.erase(c.Path(from));
    return FR_OK;
}
inline FRESULT f_open(FIL* f, const char* name, BYTE mode)
{
    FakeCard& c = FakeCard::Get();
    if (!c.present)
        return FR_NOT_READY;
    f->path = c.Path(name);
    f->fptr = 0;
    f->write = mode & FA_WRITE;
    f->mount = c.mount;
    if (f->write)
    {
        if (c.read_only)
            return FR_DENIED;
        c.files[f->path].clear();
        c.open++;
        return FR_OK;
    }
    if (!c.files.count(f->path))
        return FR_NO_FILE;
    c.open++;
    return FR_OK;
}
inline FRESULT f_read(FIL* f, void* buf, UINT n, UINT* read)
{
    *read = 0;
    if (f->mount != FakeCard::Get().mount)
        return FR_INVALID_OBJECT;
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
    *written = 0;
    if (f->mount != c.mount)
        return FR_INVALID_OBJECT;
    if (c.write_time)
        c.write_time(n);
    // a full card: FatFs writes what fits and says so in *written, with FR_OK
    n = static_cast<UINT>(std::min<size_t>(n, c.space));
    c.space -= n;
    const char* src = static_cast<const char*>(buf);
    std::string& file = c.files[f->path];
    const size_t head = std::min<size_t>(n, (kSectorSize - f->fptr % kSectorSize) % kSectorSize);
    const size_t whole = (n - head) / kSectorSize * kSectorSize;
    file.append(src, head);
    const size_t shift = reinterpret_cast<uintptr_t>(src + head) % 4;
    for (size_t i = 0; i < whole; i++)
        file.push_back(head + i >= shift ? src[head + i - shift] : '\0');
    file.append(src + head + whole, n - head - whole);
    f->fptr += n;
    *written = n;
    return FR_OK;
}
inline FRESULT f_close(FIL* f)
{
    FakeCard& c = FakeCard::Get();
    c.open -= c.open > 0;
    return f->mount == c.mount ? FR_OK : FR_INVALID_OBJECT;
}
