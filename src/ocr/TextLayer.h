#pragma once
#include "ocr/Ocr.h"

#include <functional>
#include <string>
#include <vector>

// The invisible text layer of a searchable PDF: every recognised word is drawn in text render mode 3
// (neither filled nor stroked, PDF 1.7 9.3.6) exactly over its picture, so viewers can search, select
// and copy it while the page still shows the original scan.
//
// The font is a Type0 font with Identity-H encoding whose character codes are UTF-16 code units and
// whose ToUnicode CMap maps every code to itself, so any language Windows OCR reads survives copy and
// paste. Its CIDFontType2 embeds a tiny TrueType program (pdf::GlyphlessTrueType) whose one blank
// glyph every code is drawn with, so readers such as Acrobat find a real font and open the file
// without complaint, though the text is never painted.
namespace sd::ocr
{
    struct PlacedWord
    {
        std::wstring text;
        double x = 0, y = 0;            // baseline start, PDF user space (points)
        double width = 0, height = 0;   // along and across the baseline (points)
        int rotation = 0;               // 0, 90, 180 or 270: text direction, counter-clockwise
        bool spaceAfter = false;        // another word follows on the same line
    };

    // Content-stream operators that draw the words with font resource `fontName` (without the slash).
    // Self-contained: BT ... ET, no graphics-state changes left behind.
    std::string TextLayerOps(const std::vector<PlacedWord>& words, std::string_view fontName);

    // The ToUnicode CMap (stream data) for the font: <0000>..<FFFF> map to themselves, surrogates excepted.
    std::string IdentityToUnicodeCMap();

    // The CIDToGIDMap (stream data): code 0 to glyph 0 (.notdef), every other code to glyph 1.
    std::string GlyphlessCidToGidMap();

    // Makes the font's objects with `make`, which writes one object and returns a reference to it
    // ("12 0 R"): a dictionary when `data` is null, else a stream whose data `make` deflates. `dict`
    // is the inside of the dictionary (no << >>, /Length or /Filter) and refers only to objects made
    // before it. The last object made is the Type0 font to put in a page's /Font resources.
    void MakeGlyphlessFont(const std::function<std::string(const std::string& dict, const std::string* data)>& make);

    // Hex for a Tj operand: UTF-16BE code units, characters outside the Basic Multilingual Plane
    // dropped (a lone surrogate cannot be mapped), plus a trailing space when asked.
    std::string HexCodes(std::wstring_view text, bool spaceAfter);

    // Recognised words of an image page drawn at (0,0)-(pageW,pageH): the bitmap's pixels scale to
    // the page and its top-left is the page's top-left.
    std::vector<PlacedWord> PlaceOnImagePage(const Result& ocr, double pageW, double pageH);

    // Recognised words of a rendered PDF page. `cropBox` is the page's visible box in user space
    // (x0, y0, x1, y1) and `rotate` its /Rotate (clockwise degrees, a multiple of 90); the bitmap is
    // that box as displayed, so this undoes the rotation.
    std::vector<PlacedWord> PlaceOnPdfPage(const Result& ocr, const double cropBox[4], int rotate);
}
