#pragma once
#include "daisy.h"
#include "fatfs.h"

namespace daisy
{
    class Engine; // forward declare gets around circular import
    // class VUTarget;
    // struct KeyRequest;

    /** @brief structure containing info necessary for deferred fileIO operations */
    struct FileRequest
    {
        enum class Type
        {
            OPEN_NEW, // CREATE_ALWAYS
            OPEN_NEW_TWO, // OPEN_ALWAYS
            HEADER,
            UNLINK,
            TRUNCATE,
            DUMMY,
        };

        Type type_;
        FIL *fil_;
        const char *fname_;
        size_t size_in_bytes_;
        void *id_;

        /** constructor for full request data */
        FileRequest(Type type,
                    FIL *fileptr,
                    const char *filename,
                    size_t bytes,
                    void *id)
            : type_(type),
              fil_(fileptr),
              fname_(filename),
              size_in_bytes_(bytes),
              id_(id)
        {
        }

        /** Empty, invalid request */
        FileRequest()
            : type_(Type::DUMMY),
              fil_(nullptr),
              fname_(nullptr),
              size_in_bytes_(0),
              id_(nullptr)
        {
        }
    };

    class FileStreamingManager
    {
    public:
        enum class Status
        {
            OK,
            EMPTY,
            ERR_READ,
            ERR_WRITE,
            ERR_UNKNOWN
        };

        /** FIFO of requests to fileIO that can be handled in order of reception
         *  Requests generated and pushed to the back from inside the AudioCallback,
         *  or from UI/MIDI interactions outside of the callback.
         */
        FIFO<FileRequest, 64> request_fifo;
        FIFO<FileRequest, 32> debug_fifo;

        void Init(float samplerate)
        {
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
            /** Also calcs SubChunk2Size */
            // file_header.FileSize = CalcFileSize(); // do this on write complete
        }

        Status ProcessRequests();

        WAV_FormatTypeDef file_header;

    };
} // namespace daisy