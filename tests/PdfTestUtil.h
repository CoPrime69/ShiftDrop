#pragma once
#include "TestUtil.h"
#include "Utf.h"
#include "pdf/Bitmap.h"
#include "pdf/PdfContent.h"
#include "pdf/PdfDoc.h"
#include "pdf/PdfWriter.h"
#include "pdf/TextLayout.h"

#include <qpdf/QPDFPageDocumentHelper.hh>

#include <objbase.h>

#include <format>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

// Shared by PdfTests.cpp and OcrTests.cpp.
namespace sd::test
{
    // Runs `fn` on a fresh MTA thread and waits: WinRT calls block, which an STA (the tests' main
    // thread, like the app's UI thread) must not do.
    inline void RunMta(const std::function<void()>& fn)
    {
        std::thread([&] {
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            fn();
            if (SUCCEEDED(hr))
                CoUninitialize();
        }).join();
    }

    inline std::string StreamBytes(QPDFObjectHandle stream)
    {
        auto buf = stream.getStreamData(qpdf_dl_generalized);
        return std::string(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
    }

    // A TrueType file's tables by tag ("glyf"), empty when it isn't one.
    inline std::map<std::string, std::string> SfntTables(const std::string& font)
    {
        const auto u16 = [&](size_t at) { return at + 2 <= font.size() ? unsigned(uint8_t(font[at]) << 8 | uint8_t(font[at + 1])) : 0u; };
        const auto u32 = [&](size_t at) { return (u16(at) << 16) | u16(at + 2); };
        std::map<std::string, std::string> tables;
        if (u32(0) != 0x00010000)
            return tables;
        for (unsigned i = 0, n = u16(4); i < n; ++i)
        {
            const size_t rec = 12 + size_t(i) * 16, at = u32(rec + 8), length = u32(rec + 12);
            if (at + length <= font.size())
                tables[font.substr(rec, 4)] = font.substr(at, length);
        }
        return tables;
    }

    // What a PDF reader would find missing in the fonts ShiftDrop added: every font a page's /Font
    // resources name with `prefix` must be a Type0 font (Identity-H) with a ToUnicode CMap, over a
    // CIDFontType2 with widths, a CIDToGIDMap and a full font descriptor that embeds a TrueType
    // program (FontFile2 with /Length1) holding the tables a PDF needs. `checked` counts the fonts
    // looked at. Empty when all is well.
    inline std::vector<std::string> FontProblems(const std::wstring& path, std::string_view prefix, int& checked)
    {
        checked = 0;
        std::vector<std::string> problems;
        std::wstring error;
        auto pdf = pdf::OpenPdf(path, error);
        if (!pdf)
            return { "cannot open: " + Narrow(error) };
        const auto need = [&](bool ok, const std::string& what) {
            if (!ok)
                problems.push_back(what);
            return ok;
        };
        for (auto& page : QPDFPageDocumentHelper(*pdf).getAllPages())
        {
            auto fonts = page.getAttribute("/Resources", false).getKeyIfDict("/Font");
            if (!fonts.isDictionary())
                continue;
            for (const auto& key : fonts.getKeys())
            {
                if (!key.starts_with("/" + std::string(prefix)))
                    continue;
                ++checked;
                auto font = fonts.getKey(key);
                const std::string at = key + ": ";
                need(font.getKey("/Subtype").isNameAndEquals("/Type0"), at + "not Type0");
                need(font.getKey("/Encoding").isNameAndEquals("/Identity-H"), at + "not Identity-H");
                const auto base = font.getKey("/BaseFont");
                need(base.isName(), at + "no /BaseFont");
                const auto toUnicode = font.getKey("/ToUnicode");
                if (need(toUnicode.isStream(), at + "no /ToUnicode"))
                {
                    int codeBytes = 0;
                    need(!pdf::ParseToUnicode(StreamBytes(toUnicode), codeBytes).empty() && codeBytes == 2, at + "empty /ToUnicode");
                }
                const auto kids = font.getKey("/DescendantFonts");
                if (!need(kids.isArray() && kids.getArrayNItems() == 1, at + "no descendant font"))
                    continue;
                auto cid = kids.getArrayItem(0);
                need(cid.getKey("/Subtype").isNameAndEquals("/CIDFontType2"), at + "descendant not CIDFontType2");
                need(cid.getKey("/BaseFont").isName() && base.isName() && cid.getKey("/BaseFont").getName() == base.getName(), at + "descendant /BaseFont differs");
                const auto info = cid.getKey("/CIDSystemInfo");
                need(info.isDictionary() && info.getKey("/Registry").isString() && info.getKey("/Ordering").isString() &&
                         info.getKey("/Supplement").isInteger(),
                     at + "incomplete /CIDSystemInfo");
                need(cid.getKey("/W").isArray() || cid.getKey("/DW").isNumber(), at + "no widths");
                const auto map = cid.getKey("/CIDToGIDMap");
                need(map.isNameAndEquals("/Identity") || map.isStream(), at + "no /CIDToGIDMap");
                const auto d = cid.getKey("/FontDescriptor");
                if (!need(d.isDictionary() && d.getKey("/Type").isNameAndEquals("/FontDescriptor"), at + "no font descriptor"))
                    continue;
                need(d.getKey("/FontName").isName() && base.isName() && d.getKey("/FontName").getName() == base.getName(), at + "/FontName differs");
                for (const char* k : { "/Flags", "/ItalicAngle", "/Ascent", "/Descent", "/CapHeight", "/StemV" })
                    need(d.getKey(k).isNumber(), at + "descriptor lacks " + k);
                need(d.getKey("/FontBBox").isArray() && d.getKey("/FontBBox").getArrayNItems() == 4, at + "descriptor lacks /FontBBox");
                const auto file = d.getKey("/FontFile2");
                if (!need(file.isStream(), at + "no embedded font program (/FontFile2)"))
                    continue;
                const std::string program = StreamBytes(file);
                const auto length1 = file.getDict().getKey("/Length1");
                need(length1.isInteger() && length1.getIntValue() == static_cast<long long>(program.size()), at + "/Length1 differs from the program");
                const auto tables = SfntTables(program);
                for (const char* t : { "head", "hhea", "hmtx", "loca", "glyf", "maxp" })
                    need(tables.contains(t), at + "font program lacks " + t);
                if (map.isStream() && tables.contains("maxp") && tables.at("maxp").size() >= 6)
                {
                    const auto& maxp = tables.at("maxp");
                    const unsigned glyphs = unsigned(uint8_t(maxp[4]) << 8 | uint8_t(maxp[5]));
                    const std::string gids = StreamBytes(map);
                    bool inRange = gids.size() % 2 == 0 && !gids.empty();
                    for (size_t i = 0; inRange && i + 1 < gids.size(); i += 2)
                        inRange = unsigned(uint8_t(gids[i]) << 8 | uint8_t(gids[i + 1])) < glyphs;
                    need(inRange, at + "/CIDToGIDMap points past the font's glyphs");
                }
            }
        }
        return problems;
    }

    // A page's lines joined with "|" after each, for one-line comparisons.
    inline std::wstring Joined(const pdf::PageLines& lines)
    {
        std::wstring s;
        for (const auto& l : lines)
            s += l.text + L"|";
        return s;
    }

    // A bitmap with a soft gradient and a few dark bars, so encoders have something to chew on.
    inline pdf::Bitmap Pattern(uint32_t w, uint32_t h, bool noise = false)
    {
        pdf::Bitmap b;
        b.width = w;
        b.height = h;
        b.bgra.resize(static_cast<size_t>(w) * h * 4);
        uint32_t seed = 12345;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                uint8_t* p = &b.bgra[(static_cast<size_t>(y) * w + x) * 4];
                seed = seed * 1664525u + 1013904223u;
                const uint8_t n = noise ? static_cast<uint8_t>(seed >> 24) : 0;
                p[0] = static_cast<uint8_t>((x * 255 / w) ^ n);
                p[1] = static_cast<uint8_t>((y * 255 / h) ^ n);
                p[2] = static_cast<uint8_t>(((x / 16) % 2 ? 40 : 200) ^ n);
                p[3] = 255;
            }
        return b;
    }

    // A text PDF written by hand: each entry is one page's content stream, drawn with Helvetica
    // (WinAnsiEncoding) as /F1. Optional /Rotate per page.
    inline void WriteTextPdf(const std::wstring& path, const std::vector<std::string>& contents, const std::vector<int>& rotate = {})
    {
        pdf::PdfWriter w(path);
        const int catalog = w.NewObject(), pages = w.NewObject(), font = w.NewObject();
        w.Object(font, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
        std::string kids;
        for (size_t i = 0; i < contents.size(); ++i)
        {
            const int c = w.NewObject(), page = w.NewObject();
            w.Stream(c, "", contents[i]);
            const std::string rot = i < rotate.size() && rotate[i] ? std::format(" /Rotate {}", rotate[i]) : "";
            w.Object(page, std::format("<< /Type /Page /Parent {} 0 R /MediaBox [0 0 612 792]{} /Resources << /Font << /F1 {} 0 R >> >> "
                                       "/Contents {} 0 R >>",
                                       pages, rot, font, c));
            kids += std::format(" {} 0 R", page);
        }
        w.Object(pages, std::format("<< /Type /Pages /Kids [{} ] /Count {} >>", kids, contents.size()));
        w.Object(catalog, std::format("<< /Type /Catalog /Pages {} 0 R >>", pages));
        w.Finish(catalog, 0);
    }

    // An image XObject for WritePicturePdf: the inside of its dictionary without /Length (and without
    // /Filter when `flate` deflates the data), and optionally another of the list as its /SMask.
    struct TestImage
    {
        std::string dict;
        std::string data;
        bool flate = false;
        int smask = -1;
    };

    // Like WriteTextPdf, with the images as /Im0, /Im1, ... in every page's resources.
    inline void WritePicturePdf(const std::wstring& path, const std::vector<std::string>& contents, const std::vector<TestImage>& images)
    {
        pdf::PdfWriter w(path);
        const int catalog = w.NewObject(), pages = w.NewObject(), font = w.NewObject();
        w.Object(font, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
        std::vector<int> ids;
        for (size_t i = 0; i < images.size(); ++i)
            ids.push_back(w.NewObject());
        std::string xobjects;
        for (size_t i = 0; i < images.size(); ++i)
        {
            std::string dict = images[i].dict;
            if (images[i].smask >= 0)
                dict += std::format(" /SMask {} 0 R", ids[static_cast<size_t>(images[i].smask)]);
            if (images[i].flate)
                w.FlateStream(ids[i], dict, images[i].data);
            else
                w.Stream(ids[i], dict, images[i].data);
            xobjects += std::format(" /Im{} {} 0 R", i, ids[i]);
        }
        std::string kids;
        for (const auto& content : contents)
        {
            const int c = w.NewObject(), page = w.NewObject();
            w.Stream(c, "", content);
            w.Object(page, std::format("<< /Type /Page /Parent {} 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 {} 0 R >> "
                                       "/XObject <<{} >> >> /Contents {} 0 R >>",
                                       pages, font, xobjects, c));
            kids += std::format(" {} 0 R", page);
        }
        w.Object(pages, std::format("<< /Type /Pages /Kids [{} ] /Count {} >>", kids, contents.size()));
        w.Object(catalog, std::format("<< /Type /Catalog /Pages {} 0 R >>", pages));
        w.Finish(catalog, 0);
    }

    // Raw RGB samples, w x h, with R = x * 6, G = y * 8, B = 128: easy to check after a round trip.
    inline std::string RgbRamp(uint32_t w, uint32_t h)
    {
        std::string data;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                data += static_cast<char>(x * 6);
                data += static_cast<char>(y * 8);
                data += static_cast<char>(128);
            }
        return data;
    }
}
