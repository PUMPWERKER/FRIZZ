#pragma GCC push_options
#pragma GCC optimize ("Os") // cold code, built for size so all of it fits in SRAM
#pragma once
#include "daisy.h"
#include "fatfs.h"
#include "FileStreamingManager.h"

namespace daisy {

class FileCopier
{
    public:
        FileCopier() {}
        ~FileCopier() {}

        void Init(float samplerate, Engine* fx,
                    RamBufferMemory* chompi_buff, RamBufferMemory* looper_buff)
        {
            fx_ = fx;
            copying_ = false;

            // WAV file header template
            file_header.ChunkId       = kWavFileChunkId;     /** "RIFF" */
            file_header.FileFormat    = kWavFileWaveId;      /** "WAVE" */
            file_header.SubChunk1ID   = kWavFileSubChunk1Id; /** "fmt " */
            file_header.SubChunk1Size = 16;                  // for PCM
            file_header.AudioFormat   = WAVE_FORMAT_PCM;
            file_header.NbrChannels   = 2;
            file_header.SampleRate    = static_cast<int>(samplerate);
            file_header.ByteRate      = samplerate * 2 * 16 / 8; // sr * chan * bitspersample / 8
            file_header.BlockAlign    = 2 * 16 / 8; //channels * bitspersample / 8;
            file_header.BitPerSample  = 16;
            file_header.SubChunk2ID   = kWavFileSubChunk2Id; /** "data" */

            chompi_ram.Init(chompi_buff);
            looper_ram.Init(looper_buff);
        }

        bool CopyProcess()
        {
            // looper jobs wait for copy, bake, pending change and undo
            if(!copying_ && !req_fifo.IsEmpty()
                && (req_fifo.Front().is_looper == CopyRequest::RamDir::NONE
                    || (!looper_ram.IsCopying() && !looper_ram.IsPending() && !fx_->IsLooperPrinting()
                        && !looper_ram.IsUndoing())))
            {
                copying_ = true;
                req = req_fifo.PopFront();

                is_chompi_ram = req.is_chompi;
                is_looper_ram = req.is_looper;

                chompi_ram.SetReadHead(0);
                chompi_ram.SetWriteHead(0);

                looper_ram.SetReadHead(looper_ram.LoopStart()); // a save stores what the loop plays

                if(Engine::IsKitSlot(req.src, req.src_bank, req.src_mode))
                {
                    if(!CopyKitStart())
                        return false;
                }
                else
                {

                CopyStart(req.src, req.src_bank, req.src_mode, req.dest, req.dest_bank, req.dest_mode);
                if(is_looper_ram == CopyRequest::RamDir::TO)
                    StartLooperLoad();
                }
            }

            if(!copying_)
                return false;


            if(!CopySizeFull() && !IsEOF())
            {
                UINT read_num = 0;

                uint32_t read_size = buff_size;
                if(copyread + read_size >= copysize && copysize != 0)
                    read_size = copysize - copyread;

                // read
                if(is_chompi_ram == CopyRequest::RamDir::FROM)
                {
                    chompi_ram.BlockRead((int16_t*)buff, read_size / 2);
                    read_num = read_size;
                }
                else if(is_looper_ram == CopyRequest::RamDir::FROM)
                {
                    looper_ram.BlockRead((int16_t*)buff, read_size / 2);
                    read_num = read_size;
                }
                else
                {
                    f_read(&fptr_read, buff, read_size, &read_num);
                }


                // write
                if(is_chompi_ram == CopyRequest::RamDir::TO)
                {
                    chompi_ram.BlockWrite((int16_t*)buff, read_num / 2);
                }
                else if(is_looper_ram == CopyRequest::RamDir::TO)
                {
                    if(!looper_staged || looper_ram.IsLoading()) // not after a reset
                        looper_ram.BlockWrite((int16_t*)buff, read_num / 2, !looper_staged);
                }
                else
                {
                    if(misalign != 0)
                    {
                        for(size_t i = 0; i < misalign; i++)
                        {
                            buff[i] = 0;
                        }
                        misalign = 0;
                    }
                    f_write(&fptr_write, buff, read_size, nullptr);
                    f_sync(&fptr_write);
                }

                copyread += read_num;
            }

            else
            {
                CopyDone();
            }

            return true;
        }

        inline bool IsCopying()  { return copying_ || !req_fifo.IsEmpty(); }
        inline bool IsLooperStaged() { return looper_staged; }
        inline bool LooperWasPlaying() { return looper_was_playing; }

        bool NeedsOverwrite(size_t idx, VoiceMode mode, size_t bank)
        {
            char fname[32];
            Engine::GetFileNameForSlot(idx + 1, bank, mode, fname);

            // no file, no overwrite
            if(f_stat(fname, nullptr) != FR_OK)
                return false;

            f_open(&fptr_read, fname, (FA_OPEN_ALWAYS | FA_WRITE | FA_READ));

            // large header || has footer?
            uint32_t size = 0;
            if(JumpToData(fname, &size))
            {
                CloseFile(&fptr_read);
                return true;
            }

            CloseFile(&fptr_read);
            return false;
        }

        struct CopyRequest {

            enum RamDir {
                NONE,
                FROM,
                TO
            };

            CopyRequest(size_t s, size_t b, VoiceMode m, size_t d, size_t db, VoiceMode dm, bool st, RamDir ic, RamDir il)
                : src(s),
                src_bank (b),
                src_mode(m),
                dest(d),
                dest_bank(db),
                dest_mode(dm),
                set(st),
                is_chompi(ic),
                is_looper(il)
            {
            }

            // default
            CopyRequest()
                : src(0),
                src_bank (0),
                src_mode(VoiceMode::JAMMI),
                dest(0),
                dest_bank(0),
                dest_mode(VoiceMode::JAMMI),
                set(false),
                is_chompi(RamDir::NONE),
                is_looper(RamDir::NONE)
            {
            }

            ~CopyRequest() {}

            size_t src;
            size_t src_bank;
            VoiceMode src_mode;
            size_t dest;
            size_t dest_bank;
            VoiceMode dest_mode;
            bool set;
            RamDir is_chompi;
            RamDir is_looper;
            bool append = false;
        };

        FIFO<CopyRequest, 16> req_fifo;

    private:
        // A kit copies as files, the .aif and then its sidecar, and only to another JAMMI slot on the card.
        // Both go through the normal loop as raw copies: no WAV header.
        bool CopyKitStart() __attribute__((optimize("Os")))
        {
            if(req.dest_mode != VoiceMode::JAMMI || req.dest < 1 || req.dest > 14)
            {
                copying_ = false;
                return false;
            }

            char from[32], to[32];
            Engine::GetFileNameForSlot(req.src, req.src_bank, req.src_mode, from);
            Engine::KitName(from);
            if(f_stat(from, nullptr) != FR_OK)
            {
                copying_ = false;
                return false;
            }

            Engine::GetFileNameForSlot(req.dest, req.dest_bank, req.dest_mode, to);
            f_unlink(to);
            Engine::KitName(to);
            chompi::kit_loader.Remove(to);
            fx_->SetFileExists(req.dest - 1, req.dest_bank, req.dest_mode, true);
            chompi::kit_slots[req.dest_bank] |= 1 << req.dest;

            kit_phase_ = 1;
            if(CopyRaw(from, to))
                return true;
            kit_phase_ = 0;
            copying_ = false;
            return false;
        }

        // after the .aif comes the sidecar, if there is one
        bool CopyKitNext() __attribute__((optimize("Os")))
        {
            if(kit_phase_ != 1)
                return false;
            CloseFile(&fptr_read);
            CloseFile(&fptr_write);
            char from[32], to[32];
            Engine::GetFileNameForSlot(req.src, req.src_bank, req.src_mode, from);
            Engine::GetFileNameForSlot(req.dest, req.dest_bank, req.dest_mode, to);
            chompi::KitFileName(from, from, ".json");
            chompi::KitFileName(to, to, ".json");
            kit_phase_ = 2;
            return CopyRaw(from, to);
        }

        bool CopyRaw(const char* from, const char* to)
        {
            copyread = copysize = 0;
            misalign = 0;
            if(f_open(&fptr_read, from, FA_READ) != FR_OK)
                return false;
            if(f_open(&fptr_write, to, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
            {
                CloseFile(&fptr_read);
                return false;
            }
            copysize = f_size(&fptr_read);
            return true;
        }

        void CopyStart(size_t src, size_t src_bank, VoiceMode src_mode, size_t dest, size_t dest_bank, VoiceMode dest_mode) __attribute__((optimize("Os")))
        {
            if(dest < 15 && Engine::IsKitSlot(dest, dest_bank, dest_mode)) // a sample replaces the kit
            {
                char kit[32];
                Engine::GetFileNameForSlot(dest, dest_bank, dest_mode, kit);
                Engine::KitName(kit);
                chompi::kit_loader.Remove(kit);
            }

            if(dest < 16)
                fx_->SetFileExists(dest - 1, dest_bank, dest_mode, true);

            char fname[32];

            copyread = 0;
            copysize = 0;

            if(is_chompi_ram != CopyRequest::RamDir::FROM && is_looper_ram != CopyRequest::RamDir::FROM)
            {
                // close then open read FIL
                CloseFile(&fptr_read);

                Engine::GetFileNameForSlot(src, src_bank, src_mode, fname);
                f_open(&fptr_read, fname, (FA_OPEN_ALWAYS | FA_WRITE | FA_READ));

                // jump read FIL past the header, set size variables
                JumpToData(fname, &copysize);
            }
            else if(is_chompi_ram == CopyRequest::RamDir::FROM)
            {
                copysize = chompi_ram.GetSize() * 2;
                src_bank = 0;
                src_mode = VoiceMode::CUBBI;
            }
            else if(is_looper_ram == CopyRequest::RamDir::FROM)
            {
                copysize = looper_ram.LoopLength() * 2;
                src_bank = 0;
                src_mode = VoiceMode::CUBBI;
            }

            if(is_chompi_ram != CopyRequest::RamDir::TO && is_looper_ram != CopyRequest::RamDir::TO)
            {
                // close then open the write FIL
                CloseFile(&fptr_write);


                Engine::GetFileNameForSlot(dest, dest_bank, dest_mode, fname);
                f_open(&fptr_write, fname, (FA_OPEN_ALWAYS | FA_WRITE | FA_READ));
                WriteHeader(&fptr_write);
            }

            copying_ = true;
        }

        void StartLooperLoad() __attribute__((optimize("Os")))
        {
            const size_t len = copysize / 2;
            looper_staged = false;
            if(!fx_->IsLooperFirstRecording())
            {
                // replace below the playing loop if it fits there, else after it
                looper_ram.StartLoad();
                fx_->LooperStop(fx_->IsLooperPlaying()); // ends an overdub, it would go on over the new loop
                const size_t start = looper_ram.GetStart(), end = looper_ram.End();
                looper_at = !req.append && len <= start ? 0 : end;
                looper_staged = req.append || (len && looper_at + len <= kMaxRamBuffSize);
            }
            if(!looper_staged)
            {
                loop_sync.loading = true; // the empty looper stops filling until OpenFile
                looper_was_playing = fx_->IsLooperPlaying();
                fx_->LooperStop();
                looper_ram.Reset();
                looper_at = 0;
            }
            looper_ram.SetWriteHead(looper_at);
        }

        void WriteHeader(FIL* fil)
        {
            // write header
            UINT bw = 0;
            file_header.SubCHunk2Size = f_size(fil) - sizeof(file_header);
            file_header.FileSize = f_size(fil);

            f_lseek(fil, 0);
            f_write(fil, &file_header, sizeof(file_header), &bw);
            f_sync(fil);
        }

        void CloseFile(FIL* fil)
        {
            if(f_size(fil) != 0)
            {
                f_close(fil);
                fil->obj.objsize = 0;
            }
        }

        void CopyDone() __attribute__((optimize("Os")))
        {
            if(kit_phase_ && CopyKitNext())
                return;

            if(kit_phase_)
            {
                kit_phase_ = 0;
                CloseFile(&fptr_write);
            }
            else if(is_chompi_ram != CopyRequest::RamDir::TO && is_looper_ram != CopyRequest::RamDir::TO)
            {
                f_truncate(&fptr_write);
                WriteHeader(&fptr_write);
                CloseFile(&fptr_write);
            }

            if(is_chompi_ram != CopyRequest::RamDir::FROM && is_looper_ram != CopyRequest::RamDir::FROM)
                CloseFile(&fptr_read);

            if(is_looper_ram == CopyRequest::RamDir::TO && looper_staged && looper_ram.IsLoading()) // not reset meanwhile
            {
                const size_t end = looper_ram.GetWriteHead();
                const size_t start = req.append ? looper_ram.GetStart() : looper_at;
                if(req.append)
                    looper_ram.FadeJoin(looper_at, end);
                looper_ram.FinishLoad(start, end - start);
            }

            fptr_read.obj.objsize = 0;
            fptr_write.obj.objsize = 0;

            copying_ = false;

            if(req.set)
            {
                fx_->SetBank(req.dest_bank);
                fx_->SetVoiceMode(req.dest_mode);
                fx_->SetVoiceSlot(req.dest, false);
            }

            fx_->SetAllCopyOccurred();
        }

        inline bool CopySizeFull() { return (copyread >= copysize && copysize != 0); }

        bool IsData(uint8_t* data)
        {
            return (data[0] == 'd') && (data[1] == 'a') && (data[2] == 't') && (data[3] == 'a');
        }

        bool JumpToData(char* fname, uint32_t* size)
        {
            if(size != nullptr)
                *size = f_size(&fptr_read);

            uint8_t data[4];
            copysize = 0;
            misalign = 0;

            uint32_t ctr = 0;
            UINT br;
            while(ctr < 2048 && !IsEOF())
            {
                f_lseek(&fptr_read, ctr);
                f_read(&fptr_read, data, 4, &br);
                ctr++;

                if(br != 4)
                {
                    break;
                }
                else if(IsData(data))
                {
                    f_read(&fptr_read, data, 4, &br); // read the data size

                    for(size_t i = 0; i < 4; i++)
                    {
                        copysize += uint32_t(data[i]) << (i * 8);
                    }

                    bool header = f_tell(&fptr_read) != sizeof(WAV_FormatTypeDef);
                    bool footer = f_tell(&fptr_read) + copysize != f_size(&fptr_read);

                    // fix 4 byte grid misalignment due to odd sized headers
                    misalign = f_tell(&fptr_read) % 4;
                    if(misalign == 2)
                        f_lseek(&fptr_read, f_tell(&fptr_read) - misalign);

                    return header || footer;
                }
            }

            // couldn't find "data", jump back to start
            f_lseek(&fptr_read, 0);
            return false;
        }

        bool IsEOF() 
        {
            if(is_chompi_ram == CopyRequest::RamDir::FROM)
                return chompi_ram.ReadEOF();
            else if(is_looper_ram == CopyRequest::RamDir::FROM)
                return looper_ram.ReadEOF();

            return f_eof(&fptr_read);
        }

        FIL fptr_read;
        FIL fptr_write;
        bool copying_;
        // loading next to the playing loop, which keeps playing until the loop point
        bool looper_staged = false;
        bool looper_was_playing = false;
        uint8_t kit_phase_ = 0; // copying a kit: 1 the .aif, 2 its sidecar
        uint32_t copysize = 0;
        uint32_t copyread = 0;
        WAV_FormatTypeDef file_header;

        // RAM IO
        CopyRequest::RamDir is_chompi_ram;
        CopyRequest::RamDir is_looper_ram;        
        RamBuffer chompi_ram;
        RamBuffer looper_ram;        
        size_t looper_at = 0;

        CopyRequest req;

        static const size_t buff_size = 4096;
        uint8_t buff[buff_size];
        uint8_t misalign;

        Engine* fx_;
};
} // namespace daisy
#pragma GCC pop_options
