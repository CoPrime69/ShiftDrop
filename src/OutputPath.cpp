#include "OutputPath.h"

#include <shlwapi.h>

#include <algorithm>
#include <atomic>
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

    namespace
    {
        std::atomic<OutputNaming> g_naming{ OutputNaming::Parentheses };

        bool SameName(std::wstring_view a, std::wstring_view b)
        {
            return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
        }
    }

    void SetOutputNaming(OutputNaming naming)
    {
        g_naming = naming;
    }

    OutputNaming CurrentOutputNaming()
    {
        return g_naming.load();
    }

    std::wstring OutputName(std::wstring_view stem, std::wstring_view suffix, int n, std::wstring_view extension)
    {
        return OutputName(CurrentOutputNaming(), stem, suffix, n, extension);
    }

    std::wstring OutputName(OutputNaming naming, std::wstring_view stem, std::wstring_view suffix, int n, std::wstring_view extension)
    {
        switch (naming)
        {
        case OutputNaming::Dash:
        case OutputNaming::Underscore:
        {
            const wchar_t sep = naming == OutputNaming::Dash ? L'-' : L'_';
            std::wstring joined(suffix);
            std::replace(joined.begin(), joined.end(), L' ', sep);
            return n <= 1 ? std::format(L"{}{}{}{}", stem, sep, joined, extension)
                          : std::format(L"{}{}{}{}{}{}", stem, sep, joined, sep, n, extension);
        }
        case OutputNaming::Subfolder:
            // The folder says where it came from; a second output of the same name is numbered like
            // Explorer numbers copies.
            return n <= 1 ? std::format(L"{}{}", stem, extension) : std::format(L"{} ({}){}", stem, n, extension);
        default:
            return n <= 1 ? std::format(L"{} ({}){}", stem, suffix, extension)
                          : std::format(L"{} ({} {}){}", stem, suffix, n, extension);
        }
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
        return OutputFolder(CurrentOutputNaming(), source);
    }

    std::wstring OutputFolder(OutputNaming naming, const std::wstring& source)
    {
        auto folder = std::filesystem::path(source).parent_path().wstring();
        {
            std::scoped_lock lock(g_redirectMutex);
            for (const auto& r : g_redirects)
                if (IsWithin(folder, r.folder))
                {
                    folder = r.output;
                    break;
                }
        }
        if (naming != OutputNaming::Subfolder || SameName(std::filesystem::path(folder).filename().wstring(), kOutputSubfolder))
            return folder;
        return (std::filesystem::path(folder) / kOutputSubfolder).wstring();
    }

    HRESULT MakeOutputFolder(const std::wstring& folder)
    {
        if (CreateDirectoryW(folder.c_str(), nullptr))
            return S_OK;
        const DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS)
            return HRESULT_FROM_WIN32(err);
        // A file of that name is in the way: it is never replaced.
        const DWORD attributes = GetFileAttributesW(folder.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) ? S_OK : HRESULT_FROM_WIN32(ERROR_DIRECTORY);
    }

    HRESULT CreateStaging(const std::wstring& source, std::wstring& stagingPath, Microsoft::WRL::ComPtr<IStream>& stream)
    {
        const std::filesystem::path src(source);
        const std::filesystem::path folder(OutputFolder(source));
        if (CurrentOutputNaming() == OutputNaming::Subfolder)
            if (const HRESULT hr = MakeOutputFolder(folder.wstring()); FAILED(hr))
                return hr;
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
        // The staging file's folder, not OutputFolder again: the rename must stay in one folder even
        // if the naming style changed while the output was being written.
        const auto folder = std::filesystem::path(stagingPath).parent_path();
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
