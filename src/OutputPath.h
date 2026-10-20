#pragma once
#include <windows.h>
#include <objidl.h>
#include <wrl/client.h>

#include <string>
#include <string_view>

namespace sd
{
    // Lower-case extension of a path, including the dot (".jpg"), or empty.
    std::wstring LowerExtension(std::wstring_view path);

    // "photo (converted).png" for n == 1, "photo (converted 2).png" for n == 2, ...
    std::wstring OutputName(std::wstring_view stem, std::wstring_view suffix, int n, std::wstring_view extension);

    // Output is written in two steps so that a crash or kill never leaves a half-written file under
    // the final name (ADR 0003):
    //  1. CreateStaging: a new file "~photo.1a2b3c4d.sdpart" next to the source, opened for writing.
    //  2. CommitStaging: rename it to the first free "photo (suffix[ n]).ext". MoveFileEx without
    //     MOVEFILE_REPLACE_EXISTING fails if the name exists, so we never overwrite, even in a race,
    //     and a rename within one folder is atomic.
    HRESULT CreateStaging(const std::wstring& source, std::wstring& stagingPath, Microsoft::WRL::ComPtr<IStream>& stream);
    HRESULT CommitStaging(const std::wstring& stagingPath, const std::wstring& source, std::wstring_view suffix,
                          std::wstring_view extension, std::wstring& finalPath);
}
