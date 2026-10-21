#include "docs/PdfSource.h"
#include "docs/Formats.h"
#include "Utf.h"
#include "pdf/PdfText.h"
#include "pdf/TextLayout.h"

namespace sd::docs
{
    bool ReadPdfDocument(const std::wstring& path, Document& doc, std::stop_token stop, std::wstring& error)
    {
        pdf::PdfText text;
        if (!pdf::ReadPdfText(path, true, stop, {}, text, error))
            return false;
        size_t chars = 0;
        for (const auto& page : text.pages)
            chars += pdf::VisibleChars(page);
        if (chars == 0)
        {
            error = text.ocrProblem.empty() ? L"no text found in this PDF" : text.ocrProblem;
            return false;
        }
        ReadMarkdown(Narrow(pdf::ToMarkdown(text.pages)), {}, doc);
        return true;
    }
}
