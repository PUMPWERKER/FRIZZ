// loops.cpp: checks LoopLoader.h, the loop loader's card side (#70), against an SD card in
// memory (host/fatfs.h): the WAV header's walk (what it takes and refuses, and that a broken
// length can't make it loop or read past what it has), the slots' names, the scan of the bank
// folders, a load (the data exactly, from any offset, in fragments, past a remount), what
// stops one (a read error, a run overdue, a card that doesn't come back, a file shorter than
// its header, more fragments than the link map holds), the card shared with SceneStore's save
// mid-load (no FatFs call while a run's DMA has the card), the CRC and the write test.
// Exits 0 when everything passes. Run by unit.sh loops.
#include <cstdio>
#include <string>
#include <vector>
#include "check.h"
#include "LoopLoader.h"
#include "SceneStore.h"

using namespace loops;

static FakeCard& card = FakeCard::Get();
static FATFS fs;
// the firmware's clocks: ms, and ticks at 10 MHz from just below their wrap, as System::GetTick
// wraps every ~18 s on the device
static uint32_t clock_us = 0;
static uint32_t tick_base = 0xFFFFFFFFu - 5000000u;
static uint32_t Ms() { return clock_us / 1000; }
static uint32_t Ticks() { return tick_base + clock_us * 10u; }

static void NewCard()
{
    card = FakeCard();
    card.dirs[""] = true;
    card.dirs["/FRIZZ"] = true;
    f_mount(&fs, "", 1);
}

static void Put32(std::string& s, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        s.push_back(static_cast<char>(v >> (8 * i)));
}
static void Put16(std::string& s, uint16_t v)
{
    s.push_back(static_cast<char>(v));
    s.push_back(static_cast<char>(v >> 8));
}
static std::string Chunk(const char* id, const std::string& body)
{
    std::string c(id, 4);
    Put32(c, static_cast<uint32_t>(body.size()));
    c += body;
    if (body.size() & 1)
        c.push_back('\0');
    return c;
}
static std::string Fmt(uint16_t tag, uint16_t channels, uint32_t rate, uint16_t bits, bool extensible = false)
{
    std::string f;
    Put16(f, extensible ? 0xFFFE : tag);
    Put16(f, channels);
    Put32(f, rate);
    Put32(f, rate * channels * bits / 8);
    Put16(f, static_cast<uint16_t>(channels * bits / 8));
    Put16(f, bits);
    if (extensible)
    {
        Put16(f, 22);
        Put16(f, bits);
        Put32(f, channels == 1 ? 4 : 3);
        Put16(f, tag); // the subformat GUID's first two bytes
        f += std::string("\x00\x00\x00\x00\x10\x00\x80\x00\x00\xAA\x00\x38\x9B\x71", 14);
    }
    return Chunk("fmt ", f);
}
/** The data's bytes: a pattern, different for each seed */
static std::string Pattern(size_t bytes, uint32_t seed)
{
    std::string d(bytes, '\0');
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < bytes; i++)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        d[i] = static_cast<char>(x >> 24);
    }
    return d;
}
/** A WAV: fmt, the chunks in before (between fmt and data), the data */
static std::string Wav(const std::string& fmt, const std::string& data, const std::string& before = "")
{
    std::string body = "WAVE" + fmt + before + Chunk("data", data);
    std::string w = "RIFF";
    Put32(w, static_cast<uint32_t>(body.size()));
    return w + body;
}
static std::string Stereo(const std::string& data, const std::string& before = "")
{
    return Wav(Fmt(1, 2, 48000, 16), data, before);
}

/** The walk over a whole file in windows of window bytes, as LoopLoader::Open reads them */
static int Walk(const std::string& file, uint32_t& data, uint32_t& frames, size_t window = kWindow,
                uint32_t size = 0xFFFFFFFF)
{
    WavWalk w;
    w.Start(size == 0xFFFFFFFF ? static_cast<uint32_t>(file.size()) : size);
    int r = WavWalk::MORE;
    for (size_t k = 0; k < kWindows && r == WavWalk::MORE; k++)
    {
        const uint32_t at = w.Want() & ~(kSector - 1);
        const uint32_t n = at < file.size() ? static_cast<uint32_t>(std::min(window, file.size() - at)) : 0;
        r = w.Step(reinterpret_cast<const uint8_t*>(file.data()) + at, at, n, data, frames);
    }
    return r == WavWalk::MORE ? NO_DATA : r;
}
static int Walk(const std::string& file)
{
    uint32_t data = 0, frames = 0;
    return Walk(file, data, frames);
}

static void TestHeader()
{
    uint32_t data = 0, frames = 0;
    Check(Walk(Stereo(std::string(4800 * 4, 'x')), data, frames) == STEREO && data == 44 && frames == 4800,
          "header: 16-bit stereo at 48 kHz, its data at 44, its frames");
    Check(Walk(Wav(Fmt(1, 1, 48000, 16), std::string(1000, 'x')), data, frames) == MONO && frames == 500,
          "header: mono");
    Check(Walk(Wav(Fmt(1, 2, 48000, 16, true), std::string(400, 'x')), data, frames) == STEREO && data == 68,
          "header: WAVE_FORMAT_EXTENSIBLE with PCM as its subformat is taken");
    Check(Walk(Wav(Fmt(3, 2, 48000, 16, true), std::string(400, 'x'))) == NOT_PCM,
          "header: an extensible float is refused");
    Check(Walk(Stereo(std::string(400, 'x'), Chunk("LIST", std::string(22, 'i'))), data, frames) == STEREO && data == 74,
          "header: a LIST chunk before the data (data at 74)");
    Check(Walk(Stereo(std::string(400, 'x'), Chunk("junk", std::string(9, 'j'))), data, frames) == STEREO && data == 62,
          "header: an odd chunk's pad byte skipped");
    // data at 504: its first sector holds 8 bytes of it; and the most frames there are
    const std::string at504 = Stereo(std::string(kMaxFrames * 4, 'x'), Chunk("junk", std::string(504 - 44 - 8, 'j')));
    Check(Walk(at504, data, frames) == STEREO && data == 504 && frames == kMaxFrames,
          "header: data at 504, 16 bars at 50 BPM exactly");
    Check(Walk(Stereo(std::string(kMaxFrames * 4 + 4, 'x'))) == TOO_LONG, "header: a frame longer is refused");
    Check(Walk(Stereo("")) == NO_DATA && Walk(Stereo("xx")) == NO_DATA,
          "header: a data chunk without a whole frame is refused (a loop of 0 frames would hang the looper)");
    // a broadcast WAV's bext chunk (602 bytes and more): the data past the first window
    const std::string bext = Stereo(std::string(800, 'x'), Chunk("bext", std::string(1500, 'b')));
    Check(Walk(bext, data, frames) == STEREO && data == 44 + 8 + 1500 && frames == 200,
          "header: a bext chunk of 1500 bytes before the data: found in the next window");
    Check(Walk(Stereo(std::string(800, 'x'), Chunk("bext", std::string(9000, 'b')))) == STEREO,
          "header: a chunk of 9000 bytes: skipped by its length, the data in the next window");
    {
        std::string many;
        for (int i = 0; i < 9; i++)
            many += Chunk("junk", std::string(1000, 'j'));
        Check(Walk(Stereo(std::string(800, 'x'), many)) == NO_DATA,
              "header: 9 chunks of 1000 bytes before the data: refused after 8 windows, not looked for forever");
    }
    Check(Walk(Wav(Fmt(1, 2, 44100, 16), "xxxx")) == NOT_48K, "header: 44.1 kHz refused");
    Check(Walk(Wav(Fmt(1, 2, 48000, 24), "xxxxxx")) == NOT_16BIT, "header: 24-bit refused");
    Check(Walk(Wav(Fmt(1, 4, 48000, 16), "xxxxxxxx")) == CHANNELS, "header: 4 channels refused");
    Check(Walk(Wav(Fmt(2, 2, 48000, 16), "xxxx")) == NOT_PCM, "header: ADPCM refused");
    Check(Walk("RIFX" + Stereo("xxxx").substr(4)) == NOT_WAV, "header: not RIFF refused");
    Check(Walk("RIFF") == NOT_WAV, "header: 4 bytes refused");
    {
        std::string w = "RIFF....WAVE" + Chunk("data", "xxxx") + Fmt(1, 2, 48000, 16);
        Check(Walk(w) == BAD_FMT, "header: data before fmt refused");
    }
    {
        std::string w = "RIFF....WAVE" + Chunk("fmt ", "short");
        Check(Walk(w) == BAD_FMT, "header: a fmt chunk shorter than 16 refused");
    }
    // lengths that would wrap a 32-bit position or run past the file
    {
        std::string w = Stereo("xxxx");
        std::string huge = "RIFF....WAVE" + Fmt(1, 2, 48000, 16) + "junk";
        Put32(huge, 0xFFFFFFF8u);
        huge += std::string(100, 'z');
        Check(Walk(huge) == PAST_END, "header: a chunk of 0xFFFFFFF8 bytes is refused, no wrap");
        std::string d = w;
        d[40] = '\xFF', d[41] = '\xFF', d[42] = '\xFF', d[43] = '\xFF';
        Check(Walk(d) == PAST_END, "header: data longer than the file refused (0xFFFFFFFF)");
        d[40] = 8, d[41] = 0, d[42] = 0, d[43] = 0;
        Check(Walk(d) == PAST_END, "header: data 4 bytes past the end refused");
        Check(Walk(w, data, frames, kWindow, static_cast<uint32_t>(w.size() - 1)) == PAST_END,
              "header: a file cut short in its data refused");
        std::string odd = "RIFF....WAVE" + Fmt(1, 2, 48000, 16) + "junk";
        Put32(odd, 1);
        odd += "z";
        Check(Walk(odd) == NO_DATA, "header: an odd chunk ending the file, no pad byte: no data, no read past");
    }
    // every window cut, from 0 to the whole: never more than it has
    {
        const std::string w = Stereo(std::string(64, 'x'), Chunk("LIST", std::string(30, 'i')));
        bool ok = true;
        for (size_t window = 0; window <= w.size(); window++)
        {
            const int r = Walk(w, data, frames, window);
            ok = ok && (r == STEREO || r == NO_DATA);
        }
        Check(ok, "header: windows of every size from 0: the data or none, nothing past them");
    }
}

static void TestNames()
{
    size_t b = 99, c = 99, d = 99;
    Check(SlotOf("a1.wav", false, b) == 1 && b == 0 && SlotOf("H15.WAV", false, c) == 15 && c == 7
              && SlotOf("c7.Wav", false, d) == 7 && d == 2,
          "names: a1.wav ... H15.WAV, the bank by its letter");
    size_t x = 99;
    bool none = true;
    for (const char* n : {"a0.wav", "a16.wav", "a01.wav", "a1.wave", "a1.aif", "i1.wav", "01.wav", "a.wav",
                          "a1", "a1_120bpm.wav", "a123.wav", "cubbi_a1.wav", "a1.wav.txt"})
        none = none && !SlotOf(n, false, x);
    Check(none && x == 99, "names: anything else isn't one of FRIZZ's (a leading zero neither), the bank untouched");
    Check(SlotOf("cubbi_a1.wav", true, b) == 1 && b == kCubbiBank && SlotOf("CUBBI_E15.WAV", true, c) == 15
              && c == kCubbiBank + 4 && SlotOf("Cubbi_c10.wav", true, d) == 10 && d == kCubbiBank + 2,
          "names: TAPE's cubbi_a1.wav ... cubbi_e15.wav, any case, banks from kCubbiBank");
    for (const char* n : {"cubbi_a1_double.wav", "jammi_a1.wav", "cubbi_f1.wav", "cubbi_a16.wav", "cubbi_a01.wav",
                          "a1.wav", "cubbi_a.wav", "cubbia1.wav", "temp_rec.wav"})
        none = none && !SlotOf(n, true, x);
    Check(none && x == 99, "names: TAPE's doubles, jammis, banks past e, other names aren't cubbis");
}

static Mem mem;
static std::vector<uint8_t> buffer(kBufBytes + 64);
static uint8_t* Buf() { return buffer.data() + (32 - reinterpret_cast<uintptr_t>(buffer.data()) % 32) % 32; }

/** Process until it rests, card_wanted as given, the clock 1 ms a pass; false if it doesn't */
static bool Run(LoopLoader& l, int passes = 100000, bool card_wanted = false)
{
    for (int i = 0; i < passes; i++)
    {
        l.Process(card_wanted);
        clock_us += 1000;
        if (!l.Busy())
            return true;
    }
    return false;
}

static void PutLoop(const char* path, const std::string& wav)
{
    std::string p = path;
    if (p.rfind('/'))
        card.dirs[p.substr(0, p.rfind('/'))] = true;
    card.files[p] = wav;
}

static bool Loaded(const LoopLoader& l, const std::string& data)
{
    return l.Ready() && l.Frames() * l.Channels() * 2 == data.size()
           && !memcmp(l.Data(), data.data(), data.size());
}

static uint32_t Crc(const std::string& s)
{
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : s)
    {
        c ^= b;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static void TestScan()
{
    NewCard();
    LoopLoader l;
    l.Init(Buf(), &mem, &fs, Ms, Ticks, 10000000);
    Check(Run(l) && l.GetState() == LoopLoader::IDLE, "scan: a card without /FRIZZ or cubbis: done, nothing found");
    Check(card.open == 0, "scan: no file left open");

    NewCard();
    PutLoop("/FRIZZ/a1.wav", Stereo(Pattern(4000, 1)));
    PutLoop("/FRIZZ/a3.WAV", Wav(Fmt(1, 1, 48000, 16), Pattern(2000, 2)));
    PutLoop("/FRIZZ/a4.wav", Wav(Fmt(1, 2, 44100, 16), Pattern(400, 3)));
    PutLoop("/FRIZZ/notes.txt", "hello");
    PutLoop("/FRIZZ/frizz_scenes.txt", "FRIZZ scenes 1\n");
    PutLoop("/FRIZZ/a01.wav", Stereo(Pattern(400, 5)));
    PutLoop("/FRIZZ/B2.wav/x.wav", "a folder by a loop's name");
    PutLoop("/FRIZZ/c15.wav", Stereo(Pattern(400, 4)));
    card.dirs["/FRIZZ/LOOPS/B"] = true; // the layout before 1.1: not read
    PutLoop("/FRIZZ/LOOPS/B/01.wav", Stereo(Pattern(400, 6)));
    l.Init(Buf(), &mem, &fs, Ms, Ticks, 10000000);
    Check(Run(l) && l.GetState() == LoopLoader::IDLE, "scan: done");
    Check(l.GetSlot(0, 0).result == STEREO && l.GetSlot(0, 0).frames == 1000 && l.GetSlot(0, 0).data == 44,
          "scan: a1.wav stereo, its frames, its data at 44");
    Check(l.GetSlot(0, 2).result == MONO, "scan: a3.WAV mono (any case)");
    Check(l.GetSlot(0, 3).result == NOT_48K, "scan: a4.wav refused, why");
    Check(l.GetSlot(0, 1).result == EMPTY && l.GetSlot(1, 0).result == EMPTY && l.GetSlot(7, 14).result == EMPTY,
          "scan: missing names, a01.wav, a folder named B2.wav, the old LOOPS/B/01: empty");
    Check(l.GetSlot(2, 14).result == STEREO, "scan: c15.wav");
    Check(card.open == 0, "scan: no file left open");
    Check(card.raw_reads == 0, "scan: headers through FatFs, no raw reads");
    // the passes it took: /FRIZZ's 9 entries in one, the root's 1 (FRIZZ), each folder's end,
    // one a file (4), one to end
    LoopLoader again;
    again.Init(Buf(), &mem, &fs, Ms, Ticks, 10000000);
    int passes = 0;
    while (again.Busy() && passes < 100)
    {
        again.Process(false);
        passes++;
    }
    Check(passes == 1 + 1 + 4 + 1 + 1 + 1, "scan: a pass a folder's kDirEntries, a pass its end, a pass a file, one to end");
}

/** TAPE's cubbis: in /TAPE if it's there (an empty one too, as the launcher's TAPE), else in the
 *  root; their names, a folder listed over passes and across a remount */
static void TestCubbis()
{
    NewCard();
    PutLoop("/cubbi_a1.wav", Stereo(Pattern(4000, 1)));
    PutLoop("/cubbi_a1_double.wav", Stereo(Pattern(2000, 1)));
    PutLoop("/jammi_a2.wav", Stereo(Pattern(400, 2)));
    PutLoop("/CUBBI_C14.WAV", Stereo(Pattern(800, 3)));
    PutLoop("/cubbi_e15.wav", Wav(Fmt(1, 2, 44100, 16), Pattern(400, 3)));
    PutLoop("/a1.wav", Stereo(Pattern(400, 4)));
    PutLoop("/FRIZZ/b3.wav", Stereo(Pattern(400, 5)));
    LoopLoader l;
    l.Init(Buf(), &mem, &fs, Ms, Ticks, 10000000);
    Check(Run(l) && l.GetState() == LoopLoader::IDLE, "cubbis: scanned");
    Check(l.GetSlot(kCubbiBank, 0).result == STEREO && l.GetSlot(kCubbiBank, 0).frames == 1000
              && l.GetSlot(kCubbiBank + 2, 13).result == STEREO && l.GetSlot(kCubbiBank + 4, 14).result == NOT_48K,
          "cubbis: no /TAPE, the root's: cubbi_a1, CUBBI_C14 (any case), cubbi_e15 refused");
    Check(l.GetSlot(kCubbiBank, 1).result == EMPTY && l.GetSlot(0, 0).result == EMPTY && l.GetSlot(1, 2).result == STEREO,
          "cubbis: a jammi isn't one, a1.wav in the root isn't FRIZZ's; /FRIZZ/b3.wav is");
    Check(l.Request(LoopLoader::LOADING, kCubbiBank + 2, 13) && Run(l) && Loaded(l, Pattern(800, 3)),
          "cubbis: CUBBI_C14.WAV loads from the root");

    // /TAPE there: its cubbis, the root's not
    PutLoop("/TAPE/cubbi_b2.wav", Stereo(Pattern(1200, 7)));
    PutLoop("/TAPE/presets.json", "{}");
    Check(l.Request(LoopLoader::SCANNING) && Run(l) && l.GetSlot(kCubbiBank + 1, 1).result == STEREO
              && l.GetSlot(kCubbiBank, 0).result == EMPTY && l.GetSlot(kCubbiBank + 2, 13).result == EMPTY,
          "cubbis: /TAPE there: its cubbi_b2, not the root's");
    Check(l.Request(LoopLoader::LOADING, kCubbiBank + 1, 1) && Run(l) && Loaded(l, Pattern(1200, 7)),
          "cubbis: cubbi_b2.wav loads from /TAPE");
    // an empty /TAPE: none, as the launcher's TAPE finds none
    card.files.erase("/TAPE/cubbi_b2.wav");
    card.files.erase("/TAPE/presets.json");
    card.dirs["/TAPE"] = true;
    bool none = true;
    Check(l.Request(LoopLoader::SCANNING) && Run(l), "cubbis: an empty /TAPE scanned");
    for (size_t b = kCubbiBank; b < kBanks; b++)
        for (size_t s = 0; s < kSlots; s++)
            none = none && l.GetSlot(b, s).result == EMPTY;
    Check(none, "cubbis: an empty /TAPE: none, the root's not either");

    // TAPE's card as it ships (171 files in the root): kDirEntries a pass, a remount between
    NewCard();
    for (char b : {'a', 'b', 'c'})
        for (int s = 1; s <= 14; s++)
        {
            const std::string n = std::string(1, b) + std::to_string(s);
            PutLoop(("/cubbi_" + n + ".wav").c_str(), Stereo(Pattern(100, s)));
            PutLoop(("/cubbi_" + n + "_double.wav").c_str(), Stereo(Pattern(50, s)));
            PutLoop(("/jammi_" + n + ".wav").c_str(), Stereo(Pattern(100, s)));
            PutLoop(("/jammi_" + n + "_double.wav").c_str(), Stereo(Pattern(50, s)));
        }
    PutLoop("/CHOMPI_TAPEv2_0.bin", "x");
    PutLoop("/options.json", "{}");
    PutLoop("/presets.json", "{}");
    l.Init(Buf(), &mem, &fs, Ms, Ticks, 10000000);
    int passes = 0, most = 0;
    while (l.Busy() && passes < 1000)
    {
        const uint32_t reads = card.dir_reads;
        l.Process(false);
        most = std::max(most, static_cast<int>(card.dir_reads - reads));
        if (++passes == 3)
            f_mount(&fs, "", 1); // a save's or a bug report's, mid-listing: the folder's invalid
    }
    int found = 0;
    for (size_t b = kCubbiBank; b < kBanks; b++)
        for (size_t s = 0; s < kSlots; s++)
            found += l.GetSlot(b, s).result == STEREO;
    Check(!l.Busy() && found == 42 && l.GetSlot(kCubbiBank + 2, 13).result == STEREO && l.GetSlot(kCubbiBank + 3, 0).result == EMPTY,
          "cubbis: TAPE's own card, a remount mid-listing: its 42 cubbis, a-c 1-14");
    Check(most <= static_cast<int>(kDirEntries) + 1 + 2 * static_cast<int>(kDirEntries) && card.open == 0,
          "cubbis: a pass reads at most kDirEntries (after a remount, those read before again)");
    Check(passes == 1 + (171 + kDirEntries) / kDirEntries + 1 + 1 + 42 + 1 + 1,
          "cubbis: 171 files: a pass /FRIZZ, 8 the root, a remount's, a header a pass, to end");
}

static LoopLoader& Fresh()
{
    static LoopLoader l;
    l = LoopLoader();
    l.Init(Buf(), &mem, &fs, Ms, Ticks, 10000000);
    Run(l);
    return l;
}

static void TestLoad()
{
    NewCard();
    const std::string big = Pattern(kMaxFrames * 4, 5);
    PutLoop("/FRIZZ/a1.wav", Stereo(big));
    const std::string odd = Pattern(30001 * 2, 6);
    PutLoop("/FRIZZ/a2.wav", Wav(Fmt(1, 1, 48000, 16), odd, Chunk("junk", std::string(504 - 44 - 8, 'j'))));
    const std::string past = Pattern(9000 * 4, 7);
    PutLoop("/FRIZZ/a3.wav", Stereo(past, Chunk("bext", std::string(1500, 'b'))));
    LoopLoader& l = Fresh();

    l.Request(LoopLoader::LOADING, 0, 0, 0);
    Check(Run(l) && Loaded(l, big), "load: 16 bars at 50 BPM, the most it takes: every byte as in the file");
    Check(card.raw_reads == static_cast<int>((44 + big.size() + 511) / 512 + 255) / 256,
          "load: in runs of 128 KB from the data's first sector");
    Check(card.open == 0 && card.raw_clash == 0, "load: no file left open, no FatFs call during a run");
    uint8_t d[40];
    const uint8_t part0[] = {0};
    Check(l.Dev(part0, 1, d) == 35 && d[33] == 0 && d[34] == 1 && d[0] == LoopLoader::LOADED && d[3] == 0, "load: LOOPS 0 says it's loaded");

    l.Request(LoopLoader::LOADING, 0, 1, 0);
    Check(Run(l) && Loaded(l, odd) && l.Channels() == 1, "load: mono, its data at 504 (8 bytes in the first sector)");
    l.Request(LoopLoader::LOADING, 0, 2, 0);
    Check(Run(l) && Loaded(l, past), "load: its data after a 1500-byte bext chunk");

    // the CRC the computer checks against the file
    const uint8_t crc[] = {5, 1};
    l.Dev(crc, 2, d);
    Run(l);
    l.Dev(crc, 1, d);
    const uint32_t got = static_cast<uint32_t>(d[1]) << 28 | d[2] << 21 | d[3] << 14 | d[4] << 7 | d[5];
    Check(l.Ready() && got == Crc(past), "load: LOOPS 5's CRC-32 is zlib's of the file's data");

    // in fragments: the link map's runs, each read on its own
    NewCard();
    card.frag_clusters = 7; // 28 KB fragments
    const std::string frag = Pattern(300000 * 4 / 4, 8);
    PutLoop("/FRIZZ/b5.wav", Stereo(frag));
    LoopLoader& f = Fresh();
    f.Request(LoopLoader::LOADING, 1, 4, 0);
    Check(Run(f) && Loaded(f, frag) && f.Dev(part0, 1, d) && d[34] == 11,
          "load: a file in 11 fragments, a gap between each: every byte, LOOPS 0 counts them");
    // 200 fragments of a cluster each: within the map
    NewCard();
    card.frag_clusters = 1;
    const std::string many = Pattern(200 * 4096 - 44, 13);
    PutLoop("/FRIZZ/b6.wav", Stereo(many));
    LoopLoader& g = Fresh();
    g.Request(LoopLoader::LOADING, 1, 5, 0);
    Check(Run(g) && Loaded(g, many) && card.raw_reads == 200, "load: a file in 200 fragments, a run each: every byte");
    // more than the map holds (255): refused, the card untouched
    NewCard();
    card.frag_clusters = 1;
    PutLoop("/FRIZZ/b5.wav", Stereo(Pattern(300 * 4096, 12)));
    LoopLoader& m = Fresh();
    m.Request(LoopLoader::LOADING, 1, 4, 0);
    Check(Run(m) && m.GetState() == LoopLoader::FAILED && m.Dev(part0, 1, d) && d[3] == FRAGMENTED
              && card.raw_reads == 0,
          "load: in more fragments than the link map holds: refused as FRAGMENTED, nothing read");

    // a remount between runs (a bug report's, a save's): FatFs's files are invalid after one,
    // the loader keeps none open between its runs; its map's file is opened again where it was
    NewCard();
    PutLoop("/FRIZZ/a1.wav", Stereo(big));
    LoopLoader& r = Fresh();
    r.Request(LoopLoader::LOADING, 0, 0, 0);
    for (int i = 0; i < 20; i++)
    {
        // as MainLoop has it: the loader told first, then the remount
        r.Process(i % 5 == 4);
        if (i % 5 == 4)
            f_mount(&fs, "", 1);
    }
    Check(Run(r) && Loaded(r, big), "load: remounts between its runs don't touch it");
    Check(card.raw_clash == 0, "load: and none of them came in during a run");
}

/** Process until it rests, as Run; the most FAT sectors a pass read, and how many passes */
static int FatPerPass(LoopLoader& l, int& passes, int remount_at = -1)
{
    int worst = 0;
    for (passes = 0; passes < 100000 && l.Busy(); passes++)
    {
        const int before = card.fat_reads;
        l.Process(false);
        clock_us += 1000;
        worst = std::max(worst, card.fat_reads - before);
        if (passes == remount_at)
            f_mount(&fs, "", 1);
    }
    return worst;
}

/** The link map over passes (#212): FatFs reads the FAT a sector at a time, which in one pass
 *  held MainLoop 12-35 ms on the device */
static void TestMap()
{
    NewCard();
    const std::string big = Pattern(kMaxFrames * 4, 5);
    PutLoop("/FRIZZ/a1.wav", Stereo(big));
    LoopLoader& l = Fresh();
    const int clusters = static_cast<int>((44 + big.size() + 4095) / 4096);
    int passes = 0, reads = card.fat_reads;
    l.Request(LoopLoader::LOADING, 0, 0, 0);
    int worst = FatPerPass(l, passes);
    const int contiguous = card.fat_reads - reads, contiguous_passes = passes;
    Check(Loaded(l, big) && worst <= 3 && contiguous == (clusters + 127) / 128,
          "map: 16 bars, every byte; no pass reads more than 3 FAT sectors, each read once (#212)");
    if (worst > 3)
        printf("      %d FAT sectors in a pass, %d in all\n", worst, contiguous);

    // the clusters spread wider: 16 a fragment, one free between
    NewCard();
    card.frag_clusters = 16;
    PutLoop("/FRIZZ/a1.wav", Stereo(big));
    LoopLoader& f = Fresh();
    f.Request(LoopLoader::LOADING, 0, 0, 0);
    worst = FatPerPass(f, passes);
    Check(Loaded(f, big) && worst <= 4, "map: in 16-cluster fragments, every byte; no pass reads more than 4 FAT sectors");
    uint8_t d[40];
    const uint8_t part0[] = {0};
    f.Dev(part0, 1, d);
    Check((d[33] << 7 | d[34]) == (clusters + 15) / 16, "map: LOOPS 0 counts its fragments as FatFs's link map does");

    // a remount in the middle of the map: the file opened again where it was, no FAT sector
    // read twice but the one in the window then
    NewCard();
    PutLoop("/FRIZZ/a1.wav", Stereo(big));
    LoopLoader& r = Fresh();
    reads = card.fat_reads;
    r.Request(LoopLoader::LOADING, 0, 0, 0);
    int again = 0;
    worst = FatPerPass(r, again, 4);
    Check(Loaded(r, big) && worst <= 3 && card.fat_reads - reads <= contiguous + 1 && again == contiguous_passes,
          "map: a remount in its middle: opened again where it was, every byte, nothing walked again");
    Check(card.open == 0, "map: no file left open");
}

static void TestFaults()
{
    NewCard();
    const std::string big = Pattern(200000 * 4, 9);
    PutLoop("/FRIZZ/a1.wav", Stereo(big));
    LoopLoader& l = Fresh();
    uint8_t d[40];
    const uint8_t part0[] = {0};

    card.raw_fail = true;
    l.Request(LoopLoader::LOADING, 0, 0, 0);
    Check(Run(l, 20) && l.GetState() == LoopLoader::FAILED && l.Dev(part0, 1, d) && d[3] == CARD_ERROR
              && d[8] == 0x20 && d[31] == 1 && d[32] == 0,
          "fault: a read error ends the load at once, CARD_ERROR, the SD error, the card recovered");
    card.raw_fail = false;
    l.Request(LoopLoader::LOADING, 0, 0, 0);
    Check(Run(l) && Loaded(l, big), "fault: and the next load reads it all");

    l.Request(LoopLoader::LOADING, 0, 0, LoopLoader::kFaultPastEnd);
    Check(Run(l, 20) && l.GetState() == LoopLoader::FAILED && l.Dev(part0, 1, d) && d[3] == CARD_ERROR
              && d[21] == 0 && d[22] == 2,
          "fault: a read past the card's end (the third run) refused at once, after 2 runs");

    // the tick clock wrapping in the middle of a load (every ~18 s on the device): no TIMEOUT
    // out of it, the ms it took as they were
    tick_base = 0u - 5000u * 10u - clock_us * 10u; // wraps 5 ms from now
    const uint32_t from = clock_us;
    l.Request(LoopLoader::LOADING, 0, 0, 0);
    Check(Run(l) && Loaded(l, big) && l.Dev(part0, 1, d) && d[3] == 0 && d[17] == 0 && d[18] == 0
              && d[19] + 1u >= (clock_us - from) / 1000 && d[19] > 5,
          "fault: the tick clock wrapping mid-load: loaded, no TIMEOUT, its ms right");
    tick_base = 0u - 100000u * 10u - clock_us * 10u; // and 100 ms into a run that never ends

    // a run that doesn't end: waited for kRunTimeoutUs, not 30 s
    card.raw_polls = 1000000;
    l.Request(LoopLoader::LOADING, 0, 0, 0);
    const uint32_t t0 = clock_us;
    Check(Run(l, 400) && l.GetState() == LoopLoader::FAILED && l.Dev(part0, 1, d) && d[3] == TIMEOUT,
          "fault: a run that never ends: TIMEOUT");
    Check(clock_us - t0 <= kRunTimeoutUs + 5000, "fault: after 250 ms");
    card.raw_polls = 2;
    l.Request(LoopLoader::LOADING, 0, 0, LoopLoader::kFaultCutShort);
    Check(Run(l, 20) && l.GetState() == LoopLoader::FAILED && l.Dev(part0, 1, d) && d[3] == TIMEOUT,
          "fault: a run cut short (no time given): stopped, TIMEOUT");

    l.Request(LoopLoader::LOADING, 0, 0, LoopLoader::kFaultCutShort | LoopLoader::kFaultReinit);
    Check(Run(l, 20) && l.GetState() == LoopLoader::FAILED && l.Dev(part0, 1, d) && d[3] == TIMEOUT && d[32] == 1,
          "fault: cut short, the card set up from scratch: counted");
    l.Request(LoopLoader::LOADING, 0, 0, 0);
    Check(Run(l) && Loaded(l, big), "fault: and the next load reads it all");

    card.raw_fail = card.raw_dead = true;
    l.Request(LoopLoader::LOADING, 0, 0, 0);
    Check(Run(l, 20) && l.Dev(part0, 1, d) && d[3] == CARD_LOST, "fault: a card that doesn't come back: CARD_LOST");
    card.raw_fail = card.raw_dead = false;

    // a file cut short since the scan: its header is checked again at the load
    PutLoop("/FRIZZ/a2.wav", Stereo(big));
    LoopLoader& s = Fresh();
    card.files["/FRIZZ/a2.wav"].resize(100000);
    const int reads = card.raw_reads;
    s.Request(LoopLoader::LOADING, 0, 1, 0);
    Check(Run(s, 20) && s.GetState() == LoopLoader::FAILED && s.Dev(part0, 1, d) && d[3] == PAST_END
              && card.raw_reads == reads,
          "fault: a file cut short since the scan: its header refused at the load, nothing read");
    // its clusters ending before its size does (a broken FAT): the runs stop there
    PutLoop("/FRIZZ/a3.wav", Stereo(Pattern(4000, 11)));
    s.Request(LoopLoader::LOADING, 0, 2, 0);
    Run(s);
    card.files["/FRIZZ/a3.wav"] = Stereo(big); // the chain given out stays the short one
    s.Request(LoopLoader::LOADING, 0, 2, 0);
    Check(Run(s, 20) && s.GetState() == LoopLoader::FAILED && s.Dev(part0, 1, d) && d[3] == SHORT,
          "fault: a chain shorter than the file: SHORT, not a read past it");
}

/** MainLoop's order (chompi_main.cpp): the loader first, told whether a save waits, then
 *  SceneStore. A save asked for mid-load gets the card only between runs */
static void TestShared()
{
    NewCard();
    card.raw_polls = 4;
    const std::string big = Pattern(kMaxFrames * 4, 10);
    PutLoop("/FRIZZ/a1.wav", Stereo(big));
    chompi::SceneStore store;
    store.Init(&fs, "");
    LoopLoader& l = Fresh();
    l.Request(LoopLoader::LOADING, 0, 0, 0);
    int saves = 0;
    for (int i = 0; i < 2000 && l.Busy(); i++)
    {
        if (i % 7 == 3)
        {
            store.scenes[1].used = true;
            store.RequestSave();
            saves++;
        }
        l.Process(store.Busy());
        clock_us += 1000;
        store.Process(false);
    }
    Check(Loaded(l, big), "shared: a load with saves asked for every 7th pass: every byte");
    Check(saves > 10 && store.GetSaveState() == chompi::SceneStore::SaveState::OK && card.files.count("/FRIZZ/frizz_scenes.txt"),
          "shared: and the saves went through");
    Check(card.raw_clash == 0, "shared: no FatFs call while a run had the card");
    card.raw_polls = 2;
}

static void TestWrite()
{
    NewCard();
    LoopLoader& l = Fresh();
    uint8_t d[40];
    const uint8_t part0[] = {0};
    const uint8_t write[] = {6, 0, 64, 2, 0}; // 64 KB in 256-byte writes
    l.Dev(write, 5, d);
    Check(Run(l) && l.GetState() == LoopLoader::WRITTEN && l.Dev(part0, 1, d) && d[3] == 0,
          "write test: 64 KB in 256-byte writes, renamed, read back: passed");
    Check(!card.dirs.count("/FRIZZ/T") && !card.HasDir("/FRIZZ/LOOPS") && card.files.empty() && card.open == 0,
          "write test: its file and its folder /FRIZZ/T are gone, no /FRIZZ/LOOPS made");
    // a fault for the device test (#213) in KB's high group: FatFs's next write of more than a
    // sector armed (SdDisk.h; the host card's FatFs doesn't go through it), the KB as they were
    const uint8_t faulty[] = {6, 0x10 | 1024 >> 7, 0, 32, 0}; // 1024 KB in 4096-byte writes
    l.Dev(faulty, 5, d);
    const uint8_t armed = sddisk::Fault();
    Check(Run(l) && l.GetState() == LoopLoader::WRITTEN && l.Dev(part0, 1, d) && d[3] == 0
              && (d[13] << 21 | d[14] << 14 | d[15] << 7 | d[16]) == 1024 * 1024
              && armed == (sddisk::kCutShort | sddisk::kWrites) && sddisk::Fault() == 0,
          "write test: LOOPS 6's fault in KB's high group, armed for the writes, disarmed after");
    const uint8_t odd[] = {6, 0, 3, 2, 44}; // 3 KB in 300-byte writes
    l.Dev(odd, 5, d);
    Check(Run(l) && l.Dev(part0, 1, d) && d[3] == 0 && !card.dirs.count("/FRIZZ/T"),
          "write test: 300-byte writes");
    const uint8_t mb[] = {6, 8, 0, 32, 0}; // 1024 KB: KB's high group 8, past the banks
    Check(l.Dev(mb, 5, d) == 0 && Run(l) && l.Dev(part0, 1, d) && d[3] == 0,
          "write test: 1024 KB in 4096-byte writes, the most it takes");
    // a byte the card gives back otherwise: caught
    l.Dev(write, 5, d);
    for (int i = 0; i < 10000 && !card.files.count("/FRIZZ/T/w.bin"); i++)
        l.Process(false);
    card.files["/FRIZZ/T/w.bin"][12345] ^= 1;
    Check(Run(l) && l.Dev(part0, 1, d) && d[3] == WRITE_BAD && !card.dirs.count("/FRIZZ/T"),
          "write test: a byte read back otherwise: WRITE_BAD, and still cleaned up");
    card.read_only = true;
    l.Dev(write, 5, d);
    Check(Run(l) && l.Dev(part0, 1, d) && d[3] == WRITE_FAILED && card.open == 0,
          "write test: a card that refuses: WRITE_FAILED, nothing left open");
}

int main()
{
    TestHeader();
    TestNames();
    TestScan();
    TestCubbis();
    TestLoad();
    TestMap();
    TestFaults();
    TestShared();
    TestWrite();
    return Finish();
}
