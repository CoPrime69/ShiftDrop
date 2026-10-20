#include "pdf/ImagePdf.h"
#include "Brand.h"
#include "OutputPath.h"
#include "Utf.h"
#include "ocr/TextLayer.h"
#include "pdf/Wic.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>

namespace sd::pdf
{
    namespace
    {
        constexpr double kMaxPagePt = 14400.0;   // PDF's page size limit: 200 inches

        bool IsJpegExtension(const std::wstring& ext)
        {
            return ext == L".jpg" || ext == L".jpeg" || ext == L".jpe" || ext == L".jfif";
        }

        std::string ReadFile(const std::wstring& path)
        {
            std::ifstream in(std::filesystem::path(path), std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), {});
        }

        std::string Matrix(const std::array<double, 6>& m)
        {
            return std::format("{} {} {} {} {} {}", Num(m[0]), Num(m[1]), Num(m[2]), Num(m[3]), Num(m[4]), Num(m[5]));
        }

        std::string PdfDate()
        {
            SYSTEMTIME t{};
            GetSystemTime(&t);
            return std::format("(D:{:04}{:02}{:02}{:02}{:02}{:02}Z)", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        }
    }

    bool ParseJpeg(std::string_view bytes, JpegInfo& info)
    {
        const auto* b = reinterpret_cast<const unsigned char*>(bytes.data());
        const size_t n = bytes.size();
        if (n < 4 || b[0] != 0xFF || b[1] != 0xD8)
            return false;
        info = JpegInfo{};
        bool frame = false;
        size_t pos = 2;
        while (pos + 4 <= n)
        {
            if (b[pos] != 0xFF)
                return false;
            const unsigned char marker = b[pos + 1];
            if (marker == 0xFF)   // fill byte
            {
                ++pos;
                continue;
            }
            pos += 2;
            if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8))   // markers without a length
                continue;
            if (marker == 0xD9 || marker == 0xDA)   // end of image, start of scan: headers are done
                break;
            const size_t len = (static_cast<size_t>(b[pos]) << 8) | b[pos + 1];
            if (len < 2 || pos + len > n)
                return false;
            const unsigned char* seg = b + pos + 2;
            const size_t segLen = len - 2;
            if (marker == 0xE0 && segLen >= 12 && memcmp(seg, "JFIF\0", 5) == 0)
            {
                const int units = seg[7];
                const double x = (seg[8] << 8) | seg[9], y = (seg[10] << 8) | seg[11];
                if (units == 1 && x > 1 && y > 1)
                    info.dpiX = x, info.dpiY = y;
                else if (units == 2 && x > 1 && y > 1)
                    info.dpiX = x * 2.54, info.dpiY = y * 2.54;
            }
            else if (marker == 0xEE && segLen >= 5 && memcmp(seg, "Adobe", 5) == 0)
                info.adobe = true;
            else if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2)
            {
                if (segLen < 6 || seg[0] != 8)
                    return false;   // 12-bit samples
                info.height = (static_cast<uint32_t>(seg[1]) << 8) | seg[2];
                info.width = (static_cast<uint32_t>(seg[3]) << 8) | seg[4];
                info.components = seg[5];
                frame = true;
            }
            else if ((marker >= 0xC3 && marker <= 0xCF) && marker != 0xC4 && marker != 0xC8 && marker != 0xCC)
                return false;   // lossless, hierarchical or arithmetic-coded
            pos += len;
        }
        return frame && info.width > 0 && info.height > 0 && (info.components == 1 || info.components == 3 || info.components == 4);
    }

    std::array<double, 6> OrientationMatrix(int orientation, double W, double H)
    {
        // Derived from where each EXIF orientation puts the stored image's corners (TIFF 6.0 / EXIF 2.3).
        switch (orientation)
        {
        case 2: return { -W, 0, 0, H, W, 0 };    // mirrored left-right
        case 3: return { -W, 0, 0, -H, W, H };   // turned 180°
        case 4: return { W, 0, 0, -H, 0, H };    // mirrored top-bottom
        case 5: return { 0, -H, -W, 0, W, H };   // mirrored, turned 270° clockwise
        case 6: return { 0, -H, W, 0, 0, H };    // turned 90° clockwise
        case 7: return { 0, H, W, 0, 0, 0 };     // mirrored, turned 90° clockwise
        case 8: return { 0, H, -W, 0, W, 0 };    // turned 270° clockwise
        default: return { W, 0, 0, H, 0, 0 };
        }
    }

    void PageSizeFor(uint32_t widthPx, uint32_t heightPx, double dpiX, double dpiY, double& widthPt, double& heightPt)
    {
        if (dpiX <= 1 || dpiY <= 1)
            dpiX = dpiY = 96.0;
        widthPt = std::max(1.0, widthPx * 72.0 / dpiX);
        heightPt = std::max(1.0, heightPx * 72.0 / dpiY);
        const double over = std::max(widthPt, heightPt) / kMaxPagePt;
        if (over > 1.0)
        {
            widthPt = std::max(1.0, widthPt / over);
            heightPt = std::max(1.0, heightPt / over);
        }
    }

    uint32_t ImageSource::DisplayWidth() const
    {
        if (jpeg.empty())
            return pixels.width;
        return orientation >= 5 ? info.height : info.width;
    }

    uint32_t ImageSource::DisplayHeight() const
    {
        if (jpeg.empty())
            return pixels.height;
        return orientation >= 5 ? info.width : info.height;
    }

    bool LoadImageSource(const std::wstring& path, ImageSource& out, std::wstring& error)
    {
        out = ImageSource{};
        if (IsJpegExtension(LowerExtension(path)))
        {
            std::string bytes = ReadFile(path);
            JpegInfo info;
            if (ParseJpeg(bytes, info))
            {
                out.jpeg = std::move(bytes);
                out.info = info;
                out.orientation = ReadOrientation(path);
                out.dpiX = info.dpiX, out.dpiY = info.dpiY;
                if (out.dpiX <= 1)
                    ReadResolution(path, out.dpiX, out.dpiY);
                return true;
            }
        }
        if (!DecodeImageFile(path, out.pixels, error))
            return false;
        if (!ReadResolution(path, out.dpiX, out.dpiY))
            out.dpiX = out.dpiY = 0;
        return true;
    }

    std::string PaethRows(const uint8_t* samples, uint32_t width, uint32_t height, uint32_t channels)
    {
        const size_t row = static_cast<size_t>(width) * channels;
        std::string out;
        out.resize((row + 1) * height);
        auto* dst = reinterpret_cast<uint8_t*>(out.data());
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t* cur = samples + y * row;
            const uint8_t* up = y ? cur - row : nullptr;
            uint8_t* o = dst + y * (row + 1);
            *o++ = 4;   // filter type 4: Paeth
            for (size_t i = 0; i < row; ++i)
            {
                const int a = i >= channels ? cur[i - channels] : 0;
                const int b = up ? up[i] : 0;
                const int c = up && i >= channels ? up[i - channels] : 0;
                const int p = a + b - c;
                const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                const int pred = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                o[i] = static_cast<uint8_t>(cur[i] - pred);
            }
        }
        return out;
    }

    ImagePdfBuilder::ImagePdfBuilder(const std::wstring& path) : m_writer(path)
    {
        m_catalog = m_writer.NewObject();
        m_pages = m_writer.NewObject();
        m_info = m_writer.NewObject();
    }

    int ImagePdfBuilder::TextFont()
    {
        if (m_font)
            return m_font;
        const int descriptor = m_writer.NewObject(), cid = m_writer.NewObject(), toUnicode = m_writer.NewObject();
        m_font = m_writer.NewObject();
        m_writer.Object(descriptor, "<< /Type /FontDescriptor /FontName /SDGlyphless /Flags 4 /FontBBox [0 0 500 1000] /ItalicAngle 0 "
                                    "/Ascent 1000 /Descent 0 /CapHeight 1000 /StemV 80 >>");
        m_writer.Object(cid, std::format("<< /Type /Font /Subtype /CIDFontType2 /BaseFont /SDGlyphless /CIDSystemInfo << /Registry (Adobe) "
                                         "/Ordering (Identity) /Supplement 0 >> /FontDescriptor {} 0 R /DW 500 /CIDToGIDMap /Identity >>",
                                         descriptor));
        m_writer.FlateStream(toUnicode, "", ocr::IdentityToUnicodeCMap());
        m_writer.Object(m_font, std::format("<< /Type /Font /Subtype /Type0 /BaseFont /SDGlyphless /Encoding /Identity-H "
                                            "/DescendantFonts [{} 0 R] /ToUnicode {} 0 R >>",
                                            cid, toUnicode));
        return m_font;
    }

    bool ImagePdfBuilder::AddPage(const ImageSource& image, const ocr::Result* ocrResult, std::wstring& error)
    {
        const uint32_t dw = image.DisplayWidth(), dh = image.DisplayHeight();
        if (dw == 0 || dh == 0)
        {
            error = L"empty image";
            return false;
        }
        double W = 0, H = 0;
        PageSizeFor(dw, dh, image.dpiX, image.dpiY, W, H);

        const int xobject = m_writer.NewObject();
        std::array<double, 6> placement{ W, 0, 0, H, 0, 0 };
        if (!image.jpeg.empty())
        {
            const int n = image.info.components;
            const char* space = n == 1 ? "/DeviceGray" : n == 3 ? "/DeviceRGB" : "/DeviceCMYK";
            const char* decode = n == 4 && image.info.adobe ? " /Decode [1 0 1 0 1 0 1 0]" : "";
            m_writer.Stream(xobject,
                            std::format("/Type /XObject /Subtype /Image /Width {} /Height {} /ColorSpace {} /BitsPerComponent 8 /Filter /DCTDecode{}",
                                        image.info.width, image.info.height, space, decode),
                            image.jpeg);
            placement = OrientationMatrix(image.orientation, W, H);
        }
        else
        {
            const auto& px = image.pixels;
            const size_t count = static_cast<size_t>(px.width) * px.height;
            bool alpha = false, gray = true;
            for (size_t i = 0; i < count && (gray || !alpha); ++i)
            {
                const uint8_t* p = &px.bgra[i * 4];
                alpha = alpha || p[3] != 255;
                gray = gray && p[0] == p[1] && p[1] == p[2];
            }
            const uint32_t channels = gray ? 1 : 3;
            std::vector<uint8_t> samples(count * channels), mask(alpha ? count : 0);
            for (size_t i = 0; i < count; ++i)
            {
                const uint8_t* p = &px.bgra[i * 4];
                if (gray)
                    samples[i] = p[0];
                else
                {
                    samples[i * 3] = p[2];   // BGRA -> RGB
                    samples[i * 3 + 1] = p[1];
                    samples[i * 3 + 2] = p[0];
                }
                if (alpha)
                    mask[i] = p[3];
            }
            std::string smask;
            if (alpha)
            {
                const int maskObject = m_writer.NewObject();
                m_writer.FlateStream(maskObject,
                                     std::format("/Type /XObject /Subtype /Image /Width {} /Height {} /ColorSpace /DeviceGray /BitsPerComponent 8 "
                                                 "/DecodeParms << /Predictor 15 /Colors 1 /BitsPerComponent 8 /Columns {} >>",
                                                 px.width, px.height, px.width),
                                     PaethRows(mask.data(), px.width, px.height, 1));
                smask = std::format(" /SMask {} 0 R", maskObject);
            }
            m_writer.FlateStream(xobject,
                                 std::format("/Type /XObject /Subtype /Image /Width {} /Height {} /ColorSpace {} /BitsPerComponent 8 "
                                             "/DecodeParms << /Predictor 15 /Colors {} /BitsPerComponent 8 /Columns {} >>{}",
                                             px.width, px.height, gray ? "/DeviceGray" : "/DeviceRGB", channels, px.width, smask),
                                 PaethRows(samples.data(), px.width, px.height, channels));
        }

        std::string content = std::format("q {} cm /Im0 Do Q\n", Matrix(placement));
        std::string fonts;
        if (ocrResult)
        {
            const auto words = ocr::PlaceOnImagePage(*ocrResult, W, H);
            const std::string ops = ocr::TextLayerOps(words, "SDOcr");
            if (!ops.empty())
            {
                content += ops;
                fonts = std::format(" /Font << /SDOcr {} 0 R >>", TextFont());
            }
        }
        const int contents = m_writer.NewObject();
        m_writer.FlateStream(contents, "", content);
        const int page = m_writer.NewObject();
        m_writer.Object(page, std::format("<< /Type /Page /Parent {} 0 R /MediaBox [0 0 {} {}] /Resources << /XObject << /Im0 {} 0 R >>{} >> "
                                          "/Contents {} 0 R >>",
                                          m_pages, Num(W), Num(H), xobject, fonts, contents));
        m_kids.push_back(page);
        if (!m_writer.Ok())
        {
            error = L"could not write the PDF (disk full?)";
            return false;
        }
        return true;
    }

    bool ImagePdfBuilder::Finish(std::wstring& error)
    {
        if (m_kids.empty())
        {
            error = L"no pages";
            return false;
        }
        std::string kids;
        for (const int k : m_kids)
            kids += std::format("{}{} 0 R", kids.empty() ? "" : " ", k);
        m_writer.Object(m_pages, std::format("<< /Type /Pages /Kids [{}] /Count {} >>", kids, m_kids.size()));
        m_writer.Object(m_catalog, std::format("<< /Type /Catalog /Pages {} 0 R >>", m_pages));
        m_writer.Object(m_info, std::format("<< /Producer {} /CreationDate {} >>", LiteralString(Narrow(SD_NAME)), PdfDate()));
        if (!m_writer.Finish(m_catalog, m_info))
        {
            error = L"could not write the PDF (disk full?)";
            return false;
        }
        return true;
    }
}
