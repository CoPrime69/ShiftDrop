#pragma once
#include "DialogWindow.h"
#include "Features.h"
#include "Settings.h"

#include <windows.h>
#include <commctrl.h>

#include <array>
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

    // "Settings…" from the tray. On top, the choices: the terminal File actions › Terminal opens (or
    // a command line of your own), the theme, the accent colour (a row of round swatches, the presets
    // of Accent.h then a colour of your own from the colour dialog, and a switch for the Windows accent
    // colour), the top panel's size (a slider on top of the tray's Size, with its value beside it) and
    // how outputs are named; each applies at once
    // (onPreferences). Under them a tree of every section, kind of file and slice, each with a tick.
    // Unticking hides it from the wheel at once (onChange); a section, or a kind of file, ticks or
    // unticks everything under it. Standard controls in a DialogWindow, like the history window.
    class SettingsWindow final : public DialogWindow
    {
    public:
        using Change = std::function<void(const Features&)>;
        using PreferencesChange = std::function<void(const Preferences&)>;

        SettingsWindow(HINSTANCE instance, Change onChange, PreferencesChange onPreferences);

        // Creates the window or brings it to the front, filled from `tree`, `current` and `preferences`.
        void Show(SettingsTree tree, const Features& current, const Preferences& preferences);

    private:
        LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam) override;
        void CreateChildren() override;
        void OnDpiChanged() override;
        void OnThemeChanged() override;
        COLORREF StaticTextColor(HWND control) const override;
        void Layout() override;
        void Fill();
        void FillPreferences();
        void OnPreferenceChanged();
        void OnPanelSizeScroll(WORD code);   // the slider's WM_HSCROLL
        // The accent row: radio buttons for the presets, a button for a colour of your own and a check
        // box for the Windows colour, all drawn with the control kit (as swatches and a switch).
        void FillAccent();
        void OnAccentClicked(int controlId);
        void SetAccent(const accent::Setting& setting);
        LRESULT DrawAccentControl(const NMCUSTOMDRAW& draw);
        void OnItemChanged(HTREEITEM item, UINT oldState, UINT newState);
        void SetCheck(HTREEITEM item, int check);       // 1 off, 2 on, 3 some (TreeView state images)
        int Check(HTREEITEM item) const;
        void SetSubtree(HTREEITEM item, int check);     // the item and everything under it
        void UpdateAncestors(HTREEITEM item);           // on, off or some, from the children
        Features Collect() const;
        void EverythingOn();

        Change m_onChange;
        PreferencesChange m_onPreferences;
        SettingsTree m_tree;
        Features m_features;
        Preferences m_preferences;
        std::vector<std::string> m_sliceIds;   // a slice item's lParam - kSliceTag indexes this

        HWND m_terminalLabel = nullptr, m_terminal = nullptr;   // children are destroyed with m_hwnd
        HWND m_commandLabel = nullptr, m_command = nullptr;
        HWND m_themeLabel = nullptr, m_theme = nullptr;
        HWND m_accentLabel = nullptr, m_custom = nullptr, m_windowsAccent = nullptr;
        std::array<HWND, accent::kPresetCount> m_swatches{};
        HWND m_tooltip = nullptr;                                     // the swatches' names
        accent::Choice m_beforeWindows = accent::Choice::Tide;        // where turning the switch off goes back to
        HWND m_panelSizeLabel = nullptr, m_panelSize = nullptr, m_panelSizeValue = nullptr;   // a trackbar and its "160%"
        HWND m_namingLabel = nullptr, m_naming = nullptr;
        HWND m_intro = nullptr;
        HWND m_list = nullptr;
        HWND m_allOn = nullptr;
        HWND m_close = nullptr;
        bool m_updating = false;   // our own SetCheck calls: not the user's
    };
}
