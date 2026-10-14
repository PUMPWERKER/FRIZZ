// loopxfer.cpp: checks LoopTransfer.h, the loop transfer's firmware side (#278,
// docs/LOOP_PROTOCOL.md), with the loader's scan and header walk (LoopLoader.h), against an SD
// card in memory (host/fatfs.h):
//  - test/loopxfer/vectors.json replayed byte for byte: what tools/loopxfer.py's model FRIZZ
//    sends, the firmware sends, pass for pass, and the card ends as it does; and that file is
//    what loopxfer.py vectors writes (8.1);
//  - what the vectors don't reach (8.2): COPY's TOO_LONG, the fragment check (a file in 300
//    fragments), a power cut after every name change of a write, a copy and a swap with the
//    boot after it (whole files only), no FatFs call while a load's DMA has the card, the free
//    space counted a FAT sector a pass, and after every case that only the allowed names
//    changed: /FRIZZ's slots and temp files, never /TAPE, the root or FRIZZ's other files.
// Exits 0 when everything passes. Run by unit.sh loopxfer.
// unit includes: loopxfer
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "check.h"
#include "LoopTransfer.h"

using namespace loopxfer;

static FakeCard& card = FakeCard::Get();
static std::string Dir()
{
    const std::string f = __FILE__;
    return f.substr(0, f.rfind('/') + 1);
}

// ---- a JSON reader, enough for vectors.json ----
struct J
{
    enum T
    {
        NUL,
        BOOL,
        NUM,
        STR,
        ARR,
        OBJ
    } t = NUL;
    double num = 0;
    bool b = false;
    std::string str;
    std::vector<J> arr;
    std::vector<std::pair<std::string, J>> obj;
    const J* Get(const std::string& k) const
    {
        for (const auto& kv : obj)
            if (kv.first == k)
                return &kv.second;
        return nullptr;
    }
};
struct Reader
{
    const std::string& s;
    size_t i = 0;
    void Ws()
    {
        while (i < s.size() && isspace(static_cast<unsigned char>(s[i])))
            i++;
    }
    J Value()
    {
        Ws();
        J v;
        const char c = s[i];
        if (c == '{')
        {
            v.t = J::OBJ;
            i++;
            Ws();
            if (s[i] == '}')
                return i++, v;
            while (true)
            {
                Ws();
                const std::string k = Value().str;
                Ws();
                i++; // :
                v.obj.push_back({k, Value()});
                Ws();
                if (s[i++] == '}')
                    return v;
            }
        }
        if (c == '[')
        {
            v.t = J::ARR;
            i++;
            Ws();
            if (s[i] == ']')
                return i++, v;
            while (true)
            {
                v.arr.push_back(Value());
                Ws();
                if (s[i++] == ']')
                    return v;
            }
        }
        if (c == '"')
        {
            v.t = J::STR;
            for (i++; s[i] != '"'; i++)
            {
                if (s[i] == '\\')
                {
                    i++;
                    if (s[i] == 'u')
                    {
                        v.str += static_cast<char>(strtol(s.substr(i + 1, 4).c_str(), nullptr, 16));
                        i += 4;
                        continue;
                    }
                    v.str += s[i] == 'n' ? '\n' : s[i];
                    continue;
                }
                v.str += s[i];
            }
            i++;
            return v;
        }
        if (!s.compare(i, 4, "null"))
            return i += 4, v;
        if (!s.compare(i, 4, "true") || !s.compare(i, 5, "false"))
        {
            v.t = J::BOOL;
            v.b = s[i] == 't';
            i += v.b ? 4 : 5;
            return v;
        }
        v.t = J::NUM;
        size_t used = 0;
        v.num = std::stod(s.substr(i, 32), &used);
        i += used;
        return v;
    }
};

static std::vector<uint8_t> Bytes(const std::string& hex)
{
    std::vector<uint8_t> b;
    std::istringstream in(hex);
    std::string x;
    while (in >> x)
        b.push_back(static_cast<uint8_t>(strtol(x.c_str(), nullptr, 16)));
    return b;
}
static std::string Hex(const std::vector<uint8_t>& b)
{
    std::string h;
    char x[4];
    for (uint8_t v : b)
    {
        snprintf(x, sizeof(x), "%s%02X", h.empty() ? "" : " ", v);
        h += x;
    }
    return h;
}
/** A message against its vector, ?? any byte */
static bool Same(const std::vector<uint8_t>& got, const std::string& want)
{
    std::istringstream in(want);
    std::string x;
    size_t k = 0;
    while (in >> x)
    {
        if (k >= got.size() || (x != "??" && strtol(x.c_str(), nullptr, 16) != got[k]))
            return false;
        k++;
    }
    return k == got.size();
}

static uint32_t Crc(const std::string& s)
{
    return chompi::crash::Crc32(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// ---- FRIZZ around the transfer, as chompi_main.cpp's MainLoop has it ----
static uint32_t now_ms = 0;
static uint32_t Ms() { return now_ms; }
static uint32_t Ticks() { return now_ms * 10000u; }
static std::vector<std::vector<uint8_t>> sent;
static void Send(const uint8_t* d, size_t n)
{
    // MidiControl::Reply
    std::vector<uint8_t> m = {0xF0, 0x7D, 0x43, 0x48, 0x38 | 0x40};
    for (size_t i = 0; i < n; i++)
        m.push_back(d[i] & 0x7F);
    m.push_back(0xF7);
    sent.push_back(m);
}

static loops::Mem loader_mem;
static Mem xfer_mem;
static uint8_t load_buf[1024] __attribute__((aligned(32)));
static FATFS fs;

struct Frizz
{
    std::unique_ptr<LoopLoader> loader;
    std::unique_ptr<LoopTransfer> xfer;
    std::unique_ptr<Intake> intake;
    Env env;
    bool bench = false;

    void Boot()
    {
        card.present = true;
        card.cut_after = -1;
        f_mount(&fs, "", 1);
        LoopTransfer::Boot();
        loader.reset(new LoopLoader());
        loader->Init(load_buf, &loader_mem, &fs, Ms, Ticks, 10000000);
        intake.reset(new Intake());
        xfer.reset(new LoopTransfer());
        xfer->Init(intake.get(), loader.get(), &xfer_mem, &fs, Ms, Send, nullptr, nullptr);
        env = {true, false, false, false, false, true};
    }
    /** From the computer: libDaisy's parser keeps 128 bytes after F0, MidiControl::SysEx 0x38's */
    void Deliver(const std::vector<uint8_t>& m)
    {
        if (!env.usb || m.size() < 2 || m[0] != 0xF0 || m.back() != 0xF7)
            return;
        std::vector<uint8_t> d(m.begin() + 1, m.end() - 1);
        if (d.size() > kMaxRequest)
            d.resize(kMaxRequest);
        if (d.size() >= 4 && d[0] == 0x7D && d[1] == 0x43 && d[2] == 0x48 && d[3] == 0x38)
            intake->Take(d.data() + 4, d.size() - 4);
    }
    /** A MainLoop pass: what went out */
    std::vector<std::vector<uint8_t>> Pass()
    {
        sent.clear();
        if (bench)
        {
            Request q;
            uint8_t d[40];
            if (intake->TakeRequest(q))
                Send(d, BenchAnswer(q, d));
        }
        else
        {
            if (!env.lent)
                loader->Process(env.card_busy);
            xfer->Process(env);
            int id;
            while (xfer->TakeChanged(id))
                ;
            xfer->Send();
        }
        now_ms += 2;
        if (!env.usb)
            sent.clear();
        return sent;
    }
    bool Idle() const { return bench || (!loader->Busy() && xfer->Settled()); }
    bool Settle(std::vector<std::vector<uint8_t>>& out)
    {
        for (int k = 0; k < 200000; k++)
        {
            const auto s = Pass();
            out.insert(out.end(), s.begin(), s.end());
            if (s.empty() && Idle())
                return true;
        }
        return false;
    }
    void Set(const std::string& k, bool v)
    {
        if (k == "lent" && v && !env.lent)
        {
            // CardLendProcess: the job ends first, then FatFs lets go of the card
            xfer->Lend();
            f_mount(nullptr, "", 0);
        }
        if (k == "lent" && !v && env.lent)
        {
            f_mount(&fs, "", 1);
            loader->Request(LoopLoader::SCANNING);
        }
        bool* f = k == "card" ? &env.card : k == "lent" ? &env.lent : k == "recording" ? &env.recording
                  : k == "loading" ? &env.loading : k == "card_busy" ? &env.card_busy : &env.usb;
        *f = v;
    }
};

/** One of the vectors' sessions, replayed: false (and why, printed) if anything differs */
static bool Replay(const J& session)
{
    const std::string name = session.Get("name")->str;
    const J& c = *session.Get("card");
    card = FakeCard();
    card.dirs[""] = true;
    for (const J& d : c.Get("dirs")->arr)
        card.dirs[d.str] = true;
    for (const auto& kv : c.Get("files")->obj)
    {
        const auto b = Bytes(kv.second.str);
        card.files[kv.first] = std::string(b.begin(), b.end());
    }
    card.csize = static_cast<uint16_t>(c.Get("cluster")->num / 512);
    card.clusters = static_cast<DWORD>(c.Get("clusters")->num);
    card.fsinfo = c.Get("fsinfo")->b;
    card.frag_clusters = static_cast<DWORD>(c.Get("frag_clusters")->num);
    Frizz f;
    f.bench = session.Get("bench") != nullptr;
    now_ms = 0;
    size_t n = 0;
    for (const J& st : session.Get("steps")->arr)
    {
        n++;
        std::vector<std::vector<uint8_t>> out;
        bool compare = false;
        if (st.Get("boot"))
            f.Boot();
        else if (const J* app = st.Get("app"))
            f.Deliver(Bytes(app->str));
        else if (const J* run = st.Get("run"))
        {
            for (int k = 0; k < static_cast<int>(run->num); k++)
            {
                const auto s = f.Pass();
                out.insert(out.end(), s.begin(), s.end());
            }
            compare = true;
        }
        else if (st.Get("settle"))
        {
            if (!f.Settle(out))
            {
                printf("  %s, step %zu: didn't settle\n", name.c_str(), n);
                return false;
            }
            compare = true;
        }
        else if (const J* set = st.Get("set"))
            for (const auto& kv : set->obj)
                f.Set(kv.first, kv.second.b);
        else if (st.Get("remount"))
            f_mount(&fs, "", 1);
        else if (const J* put = st.Get("put"))
            for (const auto& kv : put->obj)
            {
                const auto b = Bytes(kv.second.str);
                card.files[kv.first] = std::string(b.begin(), b.end());
            }
        else if (const J* cut = st.Get("cut"))
        {
            if (cut->num == 0)
                card.Cut();
            else
                card.cut_after = card.changes + static_cast<int>(cut->num);
        }
        const J* want = st.Get("frizz");
        if (!compare || !want || want->t == J::NUL)
            continue;
        bool same = out.size() == want->arr.size();
        for (size_t k = 0; same && k < out.size(); k++)
            same = Same(out[k], want->arr[k].str);
        if (!same)
        {
            printf("  %s, step %zu: FRIZZ sent\n", name.c_str(), n);
            for (const auto& m : out)
                printf("    %s\n", Hex(m).c_str());
            printf("  where the model sent\n");
            for (const J& m : want->arr)
                printf("    %s\n", m.str.c_str());
            return false;
        }
    }
    const J& after = *session.Get("after");
    bool same = after.obj.size() == card.files.size();
    for (const auto& kv : after.obj)
    {
        const auto it = card.files.find(kv.first);
        char crc[12];
        if (it != card.files.end())
            snprintf(crc, sizeof(crc), "%08x", Crc(it->second));
        if (it == card.files.end() || it->second.size() != kv.second.Get("size")->num || kv.second.Get("crc32")->str != crc)
        {
            printf("  %s: %s isn't as the model left it\n", name.c_str(), kv.first.c_str());
            same = false;
        }
    }
    if (after.obj.size() != card.files.size())
    {
        printf("  %s: the card's files:", name.c_str());
        for (const auto& kv : card.files)
            printf(" %s", kv.first.c_str());
        printf("\n");
    }
    return same;
}

static void TestVectors()
{
    const std::string repo = Dir() + "../";
    const std::string cmd = "python3 " + repo + "tools/loopxfer.py vectors --check --out " + Dir() + "loopxfer/vectors.json";
    Check(system(cmd.c_str()) == 0, "vectors: vectors.json is what loopxfer.py vectors writes");
    std::ifstream in(Dir() + "loopxfer/vectors.json");
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    Reader r{text};
    const J v = r.Value();
    Check(v.Get("format") && v.Get("format")->num == 1 && v.Get("protocol")->arr[0].num == kLoopsProtocol
              && v.Get("protocol")->arr[1].num == kLoopsProtocolMinor,
          "vectors: format 1, the firmware's protocol version");
    for (const J& s : v.Get("sessions")->arr)
    {
        const std::string what = "vectors: " + s.Get("name")->str + ", " + s.Get("about")->str;
        Check(Replay(s), what.c_str());
    }
}

// ---- what the vectors don't reach ----

static std::string Wav(size_t frames, uint32_t seed, uint16_t channels = 2)
{
    std::string w;
    auto put32 = [&](uint32_t v) { w.append(reinterpret_cast<const char*>(&v), 4); };
    auto put16 = [&](uint16_t v) { w.append(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t data = static_cast<uint32_t>(frames * 2 * channels);
    w += "RIFF";
    put32(36 + data);
    w += "WAVEfmt ";
    put32(16);
    put16(1);
    put16(channels);
    put32(48000);
    put32(48000 * 2 * channels);
    put16(static_cast<uint16_t>(2 * channels));
    put16(16);
    w += "data";
    put32(data);
    uint32_t x = seed * 2654435761u + 1;
    for (uint32_t i = 0; i < data; i++)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        w.push_back(static_cast<char>(x >> 24));
    }
    return w;
}

struct App
{
    Frizz& f;
    uint8_t seq = 0;
    std::vector<uint8_t> last;
    /** A request sent, passes run until its answer: its status, the answer's bytes after STATUS */
    uint8_t Ask(uint8_t op, std::vector<uint8_t> args = {}, int passes = 10)
    {
        seq = static_cast<uint8_t>(seq % 127 + 1);
        std::vector<uint8_t> m = {0xF0, 0x7D, 0x43, 0x48, 0x38, op, seq};
        m.insert(m.end(), args.begin(), args.end());
        m.push_back(0xF7);
        f.Deliver(m);
        for (int k = 0; k < passes; k++)
            for (const auto& a : f.Pass())
                if (a[5] == op && a[6] == seq)
                {
                    last.assign(a.begin() + 8, a.end() - 1);
                    return a[7];
                }
        return 0xFF;
    }
    static std::vector<uint8_t> N(uint32_t v, size_t g)
    {
        std::vector<uint8_t> d(g);
        Put7(d.data(), 0, v, g);
        return d;
    }
    /** A whole write: its TID, or 0 if WRITE_BEGIN was refused; end: WRITE_END sent */
    uint8_t Write(uint8_t bank, uint8_t slot, const std::string& data, uint8_t flags = 0, bool end = true)
    {
        std::vector<uint8_t> a = {bank, slot, flags};
        const auto size = N(static_cast<uint32_t>(data.size()), 4);
        a.insert(a.end(), size.begin(), size.end());
        if (Ask(WRITE_BEGIN, a))
            return 0;
        const uint8_t tid = last[0];
        uint32_t next = 0;
        while (next < data.size())
        {
            for (uint32_t o = next; o < data.size() && o < next + kMaxChunk * kWindow; o += kMaxChunk)
            {
                const uint8_t len = static_cast<uint8_t>(std::min<size_t>(kMaxChunk, data.size() - o));
                std::vector<uint8_t> m = {0xF0, 0x7D, 0x43, 0x48, 0x38, CHUNK, tid};
                const auto off = N(o, 4);
                m.insert(m.end(), off.begin(), off.end());
                m.push_back(len);
                uint8_t packed[kPacked];
                const size_t n = Pack(reinterpret_cast<const uint8_t*>(data.data()) + o, len, packed);
                m.insert(m.end(), packed, packed + n);
                m.push_back(0xF7);
                f.Deliver(m);
            }
            const uint32_t was = next;
            for (int k = 0; k < 50 && next == was; k++)
                for (const auto& m : f.Pass())
                    if (m[5] == CHUNK && m[6] == 0 && m[8] == tid)
                        next = Get7(m.data() + 9, 4);
            if (next == was)
                return tid; // stuck (a power cut): the caller sees it
        }
        if (end)
        {
            std::vector<uint8_t> e = {tid};
            const auto crc = N(Crc(data), 5);
            e.insert(e.end(), crc.begin(), crc.end());
            Ask(WRITE_END, e);
        }
        return tid;
    }
    /** STATUS: the job's phase, result */
    void Job(uint8_t& phase, uint8_t& result)
    {
        Ask(STATUS);
        phase = last[7];
        result = last[19];
    }
};

/** The card's names that changed since before: only /FRIZZ's slots and temp files */
static bool OnlyAllowed(const std::map<std::string, std::string>& before)
{
    std::set<std::string> names;
    for (const auto& kv : before)
        names.insert(kv.first);
    for (const auto& kv : card.files)
        names.insert(kv.first);
    for (const std::string& p : names)
    {
        const auto a = before.find(p);
        const auto b = card.files.find(p);
        if (a != before.end() && b != card.files.end() && a->second == b->second)
            continue;
        size_t bank;
        int from, to;
        const bool frizz = !p.compare(0, 7, "/FRIZZ/") && p.find('/', 7) == std::string::npos;
        const std::string n = frizz ? p.substr(7) : "";
        if (!frizz || !(loops::SlotOf(n.c_str(), false, bank) || n == "xfer.tmp" || loops::SwapOf(n.c_str(), from, to)))
        {
            printf("  %s changed\n", p.c_str());
            return false;
        }
    }
    return true;
}

/** The slots' files on the card are whole loops (they walk as MONO or STEREO) */
static bool WholeLoops()
{
    for (const auto& kv : card.files)
    {
        size_t bank;
        if (kv.first.compare(0, 7, "/FRIZZ/") || !loops::SlotOf(kv.first.c_str() + 7, false, bank))
            continue;
        loops::WavWalk w;
        w.Start(static_cast<uint32_t>(kv.second.size()));
        uint32_t data = 0, frames = 0;
        const int r = w.Step(reinterpret_cast<const uint8_t*>(kv.second.data()), 0,
                             static_cast<uint32_t>(std::min<size_t>(kv.second.size(), 1024)), data, frames);
        if ((r != loops::MONO && r != loops::STEREO) || data + frames * 4 != kv.second.size())
        {
            printf("  %s isn't a whole loop\n", kv.first.c_str());
            return false;
        }
    }
    return true;
}

static void NewCard(std::map<std::string, std::string> files = {})
{
    card = FakeCard();
    card.dirs[""] = true;
    card.dirs["/FRIZZ"] = true;
    card.files = files;
    now_ms = 0;
}

static std::map<std::string, std::string> Base()
{
    return {{"/FRIZZ/a1.wav", Wav(300, 1)}, {"/FRIZZ/a2.wav", Wav(200, 2)}, {"/TAPE/cubbi_a1.wav", Wav(250, 3)},
            {"/FRIZZ/frizz_scenes.txt", "scenes"}, {"/notes.txt", "mine"}};
}

static bool Boot(Frizz& f)
{
    f.Boot();
    std::vector<std::vector<uint8_t>> out;
    return f.Settle(out);
}

static void TestTooLong()
{
    // a cubbi of 16 bars at 50 BPM and a frame
    std::string big = Wav(loops::kMaxFrames + 1, 5);
    NewCard({{"/TAPE/cubbi_b2.wav", big}});
    const auto before = card.files;
    Frizz f;
    Boot(f);
    App app{f};
    Check(app.Ask(COPY, {9, 1, 0, 0, 0}) == TOO_LONG, "copy: a cubbi longer than 16 bars at 50 BPM: TOO_LONG");
    Check(app.Ask(SLOT, {9, 1}) == OK && app.last[2] == loops::TOO_LONG && Get7(app.last.data() + 3, 4) == loops::kMaxFrames + 1,
          "copy: its SLOT says TOO_LONG, and its frames");
    Check(card.files == before, "copy: the card untouched");
}

static void TestFragments()
{
    // 512-byte clusters, a fragment each: 255 fragments load, 300 don't
    for (const size_t clusters : {255u, 300u})
    {
        NewCard(Base());
        card.csize = 1;
        card.frag_clusters = 1;
        const auto before = card.files;
        Frizz f;
        Boot(f);
        App app{f};
        const std::string w = Wav((clusters * 512 - 44) / 4, 7);
        const uint8_t tid = app.Write(1, 0, w);
        std::vector<std::vector<uint8_t>> out;
        f.Settle(out);
        uint8_t phase, result;
        app.Job(phase, result);
        if (clusters == 255)
            Check(tid && phase == DONE && card.files.count("/FRIZZ/b1.wav"), "fragments: 255, the loader's map's most: written");
        else
            Check(tid && phase == FAILED && result == FRAGMENTED && !card.files.count("/FRIZZ/b1.wav")
                      && !card.files.count(kTmp),
                  "fragments: 300: FRAGMENTED, nothing kept, xfer.tmp deleted");
        Check(OnlyAllowed(before), "fragments: only the allowed names changed");
    }
}

/** A replacing write, a copy and a swap with the power cut after each of their name changes:
 *  the boot after it leaves whole files only, xfer.tmp gone, the swap put right */
static void TestPowerCuts()
{
    const std::string fresh = Wav(321, 11);
    for (int what = 0; what < 3; what++)
        for (int cut = 0; cut <= 4; cut++)
        {
            NewCard(Base());
            const auto before = card.files;
            Frizz f;
            Boot(f);
            App app{f};
            if (cut)
                card.cut_after = card.changes + cut;
            else
                card.cut_after = card.changes + 1000; // no cut: the whole operation
            if (what == 0)
                app.Write(0, 0, fresh, kReplace);
            else if (what == 1)
                app.Ask(COPY, {8, 0, 0, 1, kReplace});
            else
                app.Ask(SWAP, {0, 0, 0, 1});
            std::vector<std::vector<uint8_t>> out;
            for (int k = 0; k < 200; k++)
                f.Pass();
            const bool died = !card.present;
            card.cut_after = -1;
            Boot(f);
            const bool whole = WholeLoops() && !card.files.count(kTmp) && OnlyAllowed(before);
            bool kept = true;
            for (const auto& kv : card.files)
                kept = kept && kv.first.find(".swp") == std::string::npos;
            char line[160];
            snprintf(line, sizeof(line), "power cut: %s, after %d name change%s%s: whole files only, no temp file left",
                     what == 0 ? "a replacing write" : what == 1 ? "a copy" : "a swap", cut, cut == 1 ? "" : "s",
                     died ? "" : " (it was done before)");
            Check(whole && kept, line);
            if (what == 2 && died)
            {
                // undone or finished, never lost
                const bool undone = card.files["/FRIZZ/a1.wav"] == before.at("/FRIZZ/a1.wav")
                                    && card.files["/FRIZZ/a2.wav"] == before.at("/FRIZZ/a2.wav");
                const bool done = card.files["/FRIZZ/a1.wav"] == before.at("/FRIZZ/a2.wav")
                                  && card.files["/FRIZZ/a2.wav"] == before.at("/FRIZZ/a1.wav");
                Check(undone || done, "power cut: the swap's two loops are both there, undone or done");
            }
        }
}

/** A load's DMA in flight: no FatFs call from the transfer meanwhile (it pauses for the load) */
static void TestLoadInFlight()
{
    NewCard(Base());
    card.raw_polls = 5;
    Frizz f;
    Boot(f);
    App app{f};
    const std::string w = Wav(20000, 13); // 80 KB: blocks to write
    std::vector<uint8_t> a = {3, 0, 0};
    const auto size = App::N(static_cast<uint32_t>(w.size()), 4);
    a.insert(a.end(), size.begin(), size.end());
    app.Ask(WRITE_BEGIN, a);
    std::vector<uint8_t> load(loops::kBufBytes / 8);
    // the loader loads a1 meanwhile, as the Loops page would: the env says loading
    const bool asked = f.loader->Request(LoopLoader::LOADING, 0, 0, 0,
                                         reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(load.data()) + 31) & ~31u));
    f.env.loading = true;
    int passes = 0;
    while (f.loader->Busy() && passes++ < 1000)
        f.Pass();
    f.env.loading = false;
    Check(asked && f.loader->Ready() && card.raw_clash == 0, "load in flight: the write pauses, no FatFs call during its DMA");
}

/** FSINFO without the free count: counted a FAT sector a pass, then FatFs keeps it */
static void TestFreeCount()
{
    NewCard(Base());
    card.fsinfo = false;
    card.clusters = 1000;
    Frizz f;
    f.Boot();
    int passes = 0;
    while (fs.free_clst > fs.n_fatent - 2 && passes < 100)
    {
        f.Pass();
        passes++;
    }
    Check(fs.free_clst == card.Free() && passes == (1002 + 127) / 128,
          "free space: counted a FAT sector a pass, (1002 + 127) / 128 of them, as FatFs's count");
    App app{f};
    const uint32_t kb = (app.Ask(STATUS), Get7(app.last.data() + 2, 4));
    Check(kb == card.Free() * 4 && (app.last[0] & 64), "free space: STATUS says it, in KB");
}

int main()
{
    TestVectors();
    TestTooLong();
    TestFragments();
    TestPowerCuts();
    TestLoadInFlight();
    TestFreeCount();
    return Finish();
}
