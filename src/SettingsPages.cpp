#include "SettingsPages.h"
#include "Brand.h"
#include "Strings.h"

#include <algorithm>
#include <array>
#include <string>

namespace sd::settings
{
    namespace
    {
        // Glyphs, Segoe Fluent Icons (Windows 11) and MDL2 Assets (Windows 10) share these code points.
        constexpr wchar_t kGear = 0xE713, kPalette = 0xE790, kWheel = 0xE895, kDocument = 0xE8A5, kOptions = 0xE9E9, kInfo = 0xE946;
        constexpr wchar_t kPower = 0xE7E8, kFont = 0xE8D2, kPin = 0xE718, kDownload = 0xE896, kPrompt = 0xE756, kBrightness = 0xE706;
        constexpr wchar_t kPersonalize = 0xE771, kFullScreen = 0xE740, kPanel = 0xE737, kFolder = 0xE8B7, kList = 0xE8FD;
        constexpr wchar_t kCode = 0xE943, kLock = 0xE72E, kMenu = 0xE700, kUpdate = 0xE777, kRefresh = 0xE72C;

        using enum Item;
        using enum Page;
        constexpr std::array<Entry, kItemCount> kEntries{ {
            { StartWithWindows, General, nullptr, L"Start with Windows", L"Open " SD_NAME L" in the tray when you sign in", kPower,
              L"Run key; [Startup] StartWithWindows", L"tray menu", L"login sign in startup autostart boot" },
            { OcrLanguage, General, nullptr, L"Text recognition language", L"Used by Copy text from images", kFont, L"[Ocr] Language",
              L"tray menu", L"ocr scan read" },
            { FloatShelf, General, nullptr, L"Show Float shelf", L"Keep the shelf docked to the screen edge", kPin, L"shelf.json",
              L"tray menu", L"float shelf dock" },
            { BrowserDrops, General, L"Files", L"Save browser drops to", L"Files dragged in from a web browser are saved here first", kDownload,
              L"[Output] VirtualFiles", L"tray menu", L"browser mail outlook downloads desktop ask virtual" },
            { Terminal, General, L"Files", L"Terminal app", L"Used by File actions \u203A Terminal", kPrompt, L"[FileActions] Terminal",
              L"Settings \u203A Terminal", L"powershell command prompt cmd console shell" },
            { TerminalCommand, General, L"Files", L"Command line", L"For a terminal of your own: {folder} is the folder", 0,
              L"[FileActions] TerminalCommand", L"Settings \u203A Command line", L"custom terminal command" },
            { Theme, Appearance, nullptr, L"Theme", L"Light, dark, or follow Windows", kBrightness, L"[Appearance] Theme",
              L"Settings \u203A Theme", L"dark light mode colours" },
            { Style, Appearance, nullptr, L"Style", L"Glass blurs the desktop behind the wheel", kPersonalize, L"[Appearance] Style",
              L"tray menu", L"solid glass blur frosted" },
            { Size, Appearance, nullptr, L"Size", L"How big the wheel opens", kFullScreen, L"[Appearance] Size", L"tray menu",
              L"auto small medium large bigger smaller scale" },
            { PanelSize, Appearance, nullptr, L"Top panel size", L"On top of the wheel\u2019s size, from 50% to 300%", kPanel,
              L"[Appearance] PanelSize", L"Settings \u203A Top panel size", L"island bigger smaller scale percent" },
            { Accent, Appearance, nullptr, L"Accent colour", L"Used for the hovered tile, the active section and progress", kPalette,
              L"[Appearance] Accent, AccentCustom", L"Settings \u203A Accent colour", L"color colour windows tide blue purple pink red orange green graphite custom" },
            { WindowsAccent, Appearance, nullptr, L"Use my Windows accent colour", nullptr, 0, L"[Appearance] Accent=windows",
              L"Settings \u203A Accent colour", L"color colour windows" },
            { WheelContents, Wheel, nullptr, L"Wheel contents", L"Turn sections and actions on or off", kWheel,
              L"[Features] Convert, Tools, Reduce, Files, Off", L"Settings \u203A the tick list", L"hide show slices tiles actions sections" },
            { OutputNaming, Output, nullptr, L"Output naming", L"How copies are named. The original is never renamed.", kDocument,
              L"[Output] Naming", L"Settings \u203A Output names", L"name names suffix converted folder subfolder live example default" },
            { ShellMenu, Advanced, nullptr, L"Right-click menu entry", L"\u201C" SD_NAME L"\u2026\u201D in File Explorer\u2019s right-click menu", kMenu,
              L"HKCU\\Software\\Classes", L"tray menu", L"explorer context menu shell" },
            { RecipesFolder, Advanced, nullptr, L"Recipes folder", L"Custom actions you\u2019ve added to the wheel", kFolder, nullptr,
              L"tray menu", L"recipes json custom" },
            { LogsFolder, Advanced, nullptr, L"Logs folder", L"Useful when reporting a problem on GitHub", kList, nullptr, L"tray menu",
              L"log logs problem bug report" },
            { UpdateMode, Updates, nullptr, L"When an update is available", L"Checked once a day. A new version is offered a week after its release.",
              kUpdate, L"[Updates] Mode", nullptr, L"update updates upgrade new version automatic install download ask off" },
            { CheckUpdates, Updates, nullptr, L"Check for updates", L"Asks GitHub for the latest release now", kRefresh,
              L"[Updates] LastCheck", nullptr, L"update updates upgrade check now latest new version github" },
            { Version, About, nullptr, L"Version", nullptr, 0, nullptr, nullptr, L"about version" },
            { SourceCode, About, nullptr, L"Source code", L"github.com/CoPrime69/" SD_NAME, kCode, nullptr, nullptr,
              L"github source code free open" },
            { Privacy, About, nullptr, L"Private \u00B7 No account \u00B7 No telemetry",
              L"Your files never leave this PC. Only the update check goes online, to GitHub, and sends nothing about you.",
              kLock, nullptr, nullptr, L"privacy offline network" },
        } };
    }

    const wchar_t* PageTitle(Page page)
    {
        constexpr const wchar_t* kTitles[kPageCount] = { L"General", L"Appearance", L"Wheel", L"Output", L"Advanced", L"Updates", L"About" };
        const auto i = static_cast<size_t>(page);
        return i < kPageCount ? kTitles[i] : L"";
    }

    wchar_t PageIcon(Page page)
    {
        constexpr wchar_t kIcons[kPageCount] = { kGear, kPalette, kWheel, kDocument, kOptions, kUpdate, kInfo };
        const auto i = static_cast<size_t>(page);
        return i < kPageCount ? kIcons[i] : 0;
    }

    std::span<const Entry> Catalogue()
    {
        return kEntries;
    }

    const Entry& Find(Item item)
    {
        for (const auto& e : kEntries)
            if (e.item == item)
                return e;
        return kEntries.front();
    }

    std::vector<Item> OnPage(Page page)
    {
        std::vector<Item> items;
        for (const auto& e : kEntries)
            if (e.page == page)
                items.push_back(e.item);
        return items;
    }

    std::span<const wchar_t* const> Options(Item item)
    {
        static constexpr const wchar_t* kTheme[] = { L"System", L"Light", L"Dark" };
        static constexpr const wchar_t* kStyle[] = { L"Solid", L"Glass" };
        static constexpr const wchar_t* kSize[] = { L"Auto", L"Small", L"Medium", L"Large" };
        static constexpr const wchar_t* kDrops[] = { L"Desktop", L"Downloads", L"Ask every time" };
        static constexpr const wchar_t* kTerminal[] = { L"Windows Terminal", L"PowerShell", L"Command Prompt", L"Custom command" };
        static constexpr const wchar_t* kRecipes[] = { L"Open recipes folder" };
        static constexpr const wchar_t* kLogs[] = { L"Open logs folder" };
        static constexpr const wchar_t* kSource[] = { L"Open the source code on GitHub" };
        static constexpr const wchar_t* kWheel[] = { L"Find an action", L"Turn off this section", L"Turn on this section", L"Reset to default" };
        static constexpr const wchar_t* kAccent[] = { L"Custom\u2026", L"Delete colour" };
        // In updates::Mode's order.
        static constexpr const wchar_t* kUpdateModes[] = { L"Install automatically", L"Ask me first", L"Off" };
        static constexpr const wchar_t* kCheck[] = { L"Check now", L"Update now" };
        switch (item)
        {
        case Item::Theme: return kTheme;
        case Item::Style: return kStyle;
        case Item::Size: return kSize;
        case Item::BrowserDrops: return kDrops;
        case Item::Terminal: return kTerminal;
        case Item::RecipesFolder: return kRecipes;
        case Item::LogsFolder: return kLogs;
        case Item::SourceCode: return kSource;
        case Item::WheelContents: return kWheel;
        case Item::Accent: return kAccent;
        case Item::UpdateMode: return kUpdateModes;
        case Item::CheckUpdates: return kCheck;
        default: return {};
        }
    }

    std::vector<std::wstring> Alternatives(const std::wstring& word)
    {
        // Words people type for the same thing: a word of a group finds the group's other words too.
        static constexpr const wchar_t* kGroups[][6] = {   // each ends at its first null
            { L"colour", L"color" },
            { L"accent", L"highlight" },
            { L"theme", L"dark", L"light", L"mode" },
            { L"size", L"bigger", L"smaller", L"larger", L"scale" },
            { L"naming", L"rename", L"name", L"suffix" },
            { L"startup", L"start with windows", L"autostart", L"sign in", L"login", L"boot" },
            { L"ocr", L"text recognition", L"recognise", L"recognize" },
            { L"reduce", L"compress", L"shrink" },
            { L"delete", L"remove" },
            { L"folder", L"directory" },
            { L"right-click", L"context menu" },
            { L"github", L"source code" },
            { L"update", L"upgrade", L"new version" },
        };
        std::vector<std::wstring> words{ word };
        for (const auto& group : kGroups)
        {
            const auto end = std::find(std::begin(group), std::end(group), nullptr);
            if (std::any_of(std::begin(group), end, [&](const wchar_t* w) { return word == w; }))
                for (auto w = std::begin(group); w != end; ++w)
                    if (std::find(words.begin(), words.end(), *w) == words.end())
                        words.emplace_back(*w);
        }
        return words;
    }

    bool HasEveryWordOrAlternative(std::wstring_view text, const std::vector<std::wstring>& words)
    {
        const std::wstring haystack = Lower(text);
        for (const auto& word : words)
        {
            const auto alternatives = Alternatives(word);
            if (std::none_of(alternatives.begin(), alternatives.end(), [&](const std::wstring& w) { return haystack.find(w) != std::wstring::npos; }))
                return false;
        }
        return true;
    }

    namespace
    {
        // A card's name (its title and its controls' labels) and everything it is known by.
        std::wstring NameOf(const Entry& e)
        {
            std::wstring name = e.title;
            for (const wchar_t* option : Options(e.item))
                name += std::wstring(L" ") + option;
            return name;
        }

        std::wstring TextOf(const Entry& e)
        {
            std::wstring text = NameOf(e) + L" " + PageTitle(e.page);
            for (const wchar_t* more : { e.description, e.group, e.keywords })
                if (more)
                    text += std::wstring(L" ") + more;
            return text;
        }

        // How well `name` and `text` answer `words`: 0 the name has every word, 1 the text has, 2 with
        // other words for some, -1 not at all.
        int Rank(std::wstring_view name, std::wstring_view text, const std::vector<std::wstring>& words)
        {
            if (HasEveryWord(name, words))
                return 0;
            if (HasEveryWord(text, words))
                return 1;
            return HasEveryWordOrAlternative(text, words) ? 2 : -1;
        }

        // Does `name` hold at least one of `words` (or another word for it)? A kind of file or an action
        // is found by its own name, not only by its section's.
        bool NamesAny(std::wstring_view name, const std::vector<std::wstring>& words)
        {
            for (const auto& word : words)
                if (HasEveryWordOrAlternative(name, { word }))
                    return true;
            return false;
        }
    }

    std::vector<Item> Search(std::wstring_view query)
    {
        const auto words = SearchWords(query);
        std::vector<Item> found;
        if (words.empty())
            return found;
        for (const auto& e : kEntries)
            if (HasEveryWordOrAlternative(TextOf(e), words))
                found.push_back(e.item);
        return found;
    }

    std::vector<Found> SearchAll(std::wstring_view query, const SettingsTree& tree)
    {
        const auto words = SearchWords(query);
        std::vector<std::pair<int, Found>> ranked;
        if (words.empty())
            return {};
        constexpr wchar_t kStep[] = L" \u203A ";
        for (const auto& e : kEntries)
            if (const int rank = Rank(NameOf(e), TextOf(e), words); rank >= 0)
                ranked.push_back({ rank, Found{ e.item, e.page, -1, -1, -1, std::wstring(PageTitle(e.page)) + kStep + e.title } });
        const std::wstring wheel = PageTitle(Page::Wheel);
        for (const auto& section : tree.sections)
        {
            const int ring = static_cast<int>(section.ring);
            const std::wstring at = wheel + kStep + section.title;
            if (const int rank = Rank(section.title, section.title + L" " + wheel + L" section", words); rank >= 0)
                ranked.push_back({ rank, Found{ Item::WheelContents, Page::Wheel, ring, -1, -1, at } });
            for (size_t g = 0; g < section.groups.size(); ++g)
            {
                const auto& group = section.groups[g];
                const std::wstring groupText = group.title + L" " + section.title + L" " + wheel;
                if (const int rank = Rank(group.title, groupText, words); rank >= 0 && NamesAny(group.title, words))
                    ranked.push_back({ rank, Found{ Item::WheelContents, Page::Wheel, ring, static_cast<int>(g), -1, at + kStep + group.title } });
                for (size_t i = 0; i < group.slices.size(); ++i)
                {
                    const auto& slice = group.slices[i];
                    const std::wstring name = slice.label + L" " + slice.detail;
                    if (const int rank = Rank(name, name + L" " + groupText, words); rank >= 0 && NamesAny(name, words))
                        ranked.push_back({ rank, Found{ Item::WheelContents, Page::Wheel, ring, static_cast<int>(g), static_cast<int>(i),
                                                        at + kStep + group.title + kStep + slice.label } });
                }
            }
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<Found> found;
        found.reserve(ranked.size());
        for (auto& [rank, f] : ranked)
            found.push_back(std::move(f));
        return found;
    }
}
