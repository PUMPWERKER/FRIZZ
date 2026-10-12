// store.cpp: checks SceneStore.h's card handling against an SD card in memory (host/fatfs.h):
// the boot read, a .tmp left by a cut save, an unreadable or oversized file kept as .bak (also
// next to a .tmp), a card that couldn't be read at boot, which that session never writes, and
// frizz_master.txt's text and what each of its settings refuses.
// Exits 0 when everything passes. Run by unit.sh store.
#include <cmath>
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
static const std::string kMasterBak = "/FRIZZ/frizz_master.bak";

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
    a.midi_block_trs = false;
    a.midi_feedback = true;
    a.aux_dry = true;
    a.gain_aux = 100;
    a.gain_mic = 36;
    a.mic_dry = true;
    a.aux_live = false;
    a.mic_live = true;
    char buf[kMasterFileMax];
    const size_t len = FormatMaster(a, buf, sizeof(buf));
    const std::string want = "FRIZZ master 1\n"
                             "compressor2 0 125000 250000 375000 500000 625000 750000 875000\n"
                             "mono 1\nmidi_channel 3\nmidi_transport 1\nclock_factor 50\n"
                             "clock_source 2\nmidi_out 1\nled_brightness 75\nmidi_block_trs 0\nmidi_feedback 1\n"
                             "aux_dry 1\ngain_aux 100\ngain_mic 36\nmic_dry 1\naux_live 0\nmic_live 1\n";
    Check(std::string(buf) == want && len == want.size(), "master: the file's text, line for line");
    a.Reset();
    FormatMaster(a, buf, sizeof(buf));
    Check(std::string(buf).find("\nmono 0\nmidi_channel 16\nmidi_transport 0\nclock_factor 100\n"
                                "clock_source 0\nmidi_out 0\nled_brightness 100\nmidi_block_trs 1\nmidi_feedback 0\n"
                                "aux_dry 0\ngain_aux 75\ngain_mic 75\nmic_dry 0\naux_live 1\nmic_live 0\n")
              != std::string::npos,
          "master: the defaults' text");
    Check(ParseMaster("FRIZZ master 1\nmono 1\n", b) && b.midi_block_trs,
          "master: a file from before midi_block_trs has the effect channels over the jack too");
    Check(ParseMaster("FRIZZ master 1\ncompressor2 0 0 0 0 0 0 0 0\nmono 1\nmidi_channel 3\nmidi_feedback 1\n", b)
              && !b.aux_dry && b.gain_aux == 75 && b.gain_mic == 75 && b.mono && b.midi_feedback,
          "master: a file from before #152 loads, AUX through the effects, both input gains at 75 %");
    Check(ParseMaster("FRIZZ master 1\naux_dry 1\ngain_aux 60\n", b) && b.aux_dry && !b.mic_dry
              && b.aux_live && !b.mic_live,
          "master: a file from before #177 loads, its aux_dry AUX's effects, AUX live, the mic through them, not live");
    Check(ParseMaster(buf, b) && !b.aux_dry && b.gain_aux == 75,
          "master: the defaults read back");
    Check(ParseMaster("FRIZZ master 1\naux_dry 1\ngain_aux 101\ngain_mic 0\n", b) && b.aux_dry
              && b.gain_aux == 75 && b.gain_mic == 0,
          "master: an input gain over 100 keeps its default, 0 is read");
    {
        // the longest file there can be still fits
        MasterSettings m;
        m.Reset();
        for (size_t p = 0; p < kNumFxParams; p++)
            m.comp[p] = .123456f;
        m.midi_channel = 16;
        m.clock_factor = 200;
        m.led_brightness = 100;
        m.gain_aux = m.gain_mic = 100;
        char big[kMasterFileMax];
        const size_t n = FormatMaster(m, big, sizeof(big));
        printf("      the longest settings file: %zu of %zu bytes\n", n, kMasterFileMax);
        Check(n > 0 && n < kMasterFileMax - 64, "master: the longest file fits, with room to spare");
    }

    Check(ParseMaster("FRIZZ master 1\nmono 7\nmidi_transport -1\n", b) && b.mono && b.midi_transport,
          "master: a switch is on for any number but 0");
    Check(ParseMaster("FRIZZ master 1\nmono\nmidi_channel\nclock_factor\n", b) && !b.mono
              && b.midi_channel == 16 && b.clock_factor == 100,
          "master: a setting without its number keeps its default");
    Check(!ParseMaster("FRIZZ master 1\nmono 1\nmidi_channel x\n", b) && !b.mono
              && b.midi_channel == 16,
          "master: a word for a number makes the file unreadable, every setting on its default (#168)");
    Check(!ParseMaster("FRIZZ master 1\ngain_aux 7O\n", b) && b.gain_aux == 75,
          "master: so does a number with a letter in it: a garbled gain isn't a silent input");
    Check(ParseMaster("FRIZZ master 1\nmidi_channel 0\n", b) && b.midi_channel == 0,
          "master: 0 is still read where it's allowed");
    Check(ParseMaster("FRIZZ master 1\nclock_factor 150\nled_brightness 20\nmidi_channel -1\n"
                      "clock_source -1\nmidi_out -2\n", b)
              && b.clock_factor == 100 && b.led_brightness == 100 && b.midi_channel == 16
              && b.clock_source == 0 && b.midi_out == 0,
          "master: values between or below the allowed ones keep the defaults");
    Check(ParseMaster("FRIZZ master 1\nled_brightness 25\n", b) && b.led_brightness == 25
              && ParseMaster("FRIZZ master 1\nled_brightness 60\n", b) && b.led_brightness == 60
              && ParseMaster("FRIZZ master 1\nled_brightness 101\n", b) && b.led_brightness == 100,
          "master: the brightness reads 25-100 (#176), over it keeps the default");
    Check(ParseMaster("FRIZZ master 1\nclock_factor 200\nled_brightness 50\nmidi_channel 16\n"
                      "clock_source 3\nmidi_out 2\nmono 0\nmidi_channel_x 4\n", b)
              && b.clock_factor == 200 && b.led_brightness == 50 && b.midi_channel == 16
              && b.clock_source == 3 && b.midi_out == 2,
          "master: the highest allowed values are read, a longer name isn't its setting");
}

/** A master file that's there but can't be read (#168) is kept as frizz_master.bak by the
 *  next master save, not overwritten */
static void TestMasterUnreadable()
{
    NewCard(true);
    card.dirs["/FRIZZ"] = true;
    const std::string garbled = "FRIZZ master 1\nmono 1\ngain_aux 7O\n";
    card.files[kMaster] = garbled;
    SceneStore store;
    store.Init(&fs, "");
    Check(!store.master.mono && store.master.gain_aux == 75,
          "master unreadable: every setting on its default");
    Check(card.files[kMaster] == garbled, "... and the card untouched at boot");
    store.master.mono = true;
    store.RequestMasterSave();
    store.Process();
    Check(!store.TakeMasterFailed() && card.files[kMasterBak] == garbled
              && card.files[kMaster].find("\nmono 1\n") != std::string::npos,
          "master unreadable: the next save keeps it as frizz_master.bak and writes the new one");
    card.files[kMasterBak] = "older";
    store.RequestMasterSave();
    store.Process();
    Check(card.files[kMasterBak] == "older", "... once: the save after that leaves the .bak alone");
}

/** A file from before the phaser (#68): its scenes load with the phaser off on its defaults,
 *  everything else as it was, the chaos key's latch too, though its FxId moved; the next save
 *  writes the phaser's lines */
static void TestBeforePhaser()
{
    // scene 1: the filter and the chaos key latched, the phaser's line taken out
    FxScene scenes[kNumScenes];
    for (size_t s = 0; s < kNumScenes; s++)
    {
        scenes[s].used = s == 0;
        scenes[s].latched = s == 0 ? (1u << FX_FILTER) | (1u << FX_CHAOS) : 0;
        for (size_t fx = 0; fx < kNumFx; fx++)
            for (size_t p = 0; p < kNumFxParams; p++)
                scenes[s].params[fx][p] = kFxParams[fx].defaults[p];
    }
    scenes[0].params[FX_FILTER][0] = .3f;
    scenes[0].params[FX_CHAOS][0] = .7f;
    static char buf[kSceneFileMax];
    std::string text(buf, FormatScenes(scenes, buf, sizeof(buf))), old;
    for (size_t at = 0, next; at < text.size(); at = next)
    {
        next = text.find('\n', at) + 1;
        if (text.compare(at, 7, "phaser ") != 0)
            old += text.substr(at, next - at);
    }
    NewCard(true);
    card.dirs["/FRIZZ"] = true;
    card.files[kScenes] = old;
    SceneStore store;
    store.Init(&fs, "");
    const FxScene& s1 = store.scenes[1];
    bool defaults = true;
    for (size_t p = 0; p < kNumFxParams; p++)
        defaults &= s1.params[FX_PHASER][p] == kFxParams[FX_PHASER].defaults[p];
    Check(old.find("phaser") == std::string::npos && s1.used && !(s1.latched >> FX_PHASER & 1) && defaults,
          "before the phaser: a scene file without it loads, the phaser off on its defaults");
    Check(s1.latched == ((1u << FX_FILTER) | (1u << FX_CHAOS)) && fabsf(s1.params[FX_FILTER][0] - .3f) < 1e-6f
              && fabsf(s1.params[FX_CHAOS][0] - .7f) < 1e-6f,
          "before the phaser: the rest as it was, the chaos key latched with its knobs");
    Check(Saves(store) && card.files[kScenes].find("\nphaser 0 ") != std::string::npos,
          "before the phaser: the next save writes its lines");
}

int main()
{
    TestBoot();
    TestBeforePhaser();
    TestUnreadable();
    TestNotRead();
    TestMasterFormat();
    TestMasterUnreadable();
    return Finish();
}
