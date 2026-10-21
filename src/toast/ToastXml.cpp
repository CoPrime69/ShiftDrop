#include "toast/ToastXml.h"

#include <format>

namespace sd::toast
{
    namespace
    {
        constexpr std::wstring_view kReveal = L"reveal", kOpen = L"open";

        // "C:\..." or "\\server\share\...".
        bool IsAbsolute(std::wstring_view path)
        {
            const bool drive = path.size() >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
                               path[1] == L':' && path[2] == L'\\';
            return drive || path.starts_with(L"\\\\");
        }
    }

    std::wstring Arguments(Verb verb, const std::vector<std::wstring>& paths, size_t budget)
    {
        std::wstring args(verb == Verb::Open ? kOpen : kReveal);
        for (size_t i = 0; i < paths.size(); ++i)
        {
            if (i > 0 && args.size() + 1 + paths[i].size() > budget)
                break;
            args += L'|';
            args += paths[i];
            if (verb == Verb::Open)
                break;   // one file
        }
        return args;
    }

    std::optional<Activation> ParseArguments(std::wstring_view arguments)
    {
        Activation a;
        const size_t bar = arguments.find(L'|');
        const auto verb = arguments.substr(0, bar);
        if (verb == kReveal)
            a.verb = Verb::Reveal;
        else if (verb == kOpen)
            a.verb = Verb::Open;
        else
            return std::nullopt;
        for (size_t start = bar; start != std::wstring_view::npos && start < arguments.size();)
        {
            const size_t next = arguments.find(L'|', start + 1);
            const auto path = arguments.substr(start + 1, next == std::wstring_view::npos ? std::wstring_view::npos : next - start - 1);
            if (!IsAbsolute(path))
                return std::nullopt;
            a.paths.emplace_back(path);
            start = next;
        }
        if (a.paths.empty() || (a.verb == Verb::Open && a.paths.size() != 1))
            return std::nullopt;
        return a;
    }

    std::wstring Summary(const std::vector<std::wstring>& names, std::wstring_view where)
    {
        if (names.empty())
            return {};
        const auto who = names.size() == 1 ? names[0] : std::format(L"{} and {} more", names[0], names.size() - 1);
        return where.empty() ? who + L"." : std::format(L"{}, {}.", who, where);
    }

    bool ShouldNotify(bool shownInPanel, std::chrono::milliseconds took, bool cancelled)
    {
        if (!shownInPanel)
            return true;
        return !cancelled && took > kLongJob;
    }

    std::wstring EscapeXml(std::wstring_view text)
    {
        std::wstring out;
        out.reserve(text.size());
        for (const wchar_t c : text)
        {
            switch (c)
            {
            case L'&': out += L"&amp;"; break;
            case L'<': out += L"&lt;"; break;
            case L'>': out += L"&gt;"; break;
            case L'"': out += L"&quot;"; break;
            case L'\'': out += L"&apos;"; break;
            case L'\n': out += L"&#10;"; break;   // kept as a line break, not folded into a space
            default:
                if (c >= 0x20 || c == L'\t')
                    out += c;   // other control characters are not allowed in XML
                break;
            }
        }
        return out;
    }

    std::wstring BuildXml(const Content& content)
    {
        std::wstring xml;
        if (content.outputs.empty())
            xml = L"<toast>";
        else
            xml = std::format(L"<toast launch=\"{}\" activationType=\"foreground\">", EscapeXml(Arguments(Verb::Reveal, content.outputs)));
        xml += L"<visual><binding template=\"ToastGeneric\">";
        xml += std::format(L"<text>{}</text>", EscapeXml(content.title));
        if (!content.text.empty())
            xml += std::format(L"<text>{}</text>", EscapeXml(content.text));
        xml += L"</binding></visual>";
        if (content.failed)
        {
            // Windows' own dismiss: nothing to open.
            xml += L"<actions><action content=\"Dismiss\" arguments=\"dismiss\" activationType=\"system\"/></actions>";
        }
        else if (!content.outputs.empty())
        {
            xml += L"<actions>";
            if (content.outputs.size() == 1)
                xml += std::format(L"<action content=\"Open\" arguments=\"{}\" activationType=\"foreground\"/>",
                                   EscapeXml(Arguments(Verb::Open, content.outputs)));
            xml += std::format(L"<action content=\"Open folder\" arguments=\"{}\" activationType=\"foreground\"/>",
                               EscapeXml(Arguments(Verb::Reveal, content.outputs)));
            xml += L"</actions>";
        }
        xml += L"</toast>";
        return xml;
    }
}
