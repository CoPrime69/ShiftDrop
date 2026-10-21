#include "preview/Predictor.h"
#include "FormatText.h"
#include "ImageHooks.h"
#include "ImagePipeline.h"
#include "Log.h"
#include "OutputPath.h"
#include "PathUtil.h"
#include "Utf.h"
#include "VirtualDrop.h"
#include "Wic.h"
#include "formats/Frames.h"
#include "inspect/MetadataReader.h"

#include <windows.h>
#include <wincodec.h>
#include <wil/resource.h>
#include <wrl/client.h>

#include <webp/decode.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <format>
#include <initializer_list>

using Microsoft::WRL::ComPtr;

namespace sd::preview
{
    // What the predictor knows about one file, whatever the recipe.
    struct Predictor::Facts
    {
        std::wstring key;   // path|size|modified
        uint64_t bytes = 0;
        bool image = false;
        Size2 upright;      // after EXIF orientation
        MetadataFacts meta;
        bool media = false;
        bool probed = false;
        MediaInfo info;
        StreamFacts streams;
        std::shared_ptr<const Thumbnail> thumb;
    };

    namespace
    {
        constexpr size_t kBatchSample = 24;          // files sized one by one; the rest scale with their bytes
        constexpr double kExactPixels = 1.2e6;       // up to this many output pixels, encode the whole picture
        constexpr uint32_t kSmallSample = 512, kLargeSample = 1024;
        constexpr size_t kCacheLimit = 2000;

        // Whether any of `names` holds a value.
        bool Found(IWICMetadataQueryReader* reader, std::initializer_list<const wchar_t*> names)
        {
            for (const wchar_t* name : names)
            {
                PROPVARIANT v;
                PropVariantInit(&v);
                const bool found = SUCCEEDED(reader->GetMetadataByName(name, &v)) && v.vt != VT_EMPTY;
                PropVariantClear(&v);
                if (found)
                    return true;
            }
            return false;
        }

        // Size, EXIF orientation and the metadata worth naming, from the file's header (no pixels).
        bool ReadImageFacts(const std::wstring& path, Size2& upright, MetadataFacts& meta)
        {
            // AVIF, SVG (src/formats): the hook reads the size from the header; outputs carry no metadata anyway.
            if (const auto* hook = DecodeHookFor(LowerExtension(path)))
            {
                uint32_t w = 0, h = 0;
                if (!hook->ReadSize(path, w, h))
                    return false;
                upright = Size2{ w, h };
                return true;
            }
            if (LowerExtension(path) == L".webp")
            {
                wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
                uint8_t head[4096];
                DWORD read = 0;
                int w = 0, h = 0;
                if (!file || !ReadFile(file.get(), head, sizeof(head), &read, nullptr) || !WebPGetInfo(head, read, &w, &h))
                    return false;
                upright = Size2{ static_cast<uint32_t>(w), static_cast<uint32_t>(h) };
                return true;
            }
            const auto factory = wic::Factory();
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            if (!factory ||
                FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
                FAILED(decoder->GetFrame(0, &frame)))
                return false;
            UINT w = 0, h = 0;
            frame->GetSize(&w, &h);
            const int orientation = wic::ExifOrientation(frame.Get());
            ComPtr<IWICMetadataQueryReader> reader;
            if (SUCCEEDED(frame->GetMetadataQueryReader(&reader)))
            {
                // Metadata policy names first (WIC maps them to the container), then the raw EXIF and XMP paths.
                meta.gps = Found(reader.Get(), { L"System.GPS.Latitude", L"/app1/ifd/gps/{ushort=2}", L"/ifd/gps/{ushort=2}", L"/xmp/exif:GPSLatitude" });
                meta.camera = Found(reader.Get(), { L"System.Photo.CameraModel", L"System.Photo.CameraManufacturer", L"/app1/ifd/{ushort=272}",
                                                    L"/app1/ifd/{ushort=271}", L"/ifd/{ushort=272}", L"/ifd/{ushort=271}" });
                meta.dateTaken = Found(reader.Get(), { L"System.Photo.DateTaken", L"/app1/ifd/exif/{ushort=36867}", L"/ifd/exif/{ushort=36867}" });
                meta.author = Found(reader.Get(), { L"System.Author", L"/app1/ifd/{ushort=315}", L"/ifd/{ushort=315}", L"/xmp/dc:creator" });
                meta.xmp = Found(reader.Get(), { L"/xmp", L"/ifd/xmp" });
            }
            upright = Upright(Size2{ w, h }, orientation);
            return true;
        }

        // Encodes the recipe into memory and returns the byte count (0 on failure).
        uint64_t EncodedBytes(const Recipe& recipe, const std::wstring& path, std::stop_token stop)
        {
            ComPtr<IStream> memory;
            if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &memory)))
                return 0;
            std::wstring error;
            if (FAILED(RunImageRecipe(recipe, path, memory.Get(), stop, error)))
                return 0;
            STATSTG stat{};
            return SUCCEEDED(memory->Stat(&stat, STATFLAG_NONAME)) ? stat.cbSize.QuadPart : 0;
        }

        std::wstring Upper(std::wstring s)
        {
            for (auto& c : s)
                c = static_cast<wchar_t>(std::towupper(c));
            return s;
        }

        std::wstring Join(const std::vector<std::wstring>& parts)
        {
            std::wstring out;
            for (const auto& p : parts)
                if (!p.empty())
                    out += (out.empty() ? L"" : L" · ") + p;
            return out;
        }
    }

    bool Predictor::Predicts(const Recipe& recipe)
    {
        if (recipe.steps.empty())
            return false;
        if (recipe.IsFileAction())
            return recipe.steps[0].action == "file.duplicate";
        if (recipe.IsMedia())
            return true;
        const auto& last = recipe.steps.back().action;
        return last == "image.encode" || last == "image.compress";
    }

    Predictor::Predictor(std::shared_ptr<const FfmpegEngine> media, Deliver deliver, ThumbnailFn thumbnails)
        : m_media(std::move(media)), m_deliver(std::move(deliver)), m_thumbnails(std::move(thumbnails)),
          m_thread([this](std::stop_token stop) { Loop(stop); })
    {
    }

    Predictor::~Predictor()
    {
        {
            std::scoped_lock lock(m_mutex);
            m_current.request_stop();
            m_pending.reset();
        }
        m_thread.request_stop();
        m_wake.notify_all();
    }   // m_thread joins here, before the members it uses go

    void Predictor::Request(uint64_t token, std::vector<std::wstring> files, Recipe recipe)
    {
        {
            std::scoped_lock lock(m_mutex);
            m_current.request_stop();
            m_pending = Job{ token, std::move(files), std::move(recipe) };
        }
        m_wake.notify_one();
    }

    void Predictor::Cancel()
    {
        std::scoped_lock lock(m_mutex);
        m_current.request_stop();
        m_pending.reset();
    }

    void Predictor::Loop(std::stop_token stop)
    {
        // MTA: WIC, the shell's thumbnail factory and the property system all work there, and nothing
        // here needs a message loop.
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto uninit = wil::scope_exit([hr] { if (SUCCEEDED(hr)) CoUninitialize(); });
        for (;;)
        {
            Job job;
            std::stop_token jobStop;
            {
                std::unique_lock lock(m_mutex);
                m_wake.wait(lock, stop, [&] { return m_pending.has_value(); });
                if (stop.stop_requested())
                    return;
                job = std::move(*m_pending);
                m_pending.reset();
                m_current = std::stop_source();
                jobStop = m_current.get_token();
            }
            try
            {
                auto result = Predict(job.token, job.files, job.recipe, jobStop, m_deliver);
                if (!jobStop.stop_requested() && m_deliver)
                    m_deliver(std::move(result));
            }
            catch (const std::exception& e)
            {
                log::Warn(L"preview: prediction failed: {}", std::wstring(e.what(), e.what() + strlen(e.what())));
            }
        }
    }

    std::shared_ptr<const Predictor::Facts> Predictor::FactsFor(const std::wstring& path, bool probe, std::stop_token stop)
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            return nullptr;
        const uint64_t bytes = FileSizeOf(data);
        const auto key = std::format(L"{}|{}|{}|{}", path, bytes, data.ftLastWriteTime.dwHighDateTime, data.ftLastWriteTime.dwLowDateTime);
        {
            std::scoped_lock lock(m_cacheMutex);
            if (auto it = m_facts.find(key); it != m_facts.end() && (it->second->probed || !probe || !it->second->media))
                return it->second;
        }
        auto facts = std::make_shared<Facts>();
        facts->key = key;
        facts->bytes = bytes;
        const auto ext = LowerExtension(path);
        facts->media = MediaKindOfExtension(ext) != MediaKind::None;
        if (!facts->media)
            facts->image = ReadImageFacts(path, facts->upright, facts->meta);
        if (facts->media && probe && m_media && m_media->Available() && !stop.stop_requested())
        {
            const auto report = inspect::Probe(*m_media, path, stop, &facts->info);
            if (stop.stop_requested())
                return nullptr;   // a killed probe says nothing; try again next time
            for (const auto& s : report.streams)
            {
                if (s.kind == "Video" && !s.attachedPic && facts->streams.videoBps == 0)
                    facts->streams.videoBps = s.bitrate;
                if (s.kind == "Audio" && facts->streams.audioBps == 0)
                {
                    facts->streams.audioBps = s.bitrate;
                    facts->streams.sampleRate = s.sampleRate;
                    facts->streams.channels = inspect::ChannelCount(s.channels);
                }
            }
            facts->probed = true;
        }
        // The thumbnail of the previous facts for this file, if any, saves asking the shell again.
        {
            std::scoped_lock lock(m_cacheMutex);
            if (auto it = m_facts.find(key); it != m_facts.end())
                facts->thumb = it->second->thumb;
        }
        if (!facts->thumb && m_thumbnails && !stop.stop_requested())
            facts->thumb = m_thumbnails(path);
        std::scoped_lock lock(m_cacheMutex);
        if (m_facts.size() > kCacheLimit)
            m_facts.clear();
        m_facts[key] = facts;
        return facts;
    }

    std::optional<Predictor::Estimate> Predictor::EstimateImage(const std::wstring& path, const Facts& f, const Recipe& recipe, std::stop_token stop)
    {
        if (!f.image)
            return std::nullopt;
        Estimate e;
        e.size = OutputDimensions(recipe, f.upright);
        const double pixels = e.size.Pixels();
        if (pixels <= kExactPixels)
        {
            e.bytes = EncodedBytes(recipe, path, stop);
            e.exact = true;
        }
        else
        {
            const auto lowRes = SampleRecipe(recipe, kSmallSample), highRes = SampleRecipe(recipe, kLargeSample);
            const double p1 = OutputDimensions(lowRes, f.upright).Pixels(), p2 = OutputDimensions(highRes, f.upright).Pixels();
            const auto b1 = EncodedBytes(lowRes, path, stop);
            if (stop.stop_requested())
                return std::nullopt;
            const auto b2 = EncodedBytes(highRes, path, stop);
            e.bytes = Extrapolate(static_cast<double>(b2), p2, pixels, FitExponent(static_cast<double>(b1), p1, static_cast<double>(b2), p2));
        }
        if (stop.stop_requested() || e.bytes == 0)
            return std::nullopt;
        e.known = true;
        // Compress / Fit to…: the runner lowers the quality (and then the size) until it fits.
        if (recipe.steps.back().action == "image.compress")
        {
            const auto& p = recipe.steps.back().params;
            uint64_t target = 0;
            if (const int kb = p.value("maxKB", 0); kb > 0)
                target = static_cast<uint64_t>(kb) * 1024;
            else if (const int percent = p.value("percent", 0); percent > 0)
                target = f.bytes * static_cast<uint64_t>(percent) / 100;
            if (target > 0 && e.bytes > target)
            {
                e.bytes = target;
                e.capped = true;
                e.exact = false;
            }
        }
        return e;
    }

    std::optional<Predictor::Estimate> Predictor::EstimateMediaFile(const Facts& f, const Recipe& recipe)
    {
        if (!f.media || !f.probed || !m_media)
            return std::nullopt;
        const auto& step = recipe.steps[0];
        std::vector<std::wstring> args;
        uint64_t target = 0;
        if (const auto* format = recipe.MediaFormat())
        {
            const bool needsH264 = std::string_view(format->id) == "mp4" || std::string_view(format->id) == "mov";
            const auto plan = PlanConversion(*format, f.info, needsH264 ? m_media->H264() : H264Encoder::OpenH264);
            args = OutputArgs(*format, plan);
        }
        else if (recipe.IsMediaCompress())
        {
            const int maxHeight = step.params.value("maxHeight", 1080);
            if (const double mb = step.params.value("targetMB", 0.0); mb > 0)
                target = static_cast<uint64_t>(mb * 1024 * 1024);
            else if (const int percent = step.params.value("percent", 0); percent > 0)
                target = f.bytes * static_cast<uint64_t>(percent) / 100;
            SizeFit fit;
            const bool fits = target > 0 && PlanSizeFit(static_cast<int64_t>(target), f.info, fit);
            args = CompressArgs(f.info, m_media->H264(), fits ? &fit : nullptr, maxHeight);
        }
        else
            return std::nullopt;
        const auto m = EstimateMedia(args, f.info, f.streams, f.bytes);
        Estimate e;
        e.known = m.known;
        e.bytes = m.bytes;
        e.sameStreams = m.sameStreams;
        e.size = m.size;
        e.audioOnly = !m.video && !f.info.videoCodec.empty();
        if (target > 0 && (!e.known || e.bytes > target))
        {
            e.known = true;
            e.bytes = target;
            e.capped = true;
        }
        return e;
    }

    std::optional<Predictor::Estimate> Predictor::EstimateFor(const std::wstring& path, const Facts& f, const Recipe& recipe, std::stop_token stop)
    {
        const auto key = f.key + L"|" + Widen(recipe.ToJson());
        {
            std::scoped_lock lock(m_cacheMutex);
            if (auto it = m_estimates.find(key); it != m_estimates.end())
                return it->second;
        }
        std::optional<Estimate> e;
        if (recipe.IsFileAction())
            e = Estimate{ true, f.bytes, true, false, false, f.upright, false };
        else if (recipe.IsMedia())
            e = EstimateMediaFile(f, recipe);
        else
            e = EstimateImage(path, f, recipe, stop);
        if (e && !stop.stop_requested())
        {
            std::scoped_lock lock(m_cacheMutex);
            if (m_estimates.size() > kCacheLimit)
                m_estimates.clear();
            m_estimates[key] = *e;
        }
        return e;
    }

    Prediction Predictor::Predict(uint64_t token, const std::vector<std::wstring>& files, const Recipe& recipe, std::stop_token stop,
                                  const Deliver& partial)
    {
        Prediction p;
        p.token = token;
        p.final = true;
        if (files.empty() || !Predicts(recipe))
            return p;
        const auto& first = files.front();
        const auto ext = LowerExtension(first);
        const bool duplicate = recipe.IsFileAction();
        const auto outExt = duplicate ? std::filesystem::path(first).extension().wstring() : recipe.OutputExtensionFor(ext);
        if (const auto* image = recipe.OutputFormatFor(ext))
            p.format = image->label;
        else if (const auto* media = recipe.MediaFormat())
            p.format = media->label;
        else
            p.format = Upper(outExt.empty() ? std::wstring() : outExt.substr(1));
        if (!duplicate && recipe.steps.back().action == "image.compress")
            p.format += L", compressed";
        if (recipe.IsMediaCompress())
            p.format = (outExt == L".m4a" ? L"M4A" : L"MP4") + std::wstring(L", compressed");
        p.orientation = RecipeOrientation(recipe);

        // The quick part: the first file's name, picture, size and what its metadata loses.
        const auto facts = FactsFor(first, false, stop);
        if (!facts || stop.stop_requested())
        {
            // A file dragged out of a browser is not on disk until the drop: its name is all there is.
            if (!facts && !stop.stop_requested() && VirtualDrop::Find(first))
                p.name = PredictOutputName(first, recipe.suffix, outExt);
            return p;
        }
        p.thumb = facts->thumb;
        p.name = PredictOutputName(first, recipe.suffix, outExt);
        if (files.size() > 1)
            p.name += std::format(L"  + {} more", files.size() - 1);
        uint64_t sourceBytes = 0;
        for (const auto& f : files)
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &data))
                sourceBytes += FileSizeOf(data);
        }
        p.was = L"was " + SizeText(sourceBytes);
        std::vector<std::wstring> detail;
        if (facts->image && !duplicate)
        {
            detail.push_back(DimensionsChange(facts->upright, OutputDimensions(recipe, facts->upright)));
            detail.push_back(MetadataLoss(facts->meta));
        }
        p.detail = Join(detail);
        // An animated GIF or a multi-page TIFF converts every frame (formats/Frames.h): say so, and
        // give no size, since the estimate below would be for one frame.
        if (const uint32_t frames = recipe.IsSingleEncode() ? formats::FrameCount(first) : 1; frames > 1)
        {
            const auto* format = recipe.OutputFormatFor(ext);
            const auto kind = formats::FramesOutputFor(ext, format ? format->id : "");
            if (kind == formats::FramesOutput::Folder && files.size() == 1)
                p.name = PredictOutputName(first, L"frames", L"");
            if (kind == formats::FramesOutput::Animation)
                p.format += L", animated";
            p.detail = Join({ std::format(L"{} {}", frames, ext == L".gif" ? L"frames" : L"pages"), p.detail });
            return p;
        }
        if (partial)
        {
            Prediction quick = p;
            quick.final = false;
            partial(std::move(quick));
        }

        // The sizes: every file up to kBatchSample, the rest scaled by their bytes.
        uint64_t predicted = 0, sizedSource = 0;
        bool anyEstimate = false, allExact = true, anyCapped = false, allSame = true;
        size_t sized = 0;
        for (size_t i = 0; i < files.size() && sized < kBatchSample; ++i)
        {
            if (stop.stop_requested())
                return p;
            const auto f = i == 0 && !facts->media ? facts : FactsFor(files[i], true, stop);
            if (!f)
                continue;
            const auto e = EstimateFor(files[i], *f, recipe, stop);
            if (stop.stop_requested())
                return p;
            if (i == 0 && f->media)
            {
                // The probe gives the media detail line.
                std::vector<std::wstring> more;
                if (e && e->audioOnly)
                    more.push_back(L"audio only");
                else if (e && e->size.w > 0)
                    more.push_back(DimensionsChange(Size2{ static_cast<uint32_t>(f->info.width), static_cast<uint32_t>(f->info.height) }, e->size));
                if (e && e->sameStreams && recipe.MediaFormat())
                    more.push_back(L"streams copied, no re-encode");
                if (recipe.MediaFormat() && std::string_view(recipe.MediaFormat()->id) == "gif")
                    more.push_back(L"12 fps, no sound");
                p.detail = Join(more);
            }
            if (!e || !e->known)
                continue;
            ++sized;
            predicted += e->bytes;
            sizedSource += f->bytes;
            anyEstimate = true;
            allExact &= e->exact;
            anyCapped |= e->capped;
            allSame &= e->sameStreams;
        }
        if (!anyEstimate)
        {
            p.size = facts->media && (!m_media || !m_media->Available()) ? L"size unknown (no media engine)" : L"size unknown";
            return p;
        }
        if (sized < files.size() && sizedSource > 0)
        {
            predicted = static_cast<uint64_t>(static_cast<double>(predicted) * static_cast<double>(sourceBytes) / static_cast<double>(sizedSource));
            allExact = false;
        }
        const auto amount = SizeText(predicted);
        p.size = anyCapped ? L"≤ " + amount : allExact ? amount : L"~" + amount;
        if (files.size() > 1)
            p.size += L" in total";
        if (allSame && recipe.IsMedia())
            p.size = L"same streams, " + p.size;
        return p;
    }
}
