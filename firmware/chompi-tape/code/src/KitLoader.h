#pragma GCC push_options
#pragma GCC optimize ("Os") // cold code, built for size so all of it fits in SRAM
#pragma once

#include <stdio.h>
#include <algorithm>
#include "fatfs.h"
#include "RamBuffer.h"
#include "AiffKit.h"

namespace chompi
{
    static constexpr size_t kKitMaxFrames = 12 * 48000; // the OP-1 limit is 12 s
    static constexpr size_t kKitChunk     = 2048;       // frames per SD timer tick
    static constexpr size_t kKitBounce    = kKitChunk * 4 + 4;
    static constexpr uint32_t kKitSaveDelay = 1000;     // ms after the last edit

    extern daisy::RamBufferMemory kit_buff; // the loaded kit, stereo, played from RAM

    // The same name with another extension: a kit's .json sidecar holds its slice edits, the .aif stays untouched
    inline void KitFileName(char *name, const char *from, const char *ext)
    {
        if(name != from)
            strcpy(name, from);
        const size_t n = strlen(name);
        strcpy(name + (n < 4 ? n : n - 4), ext);
    }

    // Loads a JAMMI slot's OP-1 kit into kit_buff a chunk per SD timer tick and keeps its slice edits
    // in the sidecar. Everything here runs on the SD timer, except Request, Edit and Forget.
    // The SD DMA can't reach DTCM and wants aligned addresses, so reads go through a bounce buffer in SDRAM.
    class KitLoader
    {
      public:
        void Init(FIL *fil, uint8_t *bounce)
        {
            fil_ = fil;
            bounce_ = bounce;
            name_[0] = want_[0] = '\0';
            pending_ = loading_ = dirty_ = failed_ = false;
            frames_ = done_ = size_ = 0;
            last_slice_ = -1;
        }

        // from the UI: load the slot's kit unless it is there already
        void Request(const char *name)
        {
            pending_ = false;
            __DMB();
            strcpy(want_, name);
            __DMB();
            pending_ = true;
        }

        uint32_t Frames() const { return kit_buff.length / 2; }
        bool Busy() const { return pending_ || loading_; }
        bool Failed() const { return failed_; } // the file isn't one this can play

        bool HasSlice(int slice) const
        {
            return kit_buff.length && slice >= 0 && slice < int(kKitSlices)
                   && slot_aiff.end[slice] > slot_aiff.start[slice] && slot_aiff.start[slice] < Frames();
        }
        int LastSlice() const { return last_slice_; }

        // the slice under a key, -1 for none; it is the one the knobs edit next
        int Slice(int slice)
        {
            if(!HasSlice(slice))
                return -1;
            last_slice_ = slice;
            return slice;
        }

        // from the UI: move the last played slice's start or end, saved to the sidecar a little later
        void Edit(bool end, int frames, uint32_t now)
        {
            const int slice = last_slice_;
            if(slice < 0 || !kit_buff.length)
                return;
            const int32_t min_len = int32_t(slot_aiff.rate * .005f); // 5 ms
            int32_t s = slot_aiff.start[slice], e = slot_aiff.end[slice];
            if(end)
                e = std::max(s + min_len, std::min(e + frames, int32_t(Frames())));
            else
                s = std::max(int32_t(0), std::min(s + frames, e - min_len));
            slot_aiff.start[slice] = s;
            slot_aiff.end[slice] = e;
            dirty_ = true;
            edit_time_ = now;
        }

        // its files changed: load it again next time, a load under way stops
        void Forget(const char *name)
        {
            if(!strcmp(name, name_))
            {
                name_[0] = '\0';
                dirty_ = false;
            }
        }

        // SD timer: delete a kit and its sidecar
        void Remove(const char *aif)
        {
            char json[32];
            KitFileName(json, aif, ".json");
            Forget(aif);
            f_unlink(aif);
            f_unlink(json);
        }

        // SD timer
        void Step()
        {
            if(pending_)
                Open();
            if(loading_)
                Load();
        }

        // SD timer, while nothing plays: the edits go to the sidecar, written next to it and renamed over
        void Save(uint32_t now)
        {
            if(dirty_ && !loading_ && name_[0] && now - edit_time_ >= kKitSaveDelay)
                Write();
        }

      private:
        void Write()
        {
            dirty_ = false;

            char *json = reinterpret_cast<char *>(bounce_);
            int n = sprintf(json, "{\"size\":%lu,\"start\":[", (unsigned long)size_);
            for(size_t i = 0; i < kKitSlices; i++)
                n += sprintf(json + n, i ? ",%lu" : "%lu", (unsigned long)slot_aiff.start[i]);
            n += sprintf(json + n, "],\"end\":[");
            for(size_t i = 0; i < kKitSlices; i++)
                n += sprintf(json + n, i ? ",%lu" : "%lu", (unsigned long)slot_aiff.end[i]);
            n += sprintf(json + n, "]}");

            char tmp[32];
            KitFileName(tmp, name_, ".tmp");
            KitFileName(json_, name_, ".json");
            UINT bw = 0;
            if(f_open(fil_, tmp, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
                return;
            const bool ok = f_write(fil_, json, n, &bw) == FR_OK && bw == UINT(n);
            f_close(fil_);
            if(ok)
            {
                f_unlink(json_);
                f_rename(tmp, json_);
            }
        }

        bool Read(uint32_t pos, void *dst, uint32_t n)
        {
            UINT br = 0;
            const uint32_t pad = pos & 3;
            if(n + pad > kKitBounce || f_lseek(fil_, pos) != FR_OK || f_read(fil_, bounce_ + pad, n, &br) != FR_OK
               || br != n)
                return false;
            memcpy(dst, bounce_ + pad, n);
            return true;
        }

        void Open()
        {
            pending_ = false;
            if(name_[0] && !strcmp(want_, name_)) // loaded or loading
                return;
            if(loading_)
                f_close(fil_);
            loading_ = false;
            if(dirty_) // the edits of the kit before
                Write();
            kit_buff.length = 0; // silent until loaded
            last_slice_ = -1;
            failed_ = false;
            strcpy(name_, want_);
            if(f_open(fil_, name_, FA_READ) != FR_OK)
            {
                name_[0] = '\0';
                failed_ = true;
                return;
            }

            // the JSON scratch is the kit memory, the audio comes after the parse
            size_ = f_size(fil_);
            auto read = [this](uint32_t pos, void *dst, uint32_t n) { return Read(pos, dst, n); };
            if(!ParseAiff(read, size_, slot_aiff, reinterpret_cast<char *>(kit_buff.mem), kKitBounce - 4)
               || f_lseek(fil_, slot_aiff.data_offset) != FR_OK)
            {
                Fail();
                return;
            }
            frames_ = slot_aiff.frames < kKitMaxFrames ? slot_aiff.frames : kKitMaxFrames;
            done_ = 0;
            loading_ = true;
        }

        void Load()
        {
            if(!name_[0]) // forgotten
            {
                f_close(fil_);
                loading_ = false;
                return;
            }
            const AiffInfo &a = slot_aiff;
            const size_t channels = a.channels;
            const size_t n = frames_ - done_ < kKitChunk ? frames_ - done_ : kKitChunk;
            const uint32_t pad = f_tell(fil_) & 3;
            UINT br = 0;
            if(f_read(fil_, bounce_ + pad, n * channels * 2, &br) != FR_OK || br != n * channels * 2)
            {
                Fail();
                return;
            }

            const int16_t *src = reinterpret_cast<const int16_t *>(bounce_ + pad);
            int16_t *dst = kit_buff.mem + done_ * 2;
            for(size_t i = 0; i < n; i++)
            {
                uint16_t l = src[i * channels], r = src[i * channels + channels - 1];
                if(!a.little_endian)
                {
                    l = __builtin_bswap16(l);
                    r = __builtin_bswap16(r);
                }
                dst[2 * i] = l;
                dst[2 * i + 1] = r;
            }

            done_ += n;
            if(done_ >= frames_)
            {
                f_close(fil_);
                loading_ = false;
                ReadSidecar();
                __DMB(); // the voices must not see the length before the audio
                kit_buff.length = frames_ * 2;
            }
        }

        // slice edits from an earlier session, if they belong to this file; a .tmp is a save cut short
        void ReadSidecar()
        {
            KitFileName(json_, name_, ".json");
            if(f_open(fil_, json_, FA_READ) != FR_OK)
            {
                KitFileName(json_, name_, ".tmp");
                if(f_open(fil_, json_, FA_READ) != FR_OK)
                    return;
            }
            const uint32_t n = f_size(fil_);
            char *json = reinterpret_cast<char *>(bounce_);
            UINT br = 0;
            const bool ok = n < kKitBounce && f_read(fil_, json, n, &br) == FR_OK && br == n;
            f_close(fil_);
            int32_t size;
            if(!ok || JSON_Validate(json, n) != JSONSuccess || !aiff::Number(json, n, "size", 4, size)
               || uint32_t(size) != size_)
                return;
            AiffInfo edits = slot_aiff;
            for(uint32_t i = 0; i < kKitSlices; i++)
            {
                int32_t s, e;
                if(!aiff::ArrayValue(json, n, "start", i, s) || !aiff::ArrayValue(json, n, "end", i, e))
                    return;
                edits.start[i] = s;
                edits.end[i] = e;
            }
            slot_aiff = edits;
        }

        void Fail()
        {
            f_close(fil_);
            loading_ = false;
            name_[0] = '\0';
            failed_ = true;
        }

        FIL *fil_;
        uint8_t *bounce_;
        char name_[32], want_[32], json_[32];
        volatile bool pending_;
        bool loading_, failed_;
        volatile bool dirty_;
        int8_t last_slice_;
        uint32_t frames_, done_, size_, edit_time_;
    };

    extern KitLoader kit_loader;
}

#pragma GCC pop_options
