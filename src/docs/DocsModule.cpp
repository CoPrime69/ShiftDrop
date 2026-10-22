#include "docs/DocsModule.h"
#include "FileIo.h"
#include "PathUtil.h"
#include "TempPath.h"
#include "docs/Bytes.h"
#include "docs/Formats.h"
#include "docs/PdfPrint.h"
#include "docs/PdfSource.h"
#include "docs/Rtf.h"
#include "Log.h"
#include "OutputPath.h"
#include "Utf.h"

#include <windows.h>
#include <wil/resource.h>

#include <filesystem>
#include <format>

namespace sd::docs
{
    namespace
    {
        const DocTarget kTargets[] = {
            { "docx", L".docx" }, { "pdf", L".pdf" }, { "md", L".md" }, { "html", L".html" }, { "epub", L".epub" }, { "txt", L".txt" },
        };

        const char* SameFormat(DocInput input)
        {
            switch (input)
            {
            case DocInput::Docx: return "docx";
            case DocInput::Markdown: return "md";
            case DocInput::Text: return "txt";
            case DocInput::Html: return "html";
            default: return "";
            }
        }

        class DocsEngine final : public IEngine
        {
        public:
            explicit DocsEngine(const EngineServices& services) : m_history(services.history) {}

            bool Handles(const Recipe& recipe) const override
            {
                return recipe.steps.size() == 1 && recipe.steps[0].action == "docs.convert";
            }

            bool Applies(const Recipe& recipe, const std::wstring& path) const override
            {
                return TargetFits(InputOf(path), recipe.FirstParam("format"));
            }

            bool Accepts(const Recipe& recipe, const std::wstring& path, std::wstring* why) const override
            {
                const auto input = InputOf(path);
                const auto target = recipe.FirstParam("format");
                if (!TargetFits(input, target))
                {
                    if (why)
                        *why = input == DocInput::None ? L"not a document" : std::format(L"already {}", Widen(target));
                    return false;
                }
                if (!PathExists(path))
                {
                    if (why)
                        *why = L"file not found";
                    return false;
                }
                if (target == "pdf" && input != DocInput::Pages && !FindPdfPrinter(nullptr, why))
                    return false;
                return true;
            }

            ItemResult Run(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop, const ProgressFn& progress) const override
            {
                const auto target = recipe.FirstParam("format");
                const auto* t = FindDocTarget(target);
                const auto input = InputOf(source);
                const auto stem = std::filesystem::path(source).stem().wstring();
                if (progress)
                    progress(0.02);

                // PDF: the pages Edit opens too (DocumentPdf).
                if (target == "pdf")
                {
                    StagedOutput out(m_history, runId, source);
                    if (!out.Ready())
                        return out.Fail(out.Error());
                    std::string pdf;
                    std::wstring error;
                    if (!DocumentPdf(source, pdf, stop, progress, error) || !WriteFileBytes(out.Path(), pdf, WriteMode::Existing, &error))
                        return out.Fail(error, stop.stop_requested());
                    if (progress)
                        progress(1.0);
                    return out.Commit(recipe.suffix, t->extension);
                }

                Document doc;
                std::wstring error;
                if (!ReadDocument(source, input, doc, stop, error))
                {
                    StagedOutput out(m_history, runId, source);
                    return out.Fail(error.empty() ? L"cannot read this document" : error, stop.stop_requested());
                }
                if (stop.stop_requested())
                {
                    StagedOutput out(m_history, runId, source);
                    return out.Fail(L"cancelled", true);
                }
                if (progress)
                    progress(0.3);
                const std::string fallbackTitle = Narrow(stem);
                if (doc.title.empty() && target != "md" && target != "txt")
                    doc.title = TitleOf(doc, fallbackTitle);

                // Markdown and HTML with pictures: a folder holding the page and an images folder.
                const bool folder = (target == "md" || target == "html") && !doc.images.empty();
                StagedOutput out(m_history, runId, source, folder);
                if (!out.Ready())
                    return out.Fail(out.Error());

                std::string bytes;
                if (target == "docx")
                {
                    if (!WriteDocx(doc, bytes))
                        return out.Fail(L"cannot build the DOCX package");
                }
                else if (target == "epub")
                {
                    if (!WriteEpub(doc, fallbackTitle, bytes))
                        return out.Fail(L"cannot build the EPUB package");
                }
                else if (target == "md")
                    bytes = WriteMarkdown(doc, "images/");
                else if (target == "html")
                {
                    HtmlOptions o;
                    o.imageDir = "images/";
                    bytes = WriteHtml(doc, o);
                }
                else if (target == "txt")
                    bytes = WritePlainText(doc);
                else
                    return out.Fail(L"unknown output format");

                if (stop.stop_requested())
                    return out.Fail(L"cancelled", true);
                if (folder)
                {
                    const std::filesystem::path dir(out.Path());
                    if (!WriteFileBytes((dir / (stem + t->extension)).wstring(), bytes, WriteMode::New, &error))
                        return out.Fail(error);
                    std::error_code ec;
                    std::filesystem::create_directory(dir / L"images", ec);
                    for (const auto& img : doc.images)
                        if (!WriteFileBytes((dir / L"images" / Widen(img.name)).wstring(), img.data, WriteMode::New, &error))
                            return out.Fail(error);
                    if (progress)
                        progress(1.0);
                    return out.Commit(recipe.suffix, L"");
                }
                if (!WriteFileBytes(out.Path(), bytes, WriteMode::Existing, &error))
                    return out.Fail(error);
                if (progress)
                    progress(1.0);
                return out.Commit(recipe.suffix, t->extension);
            }

        private:
            HistoryStore* m_history;
        };

        void RegisterActions(ActionRegistry& registry)
        {
            registry.Add(ActionDescriptor{ "docs.convert", L"Convert document", StepKind::Engine,
                                           { ParamSpec{ "format", ParamType::Enum, 0, 0, { "docx", "pdf", "md", "html", "epub", "txt" }, "docx" } } });
        }

        const char* Recipes()
        {
            // Icons (Segoe Fluent Icons): Document E8A5, PDF EA90, Code E943, Globe E774,
            // ReadingMode E736, Font E8D2.
            return R"([
  { "id": "docs-to-docx", "label": "DOCX", "description": "Word document", "icon": "E8A5",
    "steps": [ { "action": "docs.convert", "params": { "format": "docx" } } ] },
  { "id": "docs-to-pdf", "label": "PDF", "description": "PDF, laid out for A4 or Letter", "icon": "EA90",
    "steps": [ { "action": "docs.convert", "params": { "format": "pdf" } } ] },
  { "id": "docs-to-md", "label": "MD", "description": "Markdown text, pictures in a folder", "icon": "E943",
    "steps": [ { "action": "docs.convert", "params": { "format": "md" } } ] },
  { "id": "docs-to-html", "label": "HTML", "description": "A web page", "icon": "E774",
    "steps": [ { "action": "docs.convert", "params": { "format": "html" } } ] },
  { "id": "docs-to-epub", "label": "EPUB", "description": "E-book for any reader app", "icon": "E736",
    "steps": [ { "action": "docs.convert", "params": { "format": "epub" } } ] },
  { "id": "docs-to-txt", "label": "TXT", "description": "Plain text", "icon": "E8D2",
    "steps": [ { "action": "docs.convert", "params": { "format": "txt" } } ] }
])";
        }

        std::unique_ptr<IEngine> MakeEngine(const EngineServices& services)
        {
            return std::make_unique<DocsEngine>(services);
        }
    }

    DocInput InputOf(const std::wstring& path)
    {
        const auto ext = LowerExtension(path);
        if (ext == L".rtfd")
            return IsFolder(path) ? DocInput::Rtfd : DocInput::None;
        if (IsFolder(path))
            return DocInput::None;
        if (ext == L".docx" || ext == L".docm" || ext == L".dotx")
            return DocInput::Docx;
        if (ext == L".md" || ext == L".markdown" || ext == L".mdown" || ext == L".mkd")
            return DocInput::Markdown;
        if (ext == L".txt" || ext == L".text")
            return DocInput::Text;
        if (ext == L".html" || ext == L".htm" || ext == L".xhtml")
            return DocInput::Html;
        if (ext == L".rtf")
            return DocInput::Rtf;
        if (ext == L".pdf")
            return DocInput::Pdf;
        if (ext == L".pages")
            return DocInput::Pages;
        return DocInput::None;
    }

    const DocTarget* FindDocTarget(std::string_view id)
    {
        for (const auto& t : kTargets)
            if (id == t.id)
                return &t;
        return nullptr;
    }

    bool TargetFits(DocInput input, std::string_view target)
    {
        if (input == DocInput::None || !FindDocTarget(target))
            return false;
        // PDF already converts to TXT and Markdown in the PDF module, and to PDF is no change.
        if (input == DocInput::Pdf)
            return target == "docx" || target == "epub" || target == "html";
        // Plain text to Markdown would only escape characters.
        if (input == DocInput::Text && target == "md")
            return false;
        return target != SameFormat(input);
    }

    bool ReadDocument(const std::wstring& path, DocInput input, Document& doc, std::stop_token stop, std::wstring& error)
    {
        const auto dir = std::filesystem::path(path).parent_path().wstring();
        switch (input)
        {
        case DocInput::Rtfd:
            return ReadRtfd(path, doc, error);
        case DocInput::Pdf:
            return ReadPdfDocument(path, doc, stop, error);
        case DocInput::Pages:
        {
            std::string pdf;
            if (!PagesPreviewPdf(path, pdf, error))
                return false;
            const auto temp = TempFilePath(L"pages", L".pdf");   // a fresh name of our own
            auto remove = wil::scope_exit([&] { DeleteFileW(temp.c_str()); });
            if (!WriteFileBytes(temp, pdf, WriteMode::New))
            {
                error = L"cannot write a temporary file";
                return false;
            }
            return ReadPdfDocument(temp, doc, stop, error);
        }
        default:
            break;
        }
        std::string bytes;
        if (!ReadFileBytes(path, bytes, &error))
            return false;
        switch (input)
        {
        case DocInput::Docx:
        {
            std::string e;
            if (!ReadDocx(std::move(bytes), doc, e))
            {
                error = Widen(e);
                return false;
            }
            return true;
        }
        case DocInput::Markdown:
            ReadMarkdown(DecodeText(bytes), dir, doc);
            return true;
        case DocInput::Text:
            ReadPlainText(DecodeText(bytes), doc);
            return true;
        case DocInput::Html:
            ReadHtml(DecodeText(bytes), dir, doc);
            return true;
        case DocInput::Rtf:
            return ReadRtf(bytes, L"", doc, error);
        default:
            error = L"not a document";
            return false;
        }
    }

    bool DocumentPdf(const std::wstring& source, std::string& pdf, std::stop_token stop, const ProgressFn& progress, std::wstring& error)
    {
        const auto input = InputOf(source);
        if (progress)
            progress(0.02);
        // A Pages file's pages are its preview PDF itself.
        if (input == DocInput::Pages)
            return PagesPreviewPdf(source, pdf, error);
        if (input == DocInput::None || input == DocInput::Pdf)
        {
            error = L"not a document";
            return false;
        }
        Document doc;
        if (!ReadDocument(source, input, doc, stop, error))
        {
            if (error.empty())
                error = L"cannot read this document";
            return false;
        }
        if (stop.stop_requested())
        {
            error = L"cancelled";
            return false;
        }
        if (progress)
            progress(0.3);
        if (doc.title.empty())
            doc.title = TitleOf(doc, Narrow(std::filesystem::path(source).stem().wstring()));
        const ProgressFn inner = [&](double p) {
            if (progress)
                progress(0.3 + 0.7 * p);
        };
        return DocumentToPdf(doc, Widen(doc.title), false, 11, stop, inner, pdf, error);
    }

    FeatureModule Module()
    {
        return FeatureModule{ "docs", &RegisterActions, &Recipes, &MakeEngine };
    }
}
