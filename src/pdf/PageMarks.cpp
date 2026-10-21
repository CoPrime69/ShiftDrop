#include "pdf/PageMarks.h"
#include "Log.h"
#include "Utf.h"
#include "pdf/FontSubset.h"
#include "pdf/ImagePdf.h"
#include "pdf/PdfDoc.h"
#include "pdf/PdfWriter.h"

#include <qpdf/QPDFPageDocumentHelper.hh>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>

namespace sd::pdf
{
    namespace
    {
        constexpr double kArc = 0.5522847498;   // a quarter circle as a cubic Bézier

        std::string Rgb(uint32_t rgb)
        {
            return std::format("{} {} {}", Num(((rgb >> 16) & 0xFF) / 255.0), Num(((rgb >> 8) & 0xFF) / 255.0), Num((rgb & 0xFF) / 255.0));
        }

        std::string Pt(PointD p) { return Num(p.x) + " " + Num(p.y); }

        std::string Hex16(uint16_t v) { return std::format("{:04X}", v); }

        // The UTF-16 of a text as the hex a ToUnicode CMap takes.
        std::string Utf16Hex(const std::wstring& text)
        {
            std::string out;
            for (const wchar_t c : text)
                out += Hex16(static_cast<uint16_t>(c));
            return out;
        }

        // A font name PDF accepts: letters, digits and a few marks, at most 63 characters.
        std::string CleanName(const std::string& name)
        {
            std::string out;
            for (const char c : name)
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
                    out.push_back(c);
            if (out.empty())
                out = "Font";
            return out.substr(0, 56);
        }

        // Six capitals naming a subset (PDF's "ABCDEF+Name"), from what it holds.
        std::string SubsetTag(const std::string& key, const std::vector<uint16_t>& glyphs)
        {
            uint64_t h = 1469598103934665603ull;
            const auto mix = [&](uint8_t b) { h = (h ^ b) * 1099511628211ull; };
            for (const char c : key)
                mix(uint8_t(c));
            for (const uint16_t g : glyphs)
            {
                mix(uint8_t(g & 0xFF));
                mix(uint8_t(g >> 8));
            }
            std::string tag;
            for (int i = 0; i < 6; ++i, h /= 26)
                tag.push_back(char('A' + h % 26));
            return tag;
        }

        QPDFObjectHandle Dict(const std::string& inside) { return QPDFObjectHandle::parse("<< " + inside + " >>"); }

        QPDFObjectHandle Stream(QPDF& pdf, std::string data, const std::string& dict)
        {
            auto s = pdf.newStream(std::move(data));
            const auto d = Dict(dict);
            for (const auto& key : d.getKeys())
                s.getDict().replaceKey(key, d.getKey(key));
            return s;
        }

        // A subset font as Type0 / CIDFontType2 with Identity-H, its widths and its ToUnicode map.
        QPDFObjectHandle FontObject(QPDF& pdf, const MarkFont& font, const FontSubset& subset, const std::map<uint16_t, std::wstring>& unicode)
        {
            const std::string name = SubsetTag(font.key, subset.glyphs) + "+" + CleanName(subset.postScriptName.empty() ? font.name : subset.postScriptName);
            auto file = Stream(pdf, Deflate(subset.data), std::format("/Filter /FlateDecode /Length1 {}", subset.data.size()));
            const bool italic = std::abs(subset.italicAngle) > 0.01;
            auto descriptor = pdf.makeIndirectObject(
                Dict(std::format("/Type /FontDescriptor /FontName /{} /Flags {} /FontBBox [{} {} {} {}] /ItalicAngle {} /Ascent {} /Descent {} "
                                 "/CapHeight {} /StemV 80",
                                 name, 4 | (subset.fixedPitch ? 1 : 0) | (italic ? 64 : 0), Num(subset.ToPdf(subset.xMin)), Num(subset.ToPdf(subset.yMin)),
                                 Num(subset.ToPdf(subset.xMax)), Num(subset.ToPdf(subset.yMax)), Num(subset.italicAngle), Num(subset.ToPdf(subset.ascent)),
                                 Num(subset.ToPdf(subset.descent)), Num(subset.ToPdf(subset.capHeight)))));
            descriptor.replaceKey("/FontFile2", pdf.makeIndirectObject(file));

            std::string widths;
            for (const uint16_t a : subset.advances)
                widths += (widths.empty() ? "" : " ") + Num(std::round(subset.ToPdf(a) * 10) / 10);
            auto cid = pdf.makeIndirectObject(Dict(std::format("/Type /Font /Subtype /CIDFontType2 /BaseFont /{} "
                                                               "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> "
                                                               "/CIDToGIDMap /Identity /DW 1000 /W [0 [{}]]",
                                                               name, widths)));
            cid.replaceKey("/FontDescriptor", descriptor);

            std::string cmap = "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
                               "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
                               "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
                               "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
            std::vector<std::pair<uint16_t, std::wstring>> entries(unicode.begin(), unicode.end());
            for (size_t i = 0; i < entries.size(); i += 100)
            {
                const size_t n = std::min<size_t>(100, entries.size() - i);
                cmap += std::format("{} beginbfchar\n", n);
                for (size_t k = i; k < i + n; ++k)
                    cmap += std::format("<{}> <{}>\n", Hex16(entries[k].first), Utf16Hex(entries[k].second));
                cmap += "endbfchar\n";
            }
            cmap += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";

            auto type0 = pdf.makeIndirectObject(Dict(std::format("/Type /Font /Subtype /Type0 /BaseFont /{} /Encoding /Identity-H", name)));
            auto descendants = QPDFObjectHandle::newArray();
            descendants.appendItem(cid);
            type0.replaceKey("/DescendantFonts", descendants);
            type0.replaceKey("/ToUnicode", pdf.makeIndirectObject(pdf.newStream(cmap)));
            return type0;
        }

        std::string PathOps(const MarkPath& path)
        {
            std::string ops;
            PointD at, start;
            for (const auto& s : path.steps)
            {
                switch (s.op)
                {
                case MarkPath::Op::Move:
                    ops += Pt(s.p[0]) + " m\n";
                    at = start = s.p[0];
                    break;
                case MarkPath::Op::Line:
                    ops += Pt(s.p[0]) + " l\n";
                    at = s.p[0];
                    break;
                case MarkPath::Op::Quad:
                {
                    // As the cubic with the same curve.
                    const PointD c1{ at.x + 2.0 / 3 * (s.p[0].x - at.x), at.y + 2.0 / 3 * (s.p[0].y - at.y) };
                    const PointD c2{ s.p[1].x + 2.0 / 3 * (s.p[0].x - s.p[1].x), s.p[1].y + 2.0 / 3 * (s.p[0].y - s.p[1].y) };
                    ops += Pt(c1) + " " + Pt(c2) + " " + Pt(s.p[1]) + " c\n";
                    at = s.p[1];
                    break;
                }
                case MarkPath::Op::Cubic:
                    ops += Pt(s.p[0]) + " " + Pt(s.p[1]) + " " + Pt(s.p[2]) + " c\n";
                    at = s.p[2];
                    break;
                case MarkPath::Op::Close:
                    ops += "h\n";
                    at = start;
                    break;
                }
            }
            return ops;
        }

        // A run as text: one TJ when its glyphs follow each other along the line, else each glyph
        // placed on its own (right-to-left runs, marks moved off their place).
        std::string GlyphOps(const MarkGlyphs& run, const std::string& font, const FontSubset& subset)
        {
            const size_t n = run.glyphs.size();
            const double s = std::max(0.01, run.size);
            const double skew = run.oblique ? s * 0.33 : 0;   // as DirectWrite slants a face it has no italic of
            std::string ops = "BT\n/" + font + " 1 Tf\n" + Rgb(run.rgb) + " rg\n";
            if (run.bold)
                ops += Rgb(run.rgb) + " RG " + Num(s / 36) + " w 2 Tr\n";
            const auto code = [&](size_t i) {
                const auto it = subset.remap.find(run.glyphs[i]);
                return Hex16(it == subset.remap.end() ? 0 : it->second);
            };
            const auto advance = [&](size_t i) { return i < run.advances.size() ? run.advances[i] : 0.0; };
            const auto offset = [&](size_t i) { return i < run.offsets.size() ? run.offsets[i] : PointD{}; };
            bool plain = !run.rtl;
            for (size_t i = 0; i < n && plain; ++i)
                plain = std::abs(offset(i).x) < 1e-6 && std::abs(offset(i).y) < 1e-6;
            if (plain)
            {
                ops += std::format("{} 0 {} {} {} Tm\n[", Num(s), Num(skew), Num(-s), Pt(run.origin));
                for (size_t i = 0; i < n; ++i)
                {
                    ops += "<" + code(i) + ">";
                    const auto it = subset.remap.find(run.glyphs[i]);
                    const double pdfWidth = it == subset.remap.end() ? 0 : std::round(subset.ToPdf(subset.advances[it->second]) * 10) / 10;   // as /W has it
                    const double adjust = pdfWidth - advance(i) * 1000 / s;
                    if (i + 1 < n && std::abs(adjust) > 0.05)
                        ops += " " + Num(adjust) + " ";
                }
                ops += "] TJ\n";
            }
            else
            {
                double along = 0;
                for (size_t i = 0; i < n; ++i)
                {
                    const PointD o = offset(i);
                    PointD at;
                    if (run.rtl)
                    {
                        along += advance(i);
                        at = { run.origin.x - along - o.x, run.origin.y - o.y };
                    }
                    else
                    {
                        at = { run.origin.x + along + o.x, run.origin.y - o.y };
                        along += advance(i);
                    }
                    ops += std::format("{} 0 {} {} {} Tm <{}> Tj\n", Num(s), Num(skew), Num(-s), Pt(at), code(i));
                }
            }
            return ops + "ET\n";
        }
    }

    void MarkPath::Rect(const RectD& r)
    {
        MoveTo({ r.x, r.y });
        LineTo({ r.Right(), r.y });
        LineTo({ r.Right(), r.Bottom() });
        LineTo({ r.x, r.Bottom() });
        Close();
    }

    void MarkPath::Ellipse(PointD c, double rx, double ry)
    {
        const double kx = rx * kArc, ky = ry * kArc;
        MoveTo({ c.x + rx, c.y });
        CubicTo({ c.x + rx, c.y + ky }, { c.x + kx, c.y + ry }, { c.x, c.y + ry });
        CubicTo({ c.x - kx, c.y + ry }, { c.x - rx, c.y + ky }, { c.x - rx, c.y });
        CubicTo({ c.x - rx, c.y - ky }, { c.x - kx, c.y - ry }, { c.x, c.y - ry });
        CubicTo({ c.x + kx, c.y - ry }, { c.x + rx, c.y - ky }, { c.x + rx, c.y });
        Close();
    }

    void MarkPath::RoundedRect(const RectD& r, double radius)
    {
        const double q = std::clamp(radius, 0.0, std::min(r.w, r.h) / 2), k = q * kArc;
        if (q <= 0)
        {
            Rect(r);
            return;
        }
        const double x0 = r.x, y0 = r.y, x1 = r.Right(), y1 = r.Bottom();
        MoveTo({ x0 + q, y0 });
        LineTo({ x1 - q, y0 });
        CubicTo({ x1 - q + k, y0 }, { x1, y0 + q - k }, { x1, y0 + q });
        LineTo({ x1, y1 - q });
        CubicTo({ x1, y1 - q + k }, { x1 - q + k, y1 }, { x1 - q, y1 });
        LineTo({ x0 + q, y1 });
        CubicTo({ x0 + q - k, y1 }, { x0, y1 - q + k }, { x0, y1 - q });
        LineTo({ x0, y0 + q });
        CubicTo({ x0, y0 + q - k }, { x0 + q - k, y0 }, { x0 + q, y0 });
        Close();
    }

    void PictureToPage(const double crop[4], int rotate, double width, double height, double out[6])
    {
        const double x0 = std::min(crop[0], crop[2]), x1 = std::max(crop[0], crop[2]);
        const double y0 = std::min(crop[1], crop[3]), y1 = std::max(crop[1], crop[3]);
        const bool turned = rotate == 90 || rotate == 270;
        // Points per picture pixel across and down the page as shown.
        const double sx = (turned ? y1 - y0 : x1 - x0) / std::max(1.0, width);
        const double sy = (turned ? x1 - x0 : y1 - y0) / std::max(1.0, height);
        const double m[4][6] = {
            { sx, 0, 0, -sy, x0, y1 },    // 0: x right, y down from the top
            { 0, sx, sy, 0, x0, y0 },     // 90: the left edge shows on top
            { -sx, 0, 0, sy, x1, y0 },    // 180
            { 0, -sx, -sy, 0, x1, y1 },   // 270
        };
        std::copy(std::begin(m[(rotate / 90) & 3]), std::end(m[(rotate / 90) & 3]), out);
    }

    bool WriteMarkedPage(const std::wstring& source, uint32_t page, const PageOverlay& overlay, const std::wstring& out, std::wstring& error,
                         MarkedPageInfo* info)
    {
        try
        {
            auto from = OpenPdf(source, error);
            if (!from)
                return false;
            auto pages = QPDFPageDocumentHelper(*from).getAllPages();
            if (page >= pages.size())
            {
                error = L"the page is not in the file";
                return false;
            }
            QPDF pdf;
            pdf.emptyPDF();
            QPDFPageDocumentHelper tree(pdf);
            tree.addPage(pages[page], false);   // copied in, resources and all
            QPDFPageObjectHelper target(pdf.getAllPages().back());

            // Where the picture's pixels are on the page.
            double crop[4] = { 0, 0, 612, 792 };
            const auto box = target.getCropBox(false, false);
            if (box.isArray() && box.getArrayNItems() == 4)
                for (int i = 0; i < 4; ++i)
                    if (box.getArrayItem(i).isNumber())
                        crop[i] = box.getArrayItem(i).getNumericValue();
            const auto media = target.getMediaBox(false);
            if (media.isArray() && media.getArrayNItems() == 4)
            {
                // A crop box is clipped to the media box, as readers show it.
                double m[4]{};
                for (int i = 0; i < 4; ++i)
                    m[i] = media.getArrayItem(i).isNumber() ? media.getArrayItem(i).getNumericValue() : crop[i];
                const double l = std::max(std::min(crop[0], crop[2]), std::min(m[0], m[2])), r = std::min(std::max(crop[0], crop[2]), std::max(m[0], m[2]));
                const double b = std::max(std::min(crop[1], crop[3]), std::min(m[1], m[3])), t = std::min(std::max(crop[1], crop[3]), std::max(m[1], m[3]));
                if (r > l && t > b)
                {
                    crop[0] = l;
                    crop[1] = b;
                    crop[2] = r;
                    crop[3] = t;
                }
            }
            double cm[6];
            PictureToPage(crop, RotationOf(target), overlay.width, overlay.height, cm);

            MarkedPageInfo stats;
            // The fonts: each one's glyphs, subset once.
            std::vector<std::vector<uint16_t>> used(overlay.fonts.size());
            std::vector<std::map<uint16_t, std::wstring>> unicode(overlay.fonts.size());
            for (const auto& item : overlay.items)
                if (item.kind == MarkItem::Kind::Glyphs && item.glyphs.font < overlay.fonts.size())
                    used[item.glyphs.font].insert(used[item.glyphs.font].end(), item.glyphs.glyphs.begin(), item.glyphs.glyphs.end());
            std::vector<FontSubset> subsets(overlay.fonts.size());
            std::map<size_t, std::string> fontNames;
            for (size_t f = 0; f < overlay.fonts.size(); ++f)
            {
                if (used[f].empty())
                    continue;
                std::wstring why;
                if (!SubsetTrueType(overlay.fonts[f].file, overlay.fonts[f].index, used[f], subsets[f], why))
                {
                    error = L"a font could not be embedded: " + why;
                    return false;
                }
                fontNames[f] = std::format("SDFillF{}", f + 1);
            }
            for (const auto& item : overlay.items)
                if (item.kind == MarkItem::Kind::Glyphs && fontNames.contains(item.glyphs.font))
                {
                    const auto& run = item.glyphs;
                    const auto& subset = subsets[run.font];
                    for (size_t i = 0; i < run.glyphs.size() && i < run.text.size(); ++i)
                        if (const auto it = subset.remap.find(run.glyphs[i]); it != subset.remap.end() && !run.text[i].empty())
                            unicode[run.font].emplace(it->second, run.text[i]);   // the first use of a glyph names it
                }

            auto resources = target.getAttribute("/Resources", true);
            if (!resources.isDictionary())
            {
                resources = QPDFObjectHandle::newDictionary();
                target.getObjectHandle().replaceKey("/Resources", resources);
            }
            const auto section = [&](const char* key) {
                auto d = resources.getKey(key);
                if (!d.isDictionary())
                {
                    d = QPDFObjectHandle::newDictionary();
                    resources.replaceKey(key, d);
                }
                return d;
            };
            for (const auto& [f, name] : fontNames)
            {
                section("/Font").replaceKey("/" + name, FontObject(pdf, overlay.fonts[f], subsets[f], unicode[f]));
                ++stats.fonts;
            }

            std::string ops = std::format("q {} {} {} {} {} {} cm\n", Num(cm[0]), Num(cm[1]), Num(cm[2]), Num(cm[3]), Num(cm[4]), Num(cm[5]));
            std::map<const canvas::Raster*, std::string> imageNames;
            std::map<int, std::string> alphaNames;
            for (const auto& item : overlay.items)
            {
                std::string body;
                switch (item.kind)
                {
                case MarkItem::Kind::Path:
                {
                    const auto& p = item.path;
                    if (p.steps.empty() || (!p.fill && !p.stroke))
                        continue;
                    if (p.alpha < 0.999f)
                    {
                        const int key = static_cast<int>(std::lround(p.alpha * 1000));
                        auto& name = alphaNames[key];
                        if (name.empty())
                        {
                            name = std::format("SDFillGs{}", alphaNames.size());
                            section("/ExtGState").replaceKey("/" + name, Dict(std::format("/Type /ExtGState /ca {} /CA {}", Num(p.alpha), Num(p.alpha))));
                        }
                        body += "/" + name + " gs\n";
                    }
                    if (p.fill)
                        body += Rgb(p.rgb) + " rg\n";
                    if (p.stroke)
                        body += std::format("{} RG {} w {}\n", Rgb(p.rgb), Num(p.width), p.round ? "1 J 1 j" : "0 J 0 j");
                    body += PathOps(p) + (p.fill && p.stroke ? "B\n" : p.fill ? "f\n" : "S\n");
                    ++stats.paths;
                    break;
                }
                case MarkItem::Kind::Glyphs:
                {
                    const auto it = fontNames.find(item.glyphs.font);
                    if (it == fontNames.end() || item.glyphs.glyphs.empty())
                        continue;
                    body += GlyphOps(item.glyphs, it->second, subsets[item.glyphs.font]);
                    ++stats.textRuns;
                    break;
                }
                case MarkItem::Kind::Image:
                {
                    const auto& r = item.image.raster;
                    if (!r || r->Empty() || item.image.at.w <= 0 || item.image.at.h <= 0)
                        continue;
                    auto& name = imageNames[r.get()];
                    if (name.empty())
                    {
                        name = std::format("SDFillIm{}", imageNames.size());
                        const EncodedImage encoded = EncodeImage(r->px.data(), r->width, r->height);
                        auto image = Stream(pdf, Deflate(encoded.samples), encoded.Dict() + " /Filter /FlateDecode");
                        if (!encoded.mask.empty())
                            image.getDict().replaceKey("/SMask", pdf.makeIndirectObject(Stream(pdf, Deflate(encoded.mask), encoded.MaskDict() + " /Filter /FlateDecode")));
                        section("/XObject").replaceKey("/" + name, pdf.makeIndirectObject(image));
                        ++stats.images;
                    }
                    const auto& at = item.image.at;
                    // The image's top row at the top of `at`: y runs down here.
                    body += std::format("{} 0 0 {} {} {} cm /{} Do\n", Num(at.w), Num(-at.h), Num(at.x), Num(at.Bottom()), name);
                    break;
                }
                }
                std::string clip;
                if (!item.clip.Empty())
                    clip = std::format("{} {} {} {} re W n\n", Num(item.clip.x), Num(item.clip.y), Num(item.clip.w), Num(item.clip.h));
                ops += "q\n" + clip + body + "Q\n";
            }
            ops += "Q\n";

            // q ... Q around the page's own content, so whatever state it leaves doesn't move the marks.
            target.addPageContents(pdf.newStream("q\n"), true);
            target.addPageContents(pdf.newStream("Q\n" + ops), false);
            if (!WritePdf(pdf, out, WriteOptions{}, error))
                return false;
            if (info)
                *info = stats;
            return true;
        }
        catch (const std::exception& e)
        {
            error = QpdfError(e);
            return false;
        }
    }
}
