#pragma once

#include <stdint.h>

namespace chompi
{
    // Frames [begin, end) of a sample in the order they play: backwards in reverse, from the top again when it loops.
    struct Span
    {
        uint32_t begin = 0, end = 0;
        bool reverse = false, loop = true;

        // start and end: fractions of the sample's frames
        static Span Of(float start, float end, uint32_t frames, bool reverse, bool loop)
        {
            Span s;
            s.begin = static_cast<uint32_t>(double(start) * frames); // in double, a point stays on its frame
            s.end = static_cast<uint32_t>(double(end) * frames);
            if(s.end > frames)
                s.end = frames;
            if(s.begin >= s.end)
                s.begin = s.end ? s.end - 1 : 0;
            s.reverse = reverse;
            s.loop = loop;
            return s;
        }

        bool operator==(const Span &o) const { return begin == o.begin && end == o.end && reverse == o.reverse && loop == o.loop; }
        bool Empty() const { return begin >= end; }
        uint32_t First() const { return reverse ? end - 1 : begin; }
        bool Contains(uint32_t pos) const { return pos >= begin && pos < end; }

        // the frame after pos; false past the end when it doesn't loop
        bool Next(uint32_t &pos) const
        {
            if(!reverse)
            {
                if(++pos < end)
                    return true;
                pos = begin;
                return loop;
            }
            if(pos > begin)
            {
                pos--;
                return true;
            }
            pos = end - 1;
            return loop;
        }

        // frames after pos before it wraps or ends
        uint32_t Left(uint32_t pos) const { return reverse ? pos - begin : end - 1 - pos; }

        // the frame k after pos, ~0 past the end
        uint32_t Advance(uint32_t pos, uint32_t k) const
        {
            const uint32_t lap = Left(pos) + 1;
            if(k < lap)
                return reverse ? pos - k : pos + k;
            if(!loop)
                return ~0u;
            k = (k - lap) % (end - begin);
            return reverse ? end - 1 - k : begin + k;
        }

        // the last k <= to for which the frame k after pos follows a wrap; ~0 without one
        uint32_t PrevWrap(uint32_t pos, uint32_t to) const
        {
            const uint32_t lap = Left(pos) + 1, len = end - begin;
            if(!loop || to < lap)
                return ~0u;
            return lap + (to - lap) / len * len;
        }

        // the first k >= from for which the frame k after pos follows a wrap; ~0 without one
        uint32_t NextWrap(uint32_t pos, uint32_t from) const
        {
            if(!loop)
                return ~0u;
            const uint32_t lap = Left(pos) + 1, len = end - begin;
            return from <= lap ? lap : lap + (from - lap + len - 1) / len * len;
        }
    };

    // One of a voice's rings, filled by the streamer in play order.
    struct Ring
    {
        static constexpr uint32_t kFrames = 16384; // 340 ms at 1x
        static constexpr uint32_t kHistory = 4096; // frames behind read the streamer leaves, to play back in reverse
        static constexpr int kRuns = 16;

        // from ring frame at on, the frames play span from pos; jump: pos doesn't follow the frame before
        struct Run
        {
            uint32_t at, pos;
            Span span;
            bool jump;
        };

        int16_t *frames; // kFrames stereo frames

        // the voice
        volatile uint32_t restart_seq; // fill again from pos (~0: the first frame), with start, end, reverse and loop
        volatile uint32_t pos;
        volatile float start, end;     // fractions of the file
        volatile bool reverse, loop;
        volatile bool prefill;         // waits for a note: filled after the rings that play
        volatile uint32_t read;        // frames read since the restart

        // the streamer
        volatile uint32_t ready_seq;   // restart_seq it is filled for
        volatile uint32_t written;     // frames written since the restart
        volatile uint32_t done;        // written when the span ended, kOpen while it plays on
        Run runs[kRuns];
        volatile uint32_t count;       // runs since the restart

        static constexpr uint32_t kOpen = ~0u;

        // the index of the run frame n is in, ~0 when it is gone
        uint32_t RunAt(uint32_t n) const
        {
            const uint32_t c = count;
            for(uint32_t i = c; i > 0 && i + kRuns > c; i--)
                if(runs[(i - 1) % kRuns].at <= n)
                    return i - 1;
            return ~0u;
        }

        // the file position of frame n, n < written; ~0 when unknown
        uint32_t PosAt(uint32_t n) const
        {
            const uint32_t i = RunAt(n);
            if(i == ~0u)
                return ~0u;
            const Run &r = runs[i % kRuns];
            return r.span.Advance(r.pos, n - r.at);
        }

        // the first frame from n on that follows a jump or a wrap, ~0 without one written yet
        uint32_t NextJump(uint32_t n) const
        {
            for(uint32_t i = RunAt(n), c = count; i < c; i++)
            {
                const Run &r = runs[i % kRuns];
                if(r.at >= n && r.jump)
                    return r.at;
                const uint32_t wrap = r.span.NextWrap(r.pos, n > r.at ? n - r.at : 0);
                const uint32_t ends = i + 1 < c ? runs[(i + 1) % kRuns].at : ~0u;
                if(wrap != ~0u && r.at + wrap < ends)
                    return r.at + wrap;
            }
            return ~0u;
        }

        // the last frame up to n that follows a jump or a wrap, ~0 without one
        uint32_t PrevJump(uint32_t n) const
        {
            const uint32_t c = count;
            for(uint32_t i = c; i > 0 && i + kRuns > c; i--)
            {
                const Run &r = runs[(i - 1) % kRuns];
                if(r.at > n)
                    continue;
                const uint32_t wrap = r.span.PrevWrap(r.pos, n - r.at);
                if(wrap != ~0u)
                    return r.at + wrap;
                if(r.jump)
                    return r.at;
                if(!r.at)
                    break;
                n = r.at - 1;
            }
            return ~0u;
        }
    };

    // A voice's stream: one file, a ring that plays and one that waits for the next note.
    struct Stream
    {
        char name[32];
        volatile uint32_t open_seq;   // the voice: a new name to open
        volatile uint32_t opened_seq; // the streamer: open_seq of the open file
        volatile uint32_t frames;     // of the open file, 0 when there is none
        volatile float speed;         // frames per sample, for who needs filling first
        Ring ring[2];
    };
}
