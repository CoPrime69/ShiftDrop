#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// Small file-system helpers every area shares, next to OutputPath.h (which names outputs). Paths are
// Win32 paths; names compare ignoring case, as NTFS does.
namespace sd
{
    // "photo.jpg" of "C:\pics\photo.jpg"; empty for a drive root ("D:\").
    std::wstring FileName(std::wstring_view path);

    // Is there anything at `path`? A folder, or a file (any kind)?
    bool PathExists(const std::wstring& path);
    bool IsFolder(const std::wstring& path);
    bool IsFile(const std::wstring& path);

    // A file's size in bytes, or 0 when it can't be read.
    uint64_t FileSize(const std::wstring& path);
    // The 64-bit size in a WIN32_FIND_DATAW, WIN32_FILE_ATTRIBUTE_DATA or FILEDESCRIPTORW.
    template <class Data>
    uint64_t FileSizeOf(const Data& data)
    {
        return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    }

    // Is `path` inside `folder` (at any depth)? `allowSame`: `folder` itself counts too.
    bool IsWithin(std::wstring_view path, std::wstring_view folder, bool allowSame = false);

    // This process's own exe (any length: the manifest is longPathAware). Empty on error.
    std::wstring ThisExe();
    // A helper exe: the path in the environment variable `envVar` (tests and developers point it at
    // their own copy), else "engines\<name>" next to this exe. Empty when that file is not there.
    std::wstring EngineExe(const wchar_t* envVar, std::wstring_view name);
}
