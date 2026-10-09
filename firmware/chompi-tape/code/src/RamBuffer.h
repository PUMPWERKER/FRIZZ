#pragma once
#include "daisy_seed.h"
#include "daisysp.h"

namespace daisy
{

    static const size_t kMaxRamBuffSize = 31694848 / 2;
    // ~50ms
    static const size_t kMinLoopSize = 4800;
    static const size_t kLoopFadeSize = 480;
    static constexpr float kLoopFadeStep = .00464f;
    static const size_t kCopyStepSize = 512;
    // total size = 31694848, which goes evenly into 8K, which means SDRAM page alignment

    // just a buffer with its length
    struct RamBufferMemory
    {
        // pointer to array of size kMaxRamBuffSize
        void Init(int16_t* m)
        {
            mem = m;
        }

        int16_t* mem;
        size_t length;

        volatile size_t copy_lo = 0, copy_hi = 0;

        size_t halve_len[12];
        uint8_t halve_count = 0;

        size_t pending_len = 0;

        int8_t length_exp = 0;

        void Clear()
        {
            std::fill(&mem[0], &mem[kMaxRamBuffSize], 0);
        }
    };

    // keeps track of its own read and write heads
    class RamBuffer
    {
    public:
        void Init(RamBufferMemory* b)
        {
            buff = b;
            buff->Clear();
        }

        void Reset()
        {
            buff->length = read_head = write_head = 0;
            buff->copy_lo = buff->copy_hi = 0;
            ClearLengthChanges();
        }

        void BlockRead(int16_t* copy_buff, size_t size)
        {
            const size_t end = std::min(buff->length, kMaxRamBuffSize);
            size = read_head < end ? std::min(size, end - read_head) : 0;

            std::copy(&buff->mem[read_head], &buff->mem[read_head + size], copy_buff);
            read_head += size;
        }

        void StereoRead(int16_t* l, int16_t* r, bool rev)
        {
            const size_t new_read_head = read_head + 2;
            if(!rev && new_read_head <= buff->length && new_read_head <= kMaxRamBuffSize)
            {
                *l = buff->mem[read_head];
                *r = buff->mem[read_head + 1];
                read_head = new_read_head;
            }
            else if(rev && read_head >= 2)
            {
                *l = buff->mem[read_head - 2];
                *r = buff->mem[read_head - 1];
                read_head -= 2;
            }
            else
            {
                *l = *r = 0;
            }
        }

        void BlockWrite(int16_t* copy_buff, size_t size)
        {
            if(write_head + size > kMaxRamBuffSize)
                size = kMaxRamBuffSize - write_head;

            std::copy(copy_buff, &copy_buff[size], &buff->mem[write_head]);
            write_head += size;
            buff->length = write_head;
            buff->length -= buff->length % 2;
        }

        void StereoWrite(int16_t l, int16_t r, bool rev, bool set_len)
        {
            if(!rev)
            {
                const size_t new_write_head = write_head + 2;
                if(new_write_head <= kMaxRamBuffSize)
                {
                    buff->mem[write_head] = l;
                    buff->mem[write_head + 1] = r;
                    write_head = new_write_head;
                    if(set_len)
                        buff->length = write_head;
                }
            }
            else if(write_head >= 2)
            {
                buff->mem[write_head - 2] = l;
                buff->mem[write_head - 1] = r;
                write_head -= 2;
            }
        }

        inline size_t GetSize() { return buff->length; }

        inline size_t GetReadHead() { return read_head; }
        inline bool ReadEOF() { return read_head >= buff->length || read_head >= kMaxRamBuffSize; }
        inline bool ReadLoop(bool rev) { return (rev && read_head == 0) || (!rev && ReadEOF()); }

        void SetReadHead(size_t pos) 
        { 
            if(pos <= kMaxRamBuffSize && pos <= buff->length)
                read_head = pos;
        }
        
        size_t GetRemainingRead(bool rev)
        {
            if(!rev && !ReadEOF())
                return buff->length - read_head;
            else if(rev)
                return read_head;

            return 0;
        }

        inline size_t GetWriteHead() { return write_head; }
        inline bool WriteEOF() { return write_head >= buff->length || write_head >= kMaxRamBuffSize; }
        inline bool WriteFullLength() { return write_head >= kMaxRamBuffSize; }
        inline bool WriteLoop(bool rev) { return (rev && write_head == 0) || (!rev && WriteEOF()); }

        void SetWriteHead(size_t pos) 
        {
            if(pos <= kMaxRamBuffSize && pos <= buff->length)
                write_head = pos;
        }

        size_t GetRemainingWrite(bool rev)
        {
            if(!rev && !WriteEOF())
                return buff->length - write_head;
            else if(rev)
                return write_head;

            return 0;            
        }

        void AdvanceWrite(bool rev)
        {
            if(rev && write_head >= 2)
                write_head -= 2;
            else if(!rev && write_head + 2 <= buff->length && write_head + 2 < kMaxRamBuffSize)
                write_head += 2;
        }

        inline bool IsCopying() const { return buff->copy_lo != buff->copy_hi; }

        inline bool CanRestore() const { return buff->halve_count; }
        inline void ClearLengthChanges()
        {
            buff->pending_len = 0;
            buff->halve_count = 0;
            buff->length_exp = 0;
        }

        bool ApplyPendingLength()
        {
            if(!buff->pending_len)
                return false;

            buff->length = buff->pending_len;
            buff->pending_len = 0;
            read_head %= buff->length;
            write_head %= buff->length;
            return true;
        }

        inline int8_t LengthExp() const { return buff->length_exp; }

        inline size_t LoopLength() const { return buff->pending_len ? buff->pending_len : buff->length; }

        bool CanChangeLength(int8_t steps) const
        {
            const size_t len = LoopLength();
            if(IsCopying() || !steps)
                return false;
            if(steps > 0)
                return buff->halve_count >= steps
                       || (steps == 1 && !CanRestore() && !buff->pending_len && len >= kMinLoopSize && len * 2 <= kMaxRamBuffSize);
            return (len >> -steps) >= kMinLoopSize && buff->halve_count - steps <= int(sizeof(buff->halve_len) / sizeof(size_t));
        }

        // steps > 0 doubles, < 0 halves
        bool ChangeLength(int8_t steps, bool rev) __attribute__((optimize("Os"))) // saves code space
        {
            if(!CanChangeLength(steps))
                return false;

            size_t len = LoopLength();
            if(steps > 0 && CanRestore())
            {
                buff->halve_count -= steps;
                SetLoopLength(buff->halve_len[buff->halve_count], rev);
            }
            else if(steps > 0)
            {
                FadeSeam(&buff->mem[len - 2], -2);

                buff->copy_lo = 0;
                buff->copy_hi = len;
                buff->length = len * 2;
            }
            else
            {
                for(int8_t i = steps; i < 0; i++)
                {
                    buff->halve_len[buff->halve_count++] = len;
                    len = (len / 2) & ~size_t(1);
                }
                SetLoopLength(len, rev);
            }
            buff->length_exp += steps;
            return true;
        }

        void CopyStep()
        {
            size_t lo = buff->copy_lo, hi = buff->copy_hi;
            const size_t len = buff->length / 2;

            size_t n = std::min(kCopyStepSize, hi - lo);
            memcpy(&buff->mem[lo + len], &buff->mem[lo], n * sizeof(int16_t));
            if(lo == 0)
                FadeSeam(&buff->mem[len], 2);
            lo += n;

            n = std::min(kCopyStepSize, hi - lo);
            hi -= n;
            memcpy(&buff->mem[hi + len], &buff->mem[hi], n * sizeof(int16_t));

            buff->copy_lo = lo;
            buff->copy_hi = hi;
        }

        void LoopFadeAudio(int32_t* l, int32_t* r, bool rev) const
        {
            *l = *r = 0;
            if((rev && write_head < 2) || (!rev && write_head + 2 > buff->length))
                return;

            const size_t pos = rev ? write_head - 2 : write_head;
            const size_t from_start = rev ? buff->length - pos : pos;
            const size_t to_end = rev ? pos : buff->length - pos;
            if(from_start >= kLoopFadeSize && to_end >= kLoopFadeSize)
                return;

            const float step = kLoopFadeStep * .5f;
            const float fade_in = from_start * step;
            const float fade_out = 1.f - (kLoopFadeSize - std::min(to_end, kLoopFadeSize)) * step;
            const float keep = 1.f - daisysp::fclamp(std::min(fade_in, fade_out), 0.f, 1.f);
            *l = buff->mem[pos] * keep;
            *r = buff->mem[pos + 1] * keep;
        }

        void ShiftWriteHead(int32_t offset)
        {
            const int32_t len = buff->length;
            if(len == 0)
                return;

            int32_t pos = (static_cast<int32_t>(write_head) + offset) % len;
            write_head = (pos < 0 ? pos + len : pos) & ~1;
        }

        void FadeJoin(size_t pos)
        {
            if(pos < kLoopFadeSize || pos + kLoopFadeSize > buff->length)
                return;
            FadeSeam(&buff->mem[pos - 2], -2);
            FadeSeam(&buff->mem[pos], 2);
        }

    private:
        void SetLoopLength(size_t len, bool rev)
        {
            if(len >= buff->length || read_head + (rev ? 0 : kLoopFadeSize) < len)
            {
                buff->length = len;
                buff->pending_len = 0;
            }
            else
                buff->pending_len = len;
        }

        static void FadeSeam(int16_t* seam, int step) __attribute__((noinline, optimize("Os")))
        {
            for(size_t i = 0; i < kLoopFadeSize; i += 2, seam += step)
            {
                const float gain = i / static_cast<float>(kLoopFadeSize);
                seam[0] *= gain;
                seam[1] *= gain;
            }
        }

        RamBufferMemory* buff;
        size_t read_head, write_head;
    };
} // namespace daisy