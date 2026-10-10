#pragma once

#include "daisy.h"
#include "util/scopedirqblocker.h"

namespace chompi
{
    // MIDI clock as a beat grid.
    // MIDI comes in the receive interrupts, at the audio callback's priority, so only lower priority code needs the lock.
    // The MIDI grid starts with Start or Song Position, joining a running clock would be off the beat.
    class Clock
    {
    public:
        static constexpr uint32_t kTicksPerBeat = 24;

        void Init()
        {
            source_ = nullptr;
            started_ = running_ = false;
            spp_ = spp_lsb_ = 0;
            count_ = outliers_ = 0;
            heard_ = last_ = t_ = period_ = 0;
        }

        // every received byte, before parsing
        void Byte(uint8_t byte, const void *source)
        {
            const bool data = byte < 0x80;
            if(data ? !spp_ || source != source_ : !IsTransport(byte))
            {
                if(!data && byte < 0xF8 && source == source_) // another message cuts a song position short
                    spp_ = 0;
                return;
            }

            // the first source to send clock keeps it until it goes quiet
            const uint32_t now = daisy::System::GetTick();
            const bool quiet = now - heard_ >= Lost();
            if(source != source_ && source_ && !quiet)
                return;
            source_ = source;
            heard_ = now;
            if(quiet) // the position is unknown after a gap
                started_ = running_ = false;

            if(data) // song position, in 16ths
            {
                if(spp_ == 1)
                {
                    spp_lsb_ = byte;
                    spp_ = 2;
                }
                else
                {
                    count_ = ((byte << 7) | spp_lsb_) * 6;
                    started_ = true;
                    spp_ = 0;
                }
                return;
            }

            switch(byte)
            {
                case 0xF8: Tick(now); break;
                case 0xFA: // start: the next tick is the downbeat
                    count_ = 0;
                    started_ = running_ = true;
                    break;
                case 0xFB: running_ = started_; break; // continue
                case 0xFC: running_ = false; break;    // stop
                case 0xF2: // song position, unknown until both data bytes are in
                    started_ = running_ = false;
                    spp_ = 1;
                    break;
            }
        }

        // beats since the start and the phase within the current one, false without a grid
        bool Get(uint32_t &beat, float &phase) const { return MidiGrid(beat, phase); }

    private:
        bool MidiGrid(uint32_t &beat, float &phase) const
        {
            daisy::ScopedIrqBlocker block;
            const int32_t since = daisy::System::GetTick() - t_;
            if(!running_ || !period_ || !count_ || since > int32_t(2 * period_))
                return false;

            // runs on past a late tick instead of waiting for it
            const uint32_t tick = count_ - 1;
            float into = tick % kTicksPerBeat + (since > 0 ? float(since) / period_ : 0.f);
            beat = tick / kTicksPerBeat;
            if(into >= kTicksPerBeat)
            {
                into -= kTicksPerBeat;
                beat++;
            }
            phase = into / kTicksPerBeat;
            return true;
        }

        static bool IsTransport(uint8_t byte)
        {
            return byte == 0xF8 || byte == 0xFA || byte == 0xFB || byte == 0xFC || byte == 0xF2;
        }

        static uint32_t Lost() { return daisy::System::GetTickFreq() / 4; } // slower than 10 BPM

        // times are TIM2 ticks, which wrap at 2^32 (GetUs doesn't); a small PLL keeps USB jitter off the grid
        void Tick(uint32_t now)
        {
            const uint32_t d = now - last_;
            last_ = now;
            if(running_)
                count_++;

            if(d >= Lost())
            {
                period_ = 0;
                t_ = now;
                return;
            }
            if(!period_)
            {
                period_ = d;
                t_ = now;
                outliers_ = 0;
                return;
            }

            const int32_t err = now - (t_ + period_);
            t_ += period_;
            if(err > int32_t(period_ / 2) || -err > int32_t(period_ / 2))
            {
                // bunched USB packets or a lost byte keep the grid, a real tempo jump resyncs
                if(++outliers_ >= 3)
                {
                    period_ = d;
                    t_ = now;
                    outliers_ = 0;
                }
                return;
            }

            outliers_ = 0;
            t_ += err / 4;
            period_ += err / 16;
        }

        const void *source_; // the handler the clock comes from
        bool started_;       // the count is the song position
        bool running_;       // between start / continue and stop
        uint8_t spp_;        // song position byte to come: 1 = LSB, 2 = MSB, 0 = none
        uint8_t spp_lsb_;
        uint32_t count_;     // ticks since start
        uint32_t outliers_;  // ticks in a row far off the grid
        uint32_t heard_;     // last byte from source_
        uint32_t last_;      // last tick, raw
        uint32_t t_;         // last tick, smoothed
        uint32_t period_;    // smoothed tick length, 0 while unknown
    };

    extern Clock beat_clock;
}
