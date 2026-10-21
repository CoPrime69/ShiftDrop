#include "sheets/SheetsModule.h"
#include "Ask.h"
#include "FileIo.h"
#include "FormatText.h"
#include "PathUtil.h"
#include "Strings.h"
#include "docs/Bytes.h"
#include "docs/PdfPrint.h"
#include "OutputPath.h"
#include "Utf.h"

#include <windows.h>
#include <wil/resource.h>

#include <filesystem>
#include <format>
#include <set>

namespace sd::sheets
{
    namespace
    {
        struct Target
        {
            const char* id;
            const wchar_t* extension;
        };
        const Target kTargets[] = { { "xlsx", L".xlsx" }, { "ods", L".ods" }, { "csv", L".csv" }, { "tsv", L".tsv" }, { "pdf", L".pdf" } };

        const Target* FindTarget(std::string_view id)
        {
            for (const auto& t : kTargets)
                if (id == t.id)
                    return &t;
            return nullptr;
        }

        enum RowsAnswer
        {
            kAllRows = 1,
            kFirstRows = 2,
        };

        // A PDF of a very long sheet takes a while and runs to thousands of pages: ask first.
        int AskRows(const std::wstring& source, const Workbook& book, std::stop_token stop)
        {
            size_t total = 0, cut = 0;
            for (const auto& s : book.sheets)
            {
                total += s.rows.size();
                cut += s.rows.size() > kPdfRowLimit;
            }
            const auto left = RowsLeftOut(book);
            ask::Question q;
            q.title = L"Spreadsheet to PDF";
            q.heading = std::format(L"{} has {} rows", FileName(source), Thousands(total));
            q.text = L"Printing every row can take a while and makes a long PDF.";
            q.choices = { { kAllRows, L"Print all rows", std::format(L"All {} rows", Thousands(total)) },
                          { kFirstRows, std::format(L"Print the first {} rows{}", Thousands(kPdfRowLimit), cut > 1 ? L" of each sheet" : L""),
                            std::format(L"A note on the last page says {} {} left out", Thousands(left), left == 1 ? L"row was" : L"rows were") } };
            q.unattended = kFirstRows;   // what it did before it asked
            return ask::Choose(q, stop);
        }

        class SheetsEngine final : public IEngine
        {
        public:
            explicit SheetsEngine(const EngineServices& services) : m_history(services.history) {}

            bool Handles(const Recipe& recipe) const override
            {
                return recipe.steps.size() == 1 && recipe.steps[0].action == "sheets.convert";
            }

            bool Applies(const Recipe& recipe, const std::wstring& path) const override
            {
                return TargetFits(InputOf(path), recipe.FirstParam("format"));
            }

            bool Accepts(const Recipe& recipe, const std::wstring& path, std::wstring* why) const override
            {
                const auto target = recipe.FirstParam("format");
                if (!TargetFits(InputOf(path), target))
                {
                    if (why)
                        *why = L"not a spreadsheet";
                    return false;
                }
                if (!PathExists(path))
                {
                    if (why)
                        *why = L"file not found";
                    return false;
                }
                if (target == "pdf" && !docs::FindPdfPrinter(nullptr, why))
                    return false;
                return true;
            }

            ItemResult Run(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop, const ProgressFn& progress) const override
            {
                const auto target = recipe.FirstParam("format");
                const auto* t = FindTarget(target);
                Workbook book;
                std::wstring error;
                if (progress)
                    progress(0.02);
                if (!ReadWorkbook(source, InputOf(source), book, error))
                {
                    StagedOutput out(m_history, runId, source);
                    return out.Fail(error);
                }
                if (stop.stop_requested())
                {
                    StagedOutput out(m_history, runId, source);
                    return out.Fail(L"cancelled", true);
                }
                if (progress)
                    progress(0.3);

                size_t maxRows = kPdfRowLimit;
                if (target == "pdf" && RowsLeftOut(book) > 0)
                {
                    const int answer = AskRows(source, book, stop);
                    if (answer != kAllRows && answer != kFirstRows)
                    {
                        StagedOutput out(m_history, runId, source);
                        return out.Fail(L"cancelled, nothing was printed", true);
                    }
                    if (answer == kAllRows)
                        maxRows = SIZE_MAX;
                }

                // Several sheets to CSV/TSV: one file per sheet in a folder.
                const bool text = target == "csv" || target == "tsv";
                std::vector<const Sheet*> sheets;
                for (const auto& s : book.sheets)
                    if (!s.rows.empty())
                        sheets.push_back(&s);
                if (sheets.empty() && !book.sheets.empty())
                    sheets.push_back(&book.sheets.front());
                const bool folder = text && sheets.size() > 1;
                StagedOutput out(m_history, runId, source, folder);
                if (!out.Ready())
                    return out.Fail(out.Error());

                const char delimiter = target == "tsv" ? '\t' : ',';
                if (folder)
                {
                    std::set<std::wstring> used;
                    for (const auto* s : sheets)
                    {
                        auto name = SafeFileName(s->name);
                        auto unique = name;
                        for (int n = 2; !used.insert(Widen(AsciiLower(Narrow(unique)))).second; ++n)
                            unique = std::format(L"{} ({})", name, n);
                        if (!WriteFileBytes((std::filesystem::path(out.Path()) / (unique + t->extension)).wstring(), WriteCsv(*s, delimiter), WriteMode::New, &error))
                            return out.Fail(error);
                    }
                    if (progress)
                        progress(1.0);
                    return out.Commit(recipe.suffix, L"");
                }

                std::string bytes;
                if (text)
                    bytes = sheets.empty() ? std::string("\xEF\xBB\xBF") : WriteCsv(*sheets.front(), delimiter);
                else if (target == "xlsx")
                {
                    if (!WriteXlsx(book, bytes))
                        return out.Fail(L"cannot build the XLSX package");
                }
                else if (target == "ods")
                {
                    if (!WriteOds(book, bytes))
                        return out.Fail(L"cannot build the ODS package");
                }
                else if (target == "pdf")
                {
                    const auto doc = WorkbookToDocument(book, maxRows);
                    size_t cols = 0;
                    for (const auto* s : sheets)
                        cols = std::max(cols, s->Columns());
                    const ProgressFn inner = [&](double p) {
                        if (progress)
                            progress(0.3 + 0.7 * p);
                    };
                    if (!docs::DocumentToPdf(doc, std::filesystem::path(source).stem().wstring(), cols > 6, cols > 10 ? 8.f : 9.5f, stop, inner, bytes, error))
                        return out.Fail(error, stop.stop_requested());
                }
                else
                    return out.Fail(L"unknown output format");
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
            registry.Add(ActionDescriptor{ "sheets.convert", L"Convert spreadsheet", StepKind::Engine,
                                           { ParamSpec{ "format", ParamType::Enum, 0, 0, { "xlsx", "ods", "csv", "tsv", "pdf" }, "xlsx" } } });
        }

        const char* Recipes()
        {
            // Icons (Segoe Fluent Icons): Calculator E8EF, BulletedList E8FD, PDF EA90.
            return R"([
  { "id": "sheets-to-xlsx", "label": "XLSX", "description": "Excel workbook", "icon": "E8EF",
    "steps": [ { "action": "sheets.convert", "params": { "format": "xlsx" } } ] },
  { "id": "sheets-to-ods", "label": "ODS", "description": "OpenDocument spreadsheet", "icon": "E8EF",
    "steps": [ { "action": "sheets.convert", "params": { "format": "ods" } } ] },
  { "id": "sheets-to-csv", "label": "CSV", "description": "Comma-separated values, one file per sheet", "icon": "E8FD",
    "steps": [ { "action": "sheets.convert", "params": { "format": "csv" } } ] },
  { "id": "sheets-to-tsv", "label": "TSV", "description": "Tab-separated values, one file per sheet", "icon": "E8FD",
    "steps": [ { "action": "sheets.convert", "params": { "format": "tsv" } } ] },
  { "id": "sheets-to-pdf", "label": "PDF", "description": "The sheets as printed tables", "icon": "EA90",
    "steps": [ { "action": "sheets.convert", "params": { "format": "pdf" } } ] }
])";
        }

        std::unique_ptr<IEngine> MakeEngine(const EngineServices& services)
        {
            return std::make_unique<SheetsEngine>(services);
        }
    }

    SheetInput InputOf(const std::wstring& path)
    {
        if (IsFolder(path))
            return SheetInput::None;
        const auto ext = LowerExtension(path);
        if (ext == L".xlsx" || ext == L".xlsm")
            return SheetInput::Xlsx;
        if (ext == L".xls")
            return SheetInput::Xls;
        if (ext == L".ods")
            return SheetInput::Ods;
        if (ext == L".csv")
            return SheetInput::Csv;
        if (ext == L".tsv" || ext == L".tab")
            return SheetInput::Tsv;
        return SheetInput::None;
    }

    bool TargetFits(SheetInput input, std::string_view target)
    {
        if (input == SheetInput::None || !FindTarget(target))
            return false;
        switch (input)
        {
        case SheetInput::Ods: return target != "ods";
        case SheetInput::Csv: return target != "csv";
        case SheetInput::Tsv: return target != "tsv";
        default: return true;   // .xlsm -> .xlsx drops the macros; .xls -> .xlsx modernises
        }
    }

    bool ReadWorkbook(const std::wstring& path, SheetInput input, Workbook& book, std::wstring& error)
    {
        std::string bytes;
        if (!ReadFileBytes(path, bytes, &error))
            return false;
        std::string e;
        bool ok = true;
        switch (input)
        {
        case SheetInput::Xlsx: ok = ReadXlsx(std::move(bytes), book, e); break;
        case SheetInput::Xls: ok = ReadXls(bytes, book, e); break;
        case SheetInput::Ods: ok = ReadOds(std::move(bytes), book, e); break;
        case SheetInput::Csv:
        case SheetInput::Tsv:
        {
            auto sheet = ReadCsv(docs::DecodeText(bytes), input == SheetInput::Tsv ? '\t' : 0);
            sheet.name = Narrow(std::filesystem::path(path).stem().wstring());
            book.sheets.push_back(std::move(sheet));
            break;
        }
        default:
            e = "not a spreadsheet";
            ok = false;
        }
        if (!ok)
            error = Widen(e);
        return ok;
    }

    size_t RowsLeftOut(const Workbook& book, size_t maxRows)
    {
        size_t left = 0;
        for (const auto& s : book.sheets)
            left += s.rows.size() > maxRows ? s.rows.size() - maxRows : 0;
        return left;
    }

    docs::Document WorkbookToDocument(const Workbook& book, size_t maxRows)
    {
        docs::Document doc;
        size_t withData = 0;
        for (const auto& s : book.sheets)
            withData += !s.rows.empty();
        for (const auto& s : book.sheets)
        {
            if (s.rows.empty())
                continue;
            if (withData > 1)
            {
                docs::Block h;
                h.kind = docs::BlockKind::Heading;
                h.level = 2;
                h.spans.push_back(docs::Span{ s.name });
                doc.blocks.push_back(std::move(h));
            }
            docs::Block t;
            t.kind = docs::BlockKind::Table;
            t.headerRow = s.rows.size() > 1;
            const size_t cols = s.Columns();
            for (size_t r = 0; r < s.rows.size() && r < maxRows; ++r)
            {
                std::vector<docs::TableCell> row(cols);
                for (size_t c = 0; c < s.rows[r].size(); ++c)
                    if (auto text = CellText(s.rows[r][c]); !text.empty())
                        row[c].spans.push_back(docs::Span{ std::move(text) });
                t.rows.push_back(std::move(row));
            }
            doc.blocks.push_back(std::move(t));
        }
        // One note after everything, so it is on the last page.
        if (const size_t left = RowsLeftOut(book, maxRows); left > 0)
        {
            std::vector<const Sheet*> cut;
            for (const auto& s : book.sheets)
                if (s.rows.size() > maxRows)
                    cut.push_back(&s);
            const std::wstring which = cut.size() > 1 ? std::wstring(L"each sheet") : withData > 1 ? std::format(L"\u201c{}\u201d", Widen(cut.front()->name))
                                                                                                : std::wstring(L"the sheet");
            docs::Block note;
            note.spans.push_back(docs::Span{ Narrow(std::format(L"{} {} left out: only the first {} rows of {} are printed.", Thousands(left),
                                                                left == 1 ? L"row was" : L"rows were", Thousands(maxRows), which)) });
            note.spans.back().italic = true;
            doc.blocks.push_back(std::move(note));
        }
        return doc;
    }

    std::wstring SafeFileName(const std::string& name)
    {
        auto w = Widen(name);
        for (auto& c : w)
            if (c < 0x20 || wcschr(L"<>:\"/\\|?*", c))
                c = L'_';
        while (!w.empty() && (w.back() == L'.' || w.back() == L' '))
            w.pop_back();
        if (w.empty())
            w = L"Sheet";
        // Device names can't be file names, even with an extension.
        static const wchar_t* kReserved[] = { L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"LPT1", L"LPT2", L"LPT3" };
        for (const auto* r : kReserved)
            if (EqualsNoCase(w, r))
                return L"_" + w;
        if (w.size() > 120)
            w.resize(120);
        return w;
    }

    FeatureModule Module()
    {
        return FeatureModule{ "sheets", &RegisterActions, &Recipes, &MakeEngine };
    }
}
