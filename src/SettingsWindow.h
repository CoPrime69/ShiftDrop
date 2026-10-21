#pragma once
#include "Features.h"
#include "Settings.h"

#include <windows.h>
#include <commctrl.h>
#include <wil/resource.h>

#include <functional>
#include <string>
#include <vector>

namespace sd
{
    // What Settings lists: every section, the kinds of file its slices are for, and the slices.
    struct SettingsTree
    {
        struct Slice
        {
            std::string id;          // recipe id
            std::wstring label;      // "TIFF"
            std::wstring detail;     // "TIFF, lossless"
        };
        struct Group
        {
            std::wstring title;      // "Photos", "Video", "Any file"
            std::vector<Slice> slices;
        };
        struct Section
        {
            Ring ring = Ring::Convert;
            std::wstring title;
            std::vector<Group> groups;
        };
        std::vector<Section> sections;
    };

    // "Settings…" from the tray: a tree of every section, kind of file and slice, each with a tick.
    // Unticking hides it from the wheel at once (onChange); a section, or a kind of file, ticks or
    // unticks everything under it. A plain top-level window with a TreeView, laid out in device pixels
    // from the window's DPI like the history window.
    class SettingsWindow
    {
    public:
        using Change = std::function<void(const Features&)>;

        SettingsWindow(HINSTANCE instance, Change onChange);
        ~SettingsWindow();
        SettingsWindow(const SettingsWindow&) = delete;
        SettingsWindow& operator=(const SettingsWindow&) = delete;

        // Creates the window or brings it to the front, filled from `tree` and `current`.
        void Show(SettingsTree tree, const Features& current);
        bool PreTranslate(MSG& msg);   // Tab / Esc for the message loop; true when consumed

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam);
        void CreateChildren();
        void ApplyDpi(UINT dpi);
        void ApplyTheme();
        void Layout();
        void Fill();
        void OnItemChanged(HTREEITEM item, UINT oldState, UINT newState);
        void SetCheck(HTREEITEM item, int check);       // 1 off, 2 on, 3 some (TreeView state images)
        int Check(HTREEITEM item) const;
        void SetSubtree(HTREEITEM item, int check);     // the item and everything under it
        void UpdateAncestors(HTREEITEM item);           // on, off or some, from the children
        Features Collect() const;
        void EverythingOn();

        HINSTANCE m_instance;
        Change m_onChange;
        SettingsTree m_tree;
        Features m_features;
        std::vector<std::string> m_sliceIds;   // a slice item's lParam - kSliceTag indexes this

        wil::unique_hwnd m_hwnd;
        HWND m_intro = nullptr;    // children are destroyed with m_hwnd
        HWND m_list = nullptr;
        HWND m_allOn = nullptr;
        HWND m_close = nullptr;
        wil::unique_hfont m_font;
        wil::unique_hbrush m_background;
        COLORREF m_textColour = 0;
        bool m_dark = false;
        UINT m_dpi = 96;
        bool m_updating = false;   // our own SetCheck calls: not the user's
    };
}
