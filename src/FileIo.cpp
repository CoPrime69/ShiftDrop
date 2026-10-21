#include "FileIo.h"
#include "PathUtil.h"

#include <windows.h>
#include <wil/resource.h>

#include <algorithm>
#include <format>

namespace sd
{
    namespace
    {
        constexpr size_t kChunk = 1u << 24;

        bool Fail(std::wstring* error, std::wstring text, DWORD code)
        {
            if (error)
                *error = std::move(text);
            SetLastError(code);
            return false;
        }

        template <class Bytes>
        bool Read(const std::wstring& path, Bytes& bytes, std::wstring* error, uint64_t maxBytes)
        {
            wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                               OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
            if (!file)
            {
                const DWORD code = GetLastError();
                return Fail(error, std::format(L"cannot open {} (error {})", path, code), code);
            }
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file.get(), &size))
            {
                const DWORD code = GetLastError();
                return Fail(error, std::format(L"cannot read {} (error {})", path, code), code);
            }
            if (static_cast<uint64_t>(size.QuadPart) > maxBytes)
            {
                const auto cap = maxBytes % (1ull << 30) == 0 ? std::format(L"{} GB", maxBytes >> 30) : std::format(L"{} MB", maxBytes >> 20);
                return Fail(error, std::format(L"file is too large (over {})", cap), ERROR_FILE_TOO_LARGE);
            }
            bytes.resize(static_cast<size_t>(size.QuadPart));
            size_t done = 0;
            while (done < bytes.size())
            {
                DWORD read = 0;
                const DWORD chunk = static_cast<DWORD>(std::min(bytes.size() - done, kChunk));
                if (!ReadFile(file.get(), bytes.data() + done, chunk, &read, nullptr) || read == 0)
                {
                    const DWORD code = read == 0 && GetLastError() == ERROR_SUCCESS ? ERROR_HANDLE_EOF : GetLastError();
                    return Fail(error, std::format(L"cannot read {}", path), code);
                }
                done += read;
            }
            return true;
        }

        bool Write(const std::wstring& path, const void* data, size_t size, WriteMode mode, std::wstring* error)
        {
            // Existing is TRUNCATE_EXISTING, not CREATE_ALWAYS: a staging file is already there
            // (StagedOutput reserved it), and this must never create a file under any other name.
            const DWORD disposition = mode == WriteMode::Existing ? TRUNCATE_EXISTING : mode == WriteMode::New ? CREATE_NEW : OPEN_ALWAYS;
            wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!file)
            {
                const DWORD code = GetLastError();
                return Fail(error,
                            mode == WriteMode::New ? std::format(L"cannot write {} (error {})", FileName(path), code)
                                                   : std::format(L"cannot write the output (error {})", code),
                            code);
            }
            const auto* bytes = static_cast<const char*>(data);
            size_t done = 0;
            while (done < size)
            {
                DWORD written = 0;
                const DWORD chunk = static_cast<DWORD>(std::min(size - done, kChunk));
                if (!WriteFile(file.get(), bytes + done, chunk, &written, nullptr) || written == 0)
                {
                    const DWORD code = GetLastError();
                    return Fail(error, std::format(L"cannot write the output (error {})", code), code);
                }
                done += written;
            }
            // OPEN_ALWAYS keeps an old file's length: cut what is past the new end.
            if (mode == WriteMode::Any && (!SetEndOfFile(file.get()) || !FlushFileBuffers(file.get())))
            {
                const DWORD code = GetLastError();
                return Fail(error, std::format(L"cannot write the output (error {})", code), code);
            }
            return true;
        }
    }

    bool ReadFileBytes(const std::wstring& path, std::string& bytes, std::wstring* error, uint64_t maxBytes)
    {
        return Read(path, bytes, error, maxBytes);
    }

    bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& bytes, std::wstring* error, uint64_t maxBytes)
    {
        return Read(path, bytes, error, maxBytes);
    }

    bool WriteFileBytes(const std::wstring& path, std::string_view bytes, WriteMode mode, std::wstring* error)
    {
        return Write(path, bytes.data(), bytes.size(), mode, error);
    }

    bool WriteFileBytes(const std::wstring& path, std::span<const uint8_t> bytes, WriteMode mode, std::wstring* error)
    {
        return Write(path, bytes.data(), bytes.size(), mode, error);
    }
}
