#pragma once
#include "Engine.h"
#include "docs/DocModel.h"
#include "sheets/Workbook.h"

#include <string>

// Spreadsheets: XLSX, XLS, ODS, CSV and TSV in; XLSX, ODS, CSV, TSV and a plain-table PDF out.
// One FeatureModule, one action, "sheets.convert".
namespace sd::sheets
{
    FeatureModule Module();

    enum class SheetInput
    {
        None,
        Xlsx,
        Xls,
        Ods,
        Csv,
        Tsv,
    };
    SheetInput InputOf(const std::wstring& path);
    bool TargetFits(SheetInput input, std::string_view target);
    bool ReadWorkbook(const std::wstring& path, SheetInput input, Workbook& book, std::wstring& error);

    // A PDF prints at most this many rows of a sheet unless the user asks for all of them: the
    // question comes first (SheetsModule.cpp) when a sheet is longer.
    constexpr size_t kPdfRowLimit = 20000;
    // Rows the PDF would leave out with `maxRows` rows per sheet.
    size_t RowsLeftOut(const Workbook& book, size_t maxRows = kPdfRowLimit);

    // The workbook as a document for the PDF layout: one table per sheet (its first row as the
    // header, repeated on every page), the sheet's name above it when there are several. At most
    // `maxRows` rows per sheet; a note at the very end says how many were left out.
    docs::Document WorkbookToDocument(const Workbook& book, size_t maxRows = kPdfRowLimit);
    // A sheet name made safe as a Windows file name ("Q1/Q2" -> "Q1_Q2", "CON" -> "_CON").
    std::wstring SafeFileName(const std::string& name);
}
