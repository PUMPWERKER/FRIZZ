#include "Streamer.h"
#include "Wav.h"
#include <string.h>
#include <algorithm>

namespace chompi
{
    static constexpr uint32_t kMask = Ring::kFrames - 1;
    static constexpr uint32_t kMinFill = 1024; // frames, smaller fills wait
    static constexpr float kLead = 4800.f;     // samples buffered ahead, more and moved points take longer

    constexpr uint32_t Streamer::kBlockFrames; // C++14: std::min takes it by reference

    void Streamer::Init(Stream *streams, int count, SdFile *files, uint8_t *head, int16_t *rings, uint8_t *cache, int blocks)
    {
        streams_ = streams;
        count_ = count < kMaxStreams ? count : kMaxStreams;
        files_ = files;
        head_ = head;
        for(int i = 0; i < count_; i++)
        {
            streams[i] = Stream();
            for(int r = 0; r < 2; r++)
                streams[i].ring[r].frames = rings + (i * 2 + r) * Ring::kFrames * 2;
        }
        memset(open_seq_, 0, sizeof(open_seq_));
        memset(file_, 0, sizeof(file_));
        memset(state_, 0, sizeof(state_));
        cache_ = cache;
        block_count_ = blocks < kMaxBlocks ? blocks : kMaxBlocks;
        memset(blocks_, 0, sizeof(blocks_));
        clock_ = 0;
        forget_ = false;
        reading_ = -1;
    }

    bool Streamer::Step()
    {
        if(reading_ >= 0)
            Commit();
        if(forget_)
        {
            forget_ = false;
            for(Block &b : blocks_)
                b.valid = false;
        }
        for(int i = 0; i < count_; i++)
            if(streams_[i].open_seq != open_seq_[i])
            {
                Open(i); // FatFS, one a step
                return false;
            }
        for(int i = 0; i < count_; i++)
            for(int r = 0; r < 2; r++)
                if(streams_[i].ring[r].restart_seq != state_[i][r].restart_seq)
                    Restart(i, r);
        // every ring the cache can fill, and one transfer for the neediest that it can't
        uint32_t tried = 0;
        for(int k; (k = Neediest(tried)) >= 0;)
        {
            tried |= 1u << k;
            Fill(k / 2, k % 2, reading_ < 0);
        }
        return reading_ >= 0;
    }

    void Streamer::Open(int i)
    {
        Stream &s = streams_[i];
        SdFile &file = files_[i];
        open_seq_[i] = s.open_seq;
        file.Close();
        s.frames = 0;
        uint32_t frames = 0;
        if(file.Open(s.name))
        {
            UINT n = 0;
            uint32_t at, bytes;
            if(f_lseek(&file.fil, 0) == FR_OK && f_read(&file.fil, head_, 512, &n) == FR_OK
               && WavData(head_, n, file.Size(), at, bytes))
            {
                data_at_[i] = at;
                frames = bytes / 4;
                // the same file where it starts on the card, and as long
                file_[i] = file.fil.obj.sclust * 2654435761u ^ uint32_t(file.Size());
            }
        }
        s.frames = frames;
        __DMB();
        s.opened_seq = open_seq_[i];
    }

    Span Streamer::SpanOf(int i, int r) const
    {
        const Ring &g = streams_[i].ring[r];
        return Span::Of(g.start, g.end, streams_[i].frames, g.reverse, g.loop);
    }

    void Streamer::Restart(int i, int r)
    {
        Ring &g = streams_[i].ring[r];
        State &st = state_[i][r];
        st.restart_seq = g.restart_seq;
        st.file_seq = open_seq_[i];
        const Span span = SpanOf(i, r);
        st.cursor = span.Contains(g.pos) ? g.pos : span.First();
        st.ended = span.Empty();
        g.written = 0;
        g.count = 0;
        g.done = st.ended ? 0 : Ring::kOpen;
        AddRun(g, 0, st.cursor, span, false);
        __DMB();
        g.ready_seq = st.restart_seq;
    }

    // false when the voice may still read frames of the run it would replace
    bool Streamer::AddRun(Ring &g, uint32_t at, uint32_t pos, const Span &span, bool jump)
    {
        const uint32_t n = g.count;
        if(n >= Ring::kRuns && g.runs[(n - Ring::kRuns + 1) % Ring::kRuns].at >= g.read)
            return false;
        Ring::Run &run = g.runs[n % Ring::kRuns];
        run.at = at;
        run.pos = pos;
        run.span = span;
        run.jump = jump;
        __DMB();
        g.count = n + 1;
        return true;
    }

    // the ring that runs dry soonest, of those with room for a fill; the ones waiting for a note last
    int Streamer::Neediest(uint32_t skip) const
    {
        int best = -1;
        float soonest = 0.f;
        for(int i = 0; i < count_; i++)
        {
            const Stream &s = streams_[i];
            if(s.opened_seq != open_seq_[i] || !s.frames)
                continue;
            const float speed = std::max(float(s.speed), .01f);
            for(int r = 0; r < 2; r++)
            {
                const int k = i * 2 + r;
                const Ring &g = s.ring[r];
                const State &st = state_[i][r];
                if(skip & 1u << k || g.ready_seq != st.restart_seq || g.restart_seq != st.restart_seq || st.file_seq != open_seq_[i])
                    continue;
                const uint32_t buffered = g.written - g.read;
                const float lasts = buffered / speed; // samples
                if(buffered + Ring::kHistory + kMinFill > Ring::kFrames || lasts > kLead)
                    continue;
                const float key = g.prefill ? lasts + kLead : lasts;
                if(best < 0 || key < soonest)
                {
                    best = k;
                    soonest = key;
                }
            }
        }
        return best;
    }

    // fills the ring from the cache, or starts reading the block it needs if may_read
    void Streamer::Fill(int i, int r, bool may_read)
    {
        const Span span = SpanOf(i, r);
        if(!Track(i, r, span))
            return;
        Ring &g = streams_[i].ring[r];
        State &st = state_[i][r];
        const uint32_t w = g.written, buffered = w - g.read, want = 2.f * kLead * streams_[i].speed;
        const uint32_t room = std::min(Ring::kFrames - buffered - Ring::kHistory, std::max(kMinFill, want > buffered ? want - buffered : 0));
        if(Repeat(g, st, span, room))
            return;

        // the frames up to the end of the lap or of the block
        const uint32_t index = st.cursor / kBlockFrames, base = index * kBlockFrames;
        uint32_t n = std::min(room, span.Left(st.cursor) + 1);
        n = std::min(n, span.reverse ? st.cursor - base + 1 : base + kBlockFrames - st.cursor);
        const uint32_t first = span.reverse ? st.cursor + 1 - n : st.cursor;

        int b = Cached(file_[i], index);
        if(b >= 0)
        {
            blocks_[b].used = ++clock_;
            Copy(i, r, b, first, n);
            return;
        }
        if(!may_read || (b = Victim()) < 0)
            return;
        Block &block = blocks_[b];
        const uint32_t at = data_at_[i] + base * 4, frames = std::min(kBlockFrames, streams_[i].frames - base);
        block.file = file_[i];
        block.index = index;
        block.used = ++clock_;
        block.skip = at % 512;
        block.valid = false;
        reading_ = b;
        reading_stream_ = i;
        if(!sd_dma.Read(files_[i], at - block.skip, cache_ + b * kBlockBytes, block.skip + frames * 4))
            reading_ = -1;
    }

    // the run follows the points, direction and loop; false when there is nothing more to fill
    bool Streamer::Track(int i, int r, const Span &span)
    {
        Ring &g = streams_[i].ring[r];
        State &st = state_[i][r];
        if(span == g.runs[(g.count - 1) % Ring::kRuns].span)
            return !st.ended;
        const bool inside = span.Contains(st.cursor);
        if(inside || (span.loop && !span.Empty()))
        {
            if(st.ended && g.read >= g.done) // the voice is past the end
                return false;
            const uint32_t pos = inside ? st.cursor : span.First();
            if(!AddRun(g, g.written, pos, span, !inside))
                return false;
            if(st.ended)
            {
                st.ended = false;
                g.done = Ring::kOpen;
            }
            st.cursor = pos;
        }
        else if(!st.ended)
        {
            st.ended = true;
            g.done = g.written;
        }
        return !st.ended;
    }

    // a short loop, once read whole, goes on from the ring; false when this isn't one
    bool Streamer::Repeat(Ring &g, State &st, const Span &span, uint32_t room)
    {
        const Ring::Run &run = g.runs[(g.count - 1) % Ring::kRuns];
        const uint32_t w = g.written, len = span.end - span.begin;
        if(!span.loop || len > Ring::kFrames / 2 || st.cursor != span.First() || w - run.at < span.Left(run.pos) + 1 + len)
            return false;
        for(uint32_t done = 0; done < room;) // in runs no longer than the loop, so they don't overlap
        {
            const uint32_t to = (w + done) & kMask, from = (w + done - len) & kMask;
            const uint32_t k = std::min(std::min(room - done, len), std::min(Ring::kFrames - to, Ring::kFrames - from));
            memcpy(&g.frames[to * 2], &g.frames[from * 2], k * 4);
            done += k;
        }
        st.cursor = span.Advance(span.First(), room);
        __DMB();
        g.written = w + room;
        return true;
    }

    // the block that holds index of file, -1 when none does
    int Streamer::Cached(uint32_t file, uint32_t index) const
    {
        for(int b = 0; b < block_count_; b++)
            if(blocks_[b].valid && blocks_[b].file == file && blocks_[b].index == index)
                return b;
        return -1;
    }

    // the block to read into: an empty one, else the one unused longest, never the one being read
    int Streamer::Victim() const
    {
        int victim = -1;
        for(int b = 0; b < block_count_; b++)
        {
            if(b == reading_)
                continue;
            if(!blocks_[b].valid)
                return b;
            if(victim < 0 || blocks_[b].used < blocks_[victim].used)
                victim = b;
        }
        return victim;
    }

    // n frames of block b from file frame first into the ring in play order, then on in the run's span
    void Streamer::Copy(int i, int r, int b, uint32_t first, uint32_t n)
    {
        Ring &g = streams_[i].ring[r];
        State &st = state_[i][r];
        const Span &span = g.runs[(g.count - 1) % Ring::kRuns].span;
        const uint32_t w = g.written;
        const uint8_t *src = cache_ + b * kBlockBytes + blocks_[b].skip + (first - blocks_[b].index * kBlockFrames) * 4;
        if(span.reverse)
            src += (n - 1) * 4;
        for(uint32_t done = 0; done < n;)
        {
            const uint32_t at = (w + done) & kMask, run = std::min(n - done, Ring::kFrames - at);
            int16_t *dst = &g.frames[at * 2];
            if(!span.reverse)
                memcpy(dst, src, run * 4);
            else
                for(uint32_t k = 0; k < run; k++)
                    memcpy(dst + k * 2, src - k * 4, 4);
            src += span.reverse ? -int32_t(run * 4) : int32_t(run * 4);
            done += run;
        }

        const uint32_t last = span.reverse ? first : first + n - 1; // in play order
        uint32_t next = last;
        if(!span.Next(next))
        {
            st.ended = true;
            g.done = w + n;
            next = span.reverse ? last - 1 : last + 1; // where it goes on if the end moves past it
        }
        st.cursor = next;
        __DMB();
        g.written = w + n;
    }

    // the block is in, unless the transfer failed or the cache was dropped meanwhile
    void Streamer::Commit()
    {
        Block &block = blocks_[reading_];
        block.valid = !files_[reading_stream_].Failed() && !forget_;
        reading_ = -1;
    }
}
