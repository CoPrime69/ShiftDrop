#include "sheets/Workbook.h"
#include "docs/Bytes.h"
#include "Utf.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>

namespace sd::sheets
{
    void Sheet::Set(size_t row, size_t col, Cell cell)
    {
        if (cell.type == CellType::Empty)
            return;
        if (rows.size() <= row)
            rows.resize(row + 1);
        auto& r = rows[row];
        if (r.size() <= col)
            r.resize(col + 1);
        r[col] = std::move(cell);
    }

    void Sheet::Trim()
    {
        for (auto& r : rows)
            while (!r.empty() && r.back().type == CellType::Empty)
                r.pop_back();
        while (!rows.empty() && rows.back().empty())
            rows.pop_back();
    }

    size_t Sheet::Columns() const
    {
        size_t n = 0;
        for (const auto& r : rows)
            n = std::max(n, r.size());
        return n;
    }

    namespace
    {
        // Days between 1970-01-01 and a civil date, and back (proleptic Gregorian; H. Hinnant).
        long long DaysFromCivil(long long y, unsigned m, unsigned d)
        {
            y -= m <= 2;
            const long long era = (y >= 0 ? y : y - 399) / 400;
            const auto yoe = static_cast<unsigned>(y - era * 400);
            const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
            const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            return era * 146097 + static_cast<long long>(doe) - 719468;
        }

        void CivilFromDays(long long z, long long& y, unsigned& m, unsigned& d)
        {
            z += 719468;
            const long long era = (z >= 0 ? z : z - 146096) / 146097;
            const auto doe = static_cast<unsigned>(z - era * 146097);
            const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
            y = static_cast<long long>(yoe) + era * 400;
            const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
            const unsigned mp = (5 * doy + 2) / 153;
            d = doy - (153 * mp + 2) / 5 + 1;
            m = mp < 10 ? mp + 3 : mp - 9;
            y += m <= 2;
        }

        // Excel's day 0 is 1899-12-30 (for every date after its fictitious 1900-02-29, serial 60).
        constexpr long long kSerialToUnix = 25569;
    }

    std::string FormatNumber(double v)
    {
        if (!std::isfinite(v))
            return "#NUM!";
        if (v == std::trunc(v) && std::fabs(v) < 1e15)
            return std::format("{}", static_cast<long long>(v));
        return std::format("{}", v);   // shortest text that round-trips
    }

    std::string SerialToIso(double serial)
    {
        if (!std::isfinite(serial) || serial < 0 || serial > 2958465)   // 9999-12-31
            return FormatNumber(serial);
        auto days = static_cast<long long>(std::floor(serial));
        auto seconds = static_cast<long long>(std::llround((serial - static_cast<double>(days)) * 86400.0));
        if (seconds >= 86400)
        {
            ++days;
            seconds -= 86400;
        }
        const auto time = std::format("{:02}:{:02}:{:02}", seconds / 3600, seconds / 60 % 60, seconds % 60);
        if (days == 0)
            return time;
        long long y;
        unsigned m, d;
        CivilFromDays(days - kSerialToUnix + (days < 61 ? 1 : 0), y, m, d);
        const auto date = std::format("{:04}-{:02}-{:02}", y, m, d);
        return seconds == 0 ? date : date + " " + time;
    }

    bool IsoToSerial(std::string_view s, double& serial)
    {
        int y = 0, mo = 0, d = 0, h = 0, mi = 0;
        double sec = 0;
        const auto num = [&](size_t at, size_t len, int& out) {
            if (at + len > s.size())
                return false;
            const auto r = std::from_chars(s.data() + at, s.data() + at + len, out);
            return r.ec == std::errc() && r.ptr == s.data() + at + len;
        };
        if (s.size() < 10 || s[4] != '-' || s[7] != '-' || !num(0, 4, y) || !num(5, 2, mo) || !num(8, 2, d) || mo < 1 || mo > 12 || d < 1 || d > 31)
            return false;
        if (s.size() >= 16 && (s[10] == 'T' || s[10] == ' '))
        {
            if (!num(11, 2, h) || s[13] != ':' || !num(14, 2, mi))
                return false;
            if (s.size() >= 19 && s[16] == ':')
            {
                size_t end = 19;
                while (end < s.size() && (std::isdigit(static_cast<unsigned char>(s[end])) || s[end] == '.'))
                    ++end;
                std::from_chars(s.data() + 17, s.data() + end, sec);
            }
        }
        const long long days = DaysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) + kSerialToUnix;
        serial = static_cast<double>(days < 61 ? days - 1 : days) + (h * 3600 + mi * 60 + sec) / 86400.0;
        return true;
    }

    std::string CellText(const Cell& c)
    {
        switch (c.type)
        {
        case CellType::Empty: return {};
        case CellType::Number: return FormatNumber(c.number);
        case CellType::Text: return c.text;
        case CellType::Error: return c.text;
        case CellType::Bool: return c.number != 0 ? "TRUE" : "FALSE";
        case CellType::Date: return SerialToIso(c.number);
        }
        return {};
    }

    std::string ColumnName(size_t col)
    {
        std::string name;
        ++col;
        while (col > 0)
        {
            const size_t rem = (col - 1) % 26;
            name.insert(name.begin(), static_cast<char>('A' + rem));
            col = (col - 1) / 26;
        }
        return name;
    }

    bool ParseCellRef(std::string_view ref, size_t& row, size_t& col)
    {
        size_t i = 0, c = 0, r = 0;
        while (i < ref.size() && ref[i] == '$')
            ++i;
        const size_t letters = i;
        while (i < ref.size() && std::isalpha(static_cast<unsigned char>(ref[i])))
            c = c * 26 + static_cast<size_t>(std::toupper(static_cast<unsigned char>(ref[i++])) - 'A' + 1);
        if (i == letters || c == 0 || i - letters > 3)
            return false;
        while (i < ref.size() && ref[i] == '$')
            ++i;
        const size_t digits = i;
        while (i < ref.size() && std::isdigit(static_cast<unsigned char>(ref[i])))
            r = r * 10 + static_cast<size_t>(ref[i++] - '0');
        if (i == digits || i != ref.size() || r == 0)
            return false;
        row = r - 1;
        col = c - 1;
        return true;
    }

    bool LooksNumeric(std::string_view s, double& value)
    {
        if (s.empty() || s.size() > 24)
            return false;
        size_t i = s[0] == '-' ? 1 : 0;
        const size_t intStart = i;
        size_t digits = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
            ++i, ++digits;
        const size_t intDigits = i - intStart;
        if (intDigits > 1 && s[intStart] == '0')
            return false;   // "007", "0123": codes, not numbers
        if (i < s.size() && s[i] == '.')
        {
            ++i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
                ++i, ++digits;
        }
        if (digits == 0)
            return false;
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E'))
        {
            ++i;
            if (i < s.size() && (s[i] == '-' || s[i] == '+'))
                ++i;
            const size_t exp = i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
                ++i;
            if (i == exp)
                return false;
        }
        if (i != s.size() || digits > 15)
            return false;   // more digits than a double keeps: an ID, keep it as text
        return std::from_chars(s.data(), s.data() + s.size(), value).ec == std::errc();
    }

    std::vector<std::string> SafeSheetNames(const Workbook& book)
    {
        std::vector<std::string> names;
        for (size_t i = 0; i < book.sheets.size(); ++i)
        {
            auto w = Widen(book.sheets[i].name);
            for (auto& c : w)
                if (c == L'[' || c == L']' || c == L':' || c == L'*' || c == L'?' || c == L'/' || c == L'\\' || c < 0x20)
                    c = L'_';
            while (!w.empty() && w.front() == L'\'')
                w.erase(0, 1);
            if (w.size() > 31)
                w.resize(31);
            if (w.empty())
                w = std::format(L"Sheet{}", i + 1);
            auto candidate = w;
            for (int n = 2;; ++n)
            {
                const auto lower = docs::AsciiLower(Narrow(candidate));
                if (std::none_of(names.begin(), names.end(), [&](const std::string& s) { return docs::AsciiLower(s) == lower; }))
                    break;
                const auto tail = std::format(L" ({})", n);
                candidate = w.substr(0, 31 - tail.size()) + tail;
            }
            names.push_back(Narrow(candidate));
        }
        return names;
    }

    // ---- CSV ----------------------------------------------------------------------------------------

    Sheet ReadCsv(std::string_view s, char delimiter)
    {
        if (delimiter == 0)
        {
            // The separator seen most on the first line, outside quotes.
            size_t comma = 0, semi = 0, tab = 0;
            bool quoted = false;
            for (const char c : s)
            {
                if (c == '"')
                    quoted = !quoted;
                else if (!quoted && (c == '\n' || c == '\r'))
                    break;
                else if (!quoted)
                {
                    comma += c == ',';
                    semi += c == ';';
                    tab += c == '\t';
                }
            }
            delimiter = tab > comma && tab >= semi ? '\t' : semi > comma ? ';' : ',';
        }
        Sheet sheet;
        sheet.untyped = true;
        std::vector<Cell> row;
        std::string field;
        bool quoted = false, wasQuoted = false, any = false;
        const auto endField = [&] {
            if (field.empty() && !wasQuoted)
                row.emplace_back();
            else
                row.push_back(Cell::Str(std::move(field)));
            field.clear();
            wasQuoted = false;
        };
        const auto endRow = [&] {
            endField();
            sheet.rows.push_back(std::move(row));
            row.clear();
        };
        for (size_t i = 0; i < s.size(); ++i)
        {
            const char c = s[i];
            any = true;
            if (quoted)
            {
                if (c == '"')
                {
                    if (i + 1 < s.size() && s[i + 1] == '"')
                    {
                        field.push_back('"');
                        ++i;
                    }
                    else
                        quoted = false;
                }
                else if (c == '\r')
                {
                    // Line ends inside a quoted field become "\n", as cells hold them.
                    if (i + 1 < s.size() && s[i + 1] == '\n')
                        ++i;
                    field.push_back('\n');
                }
                else
                    field.push_back(c);
                continue;
            }
            if (c == '"' && field.empty())
            {
                quoted = true;
                wasQuoted = true;
            }
            else if (c == delimiter)
                endField();
            else if (c == '\r' || c == '\n')
            {
                if (c == '\r' && i + 1 < s.size() && s[i + 1] == '\n')
                    ++i;
                endRow();
                any = false;
            }
            else
                field.push_back(c);
        }
        if (any)
            endRow();
        sheet.Trim();
        return sheet;
    }

    std::string WriteCsv(const Sheet& sheet, char delimiter)
    {
        std::string out = "\xEF\xBB\xBF";
        const size_t cols = sheet.Columns();
        for (const auto& row : sheet.rows)
        {
            for (size_t c = 0; c < cols; ++c)
            {
                if (c)
                    out.push_back(delimiter);
                if (c >= row.size())
                    continue;
                const auto text = CellText(row[c]);
                const bool quote = text.find_first_of(std::string("\"\r\n") + delimiter) != std::string::npos ||
                                   (!text.empty() && (text.front() == ' ' || text.back() == ' '));
                if (!quote)
                {
                    out += text;
                    continue;
                }
                out.push_back('"');
                for (const char ch : text)
                {
                    if (ch == '"')
                        out.push_back('"');
                    out.push_back(ch);
                }
                out.push_back('"');
            }
            out += "\r\n";
        }
        return out;
    }
}
