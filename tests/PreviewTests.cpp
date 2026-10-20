#include "ImagePipeline.h"
#include "Media.h"
#include "TestUtil.h"
#include "Utf.h"
#include "preview/Prediction.h"
#include "preview/Predictor.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

using namespace sd;
using namespace sd::preview;
using namespace sd::test;

namespace
{
    std::shared_ptr<const FfmpegEngine> Engine()
    {
        static const auto engine = std::make_shared<const FfmpegEngine>(FfmpegEngine::Locate());
        return engine;
    }

    uint64_t ActualBytes(const Recipe& recipe, const std::wstring& path)
    {
        Microsoft::WRL::ComPtr<IStream> memory;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &memory)))
            return 0;
        std::wstring error;
        if (FAILED(RunImageRecipe(recipe, path, memory.Get(), std::stop_token{}, error)))
            return 0;
        STATSTG stat{};
        memory->Stat(&stat, STATFLAG_NONAME);
        return stat.cbSize.QuadPart;
    }

    // A photo-like picture: smooth gradients plus fine noise, so JPEG and PNG sizes behave like a photo's.
    HRESULT WritePhoto(const std::wstring& path, UINT width, UINT height)
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
        if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
        if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
        const UINT stride = (width * 3 + 3) & ~3u;
        std::vector<BYTE> pixels(static_cast<size_t>(stride) * height);
        uint32_t seed = 12345;
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x)
            {
                seed = seed * 1664525u + 1013904223u;
                const int noise = static_cast<int>((seed >> 24) % 24) - 12;
                BYTE* p = &pixels[static_cast<size_t>(y) * stride + x * 3];
                p[0] = static_cast<BYTE>(std::clamp(static_cast<int>(x * 200 / width) + noise, 0, 255));
                p[1] = static_cast<BYTE>(std::clamp(static_cast<int>(y * 200 / height) + noise, 0, 255));
                p[2] = static_cast<BYTE>(std::clamp(static_cast<int>(((x / 37 + y / 23) % 7) * 30) + noise, 0, 255));
            }
        if (SUCCEEDED(hr)) hr = frame->WritePixels(height, stride, static_cast<UINT>(pixels.size()), pixels.data());
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = encoder->Commit();
        return hr;
    }
}

TEST(Prediction, OutputDimensionsFollowTheRecipe)
{
    TempDir dir;
    Recipes recipes(dir);
    const Size2 photo{ 4032, 3024 };
    EXPECT_EQ(OutputDimensions(recipes.Get("max-1920"), photo), (Size2{ 1920, 1440 }));
    EXPECT_EQ(OutputDimensions(recipes.Get("max-1920"), Size2{ 800, 600 }), (Size2{ 800, 600 }));   // never upscales
    EXPECT_EQ(OutputDimensions(recipes.Get("half-size"), photo), (Size2{ 2016, 1512 }));
    EXPECT_EQ(OutputDimensions(recipes.Get("rotate-right"), photo), (Size2{ 3024, 4032 }));
    EXPECT_EQ(OutputDimensions(recipes.Get("flip"), photo), photo);
    EXPECT_EQ(OutputDimensions(recipes.Get("to-png"), photo), photo);
    EXPECT_EQ(OutputDimensions(recipes.Get("web-ready"), photo), (Size2{ 2048, 1536 }));
    EXPECT_EQ(Upright(photo, 6), (Size2{ 3024, 4032 }));   // EXIF "rotate 90" stores the picture on its side
    EXPECT_EQ(Upright(photo, 3), photo);
}

TEST(Prediction, OrientationComposesRotateAndFlip)
{
    TempDir dir;
    Recipes recipes(dir);
    EXPECT_TRUE(RecipeOrientation(recipes.Get("to-png")).Identity());
    const auto right = RecipeOrientation(recipes.Get("rotate-right"));
    EXPECT_EQ(right, (Orientation{ 0, -1, 1, 0 }));
    EXPECT_TRUE(right.SwapsSides());
    EXPECT_EQ(RecipeOrientation(recipes.Get("rotate-left")), (Orientation{ 0, 1, -1, 0 }));
    EXPECT_EQ(RecipeOrientation(recipes.Get("flip")), (Orientation{ -1, 0, 0, 1 }));

    // Rotate right, then flip: a point on the right edge (1, 0) goes to the bottom (0, 1), then stays there.
    Recipe r = recipes.Get("rotate-right");
    r.steps.insert(r.steps.end() - 1, recipes.Get("flip").steps[0]);
    const auto m = RecipeOrientation(r);
    EXPECT_EQ(m.a * 1 + m.b * 0, 0);
    EXPECT_EQ(m.c * 1 + m.d * 0, 1);
    EXPECT_EQ(m.a * 0 + m.b * 1, 1);   // and the bottom (0, 1) ends up on the right after the flip
}

TEST(Prediction, SampleRecipeDownscalesBeforeTheEncode)
{
    TempDir dir;
    Recipes recipes(dir);
    const auto sample = SampleRecipe(recipes.Get("web-ready"), 512);
    ASSERT_EQ(sample.steps.size(), 4u);
    EXPECT_EQ(sample.steps[2].action, "image.resize");
    EXPECT_EQ(sample.steps[2].params["maxEdge"].get<int>(), 512);
    EXPECT_EQ(sample.steps.back().action, "image.encode");
    EXPECT_EQ(OutputDimensions(sample, Size2{ 4032, 3024 }), (Size2{ 512, 384 }));
}

TEST(Prediction, PowerLawFitAndExtrapolation)
{
    // Exactly proportional to pixels: k = 1.
    EXPECT_NEAR(FitExponent(100, 1000, 400, 4000), 1.0, 1e-9);
    // Doubling the pixels gives 1.74x the bytes: k = log2(1.74) ~ 0.8.
    EXPECT_NEAR(FitExponent(100, 1000, 174, 2000), 0.799, 0.01);
    EXPECT_EQ(FitExponent(100, 1000, 100000, 2000), 1.05);   // clamped
    EXPECT_EQ(FitExponent(0, 1000, 10, 2000), 1.0);
    EXPECT_EQ(Extrapolate(1000, 1e6, 4e6, 1.0), 4000u);
    EXPECT_EQ(Extrapolate(1000, 1e6, 4e6, 0.5), 2000u);
    EXPECT_EQ(Extrapolate(0, 1e6, 4e6, 1.0), 0u);
}

TEST(Prediction, Texts)
{
    EXPECT_EQ(SizeText(512), L"512 bytes");
    EXPECT_EQ(SizeText(860 * 1024), L"860 KB");
    EXPECT_EQ(SizeText(1258291), L"1.2 MB");
    EXPECT_EQ(SizeText(48ull * 1024 * 1024), L"48 MB");
    EXPECT_EQ(SizeText(3ull * 1024 * 1024 * 1024), L"3.0 GB");
    EXPECT_EQ(DimensionsChange({ 4032, 3024 }, { 1920, 1440 }), L"4032×3024 → 1920×1440");
    EXPECT_EQ(DimensionsChange({ 10, 10 }, { 10, 10 }), L"");
    EXPECT_EQ(MetadataLoss({}), L"");
    EXPECT_EQ(MetadataLoss({ true, true, false, false, false }), L"removes GPS, camera");
    EXPECT_EQ(MetadataLoss({ false, false, true, true, true }), L"removes date taken, author");
    EXPECT_EQ(MetadataLoss({ false, false, false, false, true }), L"removes XMP");
}

TEST(Prediction, OutputNameSkipsTakenNames)
{
    TempDir dir;
    const auto source = dir / L"holiday.png";
    WriteText(source, "x");
    EXPECT_EQ(PredictOutputName(source, L"converted", L".webp"), L"holiday (converted).webp");
    WriteText(dir / L"holiday (converted).webp", "y");
    EXPECT_EQ(PredictOutputName(source, L"converted", L".webp"), L"holiday (converted 2).webp");
}

TEST(Prediction, Bitrates)
{
    EXPECT_EQ(ParseBitrate(L"192k"), 192000);
    EXPECT_EQ(ParseBitrate(L"6M"), 6000000);
    EXPECT_EQ(ParseBitrate(L"8000000"), 8000000);
    EXPECT_EQ(ParseBitrate(L"0"), 0);
    EXPECT_EQ(ParseBitrate(L"fast"), 0);
    EXPECT_EQ(ParseBitrate(L""), 0);
}

TEST(Prediction, MediaFromThePlannedCommandLine)
{
    MediaInfo video;
    video.durationUs = 10'000'000;
    video.videoCodec = "h264";
    video.audioCodec = "aac";
    video.width = 1920;
    video.height = 1080;
    video.fps = 30;
    StreamFacts facts{ 6'000'000, 192'000, 48000, 2 };
    const uint64_t source = 7'740'000;

    // MP4 from H.264/AAC copies both streams: about the same size.
    const auto* mp4 = FindMediaFormat("mp4");
    const auto copy = EstimateMedia(OutputArgs(*mp4, PlanConversion(*mp4, video, H264Encoder::Nvenc)), video, facts, source);
    EXPECT_TRUE(copy.known);
    EXPECT_TRUE(copy.sameStreams);
    EXPECT_EQ(copy.bytes, source);

    // MKV: "-c copy" of everything.
    const auto* mkv = FindMediaFormat("mkv");
    EXPECT_TRUE(EstimateMedia(OutputArgs(*mkv, PlanConversion(*mkv, video, H264Encoder::Nvenc)), video, facts, source).sameStreams);

    // WMV: 6 Mbit/s video + 192 kbit/s audio for 10 s, plus the container: ~7.85 MB.
    const auto* wmv = FindMediaFormat("wmv");
    const auto w = EstimateMedia(OutputArgs(*wmv, PlanConversion(*wmv, video, H264Encoder::Nvenc)), video, facts, source);
    EXPECT_FALSE(w.sameStreams);
    EXPECT_NEAR(static_cast<double>(w.bytes), (6'000'000 + 192'000) * 10 / 8 * 1.015, 1000.0);

    // MP3 from a video: audio only, LAME -q:a 2 is about 190 kbit/s.
    const auto* mp3 = FindMediaFormat("mp3");
    const auto a = EstimateMedia(OutputArgs(*mp3, PlanConversion(*mp3, video, H264Encoder::Nvenc)), video, facts, source);
    EXPECT_FALSE(a.video);
    EXPECT_TRUE(a.audio);
    EXPECT_NEAR(static_cast<double>(a.bytes), 190'000.0 * 10 / 8 * 1.015, 100.0);

    // GIF: 480 px wide, 12 fps.
    const auto* gif = FindMediaFormat("gif");
    const auto g = EstimateMedia(OutputArgs(*gif, PlanConversion(*gif, video, H264Encoder::Nvenc)), video, facts, source);
    EXPECT_EQ(g.size, (Size2{ 480, 270 }));
    EXPECT_EQ(g.fps, 12);

    // Compress preset (quality 28, at most 1080 lines) of a 4K clip: 1920x1080 at ~0.056 bits per pixel.
    MediaInfo uhd = video;
    uhd.width = 3840;
    uhd.height = 2160;
    const auto c = EstimateMedia(CompressArgs(uhd, H264Encoder::Nvenc, nullptr, 1080), uhd, facts, source * 4);
    EXPECT_EQ(c.size, (Size2{ 1920, 1080 }));
    const double bpp = 0.1 * std::pow(2.0, -5.0 / 6.0);
    EXPECT_NEAR(static_cast<double>(c.bytes), (1920.0 * 1080 * 30 * bpp + 128'000) * 10 / 8 * 1.015, 2000.0);

    // A size target: the bitrates come from PlanSizeFit, so the estimate lands under the target.
    SizeFit fit;
    ASSERT_TRUE(PlanSizeFit(5'000'000, video, fit));
    const auto t = EstimateMedia(CompressArgs(video, H264Encoder::OpenH264, &fit, 1080), video, facts, source);
    EXPECT_LE(t.bytes, 5'000'000u);
    EXPECT_GT(t.bytes, 4'500'000u);

    // WAV from audio: PCM at the input's rate and channels.
    MediaInfo song;
    song.durationUs = 60'000'000;
    song.audioCodec = "mp3";
    const auto* wav = FindMediaFormat("wav");
    const auto pcm = EstimateMedia(OutputArgs(*wav, PlanConversion(*wav, song, H264Encoder::OpenH264)), song, StreamFacts{ 0, 320'000, 44100, 2 }, 2'400'000);
    EXPECT_NEAR(static_cast<double>(pcm.bytes), 44100.0 * 2 * 2 * 60 * 1.015, 100.0);

    // No duration: only a copy can be sized.
    MediaInfo unknown = video;
    unknown.durationUs = -1;
    EXPECT_FALSE(EstimateMedia(OutputArgs(*wmv, PlanConversion(*wmv, unknown, H264Encoder::Nvenc)), unknown, facts, source).known);
}

TEST(Predictor, SmallImageIsExact)
{
    TempDir dir;
    Recipes recipes(dir);
    const auto source = dir / L"small.png";
    ASSERT_HRESULT_SUCCEEDED(WritePhoto(source, 600, 400));
    Predictor predictor(nullptr, {});
    const auto& recipe = recipes.Get("to-jpg");
    std::vector<Prediction> partials;
    const auto p = predictor.Predict(1, { source }, recipe, std::stop_token{}, [&](Prediction q) { partials.push_back(std::move(q)); });
    EXPECT_TRUE(p.final);
    EXPECT_EQ(p.format, L"JPG");
    EXPECT_EQ(p.name, L"small (converted).jpg");
    EXPECT_EQ(p.size, SizeText(ActualBytes(recipe, source)));   // no "~": encoded for real
    EXPECT_EQ(p.was.rfind(L"was ", 0), 0u);
    ASSERT_EQ(partials.size(), 1u);
    EXPECT_FALSE(partials[0].final);
    EXPECT_EQ(partials[0].name, p.name);
    EXPECT_TRUE(partials[0].size.empty());
}

TEST(Predictor, LargeImageEstimateIsClose)
{
    TempDir dir;
    Recipes recipes(dir);
    const auto source = dir / L"photo.png";
    ASSERT_HRESULT_SUCCEEDED(WritePhoto(source, 2400, 1800));
    Predictor predictor(nullptr, {});
    for (const char* id : { "to-jpg", "to-png", "max-1920" })
    {
        const auto& recipe = recipes.Get(id);
        const auto p = predictor.Predict(2, { source }, recipe, std::stop_token{});
        ASSERT_EQ(p.size.rfind(L"~", 0), 0u) << id << ": " << Narrow(p.size);
        const double actual = static_cast<double>(ActualBytes(recipe, source));
        ASSERT_GT(actual, 0.0) << id;
        // Parse the "~1.2 MB" back to bytes for the comparison.
        const double shown = std::stod(p.size.substr(1));
        const double unit = p.size.find(L"MB") != std::wstring::npos ? 1024.0 * 1024.0 : p.size.find(L"KB") != std::wstring::npos ? 1024.0 : 1.0;
        const double ratio = shown * unit / actual;
        EXPECT_GT(ratio, 0.6) << id << ": predicted " << Narrow(p.size) << ", actual " << actual;
        EXPECT_LT(ratio, 1.6) << id << ": predicted " << Narrow(p.size) << ", actual " << actual;
    }
    const auto resized = predictor.Predict(3, { source }, recipes.Get("max-1920"), std::stop_token{});
    EXPECT_EQ(resized.detail, L"2400×1800 → 1920×1440");
    EXPECT_EQ(resized.name, L"photo (edited).png");
}

TEST(Predictor, BatchAddsUpAndNamesTheRest)
{
    TempDir dir;
    Recipes recipes(dir);
    std::vector<std::wstring> files;
    for (int i = 0; i < 3; ++i)
    {
        files.push_back(dir / std::format(L"p{}.png", i));
        ASSERT_HRESULT_SUCCEEDED(WritePhoto(files.back(), 300, 200));
    }
    Predictor predictor(nullptr, {});
    const auto p = predictor.Predict(4, files, recipes.Get("to-bmp"), std::stop_token{});
    EXPECT_EQ(p.name, L"p0 (converted).bmp  + 2 more");
    EXPECT_NE(p.size.find(L"in total"), std::wstring::npos) << Narrow(p.size);
    // 3 x (300 x 200 x 3 + header) ~ 528 KB.
    EXPECT_EQ(p.size, L"528 KB in total");
}

TEST(Predictor, NoPredictionForFileActionsAndCancelStops)
{
    TempDir dir;
    Recipes recipes(dir);
    EXPECT_FALSE(Predictor::Predicts(recipes.Get("copy")));
    EXPECT_FALSE(Predictor::Predicts(recipes.Get("terminal")));
    EXPECT_TRUE(Predictor::Predicts(recipes.Get("duplicate")));
    EXPECT_TRUE(Predictor::Predicts(recipes.Get("to-webp")));
    EXPECT_TRUE(Predictor::Predicts(recipes.Get("to-mp4")));

    const auto source = dir / L"big.png";
    ASSERT_HRESULT_SUCCEEDED(WritePhoto(source, 1600, 1200));
    std::stop_source stop;
    stop.request_stop();
    Predictor predictor(nullptr, {});
    const auto p = predictor.Predict(5, { source }, recipes.Get("to-webp"), stop.get_token());
    EXPECT_TRUE(p.size.empty());   // stopped before anything was sized

    const auto dup = predictor.Predict(6, { source }, recipes.Get("duplicate"), std::stop_token{});
    EXPECT_EQ(dup.name, L"big (copy).png");
    EXPECT_EQ(dup.format, L"PNG");
}

TEST(Predictor, WorkerDeliversTheLatestRequest)
{
    TempDir dir;
    Recipes recipes(dir);
    const auto source = dir / L"one.png";
    ASSERT_HRESULT_SUCCEEDED(WritePhoto(source, 400, 300));
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<Prediction> got;
    {
        Predictor predictor(nullptr, [&](Prediction p) {
            std::scoped_lock lock(mutex);
            got.push_back(std::move(p));
            cv.notify_all();
        });
        predictor.Request(10, { source }, recipes.Get("to-webp"));
        predictor.Request(11, { source }, recipes.Get("to-jpg"));   // cancels 10 if it is still running
        std::unique_lock lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(30), [&] {
            return std::any_of(got.begin(), got.end(), [](const Prediction& p) { return p.token == 11 && p.final; });
        }));
        const auto& last = *std::find_if(got.begin(), got.end(), [](const Prediction& p) { return p.token == 11 && p.final; });
        EXPECT_EQ(last.format, L"JPG");
        EXPECT_FALSE(last.size.empty());
    }   // the destructor stops and joins the worker
}

TEST(PredictorMedia, ProbesAndPredictsFromThePlan)
{
    const auto engine = Engine();
    if (!engine->Available())
        GTEST_SKIP() << "SHIFTDROP_FFMPEG is not set";
    TempDir dir;
    Recipes recipes(dir);
    const auto clip = dir / L"clip.avi";
    std::stop_source never;
    ASSERT_TRUE(engine->Run({ L"-hide_banner", L"-nostdin", L"-f", L"lavfi", L"-i", L"testsrc=s=320x240:r=25:d=2", L"-f", L"lavfi",
                              L"-i", L"sine=f=440:d=2", L"-c:v", L"mpeg4", L"-c:a", L"pcm_s16le", L"-shortest", L"-y", L"-f", L"avi", clip },
                            nullptr, never.get_token()).ok);
    Predictor predictor(engine, {});
    const auto mkv = predictor.Predict(1, { clip }, recipes.Get("to-mkv"), std::stop_token{});
    EXPECT_EQ(mkv.size.rfind(L"same streams", 0), 0u) << Narrow(mkv.size);
    EXPECT_NE(mkv.detail.find(L"streams copied"), std::wstring::npos) << Narrow(mkv.detail);

    const auto mp3 = predictor.Predict(2, { clip }, recipes.Get("to-mp3"), std::stop_token{});
    EXPECT_EQ(mp3.format, L"MP3");
    EXPECT_EQ(mp3.detail, L"audio only");
    EXPECT_EQ(mp3.size.rfind(L"~", 0), 0u) << Narrow(mp3.size);

    const auto gif = predictor.Predict(3, { clip }, recipes.Get("to-gif"), std::stop_token{});
    EXPECT_NE(gif.detail.find(L"12 fps"), std::wstring::npos) << Narrow(gif.detail);

    // A song to WAV: PCM, so the estimate is within a few percent of the real file.
    const auto song = dir / L"song.flac";
    ASSERT_TRUE(engine->Run({ L"-hide_banner", L"-nostdin", L"-f", L"lavfi", L"-i", L"sine=f=440:d=3:sample_rate=44100", L"-ac", L"2",
                              L"-y", L"-f", L"flac", song }, nullptr, never.get_token()).ok);
    const auto wav = predictor.Predict(4, { song }, recipes.Get("to-wav"), std::stop_token{});
    ASSERT_EQ(wav.size.rfind(L"~", 0), 0u) << Narrow(wav.size);
    const double predictedKb = std::stod(wav.size.substr(1));
    EXPECT_NEAR(predictedKb, 44100.0 * 2 * 2 * 3 / 1024, 15.0) << Narrow(wav.size);   // ~517 KB
}
