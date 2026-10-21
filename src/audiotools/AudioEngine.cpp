#include "audiotools/AudioEngine.h"
#include "Log.h"
#include "OutputPath.h"
#include "StagedOutput.h"
#include "Utf.h"
#include "audiotools/AudioTools.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>

namespace sd::audiotools
{
    using nlohmann::json;
    using namespace mediatools;

    void RegisterActions(ActionRegistry& registry)
    {
        const auto ask = [] { return ParamSpec{ "ask", ParamType::Bool, 0, 0, {}, true }; };
        const auto seconds = [](const char* key) { return ParamSpec{ key, ParamType::Number, 0, 1e7, {}, 0 }; };
        registry.Add({ "audio.normalize", L"Normalize volume", StepKind::Engine, { ParamSpec{ "target", ParamType::Number, -40, -5, {}, -16 }, ask() } });
        registry.Add({ "audio.visualizer", L"Audio visualizer", StepKind::Engine,
                       { ParamSpec{ "style", ParamType::Enum, 0, 0, { "waveform", "image" }, "waveform" },
                         ParamSpec{ "shape", ParamType::Enum, 0, 0, { "landscape", "portrait", "square" }, "landscape" }, ask() } });
        registry.Add({ "audio.trim", L"Trim audio", StepKind::Engine,
                       { seconds("start"), seconds("end"), ParamSpec{ "silence", ParamType::Bool, 0, 0, {}, false }, ask() } });
        registry.Add({ "audio.channels", L"Convert audio channels", StepKind::Engine,
                       { ParamSpec{ "mode", ParamType::Enum, 0, 0, { "stereo", "mono" }, "stereo" }, ParamSpec{ "swap", ParamType::Bool, 0, 0, {}, false },
                         ParamSpec{ "left", ParamType::Number, 0, 2, {}, 1.0 }, ParamSpec{ "right", ParamType::Number, 0, 2, {}, 1.0 }, ask() } });
        registry.Add({ "audio.bleep", L"Bleep audio", StepKind::Engine, { ParamSpec{ "tone", ParamType::Enum, 0, 0, { "tone", "silence" }, "tone" } } });
    }

    namespace
    {
        // Normalize asks how loud (Speech, Music or Broadcast) in a small chooser; the other four open
        // the waveform editor on their own tab. A recipe with its own target and "ask": false skips
        // the chooser (docs/TESTING.md, section T).
        const json& ToolRecipes()
        {
            static const json all = json::parse(R"([
  { "id": "audio-normalize", "label": "Normalize", "description": "Normalize Volume: speech, music or broadcast loudness, peaks at \u22121 dBTP", "ring": "tools", "suffix": "normalized", "icon": "E767",
    "steps": [ { "action": "audio.normalize" } ] },
  { "id": "audio-trim", "label": "Trim\u2026", "description": "Trim Audio to exact times, and silence at the ends", "ring": "tools", "suffix": "trimmed", "icon": "E78A",
    "steps": [ { "action": "audio.trim" } ] },
  { "id": "audio-channels", "label": "Channels\u2026", "description": "Convert Audio Channels: mono, stereo, left and right volume, swap", "ring": "tools", "suffix": "channels", "icon": "E7F5",
    "steps": [ { "action": "audio.channels" } ] },
  { "id": "audio-bleep", "label": "Bleep\u2026", "description": "Bleep Audio: a tone or silence over the parts you mark", "ring": "tools", "suffix": "bleeped", "icon": "E74F",
    "steps": [ { "action": "audio.bleep" } ] },
  { "id": "audio-visualizer", "label": "Visualizer\u2026", "description": "Audio Visualizer: an MP4 with a moving waveform or a picture", "ring": "tools", "suffix": "video", "icon": "E9E9",
    "steps": [ { "action": "audio.visualizer" } ] }
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

    bool NeedsEditor(const RecipeStep& step)
    {
        if (step.action == "audio.normalize")
            return step.params.value("ask", true);
        if (step.action == "audio.bleep")
            return true;
        return step.params.value("ask", true);
    }

    bool ChooseLoudness(HWND owner, size_t files, double& target)
    {
        // Command links: a bold name over a line saying what it is for.
        const TASKDIALOG_BUTTON buttons[] = {
            { 16, L"Speech\n\u221216 LUFS, for podcasts, voice notes, talks and lectures" },
            { 14, L"Music\n\u221214 LUFS, the level streaming apps play at" },
            { 23, L"Broadcast\n\u221223 LUFS, the EBU R128 level for TV and radio" },
        };
        const std::wstring content = files > 1 ? std::format(L"All {} files get the same loudness. Peaks stay under \u22121 dBTP.", files)
                                               : std::wstring(L"Peaks stay under \u22121 dBTP.");
        TASKDIALOGCONFIG config{ sizeof(config) };
        config.hwndParent = owner;
        config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        config.pszWindowTitle = L"Normalize Volume";
        config.pszMainInstruction = L"How loud should it be?";
        config.pszContent = content.c_str();
        config.cButtons = static_cast<UINT>(std::size(buttons));
        config.pButtons = buttons;
        config.nDefaultButton = 16;
        // Looked up at run time: TaskDialogIndirect is only in Common Controls 6, which the app's
        // manifest selects; a static import would stop the test runner (no manifest) from loading.
        using TaskDialogFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
        const HMODULE comctl = LoadLibraryW(L"comctl32.dll");
        const auto taskDialog = comctl ? reinterpret_cast<TaskDialogFn>(GetProcAddress(comctl, "TaskDialogIndirect")) : nullptr;
        int picked = 0;
        if (!taskDialog || FAILED(taskDialog(&config, &picked, nullptr, nullptr)) || (picked != 16 && picked != 14 && picked != 23))
            return false;
        target = -picked;
        return true;
    }

    namespace
    {
        double Number(const json& params, const char* key, double fallback)
        {
            const auto it = params.find(key);
            return it != params.end() && it->is_number() ? it->get<double>() : fallback;
        }

        ItemResult RunNormalize(const EngineServices& s, const Recipe& recipe, int64_t runId, const std::wstring& source, const ProbeInfo& info,
                                std::stop_token stop, const ProgressFn& progress)
        {
            const auto target = TargetFor(Number(recipe.steps[0].params, "target", -16));
            // Pass 1 (first half of the progress): measure.
            std::vector<std::string> log;
            const auto half = [&](double offset) {
                return [&progress, offset](double p) {
                    if (progress)
                        progress(offset + p / 2);
                };
            };
            const auto measured = s.ffmpeg->Run(MeasureArgs(source, target), half(0), stop, &log, info.DurationUs());
            if (!measured.ok)
                return FailItem(s.history, runId, source, measured.error, measured.cancelled);
            const auto loudness = ParseLoudnorm(log);
            if (!loudness)
                return FailItem(s.history, runId, source, L"could not measure the loudness");
            if (loudness->silent)
                return FailItem(s.history, runId, source, L"the audio is silent: nothing to normalize");
            if (std::abs(loudness->inputI - target.integrated) < 0.5 && loudness->inputTP <= target.truePeak + 0.1)
            {
                std::wstring level = LoudnessNote(loudness->inputI, loudness->inputI);
                level.erase(0, level.find(L'\u2192') + 2);   // just "-16.0 LUFS"
                return FailItem(s.history, runId, source, L"already at " + level, true);
            }
            log::Info(L"audio: {} measured {:.1f} LUFS, {:.1f} dBTP, LRA {:.1f}; target {}", source, loudness->inputI, loudness->inputTP,
                      loudness->inputLRA, Num(target.integrated));
            // Pass 2: apply, and read loudnorm's own measurement of the result.
            const auto ext = LowerExtension(source);
            log.clear();
            auto result = RunToStaged(*s.ffmpeg, s.history, runId, source,
                                      [&](const std::wstring& out) { return NormalizeArgs(source, ext, info, target, *loudness, out); }, info.DurationUs(),
                                      recipe.suffix, AudioOutputFormat(ext).extension, stop, half(0.5), &log);
            if (result.ok)
            {
                const auto after = ParseLoudnorm(log);
                result.message = L"Loudness " + LoudnessNote(loudness->inputI, after && !after->silent ? after->outputI : target.integrated);
                log::Info(L"audio: normalized {}: {}", source, result.message);
            }
            return result;
        }

        ItemResult RunVisualizer(const EngineServices& s, const Recipe& recipe, int64_t runId, const std::wstring& source, const ProbeInfo& info,
                                 std::stop_token stop, const ProgressFn& progress)
        {
            const auto& p = recipe.steps[0].params;
            const auto shape = ShapeByName(p.value("shape", std::string("landscape")));
            const bool still = p.value("style", std::string("waveform")) == "image";
            std::wstring image = Widen(p.value("image", std::string()));
            TempFolder temp;
            if (still && image.empty() && info.coverArt && temp.Ok())
            {
                // No picture chosen: the song's own cover art.
                const auto cover = temp.File(L"cover.png");
                if (s.ffmpeg->Run(CoverArtArgs(source, cover), nullptr, stop).ok)
                    image = cover;
            }
            const H264Encoder h264 = s.ffmpeg->H264();
            const bool useImage = still && !image.empty();
            log::Info(L"audio: visualizer for {} ({}, {})", source, useImage ? L"picture" : L"waveform", Widen(p.value("shape", std::string("landscape"))));
            return RunToStaged(*s.ffmpeg, s.history, runId, source,
                               [&](const std::wstring& out) {
                                   return useImage ? StillVideoArgs(source, image, info, shape, h264, out) : WaveformVideoArgs(source, info, shape, h264, out);
                               },
                               info.DurationUs(), recipe.suffix, L".mp4", stop, progress);
        }

        ItemResult RunTrim(const EngineServices& s, const Recipe& recipe, int64_t runId, const std::wstring& source, const ProbeInfo& info,
                           std::stop_token stop, const ProgressFn& progress)
        {
            const auto& p = recipe.steps[0].params;
            const int64_t duration = info.DurationUs();
            TimeRange keep{ FromSeconds(Number(p, "start", 0)), 0 };
            const double end = Number(p, "end", 0);
            keep.end = end > 0 ? std::min(FromSeconds(end), duration) : duration;
            if (p.value("silence", false))
            {
                std::vector<std::string> log;
                const auto detect = s.ffmpeg->Run(SilenceDetectArgs(source, keep), nullptr, stop, &log);
                if (detect.cancelled)
                    return FailItem(s.history, runId, source, L"cancelled", true);
                if (detect.ok)
                {
                    const auto trimmed = TrimSilence(keep, ParseSilence(log, keep.start, keep.end));
                    log::Info(L"audio: silence trim {} [{} .. {}] -> [{} .. {}]", source, FormatTimecode(keep.start), FormatTimecode(keep.end),
                              FormatTimecode(trimmed.start), FormatTimecode(trimmed.end));
                    keep = trimmed;
                }
            }
            if (keep.Length() < kMinRangeUs)
                return FailItem(s.history, runId, source, L"the part to keep is empty");
            if (keep.start <= 0 && keep.end >= duration)
                return FailItem(s.history, runId, source, L"nothing to trim: the whole file is kept");
            const auto ext = LowerExtension(source);
            return RunToStaged(*s.ffmpeg, s.history, runId, source, [&](const std::wstring& out) { return TrimAudioArgs(source, ext, info, keep, out); },
                               keep.Length(), recipe.suffix, AudioOutputFormat(ext).extension, stop, progress);
        }

        ItemResult RunChannels(const EngineServices& s, const Recipe& recipe, int64_t runId, const std::wstring& source, const ProbeInfo& info,
                               std::stop_token stop, const ProgressFn& progress)
        {
            const auto& p = recipe.steps[0].params;
            ChannelMix mix;
            mix.mono = p.value("mode", std::string("stereo")) == "mono";
            mix.swap = p.value("swap", false);
            mix.left = Number(p, "left", 1.0);
            mix.right = Number(p, "right", 1.0);
            if (mix.mono && info.channels == 1 && std::abs(mix.left - 1) < 1e-6 && std::abs(mix.right - 1) < 1e-6)
                return FailItem(s.history, runId, source, L"already mono", true);
            if (!mix.mono && !mix.swap && info.channels == 2 && std::abs(mix.left - 1) < 1e-6 && std::abs(mix.right - 1) < 1e-6)
                return FailItem(s.history, runId, source, L"already stereo", true);
            const auto ext = LowerExtension(source);
            // The editor's slice names the result after what it became: "song (mono).mp3", "(stereo)", "(swapped)".
            std::wstring suffix = recipe.suffix;
            if (suffix == L"channels")
                suffix = mix.mono ? L"mono" : info.channels == 1 ? L"stereo" : mix.swap ? L"swapped" : L"channels";
            return RunToStaged(*s.ffmpeg, s.history, runId, source, [&](const std::wstring& out) { return ChannelsArgs(source, ext, info, mix, out); },
                               info.DurationUs(), suffix, AudioOutputFormat(ext).extension, stop, progress);
        }

        ItemResult RunBleep(const EngineServices& s, const Recipe& recipe, int64_t runId, const std::wstring& source, const ProbeInfo& info,
                            std::stop_token stop, const ProgressFn& progress)
        {
            const auto& p = recipe.steps[0].params;
            std::vector<TimeRange> ranges;
            if (const auto it = p.find("ranges"); it != p.end() && it->is_array())
                for (const auto& r : *it)
                    if (r.is_array() && r.size() == 2 && r[0].is_number() && r[1].is_number())
                        ranges.push_back({ FromSeconds(r[0].get<double>()), FromSeconds(r[1].get<double>()) });
            ranges = NormalizeRanges(ranges, info.DurationUs());
            if (ranges.empty())
                return FailItem(s.history, runId, source, L"no parts were marked to bleep");
            const bool tone = p.value("tone", std::string("tone")) != "silence";
            const auto ext = LowerExtension(source);
            log::Info(L"audio: bleep {} range(s) in {} with {}", ranges.size(), source, tone ? L"a tone" : L"silence");
            return RunToStaged(*s.ffmpeg, s.history, runId, source, [&](const std::wstring& out) { return BleepArgs(source, ext, info, ranges, tone, out); },
                               info.DurationUs(), recipe.suffix, AudioOutputFormat(ext).extension, stop, progress);
        }
    }

    ItemResult Run(const EngineServices& s, const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                   const ProgressFn& progress)
    {
        const ProbeInfo info = Probe(*s.ffmpeg, source, stop);
        if (stop.stop_requested())
            return FailItem(s.history, runId, source, L"cancelled", true);
        if (!info.HasAudio())
            return FailItem(s.history, runId, source, L"no audio stream in this file");
        if (info.DurationUs() <= 0)
            return FailItem(s.history, runId, source, L"cannot tell how long this file is");
        const auto& action = recipe.steps[0].action;
        if (action == "audio.normalize") return RunNormalize(s, recipe, runId, source, info, stop, progress);
        if (action == "audio.visualizer") return RunVisualizer(s, recipe, runId, source, info, stop, progress);
        if (action == "audio.trim") return RunTrim(s, recipe, runId, source, info, stop, progress);
        if (action == "audio.channels") return RunChannels(s, recipe, runId, source, info, stop, progress);
        if (action == "audio.bleep") return RunBleep(s, recipe, runId, source, info, stop, progress);
        return FailItem(s.history, runId, source, L"unknown audio tool");
    }
}
