// Docs check: MANUAL.md's tables against the code they describe. The manual stays written by
// hand; its tables are checked here, so a change of a knob, a step, a colour or a CC that the
// manual doesn't follow fails. Built and run by unit.sh docs; DOCS_MANUAL=FILE checks another
// copy of the manual (to see a check fail on a falsified one).
//  - each effect's chapter: "White key N." is its key; its knob table, | Knob | Page 1 | Page 2 |,
//    has a text in the cells of the knobs it uses (FxParams::knobs) and "—" in the others, and
//    no Page 2 column without a page 2
//  - the finalizer's (#345): "White key N.", a | Knob | Function | table per module with the
//    knobs it uses (kFinalizerKnobs), its types and characters by their number and default,
//    its ranges and coarse steps as Finalizer.h has them, the key's colours by module
//  - stepped parameters: their cell lists their steps (1/16, 1/8T ... in bars, the slicer's
//    patterns bit for bit, "-12 to +12 semitones", "two steps") as the code's tables have
//    them, and its "(default ...)" is the default's step; Punch-in FX's "Stepped parameters"
//    bullet names every one, and their detents per step
//  - Coarse steps: every row against its grid (kFxGrids), the values it lists computed from
//    the grid's points over the knob's range as the effect's table gives it (and, where the
//    code has the range as a constant, that too); every parameter with a grid of its own has
//    a row, the rest are 10%
//  - the LEDs bullet: the key's colour and the knobs' first and last (FxSlots.h), by the
//    names in kColours
//  - MIDI: the notes and both CC tables against midimap (MidiControl.h), the effect channels'
//    and the latch CCs' order (FxSlots.h's keys), the CCs left free not used
//  - the settings page: the colours each key shows (SettingsPage.h's Draw), the clock factor's
//    and source's by their state
// FxSlots.h, PanelKeys.h, MidiControl.h and SettingsPage.h need the board (hardware.h), which
// a unit check doesn't have: they're read as text, strictly, and a table that doesn't parse
// fails. Prose isn't checked, only tables, lists and numbers.
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "check.h"
// the ranges some effects keep to themselves, to compare
#define private public
#include "Finalizer.h"
#include "FxControls.h"
#undef private
#include "LedColors.h"

using namespace chompi;
// CoarseStep doesn't need the engine
struct NoEngine
{
};
using Coarse = FxControls<NoEngine>;
using Strings = std::vector<std::string>;

static void CheckF(bool ok, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void CheckF(bool ok, const char* fmt, ...)
{
    char what[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(what, sizeof(what), fmt, args);
    va_end(args);
    Check(ok, what);
}

/** Checks of one kind as one line, naming each that failed */
struct Group
{
    std::string what, bad;
    explicit Group(const std::string& w) : what(w) {}
    void Add(bool ok, const char* fmt, ...) __attribute__((format(printf, 3, 4)))
    {
        if (ok)
            return;
        char one[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(one, sizeof(one), fmt, args);
        va_end(args);
        bad += (bad.empty() ? ": " : "; ") + std::string(one);
    }
    ~Group() { Check(bad.empty(), (what + bad).c_str()); }
};

static std::string Repo()
{
    std::string here = __FILE__;
    return here.substr(0, here.rfind('/')) + "/../..";
}

static std::string ReadFile(const std::string& path)
{
    std::ifstream in(path);
    if (!in)
    {
        printf("FAIL  can't read %s\n", path.c_str());
        exit(1);
    }
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

static Strings Lines(const std::string& text)
{
    Strings lines;
    std::stringstream s(text);
    std::string line;
    while (std::getline(s, line))
        lines.push_back(line);
    return lines;
}

static std::string Lower(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string Trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos)
        return "";
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

static bool Near(double a, double b, double rel) { return fabs(a - b) <= rel * fabs(b) + 1e-9; }

// ---- the manual

static Strings manual;

/** The lines under a heading ("## Name" or "### Name"), up to the next heading of its level
 *  or above; empty, and a FAIL, if there's none */
static Strings Under(const Strings& lines, const std::string& heading)
{
    const size_t level = heading.find(' ');
    for (size_t i = 0; i < lines.size(); i++)
    {
        if (lines[i] != heading)
            continue;
        Strings out;
        for (size_t j = i + 1; j < lines.size(); j++)
        {
            const size_t hashes = lines[j].find_first_not_of('#');
            if (hashes > 0 && hashes <= level && lines[j][hashes] == ' ')
                break;
            out.push_back(lines[j]);
        }
        return out;
    }
    CheckF(false, "MANUAL.md has a heading \"%s\"", heading.c_str());
    return {};
}

static Strings Cells(const std::string& line)
{
    Strings cells;
    std::string cell;
    for (size_t i = 1; i < line.size(); i++)
    {
        if (line[i] == '|')
        {
            cells.push_back(Trim(cell));
            cell.clear();
        }
        else
            cell += line[i];
    }
    return cells;
}

struct Table
{
    Strings header;
    std::vector<Strings> rows;
};

/** The n-th table (from 0) in lines whose header row starts with prefix */
static Table FindTable(const Strings& lines, const std::string& prefix, int n = 0)
{
    for (size_t i = 0; i < lines.size(); i++)
    {
        if (lines[i].compare(0, prefix.size(), prefix) != 0 || n-- > 0)
            continue;
        Table t;
        t.header = Cells(lines[i]);
        for (size_t j = i + 2; j < lines.size() && !lines[j].empty() && lines[j][0] == '|'; j++)
            t.rows.push_back(Cells(lines[j]));
        return t;
    }
    return {};
}

static std::string Strip(const std::string& s, const std::regex& re) { return std::regex_replace(s, re, ""); }
static const std::regex kParens("\\s*\\([^()]*\\)");

/** The bullet that starts with prefix, its bold left out, with its continuation lines, as
 *  one line without bold */
static std::string Bullet(const Strings& lines, const std::string& prefix)
{
    static const std::regex bold("\\*\\*");
    for (size_t i = 0; i < lines.size(); i++)
    {
        if (lines[i].compare(0, 2, "- ") != 0)
            continue;
        std::string text = Strip(lines[i].substr(2), bold);
        if (text.compare(0, prefix.size(), prefix) != 0)
            continue;
        for (size_t j = i + 1; j < lines.size() && lines[j].compare(0, 2, "  ") == 0; j++)
            text += " " + Strip(Trim(lines[j]), bold);
        return text;
    }
    return "";
}


static std::vector<double> Numbers(const std::string& s)
{
    std::vector<double> out;
    static const std::regex re("[-+]?\\d+(\\.\\d+)?");
    for (std::sregex_iterator it(s.begin(), s.end(), re), end; it != end; ++it)
        out.push_back(atof(it->str().c_str()));
    return out;
}

/** Frequencies in Hz: "100, 200, 400, 800 Hz … 6.4 kHz", a number without its unit taking
 *  the next one's. A "…" splits them: what comes before it into head, after it into tail */
static void Frequencies(const std::string& s, std::vector<double>* head, std::vector<double>* tail)
{
    static const std::regex re("(\\d+(?:\\.\\d+)?)( k?Hz)?|…");
    std::vector<double> pending;
    std::vector<double>* to = head;
    for (std::sregex_iterator it(s.begin(), s.end(), re), end; it != end; ++it)
    {
        if (it->str() == "…")
        {
            to = tail;
            continue;
        }
        pending.push_back(atof((*it)[1].str().c_str()));
        if ((*it)[2].matched)
        {
            const double unit = (*it)[2].str() == " kHz" ? 1000. : 1.;
            for (double f : pending)
                to->push_back(f * unit);
            pending.clear();
        }
    }
}

// ---- colours: the code's names, and the manual's for them

struct Colour
{
    const char* id; // LedColors.h's
    const float* rgb;
    const char* name; // the manual's
};
static const Colour kColours[] = {
    {"white", white, "white"},       {"red", red, "red"},          {"yellow", yellow, "yellow"},
    {"green", green, "green"},       {"med_blue", med_blue, "light blue"},
    {"blue", blue, "blue"},          {"pink", pink, "pink"},       {"purple", purple, "purple"},
    {"orange", orange, "orange"},    {"lime", lime, "lime"},       {"magenta", magenta, "magenta"},
    {"teal", teal, "teal"},          {"amber", amber, "amber"},    {"rose", rose, "rose"},
    {"violet", violet, "violet"},    {"cyan", cyan, "cyan"},
};

static std::string ColourName(const std::string& id)
{
    for (const auto& c : kColours)
        if (id == c.id)
            return c.name;
    return "";
}

/** The colour names in a text, in their order ("light blue" before "blue") */
static Strings ColourWords(const std::string& text)
{
    static const std::regex re("\\b(light blue|white|red|yellow|green|blue|pink|purple|orange|lime|"
                               "magenta|teal|amber|rose|violet|cyan)\\b");
    Strings out;
    for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it)
        out.push_back(it->str());
    return out;
}

// ---- the code read as text

static Strings Words(const std::string& s)
{
    static const std::regex re("\\w+");
    return {std::sregex_token_iterator(s.begin(), s.end(), re), std::sregex_token_iterator()};
}

static std::string Source(const char* file) { return ReadFile(Repo() + "/firmware/code/src/" + file); }

/** The KEY_N in a list between from and the next "};" */
static std::vector<int> KeyList(const std::string& text, const std::string& from)
{
    std::vector<int> keys;
    const size_t a = text.find(from);
    if (a == std::string::npos)
        return keys;
    const size_t b = text.find("};", a);
    const std::string list = text.substr(a, b - a);
    static const std::regex re("SwId::KEY_(\\d+)");
    for (std::sregex_iterator it(list.begin(), list.end(), re), end; it != end; ++it)
        keys.push_back(atoi((*it)[1].str().c_str()));
    return keys;
}

/** An enum's names in their order */
static Strings EnumNames(const std::string& text, const std::string& from)
{
    Strings names;
    const size_t a = text.find(from);
    if (a == std::string::npos)
        return names;
    const size_t open = text.find('{', a), close = text.find('}', a);
    std::string body = text.substr(open + 1, close - open - 1);
    static const std::regex re("\\b([A-Z_0-9]+)\\b");
    for (std::sregex_iterator it(body.begin(), body.end(), re), end; it != end; ++it)
        names.push_back(it->str());
    return names;
}

struct Slot
{
    int key;             // KEY_N
    std::string colour;  // the manual's name
    std::string knob[3]; // at 0, the middle, 1
};
static std::vector<Slot> slots;   // by FxId
static std::vector<int> white_keys, dark_keys; // KEY_N, left to right
static int comp_key;            // the finalizer's (kCompKey)
static std::string comp_knob[3];
static std::string fin_colour[4]; // its modules' (kFinColors)

/** White key 1-15 of a KEY_N, 0 if it's none */
static int WhiteOf(int key)
{
    for (size_t w = 0; w < white_keys.size(); w++)
        if (white_keys[w] == key)
            return static_cast<int>(w) + 1;
    return 0;
}

static void ReadKeys()
{
    const std::string panel = Source("PanelKeys.h");
    white_keys = KeyList(panel, "kWhiteKeys[kNumWhiteKeys] =");
    dark_keys = KeyList(panel, "kDarkKeys[kNumDarkKeys] =");
    CheckF(white_keys.size() == 15 && dark_keys.size() == 10,
           "PanelKeys.h: 15 white keys and 10 dark keys read (%zu, %zu)", white_keys.size(),
           dark_keys.size());

    const std::string fx = Source("FxSlots.h");
    static const std::regex row("\\{Hardware::SwId::KEY_(\\d+), (\\w+), \\{(\\w+), (\\w+), (\\w+)\\}, FxKind::\\w+\\}");
    bool names = true;
    for (std::sregex_iterator it(fx.begin(), fx.end(), row), end; it != end; ++it)
    {
        Slot s;
        s.key = atoi((*it)[1].str().c_str());
        s.colour = ColourName((*it)[2]);
        for (int i = 0; i < 3; i++)
            s.knob[i] = ColourName((*it)[3 + i]);
        names = names && !s.colour.empty() && !s.knob[0].empty() && !s.knob[2].empty();
        slots.push_back(s);
    }
    CheckF(slots.size() == kNumFx, "FxSlots.h: kFxSlots read, a row per effect (%zu of %zu)",
           slots.size(), static_cast<size_t>(kNumFx));
    Check(names, "FxSlots.h: every colour of kFxSlots has a name in docs.cpp's kColours");
    std::smatch m;
    std::regex_search(fx, m, std::regex("kCompKey = Hardware::SwId::KEY_(\\d+)"));
    comp_key = m.empty() ? 0 : atoi(m[1].str().c_str());
    std::regex_search(fx, m, std::regex("kCompKnobColors\\[3\\] = \\{(\\w+), (\\w+), (\\w+)\\}"));
    for (int i = 0; !m.empty() && i < 3; i++)
        comp_knob[i] = ColourName(m[1 + i]);
    Check(comp_key && !comp_knob[0].empty() && !comp_knob[2].empty(),
          "FxSlots.h: the finalizer's key and knob colours read");
    std::regex_search(fx, m, std::regex("kFinColors\\[4\\] = \\{(\\w+), (\\w+), (\\w+), (\\w+)\\}"));
    for (int i = 0; !m.empty() && i < 4; i++)
        fin_colour[i] = ColourName(m[1 + i]);
    Check(!fin_colour[0].empty() && !fin_colour[3].empty(), "FxSlots.h: the finalizer's module colours read");
    while (slots.size() < kNumFx)
        slots.push_back({});
}

// ---- the effects' chapters

static const char* const kChapters[kNumFx] = {
    "Freezer", "Shifter", "Folder", "Crusher", "Filter", "Flanger", "Resonator",
    "Slicer", "Wow & flutter", "Tape stop", "Delay", "Reverb", "Phaser", "Chaos key",
};
static const char* const kFinChapter = "Finalizer";
// the effects as prose names them
static const char* const kFxWords[kNumFx] = {
    "freezer", "shifter", "folder", "crusher", "filter", "flanger", "resonator",
    "slicer", "wow", "tape stop", "delay", "reverb", "phaser", "chaos",
};

struct Knobs
{
    std::string cell[kNumFxParams]; // page 1's knobs 1-4, then page 2's
    bool page2 = false;             // the table has a Page 2 column
};
static Knobs knobs[kNumFx];

static const FxParams& ParamsOf(size_t fx) { return kFxParams[fx]; }

static void CheckChapter(size_t fx)
{
    const char* name = kChapters[fx];
    const Strings lines = Under(manual, std::string("## ") + name);
    if (lines.empty())
        return;
    const FxParams& p = ParamsOf(fx);

    // its key
    const int key = slots[fx].key;
    int said = 0;
    std::smatch m;
    for (const auto& l : lines)
        if (std::regex_search(l, m, std::regex("^White key (\\d+)\\.")))
        {
            said = atoi(m[1].str().c_str());
            break;
        }
    CheckF(said == WhiteOf(key), "%s: \"White key %d.\", it's white key %d (FxSlots.h)", name, said,
           WhiteOf(key));

    // the knob table
    const Table t = FindTable(lines, "| Knob | Page 1");
    Knobs& k = knobs[fx];
    k.page2 = t.header.size() >= 3 && t.header[2] == "Page 2";
    CheckF(t.rows.size() == kNumFxKnobs, "%s: its knob table has a row per knob (%zu)", name,
           t.rows.size());
    for (const auto& row : t.rows)
    {
        const int knob = atoi(row[0].c_str());
        if (knob < 1 || knob > static_cast<int>(kNumFxKnobs))
            continue;
        k.cell[knob - 1] = row.size() > 1 ? row[1] : "";
        if (k.page2)
            k.cell[knob - 1 + kNumFxKnobs] = row.size() > 2 ? row[2] : "";
    }
    Group cells(std::string(name) + ": the knob table's cells, \"—\" for the knobs it doesn't use (FxParams.h)");
    for (size_t q = 0; q < kNumFxParams; q++)
    {
        const bool used = p.knobs & (1 << q);
        const std::string& cell = k.cell[q];
        const bool says = !cell.empty() && cell.compare(0, strlen("—"), "—") != 0;
        const size_t page = q / kNumFxKnobs + 1, knob = q % kNumFxKnobs + 1;
        if (page == 2 && !k.page2)
            cells.Add(!used, "page 2's knob %zu is used, no Page 2 column", knob);
        else
            cells.Add(says == used, "page %zu's knob %zu \"%.40s\" is %s", page, knob, cell.c_str(),
                      used ? "used" : "unused, \"—\"");
    }

    // its LEDs
    std::string leds = Bullet(lines, "LEDs:");
    CheckF(!leds.empty(), "%s: a bullet \"LEDs: ...\"", name);
    leds = Strip(leds, kParens);
    const std::string key_colour = slots[fx].colour;
    std::regex_search(leds, m, std::regex("the key is ([a-z ]+?)[;,.]"));
    const Strings said_key = m.empty() ? Strings() : ColourWords(m[1]);
    CheckF(said_key.size() == 1 && said_key[0] == key_colour, "%s: the key is %s (FxSlots.h)",
           name, key_colour.c_str());
    const std::string* knob_colours = slots[fx].knob;
    const std::string pair = knob_colours[0] + " to " + knob_colours[2];
    CheckF(leds.find(pair) != std::string::npos, "%s: the knobs go %s (FxSlots.h)", name,
           pair.c_str());
}

// ---- stepped parameters

static std::string List(const std::vector<double>& v);

/** A step's length in bars, from the manual: "1/8T", "1/4.", "1 bar", "2 bars", "off" */
static double Bars(const std::string& s)
{
    if (s == "off")
        return 0.;
    std::smatch m;
    if (std::regex_match(s, m, std::regex("(\\d+) bars?")))
        return atof(m[1].str().c_str());
    if (std::regex_match(s, m, std::regex("(\\d+)/(\\d+)([T.]?)")))
    {
        double v = atof(m[1].str().c_str()) / atof(m[2].str().c_str());
        if (m[3] == "T")
            v *= 2. / 3.;
        else if (m[3] == ".")
            v *= 1.5;
        return v;
    }
    return -1.;
}

/** What the code has for a stepped parameter's steps, in bars (the freezer's roll: the
 *  length's fraction); empty where it keeps them to itself (the chaos key's grid) */
static std::vector<double> CodeSteps(size_t fx, size_t q)
{
    std::vector<double> v;
    if (fx == FX_FREEZER && q == 0)
        for (uint8_t d : kFreezerBarDivisions)
            v.push_back(1. / d);
    if (fx == FX_FREEZER && q == 2)
        for (size_t i = 0; i < chompi::Freezer::kNumRolls; i++)
            v.push_back(i ? 1. / (1 << i) : 0.);
    if ((fx == FX_FILTER && q == 3) || (fx == FX_PHASER && q == 0))
        for (uint32_t pulses : kLfoDivisionPulses)
            v.push_back(static_cast<double>(pulses) / kPulsesPerBar);
    if (fx == FX_TAPESTOP && q == 0)
        for (uint8_t s : kTapeStop16ths)
            v.push_back(s / 16.);
    if (fx == FX_TAPESTOP && q == 1)
        for (uint8_t s : kTapeStart16ths)
            v.push_back(s / 16.);
    if (fx == FX_DELAY && q == 0)
        for (float d : delayDivs)
            v.push_back(d);
    return v;
}

static int NumberWord(const std::string& w)
{
    static const char* const kWords[] = {"zero", "one", "two", "three", "four", "five",
                                         "six", "seven", "eight", "nine", "ten"};
    for (int i = 0; i < 11; i++)
        if (w == kWords[i])
            return i;
    return -1;
}

static void CheckSteps(size_t fx, size_t q)
{
    const FxParams& p = kFxParams[fx];
    const size_t steps = p.steps[q];
    const bool split = p.split & (1 << q);
    const std::string& cell = knobs[fx].cell[q];
    const char* name = kChapters[fx];
    const size_t page = q / kNumFxKnobs + 1, knob = q % kNumFxKnobs + 1;
    char where[96];
    snprintf(where, sizeof(where), "%s: page %zu's knob %zu", name, page, knob);

    // its list: the longest run of steps between ", "
    const size_t colon = cell.find(':');
    std::string list = colon == std::string::npos ? cell : cell.substr(colon + 1);
    Strings tokens, run, best;
    for (size_t a = 0;;)
    {
        const size_t b = list.find(", ", a);
        tokens.push_back(Trim(list.substr(a, b == std::string::npos ? b : b - a)));
        if (b == std::string::npos)
            break;
        a = b + 2;
    }
    static const std::regex step("off|\\d+/\\d+[T.]?|\\d+ bars?|`[x.]+`");
    for (auto t : tokens)
    {
        t = Trim(t.substr(0, t.find(" (")));
        if (std::regex_match(t, step))
            run.push_back(t);
        else
            run.clear();
        if (run.size() > best.size())
            best = run;
    }

    std::smatch m;
    std::regex_search(cell, m, std::regex("\\(default ([^,)]+)"));
    const std::string said_default = m.empty() ? "" : m[1].str();
    const size_t at = StepIndex(p.defaults[q], steps);

    if (best.size() >= 2)
    {
        CheckF(best.size() == steps, "%s lists its %zu steps (%zu)", where, steps, best.size());
        if (best[0][0] == '`')
        {
            bool same = best.size() == chompi::Slicer::kNumPatterns;
            for (size_t i = 0; same && i < best.size(); i++)
            {
                uint8_t bits = 0;
                for (size_t j = 1; j + 1 < best[i].size(); j++)
                    bits = static_cast<uint8_t>(bits << 1 | (best[i][j] == 'x'));
                same = bits == kSlicerPatterns[i];
            }
            CheckF(same, "%s: the patterns, bit for bit as kSlicerPatterns", where);
        }
        else
        {
            const std::vector<double> code = CodeSteps(fx, q);
            if (!code.empty())
            {
                bool same = code.size() == best.size();
                for (size_t i = 0; same && i < best.size(); i++)
                    same = Near(Bars(best[i]), code[i], 1e-6);
                CheckF(same, "%s: its steps in bars as the code's, %s", where, List(code).c_str());
            }
        }
        if (!split)
            CheckF(at < best.size() && said_default == best[at], "%s: (default %s), the step %s",
                   where, said_default.c_str(), at < best.size() ? best[at].c_str() : "?");
        return;
    }
    if (std::regex_search(cell, m, std::regex("([-+]?\\d+) to \\+?(\\d+) semitones")))
    {
        const int lo = atoi(m[1].str().c_str()), hi = atoi(m[2].str().c_str());
        CheckF(hi - lo + 1 == static_cast<int>(steps), "%s: %d to %d semitones, %zu steps", where,
               lo, hi, steps);
        CheckF(atoi(said_default.c_str()) == lo + static_cast<int>(at),
               "%s: (default %s), the step %d", where, said_default.c_str(), lo + static_cast<int>(at));
        return;
    }
    if (std::regex_search(cell, m, std::regex("\\b([a-z]+) steps")) && NumberWord(m[1]) > 0)
    {
        CheckF(NumberWord(m[1]) == static_cast<int>(steps), "%s: %s steps, %zu", where,
               m[1].str().c_str(), steps);
        return;
    }
    if (std::regex_search(cell, m, std::regex("(\\d+)-(\\d+) apart")))
    {
        CheckF(atoi(m[2].str().c_str()) - atoi(m[1].str().c_str()) + 1 == static_cast<int>(steps),
               "%s: %s-%s apart, %zu steps", where, m[1].str().c_str(), m[2].str().c_str(), steps);
        return;
    }
    CheckF(false, "%s, a stepped parameter: its cell lists its %zu steps", where, steps);
}

/** The first word of a parameter's name in its cell: "Length", "LFO division" -> "lfo" */
static std::string ParamWord(size_t fx, size_t q)
{
    std::string c = Lower(knobs[fx].cell[q]);
    return c.substr(0, c.find_first_of(" :,("));
}

static bool Stepped(size_t fx, size_t q) { return kFxParams[fx].steps[q] && (kFxParams[fx].knobs & (1 << q)); }

static void CheckSteppedBullet()
{
    const Strings lines = Under(Under(manual, "## Punch-in FX"), "### Playing an effect");
    const std::string bullet = Lower(Bullet(lines, "Stepped parameters"));
    CheckF(!bullet.empty(), "Punch-in FX: a bullet \"Stepped parameters ...\"");
    std::smatch m;
    std::regex_search(bullet, m, std::regex("one step per (\\d+) detents"));
    CheckF(!m.empty() && atof(m[1].str().c_str()) == kFxDetentsPerStep,
           "Punch-in FX: stepped parameters move one step per %g detents (FxControls.h)",
           kFxDetentsPerStep);
    for (size_t fx = 0; fx < kNumFx; fx++)
    {
        bool any = false;
        for (size_t q = 0; q < kNumFxParams; q++)
        {
            if (!Stepped(fx, q))
                continue;
            any = true;
            const std::string w = ParamWord(fx, q);
            CheckF(bullet.find(kFxWords[fx]) != std::string::npos && bullet.find(w) != std::string::npos,
                   "Punch-in FX's stepped parameters name the %s's %s", kFxWords[fx], w.c_str());
        }
        if (!any)
            CheckF(bullet.find(kFxWords[fx]) == std::string::npos,
                   "Punch-in FX's stepped parameters: the %s has none", kFxWords[fx]);
    }
}

// ---- coarse steps

/** A grid's points in 0-1, as SHIFT + turn steps through them (Coarse::CoarseStep) */
static std::vector<float> Points(const FxGrid& g)
{
    std::vector<float> pts;
    const float first = Coarse::CoarseStep(g, 0.f, 1.f);
    if (first != 0.f && Coarse::CoarseStep(g, first, -1.f) == 0.f)
        pts.push_back(0.f);
    for (float v = 0.f;;)
    {
        const float next = Coarse::CoarseStep(g, v, 1.f);
        if (next == v)
            break;
        pts.push_back(next);
        v = next;
    }
    return pts;
}

static const FxGrid& GridOf(size_t fx, size_t q) { return kFxGrids[ParamsOf(fx).coarse[q]]; }

static std::string List(const std::vector<double>& v)
{
    std::string s;
    for (size_t i = 0; i < v.size(); i++)
    {
        char one[32];
        snprintf(one, sizeof(one), "%s%.4g", i ? ", " : "", v[i]);
        s += one;
    }
    return s;
}

/** listed (head, then after a "…" tail) against values, each within rel */
static bool Listed(const std::vector<double>& head, const std::vector<double>& tail,
                   const std::vector<double>& values, double rel)
{
    if (head.size() + tail.size() > values.size() || (tail.empty() && head.size() != values.size()))
        return false;
    for (size_t i = 0; i < head.size(); i++)
        if (!Near(head[i], values[i], rel))
            return false;
    for (size_t i = 0; i < tail.size(); i++)
        if (!Near(tail[i], values[values.size() - tail.size() + i], rel))
            return false;
    return true;
}

/** An exponential knob from lo at 0 to hi at 1, at the grid's points */
static std::vector<double> Exp(const FxGrid& g, double lo, double hi)
{
    std::vector<double> v;
    for (float p : Points(g))
        v.push_back(lo * pow(hi / lo, p));
    return v;
}

static std::vector<double> Reversed(std::vector<double> v) { return {v.rbegin(), v.rend()}; }

static bool Range(const std::string& cell, const char* re, double* lo, double* hi)
{
    std::smatch m;
    if (!std::regex_search(cell, m, std::regex(re)))
        return false;
    *lo = atof(m[1].str().c_str());
    *hi = atof(m[2].str().c_str());
    return true;
}

static void CheckCoarse()
{
    const Strings lines = Under(Under(manual, "## Punch-in FX"), "### Coarse steps");
    const Table t = FindTable(lines, "| Parameter | Coarse grid");
    CheckF(!t.rows.empty(), "Punch-in FX: the Coarse steps table");
    std::set<std::pair<size_t, size_t>> listed; // (fx, param) a row covers
    bool everything_else = false;
    for (const auto& row : t.rows)
    {
        const std::string label = row[0], grid = row.size() > 1 ? row[1] : "";
        std::vector<std::pair<size_t, size_t>> params;
        std::vector<double> head, tail;
        double lo, hi;
        auto prefix = [&](const char* s) { return label.compare(0, strlen(s), s) == 0; };
        if (prefix("Resonator pitch"))
        {
            params = {{FX_RESONATOR, 0}};
            const bool range = Range(knobs[FX_RESONATOR].cell[0], "([\\d.]+) Hz to ([\\d.]+) Hz", &lo, &hi);
            CheckF(range && lo == chompi::Resonator::kLowestHz && hi == chompi::Resonator::kHighestHz,
                   "Resonator: the pitch from %g to %g Hz (FxResonator.h)", chompi::Resonator::kLowestHz,
                   chompi::Resonator::kHighestHz);
            static const char* const kNotes[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
            bool notes = true;
            Strings names;
            for (double hz : Exp(GridOf(FX_RESONATOR, 0), lo, hi))
            {
                const double semis = 12. * log2(hz / 440.);
                notes = notes && fabs(semis - round(semis)) < .02;
                const int n = static_cast<int>(round(semis)) + 57; // A4
                names.push_back(std::string(kNotes[n % 12]) + std::to_string(n / 12));
            }
            std::smatch m;
            std::regex_search(grid, m, std::regex("([A-G]#?\\d) to ([A-G]#?\\d)"));
            CheckF(notes && !m.empty() && names.front() == m[1].str() && names.back() == m[2].str(),
                   "Coarse steps: the resonator's pitch on the notes, %s to %s", names.front().c_str(),
                   names.back().c_str());
        }
        else if (prefix("Shifter shift"))
        {
            params = {{FX_SHIFTER, 0}};
            Range(knobs[FX_SHIFTER].cell[0], "([-+]?\\d+) to \\+?(\\d+) semitones", &lo, &hi);
            std::vector<double> semis;
            for (float p : Points(GridOf(FX_SHIFTER, 0)))
                semis.push_back(round(lo + p * (hi - lo)));
            CheckF(Numbers(grid) == semis, "Coarse steps: the shifter's shift, %s semitones",
                   List(semis).c_str());
        }
        else if (prefix("Shifter stereo"))
        {
            params = {{FX_SHIFTER, kFxStereoParam}};
            if (grid.find("Quarter") != std::string::npos)
                CheckF(GridOf(FX_SHIFTER, kFxStereoParam).spacing == .25f,
                       "Coarse steps: the shifter's stereo in quarters (of its semitone)");
        }
        else if ((prefix("Mix and Band") || prefix("Level")) && Numbers(grid).empty())
        {
            CheckF(false, "Coarse steps: %s, a number in its grid", label.c_str());
            continue;
        }
        else if (prefix("Mix and Band"))
        {
            Group g("Coarse steps: every Mix and Band (page 2) in the row's 10%");
            for (size_t fx = 0; fx < kNumFx; fx++)
                for (size_t q : {size_t(4), size_t(6)})
                    if ((kFxParams[fx].knobs & (1 << q)) && ParamWord(fx, q) == (q == 4 ? "mix" : "band"))
                    {
                        params.push_back({fx, q});
                        g.Add(Near(GridOf(fx, q).spacing * 100., Numbers(grid).at(0), 1e-4),
                              "the %s's %s in %g%%", kFxWords[fx], q == 4 ? "Mix" : "Band",
                              GridOf(fx, q).spacing * 100.);
                    }
        }
        else if (prefix("Level"))
        {
            Group levels("Coarse steps: every Level (page 2) in the row's dB steps, 0 dB on one");
            for (size_t fx = 0; fx < kNumFx; fx++)
                if (kFxParams[fx].knobs & 0x80)
                {
                    params.push_back({fx, 7});
                    const FxGrid& g = GridOf(fx, 7);
                    const double db = 20. * log10(FxOutput::LevelGain(g.origin + g.spacing)
                                                  / FxOutput::LevelGain(g.origin));
                    levels.Add(Near(db, Numbers(grid).at(0), 1e-3) && FxOutput::LevelGain(g.origin) == 1.f,
                               "the %s's in %g dB steps from %g", kFxWords[fx], db, g.origin);
                }
        }
        else if (prefix("Crusher rate"))
        {
            params = {{FX_CRUSHER, 0}};
            const std::string& cell = knobs[FX_CRUSHER].cell[0];
            const bool range = Range(cell, "([\\d.]+) kHz down to ([\\d.]+) Hz", &hi, &lo);
            hi *= 1000.;
            CheckF(range && Near(lo, chompi::Crusher::kRateBottom * 48000., 1e-4),
                   "Crusher: the rate down to %g Hz (FxCrusher.h)", chompi::Crusher::kRateBottom * 48000.);
            std::smatch m;
            if (std::regex_search(cell, m, std::regex("halving every (\\d+)% of the knob")))
                CheckF(Near(log(2.) / log(hi / lo) * 100., atof(m[1].str().c_str()), .02),
                       "Crusher: the rate halves every %s%% of the knob", m[1].str().c_str());
            Frequencies(grid.substr(grid.find(':') + 1), &head, &tail);
            const std::vector<double> hz = Exp(GridOf(FX_CRUSHER, 0), hi, lo);
            CheckF(Listed(head, tail, hz, .03), "Coarse steps: the crusher's rate, %s Hz", List(hz).c_str());
        }
        else if (prefix("Crusher bits"))
        {
            params = {{FX_CRUSHER, 1}};
            Range(knobs[FX_CRUSHER].cell[1], "(\\d+) down to (\\d+)", &hi, &lo);
            if (grid.find("Whole bits") != std::string::npos)
                CheckF(Near(GridOf(FX_CRUSHER, 1).spacing * (hi - lo), 1., 1e-5),
                       "Coarse steps: the crusher's bits, whole bits from %g to %g", hi, lo);
        }
        else if (prefix("Folder drive"))
        {
            params = {{FX_FOLDER, 0}};
            const bool range = Range(knobs[FX_FOLDER].cell[0], "([\\d.]+)× up to ([\\d.]+)×", &lo, &hi);
            CheckF(range && hi == chompi::Folder::kMaxDrive, "Folder: the drive up to %g× (FxFolder.h)",
                   chompi::Folder::kMaxDrive);
            std::vector<double> listed_x;
            static const std::regex re("([\\d.]+)×");
            for (std::sregex_iterator it(grid.begin(), grid.end(), re), end; it != end; ++it)
                listed_x.push_back(atof((*it)[1].str().c_str()));
            const std::vector<double> x = Exp(GridOf(FX_FOLDER, 0), lo, hi);
            CheckF(Listed(listed_x, {}, x, 1e-3), "Coarse steps: the folder's drive, %s×", List(x).c_str());
        }
        else if (prefix("Folder and crusher tone") || prefix("Wow & flutter tone"))
        {
            params = prefix("Folder") ? std::vector<std::pair<size_t, size_t>>{{FX_FOLDER, 2}, {FX_CRUSHER, 2}}
                                      : std::vector<std::pair<size_t, size_t>>{{FX_WARBLE, 2}};
            for (const auto& fp : params)
            {
                // ToneCoeff's lowpass, back to its Hz (just below the top, where it's open)
                std::vector<double> hz;
                for (float p : Points(GridOf(fp.first, fp.second)))
                {
                    const double c = ToneCoeff(p >= 1.f ? 1.f - 1e-6f : p, 48000.f);
                    hz.push_back(-log(1. - c) * 48000. / (2. * M_PI));
                }
                hz = Reversed(hz);
                if (prefix("Folder"))
                {
                    head.clear(), tail.clear();
                    Frequencies(grid.substr(grid.find(':') + 1), &head, &tail);
                    CheckF(Listed(head, tail, hz, .02), "Coarse steps: the %s's tone, octaves %s Hz",
                           kFxWords[fp.first], List(hz).c_str());
                }
                else
                    CheckF(kFxParams[FX_WARBLE].coarse[2] == kFxParams[FX_FOLDER].coarse[2],
                           "Coarse steps: the wow & flutter's tone as the folder's");
            }
        }
        else if (prefix("Resonator tone"))
        {
            params = {{FX_RESONATOR, 2}};
            Range(knobs[FX_RESONATOR].cell[2], "([\\d.]+) kHz to ([\\d.]+) kHz", &lo, &hi);
            Frequencies(grid, &head, &tail);
            const std::vector<double> hz = Reversed(Exp(GridOf(FX_RESONATOR, 2), lo * 1000., hi * 1000.));
            CheckF(Listed(head, tail, hz, .03), "Coarse steps: the resonator's tone, %s Hz", List(hz).c_str());
        }
        else if (prefix("Stepped parameters"))
        {
            const std::string text = Lower(label);
            for (size_t fx = 0; fx < kNumFx; fx++)
                for (size_t q = 0; q < kNumFxParams; q++)
                {
                    const FxParams& p = kFxParams[fx];
                    const FxGrid& g = GridOf(fx, q);
                    if (!Stepped(fx, q) || (p.split & (1 << q)) || g.points)
                        continue;
                    params.push_back({fx, q});
                    const std::string w = ParamWord(fx, q);
                    CheckF(g.origin == 0.f && Near(g.spacing * (p.steps[q] - 1), 1., 1e-5)
                               && text.find(kFxWords[fx]) != std::string::npos
                               && text.find(w) != std::string::npos,
                           "Coarse steps: the %s's %s, a step per detent, in the stepped row",
                           kFxWords[fx], w.c_str());
                }
            std::smatch m;
            std::regex_search(grid, m, std::regex("instead of per (\\d+)"));
            CheckF(!m.empty() && atof(m[1].str().c_str()) == kFxDetentsPerStep,
                   "Coarse steps: instead of a step per %g detents", kFxDetentsPerStep);
        }
        else if (prefix("Phaser rate"))
        {
            params = {{FX_PHASER, 0}};
            const FxParams& p = kFxParams[FX_PHASER];
            std::smatch m;
            std::regex_search(grid, m, std::regex("\\(([^)]*Hz)\\)"));
            const std::vector<double> said = m.empty() ? std::vector<double>() : Numbers(m[1]);
            std::vector<double> hz;
            size_t synced = 0;
            bool on_steps = true;
            for (float pt : Points(GridOf(FX_PHASER, 0)))
            {
                if (SplitUpper(pt))
                    on_steps = on_steps && Near(pt, SplitStepValue(static_cast<float>(synced++), p.steps[0]), 1e-5);
                else
                    hz.push_back(CurveMap(SplitLower(pt), kFlangerRateX, kFlangerRateHz, 5));
            }
            CheckF(Listed(said, {}, hz, .06), "Coarse steps: the phaser's free rate, %s Hz", List(hz).c_str());
            CheckF(on_steps && synced == p.steps[0], "Coarse steps: the phaser's synced rate, each of its %zu divisions",
                   static_cast<size_t>(p.steps[0]));
        }
        else if (prefix("Phaser centre"))
        {
            params = {{FX_PHASER, 3}};
            const std::string& cell = knobs[FX_PHASER].cell[3];
            std::vector<double> range_head, range_tail;
            Frequencies(cell.substr(0, cell.find(" (")), &range_head, &range_tail);
            CheckF(range_head.size() >= 2 && range_head[0] == chompi::Phaser::kCentreLowHz
                       && Near(range_head[1], chompi::Phaser::kCentreLowHz * pow(2., chompi::Phaser::kCentreOctaves), 1e-6),
                   "Phaser: the centre from %g Hz, %g octaves (FxPhaser.h)", chompi::Phaser::kCentreLowHz,
                   chompi::Phaser::kCentreOctaves);
            if (range_head.size() >= 2)
            {
                Frequencies(grid, &head, &tail);
                const std::vector<double> hz = Exp(GridOf(FX_PHASER, 3), range_head[0], range_head[1]);
                CheckF(Listed(head, tail, hz, 1e-3), "Coarse steps: the phaser's centre, %s Hz", List(hz).c_str());
            }
        }
        else if (prefix("Everything else"))
        {
            everything_else = true;
            continue;
        }
        else
        {
            CheckF(false, "Coarse steps: a row docs.cpp knows, \"%s\"", label.c_str());
            continue;
        }
        listed.insert(params.begin(), params.end());
    }
    Check(everything_else, "Coarse steps: a row \"Everything else\"");
    Group rest("Coarse steps: every other parameter in 10% steps (\"Everything else\")");
    for (size_t fx = 0; fx < kNumFx; fx++)
        for (size_t q = 0; q < kNumFxParams; q++)
            if ((kFxParams[fx].knobs & (1 << q)) && !listed.count({fx, q}))
                rest.Add(kFxParams[fx].coarse[q] == GRID_10, "the %s's page %zu knob %zu has a grid of its own, no row",
                         kFxWords[fx], q / kNumFxKnobs + 1, q % kNumFxKnobs + 1);
}

// ---- the finalizer (#345)

static const char* const kFinModuleNames[chompi::kFinModules] = {"Level", "EQ", "Comp", "Width"};

/** The names a stepped cell lists before its "(default ...)", comma-separated after its ": " */
static Strings StepNames(const std::string& cell)
{
    Strings out;
    const size_t colon = cell.find(": ");
    const size_t paren = cell.find(" (default");
    if (colon == std::string::npos || paren == std::string::npos || paren < colon)
        return out;
    std::stringstream list(cell.substr(colon + 2, paren - colon - 2));
    std::string name;
    while (std::getline(list, name, ','))
        out.push_back(Trim(name));
    return out;
}

static void CheckFinalizer()
{
    const Strings lines = Under(manual, std::string("## ") + kFinChapter);
    if (lines.empty())
        return;
    std::smatch m;
    int said = 0;
    for (const auto& l : lines)
        if (std::regex_search(l, m, std::regex("^White key (\\d+)\\.")))
        {
            said = atoi(m[1].str().c_str());
            break;
        }
    CheckF(said == WhiteOf(comp_key), "Finalizer: \"White key %d.\", it's white key %d (FxSlots.h)", said,
           WhiteOf(comp_key));

    std::string cell[kNumFinParams];
    for (size_t mod = 0; mod < chompi::kFinModules; mod++)
    {
        const std::string heading = std::string("### ") + kFinModuleNames[mod] + " module";
        const Strings sub = Under(lines, heading);
        const Table t = FindTable(sub, "| Knob | Function");
        CheckF(t.rows.size() == kNumFxKnobs, "Finalizer: %s's table, | Knob | Function |, a row per knob (%zu)",
               kFinModuleNames[mod], t.rows.size());
        Group cells(std::string("Finalizer: ") + kFinModuleNames[mod] + "'s cells, \"—\" for the knobs it doesn't use (kFinalizerKnobs)");
        for (const auto& row : t.rows)
        {
            const int k = atoi(row[0].c_str());
            if (k < 1 || k > static_cast<int>(kNumFxKnobs))
                continue;
            const size_t p = mod * kNumFxKnobs + k - 1;
            cell[p] = row.size() > 1 ? row[1] : "";
            const bool used = kFinalizerKnobs[mod].knobs >> (k - 1) & 1;
            const bool says = !cell[p].empty() && cell[p].compare(0, strlen("—"), "—") != 0;
            cells.Add(says == used, "knob %d \"%.40s\" is %s", k, cell[p].c_str(), used ? "used" : "unused, \"—\"");
        }
    }

    // the stepped ones: the types and the characters, by their number and their default
    const Strings types = StepNames(cell[Finalizer::TYPE]);
    std::regex_search(cell[Finalizer::TYPE], m, std::regex("\\(default (\\w+)\\)"));
    CheckF(types.size() == kFinTypes && !m.empty()
               && m[1] == types[StepIndex(kFinalizerKnobs[0].defaults[Finalizer::TYPE], kFinTypes)],
           "Finalizer: Type's %d types, its default the default's step", static_cast<int>(kFinTypes));
    const Strings characters = StepNames(cell[Finalizer::CHARACTER_LOW]);
    std::regex_search(cell[Finalizer::CHARACTER_LOW], m, std::regex("\\(default ([^)]+)\\)"));
    const size_t def = StepIndex(kFinalizerKnobs[2].defaults[0], kFinCharacters);
    CheckF(characters.size() == kFinCharacters && !m.empty() && def < characters.size() && m[1] == characters[def],
           "Finalizer: Comp's %d characters, its default the default's step", static_cast<int>(kFinCharacters));
    std::string bullets;
    for (const auto& l : Under(lines, "### Comp module"))
        bullets += l + "\n";
    Group named("Finalizer: each character a bullet of its own");
    for (const auto& c : characters)
        named.Add(bullets.find("- **" + c + ":**") != std::string::npos, "\"%s\"", c.c_str());

    // the ranges, as Finalizer.h has them, and the coarse steps in dB
    double lo, hi;
    CheckF(Range(cell[Finalizer::INPUT], "(-[\\d.]+) dB to \\+([\\d.]+) dB", &lo, &hi) && lo == -Finalizer::kInputDb
               && hi == Finalizer::kInputDb,
           "Finalizer: Input -%g dB to +%g dB", Finalizer::kInputDb, Finalizer::kInputDb);
    auto coarse = [&](size_t p, double range, const char* what) {
        const FxParams& f = kFinalizerKnobs[p / kNumFxKnobs];
        const double step = kFxGrids[f.coarse[p % kNumFxKnobs]].spacing * range;
        std::regex_search(cell[p], m, std::regex("([\\d.]+) dB a coarse step"));
        CheckF(!m.empty() && Near(atof(m[1].str().c_str()), step, 1e-4), "Finalizer: %s, %g dB a coarse step", what, step);
    };
    coarse(Finalizer::INPUT, 2. * Finalizer::kInputDb, "Input");
    coarse(Finalizer::LOUD, Finalizer::kLoudDb, "Loud");
    CheckF(Range(cell[Finalizer::DRIVE], "([\\d.]+) to \\+([\\d.]+) dB", &lo, &hi) && hi == Finalizer::kDriveDb,
           "Finalizer: Drive to +%g dB", Finalizer::kDriveDb);
    CheckF(Range(cell[Finalizer::LOUD], "([\\d.]+) to \\+([\\d.]+) dB", &lo, &hi) && hi == Finalizer::kLoudDb,
           "Finalizer: Loud to +%g dB", Finalizer::kLoudDb);
    Group eq("Finalizer: EQ's bands, -20 dB to +8 dB (Finalizer.h)");
    for (size_t b = 0; b < 3; b++)
        eq.Add(Range(cell[Finalizer::EQ_LOW + b], "(-[\\d.]+) dB to \\+([\\d.]+) dB", &lo, &hi)
                   && lo == -Finalizer::kEqCutDb && hi == Finalizer::kEqBoostDb,
               "band %zu", b + 1);
    CheckF(Range(cell[Finalizer::DEPTH], "([\\d.]+) dB down to (-[\\d.]+) dB", &lo, &hi) && hi == -Finalizer::kDepthDb,
           "Finalizer: Depth down to -%g dB", Finalizer::kDepthDb);
    Group width("Finalizer: Width up to 1.5x for the low band, 2x the others (Finalizer.h)");
    for (size_t b = 0; b < 3; b++)
    {
        std::regex_search(cell[Finalizer::WIDTH_LOW + b], m, std::regex("up to ([\\d.]+)x"));
        width.Add(!m.empty() && atof(m[1].str().c_str()) == (b ? Finalizer::kMaxWidth : Finalizer::kMaxLowWidth),
                  "band %zu", b + 1);
    }
    std::string text;
    for (const auto& l : lines)
        text += l + " ";
    std::regex_search(text, m, std::regex("split at (\\d+) Hz and (\\d+) kHz"));
    CheckF(!m.empty() && atof(m[1].str().c_str()) == Finalizer::kSplitLow && atof(m[2].str().c_str()) * 1000. == Finalizer::kSplitHigh,
           "Finalizer: the bands split at %g Hz and %g Hz", Finalizer::kSplitLow, Finalizer::kSplitHigh);

    // its LEDs: the key in the module's colour, the knobs light blue to orange
    std::string leds = Strip(Bullet(Under(lines, "### Finalizer LEDs and settings"), "LEDs:"), kParens);
    std::regex_search(leds, m, std::regex("the key is ([a-z ,]+?)[;.]"));
    const Strings said_key = m.empty() ? Strings() : ColourWords(m[1]);
    CheckF(said_key.size() == 4 && said_key[0] == fin_colour[0] && said_key[1] == fin_colour[1]
               && said_key[2] == fin_colour[2] && said_key[3] == fin_colour[3],
           "Finalizer: the key is %s, %s, %s or %s, by its module (FxSlots.h)", fin_colour[0].c_str(),
           fin_colour[1].c_str(), fin_colour[2].c_str(), fin_colour[3].c_str());
    const std::string pair = comp_knob[0] + " to " + comp_knob[2];
    CheckF(leds.find(pair) != std::string::npos, "Finalizer: the knobs go %s (FxSlots.h)", pair.c_str());
}

// ---- MIDI

static std::map<std::string, int> midimap;

static int Midi(const char* name)
{
    if (!midimap.count(name))
    {
        CheckF(false, "MidiControl.h: midimap has %s", name);
        return -1;
    }
    return midimap[name];
}

static void ReadMidi()
{
    const std::string text = Source("MidiControl.h");
    const size_t a = text.find("namespace midimap"), b = text.find("enum Value", a);
    const std::string block = a == std::string::npos ? "" : text.substr(a, b - a);
    static const std::regex re("\\b(k\\w+) = (\\d+)");
    for (std::sregex_iterator it(block.begin(), block.end(), re), end; it != end; ++it)
        midimap[(*it)[1]] = atoi((*it)[2].str().c_str());
    CheckF(midimap.size() >= 20, "MidiControl.h: midimap read (%zu constants)", midimap.size());
}

/** A table's first column, numbers and ranges ("14, 15", "24-31"), to its row's text */
static std::map<int, std::string> NumberRows(const Table& t)
{
    std::map<int, std::string> out;
    static const std::regex re("(\\d+)(?:-(\\d+))?");
    for (const auto& row : t.rows)
    {
        const std::string& first = row[0];
        for (std::sregex_iterator it(first.begin(), first.end(), re), end; it != end; ++it)
        {
            const int lo = atoi((*it)[1].str().c_str());
            const int hi = (*it)[2].matched ? atoi((*it)[2].str().c_str()) : lo;
            for (int n = lo; n <= hi; n++)
                out[n] = Lower(row.size() > 1 ? row[1] : "");
        }
    }
    return out;
}

struct Expect
{
    int from, count;
    const char* word; // in its row, lower case
};

static void CheckRows(const char* what, const std::map<int, std::string>& rows,
                      const std::vector<Expect>& expect)
{
    std::set<int> known;
    Group g(std::string(what) + ", each in its row and none else (MidiControl.h)");
    for (const auto& e : expect)
        for (int n = e.from; n < e.from + e.count; n++)
        {
            known.insert(n);
            const auto it = rows.find(n);
            g.Add(it != rows.end() && it->second.find(e.word) != std::string::npos, "%d \"%s\" %s", n, e.word,
                  it == rows.end() ? "missing" : "not in its row");
        }
    for (const auto& r : rows)
        g.Add(known.count(r.first), "%d isn't one", r.first);
}

static std::set<int> used_ccs;

static void CheckMidi()
{
    const Strings appendix = Under(manual, "## Appendix: MIDI");

    // the notes
    const Strings notes = Under(appendix, "### Notes: the keys");
    std::string text;
    for (const auto& l : notes)
        text += l + " ";
    std::smatch m;
    std::regex_search(text, m, std::regex("from (?:\\*\\*)?note (\\d+)"));
    CheckF(!m.empty() && atoi(m[1].str().c_str()) == Midi("kBaseNote"),
           "MIDI: the keys from note %d (MidiControl.h)", Midi("kBaseNote"));
    const int knob = Midi("kKnobNote"), chompi = Midi("kChompiNote");
    CheckF(Midi("kNumKnobs") == 6, "MIDI: six knob notes, knobs 1-4, the transport, VOLUME");
    CheckRows("MIDI's notes", NumberRows(FindTable(notes, "| Note | Key")),
              {{knob, 4, "knob"}, {knob + 4, 1, "transport"}, {knob + 5, 1, "volume"},
               {chompi, 1, "chompi"}, {chompi + 1, 1, "play"}, {chompi + 2, 1, "loop"}});

    // the effect channels, by their keys
    const std::string channels = Bullet(Under(appendix, "### Channels"), "The effect channels");
    const size_t from = channels.find("left to right");
    CheckF(from != std::string::npos, "MIDI: the effect channels, \"left to right: 1 freezer, ...\"");
    const std::string list = from == std::string::npos ? "" : channels.substr(from + 13, channels.find('.', from) - from - 13);
    static const std::regex entry("(\\d+) ([a-z][a-z &]*[a-z])");
    std::set<int> seen;
    Group order("MIDI: the effect channels in the keys' order (FxSlots.h)");
    for (std::sregex_iterator it(list.begin(), list.end(), entry), end; it != end; ++it)
    {
        const int ch = atoi((*it)[1].str().c_str());
        const std::string name = (*it)[2];
        int key = name == Lower(kFinChapter) ? comp_key : 0;
        for (size_t fx = 0; fx < kNumFx; fx++)
            if (name == Lower(kChapters[fx]))
                key = slots[fx].key;
        order.Add(key && WhiteOf(key) == ch, "%d the %s, on white key %d", ch, name.c_str(), WhiteOf(key));
        seen.insert(ch);
    }
    CheckF(static_cast<int>(seen.size()) == Midi("kBlockChannels"), "MIDI: %d effect channels listed",
           Midi("kBlockChannels"));

    // the CCs
    const Strings ccs = Under(appendix, "### Controllers");
    const int knob_cc = Midi("kKnobCC"), params = Midi("kKeyParams");
    const Table own = FindTable(ccs, "| CC | Function", 0), global = FindTable(ccs, "| CC | Function", 1);
    CheckRows("MIDI's effect channels' CCs", NumberRows(own),
              {{knob_cc, 4, "page 1"}, {knob_cc + 4, 4, "page 2"}, {Midi("kLatchCC"), 1, "latch"}});
    const int key_latch = Midi("kKeyLatchCC");
    const std::vector<Expect> global_ccs = {
        {knob_cc, params + 1, "in focus"},
        {Midi("kFocusCC"), 1, "focus"},
        {Midi("kFinGlobalCC"), 4, "finalizer's level"},
        {Midi("kFinGlobalCC") + 4, 1, "finalizer's depth"},
        {Midi("kSourceCC"), 1, "source"},
        {Midi("kOutGainCC"), 1, "output gain"},
        {Midi("kInGainCC"), 1, "input gain"},
        {Midi("kMixCC"), 1, "input/loop mix"},
        {Midi("kHpCueCC"), 1, "headphone"},
        {Midi("kSpeedCC"), 1, "speed"},
        {Midi("kMorphCC"), 1, "morph"},
        {Midi("kMorphBarsCC"), 1, "bars"},
        {Midi("kBankCC"), 1, "program changes"},
        {Midi("kLoopTimingCC"), 1, "loop's program change"},
        {Midi("kFaderCC"), 1, "crossfader"},
        {Midi("kStopMorphCC"), 1, "stops"},
        {Midi("kTapCC"), 1, "tap"},
        {Midi("kDropCC"), 1, "drop"},
        {key_latch, Midi("kBlockChannels") - 1, "latch"},
        {Midi("kDryCC"), 1, "effects"},
        {Midi("kLiveCC"), 1, "monitoring"},
        {Midi("kAllSoundOffCC"), 1, "all sound off"},
        {Midi("kAllNotesOffCC"), 1, "all notes off"},
    };
    const std::map<int, std::string> global_rows = NumberRows(global);
    CheckRows("MIDI's global CCs", global_rows, global_ccs);
    // the program changes by CC 0's bank (#259)
    const int cubbi = Midi("kCubbiProgramBank"), banks = Midi("kLoopBanks");
    CheckRows("MIDI's program change banks",
              NumberRows(FindTable(Under(appendix, "### Program changes"), "| CC 0 | Program change", 0)),
              {{0, 1, "scene"}, {1, cubbi - 1, "loop"}, {cubbi, banks - cubbi + 1, "cubbi"},
               {banks + 1, 127 - banks, "nothing"}});
    for (const auto& e : global_ccs)
        for (int n = e.from; n < e.from + e.count; n++)
            used_ccs.insert(n);

    // the finalizer's channel (#345): its parameters at fixed CCs, its preset, on, module, mutes
    const int fin_high = Midi("kFinHighCC"), fin_mute = Midi("kFinMuteCC");
    CheckRows("MIDI's finalizer channel", NumberRows(FindTable(ccs, "| CC | Function", 2)),
              {{knob_cc, 4, "level"}, {knob_cc + 4, 3, "eq"}, {Midi("kFinPresetCC"), 1, "preset"},
               {Midi("kFinOnCC"), 1, "bypassed"}, {Midi("kFinModuleCC"), 1, "module"},
               {fin_high, 4, "comp"}, {fin_high + 4, 3, "width"}, {fin_mute, 3, "mute"}});
    CheckF(Midi("kFinPresetCC") == knob_cc + 7 && Midi("kFinOnCC") == Midi("kLatchCC") && Midi("kFinModuleCC") == Midi("kFocusCC"),
           "MIDI: the finalizer's preset CC after EQ's, its on or off the latch's CC, its module the focus's");
    // a morph's bars
    const auto bars = global_rows.find(Midi("kMorphBarsCC"));
    CheckF(bars != global_rows.end() && std::regex_search(bars->second, m, std::regex("(\\d+)-(\\d+)"))
               && atoi(m[2].str().c_str()) == Midi("kMorphBarsMax"),
           "MIDI: a morph over 1-%d bars", Midi("kMorphBarsMax"));
    // the latches by key
    const auto latches = global_rows.find(key_latch);
    const std::string latch_text = latches == global_rows.end() ? "" : latches->second;
    int named = 0;
    static const std::regex latch_entry("(\\d+) the ([a-z][a-z &]*[a-z])");
    for (std::sregex_iterator it(latch_text.begin(), latch_text.end(), latch_entry), end; it != end; ++it)
    {
        const std::string name = (*it)[2];
        int key = 0;
        for (size_t fx = 0; fx < kNumFx; fx++)
            if (name == Lower(kChapters[fx]) || name == kFxWords[fx])
                key = slots[fx].key;
        const int cc = atoi((*it)[1].str().c_str());
        CheckF(key && cc == key_latch + WhiteOf(key) - 1, "MIDI: CC %d the %s's latch, white key %d", cc,
               name.c_str(), WhiteOf(key));
        named++;
    }
    CheckF(named >= 2, "MIDI: the latch CCs name their effects (%d)", named);

    // the CCs left free
    const std::string free = Bullet(ccs, "Left free on purpose");
    CheckF(!free.empty(), "MIDI: a bullet \"Left free on purpose ...\"");
    for (int n = knob_cc; n < knob_cc + params + 1; n++)
        used_ccs.insert(n);
    static const std::regex number("\\b(\\d+)(?:-(\\d+))?\\b(?!-bit)");
    Group unused("MIDI: the CCs left free, none of them used (MidiControl.h)");
    for (std::sregex_iterator it(free.begin(), free.end(), number), end; it != end; ++it)
    {
        const int lo = atoi((*it)[1].str().c_str());
        const int hi = (*it)[2].matched ? atoi((*it)[2].str().c_str()) : lo;
        for (int n = lo; n <= hi; n++)
            unused.Add(!used_ccs.count(n), "%d is used", n);
    }
}

// ---- the settings page

static void CheckSettings()
{
    const std::string text = Source("SettingsPage.h");
    std::map<std::string, int> key_of; // its names for keys, to KEY_N
    static const std::regex named("(k\\w+) = Hardware::SwId::KEY_(\\d+);");
    for (std::sregex_iterator it(text.begin(), text.end(), named), end; it != end; ++it)
        key_of[(*it)[1]] = atoi((*it)[2].str().c_str());
    auto array = [&](const char* name) {
        Strings colours;
        std::smatch m;
        if (std::regex_search(text, m, std::regex(std::string(name) + "\\[\\w*\\] = \\{([^}]*)\\}")))
            for (const auto& w : Words(m[1]))
                colours.push_back(ColourName(w));
        return colours;
    };
    const Strings factor = array("kFactorColors"), source = array("kSourceColors");
    const Strings input = array("kInSourceColors"); // the upper F#'s, by the input's source

    // each key's colours, as Draw lights them: KeyLed(KEY, COLOUR, ...) and
    // SmtLed(KeyLedOf(KEY), COLOUR, ...), COLOUR a name, a choice of two or a colour array
    std::map<int, std::set<std::string>> code;
    static const std::regex led("(?:KeyLed\\(|SmtLed\\(KeyLedOf\\()([\\w:\\[\\]]+)\\)?, ([^,]+),");
    for (std::sregex_iterator it(text.begin(), text.end(), led), end; it != end; ++it)
    {
        std::vector<int> keys;
        const std::string k = (*it)[1], colour = (*it)[2];
        if (k == "kWhiteKeys[ch]")
            for (int w = 0; w < 14; w++)
                keys.push_back(white_keys[w]);
        else if (k.compare(0, 20, "Hardware::SwId::KEY_") == 0)
            keys.push_back(atoi(k.c_str() + 20));
        else if (key_of.count(k))
            keys.push_back(key_of[k]);
        else
            continue;
        Strings names;
        if (colour.find("kFactorColors") != std::string::npos)
            names = factor;
        else if (colour.find("kSourceColors") != std::string::npos)
            names = source;
        else if (colour.find("kInSourceColors") != std::string::npos)
            names = input;
        else
            for (const auto& w : Words(colour))
                if (!ColourName(w).empty())
                    names.push_back(ColourName(w));
        for (int key : keys)
            code[key].insert(names.begin(), names.end());
    }
    CheckF(code.size() >= 20, "SettingsPage.h: the keys' colours read (%zu keys)", code.size());

    // the manual's: every colour its rows name for a key, the setting's text too (a red blink)
    const Table t = FindTable(Under(manual, "## Settings page"), "| Control | Setting | Colour");
    CheckF(!t.rows.empty(), "Settings page: its table");
    std::map<int, std::set<std::string>> said;
    static const char* const kDark[] = {"lower c#", "lower d#", "lower f#", "lower g#", "lower a#",
                                        "upper c#", "upper d#", "upper f#", "upper g#", "upper a#"};
    std::smatch m;
    for (const auto& row : t.rows)
    {
        const std::string control = Lower(row[0]);
        std::vector<int> keys;
        if (std::regex_search(control, m, std::regex("white keys (\\d+) to (\\d+)")))
            for (int w = atoi(m[1].str().c_str()); w <= atoi(m[2].str().c_str()); w++)
                keys.push_back(white_keys[w - 1]);
        else if (std::regex_search(control, m, std::regex("white key (\\d+)")))
            keys.push_back(white_keys[atoi(m[1].str().c_str()) - 1]);
        else
            for (int d = 0; d < 10; d++)
                if (control.find(kDark[d]) != std::string::npos)
                    keys.push_back(dark_keys[d]);
        if (keys.empty())
            continue; // knob 1
        const Strings words = ColourWords(row[1] + " " + row[2]);
        for (int key : keys)
            said[key].insert(words.begin(), words.end());

        // a colour per state: "yellow (as sent), red (double), light blue (half)"
        const Strings* by_state = nullptr;
        std::map<std::string, std::string> states;
        if (keys.size() == 1 && keys[0] == key_of["kFactorKey"])
            by_state = &factor, states = {{"half", "HALF"}, {"as sent", "ONE"}, {"double", "DOUBLE"}};
        if (keys.size() == 1 && keys[0] == key_of["kSourceKey"])
            by_state = &source, states = {{"auto", "AUTO"}, {"trs", "TRS"}, {"usb", "USB"}, {"internal", "INTERNAL"}};
        if (!by_state)
            continue;
        const Strings order = EnumNames(Source("MidiClock.h"), by_state == &factor ? "enum class ClockFactor" : "enum class ClockSource");
        const std::string cell = Lower(row[2]);
        static const std::regex pair("(light blue|[a-z]+) \\(([^)]*)\\)");
        size_t n = 0;
        for (std::sregex_iterator it(cell.begin(), cell.end(), pair), end; it != end; ++it, n++)
        {
            const std::string state = states.count((*it)[2]) ? states[(*it)[2]] : "?";
            size_t i = 0;
            while (i < order.size() && order[i] != state)
                i++;
            CheckF(i < by_state->size() && (*by_state)[i] == (*it)[1].str(),
                   "Settings page: %s %s (SettingsPage.h)", row[0].c_str(), (*it).str().c_str());
        }
        CheckF(n == by_state->size(), "Settings page: %s, a colour for each of its %zu states", row[0].c_str(),
               by_state->size());
    }
    Group keys("Settings page: each key's colours, as SettingsPage.h lights it");
    for (const auto& k : code)
    {
        std::string a, b;
        for (const auto& c : k.second)
            a += (a.empty() ? "" : ", ") + c;
        for (const auto& c : said[k.first])
            b += (b.empty() ? "" : ", ") + c;
        const int w = WhiteOf(k.first);
        int d = 0;
        while (d < 10 && dark_keys[d] != k.first)
            d++;
        keys.Add(k.second == said[k.first], "%s%s %s, the manual says %s", w ? "white key " : d < 10 ? kDark[d] : "KEY",
                 w ? std::to_string(w).c_str() : "", a.c_str(), b.empty() ? "nothing" : b.c_str());
    }
    for (const auto& k : said)
        keys.Add(code.count(k.first), "KEY_%d isn't lit by SettingsPage.h", k.first);
}

int main()
{
    const char* path = getenv("DOCS_MANUAL");
    manual = Lines(ReadFile(path ? path : Repo() + "/MANUAL.md"));
    ReadKeys();
    ReadMidi();

    for (size_t fx = 0; fx < kNumFx; fx++)
        CheckChapter(fx);
    CheckFinalizer();
    for (size_t fx = 0; fx < kNumFx; fx++)
        for (size_t q = 0; q < kNumFxParams; q++)
            if (Stepped(fx, q))
                CheckSteps(fx, q);
    CheckSteppedBullet();
    CheckCoarse();
    CheckMidi();
    CheckSettings();
    return Finish();
}
