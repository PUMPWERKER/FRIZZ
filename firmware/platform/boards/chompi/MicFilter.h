/** @file MicFilter.h
 *  @brief The built-in microphone's filter: TAPE's (TEMPO's is the same) highpass at 150 Hz,
 *  with its input clamp. TAPE follows it with five notches (2, 4, 8, 9 and 12 kHz, SVFs at
 *  resonance .99); they are some 20 Hz narrow and saturate, so they barely touch the sound
 *  but cost five sixths of the filter, and FRIZZ leaves them out (#127). One channel;
 *  PassthroughEngine runs it only while the mic is heard.
 */
#pragma once
#include "daisysp.h"

namespace chompi
{

class MicFilter
{
public:
    void Init(float sr)
    {
        hp.Init(sr);
        hp.SetFreq(150.f);
        hp.SetRes(.4f);
    }

    float Process(float in)
    {
        hp.Process(daisysp::fclamp(in, -.98f, .98f));
        return hp.High();
    }

private:
    daisysp::Svf hp;
}; // MicFilter
} // namespace chompi
