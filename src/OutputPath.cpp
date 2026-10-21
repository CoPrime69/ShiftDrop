#include "OutputPath.h"

#include <shlwapi.h>

#include <cwctype>
#include <filesystem>
#include <format>
#include <mutex>
#include <random>
#include <vector>

namespace sd
{
    std::wstring LowerExtension(std::wstring_view path)
    {
        auto ext = std::filesystem::path(path).extension().wstring();
        for (auto& c : ext)
            c = static_cast<wchar_t>(std::towlower(c));
        return ext;
    }

    std::wstring OutputName(std::wstring_view stem, std::wstring_view suffix, int n, std::wstring_view extension)
    {
        return n <= 1 ? std::format(L"{} ({}){}", stem, suffix, extension)
                      : std::format(L"{} ({} {}){}", stem, suffix, n, extension);
    }

    namespace
    {
        struct Redirect
        {
            std::wstring folder;   // no trailing backslash
            std::wstring output;
        };
        std::mutex g_redirectMutex;
        std::vector<Redirect> g_redirects;

        std::wstring TrimSlash(std::wstring path)
        {
            while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/'))
                path.pop_back();
            return path;
        }

        // `path` is `folder` or inside it, ignoring case as NTFS does.
        bool IsWithin(const std::wstring& path, const std::wstring& folder)
        {
            const int n = static_cast<int>(folder.size());
            if (folder.empty() || path.size() < folder.size() ||
                CompareStringOrdinal(path.c_str(), n, folder.c_str(), n, TRUE) != CSTR_EQUAL)
                return false;
            return path.size() == folder.size() || path[folder.size()] == L'\\' || path[folder.size()] == L'/';
        }
    }

    void RedirectOutputs(const std::wstring& folder, const std::wstring& outputFolder)
    {
        const auto key = TrimSlash(folder);
        std::scoped_lock lock(g_redirectMutex);
        std::erase_if(g_redirects, [&](const Redirect& r) {
            return CompareStringOrdinal(r.folder.c_str(), -1, key.c_str(), -1, TRUE) == CSTR_EQUAL;
        });
        if (!outputFolder.empty())
            g_redirects.push_back(Redirect{ key, TrimSlash(outputFolder) });
    }

    std::wstring OutputFolder(const std::wstring& source)
    {
        auto parent = std::filesystem::path(source).parent_path().wstring();
        std::scoped_lock lock(g_redirectMutex);
        for (const auto& r : g_redirects)
            if (IsWithin(parent, r.folder))
                return r.output;
        return parent;
    }

    HRESULT CreateStaging(const std::wstring& source, std::wstring& stagingPath, Microsoft::WRL::ComPtr<IStream>& stream)
    {
        const std::filesystem::path src(source);
        const std::filesystem::path folder(OutputFolder(source));
        thread_local std::mt19937 rng{ std::random_device{}() };
        for (int attempt = 0; attempt < 16; ++attempt)
        {
            const auto name = std::format(L"~{}.{:08x}.sdpart", src.stem().wstring(), static_cast<unsigned>(rng()));
            const auto candidate = (folder / name).wstring();
            // fCreate=TRUE + STGM_FAILIFTHERE: only ever creates a new file.
            const HRESULT hr = SHCreateStreamOnFileEx(candidate.c_str(), STGM_WRITE | STGM_SHARE_DENY_WRITE | STGM_FAILIFTHERE,
                                                      FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, stream.ReleaseAndGetAddressOf());
            if (SUCCEEDED(hr))
            {
                stagingPath = candidate;
                return S_OK;
            }
            if (hr != HRESULT_FROM_WIN32(ERROR_FILE_EXISTS) && hr != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
                return hr;   // e.g. access denied in a read-only folder
        }
        return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
    }

    HRESULT CommitStaging(const std::wstring& stagingPath, const std::wstring& source, std::wstring_view suffix,
                          std::wstring_view extension, std::wstring& finalPath)
    {
        const std::filesystem::path src(source);
        const auto stem = src.stem().wstring();
        const std::filesystem::path folder(OutputFolder(source));
        for (int n = 1; n < 1000; ++n)
        {
            const auto candidate = (folder / OutputName(stem, suffix, n, extension)).wstring();
            if (MoveFileExW(stagingPath.c_str(), candidate.c_str(), MOVEFILE_WRITE_THROUGH))
            {
                finalPath = candidate;
                return S_OK;
            }
            const DWORD err = GetLastError();
            if (err != ERROR_ALREADY_EXISTS && err != ERROR_FILE_EXISTS)
                return HRESULT_FROM_WIN32(err);
        }
        return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
    }
}
