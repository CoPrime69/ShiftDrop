#include "OutputPath.h"
#include "PathUtil.h"
#include "Strings.h"

#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <mutex>
#include <random>
#include <vector>

namespace sd
{
    std::wstring LowerExtension(std::wstring_view path)
    {
        return Lower(std::filesystem::path(path).extension().wstring());
    }

    namespace
    {
        std::atomic<OutputNaming> g_naming{ OutputNaming::Parentheses };
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
        if (suffix.empty())
        {
            // A name of its own ("Archive.zip"): numbered when taken, in the style's own way.
            if (n <= 1)
                return std::format(L"{}{}", stem, extension);
            if (naming == OutputNaming::Dash || naming == OutputNaming::Underscore)
                return std::format(L"{}{}{}{}", stem, naming == OutputNaming::Dash ? L'-' : L'_', n, extension);
            return std::format(L"{} ({}){}", stem, n, extension);
        }
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
    }

    void RedirectOutputs(const std::wstring& folder, const std::wstring& outputFolder)
    {
        const auto key = TrimSlash(folder);
        std::scoped_lock lock(g_redirectMutex);
        std::erase_if(g_redirects, [&](const Redirect& r) {
            return EqualsNoCase(r.folder, key);
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
                if (IsWithin(folder, r.folder, true))   // the folder itself or anything inside it
                {
                    folder = r.output;
                    break;
                }
        }
        if (naming != OutputNaming::Subfolder || EqualsNoCase(FileName(folder), kOutputSubfolder))
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
        return IsFolder(folder) ? S_OK : HRESULT_FROM_WIN32(ERROR_DIRECTORY);
    }

    HRESULT CreateStaging(const std::wstring& source, std::wstring& stagingPath, Microsoft::WRL::ComPtr<IStream>& stream)
    {
        const std::filesystem::path src(source);
        const std::filesystem::path folder(OutputFolder(source));
        if (CurrentOutputNaming() == OutputNaming::Subfolder)
            if (const HRESULT hr = MakeOutputFolder(folder.wstring()); FAILED(hr))
                return hr;
        for (int attempt = 0; attempt < 16; ++attempt)
        {
            const auto candidate = (folder / StagingName(src.stem().wstring())).wstring();
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

    std::wstring StagingName(std::wstring_view stem)
    {
        thread_local std::mt19937 rng{ std::random_device{}() };
        return std::format(L"~{}.{:08x}.sdpart", stem, static_cast<unsigned>(rng()));
    }

    HRESULT CommitStaging(const std::wstring& stagingPath, const std::wstring& source, std::wstring_view suffix,
                          std::wstring_view extension, std::wstring& finalPath)
    {
        return RenameToFreeName(stagingPath, CurrentOutputNaming(), std::filesystem::path(source).stem().wstring(), suffix, extension, finalPath);
    }

    HRESULT RenameToFreeName(const std::wstring& stagingPath, OutputNaming naming, std::wstring_view stem, std::wstring_view suffix,
                             std::wstring_view extension, std::wstring& finalPath)
    {
        // The staging file's folder, not OutputFolder again: the rename must stay in one folder even
        // if the naming style changed while the output was being written.
        const auto folder = std::filesystem::path(stagingPath).parent_path();
        for (int n = 1; n < 1000; ++n)
        {
            const auto candidate = (folder / OutputName(naming, stem, suffix, n, extension)).wstring();
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
