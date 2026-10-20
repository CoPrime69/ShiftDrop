#include "docs/Rtf.h"
#include "docs/Bytes.h"
#include "Utf.h"

#include <windows.h>
#include <richedit.h>
#include <wil/resource.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <filesystem>
#include <format>
#include <map>

namespace sd::docs
{
    namespace
    {
        // Index of the '}' closing the group opened at rtf[open] == '{', honouring \{ \} \\ and \binN.
        size_t GroupEnd(std::string_view rtf, size_t open)
        {
            int depth = 0;
            for (size_t i = open; i < rtf.size(); ++i)
            {
                const char c = rtf[i];
                if (c == '\\')
                {
                    if (rtf.substr(i, 4) == "\\bin")
                    {
                        size_t j = i + 4;
                        size_t n = 0;
                        while (j < rtf.size() && rtf[j] >= '0' && rtf[j] <= '9')
                            n = n * 10 + static_cast<size_t>(rtf[j++] - '0');
                        if (j < rtf.size() && rtf[j] == ' ')
                            ++j;
                        i = j + n - 1;
                        continue;
                    }
                    ++i;   // skip the escaped character or the control word's first letter
                    continue;
                }
                if (c == '{')
                    ++depth;
                else if (c == '}' && --depth == 0)
                    return i;
            }
            return rtf.size();
        }

        // The control word right after '{' (skipping "\*"): "pict", "field", "NeXTGraphic"...
        std::string_view GroupWord(std::string_view rtf, size_t open)
        {
            size_t i = open + 1;
            while (i < rtf.size() && (rtf[i] == ' ' || rtf[i] == '\r' || rtf[i] == '\n'))
                ++i;
            if (rtf.substr(i, 2) == "\\*")
            {
                i += 2;
                while (i < rtf.size() && (rtf[i] == ' ' || rtf[i] == '\r' || rtf[i] == '\n'))
                    ++i;
            }
            if (i >= rtf.size() || rtf[i] != '\\')
                return {};
            const size_t start = ++i;
            while (i < rtf.size() && std::isalpha(static_cast<unsigned char>(rtf[i])))
                ++i;
            return rtf.substr(start, i - start);
        }

        struct Pict
        {
            std::string data;
            std::string kind;   // "png", "jpeg", other
            int goalW = 0, goalH = 0, picW = 0, picH = 0, scaleX = 100, scaleY = 100;
        };

        Pict ParsePict(std::string_view g)
        {
            Pict p;
            int nibble = -1;
            for (size_t i = 1; i + 1 < g.size(); ++i)
            {
                const char c = g[i];
                if (c == '{')
                {
                    i = GroupEnd(g, i);   // \*\blipuid, \*\picprop...: not picture data
                    continue;
                }
                if (c == '\\')
                {
                    size_t j = i + 1;
                    while (j < g.size() && std::isalpha(static_cast<unsigned char>(g[j])))
                        ++j;
                    const auto word = g.substr(i + 1, j - i - 1);
                    bool neg = false;
                    if (j < g.size() && g[j] == '-')
                    {
                        neg = true;
                        ++j;
                    }
                    long value = 0;
                    bool hasValue = false;
                    while (j < g.size() && g[j] >= '0' && g[j] <= '9')
                    {
                        value = value * 10 + (g[j++] - '0');
                        hasValue = true;
                    }
                    if (neg)
                        value = -value;
                    if (j < g.size() && g[j] == ' ')
                        ++j;
                    if (word == "pngblip") p.kind = "png";
                    else if (word == "jpegblip") p.kind = "jpeg";
                    else if (word == "emfblip" || word == "wmetafile" || word == "macpict" || word == "dibitmap" || word == "wbitmap") p.kind = std::string(word);
                    else if (word == "picwgoal") p.goalW = static_cast<int>(value);
                    else if (word == "pichgoal") p.goalH = static_cast<int>(value);
                    else if (word == "picw") p.picW = static_cast<int>(value);
                    else if (word == "pich") p.picH = static_cast<int>(value);
                    else if (word == "picscalex") p.scaleX = static_cast<int>(value);
                    else if (word == "picscaley") p.scaleY = static_cast<int>(value);
                    else if (word == "bin" && hasValue)
                    {
                        p.data.append(g.substr(j, static_cast<size_t>(std::max(0l, value))));
                        j += static_cast<size_t>(std::max(0l, value));
                    }
                    i = j - 1;
                    continue;
                }
                int v = -1;
                if (c >= '0' && c <= '9') v = c - '0';
                else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
                if (v < 0)
                    continue;
                if (nibble < 0)
                    nibble = v;
                else
                {
                    p.data.push_back(static_cast<char>(nibble << 4 | v));
                    nibble = -1;
                }
            }
            return p;
        }

        // The plain text of an RTF fragment (field instructions): control words and braces dropped.
        std::string RtfPlain(std::string_view g)
        {
            std::string out;
            for (size_t i = 0; i < g.size(); ++i)
            {
                const char c = g[i];
                if (c == '{' || c == '}' || c == '\r' || c == '\n')
                    continue;
                if (c == '\\')
                {
                    if (i + 1 < g.size() && (g[i + 1] == '\\' || g[i + 1] == '{' || g[i + 1] == '}'))
                    {
                        out.push_back(g[++i]);
                        continue;
                    }
                    size_t j = i + 1;
                    while (j < g.size() && (std::isalnum(static_cast<unsigned char>(g[j])) || g[j] == '-' || g[j] == '*'))
                        ++j;
                    if (j < g.size() && g[j] == ' ')
                        ++j;
                    i = j - 1;
                    continue;
                }
                out.push_back(c);
            }
            return out;
        }

        void Prepare(std::string_view rtf, const std::wstring& folder, RtfPrepared& p, std::string& out)
        {
            out.reserve(out.size() + rtf.size());
            for (size_t i = 0; i < rtf.size(); ++i)
            {
                const char c = rtf[i];
                if (c == '\\')
                {
                    if (rtf.substr(i, 4) == "\\bin")
                    {
                        // \binN then N raw bytes, copied untouched (they may contain braces).
                        size_t j = i + 4, n = 0;
                        while (j < rtf.size() && rtf[j] >= '0' && rtf[j] <= '9')
                            n = n * 10 + static_cast<size_t>(rtf[j++] - '0');
                        if (j < rtf.size() && rtf[j] == ' ')
                            ++j;
                        const size_t stop = std::min(rtf.size(), j + n);
                        out.append(rtf.substr(i, stop - i));
                        i = stop - 1;
                        continue;
                    }
                    out.push_back(c);
                    if (i + 1 < rtf.size())
                        out.push_back(rtf[++i]);
                    continue;
                }
                if (c != '{')
                {
                    out.push_back(c);
                    continue;
                }
                const auto word = GroupWord(rtf, i);
                const size_t end = GroupEnd(rtf, i);
                const auto group = rtf.substr(i, end - i + 1);
                if (word == "nonshppict")
                {
                    i = end;   // the old-reader copy of a picture also given as \shppict
                    continue;
                }
                if (word == "shppict" || word == "pict")
                {
                    const auto at = word == "pict" ? 0 : group.find("{\\pict");
                    if (at != std::string_view::npos)
                    {
                        const auto pg = group.substr(at, GroupEnd(group, at) - at + 1);
                        auto pict = ParsePict(pg);
                        if ((pict.kind == "png" || pict.kind == "jpeg") && !pict.data.empty())
                        {
                            // Display size: \picwgoal is in twips (1/1440 inch), pixels are 1/96 inch.
                            int w = pict.goalW > 0 ? pict.goalW * pict.scaleX / 100 / 15 : 0;
                            int h = pict.goalH > 0 ? pict.goalH * pict.scaleY / 100 / 15 : 0;
                            p.images.push_back(std::move(pict.data));
                            p.imageWidths.push_back(w);
                            p.imageHeights.push_back(h);
                            out += std::format("@@SDIMG{}@@", p.images.size() - 1);
                        }
                    }
                    i = end;
                    continue;
                }
                if (word == "NeXTGraphic")
                {
                    // {\NeXTGraphic name.png \width2000 \height1500 ...}: an RTFD attachment file.
                    auto inner = group.substr(1, group.size() - 2);
                    const auto at = inner.find("NeXTGraphic");
                    inner.remove_prefix(at + 11);
                    const auto stop = inner.find('\\');
                    const auto name = std::string(Trim(inner.substr(0, stop)));
                    if (!folder.empty() && !name.empty() && name.find("..") == std::string::npos)
                    {
                        std::string bytes;
                        const auto path = (std::filesystem::path(folder) / Widen(name)).wstring();
                        if (ReadFileBytes(path, bytes) && !SniffImage(bytes).empty())
                        {
                            p.images.push_back(std::move(bytes));
                            p.imageWidths.push_back(0);
                            p.imageHeights.push_back(0);
                            out += std::format("@@SDIMG{}@@", p.images.size() - 1);
                        }
                    }
                    i = end;
                    // TextEdit follows the attachment with a placeholder character (\'ac).
                    if (rtf.substr(i + 1, 4) == "\\'ac")
                        i += 4;
                    continue;
                }
                if (word == "field")
                {
                    // {\field{\*\fldinst HYPERLINK "url"}{\fldrslt text}}: keep the shown text, mark the link.
                    std::string link;
                    std::string_view result;
                    for (size_t k = 1; k < group.size(); ++k)
                    {
                        if (group[k] == '\\' && k + 1 < group.size())
                        {
                            ++k;
                            continue;
                        }
                        if (group[k] != '{')
                            continue;
                        const auto w = GroupWord(group, k);
                        const auto e = GroupEnd(group, k);
                        if (w == "fldinst")
                        {
                            const auto instr = std::string(Trim(RtfPlain(group.substr(k, e - k + 1))));
                            if (instr.starts_with("HYPERLINK"))
                            {
                                const auto q1 = instr.find('"');
                                const auto q2 = q1 == std::string::npos ? q1 : instr.find('"', q1 + 1);
                                if (q2 != std::string::npos)
                                    link = instr.substr(q1 + 1, q2 - q1 - 1);
                            }
                        }
                        else if (w == "fldrslt")
                        {
                            auto r = group.substr(k + 1, e - k - 1);
                            const auto pos = r.find("fldrslt");
                            r.remove_prefix(pos + 7);
                            result = r;
                        }
                        k = e;
                    }
                    out.push_back('{');
                    if (!link.empty())
                    {
                        p.links.push_back(link);
                        out += std::format("@@SDLNK{}@@", p.links.size() - 1);
                    }
                    Prepare(result, folder, p, out);
                    if (!link.empty())
                        out += "@@SDEND@@";
                    out.push_back('}');
                    i = end;
                    continue;
                }
                out.push_back(c);
            }
        }
    }

    RtfPrepared RtfPrepare(std::string_view rtf, const std::wstring& folder)
    {
        RtfPrepared p;
        Prepare(rtf, folder, p, p.rtf);
        return p;
    }

    void RtfBuildDocument(const std::vector<RtfParagraph>& paragraphs, RtfPrepared& prepared, Document& doc)
    {
        // Body size: the size most characters are set in.
        std::map<int, size_t> bySize;   // half points -> characters
        for (const auto& p : paragraphs)
            for (const auto& r : p.runs)
                bySize[static_cast<int>(std::lround(r.sizePt * 2))] += r.text.size();
        int body = 24;
        size_t most = 0;
        for (const auto& [size, count] : bySize)
            if (count > most)
            {
                most = count;
                body = size;
            }

        // A paragraph set entirely in a size clearly above the body, and short, reads as a heading.
        const auto headingSize = [&](const RtfParagraph& p) -> int {
            if (p.numbering != 0 || p.runs.empty())
                return 0;
            size_t chars = 0;
            int size = 0;
            for (const auto& r : p.runs)
            {
                const auto t = Trim(Narrow(r.text));
                if (t.empty() || t.starts_with("@@SDIMG"))
                    continue;
                const int s = static_cast<int>(std::lround(r.sizePt * 2));
                if (size == 0)
                    size = s;
                else if (s != size)
                    return 0;
                chars += r.text.size();
            }
            if (chars == 0 || chars > 200 || size * 10 < body * 12)
                return 0;
            return size;
        };
        std::vector<int> headingSizes;
        for (const auto& p : paragraphs)
            if (const int s = headingSize(p); s && std::find(headingSizes.begin(), headingSizes.end(), s) == headingSizes.end())
                headingSizes.push_back(s);
        std::sort(headingSizes.rbegin(), headingSizes.rend());
        // List levels count from the shallowest list indent in the document.
        int minIndent = INT_MAX;
        for (const auto& p : paragraphs)
            if (p.numbering != 0)
                minIndent = std::min(minIndent, p.indentTwips);

        int link = -1;   // open link marker, carried across runs
        bool lastWasCode = false;
        for (const auto& p : paragraphs)
        {
            Block b;
            std::vector<Span> spans;
            bool allMono = true, anyText = false;
            for (const auto& r : p.runs)
            {
                auto text = Narrow(r.text);
                // Split at markers.
                size_t pos = 0;
                while (pos <= text.size())
                {
                    const auto at = text.find("@@SD", pos);
                    const auto piece = text.substr(pos, (at == std::string::npos ? text.size() : at) - pos);
                    if (!piece.empty())
                    {
                        // \line comes back as a vertical tab; RichEdit's object placeholder is dropped.
                        size_t s = 0;
                        while (s <= piece.size())
                        {
                            const auto vt = piece.find('\v', s);
                            auto part = piece.substr(s, (vt == std::string::npos ? piece.size() : vt) - s);
                            for (size_t k; (k = part.find("\xEF\xBF\xBC")) != std::string::npos;)
                                part.erase(k, 3);
                            if (!part.empty())
                            {
                                Span sp;
                                sp.text = part;
                                sp.bold = r.bold;
                                sp.italic = r.italic;
                                sp.strike = r.strike;
                                sp.code = r.mono;
                                if (link >= 0 && static_cast<size_t>(link) < prepared.links.size())
                                    sp.link = prepared.links[static_cast<size_t>(link)];
                                if (!Trim(part).empty())
                                {
                                    anyText = true;
                                    allMono &= r.mono;
                                }
                                spans.push_back(std::move(sp));
                            }
                            if (vt == std::string::npos)
                                break;
                            Span br;
                            br.lineBreak = true;
                            spans.push_back(br);
                            s = vt + 1;
                        }
                    }
                    if (at == std::string::npos)
                        break;
                    const auto close = text.find("@@", at + 4);
                    if (close == std::string::npos)
                        break;
                    const auto marker = text.substr(at + 4, close - at - 4);   // "IMG3", "LNK0", "END"
                    if (marker.starts_with("IMG"))
                    {
                        const auto n = static_cast<size_t>(std::atoi(marker.c_str() + 3));
                        if (n < prepared.images.size() && !prepared.images[n].empty())
                        {
                            Span sp;
                            sp.image = AddImage(doc, std::move(prepared.images[n]));
                            prepared.images[n].clear();
                            auto& img = doc.images.back();
                            if (prepared.imageWidths[n] > 0 && prepared.imageHeights[n] > 0)
                            {
                                img.width = prepared.imageWidths[n];
                                img.height = prepared.imageHeights[n];
                            }
                            spans.push_back(std::move(sp));
                            anyText = true;
                            allMono = false;
                        }
                    }
                    else if (marker.starts_with("LNK"))
                        link = std::atoi(marker.c_str() + 3);
                    else if (marker == "END")
                        link = -1;
                    pos = close + 2;
                }
            }
            NormalizeSpans(spans);
            if (!anyText)
            {
                lastWasCode = false;
                continue;   // empty paragraphs are spacing
            }
            const int hs = headingSize(p);
            if (hs && !allMono)
            {
                b.kind = BlockKind::Heading;
                b.level = std::min(6, static_cast<int>(std::find(headingSizes.begin(), headingSizes.end(), hs) - headingSizes.begin()) + 1);
                for (auto& s : spans)
                    s.bold = false;
            }
            else if (p.numbering != 0)
            {
                b.kind = BlockKind::ListItem;
                b.ordered = p.numbering >= 2;
                b.level = std::clamp((p.indentTwips - minIndent + 180) / 360, 0, 8);
                b.start = p.numberStart;
                // Consecutive numbered paragraphs count on from the previous one at the same level.
                if (b.ordered && !doc.blocks.empty() && doc.blocks.back().kind == BlockKind::ListItem && doc.blocks.back().ordered &&
                    doc.blocks.back().level == b.level)
                    b.start = doc.blocks.back().start + 1;
            }
            else if (allMono)
            {
                const auto text = PlainText(spans);
                if (lastWasCode && !doc.blocks.empty() && doc.blocks.back().kind == BlockKind::Code)
                {
                    doc.blocks.back().spans.back().text += "\n" + text;
                    continue;
                }
                b.kind = BlockKind::Code;
                Span s;
                s.text = text;
                spans = { s };
            }
            lastWasCode = b.kind == BlockKind::Code;
            b.spans = std::move(spans);
            doc.blocks.push_back(std::move(b));
        }
    }

    namespace
    {
        struct StreamSource
        {
            std::string_view data;
            size_t pos = 0;
        };

        DWORD CALLBACK StreamIn(DWORD_PTR cookie, LPBYTE buffer, LONG size, LONG* read)
        {
            auto* s = reinterpret_cast<StreamSource*>(cookie);
            const size_t n = std::min(static_cast<size_t>(size), s->data.size() - s->pos);
            memcpy(buffer, s->data.data() + s->pos, n);
            s->pos += n;
            *read = static_cast<LONG>(n);
            return 0;
        }

        CHARFORMAT2W CharFormat(HWND edit, LONG from, LONG to)
        {
            CHARRANGE range{ from, to };
            SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
            CHARFORMAT2W cf{};
            cf.cbSize = sizeof(cf);
            cf.dwMask = CFM_BOLD | CFM_ITALIC | CFM_STRIKEOUT | CFM_SIZE | CFM_FACE;
            // For a selection, RichEdit clears the mask bits of attributes that vary inside it.
            SendMessageW(edit, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
            return cf;
        }

        bool Uniform(HWND edit, LONG from, LONG to)
        {
            const auto cf = CharFormat(edit, from, to);
            const DWORD all = CFM_BOLD | CFM_ITALIC | CFM_STRIKEOUT | CFM_SIZE | CFM_FACE;
            return (cf.dwMask & all) == all;
        }
    }

    bool ReadRtf(std::string_view rtf, const std::wstring& rtfdFolder, Document& doc, std::wstring& error)
    {
        if (!rtf.starts_with("{\\rtf"))
        {
            error = L"not an RTF document";
            return false;
        }
        // msftedit.dll is RichEdit 4.1+ ("RICHEDIT50W"), part of every Windows since XP SP1.
        static const HMODULE richEdit = LoadLibraryW(L"msftedit.dll");
        if (!richEdit)
        {
            error = L"Windows' RichEdit (msftedit.dll) is missing";
            return false;
        }
        auto prepared = RtfPrepare(rtf, rtfdFolder);

        // A hidden, never-shown window owned by this thread; nothing is ever painted.
        wil::unique_hwnd edit(CreateWindowExW(0, MSFTEDIT_CLASS, L"", WS_POPUP | ES_MULTILINE, 0, 0, 800, 600, nullptr, nullptr,
                                              GetModuleHandleW(nullptr), nullptr));
        if (!edit)
        {
            error = std::format(L"cannot start RichEdit (error {})", GetLastError());
            return false;
        }
        SendMessageW(edit.get(), EM_EXLIMITTEXT, 0, 0x7FFFFFFE);
        StreamSource source{ prepared.rtf };
        EDITSTREAM stream{};
        stream.dwCookie = reinterpret_cast<DWORD_PTR>(&source);
        stream.pfnCallback = &StreamIn;
        SendMessageW(edit.get(), EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&stream));
        if (stream.dwError != 0)
        {
            error = L"RichEdit could not read this RTF";
            return false;
        }

        GETTEXTLENGTHEX lengthEx{ GTL_NUMCHARS | GTL_PRECISE, 1200 };
        const auto length = static_cast<LONG>(SendMessageW(edit.get(), EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&lengthEx), 0));
        std::wstring text(static_cast<size_t>(std::max(0l, length)) + 1, L'\0');
        GETTEXTEX getText{};
        getText.cb = static_cast<DWORD>(text.size() * sizeof(wchar_t));
        getText.flags = GT_DEFAULT;   // paragraphs end in '\r'; one character per position
        getText.codepage = 1200;      // UTF-16
        const auto got = SendMessageW(edit.get(), EM_GETTEXTEX, reinterpret_cast<WPARAM>(&getText), reinterpret_cast<LPARAM>(text.data()));
        text.resize(static_cast<size_t>(std::max<LRESULT>(0, got)));

        std::vector<RtfParagraph> paragraphs;
        LONG start = 0;
        const auto total = static_cast<LONG>(text.size());
        while (start <= total)
        {
            LONG end = start;
            while (end < total && text[static_cast<size_t>(end)] != L'\r')
                ++end;
            RtfParagraph para;
            {
                CHARRANGE at{ start, start };
                SendMessageW(edit.get(), EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&at));
                PARAFORMAT2 pf{};
                pf.cbSize = sizeof(pf);
                pf.dwMask = PFM_NUMBERING | PFM_NUMBERINGSTART | PFM_STARTINDENT;
                SendMessageW(edit.get(), EM_GETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&pf));
                para.numbering = pf.wNumbering;
                para.numberStart = std::max<int>(1, pf.wNumberingStart);
                para.indentTwips = pf.dxStartIndent;
            }
            // Runs: grow a uniform range by doubling, then binary-search its exact end, so a long
            // paragraph costs a few dozen messages per run instead of one per character.
            LONG p = start;
            while (p < end)
            {
                LONG good = 1, step = 1;
                while (p + step * 2 <= end && Uniform(edit.get(), p, p + step * 2))
                {
                    step *= 2;
                    good = step;
                }
                LONG lo = good, hi = std::min<LONG>(step * 2, end - p);
                if (hi > lo && Uniform(edit.get(), p, p + hi))
                    lo = hi;
                while (hi - lo > 1)
                {
                    const LONG mid = (lo + hi) / 2;
                    if (Uniform(edit.get(), p, p + mid))
                        lo = mid;
                    else
                        hi = mid;
                }
                const auto cf = CharFormat(edit.get(), p, p + lo);
                RtfRun run;
                run.text = text.substr(static_cast<size_t>(p), static_cast<size_t>(lo));
                run.bold = (cf.dwEffects & CFE_BOLD) != 0;
                run.italic = (cf.dwEffects & CFE_ITALIC) != 0;
                run.strike = (cf.dwEffects & CFE_STRIKEOUT) != 0;
                run.sizePt = cf.yHeight / 20.0f;
                run.mono = IsMonoFontName(Narrow(cf.szFaceName));
                para.runs.push_back(std::move(run));
                p += lo;
            }
            paragraphs.push_back(std::move(para));
            start = end + 1;
        }
        RtfBuildDocument(paragraphs, prepared, doc);
        return true;
    }

    bool ReadRtfd(const std::wstring& folder, Document& doc, std::wstring& error)
    {
        // Mac TextEdit packages: TXT.rtf next to the attachments (some writers use other names).
        std::wstring rtfPath = (std::filesystem::path(folder) / L"TXT.rtf").wstring();
        if (GetFileAttributesW(rtfPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            rtfPath.clear();
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(folder, ec))
                if (e.path().extension() == L".rtf" || e.path().extension() == L".RTF")
                {
                    rtfPath = e.path().wstring();
                    break;
                }
        }
        if (rtfPath.empty())
        {
            error = L"this RTFD has no TXT.rtf inside";
            return false;
        }
        std::string bytes;
        if (!ReadFileBytes(rtfPath, bytes, &error))
            return false;
        return ReadRtf(bytes, folder, doc, error);
    }
}
