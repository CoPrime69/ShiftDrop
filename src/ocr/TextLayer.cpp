#include "ocr/TextLayer.h"
#include "pdf/PdfWriter.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace sd::ocr
{
    namespace
    {
        // Every glyph of the font is 500/1000 em wide (its /DW); Tz stretches a word to its box.
        constexpr double kGlyphEm = 0.5;

        size_t CodeCount(std::wstring_view text)
        {
            size_t n = 0;
            for (const wchar_t c : text)
                if (c < 0xD800 || c > 0xDFFF)
                    ++n;
            return n;
        }

        std::vector<PlacedWord> Place(const Result& ocr, auto&& toUser, double sx, double sy, int rotation)
        {
            std::vector<PlacedWord> out;
            for (const auto& line : ocr.lines)
            {
                for (size_t i = 0; i < line.words.size(); ++i)
                {
                    const auto& w = line.words[i];
                    if (w.text.empty() || w.w <= 0 || w.h <= 0)
                        continue;
                    PlacedWord p;
                    p.text = w.text;
                    // Baseline start = the box's bottom-left as displayed (the font's glyphs sit on the
                    // baseline and are one em tall, so the selection covers the box).
                    toUser(w.x * sx, (w.y + w.h) * sy, p.x, p.y);
                    p.width = w.w * sx;
                    p.height = w.h * sy;
                    p.rotation = rotation;
                    p.spaceAfter = i + 1 < line.words.size();
                    out.push_back(std::move(p));
                }
            }
            return out;
        }
    }

    std::string HexCodes(std::wstring_view text, bool spaceAfter)
    {
        std::string hex = "<";
        for (const wchar_t c : text)
            if (c < 0xD800 || c > 0xDFFF)
                hex += std::format("{:04X}", static_cast<unsigned>(c));
        if (spaceAfter)
            hex += "0020";
        return hex + ">";
    }

    std::string TextLayerOps(const std::vector<PlacedWord>& words, std::string_view fontName)
    {
        std::string ops;
        for (const auto& w : words)
        {
            const size_t n = CodeCount(w.text);
            if (n == 0 || w.height < 0.1 || w.width < 0.1)
                continue;
            if (ops.empty())
                ops = "BT\n3 Tr\n";
            const double size = w.height;
            const double scale = std::clamp(100.0 * w.width / (static_cast<double>(n) * kGlyphEm * size), 1.0, 10000.0);
            const double angle = (((w.rotation % 360) + 360) % 360) * 3.14159265358979323846 / 180.0;
            const double c = std::round(std::cos(angle)), s = std::round(std::sin(angle));   // exact for multiples of 90
            ops += std::format("/{} {} Tf {} Tz {} {} {} {} {} {} Tm {} Tj\n", fontName, pdf::Num(size), pdf::Num(scale), pdf::Num(c),
                               pdf::Num(s), pdf::Num(-s), pdf::Num(c), pdf::Num(w.x), pdf::Num(w.y), HexCodes(w.text, w.spaceAfter));
        }
        if (!ops.empty())
            ops += "ET\n";
        return ops;
    }

    std::string IdentityToUnicodeCMap()
    {
        std::string cmap =
            "/CIDInit /ProcSet findresource begin\n"
            "12 dict begin\n"
            "begincmap\n"
            "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
            "/CMapName /Adobe-Identity-UCS def\n"
            "/CMapType 2 def\n"
            "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
        // A bfrange may only vary in its last byte, so one range per high byte (minus the surrogate
        // blocks D8..DF), at most 100 ranges per section.
        std::vector<unsigned> highs;
        for (unsigned hi = 0; hi < 256; ++hi)
            if (hi < 0xD8 || hi > 0xDF)
                highs.push_back(hi);
        for (size_t i = 0; i < highs.size(); i += 100)
        {
            const size_t n = std::min<size_t>(100, highs.size() - i);
            cmap += std::format("{} beginbfrange\n", n);
            for (size_t k = 0; k < n; ++k)
                cmap += std::format("<{0:02X}00> <{0:02X}FF> <{0:02X}00>\n", highs[i + k]);
            cmap += "endbfrange\n";
        }
        cmap += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
        return cmap;
    }

    std::vector<PlacedWord> PlaceOnImagePage(const Result& ocr, double pageW, double pageH)
    {
        if (ocr.width == 0 || ocr.height == 0)
            return {};
        const double sx = pageW / ocr.width, sy = pageH / ocr.height;
        return Place(
            ocr, [&](double xd, double yd, double& ux, double& uy) { ux = xd, uy = pageH - yd; }, sx, sy, 0);
    }

    std::vector<PlacedWord> PlaceOnPdfPage(const Result& ocr, const double box[4], int rotate)
    {
        if (ocr.width == 0 || ocr.height == 0)
            return {};
        const double x0 = std::min(box[0], box[2]), x1 = std::max(box[0], box[2]);
        const double y0 = std::min(box[1], box[3]), y1 = std::max(box[1], box[3]);
        const int r = ((rotate % 360) + 360) % 360;
        const bool sideways = r == 90 || r == 270;
        const double dispW = sideways ? y1 - y0 : x1 - x0;
        const double dispH = sideways ? x1 - x0 : y1 - y0;
        const double sx = dispW / ocr.width, sy = dispH / ocr.height;
        // Displayed point (xd right, yd down from the top) back to user space, per /Rotate.
        auto toUser = [&](double xd, double yd, double& ux, double& uy) {
            switch (r)
            {
            case 90: ux = x0 + yd, uy = y0 + xd; break;
            case 180: ux = x1 - xd, uy = y0 + yd; break;
            case 270: ux = x1 - yd, uy = y1 - xd; break;
            default: ux = x0 + xd, uy = y1 - yd; break;
            }
        };
        return Place(ocr, toUser, sx, sy, r);
    }
}
