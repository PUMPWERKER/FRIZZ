#pragma GCC push_options
#pragma GCC optimize ("Os")
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include "Clock.h"

namespace chompi
{
    // audio thread; the setters are called from the UI
    class Arp
    {
      public:
        static constexpr int kNotes = 7, kFree = kNotes;
        static constexpr int kPatterns = 8;

        void Init()
        {
            note_ = seen_note_ = -1;
            slow_ = 0.f;
            pattern_ = 0;
            octaves_ = 1;
            latch_ = latched_ = clear_ = false;
            first_ = recording_ = grid_ = false;
            count_ = drops_ = 0;
            playing_ = -1;
            start_ = false;
            next_ = 0;
            tick_ = -1;
            steps_ = origin_ = since_ = 0.f;
            done_ = 0;
            seed_ = 1;
        }

        // -1 off, a note value, or kFree
        void SetNote(int note)
        {
            note_ = note < -1 ? -1 : note > kFree ? kFree : note;
            slow_ = 0.f;
        }
        int Note() const { return note_; }
        bool On() const { return note_ >= 0; }
        // free time: 0 is four times as fast as 1/32, 1 is 2 s
        void SetSlow(float slow) { slow_ = slow < 0.f ? 0.f : slow > 1.f ? 1.f : slow; }
        float Slow() const { return slow_; }
        void SetPattern(int pattern) { pattern_ = pattern < 0 ? 0 : pattern >= kPatterns ? kPatterns - 1 : pattern; }
        int Pattern() const { return pattern_; }
        void SetOctaves(int octaves) { octaves_ = octaves < 1 ? 1 : octaves > 3 ? 3 : octaves; }
        int Octaves() const { return octaves_; }
        void SetLatch(bool latch) { latch_ = latch; }
        bool Latched() const { return latch_; }
        void Clear() { clear_ = true; }
        int Playing() const { return playing_; }

        static float NoteQuarters(int note)
        {
            static const float q[kNotes] = {1.f, .75f, .5f, .375f, .25f, .1875f, .125f};
            return q[note];
        }

        void Hold(int key, float nn, float vel)
        {
            const bool running = count_ > 0;
            if(latched_ && !AnyDown())
                DropAll();
            Undrop(key);
            int i = Find(key);
            if(i < 0)
            {
                if(count_ == kMaxKeys)
                    return;
                i = count_++;
            }
            keys_[i] = {int8_t(key), nn, vel, true};
            if(!running)
                start_ = true;
        }

        bool Release(int key)
        {
            const int i = Find(key);
            if(i < 0)
                return false;
            keys_[i].down = false;
            if(!latched_)
                Remove(i);
            return true;
        }

        int Drop() { return drops_ ? drop_[--drops_] : -1; }

        // quarter: tempo without a grid; first, recording: looper state
        void Step(size_t size, bool allowed, float quarter, bool first, bool recording, int &stop, int &key, float &nn,
                  float &vel)
        {
            stop = key = -1;
            Housekeeping(allowed, first, recording);
            if(!On() || !count_)
            {
                stop = playing_;
                playing_ = -1;
                start_ = false;
                tick_ = -1;
                return;
            }

            bool accent = false;
            const int32_t step = Due(size, quarter, accent);
            if(step < 0 || !(Patterns(pattern_).sounds >> (step & 7) & 1))
                return;

            float octave;
            const Key &k = Next(octave);
            stop = playing_;
            key = playing_ = k.key;
            nn = k.nn + 12.f * octave;
            vel = accent ? k.vel : .8f * k.vel;
        }

      private:
        static constexpr int kMaxKeys = 8;
        static constexpr float kLate = .24f; // swing at 62%
        enum Order { kUp, kDown, kUpDown, kPlayed, kRandom, kBounce };
        struct Shape
        {
            uint8_t order;
            uint8_t sounds; // of eight steps
            bool swing;
        };
        static const Shape &Patterns(int i)
        {
            static const Shape table[kPatterns] = {
                {kUp, 0xff, false},     {kDown, 0xff, false},   {kUpDown, 0xff, false}, {kPlayed, 0xff, false},
                {kRandom, 0xff, false}, {kBounce, 0xff, false}, {kUp, 0x49, false}, // tresillo
                {kUp, 0xff, true},
            };
            return table[i];
        }

        struct Key
        {
            int8_t key;
            float nn, vel;
            bool down;
        };

        void Housekeeping(bool allowed, bool first, bool recording)
        {
            if((first_ && !first) || (recording_ && !recording))
                latch_ = false;
            first_ = first;
            recording_ = recording;
            if(latch_ != latched_)
            {
                latched_ = latch_;
                for(int i = count_ - 1; i >= 0 && !latched_; i--)
                    if(!keys_[i].down)
                        Remove(i);
            }
            if(clear_ || !allowed)
            {
                clear_ = false;
                DropAll();
            }
        }

        int32_t Due(size_t size, float quarter, bool &accent)
        {
            uint32_t beat;
            float phase, length;
            const bool free = note_ >= kFree;
            const bool grid = beat_clock.Get(beat, phase, &length) && !free;
            if(grid)
                quarter = length / beat_clock.QuartersPerBeat();
            const float fast = quarter * NoteQuarters(kNotes - 1) / 4.f;
            const float samples = free ? fast * powf(2.f * beat_clock.SampleRate() / fast, slow_) : quarter * NoteQuarters(note_);
            const float sixteenths = samples / quarter * 4.f;
            const float pos = grid ? (float(beat) + phase) * beat_clock.QuartersPerBeat() * 4.f : 0.f;

            if(start_ || grid != grid_)
            {
                grid_ = grid;
                origin_ = floorf(pos) + 1.f;
                steps_ = 0.f;
                tick_ = -1;
                if(start_)
                {
                    since_ = samples;
                    next_ = 0;
                    done_ = 0;
                }
                start_ = false;
            }
            else if(note_ != seen_note_)
            {
                tick_ = grid ? int32_t(fmaxf(pos - origin_, 0.f) / sixteenths) : 0;
                steps_ = 0.f;
            }
            seen_note_ = note_;

            since_ += size;
            float p;
            if(grid)
            {
                if(pos < origin_)
                    return -1;
                p = (pos - origin_) / sixteenths;
            }
            else
            {
                p = steps_;
                steps_ += size / samples;
            }
            int32_t t = int32_t(p);
            if(Patterns(pattern_).swing)
                t = t / 2 * 2 + (p - t / 2 * 2 >= 1.f + kLate);

            if(t <= tick_)
                return -1;
            tick_ = t;
            if(since_ < .5f * samples)
                return -1;
            since_ = 0.f;
            const float beats = (origin_ + t * sixteenths) / (4.f * beat_clock.QuartersPerBeat());
            accent = grid && fabsf(beats - roundf(beats)) < .01f;
            return t;
        }

        const Key &Next(float &octave)
        {
            const uint8_t order = Patterns(pattern_).order;
            const int span = count_ * octaves_;
            const int cycle = order == kBounce ? 2 * span : order == kUpDown && span > 1 ? 2 * span - 2 : span;
            if(next_ >= cycle)
                next_ = 0;
            const int n = next_++;
            int j = n;
            if(order == kDown)
                j = span - 1 - n;
            else if(order == kUpDown && n >= span)
                j = cycle - n;
            else if(order == kBounce)
                j = n / 2;
            else if(order == kRandom)
            {
                if(done_ >= (1u << span) - 1)
                    done_ = 0;
                seed_ = seed_ * 1664525u + 1013904223u;
                j = (seed_ >> 16) % span;
                while(done_ >> j & 1)
                    j = (j + 1) % span;
                done_ |= 1u << j;
            }
            octave = float(j / count_ + (order == kBounce ? n % 2 : 0));
            return order == kPlayed ? keys_[j % count_] : Lowest(j % count_);
        }

        // keys_ is in press order
        const Key &Lowest(int i) const
        {
            for(int k = 0; k < count_; k++)
            {
                int lower = 0;
                for(int m = 0; m < count_; m++)
                    lower += keys_[m].nn < keys_[k].nn || (keys_[m].nn == keys_[k].nn && m < k);
                if(lower == i)
                    return keys_[k];
            }
            return keys_[0];
        }

        int Find(int key) const
        {
            for(int i = 0; i < count_; i++)
                if(keys_[i].key == key)
                    return i;
            return -1;
        }

        // the playing note is stopped by Step, not dropped
        void Remove(int i)
        {
            if(keys_[i].key != playing_ && drops_ < kMaxKeys)
                drop_[drops_++] = keys_[i].key;
            for(count_--; i < count_; i++)
                keys_[i] = keys_[i + 1];
        }

        void DropAll()
        {
            while(count_)
                Remove(count_ - 1);
        }

        void Undrop(int key)
        {
            for(int i = drops_ - 1; i >= 0; i--)
                if(drop_[i] == key)
                    drop_[i] = drop_[--drops_];
        }

        bool AnyDown() const
        {
            for(int i = 0; i < count_; i++)
                if(keys_[i].down)
                    return true;
            return false;
        }

        volatile int8_t note_, pattern_, octaves_;
        volatile float slow_;
        volatile bool latch_, clear_;

        Key keys_[kMaxKeys];
        int8_t drop_[kMaxKeys];
        int8_t count_, drops_, playing_;
        bool latched_, start_, first_, recording_;

        int8_t seen_note_;
        bool grid_;
        float origin_; // sixteenths
        float steps_;
        float since_;  // samples
        int32_t tick_;

        int8_t next_;
        uint32_t done_;
        uint32_t seed_;
    };

    extern Arp arp;
}

#pragma GCC pop_options
