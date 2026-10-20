#pragma once
#include "ocr/Ocr.h"
#include "pdf/Bitmap.h"
#include "pdf/PdfWriter.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Images -> PDF: one page per image, each page the size of its picture. JPEG files go in as they are
// (DCTDecode pass-through: no quality lost, no time spent); everything else is decoded and stored
// losslessly (Flate with PNG predictors), with a soft mask when it has transparency.
namespace sd::pdf
{
    struct JpegInfo
    {
        uint32_t width = 0, height = 0;
        int components = 0;    // 1 gray, 3 YCbCr/RGB, 4 CMYK
        bool adobe = false;    // has an Adobe APP14 marker (CMYK JPEGs from Photoshop store inverted values)
        double dpiX = 0, dpiY = 0;   // from the JFIF header, when it says dots per inch or per cm
    };
    // True for 8-bit baseline or progressive JPEGs, which every PDF reader decodes. Arithmetic-coded,
    // lossless and 12-bit JPEGs return false and get decoded instead.
    bool ParseJpeg(std::string_view bytes, JpegInfo& info);

    // The `cm` matrix that draws a stored image (unit square, row 0 at the top) upright on a page of
    // w x h points, for EXIF orientation 1-8.
    std::array<double, 6> OrientationMatrix(int orientation, double w, double h);

    // A page the size of the picture at its own resolution (96 dpi when the file has none), kept
    // within PDF's 200-inch limit.
    void PageSizeFor(uint32_t widthPx, uint32_t heightPx, double dpiX, double dpiY, double& widthPt, double& heightPt);

    struct ImageSource
    {
        std::string jpeg;      // pass-through bytes, or empty
        JpegInfo info;
        int orientation = 1;   // EXIF orientation of the pass-through JPEG
        Bitmap pixels;         // decoded and upright, when jpeg is empty
        double dpiX = 0, dpiY = 0;

        uint32_t DisplayWidth() const;
        uint32_t DisplayHeight() const;
    };
    bool LoadImageSource(const std::wstring& path, ImageSource& out, std::wstring& error);

    class ImagePdfBuilder
    {
    public:
        explicit ImagePdfBuilder(const std::wstring& path);
        bool Ok() const { return m_writer.Ok(); }

        // Adds a page showing `image`; with `ocr` (boxes in the upright image's pixels), also an
        // invisible text layer over it.
        bool AddPage(const ImageSource& image, const ocr::Result* ocr, std::wstring& error);
        bool Finish(std::wstring& error);
        size_t Pages() const { return m_kids.size(); }

    private:
        int TextFont();

        PdfWriter m_writer;
        int m_catalog = 0, m_pages = 0, m_info = 0, m_font = 0;
        std::vector<int> m_kids;
    };

    // PNG "Paeth" prediction of 8-bit rows (each row gets its filter-type byte), so Flate compresses
    // photos and screenshots far better. Exposed for tests.
    std::string PaethRows(const uint8_t* samples, uint32_t width, uint32_t height, uint32_t channels);
}
