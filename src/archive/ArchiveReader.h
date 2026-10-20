#pragma once
#include "archive/ArchiveFormats.h"
#include "archive/Entries.h"

#include <windows.h>

#include <functional>

namespace sd::archive
{
    struct ReadOptions
    {
        Kind kind = Kind::None;   // from the file name; the content decides the actual format
        Limits limits;
        uint64_t inputSize = 0;   // for progress and the zip-bomb ratio
    };

    // Reads every entry of the archive open as `input` (read access is enough; seeking is used when
    // the handle allows it, as ZIP's central directory and 7z's header need) and hands them to `sink`.
    // This is the code that parses untrusted bytes, so it normally runs in the sandboxed worker
    // (ArchiveSource.h). `tick` gets the archive bytes consumed so far and returns false to stop.
    //
    // Encrypted archives stop with Status::Password before any of their data is read.
    Result ReadArchive(HANDLE input, const ReadOptions& options, IEntrySink& sink, const std::function<bool(uint64_t consumed)>& tick);
}
