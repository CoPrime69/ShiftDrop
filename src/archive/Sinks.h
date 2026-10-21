#pragma once
#include "archive/ArchiveWriter.h"
#include "archive/Entries.h"

#include <windows.h>
#include <wil/resource.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace sd::archive
{
    // Sits between any reader and any sink, in the trusted parent. It
    //  - refuses the whole archive on an absolute or ".." name (Status::Unsafe), so a crafted archive
    //    never gets a partial extraction that looks fine;
    //  - rewrites every other name to a safe relative path (SafeParts);
    //  - skips symlinks, devices and FIFOs (counted in skippedLinks), and hard links that point
    //    outside the archive;
    //  - enforces the entry count, the total size and the zip-bomb ratio, and that no entry is longer
    //    than its header declared.
    class GuardSink : public IEntrySink
    {
    public:
        GuardSink(IEntrySink& inner, const Limits& limits, uint64_t inputSize, std::wstring rawName = {});
        Result Begin(const Entry& entry) override;
        Result Data(const void* bytes, size_t size) override;
        Result End() override;

        // Bytes written on the archive's behalf without a Data call (a hard link copied as a file).
        Result Charge(uint64_t bytes);

        uint64_t files = 0, folders = 0, skippedLinks = 0, bytes = 0;

    private:
        IEntrySink& m_inner;
        Limits m_limits;
        uint64_t m_inputSize;
        std::wstring m_rawName;   // a .gz holds one nameless file: it is named after the archive
        uint64_t m_entries = 0;
        bool m_skip = false;
        int64_t m_declared = -1;
        uint64_t m_received = 0;
    };

    // Writes entries into a folder (the StagedOutput staging folder). Names are already safe
    // (GuardSink); files are created new only, so two names that differ only in case become
    // "readme" and "readme (2)" instead of one overwriting the other.
    class ExtractSink : public IEntrySink
    {
    public:
        // `charge` is GuardSink::Charge, for hard links copied as files.
        ExtractSink(std::wstring root, std::function<Result(uint64_t)> charge = {});
        Result Begin(const Entry& entry) override;
        Result Data(const void* bytes, size_t size) override;
        Result End() override;

        // Folder times last (writing their files changed them), deepest first.
        void Finish();
        // "Photos.zip" holding just "Photos/..." gives "Photos (extracted)\..." rather than
        // "Photos (extracted)\Photos\...". True when it moved something.
        bool FlattenSingleFolder();

    private:
        Result MakeFolders(const std::vector<std::wstring>& parts, size_t count);
        std::wstring m_root;
        std::function<Result(uint64_t)> m_charge;
        wil::unique_hfile m_file;
        int64_t m_mtime = 0, m_atime = 0;
        std::map<std::wstring, std::wstring> m_written;   // lower-case entry name -> file on disk (hard links)
        std::vector<std::pair<std::wstring, std::pair<int64_t, int64_t>>> m_folders;   // path, mtime, atime
    };

    // The one file of a .gz/.bz2/.xz, written into a staging file.
    class FileSink : public IEntrySink
    {
    public:
        explicit FileSink(std::wstring path) : m_path(std::move(path)) {}
        Result Begin(const Entry& entry) override;
        Result Data(const void* bytes, size_t size) override;
        Result End() override;

    private:
        std::wstring m_path;
        wil::unique_hfile m_file;
        int64_t m_mtime = 0, m_atime = 0;
        bool m_done = false;
    };

    // Feeds entries into another archive (Convert). Entries whose size is not known up front (a raw
    // .gz) are spooled to a delete-on-close file next to the output first, because tar and 7z need
    // the size in the header.
    class WriteSink : public IEntrySink
    {
    public:
        WriteSink(ArchiveWriter& writer, WriteFormat format, std::wstring spoolFolder);
        Result Begin(const Entry& entry) override;
        Result Data(const void* bytes, size_t size) override;
        Result End() override;

    private:
        ArchiveWriter& m_writer;
        WriteFormat m_format;
        std::wstring m_spoolFolder;
        Entry m_entry;
        bool m_skip = false;
        wil::unique_hfile m_spool;
        uint64_t m_spooled = 0;
    };

    // Sets a file's or folder's times from FILETIME ticks (0 = leave as is).
    void SetTimes(HANDLE handle, int64_t mtime, int64_t atime);
}
