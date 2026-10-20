#include "pdf/PdfContent.h"

#include <windows.h>

#include <qpdf/QPDFObjectHandle.hh>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <memory>
#include <set>
#include <unordered_map>

namespace sd::pdf
{
    namespace
    {
        // ---- Glyph names -------------------------------------------------------------------------

        const std::unordered_map<std::string_view, char32_t>& GlyphTable()
        {
            static const auto table = [] {
                std::unordered_map<std::string_view, char32_t> t;
                // ASCII 0x20..0x7E in order, then Latin-1 0xA0..0xFF in order (Adobe Glyph List names).
                static constexpr std::string_view ascii[] = {
                    "space", "exclam", "quotedbl", "numbersign", "dollar", "percent", "ampersand", "quotesingle", "parenleft",
                    "parenright", "asterisk", "plus", "comma", "hyphen", "period", "slash", "zero", "one", "two", "three", "four",
                    "five", "six", "seven", "eight", "nine", "colon", "semicolon", "less", "equal", "greater", "question", "at" };
                for (size_t i = 0; i < std::size(ascii); ++i)
                    t.emplace(ascii[i], static_cast<char32_t>(0x20 + i));
                static constexpr std::string_view ascii2[] = { "bracketleft", "backslash", "bracketright", "asciicircum", "underscore", "grave" };
                for (size_t i = 0; i < std::size(ascii2); ++i)
                    t.emplace(ascii2[i], static_cast<char32_t>(0x5B + i));
                static constexpr std::string_view ascii3[] = { "braceleft", "bar", "braceright", "asciitilde" };
                for (size_t i = 0; i < std::size(ascii3); ++i)
                    t.emplace(ascii3[i], static_cast<char32_t>(0x7B + i));
                static constexpr std::string_view latin1[] = {
                    "nbspace", "exclamdown", "cent", "sterling", "currency", "yen", "brokenbar", "section", "dieresis", "copyright",
                    "ordfeminine", "guillemotleft", "logicalnot", "sfthyphen", "registered", "macron", "degree", "plusminus",
                    "twosuperior", "threesuperior", "acute", "mu", "paragraph", "periodcentered", "cedilla", "onesuperior",
                    "ordmasculine", "guillemotright", "onequarter", "onehalf", "threequarters", "questiondown", "Agrave", "Aacute",
                    "Acircumflex", "Atilde", "Adieresis", "Aring", "AE", "Ccedilla", "Egrave", "Eacute", "Ecircumflex", "Edieresis",
                    "Igrave", "Iacute", "Icircumflex", "Idieresis", "Eth", "Ntilde", "Ograve", "Oacute", "Ocircumflex", "Otilde",
                    "Odieresis", "multiply", "Oslash", "Ugrave", "Uacute", "Ucircumflex", "Udieresis", "Yacute", "Thorn", "germandbls",
                    "agrave", "aacute", "acircumflex", "atilde", "adieresis", "aring", "ae", "ccedilla", "egrave", "eacute",
                    "ecircumflex", "edieresis", "igrave", "iacute", "icircumflex", "idieresis", "eth", "ntilde", "ograve", "oacute",
                    "ocircumflex", "otilde", "odieresis", "divide", "oslash", "ugrave", "uacute", "ucircumflex", "udieresis", "yacute",
                    "thorn", "ydieresis" };
                for (size_t i = 0; i < std::size(latin1); ++i)
                    t.emplace(latin1[i], static_cast<char32_t>(0xA0 + i));
                static constexpr std::pair<std::string_view, char32_t> extra[] = {
                    { "quoteright", 0x2019 }, { "quoteleft", 0x2018 }, { "quotedblleft", 0x201C }, { "quotedblright", 0x201D },
                    { "quotesinglbase", 0x201A }, { "quotedblbase", 0x201E }, { "endash", 0x2013 }, { "emdash", 0x2014 },
                    { "bullet", 0x2022 }, { "ellipsis", 0x2026 }, { "dagger", 0x2020 }, { "daggerdbl", 0x2021 },
                    { "perthousand", 0x2030 }, { "guilsinglleft", 0x2039 }, { "guilsinglright", 0x203A }, { "trademark", 0x2122 },
                    { "Euro", 0x20AC }, { "ff", 0xFB00 }, { "fi", 0xFB01 }, { "fl", 0xFB02 }, { "ffi", 0xFB03 }, { "ffl", 0xFB04 },
                    { "minus", 0x2212 }, { "fraction", 0x2044 }, { "florin", 0x0192 }, { "circumflex", 0x02C6 }, { "tilde", 0x02DC },
                    { "OE", 0x0152 }, { "oe", 0x0153 }, { "Scaron", 0x0160 }, { "scaron", 0x0161 }, { "Zcaron", 0x017D },
                    { "zcaron", 0x017E }, { "Ydieresis", 0x0178 }, { "dotlessi", 0x0131 }, { "Lslash", 0x0141 }, { "lslash", 0x0142 },
                    { "ring", 0x02DA }, { "caron", 0x02C7 }, { "breve", 0x02D8 }, { "dotaccent", 0x02D9 }, { "ogonek", 0x02DB },
                    { "hungarumlaut", 0x02DD }, { "Delta", 0x2206 }, { "Omega", 0x2126 }, { "pi", 0x03C0 }, { "approxequal", 0x2248 },
                    { "notequal", 0x2260 }, { "lessequal", 0x2264 }, { "greaterequal", 0x2265 }, { "infinity", 0x221E },
                    { "partialdiff", 0x2202 }, { "summation", 0x2211 }, { "product", 0x220F }, { "radical", 0x221A },
                    { "integral", 0x222B }, { "lozenge", 0x25CA }, { "arrowright", 0x2192 }, { "arrowleft", 0x2190 },
                    { "arrowup", 0x2191 }, { "arrowdown", 0x2193 }, { "checkmark", 0x2713 }, { "nonbreakingspace", 0x00A0 },
                    { "softhyphen", 0x00AD }, { "middot", 0x00B7 }, { "mu1", 0x00B5 }, { "periodcentered", 0x00B7 } };
                for (const auto& [name, code] : extra)
                    t.emplace(name, code);
                static constexpr char kLetters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
                for (size_t i = 0; i + 1 < sizeof(kLetters); ++i)
                    t.emplace(std::string_view(kLetters + i, 1), static_cast<char32_t>(kLetters[i]));
                return t;
            }();
            return table;
        }

        void AppendCodePoint(std::wstring& out, char32_t cp)
        {
            if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
                return;
            if (cp < 0x10000)
                out += static_cast<wchar_t>(cp);
            else
            {
                cp -= 0x10000;
                out += static_cast<wchar_t>(0xD800 + (cp >> 10));
                out += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
            }
        }

        bool ParseHex(std::string_view s, uint32_t& value)
        {
            if (s.empty() || s.size() > 8)
                return false;
            const auto r = std::from_chars(s.data(), s.data() + s.size(), value, 16);
            return r.ec == std::errc{} && r.ptr == s.data() + s.size();
        }

        // ---- Matrices (PDF row-vector convention: p' = p x M) ------------------------------------

        struct Matrix
        {
            double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
        };

        // First A, then B.
        Matrix Mul(const Matrix& A, const Matrix& B)
        {
            return Matrix{ A.a * B.a + A.b * B.c,       A.a * B.b + A.b * B.d,       A.c * B.a + A.d * B.c,
                           A.c * B.b + A.d * B.d,       A.e * B.a + A.f * B.c + B.e, A.e * B.b + A.f * B.d + B.f };
        }

        Matrix Translate(double x, double y) { return Matrix{ 1, 0, 0, 1, x, y }; }

        // ---- Fonts -------------------------------------------------------------------------------

        struct Glyph
        {
            std::wstring text;
            double width = 0;        // glyph space / 1000 (text space units per 1 pt of font size)
            bool wordSpace = false;  // single-byte code 32: Tw applies
        };

        std::array<wchar_t, 256> CodePageTable(UINT codePage)
        {
            std::array<wchar_t, 256> table{};
            for (int c = 0x20; c < 256; ++c)
            {
                const char byte = static_cast<char>(c);
                wchar_t w = 0;
                if (MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, &byte, 1, &w, 1) == 1)
                    table[c] = w;
            }
            return table;
        }

        std::array<wchar_t, 256> BaseEncoding(const std::string& name)
        {
            if (name == "/MacRomanEncoding")
                return CodePageTable(10000);
            auto table = CodePageTable(1252);
            if (name == "/StandardEncoding")
            {
                // The few places Adobe's StandardEncoding differs in everyday text.
                table[0x27] = 0x2019;
                table[0x60] = 0x2018;
            }
            return table;
        }

        class Font
        {
        public:
            explicit Font(QPDFObjectHandle dict)
            {
                try
                {
                    Load(dict);
                }
                catch (const std::exception&)
                {
                    // A broken font dictionary: keep whatever was read; text in it may be missing.
                }
            }

            void Decode(const std::string& bytes, std::vector<Glyph>& out) const
            {
                out.clear();
                const size_t step = static_cast<size_t>(m_codeBytes);
                for (size_t i = 0; i + step <= bytes.size(); i += step)
                {
                    uint32_t code = 0;
                    for (size_t k = 0; k < step; ++k)
                        code = (code << 8) | static_cast<unsigned char>(bytes[i + k]);
                    Glyph g;
                    if (auto it = m_toUnicode.find(code); it != m_toUnicode.end())
                        g.text = it->second;
                    else if (m_codeBytes == 1 && m_hasEncoding && code < 256 && m_encoding[code])
                        g.text = std::wstring(1, m_encoding[code]);
                    auto w = m_widths.find(code);
                    g.width = (w != m_widths.end() ? w->second : m_defaultWidth) * m_widthScale / 1000.0;
                    g.wordSpace = m_codeBytes == 1 && code == 32;
                    out.push_back(std::move(g));
                }
            }

        private:
            void Load(QPDFObjectHandle dict)
            {
                const std::string subtype = dict.getKey("/Subtype").isName() ? dict.getKey("/Subtype").getName() : "";
                if (subtype == "/Type0")
                {
                    m_codeBytes = 2;   // Identity-H/V and the other common CMaps use two bytes
                    auto encoding = dict.getKey("/Encoding");
                    if (encoding.isStream())
                    {
                        int bytes = 0;
                        const auto data = encoding.getStreamData(qpdf_dl_generalized);
                        ParseToUnicode(std::string_view(reinterpret_cast<const char*>(data->getBuffer()), data->getSize()), bytes);
                        if (bytes >= 1 && bytes <= 4)
                            m_codeBytes = bytes;
                    }
                    auto descendants = dict.getKey("/DescendantFonts");
                    auto cid = descendants.isArray() && descendants.getArrayNItems() > 0 ? descendants.getArrayItem(0) : QPDFObjectHandle::newNull();
                    if (cid.isDictionary())
                    {
                        m_defaultWidth = cid.getKey("/DW").isNumber() ? cid.getKey("/DW").getNumericValue() : 1000.0;
                        LoadCidWidths(cid.getKey("/W"));
                    }
                    else
                        m_defaultWidth = 1000.0;
                }
                else
                {
                    LoadSimpleWidths(dict);
                    LoadEncoding(dict.getKey("/Encoding"));
                    if (subtype == "/Type3")
                    {
                        auto matrix = dict.getKey("/FontMatrix");
                        if (matrix.isArray() && matrix.getArrayNItems() == 6 && matrix.getArrayItem(0).isNumber())
                            m_widthScale = matrix.getArrayItem(0).getNumericValue() * 1000.0;
                    }
                }
                auto toUnicode = dict.getKey("/ToUnicode");
                if (toUnicode.isStream())
                {
                    int bytes = 0;
                    const auto data = toUnicode.getStreamData(qpdf_dl_generalized);
                    m_toUnicode = ParseToUnicode(std::string_view(reinterpret_cast<const char*>(data->getBuffer()), data->getSize()), bytes);
                }
            }

            void LoadSimpleWidths(QPDFObjectHandle dict)
            {
                auto widths = dict.getKey("/Widths");
                auto first = dict.getKey("/FirstChar");
                if (widths.isArray() && first.isNumber())
                {
                    const auto fc = static_cast<uint32_t>(first.getNumericValue());
                    const int n = widths.getArrayNItems();
                    for (int i = 0; i < n && i < 256; ++i)
                        if (auto w = widths.getArrayItem(i); w.isNumber())
                            m_widths[fc + static_cast<uint32_t>(i)] = w.getNumericValue();
                }
                auto descriptor = dict.getKey("/FontDescriptor");
                if (descriptor.isDictionary() && descriptor.getKey("/MissingWidth").isNumber())
                    m_defaultWidth = descriptor.getKey("/MissingWidth").getNumericValue();
                if (m_defaultWidth <= 0)
                    m_defaultWidth = 500.0;
            }

            void LoadCidWidths(QPDFObjectHandle w)
            {
                if (!w.isArray())
                    return;
                const int n = w.getArrayNItems();
                for (int i = 0; i < n;)
                {
                    auto first = w.getArrayItem(i);
                    if (!first.isNumber() || i + 1 >= n)
                        break;
                    const auto c = static_cast<uint32_t>(first.getNumericValue());
                    auto next = w.getArrayItem(i + 1);
                    if (next.isArray())   // c [w1 w2 ...]
                    {
                        const int m = std::min(next.getArrayNItems(), 65536);
                        for (int k = 0; k < m; ++k)
                            if (auto v = next.getArrayItem(k); v.isNumber())
                                m_widths[c + static_cast<uint32_t>(k)] = v.getNumericValue();
                        i += 2;
                    }
                    else if (next.isNumber() && i + 2 < n && w.getArrayItem(i + 2).isNumber())   // c_first c_last w
                    {
                        const auto last = static_cast<uint32_t>(next.getNumericValue());
                        const double value = w.getArrayItem(i + 2).getNumericValue();
                        for (uint32_t k = c; k <= last && k - c < 65536; ++k)
                            m_widths[k] = value;
                        i += 3;
                    }
                    else
                        break;
                }
            }

            void LoadEncoding(QPDFObjectHandle encoding)
            {
                m_hasEncoding = true;
                if (encoding.isName())
                {
                    m_encoding = BaseEncoding(encoding.getName());
                    return;
                }
                m_encoding = BaseEncoding(encoding.isDictionary() && encoding.getKey("/BaseEncoding").isName()
                                              ? encoding.getKey("/BaseEncoding").getName()
                                              : "/StandardEncoding");
                if (!encoding.isDictionary())
                    return;
                auto differences = encoding.getKey("/Differences");
                if (!differences.isArray())
                    return;
                uint32_t code = 0;
                const int n = differences.getArrayNItems();
                for (int i = 0; i < n; ++i)
                {
                    auto item = differences.getArrayItem(i);
                    if (item.isNumber())
                        code = static_cast<uint32_t>(item.getNumericValue());
                    else if (item.isName())
                    {
                        if (code < 256)
                        {
                            const std::wstring text = GlyphNameToUnicode(item.getName().substr(1));
                            if (text.size() == 1)
                                m_encoding[code] = text[0];
                            else if (!text.empty())
                                m_toUnicodeFromNames[code] = text;   // a ligature name like f_f_i
                            else
                                m_encoding[code] = 0;   // unknown glyph: better nothing than a wrong letter
                        }
                        ++code;
                    }
                }
                for (auto& [c, text] : m_toUnicodeFromNames)
                    m_toUnicode.emplace(c, text);
            }

            int m_codeBytes = 1;
            std::map<uint32_t, std::wstring> m_toUnicode;
            std::map<uint32_t, std::wstring> m_toUnicodeFromNames;
            std::array<wchar_t, 256> m_encoding{};
            bool m_hasEncoding = false;
            std::map<uint32_t, double> m_widths;
            double m_defaultWidth = 500.0;
            double m_widthScale = 1.0;
        };

        // ---- The content walker ------------------------------------------------------------------

        struct RawSpan
        {
            double x0 = 0, y0 = 0, x1 = 0, y1 = 0, size = 0, angle = 0;
            std::wstring text;
        };

        class Walker : public QPDFObjectHandle::ParserCallbacks
        {
        public:
            Walker(bool text, std::map<QPDFObjGen, Placement>* placements) : m_text(text), m_placements(placements) {}

            void Run(QPDFPageObjectHelper& page)
            {
                m_resources.push_back(page.getAttribute("/Resources", false));
                try
                {
                    page.parseContents(this);
                }
                catch (const std::exception&)
                {
                    // Damaged content: keep what was read before the error.
                }
            }

            std::vector<RawSpan> spans;

        private:
            struct State
            {
                Matrix ctm;
                double tc = 0, tw = 0, th = 1, tl = 0, fontSize = 0;
                const Font* font = nullptr;
            };

            void handleObject(QPDFObjectHandle obj) override
            {
                if (++m_count > 5'000'000)   // a pathological stream; enough is enough
                    terminateParsing();
                if (obj.isOperator())
                {
                    Operator(obj.getOperatorValue());
                    m_operands.clear();
                }
                else
                    m_operands.push_back(obj);
            }
            void handleEOF() override {}

            double Number(size_t i) const
            {
                return i < m_operands.size() && m_operands[i].isNumber() ? m_operands[i].getNumericValue() : 0.0;
            }
            bool Numbers(size_t n) const
            {
                if (m_operands.size() < n)
                    return false;
                for (size_t i = m_operands.size() - n; i < m_operands.size(); ++i)
                    if (!m_operands[i].isNumber())
                        return false;
                return true;
            }
            Matrix MatrixOperands() const
            {
                const size_t b = m_operands.size() - 6;
                return Matrix{ Number(b), Number(b + 1), Number(b + 2), Number(b + 3), Number(b + 4), Number(b + 5) };
            }

            void NextLine(double tx, double ty)
            {
                m_tlm = Mul(Translate(tx, ty), m_tlm);
                m_tm = m_tlm;
            }

            void Operator(const std::string& op)
            {
                if (op == "q")
                    m_stack.push_back(m_gs);
                else if (op == "Q")
                {
                    if (!m_stack.empty())
                    {
                        m_gs = m_stack.back();
                        m_stack.pop_back();
                    }
                }
                else if (op == "cm" && Numbers(6))
                    m_gs.ctm = Mul(MatrixOperands(), m_gs.ctm);
                else if (op == "Do" && !m_operands.empty() && m_operands.back().isName())
                    DoXObject(m_operands.back().getName());
                else if (!m_text)
                    return;
                else if (op == "BT")
                    m_tm = m_tlm = Matrix{};
                else if (op == "Tc" && Numbers(1))
                    m_gs.tc = m_operands.back().getNumericValue();
                else if (op == "Tw" && Numbers(1))
                    m_gs.tw = m_operands.back().getNumericValue();
                else if (op == "Tz" && Numbers(1))
                    m_gs.th = m_operands.back().getNumericValue() / 100.0;
                else if (op == "TL" && Numbers(1))
                    m_gs.tl = m_operands.back().getNumericValue();
                else if (op == "Tf" && m_operands.size() >= 2 && m_operands[m_operands.size() - 2].isName() && m_operands.back().isNumber())
                {
                    m_gs.font = FindFont(m_operands[m_operands.size() - 2].getName());
                    m_gs.fontSize = m_operands.back().getNumericValue();
                }
                else if (op == "Td" && Numbers(2))
                    NextLine(Number(m_operands.size() - 2), Number(m_operands.size() - 1));
                else if (op == "TD" && Numbers(2))
                {
                    m_gs.tl = -Number(m_operands.size() - 1);
                    NextLine(Number(m_operands.size() - 2), Number(m_operands.size() - 1));
                }
                else if (op == "Tm" && Numbers(6))
                    m_tm = m_tlm = MatrixOperands();
                else if (op == "T*")
                    NextLine(0, -m_gs.tl);
                else if (op == "Tj" && !m_operands.empty() && m_operands.back().isString())
                    Show({ m_operands.back() });
                else if (op == "'" && !m_operands.empty() && m_operands.back().isString())
                {
                    NextLine(0, -m_gs.tl);
                    Show({ m_operands.back() });
                }
                else if (op == "\"" && m_operands.size() >= 3 && m_operands.back().isString())
                {
                    m_gs.tw = Number(m_operands.size() - 3);
                    m_gs.tc = Number(m_operands.size() - 2);
                    NextLine(0, -m_gs.tl);
                    Show({ m_operands.back() });
                }
                else if (op == "TJ" && !m_operands.empty() && m_operands.back().isArray())
                    Show(m_operands.back().getArrayAsVector());
            }

            const Font* FindFont(const std::string& name)
            {
                auto fonts = m_resources.back().isDictionary() ? m_resources.back().getKey("/Font") : QPDFObjectHandle::newNull();
                auto dict = fonts.isDictionary() ? fonts.getKey(name) : QPDFObjectHandle::newNull();
                if (!dict.isDictionary())
                    return nullptr;
                if (dict.isIndirect())
                {
                    auto& slot = m_fonts[dict.getObjGen()];
                    if (!slot)
                        slot = std::make_unique<Font>(dict);
                    return slot.get();
                }
                m_directFonts.push_back(std::make_unique<Font>(dict));
                return m_directFonts.back().get();
            }

            void Show(const std::vector<QPDFObjectHandle>& items)
            {
                const Font* font = m_gs.font;
                if (!font)
                    return;
                RawSpan span;
                bool started = false;
                const auto mark = [&] {
                    const Matrix m = Mul(m_tm, m_gs.ctm);
                    if (!started)
                    {
                        span.x0 = m.e, span.y0 = m.f;
                        span.size = std::abs(m_gs.fontSize) * std::hypot(m.c, m.d);
                        span.angle = std::atan2(m.b, m.a);
                        started = true;
                    }
                };
                std::vector<Glyph> glyphs;
                for (const auto& item : items)
                {
                    if (item.isNumber())
                    {
                        // TJ adjustment in thousandths of an em; a big gap is a space between words.
                        const double adjust = item.getNumericValue();
                        if (-adjust >= 200 && !span.text.empty() && span.text.back() != L' ')
                            span.text += L' ';
                        m_tm = Mul(Translate(-adjust / 1000.0 * m_gs.fontSize * m_gs.th, 0), m_tm);
                        continue;
                    }
                    if (!item.isString())
                        continue;
                    font->Decode(item.getStringValue(), glyphs);
                    for (const auto& g : glyphs)
                    {
                        mark();
                        for (const wchar_t c : g.text)
                            if (c >= 0x20 || c == L'\t')
                                span.text += c == L'\t' ? L' ' : c;
                        const double advance = (g.width * m_gs.fontSize + m_gs.tc + (g.wordSpace ? m_gs.tw : 0.0)) * m_gs.th;
                        m_tm = Mul(Translate(advance, 0), m_tm);
                    }
                }
                if (!started || span.text.empty())
                    return;
                const Matrix end = Mul(m_tm, m_gs.ctm);
                span.x1 = end.e, span.y1 = end.f;
                spans.push_back(std::move(span));
            }

            void DoXObject(const std::string& name)
            {
                auto xobjects = m_resources.back().isDictionary() ? m_resources.back().getKey("/XObject") : QPDFObjectHandle::newNull();
                auto xobject = xobjects.isDictionary() ? xobjects.getKey(name) : QPDFObjectHandle::newNull();
                if (!xobject.isStream())
                    return;
                auto dict = xobject.getDict();
                const std::string subtype = dict.getKey("/Subtype").isName() ? dict.getKey("/Subtype").getName() : "";
                if (subtype == "/Image")
                {
                    if (m_placements && xobject.isIndirect())
                    {
                        auto& p = (*m_placements)[xobject.getObjGen()];
                        p.width = std::max(p.width, std::hypot(m_gs.ctm.a, m_gs.ctm.b));
                        p.height = std::max(p.height, std::hypot(m_gs.ctm.c, m_gs.ctm.d));
                    }
                    return;
                }
                if (subtype != "/Form" || m_depth >= 12)
                    return;
                const QPDFObjGen id = xobject.getObjGen();
                if (xobject.isIndirect() && !m_activeForms.insert(id).second)
                    return;   // a form that draws itself
                const State saved = m_gs;
                const auto operands = std::move(m_operands);
                m_operands.clear();
                auto matrix = dict.getKey("/Matrix");
                if (matrix.isArray() && matrix.getArrayNItems() == 6)
                {
                    Matrix m;
                    double* fields[] = { &m.a, &m.b, &m.c, &m.d, &m.e, &m.f };
                    for (int i = 0; i < 6; ++i)
                        if (auto v = matrix.getArrayItem(i); v.isNumber())
                            *fields[i] = v.getNumericValue();
                    m_gs.ctm = Mul(m, m_gs.ctm);
                }
                auto resources = dict.getKey("/Resources");
                m_resources.push_back(resources.isDictionary() ? resources : m_resources.back());
                const auto stack = m_stack;
                ++m_depth;
                try
                {
                    xobject.parseAsContents(this);
                }
                catch (const std::exception&)
                {
                }
                --m_depth;
                m_stack = stack;
                m_resources.pop_back();
                if (xobject.isIndirect())
                    m_activeForms.erase(id);
                m_gs = saved;
                m_operands = operands;
            }

            bool m_text;
            std::map<QPDFObjGen, Placement>* m_placements;
            State m_gs;
            std::vector<State> m_stack;
            Matrix m_tm, m_tlm;
            std::vector<QPDFObjectHandle> m_operands;
            std::vector<QPDFObjectHandle> m_resources;
            std::map<QPDFObjGen, std::unique_ptr<Font>> m_fonts;
            std::vector<std::unique_ptr<Font>> m_directFonts;
            std::set<QPDFObjGen> m_activeForms;
            int m_depth = 0;
            size_t m_count = 0;
        };

        // Turns (x, y) by -quarter*90 degrees so text at that angle runs left to right.
        void Unrotate(int quarter, double x, double y, double& ox, double& oy)
        {
            switch (quarter)
            {
            case 1: ox = y, oy = -x; break;
            case 2: ox = -x, oy = -y; break;
            case 3: ox = -y, oy = x; break;
            default: ox = x, oy = y; break;
            }
        }
    }

    std::wstring GlyphNameToUnicode(std::string_view name)
    {
        if (const auto dot = name.find('.'); dot != std::string_view::npos && dot > 0)
            name = name.substr(0, dot);   // "a.sc", "one.oldstyle"
        if (name.find('_') != std::string_view::npos)
        {
            std::wstring out;   // ligatures: "f_f_i"
            size_t start = 0;
            while (start <= name.size())
            {
                const size_t end = std::min(name.find('_', start), name.size());
                const std::wstring part = GlyphNameToUnicode(name.substr(start, end - start));
                if (part.empty())
                    return {};
                out += part;
                start = end + 1;
            }
            return out;
        }
        if (const auto& table = GlyphTable(); table.count(name))
        {
            std::wstring out;
            AppendCodePoint(out, table.at(name));
            return out;
        }
        uint32_t value = 0;
        if (name.size() >= 7 && name.starts_with("uni") && (name.size() - 3) % 4 == 0)
        {
            std::wstring out;   // "uni00E9" or several code units in a row, "uni00660069"
            for (size_t i = 3; i < name.size(); i += 4)
            {
                if (!ParseHex(name.substr(i, 4), value))
                    return {};
                AppendCodePoint(out, value);
            }
            return out;
        }
        if (name.size() >= 5 && name.size() <= 7 && name[0] == 'u' && ParseHex(name.substr(1), value))
        {
            std::wstring out;
            AppendCodePoint(out, value);
            return out;
        }
        return {};
    }

    std::map<uint32_t, std::wstring> ParseToUnicode(std::string_view cmap, int& codeBytes)
    {
        std::map<uint32_t, std::wstring> out;
        codeBytes = 0;
        // Tokens: <hex>, [, ], and bare words. Strings and dictionaries in a CMap do not matter here.
        struct Token
        {
            enum Kind { Hex, Open, Close, Word } kind;
            std::string_view text;
        };
        std::vector<Token> tokens;
        for (size_t i = 0; i < cmap.size();)
        {
            const char c = cmap[i];
            if (c == '%')
            {
                while (i < cmap.size() && cmap[i] != '\n' && cmap[i] != '\r')
                    ++i;
            }
            else if (c == '<' && i + 1 < cmap.size() && cmap[i + 1] != '<')
            {
                const size_t end = cmap.find('>', i + 1);
                if (end == std::string_view::npos)
                    break;
                tokens.push_back({ Token::Hex, cmap.substr(i + 1, end - i - 1) });
                i = end + 1;
            }
            else if (c == '[' || c == ']')
            {
                tokens.push_back({ c == '[' ? Token::Open : Token::Close, cmap.substr(i, 1) });
                ++i;
            }
            else if (std::isspace(static_cast<unsigned char>(c)) || c == '<' || c == '>' || c == '(' || c == ')' || c == '{' || c == '}' ||
                     c == '/')
                ++i;
            else
            {
                size_t end = i;
                while (end < cmap.size() && !std::isspace(static_cast<unsigned char>(cmap[end])) && std::string_view("<>[]()/%{}").find(cmap[end]) == std::string_view::npos)
                    ++end;
                tokens.push_back({ Token::Word, cmap.substr(i, end - i) });
                i = end;
            }
        }

        const auto clean = [](std::string_view hex) {
            std::string s;
            for (const char c : hex)
                if (!std::isspace(static_cast<unsigned char>(c)))
                    s += c;
            return s;
        };
        const auto utf16 = [&](std::string_view hex) {
            const std::string s = clean(hex);
            std::wstring text;
            for (size_t i = 0; i + 4 <= s.size(); i += 4)
            {
                uint32_t unit = 0;
                if (ParseHex(std::string_view(s).substr(i, 4), unit))
                    text += static_cast<wchar_t>(unit);
            }
            if (s.size() == 2)   // some writers give one byte; treat it as Latin-1
            {
                uint32_t b = 0;
                if (ParseHex(s, b))
                    text += static_cast<wchar_t>(b);
            }
            return text;
        };
        const auto code = [&](std::string_view hex, uint32_t& value, int& bytes) {
            const std::string s = clean(hex);
            bytes = static_cast<int>(s.size() / 2);
            return ParseHex(s, value);
        };

        enum class Section { None, Codespace, Char, Range } section = Section::None;
        for (size_t i = 0; i < tokens.size(); ++i)
        {
            const auto& t = tokens[i];
            if (t.kind == Token::Word)
            {
                if (t.text == "begincodespacerange") section = Section::Codespace;
                else if (t.text == "beginbfchar") section = Section::Char;
                else if (t.text == "beginbfrange") section = Section::Range;
                else if (t.text.starts_with("end")) section = Section::None;
                continue;
            }
            uint32_t lo = 0, hi = 0;
            int bytes = 0;
            if (section == Section::Codespace && t.kind == Token::Hex && i + 1 < tokens.size() && tokens[i + 1].kind == Token::Hex)
            {
                if (code(t.text, lo, bytes) && codeBytes == 0)
                    codeBytes = bytes;
                ++i;
            }
            else if (section == Section::Char && t.kind == Token::Hex && i + 1 < tokens.size() && tokens[i + 1].kind == Token::Hex)
            {
                if (code(t.text, lo, bytes))
                {
                    out[lo] = utf16(tokens[i + 1].text);
                    if (codeBytes == 0)
                        codeBytes = bytes;
                }
                ++i;
            }
            else if (section == Section::Range && t.kind == Token::Hex && i + 2 < tokens.size() && tokens[i + 1].kind == Token::Hex)
            {
                if (!code(t.text, lo, bytes) || !code(tokens[i + 1].text, hi, bytes) || hi < lo || hi - lo > 0xFFFF)
                {
                    i += 2;
                    continue;
                }
                if (codeBytes == 0)
                    codeBytes = bytes;
                if (tokens[i + 2].kind == Token::Hex)
                {
                    // Consecutive codes map to consecutive values in the last UTF-16 unit.
                    const std::wstring base = utf16(tokens[i + 2].text);
                    for (uint32_t c = lo; c <= hi && !base.empty(); ++c)
                    {
                        std::wstring text = base;
                        text.back() = static_cast<wchar_t>(text.back() + (c - lo));
                        out[c] = std::move(text);
                    }
                    i += 2;
                }
                else if (tokens[i + 2].kind == Token::Open)
                {
                    size_t k = i + 3;
                    for (uint32_t c = lo; k < tokens.size() && tokens[k].kind == Token::Hex; ++k, ++c)
                        if (c <= hi)
                            out[c] = utf16(tokens[k].text);
                    i = k;   // at the closing bracket
                }
                else
                    i += 2;
            }
        }
        return out;
    }

    PageLines ExtractPageLines(QPDFPageObjectHelper& page)
    {
        Walker walker(true, nullptr);
        walker.Run(page);
        // Which way does most of the text run? Weighted by characters, to the nearest quarter turn.
        size_t votes[4] = {};
        for (const auto& s : walker.spans)
        {
            const double turns = s.angle / (3.14159265358979323846 / 2);
            const int quarter = ((static_cast<int>(std::lround(turns)) % 4) + 4) % 4;
            votes[quarter] += s.text.size();
        }
        const int quarter = static_cast<int>(std::max_element(std::begin(votes), std::end(votes)) - std::begin(votes));
        std::vector<TextSpan> spans;
        spans.reserve(walker.spans.size());
        for (auto& s : walker.spans)
        {
            TextSpan t;
            double ex = 0, ey = 0;
            Unrotate(quarter, s.x0, s.y0, t.x, t.y);
            Unrotate(quarter, s.x1, s.y1, ex, ey);
            t.endX = ex;
            t.size = s.size > 0 ? s.size : 10.0;
            t.text = std::move(s.text);
            spans.push_back(std::move(t));
        }
        return JoinSpans(spans);
    }

    void CollectImagePlacements(QPDFPageObjectHelper& page, std::map<QPDFObjGen, Placement>& out)
    {
        Walker walker(false, &out);
        walker.Run(page);
    }
}
