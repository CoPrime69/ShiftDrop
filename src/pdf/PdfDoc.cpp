#include "pdf/PdfDoc.h"
#include "Utf.h"

#include <qpdf/QPDFAcroFormDocumentHelper.hh>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <cstdio>
#include <filesystem>
#include <format>
#include <map>
#include <set>

namespace sd::pdf
{
    namespace
    {
        struct Cancelled
        {
        };

        std::string Description(const std::wstring& path)
        {
            return Narrow(std::filesystem::path(path).filename().wstring());
        }

        // The base's pages that are not in the output stay in its object table, and its outline,
        // named destinations or form fields can still point at them, which would carry their content
        // into the new file. So each one left out (deleted, or replaced by an edited copy such as a
        // redaction) is emptied, and the form fields on it are removed. When an edited copy replaced
        // one, the tag tree goes as well: its alternate texts may quote what was removed.
        template <class Placed>
        void EmptyLeftOutPages(QPDF& base, std::vector<QPDFPageObjectHelper>& basePages,
                               const std::map<std::pair<uint32_t, uint32_t>, Placed>& placed, bool replacedBasePage)
        {
            std::vector<QPDFPageObjectHelper> leftOut;
            for (uint32_t p = 0; p < basePages.size(); ++p)
                if (!placed.contains({ 0u, p }))
                    leftOut.push_back(basePages[p]);
            if (leftOut.empty())
                return;
            QPDFAcroFormDocumentHelper forms(base);
            std::set<QPDFObjGen> fields;
            if (forms.hasAcroForm())
                for (auto& page : leftOut)
                    for (auto& widget : forms.getWidgetAnnotationsForPage(page))
                    {
                        auto field = forms.getFieldForAnnotation(widget);
                        if (!field.isNull())
                            fields.insert(field.getObjectHandle().getObjGen());
                    }
            if (!fields.empty())
                forms.removeFormFields(fields);
            for (auto& page : leftOut)
            {
                auto dict = page.getObjectHandle();
                for (const char* key : { "/Annots", "/Thumb", "/B", "/PieceInfo", "/Metadata", "/AA", "/StructParents", "/Group" })
                    dict.removeKey(key);
                dict.replaceKey("/Contents", base.newStream(std::string()));
                dict.replaceKey("/Resources", QPDFObjectHandle::newDictionary());
            }
            if (replacedBasePage)
            {
                auto root = base.getRoot();
                root.removeKey("/StructTreeRoot");
                root.removeKey("/MarkInfo");
            }
        }
    }

    std::wstring QpdfError(const std::exception& e)
    {
        if (const auto* q = dynamic_cast<const QPDFExc*>(&e))
            if (!q->getMessageDetail().empty())
                return Widen(q->getMessageDetail());
        return Widen(e.what());
    }

    std::unique_ptr<QPDF> OpenPdf(const std::wstring& path, std::wstring& error)
    {
        FILE* file = _wfopen(path.c_str(), L"rb");
        if (!file)
        {
            error = L"cannot open the file";
            return nullptr;
        }
        auto pdf = std::make_unique<QPDF>();
        pdf->setSuppressWarnings(true);   // damaged files are repaired quietly; we only report failures
        try
        {
            pdf->processFile(Description(path).c_str(), file, true);   // QPDF owns and closes the FILE
        }
        catch (const QPDFExc& e)
        {
            error = e.getErrorCode() == qpdf_e_password ? L"the PDF is locked with a password" : QpdfError(e);
            return nullptr;
        }
        catch (const std::exception& e)
        {
            error = QpdfError(e);
            return nullptr;
        }
        return pdf;
    }

    bool WritePdf(QPDF& pdf, const std::wstring& path, const WriteOptions& options, std::wstring& error, std::stop_token stop,
                  const ProgressFn& progress)
    {
        FILE* file = _wfopen(path.c_str(), L"wb");
        if (!file)
        {
            error = L"cannot write next to the source";
            return false;
        }
        try
        {
            QPDFWriter writer(pdf, Description(path).c_str(), file, true);
            if (options.compact)
            {
                // Pack small objects into compressed object streams (PDF 1.5) and re-deflate every
                // stream QPDF can decode losslessly at the best zlib level.
                writer.setObjectStreamMode(qpdf_o_generate);
                writer.setCompressStreams(true);
                writer.setRecompressFlate(true);
                writer.setDecodeLevel(qpdf_dl_generalized);
            }
            writer.registerProgressReporter(std::make_shared<QPDFWriter::FunctionProgressReporter>([&](int percent) {
                if (stop.stop_requested())
                    throw Cancelled{};
                if (progress)
                    progress(percent / 100.0);
            }));
            writer.write();
        }
        catch (const Cancelled&)
        {
            error = kCancelled;
            return false;
        }
        catch (const std::exception& e)
        {
            error = QpdfError(e);
            return false;
        }
        return true;
    }

    bool CountPages(const std::vector<std::wstring>& files, std::vector<uint32_t>& counts, std::wstring& error)
    {
        counts.clear();
        for (const auto& f : files)
        {
            auto pdf = OpenPdf(f, error);
            if (!pdf)
            {
                error = std::format(L"{}: {}", std::filesystem::path(f).filename().wstring(), error);
                return false;
            }
            try
            {
                counts.push_back(static_cast<uint32_t>(pdf->getAllPages().size()));
            }
            catch (const std::exception& e)
            {
                error = std::format(L"{}: {}", std::filesystem::path(f).filename().wstring(), QpdfError(e));
                return false;
            }
        }
        return true;
    }

    bool AssemblePages(const std::vector<std::wstring>& sources, const PagePlan& plan, const std::wstring& out, std::stop_token stop,
                       const ProgressFn& progress, std::wstring& error)
    {
        if (sources.empty() || plan.empty())
        {
            error = L"no pages to write";
            return false;
        }
        // Every source stays open until the output is written: copied pages read their streams lazily.
        std::vector<std::unique_ptr<QPDF>> docs(sources.size());
        std::vector<std::vector<QPDFPageObjectHelper>> pages(sources.size());
        // Edited pages: each a one-page PDF of its own (PageEdits.h), opened once.
        std::map<std::wstring, std::unique_ptr<QPDF>> editedDocs;
        std::map<std::wstring, QPDFPageObjectHelper> editedPages;
        try
        {
            for (const auto& ref : plan)
            {
                if (ref.file >= sources.size())
                {
                    error = L"the page list names a file that was not dropped";
                    return false;
                }
                if (!ref.edited.empty())
                {
                    if (editedDocs.contains(ref.edited))
                        continue;
                    auto doc = OpenPdf(ref.edited, error);
                    if (!doc)
                    {
                        error = std::format(L"an edited page of {} is gone: {}", std::filesystem::path(sources[ref.file]).filename().wstring(), error);
                        return false;
                    }
                    auto all = QPDFPageDocumentHelper(*doc).getAllPages();
                    if (all.empty())
                    {
                        error = L"an edited page is empty";
                        return false;
                    }
                    editedPages.emplace(ref.edited, all.front());
                    editedDocs.emplace(ref.edited, std::move(doc));
                    continue;
                }
                if (!docs[ref.file])
                {
                    docs[ref.file] = OpenPdf(sources[ref.file], error);
                    if (!docs[ref.file])
                    {
                        error = std::format(L"{}: {}", std::filesystem::path(sources[ref.file]).filename().wstring(), error);
                        return false;
                    }
                    pages[ref.file] = QPDFPageDocumentHelper(*docs[ref.file]).getAllPages();
                }
                if (ref.page >= pages[ref.file].size())
                {
                    error = std::format(L"{} has no page {}", std::filesystem::path(sources[ref.file]).filename().wstring(), ref.page + 1);
                    return false;
                }
            }
            if (!docs[0])
            {
                // The base must be open even when none of its pages are kept.
                docs[0] = OpenPdf(sources[0], error);
                if (!docs[0])
                    return false;
                pages[0] = QPDFPageDocumentHelper(*docs[0]).getAllPages();
            }
            if (progress)
                progress(0.1);

            QPDF& base = *docs[0];
            QPDFPageDocumentHelper tree(base);
            for (auto& page : pages[0])
                tree.removePage(page);   // unlinked from the page tree, still alive for re-adding

            struct Placed
            {
                QPDFObjectHandle page;   // the page as it now lives in the base document
                int rotation = 0;        // its own /Rotate before we touched it
            };
            std::map<std::pair<uint32_t, uint32_t>, Placed> placed;
            std::map<std::wstring, Placed> placedEdits;
            bool replacedBasePage = false;
            for (size_t i = 0; i < plan.size(); ++i)
            {
                if (stop.stop_requested())
                {
                    error = kCancelled;
                    return false;
                }
                const auto& ref = plan[i];
                const bool edited = !ref.edited.empty();
                replacedBasePage |= edited && ref.file == 0;
                QPDFPageObjectHelper source = edited ? editedPages.at(ref.edited) : pages[ref.file][ref.page];
                Placed* already = nullptr;
                if (edited)
                {
                    if (auto it = placedEdits.find(ref.edited); it != placedEdits.end())
                        already = &it->second;
                }
                else if (auto it = placed.find({ ref.file, ref.page }); it != placed.end())
                    already = &it->second;
                if (!already)
                {
                    const int own = RotationOf(source);
                    // A foreign page passed to addPage is copied in (resources and all) by QPDF.
                    tree.addPage(source, false);
                    QPDFObjectHandle local = !edited && ref.file == 0 ? source.getObjectHandle() : base.getAllPages().back();
                    already = edited ? &placedEdits.emplace(ref.edited, Placed{ local, own }).first->second
                                     : &placed.emplace(std::make_pair(ref.file, ref.page), Placed{ local, own }).first->second;
                    if (ref.rotation)
                        QPDFPageObjectHelper(local).rotatePage((own + ref.rotation) % 360, false);
                }
                else
                {
                    // The same page again: a shallow copy shares its content streams and resources.
                    QPDFPageObjectHelper copy = QPDFPageObjectHelper(already->page).shallowCopyPage();
                    copy.rotatePage((already->rotation + ref.rotation) % 360, false);
                    tree.addPage(copy, false);
                }
                if (progress)
                    progress(0.1 + 0.3 * static_cast<double>(i + 1) / static_cast<double>(plan.size()));
            }
            EmptyLeftOutPages(base, pages[0], placed, replacedBasePage);
            return WritePdf(base, out, WriteOptions{}, error, stop, [&](double f) {
                if (progress)
                    progress(0.4 + 0.6 * f);
            });
        }
        catch (const std::exception& e)
        {
            error = QpdfError(e);
            return false;
        }
    }

    int RotationOf(QPDFPageObjectHelper& page)
    {
        auto rotate = page.getAttribute("/Rotate", false);
        const int r = rotate.isInteger() ? static_cast<int>(rotate.getIntValue()) : 0;
        return ((r % 360) + 360) % 360 / 90 * 90;
    }

    std::vector<std::string> StreamFilters(QPDFObjectHandle filter)
    {
        std::vector<std::string> out;
        if (filter.isName())
            out.push_back(filter.getName());
        else if (filter.isArray())
            for (int i = 0; i < filter.getArrayNItems(); ++i)
                if (filter.getArrayItem(i).isName())
                    out.push_back(filter.getArrayItem(i).getName());
        return out;
    }

    bool IsDctFilter(std::string_view f) { return f == "/DCTDecode" || f == "/DCT"; }

    bool IsLosslessFilter(std::string_view f)
    {
        return f == "/FlateDecode" || f == "/Fl" || f == "/LZWDecode" || f == "/LZW" || f == "/RunLengthDecode" || f == "/RL" ||
               f == "/ASCII85Decode" || f == "/A85" || f == "/ASCIIHexDecode" || f == "/AHx";
    }
}
