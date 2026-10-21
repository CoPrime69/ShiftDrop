#include "Ask.h"
#include "PipelineRunner.h"
#include "TestUtil.h"
#include "Utf.h"
#include "formats/Avif.h"
#include "formats/Frames.h"

#include <avif/avif.h>
#include <avif/avif_cxx.h>
#include <wil/result_macros.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>

using namespace sd;
using namespace sd::test;
using Microsoft::WRL::ComPtr;

// Converting an animated GIF or a multi-page TIFF keeps every frame (src/formats/Frames.h).
namespace
{
    // Answers the questions of src/Ask.h for one test.
    struct ScopedAnswers
    {
        explicit ScopedAnswers(ask::ChooseFn choose) { ask::SetAnswers(std::move(choose), {}); }
        ~ScopedAnswers() { ask::SetAnswers({}, {}); }
    };

    struct Fixture
    {
        TempDir dir;
        Recipes recipes{ dir };
        std::shared_ptr<const ImageCodecs> codecs = std::make_shared<const ImageCodecs>();
        PipelineRunner runner{ codecs, nullptr };

        ItemResult Run(std::string_view id, const std::wstring& source) { return runner.Run(recipes.Get(id), 0, source, {}); }
    };

    // One GIF frame: a rectangle of one palette colour (0 = see-through, 1 red, 2 green, 3 blue).
    struct GifFrame
    {
        uint16_t left, top, width, height;
        uint8_t colour;
        uint8_t disposal;   // 1 keep, 2 clear to see-through, 3 restore what was there before
        uint16_t delay;     // hundredths of a second
    };

    void Put16(std::string& s, uint16_t v)
    {
        s += static_cast<char>(v & 0xFF);
        s += static_cast<char>(v >> 8);
    }

    // A GIF written byte by byte, so the test controls every frame's place, disposal and delay.
    // The LZW stream uses only literal 9-bit codes, with a clear code often enough that the code
    // size never grows: valid for every decoder, if not small.
    std::string MakeGif(uint16_t width, uint16_t height, const std::vector<GifFrame>& frames)
    {
        std::string s = "GIF89a";
        Put16(s, width);
        Put16(s, height);
        s += static_cast<char>(0xF7);   // global colour table of 256 entries
        s += '\0';
        s += '\0';
        for (int i = 0; i < 256; ++i)
        {
            s += static_cast<char>(i == 1 ? 255 : 0);
            s += static_cast<char>(i == 2 ? 255 : 0);
            s += static_cast<char>(i == 3 ? 255 : 0);
        }
        s += std::string("\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00", 19);   // loop for ever
        for (const auto& f : frames)
        {
            s += std::string("\x21\xF9\x04", 3);
            s += static_cast<char>((f.disposal << 2) | 1);   // index 0 is see-through
            Put16(s, f.delay);
            s += '\0';
            s += '\0';
            s += ',';
            Put16(s, f.left);
            Put16(s, f.top);
            Put16(s, f.width);
            Put16(s, f.height);
            s += '\0';
            s += static_cast<char>(8);   // LZW minimum code size

            std::string data;
            uint32_t bits = 0, count = 0;
            const auto code = [&](uint32_t c) {
                bits |= c << count;
                count += 9;
                while (count >= 8)
                {
                    data += static_cast<char>(bits & 0xFF);
                    bits >>= 8;
                    count -= 8;
                }
            };
            const uint32_t pixels = static_cast<uint32_t>(f.width) * f.height;
            for (uint32_t i = 0; i < pixels; ++i)
            {
                if (i % 200 == 0)
                    code(256);   // clear
                code(f.colour);
            }
            code(257);   // end
            if (count)
                data += static_cast<char>(bits & 0xFF);
            for (size_t at = 0; at < data.size(); at += 255)
            {
                const size_t n = std::min<size_t>(255, data.size() - at);
                s += static_cast<char>(n);
                s += data.substr(at, n);
            }
            s += '\0';
        }
        s += ';';
        return s;
    }

    // 8x8: red everywhere; a green square that is cleared afterwards; a blue corner that is undone
    // afterwards; a green corner.
    std::wstring WriteAnimatedGif(const TempDir& dir, const std::wstring& name = L"anim.gif")
    {
        const auto path = dir / name;
        WriteText(path, MakeGif(8, 8,
                                { { 0, 0, 8, 8, 1, 1, 10 }, { 2, 2, 4, 4, 2, 2, 20 }, { 0, 0, 2, 2, 3, 3, 0 }, { 6, 6, 2, 2, 2, 1, 5 } }));
        return path;
    }

    formats::Image Solid(uint32_t w, uint32_t h, uint8_t b, uint8_t g, uint8_t r)
    {
        formats::Image image;
        image.width = w;
        image.height = h;
        image.bgra.resize(static_cast<size_t>(w) * h * 4);
        for (size_t i = 0; i < image.bgra.size(); i += 4)
        {
            image.bgra[i] = b;
            image.bgra[i + 1] = g;
            image.bgra[i + 2] = r;
            image.bgra[i + 3] = 255;
        }
        return image;
    }

    ComPtr<IWICImagingFactory> Factory()
    {
        ComPtr<IWICImagingFactory> factory;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        return factory;
    }

    HRESULT WriteTiff(const std::wstring& path, const std::vector<formats::Image>& pages)
    {
        auto factory = Factory();
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        RETURN_IF_FAILED(factory->CreateStream(&stream));
        RETURN_IF_FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
        RETURN_IF_FAILED(factory->CreateEncoder(GUID_ContainerFormatTiff, nullptr, &encoder));
        RETURN_IF_FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
        for (const auto& page : pages)
        {
            ComPtr<IWICBitmapFrameEncode> frame;
            RETURN_IF_FAILED(encoder->CreateNewFrame(&frame, nullptr));
            RETURN_IF_FAILED(frame->Initialize(nullptr));
            RETURN_IF_FAILED(frame->SetSize(page.width, page.height));
            WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
            RETURN_IF_FAILED(frame->SetPixelFormat(&format));
            ComPtr<IWICBitmapSource> pixels;
            RETURN_IF_FAILED(formats::ToWicBitmap(factory.Get(), page, pixels));
            RETURN_IF_FAILED(frame->WriteSource(pixels.Get(), nullptr));
            RETURN_IF_FAILED(frame->Commit());
        }
        return encoder->Commit();
    }

    bool ReadPixels(const std::wstring& path, formats::Image& out)
    {
        auto factory = Factory();
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        return SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
               SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(formats::FromWicSource(factory.Get(), frame.Get(), out));
    }

    const uint8_t* Px(const formats::Image& image, uint32_t x, uint32_t y) { return image.Row(y) + static_cast<size_t>(x) * 4; }
    bool IsColour(const uint8_t* p, uint8_t b, uint8_t g, uint8_t r, int slack = 0)
    {
        return std::abs(p[0] - b) <= slack && std::abs(p[1] - g) <= slack && std::abs(p[2] - r) <= slack && p[3] > 200;
    }

    size_t Count(const std::string& text, const std::string& what)
    {
        size_t n = 0;
        for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size()))
            ++n;
        return n;
    }
}

TEST(Frames, GifFramesAreDrawnInTurnWithTheirDelays)
{
    TempDir dir;
    const auto gif = WriteAnimatedGif(dir);
    EXPECT_EQ(formats::FrameCount(gif), 4u);

    std::vector<formats::Image> seen;
    std::vector<int> delays;
    std::wstring error;
    ASSERT_HRESULT_SUCCEEDED(formats::ForEachFrame(gif, {}, [&](const formats::Image& frame, uint32_t index, uint32_t count, int delayMs) {
        EXPECT_EQ(index, seen.size());
        EXPECT_EQ(count, 4u);
        seen.push_back(frame);
        delays.push_back(delayMs);
        return true;
    }, error)) << Narrow(error);
    ASSERT_EQ(seen.size(), 4u);
    EXPECT_EQ(delays, (std::vector<int>{ 100, 200, 100, 50 }));   // no delay shows for 100 ms, like browsers
    for (const auto& frame : seen)
    {
        EXPECT_EQ(frame.width, 8u);
        EXPECT_EQ(frame.height, 8u);
    }
    EXPECT_TRUE(IsColour(Px(seen[0], 4, 4), 0, 0, 255));
    EXPECT_TRUE(IsColour(Px(seen[1], 3, 3), 0, 255, 0));    // the green square on top
    EXPECT_TRUE(IsColour(Px(seen[1], 0, 0), 0, 0, 255));    // red around it
    EXPECT_TRUE(IsColour(Px(seen[2], 0, 0), 255, 0, 0));    // blue corner
    EXPECT_EQ(Px(seen[2], 3, 3)[3], 0);                     // the green square was cleared
    EXPECT_TRUE(IsColour(Px(seen[2], 7, 7), 0, 0, 255));
    EXPECT_TRUE(IsColour(Px(seen[3], 0, 0), 0, 0, 255));    // the blue corner was undone
    EXPECT_TRUE(IsColour(Px(seen[3], 7, 7), 0, 255, 0));
    EXPECT_EQ(Px(seen[3], 3, 3)[3], 0);

    // A plain picture has one frame and goes through the normal pipeline.
    const auto png = dir / L"still.png";
    ASSERT_HRESULT_SUCCEEDED(WritePng(png, 10, 10));
    EXPECT_EQ(formats::FrameCount(png), 1u);
    Fixture f;
    const auto one = f.dir / L"one.gif";
    WriteText(one, MakeGif(4, 4, { { 0, 0, 4, 4, 2, 1, 0 } }));
    EXPECT_EQ(formats::FrameCount(one), 1u);
    const auto result = f.Run("to-png", one);
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    EXPECT_EQ(std::filesystem::path(result.output).filename(), L"one (converted).png");
}

TEST(Frames, AnimatedGifToWebPStaysAnimated)
{
    Fixture f;
    const auto gif = WriteAnimatedGif(f.dir);
    const auto result = f.Run("to-webp", gif);
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    EXPECT_EQ(std::filesystem::path(result.output).filename(), L"anim (converted).webp");
    const auto bytes = ReadAll(result.output);
    EXPECT_EQ(bytes.substr(0, 4), "RIFF");
    EXPECT_EQ(Count(bytes, "ANIM"), 1u);
    EXPECT_EQ(Count(bytes, "ANMF"), 4u);   // one frame chunk per GIF frame
    EXPECT_NE(result.message.find(L"4 frames"), std::wstring::npos) << Narrow(result.message);
}

TEST(Frames, AnimatedGifToAvifStaysAnimated)
{
    Fixture f;
    const auto gif = WriteAnimatedGif(f.dir);
    const auto result = f.Run("to-avif", gif);
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    const auto bytes = ReadAll(result.output);
    avif::DecoderPtr decoder(avifDecoderCreate());
    ASSERT_EQ(avifDecoderSetIOMemory(decoder.get(), reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), AVIF_RESULT_OK);
    ASSERT_EQ(avifDecoderParse(decoder.get()), AVIF_RESULT_OK);
    EXPECT_EQ(decoder->imageCount, 4);
    EXPECT_NEAR(decoder->duration, 0.45, 0.01);   // 100 + 200 + 100 + 50 ms
    for (int i = 0; i < 4; ++i)
        ASSERT_EQ(avifDecoderNextImage(decoder.get()), AVIF_RESULT_OK) << i;
    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, decoder->image);
    rgb.format = AVIF_RGB_FORMAT_BGRA;
    rgb.depth = 8;
    std::vector<uint8_t> pixels(8 * 8 * 4);
    rgb.pixels = pixels.data();
    rgb.rowBytes = 8 * 4;
    ASSERT_EQ(avifImageYUVToRGB(decoder->image, &rgb), AVIF_RESULT_OK);
    const uint8_t* corner = pixels.data() + (7 * 8 + 7) * 4;   // the last frame's green corner
    EXPECT_GT(corner[1], 160);
    EXPECT_LT(corner[2], 100);
    EXPECT_LT(pixels[(3 * 8 + 3) * 4 + 3], 40);                // and its see-through middle
}

TEST(Frames, AnimatedGifToPngGivesAFramesFolder)
{
    Fixture f;
    const auto gif = WriteAnimatedGif(f.dir);
    const auto result = f.Run("to-png", gif);
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    EXPECT_EQ(std::filesystem::path(result.output).filename(), L"anim (frames)");
    ASSERT_TRUE(std::filesystem::is_directory(result.output));
    std::vector<std::wstring> names;
    for (const auto& e : std::filesystem::directory_iterator(result.output))
        names.push_back(e.path().filename().wstring());
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::wstring>{ L"anim frame 01.png", L"anim frame 02.png", L"anim frame 03.png", L"anim frame 04.png" }));
    formats::Image last;
    ASSERT_TRUE(ReadPixels(result.output + L"\\anim frame 04.png", last));
    EXPECT_TRUE(IsColour(Px(last, 7, 7), 0, 255, 0));
    EXPECT_EQ(Px(last, 3, 3)[3], 0);   // PNG keeps the see-through middle
    EXPECT_NE(result.message.find(L"4 frames saved as PNG"), std::wstring::npos) << Narrow(result.message);

    // JPG has no transparency: the frames come out on white. A second run never touches the first.
    const auto jpg = f.Run("to-jpg", gif);
    ASSERT_TRUE(jpg.ok) << Narrow(jpg.error);
    EXPECT_EQ(std::filesystem::path(jpg.output).filename(), L"anim (frames 2)");
    formats::Image white;
    ASSERT_TRUE(ReadPixels(jpg.output + L"\\anim frame 03.jpg", white));
    // White, give or take JPEG's ringing next to red and blue in an 8 x 8 picture.
    EXPECT_TRUE(IsColour(Px(white, 3, 3), 255, 255, 255, 60))
        << int(Px(white, 3, 3)[0]) << "," << int(Px(white, 3, 3)[1]) << "," << int(Px(white, 3, 3)[2]);
    EXPECT_TRUE(std::filesystem::exists(result.output + L"\\anim frame 01.png"));
}

TEST(Frames, StepForEveryNthFrame)
{
    EXPECT_EQ(formats::FrameStep(101), 2u);
    EXPECT_EQ(formats::FrameStep(200), 2u);
    EXPECT_EQ(formats::FrameStep(201), 3u);
    EXPECT_EQ(formats::FrameStep(250), 3u);
    EXPECT_EQ(formats::FrameStep(1000), 10u);
    EXPECT_EQ(formats::FrameStep(1001), 11u);
}

TEST(Frames, ALongGifAsksBeforeItBecomesAFolder)
{
    Fixture f;
    // 250 frames of 2 x 2: red, green, blue, red...
    std::vector<GifFrame> frames;
    for (uint8_t i = 0; i < 250; ++i)
        frames.push_back({ 0, 0, 2, 2, static_cast<uint8_t>(1 + i % 3), 1, 2 });
    const auto gif = f.dir / L"long.gif";
    WriteText(gif, MakeGif(2, 2, frames));
    ASSERT_EQ(formats::FrameCount(gif), 250u);

    std::vector<ask::Question> asked;
    int answer = ask::kCancel;
    ScopedAnswers answers([&](const ask::Question& q) {
        asked.push_back(q);
        return answer;
    });
    const auto names = [](const std::wstring& folder) {
        std::vector<std::wstring> out;
        for (const auto& e : std::filesystem::directory_iterator(folder))
            out.push_back(e.path().filename().wstring());
        std::sort(out.begin(), out.end());
        return out;
    };

    // Cancel: nothing is made.
    const auto cancelled = f.Run("to-png", gif);
    EXPECT_FALSE(cancelled.ok);
    EXPECT_TRUE(cancelled.skipped) << Narrow(cancelled.error);
    EXPECT_NE(cancelled.error.find(L"no frames were saved"), std::wstring::npos) << Narrow(cancelled.error);
    EXPECT_FALSE(std::filesystem::exists(f.dir / L"long (frames)"));
    EXPECT_EQ(CountFiles(f.dir, L".sdpart"), 0u);
    ASSERT_EQ(asked.size(), 1u);
    const auto q = asked[0];
    EXPECT_EQ(q.title, L"GIF to PNG");
    EXPECT_EQ(q.heading, L"long.gif has 250 frames");
    ASSERT_EQ(q.choices.size(), 2u);
    EXPECT_EQ(q.choices[0].label, L"Save all 250 frames");
    EXPECT_EQ(q.choices[1].label, L"Save every 3rd frame");
    EXPECT_EQ(q.choices[1].note, L"84 files: frames 1, 4, 7 and so on");
    EXPECT_EQ(q.unattended, q.choices[0].id) << "with no one to ask, every frame as before";

    // Every 3rd: frames 1, 4, ... 250 under their own numbers.
    answer = q.choices[1].id;
    const auto some = f.Run("to-png", gif);
    ASSERT_TRUE(some.ok) << Narrow(some.error);
    const auto kept = names(some.output);
    ASSERT_EQ(kept.size(), 84u);
    EXPECT_EQ(kept.front(), L"long frame 001.png");
    EXPECT_EQ(kept[1], L"long frame 004.png");
    EXPECT_EQ(kept.back(), L"long frame 250.png");
    formats::Image fourth;
    ASSERT_TRUE(ReadPixels(some.output + L"\\long frame 004.png", fourth));
    EXPECT_TRUE(IsColour(Px(fourth, 1, 1), 0, 0, 255)) << "frame 4 is red";
    EXPECT_EQ(some.message, L"84 of 250 frames saved as PNG");

    // All of them.
    answer = q.choices[0].id;
    const auto all = f.Run("to-png", gif);
    ASSERT_TRUE(all.ok) << Narrow(all.error);
    EXPECT_EQ(names(all.output).size(), 250u);

    // Short GIFs, and long ones that stay one file (WebP, TIFF), never ask.
    asked.clear();
    ASSERT_TRUE(f.Run("to-png", WriteAnimatedGif(f.dir, L"short.gif")).ok);
    ASSERT_TRUE(f.Run("to-tiff", gif).ok);
    EXPECT_TRUE(asked.empty());
}

TEST(Frames, AnimatedGifToTiffGivesOneTiffWithEveryFrame)
{
    Fixture f;
    const auto gif = WriteAnimatedGif(f.dir);
    const auto result = f.Run("to-tiff", gif);
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    EXPECT_EQ(std::filesystem::path(result.output).filename(), L"anim (converted).tiff");
    EXPECT_EQ(formats::FrameCount(result.output), 4u);
}

TEST(Frames, TiffPagesToPngGiveAFolderAndToPdfKeepEveryPage)
{
    Fixture f;
    const auto tiff = f.dir / L"scan.tiff";
    ASSERT_HRESULT_SUCCEEDED(WriteTiff(tiff, { Solid(30, 20, 0, 0, 255), Solid(20, 30, 0, 255, 0), Solid(10, 10, 255, 0, 0) }));
    ASSERT_EQ(formats::FrameCount(tiff), 3u);

    const auto png = f.Run("to-png", tiff);
    ASSERT_TRUE(png.ok) << Narrow(png.error);
    EXPECT_EQ(std::filesystem::path(png.output).filename(), L"scan (frames)");
    formats::Image second;
    ASSERT_TRUE(ReadPixels(png.output + L"\\scan page 02.png", second));
    EXPECT_EQ(second.width, 20u);   // each page keeps its own size
    EXPECT_EQ(second.height, 30u);
    EXPECT_TRUE(IsColour(Px(second, 5, 5), 0, 255, 0));
    EXPECT_TRUE(std::filesystem::exists(png.output + L"\\scan page 03.png"));

    // TIFF pages are not an animation: to WebP they are a WebP each.
    const auto webp = f.Run("to-webp", tiff);
    ASSERT_TRUE(webp.ok) << Narrow(webp.error);
    EXPECT_TRUE(std::filesystem::exists(webp.output + L"\\scan page 01.webp"));

    const auto& toPdf = f.recipes.Get("images-to-pdf");
    const auto still = f.dir / L"cover.png";
    ASSERT_HRESULT_SUCCEEDED(WritePng(still, 40, 40));
    const auto pdf = f.runner.RunBatch(toPdf, 0, { still, tiff }, {});
    ASSERT_TRUE(pdf.ok) << Narrow(pdf.error);
    const auto bytes = ReadAll(pdf.output);
    EXPECT_EQ(Count(bytes, "/Type /Page /Parent"), 4u);   // the PNG and all three TIFF pages
    EXPECT_NE(pdf.message.find(L"4 pages"), std::wstring::npos) << Narrow(pdf.message);
}
