#include "SettingsWindow.h"
#include "Brand.h"
#include "Log.h"
#include "PanelLayout.h"
#include "Theme.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace sd
{
    namespace
    {
        constexpr wchar_t kClass[] = L"" SD_NAME L".Settings";
        constexpr int kIdTree = 100;
        constexpr int kIdAllOn = 101;
        constexpr int kIdTerminal = 102, kIdCommand = 103, kIdTheme = 104, kIdNaming = 105, kIdPanelSize = 106;
        constexpr int kIdSwatch = 110;   // to 110 + accent::kPresetCount - 1, in the order of accent::Choice
        constexpr int kIdCustom = 120, kIdWindowsAccent = 121;

        // What an item's lParam holds: a tag plus the section's Ring or the slice's index.
        constexpr LPARAM kSectionTag = 0x10000, kGroupTag = 0x20000, kSliceTag = 0x30000;
        constexpr LPARAM kTagMask = 0xF0000;

        // TreeView state images with TVS_EX_PARTIALCHECKBOXES: 1 unticked, 2 ticked, 3 partly.
        constexpr int kOff = 1, kOn = 2, kSome = 3;

        // Control sizes in DIPs; scaled by the window's DPI in Layout().
        constexpr int kMargin = 14, kGap = 8, kButtonW = 150, kButtonH = 30, kIntroH = 40;
        constexpr int kLabelW = 118, kRowH = 26, kRowGap = 6;   // the choices above the tree
        constexpr int kSliderH = 30, kValueW = 48;              // the Top panel size slider and its value
        constexpr int kSwatchCell = 34;                          // a swatch and the room for its focus ring, at most
        constexpr float kSwitchLabel = 12.f;                     // the message font's size, so it reads like the labels

        constexpr wchar_t kGlyphCheck = 0xE73E, kGlyphPalette = 0xE790;   // Segoe Fluent Icons / MDL2 Assets
        constexpr wchar_t kWindowsAccentText[] = L"Use my &Windows accent colour";

        // The colour button's text, which screen readers and its tooltip say: "Custom colour #12A58E…".
        std::wstring CustomText(const accent::Setting& a)
        {
            if (a.custom == accent::kNoColour)
                return L"Custom colour\u2026";
            return std::format(L"Custom colour {}{}\u2026", accent::ToHex(a.custom), a.choice == accent::Choice::Custom ? L" (in use)" : L"");
        }

        std::wstring WithoutMnemonic(std::wstring_view text)
        {
            std::wstring out(text);
            std::erase(out, L'&');
            return out;
        }

        // Combo box items, in the order of the enums in Settings.h and OutputPath.h.
        constexpr const wchar_t* kTerminals[] = { L"Windows Terminal", L"PowerShell", L"Command Prompt", L"Custom command" };
        constexpr const wchar_t* kThemes[] = { L"Follow Windows", L"Light", L"Dark" };
        constexpr const wchar_t* kNamings[] = { L"photo (converted).png", L"photo-converted.png", L"photo_converted.png",
                                                L"photo.png in a \u201cShiftDrop\u201d folder" };

        // The slider's value as it reads beside it: "160%".
        std::wstring PercentText(int percent) { return std::format(L"{}%", percent); }

        constexpr wchar_t kIntro[] = L"Choose what the wheel offers. Untick a slice, a kind of file or a whole section to "
                                     L"hide it; changes apply right away.";
    }

    SettingsWindow::SettingsWindow(HINSTANCE instance, Change onChange, PreferencesChange onPreferences)
        : DialogWindow(instance, { .className = kClass, .width = 480, .height = 740, .minWidth = 360, .minHeight = 480 }),
          m_onChange(std::move(onChange)), m_onPreferences(std::move(onPreferences))
    {
    }

    void SettingsWindow::Show(SettingsTree tree, const Features& current, const Preferences& preferences)
    {
        m_tree = std::move(tree);
        m_features = current;
        m_preferences = preferences;
        if (!m_hwnd && !Create(L"" SD_NAME L" settings"))
            return;
        FillPreferences();
        Fill();
        Present(m_terminal);   // allowed in front: we are handling the user's tray-menu click
    }

    void SettingsWindow::CreateChildren()
    {
        // Each label is created just before its control: screen readers name a control after the
        // static text in front of it, and Tab visits the controls in this order.
        const auto label = [&](const wchar_t* text) { return Control(WC_STATICW, text, SS_LEFT | SS_CENTERIMAGE); };
        const auto combo = [&](int controlId, const auto& items) {
            HWND h = Control(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, controlId);
            for (const wchar_t* item : items)
                SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
            return h;
        };
        m_terminalLabel = label(L"&Terminal");
        m_terminal = combo(kIdTerminal, kTerminals);
        m_commandLabel = label(L"&Command line");
        m_command = Control(WC_EDITW, L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, kIdCommand);
        // TRUE: the hint stays while the box has the focus and is empty.
        SendMessageW(m_command, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"e.g. wezterm start --cwd {folder}"));
        SendMessageW(m_command, EM_LIMITTEXT, 1024, 0);
        m_themeLabel = label(L"T&heme");
        m_theme = combo(kIdTheme, kThemes);
        // The accent swatches are a radio group (arrows move between them, Tab lands on the chosen one);
        // the colour button and the switch end it.
        m_accentLabel = label(L"&Accent colour");
        for (int i = 0; i < accent::kPresetCount; ++i)
            m_swatches[static_cast<size_t>(i)] = Control(WC_BUTTONW, accent::Name(static_cast<accent::Choice>(i)),
                                                         BS_AUTORADIOBUTTON | (i == 0 ? WS_GROUP | WS_TABSTOP : 0), kIdSwatch + i);
        m_custom = Control(WC_BUTTONW, L"", WS_GROUP | WS_TABSTOP | BS_PUSHBUTTON, kIdCustom);
        m_windowsAccent = Control(WC_BUTTONW, kWindowsAccentText, WS_GROUP | WS_TABSTOP | BS_AUTOCHECKBOX, kIdWindowsAccent);
        // Each swatch's name on hover (its button text, so the same as a screen reader says).
        m_tooltip = CreateWindowExW(0, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, m_hwnd, nullptr, m_instance, nullptr);
        for (HWND swatch : m_swatches)
        {
            TTTOOLINFOW tool{ sizeof(tool) };
            tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            tool.hwnd = m_hwnd;
            tool.uId = reinterpret_cast<UINT_PTR>(swatch);
            tool.lpszText = LPSTR_TEXTCALLBACKW;
            SendMessageW(m_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        }
        TTTOOLINFOW tool{ sizeof(tool) };
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = m_hwnd;
        tool.uId = reinterpret_cast<UINT_PTR>(m_custom);
        tool.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(m_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        m_panelSizeLabel = label(L"Top &panel size");
        m_panelSize = Control(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS | TBS_BOTTOM, kIdPanelSize);
        // In percent, so screen readers read the same number as the text beside it; one step is 10%.
        SendMessageW(m_panelSize, TBM_SETRANGE, FALSE, MAKELPARAM(panel::kSizeMinPercent, panel::kSizeMaxPercent));
        SendMessageW(m_panelSize, TBM_SETTICFREQ, panel::kSizeStepPercent, 0);
        SendMessageW(m_panelSize, TBM_SETLINESIZE, 0, panel::kSizeStepPercent);
        SendMessageW(m_panelSize, TBM_SETPAGESIZE, 0, 5 * panel::kSizeStepPercent);
        m_panelSizeValue = Control(WC_STATICW, L"", SS_RIGHT | SS_CENTERIMAGE);
        m_namingLabel = Control(WC_STATICW, L"&Output names", SS_LEFT | SS_CENTERIMAGE | WS_GROUP);   // after the accent's group
        m_naming = combo(kIdNaming, kNamings);

        m_intro = Control(WC_STATICW, kIntro, SS_LEFT);
        m_list = Control(WC_TREEVIEWW, L"", WS_TABSTOP | WS_BORDER | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_FULLROWSELECT, kIdTree);
        // TVS_CHECKBOXES only works when added after the control exists (documented TreeView quirk).
        SetWindowLongPtrW(m_list, GWL_STYLE, GetWindowLongPtrW(m_list, GWL_STYLE) | TVS_CHECKBOXES);
        TreeView_SetExtendedStyle(m_list, TVS_EX_PARTIALCHECKBOXES | TVS_EX_DOUBLEBUFFER,
                                  TVS_EX_PARTIALCHECKBOXES | TVS_EX_DOUBLEBUFFER);
        const auto button = [&](const wchar_t* text, int id) { return Control(WC_BUTTONW, text, WS_TABSTOP | BS_PUSHBUTTON, id); };
        m_allOn = button(L"Turn &everything on", kIdAllOn);
        m_close = button(L"Close", IDCANCEL);   // IDCANCEL: IsDialogMessage maps Esc to it
    }

    void SettingsWindow::OnDpiChanged()
    {
        TreeView_SetItemHeight(m_list, Px(24));
    }

    void SettingsWindow::OnThemeChanged()
    {
        SetTheme({ m_list, m_allOn, m_close }, L"DarkMode_Explorer", L"Explorer");
        SetTheme({ m_terminal, m_command, m_theme, m_naming }, L"DarkMode_CFD");
        SetTheme({ m_tooltip }, L"DarkMode_Explorer");
        FillAccent();   // the swatches redraw in the new colours
        TreeView_SetBkColor(m_list, m_colors.background);
        TreeView_SetTextColor(m_list, m_colors.text);
    }

    COLORREF SettingsWindow::StaticTextColor(HWND control) const
    {
        // The command box when it is turned off is drawn as a static: dimmed text.
        return control == m_command ? GetSysColor(COLOR_GRAYTEXT) : m_colors.text;
    }

    void SettingsWindow::Layout()
    {
        RECT rc{};
        GetClientRect(m_hwnd, &rc);
        const int margin = Px(kMargin), gap = Px(kGap), bw = Px(kButtonW), bh = Px(kButtonH), ih = Px(kIntroH);
        const int buttonsY = rc.bottom - margin - bh;
        // The choices: a label column, then the control filling the rest of the row.
        const int lw = Px(kLabelW), rh = Px(kRowH), fieldX = margin + lw, fieldW = std::max(0, static_cast<int>(rc.right) - margin - fieldX);
        int y = margin;
        const auto row = [&](HWND labelHwnd, HWND control, bool isCombo) {
            MoveWindow(labelHwnd, margin, y, lw - gap, rh, TRUE);
            // A drop-down list's height includes its open list.
            MoveWindow(control, fieldX, y, fieldW, isCombo ? Px(200) : rh, TRUE);
            y += rh + Px(kRowGap);
        };
        row(m_terminalLabel, m_terminal, true);
        row(m_commandLabel, m_command, false);
        row(m_themeLabel, m_theme, true);
        // The swatches in a row, as large as fits up to kSwatchCell each; the switch under them.
        const int swatches = accent::kPresetCount + 1, cell = std::max(Px(16), std::min(Px(kSwatchCell), fieldW / swatches));
        MoveWindow(m_accentLabel, margin, y, lw - gap, cell, TRUE);
        for (int i = 0; i < swatches; ++i)
            MoveWindow(i < accent::kPresetCount ? m_swatches[static_cast<size_t>(i)] : m_custom, fieldX + i * cell, y, cell, cell, TRUE);
        y += cell;
        MoveWindow(m_windowsAccent, fieldX, y, fieldW, rh, TRUE);
        y += rh + Px(kRowGap);
        // The slider, then its value at the end of the row; a little taller for its ticks.
        const int sh = Px(kSliderH), vw = std::min(Px(kValueW), fieldW);
        MoveWindow(m_panelSizeLabel, margin, y, lw - gap, sh, TRUE);
        MoveWindow(m_panelSize, fieldX, y, std::max(0, fieldW - vw - gap), sh, TRUE);
        MoveWindow(m_panelSizeValue, fieldX + fieldW - vw, y, vw, sh, TRUE);
        y += sh + Px(kRowGap);
        row(m_namingLabel, m_naming, true);
        MoveWindow(m_intro, margin, y + gap, rc.right - 2 * margin, ih, TRUE);
        const int listTop = y + gap + ih + gap;
        MoveWindow(m_list, margin, listTop, rc.right - 2 * margin, std::max(0, buttonsY - gap - listTop), TRUE);
        MoveWindow(m_allOn, margin, buttonsY, bw, bh, TRUE);
        MoveWindow(m_close, rc.right - margin - Px(96), buttonsY, Px(96), bh, TRUE);
    }

    // ---- The choices -----------------------------------------------------------------------------

    void SettingsWindow::FillPreferences()
    {
        m_updating = true;
        SendMessageW(m_terminal, CB_SETCURSEL, static_cast<WPARAM>(m_preferences.terminal), 0);
        SendMessageW(m_theme, CB_SETCURSEL, static_cast<WPARAM>(m_preferences.theme), 0);
        SendMessageW(m_naming, CB_SETCURSEL, static_cast<WPARAM>(m_preferences.naming), 0);
        SendMessageW(m_panelSize, TBM_SETPOS, TRUE, panel::SizePercent(m_preferences.panelSize));
        SetWindowTextW(m_panelSizeValue, PercentText(panel::SizePercent(m_preferences.panelSize)).c_str());
        SetWindowTextW(m_command, m_preferences.terminalCommand.c_str());
        EnableWindow(m_command, m_preferences.terminal == TerminalChoice::Custom);
        FillAccent();
        m_updating = false;
    }

    void SettingsWindow::OnPreferenceChanged()
    {
        if (m_updating)
            return;
        Preferences p = m_preferences;
        const auto pick = [](HWND combo, size_t count, int fallback) {
            const auto i = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
            return i >= 0 && static_cast<size_t>(i) < count ? i : fallback;
        };
        p.terminal = static_cast<TerminalChoice>(pick(m_terminal, ARRAYSIZE(kTerminals), static_cast<int>(p.terminal)));
        p.theme = static_cast<ThemeChoice>(pick(m_theme, ARRAYSIZE(kThemes), static_cast<int>(p.theme)));
        p.naming = static_cast<OutputNaming>(pick(m_naming, ARRAYSIZE(kNamings), static_cast<int>(p.naming)));
        p.panelSize = panel::SizePercent(static_cast<int>(SendMessageW(m_panelSize, TBM_GETPOS, 0, 0)));
        std::wstring command(static_cast<size_t>(GetWindowTextLengthW(m_command)) + 1, L'\0');
        command.resize(static_cast<size_t>(GetWindowTextW(m_command, command.data(), static_cast<int>(command.size()))));
        p.terminalCommand = std::move(command);
        EnableWindow(m_command, p.terminal == TerminalChoice::Custom);
        if (p == m_preferences)
            return;
        const bool themeChanged = p.theme != m_preferences.theme;
        if (p.terminal != m_preferences.terminal || themeChanged || p.naming != m_preferences.naming || p.panelSize != m_preferences.panelSize)
            log::Info(L"settings: terminal {}, theme {}, output names {}, top panel size {}%", kTerminals[static_cast<int>(p.terminal)],
                      kThemes[static_cast<int>(p.theme)], kNamings[static_cast<int>(p.naming)], p.panelSize);
        m_preferences = std::move(p);
        if (m_onPreferences)
            m_onPreferences(m_preferences);
        if (themeChanged)
            ApplyTheme();   // the callback has set the new theme
    }

    void SettingsWindow::FillAccent()
    {
        if (!m_custom)
            return;   // not created yet
        const auto& a = m_preferences.accent;
        const int checked = static_cast<int>(a.choice) < accent::kPresetCount ? static_cast<int>(a.choice) : -1;
        for (int i = 0; i < accent::kPresetCount; ++i)
        {
            const HWND swatch = m_swatches[static_cast<size_t>(i)];
            Button_SetCheck(swatch, i == checked ? BST_CHECKED : BST_UNCHECKED);
            // Tab goes to the chosen swatch, or the first while none is, as in any radio group.
            const LONG_PTR style = GetWindowLongPtrW(swatch, GWL_STYLE);
            SetWindowLongPtrW(swatch, GWL_STYLE, i == std::max(checked, 0) ? style | WS_TABSTOP : style & ~static_cast<LONG_PTR>(WS_TABSTOP));
            InvalidateRect(swatch, nullptr, FALSE);
        }
        Button_SetCheck(m_windowsAccent, a.choice == accent::Choice::Windows ? BST_CHECKED : BST_UNCHECKED);
        SetWindowTextW(m_custom, CustomText(a).c_str());
        InvalidateRect(m_custom, nullptr, FALSE);
        InvalidateRect(m_windowsAccent, nullptr, FALSE);
    }

    void SettingsWindow::OnAccentClicked(int controlId)
    {
        using accent::Choice;
        auto a = m_preferences.accent;
        if (controlId >= kIdSwatch && controlId < kIdSwatch + accent::kPresetCount)
            a.choice = static_cast<Choice>(controlId - kIdSwatch);
        else if (controlId == kIdCustom)
        {
            // The colour dialog opens on the last colour of your own, or on the accent in use.
            uint32_t rgb = a.custom != accent::kNoColour ? a.custom : accent::Current().fill;
            if (!PickColour(m_hwnd, rgb))
                return;
            a.custom = rgb;
            a.choice = Choice::Custom;
        }
        else if (Button_GetCheck(m_windowsAccent) == BST_CHECKED)
        {
            if (a.choice != Choice::Windows)
                m_beforeWindows = a.choice;
            a.choice = Choice::Windows;
        }
        else
        {
            a.choice = m_beforeWindows;
        }
        SetAccent(a);
    }

    void SettingsWindow::SetAccent(const accent::Setting& a)
    {
        if (a == m_preferences.accent)
        {
            FillAccent();
            return;
        }
        log::Info(L"settings: accent colour {}{}", accent::Name(a.choice), a.choice == accent::Choice::Custom ? L" " + accent::ToHex(a.custom) : L"");
        m_preferences.accent = a;
        if (m_onPreferences)
            m_onPreferences(m_preferences);
        ApplyTheme();   // the callback has set the new accent: the switch and the swatches redraw with it
    }

    LRESULT SettingsWindow::DrawAccentControl(const NMCUSTOMDRAW& draw)
    {
        const HWND control = draw.hdr.hwndFrom;
        const auto& a = m_preferences.accent;
        return CustomDraw(draw, [&](ControlKit& kit, const D2D1_RECT_F& r, ControlKit::ControlState s) {
            const auto colour = [](uint32_t rgb) { return D2D1::ColorF(rgb, 1.f); };
            if (control == m_windowsAccent)
            {
                const float cy = std::round((r.top + r.bottom) / 2);
                kit.DrawSwitch(D2D1::RectF(r.left + 4, cy - 10, r.left + 44, cy + 10), a.choice == accent::Choice::Windows, s);
                kit.DrawText(WithoutMnemonic(kWindowsAccentText), kit.TextFormat(kSwitchLabel), D2D1::RectF(r.left + 54, r.top, r.right, r.bottom),
                             kit.Theme().text);
            }
            else if (control == m_custom)
            {
                // The colour of your own once there is one, with a palette on it; ticked while in use.
                const bool picked = a.custom != accent::kNoColour, selected = a.choice == accent::Choice::Custom;
                kit.DrawSwatch(r, picked ? colour(a.custom) : kit.Theme().control, selected ? kGlyphCheck : kGlyphPalette,
                               picked ? colour(accent::TextOn(a.custom)) : kit.Theme().text, selected, s);
            }
            else
            {
                for (int i = 0; i < accent::kPresetCount; ++i)
                    if (control == m_swatches[static_cast<size_t>(i)])
                    {
                        const uint32_t fill = accent::Preset(static_cast<accent::Choice>(i)).fill;
                        const bool selected = a.choice == static_cast<accent::Choice>(i);
                        kit.DrawSwatch(r, colour(fill), selected ? kGlyphCheck : 0, colour(accent::TextOn(fill)), selected, s);
                    }
            }
        });
    }

    void SettingsWindow::OnPanelSizeScroll(WORD code)
    {
        // The value beside it follows the thumb; the size is kept (and saved) when the thumb lands,
        // on a 10% step.
        const int percent = panel::SizePercent(static_cast<int>(SendMessageW(m_panelSize, TBM_GETPOS, 0, 0)));
        SetWindowTextW(m_panelSizeValue, PercentText(percent).c_str());
        if (code == TB_THUMBTRACK)
            return;
        if (SendMessageW(m_panelSize, TBM_GETPOS, 0, 0) != percent)
            SendMessageW(m_panelSize, TBM_SETPOS, TRUE, percent);
        OnPreferenceChanged();
    }

    // ---- The tree --------------------------------------------------------------------------------

    void SettingsWindow::Fill()
    {
        m_updating = true;
        SendMessageW(m_list, WM_SETREDRAW, FALSE, 0);
        TreeView_DeleteAllItems(m_list);
        m_sliceIds.clear();
        const auto insert = [this](HTREEITEM parent, const std::wstring& text, LPARAM param) {
            TVINSERTSTRUCTW in{};
            in.hParent = parent;
            in.hInsertAfter = TVI_LAST;
            in.item.mask = TVIF_TEXT | TVIF_PARAM;
            in.item.pszText = const_cast<wchar_t*>(text.c_str());
            in.item.lParam = param;
            return TreeView_InsertItem(m_list, &in);
        };
        for (const auto& section : m_tree.sections)
        {
            const bool sectionOn = m_features.SectionOn(section.ring);
            const HTREEITEM s = insert(TVI_ROOT, section.title, kSectionTag | static_cast<LPARAM>(section.ring));
            for (const auto& group : section.groups)
            {
                if (group.slices.empty())
                    continue;
                const HTREEITEM g = insert(s, group.title, kGroupTag);
                for (const auto& slice : group.slices)
                {
                    const std::wstring text = slice.detail.empty() || slice.detail == slice.label
                                                  ? slice.label
                                                  : std::format(L"{}   ·   {}", slice.label, slice.detail);
                    const HTREEITEM item = insert(g, text, kSliceTag | static_cast<LPARAM>(m_sliceIds.size()));
                    m_sliceIds.push_back(slice.id);
                    SetCheck(item, sectionOn && !m_features.off.contains(slice.id) ? kOn : kOff);
                }
                UpdateAncestors(TreeView_GetChild(m_list, g));
            }
            if (!TreeView_GetChild(m_list, s))
                SetCheck(s, sectionOn ? kOn : kOff);
            TreeView_Expand(m_list, s, TVE_EXPAND);
        }
        SendMessageW(m_list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(m_list, nullptr, TRUE);
        if (const HTREEITEM first = TreeView_GetRoot(m_list))
            TreeView_SelectItem(m_list, first);
        m_updating = false;
    }

    int SettingsWindow::Check(HTREEITEM item) const
    {
        const UINT state = TreeView_GetItemState(m_list, item, TVIS_STATEIMAGEMASK);
        return static_cast<int>(state >> 12);
    }

    void SettingsWindow::SetCheck(HTREEITEM item, int check)
    {
        TreeView_SetItemState(m_list, item, INDEXTOSTATEIMAGEMASK(check), TVIS_STATEIMAGEMASK);
    }

    void SettingsWindow::SetSubtree(HTREEITEM item, int check)
    {
        SetCheck(item, check);
        for (HTREEITEM c = TreeView_GetChild(m_list, item); c; c = TreeView_GetNextSibling(m_list, c))
            SetSubtree(c, check);
    }

    void SettingsWindow::UpdateAncestors(HTREEITEM item)
    {
        for (HTREEITEM parent = item ? TreeView_GetParent(m_list, item) : nullptr; parent; parent = TreeView_GetParent(m_list, parent))
        {
            bool any = false, all = true;
            for (HTREEITEM c = TreeView_GetChild(m_list, parent); c; c = TreeView_GetNextSibling(m_list, c))
            {
                const int check = Check(c);
                any |= check != kOff;
                all &= check == kOn;
            }
            SetCheck(parent, all ? kOn : any ? kSome : kOff);
        }
    }

    void SettingsWindow::OnItemChanged(HTREEITEM item, UINT oldState, UINT newState)
    {
        const int before = static_cast<int>((oldState & TVIS_STATEIMAGEMASK) >> 12);
        int after = static_cast<int>((newState & TVIS_STATEIMAGEMASK) >> 12);
        if (m_updating || before == after || before == 0)
            return;
        // A click cycles off, on, partly: "partly" is only ours to show, so from on it means off, and a
        // click on a partly ticked line ticks everything under it.
        if (before == kSome)
            after = kOn;
        else if (after == kSome)
            after = kOff;
        m_updating = true;
        SetSubtree(item, after);
        UpdateAncestors(item);
        auto features = Collect();
        if (features.SectionsOn() == 0)
        {
            // The wheel needs at least one section: put this one back.
            SetSubtree(item, kOn);
            UpdateAncestors(item);
            features = Collect();
            MessageBeep(MB_ICONWARNING);
            SetWindowTextW(m_intro, L"At least one section has to stay on, or the wheel would have nothing to show.");
        }
        else
        {
            SetWindowTextW(m_intro, kIntro);
        }
        m_updating = false;
        if (!(features == m_features))
        {
            m_features = std::move(features);
            log::Info(L"settings: {} section(s) on, {} slice(s) off", m_features.SectionsOn(), m_features.off.size());
            if (m_onChange)
                m_onChange(m_features);
        }
    }

    Features SettingsWindow::Collect() const
    {
        Features f;
        // Slices this tree does not list (a recipe from a module that is not loaded) keep their state.
        f.off = m_features.off;
        for (const auto& id : m_sliceIds)
            f.off.erase(id);
        for (HTREEITEM s = TreeView_GetRoot(m_list); s; s = TreeView_GetNextSibling(m_list, s))
        {
            TVITEMW tv{};
            tv.mask = TVIF_PARAM;
            tv.hItem = s;
            TreeView_GetItem(m_list, &tv);
            if ((tv.lParam & kTagMask) != kSectionTag)
                continue;
            const auto ring = static_cast<size_t>(tv.lParam & ~kTagMask);
            if (ring >= f.sections.size())
                continue;
            const bool on = Check(s) != kOff;
            f.sections[ring] = on;
            if (!on)
                continue;   // a whole section off: its slices come back ticked when it is ticked again
            for (HTREEITEM g = TreeView_GetChild(m_list, s); g; g = TreeView_GetNextSibling(m_list, g))
                for (HTREEITEM item = TreeView_GetChild(m_list, g); item; item = TreeView_GetNextSibling(m_list, item))
                {
                    TVITEMW si{};
                    si.mask = TVIF_PARAM;
                    si.hItem = item;
                    TreeView_GetItem(m_list, &si);
                    const auto index = static_cast<size_t>(si.lParam & ~kTagMask);
                    if ((si.lParam & kTagMask) == kSliceTag && index < m_sliceIds.size() && Check(item) == kOff)
                        f.off.insert(m_sliceIds[index]);
                }
        }
        return f;
    }

    void SettingsWindow::EverythingOn()
    {
        m_updating = true;
        for (HTREEITEM s = TreeView_GetRoot(m_list); s; s = TreeView_GetNextSibling(m_list, s))
            SetSubtree(s, kOn);
        m_updating = false;
        SetWindowTextW(m_intro, kIntro);
        Features all;
        if (!(all == m_features))
        {
            m_features = all;
            log::Info(L"settings: everything turned on");
            if (m_onChange)
                m_onChange(m_features);
        }
    }

    // ---- Window ----------------------------------------------------------------------------------

    LRESULT SettingsWindow::Handle(UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case kIdAllOn: EverythingOn(); break;
            case IDCANCEL: ShowWindow(m_hwnd, SW_HIDE); break;
            case kIdTerminal:
            case kIdTheme:
            case kIdNaming:
                if (HIWORD(wParam) == CBN_SELCHANGE)
                    OnPreferenceChanged();
                break;
            case kIdCommand:
                if (HIWORD(wParam) == EN_CHANGE)
                    OnPreferenceChanged();
                break;
            default:
                if (const int id = LOWORD(wParam);
                    HIWORD(wParam) == BN_CLICKED && ((id >= kIdSwatch && id < kIdSwatch + accent::kPresetCount) || id == kIdCustom || id == kIdWindowsAccent))
                    OnAccentClicked(id);
                break;
            }
            return 0;
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lParam) == m_panelSize)
                OnPanelSizeScroll(LOWORD(wParam));
            return 0;
        case WM_NOTIFY:
        {
            const auto* hdr = reinterpret_cast<const NMHDR*>(lParam);
            if (hdr->code == NM_CUSTOMDRAW && hdr->hwndFrom &&
                (hdr->hwndFrom == m_custom || hdr->hwndFrom == m_windowsAccent ||
                 std::find(m_swatches.begin(), m_swatches.end(), hdr->hwndFrom) != m_swatches.end()))
                return DrawAccentControl(*reinterpret_cast<const NMCUSTOMDRAW*>(lParam));
            if (hdr->code == TTN_GETDISPINFOW)
            {
                auto* tip = reinterpret_cast<NMTTDISPINFOW*>(lParam);
                GetWindowTextW(reinterpret_cast<HWND>(hdr->idFrom), tip->szText, ARRAYSIZE(tip->szText));
                return 0;
            }
            if (hdr->idFrom == kIdTree && hdr->code == TVN_ITEMCHANGEDW)
            {
                const auto* change = reinterpret_cast<const NMTVITEMCHANGE*>(lParam);
                if (change->uChanged & TVIF_STATE)
                    OnItemChanged(change->hItem, change->uStateOld, change->uStateNew);
            }
            return 0;
        }
        case WM_CLOSE:
            ShowWindow(m_hwnd, SW_HIDE);   // kept for next time; destroyed with the app
            return 0;
        default:
            break;
        }
        return DialogWindow::Handle(msg, wParam, lParam);
    }
}
