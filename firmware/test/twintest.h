// twintest.h: what the checks on the virtual CHOMPI share (ui.cpp, remote.cpp, timing.h's).
// Tap() runs the including file's RunMs(ms), so it is included after that.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include "twin.h"

// boot, the rainbow, and the play page ready
static const uint32_t kReadyMs = 6000;

/** A file on the twin's card, "" if there is none */
__attribute__((unused)) static std::string Card(const char* path)
{
    auto it = twin::CardFiles().find(path);
    return it == twin::CardFiles().end() ? "" : it->second;
}

/** An LED's colour as six hex digits, as remote.py and the scripts write it */
__attribute__((unused)) static std::string Hex(const twin::Rgb& c)
{
    char s[8];
    snprintf(s, sizeof(s), "%02x%02x%02x", c.r, c.g, c.b);
    return s;
}

/** A key pressed for ms, then let go */
__attribute__((unused)) static void Tap(const char* key, uint32_t ms = 60)
{
    twin::Press(key, true);
    RunMs(ms);
    twin::Press(key, false);
}
