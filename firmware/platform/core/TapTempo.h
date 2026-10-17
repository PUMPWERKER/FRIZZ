/** @file TapTempo.h
 *  @brief Tap tempo from SHIFT + LOOP: the tempo from the last few taps' intervals. No
 *  hardware here, so it can be tested on the host; NormalPage.h feeds it the key presses and
 *  hands the tempo to the engine (TempoClock::Tap).
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace chompi
{

class TapTempo
{
public:
    static const size_t kMaxTaps = 5;          // the mean is over the last 4 intervals
    static const size_t kMinTaps = 3;          // 2 intervals before a tempo is given
    static const uint32_t kResetMs = 2000;     // a longer gap starts over (30 BPM)

    /** A tap at now_ms (System::GetNow()). True when it gives a tempo: Bpm() */
    bool Tap(uint32_t now_ms)
    {
        if (count_ > 0 && now_ms - taps_[count_ - 1] > kResetMs)
            count_ = 0;
        if (count_ == kMaxTaps)
        {
            for (size_t i = 1; i < kMaxTaps; i++)
                taps_[i - 1] = taps_[i];
            count_--;
        }
        taps_[count_++] = now_ms;
        return count_ >= kMinTaps;
    }

    /** The mean tempo of the taps, unclamped: TempoClock clamps it. 0 before kMinTaps */
    float Bpm() const
    {
        if (count_ < kMinTaps)
            return 0.f;
        const float mean_ms = static_cast<float>(taps_[count_ - 1] - taps_[0]) / (count_ - 1);
        return mean_ms > 0.f ? 60000.f / mean_ms : 0.f;
    }

private:
    uint32_t taps_[kMaxTaps];
    size_t count_ = 0;
};

} // namespace chompi
