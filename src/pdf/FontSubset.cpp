#include "pdf/FontSubset.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <set>

namespace sd::pdf
{
    namespace
    {
        // Big-endian reads that never go past the end (0 there instead).
        struct Reader
        {
            std::string_view data;
            bool Has(size_t at, size_t n) const { return at <= data.size() && n <= data.size() - at; }
            uint16_t U16(size_t at) const
            {
                return Has(at, 2) ? static_cast<uint16_t>((uint8_t(data[at]) << 8) | uint8_t(data[at + 1])) : 0;
            }
            int16_t I16(size_t at) const { return static_cast<int16_t>(U16(at)); }
            uint32_t U32(size_t at) const { return (uint32_t(U16(at)) << 16) | U16(at + 2); }
        };

        void Put16(std::string& s, uint16_t v)
        {
            s.push_back(char(v >> 8));
            s.push_back(char(v & 0xFF));
        }
        void Put32(std::string& s, uint32_t v)
        {
            Put16(s, uint16_t(v >> 16));
            Put16(s, uint16_t(v & 0xFFFF));
        }
        void Set16(std::string& s, size_t at, uint16_t v)
        {
            if (at + 2 <= s.size())
            {
                s[at] = char(v >> 8);
                s[at + 1] = char(v & 0xFF);
            }
        }
        void Set32(std::string& s, size_t at, uint32_t v)
        {
            Set16(s, at, uint16_t(v >> 16));
            Set16(s, at + 2, uint16_t(v & 0xFFFF));
        }

        constexpr uint32_t Tag(const char (&t)[5]) { return (uint32_t(uint8_t(t[0])) << 24) | (uint32_t(uint8_t(t[1])) << 16) | (uint32_t(uint8_t(t[2])) << 8) | uint8_t(t[3]); }

        struct Font
        {
            Reader r;
            bool cff = false;
            std::map<uint32_t, std::string_view> tables;

            std::string_view Table(uint32_t tag) const
            {
                const auto it = tables.find(tag);
                return it == tables.end() ? std::string_view{} : it->second;
            }
        };

        // The table directory of face `index`.
        bool Open(std::string_view file, uint32_t index, Font& font)
        {
            font.r = { file };
            const Reader& r = font.r;
            size_t dir = 0;
            if (r.U32(0) == Tag("ttcf"))
            {
                if (index >= r.U32(8))
                    return false;
                dir = r.U32(12 + 4 * size_t(index));
            }
            const uint32_t version = r.U32(dir);
            if (version == Tag("OTTO"))
                font.cff = true;
            else if (version != 0x00010000 && version != Tag("true"))
                return false;
            const uint16_t n = r.U16(dir + 4);
            if (!r.Has(dir + 12, size_t(n) * 16))
                return false;
            for (uint16_t i = 0; i < n; ++i)
            {
                const size_t rec = dir + 12 + size_t(i) * 16;
                const uint32_t offset = r.U32(rec + 8), length = r.U32(rec + 12);
                if (r.Has(offset, length))
                    font.tables[r.U32(rec)] = file.substr(offset, length);
            }
            return !font.tables.empty();
        }

        uint32_t Checksum(std::string_view t)
        {
            uint32_t sum = 0;
            for (size_t i = 0; i < t.size(); i += 4)
            {
                uint32_t v = 0;
                for (size_t k = 0; k < 4; ++k)
                    v = (v << 8) | (i + k < t.size() ? uint8_t(t[i + k]) : 0);
                sum += v;
            }
            return sum;
        }

        // A TrueType file from its tables: the directory (sorted by tag, as it must be), every
        // table's checksum and head's checkSumAdjustment.
        std::string Sfnt(std::map<uint32_t, std::string> tables)
        {
            const uint16_t count = static_cast<uint16_t>(tables.size());
            uint16_t power = 1, selector = 0;
            while (power * 2 <= count)
            {
                power *= 2;
                ++selector;
            }
            std::string sfnt;
            Put32(sfnt, 0x00010000);
            Put16(sfnt, count);
            Put16(sfnt, uint16_t(power * 16));
            Put16(sfnt, selector);
            Put16(sfnt, uint16_t(count * 16 - power * 16));
            size_t offset = 12 + size_t(count) * 16, headAt = 0;
            std::string body;
            for (const auto& [tag, data] : tables)
            {
                Put32(sfnt, tag);
                Put32(sfnt, Checksum(data));
                Put32(sfnt, uint32_t(offset + body.size()));
                Put32(sfnt, uint32_t(data.size()));
                if (tag == Tag("head"))
                    headAt = offset + body.size();
                body += data;
                while (body.size() % 4)
                    body.push_back('\0');
            }
            std::string out = sfnt + body;
            if (headAt)
                Set32(out, headAt + 8, 0xB1B0AFBAu - Checksum(out));
            return out;
        }

        // A cmap that maps nothing (a PDF picks the glyphs by number): one format 4 subtable for
        // Windows Unicode holding only the closing segment.
        std::string EmptyCmap()
        {
            std::string cmap;
            for (const uint16_t v : { 0, 1, 3, 1 })
                Put16(cmap, v);
            Put32(cmap, 12);
            for (const uint16_t v : { 4, 24, 0, 2, 2, 0, 0, 0xFFFF, 0, 0xFFFF, 1, 0 })
                Put16(cmap, uint16_t(v));
            return cmap;
        }

        // The PostScript name (name ID 6), if the font has one.
        std::string PostScriptName(std::string_view table)
        {
            const Reader r{ table };
            const uint16_t count = r.U16(2), strings = r.U16(4);
            std::string mac;
            for (uint16_t i = 0; i < count; ++i)
            {
                const size_t rec = 6 + size_t(i) * 12;
                const uint16_t platform = r.U16(rec), id = r.U16(rec + 6), length = r.U16(rec + 8), offset = r.U16(rec + 10);
                if (id != 6 || !r.Has(size_t(strings) + offset, length))
                    continue;
                const auto bytes = table.substr(size_t(strings) + offset, length);
                if (platform == 3 || platform == 0)
                {
                    std::string out;
                    for (size_t k = 0; k + 1 < bytes.size(); k += 2)
                        if (bytes[k] == 0)
                            out.push_back(bytes[k + 1]);
                    return out;
                }
                if (platform == 1)
                    mac.assign(bytes);
            }
            return mac;
        }
    }

    FontUse CheckFont(std::string_view file, uint32_t index, int weight, bool italic)
    {
        Font font;
        if (!Open(file, index, font))
            return FontUse::Broken;
        if (font.cff || font.Table(Tag("glyf")).empty() || font.Table(Tag("loca")).empty())
            return FontUse::NoOutlines;
        const Reader os2{ font.Table(Tag("OS/2")) };
        const uint16_t fsType = os2.U16(8);
        if ((fsType & 0x000F) == 0x0002 || (fsType & 0x0200))
            return FontUse::Restricted;
        // A variable font's 'glyf' holds its default instance; another weight or slant is made by
        // the renderer, which a PDF reader can't do.
        const auto fvar = font.Table(Tag("fvar"));
        if (!fvar.empty())
        {
            const Reader v{ fvar };
            const uint16_t axes = v.U16(4), count = v.U16(8), size = v.U16(10);
            for (uint16_t i = 0; i < count; ++i)
            {
                const size_t rec = size_t(axes) + size_t(i) * size;
                const uint32_t tag = v.U32(rec);
                const double value = int32_t(v.U32(rec + 8)) / 65536.0;
                if (tag == Tag("wght") && std::abs(value - weight) > 1)
                    return FontUse::Instance;
                if ((tag == Tag("ital") || tag == Tag("slnt")) && italic && std::abs(value) < 0.01)
                    return FontUse::Instance;
            }
        }
        return FontUse::Embed;
    }

    bool SubsetTrueType(std::string_view file, uint32_t index, const std::vector<uint16_t>& used, FontSubset& out, std::wstring& error)
    {
        out = {};
        Font font;
        if (!Open(file, index, font) || CheckFont(file, index, 400, false) == FontUse::Broken)
        {
            error = L"the font file could not be read";
            return false;
        }
        const Reader head{ font.Table(Tag("head")) }, hhea{ font.Table(Tag("hhea")) }, maxp{ font.Table(Tag("maxp")) };
        const Reader loca{ font.Table(Tag("loca")) }, hmtx{ font.Table(Tag("hmtx")) };
        const auto glyf = font.Table(Tag("glyf"));
        if (font.cff || glyf.empty() || head.data.size() < 54 || hhea.data.size() < 36 || maxp.data.size() < 6)
        {
            error = L"the font has no TrueType outlines";
            return false;
        }
        const uint16_t numGlyphs = maxp.U16(4), metrics = std::max<uint16_t>(1, hhea.U16(34));
        const bool longLoca = head.I16(50) == 1;
        const auto glyphRange = [&](uint16_t g, size_t& from, size_t& to) {
            if (g >= numGlyphs)
                return false;
            from = longLoca ? loca.U32(size_t(g) * 4) : size_t(loca.U16(size_t(g) * 2)) * 2;
            to = longLoca ? loca.U32(size_t(g + 1) * 4) : size_t(loca.U16(size_t(g + 1) * 2)) * 2;
            return from <= to && to <= glyf.size();
        };

        // The glyphs used, and the ones composite glyphs are made of.
        std::set<uint16_t> keep{ 0 };
        std::deque<uint16_t> queue(used.begin(), used.end());
        while (!queue.empty())
        {
            const uint16_t g = queue.front();
            queue.pop_front();
            if (g >= numGlyphs || !keep.insert(g).second)
                continue;
            size_t from = 0, to = 0;
            if (!glyphRange(g, from, to) || to - from < 10)
                continue;
            const Reader gr{ glyf.substr(from, to - from) };
            if (gr.I16(0) >= 0)
                continue;
            size_t at = 10;
            for (uint16_t flags = 0x20; (flags & 0x20) && gr.Has(at, 4);)
            {
                flags = gr.U16(at);
                queue.push_back(gr.U16(at + 2));
                at += 4 + ((flags & 1) ? 4 : 2) + ((flags & 8) ? 2 : (flags & 0x40) ? 4 : (flags & 0x80) ? 8 : 0);
            }
        }
        out.glyphs.assign(keep.begin(), keep.end());
        for (size_t i = 0; i < out.glyphs.size(); ++i)
            out.remap[out.glyphs[i]] = static_cast<uint16_t>(i);
        const uint16_t n = static_cast<uint16_t>(out.glyphs.size());

        // glyf and loca (long offsets), with composite glyphs pointing at the new numbers.
        std::string newGlyf, newLoca;
        for (const uint16_t g : out.glyphs)
        {
            Put32(newLoca, uint32_t(newGlyf.size()));
            size_t from = 0, to = 0;
            if (!glyphRange(g, from, to))
                continue;
            std::string bytes(glyf.substr(from, to - from));
            const Reader gr{ bytes };
            if (bytes.size() >= 10 && gr.I16(0) < 0)
            {
                size_t at = 10;
                for (uint16_t flags = 0x20; (flags & 0x20) && gr.Has(at, 4);)
                {
                    flags = gr.U16(at);
                    const auto it = out.remap.find(gr.U16(at + 2));
                    Set16(bytes, at + 2, it == out.remap.end() ? 0 : it->second);
                    at += 4 + ((flags & 1) ? 4 : 2) + ((flags & 8) ? 2 : (flags & 0x40) ? 4 : (flags & 0x80) ? 8 : 0);
                }
            }
            newGlyf += bytes;
            while (newGlyf.size() % 4)
                newGlyf.push_back('\0');
        }
        Put32(newLoca, uint32_t(newGlyf.size()));

        // Metrics for every new glyph.
        std::string newHmtx;
        for (const uint16_t g : out.glyphs)
        {
            const uint16_t advance = hmtx.U16(size_t(std::min<uint16_t>(g, uint16_t(metrics - 1))) * 4);
            const int16_t lsb = g < metrics ? hmtx.I16(size_t(g) * 4 + 2) : hmtx.I16(size_t(metrics) * 4 + size_t(g - metrics) * 2);
            Put16(newHmtx, advance);
            Put16(newHmtx, uint16_t(lsb));
            out.advances.push_back(advance);
        }

        std::string newHead(head.data), newHhea(hhea.data), newMaxp(maxp.data);
        Set32(newHead, 8, 0);   // checkSumAdjustment, set below
        Set16(newHead, 50, 1);  // long loca
        Set16(newHhea, 34, n);
        Set16(newMaxp, 4, n);

        // A cmap that maps nothing (the PDF picks glyphs by number) and a post table without names.
        const std::string cmap = EmptyCmap();
        std::string post(font.Table(Tag("post")).substr(0, 32));
        post.resize(32, '\0');
        Set32(post, 0, 0x00030000);

        std::map<uint32_t, std::string> tables = {
            { Tag("cmap"), cmap }, { Tag("glyf"), newGlyf }, { Tag("head"), newHead }, { Tag("hhea"), newHhea },
            { Tag("hmtx"), newHmtx }, { Tag("loca"), newLoca }, { Tag("maxp"), newMaxp }, { Tag("post"), post },
        };
        // Kept as they are: the hinting programs and the names and style the font describes itself with.
        for (const uint32_t tag : { Tag("OS/2"), Tag("cvt "), Tag("fpgm"), Tag("prep"), Tag("name"), Tag("gasp") })
            if (const auto t = font.Table(tag); !t.empty())
                tables[tag] = std::string(t);

        out.data = Sfnt(std::move(tables));

        out.unitsPerEm = std::max<uint16_t>(16, head.U16(18));
        out.xMin = head.I16(36);
        out.yMin = head.I16(38);
        out.xMax = head.I16(40);
        out.yMax = head.I16(42);
        out.ascent = hhea.I16(4);
        out.descent = hhea.I16(6);
        const Reader os2{ font.Table(Tag("OS/2")) };
        out.capHeight = os2.U16(0) >= 2 && os2.Has(88, 2) ? os2.I16(88) : int(out.ascent * 0.7);
        const Reader postIn{ font.Table(Tag("post")) };
        out.italicAngle = int32_t(postIn.U32(4)) / 65536.0;
        out.fixedPitch = postIn.U32(12) != 0;
        out.postScriptName = PostScriptName(font.Table(Tag("name")));
        return true;
    }

    std::string GlyphlessTrueType()
    {
        // Glyph 0 (.notdef) is a plain box so the font has outlines a checker can read; glyph 1 has
        // none. Both advance half an em; the em is 1000 units, one PDF text-space unit each.
        std::string glyf;
        for (const int16_t v : { 1, 50, 0, 450, 1000, 3, 0 })   // one contour, bbox, last point, no instructions
            Put16(glyf, uint16_t(v));
        for (int i = 0; i < 4; ++i)
            glyf.push_back('\x01');   // on-curve points, full 16-bit coordinate steps
        for (const int16_t v : { 50, 0, 400, 0, 0, 1000, 0, -1000 })   // x steps, then y steps: clockwise
            Put16(glyf, uint16_t(v));
        while (glyf.size() % 4)
            glyf.push_back('\0');
        std::string loca;
        for (const uint32_t v : { 0u, uint32_t(glyf.size()), uint32_t(glyf.size()) })
            Put32(loca, v);
        std::string hmtx;
        for (const uint16_t v : { 500, 50, 500, 0 })
            Put16(hmtx, v);

        std::string head;
        for (const uint32_t v : { 0x00010000u, 0x00010000u, 0u, 0x5F0F3CF5u })   // version, revision, adjustment, magic
            Put32(head, v);
        Put16(head, 0x000B);   // baseline and left sidebearing at 0, integer scaling
        Put16(head, 1000);     // unitsPerEm
        for (int i = 0; i < 2; ++i)
        {
            Put32(head, 0);    // created, then modified: 2026-01-01, in seconds since 1904
            Put32(head, 3850070400u);
        }
        for (const int16_t v : { 0, 0, 500, 1000, 0, 8, 2, 1, 0 })   // bbox, macStyle, lowestRecPPEM, direction, long loca, format
            Put16(head, uint16_t(v));

        std::string hhea;
        Put32(hhea, 0x00010000);
        for (const int16_t v : { 1000, 0, 0, 500, 50, 50, 450, 1, 0, 0, 0, 0, 0, 0, 0, 2 })
            Put16(hhea, uint16_t(v));   // ascender .. caret, reserved, metric format, numberOfHMetrics

        std::string maxp;
        Put32(maxp, 0x00010000);
        for (const uint16_t v : { 2, 4, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0 })
            Put16(maxp, v);   // numGlyphs, maxPoints, maxContours, ..., maxZones, the rest unused

        std::string os2;
        for (const int16_t v : { 4, 500, 400, 5, 0, 650, 700, 0, 140, 650, 700, 0, 480, 50, 300, 0 })
            Put16(os2, uint16_t(v));   // version 4, widths, weight, installable, sub/superscript, strikeout, class
        os2.append(10, '\0');          // PANOSE: any
        for (int i = 0; i < 4; ++i)
            Put32(os2, 0);             // Unicode ranges: none claimed
        os2 += "NONE";
        for (const uint16_t v : { 0x0040, 0x0020, 0x0020, 1000, 0, 0, 1000, 0 })
            Put16(os2, v);             // regular; first/last char; typo ascender, descender, gap; win ascent, descent
        Put32(os2, 1);                 // code pages: Latin 1
        Put32(os2, 0);
        for (const uint16_t v : { 500, 1000, 0, 0x0020, 0 })
            Put16(os2, v);             // x-height, cap height, default char, break char, context

        // name: family, style, unique id, full name, version, PostScript name (Windows, US English).
        const std::string_view names[] = { "SDGlyphless", "Regular", "SDGlyphless-1", "SDGlyphless", "Version 1.0", "SDGlyphless" };
        std::string name, strings;
        Put16(name, 0);
        Put16(name, uint16_t(std::size(names)));
        Put16(name, uint16_t(6 + std::size(names) * 12));
        for (size_t i = 0; i < std::size(names); ++i)
        {
            for (const uint16_t v : std::initializer_list<uint16_t>{ 3, 1, 0x0409, uint16_t(i + 1), uint16_t(names[i].size() * 2), uint16_t(strings.size()) })
                Put16(name, v);
            for (const char c : names[i])
                Put16(strings, uint16_t(uint8_t(c)));
        }
        name += strings;

        std::string post;
        Put32(post, 0x00030000);   // version 3: no glyph names
        Put32(post, 0);            // italic angle
        Put16(post, uint16_t(int16_t(-100)));
        Put16(post, 50);
        for (int i = 0; i < 5; ++i)
            Put32(post, 0);        // fixed pitch, memory hints

        return Sfnt({ { Tag("OS/2"), os2 }, { Tag("cmap"), EmptyCmap() }, { Tag("glyf"), glyf }, { Tag("head"), head }, { Tag("hhea"), hhea },
                      { Tag("hmtx"), hmtx }, { Tag("loca"), loca }, { Tag("maxp"), maxp }, { Tag("name"), name }, { Tag("post"), post } });
    }
}
