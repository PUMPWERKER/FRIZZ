// store.cpp: checks SceneStore.h's card handling against an SD card in memory (host/fatfs.h):
// the boot read, a .tmp left by a cut save, an unreadable or oversized file kept as .bak (also
// next to a .tmp), a card that couldn't be read at boot, which that session never writes, and
// frizz_master.txt's text and what each of its settings refuses.
// Exits 0 when everything passes. Run by unit.sh store.
#include <cstdio>
#include <string>
#include "check.h"
#include "SceneStore.h"

using namespace chompi;

static FakeCard& card = FakeCard::Get();
static FATFS fs;

static const std::string kScenes = "/FRIZZ/frizz_scenes.txt";
static const std::string kScenesBak = "/FRIZZ/frizz_scenes.bak";
static const std::string kMaster = "/FRIZZ/frizz_master.txt";

static void NewCard(bool present)
{
    card = FakeCard();
    card.present = present;
}

/** A scene file whose used scenes are those in mask (bit s: scene s + 1), each with its first
 *  knob at value */
static std::string SceneText(unsigned mask, float value)
{
    FxScene scenes[kNumScenes];
    for (size_t s = 0; s < kNumScenes; s++)
    {
        scenes[s].used = mask & (1u << s);
        scenes[s].latched = 0;
        for (size_t fx = 0; fx < kNumFx; fx++)
            for (size_t p = 0; p < kNumFxParams; p++)
                scenes[s].params[fx][p] = kFxParams[fx].defaults[p];
        scenes[s].params[0][0] = value;
    }
    static char buf[kSceneFileMax];
    return std::string(buf, FormatScenes(scenes, buf, sizeof(buf)));
}

/** A saved scene in slot (1..4), its first knob at value, as a confirm leaves it */
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

static bool Saves(SceneStore& store)
{
    store.RequestSave();
    store.Process();
    return store.GetSaveState() == SceneStore::SaveState::OK;
}

/** The scenes a file on the card holds, as a mask, and scene `slot`'s first knob */
static unsigned UsedIn(const std::string& text, size_t slot = 0, float* value = nullptr)
{
    float defaults[kNumFx][kNumFxParams];
    for (size_t fx = 0; fx < kNumFx; fx++)
        for (size_t p = 0; p < kNumFxParams; p++)
            defaults[fx][p] = kFxParams[fx].defaults[p];
    FxScene scenes[kNumScenes];
    if (!ParseScenes(text.c_str(), defaults, scenes))
        return 0xff;
    unsigned mask = 0;
    for (size_t s = 0; s < kNumScenes; s++)
        mask |= scenes[s].used ? 1u << s : 0;
    if (value && slot)
        *value = scenes[slot - 1].params[0][0];
    return mask;
}

static void TestBoot()
{
    NewCard(true);
    card.dirs["/FRIZZ"] = true;
    card.files[kScenes] = SceneText(0x3, .25f);
    SceneStore store;
    store.Init(&fs, "");
    Check(store.scenes[1].used && store.scenes[2].used && !store.scenes[3].used,
          "boot: the card's scenes 1 and 2 are read");
    SaveInRam(store, 3, .5f);
    Check(Saves(store) && UsedIn(card.files[kScenes]) == 0x7 && !card.files.count(kScenesBak),
          "boot: a save writes all three, no .bak");

    NewCard(true);
    card.dirs["/FRIZZ"] = true;
    card.files["/FRIZZ/frizz_scenes.tmp"] = SceneText(0x2, .25f);
    store.Init(&fs, "");
    Check(store.scenes[2].used && card.files.count(kScenes) && !card.files.count("/FRIZZ/frizz_scenes.tmp"),
          "boot: a .tmp from a cut save is read and its rename finished");

    NewCard(true);
    card.files["/frizz_scenes.txt"] = SceneText(0x1, .25f);
    store.Init(&fs, "");
    Check(store.scenes[1].used && card.files.count(kScenes), "boot: an old file in the root moves to /FRIZZ");
}

static void TestUnreadable()
{
    NewCard(true);
    card.dirs["/FRIZZ"] = true;
    card.files[kScenes] = "FRIZZ scenes 2\nsomething else\n";
    SceneStore store;
    store.Init(&fs, "");
    SaveInRam(store, 1, .5f);
    Check(Saves(store) && card.files[kScenesBak] == "FRIZZ scenes 2\nsomething else\n" &&
              UsedIn(card.files[kScenes]) == 0x1,
          "unreadable: kept as .bak on the first save");

    NewCard(true);
    card.dirs["/FRIZZ"] = true;
    card.files[kScenes] = "FRIZZ scenes 2\n";
    store.Init(&fs, "");
    card.files.erase(kScenes);
    SaveInRam(store, 1, .5f);
    Check(Saves(store) && UsedIn(card.files[kScenes]) == 0x1,
          "unreadable, then deleted before the save: the save still works");
    SaveInRam(store, 2, .5f);
    Check(Saves(store), "... and the next one too");

    // a save cut short next to a file that can't be read: the .tmp is read and finished,
    // and the unreadable file is kept as .bak, as a save keeps it
    NewCard(true);
    card.dirs["/FRIZZ"] = true;
    card.files[kScenes] = "FRIZZ scenes 2\nsomething else\n";
    card.files["/FRIZZ/frizz_scenes.tmp"] = SceneText(0x2, .25f);
    card.files[kScenesBak] = "an older .bak\n";
    store.Init(&fs, "");
    Check(store.scenes[2].used && UsedIn(card.files[kScenes]) == 0x2
              && !card.files.count("/FRIZZ/frizz_scenes.tmp")
              && card.files[kScenesBak] == "FRIZZ scenes 2\nsomething else\n",
          "unreadable, with a .tmp from a cut save: the .tmp read and finished, the file kept as .bak");
    SaveInRam(store, 3, .5f);
    Check(Saves(store) && UsedIn(card.files[kScenes]) == 0x6
              && card.files[kScenesBak] == "FRIZZ scenes 2\nsomething else\n",
          "... and the next save keeps that .bak");

    // a valid file padded past what the buffer holds would be read cut short
    NewCard(true);
    card.dirs["/FRIZZ"] = true;
    const std::string big = SceneText(0x1, .25f) + std::string(kSceneFileMax, '#');
    card.files[kScenes] = big;
    store.Init(&fs, "");
    Check(!store.scenes[1].used, "too long: not read");
    SaveInRam(store, 2, .5f);
    Check(Saves(store) && card.files[kScenesBak] == big, "... and kept as .bak on the first save");
}

static void TestNotRead()
{
    // the mount failed at boot (FRIZZ itself came from the card, so it's there)
    NewCard(false);
    SceneStore store;
    store.Init(&fs, "");
    SaveInRam(store, 3, .5f);
    Check(!Saves(store), "not read at boot: the save fails");

    // the card answers now, with scenes 1 and 2 and master settings: none of it is touched
    NewCard(true);
    card.dirs["/FRIZZ"] = true;
    const std::string scenes = SceneText(0x3, .25f);
    const std::string master = "FRIZZ master 1\ncompressor 300000 0 0 0\nmono 1\n";
    card.files[kScenes] = scenes;
    card.files[kMaster] = master;
    Check(!Saves(store), "not read at boot: a save fails while the card answers too");
    store.RequestMasterSave();
    store.Process();
    Check(store.TakeMasterFailed(), "not read at boot: so does a master save");
    Check(card.files[kScenes] == scenes && card.files[kMaster] == master && card.files.size() == 2,
          "... and the card is untouched");

    // after a power cycle it's read, and written again
    store.Init(&fs, "");
    SaveInRam(store, 3, .5f);
    Check(Saves(store) && UsedIn(card.files[kScenes]) == 0x7, "read at the next boot: written again");
}

/** frizz_master.txt's text, line for line, and how each setting reads what it can't take */
static void TestMasterFormat()
{
    MasterSettings a, b;
    a.Reset();
    for (size_t p = 0; p < kNumFxParams; p++)
        a.comp[p] = .125f * p;
    a.mono = true;
    a.midi_channel = 3;
    a.midi_transport = true;
    a.clock_factor = 50;
    a.clock_source = 2;
    a.midi_out = 1;
    a.led_brightness = 75;
    char buf[kMasterFileMax];
    const size_t len = FormatMaster(a, buf, sizeof(buf));
    const std::string want = "FRIZZ master 1\n"
                             "compressor2 0 125000 250000 375000 500000 625000 750000 875000\n"
                             "mono 1\nmidi_channel 3\nmidi_transport 1\nclock_factor 50\n"
                             "clock_source 2\nmidi_out 1\nled_brightness 75\n";
    Check(std::string(buf) == want && len == want.size(), "master: the file's text, line for line");
    a.Reset();
    FormatMaster(a, buf, sizeof(buf));
    Check(std::string(buf).find("\nmono 0\nmidi_channel 16\nmidi_transport 0\nclock_factor 100\n"
                                "clock_source 0\nmidi_out 0\nled_brightness 100\n") != std::string::npos,
          "master: the defaults' text");

    Check(ParseMaster("FRIZZ master 1\nmono 7\nmidi_transport -1\n", b) && b.mono && b.midi_transport,
          "master: a switch is on for any number but 0");
    Check(ParseMaster("FRIZZ master 1\nmono\nmidi_channel\nclock_factor\n", b) && !b.mono
              && b.midi_channel == 16 && b.clock_factor == 100,
          "master: a setting without its number keeps its default");
    Check(ParseMaster("FRIZZ master 1\nmidi_channel x\nclock_source x\nmidi_out x\n", b)
              && b.midi_channel == 0 && b.clock_source == 0 && b.midi_out == 0,
          "master: a word for a number reads as 0 (strtol), where 0 is allowed");
    Check(ParseMaster("FRIZZ master 1\nclock_factor 150\nled_brightness 60\nmidi_channel -1\n"
                      "clock_source -1\nmidi_out -2\n", b)
              && b.clock_factor == 100 && b.led_brightness == 100 && b.midi_channel == 16
              && b.clock_source == 0 && b.midi_out == 0,
          "master: values between or below the allowed ones keep the defaults");
    Check(ParseMaster("FRIZZ master 1\nclock_factor 200\nled_brightness 50\nmidi_channel 16\n"
                      "clock_source 3\nmidi_out 2\nmono 0\nmidi_channel_x 4\n", b)
              && b.clock_factor == 200 && b.led_brightness == 50 && b.midi_channel == 16
              && b.clock_source == 3 && b.midi_out == 2,
          "master: the highest allowed values are read, a longer name isn't its setting");
}

int main()
{
    TestBoot();
    TestUnreadable();
    TestNotRead();
    TestMasterFormat();
    return Finish();
}
