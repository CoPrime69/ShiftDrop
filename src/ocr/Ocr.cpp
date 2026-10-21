#include "ocr/Ocr.h"

#include <windows.h>

#include <cwctype>
#include <format>
#include <mutex>

namespace sd::ocr
{
    Hooks& Platform()
    {
        static Hooks hooks;   // filled in by WinOcr.cpp's static initialiser, before main()
        return hooks;
    }

    namespace
    {
        std::mutex g_languageLock;
        std::wstring g_language;   // empty: automatic
    }

    void SetLanguage(const std::wstring& tag)
    {
        std::lock_guard lock(g_languageLock);
        g_language = tag;
    }

    std::wstring LanguageChoice()
    {
        std::lock_guard lock(g_languageLock);
        return g_language;
    }

    std::vector<Language> Languages()
    {
        return Platform().languages ? Platform().languages() : std::vector<Language>{};
    }

    bool Available(std::wstring* why)
    {
        if (!Platform().available)
        {
            if (why)
                *why = L"Windows OCR is not available in this build";
            return false;
        }
        return Platform().available(why);
    }

    bool Recognize(const pdf::Bitmap& image, Result& out, std::wstring& error)
    {
        if (!Platform().recognize)
        {
            error = L"Windows OCR is not available in this build";
            return false;
        }
        if (image.Empty())
        {
            error = L"empty image";
            return false;
        }
        return Platform().recognize(image, out, error);
    }

    size_t CountWords(std::wstring_view text)
    {
        size_t words = 0;
        bool counted = false;
        for (const wchar_t c : text)
        {
            if (std::iswspace(c) || c == 0x3000)   // U+3000 ideographic space
            {
                counted = false;
                continue;
            }
            // IsCharAlphaNumericW knows every script's letters, unlike iswalnum in the "C" locale.
            if (!counted && IsCharAlphaNumericW(c))
            {
                ++words;
                counted = true;
            }
        }
        return words;
    }

    std::wstring CopiedMessage(size_t words, size_t files)
    {
        if (words == 0)
            return L"No text found";
        const std::wstring count = words == 1 ? L"1 word" : std::format(L"{} words", words);
        return files > 1 ? std::format(L"Copied {} from {} files", count, files) : std::format(L"Copied {}", count);
    }
}
