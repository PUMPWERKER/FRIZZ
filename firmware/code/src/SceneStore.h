/** @file SceneStore.h
 *  @brief The FX scenes (FxScenes.h) and their file on the SD card, frizz_scenes.txt in the
 *  current directory, /FRIZZ (EnterFrizzDir), or the root if that can't be made. Read once at
 *  boot; written only when the play page saves, copies or deletes one, never on a recall, so a
 *  performance doesn't touch the card.
 *
 *  Next to it, frizz_master.txt holds the master compressor's knobs, the mono input and the
 *  MIDI settings (MasterSettings.h): read at boot, written when the play page asks, a while after they were
 *  last turned. It goes through the same .tmp; one that can't be read is simply replaced, it
 *  holds little.
 *
 *  The write runs in MainLoop (Process), not in the button handler: the audio callback runs
 *  the UI during boot. It goes to frizz_scenes.tmp first, which then replaces the old file,
 *  so a power cut mid-write leaves one or the other. Reading falls back to the .tmp, and
 *  then finishes that save, so the next one doesn't overwrite the only good copy. A file that
 *  is there but can't be read (another version, edited into something else) isn't
 *  overwritten: the next save moves it to frizz_scenes.bak first, or the boot, when it
 *  finishes a .tmp.
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

static const char kSceneFile[] = "frizz_scenes.txt";
static const char kSceneTmpFile[] = "frizz_scenes.tmp";
static const char kSceneBakFile[] = "frizz_scenes.bak";
static const char kMasterFile[] = "frizz_master.txt";
static const char kMasterTmpFile[] = "frizz_master.tmp";

// FRIZZ's folder on the card, so it can share a card with other firmwares (the launcher at
// github.com/sfaber02/CHOMPI gives each its own folder)
static const char kFrizzDir[] = "/FRIZZ";

/** After f_mount: makes /FRIZZ the current directory, creating it on a card that doesn't have
 *  it yet and moving the scene files a FRIZZ before it left in the root into it. Every path
 *  FRIZZ opens is relative, so they all land there. If the folder can't be made (a file in
 *  the way, a read-only card), FRIZZ stays in the root. Before audio starts */
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
        for (size_t s = 0; s < kNumScenes; s++)
            Saved()[s].used = false;
        master.Reset();
        loaded_ = Mount();
        if (!loaded_)
            return;
        LoadMaster();
        unreadable_ = !LoadScenes(Saved());
    }

    /** From the play page after a change: the next Process writes the file */
    inline void RequestSave() { save_state_ = SaveState::PENDING; }
    /** From the play page, master changed: the next Process writes its file */
    inline void RequestMasterSave() { master_pending_ = true; }

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

    /** From MainLoop */
    void Process()
    {
        if (save_state_ == SaveState::PENDING)
        {
            Remount();
            failed_ = !(loaded_ && mounted_ && Save());
            save_state_ = failed_ ? SaveState::FAILED : SaveState::OK;
        }
        if (master_pending_)
        {
            master_pending_ = false;
            Remount();
            failed_ = !(loaded_ && mounted_);
            const size_t len = FormatMaster(master, buf_, kMasterFileMax);
            failed_ = failed_ || !(len && WriteText(kMasterFile, kMasterTmpFile, len));
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

    bool Load(const char* name, const float (*defaults)[kNumFxParams], FxScene* into)
    {
        return ReadText(name) && ParseScenes(buf_, defaults, into);
    }

    /** The card's scenes into `into`, or a .tmp a save cut short before its rename, which it
     *  finishes. False if a scene file is there but can't be read */
    bool LoadScenes(FxScene* into)
    {
        // for the effects a saved scene leaves out
        float defaults[kNumFx][kNumFxParams];
        for (size_t fx = 0; fx < kNumFx; fx++)
            for (size_t p = 0; p < kNumFxParams; p++)
                defaults[fx][p] = kFxParams[fx].defaults[p];

        if (Load(kSceneFile, defaults, into))
            return true;
        const bool there = Exists(kSceneFile);
        if (Load(kSceneTmpFile, defaults, into))
        {
            // the file that couldn't be read is kept, as a save keeps it; if it can't be
            // moved, the .tmp stays too, and the next save tries again
            if (there && !KeepUnreadable())
                return false;
            FinishRename(kSceneFile, kSceneTmpFile);
            return true;
        }
        return !there;
    }

    /** A scene file that couldn't be read to frizz_scenes.bak, in place of the one there;
     *  true once it's out of the way, or gone meanwhile (deleted, or the card swapped) */
    static bool KeepUnreadable()
    {
        const FRESULT del = f_unlink(kSceneBakFile);
        if (del != FR_OK && del != FR_NO_FILE)
            return false;
        const FRESULT moved = f_rename(kSceneFile, kSceneBakFile);
        return moved == FR_OK || moved == FR_NO_FILE;
    }

    /** The master settings, or a .tmp a save cut short before its rename, which it finishes */
    void LoadMaster()
    {
        if (ReadText(kMasterFile) && ParseMaster(buf_, master))
            return;
        if (ReadText(kMasterTmpFile) && ParseMaster(buf_, master))
            FinishRename(kMasterFile, kMasterTmpFile);
    }

    /** A save cut short before its rename (only tmp was read): finish it */
    static void FinishRename(const char* name, const char* tmp)
    {
        const FRESULT del = f_unlink(name);
        if (del == FR_OK || del == FR_NO_FILE)
            f_rename(tmp, name);
    }

    /** After a failure: mount the card read at boot again */
    void Remount()
    {
        if (loaded_ && (!mounted_ || failed_))
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
            if (!KeepUnreadable())
                return false;
            unreadable_ = false;
        }

        return WriteText(kSceneFile, kSceneTmpFile, len);
    }

    FATFS* fs_ = nullptr;
    const char* path_ = nullptr;
    bool unreadable_ = false; // a scene file is there that couldn't be read, or would be lost
    bool loaded_ = false;     // the card was read at boot: only then is it written
    bool failed_ = false;     // the last save failed
    FIL file_;
    // FatFs reads whole sectors straight into it by DMA, so on a cache line of its own
    alignas(32) char buf_[kSceneFileMax];
    bool mounted_;
    volatile SaveState save_state_;
    bool master_pending_ = false;
    bool master_failed_ = false;
};

} // namespace chompi
