// The PDF engine: PDF -> JPG/PNG/TXT/Markdown, Images -> PDF, Merge PDFs, Organize PDF Pages, Edit
// (a PDF, or any document laid out as PDF pages first: DocumentPages.h) and Compress PDF. Every output goes through StagedOutput, so it is named "name (suffix).ext", never
// overwrites anything and can be undone.
#include "ErrorText.h"
#include "FileIo.h"
#include "FormatText.h"
#include "ImagePipeline.h"
#include "Log.h"
#include "PathUtil.h"
#include "StagedOutput.h"
#include "Utf.h"
#include "docs/DocsModule.h"
#include "docs/PdfPrint.h"
#include "formats/Frames.h"
#include "pdf/DocumentPages.h"
#include "pdf/ImagePdf.h"
#include "pdf/PageEdits.h"
#include "pdf/PageRenderer.h"
#include "pdf/PdfCompress.h"
#include "pdf/PdfDoc.h"
#include "pdf/PdfEditors.h"
#include "pdf/PdfModule.h"
#include "pdf/PdfText.h"
#include "pdf/Parallel.h"
#include "pdf/Wic.h"

#include <atomic>
#include <filesystem>
#include <format>
#include <mutex>
#include <set>

#include <wil/resource.h>

namespace sd::pdf
{
    namespace
    {
        const std::string& ActionOf(const Recipe& r) { return r.steps.front().action; }

        std::wstring Pages(size_t n) { return n == 1 ? L"1 page" : std::format(L"{} pages", n); }

        bool EditsPages(const std::string& action) { return action == "pdf.edit" || action == "pdf.lay_out"; }

        // The PDFs laid out for one Edit (each in a folder of its own, made by NewEditFolder). They go
        // with their folders when the editor closes without saving; Save hands them to the export,
        // which removes them once it has read them.
        struct LaidOutPages
        {
            std::vector<std::wstring> pdfs;
            bool kept = false;
            ~LaidOutPages()
            {
                if (!kept)
                    for (const auto& pdf : pdfs)
                        RemoveEditFolder(std::filesystem::path(pdf).parent_path().wstring());
            }
        };

        class PdfEngine final : public IEngine
        {
        public:
            explicit PdfEngine(const EngineServices& services) : m_history(services.history), m_apps(office::DetectApps()) {}

            bool Handles(const Recipe& recipe) const override
            {
                return recipe.steps.size() == 1 && ActionOf(recipe).starts_with("pdf.");
            }

            bool Applies(const Recipe& recipe, const std::wstring& path) const override
            {
                const auto& action = ActionOf(recipe);
                if (action == "pdf.from_images")
                    return IsReadableImage(path);
                if (EditsPages(action))
                    return PageSourceOf(path, m_apps) != PageSource::None;
                return HasPdfExtension(path);
            }

            bool Accepts(const Recipe& recipe, const std::wstring& path, std::wstring* why) const override
            {
                const auto& action = ActionOf(recipe);
                if (action == "pdf.from_images")
                {
                    if (!PathExists(path))
                    {
                        if (why)
                            *why = L"file not found";
                        return false;
                    }
                    if (!IsReadableImage(path))
                    {
                        if (why)
                            *why = L"not an image this PC can read";
                        return false;
                    }
                    return true;
                }
                if (EditsPages(action))
                    if (const auto from = PageSourceOf(path, m_apps); from != PageSource::Pdf)
                        return CanLayOut(path, from, why);
                if (!LooksLikePdf(path, why))
                    return false;
                if (action == "pdf.to_images" && !RenderingAvailable())
                {
                    if (why)
                        *why = L"Windows' PDF renderer is not available";
                    return false;
                }
                return true;
            }

            // Office's COM objects need its STA lane; our own layout runs anywhere.
            JobLane Lane(const Recipe& recipe) const override
            {
                return ActionOf(recipe) == "pdf.lay_out" && recipe.steps.front().params.value("office", false) ? JobLane::Office : JobLane::Pool;
            }

            bool IsBatch(const Recipe& recipe) const override
            {
                const auto& a = ActionOf(recipe);
                return a == "pdf.from_images" || a == "pdf.merge" || a == "pdf.organize" || a == "pdf.edit";
            }

            bool NeedsEditor(const Recipe& recipe) const override
            {
                const auto& a = ActionOf(recipe);
                return a == "pdf.merge" || a == "pdf.organize" || a == "pdf.edit";
            }

            void OpenEditor(HWND owner, const Recipe& recipe, const std::vector<std::wstring>& files,
                            std::function<void(Recipe, std::vector<std::wstring>)> done) const override
            {
                const auto& action = ActionOf(recipe);
                if (action == "pdf.merge" || action == "pdf.edit")
                {
                    // One PDF has nothing to merge with: run at once so the panel says so.
                    if ((action == "pdf.merge" && files.size() < 2) || !Editors().merge)
                    {
                        done(recipe, files);
                        return;
                    }
                    Editors().merge(owner, files, {}, action == "pdf.edit", [recipe, done](std::vector<std::wstring> ordered, PagePlan plan) {
                        Recipe adjusted = recipe;
                        if (!plan.empty())
                            adjusted.steps.front().params["plan"] = PlanToJson(plan);
                        done(std::move(adjusted), std::move(ordered));
                    });
                    return;
                }
                if (!Editors().organize)
                {
                    done(recipe, files);
                    return;
                }
                // One window per PDF, each saved on its own as "name (organized).pdf".
                for (const auto& file : files)
                    Editors().organize(owner, { file }, [recipe, file, done](PagePlan plan) {
                        Recipe adjusted = recipe;
                        adjusted.steps.front().params["plan"] = PlanToJson(plan);
                        done(std::move(adjusted), { file });
                    });
            }

            // Edit with documents among the files: their pages are laid out first, as a batch with
            // progress and × in the top panel.
            std::optional<Recipe> EditorInput(const Recipe& recipe, const std::vector<std::wstring>& files) const override
            {
                if (ActionOf(recipe) != "pdf.edit")
                    return std::nullopt;
                bool documents = false, office = false;
                for (const auto& f : files)
                {
                    const auto from = PageSourceOf(f, m_apps);
                    documents |= from != PageSource::Pdf;
                    office |= from == PageSource::Office;
                }
                if (!documents)
                    return std::nullopt;
                Recipe layOut;
                layOut.id = "pdf-lay-out";
                layOut.label = recipe.label;
                layOut.description = L"Lay out the pages to edit";
                layOut.ring = recipe.ring;
                layOut.suffix = recipe.suffix;
                layOut.builtIn = true;
                layOut.steps.push_back(RecipeStep{ "pdf.lay_out", nlohmann::json{ { "office", office } } });
                return layOut;
            }

            void OpenEditorOn(HWND owner, const Recipe& recipe, const std::vector<std::wstring>& files, const std::vector<std::wstring>& made,
                              bool cancelled, std::function<void(Recipe, std::vector<std::wstring>)> done) const override
            {
                // The files whose pages are there, and those pages: a PDF is its own.
                std::vector<std::wstring> sources, pages, names;
                auto laidOut = std::make_shared<LaidOutPages>();
                for (size_t i = 0; i < files.size() && i < made.size(); ++i)
                {
                    if (made[i].empty())
                        continue;
                    sources.push_back(files[i]);
                    pages.push_back(made[i]);
                    names.push_back(FileName(files[i]));
                    if (made[i] != files[i])
                        laidOut->pdfs.push_back(made[i]);
                }
                if (cancelled || pages.empty())
                    return;   // laidOut removes what was made
                // Save: the files go back to the documents (so the output is named after the first and
                // saved next to it), each one's laid-out pages listed in the recipe for the export.
                auto finish = [recipe, done, sources, pages, laidOut](std::vector<std::wstring> ordered, PagePlan plan) {
                    laidOut->kept = true;
                    Recipe adjusted = recipe;
                    auto& params = adjusted.steps.front().params;
                    if (!plan.empty())
                        params["plan"] = PlanToJson(plan);
                    params["laidOut"] = nlohmann::json::object();
                    for (auto& f : ordered)
                        for (size_t k = 0; k < pages.size(); ++k)
                            if (f == pages[k])
                            {
                                if (pages[k] != sources[k])
                                    params["laidOut"][Narrow(sources[k])] = Narrow(pages[k]);
                                f = sources[k];
                                break;
                            }
                    done(std::move(adjusted), std::move(ordered));
                };
                if (!Editors().merge)
                {
                    finish(pages, {});   // no editor (unit tests): every page as it is
                    return;
                }
                Editors().merge(owner, pages, names, true, std::move(finish));
            }

            ItemResult Run(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                           const ProgressFn& progress) const override
            {
                try
                {
                    const auto& a = ActionOf(recipe);
                    if (a == "pdf.to_images")
                        return ToImages(recipe, runId, source, stop, progress);
                    if (a == "pdf.to_text")
                        return ToText(recipe, runId, source, stop, progress);
                    if (a == "pdf.compress")
                        return Compress(recipe, runId, source, stop, progress);
                    if (a == "pdf.lay_out")
                        return LayOut(source, stop, progress);
                }
                catch (const std::exception& e)
                {
                    return RecordFailure(m_history, runId, source, QpdfError(e));
                }
                return RecordFailure(m_history, runId, source, L"not a per-file action");
            }

            ItemResult RunBatch(const Recipe& recipe, int64_t runId, const std::vector<std::wstring>& sources, std::stop_token stop,
                                const ProgressFn& progress) const override
            {
                if (sources.empty())
                    return ItemResult{ false, false, {}, L"no files", 0 };
                try
                {
                    const auto& a = ActionOf(recipe);
                    if (a == "pdf.from_images")
                        return FromImages(recipe, runId, sources, stop, progress);
                    if (a == "pdf.merge" || a == "pdf.organize" || a == "pdf.edit")
                        return Assemble(recipe, runId, sources, stop, progress);
                }
                catch (const std::exception& e)
                {
                    return RecordFailure(m_history, runId, sources.front(), QpdfError(e));
                }
                return RecordFailure(m_history, runId, sources.front(), L"not a batch action");
            }

        private:
            // A document Edit can lay out now: it is there, and the PDF printer our own layout prints
            // through is installed (Convert's PDF asks the same).
            static bool CanLayOut(const std::wstring& path, PageSource from, std::wstring* why)
            {
                if (from == PageSource::None)
                {
                    if (why)
                        *why = L"not a document";
                    return false;
                }
                if (!PathExists(path))
                {
                    if (why)
                        *why = L"file not found";
                    return false;
                }
                if (from != PageSource::Office && docs::InputOf(path) != docs::DocInput::Pages && !docs::FindPdfPrinter(nullptr, why))
                    return false;
                return true;
            }

            // Edit's first step for one file: a PDF is its own pages; a document is laid out into a
            // folder of its own under %TEMP% (the page view's, PageEdits.h). Nothing goes next to the
            // document and nothing is recorded: the edit's export is the run History keeps.
            ItemResult LayOut(const std::wstring& source, std::stop_token stop, const ProgressFn& progress) const
            {
                ItemResult result;
                if (PageSourceOf(source, m_apps) == PageSource::Pdf)
                {
                    result.ok = true;
                    result.output = source;
                    return result;
                }
                const auto folder = NewEditFolder();
                if (folder.empty())
                {
                    result.error = L"cannot write a temporary file";
                    return result;
                }
                const auto pdf = (std::filesystem::path(folder) / (std::filesystem::path(source).stem().wstring() + L".pdf")).wstring();
                const auto outcome = LayOutPages(source, m_apps, pdf, stop, progress);
                if (!outcome.ok)
                {
                    RemoveEditFolder(folder);
                    result.skipped = outcome.cancelled;
                    result.error = outcome.error.empty() ? L"cannot lay out the pages" : outcome.error;
                    return result;
                }
                if (progress)
                    progress(1.0);
                result.ok = true;
                result.output = pdf;
                result.size = FileSize(pdf);
                return result;
            }

            ItemResult ToImages(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                                const ProgressFn& progress) const
            {
                const auto& params = recipe.steps.front().params;
                const bool jpeg = params.value("format", std::string("png")) == "jpg";
                const double dpi = params.value("dpi", 300);
                const int quality = params.value("quality", 90);
                const std::wstring ext = jpeg ? L".jpg" : L".png";

                std::wstring error;
                auto doc = OpenForRendering(source, error);
                if (!doc)
                    return RecordFailure(m_history, runId, source, error);
                const uint32_t count = doc->PageCount();
                if (count == 0)
                    return RecordFailure(m_history, runId, source, L"the PDF has no pages");

                // One page: one file next to the PDF. More: a folder "name (pages)" holding them all.
                StagedOutput out(m_history, runId, source, count > 1);
                if (!out.Ready())
                    return out.Fail(out.Error());
                const std::wstring stem = std::filesystem::path(source).stem().wstring();
                const int digits = std::max(2, static_cast<int>(std::to_wstring(count).size()));
                // Windows' renderer draws one page at a time; encoding (the slow part of a PNG) runs on
                // several threads, each taking the next page as soon as the renderer is free.
                std::mutex renderer, failed;
                std::wstring firstError;   // the lowest page's, under `failed`
                uint32_t firstFailed = count;
                std::atomic<uint32_t> finished{ 0 };
                std::atomic<bool> anyFailed{ false };   // the other threads stop taking pages
                ForEachParallel(count, [&](size_t index) {
                    const auto i = static_cast<uint32_t>(index);
                    const auto fail = [&](std::wstring why) {
                        anyFailed = true;
                        std::scoped_lock lock(failed);
                        if (i < firstFailed)
                        {
                            firstFailed = i;
                            firstError = std::format(L"page {}: {}", i + 1, why);
                        }
                    };
                    if (stop.stop_requested() || anyFailed)
                        return;
                    Bitmap page;
                    uint32_t pw = 0, ph = 0;
                    {
                        std::scoped_lock lock(renderer);
                        double w = 0, h = 0;
                        if (!doc->PageSize(i, w, h))
                            return fail(L"cannot read its size");
                        PixelSize(w, h, dpi, 16000, pw, ph);
                        std::wstring why;
                        if (!doc->Render(i, pw, ph, page, why))
                            return fail(why);
                    }
                    const std::wstring path =
                        count == 1 ? out.Path() : std::format(L"{}\\{} page {:0{}}{}", out.Path(), stem, i + 1, digits, ext);
                    if (const HRESULT hr = EncodeImageFile(page, path, jpeg, quality, dpi); FAILED(hr))
                        return fail(HResultText(hr));
                    if (progress)
                        progress(static_cast<double>(++finished) / count);
                });
                if (stop.stop_requested())
                    return out.Fail(kCancelled, true);
                if (firstFailed < count)
                    return out.Fail(firstError);
                auto result = count == 1 ? out.Commit(recipe.suffix, ext) : out.Commit(L"pages", L"");
                if (result.ok && count > 1)
                    result.message = std::format(L"{} saved as {} images", Pages(count), jpeg ? L"JPG" : L"PNG");
                return result;
            }

            ItemResult ToText(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                              const ProgressFn& progress) const
            {
                const auto& params = recipe.steps.front().params;
                const bool markdown = params.value("format", std::string("txt")) == "md";
                StagedOutput out(m_history, runId, source);
                if (!out.Ready())
                    return out.Fail(out.Error());
                PdfText text;
                std::wstring error;
                if (!ReadPdfText(source, params.value("ocr", true), stop, progress, text, error))
                    return out.Fail(error, error == kCancelled);
                const std::wstring content = markdown ? ToMarkdown(text.pages) : pdf::ToText(text.pages);
                if (content.empty())
                {
                    std::wstring why = L"no text found";
                    if (!text.ocrProblem.empty())
                        why = L"no text layer, and OCR could not run: " + text.ocrProblem;
                    return out.Fail(why);
                }
                if (!WriteFileBytes(out.Path(), Narrow(content), WriteMode::Existing))
                    return out.Fail(L"could not write the text file");
                auto result = out.Commit(recipe.suffix, markdown ? L".md" : L".txt");
                if (result.ok && text.ocrPages > 0)
                    result.message = std::format(L"Text saved; {} read with OCR", Pages(text.ocrPages));
                return result;
            }

            ItemResult Compress(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                                const ProgressFn& progress) const
            {
                const auto& params = recipe.steps.front().params;
                const uint64_t before = FileSize(source);
                const int percent = params.value("percent", 0);
                const uint64_t target = percent > 0 ? before * static_cast<uint64_t>(percent) / 100 : 0;
                // With a target (Reduce size) the pictures step down until the file fits. Only pictures
                // shrink: text and drawings stay as they are, so a PDF of mostly text may stop short.
                std::vector<CompressOptions> ladder;
                if (target > 0)
                    ladder = { { 150, 70 }, { 120, 60 }, { 96, 50 }, { 72, 40 }, { 60, 30 }, { 48, 25 } };
                else
                    ladder = { { params.value("dpi", 150), params.value("quality", 70) } };
                StagedOutput out(m_history, runId, source);
                if (!out.Ready())
                    return out.Fail(out.Error());
                uint64_t after = 0;
                for (size_t i = 0; i < ladder.size(); ++i)
                {
                    CompressStats stats;
                    std::wstring error;
                    const auto step = [&](double f) { if (progress) progress((static_cast<double>(i) + f) / static_cast<double>(ladder.size())); };
                    if (!CompressPdf(source, out.Path(), ladder[i], stop, progress ? ProgressFn(step) : ProgressFn{}, stats, error))
                        return out.Fail(error, error == kCancelled);
                    after = FileSize(out.Path());
                    log::Info(L"pdf compress: {} -> {} bytes at {} dpi, quality {}; {} of {} images re-encoded", before, after,
                              ladder[i].dpi, ladder[i].quality, stats.recompressed, stats.images);
                    if (target == 0 || (after > 0 && after <= target) || stats.recompressed == 0)
                        break;   // fits, or there are no pictures left to shrink
                }
                // Never keep a "compressed" file that is not smaller than what it came from.
                if (after == 0 || after >= before)
                    return out.Fail(L"already compact: compressing would not make it smaller", true);
                auto result = out.Commit(recipe.suffix, L".pdf");
                if (result.ok)
                {
                    result.message = std::format(L"{} to {} ({}% smaller)", SizeText(before), SizeText(after),
                                                 static_cast<int>(100 - after * 100 / before));
                    if (target > 0 && after > target)
                        result.message += L"; mostly text, so it could not get smaller";
                }
                return result;
            }

            ItemResult FromImages(const Recipe& recipe, int64_t runId, const std::vector<std::wstring>& sources, std::stop_token stop,
                                  const ProgressFn& progress) const
            {
                StagedOutput out(m_history, runId, sources.front());
                if (!out.Ready())
                    return out.Fail(out.Error());
                ImagePdfBuilder pdf(out.Path());
                if (!pdf.Ok())
                    return out.Fail(L"cannot write next to the source");
                std::vector<std::wstring> skipped;
                for (size_t i = 0; i < sources.size(); ++i)
                {
                    if (stop.stop_requested())
                        return out.Fail(kCancelled, true);
                    ImageSource image;
                    std::wstring error;
                    // A multi-page TIFF (or an animated GIF) gives a page per frame (src/formats/Frames.h).
                    if (formats::FrameCount(sources[i]) > 1)
                    {
                        double dpiX = 0, dpiY = 0;   // the first page's resolution sizes them all (scans share one)
                        if (!ReadResolution(sources[i], dpiX, dpiY))
                            dpiX = dpiY = 0;
                        const HRESULT hr = formats::ForEachFrame(sources[i], stop, [&](const formats::Image& frame, uint32_t, uint32_t, int) {
                            ImageSource page;
                            page.dpiX = dpiX;
                            page.dpiY = dpiY;
                            page.pixels.width = frame.width;
                            page.pixels.height = frame.height;
                            page.pixels.bgra = frame.bgra;
                            return pdf.AddPage(page, nullptr, error);
                        }, error);
                        if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
                            return out.Fail(kCancelled, true);
                        if (FAILED(hr) || !error.empty())
                            return out.Fail(std::format(L"{}: {}", FileName(sources[i]), error));
                    }
                    else if (!IsReadableImage(sources[i]) || !LoadImageSource(sources[i], image, error))
                    {
                        log::Warn(L"images to pdf: {} skipped: {}", sources[i], error);
                        skipped.push_back(FileName(sources[i]));
                    }
                    else if (!pdf.AddPage(image, nullptr, error))
                        return out.Fail(error);
                    if (progress)
                        progress(static_cast<double>(i + 1) / static_cast<double>(sources.size()));
                }
                std::wstring error;
                if (pdf.Pages() == 0)
                    return out.Fail(L"none of the files could be read as images");
                if (!pdf.Finish(error))
                    return out.Fail(error);
                auto result = out.Commit(recipe.suffix, L".pdf");
                if (result.ok)
                    result.message = skipped.empty() ? std::format(L"PDF made from {}", Pages(pdf.Pages()))
                                                     : std::format(L"PDF made from {}; {} skipped", Pages(pdf.Pages()), skipped.size());
                return result;
            }

            ItemResult Assemble(const Recipe& recipe, int64_t runId, const std::vector<std::wstring>& sources, std::stop_token stop,
                                const ProgressFn& progress) const
            {
                // Edit PDF with several files merges them too, and names the result the same way.
                const bool merge = ActionOf(recipe) == "pdf.merge" || (ActionOf(recipe) == "pdf.edit" && sources.size() > 1);
                if (ActionOf(recipe) == "pdf.merge" && sources.size() < 2)
                    return RecordFailure(m_history, runId, sources.front(), L"drop two or more PDFs to merge them");
                // Edit on a document: its laid-out pages stand in for it (OpenEditorOn), and go once read.
                // The output is still named after the first file and saved next to it.
                const auto& params = recipe.steps.front().params;
                std::vector<std::wstring> pdfs = sources;
                std::set<std::wstring> laidOutFolders;
                if (const auto laidOut = params.find("laidOut"); laidOut != params.end() && laidOut->is_object())
                    for (auto& pdf : pdfs)
                        if (const auto it = laidOut->find(Narrow(pdf)); it != laidOut->end() && it->is_string())
                        {
                            pdf = Widen(it->get<std::string>());
                            laidOutFolders.insert(std::filesystem::path(pdf).parent_path().wstring());
                        }
                const auto removeLaidOut = wil::scope_exit([&] {
                    for (const auto& folder : laidOutFolders)
                        RemoveEditFolder(folder);   // only ever a folder the page view's code made
                });
                for (size_t i = 0; i < pdfs.size(); ++i)
                {
                    std::wstring why;
                    if (!LooksLikePdf(pdfs[i], &why))
                        return RecordFailure(m_history, runId, sources.front(), std::format(L"{}: {}", FileName(sources[i]), why));
                }
                std::vector<uint32_t> counts;
                std::wstring error;
                if (!CountPages(pdfs, counts, error))
                    return RecordFailure(m_history, runId, sources.front(), error);
                PagePlan plan = AllPages(counts);
                if (params.contains("plan"))
                {
                    std::string problem;
                    if (!PlanFromJson(params["plan"], plan, problem))
                        return RecordFailure(m_history, runId, sources.front(), Widen(problem));
                }
                // The page view's edited pages are read once, here; their folder goes afterwards,
                // whatever happens (RemoveEditFolder only touches folders the page view made).
                const auto removeEdits = wil::scope_exit([&] {
                    std::set<std::wstring> folders;
                    for (const auto& r : plan)
                        if (!r.edited.empty())
                            folders.insert(std::filesystem::path(r.edited).parent_path().wstring());
                    for (const auto& folder : folders)
                        RemoveEditFolder(folder);
                });
                std::string problem;
                if (!CheckPlan(plan, counts, problem))
                    return RecordFailure(m_history, runId, sources.front(), Widen(problem));

                StagedOutput out(m_history, runId, sources.front());
                if (!out.Ready())
                    return out.Fail(out.Error());
                if (!AssemblePages(pdfs, plan, out.Path(), stop, progress, error))
                    return out.Fail(error, error == kCancelled);
                const size_t edited = EditedPages(plan);
                auto result = out.Commit(merge && ActionOf(recipe) == "pdf.edit" ? std::wstring(L"merged") : recipe.suffix, L".pdf");
                if (result.ok)
                    result.message = merge ? std::format(L"Merged {} {} into {}", sources.size(), laidOutFolders.empty() ? L"PDFs" : L"files", Pages(plan.size()))
                                           : std::format(L"Saved {}", Pages(plan.size()));
                if (result.ok && edited > 0)
                    result.message += std::format(L", {} edited", edited);
                return result;
            }

            HistoryStore* m_history;
            office::Apps m_apps;   // who can lay out office files (Edit), found once
        };
    }

    std::unique_ptr<IEngine> MakePdfEngine(const EngineServices& services) { return std::make_unique<PdfEngine>(services); }
}
