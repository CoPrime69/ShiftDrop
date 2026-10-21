#include "preview/Prediction.h"
#include "OutputPath.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>

namespace sd::preview
{
    Size2 Upright(Size2 s, int orientation)
    {
        return orientation >= 5 && orientation <= 8 ? Size2{ s.h, s.w } : s;
    }

    Size2 OutputDimensions(const Recipe& recipe, Size2 s)
    {
        for (const auto& step : recipe.steps)
        {
            if (step.action == "image.resize")
            {
                const auto maxEdge = step.params.value("maxEdge", 0u);
                if (maxEdge > 0 && std::max(s.w, s.h) > maxEdge)
                {
                    const double k = static_cast<double>(maxEdge) / std::max(s.w, s.h);
                    s = Size2{ std::max(1u, static_cast<uint32_t>(s.w * k + 0.5)), std::max(1u, static_cast<uint32_t>(s.h * k + 0.5)) };
                }
            }
            else if (step.action == "image.scale")
            {
                const double k = step.params.value("percent", 100) / 100.0;
                if (k < 1.0)
                    s = Size2{ std::max(1u, static_cast<uint32_t>(s.w * k + 0.5)), std::max(1u, static_cast<uint32_t>(s.h * k + 0.5)) };
            }
            else if (step.action == "image.rotate")
            {
                const auto degrees = step.params.value("degrees", std::string("90"));
                if (degrees == "90" || degrees == "270")
                    s = Size2{ s.h, s.w };
            }
        }
        return s;
    }

    Orientation RecipeOrientation(const Recipe& recipe)
    {
        Orientation m;
        const auto apply = [&m](int a, int b, int c, int d) {
            // The step comes after everything so far: m = step * m.
            m = Orientation{ a * m.a + b * m.c, a * m.b + b * m.d, c * m.a + d * m.c, c * m.b + d * m.d };
        };
        for (const auto& step : recipe.steps)
        {
            if (step.action == "image.rotate")
            {
                // Clockwise on screen, where y points down.
                const auto degrees = step.params.value("degrees", std::string("90"));
                if (degrees == "90")
                    apply(0, -1, 1, 0);
                else if (degrees == "180")
                    apply(-1, 0, 0, -1);
                else if (degrees == "270")
                    apply(0, 1, -1, 0);
            }
            else if (step.action == "image.flip")
            {
                if (step.params.value("axis", std::string("horizontal")) == "vertical")
                    apply(1, 0, 0, -1);
                else
                    apply(-1, 0, 0, 1);
            }
        }
        return m;
    }

    Recipe SampleRecipe(const Recipe& recipe, uint32_t maxEdge)
    {
        Recipe sample = recipe;
        RecipeStep resize;
        resize.action = "image.resize";
        resize.params = nlohmann::json{ { "maxEdge", maxEdge } };
        const auto at = sample.steps.empty() ? sample.steps.end() : sample.steps.end() - 1;
        sample.steps.insert(at, std::move(resize));
        return sample;
    }

    double FitExponent(double b1, double p1, double b2, double p2)
    {
        if (b1 <= 0 || b2 <= 0 || p1 <= 0 || p2 <= 0 || std::abs(std::log(p2 / p1)) < 1e-6)
            return 1.0;
        return std::clamp(std::log(b2 / b1) / std::log(p2 / p1), 0.5, 1.05);
    }

    uint64_t Extrapolate(double bytes, double pixels, double target, double k)
    {
        if (bytes <= 0 || pixels <= 0 || target <= 0)
            return 0;
        return static_cast<uint64_t>(std::llround(bytes * std::pow(target / pixels, k)));
    }

    std::wstring SizeText(uint64_t bytes)
    {
        if (bytes < 1024)
            return std::format(L"{} bytes", bytes);
        const double kb = static_cast<double>(bytes) / 1024.0;
        if (kb < 1000.0)
            return std::format(L"{:.0f} KB", kb);
        const double mb = kb / 1024.0;
        if (mb < 1000.0)
            return mb < 10.0 ? std::format(L"{:.1f} MB", mb) : std::format(L"{:.0f} MB", mb);
        return std::format(L"{:.1f} GB", mb / 1024.0);
    }

    std::wstring DimensionsChange(Size2 from, Size2 to)
    {
        if (from == to || from.w == 0 || to.w == 0)
            return {};
        return std::format(L"{}×{} → {}×{}", from.w, from.h, to.w, to.h);
    }

    std::wstring MetadataLoss(const MetadataFacts& f)
    {
        std::vector<const wchar_t*> parts;
        if (f.gps) parts.push_back(L"GPS");
        if (f.camera) parts.push_back(L"camera");
        if (f.dateTaken) parts.push_back(L"date taken");
        if (f.author) parts.push_back(L"author");
        if (f.xmp && parts.empty()) parts.push_back(L"XMP");   // only worth naming when nothing more specific is
        if (parts.empty())
            return {};
        std::wstring text = L"removes ";
        for (size_t i = 0; i < parts.size(); ++i)
            text += (i ? L", " : L"") + std::wstring(parts[i]);
        return text;
    }

    std::wstring PredictOutputName(const std::wstring& source, const std::wstring& suffix, const std::wstring& extension)
    {
        const std::filesystem::path src(source);
        const std::filesystem::path folder(OutputFolder(source));   // a dragged-in file's outputs go elsewhere
        const auto stem = src.stem().wstring();
        // Outputs in their own "ShiftDrop" folder say so, as that is where to look for them.
        const std::wstring prefix = folder != src.parent_path() && folder.filename() == kOutputSubfolder ? std::wstring(kOutputSubfolder) + L"\\" : L"";
        for (int n = 1; n < 1000; ++n)
        {
            const auto name = OutputName(stem, suffix, n, extension);
            if (GetFileAttributesW((folder / name).c_str()) == INVALID_FILE_ATTRIBUTES)
                return prefix + name;
        }
        return prefix + OutputName(stem, suffix, 1000, extension);
    }

    int64_t ParseBitrate(const std::wstring& text)
    {
        if (text.empty())
            return 0;
        double value = 0;
        size_t used = 0;
        try
        {
            value = std::stod(text, &used);
        }
        catch (...)
        {
            return 0;
        }
        double unit = 1;
        if (used < text.size())
        {
            const wchar_t u = text[used];
            unit = u == L'k' || u == L'K' ? 1e3 : u == L'm' || u == L'M' ? 1e6 : u == L'g' || u == L'G' ? 1e9 : 0;
        }
        return static_cast<int64_t>(value * unit);
    }

    namespace
    {
        // The value after `key` in an argument list, or empty.
        std::wstring After(const std::vector<std::wstring>& args, std::wstring_view key)
        {
            for (size_t i = 0; i + 1 < args.size(); ++i)
                if (args[i] == key)
                    return args[i + 1];
            return {};
        }

        bool Has(const std::vector<std::wstring>& args, std::wstring_view key)
        {
            return std::find(args.begin(), args.end(), key) != args.end();
        }

        int Number(const std::wstring& s, int fallback)
        {
            try
            {
                return s.empty() ? fallback : std::stoi(s);
            }
            catch (...)
            {
                return fallback;
            }
        }

        uint32_t Even(double v) { return static_cast<uint32_t>(std::max(2.0, std::floor(v / 2.0) * 2.0)); }
    }

    MediaEstimate EstimateMedia(const std::vector<std::wstring>& args, const MediaInfo& info, const StreamFacts& facts, uint64_t sourceBytes)
    {
        MediaEstimate e;
        const double seconds = info.durationUs > 0 ? static_cast<double>(info.durationUs) / 1e6 : 0.0;
        const std::wstring format = After(args, L"-f");

        // MKV and friends: "-map 0 -c copy" keeps every stream untouched.
        if (After(args, L"-c") == L"copy")
        {
            e.known = true;
            e.sameStreams = true;
            e.bytes = sourceBytes;
            e.video = !info.videoCodec.empty();
            e.audio = !info.audioCodec.empty();
            e.size = Size2{ static_cast<uint32_t>(info.width), static_cast<uint32_t>(info.height) };
            e.fps = info.fps;
            return e;
        }

        e.video = !info.videoCodec.empty() && !Has(args, L"-vn");
        e.audio = !info.audioCodec.empty() && !Has(args, L"-an");
        const std::wstring vcodec = After(args, L"-c:v"), acodec = After(args, L"-c:a");
        const bool copyVideo = vcodec == L"copy", copyAudio = acodec == L"copy";
        e.sameStreams = (!e.video || copyVideo) && (!e.audio || copyAudio);

        // Picture size and frame rate after the filters.
        double fps = info.fps > 0 ? std::min(info.fps, 60.0) : 30.0;
        Size2 size{ static_cast<uint32_t>(std::max(info.width, 0)), static_cast<uint32_t>(std::max(info.height, 0)) };
        const std::wstring vf = After(args, L"-vf");
        if (format == L"gif")
        {
            fps = 12;
            if (size.w > 480)
                size = Size2{ 480, Even(480.0 * size.h / size.w) };
        }
        else if (const auto at = vf.find(L"scale=-2:"); at != std::wstring::npos && size.h > 0)
        {
            const int h = Number(vf.substr(at + 9, vf.find(L':', at + 9) - at - 9), static_cast<int>(size.h));
            size = Size2{ Even(static_cast<double>(size.w) * h / size.h), static_cast<uint32_t>(h) };
        }
        else if (!copyVideo && size.w > 0)
        {
            size = Size2{ size.w & ~1u, size.h & ~1u };   // the even-size crop
        }
        if (e.video)
        {
            e.size = size;
            e.fps = fps;
        }

        // Video bitrate: copied, given, or from the quality setting in bits per pixel per frame.
        double video = 0;
        if (e.video)
        {
            const double pixelsPerSecond = static_cast<double>(size.w) * size.h * fps;
            if (copyVideo)
                video = facts.videoBps > 0 ? static_cast<double>(facts.videoBps)
                        : seconds > 0      ? std::max(0.0, sourceBytes * 8.0 / seconds - static_cast<double>(facts.audioBps))
                                           : 0.0;
            else if (format == L"gif")
                video = pixelsPerSecond * 0.3 * 8.0;   // LZW of a dithered palette: about 0.3 bytes per pixel per frame
            else if (const auto b = ParseBitrate(After(args, L"-b:v")); b > 0)
                video = static_cast<double>(b);
            else
            {
                // H.264 constant quality: about 0.1 bits per pixel at 23, halving every 6 steps.
                std::wstring q = After(args, L"-cq");
                if (q.empty()) q = After(args, L"-global_quality");
                if (q.empty()) q = After(args, L"-qp_p");
                double bpp = 0.1;
                if (!q.empty())
                    bpp = 0.1 * std::pow(2.0, (23.0 - Number(q, 23)) / 6.0);
                else if (vcodec == L"libvpx-vp9")
                    bpp = 0.07 * std::pow(2.0, (32.0 - Number(After(args, L"-crf"), 32)) / 8.0);   // realtime mode, speed 6
                else if (vcodec == L"mpeg4")
                    bpp = 0.15 * 3.0 / std::max(1, Number(After(args, L"-q:v"), 3));
                video = pixelsPerSecond * bpp;
            }
        }

        double audio = 0;
        if (e.audio)
        {
            const double rate = facts.sampleRate > 0 ? facts.sampleRate : 44100.0;
            const double channels = facts.channels > 0 ? facts.channels : 2.0;
            const double pcm = rate * channels * 16.0;
            if (copyAudio)
                audio = facts.audioBps > 0 ? static_cast<double>(facts.audioBps) : 128'000.0;
            else if (const auto b = ParseBitrate(After(args, L"-b:a")); b > 0)
                audio = static_cast<double>(b);
            else if (acodec.starts_with(L"pcm_"))
                audio = pcm;
            else if (acodec == L"flac")
                audio = pcm * 0.55;   // lossless compression of typical music
            else if (acodec == L"libmp3lame")
            {
                // LAME's VBR quality levels, -q:a 0 (best) to 9.
                static constexpr int kKbps[] = { 245, 225, 190, 175, 165, 130, 115, 100, 85, 65 };
                audio = kKbps[std::clamp(Number(After(args, L"-q:a"), 4), 0, 9)] * 1000.0;
            }
            else if (acodec == L"libvorbis")
            {
                // Vorbis -q:a 0..10, roughly 64 to 500 kbit/s; 5 is about 160.
                static constexpr int kKbps[] = { 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 500 };
                audio = kKbps[std::clamp(Number(After(args, L"-q:a"), 5), 0, 10)] * 1000.0;
            }
            else
                audio = 128'000.0;
        }

        if (seconds <= 0)
        {
            // No duration: only a copy can be sized (about the input).
            e.known = e.sameStreams;
            e.bytes = e.sameStreams ? sourceBytes : 0;
            return e;
        }
        e.known = true;
        // 1.5% for the container (headers, index, interleaving).
        e.bytes = static_cast<uint64_t>((video + audio) * seconds / 8.0 * 1.015);
        if (e.sameStreams && sourceBytes > 0)
            e.bytes = sourceBytes;
        return e;
    }
}
