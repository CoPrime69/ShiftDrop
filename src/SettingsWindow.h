#pragma once
#include "DialogWindow.h"
#include "Features.h"
#include "Settings.h"
#include "SettingsPages.h"
#include "WheelContents.h"

#include <windows.h>

#include <array>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace sd
{
    // Everything the Settings window shows, as the app has it when the window opens.
    struct SettingsState
    {
        Features features;
        Preferences preferences;
        Appearance appearance;
        bool startWithWindows = true;
        bool shelfAvailable = false, shelfOn = false;
        VirtualOutput browserDrops = VirtualOutput::Desktop;
        std::vector<std::wstring> ocrLanguages;   // the text recognition languages, as Windows names them
        int ocrChoice = 0;                        // 0 automatic, else 1 + the index in ocrLanguages
        bool shellMenuAvailable = false, shellMenuOn = false;
    };

    // "Settings…" from the tray (App Windows page): navigation on the left (General, Appearance, Wheel,
    // Output, Advanced, About, and Find a setting over it) and a page of cards for each, one setting per
    // card (SettingsPages.h says which and where). Every choice applies at once: the slices through
    // `onFeatures`, the choices kept in Preferences through `onPreferences`, and what the tray menu
    // also offers (start with Windows, the shelf, size and style, where browser drops go, the text
    // recognition language, the right-click entry, the folders) through `command`, with the tray
    // menu's own command ids (resource.h), so the app does each of them in one place.
    //
    // Wheel is the old tick list: a tab per section, a card per kind of file with a chip per slice,
    // and a small wheel drawn from the real wheel's layout showing the tiles those chips make.
    // Standard controls drawn by the kit (DialogWindow), like History and Rename.
    class SettingsWindow final : public DialogWindow
    {
    public:
        using FeaturesChange = std::function<void(const Features&)>;
        using PreferencesChange = std::function<void(const Preferences&)>;
        using Command = std::function<void(UINT id)>;

        SettingsWindow(HINSTANCE instance, FeaturesChange onFeatures, PreferencesChange onPreferences, Command command);
        ~SettingsWindow() override;

        // Creates the window or brings it to the front, filled from `tree` and `state`.
        void Show(SettingsTree tree, SettingsState state);

    private:
        using Item = settings::Item;
        using Page = settings::Page;

        LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam) override;
        void CreateChildren() override;
        void OnDpiChanged() override;
        void Layout() override;
        void PaintSurface(ControlKit& kit, const D2D1_RECT_F& client) override;
        void PaintGdi(HDC dc) override;
        void DrawControl(HWND control, ControlKit& kit, const D2D1_RECT_F& r, ControlKit::ControlState s) override;

        // One card: its title and description (labels from the catalogue) and its control.
        struct Row
        {
            HWND title = nullptr, description = nullptr;
            HWND control = nullptr;   // at the end of the row, or null
            RECT card{};              // where Layout() put its card, client pixels
        };
        Row& MakeRow(Item item, HWND control);
        void Fill();                 // every control from m_state
        void BuildWheel();           // the Wheel page's cards and chips for m_tree
        void ShowPage(Page page);
        void OnCommand(int id, int code, HWND control);
        void OnFind();
        struct GroupView;
        GroupView* FoundGroup(const settings::Found& found);   // the Wheel page's card for what was found
        void PlaceFound();                                     // in Layout(): the ring and Enter's target
        void ChangePreferences(const Preferences& p);
        void SetAccent(const accent::Setting& setting);
        void FillAccent();
        void SyncCustomSwatches();               // a swatch for each colour of your own
        std::wstring SwatchLabel(HWND swatch) const;   // the name under a swatch
        void DeleteCustomColour(size_t index);
        void NoteBeforeCustom();                 // before a colour of your own is chosen: was the Windows accent in use?
        void OnPanelSizeScroll(WORD code);
        void SetFeatures(const Features& features);
        void FillWheel();            // the chips, the counts and the section's line, from m_state.features
        void FillNaming();
        void SendCommand(UINT id);

        // ---- Layout ----
        void Place(HWND control, int x, int y, int w, int h);
        int LayoutRow(Row& row, int x, int y, int width, int controlW, int controlH);
        int LayoutPage(int x, int y, int width);
        int LayoutWheel(int x, int y, int width);

        FeaturesChange m_onFeatures;
        PreferencesChange m_onPreferences;
        Command m_command;
        SettingsTree m_tree;
        SettingsState m_state;
        Page m_page = Page::General;
        bool m_updating = false;

        HICON m_icon = nullptr;   // the app's, for the header and About
        int m_iconPx = 0;
        HWND m_appName = nullptr, m_version = nullptr, m_find = nullptr, m_findHint = nullptr;
        std::array<HWND, settings::kPageCount> m_nav{};
        HWND m_heading = nullptr;
        std::map<Item, Row> m_rows;
        std::vector<settings::Found> m_results;   // what Find a setting found, the best first
        RECT m_foundRect{};                       // the ring around the best, on this page (client pixels)
        float m_foundRadius = 0;
        HWND m_foundControl = nullptr;            // where Enter in Find a setting goes

        // General
        HWND m_filesGroup = nullptr, m_startup = nullptr, m_ocr = nullptr, m_shelf = nullptr, m_terminal = nullptr, m_commandLine = nullptr;
        std::array<HWND, 3> m_drops{};
        // Appearance
        HWND m_previewLabel = nullptr;
        std::array<HWND, 3> m_themes{};
        std::array<HWND, 2> m_looks{};
        std::array<HWND, 4> m_sizes{};
        HWND m_panelSize = nullptr, m_panelValue = nullptr, m_panelLow = nullptr, m_panelHigh = nullptr;
        HWND m_accentHeader = nullptr, m_custom = nullptr, m_windowsAccent = nullptr;
        std::array<HWND, accent::kPresetCount> m_swatches{};
        std::vector<HWND> m_customSwatches;   // the colours of your own, in accent.customs' order
        bool m_windowsBeforeCustom = true;    // deleting the colour in use goes back to Windows' (else Tide)
        bool m_accentOpen = true;
        accent::Choice m_beforeWindows = accent::Choice::Tide;   // where turning the Windows switch off goes back to
        // Wheel
        HWND m_findAction = nullptr, m_summary = nullptr, m_sectionOff = nullptr, m_reset = nullptr, m_noAction = nullptr;
        std::array<HWND, kRingCount> m_tabs{};
        Ring m_section = Ring::Convert;
        struct Chip
        {
            HWND button = nullptr;
            const SettingsTree::Slice* slice = nullptr;   // in m_tree
        };
        struct GroupView
        {
            Ring ring = Ring::Convert;
            const SettingsTree::Group* group = nullptr;   // in m_tree
            HWND header = nullptr, hint = nullptr, preview = nullptr;
            std::vector<Chip> chips;
            bool open = false;
            RECT body{};                  // the open part of its card, client pixels
            RECT card{};                  // its whole card
            D2D1_POINT_2F wheel{};        // its preview's centre, DIPs
            float radius = 0;
        };
        std::vector<GroupView> m_groups;
        // Output
        std::array<HWND, 4> m_namings{};
        HWND m_example = nullptr, m_exampleNote = nullptr;
        RECT m_exampleBox{};
        // Advanced and About
        HWND m_shellMenu = nullptr, m_recipes = nullptr, m_logs = nullptr, m_source = nullptr;
        HWND m_aboutName = nullptr, m_aboutVersion = nullptr, m_aboutFree = nullptr;
        RECT m_aboutCard{}, m_freePill{};

        // What Layout() placed this time: the rest of m_all is hidden.
        std::vector<HWND> m_all;
        std::vector<HWND> m_placed;
        struct Card
        {
            RECT rect{};
            wchar_t icon = 0;
            int iconY = 0;   // the glyph's centre line, client pixels (0: the card's middle)
        };
        std::vector<Card> m_cards;
        std::vector<RECT> m_lines;   // dividers inside cards, and under the tabs
        RECT m_preview{};            // Appearance's preview
        RECT m_header{};             // the app's icon at the top of the navigation
        int m_navWidth = 0;
    };
}
