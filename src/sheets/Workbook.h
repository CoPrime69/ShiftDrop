#pragma once
#include <string>
#include <string_view>
#include <vector>

// Spreadsheets (M11): one model for XLSX, ODS, XLS, CSV and TSV. Values only: formulas arrive as
// their last computed result, and styles are not kept (dates excepted, so they stay dates).
namespace sd::sheets
{
    enum class CellType
    {
        Empty,
        Number,
        Text,
        Bool,
        Error,   // "#N/A", "#DIV/0!"...: text holds it
        Date,    // number holds the serial date (days since 1899-12-30, fractions are the time)
    };

    struct Cell
    {
        CellType type = CellType::Empty;
        double number = 0;
        std::string text;   // UTF-8

        static Cell Num(double v) { return Cell{ CellType::Number, v, {} }; }
        static Cell Str(std::string s) { return Cell{ CellType::Text, 0, std::move(s) }; }
    };

    struct Sheet
    {
        std::string name;
        std::vector<std::vector<Cell>> rows;
        // Cells are untyped text (a CSV): writers of typed formats store "42" as the number 42.
        bool untyped = false;

        void Set(size_t row, size_t col, Cell cell);
        // Drops empty rows at the end and empty cells at the ends of rows.
        void Trim();
        size_t Columns() const;
    };

    struct Workbook
    {
        std::vector<Sheet> sheets;
    };

    // ---- Values as text ---------------------------------------------------------------------------
    // Shortest text that reads back as the same number ("3", "0.1", "1.5e-07").
    std::string FormatNumber(double v);
    // Serial date to ISO 8601: "2024-01-31", "2024-01-31 13:45:00", or "13:45:00" for a time alone.
    std::string SerialToIso(double serial);
    // ISO date or date-time ("2024-01-31", "2024-01-31T13:45:00") to a serial date.
    bool IsoToSerial(std::string_view iso, double& serial);
    // The cell as CSV and PDF show it.
    std::string CellText(const Cell& c);
    // "A" for 0, "Z" for 25, "AA" for 26.
    std::string ColumnName(size_t col);
    // "B12" -> row 11, col 1 (0-based). False if it is not a cell reference.
    bool ParseCellRef(std::string_view ref, size_t& row, size_t& col);
    // A plain number written as text in a CSV ("42", "-3.5", "1e6"), but not "007" or "+1 555".
    bool LooksNumeric(std::string_view s, double& value);

    // ---- Formats ----------------------------------------------------------------------------------
    // CSV/TSV: RFC 4180 quoting; `delimiter` 0 detects comma, semicolon or tab from the first line.
    // Text is UTF-8 (decode BOMs and UTF-16 first, docs::DecodeText).
    Sheet ReadCsv(std::string_view utf8, char delimiter = 0);
    // UTF-8 with a BOM (so Excel reads accents right), CRLF line ends, quotes only where needed.
    std::string WriteCsv(const Sheet& sheet, char delimiter);

    bool ReadXlsx(std::string bytes, Workbook& book, std::string& error);
    bool WriteXlsx(const Workbook& book, std::string& out);
    bool ReadOds(std::string bytes, Workbook& book, std::string& error);
    bool WriteOds(const Workbook& book, std::string& out);

    // Excel 97-2003 (.xls): the "Workbook" stream of an OLE compound file, read through Windows'
    // structured storage (StgOpenStorageOnILockBytes), then its BIFF8 records.
    bool ReadXls(const std::string& bytes, Workbook& book, std::string& error);
    bool ParseBiff8(std::string_view stream, Workbook& book, std::string& error);

    // Sheet names Excel accepts: at most 31 characters, none of []:*?/\, unique in the book.
    std::vector<std::string> SafeSheetNames(const Workbook& book);
}
