#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Whole-file reads and writes for every converter. Reads open the file with full sharing (a document
// open in Word can still be read) and stop at a size cap; both go in 16 MB chunks. On failure they
// return false, set `error` (when given) to a short readable reason and leave the Win32 error in
// GetLastError() (ERROR_FILE_TOO_LARGE over the cap), so HRESULT callers can pass it on.
namespace sd
{
    inline constexpr uint64_t kMaxReadBytes = 1ull << 31;   // 2 GB

    bool ReadFileBytes(const std::wstring& path, std::string& bytes, std::wstring* error = nullptr, uint64_t maxBytes = kMaxReadBytes);
    bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& bytes, std::wstring* error = nullptr, uint64_t maxBytes = kMaxReadBytes);

    enum class WriteMode
    {
        Existing,   // replace the contents of a file that is there (a staging file); never creates one
        New,        // a new file; fails if the name is taken
        Any,        // either, then flushed to disk
    };
    bool WriteFileBytes(const std::wstring& path, std::string_view bytes, WriteMode mode, std::wstring* error = nullptr);
    bool WriteFileBytes(const std::wstring& path, std::span<const uint8_t> bytes, WriteMode mode, std::wstring* error = nullptr);
}
