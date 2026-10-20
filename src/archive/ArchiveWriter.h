#pragma once
#include "archive/ArchiveFormats.h"
#include "archive/Entries.h"

#include <functional>
#include <stop_token>
#include <string>
#include <vector>

struct archive;

namespace sd::archive
{
    // Writes one archive file with libarchive: ZIP (Deflate, UTF-8 names with the UTF-8 flag set),
    // TAR.GZ (POSIX pax, UTF-8 names) or 7z (LZMA2). Runs in our own process: what it reads are the
    // user's own files or entries the parent has already checked.
    class ArchiveWriter
    {
    public:
        ArchiveWriter() = default;
        ~ArchiveWriter();
        ArchiveWriter(const ArchiveWriter&) = delete;
        ArchiveWriter& operator=(const ArchiveWriter&) = delete;

        Result Open(const std::wstring& path, WriteFormat format);
        // A header; for a file, `entry.size` bytes of Write must follow. Names are '/'-separated.
        Result Add(const Entry& entry);
        Result Write(const void* bytes, size_t size);
        Result Close();   // writes the end of the archive (7z writes everything here)

    private:
        struct archive* m_a = nullptr;
        WriteFormat m_format = WriteFormat::Zip;
    };

    // Packs files and folders into one archive at `output` (a staging file). Each item goes in under
    // its own name ("Photos/..." for a folder); links and junctions inside folders are skipped, so a
    // loop can't make it run forever. Progress is bytes read over the total size.
    Result Compress(const std::vector<std::wstring>& items, WriteFormat format, const std::wstring& output, std::stop_token stop,
                    const std::function<void(double)>& progress);
}
