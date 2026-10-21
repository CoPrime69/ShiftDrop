#pragma once
#include "archive/ArchiveReader.h"

#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace sd::archive
{
    enum class ReadMode
    {
        Sandboxed,    // the worker, in the AppContainer
        Worker,       // the worker as a plain child process (AppContainer unavailable or turned off)
        InProcess,    // no worker exe found: libarchive in our own process
    };

    // Reads an archive's entries into `sink` the safe way: the sandboxed worker parses the file
    // (opened here, read-only, and passed as a handle) and streams the entries back; GuardSink should
    // wrap `sink` to check them. Cancel kills the worker at once. `progress` gets 0..1.
    //
    // An encrypted archive calls `password` (on this thread) when the worker needs it; the answer goes
    // to the worker over the same pipe, and only there.
    Result ReadEntries(const std::wstring& path, Kind kind, const Limits& limits, IEntrySink& sink, std::stop_token stop,
                       const std::function<void(double)>& progress, ReadMode* mode = nullptr, const PasswordFn& password = {});

    // The worker's side: "archive-read --in <handle> --out <handle> --kind <n> --size <bytes>
    // --max-bytes <n> --max-entries <n>", where --out is the pipe (both ways). Returns the process
    // exit code.
    int RunReadJob(const std::vector<std::wstring>& args);
}
