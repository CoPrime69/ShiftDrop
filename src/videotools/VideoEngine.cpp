#include "videotools/VideoEngine.h"
#include "ErrorText.h"
#include "ImagePipeline.h"
#include "Log.h"
#include "OutputPath.h"
#include "PathUtil.h"
#include "StagedOutput.h"
#include "Utf.h"
#include "Wic.h"
#include "videotools/VideoTools.h"

#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>

namespace sd::videotools
{
    using Microsoft::WRL::ComPtr;
    using nlohmann::json;
    using namespace mediatools;

    void RegisterActions(ActionRegistry& registry)
    {
        const auto ask = [] { return ParamSpec{ "ask", ParamType::Bool, 0, 0, {}, true }; };   // open the editor first
        const auto seconds = [](const char* key) { return ParamSpec{ key, ParamType::Number, 0, 1e7, {}, 0 }; };
        registry.Add({ "video.trim", L"Trim video", StepKind::Engine,
                       { ParamSpec{ "mode", ParamType::Enum, 0, 0, { "exact", "fast" }, "exact" }, seconds("start"), seconds("end"),
                         ParamSpec{ "part", ParamType::Enum, 0, 0, { "keep", "remove" }, "keep" }, ParamSpec{ "fade", ParamType::Bool, 0, 0, {}, false },
                         ask() } });
        registry.Add({ "video.crop", L"Crop video", StepKind::Engine,
                       { ParamSpec{ "aspect", ParamType::Enum, 0, 0, { "free", "original", "16:9", "9:16", "1:1", "4:5" }, "free" },
                         ParamSpec{ "width", ParamType::Int, 0, 8192, {}, 0 }, ParamSpec{ "height", ParamType::Int, 0, 8192, {}, 0 }, ask() } });
        registry.Add({ "video.speed", L"Change video speed", StepKind::Engine,
                       { ParamSpec{ "factor", ParamType::Number, kMinSpeed, kMaxSpeed, {}, 2.0 }, ParamSpec{ "pitch", ParamType::Bool, 0, 0, {}, true },
                         ask() } });
        registry.Add({ "video.join", L"Join videos", StepKind::Engine,
                       { ParamSpec{ "fit", ParamType::Enum, 0, 0, { "fit", "fill" }, "fit" }, ask() } });
        registry.Add({ "video.frames", L"Save video frames", StepKind::Engine,
                       { ParamSpec{ "pick", ParamType::Enum, 0, 0, { "marks", "every", "rate" }, "marks" },
                         ParamSpec{ "every", ParamType::Int, 1, kMaxEvery, {}, 30 }, ParamSpec{ "rate", ParamType::Number, kMinPerSecond, kMaxPerSecond, {}, 1.0 },
                         ParamSpec{ "format", ParamType::Enum, 0, 0, { "png", "jpg", "webp" }, "png" } } });
        registry.Add({ "video.split", L"Split video", StepKind::Engine,
                       { ParamSpec{ "mode", ParamType::Enum, 0, 0, { "exact", "fast" }, "exact" }, ParamSpec{ "parts", ParamType::Int, 0, 100, {}, 0 },
                         seconds("every"), ask() } });
        registry.Add({ "video.redact", L"Redact video", StepKind::Engine,
                       { ParamSpec{ "style", ParamType::Enum, 0, 0, { "blur", "solid" }, "blur" } } });
    }

    namespace
    {
        // Every video tool as a slice, each opening the editor on its own tab. With the four Compress
        // presets and Inspect a video's Tools wheel has twelve slices, on two rings.
        const json& ToolRecipes()
        {
            static const json all = json::parse(R"([
  { "id": "video-trim", "label": "Trim", "description": "Trim Video: keep a part of it, or cut a part out", "ring": "tools", "suffix": "trimmed", "icon": "E78A",
    "steps": [ { "action": "video.trim" } ] },
  { "id": "video-crop", "label": "Crop", "description": "Crop Video: exact size, 16:9, 9:16, 1:1, 4:5", "ring": "tools", "suffix": "cropped", "icon": "E7A8",
    "steps": [ { "action": "video.crop" } ] },
  { "id": "video-speed", "label": "Speed", "description": "Change Video Speed: 0.25x to 4x, pitch kept or not", "ring": "tools", "suffix": "speed", "icon": "EC49",
    "steps": [ { "action": "video.speed" } ] },
  { "id": "video-join", "label": "Join", "description": "Join Videos into one MP4, in the order you choose", "ring": "tools", "suffix": "joined", "icon": "E71B",
    "steps": [ { "action": "video.join" } ] },
  { "id": "video-frames", "label": "Frames", "description": "Save Video Frames as PNG, JPG or WebP: marked ones, every Nth, or N a second", "ring": "tools", "suffix": "frames", "icon": "E722",
    "steps": [ { "action": "video.frames" } ] },
  { "id": "video-split", "label": "Split", "description": "Split Video at points you pick, into equal parts, or every so long", "ring": "tools", "suffix": "parts", "icon": "E8C6",
    "steps": [ { "action": "video.split" } ] },
  { "id": "video-redact", "label": "Redact", "description": "Redact Video: blur or cover areas, each for its own time", "ring": "tools", "suffix": "redacted", "icon": "E72E",
    "steps": [ { "action": "video.redact" } ] }
])");
            return all;
        }
    }

    const char* BuiltInRecipes()
    {
        static const std::string all = ToolRecipes().dump();
        return all.c_str();
    }

    json ToolRecipe(EditorTool tool)
    {
        return FindToolRecipe(ToolRecipes(), ToolAction(tool));
    }

    bool IsBatchAction(const std::string& action) { return action == "video.join"; }

    bool NeedsEditor(const RecipeStep& step)
    {
        if (step.action == "video.frames" || step.action == "video.redact")
            return true;
        return step.params.value("ask", true);
    }

    namespace
    {
        // The probe, with the reason a file can't be edited as video.
        bool ProbeVideo(const FfmpegEngine& ffmpeg, const std::wstring& source, std::stop_token stop, ProbeInfo& info, std::wstring& why)
        {
            info = Probe(ffmpeg, source, stop);
            if (!info.HasVideo())
                why = L"no video stream in this file";
            else if (info.DurationUs() <= 0)
                why = L"cannot tell how long this video is";
            else
                return true;
            return false;
        }

        ItemResult RunTrim(const EngineServices& s, const Recipe& recipe, int64_t runId, const std::wstring& source, const ProbeInfo& info,
                           std::stop_token stop, const ProgressFn& progress)
        {
            const auto& p = recipe.steps[0].params;
            const int64_t duration = info.DurationUs();
            TimeRange selection{ FromSeconds(JsonNumber(p, "start", 0)), 0 };
            const double end = JsonNumber(p, "end", 0);
            selection.end = end > 0 ? std::min(FromSeconds(end), duration) : duration;
            TrimChoice choice;
            choice.remove = p.value("part", std::string("keep")) == "remove";
            choice.fade = p.value("fade", false);
            if (selection.Length() < kMinRangeUs)
                return RecordFailure(s.history, runId, source, choice.remove ? L"the part to remove is empty" : L"the part to keep is empty");
            if (selection.start <= 0 && selection.end >= duration)
                return RecordFailure(s.history, runId, source,
                                     choice.remove ? L"nothing would be left: the whole video is selected" : L"nothing to trim: the whole video is kept");
            const int64_t length = TrimmedLength(selection, duration, choice.remove);
            if (length < kMinRangeUs)
                return RecordFailure(s.history, runId, source, L"nothing would be left after removing that part");
            const bool fast = p.value("mode", std::string("exact")) == "fast";
            const bool copy = TrimCopies(fast, choice);
            const H264Encoder h264 = copy ? H264Encoder::OpenH264 : s.ffmpeg->H264();
            const auto ext = LowerExtension(source);
            log::Info(L"video: trim {} {} [{} .. {}]{} {}", source, choice.remove ? L"removing" : L"keeping", FormatTimecode(selection.start),
                      FormatTimecode(selection.end), choice.fade ? L" with fades" : L"", copy ? L"fast (copy)" : EncoderName(h264));
            return RunToStaged(*s.ffmpeg, s.history, runId, source,
                               [&](const std::wstring& out) { return TrimArgs(source, ext, info, selection, fast, h264, out, choice); }, length,
                               recipe.suffix, TrimContainer(ext, copy).extension, stop, progress);
        }

        ItemResult RunCrop(const EngineServices& s, const Recipe& recipe, int64_t runId, const std::wstring& source, const ProbeInfo& info,
                           std::stop_token stop, const ProgressFn& progress)
        {
            const auto& p = recipe.steps[0].params;
            PixelRect box;
            if (const auto it = p.find("box"); it != p.end() && it->is_array() && it->size() == 4)
                box = { (*it)[0].get<int>(), (*it)[1].get<int>(), (*it)[2].get<int>(), (*it)[3].get<int>() };
            else
                box = CenteredCrop(info.width, info.height, AspectByName(p.value("aspect", std::string("free")), info.width, info.height));
            box = ClampCrop(box, info.width, info.height);
            int outW = 0, outH = 0;
            CropOutputSize(box, p.value("width", 0), p.value("height", 0), outW, outH);
            if (box == PixelRect{ 0, 0, info.width, info.height } && outW == info.width && outH == info.height)
                return RecordFailure(s.history, runId, source, L"nothing to crop: the box is the whole picture");
            const H264Encoder h264 = s.ffmpeg->H264();
            const auto ext = LowerExtension(source);
            log::Info(L"video: crop {} to {}x{}+{}+{} -> {}x{}", source, box.w, box.h, box.x, box.y, outW, outH);
            return RunToStaged(*s.ffmpeg, s.history, runId, source,
                               [&](const std::wstring& out) { return CropArgs(source, ext, info, box, outW, outH, h264, out); }, info.DurationUs(),
                               recipe.suffix, EncodeContainer(ext).extension, stop, progress);
        }

        ItemResult RunSpeed(const EngineServices& s, const Recipe& recipe, int64_t runId, const std::wstring& source, const ProbeInfo& info,
                            std::stop_token stop, const ProgressFn& progress)
        {
            const double factor = std::clamp(JsonNumber(recipe.steps[0].params, "factor", 2.0), kMinSpeed, kMaxSpeed);
            if (std::abs(factor - 1.0) < 0.001)
                return RecordFailure(s.history, runId, source, L"1x is the speed it already has");
            const H264Encoder h264 = s.ffmpeg->H264();
            const auto ext = LowerExtension(source);
            const bool keepPitch = recipe.steps[0].params.value("pitch", true);
            const auto outputUs = static_cast<int64_t>(static_cast<double>(info.DurationUs()) / factor);
            log::Info(L"video: speed {} x{}{}", source, Num(factor), !info.HasAudio() ? L" (silent)" : keepPitch ? L"" : L", pitch follows");
            // The output is named after the speed: "clip (2x).mp4", "clip (0.5x).mp4".
            return RunToStaged(*s.ffmpeg, s.history, runId, source,
                               [&](const std::wstring& out) { return SpeedArgs(source, ext, info, factor, h264, out, keepPitch); }, outputUs,
                               SpeedLabel(factor), EncodeContainer(ext).extension, stop, progress);
        }

        // A frame ffmpeg wrote as `png`, under its final name `target` in `format`: moved as it is,
        // or encoded by the app's image encoders and the PNG deleted. Never replaces a file.
        FfmpegEngine::Result SaveFrame(const std::wstring& png, const std::wstring& target, const ImageFormatInfo& format)
        {
            if (std::string_view(format.id) == "png")
            {
                if (!MoveFileExW(png.c_str(), target.c_str(), 0))
                    return { false, false, std::format(L"{}: {}", FileName(target), HResultMessage(HRESULT_FROM_WIN32(GetLastError()))) };
                return { true, false, {} };
            }
            std::wstring why;
            HRESULT hr = E_FAIL;
            {
                const auto factory = wic::Factory();
                ComPtr<IWICBitmapDecoder> decoder;
                ComPtr<IWICBitmapFrameDecode> frame;
                ComPtr<IStream> file;
                hr = factory ? factory->CreateDecoderFromFilename(png.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)
                             : REGDB_E_CLASSNOTREG;
                if (SUCCEEDED(hr))
                    hr = decoder->GetFrame(0, &frame);
                if (SUCCEEDED(hr))   // a new file in our own new staging folder
                    hr = SHCreateStreamOnFileEx(target.c_str(), STGM_WRITE | STGM_SHARE_DENY_WRITE | STGM_FAILIFTHERE, FILE_ATTRIBUTE_NORMAL, TRUE,
                                                nullptr, &file);
                if (SUCCEEDED(hr))
                    hr = EncodePicture(frame.Get(), format, FrameQuality(format.id), file.Get(), why);
            }   // the PNG is closed before it is deleted
            DeleteFileW(png.c_str());
            if (FAILED(hr))
                return { false, false, std::format(L"{}: {}", FileName(target), why.empty() ? HResultMessage(hr) : why) };
            return { true, false, {} };
        }

        ItemResult RunFrames(const EngineServices& s, const Recipe& recipe, int64_t runId, const std::wstring& source, const ProbeInfo& info,
                             std::stop_token stop, const ProgressFn& progress)
        {
            const auto& p = recipe.steps[0].params;
            const auto* format = FindImageFormat(p.value("format", std::string("png")));
            if (!format)
                format = FindImageFormat("png");
            const std::wstring extension = format->extension;
            const auto stem = Stem(source);
            const FramePick pick = FramePickByName(p.value("pick", std::string("marks")));
            if (pick != FramePick::Marks)
            {
                FrameSequence seq{ pick, p.value("every", 30), JsonNumber(p, "rate", 1.0) };
                const int64_t expected = FrameSequenceCount(seq, info.media.fps, info.DurationUs());
                if (pick == FramePick::Every && expected <= 0)
                    return RecordFailure(s.history, runId, source, L"cannot tell this video's frame rate: mark the frames instead");
                log::Info(L"video: about {} frame(s) of {} as {} ({})", expected, source, format->label, FrameSequenceFilter(seq));
                // One pass writes numbered PNGs (most of the progress); each is then named by its time.
                const bool convert = std::string_view(format->id) != "png";
                return RunToStagedFolder(
                    s.history, runId, source, 1,
                    [&](int, const std::wstring& folder, const ProgressFn& part) {
                        const double share = convert ? 0.7 : 0.95;
                        auto r = s.ffmpeg->Run(FrameSequenceArgs(source, seq, folder), [&](double x) { if (part) part(x * share); }, stop, nullptr,
                                               info.DurationUs());
                        if (!r.ok)
                            return r;
                        std::vector<std::wstring> written;
                        for (const auto& e : std::filesystem::directory_iterator(folder))
                            if (e.path().extension() == L".png")
                                written.push_back(e.path().wstring());
                        // 000000.png, 000001.png... (a millionth frame has seven digits)
                        std::sort(written.begin(), written.end(), [](const std::wstring& a, const std::wstring& b) {
                            return a.size() != b.size() ? a.size() < b.size() : a < b;
                        });
                        if (written.empty())
                            return FfmpegEngine::Result{ false, false, L"no frames came out" };
                        for (size_t i = 0; i < written.size(); ++i)
                        {
                            if (stop.stop_requested())
                                return FfmpegEngine::Result{ false, true, L"cancelled" };
                            const auto t = FrameSequenceTime(seq, info.media.fps, static_cast<int64_t>(i));
                            r = SaveFrame(written[i], folder + L"\\" + FrameFileName(stem, t, extension), *format);
                            if (!r.ok)
                                return r;
                            if (part)
                                part(share + (1 - share) * static_cast<double>(i + 1) / static_cast<double>(written.size()));
                        }
                        return r;
                    },
                    recipe.suffix, stop, progress);
            }
            const auto times = NormalizeFrameTimes(TimesFrom(p, "frames"), info.media.fps, info.DurationUs());
            if (times.empty())
                return RecordFailure(s.history, runId, source, L"no frames were marked");
            log::Info(L"video: {} frame(s) of {} as {}", times.size(), source, format->label);
            return RunToStagedFolder(
                s.history, runId, source, static_cast<int>(times.size()),
                [&](int i, const std::wstring& folder, const ProgressFn& part) {
                    const int64_t t = times[static_cast<size_t>(i)];
                    const auto target = folder + L"\\" + FrameFileName(stem, t, extension);
                    const auto png = std::string_view(format->id) == "png" ? target : folder + L"\\frame.png";
                    auto r = s.ffmpeg->Run(FrameToPngArgs(source, t, info.media.fps, png), part, stop);
                    if (r.ok && !PathExists(png))
                        r = { false, false, std::format(L"no frame at {}", FormatTimecode(t)) };
                    if (r.ok && png != target)
                        r = SaveFrame(png, target, *format);
                    return r;
                },
                recipe.suffix, stop, progress);
        }

        ItemResult RunSplit(const EngineServices& s, const Recipe& recipe, int64_t runId, const std::wstring& source, const ProbeInfo& info,
                            std::stop_token stop, const ProgressFn& progress)
        {
            const auto& p = recipe.steps[0].params;
            const int64_t duration = info.DurationUs();
            auto points = NormalizePoints(TimesFrom(p, "points"), duration);
            if (points.empty())
                points = EveryPoints(duration, FromSeconds(JsonNumber(p, "every", 0)));
            if (points.empty())
                points = EqualPartPoints(duration, p.value("parts", 0));
            if (points.empty())
                return RecordFailure(s.history, runId, source, L"no split points: mark where to cut, or choose a number of parts or a length");
            if (static_cast<int>(points.size()) + 1 > kMaxSplitParts)
                return RecordFailure(s.history, runId, source, std::format(L"that makes more than {} parts: choose a longer length", kMaxSplitParts));
            const bool fast = p.value("mode", std::string("exact")) == "fast";
            const auto ext = LowerExtension(source);
            const auto stem = Stem(source);
            log::Info(L"video: split {} into {} parts ({})", source, points.size() + 1, fast ? L"fast, copied" : L"exact");
            if (fast)
            {
                return RunToStagedFolder(
                    s.history, runId, source, 1,
                    [&](int, const std::wstring& folder, const ProgressFn& part) {
                        return s.ffmpeg->Run(SplitCopyArgs(source, ext, points, folder, stem), part, stop, nullptr, duration);
                    },
                    recipe.suffix, stop, progress);
            }
            const auto parts = PartsFromPoints(points, duration);
            const H264Encoder h264 = s.ffmpeg->H264();
            const auto extension = EncodeContainer(ext).extension;
            // Each part's share of the progress follows its length.
            return RunToStagedFolder(
                s.history, runId, source, static_cast<int>(parts.size()),
                [&](int i, const std::wstring& folder, const ProgressFn& part) {
                    const auto& range = parts[static_cast<size_t>(i)];
                    const auto file = folder + L"\\" + PartName(stem, i + 1, static_cast<int>(parts.size()), extension);
                    return s.ffmpeg->Run(SplitPartArgs(source, ext, info, range, h264, file), part, stop, nullptr, range.Length());
                },
                recipe.suffix, stop, progress);
        }

        ItemResult RunRedact(const EngineServices& s, const Recipe& recipe, int64_t runId, const std::wstring& source, const ProbeInfo& info,
                             std::stop_token stop, const ProgressFn& progress)
        {
            std::vector<RedactBox> boxes;
            const auto& p = recipe.steps[0].params;
            if (const auto it = p.find("boxes"); it != p.end() && it->is_array())
                for (const auto& b : *it)
                {
                    if (!b.is_object())
                        continue;
                    RedactBox box;
                    box.box = { b.value("x", 0), b.value("y", 0), b.value("w", 0), b.value("h", 0) };
                    box.range = { FromSeconds(b.value("start", 0.0)), FromSeconds(b.value("end", 0.0)) };
                    box.blur = b.value("style", std::string("blur")) != "solid";
                    if (box.range.end <= box.range.start)
                        box.range.end = info.DurationUs();
                    if (box.box.w > 0 && box.box.h > 0 && box.range.Length() > 0)
                        boxes.push_back(box);
                }
            if (boxes.empty())
                return RecordFailure(s.history, runId, source, L"no areas to redact");
            const H264Encoder h264 = s.ffmpeg->H264();
            const auto ext = LowerExtension(source);
            log::Info(L"video: redact {} area(s) in {}", boxes.size(), source);
            return RunToStaged(*s.ffmpeg, s.history, runId, source,
                               [&](const std::wstring& out) { return RedactArgs(source, ext, info, boxes, h264, out); }, info.DurationUs(),
                               recipe.suffix, EncodeContainer(ext).extension, stop, progress);
        }
    }

    ItemResult Run(const EngineServices& s, const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                   const ProgressFn& progress)
    {
        ProbeInfo info;
        std::wstring why;
        if (!ProbeVideo(*s.ffmpeg, source, stop, info, why))
            return RecordFailure(s.history, runId, source, stop.stop_requested() ? L"cancelled" : why, stop.stop_requested());
        const auto& action = recipe.steps[0].action;
        if (action == "video.trim") return RunTrim(s, recipe, runId, source, info, stop, progress);
        if (action == "video.crop") return RunCrop(s, recipe, runId, source, info, stop, progress);
        if (action == "video.speed") return RunSpeed(s, recipe, runId, source, info, stop, progress);
        if (action == "video.frames") return RunFrames(s, recipe, runId, source, info, stop, progress);
        if (action == "video.split") return RunSplit(s, recipe, runId, source, info, stop, progress);
        if (action == "video.redact") return RunRedact(s, recipe, runId, source, info, stop, progress);
        return RecordFailure(s.history, runId, source, Widen(std::format("{} runs on all files at once", action)));
    }

    ItemResult RunBatch(const EngineServices& s, const Recipe& recipe, int64_t runId, const std::vector<std::wstring>& sources, std::stop_token stop,
                        const ProgressFn& progress)
    {
        const auto& first = sources.front();
        if (sources.size() < 2)
            return RecordFailure(s.history, runId, first, L"Join needs two or more videos");
        std::vector<ProbeInfo> clips;
        int64_t total = 0;
        for (const auto& source : sources)
        {
            ProbeInfo info;
            std::wstring why;
            if (!ProbeVideo(*s.ffmpeg, source, stop, info, why))
                return RecordFailure(s.history, runId, first,
                                std::format(L"{}: {}", FileName(source), stop.stop_requested() ? L"cancelled" : why),
                                stop.stop_requested());
            total += info.DurationUs();
            clips.push_back(std::move(info));
        }
        StagedOutput out(s.history, runId, first);
        if (!out.Ready())
            return out.Fail(out.Error());
        FfmpegEngine::Result r;
        bool copied = false;
        if (CanJoinByCopy(clips))
        {
            // Matching clips (one camera's): joined as they are, in a moment.
            TempFolder temp;
            const auto list = temp.File(L"clips.ffconcat");
            std::ofstream(std::filesystem::path(list), std::ios::binary) << ConcatList(sources);
            log::Info(L"video: join {} clips by copying", sources.size());
            r = s.ffmpeg->Run(JoinCopyArgs(list, clips.front(), out.Path()), progress, stop, nullptr, total);
            copied = r.ok;
            if (!r.ok && !r.cancelled)
                log::Info(L"video: joining by copy failed ({}); encoding", r.error);
        }
        if (!copied && !r.cancelled)
        {
            const H264Encoder h264 = s.ffmpeg->H264();
            log::Info(L"video: join {} clips, encoded with {}", sources.size(), EncoderName(h264));
            const bool fill = recipe.steps[0].params.value("fit", std::string("fit")) == "fill";
            r = s.ffmpeg->Run(JoinEncodeArgs(sources, clips, h264, out.Path(), fill), progress, stop, nullptr, total);
        }
        if (!r.ok)
            return out.Fail(r.error, r.cancelled);
        return out.Commit(recipe.suffix, L".mp4");
    }
}
