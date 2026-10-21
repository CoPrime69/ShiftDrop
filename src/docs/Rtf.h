#pragma once
#include "docs/DocModel.h"

#include <string>
#include <string_view>
#include <vector>

// RTF input through Windows' own RichEdit control (msftedit.dll): RichEdit parses the RTF, then the
// text is read back with its character and paragraph formatting into the document model. Pictures,
// hyperlinks and RTFD attachments, which RichEdit would hide from us, are first swapped for text
// markers by RtfPrepare and turned back into images and links afterwards.
namespace sd::docs
{
    struct RtfPrepared
    {
        std::string rtf;                    // the RTF with markers in place of pictures and links
        std::vector<std::string> images;    // encoded PNG/JPEG bytes, by marker number
        std::vector<int> imageWidths;       // display size in pixels (0 = unknown)
        std::vector<int> imageHeights;
        std::vector<std::string> links;     // hyperlink targets, by marker number
    };

    // Marker texts: "@@SDIMG<n>@@" for a picture, "@@SDLNK<n>@@" ... "@@SDEND@@" around link text.
    // `rtfdFolder` (an RTFD package) resolves \NeXTGraphic attachments; empty for plain RTF.
    RtfPrepared RtfPrepare(std::string_view rtf, const std::wstring& rtfdFolder);

    // One run of uniformly formatted text as RichEdit reports it.
    struct RtfRun
    {
        std::wstring text;
        bool bold = false, italic = false, strike = false, mono = false;
        float sizePt = 0;
    };
    struct RtfParagraph
    {
        std::vector<RtfRun> runs;
        int numbering = 0;     // PARAFORMAT2 wNumbering: 0 none, 1 bullet, 2.. numbered
        int numberStart = 1;
        int indentTwips = 0;
    };
    // Builds the document from RichEdit's paragraphs: headings from font size (the RTF's styles do
    // not survive RichEdit), lists, code (monospace) and the markers from RtfPrepare. Pure, so the
    // heuristics are tested without RichEdit.
    void RtfBuildDocument(const std::vector<RtfParagraph>& paragraphs, RtfPrepared& prepared, Document& doc);

    // Reads RTF into the document. Creates a hidden RichEdit window on the calling thread (any worker
    // thread; the window never needs a message loop because every call is a direct SendMessage).
    bool ReadRtf(std::string_view rtf, const std::wstring& rtfdFolder, Document& doc, std::wstring& error);
    // An RTFD package: the folder holding TXT.rtf and its pictures.
    bool ReadRtfd(const std::wstring& folder, Document& doc, std::wstring& error);
}
