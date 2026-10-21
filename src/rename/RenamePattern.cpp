#include "rename/RenamePattern.h"
#include "PathUtil.h"

#include <windows.h>

#include <cwctype>
#include <filesystem>
#include <format>
#include <map>

namespace sd::rename
{
    namespace
    {
        constexpr wchar_t kFields[] = L"Use {name}, {n}, {n:000}, {date}, {ext} or {folder}.";

        bool SameText(std::wstring_view a, std::wstring_view b, bool matchCase)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (matchCase ? a[i] != b[i] : std::towlower(a[i]) != std::towlower(b[i]))
                    return false;
            return true;
        }

        std::wstring ReplaceAll(std::wstring text, std::wstring_view find, std::wstring_view with, bool matchCase)
        {
            if (find.empty())
                return text;
            for (size_t at = 0; at + find.size() <= text.size();)
            {
                if (SameText(std::wstring_view(text).substr(at, find.size()), find, matchCase))
                {
                    text.replace(at, find.size(), with);
                    at += with.size();
                }
                else
                {
                    ++at;
                }
            }
            return text;
        }

        // NTFS compares names without case (by its upcase table; towupper is close enough here).
        std::wstring Key(std::wstring_view path)
        {
            std::wstring key(path);
            for (auto& c : key)
                c = static_cast<wchar_t>(std::towupper(c));
            return key;
        }

        bool DiskHas(const std::wstring& path)
        {
            return PathExists(path);
        }
    }

    std::wstring Expand(const Pattern& pattern, const Item& item, size_t n, std::wstring& error)
    {
        const std::filesystem::path path(item.path);
        const std::wstring stem = item.folder ? path.filename().wstring() : path.stem().wstring();
        std::wstring ext = item.folder ? L"" : path.extension().wstring();
        const std::wstring name = ReplaceAll(stem, pattern.find, pattern.replace, pattern.matchCase);
        const long long number = static_cast<long long>(pattern.start) + static_cast<long long>(n);

        std::wstring out;
        const std::wstring_view text = pattern.text;
        for (size_t i = 0; i < text.size(); ++i)
        {
            const wchar_t c = text[i];
            if (c == L'}')
            {
                if (i + 1 < text.size() && text[i + 1] == L'}')
                {
                    out += L'}';
                    ++i;
                    continue;
                }
                error = L"A “}” has no “{” before it. Type }} for the brace itself.";
                return {};
            }
            if (c != L'{')
            {
                out += c;
                continue;
            }
            if (i + 1 < text.size() && text[i + 1] == L'{')
            {
                out += L'{';
                ++i;
                continue;
            }
            const size_t close = text.find(L'}', i + 1);
            if (close == std::wstring_view::npos)
            {
                error = std::format(L"A “{{” is not closed. {}", kFields);
                return {};
            }
            const std::wstring_view field = text.substr(i + 1, close - i - 1);
            i = close;
            if (field == L"name")
                out += name;
            else if (field == L"ext")
                out += ext.empty() ? L"" : ext.substr(1);
            else if (field == L"date")
                out += item.date;
            else if (field == L"folder")
                out += path.parent_path().filename().wstring();
            else if (field == L"n")
                out += std::to_wstring(number);
            else if (field.starts_with(L"n:") && field.size() > 2 && field.size() <= 12 &&
                     field.find_first_not_of(L'0', 2) == std::wstring_view::npos)
                out += std::format(L"{:0{}}", number, field.size() - 2);
            else
            {
                error = std::format(L"{{{}}} is not a field. {}", field, kFields);
                return {};
            }
        }
        return out + ext;
    }

    std::wstring NameProblem(std::wstring_view name)
    {
        if (name.empty() || name == L"." || name == L"..")
            return L"the name is empty";
        if (name.size() > 255)
            return L"the name is too long";
        for (const wchar_t c : name)
            if (c < 32 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos)
                return std::format(L"a name can't contain {}", c < 32 ? std::wstring(L"control characters") : std::format(L"“{}”", c));
        if (name.back() == L'.' || name.back() == L' ')
            return L"a name can't end with a dot or a space";
        // Device names are reserved with any extension ("con.txt" too).
        std::wstring base(name.substr(0, name.find(L'.')));
        while (!base.empty() && base.back() == L' ')
            base.pop_back();
        static constexpr const wchar_t* kReserved[] = { L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6",
                                                        L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6",
                                                        L"LPT7", L"LPT8", L"LPT9" };
        for (const wchar_t* reserved : kReserved)
            if (SameText(base, reserved, false))
                return std::format(L"{} is a name Windows keeps for itself", reserved);
        return {};
    }

    Plan MakePlan(const Pattern& pattern, const std::vector<Item>& items, const Exists& exists)
    {
        Plan plan;
        const Exists& has = exists ? exists : Exists(DiskHas);
        plan.items.reserve(items.size());
        for (size_t i = 0; i < items.size(); ++i)
        {
            std::wstring error;
            const auto name = Expand(pattern, items[i], i, error);
            if (!error.empty())
            {
                plan.items.clear();
                plan.error = std::move(error);
                return plan;
            }
            Planned p;
            p.from = items[i].path;
            p.to = (std::filesystem::path(items[i].path).parent_path() / name).wstring();
            p.problem = NameProblem(name);   // a slash in it is a problem too, so `to` stays in the folder
            plan.items.push_back(std::move(p));
        }

        // Two items given one name: both are marked, so it is clear which ones clash.
        std::map<std::wstring, std::vector<size_t>> targets;
        for (size_t i = 0; i < plan.items.size(); ++i)
            targets[Key(plan.items[i].to)].push_back(i);
        for (auto& [key, indices] : targets)
            if (indices.size() > 1)
                for (const size_t i : indices)
                    if (plan.items[i].problem.empty())
                        plan.items[i].problem = L"another file gets the same name";

        for (auto& p : plan.items)
        {
            // Taken by something else: never overwritten. The item itself (the same name, or only its
            // case changed) is not in the way.
            if (p.problem.empty() && Key(p.to) != Key(p.from) && has(p.to))
                p.problem = std::format(L"“{}” is already in this folder", FileName(p.to));
            if (!p.problem.empty())
                ++plan.problems;
            else if (p.to != p.from)
                ++plan.changes;
        }
        return plan;
    }
}
