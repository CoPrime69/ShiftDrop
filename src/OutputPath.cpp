#include "OutputPath.h"

#include <shlwapi.h>

#include <cwctype>
#include <filesystem>
#include <format>

namespace sd
{
    std::wstring LowerExtension(std::wstring_view path)
    {
        auto ext = std::filesystem::path(path).extension().wstring();
        for (auto& c : ext)
            c = static_cast<wchar_t>(std::towlower(c));
        return ext;
    }

    HRESULT CreateUniqueOutput(const std::wstring& source, std::wstring_view newExtension,
                               std::wstring& outputPath, Microsoft::WRL::ComPtr<IStream>& stream)
    {
        const std::filesystem::path src(source);
        const auto folder = src.parent_path();
        const auto stem = src.stem().wstring();

        for (int n = 1; n < 1000; ++n)
        {
            const auto name = n == 1 ? std::format(L"{} (converted){}", stem, newExtension)
                                     : std::format(L"{} (converted {}){}", stem, n, newExtension);
            const auto candidate = (folder / name).wstring();
            // fCreate=TRUE + STGM_FAILIFTHERE: create a new file, fail if the name exists. Never overwrites.
            const HRESULT hr = SHCreateStreamOnFileEx(candidate.c_str(), STGM_WRITE | STGM_SHARE_DENY_WRITE | STGM_FAILIFTHERE,
                                                      FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, stream.ReleaseAndGetAddressOf());
            if (SUCCEEDED(hr))
            {
                outputPath = candidate;
                return S_OK;
            }
            if (hr != HRESULT_FROM_WIN32(ERROR_FILE_EXISTS) && hr != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
                return hr;   // e.g. access denied (read-only folder): report it, do not loop
        }
        return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
    }
}
