#include "sheets/Workbook.h"
#include "docs/Bytes.h"
#include "Utf.h"

#include <windows.h>
#include <objbase.h>
#include <wil/resource.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <format>
#include <map>

// Excel 97-2003 workbooks (BIFF8, [MS-XLS]) read with our own record parser. The file is an OLE
// compound document; Windows' structured storage opens it and hands us the "Workbook" stream, so
// no third-party library is needed (libxls is not in the vcpkg baseline we pin).
namespace sd::sheets
{
    namespace
    {
        uint16_t U16(std::string_view d, size_t at) { return at + 2 <= d.size() ? static_cast<uint16_t>(static_cast<uint8_t>(d[at]) | static_cast<uint8_t>(d[at + 1]) << 8) : 0; }
        uint32_t U32(std::string_view d, size_t at) { return at + 4 <= d.size() ? static_cast<uint32_t>(U16(d, at)) | static_cast<uint32_t>(U16(d, at + 2)) << 16 : 0; }
        double F64(std::string_view d, size_t at)
        {
            double v = 0;
            if (at + 8 <= d.size())
                std::memcpy(&v, d.data() + at, 8);
            return v;
        }

        double Rk(uint32_t rk)
        {
            double v;
            if (rk & 2)
                v = static_cast<double>(static_cast<int32_t>(rk) >> 2);
            else
            {
                const uint64_t bits = static_cast<uint64_t>(rk & 0xFFFFFFFCu) << 32;
                std::memcpy(&v, &bits, 8);
            }
            return (rk & 1) ? v / 100 : v;
        }

        std::string ErrorText(uint8_t code)
        {
            switch (code)
            {
            case 0x00: return "#NULL!";
            case 0x07: return "#DIV/0!";
            case 0x0F: return "#VALUE!";
            case 0x17: return "#REF!";
            case 0x1D: return "#NAME?";
            case 0x24: return "#NUM!";
            case 0x2A: return "#N/A";
            default: return "#ERROR";
            }
        }

        // Characters of an XLUnicodeString body: compressed (one byte each, Latin-1) or UTF-16LE.
        std::string Chars(std::string_view d, size_t at, size_t count, bool wide, size_t* end = nullptr)
        {
            std::wstring w;
            w.reserve(count);
            size_t i = at;
            for (size_t k = 0; k < count && i < d.size(); ++k)
            {
                if (wide)
                {
                    w.push_back(static_cast<wchar_t>(U16(d, i)));
                    i += 2;
                }
                else
                    w.push_back(static_cast<wchar_t>(static_cast<uint8_t>(d[i++])));
            }
            if (end)
                *end = i;
            return Narrow(w);
        }

        // XLUnicodeString with a 16-bit count (LABEL, STRING, FORMAT).
        std::string UnicodeString16(std::string_view d, size_t at)
        {
            const size_t cch = U16(d, at);
            const bool wide = at + 2 < d.size() && (static_cast<uint8_t>(d[at + 2]) & 1);
            return Chars(d, at + 3, cch, wide);
        }

        struct Record
        {
            uint16_t type;
            std::string_view data;
            size_t offset;   // of the record header in the stream
        };

        // Shared strings: the SST record and its CONTINUE records. A string's characters may break
        // across a CONTINUE; the continuation then starts with a fresh flags byte for the rest.
        std::vector<std::string> ReadSst(const std::vector<std::string_view>& parts)
        {
            std::vector<std::string> out;
            if (parts.empty() || parts[0].size() < 8)
                return out;
            size_t part = 0, pos = 8;
            // A string's header is never split: if it doesn't fit, it starts the next CONTINUE.
            const auto need = [&](size_t n) {
                while (part < parts.size() && pos + n > parts[part].size())
                {
                    ++part;
                    pos = 0;
                }
                return part < parts.size();
            };
            const uint32_t unique = U32(parts[0], 4);
            for (uint32_t s = 0; s < unique && s < 4'000'000; ++s)
            {
                if (!need(3))
                    break;
                auto d = parts[part];
                const size_t cch = U16(d, pos);
                const uint8_t flags = static_cast<uint8_t>(d[pos + 2]);
                pos += 3;
                bool wide = flags & 1;
                size_t runs = 0, ext = 0;
                if (flags & 8)
                {
                    runs = U16(d, pos);
                    pos += 2;
                }
                if (flags & 4)
                {
                    ext = U32(d, pos);
                    pos += 4;
                }
                std::wstring text;
                size_t left = cch;
                while (left > 0 && part < parts.size())
                {
                    d = parts[part];
                    const size_t avail = (d.size() - std::min(pos, d.size())) / (wide ? 2 : 1);
                    const size_t take = std::min(left, avail);
                    for (size_t k = 0; k < take; ++k)
                    {
                        text.push_back(static_cast<wchar_t>(wide ? U16(d, pos) : static_cast<uint8_t>(d[pos])));
                        pos += wide ? 2 : 1;
                    }
                    left -= take;
                    if (left > 0)
                    {
                        // Next CONTINUE: one flags byte, then the remaining characters.
                        ++part;
                        if (part >= parts.size())
                            break;
                        pos = 0;
                        wide = static_cast<uint8_t>(parts[part][0]) & 1;
                        pos = 1;
                    }
                }
                out.push_back(Narrow(text));
                // Skip formatting runs (4 bytes each) and the extended data, which may also cross records.
                size_t skip = runs * 4 + ext;
                while (skip > 0 && part < parts.size())
                {
                    const size_t avail = parts[part].size() - std::min(pos, parts[part].size());
                    const size_t step = std::min(skip, avail);
                    pos += step;
                    skip -= step;
                    if (skip > 0)
                    {
                        ++part;
                        pos = 0;
                    }
                }
            }
            return out;
        }

        bool IsDateCode(const std::string& code)
        {
            bool quoted = false, bracket = false;
            for (size_t i = 0; i < code.size(); ++i)
            {
                const char c = code[i];
                if (c == '"') quoted = !quoted;
                else if (quoted) continue;
                else if (c == '[') bracket = true;
                else if (c == ']') bracket = false;
                else if (bracket) continue;
                else if (c == '\\' || c == '_' || c == '*') ++i;
                else if (std::strchr("dDmMyYhHsS", c)) return docs::AsciiLower(code) != "general";
            }
            return false;
        }
    }

    bool ParseBiff8(std::string_view s, Workbook& book, std::string& error)
    {
        std::vector<Record> records;
        for (size_t at = 0; at + 4 <= s.size();)
        {
            const uint16_t type = U16(s, at), len = U16(s, at + 2);
            if (at + 4 + len > s.size())
                break;
            records.push_back({ type, s.substr(at + 4, len), at });
            at += 4 + len;
        }
        if (records.empty() || records[0].type != 0x0809)
        {
            error = records.empty() || (records[0].type != 0x0209 && records[0].type != 0x0409) ? "not an Excel workbook"
                                                                                                  : "Excel files older than Excel 97 are not supported";
            return false;
        }
        if (U16(records[0].data, 0) != 0x0600)
        {
            error = "Excel 95 and older workbooks are not supported (open and save it in a newer Excel)";
            return false;
        }

        struct SheetRef
        {
            uint32_t offset;
            std::string name;
        };
        std::vector<SheetRef> sheetRefs;
        std::vector<std::string> sst;
        std::map<uint16_t, std::string> formats;
        std::vector<uint16_t> xfFormat;
        bool date1904 = false;

        // Globals: up to the first EOF.
        size_t i = 1;
        for (; i < records.size() && records[i].type != 0x000A; ++i)
        {
            const auto& r = records[i];
            switch (r.type)
            {
            case 0x002F:
                error = "the workbook is password protected";
                return false;
            case 0x0022:
                date1904 = U16(r.data, 0) == 1;
                break;
            case 0x0085:
            {
                // BOUNDSHEET8: stream offset, visibility, type (0 = worksheet), short name.
                if (r.data.size() < 8 || static_cast<uint8_t>(r.data[5]) != 0)
                    break;
                const size_t cch = static_cast<uint8_t>(r.data[6]);
                const bool wide = static_cast<uint8_t>(r.data[7]) & 1;
                sheetRefs.push_back({ U32(r.data, 0), Chars(r.data, 8, cch, wide) });
                break;
            }
            case 0x041E:
                formats[U16(r.data, 0)] = UnicodeString16(r.data, 2);
                break;
            case 0x00E0:
                xfFormat.push_back(U16(r.data, 2));
                break;
            case 0x00FC:
            {
                std::vector<std::string_view> parts{ r.data };
                for (size_t k = i + 1; k < records.size() && records[k].type == 0x003C; ++k)
                    parts.push_back(records[k].data);
                sst = ReadSst(parts);
                break;
            }
            default:
                break;
            }
        }

        const auto isDate = [&](uint16_t xf) {
            if (xf >= xfFormat.size())
                return false;
            const uint16_t f = xfFormat[xf];
            if ((f >= 14 && f <= 22) || (f >= 45 && f <= 47))
                return true;
            const auto it = formats.find(f);
            return it != formats.end() && IsDateCode(it->second);
        };
        const auto number = [&](double v, uint16_t xf) {
            if (isDate(xf))
                return Cell{ CellType::Date, date1904 ? v + 1462 : v, {} };
            return Cell::Num(v);
        };

        std::map<size_t, size_t> recordAt;   // stream offset -> record index
        for (size_t k = 0; k < records.size(); ++k)
            recordAt[records[k].offset] = k;

        for (const auto& ref : sheetRefs)
        {
            Sheet sheet;
            sheet.name = ref.name;
            const auto start = recordAt.find(ref.offset);
            if (start == recordAt.end())
                continue;
            size_t pendingRow = SIZE_MAX, pendingCol = 0;   // a FORMULA whose string result follows in STRING
            for (size_t k = start->second + 1; k < records.size() && records[k].type != 0x000A; ++k)
            {
                const auto& r = records[k];
                const auto& d = r.data;
                const size_t row = U16(d, 0), col = U16(d, 2);
                const uint16_t xf = U16(d, 4);
                switch (r.type)
                {
                case 0x00FD:   // LABELSST
                    if (const uint32_t idx = U32(d, 6); idx < sst.size())
                        sheet.Set(row, col, Cell::Str(sst[idx]));
                    break;
                case 0x0204:   // LABEL
                    sheet.Set(row, col, Cell::Str(UnicodeString16(d, 6)));
                    break;
                case 0x0203:   // NUMBER
                    sheet.Set(row, col, number(F64(d, 6), xf));
                    break;
                case 0x027E:   // RK
                    sheet.Set(row, col, number(Rk(U32(d, 6)), xf));
                    break;
                case 0x00BD:   // MULRK: one row, several columns
                {
                    if (d.size() < 6)
                        break;
                    const size_t last = U16(d, d.size() - 2);
                    size_t at = 4;
                    for (size_t c = col; c <= last && at + 6 <= d.size() - 2; ++c, at += 6)
                        sheet.Set(row, c, number(Rk(U32(d, at + 2)), U16(d, at)));
                    break;
                }
                case 0x0205:   // BOOLERR
                    if (d.size() >= 8)
                    {
                        const auto v = static_cast<uint8_t>(d[6]);
                        if (d[7])
                            sheet.Set(row, col, Cell{ CellType::Error, 0, ErrorText(v) });
                        else
                            sheet.Set(row, col, Cell{ CellType::Bool, v ? 1.0 : 0.0, {} });
                    }
                    break;
                case 0x0006:   // FORMULA: its cached result
                    if (d.size() >= 14)
                    {
                        if (U16(d, 12) == 0xFFFF)
                        {
                            const auto kind = static_cast<uint8_t>(d[6]);
                            if (kind == 0)
                            {
                                pendingRow = row;
                                pendingCol = col;
                            }
                            else if (kind == 1)
                                sheet.Set(row, col, Cell{ CellType::Bool, d[8] ? 1.0 : 0.0, {} });
                            else if (kind == 2)
                                sheet.Set(row, col, Cell{ CellType::Error, 0, ErrorText(static_cast<uint8_t>(d[8])) });
                        }
                        else
                            sheet.Set(row, col, number(F64(d, 6), xf));
                    }
                    break;
                case 0x0207:   // STRING: the text result of the FORMULA just before
                    if (pendingRow != SIZE_MAX)
                    {
                        sheet.Set(pendingRow, pendingCol, Cell::Str(UnicodeString16(d, 0)));
                        pendingRow = SIZE_MAX;
                    }
                    break;
                default:
                    break;
                }
            }
            sheet.Trim();
            book.sheets.push_back(std::move(sheet));
        }
        if (book.sheets.empty())
        {
            error = "the workbook has no worksheets";
            return false;
        }
        return true;
    }

    bool ReadXls(const std::string& bytes, Workbook& book, std::string& error)
    {
        using Microsoft::WRL::ComPtr;
        // An in-memory ILockBytes over a copy of the file, so a workbook open in Excel still reads.
        wil::unique_hglobal memory(GlobalAlloc(GMEM_MOVEABLE, bytes.size()));
        if (!memory)
        {
            error = "out of memory";
            return false;
        }
        if (void* p = GlobalLock(memory.get()))
        {
            std::memcpy(p, bytes.data(), bytes.size());
            GlobalUnlock(memory.get());
        }
        ComPtr<ILockBytes> lockBytes;
        if (FAILED(CreateILockBytesOnHGlobal(memory.get(), FALSE, &lockBytes)))
        {
            error = "cannot read the workbook";
            return false;
        }
        ComPtr<IStorage> storage;
        if (FAILED(StgOpenStorageOnILockBytes(lockBytes.Get(), nullptr, STGM_READ | STGM_SHARE_EXCLUSIVE, nullptr, 0, &storage)))
        {
            error = "not an Excel 97-2003 workbook";
            return false;
        }
        ComPtr<IStream> stream;
        if (FAILED(storage->OpenStream(L"Workbook", nullptr, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &stream)) &&
            FAILED(storage->OpenStream(L"Book", nullptr, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &stream)))
        {
            error = "not an Excel workbook (no Workbook stream)";
            return false;
        }
        STATSTG stat{};
        stream->Stat(&stat, STATFLAG_NONAME);
        if (stat.cbSize.QuadPart > (1ull << 31))
        {
            error = "the workbook is too large";
            return false;
        }
        std::string data(static_cast<size_t>(stat.cbSize.QuadPart), '\0');
        ULONG read = 0;
        if (!data.empty() && (FAILED(stream->Read(data.data(), static_cast<ULONG>(data.size()), &read)) || read != data.size()))
        {
            error = "cannot read the workbook";
            return false;
        }
        return ParseBiff8(data, book, error);
    }
}
