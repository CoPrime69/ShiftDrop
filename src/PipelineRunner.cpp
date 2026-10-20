#include "PipelineRunner.h"
#include "Hash.h"
#include "Log.h"
#include "OutputPath.h"

#include <windows.h>
#include <wrl/client.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <format>

namespace sd
{
    PipelineRunner::PipelineRunner(std::shared_ptr<const ImageCodecs> codecs, HistoryStore* history)
        : m_codecs(std::move(codecs)), m_history(history)
    {
    }

    bool PipelineRunner::Accepts(const Recipe& recipe, const std::wstring& path, std::wstring* why) const
    {
        if (recipe.IsFileAction())
        {
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                if (why)
                    *why = L"file not found";
                return false;
            }
            // Every file action takes folders too, except Duplicate (a folder copy needs progress and
            // cancel, which arrive with the media worker in M3).
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && recipe.steps[0].action == "file.duplicate")
            {
                if (why)
                    *why = L"folders can't be duplicated yet";
                return false;
            }
            return true;
        }
        const auto ext = LowerExtension(path);
        if (!m_codecs->CanDecode(ext))
        {
            if (why)
                *why = std::format(L"no image decoder for '{}' on this PC", ext);
            return false;
        }
        const auto* format = recipe.OutputFormatFor(ext);
        if (!format || !m_codecs->CanEncode(*format))
        {
            if (why)
                *why = L"output format unavailable";
            return false;
        }
        return true;
    }

    bool PipelineRunner::IsNoOp(const Recipe& recipe, const std::wstring& path) const
    {
        const auto ext = LowerExtension(path);
        const auto* format = recipe.OutputFormatFor(ext);
        return recipe.IsSingleEncode() && format && IsAlreadyFormat(*format, ext);
    }

    ItemResult PipelineRunner::Run(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop) const
    {
        ItemResult result;
        const auto started = std::chrono::steady_clock::now();
        const std::function<int64_t()> elapsedMs = [started] {
            return static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
        };

        std::wstring why;
        if (IsNoOp(recipe, source))
        {
            result.skipped = true;
            result.error = std::format(L"already {}", recipe.OutputFormatFor(LowerExtension(source))->label);
        }
        else if (!Accepts(recipe, source, &why))
        {
            result.error = why;
        }
        if (result.skipped || !result.error.empty())
        {
            if (m_history)
            {
                const auto item = m_history->BeginItem(runId, source, L"");
                m_history->FinishItem(item, result.skipped ? status::Skipped : status::Failed, L"", 0, "", result.error, elapsedMs());
            }
            return result;
        }

        if (recipe.IsFileAction())
            return RunDuplicate(recipe, runId, source, elapsedMs);

        std::wstring staging;
        Microsoft::WRL::ComPtr<IStream> stream;
        if (const HRESULT hr = CreateStaging(source, staging, stream); FAILED(hr))
        {
            result.error = std::format(L"cannot write next to the source: {}", HResultText(hr));
            if (m_history)
            {
                const auto item = m_history->BeginItem(runId, source, L"");
                m_history->FinishItem(item, status::Failed, L"", 0, "", result.error, elapsedMs());
            }
            return result;
        }
        // Recorded before any bytes are written, so a kill from here on is cleaned up at next start.
        const int64_t item = m_history ? m_history->BeginItem(runId, source, staging) : 0;

        std::wstring error;
        HRESULT hr = RunImageRecipe(recipe, source, stream.Get(), stop, error);
        stream.Reset();   // close the handle before renaming or deleting

        std::wstring finalPath;
        if (SUCCEEDED(hr))
        {
            hr = CommitStaging(staging, source, recipe.suffix, recipe.OutputFormatFor(LowerExtension(source))->extension, finalPath);
            if (FAILED(hr))
                error = std::format(L"commit: {}", HResultText(hr));
        }
        if (FAILED(hr))
        {
            DeleteFileW(staging.c_str());
            result.error = hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) ? L"cancelled" : error;
            if (m_history)
                m_history->FinishItem(item, status::Failed, L"", 0, "", result.error, elapsedMs());
            return result;
        }

        result.ok = true;
        result.output = finalPath;
        const auto sha = Sha256File(finalPath, &result.size);
        if (m_history)
            m_history->FinishItem(item, status::Done, finalPath, result.size, sha, L"", elapsedMs());
        return result;
    }

    // file.duplicate: copy to a staging name, then the same atomic commit as every other output, so a
    // duplicate is recorded in history and can be undone like a conversion.
    ItemResult PipelineRunner::RunDuplicate(const Recipe& recipe, int64_t runId, const std::wstring& source,
                                            const std::function<int64_t()>& elapsedMs) const
    {
        ItemResult result;
        if (recipe.steps[0].action != "file.duplicate")
        {
            result.error = L"this file action runs on the UI thread";   // copy, move, copy path, terminal
            return result;
        }
        if (GetFileAttributesW(source.c_str()) & FILE_ATTRIBUTE_DIRECTORY)
        {
            result.error = L"folders can't be duplicated yet";
            return result;
        }
        std::wstring staging;
        {
            Microsoft::WRL::ComPtr<IStream> reserve;   // reserves a unique staging name
            if (const HRESULT hr = CreateStaging(source, staging, reserve); FAILED(hr))
            {
                result.error = HResultText(hr);
                return result;
            }
        }
        const int64_t item = m_history ? m_history->BeginItem(runId, source, staging) : 0;
        std::wstring finalPath;
        HRESULT hr = CopyFileExW(source.c_str(), staging.c_str(), nullptr, nullptr, nullptr, 0) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
        if (SUCCEEDED(hr))
            hr = CommitStaging(staging, source, recipe.suffix, std::filesystem::path(source).extension().wstring(), finalPath);
        if (FAILED(hr))
        {
            DeleteFileW(staging.c_str());
            result.error = HResultText(hr);
            if (m_history)
                m_history->FinishItem(item, status::Failed, L"", 0, "", result.error, elapsedMs());
            return result;
        }
        result.ok = true;
        result.output = finalPath;
        const auto sha = Sha256File(finalPath, &result.size);
        if (m_history)
            m_history->FinishItem(item, status::Done, finalPath, result.size, sha, L"", elapsedMs());
        return result;
    }

    void PipelineRunner::RecoverInterrupted() const
    {
        if (!m_history)
            return;
        for (const auto& item : m_history->RecoverInterrupted())
        {
            if (!item.staging.empty() && DeleteFileW(item.staging.c_str()))
                log::Warn(L"recovery: removed leftover staging file {}", item.staging);
            else
                log::Warn(L"recovery: run {} item for {} was interrupted", item.runId, item.source);
        }
    }
}
