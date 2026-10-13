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
    Check(SlotOf("01.wav") == 1 && SlotOf("15.WAV") == 15 && SlotOf("07.Wav") == 7, "names: 01.wav ... 15.WAV");
    Check(!SlotOf("00.wav") && !SlotOf("16.wav") && !SlotOf("1.wav") && !SlotOf("01.wave")
              && !SlotOf("01.aif") && !SlotOf("a1.wav") && !SlotOf("01_120bpm.wav"),
          "names: anything else isn't a slot");
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
    card.dirs[p.substr(0, p.rfind('/'))] = true;
    card.dirs["/FRIZZ/LOOPS"] = true;
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
    Check(Run(l) && l.GetState() == LoopLoader::IDLE, "scan: a card without /FRIZZ/LOOPS: done, nothing found");
    Check(card.open == 0, "scan: no file left open");

    NewCard();
    PutLoop("/FRIZZ/LOOPS/A/01.wav", Stereo(Pattern(4000, 1)));
    PutLoop("/FRIZZ/LOOPS/A/03.WAV", Wav(Fmt(1, 1, 48000, 16), Pattern(2000, 2)));
    PutLoop("/FRIZZ/LOOPS/A/04.wav", Wav(Fmt(1, 2, 44100, 16), Pattern(400, 3)));
    PutLoop("/FRIZZ/LOOPS/A/notes.txt", "hello");
    PutLoop("/FRIZZ/LOOPS/C/15.wav", Stereo(Pattern(400, 4)));
    card.dirs["/FRIZZ/LOOPS/B"] = true;
    l.Init(Buf(), &mem, &fs, Ms, Ticks, 10000000);
    Check(Run(l) && l.GetState() == LoopLoader::IDLE, "scan: done");
    Check(l.GetSlot(0, 0).result == STEREO && l.GetSlot(0, 0).frames == 1000 && l.GetSlot(0, 0).data == 44,
          "scan: A/01 stereo, its frames, its data at 44");
    Check(l.GetSlot(0, 2).result == MONO, "scan: A/03.WAV mono (any case)");
    Check(l.GetSlot(0, 3).result == NOT_48K, "scan: A/04 refused, why");
    Check(l.GetSlot(0, 1).result == EMPTY && l.GetSlot(1, 0).result == EMPTY && l.GetSlot(7, 14).result == EMPTY,
          "scan: missing names, an empty bank, a missing one: empty");
    Check(l.GetSlot(2, 14).result == STEREO, "scan: C/15");
    Check(card.open == 0, "scan: no file left open");
    Check(card.raw_reads == 0, "scan: headers through FatFs, no raw reads");
    // the passes it took: one a folder (8) and one a bank's end (8), one a file (4), one to end
    LoopLoader again;
    again.Init(Buf(), &mem, &fs, Ms, Ticks, 10000000);
    int passes = 0;
    while (again.Busy() && passes < 100)
    {
        again.Process(false);
        passes++;
    }
    Check(passes == 8 + 8 + 4 + 1, "scan: a pass a folder, a pass a bank's end, a pass a file, one to end");
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
    PutLoop("/FRIZZ/LOOPS/A/01.wav", Stereo(big));
    const std::string odd = Pattern(30001 * 2, 6);
    PutLoop("/FRIZZ/LOOPS/A/02.wav", Wav(Fmt(1, 1, 48000, 16), odd, Chunk("junk", std::string(504 - 44 - 8, 'j'))));
    const std::string past = Pattern(9000 * 4, 7);
    PutLoop("/FRIZZ/LOOPS/A/03.wav", Stereo(past, Chunk("bext", std::string(1500, 'b'))));
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
    PutLoop("/FRIZZ/LOOPS/B/05.wav", Stereo(frag));
    LoopLoader& f = Fresh();
    f.Request(LoopLoader::LOADING, 1, 4, 0);
    Check(Run(f) && Loaded(f, frag) && f.Dev(part0, 1, d) && d[34] == 11,
          "load: a file in 11 fragments, a gap between each: every byte, LOOPS 0 counts them");
    // 200 fragments of a cluster each: within the map
    NewCard();
    card.frag_clusters = 1;
    const std::string many = Pattern(200 * 4096 - 44, 13);
    PutLoop("/FRIZZ/LOOPS/B/06.wav", Stereo(many));
    LoopLoader& g = Fresh();
    g.Request(LoopLoader::LOADING, 1, 5, 0);
    Check(Run(g) && Loaded(g, many) && card.raw_reads == 200, "load: a file in 200 fragments, a run each: every byte");
    // more than the map holds (255): refused, the card untouched
    NewCard();
    card.frag_clusters = 1;
    PutLoop("/FRIZZ/LOOPS/B/05.wav", Stereo(Pattern(300 * 4096, 12)));
    LoopLoader& m = Fresh();
    m.Request(LoopLoader::LOADING, 1, 4, 0);
    Check(Run(m) && m.GetState() == LoopLoader::FAILED && m.Dev(part0, 1, d) && d[3] == FRAGMENTED
              && card.raw_reads == 0,
          "load: in more fragments than the link map holds: refused as FRAGMENTED, nothing read");

    // a remount between runs (a bug report's, a save's): FatFs's files are invalid after one,
    // the loader keeps none open
    NewCard();
    PutLoop("/FRIZZ/LOOPS/A/01.wav", Stereo(big));
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

static void TestFaults()
{
    NewCard();
    const std::string big = Pattern(200000 * 4, 9);
    PutLoop("/FRIZZ/LOOPS/A/01.wav", Stereo(big));
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

    card.raw_fail = card.raw_dead = true;
    l.Request(LoopLoader::LOADING, 0, 0, 0);
    Check(Run(l, 20) && l.Dev(part0, 1, d) && d[3] == CARD_LOST, "fault: a card that doesn't come back: CARD_LOST");
    card.raw_fail = card.raw_dead = false;

    // a file cut short since the scan: its header is checked again at the load
    PutLoop("/FRIZZ/LOOPS/A/02.wav", Stereo(big));
    LoopLoader& s = Fresh();
    card.files["/FRIZZ/LOOPS/A/02.wav"].resize(100000);
    const int reads = card.raw_reads;
    s.Request(LoopLoader::LOADING, 0, 1, 0);
    Check(Run(s, 20) && s.GetState() == LoopLoader::FAILED && s.Dev(part0, 1, d) && d[3] == PAST_END
              && card.raw_reads == reads,
          "fault: a file cut short since the scan: its header refused at the load, nothing read");
    // its clusters ending before its size does (a broken FAT): the runs stop there
    PutLoop("/FRIZZ/LOOPS/A/03.wav", Stereo(Pattern(4000, 11)));
    s.Request(LoopLoader::LOADING, 0, 2, 0);
    Run(s);
    card.files["/FRIZZ/LOOPS/A/03.wav"] = Stereo(big); // the chain given out stays the short one
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
    PutLoop("/FRIZZ/LOOPS/A/01.wav", Stereo(big));
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
    Check(!card.dirs.count("/FRIZZ/LOOPS/T") && !card.dirs.count("/FRIZZ/LOOPS") && card.files.empty() && card.open == 0,
          "write test: its file, its folder and /FRIZZ/LOOPS it made are gone");
    card.dirs["/FRIZZ/LOOPS"] = true;
    const uint8_t odd[] = {6, 0, 3, 2, 44}; // 3 KB in 300-byte writes
    l.Dev(odd, 5, d);
    Check(Run(l) && l.Dev(part0, 1, d) && d[3] == 0 && card.dirs.count("/FRIZZ/LOOPS") && !card.dirs.count("/FRIZZ/LOOPS/T"),
          "write test: 300-byte writes; a /FRIZZ/LOOPS that was there stays");
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
    TestLoad();
    TestFaults();
    TestShared();
    TestWrite();
    return Finish();
}
