#include "Ffmpeg.h"
#include "FileIo.h"
#include "PathUtil.h"
#include "PipelineRunner.h"
#include "TestUtil.h"
#include "Utf.h"
#include "canvas/Raster.h"
#include "metadata/Container.h"
#include "metadata/Exif.h"
#include "metadata/MediaMetadata.h"
#include "metadata/Metadata.h"
#include "redact/RedactTool.h"

#include <webp/decode.h>
#include <zlib.h>

#include <cstring>

using namespace sd;
using namespace sd::metadata;
using namespace sd::test;
using Bytes = std::vector<uint8_t>;

namespace
{
    // ---- A TIFF/EXIF structure written by hand (little-endian), independent of the code under test ----

    struct Entry
    {
        uint16_t tag, type;
        uint32_t count;
        Bytes value;
    };

    void Put16(Bytes& b, uint32_t v) { b.insert(b.end(), { static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8) }); }
    void Put32(Bytes& b, uint32_t v)
    {
        b.insert(b.end(), { static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24) });
    }
    void PutBE16(Bytes& b, uint32_t v) { b.insert(b.end(), { static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v) }); }
    void PutBE32(Bytes& b, uint32_t v)
    {
        b.insert(b.end(), { static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v) });
    }

    Entry Ascii(uint16_t tag, const std::string& s)
    {
        Bytes v(s.begin(), s.end());
        v.push_back(0);
        return { tag, 2, static_cast<uint32_t>(v.size()), v };
    }
    Entry Short(uint16_t tag, uint16_t value)
    {
        Bytes v;
        Put16(v, value);
        return { tag, 3, 1, v };
    }
    Entry Long(uint16_t tag, uint32_t value)
    {
        Bytes v;
        Put32(v, value);
        return { tag, 4, 1, v };
    }
    Entry Rationals(uint16_t tag, std::initializer_list<std::pair<uint32_t, uint32_t>> values)
    {
        Bytes v;
        for (const auto& [n, d] : values)
        {
            Put32(v, n);
            Put32(v, d);
        }
        return { tag, 5, static_cast<uint32_t>(values.size()), v };
    }

    size_t IfdSize(const std::vector<Entry>& e)
    {
        size_t n = 2 + 12 * e.size() + 4;
        for (const auto& x : e)
            if (x.value.size() > 4)
                n += (x.value.size() + 1) & ~size_t{ 1 };
        return n;
    }

    void WriteIfd(Bytes& out, const std::vector<Entry>& entries)
    {
        uint32_t extra = static_cast<uint32_t>(out.size() + 2 + 12 * entries.size() + 4);
        Put16(out, static_cast<uint32_t>(entries.size()));
        for (const auto& e : entries)
        {
            Put16(out, e.tag);
            Put16(out, e.type);
            Put32(out, e.count);
            if (e.value.size() <= 4)
            {
                Bytes v = e.value;
                v.resize(4, 0);
                out.insert(out.end(), v.begin(), v.end());
            }
            else
            {
                Put32(out, extra);
                extra += static_cast<uint32_t>((e.value.size() + 1) & ~size_t{ 1 });
            }
        }
        Put32(out, 0);
        for (const auto& e : entries)
            if (e.value.size() > 4)
            {
                out.insert(out.end(), e.value.begin(), e.value.end());
                if (e.value.size() & 1)
                    out.push_back(0);
            }
    }

    // IFD0 (camera, date), the Exif IFD (date taken) and the GPS IFD (48 deg 51' 29.6" N, 2 deg 17' 40.2" E).
    Bytes CameraExif(uint16_t orientation = 1)
    {
        std::vector<Entry> exif = { Ascii(0x9003, "2026:07:14 18:30:00") };
        std::vector<Entry> gps = { Ascii(0x0001, "N"), Rationals(0x0002, { { 48, 1 }, { 51, 1 }, { 296, 10 } }), Ascii(0x0003, "E"),
                                   Rationals(0x0004, { { 2, 1 }, { 17, 1 }, { 402, 10 } }) };
        std::vector<Entry> ifd0 = { Ascii(0x010F, "TestCam"), Ascii(0x0110, "X1 Pro"), Short(0x0112, orientation), Ascii(0x0132, "2026:07:14 18:30:00"),
                                    Long(0x8769, 0), Long(0x8825, 0) };
        const uint32_t exifAt = static_cast<uint32_t>(8 + IfdSize(ifd0));
        const uint32_t gpsAt = static_cast<uint32_t>(exifAt + IfdSize(exif));
        ifd0[4] = Long(0x8769, exifAt);
        ifd0[5] = Long(0x8825, gpsAt);
        Bytes t = { 'I', 'I', 42, 0 };
        Put32(t, 8);
        WriteIfd(t, ifd0);
        WriteIfd(t, exif);
        WriteIfd(t, gps);
        return t;
    }

    canvas::Raster Picture(UINT w, UINT h)
    {
        canvas::Raster r;
        r.Allocate(w, h);
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
            {
                uint8_t* p = r.At(x, y);
                p[0] = static_cast<uint8_t>(x * 4);
                p[1] = static_cast<uint8_t>(y * 4);
                p[2] = static_cast<uint8_t>((x ^ y) * 8);
                p[3] = 255;
            }
        return r;
    }

    Bytes Encode(const char* format, UINT w = 64, UINT h = 48)
    {
        Bytes out;
        std::wstring error;
        EXPECT_HRESULT_SUCCEEDED(canvas::EncodeRaster(Picture(w, h), *FindImageFormat(format), 90, out, error)) << Narrow(error);
        if (out.size() < 16)
            throw std::runtime_error("could not encode the test picture");   // fails the test instead of crashing it
        return out;
    }

    Bytes Segment(uint8_t marker, std::string_view signature, const Bytes& payload)
    {
        Bytes s = { 0xFF, marker };
        PutBE16(s, static_cast<uint32_t>(2 + signature.size() + payload.size()));
        s.insert(s.end(), signature.begin(), signature.end());
        s.insert(s.end(), payload.begin(), payload.end());
        return s;
    }

    // A JPEG as a phone writes it: EXIF with GPS, an XMP packet, a comment, and a video after the end.
    Bytes PhoneJpeg(uint16_t orientation = 1)
    {
        const Bytes plain = Encode("jpg");
        Bytes out(plain.begin(), plain.begin() + 2);   // SOI
        const auto app1 = Segment(0xE1, std::string_view("Exif\0\0", 6), CameraExif(orientation));
        out.insert(out.end(), app1.begin(), app1.end());
        const std::string xmp = R"(<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
                                R"(<rdf:Description xmlns:exif="http://ns.adobe.com/exif/1.0/" exif:GPSLatitude="48,51.49N"/></rdf:RDF></x:xmpmeta>)";
        const auto app1x = Segment(0xE1, std::string_view("http://ns.adobe.com/xap/1.0/\0", 29), Bytes(xmp.begin(), xmp.end()));
        out.insert(out.end(), app1x.begin(), app1x.end());
        const std::string comment = "secret comment";
        const auto com = Segment(0xFE, "", Bytes(comment.begin(), comment.end()));
        out.insert(out.end(), com.begin(), com.end());
        out.insert(out.end(), plain.begin() + 2, plain.end());
        const std::string trailer = "MOTIONPHOTO-DATA-0123456789";
        out.insert(out.end(), trailer.begin(), trailer.end());
        return out;
    }

    // The entropy-coded picture: from the first SOS to EOI.
    Bytes ScanData(const Bytes& jpeg)
    {
        for (size_t i = 2; i + 1 < jpeg.size(); ++i)
            if (jpeg[i] == 0xFF && jpeg[i + 1] == 0xDA)
                for (size_t j = jpeg.size() - 2; j > i; --j)
                    if (jpeg[j] == 0xFF && jpeg[j + 1] == 0xD9)
                        return Bytes(jpeg.begin() + static_cast<std::ptrdiff_t>(i), jpeg.begin() + static_cast<std::ptrdiff_t>(j) + 2);
        return {};
    }

    bool Contains(const Bytes& b, std::string_view needle)
    {
        return std::search(b.begin(), b.end(), needle.begin(), needle.end()) != b.end();
    }

    Bytes PngChunk(const char type[4], const Bytes& data)
    {
        Bytes c;
        PutBE32(c, static_cast<uint32_t>(data.size()));
        c.insert(c.end(), type, type + 4);
        c.insert(c.end(), data.begin(), data.end());
        PutBE32(c, static_cast<uint32_t>(crc32(0, c.data() + 4, static_cast<uInt>(data.size() + 4))));
        return c;
    }

    bool HasField(const Inventory& inv, const std::string& key)
    {
        return std::any_of(inv.fields.begin(), inv.fields.end(), [&](const Field& f) { return f.key == key; });
    }

    bool HasGroup(const Inventory& inv, const std::wstring& group)
    {
        return std::any_of(inv.fields.begin(), inv.fields.end(), [&](const Field& f) { return f.group == group && f.personal; });
    }

    struct Fixture
    {
        TempDir dir;
        Recipes recipes{ dir };
        std::unique_ptr<HistoryStore> history;
        std::unique_ptr<PipelineRunner> runner;
        explicit Fixture(std::shared_ptr<const FfmpegEngine> ffmpeg = nullptr)
        {
            std::wstring error;
            history = HistoryStore::Open(dir / L"history.db", error);
            runner = std::make_unique<PipelineRunner>(std::make_shared<const ImageCodecs>(), history.get(), std::move(ffmpeg));
        }
        ItemResult Run(const Recipe& recipe, const std::wstring& file)
        {
            const auto runId = history->BeginRun(recipe, 1);
            auto r = runner->Run(recipe, runId, file, std::stop_token{});
            history->FinishRun(runId);
            return r;
        }
        std::wstring Write(const std::wstring& name, const Bytes& bytes)
        {
            const auto path = dir / name;
            EXPECT_TRUE(WriteFileBytes(path, bytes, WriteMode::Any));
            return path;
        }
    };
}

// ---- Reading -------------------------------------------------------------------------------------------

TEST(ShareSafe, ReadsMetadataGrouped)
{
    const Bytes jpeg = PhoneJpeg();
    ASSERT_EQ(DetectContainer(jpeg), Container::Jpeg);
    const auto inv = ReadMetadata(jpeg);
    EXPECT_TRUE(inv.lossless);
    EXPECT_TRUE(HasGroup(inv, L"Location"));
    EXPECT_TRUE(HasGroup(inv, L"Camera"));
    EXPECT_TRUE(HasGroup(inv, L"Dates"));
    EXPECT_TRUE(HasGroup(inv, L"XMP"));
    EXPECT_TRUE(HasGroup(inv, L"Comments"));
    EXPECT_TRUE(HasGroup(inv, L"Hidden data"));   // the bytes after the end of the picture
    EXPECT_TRUE(HasField(inv, "exif.0th.271"));
    const auto make = std::find_if(inv.fields.begin(), inv.fields.end(), [](const Field& f) { return f.key == "exif.0th.271"; });
    EXPECT_EQ(make->value, L"TestCam");
    EXPECT_TRUE(make->editable);
    const auto groups = inv.Groups();
    EXPECT_EQ(groups.front(), L"Location");   // the most telling first

    ExifData exif;
    ASSERT_TRUE(exif.Parse(CameraExif()));
    const auto lat = GpsDegrees(exif, true), lon = GpsDegrees(exif, false);
    ASSERT_TRUE(lat && lon);
    EXPECT_NEAR(*lat, 48 + 51 / 60.0 + 29.6 / 3600, 1e-9);
    EXPECT_NEAR(*lon, 2 + 17 / 60.0 + 40.2 / 3600, 1e-9);
}

TEST(ShareSafe, ExifRoundTripsAndEdits)
{
    ExifData exif;
    ASSERT_TRUE(exif.Parse(CameraExif()));
    EXPECT_EQ(exif.FieldCount(), 4u + 1u + 4u);   // pointers are not fields
    const auto again = exif.Serialize();
    ExifData back;
    ASSERT_TRUE(back.Parse(again));
    EXPECT_EQ(back.FieldCount(), exif.FieldCount());
    ASSERT_NE(back.Find(Ifd::Gps, 0x0002), nullptr);
    EXPECT_DOUBLE_EQ(back.Number(*back.Find(Ifd::Gps, 0x0002), 2), 29.6);

    const auto make = back.FromText(Ifd::Primary, 0x010F, L"Canon");
    ASSERT_TRUE(make.has_value());
    back.Set(Ifd::Primary, *make);
    EXPECT_FALSE(back.FromText(Ifd::Exif, 0x829A, L"fast").has_value());   // exposure time must be a number
    back.RemoveIfd(Ifd::Gps);
    ExifData edited;
    ASSERT_TRUE(edited.Parse(back.Serialize()));
    EXPECT_EQ(edited.Find(Ifd::Gps, 0x0002), nullptr);
    EXPECT_EQ(FormatValue(edited, Ifd::Primary, *edited.Find(Ifd::Primary, 0x010F)), L"Canon");
}

// ---- Removing ------------------------------------------------------------------------------------------

TEST(ShareSafe, StripKeepsTheCompressedPictureByteForByte)
{
    const Bytes jpeg = PhoneJpeg();
    Bytes clean;
    std::vector<Block> removed;
    std::wstring error;
    ASSERT_TRUE(StripPersonal(jpeg, clean, &removed, &error)) << Narrow(error);
    EXPECT_GE(removed.size(), 4u);   // EXIF, XMP, comment, trailer
    EXPECT_EQ(ScanData(clean), ScanData(jpeg));
    EXPECT_FALSE(ScanData(clean).empty());
    for (const auto* secret : { "TestCam", "secret comment", "MOTIONPHOTO", "GPSLatitude", "2026:07:14" })
        EXPECT_FALSE(Contains(clean, secret)) << secret;
    EXPECT_EQ(clean[clean.size() - 2], 0xFF);
    EXPECT_EQ(clean.back(), 0xD9);   // ends where the picture ends
    EXPECT_EQ(ReadMetadata(clean).PersonalCount(), 0u);
}

TEST(ShareSafe, RemoveMetadataIsVerifiedAndReported)
{
    Fixture f;
    const auto source = f.Write(L"IMG_0001.jpg", PhoneJpeg());
    EXPECT_FALSE(VerifyNoMetadata(source).clean);
    EXPECT_FALSE(VerifyNoMetadata(source).leftovers.empty());

    const auto& recipe = f.recipes.Get("remove-metadata");
    EXPECT_EQ(recipe.ring, "files");
    EXPECT_FALSE(f.runner->EngineFor(recipe)->NeedsEditor(recipe));
    const auto r = f.Run(recipe, source);
    ASSERT_TRUE(r.ok) << Narrow(r.error);
    EXPECT_EQ(FileName(r.output), L"IMG_0001 (no metadata).jpg");
    EXPECT_TRUE(r.message.starts_with(L"Removed GPS, camera, ")) << Narrow(r.message);
    EXPECT_TRUE(r.message.ends_with(L" fields")) << Narrow(r.message);

    // Re-opened independently: nothing personal, the picture intact.
    Bytes out;
    ASSERT_TRUE(ReadFileBytes(r.output, out));
    EXPECT_EQ(ScanData(out), ScanData(PhoneJpeg()));
    for (const auto* secret : { "TestCam", "X1 Pro", "secret comment", "MOTIONPHOTO", "GPSLatitude" })
        EXPECT_FALSE(Contains(out, secret)) << secret;
    const auto check = VerifyNoMetadata(r.output);
    EXPECT_TRUE(check.clean);
    EXPECT_TRUE(check.opens);
    // WIC's own metadata readers see nothing personal either (where WIC exposes JPEG metadata).
    if (!WicMetadataPaths(source).empty())
        for (const auto& path : WicMetadataPaths(r.output))
            EXPECT_FALSE(IsPersonalWicPath(path)) << Narrow(path);

    // A clean file: nothing to do, said plainly.
    const auto again = f.Run(recipe, r.output);
    EXPECT_TRUE(again.skipped);
    EXPECT_NE(again.error.find(L"no metadata"), std::wstring::npos);
    EXPECT_EQ(CountFiles(f.dir, L".sdpart"), 0u);
}

TEST(ShareSafe, SidewaysPhotosStayTheRightWayUp)
{
    Fixture f;
    const auto source = f.Write(L"portrait.jpg", PhoneJpeg(6));
    const auto r = f.Run(f.recipes.Get("share-safe"), source);
    ASSERT_TRUE(r.ok) << Narrow(r.error);
    EXPECT_EQ(FileName(r.output), L"portrait (share-safe).jpg");
    const auto inv = ReadMetadata(r.output);
    EXPECT_EQ(inv.PersonalCount(), 0u);
    ASSERT_TRUE(HasField(inv, "exif.0th.274"));   // only the orientation is left
    EXPECT_EQ(inv.fields.size(), 1u);
    EXPECT_TRUE(VerifyNoMetadata(r.output).clean);
    // Shown the same way up as the original (turned, where WIC reads the orientation).
    canvas::Raster shown, original;
    std::wstring error;
    ASSERT_HRESULT_SUCCEEDED(canvas::LoadRaster(r.output, shown, error));
    ASSERT_HRESULT_SUCCEEDED(canvas::LoadRaster(source, original, error));
    EXPECT_EQ(shown.width, original.width);
    EXPECT_EQ(shown.height, original.height);
}

TEST(ShareSafe, PngTextAndTimeAreRemoved)
{
    Fixture f;
    Bytes png = Encode("png");
    std::string text = "Author";
    text.push_back('\0');
    text += "Jane Doe";
    const auto tEXt = PngChunk("tEXt", Bytes(text.begin(), text.end()));
    const auto tIME = PngChunk("tIME", { 0x07, 0xEA, 7, 14, 18, 30, 0 });
    png.insert(png.begin() + 33, tIME.begin(), tIME.end());   // after the signature and IHDR
    png.insert(png.begin() + 33, tEXt.begin(), tEXt.end());
    const auto source = f.Write(L"scan.png", png);
    EXPECT_TRUE(HasField(ReadMetadata(source), "png.text.Author"));
    EXPECT_TRUE(HasField(ReadMetadata(source), "png.time"));

    const auto r = f.Run(f.recipes.Get("remove-metadata"), source);
    ASSERT_TRUE(r.ok) << Narrow(r.error);
    Bytes out;
    ASSERT_TRUE(ReadFileBytes(r.output, out));
    EXPECT_FALSE(Contains(out, "Jane Doe"));
    EXPECT_FALSE(Contains(out, "tIME"));
    EXPECT_TRUE(VerifyNoMetadata(r.output).clean);
    EXPECT_EQ(out.size(), Encode("png").size());   // exactly the plain file again
}

TEST(ShareSafe, WebPExifIsRemovedAndTheHeaderFixed)
{
    const Bytes plain = Encode("webp");
    ASSERT_EQ(DetectContainer(plain), Container::WebP);
    // Rebuild it as an extended WebP with an EXIF chunk, as cameras and phones write it.
    const Bytes tiff = CameraExif();
    Bytes out = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', 'X' };
    Put32(out, 10);
    out.insert(out.end(), { 0x08, 0, 0, 0 });   // EXIF flag
    const uint32_t w1 = 63, h1 = 47;
    out.insert(out.end(), { static_cast<uint8_t>(w1), 0, 0, static_cast<uint8_t>(h1), 0, 0 });
    out.insert(out.end(), plain.begin() + 12, plain.end());   // the picture's own chunk
    out.insert(out.end(), { 'E', 'X', 'I', 'F' });
    Put32(out, static_cast<uint32_t>(tiff.size()));
    out.insert(out.end(), tiff.begin(), tiff.end());
    if (tiff.size() & 1)
        out.push_back(0);
    const uint32_t riff = static_cast<uint32_t>(out.size() - 8);
    std::memcpy(out.data() + 4, &riff, 4);
    int w = 0, h = 0;
    ASSERT_TRUE(WebPGetInfo(out.data(), out.size(), &w, &h));
    EXPECT_TRUE(HasGroup(ReadMetadata(out), L"Location"));

    Bytes clean;
    std::wstring error;
    ASSERT_TRUE(StripPersonal(out, clean, nullptr, &error)) << Narrow(error);
    EXPECT_FALSE(Contains(clean, "TestCam"));
    EXPECT_EQ(clean[20] & 0x08, 0);   // the EXIF flag is cleared
    uint32_t size = 0;
    std::memcpy(&size, clean.data() + 4, 4);
    EXPECT_EQ(size, clean.size() - 8);
    ASSERT_TRUE(WebPGetInfo(clean.data(), clean.size(), &w, &h));
    EXPECT_EQ(w, 64);
    EXPECT_EQ(ReadMetadata(clean).PersonalCount(), 0u);
}

// ---- Editing (the API Edit Metadata builds on) -----------------------------------------------------------

TEST(ShareSafe, RemoveSomeFieldsAndSetOne)
{
    const Bytes jpeg = PhoneJpeg();
    Bytes noGps;
    std::wstring error;
    ASSERT_TRUE(RemoveFields(jpeg, { "exif.gps", "trailer" }, noGps, error)) << Narrow(error);
    const auto inv = ReadMetadata(noGps);
    EXPECT_FALSE(std::any_of(inv.fields.begin(), inv.fields.end(), [](const Field& f) { return f.key.starts_with("exif.gps"); }));
    EXPECT_TRUE(HasField(inv, "exif.0th.271"));   // the camera stays
    EXPECT_TRUE(HasField(inv, "xmp"));
    EXPECT_FALSE(Contains(noGps, "MOTIONPHOTO"));
    EXPECT_EQ(ScanData(noGps), ScanData(jpeg));

    Bytes renamed;
    ASSERT_TRUE(SetField(noGps, "exif.0th.271", L"Leica", renamed, error)) << Narrow(error);
    const auto after = ReadMetadata(renamed);
    const auto make = std::find_if(after.fields.begin(), after.fields.end(), [](const Field& f) { return f.key == "exif.0th.271"; });
    ASSERT_NE(make, after.fields.end());
    EXPECT_EQ(make->value, L"Leica");
    EXPECT_FALSE(SetField(noGps, "exif.exif.33434", L"fast", renamed, error));
    EXPECT_FALSE(error.empty());

    // Adding EXIF to a file that has none.
    Bytes dated;
    ASSERT_TRUE(SetField(Encode("jpg"), "exif.exif.36867", L"2026:10:20 09:00:00", dated, error)) << Narrow(error);
    EXPECT_TRUE(HasField(ReadMetadata(dated), "exif.exif.36867"));
}

TEST(ShareSafe, CropCarriesDatesButNotTheOldThumbnail)
{
    Bytes edited = Encode("jpg", 20, 10);
    ASSERT_TRUE(CarryEditedExif(PhoneJpeg(6), edited, 20, 10));
    const auto inv = ReadMetadata(edited);
    EXPECT_TRUE(HasField(inv, "exif.0th.271"));
    EXPECT_FALSE(HasField(inv, "exif.0th.274"));   // the pixels are upright now
    EXPECT_FALSE(HasField(inv, "exif.1st.thumbnail"));
    EXPECT_FALSE(CarryEditedExif(Encode("jpg"), edited, 20, 10));   // nothing to carry
}

// ---- Share-safe with covers from the review -----------------------------------------------------------------

TEST(ShareSafe, ReviewCoversAreBurnedInAndMetadataGoes)
{
    Fixture f;
    const auto source = f.Write(L"receipt.jpg", PhoneJpeg());
    Recipe recipe = f.recipes.Get("share-safe");
    EXPECT_EQ(recipe.ring, "tools");
    EXPECT_FALSE(f.runner->EngineFor(recipe)->NeedsEditor(recipe));   // no editor window in the tests
    recipe.steps[0].params = redact::CoversToJson({ source }, { { { { 8, 8, 16, 8 }, canvas::CoverKind::Solid, 0x000000 } } });
    const auto r = f.Run(recipe, source);
    ASSERT_TRUE(r.ok) << Narrow(r.error);
    EXPECT_TRUE(r.message.starts_with(L"Covered 1 detail")) << Narrow(r.message);
    EXPECT_TRUE(VerifyNoMetadata(r.output).clean);
    canvas::Raster out;
    std::wstring error;
    ASSERT_HRESULT_SUCCEEDED(canvas::LoadRaster(r.output, out, error));
    for (UINT y = 10; y < 14; ++y)   // inside, away from the JPEG block edges
        for (UINT x = 10; x < 22; ++x)
            EXPECT_LT(out.At(x, y)[0] + out.At(x, y)[1] + out.At(x, y)[2], 90);   // black, give or take JPEG ringing
}

// ---- Video and audio --------------------------------------------------------------------------------------

TEST(ShareSafe, MediaTagsFromAProbeLog)
{
    const std::vector<std::string> log = {
        "Input #0, mov,mp4,m4a,3gp,3g2,mj2, from 'C:\\clip.mp4':",
        "  Metadata:",
        "    major_brand     : isom",
        "    creation_time   : 2026-07-14T18:30:00.000000Z",
        "    location        : +48.8577+002.2950/",
        "    com.apple.quicktime.make: Apple",
        "  Duration: 00:00:02.00, start: 0.000000, bitrate: 300 kb/s",
        "  Chapters:",
        "    Chapter #0:0: start 0.000000, end 1.000000",
        "      Metadata:",
        "        title           : Beach",
        "  Stream #0:0[0x1](und): Video: h264 (High) (avc1 / 0x31637661), yuv420p, 1920x1080, 25 fps",
        "      Metadata:",
        "        handler_name    : VideoHandler",
        "        vendor_id       : [0][0][0][0]",
        "  Stream #0:1[0x2](und): Data: none (mett / 0x7474656D)",
        "  Stream #0:2: Video: mjpeg (Baseline), yuvj420p, 600x600, 90k tbr (attached pic)",
        "At least one output file must be specified",
    };
    const auto tags = ParseMediaTags(log);
    EXPECT_EQ(tags.chapters, 1u);
    EXPECT_EQ(tags.dataStreams, 1u);
    EXPECT_EQ(tags.coverArt, 1u);
    const auto personal = tags.Personal();
    ASSERT_EQ(personal.size(), 4u);   // creation_time, location, make, the chapter title
    EXPECT_EQ(personal[1].key, "location");
    EXPECT_EQ(personal[2].key, "com.apple.quicktime.make");
    EXPECT_EQ(personal[3].scope, MediaTag::Scope::Chapter);
    EXPECT_EQ(tags.PersonalCount(), 7u);
    const auto kinds = tags.Kinds();
    ASSERT_GE(kinds.size(), 3u);
    EXPECT_EQ(kinds[0], L"GPS");
    EXPECT_EQ(kinds[1], L"camera");
    EXPECT_TRUE(IsPlumbingTag("compatible_brands"));
    EXPECT_FALSE(IsPlumbingTag("artist"));
}

TEST(ShareSafe, MediaArgsCopyStreamsAndDropMetadata)
{
    const auto a = StripMediaArgs(L".mp4");
    const auto has = [&](std::initializer_list<const wchar_t*> seq) {
        const std::vector<std::wstring> s(seq.begin(), seq.end());
        return std::search(a.begin(), a.end(), s.begin(), s.end()) != a.end();
    };
    EXPECT_TRUE(has({ L"-map_metadata", L"-1" }));
    EXPECT_TRUE(has({ L"-map_chapters", L"-1" }));
    EXPECT_TRUE(has({ L"-c", L"copy" }));
    EXPECT_TRUE(has({ L"-map", L"0:V?" }));   // not cover art
    EXPECT_TRUE(has({ L"-f", L"mp4" }));
    EXPECT_FALSE(has({ L"-map", L"0:d?" }));   // never the data tracks
    EXPECT_EQ(MuxerFor(L".mkv"), L"matroska");
    EXPECT_EQ(MuxerFor(L".m4a"), L"ipod");
    EXPECT_TRUE(MuxerFor(L".xyz").empty());
    const auto mp3 = StripMediaArgs(L".mp3");
    EXPECT_NE(std::find(mp3.begin(), mp3.end(), L"-write_xing"), mp3.end());
}

TEST(ShareSafe, VideoIsStreamCopiedWithoutMetadata)
{
    const auto engine = TestFfmpeg();
    if (!engine->Available())
        GTEST_SKIP() << "SHIFTDROP_FFMPEG is not set";
    Fixture f(engine);
    const auto source = f.dir / L"holiday.mp4";
    std::stop_source never;
    ASSERT_TRUE(engine->Run({ L"-hide_banner", L"-nostdin", L"-f", L"lavfi", L"-i", L"testsrc=s=160x120:r=25:d=1", L"-f", L"lavfi", L"-i",
                              L"sine=f=440:d=1", L"-c:v", L"mpeg4", L"-c:a", L"aac", L"-shortest", L"-metadata", L"title=Secret trip",
                              L"-metadata", L"creation_time=2026-07-14T18:30:00Z", L"-metadata", L"location=+48.8577+002.2950/",
                              L"-y", L"-f", L"mp4", source },
                            nullptr, never.get_token())
                    .ok);
    std::vector<std::string> log;
    engine->Run(ProbeArgs(source), nullptr, never.get_token(), &log);
    const auto before = ParseMediaTags(log);
    ASSERT_GE(before.Personal().size(), 2u);

    const auto& recipe = f.recipes.Get("share-safe");
    EXPECT_EQ(f.runner->Lane(recipe), JobLane::Media);
    EXPECT_TRUE(f.runner->Accepts(recipe, source, nullptr));
    const auto r = f.Run(recipe, source);
    ASSERT_TRUE(r.ok) << Narrow(r.error);
    EXPECT_EQ(FileName(r.output), L"holiday (share-safe).mp4");
    EXPECT_TRUE(r.message.starts_with(L"Removed ")) << Narrow(r.message);

    log.clear();
    engine->Run(ProbeArgs(r.output), nullptr, never.get_token(), &log);
    const auto after = ParseMediaTags(log);
    EXPECT_EQ(after.PersonalCount(), 0u);
    const auto info = ParseMediaInfo(log);
    EXPECT_EQ(info.videoCodec, "mpeg4");   // copied, not re-encoded
    EXPECT_EQ(info.audioCodec, "aac");
    EXPECT_FALSE(Contains([&] { Bytes b; ReadFileBytes(r.output, b); return b; }(), "Secret trip"));

    const auto again = f.Run(recipe, r.output);
    EXPECT_TRUE(again.skipped);
    EXPECT_EQ(CountFiles(f.dir, L".sdpart"), 0u);
}
