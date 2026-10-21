#include "archive/ArchiveWriter.h"
#include "PathUtil.h"
#include "Strings.h"

#include <windows.h>
#include <archive.h>
#include <archive_entry.h>
#include <wil/resource.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <memory>
#include <set>

namespace sd::archive
{
    namespace
    {
        std::wstring Message(struct archive* a, const wchar_t* what)
        {
            std::wstring detail;
            if (const char* text = archive_error_string(a))
                for (const char* p = text; *p; ++p)
                    detail += static_cast<wchar_t>(static_cast<unsigned char>(*p));
            return detail.empty() ? std::wstring(what) : std::format(L"{}: {}", what, detail);
        }

        int64_t Ticks(const FILETIME& ft)
        {
            return static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
        }
    }

    bool IsCompressedAlready(const std::wstring& name)
    {
        // Photos, video, music and other archives (Office files and EPUBs are ZIPs too): Deflate
        // spends a second per 30 MB or so on them to save a percent or two.
        static constexpr std::wstring_view kPacked[] = {
            L".jpg", L".jpeg", L".png", L".gif", L".webp", L".heic", L".heif", L".avif", L".jxl", L".mp4", L".m4v", L".mov", L".mkv",
            L".webm", L".avi", L".wmv", L".3gp", L".mp3", L".m4a", L".aac", L".ogg", L".opus", L".flac", L".wma", L".zip", L".7z",
            L".rar", L".gz", L".tgz", L".bz2", L".xz", L".zst", L".cab", L".docx", L".xlsx", L".pptx", L".odt", L".ods", L".odp",
            L".epub", L".jar", L".apk",
        };
        const auto dot = name.find_last_of(L"./");
        if (dot == std::wstring::npos || name[dot] != L'.')
            return false;
        const auto ext = Lower(name.substr(dot));
        return std::find(std::begin(kPacked), std::end(kPacked), ext) != std::end(kPacked);
    }

    ArchiveWriter::~ArchiveWriter()
    {
        if (m_a)
            archive_write_free(m_a);   // also closes the file if Close was never called
    }

    Result ArchiveWriter::Open(const std::wstring& path, WriteFormat format)
    {
        m_format = format;
        m_a = archive_write_new();
        if (!m_a)
            return Fail(Status::Io, L"out of memory");
        int r = ARCHIVE_OK;
        switch (format)
        {
        case WriteFormat::Zip:
            r = archive_write_set_format_zip(m_a);
            // UTF-8 names with the "language encoding" flag (bit 11), which Explorer, 7-Zip and macOS
            // all read; without it names would be written in this PC's ANSI code page.
            archive_write_set_format_option(m_a, "zip", "hdrcharset", "UTF-8");
            archive_write_set_format_option(m_a, "zip", "compression", "deflate");
            break;
        case WriteFormat::TarGz:
            // pax_restricted: plain ustar headers unless a name or size needs a pax extension (UTF-8).
            r = archive_write_set_format_pax_restricted(m_a);
            if (r == ARCHIVE_OK)
                r = archive_write_add_filter_gzip(m_a);
            break;
        case WriteFormat::SevenZip:
            r = archive_write_set_format_7zip(m_a);
            archive_write_set_format_option(m_a, "7zip", "compression", "lzma2");
            break;
        }
        if (r != ARCHIVE_OK)
            return Fail(Status::Io, Message(m_a, L"cannot set up the archive format"));
        if (format != WriteFormat::TarGz)
            archive_write_set_bytes_in_last_block(m_a, 1);   // no padding: only tar wants 10 KiB blocks
        if (archive_write_open_filename_w(m_a, path.c_str()) != ARCHIVE_OK)
            return Fail(Status::Io, Message(m_a, L"cannot write the archive"));
        return {};
    }

    Result ArchiveWriter::Add(const Entry& entry)
    {
        std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)> e(archive_entry_new(), &archive_entry_free);
        archive_entry_copy_pathname_w(e.get(), entry.name.c_str());
        switch (entry.type)
        {
        case EntryType::Directory:
            archive_entry_set_filetype(e.get(), AE_IFDIR);
            archive_entry_set_perm(e.get(), 0755);
            archive_entry_set_size(e.get(), 0);
            break;
        case EntryType::Hardlink:
            archive_entry_set_filetype(e.get(), AE_IFREG);
            archive_entry_set_perm(e.get(), 0644);
            archive_entry_copy_hardlink_w(e.get(), entry.link.c_str());
            archive_entry_set_size(e.get(), 0);
            break;
        default:
            archive_entry_set_filetype(e.get(), AE_IFREG);
            archive_entry_set_perm(e.get(), 0644);
            archive_entry_set_size(e.get(), entry.size < 0 ? 0 : entry.size);
            // A ZIP stores what is compressed already as it is: the same files come out, much sooner.
            if (m_format == WriteFormat::Zip)
            {
                if (IsCompressedAlready(entry.name))
                    archive_write_zip_set_compression_store(m_a);
                else
                    archive_write_zip_set_compression_deflate(m_a);
            }
            break;
        }
        if (entry.mtime)
        {
            int64_t seconds = 0;
            long nanoseconds = 0;
            UnixFromTicks(entry.mtime, seconds, nanoseconds);
            archive_entry_set_mtime(e.get(), seconds, nanoseconds);
        }
        if (entry.atime)
        {
            int64_t seconds = 0;
            long nanoseconds = 0;
            UnixFromTicks(entry.atime, seconds, nanoseconds);
            archive_entry_set_atime(e.get(), seconds, nanoseconds);
        }
        if (archive_write_header(m_a, e.get()) < ARCHIVE_WARN)
            return Fail(Status::Io, Message(m_a, L"cannot add to the archive"));
        return {};
    }

    Result ArchiveWriter::Write(const void* bytes, size_t size)
    {
        const auto* p = static_cast<const char*>(bytes);
        while (size > 0)
        {
            const la_ssize_t n = archive_write_data(m_a, p, size);
            if (n <= 0)
                return Fail(Status::Io, Message(m_a, L"cannot write the archive"));
            p += n;
            size -= static_cast<size_t>(n);
        }
        return {};
    }

    Result ArchiveWriter::Close()
    {
        const int r = archive_write_close(m_a);
        Result result = r == ARCHIVE_OK ? Result{} : Fail(Status::Io, Message(m_a, L"cannot finish the archive"));
        archive_write_free(m_a);
        m_a = nullptr;
        return result;
    }

    namespace
    {
        struct Item
        {
            std::wstring path;   // on disk
            std::wstring name;   // in the archive
            bool directory = false;
            uint64_t size = 0;
            int64_t mtime = 0;
            int64_t atime = 0;
        };

        void Walk(const std::wstring& folder, const std::wstring& prefix, std::vector<Item>& out)
        {
            WIN32_FIND_DATAW data{};
            wil::unique_hfind find(FindFirstFileExW(LongPath(folder + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                                    nullptr, FIND_FIRST_EX_LARGE_FETCH));
            if (!find)
                return;
            do
            {
                const std::wstring name = data.cFileName;
                if (name == L"." || name == L"..")
                    continue;
                // Symlinks and junctions are not followed: they can point anywhere, even at a parent.
                if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    continue;
                Item item;
                item.path = folder + L"\\" + name;
                item.name = prefix + L"/" + name;
                item.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                item.size = item.directory ? 0 : FileSizeOf(data);
                item.mtime = Ticks(data.ftLastWriteTime);
                item.atime = Ticks(data.ftLastAccessTime);
                out.push_back(item);
                if (item.directory)
                    Walk(item.path, item.name, out);
            } while (FindNextFileW(find.get(), &data));
        }
    }

    Result Compress(const std::vector<std::wstring>& items, WriteFormat format, const std::wstring& output, std::stop_token stop,
                    const std::function<void(double)>& progress)
    {
        std::vector<Item> all;
        std::set<std::wstring> topNames;
        for (const auto& source : items)
        {
            WIN32_FILE_ATTRIBUTE_DATA info{};
            if (!GetFileAttributesExW(LongPath(source).c_str(), GetFileExInfoStandard, &info))
                return Fail(Status::Io, std::format(L"cannot read {}", FileName(source)));
            // Items from different folders can share a name: "a.txt", then "a (2).txt".
            const std::filesystem::path p(source);
            std::wstring name = p.filename().wstring();
            if (name.empty())
                name = L"Drive";
            for (int n = 2; !topNames.insert(Lower(name)).second; ++n)
                name = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? std::format(L"{} ({})", p.filename().wstring(), n)
                                                                          : std::format(L"{} ({}){}", p.stem().wstring(), n, p.extension().wstring());
            Item top;
            top.path = source;
            top.name = name;
            top.directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            top.size = top.directory ? 0 : FileSizeOf(info);
            top.mtime = Ticks(info.ftLastWriteTime);
            top.atime = Ticks(info.ftLastAccessTime);
            all.push_back(top);
            if (top.directory)
                Walk(source, name, all);
        }
        uint64_t total = 0;
        for (const auto& i : all)
            total += i.size;

        ArchiveWriter writer;
        if (auto r = writer.Open(output, format); !r.ok())
            return r;
        std::vector<char> buffer(1 << 20);
        uint64_t done = 0;
        for (const auto& item : all)
        {
            if (stop.stop_requested())
                return Fail(Status::Cancelled, L"cancelled");
            Entry e;
            e.name = item.name;
            e.type = item.directory ? EntryType::Directory : EntryType::File;
            e.size = static_cast<int64_t>(item.size);
            e.mtime = item.mtime;
            e.atime = item.atime;
            if (item.directory)
            {
                if (auto r = writer.Add(e); !r.ok())
                    return r;
                continue;
            }
            wil::unique_hfile file(CreateFileW(LongPath(item.path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
            if (!file)
                return Fail(Status::Io, std::format(L"cannot read {} ({})", item.name, GetLastError()));
            // The size now, not at enumeration: the header must match the bytes that follow.
            LARGE_INTEGER size{};
            GetFileSizeEx(file.get(), &size);
            e.size = size.QuadPart;
            if (auto r = writer.Add(e); !r.ok())
                return r;
            int64_t left = e.size;
            while (left > 0)
            {
                if (stop.stop_requested())
                    return Fail(Status::Cancelled, L"cancelled");
                DWORD read = 0;
                const DWORD want = static_cast<DWORD>(std::min<int64_t>(left, static_cast<int64_t>(buffer.size())));
                if (!ReadFile(file.get(), buffer.data(), want, &read, nullptr) || read == 0)
                    return Fail(Status::Io, std::format(L"cannot read {} ({})", item.name, GetLastError()));
                if (auto r = writer.Write(buffer.data(), read); !r.ok())
                    return r;
                left -= read;
                done += read;
                if (progress && total > 0)
                    progress(std::min(1.0, static_cast<double>(done) / static_cast<double>(total)));
            }
        }
        if (stop.stop_requested())
            return Fail(Status::Cancelled, L"cancelled");
        return writer.Close();
    }
}
