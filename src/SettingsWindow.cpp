#include "SettingsWindow.h"
#include "Brand.h"
#include "Log.h"
#include "Strings.h"
#include "Theme.h"
#include "resource.h"

#include <dwmapi.h>
#include <uxtheme.h>
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

        // Window and control sizes in DIPs; scaled by the window's DPI in Layout().
        constexpr int kWidth = 480, kHeight = 740, kMinWidth = 360, kMinHeight = 480;
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
        : m_instance(instance), m_onChange(std::move(onChange)), m_onPreferences(std::move(onPreferences))
    {
    }

    SettingsWindow::~SettingsWindow() = default;

    void SettingsWindow::Show(SettingsTree tree, const Features& current, const Preferences& preferences)
    {
        m_tree = std::move(tree);
        m_features = current;
        m_preferences = preferences;
        if (!m_hwnd)
        {
            static const bool registered = [this] {
                WNDCLASSEXW wc{ sizeof(wc) };
                wc.lpfnWndProc = WndProc;
                wc.hInstance = m_instance;
                wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                wc.hIcon = LoadIconW(m_instance, MAKEINTRESOURCEW(IDI_APPICON));
                wc.lpszClassName = kClass;
                return RegisterClassExW(&wc) != 0;
            }();
            if (!registered)
                return;
            // WS_EX_CONTROLPARENT lets IsDialogMessage move focus with Tab between our child controls.
            m_hwnd.reset(CreateWindowExW(WS_EX_CONTROLPARENT, kClass, L"" SD_NAME L" settings", WS_OVERLAPPEDWINDOW,
                                         CW_USEDEFAULT, CW_USEDEFAULT, kWidth, kHeight, nullptr, nullptr, m_instance, this));
            if (!m_hwnd)
                return;
            CreateChildren();
            const UINT dpi = GetDpiForWindow(m_hwnd.get());
            RECT r{ 0, 0, MulDiv(kWidth, static_cast<int>(dpi), 96), MulDiv(kHeight, static_cast<int>(dpi), 96) };
            AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_CONTROLPARENT, dpi);
            SetWindowPos(m_hwnd.get(), nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            ApplyDpi(dpi);
        }
        ApplyTheme();
        FillPreferences();
        Fill();
        ShowWindow(m_hwnd.get(), IsIconic(m_hwnd.get()) ? SW_RESTORE : SW_SHOWNORMAL);
        SetForegroundWindow(m_hwnd.get());   // allowed: we are handling the user's tray-menu click
        SetFocus(m_terminal);
    }

    void SettingsWindow::CreateChildren()
    {
        const HWND parent = m_hwnd.get();
        const auto id = [](int value) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(value)); };
        // Each label is created just before its control: screen readers name a control after the
        // static text in front of it, and Tab visits the controls in this order.
        const auto label = [&](const wchar_t* text) {
            return CreateWindowExW(0, WC_STATICW, text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE, 0, 0, 0, 0, parent, nullptr, m_instance, nullptr);
        };
        const auto combo = [&](int controlId, const auto& items) {
            HWND h = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0, 0, 0, 0,
                                     parent, id(controlId), m_instance, nullptr);
            for (const wchar_t* item : items)
                SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
            return h;
        };
        m_terminalLabel = label(L"&Terminal");
        m_terminal = combo(kIdTerminal, kTerminals);
        m_commandLabel = label(L"&Command line");
        m_command = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 0, 0, parent,
                                    id(kIdCommand), m_instance, nullptr);
        // TRUE: the hint stays while the box has the focus and is empty.
        SendMessageW(m_command, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"e.g. wezterm start --cwd {folder}"));
        SendMessageW(m_command, EM_LIMITTEXT, 1024, 0);
        m_themeLabel = label(L"T&heme");
        m_theme = combo(kIdTheme, kThemes);
        m_namingLabel = label(L"&Output names");
        m_naming = combo(kIdNaming, kNamings);

        m_intro = CreateWindowExW(0, WC_STATICW, kIntro, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, parent, nullptr, m_instance, nullptr);
        m_list = CreateWindowExW(0, WC_TREEVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS |
                                     TVS_FULLROWSELECT,
                                 0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdTree)), m_instance, nullptr);
        // TVS_CHECKBOXES only works when added after the control exists (documented TreeView quirk).
        SetWindowLongPtrW(m_list, GWL_STYLE, GetWindowLongPtrW(m_list, GWL_STYLE) | TVS_CHECKBOXES);
        TreeView_SetExtendedStyle(m_list, TVS_EX_PARTIALCHECKBOXES | TVS_EX_DOUBLEBUFFER,
                                  TVS_EX_PARTIALCHECKBOXES | TVS_EX_DOUBLEBUFFER);
        auto button = [&](const wchar_t* text, int id) {
            return CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, parent,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), m_instance, nullptr);
        };
        m_allOn = button(L"Turn &everything on", kIdAllOn);
        m_close = button(L"Close", IDCANCEL);   // IDCANCEL: IsDialogMessage maps Esc to it
    }

    void SettingsWindow::ApplyDpi(UINT dpi)
    {
        m_dpi = dpi;
        NONCLIENTMETRICSW metrics{ sizeof(metrics) };
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi))
            m_font.reset(CreateFontIndirectW(&metrics.lfMessageFont));
        for (HWND child : { m_terminalLabel, m_terminal, m_commandLabel, m_command, m_themeLabel, m_theme, m_namingLabel, m_naming, m_intro,
                            m_list, m_allOn, m_close })
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(m_font.get()), TRUE);
        TreeView_SetItemHeight(m_list, MulDiv(24, static_cast<int>(dpi), 96));
        Layout();
    }

    void SettingsWindow::ApplyTheme()
    {
        // Follows Windows' app mode (light or dark), read each time the window is shown.
        m_dark = theme::Current(Style::Solid).dark;
        const BOOL dark = m_dark;
        DwmSetWindowAttribute(m_hwnd.get(), 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        const COLORREF back = m_dark ? RGB(32, 32, 32) : GetSysColor(COLOR_WINDOW);
        m_textColour = m_dark ? RGB(240, 240, 240) : GetSysColor(COLOR_WINDOWTEXT);
        m_fieldColour = RGB(45, 45, 45);
        m_background.reset(CreateSolidBrush(back));
        m_field.reset(CreateSolidBrush(m_fieldColour));
        // "DarkMode_Explorer" is the theme Explorer's own tree and buttons use in dark mode; an empty
        // name goes back to the normal one. Combo and edit boxes have theirs in "DarkMode_CFD" (the
        // common file dialog's).
        const wchar_t* themeName = m_dark ? L"DarkMode_Explorer" : L"Explorer";
        for (HWND child : { m_list, m_allOn, m_close })
            SetWindowTheme(child, themeName, nullptr);
        for (HWND child : { m_terminal, m_command, m_theme, m_naming })
            SetWindowTheme(child, m_dark ? L"DarkMode_CFD" : nullptr, nullptr);
        TreeView_SetBkColor(m_list, back);
        TreeView_SetTextColor(m_list, m_textColour);
        InvalidateRect(m_hwnd.get(), nullptr, TRUE);
    }

    void SettingsWindow::Layout()
    {
        RECT rc{};
        GetClientRect(m_hwnd.get(), &rc);
        auto px = [this](int dip) { return MulDiv(dip, static_cast<int>(m_dpi), 96); };
        const int margin = px(kMargin), gap = px(kGap), bw = px(kButtonW), bh = px(kButtonH), ih = px(kIntroH);
        const int buttonsY = rc.bottom - margin - bh;
        // The choices: a label column, then the control filling the rest of the row.
        const int lw = px(kLabelW), rh = px(kRowH), fieldX = margin + lw, fieldW = std::max(0, static_cast<int>(rc.right) - margin - fieldX);
        int y = margin;
        const auto row = [&](HWND labelHwnd, HWND control, bool isCombo) {
            MoveWindow(labelHwnd, margin, y, lw - gap, rh, TRUE);
            // A drop-down list's height includes its open list.
            MoveWindow(control, fieldX, y, fieldW, isCombo ? px(200) : rh, TRUE);
            y += rh + px(kRowGap);
        };
        row(m_terminalLabel, m_terminal, true);
        row(m_commandLabel, m_command, false);
        row(m_themeLabel, m_theme, true);
        row(m_namingLabel, m_naming, true);
        MoveWindow(m_intro, margin, y + gap, rc.right - 2 * margin, ih, TRUE);
        const int listTop = y + gap + ih + gap;
        MoveWindow(m_list, margin, listTop, rc.right - 2 * margin, std::max(0, buttonsY - gap - listTop), TRUE);
        MoveWindow(m_allOn, margin, buttonsY, bw, bh, TRUE);
        MoveWindow(m_close, rc.right - margin - px(96), buttonsY, px(96), bh, TRUE);
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

    bool SettingsWindow::PreTranslate(MSG& msg)
    {
        if (!m_hwnd || (msg.hwnd != m_hwnd.get() && !IsChild(m_hwnd.get(), msg.hwnd)))
            return false;
        return IsDialogMessageW(m_hwnd.get(), &msg) != FALSE;
    }

    LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_NCCREATE)
        {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self || !self->m_hwnd || self->m_hwnd.get() != hwnd)
            return DefWindowProcW(hwnd, msg, wParam, lParam);   // messages before CreateWindowExW returns
        return self->Handle(msg, wParam, lParam);
    }

    LRESULT SettingsWindow::Handle(UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_SIZE:
            Layout();
            return 0;
        case WM_GETMINMAXINFO:
        {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize = { MulDiv(kMinWidth, static_cast<int>(m_dpi), 96), MulDiv(kMinHeight, static_cast<int>(m_dpi), 96) };
            return 0;
        }
        case WM_DPICHANGED:
        {
            const auto* r = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(m_hwnd.get(), nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            ApplyDpi(HIWORD(wParam));
            return 0;
        }
        case WM_SETTINGCHANGE:
            // "ImmersiveColorSet": the user switched between light and dark.
            if (lParam && EqualsNoCase(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet"))
                ApplyTheme();
            break;
        case WM_ERASEBKGND:
        {
            RECT rc{};
            GetClientRect(m_hwnd.get(), &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, m_background.get());
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            // The command box when it is turned off is drawn as a static: dimmed text.
            SetTextColor(dc, reinterpret_cast<HWND>(lParam) == m_command ? GetSysColor(COLOR_GRAYTEXT) : m_textColour);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(m_background.get());
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            if (m_dark)
            {
                const HDC dc = reinterpret_cast<HDC>(wParam);
                SetTextColor(dc, m_textColour);
                SetBkColor(dc, m_fieldColour);
                return reinterpret_cast<LRESULT>(m_field.get());
            }
            break;
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case kIdAllOn: EverythingOn(); break;
            case IDCANCEL: ShowWindow(m_hwnd.get(), SW_HIDE); break;
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
            ShowWindow(m_hwnd.get(), SW_HIDE);   // kept for next time; destroyed with the app
            return 0;
        default:
            break;
        }
        return DefWindowProcW(m_hwnd.get(), msg, wParam, lParam);
    }
}
