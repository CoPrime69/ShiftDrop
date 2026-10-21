#pragma once
#include "Engine.h"
#include "docs/PageLayout.h"

#include <memory>
#include <stop_token>
#include <string>

// Documents and spreadsheets to PDF without a bundled PDF engine: our own layout (PageLayout.h),
// measured with DirectWrite and printed through Windows' built-in "Microsoft Print to PDF" printer.
// The printer writes a real vector PDF with the fonts embedded and the text selectable.
//
// Silent printing: StartDoc with DOCINFO.lpszOutput set to a file path makes the printer write
// there instead of asking where to save. The spooler writes the file after EndDoc, so we wait
// for the job to leave the queue before reading it back.
namespace sd::docs
{
    // The "Microsoft Print to PDF" printer's name (found by its driver, so a renamed one still counts).
    // The answer is cached for 30 seconds: Accepts asks for every slice while the wheel opens.
    bool FindPdfPrinter(std::wstring* name, std::wstring* why);

    // Text measuring with DirectWrite (Segoe UI, Consolas for code), cached per word.
    std::unique_ptr<ITextMeasure> MakeDirectWriteMeasure(std::wstring& error);

    // Prints laid-out pages to PDF bytes. `landscape` turns the paper (wide spreadsheets).
    bool PrintPagesToPdf(const std::vector<Page>& pages, const Document& doc, const PageSetup& page, bool letter, bool landscape,
                         const std::wstring& title, std::stop_token stop, const ProgressFn& progress, std::string& pdf, std::wstring& error);

    // Layout + print in one step, on the user's paper size.
    bool DocumentToPdf(const Document& doc, const std::wstring& title, bool landscape, float bodySize, std::stop_token stop,
                       const ProgressFn& progress, std::string& pdf, std::wstring& error);
}
