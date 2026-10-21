#pragma once
#include "canvas/Geometry.h"
#include "canvas/Raster.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Marks drawn onto a PDF page as PDF content, over the page as it is (Fill and sign, Annotate on a
// page): the page keeps its own text and drawings, sharp at any zoom; typed text goes in as real
// text in a subset of its font (searchable, copyable); pictures and signatures as images; shapes as
// paths. What can't be written that way (a magnifier's loupe, text in a font that may not be
// embedded) comes as a high-resolution picture of just that mark.
//
// Positions are in the pixels of the picture of the page the marks were made on: (0, 0) is the
// top-left corner of the page as shown (its crop box, turned by its /Rotate), y going down.
namespace sd::pdf
{
    using canvas::PointD;
    using canvas::RectD;

    // A font file to embed a subset of: one face (of a collection) as DirectWrite picked it.
    struct MarkFont
    {
        std::string file;           // the whole font file
        uint32_t index = 0;         // the face in a collection
        std::string key;            // the same face, the same key
        std::string name;           // a fallback name ("SegoeUI-Bold") when the file has none
    };

    // A run of glyphs of one font at one size.
    struct MarkGlyphs
    {
        size_t font = 0;                     // into PageOverlay::fonts
        double size = 0;                     // em size
        PointD origin;                       // where the run's baseline starts (its right end when `rtl`)
        bool rtl = false;
        std::vector<uint16_t> glyphs;
        std::vector<double> advances;
        std::vector<PointD> offsets;         // per glyph: along the line, and up from the baseline (may be empty)
        std::vector<std::wstring> text;      // per glyph: the characters it shows (empty for a cluster's later glyphs)
        uint32_t rgb = 0;
        bool bold = false, oblique = false;  // simulated by the renderer: drawn thicker / slanted here too
    };

    struct MarkPath
    {
        enum class Op : uint8_t { Move, Line, Quad, Cubic, Close };
        struct Step
        {
            Op op = Op::Move;
            PointD p[3];   // Quad: control, end; Cubic: two controls, end
        };
        std::vector<Step> steps;
        bool fill = false, stroke = false;
        double width = 1;          // stroke width
        uint32_t rgb = 0;
        float alpha = 1;
        bool round = true;         // round caps and joins

        void MoveTo(PointD p) { steps.push_back({ Op::Move, { p } }); }
        void LineTo(PointD p) { steps.push_back({ Op::Line, { p } }); }
        void QuadTo(PointD c, PointD p) { steps.push_back({ Op::Quad, { c, p } }); }
        void CubicTo(PointD c1, PointD c2, PointD p) { steps.push_back({ Op::Cubic, { c1, c2, p } }); }
        void Close() { steps.push_back({ Op::Close, {} }); }
        void Rect(const RectD& r);
        void Ellipse(PointD centre, double rx, double ry);
        void RoundedRect(const RectD& r, double radius);
    };

    // A picture placed in `at` (a signature, an inserted picture, or a mark drawn as pixels).
    struct MarkImage
    {
        std::shared_ptr<const canvas::Raster> raster;
        RectD at;
    };

    struct MarkItem
    {
        enum class Kind : uint8_t { Path, Glyphs, Image };
        Kind kind = Kind::Path;
        MarkPath path;
        MarkGlyphs glyphs;
        MarkImage image;
        RectD clip;   // not empty: drawn clipped to it
    };

    struct PageOverlay
    {
        double width = 0, height = 0;   // the picture of the page, pixels
        std::vector<MarkFont> fonts;
        std::vector<MarkItem> items;    // bottom to top
    };

    // How a page was written, for logs and tests.
    struct MarkedPageInfo
    {
        size_t textRuns = 0, fonts = 0, images = 0, paths = 0;
    };

    // Writes page `page` (from 0) of `source` with `overlay` drawn over it as a new one-page PDF at
    // `out`. The page's own content, boxes, rotation and links are kept as they are.
    bool WriteMarkedPage(const std::wstring& source, uint32_t page, const PageOverlay& overlay, const std::wstring& out, std::wstring& error,
                         MarkedPageInfo* info = nullptr);

    // The matrix (a b c d e f) from picture pixels to the page's default user space, for a page whose
    // crop box is `crop` (x0 y0 x1 y1) turned by `rotate` degrees, shown as a `width` x `height` picture.
    void PictureToPage(const double crop[4], int rotate, double width, double height, double out[6]);
}
