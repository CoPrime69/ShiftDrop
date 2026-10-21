#include "SettingsWindow.h"
#include "Brand.h"
#include "Log.h"
#include "Theme.h"

#include <windowsx.h>

#include <algorithm>
#include <format>

namespace sd
{
    namespace
    {
        constexpr wchar_t kClass[] = L"" SD_NAME L".Settings";
        constexpr int kIdTree = 100;
        constexpr int kIdAllOn = 101;
        constexpr int kIdTerminal = 102, kIdCommand = 103, kIdTheme = 104, kIdNaming = 105;

        // What an item's lParam holds: a tag plus the section's Ring or the slice's index.
        constexpr LPARAM kSectionTag = 0x10000, kGroupTag = 0x20000, kSliceTag = 0x30000;
        constexpr LPARAM kTagMask = 0xF0000;

        // TreeView state images with TVS_EX_PARTIALCHECKBOXES: 1 unticked, 2 ticked, 3 partly.
        constexpr int kOff = 1, kOn = 2, kSome = 3;

        // Control sizes in DIPs; scaled by the window's DPI in Layout().
        constexpr int kMargin = 14, kGap = 8, kButtonW = 150, kButtonH = 30, kIntroH = 40;
        constexpr int kLabelW = 118, kRowH = 26, kRowGap = 6;   // the choices above the tree

        // Combo box items, in the order of the enums in Settings.h and OutputPath.h.
        constexpr const wchar_t* kTerminals[] = { L"Windows Terminal", L"PowerShell", L"Command Prompt", L"Custom command" };
        constexpr const wchar_t* kThemes[] = { L"Follow Windows", L"Light", L"Dark" };
        constexpr const wchar_t* kNamings[] = { L"photo (converted).png", L"photo-converted.png", L"photo_converted.png",
                                                L"photo.png in a \u201cShiftDrop\u201d folder" };

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
        m_namingLabel = label(L"&Output names");
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
        SetWindowTextW(m_command, m_preferences.terminalCommand.c_str());
        EnableWindow(m_command, m_preferences.terminal == TerminalChoice::Custom);
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
        std::wstring command(static_cast<size_t>(GetWindowTextLengthW(m_command)) + 1, L'\0');
        command.resize(static_cast<size_t>(GetWindowTextW(m_command, command.data(), static_cast<int>(command.size()))));
        p.terminalCommand = std::move(command);
        EnableWindow(m_command, p.terminal == TerminalChoice::Custom);
        if (p == m_preferences)
            return;
        const bool themeChanged = p.theme != m_preferences.theme;
        if (p.terminal != m_preferences.terminal || themeChanged || p.naming != m_preferences.naming)
            log::Info(L"settings: terminal {}, theme {}, output names {}", kTerminals[static_cast<int>(p.terminal)],
                      kThemes[static_cast<int>(p.theme)], kNamings[static_cast<int>(p.naming)]);
        m_preferences = std::move(p);
        if (m_onPreferences)
            m_onPreferences(m_preferences);
        if (themeChanged)
            ApplyTheme();   // the callback has set the new theme
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
            default: break;
            }
            return 0;
        case WM_NOTIFY:
        {
            const auto* hdr = reinterpret_cast<const NMHDR*>(lParam);
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
