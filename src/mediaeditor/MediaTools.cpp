#include "mediaeditor/MediaTools.h"
#include "Brand.h"
#include "FileIo.h"
#include "OutputPath.h"
#include "StagedOutput.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>

namespace sd::mediatools
{
    // ---- Command lines ---------------------------------------------------------------------------

    std::vector<std::wstring> MultiInputArgs(const std::vector<Input>& inputs, const std::vector<std::wstring>& outputArgs,
                                             const std::wstring& output)
    {
        if (inputs.empty())
            return {};
        // FfmpegArgs whitelists its first input; each later input gets its own whitelist right after
        // the -i before it.
        std::vector<std::wstring> before;
        for (size_t i = 0; i + 1 < inputs.size(); ++i)
        {
            before.insert(before.end(), inputs[i].options.begin(), inputs[i].options.end());
            before.insert(before.end(), { L"-i", inputs[i].path, L"-protocol_whitelist", L"file" });
        }
        before.insert(before.end(), inputs.back().options.begin(), inputs.back().options.end());
        return FfmpegArgs(inputs.back().path, before, outputArgs, output);
    }

    std::wstring Seconds(int64_t us)
    {
        us = std::max<int64_t>(0, us);
        return std::format(L"{}.{:06}", us / 1'000'000, us % 1'000'000);
    }

    std::wstring Num(double value)
    {
        const double r = std::round(value * 1e6) / 1e6;
        return std::format(L"{}", r);
    }

    std::wstring FpsExpr(double fps)
    {
        // NTSC rates are 1000/1001 of a whole rate; "29.97" from the log would drift a frame every few
        // minutes against the real 30000/1001.
        for (const int whole : { 24, 30, 48, 60, 120 })
            if (std::abs(fps - whole * 1000.0 / 1001.0) < 0.006)
                return std::format(L"{}/1001", whole * 1000);
        return Num(fps);
    }

    double ToSeconds(int64_t us) { return static_cast<double>(us) / 1e6; }
    int64_t FromSeconds(double seconds) { return std::isfinite(seconds) ? static_cast<int64_t>(std::llround(seconds * 1e6)) : 0; }

    std::vector<int64_t> TimesFrom(const nlohmann::json& params, const char* key)
    {
        std::vector<int64_t> out;
        if (const auto it = params.find(key); it != params.end() && it->is_array())
            for (const auto& v : *it)
                if (v.is_number())
                    out.push_back(FromSeconds(v.get<double>()));
        return out;
    }

    // ---- Where an edited file goes -----------------------------------------------------------------

    Container CopyContainer(const std::wstring& ext)
    {
        if (ext == L".mp4" || ext == L".m4v") return { L"mp4", L".mp4" };
        if (ext == L".mov" || ext == L".qt") return { L"mov", L".mov" };
        if (ext == L".mkv") return { L"matroska", L".mkv" };
        if (ext == L".webm") return { L"webm", L".webm" };
        if (ext == L".avi") return { L"avi", L".avi" };
        if (ext == L".ts" || ext == L".mts" || ext == L".m2ts") return { L"mpegts", ext };
        if (ext == L".3gp") return { L"3gp", L".3gp" };
        return { L"matroska", L".mkv" };
    }

    Container EncodeContainer(const std::wstring& ext)
    {
        if (ext == L".mov" || ext == L".qt") return { L"mov", L".mov" };
        if (ext == L".mkv") return { L"matroska", L".mkv" };
        return { L"mp4", L".mp4" };
    }

    bool ContainerTakesAudio(const Container& c, const std::string& codec)
    {
        if (codec.empty())
            return false;
        const auto one = [&](std::initializer_list<const char*> codecs) {
            return std::any_of(codecs.begin(), codecs.end(), [&](const char* x) { return codec == x; });
        };
        if (c.muxer == L"matroska") return true;
        if (c.muxer == L"mp4") return one({ "aac", "mp3", "alac" });
        if (c.muxer == L"mov") return one({ "aac", "mp3", "alac", "pcm_s16le", "pcm_s16be", "pcm_s24le" });
        if (c.muxer == L"webm") return one({ "opus", "vorbis" });
        return false;
    }

    std::vector<std::wstring> EditVideoArgs(H264Encoder encoder, int width, int height, double fps)
    {
        // Edits should look like the original: quality 20, or about 0.12 bits per pixel per frame.
        const double rate = fps > 0 ? std::min(fps, 60.0) : 30.0;
        const double bits = static_cast<double>(std::max(width, 2)) * std::max(height, 2) * rate * 0.12;
        const auto bps = static_cast<int64_t>(std::clamp(bits, 2'000'000.0, 50'000'000.0));
        return H264QualityArgs(encoder, 20, bps);
    }

    std::vector<std::wstring> DecodeArgs(H264Encoder encoder)
    {
        if (encoder == H264Encoder::Nvenc || encoder == H264Encoder::Qsv || encoder == H264Encoder::Amf)
            return { L"-hwaccel", L"auto" };
        return {};
    }

    std::vector<std::wstring> SoundtrackArgs(const Container& container, const ProbeInfo& info)
    {
        if (!info.HasAudio())
            return { L"-an" };
        if (ContainerTakesAudio(container, info.media.audioCodec))
            return { L"-c:a", L"copy" };
        return { L"-c:a", L"aac", L"-b:a", L"192k" };
    }

    const MediaFormatInfo& AudioOutputFormat(const std::wstring& ext)
    {
        for (const auto& f : MediaFormats())
            if (f.fromAudio && IsAlreadyMediaFormat(f, ext))
                return f;
        return *FindMediaFormat("m4a");
    }

    std::vector<std::wstring> AudioCodecArgs(const MediaFormatInfo& format, int sampleRate)
    {
        const auto all = OutputArgs(format, MediaPlan{});
        std::vector<std::wstring> a;
        for (size_t i = 0; i < all.size(); ++i)
        {
            if (all[i] == L"-map")
            {
                ++i;   // and its value
                continue;
            }
            if (all[i] == L"-vn")
                continue;
            a.push_back(all[i]);
        }
        // Keep the source's rate (loudnorm works at 192 kHz inside); Opus only takes 48 kHz.
        if (std::string_view(format.id) == "opus")
            a.insert(a.begin(), { L"-ar", L"48000" });
        else if (sampleRate > 0)
            a.insert(a.begin(), { L"-ar", std::to_wstring(sampleRate) });
        return a;
    }

    // ---- Extraction for the editor -------------------------------------------------------------------

    void FitFrame(const ProbeInfo& info, int maxW, int maxH, int& w, int& h)
    {
        const double sw = std::max(1, info.width) * (info.sar > 0 ? info.sar : 1.0);
        const double sh = std::max(1, info.height);
        const double scale = std::min({ maxW / sw, maxH / sh, 1.0 });   // never larger than the source
        w = std::max(2, static_cast<int>(sw * scale) & ~1);
        h = std::max(2, static_cast<int>(sh * scale) & ~1);
    }

    int64_t FrameSeekUs(int64_t us, double fps)
    {
        const int64_t frame = FrameDurationUs(fps);
        return std::max<int64_t>(0, us - frame / 4);
    }

    std::vector<std::wstring> FrameArgs(const std::wstring& input, int64_t us, double fps, int w, int h, bool fast, const std::wstring& output)
    {
        std::vector<std::wstring> in;
        if (fast)
            // Keyframes only, and the one at or before `us` (no decoding up to the exact frame).
            in = { L"-skip_frame", L"nokey", L"-noaccurate_seek", L"-ss", Seconds(us) };
        else
            in = { L"-ss", Seconds(FrameSeekUs(us, fps)) };
        // passthrough: the keyframe before `us` comes out with a negative time, which would be dropped.
        const std::vector<std::wstring> out{ L"-map", L"0:v:0", L"-an", L"-sn", L"-dn", L"-frames:v", L"1", L"-fps_mode", L"passthrough",
                                             L"-vf", std::format(L"scale={}:{}:flags={}", w, h, fast ? L"bilinear" : L"bicubic"),
                                             L"-pix_fmt", L"bgra", L"-f", L"rawvideo" };
        return FfmpegArgs(input, in, out, output);
    }

    std::vector<std::wstring> MonoPcmArgs(const std::wstring& input, int sampleRate, const std::wstring& output)
    {
        return FfmpegArgs(input, {}, { L"-map", L"0:a:0", L"-vn", L"-sn", L"-dn", L"-ac", L"1", L"-ar", std::to_wstring(sampleRate),
                                       L"-c:a", L"pcm_s16le", L"-f", L"s16le" }, output);
    }

    std::vector<std::wstring> PlaybackPcmArgs(const std::wstring& input, TimeRange range, const std::wstring& filter, bool complex,
                                              int sampleRate, const std::wstring& output)
    {
        std::vector<std::wstring> in{ L"-ss", Seconds(range.start) };
        if (range.end > range.start)
            in.insert(in.end(), { L"-t", Seconds(range.Length()) });
        std::vector<std::wstring> out;
        if (complex && !filter.empty())
            out = { L"-filter_complex", filter, L"-map", L"[out]" };
        else
        {
            out = { L"-map", L"0:a:0" };
            if (!filter.empty())
                out.insert(out.end(), { L"-af", filter });
        }
        out.insert(out.end(), { L"-vn", L"-sn", L"-dn", L"-ac", L"2", L"-ar", std::to_wstring(sampleRate), L"-c:a", L"pcm_s16le", L"-f", L"s16le" });
        return FfmpegArgs(input, in, out, output);
    }

    int WaveformRate(int64_t durationUs)
    {
        // About eight million samples at most (16 MB), never coarser than 200 Hz or finer than 8 kHz.
        if (durationUs <= 0)
            return 8000;
        const double rate = 8e6 / (static_cast<double>(durationUs) / 1e6);
        return std::clamp(static_cast<int>(rate / 100) * 100, 200, 8000);
    }

    namespace
    {
        bool ReadPcm(const std::wstring& path, std::vector<int16_t>& out)
        {
            std::string bytes;
            if (!ReadFileBytes(path, bytes))
                return false;
            out.resize(bytes.size() / 2);
            std::memcpy(out.data(), bytes.data(), out.size() * 2);
            DeleteFileW(path.c_str());
            return true;
        }
    }

    bool ExtractFrame(const FfmpegEngine& engine, const std::wstring& input, const ProbeInfo& info, int64_t us, int maxW, int maxH,
                      bool fast, const TempFolder& temp, std::stop_token stop, Frame& out)
    {
        if (!temp.Ok() || !info.HasVideo())
            return false;
        int w = 0, h = 0;
        FitFrame(info, maxW, maxH, w, h);
        static std::atomic<unsigned> serial{ 0 };
        const auto file = temp.File(std::format(L"frame{}.bgra", serial++));
        us = std::min(us, LastFrameTime(info.media.fps, info.DurationUs()));
        if (!engine.Run(FrameArgs(input, us, info.media.fps, w, h, fast, file), nullptr, stop).ok)
            return false;
        std::string bytes;
        const bool read = ReadFileBytes(file, bytes);
        DeleteFileW(file.c_str());
        if (!read || bytes.size() < static_cast<size_t>(w) * h * 4)
            return false;
        out.w = w;
        out.h = h;
        out.bgra.resize(static_cast<size_t>(w) * h);
        std::memcpy(out.bgra.data(), bytes.data(), out.bgra.size() * 4);
        for (auto& p : out.bgra)
            p |= 0xFF000000u;
        return true;
    }

    bool ExtractMonoPcm(const FfmpegEngine& engine, const std::wstring& input, int sampleRate, const TempFolder& temp, std::stop_token stop,
                        std::vector<int16_t>& out, const ProgressFn& progress, int64_t durationUs)
    {
        if (!temp.Ok())
            return false;
        const auto file = temp.File(L"waveform.pcm");
        if (!engine.Run(MonoPcmArgs(input, sampleRate, file), progress, stop, nullptr, durationUs).ok)
            return false;
        return ReadPcm(file, out);
    }

    bool DecodePlaybackPcm(const FfmpegEngine& engine, const std::wstring& input, TimeRange range, const std::wstring& filter, bool complex,
                           int sampleRate, const TempFolder& temp, std::stop_token stop, std::vector<int16_t>& out)
    {
        if (!temp.Ok())
            return false;
        static std::atomic<unsigned> serial{ 0 };
        const auto file = temp.File(std::format(L"play{}.pcm", serial++));
        const auto r = engine.Run(PlaybackPcmArgs(input, range, filter, complex, sampleRate, file), nullptr, stop);
        if (!r.ok)
        {
            DeleteFileW(file.c_str());
            return false;
        }
        return ReadPcm(file, out);
    }

    // ---- Running a tool ------------------------------------------------------------------------------

    ItemResult RunToStaged(const FfmpegEngine& engine, HistoryStore* history, int64_t runId, const std::wstring& source,
                           const std::function<std::vector<std::wstring>(const std::wstring& staging)>& args, int64_t outputUs,
                           std::wstring_view suffix, std::wstring_view extension, std::stop_token stop, const ProgressFn& progress,
                           std::vector<std::string>* log)
    {
        StagedOutput out(history, runId, source);
        if (!out.Ready())
            return out.Fail(out.Error());
        const auto r = engine.Run(args(out.Path()), progress, stop, log, outputUs);
        if (!r.ok)
            return out.Fail(r.error, r.cancelled);
        return out.Commit(suffix, extension);
    }

    ItemResult RunToStagedFolder(HistoryStore* history, int64_t runId, const std::wstring& source, int steps,
                                 const std::function<FfmpegEngine::Result(int i, const std::wstring& folder, const ProgressFn& progress)>& run,
                                 std::wstring_view suffix, std::stop_token stop, const ProgressFn& progress)
    {
        StagedOutput out(history, runId, source, true);
        if (!out.Ready())
            return out.Fail(out.Error());
        for (int i = 0; i < steps; ++i)
        {
            if (stop.stop_requested())
                return out.Fail(L"cancelled", true);
            const auto r = run(i, out.Path(), [&](double p) {
                if (progress)
                    progress((i + std::clamp(p, 0.0, 1.0)) / steps);
            });
            if (!r.ok)
                return out.Fail(r.error, r.cancelled);
            if (progress)
                progress(static_cast<double>(i + 1) / steps);
        }
        return out.Commit(suffix, L"");
    }

    std::wstring EscapePercent(const std::wstring& text)
    {
        std::wstring out;
        for (const wchar_t c : text)
        {
            out += c;
            if (c == L'%')
                out += L'%';
        }
        return out;
    }

    std::wstring Stem(const std::wstring& path) { return std::filesystem::path(path).stem().wstring(); }
}
