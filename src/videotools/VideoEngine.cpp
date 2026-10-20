#include "videotools/VideoEngine.h"
#include "Log.h"
#include "OutputPath.h"
#include "StagedOutput.h"
#include "Utf.h"
#include "videotools/VideoTools.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>

namespace sd::videotools
{
    using nlohmann::json;
    using namespace mediatools;

    void RegisterActions(ActionRegistry& registry)
    {
        const auto ask = [] { return ParamSpec{ "ask", ParamType::Bool, 0, 0, {}, true }; };   // open the editor first
        const auto seconds = [](const char* key) { return ParamSpec{ key, ParamType::Number, 0, 1e7, {}, 0 }; };
        registry.Add({ "video.trim", L"Trim video", StepKind::Engine,
                       { ParamSpec{ "mode", ParamType::Enum, 0, 0, { "exact", "fast" }, "exact" }, seconds("start"), seconds("end"), ask() } });
        registry.Add({ "video.crop", L"Crop video", StepKind::Engine,
                       { ParamSpec{ "aspect", ParamType::Enum, 0, 0, { "free", "original", "16:9", "9:16", "1:1", "4:5" }, "free" },
                         ParamSpec{ "width", ParamType::Int, 0, 8192, {}, 0 }, ParamSpec{ "height", ParamType::Int, 0, 8192, {}, 0 }, ask() } });
        registry.Add({ "video.speed", L"Change video speed", StepKind::Engine,
                       { ParamSpec{ "factor", ParamType::Number, kMinSpeed, kMaxSpeed, {}, 2.0 }, ask() } });
        registry.Add({ "video.join", L"Join videos", StepKind::Engine, { ask() } });
        registry.Add({ "video.frames", L"Save video frames", StepKind::Engine, {} });
        registry.Add({ "video.split", L"Split video", StepKind::Engine,
                       { ParamSpec{ "mode", ParamType::Enum, 0, 0, { "exact", "fast" }, "exact" }, ParamSpec{ "parts", ParamType::Int, 0, 100, {}, 0 },
                         ask() } });
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
  { "id": "video-trim", "label": "Trim\u2026", "description": "Trim Video: keep a part of it", "ring": "tools", "suffix": "trimmed", "icon": "E78A",
    "steps": [ { "action": "video.trim" } ] },
  { "id": "video-crop", "label": "Crop\u2026", "description": "Crop Video: exact size, 16:9, 9:16, 1:1, 4:5", "ring": "tools", "suffix": "cropped", "icon": "E7A8",
    "steps": [ { "action": "video.crop" } ] },
  { "id": "video-speed", "label": "Speed\u2026", "description": "Change Video Speed: 0.25x to 4x, pitch kept", "ring": "tools", "suffix": "speed", "icon": "EC49",
    "steps": [ { "action": "video.speed" } ] },
  { "id": "video-join", "label": "Join\u2026", "description": "Join Videos into one MP4, in the order you choose", "ring": "tools", "suffix": "joined", "icon": "E71B",
    "steps": [ { "action": "video.join" } ] },
  { "id": "video-frames", "label": "Frames\u2026", "description": "Save Video Frames as full-size PNGs", "ring": "tools", "suffix": "frames", "icon": "E722",
    "steps": [ { "action": "video.frames" } ] },
  { "id": "video-split", "label": "Split\u2026", "description": "Split Video at points you pick, or into equal parts", "ring": "tools", "suffix": "parts", "icon": "E8C6",
    "steps": [ { "action": "video.split" } ] },
  { "id": "video-redact", "label": "Redact\u2026", "description": "Redact Video: blur or cover areas, each for its own time", "ring": "tools", "suffix": "redacted", "icon": "E72E",
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
        for (const auto& r : ToolRecipes())
            if (r["steps"][0]["action"] == ToolAction(tool))
                return r;
        return json();
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
        double Number(const json& params, const char* key, double fallback)
        {
            const auto it = params.find(key);
            return it != params.end() && it->is_number() ? it->get<double>() : fallback;
        }

        std::vector<int64_t> Times(const json& params, const char* key)
        {
            std::vector<int64_t> out;
            if (const auto it = params.find(key); it != params.end() && it->is_array())
                for (const auto& v : *it)
                    if (v.is_number())
                        out.push_back(FromSeconds(v.get<double>()));
            return out;
        }

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
            TimeRange keep{ FromSeconds(Number(p, "start", 0)), 0 };
            const double end = Number(p, "end", 0);
            keep.end = end > 0 ? std::min(FromSeconds(end), duration) : duration;
            if (keep.Length() < kMinRangeUs)
                return FailItem(s.history, runId, source, L"the part to keep is empty");
            if (keep.start <= 0 && keep.end >= duration)
                return FailItem(s.history, runId, source, L"nothing to trim: the whole video is kept");
            const bool fast = p.value("mode", std::string("exact")) == "fast";
            const H264Encoder h264 = fast ? H264Encoder::OpenH264 : s.ffmpeg->H264();
            const auto ext = LowerExtension(source);
            log::Info(L"video: trim {} [{} .. {}] {}", source, FormatTimecode(keep.start), FormatTimecode(keep.end), fast ? L"fast (copy)" : EncoderName(h264));
            return RunToStaged(*s.ffmpeg, s.history, runId, source,
                               [&](const std::wstring& out) { return TrimArgs(source, ext, info, keep, fast, h264, out); }, keep.Length(),
                               recipe.suffix, TrimContainer(ext, fast).extension, stop, progress);
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
                return FailItem(s.history, runId, source, L"nothing to crop: the box is the whole picture");
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
            const double factor = std::clamp(Number(recipe.steps[0].params, "factor", 2.0), kMinSpeed, kMaxSpeed);
            if (std::abs(factor - 1.0) < 0.001)
                return FailItem(s.history, runId, source, L"1x is the speed it already has");
            const H264Encoder h264 = s.ffmpeg->H264();
            const auto ext = LowerExtension(source);
            const auto outputUs = static_cast<int64_t>(static_cast<double>(info.DurationUs()) / factor);
            log::Info(L"video: speed {} x{}{}", source, Num(factor), info.HasAudio() ? L"" : L" (silent)");
            // The output is named after the speed: "clip (2x).mp4", "clip (0.5x).mp4".
            return RunToStaged(*s.ffmpeg, s.history, runId, source,
                               [&](const std::wstring& out) { return SpeedArgs(source, ext, info, factor, h264, out); }, outputUs,
                               SpeedLabel(factor), EncodeContainer(ext).extension, stop, progress);
        }

        ItemResult RunFrames(const EngineServices& s, const Recipe& recipe, int64_t runId, const std::wstring& source, const ProbeInfo& info,
                             std::stop_token stop, const ProgressFn& progress)
        {
            const auto times = NormalizeFrameTimes(Times(recipe.steps[0].params, "frames"), info.media.fps, info.DurationUs());
            if (times.empty())
                return FailItem(s.history, runId, source, L"no frames were marked");
            const auto stem = Stem(source);
            log::Info(L"video: {} frame(s) of {} as PNG", times.size(), source);
            return RunToStagedFolder(
                s.history, runId, source, static_cast<int>(times.size()),
                [&](int i, const std::wstring& folder, const ProgressFn& part) {
                    const auto file = folder + L"\\" + FramePngName(stem, times[static_cast<size_t>(i)]);
                    auto r = s.ffmpeg->Run(FrameToPngArgs(source, times[static_cast<size_t>(i)], info.media.fps, file), part, stop);
                    if (r.ok && GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES)
                        r = { false, false, std::format(L"no frame at {}", FormatTimecode(times[static_cast<size_t>(i)])) };
                    return r;
                },
                recipe.suffix, stop, progress);
        }

        ItemResult RunSplit(const EngineServices& s, const Recipe& recipe, int64_t runId, const std::wstring& source, const ProbeInfo& info,
                            std::stop_token stop, const ProgressFn& progress)
        {
            const auto& p = recipe.steps[0].params;
            const int64_t duration = info.DurationUs();
            auto points = NormalizePoints(Times(p, "points"), duration);
            if (points.empty())
                points = EqualPartPoints(duration, p.value("parts", 0));
            if (points.empty())
                return FailItem(s.history, runId, source, L"no split points: mark where to cut, or choose a number of parts");
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
                return FailItem(s.history, runId, source, L"no areas to redact");
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
            return FailItem(s.history, runId, source, stop.stop_requested() ? L"cancelled" : why, stop.stop_requested());
        const auto& action = recipe.steps[0].action;
        if (action == "video.trim") return RunTrim(s, recipe, runId, source, info, stop, progress);
        if (action == "video.crop") return RunCrop(s, recipe, runId, source, info, stop, progress);
        if (action == "video.speed") return RunSpeed(s, recipe, runId, source, info, stop, progress);
        if (action == "video.frames") return RunFrames(s, recipe, runId, source, info, stop, progress);
        if (action == "video.split") return RunSplit(s, recipe, runId, source, info, stop, progress);
        if (action == "video.redact") return RunRedact(s, recipe, runId, source, info, stop, progress);
        return FailItem(s.history, runId, source, Widen(std::format("{} runs on all files at once", action)));
    }

    ItemResult RunBatch(const EngineServices& s, const Recipe& recipe, int64_t runId, const std::vector<std::wstring>& sources, std::stop_token stop,
                        const ProgressFn& progress)
    {
        const auto& first = sources.front();
        if (sources.size() < 2)
            return FailItem(s.history, runId, first, L"Join needs two or more videos");
        std::vector<ProbeInfo> clips;
        int64_t total = 0;
        for (const auto& source : sources)
        {
            ProbeInfo info;
            std::wstring why;
            if (!ProbeVideo(*s.ffmpeg, source, stop, info, why))
                return FailItem(s.history, runId, first,
                                std::format(L"{}: {}", std::filesystem::path(source).filename().wstring(), stop.stop_requested() ? L"cancelled" : why),
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
            r = s.ffmpeg->Run(JoinEncodeArgs(sources, clips, h264, out.Path()), progress, stop, nullptr, total);
        }
        if (!r.ok)
            return out.Fail(r.error, r.cancelled);
        return out.Commit(recipe.suffix, L".mp4");
    }
}
