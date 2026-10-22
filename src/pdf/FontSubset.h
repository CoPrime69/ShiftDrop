#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// The part of a TrueType font a PDF needs to show some text: only the glyphs used (and the glyphs
// they are built from), numbered afresh from 0, in a small TrueType file for a CIDFontType2 with an
// Identity CIDToGIDMap. Fill and sign writes typed text into a page this way, so it stays sharp text
// that can be searched and copied.
namespace sd::pdf
{
    enum class FontUse
    {
        Embed,        // can go in as a subset
        Restricted,   // the font's licence doesn't allow embedding (OS/2 fsType), or only as bitmaps
        NoOutlines,   // CFF (PostScript) outlines or no 'glyf' table: not a TrueType subset
        Instance,     // a variable font drawn at another instance than the default the outlines hold
        Broken,       // not a font file this can read
    };

    // Whether face `index` of `file` (a .ttf, or a .ttc collection) can be embedded as a subset when
    // drawn at `weight` and, if `italic`, slanted.
    FontUse CheckFont(std::string_view file, uint32_t index, int weight, bool italic);

    struct FontSubset
    {
        std::string data;                    // the TrueType file
        std::vector<uint16_t> glyphs;        // the original glyph of each new one (new id = position)
        std::map<uint16_t, uint16_t> remap;  // original glyph -> new
        std::vector<uint16_t> advances;      // per new glyph, font units
        unsigned unitsPerEm = 1000;
        int ascent = 0, descent = 0, capHeight = 0;   // font units; descent is negative
        int xMin = 0, yMin = 0, xMax = 0, yMax = 0;
        double italicAngle = 0;
        bool fixedPitch = false;
        std::string postScriptName;          // from the 'name' table, or empty

        // Font units -> the 1000-unit glyph space PDF widths use.
        double ToPdf(double units) const { return units * 1000.0 / unitsPerEm; }
    };

    // Glyph 0 (.notdef) is always kept, so new ids start at 1 for the first used glyph.
    bool SubsetTrueType(std::string_view file, uint32_t index, const std::vector<uint16_t>& used, FontSubset& out, std::wstring& error);

    // A TrueType font with no visible glyphs, for text that is never painted (a searchable PDF's
    // invisible OCR layer): .notdef and one blank glyph, both 500 of 1000 units wide. Small (under
    // 1 KB) and complete, so PDF readers find the font they were told is embedded.
    std::string GlyphlessTrueType();
}
