#include "pdf/PdfDoc.h"
#include "PathUtil.h"
#include "Utf.h"

#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <cstdio>
#include <format>
#include <map>

namespace sd::pdf
{
    namespace
    {
        struct Cancelled
        {
        };

        std::string Description(const std::wstring& path)
        {
            return Narrow(FileName(path));
        }

        int RotationOf(QPDFPageObjectHelper& page)
        {
            // /Rotate is inheritable from the page tree; a missing or odd value counts as 0.
            auto rotate = page.getAttribute("/Rotate", false);
            const int r = rotate.isInteger() ? static_cast<int>(rotate.getIntValue()) : 0;
            return ((r % 360) + 360) % 360 / 90 * 90;
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
                error = std::format(L"{}: {}", FileName(f), error);
                return false;
            }
            try
            {
                counts.push_back(static_cast<uint32_t>(pdf->getAllPages().size()));
            }
            catch (const std::exception& e)
            {
                error = std::format(L"{}: {}", FileName(f), QpdfError(e));
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
        try
        {
            for (const auto& ref : plan)
            {
                if (ref.file >= sources.size())
                {
                    error = L"the page list names a file that was not dropped";
                    return false;
                }
                if (!docs[ref.file])
                {
                    docs[ref.file] = OpenPdf(sources[ref.file], error);
                    if (!docs[ref.file])
                    {
                        error = std::format(L"{}: {}", FileName(sources[ref.file]), error);
                        return false;
                    }
                    pages[ref.file] = QPDFPageDocumentHelper(*docs[ref.file]).getAllPages();
                }
                if (ref.page >= pages[ref.file].size())
                {
                    error = std::format(L"{} has no page {}", FileName(sources[ref.file]), ref.page + 1);
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
            for (size_t i = 0; i < plan.size(); ++i)
            {
                if (stop.stop_requested())
                {
                    error = kCancelled;
                    return false;
                }
                const auto& ref = plan[i];
                const auto key = std::make_pair(ref.file, ref.page);
                QPDFPageObjectHelper source = pages[ref.file][ref.page];
                auto it = placed.find(key);
                if (it == placed.end())
                {
                    const int own = RotationOf(source);
                    // A foreign page passed to addPage is copied in (resources and all) by QPDF.
                    tree.addPage(source, false);
                    QPDFObjectHandle local = ref.file == 0 ? source.getObjectHandle() : base.getAllPages().back();
                    it = placed.emplace(key, Placed{ local, own }).first;
                    if (ref.rotation)
                        QPDFPageObjectHelper(local).rotatePage((own + ref.rotation) % 360, false);
                }
                else
                {
                    // The same page again: a shallow copy shares its content streams and resources.
                    QPDFPageObjectHelper copy = QPDFPageObjectHelper(it->second.page).shallowCopyPage();
                    copy.rotatePage((it->second.rotation + ref.rotation) % 360, false);
                    tree.addPage(copy, false);
                }
                if (progress)
                    progress(0.1 + 0.3 * static_cast<double>(i + 1) / static_cast<double>(plan.size()));
            }
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
}
