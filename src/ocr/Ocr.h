#pragma once
#include "pdf/Bitmap.h"

#include <string>
#include <string_view>
#include <vector>

// Text recognition through Windows' built-in OCR, Windows.Media.Ocr. It is
// part of Windows: nothing is bundled or downloaded, and it uses the OCR languages the user has
// installed. The WinRT code lives in WinOcr.cpp (built only by MSVC); everything here is plain C++ so
// the logic around it is unit-tested anywhere.
namespace sd::ocr
{
    struct Word
    {
        std::wstring text;
        double x = 0, y = 0, w = 0, h = 0;   // pixels of the recognised bitmap, top-left origin
    };

    struct Line
    {
        std::wstring text;          // the line as Windows joined it (spaces where the language uses them)
        std::vector<Word> words;
    };

    struct Result
    {
        std::vector<Line> lines;
        uint32_t width = 0, height = 0;   // the bitmap the boxes refer to
        std::wstring language;            // BCP-47 tag of the recogniser that ran, "en-US"
    };

    // An OCR language Windows has installed.
    struct Language
    {
        std::wstring tag;    // BCP-47, "en-US"
        std::wstring name;   // as Windows names it, "English (United States)"
    };

    // Installed by WinOcr.cpp at startup; all null where Windows OCR is not compiled in (MinGW tests).
    struct Hooks
    {
        // Is OCR usable on this PC? `why` explains a "no" (no OCR language installed...). Cheap, may be
        // called on the UI thread.
        bool (*available)(std::wstring* why) = nullptr;
        // Recognises text in `image` (any size; scaled down internally if Windows' limit is smaller,
        // boxes are given back in `image`'s pixels). Blocks: call it on a worker thread (MTA).
        bool (*recognize)(const pdf::Bitmap& image, Result& out, std::wstring& error) = nullptr;
        // The installed OCR languages, in Windows' order. Cheap, may be called on the UI thread.
        std::vector<Language> (*languages)() = nullptr;
    };
    Hooks& Platform();

    // Convenience wrappers that say why when OCR is not compiled in.
    std::vector<Language> Languages();
    bool Available(std::wstring* why);

    // Which language Recognize reads: a tag from Languages(), or empty for automatic (the user's own
    // languages first). Set from the tray menu; read on worker threads.
    void SetLanguage(const std::wstring& tag);
    std::wstring LanguageChoice();
    bool Recognize(const pdf::Bitmap& image, Result& out, std::wstring& error);

    // Words in a piece of text: runs of non-space characters that hold a letter or a digit.
    size_t CountWords(std::wstring_view text);
    // The top panel's line after Copy text: "Copied 214 words", "Copied 1 word from 3 files",
    // "No text found".
    std::wstring CopiedMessage(size_t words, size_t files);

    // Puts text on the clipboard as CF_UNICODETEXT. Works from any thread (OpenClipboard with no
    // owner window); retries briefly when another app holds the clipboard.
    bool SetClipboardText(const std::wstring& text, std::wstring& error);
}
