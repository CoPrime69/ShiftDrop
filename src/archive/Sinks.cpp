#include "archive/Sinks.h"

#include <cwctype>
#include <filesystem>
#include <format>
#include <random>

namespace sd::archive
{
    namespace
    {
        std::wstring Lower(std::wstring s)
        {
            for (auto& c : s)
                c = static_cast<wchar_t>(std::towlower(c));
            return s;
        }

        std::vector<std::wstring> Split(const std::wstring& name)
        {
            std::vector<std::wstring> parts;
            size_t start = 0;
            while (start <= name.size())
            {
                const size_t end = std::min(name.find(L'/', start), name.size());
                if (end > start)
                    parts.push_back(name.substr(start, end - start));
                start = end + 1;
            }
            return parts;
        }

        FILETIME ToFileTime(int64_t ticks)
        {
            FILETIME ft{};
            ft.dwLowDateTime = static_cast<DWORD>(static_cast<uint64_t>(ticks) & 0xFFFFFFFFu);
            ft.dwHighDateTime = static_cast<DWORD>(static_cast<uint64_t>(ticks) >> 32);
            return ft;
        }

        bool WriteAll(HANDLE h, const void* data, size_t size)
        {
            const auto* p = static_cast<const char*>(data);
            while (size > 0)
            {
                DWORD written = 0;
                if (!WriteFile(h, p, static_cast<DWORD>(std::min<size_t>(size, 1u << 20)), &written, nullptr) || written == 0)
                    return false;
                p += written;
                size -= written;
            }
            return true;
        }

        Result WriteError(const std::wstring& what)
        {
            const DWORD err = GetLastError();
            return Fail(Status::Io, err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL ? L"the disk is full"
                                                                                           : std::format(L"cannot write {} ({})", what, err));
        }

        // "name (2).ext" for the n-th file wanting the same name.
        std::wstring Numbered(const std::wstring& path, int n)
        {
            const std::filesystem::path p(path);
            return (p.parent_path() / std::format(L"{} ({}){}", p.stem().wstring(), n, p.extension().wstring())).wstring();
        }
    }

    void SetTimes(HANDLE handle, int64_t mtime, int64_t atime)
    {
        if (!mtime && !atime)
            return;
        const FILETIME m = ToFileTime(mtime), a = ToFileTime(atime ? atime : mtime);
        SetFileTime(handle, nullptr, &a, mtime ? &m : nullptr);
    }

    // ---- GuardSink ------------------------------------------------------------------------------

    GuardSink::GuardSink(IEntrySink& inner, const Limits& limits, uint64_t inputSize, std::wstring rawName)
        : m_inner(inner), m_limits(limits), m_inputSize(inputSize), m_rawName(std::move(rawName))
    {
    }

    Result GuardSink::Charge(uint64_t n)
    {
        bytes += n;
        if (bytes > m_limits.maxTotalBytes)
            return Fail(Status::Limit, std::format(L"it unpacks to more than {} GB", m_limits.maxTotalBytes >> 30));
        if (bytes > m_limits.ratioAfterBytes && m_inputSize > 0 && static_cast<double>(bytes) > m_limits.maxRatio * static_cast<double>(m_inputSize))
            return Fail(Status::Limit, L"it unpacks to far more than its own size (a zip bomb?)");
        return {};
    }

    Result GuardSink::Begin(const Entry& in)
    {
        if (++m_entries > m_limits.maxEntries)
            return Fail(Status::Limit, std::format(L"it has more than {} entries", m_limits.maxEntries));
        Entry e = in;
        m_skip = false;
        m_declared = e.type == EntryType::File ? e.size : -1;
        m_received = 0;
        if (!m_rawName.empty() && e.type == EntryType::File)
            e.name = m_rawName;

        std::vector<std::wstring> parts;
        std::wstring why;
        if (!SafeParts(e.name, parts, &why))
            return Fail(Status::Unsafe, std::format(L"it contains {} (\"{}\"), so nothing was extracted", why, e.name.substr(0, 120)));
        if (e.type == EntryType::Symlink || e.type == EntryType::Other)
        {
            // A link could point anywhere (C:\Windows, or ..\..): never created. Windows also needs
            // Developer Mode to create symlinks at all.
            ++skippedLinks;
            m_skip = true;
            return {};
        }
        if (parts.empty())
        {
            m_skip = true;   // "./" — the root itself
            return {};
        }
        e.name = JoinParts(parts, L'/');
        if (e.type == EntryType::Hardlink)
        {
            std::vector<std::wstring> target;
            if (!SafeParts(e.link, target, nullptr) || target.empty())
            {
                ++skippedLinks;
                m_skip = true;
                return {};
            }
            e.link = JoinParts(target, L'/');
        }
        if (e.type == EntryType::File || e.type == EntryType::Hardlink)
            ++files;
        else if (e.type == EntryType::Directory)
            ++folders;
        return m_inner.Begin(e);
    }

    Result GuardSink::Data(const void* data, size_t size)
    {
        if (m_skip)
            return {};
        m_received += size;
        if (m_declared >= 0 && m_received > static_cast<uint64_t>(m_declared))
            return Fail(Status::Corrupt, L"an entry is longer than its header says");
        if (auto r = Charge(size); !r.ok())
            return r;
        return m_inner.Data(data, size);
    }

    Result GuardSink::End()
    {
        if (m_skip)
        {
            m_skip = false;
            return {};
        }
        if (m_declared >= 0 && m_received != static_cast<uint64_t>(m_declared))
            return Fail(Status::Corrupt, L"an entry is shorter than its header says (truncated archive)");
        return m_inner.End();
    }

    // ---- ExtractSink ----------------------------------------------------------------------------

    ExtractSink::ExtractSink(std::wstring root, std::function<Result(uint64_t)> charge) : m_root(std::move(root)), m_charge(std::move(charge)) {}

    Result ExtractSink::MakeFolders(const std::vector<std::wstring>& parts, size_t count)
    {
        std::wstring path = m_root;
        for (size_t i = 0; i < count; ++i)
        {
            path += L"\\" + parts[i];
            const auto longPath = LongPath(path);
            if (CreateDirectoryW(longPath.c_str(), nullptr))
                continue;
            if (GetLastError() != ERROR_ALREADY_EXISTS)
                return WriteError(parts[i]);
            const DWORD attributes = GetFileAttributesW(longPath.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                return Fail(Status::Corrupt, std::format(L"it has a file and a folder both called \"{}\"", parts[i]));
        }
        return {};
    }

    Result ExtractSink::Begin(const Entry& e)
    {
        const auto parts = Split(e.name);
        if (parts.empty())
            return {};
        if (e.type == EntryType::Directory)
        {
            if (auto r = MakeFolders(parts, parts.size()); !r.ok())
                return r;
            if (e.mtime || e.atime)
                m_folders.push_back({ m_root + L"\\" + JoinParts(parts, L'\\'), { e.mtime, e.atime } });
            return {};
        }
        if (e.type != EntryType::File && e.type != EntryType::Hardlink)
            return {};
        if (auto r = MakeFolders(parts, parts.size() - 1); !r.ok())
            return r;

        const std::wstring wanted = m_root + L"\\" + JoinParts(parts, L'\\');
        std::wstring source;
        if (e.type == EntryType::Hardlink)
        {
            const auto it = m_written.find(Lower(e.link));
            if (it == m_written.end())
                return {};   // points at nothing we extracted: skipped
            source = it->second;
        }
        // Create new only; a name taken (by a file that differs only in case) gets a number.
        for (int n = 1; n < 1000; ++n)
        {
            const std::wstring path = n == 1 ? wanted : Numbered(wanted, n);
            if (GetFileAttributesW(LongPath(path).c_str()) != INVALID_FILE_ATTRIBUTES)
                continue;
            if (e.type == EntryType::Hardlink)
            {
                // Copied, not linked: a hard link would tie the output to another file's future edits.
                WIN32_FILE_ATTRIBUTE_DATA info{};
                if (GetFileAttributesExW(LongPath(source).c_str(), GetFileExInfoStandard, &info) && m_charge)
                    if (auto r = m_charge((static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow); !r.ok())
                        return r;
                if (!CopyFileW(LongPath(source).c_str(), LongPath(path).c_str(), TRUE))
                    return WriteError(parts.back());
                m_written[Lower(e.name)] = path;
                return {};
            }
            m_file.reset(CreateFileW(LongPath(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
            if (!m_file)
            {
                if (GetLastError() == ERROR_FILE_EXISTS)
                    continue;
                return WriteError(parts.back());
            }
            m_written[Lower(e.name)] = path;
            m_mtime = e.mtime;
            m_atime = e.atime;
            return {};
        }
        return Fail(Status::Corrupt, std::format(L"too many entries called \"{}\"", parts.back()));
    }

    Result ExtractSink::Data(const void* bytes, size_t size)
    {
        if (!m_file)
            return {};
        return WriteAll(m_file.get(), bytes, size) ? Result{} : WriteError(L"a file");
    }

    Result ExtractSink::End()
    {
        if (m_file)
        {
            SetTimes(m_file.get(), m_mtime, m_atime);
            m_file.reset();
        }
        return {};
    }

    void ExtractSink::Finish()
    {
        for (auto it = m_folders.rbegin(); it != m_folders.rend(); ++it)
        {
            // FILE_FLAG_BACKUP_SEMANTICS is what lets CreateFile open a folder.
            wil::unique_hfile folder(CreateFileW(LongPath(it->first).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
            if (folder)
                SetTimes(folder.get(), it->second.first, it->second.second);
        }
    }

    bool ExtractSink::FlattenSingleFolder()
    {
        std::wstring only;
        WIN32_FIND_DATAW data{};
        {
            wil::unique_hfind find(FindFirstFileExW(LongPath(m_root + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0));
            if (!find)
                return false;
            int count = 0;
            do
            {
                const std::wstring name = data.cFileName;
                if (name == L"." || name == L"..")
                    continue;
                if (++count > 1 || !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    return false;
                only = name;
            } while (FindNextFileW(find.get(), &data));
        }
        if (only.empty())
            return false;
        // Move it aside first: it may contain an entry with its own name ("Photos/Photos").
        thread_local std::mt19937 rng{ std::random_device{}() };
        const std::wstring aside = m_root + std::format(L"\\~{:08x}.sdflat", static_cast<unsigned>(rng()));
        if (!MoveFileExW(LongPath(m_root + L"\\" + only).c_str(), LongPath(aside).c_str(), 0))
            return false;
        bool ok = true;
        {
            wil::unique_hfind find(FindFirstFileExW(LongPath(aside + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0));
            if (find)
                do
                {
                    const std::wstring name = data.cFileName;
                    if (name == L"." || name == L"..")
                        continue;
                    ok &= MoveFileExW(LongPath(aside + L"\\" + name).c_str(), LongPath(m_root + L"\\" + name).c_str(), 0) != FALSE;
                } while (FindNextFileW(find.get(), &data));
        }
        if (ok)
            RemoveDirectoryW(LongPath(aside).c_str());
        else   // put back what is left under its real name rather than leave a "~....sdflat" folder
            MoveFileExW(LongPath(aside).c_str(), LongPath(m_root + L"\\" + only).c_str(), 0);
        return ok;
    }

    // ---- FileSink -------------------------------------------------------------------------------

    Result FileSink::Begin(const Entry& e)
    {
        if (e.type != EntryType::File || m_done)
            return {};
        m_file.reset(CreateFileW(LongPath(m_path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!m_file)
            return WriteError(L"the output");
        m_mtime = e.mtime;
        m_atime = e.atime;
        return {};
    }

    Result FileSink::Data(const void* bytes, size_t size)
    {
        if (!m_file)
            return {};
        return WriteAll(m_file.get(), bytes, size) ? Result{} : WriteError(L"the output");
    }

    Result FileSink::End()
    {
        if (m_file)
        {
            SetTimes(m_file.get(), m_mtime, m_atime);
            m_file.reset();
            m_done = true;
        }
        return {};
    }

    // ---- WriteSink ------------------------------------------------------------------------------

    WriteSink::WriteSink(ArchiveWriter& writer, WriteFormat format, std::wstring spoolFolder)
        : m_writer(writer), m_format(format), m_spoolFolder(std::move(spoolFolder))
    {
    }

    Result WriteSink::Begin(const Entry& e)
    {
        m_entry = e;
        m_skip = false;
        m_spooled = 0;
        // Only tar can store a hard link; ZIP and 7z have no such entry.
        if (e.type == EntryType::Hardlink && m_format != WriteFormat::TarGz)
        {
            m_skip = true;
            return {};
        }
        if (e.type != EntryType::File || e.size >= 0)
            return m_writer.Add(e);
        // Unknown size: spool. Delete-on-close, so even a crash leaves nothing behind.
        thread_local std::mt19937 rng{ std::random_device{}() };
        const auto path = m_spoolFolder + std::format(L"\\~spool.{:08x}.sdpart", static_cast<unsigned>(rng()));
        m_spool.reset(CreateFileW(LongPath(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
        return m_spool ? Result{} : WriteError(L"a temporary file");
    }

    Result WriteSink::Data(const void* bytes, size_t size)
    {
        if (m_skip)
            return {};
        if (m_spool)
        {
            m_spooled += size;
            return WriteAll(m_spool.get(), bytes, size) ? Result{} : WriteError(L"a temporary file");
        }
        return m_writer.Write(bytes, size);
    }

    Result WriteSink::End()
    {
        if (m_skip || !m_spool)
            return {};
        m_entry.size = static_cast<int64_t>(m_spooled);
        if (auto r = m_writer.Add(m_entry); !r.ok())
            return r;
        LARGE_INTEGER zero{};
        SetFilePointerEx(m_spool.get(), zero, nullptr, FILE_BEGIN);
        std::vector<char> buffer(1 << 20);
        for (uint64_t left = m_spooled; left > 0;)
        {
            DWORD read = 0;
            if (!ReadFile(m_spool.get(), buffer.data(), static_cast<DWORD>(std::min<uint64_t>(left, buffer.size())), &read, nullptr) || read == 0)
                return WriteError(L"a temporary file");
            if (auto r = m_writer.Write(buffer.data(), read); !r.ok())
                return r;
            left -= read;
        }
        m_spool.reset();
        return {};
    }
}
