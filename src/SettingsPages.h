#pragma once
#include "WheelContents.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

// Where every setting lives in the Settings window (App Windows page: navigation on the left, a page
// of cards per area), what its card says, and where it is kept. One table, so the window, "Find a
// setting" and the tests read the same thing, and no setting can drop out of the window unnoticed.
// Pure: no Windows calls.
namespace sd::settings
{
    enum class Page { General, Appearance, Wheel, Output, Advanced, Updates, About };
    inline constexpr int kPageCount = 7;
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
        UpdateMode, CheckUpdates,                                       // Updates
        Version, SourceCode, Privacy,                                   // About
    };
    inline constexpr int kItemCount = 22;

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

    // The labels of an item's own controls, in the order the window shows them: a segmented choice's
    // options ("System", "Light", "Dark"), a drop-down's choices, a button's text. The window makes its
    // controls from these, and Find a setting knows the item by them. Empty for none.
    std::span<const wchar_t* const> Options(Item item);

    // Other words for a word typed in Find a setting ("color" for "colour", "theme" for "dark",
    // "text recognition" for "ocr"), the word itself first. Lower case.
    std::vector<std::wstring> Alternatives(const std::wstring& word);
    // Does `text` (any case) hold every word of `words` (SearchWords), or one of its Alternatives?
    bool HasEveryWordOrAlternative(std::wstring_view text, const std::vector<std::wstring>& words);

    // Find a setting: the entries with every word of `query` (or another word for it) in their title,
    // description, group, page, keywords or their controls' labels (any case), in the window's order.
    // Nothing for an empty query.
    std::vector<Item> Search(std::wstring_view query);

    // Find a setting over the whole window: the cards above, and on the Wheel page every section, kind
    // of file and action of `tree`. The best first: what is named by every word (a card's title, a
    // section, a kind of file, an action) before what only mentions them; then in the window's order.
    struct Found
    {
        Item item = Item::StartWithWindows;   // the card; WheelContents for the Wheel page's
        Page page = Page::General;
        int section = -1;                     // the Wheel page: the section's Ring, else -1
        int group = -1;                       // and the kind of file, its index in the section's groups, or -1
        int slice = -1;                       // and the action, its index in the group's slices, or -1
        std::wstring where;                   // "Wheel › Reduce size › Photos"
        bool operator==(const Found&) const = default;
    };
    std::vector<Found> SearchAll(std::wstring_view query, const SettingsTree& tree);
}
