#include "FileStreamingManager.h"
#include "DSPEngine.h"
#include "util/scopedirqblocker.h"

using namespace daisy;

FileStreamingManager::Status FileStreamingManager::ProcessRequests()
{
    FRESULT fres;
    Status retval = Status::ERR_UNKNOWN;
    if (!request_fifo.IsEmpty())
    {
        // auto req = request_fifo.PopFront();
        auto req = request_fifo[0];
        switch (req.type_)
        {
        case FileRequest::Type::OPEN_NEW:
            if (req.fil_ && req.fname_)
            {
                fres = f_open(req.fil_,
                                req.fname_,
                                (FA_CREATE_ALWAYS | FA_WRITE | FA_READ));

                UINT bw = 0;
                fres = f_write(req.fil_, &file_header, sizeof(file_header), &bw);
                fres = f_sync(req.fil_);

                retval = fres == FR_OK ? Status::OK : Status::ERR_READ;
            }
            break;
            case FileRequest::Type::OPEN_NEW_TWO:
            if (req.fil_ && req.fname_)
            {
                fres = f_open(req.fil_,
                                req.fname_,
                                (FA_OPEN_ALWAYS | FA_WRITE | FA_READ));

                UINT bw = 0;
                fres = f_write(req.fil_, &file_header, sizeof(file_header), &bw);
                fres = f_sync(req.fil_);

                retval = fres == FR_OK ? Status::OK : Status::ERR_READ;
            }
            break;
        case FileRequest::Type::HEADER: // update the WAV header after a write
            if (req.fil_)
            {
                UINT bw = 0;
                file_header.SubCHunk2Size = f_size(req.fil_) - sizeof(file_header);
                file_header.FileSize = f_size(req.fil_);

                fres = f_lseek(req.fil_, 0);
                fres = f_write(req.fil_, &file_header, sizeof(file_header), &bw);
                fres = f_sync(req.fil_);

                retval = fres == FR_OK ? Status::OK : Status::ERR_WRITE;
            }
            break;
        case FileRequest::Type::UNLINK:
            fres = f_unlink(req.fname_);

            static_cast<Engine*>(req.id_)->EraseFinished();

            retval = fres == FR_OK ? Status::OK : Status::ERR_UNKNOWN;
            break;
        case FileRequest::Type::TRUNCATE:
        {
            FRESULT frest = f_truncate(req.fil_);
            fres = f_sync(req.fil_);

            if(frest != FR_OK || fres != FR_OK)
                retval = Status::ERR_UNKNOWN;
            else
                retval = Status::OK; 
        break;
        }
        default:
            retval = Status::ERR_UNKNOWN;
            break;
        }
    
        if (retval == Status::ERR_READ 
            || retval == Status::ERR_WRITE
            || retval == Status::ERR_UNKNOWN
        )
        {
            auto db_req = request_fifo.PopFront();
            
            if(debug_fifo.IsFull())
                debug_fifo.PopFront();
            
            debug_fifo.PushBack(db_req);
        }
        else
        {
            request_fifo.PopFront();
        }
    }
    else
    {
        retval = Status::EMPTY;
    }
    
    return retval;
}
