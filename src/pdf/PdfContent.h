#pragma once
#include "pdf/TextLayout.h"

#include <qpdf/QPDFObjGen.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <map>
#include <string>
#include <vector>

// Reading a page's content stream with QPDF's parser: the text it shows (with positions and sizes,
// for TXT and Markdown) and where it draws each image (for Compress PDF's resolution target).
//
// Text decoding covers what simple PDFs use: ToUnicode CMaps (any font), WinAnsi / MacRoman /
// Standard encodings with /Differences glyph names (simple fonts), widths from /Widths and /W.
// Fonts with neither a ToUnicode map nor a known encoding (some CID fonts) give no text, so the page
// counts as having none and OCR reads it instead.
namespace sd::pdf
{
    // The page's text in reading orientation: if most of it runs sideways (a landscape scan turned
    // with /Rotate, or rotated content), coordinates are turned so lines run left to right.
    PageLines ExtractPageLines(QPDFPageObjectHelper& page);

    // Largest size each image XObject is drawn at on this page, in points (width, height of the
    // unit square after the current transformation), keyed by the image's object id.
    struct Placement
    {
        double width = 0, height = 0;
    };
    void CollectImagePlacements(QPDFPageObjectHelper& page, std::map<QPDFObjGen, Placement>& out);

    // Exposed for tests: decodes a ToUnicode CMap (bfchar and bfrange) into code -> text.
    std::map<uint32_t, std::wstring> ParseToUnicode(std::string_view cmap, int& codeBytes);
    // Unicode for a glyph name ("A", "eacute", "uni00E9", "u1F600", "f_i", "a.sc"); empty if unknown.
    std::wstring GlyphNameToUnicode(std::string_view name);
}
