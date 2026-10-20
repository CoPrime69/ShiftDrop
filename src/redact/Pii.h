#pragma once
#include "canvas/Geometry.h"

#include <string>
#include <string_view>
#include <vector>

// Rule-based detection of personal details in text read from a picture (Windows OCR): email
// addresses, phone numbers, payment-card numbers (Luhn-checked) and IBANs (mod-97-checked). No
// models: regular expressions and checksums only. Each hit becomes a proposed cover the user reviews.
namespace sd::redact
{
    struct OcrWord
    {
        std::wstring text;
        canvas::RectD box;   // image pixels
    };

    struct OcrLine
    {
        std::vector<OcrWord> words;
    };

    enum class PiiKind { Email, Phone, Card, Iban };
    const wchar_t* PiiLabel(PiiKind kind);   // "Email", "Phone", "Card number", "IBAN"

    struct PiiHit
    {
        PiiKind kind;
        std::wstring text;
        canvas::RectD box;   // the union of the words it spans
    };

    // Checksums, exposed for tests.
    bool LuhnValid(std::wstring_view digits);
    bool IbanValid(std::wstring_view iban);   // spaces allowed

    // What a line of text contains, as character ranges.
    struct PiiMatch
    {
        PiiKind kind;
        size_t begin, end;
    };
    std::vector<PiiMatch> FindPiiInText(std::wstring_view text);

    // Runs FindPiiInText on each line (its words joined by spaces) and maps every match back to the
    // boxes of the words it touches.
    std::vector<PiiHit> FindPii(const std::vector<OcrLine>& lines);
}
