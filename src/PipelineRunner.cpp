#include "PipelineRunner.h"
#include "Hash.h"
#include "Log.h"
#include "OutputPath.h"
#include "Utf.h"

#include <windows.h>
#include <wrl/client.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <format>

namespace sd
{
    PipelineRunner::PipelineRunner(std::shared_ptr<const ImageCodecs> codecs, HistoryStore* history, std::shared_ptr<const FfmpegEngine> media)
        : m_codecs(std::move(codecs)), m_history(history), m_media(std::move(media))
    {
        m_engines = MakeModuleEngines(EngineServices{ m_history, m_media });
    }

    const IEngine* PipelineRunner::EngineFor(const Recipe& recipe) const
    {
        for (const auto& e : m_engines)
            if (e->Handles(recipe))
                return e.get();
        return nullptr;
    }

    JobLane PipelineRunner::Lane(const Recipe& recipe) const
    {
        if (const auto* e = EngineFor(recipe))
            return e->Lane(recipe);
        return recipe.IsMedia() ? JobLane::Media : JobLane::Pool;
    }

    ItemResult PipelineRunner::RunBatch(const Recipe& recipe, int64_t runId, const std::vector<std::wstring>& sources, std::stop_token stop,
                                        const std::function<void(double)>& progress) const
    {
        const auto* e = EngineFor(recipe);
        if (!e || !e->IsBatch(recipe))
            return ItemResult{ false, false, {}, L"not a batch action", 0 };
        if (stop.stop_requested())
            return ItemResult{ false, true, {}, L"cancelled", 0 };
        return e->RunBatch(recipe, runId, sources, stop, progress);
    }

    bool PipelineRunner::Applies(const Recipe& recipe, const std::wstring& path) const
    {
        if (const auto* e = EngineFor(recipe))
            return e->Applies(recipe, path);
        if (recipe.IsFileAction())
            return true;
        const auto ext = LowerExtension(path);
        if (const auto* media = recipe.MediaFormat())
            return MediaFormatAccepts(*media, MediaKindOfExtension(ext));
        if (recipe.IsMediaCompress())
            return MediaKindOfExtension(ext) != MediaKind::None;
        return m_codecs->CanDecode(ext);
    }

    bool PipelineRunner::Accepts(const Recipe& recipe, const std::wstring& path, std::wstring* why) const
    {
        if (const auto* e = EngineFor(recipe))
            return e->Accepts(recipe, path, why);
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
        if (const auto* media = recipe.MediaFormat())
        {
            const auto kind = MediaKindOfExtension(ext);
            if (!MediaFormatAccepts(*media, kind))
            {
                if (why)
                    *why = kind == MediaKind::None ? L"not a video or audio file" : std::format(L"{} is not made from {}", media->label,
                                                                                               kind == MediaKind::Video ? L"videos" : L"audio files");
                return false;
            }
        }
        else if (recipe.IsMediaCompress() && MediaKindOfExtension(ext) == MediaKind::None)
        {
            if (why)
                *why = L"not a video or audio file";
            return false;
        }
        if (recipe.IsMedia())
        {
            if (!m_media || !m_media->Available())
            {
                if (why)
                    *why = L"the media engine is missing; reinstall to convert video and audio";
                return false;
            }
            return true;
        }
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
        if (const auto* e = EngineFor(recipe))
            return e->IsNoOp(recipe, path);
        const auto ext = LowerExtension(path);
        if (const auto* media = recipe.MediaFormat())
            return IsAlreadyMediaFormat(*media, ext);
        const auto* format = recipe.OutputFormatFor(ext);
        return recipe.IsSingleEncode() && format && IsAlreadyFormat(*format, ext);
    }

    ItemResult PipelineRunner::Run(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                                   const std::function<void(double)>& progress) const
    {
        ItemResult result;
        const auto started = std::chrono::steady_clock::now();
        const std::function<int64_t()> elapsedMs = [started] {
            return static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
        };

        std::wstring why;
        if (stop.stop_requested())
        {
            // Cancelled from the top panel (or shutting down) before this file's turn came.
            result.skipped = true;
            result.error = L"cancelled";
        }
        else if (IsNoOp(recipe, source))
        {
            result.skipped = true;
            const auto* media = recipe.MediaFormat();
            result.error = std::format(L"already {}", media ? media->label : recipe.OutputFormatFor(LowerExtension(source))->label);
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

        if (const auto* e = EngineFor(recipe))
            return e->Run(recipe, runId, source, stop, progress);
        if (recipe.IsFileAction())
            return RunDuplicate(recipe, runId, source, elapsedMs);
        if (recipe.IsMedia())
            return RunMedia(recipe, runId, source, stop, progress, elapsedMs);

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

    // media.convert and media.compress: ffmpeg writes straight into the staging file (its muxer is
    // named with -f, since the staging name has no real extension), then the same atomic commit as
    // images. A quick probe first says what the input holds, so streams the target container plays
    // are copied as they are: a phone's H.264 MOV becomes an MP4 in a moment, with no quality lost.
    ItemResult PipelineRunner::RunMedia(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                                        const std::function<void(double)>& progress, const std::function<int64_t()>& elapsedMs) const
    {
        ItemResult result;
        std::wstring staging;
        {
            Microsoft::WRL::ComPtr<IStream> reserve;   // reserves a unique staging name; closed before ffmpeg opens it
            if (const HRESULT hr = CreateStaging(source, staging, reserve); FAILED(hr))
            {
                result.error = std::format(L"cannot write next to the source: {}", HResultText(hr));
                return result;
            }
        }
        const int64_t item = m_history ? m_history->BeginItem(runId, source, staging) : 0;
        const auto fail = [&](std::wstring error, bool cancelled) {
            DeleteFileW(staging.c_str());
            result.error = std::move(error);
            result.skipped = cancelled;   // cancelled from the top panel: nothing was made, nothing failed
            if (m_history)
                m_history->FinishItem(item, cancelled ? status::Skipped : status::Failed, L"", 0, "", result.error, elapsedMs());
            return result;
        };

        const MediaInfo info = m_media->Probe(source);
        if (stop.stop_requested())
            return fail(L"cancelled", true);
        const bool video = !info.videoCodec.empty();
        const auto needsH264 = [&] {
            if (!video)
                return false;
            if (recipe.IsMediaCompress())
                return true;
            const std::string_view id(recipe.MediaFormat()->id);
            return id == "mp4" || id == "mov" || id == "mkv";
        };
        const H264Encoder h264 = needsH264() ? m_media->H264() : H264Encoder::OpenH264;

        FfmpegEngine::Result run;
        std::wstring extension;
        if (const auto* format = recipe.MediaFormat())
        {
            extension = format->extension;
            const MediaPlan plan = PlanConversion(*format, info, h264);
            log::Info(L"media: {} -> {} ({} video {}, audio {}{})", source, format->label, Widen(info.videoCodec.empty() ? "no" : info.videoCodec),
                      plan.copyAll || plan.copyVideo ? L"copied" : L"encoded", plan.copyAll || plan.copyAudio ? L"copied" : L"encoded",
                      plan.hwDecode ? L", GPU decode" : L"");
            run = m_media->Run(FfmpegArgs(source, InputArgs(plan), OutputArgs(*format, plan), staging), progress, stop);
            if (!run.ok && !run.cancelled && (plan.copyAll || plan.copyVideo || plan.copyAudio))
            {
                // Copying a stream failed (one the container can't hold after all): encode everything.
                log::Info(L"media: copying streams into {} failed ({}); re-encoding", format->label, run.error);
                const MediaPlan full = TranscodePlan(plan);
                run = m_media->Run(FfmpegArgs(source, InputArgs(full), OutputArgs(*format, full), staging), progress, stop);
            }
        }
        else
        {
            run = RunCompress(recipe, source, info, h264, staging, progress, stop);
            extension = video ? L".mp4" : L".m4a";
        }
        if (!run.ok)
            return fail(run.error, run.cancelled);

        std::wstring finalPath;
        if (const HRESULT hr = CommitStaging(staging, source, recipe.suffix, extension, finalPath); FAILED(hr))
            return fail(std::format(L"commit: {}", HResultText(hr)), false);
        result.ok = true;
        result.output = finalPath;
        const auto sha = Sha256File(finalPath, &result.size);
        if (m_history)
            m_history->FinishItem(item, status::Done, finalPath, result.size, sha, L"", elapsedMs());
        return result;
    }

    // media.compress into `staging`. A size target (targetMB, or percent of the source) is met by
    // bitrate; if the encoder overshoots, it runs again lower, and a result above the target is never
    // kept. The preset (no target) must at least come out smaller than the source.
    FfmpegEngine::Result PipelineRunner::RunCompress(const Recipe& recipe, const std::wstring& source, const MediaInfo& info, H264Encoder h264,
                                                     const std::wstring& staging, const std::function<void(double)>& progress,
                                                     std::stop_token stop) const
    {
        const auto& params = recipe.steps[0].params;
        std::error_code ec;
        const auto sourceBytes = static_cast<int64_t>(std::filesystem::file_size(source, ec));
        const double targetMB = params.value("targetMB", 0.0);
        const int percent = params.value("percent", 0);
        const int maxHeight = params.value("maxHeight", 0);
        const int64_t target = targetMB > 0 ? static_cast<int64_t>(targetMB * 1024 * 1024)
                               : percent > 0 ? sourceBytes * percent / 100
                                             : 0;
        const auto size = [&] { return static_cast<int64_t>(std::filesystem::file_size(staging, ec)); };
        FfmpegEngine::Result run;

        if (target <= 0)
        {
            run = m_media->Run(FfmpegArgs(source, {}, CompressArgs(info, h264, nullptr, maxHeight), staging), progress, stop);
            if (run.ok && sourceBytes > 0 && size() >= sourceBytes)
            {
                run.ok = false;
                run.error = L"already compressed: the result was not smaller";
            }
            return run;
        }
        if (sourceBytes > 0 && sourceBytes <= target && targetMB > 0)
        {
            run.error = std::format(L"already under {} MB", targetMB);
            return run;
        }
        SizeFit fit;
        if (!PlanSizeFit(target, info, fit))
        {
            run.error = info.durationUs <= 0 ? L"cannot tell how long this file is" : L"the target is too small for a file this long";
            return run;
        }
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            run = m_media->Run(FfmpegArgs(source, {}, CompressArgs(info, h264, &fit, maxHeight), staging), progress, stop);
            if (!run.ok)
                return run;
            const int64_t got = size();
            log::Info(L"media: fit attempt {}: {} kbit/s video -> {} bytes (target {})", attempt + 1, fit.videoBps / 1000, got, target);
            if (got <= target)
                return run;
            // Over: scale every bitrate by how much it missed, with a margin.
            const double k = static_cast<double>(target) / static_cast<double>(got) * 0.95;
            fit.videoBps = static_cast<int64_t>(static_cast<double>(fit.videoBps) * k);
            fit.audioBps = static_cast<int64_t>(static_cast<double>(fit.audioBps) * (info.videoCodec.empty() ? k : 1.0));
        }
        run.ok = false;
        run.error = L"could not get under the target size";
        return run;
    }

    void PipelineRunner::RecoverInterrupted() const
    {
        if (!m_history)
            return;
        for (const auto& item : m_history->RecoverInterrupted())
        {
            if (!item.staging.empty() && RemoveStaging(item.staging))
                log::Warn(L"recovery: removed leftover staging file {}", item.staging);
            else
                log::Warn(L"recovery: run {} item for {} was interrupted", item.runId, item.source);
        }
    }
}
