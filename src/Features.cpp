#include "Features.h"
#include "Utf.h"

#include <windows.h>

#include <vector>

namespace sd
{
    bool Features::RecipeOn(const Recipe& recipe) const
    {
        return sections[static_cast<size_t>(RingFromId(recipe.ring))] && !off.contains(recipe.id);
    }

    int Features::SectionsOn() const
    {
        int n = 0;
        for (const bool on : sections)
            n += on ? 1 : 0;
        return n;
    }

    std::string JoinIds(const std::set<std::string>& ids)
    {
        std::string text;
        for (const auto& id : ids)
            text += (text.empty() ? "" : ",") + id;
        return text;
    }

    std::set<std::string> SplitIds(std::string_view text)
    {
        std::set<std::string> ids;
        size_t start = 0;
        while (start <= text.size())
        {
            const size_t end = std::min(text.find(',', start), text.size());
            auto id = text.substr(start, end - start);
            while (!id.empty() && id.front() == ' ')
                id.remove_prefix(1);
            while (!id.empty() && id.back() == ' ')
                id.remove_suffix(1);
            if (!id.empty())
                ids.emplace(id);
            start = end + 1;
        }
        return ids;
    }

    namespace
    {
        constexpr const wchar_t* kSectionKeys[kRingCount] = { L"Convert", L"Tools", L"Reduce", L"Files" };
    }

    Features LoadFeatures()
    {
        const auto path = SettingsPath();
        Features f;
        for (int i = 0; i < kRingCount; ++i)
            f.sections[static_cast<size_t>(i)] = GetPrivateProfileIntW(L"Features", kSectionKeys[i], 1, path.c_str()) != 0;
        if (f.SectionsOn() == 0)
            f.sections[0] = true;   // a wheel with no section cannot open
        // Every recipe id could be off at once: size the buffer for that, not for a typical line.
        std::vector<wchar_t> buf(32 * 1024);
        GetPrivateProfileStringW(L"Features", L"Off", L"", buf.data(), static_cast<DWORD>(buf.size()), path.c_str());
        f.off = SplitIds(Narrow(buf.data()));
        return f;
    }

    void SaveFeatures(const Features& f)
    {
        const auto path = SettingsPath();
        for (int i = 0; i < kRingCount; ++i)
            WritePrivateProfileStringW(L"Features", kSectionKeys[i], f.sections[static_cast<size_t>(i)] ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"Features", L"Off", Widen(JoinIds(f.off)).c_str(), path.c_str());
    }
}
