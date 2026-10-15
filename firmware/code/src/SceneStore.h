/** @file SceneStore.h
 *  @brief The FX scenes (FxScenes.h) and their file on the SD card, /FRIZZ/frizz_scenes.txt
 *  (EnterFrizzDir makes the folder). Read once at
 *  boot; written only when the play page saves, copies or deletes one, never on a recall, so a
 *  performance doesn't touch the card.
 *
 *  Next to it, frizz_master.txt holds the finalizer's knobs and presets, the mono input and the
 *  MIDI settings (MasterSettings.h): read at boot, written when the play page asks, a while after they were
 *  last turned. It goes through the same .tmp; one that's there but can't be read (#168) is
 *  kept as frizz_master.bak by the next save, as the scenes' is.
 *
 *  The write runs in MainLoop (Process), not in the button handler: the audio callback runs
 *  the UI during boot. It goes to frizz_scenes.tmp first, which then replaces the old file,
 *  so a power cut mid-write leaves one or the other. Reading falls back to the .tmp, and
 *  then finishes that save, so the next one doesn't overwrite the only good copy. A file that
 *  is there but can't be read (another version, edited into something else) isn't
 *  overwritten: the next save moves it to frizz_scenes.bak first, or the boot, when it
 *  finishes a .tmp.
 *
 *  The card lent to the computer as a USB drive (CardLend.h) and taken back: Reread reads
 *  each file again that the computer changed (its text against what FRIZZ last read or wrote
 *  of it, a hash), as at boot, when nothing of it waits to be saved on the device. If
 *  something does (a scene saved meanwhile, a setting changed, still in its rest), the device
 *  wins: its save moves the computer's file to the .bak, as it does an unreadable one. One the
 *  computer left unreadable goes there too, and the device's own is written at once, so the
 *  card holds one that reads; one it deleted is written again by the next save. An unchanged
 *  file isn't read again at all.
 *
 *  For a test on the device, KeepInRam (kCmdDev's PERSIST 0) stops the writes until a
 *  restart: the session's scenes and settings stay in RAM, the card as it was at boot.
 *
 *  A write that fails is reported (GetSaveState); the next save mounts the card again and
 *  tries again. A card that couldn't be read at boot is never written in that session: the
 *  scenes work in RAM only, and a save would replace the card's with the session's. The
 *  bootloader loads FRIZZ from the card, so that's a mount that failed, not a missing card;
 *  pulling or swapping the card while FRIZZ runs isn't looked after (MANUAL.md).
 */
#pragma once
#include "daisy.h"
#include "fatfs.h"
#include "FxScenes.h"
#include "MasterSettings.h"

namespace chompi
{

// FRIZZ's folder on the card, so it can share a card with other firmwares (the launcher at
// github.com/sfaber02/CHOMPI gives each its own folder)
static const char kFrizzDir[] = "/FRIZZ";

// Whole paths, never relative ones: FatFs goes back to the root at every mount, also one of
// its own (a card that didn't answer is mounted again inside the next f_open), and a save
// there would land in the root and be lost at the next power-on
static const char kSceneFile[] = "/FRIZZ/frizz_scenes.txt";
static const char kSceneTmpFile[] = "/FRIZZ/frizz_scenes.tmp";
static const char kSceneBakFile[] = "/FRIZZ/frizz_scenes.bak";
static const char kMasterFile[] = "/FRIZZ/frizz_master.txt";
static const char kMasterTmpFile[] = "/FRIZZ/frizz_master.tmp";
static const char kMasterBakFile[] = "/FRIZZ/frizz_master.bak";

/** After f_mount: creates /FRIZZ on a card that doesn't have it yet (or no longer: the computer
 *  deleted it while the card was lent) and moves the scene files a FRIZZ before it left in the
 *  root into it, and makes it the current directory. If the folder can't be made (a file in
 *  the way, a read-only card), FRIZZ's files can't be written: a save fails. Before audio
 *  starts */
inline void EnterFrizzDir()
{
    if (f_chdir(kFrizzDir) == FR_OK)
        return;
    if (f_mkdir(kFrizzDir) != FR_OK)
        return;

    // FRIZZ's earlier home: the root
    static const char* const kOldFiles[][2] = {
        {"/frizz_scenes.txt", "/FRIZZ/frizz_scenes.txt"},
        {"/frizz_scenes.tmp", "/FRIZZ/frizz_scenes.tmp"},
    };
    for (const auto& f : kOldFiles)
        f_rename(f[0], f[1]); // FR_NO_FILE when there's nothing to move

    f_chdir(kFrizzDir);
}

class SceneStore
{
public:
    enum class SaveState
    {
        IDLE,    // nothing saved yet
        PENDING, // requested, the next Process writes it
        OK,      // the last save is on the card
        FAILED,  // the last save isn't: no card, or the write failed
    };

    /** Mounts the card (fs, path: FatFSInterface's), enters /FRIZZ and reads the scenes;
     *  before audio starts */
    void Init(FATFS* fs, const char* path)
    {
        fs_ = fs;
        path_ = path;
        save_state_ = SaveState::IDLE;
        unreadable_ = false;
        master_unreadable_ = false;
        for (size_t s = 0; s < kNumScenes; s++)
            Saved()[s].used = false;
        master.Reset();
        loaded_ = Mount();
        if (!loaded_)
            return;
        LoadMaster();
        unreadable_ = !LoadScenes(Saved());
        // what's on the card now, for Reread
        master_hash_ = FileHash(kMasterFile);
        scenes_hash_ = FileHash(kSceneFile);
    }

    // Reread: the master settings taken from the card; bit s, slot s's scene changed
    static const uint8_t kRereadMaster = 0x80;

    /** From MainLoop, the card taken back from the computer (CardLend.h): mounts it again and
     *  reads what the computer changed of FRIZZ's files (the file comment). master_waiting: the
     *  play page has a master change not yet asked to be saved (NormalPage::MasterWaiting).
     *  Returns what it took into scenes and master: kRereadMaster, and a bit per slot that
     *  changed */
    __attribute__((noinline, optimize("Os"))) uint8_t Reread(bool master_waiting)
    {
        if (!Mount() || !loaded_)
            return 0;
        uint8_t taken = 0;
        const uint32_t master_hash = FileHash(kMasterFile);
        if (master_hash != master_hash_)
        {
            master_hash_ = master_hash;
            MasterSettings read;
            if (master_hash > 1 && !master_waiting && !master_pending_ && ParseMaster(buf_, read))
            {
                master = read;
                master_unreadable_ = false;
                taken = kRereadMaster;
            }
            else if (master_hash)
            {
                // the device's wins, the computer's goes to the .bak
                master_unreadable_ = true;
                master_pending_ = true;
            }
        }
        const uint32_t scenes_hash = FileHash(kSceneFile);
        if (scenes_hash != scenes_hash_)
        {
            scenes_hash_ = scenes_hash;
            FxScene read[kNumScenes];
            if (scenes_hash > 1 && save_state_ != SaveState::PENDING && ParseSaved(read))
            {
                for (size_t s = 0; s < kNumScenes; s++)
                {
                    FxScene& to = Saved()[s];
                    if (to.used != read[s].used
                        || (to.used
                            && (to.latched != read[s].latched || memcmp(to.params, read[s].params, sizeof(to.params)))))
                        taken |= 1u << (kBlankSlot + 1 + s);
                    to = read[s];
                }
                unreadable_ = false;
            }
            else if (scenes_hash)
            {
                unreadable_ = true;
                save_state_ = SaveState::PENDING;
            }
        }
        return taken;
    }

    /** From the play page after a change: the next Process writes the file */
    inline void RequestSave() { save_state_ = SaveState::PENDING; }
    /** From the play page, master changed: the next Process writes its file */
    inline void RequestMasterSave() { master_pending_ = true; }

    /** Until a restart, nothing more is written to the card (kCmdDev's PERSIST 0, for a
     *  test on the device): a save requested goes through as if it was written, so the
     *  play page shows it as saved, and the card keeps what it had at boot */
    inline void KeepInRam() { in_ram_ = true; }
    inline bool InRam() const { return in_ram_; }

    /** The card mounted, as the last mount left it (the loop transfer's NO_CARD) */
    inline bool Mounted() const { return mounted_; }

    /** A save requested and not yet written */
    inline bool Busy() const { return save_state_ == SaveState::PENDING || master_pending_; }

    /** How the last requested save went, for the play page's confirmation */
    inline SaveState GetSaveState() const { return save_state_; }
    /** True once after a master save failed, so the play page can show it and try again */
    bool TakeMasterFailed()
    {
        const bool failed = master_failed_;
        master_failed_ = false;
        return failed;
    }

    /** From MainLoop. log_open: a bug report is being written (EventLog::Writing); a remount
     *  would make its open file invalid, so a write that needs one waits for it */
    void Process(bool log_open)
    {
        if (in_ram_)
        {
            if (save_state_ == SaveState::PENDING)
                save_state_ = SaveState::OK;
            master_pending_ = false;
            return;
        }
        if (save_state_ == SaveState::PENDING && !(log_open && RemountDue()))
        {
            Remount();
            failed_ = !(loaded_ && mounted_ && Save());
            save_state_ = failed_ ? SaveState::FAILED : SaveState::OK;
        }
        if (master_pending_ && !(log_open && RemountDue()))
        {
            master_pending_ = false;
            Remount();
            failed_ = !(loaded_ && mounted_);
            const size_t len = FormatMaster(master, buf_, kMasterFileMax);
            // a file that couldn't be read is kept, not overwritten
            if (!failed_ && len && master_unreadable_)
            {
                failed_ = !KeepUnreadable(kMasterFile, kMasterBakFile);
                master_unreadable_ = failed_;
            }
            failed_ = failed_ || !(len && WriteText(kMasterFile, kMasterTmpFile, len));
            if (!failed_)
                master_hash_ = Hash(len);
            master_failed_ = failed_;
        }
    }

    /** The play page's slots; SceneControls fills the blank one (kBlankSlot) */
    FxScene scenes[kNumSlots];
    /** What the play page keeps outside the scenes: read at boot, written on RequestMasterSave */
    MasterSettings master;

private:
    /** The slots the file holds, its scene 1 first */
    inline FxScene* Saved() { return scenes + kBlankSlot + 1; }

    /** (Re)mounts the card and enters /FRIZZ; false without one */
    bool Mount()
    {
        mounted_ = f_mount(fs_, path_, 1) == FR_OK;
        if (mounted_)
            EnterFrizzDir();
        return mounted_;
    }

    inline bool Exists(const char* name)
    {
        FILINFO info;
        return f_stat(name, &info) == FR_OK;
    }

    /** The file's text into buf_, terminated; false for one too long for it, which would be
     *  read cut short */
    bool ReadText(const char* name)
    {
        if (f_open(&file_, name, FA_READ) != FR_OK)
            return false;
        UINT len = 0;
        const FRESULT res = f_read(&file_, buf_, kSceneFileMax - 1, &len);
        const bool whole = f_eof(&file_);
        f_close(&file_);
        if (res != FR_OK || !whole)
            return false;
        buf_[len] = '\0';
        return true;
    }

    /** buf_'s first len bytes to tmp, which then replaces name */
    bool WriteText(const char* name, const char* tmp, size_t len)
    {
        if (f_open(&file_, tmp, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
            return false;
        UINT written = 0;
        const FRESULT res = f_write(&file_, buf_, len, &written);
        if (f_close(&file_) != FR_OK || res != FR_OK || written != len)
            return false;

        // f_rename won't replace a file
        const FRESULT del = f_unlink(name);
        if (del != FR_OK && del != FR_NO_FILE)
            return false;
        return f_rename(tmp, name) == FR_OK;
    }

    /** buf_'s text hashed (FNV-1a), never 0 or 1 */
    __attribute__((optimize("Os"))) uint32_t Hash(size_t len) const
    {
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < len; i++)
            h = (h ^ static_cast<uint8_t>(buf_[i])) * 16777619u;
        return h | 2;
    }

    /** The file's text hashed, for Reread: 0 if it isn't there, 1 if it can't be read whole.
     *  Leaves its text in buf_ */
    __attribute__((noinline, optimize("Os"))) uint32_t FileHash(const char* name)
    {
        if (!ReadText(name))
            return Exists(name) ? 1 : 0;
        return Hash(strlen(buf_));
    }

    /** buf_'s scenes into `into`; false if they can't be read */
    __attribute__((noinline, optimize("Os"))) bool ParseSaved(FxScene* into)
    {
        // for the effects a saved scene leaves out
        float defaults[kNumFx][kNumFxParams];
        for (size_t fx = 0; fx < kNumFx; fx++)
            for (size_t p = 0; p < kNumFxParams; p++)
                defaults[fx][p] = kFxParams[fx].defaults[p];
        return ParseScenes(buf_, defaults, into);
    }

    /** The card's scenes into `into`, or a .tmp a save cut short before its rename, which it
     *  finishes. False if a scene file is there but can't be read */
    bool LoadScenes(FxScene* into)
    {
        if (ReadText(kSceneFile) && ParseSaved(into))
            return true;
        const bool there = Exists(kSceneFile);
        if (ReadText(kSceneTmpFile) && ParseSaved(into))
        {
            // the file that couldn't be read is kept, as a save keeps it; if it can't be
            // moved, the .tmp stays too, and the next save tries again
            if (there && !KeepUnreadable(kSceneFile, kSceneBakFile))
                return false;
            FinishRename(kSceneFile, kSceneTmpFile);
            return true;
        }
        return !there;
    }

    /** A file that couldn't be read to its .bak, in place of the one there; true once it's
     *  out of the way, or gone meanwhile (deleted, or the card swapped) */
    static bool KeepUnreadable(const char* name, const char* bak)
    {
        const FRESULT del = f_unlink(bak);
        if (del != FR_OK && del != FR_NO_FILE)
            return false;
        const FRESULT moved = f_rename(name, bak);
        return moved == FR_OK || moved == FR_NO_FILE;
    }

    /** The master settings, or a .tmp a save cut short before its rename, which it finishes;
     *  a file there that can't be read is kept for the next save to move aside */
    void LoadMaster()
    {
        if (ReadText(kMasterFile) && ParseMaster(buf_, master))
            return;
        const bool there = Exists(kMasterFile);
        if (ReadText(kMasterTmpFile) && ParseMaster(buf_, master))
        {
            if (there && !KeepUnreadable(kMasterFile, kMasterBakFile))
                return;
            FinishRename(kMasterFile, kMasterTmpFile);
            return;
        }
        master_unreadable_ = there;
    }

    /** A save cut short before its rename (only tmp was read): finish it */
    static void FinishRename(const char* name, const char* tmp)
    {
        const FRESULT del = f_unlink(name);
        if (del == FR_OK || del == FR_NO_FILE)
            f_rename(tmp, name);
    }

    /** After a failure: mount the card read at boot again */
    inline bool RemountDue() const { return loaded_ && (!mounted_ || failed_); }
    void Remount()
    {
        if (RemountDue())
            Mount();
    }

    bool Save()
    {
        const size_t len = FormatScenes(Saved(), buf_, kSceneFileMax);
        if (len == 0)
            return false;

        // a file that couldn't be read is kept, not overwritten
        if (unreadable_)
        {
            if (!KeepUnreadable(kSceneFile, kSceneBakFile))
                return false;
            unreadable_ = false;
        }

        if (!WriteText(kSceneFile, kSceneTmpFile, len))
            return false;
        scenes_hash_ = Hash(len);
        return true;
    }

    FATFS* fs_ = nullptr;
    const char* path_ = nullptr;
    // a scene file is there that couldn't be read, or would be lost, or the computer's that the
    // device's save replaces (Reread): the next save keeps it as the .bak
    bool unreadable_ = false;
    bool loaded_ = false;     // the card was read at boot: only then is it written
    bool failed_ = false;     // the last save failed
    FIL file_;
    // FatFs reads whole sectors straight into it by DMA, so on a cache line of its own
    alignas(32) char buf_[kSceneFileMax];
    bool mounted_;
    volatile SaveState save_state_;
    bool master_pending_ = false;
    bool master_failed_ = false;
    bool master_unreadable_ = false; // a master file is there that couldn't be read, or the computer's
    uint32_t master_hash_ = 0;       // each file's text as FRIZZ last read or wrote it (FileHash)
    uint32_t scenes_hash_ = 0;
    bool in_ram_ = false;
};

} // namespace chompi
