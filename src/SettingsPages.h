#pragma once
#include <span>
#include <string_view>
#include <vector>

// Where every setting lives in the Settings window (App Windows page: navigation on the left, a page
// of cards per area), what its card says, and where it is kept. One table, so the window, "Find a
// setting" and the tests read the same thing, and no setting can drop out of the window unnoticed.
// Pure: no Windows calls.
namespace sd::settings
{
    enum class Page { General, Appearance, Wheel, Output, Advanced, About };
    inline constexpr int kPageCount = 6;
    const wchar_t* PageTitle(Page page);   // "General"...
    wchar_t PageIcon(Page page);           // a Segoe Fluent Icons / MDL2 Assets glyph

    // Every setting and every row of the window, in the order the pages show them.
    enum class Item
    {
        StartWithWindows, OcrLanguage, FloatShelf,                      // General
        BrowserDrops, Terminal, TerminalCommand,                        // General › Files
        Theme, Style, Size, PanelSize, Accent, WindowsAccent,           // Appearance
        WheelContents,                                                  // Wheel
        OutputNaming,                                                   // Output
        ShellMenu, RecipesFolder, LogsFolder,                           // Advanced
        Version, SourceCode, Privacy,                                   // About
    };
    inline constexpr int kItemCount = 20;

    struct Entry
    {
        Item item;
        Page page;
        const wchar_t* group;         // the heading over its card ("Files"), or nullptr
        const wchar_t* title;         // the card's title, which is also its control's name for screen readers
        const wchar_t* description;   // the line under the title, or nullptr
        wchar_t icon;                 // the card's glyph, 0 for none (a row inside another card)
        const wchar_t* stored;        // where it is kept: "[Appearance] Theme", "Run key"; nullptr for none
        const wchar_t* before;        // where it was before this window had pages: "Settings › Theme", "tray menu"
        const wchar_t* keywords;      // more words "Find a setting" knows it by
    };
    std::span<const Entry> Catalogue();
    const Entry& Find(Item item);
    std::vector<Item> OnPage(Page page);

    // Find a setting: the entries with every word of `query` in their title, description, group,
    // page or keywords (any case), in the window's order. Nothing for an empty query.
    std::vector<Item> Search(std::wstring_view query);
}
