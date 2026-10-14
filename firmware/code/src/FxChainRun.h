/** @file FxChainRun.h
 *  @brief FxChain::Run, the effects' per-sample code (FxChain.h): in the firmware a
 *  translation unit of its own, FxChain.cpp (#286), so its machine code follows only its own
 *  sources and flags. Compiled with the rest of chompi_main.cpp, GCC's loop transformations
 *  and register allocation in Run changed with code elsewhere that the audio never runs
 *  (#285, #298), and the build players run with it. Run calls nothing in chompi_main.cpp: the
 *  engine calls it once per run of up to kFxMaxBlock samples (FxChain::Process), the one call
 *  across. The host checks that include FxChain.h alone get it from there, inline.
 */
#pragma once
#include "FxChain.h"

namespace chompi
{

FX_CHAIN_RUN void FxChain::Run(float* l, float* r, size_t n)
{
    Insert(FX_FREEZER, freezer_, l, r, n);
    // the resonator's loop wraps everything from here to the phaser: its Feeds, the
    // inserts in it, then its Taps (Resonator::Feed)
    uint32_t res_idle = 0; // bit i: idle after Feed i, for what goes into the loop then
    for (size_t i = 0; i < n; i++)
    {
        resonator_.Feed(&l[i], &r[i], i);
        if (resonator_.Idle())
            res_idle |= 1u << i;
        else
            Meter(FX_RESONATOR, resonator_.Return());
    }
    BENCH_MARK_FX(FX_RESONATOR);
    Insert(FX_SHIFTER, shifter_, l, r, n);
    Insert(FX_FOLDER, folder_, l, r, n);
    Insert(FX_CRUSHER, crusher_, l, r, n);
    Insert(FX_FILTER, filter_, l, r, n);
    Insert(FX_FLANGER, flanger_, l, r, n);
    Insert(FX_PHASER, phaser_, l, r, n);
    bool res_band = OutBusy(FX_RESONATOR);
    for (size_t i = 0; i < n; i++)
    {
        float tl = l[i], tr = r[i];
        SendBand(FX_RESONATOR, res_band, res_idle >> i & 1, &tl, &tr);
        resonator_.Tap(tl, tr, i);
    }
    BENCH_MARK_FX(FX_RESONATOR);
    Insert(FX_SLICER, slicer_, l, r, n);
    Insert(FX_WARBLE, warble_, l, r, n);
    Insert(FX_TAPESTOP, tapestop_, l, r, n);

    // sends: the delay from the inserts' output, the reverb from that plus the delay's
    // return, so the echoes are reverberated. Both returns are added on top.
    float delayl[kFxMaxBlock], delayr[kFxMaxBlock];
    bool delay_band = OutBusy(FX_DELAY);
    for (size_t i = 0; i < n; i++)
    {
        const float sendl = l[i], sendr = r[i];
        float inl = sendl, inr = sendr; // what goes in is faded with the key
        SendBand(FX_DELAY, delay_band, delay_.Idle(), &inl, &inr);
        float outl = sendl, outr = sendr; // the return's added to it
        delay_.Process(inl, inr, &outl, &outr);
        delayl[i] = outl;
        delayr[i] = outr;
        if (!delay_.Sleeping())
            Meter(FX_DELAY, outl - sendl + outr - sendr);
    }
    BENCH_MARK_FX(FX_DELAY);
    bool reverb_band = OutBusy(FX_REVERB);
    for (size_t i = 0; i < n; i++)
    {
        float inl = delayl[i], inr = delayr[i];
        SendBand(FX_REVERB, reverb_band, reverb_.Idle(), &inl, &inr);
        float outl = delayl[i], outr = delayr[i];
        reverb_.Process(inl, inr, &outl, &outr);
        l[i] = outl;
        r[i] = outr;
        if (!reverb_.Sleeping())
            Meter(FX_REVERB, outl - delayl[i] + outr - delayr[i]);
    }
    BENCH_MARK_FX(FX_REVERB);
}

} // namespace chompi
