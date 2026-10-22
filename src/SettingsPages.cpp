#include "SettingsPages.h"
#include "Brand.h"
#include "Strings.h"

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
        constexpr wchar_t kCode = 0xE943, kLock = 0xE72E, kMenu = 0xE700;

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
              L"[Output] Naming", L"Settings \u203A Output names", L"name names suffix converted folder subfolder" },
            { ShellMenu, Advanced, nullptr, L"Right-click menu entry", L"\u201C" SD_NAME L"\u2026\u201D in File Explorer\u2019s right-click menu", kMenu,
              L"HKCU\\Software\\Classes", L"tray menu", L"explorer context menu shell" },
            { RecipesFolder, Advanced, nullptr, L"Recipes folder", L"Custom actions you\u2019ve added to the wheel", kFolder, nullptr,
              L"tray menu", L"recipes json custom" },
            { LogsFolder, Advanced, nullptr, L"Logs folder", L"Useful when reporting a problem on GitHub", kList, nullptr, L"tray menu",
              L"log logs problem bug report" },
            { Version, About, nullptr, L"Version", nullptr, 0, nullptr, nullptr, L"about version" },
            { SourceCode, About, nullptr, L"Source code", L"github.com/CoPrime69/" SD_NAME, kCode, nullptr, nullptr,
              L"github source code free open" },
            { Privacy, About, nullptr, L"Offline \u00B7 No account \u00B7 No telemetry", L"Everything runs on this PC. Nothing is uploaded.",
              kLock, nullptr, nullptr, L"privacy offline network" },
        } };
    }

    const wchar_t* PageTitle(Page page)
    {
        constexpr const wchar_t* kTitles[kPageCount] = { L"General", L"Appearance", L"Wheel", L"Output", L"Advanced", L"About" };
        const auto i = static_cast<size_t>(page);
        return i < kPageCount ? kTitles[i] : L"";
    }

    wchar_t PageIcon(Page page)
    {
        constexpr wchar_t kIcons[kPageCount] = { kGear, kPalette, kWheel, kDocument, kOptions, kInfo };
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

    std::vector<Item> Search(std::wstring_view query)
    {
        const auto words = SearchWords(query);
        std::vector<Item> found;
        if (words.empty())
            return found;
        for (const auto& e : kEntries)
        {
            std::wstring text = std::wstring(e.title) + L" " + PageTitle(e.page);
            for (const wchar_t* more : { e.description, e.group, e.keywords })
                if (more)
                    text += std::wstring(L" ") + more;
            if (HasEveryWord(text, words))
                found.push_back(e.item);
        }
        return found;
    }
}
