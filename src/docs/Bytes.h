#pragma once
#include <string>
#include <string_view>

// Small helpers shared by the document, spreadsheet and subtitle converters: whole-file I/O,
// text decoding (BOMs, UTF-16, the ANSI code page) and XML escaping.
namespace sd::docs
{
    // Reads a whole file. Opened with full sharing, so a document open in Word can still be read.
    bool ReadFileBytes(const std::wstring& path, std::string& bytes, std::wstring* error = nullptr);
    // Writes `bytes` to `path`, replacing its contents. Only used on staging files we created.
    bool WriteFileBytes(const std::wstring& path, std::string_view bytes, std::wstring* error = nullptr);

    enum class TextEncoding
    {
        Utf8,
        Utf8Bom,
        Utf16Le,
        Utf16Be,
        Ansi,   // not valid UTF-8 and no BOM: the PC's ANSI code page (Windows-1252 in the West)
    };
    // Detects the encoding of a text file's bytes: a BOM wins, then UTF-16 without a BOM (many NULs
    // in alternate bytes), then valid UTF-8, else ANSI.
    TextEncoding DetectEncoding(std::string_view bytes);
    // Decodes to UTF-8 (without a BOM), using DetectEncoding.
    std::string DecodeText(std::string_view bytes, TextEncoding* detected = nullptr);
    bool IsValidUtf8(std::string_view s);

    // Line endings to "\n" (CRLF and lone CR).
    std::string NormalizeNewlines(std::string_view s);

    // XML text and attribute escaping (& < > " '), and dropping characters XML 1.0 forbids.
    std::string XmlEscape(std::string_view s);

    // Decodes one HTML character reference body ("amp", "#233", "#x1F600") to UTF-8, or empty if unknown.
    std::string DecodeEntity(std::string_view name);
    // Every character reference in `s` decoded ("&amp;" -> "&"); an "&" that starts no known
    // reference stays as written.
    std::string DecodeEntities(std::string_view s);
    // Appends a code point as UTF-8.
    void AppendUtf8(std::string& out, char32_t cp);

    // Lower-case ASCII copy.
    std::string AsciiLower(std::string_view s);
    // Trims ASCII whitespace at both ends.
    std::string_view Trim(std::string_view s);

    // Is this a monospace font family name (Consolas, Courier New, Menlo...)? Marks code.
    bool IsMonoFontName(std::string_view face);

    // A new random UUID, as "urn:uuid:..." text for EPUB identifiers.
    std::string NewUuid();
    // Current UTC time as "2026-10-20T18:30:00Z".
    std::string UtcNowIso();
}
