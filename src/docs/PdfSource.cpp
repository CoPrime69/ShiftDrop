#include "docs/PdfSource.h"
#include "docs/Formats.h"
#include "Utf.h"
#include "pdf/PdfText.h"
#include "pdf/TextLayout.h"

#include <algorithm>
#include <cmath>

namespace sd::docs
{
    namespace
    {
        // The PDF module's Markdown parts into `doc`; each picture part becomes a paragraph holding
        // that picture, taken out of `images` and sized from its box in `placed`.
        void AppendPdfParts(const std::vector<pdf::MarkdownPart>& parts, const std::vector<pdf::PagePicture>& placed,
                            std::vector<pdf::ImageFile>& images, Document& doc)
        {
            for (const auto& part : parts)
            {
                if (part.picture < 0)
                {
                    Document text;
                    ReadMarkdown(Narrow(part.markdown), {}, text);
                    // Pictures of the text itself (a data: URI written out in the PDF) get names in `doc`.
                    std::vector<int> moved;
                    for (auto& image : text.images)
                        moved.push_back(AddImage(doc, std::move(image.data)));
                    const auto shift = [&](std::vector<Span>& spans) {
                        for (auto& span : spans)
                            if (span.image >= 0)
                                span.image = static_cast<size_t>(span.image) < moved.size() ? moved[static_cast<size_t>(span.image)] : -1;
                    };
                    for (auto& block : text.blocks)
                    {
                        shift(block.spans);
                        for (auto& row : block.rows)
                            for (auto& cell : row)
                                shift(cell.spans);
                        doc.blocks.push_back(std::move(block));
                    }
                    continue;
                }
                const auto id = static_cast<size_t>(part.picture);
                if (id >= images.size() || images[id].bytes.empty())
                    continue;
                const int index = AddImage(doc, std::move(images[id].bytes));
                auto& image = doc.images[static_cast<size_t>(index)];
                // As wide as it was drawn on the page (96 pixels to 72 points), in the picture's own
                // shape, and no taller than the text area of an A4 or Letter page (9 inches); the
                // writers shrink it to the text width when it is wider.
                const pdf::PagePicture* box = nullptr;
                for (const auto& p : placed)
                    if (p.id == part.picture)
                        box = &p;
                const double pixelsW = images[id].width ? static_cast<double>(images[id].width) : static_cast<double>(image.width);
                const double pixelsH = images[id].height ? static_cast<double>(images[id].height) : static_cast<double>(image.height);
                if (box && pixelsW > 0 && pixelsH > 0)
                {
                    constexpr double kMaxHeight = 9 * 96.0;
                    double w = (box->right - box->left) * 96.0 / 72.0;
                    if (w * pixelsH / pixelsW > kMaxHeight)
                        w = kMaxHeight * pixelsW / pixelsH;
                    image.width = std::max(1, static_cast<int>(std::lround(w)));
                    image.height = std::max(1, static_cast<int>(std::lround(w * pixelsH / pixelsW)));
                }
                Block block;
                Span span;
                span.image = index;
                block.spans.push_back(std::move(span));
                doc.blocks.push_back(std::move(block));
            }
        }
    }

    bool ReadPdfDocument(const std::wstring& path, Document& doc, std::stop_token stop, std::wstring& error)
    {
        pdf::PdfText text;
        if (!pdf::ReadPdfText(path, true, stop, {}, text, error, true))
            return false;
        size_t chars = 0;
        for (const auto& page : text.pages)
            chars += pdf::VisibleChars(page);
        if (chars == 0 && text.images.empty())
        {
            error = text.ocrProblem.empty() ? L"no text found in this PDF" : text.ocrProblem;
            return false;
        }
        std::vector<pdf::PagePicture> placed;
        for (const auto& page : text.pictures)
            placed.insert(placed.end(), page.begin(), page.end());
        AppendPdfParts(pdf::ToMarkdownParts(text.pages, text.pictures), placed, text.images, doc);
        return true;
    }
}
