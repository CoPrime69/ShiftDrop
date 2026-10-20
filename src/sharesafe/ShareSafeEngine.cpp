#include "sharesafe/ShareSafeEngine.h"
#include "Log.h"
#include "Media.h"
#include "OutputPath.h"
#include "Utf.h"
#include "canvas/Editor.h"
#include "metadata/MediaMetadata.h"
#include "metadata/Metadata.h"
#include "redact/RedactEngine.h"
#include "redact/RedactTool.h"
#include "redact/Vision.h"

#include <algorithm>
#include <format>

namespace sd::sharesafe
{
    namespace
    {
        bool IsShareSafe(const Recipe& recipe) { return recipe.steps[0].action == "sharesafe.clean"; }

        std::wstring Join(const std::vector<std::wstring>& items, size_t max)
        {
            std::wstring s;
            for (size_t i = 0; i < items.size() && i < max; ++i)
                s += (i ? L", " : L"") + items[i];
            if (items.size() > max)
                s += std::format(L" and {} more", items.size() - max);
            return s;
        }

        // "Removed GPS, dates, 6 tags" for video and audio.
        std::wstring MediaSummary(const metadata::MediaTags& removed)
        {
            const size_t n = removed.PersonalCount();
            if (n == 0)
                return L"No metadata found";
            std::wstring s = L"Removed ";
            const auto kinds = removed.Kinds();
            for (size_t i = 0; i < std::min<size_t>(kinds.size(), 2); ++i)
                s += kinds[i] + L", ";
            return s + (n == 1 ? std::wstring(L"1 item") : std::format(L"{} items", n));
        }
    }

    ShareSafeEngine::ShareSafeEngine(const EngineServices& services, std::shared_ptr<const ImageCodecs> codecs)
        : m_services(services), m_codecs(std::move(codecs))
    {
    }

    bool ShareSafeEngine::Handles(const Recipe& recipe) const
    {
        return recipe.steps.size() == 1 && (recipe.steps[0].action == "sharesafe.clean" || recipe.steps[0].action == "metadata.remove");
    }

    bool ShareSafeEngine::IsImage(const std::wstring& ext) const
    {
        return MediaKindOfExtension(ext) == MediaKind::None && m_codecs && m_codecs->CanDecode(ext);
    }

    bool ShareSafeEngine::Applies(const Recipe&, const std::wstring& path) const
    {
        const auto ext = LowerExtension(path);
        return IsImage(ext) || MediaKindOfExtension(ext) != MediaKind::None;
    }

    bool ShareSafeEngine::Accepts(const Recipe&, const std::wstring& path, std::wstring* why) const
    {
        const auto ext = LowerExtension(path);
        if (IsImage(ext))
            return true;
        if (MediaKindOfExtension(ext) == MediaKind::None)
        {
            if (why)
                *why = std::format(L"no image decoder for '{}' on this PC", ext);
            return false;
        }
        if (!m_services.ffmpeg || !m_services.ffmpeg->Available())
        {
            if (why)
                *why = L"the media engine is missing; reinstall to clean video and audio";
            return false;
        }
        if (metadata::MuxerFor(ext).empty())
        {
            if (why)
                *why = std::format(L"'{}' files can't be rewritten without metadata yet", ext);
            return false;
        }
        return true;
    }

    ItemResult ShareSafeEngine::Run(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                                    const ProgressFn& progress) const
    {
        return IsImage(LowerExtension(source)) ? RunImage(recipe, runId, source, stop, progress) : RunMedia(recipe, runId, source, stop, progress);
    }

    ItemResult ShareSafeEngine::RunImage(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                                         const ProgressFn& progress) const
    {
        StagedOutput out(m_services.history, runId, source);
        if (!out.Ready())
            return out.Fail(out.Error());
        const auto& params = recipe.steps[0].params;
        const bool covered = IsShareSafe(recipe) && !redact::CoversFor(params, source).empty();

        std::wstring extension, error;
        metadata::ScrubReport report;
        size_t covers = 0;
        if (covered)
        {
            // Covers chosen in the review: the picture is rewritten, which drops all metadata too.
            report = metadata::ReportFor(metadata::ReadMetadata(source));
            const HRESULT hr = redact::WriteRedacted(source, params, out.Path(), extension, covers, stop, error);
            if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
                return out.Fail(L"cancelled", true);
            if (FAILED(hr))
                return out.Fail(error);
            report.reencoded = true;
        }
        else
        {
            if (metadata::ReadMetadata(source).PersonalCount() == 0)
                return out.Fail(L"no metadata found; it is already clean", true);
            if (FAILED(metadata::WriteWithoutMetadata(source, out.Path(), report, extension, error)))
                return out.Fail(error.empty() ? std::wstring(L"the copy could not be written") : error);
        }
        if (stop.stop_requested())
            return out.Fail(L"cancelled", true);
        if (progress)
            progress(0.8);

        // Read back what was written: nothing personal may be left, and the picture must still open.
        const auto check = metadata::VerifyNoMetadata(out.Path());
        if (!check.opens)
            return out.Fail(L"the cleaned copy did not open again");
        if (!check.clean)
        {
            log::Warn(L"share-safe: {} still has {}", source, Join(check.leftovers, 8));
            return out.Fail(L"could not remove " + Join(check.leftovers, 2));
        }
        if (progress)
            progress(1.0);
        auto result = out.Commit(recipe.suffix, extension);
        if (result.ok)
        {
            result.note = report.Summary();
            if (covers)
                result.note = std::format(L"Covered {} · {}", covers == 1 ? std::wstring(L"1 detail") : std::format(L"{} details", covers),
                                          report.fields ? result.note : std::wstring(L"no metadata"));
        }
        return result;
    }

    ItemResult ShareSafeEngine::RunMedia(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                                         const ProgressFn& progress) const
    {
        const auto& ffmpeg = m_services.ffmpeg;
        StagedOutput out(m_services.history, runId, source);
        if (!out.Ready())
            return out.Fail(out.Error());
        const auto ext = LowerExtension(source);
        if (!ffmpeg || !ffmpeg->Available())
            return out.Fail(L"the media engine is missing; reinstall to clean video and audio");
        if (metadata::MuxerFor(ext).empty())
            return out.Fail(std::format(L"'{}' files can't be rewritten without metadata yet", ext));

        std::vector<std::string> log;
        ffmpeg->Run(ProbeArgs(source), nullptr, stop, &log);   // exits with 1 by design: no output named
        const auto before = metadata::ParseMediaTags(log);
        const auto info = ParseMediaInfo(log);
        if (stop.stop_requested())
            return out.Fail(L"cancelled", true);
        if (before.PersonalCount() == 0)
            return out.Fail(L"no metadata found; it is already clean", true);

        // Stream copy: the picture and sound are not re-encoded, only the container is rewritten.
        std::vector<std::string> runLog;
        const auto run = ffmpeg->Run(FfmpegArgs(source, {}, metadata::StripMediaArgs(ext), out.Path()),
                                     [&](double p) { if (progress) progress(p * 0.9); }, stop, &runLog);
        if (run.cancelled || stop.stop_requested())
            return out.Fail(L"cancelled", true);
        if (!run.ok)
            return out.Fail(run.error.empty() ? std::wstring(L"ffmpeg could not copy the streams") : run.error);

        // Probe the copy: no personal tags, chapters, data tracks or cover art, and the same streams.
        log.clear();
        ffmpeg->Run(ProbeArgs(out.Path()), nullptr, stop, &log);
        const auto after = metadata::ParseMediaTags(log);
        const auto infoAfter = ParseMediaInfo(log);
        if (after.PersonalCount() > 0)
        {
            std::vector<std::wstring> left;
            for (const auto& t : after.Personal())
                left.push_back(Widen(t.key));
            if (after.chapters) left.emplace_back(L"chapters");
            if (after.dataStreams) left.emplace_back(L"data tracks");
            if (after.coverArt) left.emplace_back(L"cover art");
            if (after.attachments) left.emplace_back(L"attachments");
            log::Warn(L"share-safe: {} still has {}", source, Join(left, 8));
            return out.Fail(L"could not remove " + Join(left, 2));
        }
        if (infoAfter.videoCodec != info.videoCodec || infoAfter.audioCodec != info.audioCodec)
            return out.Fail(L"the cleaned copy lost a stream; nothing was saved");
        if (progress)
            progress(1.0);
        auto result = out.Commit(recipe.suffix, ext);
        if (result.ok)
            result.note = MediaSummary(before);
        return result;
    }

    bool ShareSafeEngine::NeedsEditor(const Recipe& recipe) const
    {
        // Share-safe checks pictures for written personal details first, when Windows can read text.
        // Nothing is shown unless something is found.
        return IsShareSafe(recipe) && recipe.steps[0].params.value("findDetails", true) && canvas::HasEditorWindow() && vision::CanReadText();
    }

    void ShareSafeEngine::OpenEditor(HWND, const Recipe& recipe, const std::vector<std::wstring>& files,
                                     std::function<void(Recipe adjusted, std::vector<std::wstring> files)> done) const
    {
        std::vector<std::wstring> pictures;
        for (const auto& f : files)
            if (IsImage(LowerExtension(f)))
                pictures.push_back(f);
        if (pictures.empty())
        {
            done(recipe, files);   // only video and audio: nothing to read
            return;
        }
        // The review sees the pictures; the run gets every file of the drop.
        canvas::OpenToolEditor(recipe, pictures, std::make_unique<redact::RedactTool>(pictures, redact::RedactTool::Mode::Review),
                               [files, done = std::move(done)](Recipe adjusted, std::vector<std::wstring>) { done(std::move(adjusted), files); });
    }
}
