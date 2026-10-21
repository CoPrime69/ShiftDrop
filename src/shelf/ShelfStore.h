#pragma once
#include "shelf/ShelfGeometry.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What the Float shelf keeps between runs, and the list operations behind its buttons. No Win32
// UI here, so it is unit-tested. Saved as %LOCALAPPDATA%\ShiftDrop\shelf.json:
//   { "version": 1, "items": [ "C:\\a.png", ... ], "edge": "right", "x": 1500, "y": 300 }
namespace sd::shelf
{
    struct SavedShelf
    {
        std::vector<std::wstring> items;   // newest first
        Edge edge = Edge::Right;
        std::optional<int> x, y;           // the panel's top-left corner in screen pixels; none = first run
    };

    std::string ToJson(const SavedShelf& shelf);
    // False (and `shelf` untouched) when the text is not a shelf file. Unknown keys are ignored and
    // entries that are not strings skipped, so a newer or hand-edited file still loads.
    bool FromJson(std::string_view json, SavedShelf& shelf);

    // Whole-file save through a temporary file and a rename, so a crash never leaves half a file.
    bool Save(const std::wstring& path, const SavedShelf& shelf);
    SavedShelf Load(const std::wstring& path);   // a missing or unreadable file is an empty shelf

    // Paths compare like the file system does: case-insensitive, either slash.
    bool SamePath(std::wstring_view a, std::wstring_view b);

    // Puts `added` at the top in the order given; a path already on the shelf moves up instead of
    // appearing twice. Returns how many were new. Never holds more than `limit` (the oldest go).
    size_t AddToTop(std::vector<std::wstring>& items, const std::vector<std::wstring>& added, size_t limit = 500);
    // Removes the items `gone` says are gone; returns how many went.
    size_t RemoveIf(std::vector<std::wstring>& items, const std::function<bool(const std::wstring&)>& gone);

    // Which items are selected, one flag per item, with Explorer's click rules.
    class Selection
    {
    public:
        void Resize(size_t count);
        // A plain click selects only `index`; Ctrl toggles it; Shift selects the range from the
        // last plain or Ctrl click (Ctrl+Shift adds the range).
        void Click(size_t index, bool ctrl, bool shift);
        void SelectAll();
        void Clear();
        bool IsSelected(size_t index) const { return index < m_flags.size() && m_flags[index]; }
        size_t Count() const;
        std::vector<size_t> Indices() const;
        size_t Anchor() const { return m_anchor; }
        // Keeps the flags lined up after item `index` is removed.
        void Erase(size_t index);

        // What a drag that starts on `pressed` carries: the selection if `pressed` is part of it,
        // otherwise just `pressed`.
        std::vector<size_t> DragSet(size_t pressed) const;
        // What the "drag" handle at the bottom carries: the selection, or everything when none.
        std::vector<size_t> HandleSet() const;

    private:
        std::vector<bool> m_flags;
        size_t m_anchor = 0;
    };

    // "1 item", "3 items" (folders can be on the shelf too).
    std::wstring CountText(size_t count);
}
