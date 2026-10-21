#include "StagedOutput.h"
#include "ErrorText.h"
#include "Hash.h"
#include "Log.h"
#include "OutputPath.h"
#include "Strings.h"

#include <filesystem>
#include <format>

namespace sd
{
    namespace
    {
        // "~video.1a2b3c4d.sdpart" as a folder, created new (never an existing one).
        bool CreateStagingFolder(const std::wstring& source, std::wstring& path)
        {
            const std::filesystem::path src(source);
            const std::filesystem::path dir(OutputFolder(source));
            if (CurrentOutputNaming() == OutputNaming::Subfolder && FAILED(MakeOutputFolder(dir.wstring())))
                return false;
            for (int attempt = 0; attempt < 16; ++attempt)
            {
                const auto candidate = (dir / StagingName(src.stem().wstring())).wstring();
                if (CreateDirectoryW(candidate.c_str(), nullptr))
                {
                    path = candidate;
                    return true;
                }
                if (GetLastError() != ERROR_ALREADY_EXISTS)
                    return false;
            }
            return false;
        }
    }

    StagedOutput::StagedOutput(HistoryStore* history, int64_t runId, const std::wstring& source, bool folder, const OutputPlace& place)
        : m_history(history), m_source(source), m_namedAfter(source), m_folder(folder), m_started(GetTickCount64())
    {
        if (!place.folder.empty() || !place.stem.empty())
        {
            // OutputPath names outputs after a path's folder and stem; a made-up path with a dummy
            // extension gives exactly the stem asked for, dots and all ("photos.2024"). A folder inside
            // a drop folder stays redirected (OutputFolder), so outputs of dragged-in files still land
            // where the user chose.
            const std::filesystem::path src(source);
            const auto dir = place.folder.empty() ? src.parent_path() : std::filesystem::path(place.folder);
            m_namedAfter = (dir / ((place.stem.empty() ? src.stem().wstring() : place.stem) + L".x")).wstring();
        }
        if (folder)
        {
            if (!CreateStagingFolder(m_namedAfter, m_path))
                m_error = std::format(L"cannot write next to the source: {}", HResultText(HRESULT_FROM_WIN32(GetLastError())));
        }
        else
        {
            Microsoft::WRL::ComPtr<IStream> reserve;   // reserves a unique name; closed so the engine can open it
            if (const HRESULT hr = CreateStaging(m_namedAfter, m_path, reserve); FAILED(hr))
            {
                m_path.clear();
                m_error = std::format(L"cannot write next to the source: {}", HResultText(hr));
            }
        }
        if (!m_path.empty() && m_history)
            m_item = m_history->BeginItem(runId, source, m_path);
    }

    StagedOutput::~StagedOutput()
    {
        if (!m_done)
            Discard();
    }

    void StagedOutput::Discard()
    {
        if (!m_path.empty())
            RemoveStaging(m_path);
    }

    ItemResult StagedOutput::Commit(std::wstring_view suffix, std::wstring_view extension)
    {
        ItemResult result;
        if (m_path.empty())
            return Fail(m_error.empty() ? L"no staging output" : m_error);
        std::wstring finalPath;
        if (const HRESULT hr = CommitStaging(m_path, m_namedAfter, suffix, extension, finalPath); FAILED(hr))
            return Fail(std::format(L"commit: {}", HResultText(hr)));
        m_done = true;
        result.ok = true;
        result.output = finalPath;
        std::string sha;
        if (!m_folder)
            sha = Sha256File(finalPath, &result.size);
        if (m_history)
            m_history->FinishItem(m_item, status::Done, finalPath, result.size, sha, L"", static_cast<int64_t>(GetTickCount64() - m_started));
        return result;
    }

    ItemResult StagedOutput::Fail(std::wstring error, bool cancelled)
    {
        ItemResult result;
        Discard();
        m_done = true;
        result.error = std::move(error);
        result.skipped = cancelled;
        if (m_history && m_item)
            m_history->FinishItem(m_item, cancelled ? status::Skipped : status::Failed, L"", 0, "", result.error,
                                  static_cast<int64_t>(GetTickCount64() - m_started));
        return result;
    }

    bool RemoveStaging(const std::wstring& path)
    {
        // Only ever our own "~name.xxxxxxxx.sdpart": never follow anything else.
        if (!path.ends_with(L".sdpart"))
            return false;
        std::error_code ec;
        const bool removed = std::filesystem::is_directory(path, ec) ? std::filesystem::remove_all(path, ec) > 0 && !ec
                                                                      : DeleteFileW(path.c_str()) != FALSE;
        // A "ShiftDrop" output folder made for this output alone goes too (RemoveDirectory only
        // removes an empty folder).
        const auto parent = std::filesystem::path(path).parent_path();
        if (removed && CurrentOutputNaming() == OutputNaming::Subfolder && EqualsNoCase(parent.filename().wstring(), kOutputSubfolder))
            RemoveDirectoryW(parent.c_str());
        return removed;
    }

    ItemResult RecordFailure(HistoryStore* history, int64_t runId, const std::wstring& source, std::wstring error, bool skipped)
    {
        if (history)
        {
            const auto item = history->BeginItem(runId, source, L"");
            history->FinishItem(item, skipped ? status::Skipped : status::Failed, L"", 0, "", error, 0);
        }
        ItemResult r;
        r.skipped = skipped;
        r.error = std::move(error);
        return r;
    }

    void RecordDone(HistoryStore* history, int64_t runId, const std::wstring& source)
    {
        if (history)
        {
            const auto item = history->BeginItem(runId, source, L"");
            history->FinishItem(item, status::Done, L"", 0, "", L"", 0);
        }
    }
}
