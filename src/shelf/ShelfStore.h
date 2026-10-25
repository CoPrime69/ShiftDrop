#pragma once
#include "shelf/ShelfGeometry.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What the Float shelf keeps between runs, and the list operations behind its buttons. No Win32
// UI here, so it is unit-tested. Saved as %LOCALAPPDATA%\ShiftDrop\shelf.json:
//   { "version": 1, "items": [ "C:\\a.png", ... ], "edge": "right", "x": 1500, "y": 300, "shown": true,
//     "pinned": [ "C:\\b.pdf", ... ], "hidden": [ "C:\\c (converted).png", ... ],
//     "place": { "edge": "right", "device": "\\\\.\\DISPLAY1", "work": [ 0, 0, 1920, 1032 ], "x": 1.0, "y": 0.2 } }
// "place" is where the user left the shelf (see shelf::Place); "edge", "x" and "y" are the pixels of
// files written before it, still read when "place" is missing.
namespace sd::shelf
{
    struct SavedShelf
    {
        std::vector<std::wstring> items;   // newest first
        Edge edge = Edge::Right;
        std::optional<int> x, y;           // the panel's top-left corner in screen pixels; none = first run
        bool shown = true;                 // the tray's "Show Float shelf" (a file without it: on)
        std::vector<std::wstring> pinned;  // the drop-down's Pinned list, newest first
        std::vector<std::wstring> hidden;  // recent conversions taken off the drop-down's list (× on them)
        std::optional<Place> place;        // where the user left it; none = first run (or an older file)
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
    // Pinned items (the header's drop-down): pinning puts a path at the top of `pinned` (once);
    // unpinning takes it off. Returns whether it is pinned now. Never more than `limit`.
    bool IsPinned(const std::vector<std::wstring>& pinned, std::wstring_view path);
    bool SetPinned(std::vector<std::wstring>& pinned, const std::wstring& path, bool pin, size_t limit = 100);
    // The drop-down's Recent conversions from History's outputs (HistoryStore::RecentOutputs, newest
    // first): each path once, none of `hidden` (taken off the list with ×; the file and its History
    // entry stay), at most `limit`. A recent output pinned as well shows in both lists.
    std::vector<std::wstring> RecentList(const std::vector<std::wstring>& outputs, const std::vector<std::wstring>& hidden, size_t limit);
    // × on a recent conversion: remembered, newest first, the oldest forgotten past `limit`.
    void HideRecent(std::vector<std::wstring>& hidden, const std::wstring& path, size_t limit = 200);
    inline constexpr size_t kRecentCount = 10;   // the drop-down lists this many recent conversions

    // Removes the items `gone` says are gone; returns how many went.
    size_t RemoveIf(std::vector<std::wstring>& items, const std::function<bool(const std::wstring&)>& gone);

    // When the shelf is on screen. It stays there while it is switched on, with files or empty:
    // removing, clearing or dragging out its last file never hides it, and an empty shelf is a place
    // to drop files. Only switching it off hides it (the tray's "Show Float shelf", the shelf's own
    // close button), and the window saves that, so it lasts across restarts. Files put on it switch it
    // back on: they are there to be seen.
    class Visibility
    {
    public:
        explicit Visibility(bool on = true) : m_on(on) {}
        // On screen (and ticked in the tray), however many files it holds.
        bool On() const { return m_on; }
        // The tray item and the close button. Returns whether it changed (then it is saved).
        bool Switch(bool on);
        // Files put on the shelf, from wherever they come: the Float slice, a result's Float button or
        // a drop on the shelf itself. All of them go through AddToTop, so a file already there moves up
        // instead of appearing twice. Returns how many were new.
        size_t Put(std::vector<std::wstring>& items, const std::vector<std::wstring>& files, size_t limit = 500);

    private:
        bool m_on;
    };

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
    // The header's count: "3 files", "1 folder", "4 items" when both are there; nothing when empty
    // (the empty shelf's drop box says that).
    std::wstring CountText(size_t files, size_t folders);
}
