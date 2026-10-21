#include "ImageHooks.h"
#include "PipelineRunner.h"
#include "TestUtil.h"
#include "Utf.h"
#include "canvas/Raster.h"
#include "formats/Avif.h"
#include "formats/Heic.h"
#include "formats/Svg.h"

#include <avif/avif.h>
#include <avif/avif_cxx.h>
#include <zlib.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace sd;
using namespace sd::test;
using Microsoft::WRL::ComPtr;

namespace
{
    struct Fixture
    {
        TempDir dir;
        Recipes recipes{ dir };
        std::shared_ptr<const ImageCodecs> codecs = std::make_shared<const ImageCodecs>();
        PipelineRunner runner{ codecs, nullptr };

        ItemResult Run(std::string_view id, const std::wstring& source) { return runner.Run(recipes.Get(id), 0, source, {}); }
    };

    // Any file WIC reads, as straight BGRA.
    bool ReadPixels(const std::wstring& path, formats::Image& out)
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
               SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
               SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(formats::FromWicSource(factory.Get(), frame.Get(), out));
    }

    bool ReadAvifFile(const std::wstring& path, formats::Image& out)
    {
        const auto bytes = ReadAll(path);
        std::wstring error;
        return SUCCEEDED(formats::DecodeAvif(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), out, error));
    }

    const uint8_t* Px(const formats::Image& image, uint32_t x, uint32_t y) { return image.Row(y) + static_cast<size_t>(x) * 4; }

    // A picture with the structure of a photo: smooth colour fields, detail at several scales and a
    // little sensor grain, so the encoder timings mean something (pure noise or flat colour don't).
    formats::Image PhotoLike(uint32_t w, uint32_t h)
    {
        formats::Image image;
        image.width = w;
        image.height = h;
        image.bgra.resize(static_cast<size_t>(w) * h * 4);
        for (uint32_t y = 0; y < h; ++y)
        {
            uint8_t* p = image.Row(y);
            for (uint32_t x = 0; x < w; ++x, p += 4)
            {
                const double u = static_cast<double>(x) / w, v = static_cast<double>(y) / h;
                const double detail = std::sin(u * 40 + std::sin(v * 13) * 3) * std::cos(v * 37 + std::sin(u * 11) * 2) +
                                      0.5 * std::sin((u + v) * 160) * std::sin((u - v) * 90);
                uint32_t n = x * 0x9E3779B9u ^ y * 0x85EBCA6Bu;
                n ^= n >> 15;
                n *= 0x2C1B3C6Du;
                n ^= n >> 12;
                const double grain = static_cast<double>(n & 0xFF) / 255.0 - 0.5;
                const auto channel = [&](double base) {
                    return static_cast<uint8_t>(std::clamp(base + detail * 38 + grain * 14, 0.0, 255.0));
                };
                p[0] = channel(90 + 80 * v);           // sky-ish blue towards the bottom
                p[1] = channel(110 + 60 * std::sin(u * 3.1));
                p[2] = channel(150 - 70 * v + 40 * u);
                p[3] = 255;
            }
        }
        return image;
    }

    const formats::Image& Photo12Mp()
    {
        static const formats::Image image = PhotoLike(4000, 3000);
        return image;
    }
}

// ---- SVG ------------------------------------------------------------------------------------------

TEST(Formats, SvgSizeDefaults)
{
    using formats::SvgRasterSize;
    // Its own size, as stated (lunasvg has resolved units and filled the other side from the viewBox).
    EXPECT_EQ(SvgRasterSize(true, 200, 100, 0, 0).width, 200u);
    EXPECT_EQ(SvgRasterSize(true, 200, 100, 0, 0).height, 100u);
    EXPECT_EQ(SvgRasterSize(true, 16, 16, 0, 0).width, 16u);   // a small icon stays small
    EXPECT_EQ(SvgRasterSize(true, 99.6f, 10, 0, 0).width, 100u);
    // No size: 1024 px on the long side, in the viewBox's shape...
    EXPECT_EQ(SvgRasterSize(false, 24, 12, 0, 0).width, 1024u);
    EXPECT_EQ(SvgRasterSize(false, 24, 12, 0, 0).height, 512u);
    EXPECT_EQ(SvgRasterSize(false, 10, 40, 0, 0).height, 1024u);
    EXPECT_EQ(SvgRasterSize(false, 10, 40, 0, 0).width, 256u);
    // ...or the drawing's, or square.
    EXPECT_EQ(SvgRasterSize(false, 0, 0, 300, 150).height, 512u);
    EXPECT_EQ(SvgRasterSize(false, 0, 0, 0, 0).width, 1024u);
    EXPECT_EQ(SvgRasterSize(false, 0, 0, 0, 0).height, 1024u);
    // An enormous stated size is capped.
    EXPECT_EQ(SvgRasterSize(true, 100000, 50000, 0, 0).width, formats::kSvgMaxSide);
    EXPECT_EQ(SvgRasterSize(true, 100000, 50000, 0, 0).height, formats::kSvgMaxSide / 2);

    EXPECT_TRUE(formats::IsAbsoluteLength("200"));
    EXPECT_TRUE(formats::IsAbsoluteLength(" 10.5cm "));
    EXPECT_TRUE(formats::IsAbsoluteLength(".5in"));
    EXPECT_FALSE(formats::IsAbsoluteLength("100%"));
    EXPECT_FALSE(formats::IsAbsoluteLength("auto"));
    EXPECT_FALSE(formats::IsAbsoluteLength(""));
}

TEST(Formats, SvgToPngKeepsOwnSizeAndTransparency)
{
    Fixture f;
    const auto svg = f.dir / L"logo.svg";
    WriteText(svg, R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100"><rect x="0" y="0" width="100" height="100" fill="#ff0000"/></svg>)");
    EXPECT_TRUE(f.codecs->CanDecode(L".svg"));
    EXPECT_TRUE(f.runner.Applies(f.recipes.Get("to-png"), svg));

    const auto result = f.Run("to-png", svg);
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    EXPECT_EQ(std::filesystem::path(result.output).filename(), L"logo (converted).png");
    formats::Image png;
    ASSERT_TRUE(ReadPixels(result.output, png));
    EXPECT_EQ(png.width, 200u);
    EXPECT_EQ(png.height, 100u);
    const auto* red = Px(png, 50, 50);
    EXPECT_EQ(red[2], 255);   // BGRA
    EXPECT_EQ(red[1], 0);
    EXPECT_EQ(red[3], 255);
    EXPECT_EQ(Px(png, 150, 50)[3], 0);   // nothing drawn: transparent
}

TEST(Formats, SvgWithoutSizeIs1024OnTheLongSide)
{
    Fixture f;
    const auto svg = f.dir / L"icon.svg";
    WriteText(svg, R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 12"><circle cx="6" cy="6" r="6" fill="blue"/></svg>)");
    const auto result = f.Run("to-png", svg);
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    UINT w = 0, h = 0;
    ASSERT_TRUE(ImageSize(result.output, w, h));
    EXPECT_EQ(w, 1024u);
    EXPECT_EQ(h, 512u);
    // The preview card reads the same size without drawing.
    uint32_t pw = 0, ph = 0;
    ASSERT_NE(DecodeHookFor(L".svg"), nullptr);
    ASSERT_TRUE(DecodeHookFor(L".svg")->ReadSize(svg, pw, ph));
    EXPECT_EQ(pw, 1024u);
    EXPECT_EQ(ph, 512u);
}

TEST(Formats, SvgConvertsToEveryImageFormatAndPdf)
{
    Fixture f;
    const auto svg = f.dir / L"badge.svg";
    WriteText(svg, R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="48" viewBox="0 0 64 48">)"
                   R"(<rect width="64" height="48" rx="8" fill="#2266cc"/><path d="M8 24 L32 8 L56 24 Z" fill="#ffcc00"/></svg>)");
    for (const char* id : { "to-jpg", "to-webp", "to-avif", "to-tiff", "to-bmp" })
    {
        const auto result = f.Run(id, svg);
        ASSERT_TRUE(result.ok) << id << ": " << Narrow(result.error);
        EXPECT_GT(result.size, 0);
    }
    formats::Image avif;
    ASSERT_TRUE(ReadAvifFile(f.dir / L"badge (converted).avif", avif));
    EXPECT_EQ(avif.width, 64u);
    EXPECT_EQ(avif.height, 48u);

    const auto& toPdf = f.recipes.Get("images-to-pdf");
    ASSERT_TRUE(f.runner.Applies(toPdf, svg));
    const auto pdf = f.runner.RunBatch(toPdf, 0, { svg }, {});
    ASSERT_TRUE(pdf.ok) << Narrow(pdf.error);
    EXPECT_EQ(ReadAll(pdf.output).rfind("%PDF-", 0), 0u);
}

TEST(Formats, SvgzIsUnzipped)
{
    // A gzipped SVG (.svgz), written with zlib's gz* functions.
    Fixture f;
    const auto path = f.dir / L"zipped.svgz";
    const std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="30" height="20"><rect width="30" height="20" fill="green"/></svg>)";
    gzFile gz = gzopen_w(path.c_str(), "wb");
    ASSERT_NE(gz, nullptr);
    gzwrite(gz, svg.data(), static_cast<unsigned>(svg.size()));
    gzclose(gz);
    const auto result = f.Run("to-png", path);
    ASSERT_TRUE(result.ok) << Narrow(result.error);
    UINT w = 0, h = 0;
    ASSERT_TRUE(ImageSize(result.output, w, h));
    EXPECT_EQ(w, 30u);
    EXPECT_EQ(h, 20u);
}

TEST(Formats, BrokenSvgFailsWithAReason)
{
    Fixture f;
    const auto svg = f.dir / L"broken.svg";
    WriteText(svg, "this is not an svg");
    const auto result = f.Run("to-png", svg);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find(L"SVG"), std::wstring::npos) << Narrow(result.error);
    EXPECT_EQ(CountFiles(f.dir, L".sdpart"), 0u);
}

// ---- AVIF -----------------------------------------------------------------------------------------

TEST(Formats, AvifRoundTripKeepsSizeColourAndAlpha)
{
    Fixture f;
    const auto png = f.dir / L"photo.png";
    ASSERT_HRESULT_SUCCEEDED(WritePng(png, 320, 240));   // gradient, transparent left half
    const auto& toAvif = f.recipes.Get("to-avif");
    EXPECT_EQ(toAvif.steps[0].params["quality"], 60);
    const auto avif = f.Run("to-avif", png);
    ASSERT_TRUE(avif.ok) << Narrow(avif.error);
    EXPECT_EQ(std::filesystem::path(avif.output).filename(), L"photo (converted).avif");

    formats::Image decoded;
    ASSERT_TRUE(ReadAvifFile(avif.output, decoded));
    EXPECT_EQ(decoded.width, 320u);
    EXPECT_EQ(decoded.height, 240u);
    EXPECT_EQ(Px(decoded, 10, 120)[3], 0);     // transparent half stays transparent
    EXPECT_EQ(Px(decoded, 300, 120)[3], 255);
    const auto* p = Px(decoded, 240, 120);     // WritePng: B = x*255/w, G = y*255/h, R = 128
    EXPECT_NEAR(p[0], 240 * 255 / 320, 12);
    EXPECT_NEAR(p[1], 120 * 255 / 240, 12);
    EXPECT_NEAR(p[2], 128, 12);

    // And back: AVIF is a source like any other image.
    EXPECT_TRUE(f.runner.Applies(f.recipes.Get("to-png"), avif.output));
    EXPECT_TRUE(f.runner.IsNoOp(toAvif, avif.output));
    const auto back = f.Run("to-png", avif.output);
    ASSERT_TRUE(back.ok) << Narrow(back.error);
    UINT w = 0, h = 0;
    ASSERT_TRUE(ImageSize(back.output, w, h));
    EXPECT_EQ(w, 320u);
    EXPECT_EQ(h, 240u);
    uint32_t pw = 0, ph = 0;
    ASSERT_TRUE(DecodeHookFor(L".avif")->ReadSize(avif.output, pw, ph));
    EXPECT_EQ(pw, 320u);
    EXPECT_EQ(ph, 240u);
}

TEST(Formats, AvifToolsKeepAvifAndCompressStaysAvif)
{
    Fixture f;
    const auto png = f.dir / L"pic.png";
    ASSERT_HRESULT_SUCCEEDED(WritePng(png, 400, 200, true));
    const auto avif = f.Run("to-avif", png);
    ASSERT_TRUE(avif.ok) << Narrow(avif.error);

    const auto rotated = f.Run("rotate-right", avif.output);
    ASSERT_TRUE(rotated.ok) << Narrow(rotated.error);
    EXPECT_EQ(std::filesystem::path(rotated.output).extension(), L".avif");
    formats::Image turned;
    ASSERT_TRUE(ReadAvifFile(rotated.output, turned));
    EXPECT_EQ(turned.width, 200u);
    EXPECT_EQ(turned.height, 400u);

    EXPECT_STREQ(f.recipes.Get("compress-image").OutputFormatFor(L".avif")->id, "avif");
    EXPECT_STREQ(f.recipes.Get("image-under-1mb").OutputFormatFor(L".avif")->id, "avif");
    EXPECT_STREQ(f.recipes.Get("compress-image").OutputFormatFor(L".png")->id, "jpg");
    // Under 1 MB on a picture already far under it: skipped, nothing written.
    const auto fit = f.Run("image-under-1mb", avif.output);
    EXPECT_FALSE(fit.ok);
}

TEST(Formats, AvifTurnAndMirrorBoxesAreApplied)
{
    // A 4 x 2 picture with one red pixel at the top left, stored with irot (90 degrees anti-clockwise):
    // decoded upright it is 2 x 4 with the red pixel at the bottom left.
    formats::Image source;
    source.width = 4;
    source.height = 2;
    source.bgra.assign(4 * 2 * 4, 0);
    for (size_t i = 0; i < source.bgra.size(); i += 4)
        source.bgra[i + 3] = 255;   // opaque black
    source.bgra[2] = 255;           // (0, 0) red

    // Encode through libavif with the box set, the way a camera would write it.
    avif::ImagePtr image(avifImageCreate(4, 2, 8, AVIF_PIXEL_FORMAT_YUV444));
    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, image.get());
    rgb.format = AVIF_RGB_FORMAT_BGRA;
    rgb.ignoreAlpha = AVIF_TRUE;
    rgb.pixels = source.bgra.data();
    rgb.rowBytes = 16;
    ASSERT_EQ(avifImageRGBToYUV(image.get(), &rgb), AVIF_RESULT_OK);
    image->transformFlags = AVIF_TRANSFORM_IROT;
    image->irot.angle = 1;
    avif::EncoderPtr encoder(avifEncoderCreate());
    encoder->quality = 100;
    avifRWData out = AVIF_DATA_EMPTY;
    ASSERT_EQ(avifEncoderWrite(encoder.get(), image.get(), &out), AVIF_RESULT_OK);

    formats::Image upright;
    std::wstring error;
    ASSERT_HRESULT_SUCCEEDED(formats::DecodeAvif(out.data, out.size, upright, error)) << Narrow(error);
    uint32_t w = 0, h = 0;
    EXPECT_TRUE(formats::ReadAvifSize(out.data, out.size, w, h));
    avifRWDataFree(&out);
    EXPECT_EQ(upright.width, 2u);
    EXPECT_EQ(upright.height, 4u);
    EXPECT_EQ(w, 2u);
    EXPECT_EQ(h, 4u);
    EXPECT_GT(Px(upright, 0, 3)[2], 200);   // red now at the bottom left
    EXPECT_LT(Px(upright, 0, 0)[2], 60);
}

TEST(Formats, TurnsAndMirrorsMoveThePixelsTheRightWay)
{
    formats::Image image;
    image.width = 3;
    image.height = 2;
    image.bgra.resize(3 * 2 * 4);
    for (uint8_t i = 0; i < 6; ++i)
        image.bgra[i * 4] = i;   // 0 1 2 / 3 4 5
    auto at = [](const formats::Image& m, uint32_t x, uint32_t y) { return Px(m, x, y)[0]; };

    auto ccw = image;
    formats::RotateAntiClockwise(ccw, 1);   // 2 5 / 1 4 / 0 3
    ASSERT_EQ(ccw.width, 2u);
    EXPECT_EQ(at(ccw, 0, 0), 2);
    EXPECT_EQ(at(ccw, 1, 0), 5);
    EXPECT_EQ(at(ccw, 0, 2), 0);
    auto half = image;
    formats::RotateAntiClockwise(half, 2);   // 5 4 3 / 2 1 0
    EXPECT_EQ(at(half, 0, 0), 5);
    auto cw = image;
    formats::RotateAntiClockwise(cw, 3);     // 3 0 / 4 1 / 5 2
    EXPECT_EQ(at(cw, 0, 0), 3);
    EXPECT_EQ(at(cw, 1, 2), 2);
    auto flipped = image;
    formats::Mirror(flipped, 0);             // 3 4 5 / 0 1 2
    EXPECT_EQ(at(flipped, 0, 0), 3);
    auto mirrored = image;
    formats::Mirror(mirrored, 1);            // 2 1 0 / 5 4 3
    EXPECT_EQ(at(mirrored, 0, 0), 2);
    EXPECT_EQ(at(mirrored, 2, 1), 3);
}

TEST(Formats, AvifChromaAndDefaults)
{
    EXPECT_FALSE(formats::AvifFullChroma(60));
    EXPECT_TRUE(formats::AvifFullChroma(90));
    EXPECT_GE(formats::kAvifSpeed, 6);
    EXPECT_LE(formats::kAvifSpeed, 9);
    // Higher quality makes a bigger file (the Compress search depends on it).
    const auto photo = PhotoLike(512, 384);
    std::vector<uint8_t> low, high;
    std::wstring error;
    ASSERT_HRESULT_SUCCEEDED(formats::EncodeAvif(photo, { 40 }, low, error)) << Narrow(error);
    ASSERT_HRESULT_SUCCEEDED(formats::EncodeAvif(photo, { 85 }, high, error)) << Narrow(error);
    EXPECT_LT(low.size(), high.size());
}

// ---- HEIC -----------------------------------------------------------------------------------------

TEST(Formats, MissingHeicCodecDimsTheSliceAndSaysWhy)
{
    Fixture f;
    const auto png = f.dir / L"photo.png";
    ASSERT_HRESULT_SUCCEEDED(WritePng(png, 64, 64));
    const auto& toHeic = f.recipes.Get("to-heic");
    ASSERT_TRUE(f.runner.Applies(toHeic, png));   // shown on the wheel...

    formats::OverrideHeicSupportForTests(formats::HeicSupport::NoHeif);
    std::wstring why;
    EXPECT_FALSE(f.runner.Accepts(toHeic, png, &why));   // ...but dimmed, with what to install
    EXPECT_NE(why.find(L"HEIF Image Extensions"), std::wstring::npos) << Narrow(why);
    EXPECT_NE(why.find(L"HEVC Video Extensions"), std::wstring::npos) << Narrow(why);
    const auto result = f.Run("to-heic", png);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find(L"HEIF Image Extensions"), std::wstring::npos);
    EXPECT_EQ(CountFiles(f.dir, L".heic"), 0u);

    formats::OverrideHeicSupportForTests(formats::HeicSupport::NoHevc);   // HEIF there, HEVC encoder missing
    why.clear();
    EXPECT_FALSE(f.runner.Accepts(toHeic, png, &why));
    EXPECT_NE(why.find(L"HEVC Video Extensions"), std::wstring::npos) << Narrow(why);

    formats::OverrideHeicSupportForTests(formats::HeicSupport::Ready);
    EXPECT_TRUE(f.runner.Accepts(toHeic, png, &why));
    formats::OverrideHeicSupportForTests(std::nullopt);
}

TEST(Formats, HeicSourceWithoutCodecIsExplained)
{
    Fixture f;
    const auto heic = f.dir / L"IMG_0001.heic";
    WriteText(heic, "not really a heic");
    const auto& toJpg = f.recipes.Get("to-jpg");
    EXPECT_TRUE(f.runner.Applies(toJpg, heic));   // an image, whatever this PC can open
    if (f.codecs->CanDecode(L".heic"))
    {
        // Windows' HEIF codec is installed here: a broken file still names the extensions to check.
        const auto result = f.Run("to-jpg", heic);
        EXPECT_FALSE(result.ok);
        EXPECT_NE(result.error.find(L"HEVC Video Extensions"), std::wstring::npos) << Narrow(result.error);
        return;
    }
    std::wstring why;
    EXPECT_FALSE(f.runner.Accepts(toJpg, heic, &why));
    EXPECT_NE(why.find(L"HEIF Image Extensions"), std::wstring::npos) << Narrow(why);
    EXPECT_EQ(f.codecs->MissingCodecLabel(L".heic"), L"Needs HEIF Image Extensions");
    EXPECT_TRUE(f.codecs->MissingCodecLabel(L".png").empty());
    EXPECT_FALSE(f.runner.Applies(toJpg, f.dir / L"notes.txt"));
}

TEST(Formats, HeicSameFormatBecomesJpgAndHeifIsAlreadyHeic)
{
    TempDir dir;
    Recipes recipes(dir);
    EXPECT_STREQ(recipes.Get("rotate-right").OutputFormatFor(L".heic")->id, "jpg");
    EXPECT_STREQ(recipes.Get("rotate-right").OutputFormatFor(L".avif")->id, "avif");
    EXPECT_STREQ(recipes.Get("to-heic").OutputFormatFor(L".png")->id, "heic");
    PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr);
    EXPECT_TRUE(runner.IsNoOp(recipes.Get("to-heic"), dir / L"x.heif"));
    EXPECT_FALSE(runner.IsNoOp(recipes.Get("to-heic"), dir / L"x.png"));
}

TEST(Formats, HeicRoundTripWhereWindowsHasTheCodec)
{
    auto support = formats::HeicEncoderSupport();
    for (int i = 0; i < 100 && support == formats::HeicSupport::Checking; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        support = formats::HeicEncoderSupport();
    }
    if (support != formats::HeicSupport::Ready)
        GTEST_SKIP() << "Windows' HEIF/HEVC codecs are not installed: " << Narrow(formats::HeicMissingReason(support));
    Fixture f;
    const auto png = f.dir / L"photo.png";
    ASSERT_HRESULT_SUCCEEDED(WritePng(png, 640, 480, true));
    const auto heic = f.Run("to-heic", png);
    ASSERT_TRUE(heic.ok) << Narrow(heic.error);
    const auto back = f.Run("to-png", heic.output);
    ASSERT_TRUE(back.ok) << Narrow(back.error);
    UINT w = 0, h = 0;
    ASSERT_TRUE(ImageSize(back.output, w, h));
    EXPECT_EQ(w, 640u);
    EXPECT_EQ(h, 480u);
}

// ---- The image editors (M6: Crop, Redact, Share-safe) take the new formats too ---------------------

TEST(Formats, EditorsReadAvifAndSvgAndWriteAvif)
{
    Fixture f;
    const auto svg = f.dir / L"art.svg";
    WriteText(svg, R"(<svg xmlns="http://www.w3.org/2000/svg" width="80" height="60"><rect width="40" height="60" fill="#00ff00"/></svg>)");
    for (const char* id : { "crop", "redact", "share-safe" })
        if (const auto* r = f.recipes.store.Find(id))
            EXPECT_TRUE(f.runner.Applies(*r, svg)) << id;

    canvas::Raster raster;
    std::wstring error;
    ASSERT_HRESULT_SUCCEEDED(canvas::LoadRaster(svg, raster, error)) << Narrow(error);
    EXPECT_EQ(raster.width, 80u);
    EXPECT_EQ(raster.At(10, 10)[1], 255);
    EXPECT_EQ(raster.At(70, 10)[3], 0);

    // An edited AVIF stays AVIF; an edited HEIC becomes JPEG (Windows' encoder is optional); SVG becomes PNG.
    EXPECT_STREQ(canvas::EditedFormatFor(L".avif").id, "avif");
    EXPECT_STREQ(canvas::EditedFormatFor(L".heic").id, "jpg");
    EXPECT_STREQ(canvas::EditedFormatFor(L".svg").id, "png");
    const auto avif = f.dir / L"edited.avif";
    ASSERT_HRESULT_SUCCEEDED(canvas::SaveRasterToFile(raster, *FindImageFormat("avif"), canvas::EditedQuality(*FindImageFormat("avif")), avif, error))
        << Narrow(error);
    canvas::Raster back;
    ASSERT_HRESULT_SUCCEEDED(canvas::LoadRaster(avif, back, error)) << Narrow(error);
    EXPECT_EQ(back.width, 80u);
    EXPECT_EQ(back.height, 60u);
    EXPECT_LT(back.At(70, 10)[3], 8);   // transparency survives
}

// ---- AVIF speed (ctest prints each test's time; the Generate test is the baseline to subtract) ------

namespace
{
    void EncodeAtSpeed(int speed)
    {
        const auto& photo = Photo12Mp();
        std::vector<uint8_t> out;
        std::wstring error;
        const auto t0 = std::chrono::steady_clock::now();
        ASSERT_HRESULT_SUCCEEDED(formats::EncodeAvif(photo, { 60, speed }, out, error)) << Narrow(error);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::printf("AVIF 12 MP, quality 60, speed %d, %d threads: %lld ms, %zu bytes\n", speed, formats::CodecThreads(),
                    static_cast<long long>(ms), out.size());
        EXPECT_GT(out.size(), 10'000u);
    }
}

TEST(FormatsSpeed, Generate12Mp) { EXPECT_EQ(Photo12Mp().width, 4000u); }
TEST(FormatsSpeed, AvifEncode12MpSpeed6) { EncodeAtSpeed(6); }
TEST(FormatsSpeed, AvifEncode12MpSpeed7) { EncodeAtSpeed(7); }
TEST(FormatsSpeed, AvifEncode12MpSpeed8) { EncodeAtSpeed(8); }
TEST(FormatsSpeed, AvifEncode12MpSpeed9) { EncodeAtSpeed(9); }

TEST(FormatsSpeed, AvifDecode12Mp)
{
    std::vector<uint8_t> avif;
    std::wstring error;
    ASSERT_HRESULT_SUCCEEDED(formats::EncodeAvif(Photo12Mp(), { 60, 9 }, avif, error)) << Narrow(error);
    formats::Image decoded;
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_HRESULT_SUCCEEDED(formats::DecodeAvif(avif.data(), avif.size(), decoded, error)) << Narrow(error);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("AVIF 12 MP decode (dav1d, %d threads): %lld ms\n", formats::CodecThreads(), static_cast<long long>(ms));
    EXPECT_EQ(decoded.width, 4000u);
}
