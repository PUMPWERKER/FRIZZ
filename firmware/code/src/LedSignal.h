/** @file LedSignal.h
 *  @brief The pages' short LED signals, in one language: 3 fast blinks, red for refused
 *  or not stored and white for done, and a short flash (a tempo tap, an FX select). A signal
 *  ends by itself once its time is up, so it can't come back when the millisecond counter
 *  wraps.
 */
#pragma once
#include <stdint.h>

namespace chompi
{

static const uint32_t kSignalBlinkMs = 100;                 // half a blink
static const uint32_t kSignalBlinksMs = 6 * kSignalBlinkMs; // 3 blinks

/** A steady blink, lit for the first half_ms of every 2 * half_ms */
inline bool BlinkOn(uint32_t now, uint32_t half_ms)
{
    return (now / half_ms) % 2 == 0;
}

class LedSignal
{
public:
    /** Starts it, for length ms: kSignalBlinksMs for the 3 blinks */
    void Start(uint32_t now, uint32_t length = kSignalBlinksMs)
    {
        start_ = now;
        length_ = length;
        active_ = true;
    }

    void Stop() { active_ = false; }

    /** Whether it's still showing */
    bool Active(uint32_t now)
    {
        if (active_ && now - start_ >= length_)
            active_ = false;
        return active_;
    }

    /** While it shows: whether a blink is lit now */
    inline bool BlinkLit(uint32_t now) const { return ((now - start_) / kSignalBlinkMs) % 2 == 0; }
    /** And as an LED's level, full or dark: out of line, once for every LED that blinks */
    __attribute__((noinline)) float BlinkLevel(uint32_t now) const { return BlinkLit(now) ? 1.f : 0.f; }

private:
    uint32_t start_ = 0;
    uint32_t length_ = 0;
    bool active_ = false;
};

} // namespace chompi
