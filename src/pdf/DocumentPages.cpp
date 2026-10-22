#include "pdf/DocumentPages.h"
#include "FileIo.h"
#include "PathUtil.h"
#include "docs/DocsModule.h"
#include "pdf/PdfModule.h"
#include "sheets/SheetsModule.h"

namespace sd::pdf
{
    PageSource PageSourceOf(const std::wstring& path, const office::Apps& apps)
    {
        if (HasPdfExtension(path))
            return PageSource::Pdf;
        if (office::Choose(apps, office::FamilyOf(path), "pdf") != office::Backend::None)
            return PageSource::Office;
        if (sheets::InputOf(path) != sheets::SheetInput::None)
            return PageSource::Spreadsheet;
        if (docs::InputOf(path) != docs::DocInput::None)
            return PageSource::Document;
        return PageSource::None;
    }

    office::Outcome LayOutPages(const std::wstring& source, const office::Apps& apps, const std::wstring& pdf, std::stop_token stop,
                                const ProgressFn& progress)
    {
        office::Outcome outcome;
        std::string bytes;
        switch (PageSourceOf(source, apps))
        {
        case PageSource::Office:
        {
            office::Job job;
            job.source = source;
            job.target = "pdf";
            job.output = pdf;
            job.stop = stop;
            job.progress = progress;
            const auto backend = office::Choose(apps, office::FamilyOf(source), "pdf");
            return backend == office::Backend::LibreOffice ? office::ConvertWithLibreOffice(apps.soffice, job) : office::ConvertWithOffice(backend, job);
        }
        case PageSource::Spreadsheet:
        {
            sheets::Workbook book;
            if (progress)
                progress(0.02);
            if (!sheets::ReadWorkbook(source, sheets::InputOf(source), book, outcome.error))
                return outcome;
            if (progress)
                progress(0.3);
            bool declined = false;
            if (!sheets::WorkbookPdf(source, book, L"Edit spreadsheet", stop, progress, bytes, outcome.error, declined))
            {
                outcome.cancelled = declined || stop.stop_requested();
                return outcome;
            }
            break;
        }
        case PageSource::Document:
            if (!docs::DocumentPdf(source, bytes, stop, progress, outcome.error))
            {
                outcome.cancelled = stop.stop_requested();
                return outcome;
            }
            break;
        default:
            outcome.error = L"not a document";
            return outcome;
        }
        if (stop.stop_requested())
        {
            outcome.cancelled = true;
            outcome.error = L"cancelled";
            return outcome;
        }
        outcome.ok = WriteFileBytes(pdf, bytes, WriteMode::New, &outcome.error);
        return outcome;
    }
}
