#include "ocr/OcrEngine.h"
#include "Clipboard.h"
#include "Log.h"
#include "ocr/Ocr.h"
#include "pdf/ImagePdf.h"
#include "pdf/PageRenderer.h"
#include "pdf/PdfContent.h"
#include "pdf/PdfDoc.h"
#include "pdf/PdfModule.h"
#include "pdf/PdfText.h"
#include "pdf/Wic.h"

#include <qpdf/QPDFPageDocumentHelper.hh>

#include <format>

namespace sd::ocr
{
    namespace
    {
        using namespace sd::pdf;

        const std::string& ActionOf(const Recipe& r) { return r.steps.front().action; }

        // Upright pixels of an image file, on white, recognised.
        bool RecognizeImage(const std::wstring& path, Result& result, std::wstring& error)
        {
            Bitmap bitmap;
            if (!DecodeImageFile(path, bitmap, error))
                return false;
            FlattenOnWhite(bitmap);
            return Recognize(bitmap, result, error);
        }

        class OcrEngine final : public IEngine
        {
        public:
            explicit OcrEngine(const EngineServices& services) : m_history(services.history) {}

            bool Handles(const Recipe& recipe) const override { return recipe.steps.size() == 1 && ActionOf(recipe).starts_with("ocr."); }

            bool Applies(const Recipe&, const std::wstring& path) const override { return HasPdfExtension(path) || IsReadableImage(path); }

            bool Accepts(const Recipe& recipe, const std::wstring& path, std::wstring* why) const override
            {
                const bool isPdf = HasPdfExtension(path);
                if (isPdf && !LooksLikePdf(path, why))
                    return false;
                if (!isPdf && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
                {
                    if (why)
                        *why = L"file not found";
                    return false;
                }
                // A PDF with a text layer copies without OCR; everything else needs it.
                if (isPdf && ActionOf(recipe) == "ocr.copy_text")
                    return true;
                if (!Available(why))
                    return false;
                if (isPdf && !RenderingAvailable())
                {
                    if (why)
                        *why = L"Windows' PDF renderer is not available";
                    return false;
                }
                return true;
            }

            bool IsBatch(const Recipe& recipe) const override { return ActionOf(recipe) == "ocr.copy_text"; }

            ItemResult Run(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                           const ProgressFn& progress) const override
            {
                if (ActionOf(recipe) != "ocr.searchable_pdf")
                    return FailItem(m_history, runId, source, L"not a per-file action");
                try
                {
                    return HasPdfExtension(source) ? SearchablePdf(recipe, runId, source, stop, progress)
                                                   : SearchableImage(recipe, runId, source, progress);
                }
                catch (const std::exception& e)
                {
                    return FailItem(m_history, runId, source, QpdfError(e));
                }
            }

            ItemResult RunBatch(const Recipe&, int64_t runId, const std::vector<std::wstring>& sources, std::stop_token stop,
                                const ProgressFn& progress) const override
            {
                // Copy text: every file's text, one after the other, in a single clipboard entry.
                std::wstring all, firstError;
                size_t words = 0, withText = 0;
                for (size_t i = 0; i < sources.size(); ++i)
                {
                    if (stop.stop_requested())
                        return ItemResult{ false, true, {}, kCancelled, 0 };
                    const auto& source = sources[i];
                    const auto fileProgress = [&](double f) {
                        if (progress)
                            progress((static_cast<double>(i) + f) / static_cast<double>(sources.size()));
                    };
                    std::wstring text, error;
                    try
                    {
                        if (HasPdfExtension(source))
                        {
                            PdfText pdfText;
                            if (ReadPdfText(source, true, stop, fileProgress, pdfText, error))
                            {
                                text = pdf::ToText(pdfText.pages);
                                if (text.empty() && !pdfText.ocrProblem.empty())
                                    error = pdfText.ocrProblem;
                            }
                        }
                        else
                        {
                            Result result;
                            if (RecognizeImage(source, result, error))
                                text = pdf::ToText({ LinesFromOcr(result, 1.0) });
                        }
                    }
                    catch (const std::exception& e)
                    {
                        error = QpdfError(e);
                    }
                    if (error == kCancelled)
                        return ItemResult{ false, true, {}, kCancelled, 0 };
                    if (!error.empty())
                        log::Warn(L"copy text: {}: {}", source, error);
                    if (firstError.empty() && !error.empty())
                        firstError = std::format(L"{}: {}", FileNameOf(source), error);
                    fileProgress(1.0);
                    if (text.empty())
                        continue;
                    ++withText;
                    words += CountWords(text);
                    if (!all.empty())
                        all += L"\r\n";
                    all += text;
                }
                if (all.empty())
                    return FailItem(m_history, runId, sources.front(), firstError.empty() ? std::wstring(L"no text found") : firstError);
                std::wstring error;
                if (!clipboard::SetText(nullptr, all, &error))
                    return FailItem(m_history, runId, sources.front(), error);
                RecordNoFileItem(m_history, runId, sources.front());
                ItemResult result;
                result.ok = true;
                result.message = CopiedMessage(words, withText);
                return result;
            }

        private:
            ItemResult SearchableImage(const Recipe& recipe, int64_t runId, const std::wstring& source, const ProgressFn& progress) const
            {
                ImageSource image;
                std::wstring error;
                if (!LoadImageSource(source, image, error))
                    return FailItem(m_history, runId, source, error);
                // OCR reads the picture upright; the page shows the same thing (a pass-through JPEG is
                // turned by the page's drawing matrix instead of by re-encoding it).
                Bitmap upright;
                if (image.jpeg.empty())
                    upright = image.pixels;
                else if (!DecodeImageFile(source, upright, error))
                    return FailItem(m_history, runId, source, error);
                FlattenOnWhite(upright);
                Result result;
                if (!Recognize(upright, result, error))
                    return FailItem(m_history, runId, source, error);
                if (progress)
                    progress(0.7);
                if (result.lines.empty())
                    return FailItem(m_history, runId, source, L"no text found in the picture");
                StagedOutput out(m_history, runId, source);
                if (!out.Ready())
                    return out.Fail(out.Error());
                ImagePdfBuilder pdf(out.Path());
                if (!pdf.AddPage(image, &result, error) || !pdf.Finish(error))
                    return out.Fail(error);
                auto done = out.Commit(recipe.suffix, L".pdf");
                if (done.ok)
                    done.message = std::format(L"Searchable PDF: {} words", CountWords(pdf::ToText({ LinesFromOcr(result, 1.0) })));
                return done;
            }

            ItemResult SearchablePdf(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                                     const ProgressFn& progress) const
            {
                std::wstring error;
                auto doc = OpenPdf(source, error);
                if (!doc)
                    return FailItem(m_history, runId, source, error);
                auto pages = QPDFPageDocumentHelper(*doc).getAllPages();
                std::unique_ptr<IRenderedPdf> rendered;
                PdfTextLayer layer(*doc);
                size_t needed = 0, added = 0;
                for (size_t i = 0; i < pages.size(); ++i)
                {
                    if (stop.stop_requested())
                        return FailItem(m_history, runId, source, kCancelled, true);
                    if (progress)
                        progress(0.8 * static_cast<double>(i) / static_cast<double>(pages.size()));
                    if (VisibleChars(ExtractPageLines(pages[i])) >= kMinTextChars)
                        continue;   // already has real text
                    ++needed;
                    if (!rendered && !(rendered = OpenForRendering(source, error)))
                        return FailItem(m_history, runId, source, error);
                    Bitmap bitmap;
                    Result result;
                    if (!RenderForOcr(*rendered, static_cast<uint32_t>(i), bitmap, error) || !Recognize(bitmap, result, error))
                        return FailItem(m_history, runId, source, std::format(L"page {}: {}", i + 1, error));
                    if (layer.AddToPage(pages[i], result))
                        ++added;
                }
                if (needed == 0)
                    return FailItem(m_history, runId, source, L"already searchable: every page has text", true);
                if (added == 0)
                    return FailItem(m_history, runId, source, L"no text found on the scanned pages");
                StagedOutput out(m_history, runId, source);
                if (!out.Ready())
                    return out.Fail(out.Error());
                if (!WritePdf(*doc, out.Path(), WriteOptions{}, error, stop, [&](double f) {
                        if (progress)
                            progress(0.8 + 0.2 * f);
                    }))
                    return out.Fail(error, error == kCancelled);
                auto done = out.Commit(recipe.suffix, L".pdf");
                if (done.ok)
                    done.message = std::format(L"Text added to {} of {} pages", added, pages.size());
                return done;
            }

            HistoryStore* m_history;
        };
    }

    std::unique_ptr<IEngine> MakeOcrEngine(const EngineServices& services) { return std::make_unique<OcrEngine>(services); }
}
