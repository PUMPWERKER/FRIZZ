/** @file MicFilter.h
 *  @brief The built-in microphone's filter, TAPE's (TEMPO's is the same) unchanged: a
 *  highpass at 150 Hz and narrow notches at 2, 4, 8, 9 and 12 kHz, as TAPE tuned them for the
 *  CHOMPI's mic. One channel; PassthroughEngine runs it only while the mic is the input.
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

        br1.Init(sr);
        br1.SetFreq(2000.f);
        br1.SetRes(.99f);
        br1.SetDrive(.6f);

        br2.Init(sr);
        br2.SetFreq(12000.f);
        br2.SetRes(.99f);

        br3.Init(sr);
        br3.SetFreq(8000.f);
        br3.SetRes(.99f);

        br4.Init(sr);
        br4.SetFreq(4000.f);
        br4.SetRes(.99f);
        br4.SetDrive(.6f);

        br5.Init(sr);
        br5.SetFreq(9000.f);
        br5.SetRes(.99f);
    }

    float Process(float in)
    {
        in = daisysp::fclamp(in, -.98f, .98f);

        hp.Process(in);
        in = hp.High();

        br1.Process(in);
        in = br1.Notch();

        br2.Process(in);
        in = br2.Notch();

        br3.Process(in);
        in = br3.Notch();

        br4.Process(in);
        in = br4.Notch();

        br5.Process(in);
        in = br5.Notch();

        return in;
    }

private:
    daisysp::Svf hp, br1, br2, br3, br4, br5;
}; // MicFilter
} // namespace chompi
