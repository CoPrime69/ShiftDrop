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

    // Picks "name (converted).ext", then "name (converted 2).ext", ... in the source's folder and opens
    // it with a stream that is only ever *created*, never truncated: if another process creates the
    // same name between our check and our open, the open fails and we try the next number.
    HRESULT CreateUniqueOutput(const std::wstring& source, std::wstring_view newExtension,
                               std::wstring& outputPath, Microsoft::WRL::ComPtr<IStream>& stream);
}
