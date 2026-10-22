// The accessibility GUIDs (CLSID_AccPropServices, PROPID_ACC_*) are defined here: no import library has them.
#include <windows.h>
#include <initguid.h>
#include <oleacc.h>

#include "DialogWindow.h"
#include "Strings.h"
#include "resource.h"

#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace sd
{
    namespace
    {
        constexpr wchar_t kGlyphSearch = 0xE721, kGlyphError = 0xE783;   // Search, ErrorBadge

        // Owner-drawn combo boxes: the closed box (item -1) and the rows of the open list, DIPs.
        constexpr int kDropDownField = 26, kDropDownRow = 30;

        std::wstring WithoutMnemonic(std::wstring text)
        {
            std::erase(text, L'&');
            return text;
        }

        D2D1_COLOR_F FromColorRef(COLORREF c)
        {
            return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f);
        }

        // Every focus cue shows once the keyboard was used in the window (WM_CHANGEUISTATE).
        bool FocusCues(HWND control)
        {
            return !(SendMessageW(control, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
        }
    }

    struct DialogWindow::KitTarget
    {
        Microsoft::WRL::ComPtr<ID2D1Factory> d2d;
        Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> target;   // bound to each control's DC in turn
        UINT dpi = 0;                                          // the target's
        ControlKit kit;
    };

    DialogWindow::DialogWindow(HINSTANCE instance, const Frame& frame) : m_instance(instance), m_frame(frame) {}

    DialogWindow::~DialogWindow()
    {
        if (m_hwnd)
        {
            // The derived window is gone already: its messages from here on go to DefWindowProc.
            SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
            DestroyWindow(m_hwnd);
        }
    }

    bool DialogWindow::Create(const wchar_t* title)
    {
        return Create(title, nullptr);
    }

    bool DialogWindow::Create(const wchar_t* title, HWND owner)
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        if (!GetClassInfoExW(m_instance, m_frame.className, &wc))
        {
            wc = WNDCLASSEXW{ sizeof(wc) };
            wc.lpfnWndProc = WndProc;
            wc.hInstance = m_instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hIcon = LoadIconW(m_instance, MAKEINTRESOURCEW(IDI_APPICON));
            wc.lpszClassName = m_frame.className;   // no background brush: WM_PAINT paints m_colors.background
            if (!RegisterClassExW(&wc))
                return false;
        }
        // WS_EX_CONTROLPARENT lets IsDialogMessage move focus with Tab between our child controls;
        // WS_CLIPCHILDREN keeps the window's painting (cards) off its controls.
        // A question has a dialog's frame, and a taskbar button of its own when nothing owns it.
        const DWORD style = (m_frame.fixed ? WS_POPUP | WS_CAPTION | WS_SYSMENU : WS_OVERLAPPEDWINDOW) | WS_CLIPCHILDREN | (m_frame.scrolls ? WS_VSCROLL : 0);
        const DWORD exStyle = WS_EX_CONTROLPARENT | (m_frame.fixed ? WS_EX_DLGMODALFRAME | (owner ? 0 : WS_EX_APPWINDOW) : 0);
        m_owner = owner;
        m_style = style;
        m_exStyle = exStyle;
        m_hwnd = CreateWindowExW(exStyle, m_frame.className, title, style, CW_USEDEFAULT, CW_USEDEFAULT, m_frame.width, m_frame.height, owner,
                                 nullptr, m_instance, this);
        if (!m_hwnd)
            return false;
        CreateChildren();
        // Size the client area for this monitor's DPI; CreateWindow took the size in physical pixels.
        const UINT dpi = GetDpiForWindow(m_hwnd);
        m_dpi = dpi;
        SetClientSize(m_frame.width, m_frame.height);
        ApplyDpi(dpi);
        ApplyTheme();
        return true;
    }

    void DialogWindow::SetClientSize(int width, int height)
    {
        RECT r{ 0, 0, Px(width), Px(height) };
        AdjustWindowRectExForDpi(&r, m_style, FALSE, m_exStyle, m_dpi);
        SetWindowPos(m_hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void DialogWindow::Present(HWND focus)
    {
        ApplyTheme();   // the theme may have changed while it was hidden
        ShowWindow(m_hwnd, IsIconic(m_hwnd) ? SW_RESTORE : SW_SHOWNORMAL);
        SetForegroundWindow(m_hwnd);
        SetFocus(focus);
    }

    HWND DialogWindow::Control(const wchar_t* windowClass, const wchar_t* text, DWORD style, int id, DWORD exStyle) const
    {
        return CreateWindowExW(exStyle, windowClass, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, m_hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), m_instance, nullptr);
    }

    void DialogWindow::ApplyDpi(UINT dpi)
    {
        m_dpi = dpi;
        // The message font ("Segoe UI 9pt") as the user configured it, at this window's DPI; or the
        // kit's font, so typed text matches the text the kit draws around it.
        NONCLIENTMETRICSW metrics{ sizeof(metrics) };
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi))
        {
            LOGFONTW font = metrics.lfMessageFont;
            if (m_frame.kitFont)
            {
                font.lfHeight = -MulDiv(13, static_cast<int>(dpi), 96);
                font.lfWeight = FW_NORMAL;
                wcsncpy_s(font.lfFaceName, theme::TextFamily(Kit().DWrite()).c_str(), _TRUNCATE);
            }
            m_font.reset(CreateFontIndirectW(&font));
        }
        for (HWND child = GetWindow(m_hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(m_font.get()), TRUE);
        for (const auto& [combo, on] : m_dropDowns)
        {
            SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), Px(kDropDownField));
            SendMessageW(combo, CB_SETITEMHEIGHT, 0, Px(kDropDownRow));
        }
        // A list's rows are as tall as its (empty) small image list says.
        for (const auto& [list, info] : m_lists)
            if (const HIMAGELIST old = ListView_SetImageList(list, ImageList_Create(1, Px(info.rowDip), ILC_COLOR32, 1, 0), LVSIL_SMALL))
                ImageList_Destroy(old);
        OnDpiChanged();
        Layout();
        for (const auto& [edit, field] : m_fields)
            PlaceField(edit, field.box);   // the edit's height follows the new font
    }

    void DialogWindow::ApplyTheme()
    {
        m_colors = theme::Dialog(m_frame.lightBackground);
        theme::ApplyDarkCaption(m_hwnd, m_colors.dark);
        m_background.reset(CreateSolidBrush(m_colors.background));
        m_field.reset(CreateSolidBrush(m_colors.field));
        Kit().SetPalette(theme::EditorColors());
        if (m_frame.scrolls)
            SetWindowTheme(m_hwnd, m_colors.dark ? L"DarkMode_Explorer" : nullptr, nullptr);   // the scroll bar
        for (const auto& [list, info] : m_lists)
        {
            // "Explorer" in light mode too: its hot tracking tells the kit which row is under the mouse.
            SetTheme({ list }, L"DarkMode_Explorer", L"Explorer");
            SetTheme({ ListView_GetHeader(list) }, L"DarkMode_ItemsView");
            const COLORREF behind = theme::ToColorRef(SurfaceColor(info.on));
            ListView_SetBkColor(list, behind);
            ListView_SetTextBkColor(list, behind);
            ListView_SetTextColor(list, m_colors.text);   // the group headers'
        }
        OnThemeChanged();
        RedrawWindow(m_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
    }

    void DialogWindow::SetTheme(std::initializer_list<HWND> controls, const wchar_t* dark, const wchar_t* light) const
    {
        for (HWND control : controls)
            SetWindowTheme(control, m_colors.dark ? dark : light, nullptr);
    }

    void DialogWindow::ThemeList(HWND list, const wchar_t* light) const
    {
        SetTheme({ list }, L"DarkMode_Explorer", light);
        SetTheme({ ListView_GetHeader(list) }, L"DarkMode_ItemsView");
        ListView_SetBkColor(list, m_colors.field);
        ListView_SetTextBkColor(list, m_colors.field);
        ListView_SetTextColor(list, m_colors.text);
    }

    std::wstring DialogWindow::Text(HWND control)
    {
        std::wstring text(static_cast<size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
        text.resize(static_cast<size_t>(GetWindowTextW(control, text.data(), static_cast<int>(text.size()))));
        return text;
    }

    // ---- Drawing with the kit -------------------------------------------------------------------------

    ControlKit& DialogWindow::Kit()
    {
        if (!m_kit)
        {
            m_kit = std::make_unique<KitTarget>();
            D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_kit->d2d.GetAddressOf());
            m_kit->kit.SetPalette(theme::EditorColors());
        }
        return m_kit->kit;
    }

    D2D1_COLOR_F DialogWindow::SurfaceColor(Surface on) const
    {
        return on == Surface::Card && m_kit ? m_kit->kit.Theme().panel : FromColorRef(m_colors.background);
    }

    bool DialogWindow::Paint(HDC dc, const RECT& rc, const D2D1_COLOR_F& behind, const std::function<void(ControlKit&, const D2D1_RECT_F&)>& paint)
    {
        Kit();
        auto& k = *m_kit;
        if (k.target && k.dpi != m_dpi)
        {
            k.kit.SetTarget(nullptr);
            k.target.Reset();
        }
        if (!k.target)
        {
            // In DIPs at the window's DPI; opaque, so text gets ClearType like the standard controls'.
            const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                                                            static_cast<float>(m_dpi), static_cast<float>(m_dpi));
            if (!k.d2d || FAILED(k.d2d->CreateDCRenderTarget(&props, &k.target)))
                return false;
            k.dpi = m_dpi;
            k.kit.SetTarget(k.target.Get());
        }
        if (rc.right <= rc.left || rc.bottom <= rc.top || FAILED(k.target->BindDC(dc, &rc)))
            return false;
        const float scale = static_cast<float>(m_dpi) / 96.f;
        k.target->BeginDraw();
        k.target->SetTransform(D2D1::Matrix3x2F::Identity());
        k.target->Clear(D2D1::ColorF(behind.r, behind.g, behind.b));
        paint(k.kit, D2D1::RectF(0, 0, static_cast<float>(rc.right - rc.left) / scale, static_cast<float>(rc.bottom - rc.top) / scale));
        if (k.target->EndDraw() == static_cast<HRESULT>(D2DERR_RECREATE_TARGET))
        {
            k.kit.SetTarget(nullptr);
            k.target.Reset();
            return false;
        }
        return true;
    }

    LRESULT DialogWindow::CustomDraw(const NMCUSTOMDRAW& draw, const KitPaint& paint)
    {
        if (draw.dwDrawStage != CDDS_PREPAINT)
            return CDRF_DODEFAULT;
        const UINT state = draw.uItemState;
        const ControlKit::ControlState s{ !(state & CDIS_DISABLED), (state & CDIS_HOT) != 0, (state & CDIS_SELECTED) != 0,
                                          (state & CDIS_FOCUS) && FocusCues(draw.hdr.hwndFrom) };
        if (!Paint(draw.hdc, draw.rc, FromColorRef(m_colors.background), [&](ControlKit& kit, const D2D1_RECT_F& r) { paint(kit, r, s); }))
            InvalidateRect(draw.hdr.hwndFrom, nullptr, FALSE);
        return CDRF_SKIPDEFAULT;
    }

    LRESULT DialogWindow::DrawStyled(const NMCUSTOMDRAW& draw, const Style& style)
    {
        if (draw.dwDrawStage != CDDS_PREPAINT)
            return CDRF_DODEFAULT;
        const HWND control = draw.hdr.hwndFrom;
        const UINT state = draw.uItemState;
        const ControlKit::ControlState s{ !(state & CDIS_DISABLED), (state & CDIS_HOT) != 0, (state & CDIS_SELECTED) != 0,
                                          (state & CDIS_FOCUS) && FocusCues(control) };
        const bool checked = Checked(control);
        const std::wstring text = WithoutMnemonic(Text(control));
        const bool painted = Paint(draw.hdc, draw.rc, SurfaceColor(style.on), [&](ControlKit& kit, const D2D1_RECT_F& r) {
            switch (style.look)
            {
            case Look::Button: kit.DrawButton(r, text, style.icon, false, s); break;
            case Look::Primary: kit.DrawButton(r, text, style.icon, true, s); break;
            case Look::Icon: kit.DrawIconButton(r, style.icon, false, s); break;
            case Look::Toggle: kit.DrawToggle(r, checked, s); break;
            case Look::Chip: kit.DrawChip(r, text, checked, s); break;
            case Look::Segment: kit.DrawSegment(r, text, checked, style.part, s); break;
            case Look::Tab: kit.DrawTab(r, text, checked, s); break;
            case Look::Nav: kit.DrawNavItem(r, style.icon, text, checked, s); break;
            case Look::Expander: kit.DrawExpander(r, style.icon, text, style.detail, style.trailing, style.open, s); break;
            case Look::Radio: kit.DrawRadio(r, text, style.detail, checked, s); break;
            case Look::Custom: DrawControl(control, kit, r, s); break;
            }
        });
        if (!painted)
            InvalidateRect(control, nullptr, FALSE);
        return CDRF_SKIPDEFAULT;
    }

    ControlKit::ControlState DialogWindow::StateOf(HWND control, bool pressed) const
    {
        POINT cursor{};
        GetCursorPos(&cursor);
        const bool hot = WindowFromPoint(cursor) == control;
        return { IsWindowEnabled(control) != FALSE, hot, pressed, GetFocus() == control && FocusCues(control) };
    }

    // ---- Kit buttons ------------------------------------------------------------------------------------

    HWND DialogWindow::KitButton(Look look, const wchar_t* text, int id, Surface on, wchar_t icon, bool first)
    {
        DWORD style = 0;
        switch (look)
        {
        case Look::Button:
        case Look::Icon:
        case Look::Expander: style = BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP; break;
        case Look::Primary: style = BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP; break;
        case Look::Toggle:
        case Look::Chip: style = BS_AUTOCHECKBOX | (first ? WS_TABSTOP | WS_GROUP : 0); break;
        case Look::Segment:
        case Look::Tab:
        case Look::Nav:
        case Look::Radio:
        case Look::Custom: style = BS_AUTORADIOBUTTON | (first ? WS_TABSTOP | WS_GROUP : 0); break;
        }
        const HWND h = Control(WC_BUTTONW, text, style, id);
        if (h)
            m_styles[h] = Style{ .look = look, .on = on, .icon = icon };
        return h;
    }

    DialogWindow::Style& DialogWindow::StyleOf(HWND control)
    {
        return m_styles[control];
    }

    void DialogWindow::DestroyControl(HWND control)
    {
        if (!control)
            return;
        m_styles.erase(control);
        m_labels.erase(control);
        m_dropDowns.erase(control);
        m_sliders.erase(control);
        m_fields.erase(control);
        m_lists.erase(control);
        if (m_accessibility && std::erase(m_described, control))
        {
            const MSAAPROPID props[] = { PROPID_ACC_NAME, PROPID_ACC_DESCRIPTION };
            m_accessibility->ClearHwndProps(control, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, props, 2);
        }
        DestroyWindow(control);
    }

    void DialogWindow::CheckOne(std::span<const HWND> group, int index)
    {
        for (size_t i = 0; i < group.size(); ++i)
        {
            const HWND button = group[i];
            SendMessageW(button, BM_SETCHECK, static_cast<int>(i) == index ? BST_CHECKED : BST_UNCHECKED, 0);
            // Tab goes to the checked button, or the first while none is, as in any radio group.
            const LONG_PTR style = GetWindowLongPtrW(button, GWL_STYLE);
            const bool stop = static_cast<int>(i) == std::max(index, 0);
            SetWindowLongPtrW(button, GWL_STYLE, stop ? style | WS_TABSTOP : style & ~static_cast<LONG_PTR>(WS_TABSTOP));
            InvalidateRect(button, nullptr, FALSE);
        }
    }

    // ---- Labels -----------------------------------------------------------------------------------------

    namespace
    {
        struct RoleLook
        {
            float size;
            DWRITE_FONT_WEIGHT weight;
            int colour;   // 0 text, 1 subtle, 2 faint, 3 error
            bool wrap;
            DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING;
        };

        RoleLook LookOf(int role)
        {
            constexpr auto kSemi = DWRITE_FONT_WEIGHT_SEMI_BOLD, kNormal = DWRITE_FONT_WEIGHT_NORMAL;
            constexpr RoleLook kLooks[] = {
                { 28.f, kSemi, 0, false },   // Heading
                { 14.f, kSemi, 0, false },   // Group
                { 14.f, kNormal, 0, false }, // Title
                { 13.f, kNormal, 0, true },  // Body
                { 12.f, kNormal, 1, true },  // Caption
                { 12.f, kNormal, 2, true },  // Hint
                { 12.f, kNormal, 3, true },  // Error
                { 13.f, kSemi, 0, false },   // Strong
                { 13.f, kSemi, 0, false, DWRITE_TEXT_ALIGNMENT_TRAILING },   // Value
                { 16.f, kSemi, 0, false },   // AppName
                { 11.5f, kNormal, 2, false },// Small
                { 12.f, kNormal, 1, true, DWRITE_TEXT_ALIGNMENT_CENTER },    // Centred
                { 20.f, kSemi, 0, true },    // Question
            };
            return kLooks[std::clamp(role, 0, static_cast<int>(std::size(kLooks)) - 1)];
        }
    }

    HWND DialogWindow::Label(const wchar_t* text, TextRole role, Surface on)
    {
        // SS_NOPREFIX: an "&" is shown as it is. WS_GROUP: a label never joins the controls' groups.
        const HWND h = Control(WC_STATICW, text, SS_OWNERDRAW | SS_NOPREFIX | WS_GROUP);
        if (h)
            m_labels[h] = { role, on };
        return h;
    }

    void DialogWindow::SetRole(HWND label, TextRole role)
    {
        if (auto it = m_labels.find(label); it != m_labels.end() && it->second.role != role)
        {
            it->second.role = role;
            InvalidateRect(label, nullptr, FALSE);
        }
    }

    int DialogWindow::LabelHeight(HWND label, int widthPx)
    {
        const auto it = m_labels.find(label);
        const auto look = LookOf(static_cast<int>(it == m_labels.end() ? TextRole::Body : it->second.role));
        const std::wstring text = Text(label);
        const float width = Dip(widthPx) - (it != m_labels.end() && it->second.role == TextRole::Error ? 18.f : 0.f);
        const float h = look.wrap ? Kit().TextHeight(text.empty() ? L" " : text, look.size, look.weight, width)
                                  : std::ceil(look.size * 1.34f);
        return Px(static_cast<int>(std::ceil(h))) + 1;
    }

    void DialogWindow::DrawLabel(const DRAWITEMSTRUCT& item)
    {
        const auto& info = m_labels[item.hwndItem];
        const auto look = LookOf(static_cast<int>(info.role));
        const std::wstring text = Text(item.hwndItem);
        const float alpha = IsWindowEnabled(item.hwndItem) ? 1.f : ControlKit::kDisabledAlpha;
        Paint(item.hDC, item.rcItem, SurfaceColor(info.on), [&](ControlKit& kit, const D2D1_RECT_F& r) {
            const auto& c = kit.Theme();
            const D2D1_COLOR_F colours[] = { c.text, c.textDim, c.textFaint, c.danger };
            const auto colour = theme::WithAlpha(colours[look.colour], alpha);
            auto box = r;
            if (info.role == TextRole::Error && !text.empty())
            {
                kit.DrawGlyph(kGlyphError, 12.f, D2D1::RectF(r.left, r.top, r.left + 14, r.top + 17), colour);
                box.left += 18;
            }
            if (look.wrap)
                kit.DrawParagraph(text, look.size, look.weight, box, colour, look.align);
            else
                kit.DrawText(text, kit.TextFormat(look.size, look.weight, look.align), box, colour);
        });
    }

    // ---- Drop-downs ---------------------------------------------------------------------------------------

    HWND DialogWindow::DropDown(int id, std::span<const std::wstring> items, Surface on)
    {
        const HWND h = Control(WC_COMBOBOXW, L"", WS_TABSTOP | WS_GROUP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, id);
        if (!h)
            return nullptr;
        for (const auto& item : items)
            SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
        m_dropDowns[h] = on;
        SetWindowSubclass(h, DropDownProc, 1, reinterpret_cast<DWORD_PTR>(this));
        return h;
    }

    void DialogWindow::PaintDropDown(HWND combo)
    {
        PAINTSTRUCT ps;
        const HDC dc = BeginPaint(combo, &ps);
        RECT rc{};
        GetClientRect(combo, &rc);
        std::wstring text;
        if (const auto i = SendMessageW(combo, CB_GETCURSEL, 0, 0); i != CB_ERR)
        {
            text.resize(static_cast<size_t>(std::max<LRESULT>(0, SendMessageW(combo, CB_GETLBTEXTLEN, static_cast<WPARAM>(i), 0))) + 1);
            text.resize(static_cast<size_t>(std::max<LRESULT>(0, SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(text.data())))));
        }
        const bool open = SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) != FALSE;
        auto s = StateOf(combo, false);
        s.hot = s.hot || m_hotDropDown == combo;
        const auto it = m_dropDowns.find(combo);
        Paint(dc, rc, SurfaceColor(it == m_dropDowns.end() ? Surface::Card : it->second),
              [&](ControlKit& kit, const D2D1_RECT_F& r) { kit.DrawDropDown(r, text, open, s); });
        EndPaint(combo, &ps);
    }

    void DialogWindow::DrawDropDownItem(const DRAWITEMSTRUCT& item)
    {
        if (item.itemState & ODS_COMBOBOXEDIT)
        {
            // The closed box: drawn whole by our WM_PAINT.
            InvalidateRect(item.hwndItem, nullptr, FALSE);
            return;
        }
        if (item.itemID == static_cast<UINT>(-1))
            return;
        std::wstring text(static_cast<size_t>(std::max<LRESULT>(0, SendMessageW(item.hwndItem, CB_GETLBTEXTLEN, item.itemID, 0))) + 1, L'\0');
        text.resize(static_cast<size_t>(std::max<LRESULT>(0, SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(text.data())))));
        const bool hot = (item.itemState & ODS_SELECTED) != 0;
        Paint(item.hDC, item.rcItem, FromColorRef(m_colors.field),
              [&](ControlKit& kit, const D2D1_RECT_F& r) { kit.DrawListRow(r, text, false, hot); });
    }

    LRESULT CALLBACK DialogWindow::DropDownProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR self)
    {
        auto* window = reinterpret_cast<DialogWindow*>(self);
        switch (msg)
        {
        case WM_PAINT:
            window->PaintDropDown(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE:
            if (window->m_hotDropDown != hwnd)
            {
                window->m_hotDropDown = hwnd;
                TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&track);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            if (window->m_hotDropDown == hwnd)
            {
                window->m_hotDropDown = nullptr;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ENABLE:
        {
            const LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
            InvalidateRect(hwnd, nullptr, FALSE);
            return result;
        }
        case WM_MOUSEWHEEL:
            // Closed, the wheel scrolls the window instead of changing the choice under the cursor.
            if (!SendMessageW(hwnd, CB_GETDROPPEDSTATE, 0, 0))
                return SendMessageW(GetParent(hwnd), msg, wParam, lParam);
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, DropDownProc, id);
            if (window->m_hotDropDown == hwnd)
                window->m_hotDropDown = nullptr;
            break;
        default:
            break;
        }
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    // ---- Text fields --------------------------------------------------------------------------------------

    HWND DialogWindow::TextField(int id, DWORD style, const wchar_t* cue, bool search, Surface on)
    {
        const HWND h = Control(WC_EDITW, L"", WS_TABSTOP | WS_GROUP | ES_AUTOHSCROLL | style, id);
        if (!h)
            return nullptr;
        if (cue)
            SendMessageW(h, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));   // TRUE: kept while focused and empty
        m_fields[h] = FieldInfo{ {}, search, false, on };
        return h;
    }

    void DialogWindow::PlaceField(HWND edit, const RECT& box)
    {
        auto it = m_fields.find(edit);
        if (it == m_fields.end())
            return;
        InvalidateRect(m_hwnd, &it->second.box, FALSE);
        it->second.box = box;
        InvalidateRect(m_hwnd, &box, FALSE);
        // The edit sits inside the box, as tall as a line of its font and centred.
        int line = Px(18);
        if (const HDC dc = GetDC(edit))
        {
            const auto old = SelectObject(dc, m_font.get());
            TEXTMETRICW tm{};
            if (GetTextMetricsW(dc, &tm))
                line = tm.tmHeight + Px(2);
            SelectObject(dc, old);
            ReleaseDC(edit, dc);
        }
        const int left = box.left + Px(it->second.search ? 34 : 10), right = box.right - Px(8);
        const int top = box.top + std::max(0, static_cast<int>(box.bottom - box.top) - line) / 2;
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        MoveWindow(edit, left, top, std::max(0, right - left), std::min(line, static_cast<int>(box.bottom - box.top)), TRUE);
    }

    void DialogWindow::SetInvalid(HWND edit, bool invalid)
    {
        if (auto it = m_fields.find(edit); it != m_fields.end() && it->second.invalid != invalid)
        {
            it->second.invalid = invalid;
            InvalidateRect(m_hwnd, &it->second.box, FALSE);
        }
    }

    // ---- Sliders ------------------------------------------------------------------------------------------

    HWND DialogWindow::Slider(int id, int low, int high, int step, Surface on)
    {
        const HWND h = Control(TRACKBAR_CLASSW, L"", WS_TABSTOP | WS_GROUP | TBS_HORZ | TBS_NOTICKS, id);
        if (!h)
            return nullptr;
        SendMessageW(h, TBM_SETRANGE, FALSE, MAKELPARAM(low, high));
        SendMessageW(h, TBM_SETLINESIZE, 0, step);
        SendMessageW(h, TBM_SETPAGESIZE, 0, 5 * step);
        m_sliders[h] = on;
        return h;
    }

    LRESULT DialogWindow::DrawSlider(const NMCUSTOMDRAW& draw)
    {
        if (draw.dwDrawStage != CDDS_PREPAINT)
            return CDRF_DODEFAULT;
        const HWND slider = draw.hdr.hwndFrom;
        const auto low = static_cast<float>(SendMessageW(slider, TBM_GETRANGEMIN, 0, 0)), high = static_cast<float>(SendMessageW(slider, TBM_GETRANGEMAX, 0, 0));
        const auto pos = static_cast<float>(SendMessageW(slider, TBM_GETPOS, 0, 0));
        const auto s = StateOf(slider, GetCapture() == slider);
        RECT rc{};
        GetClientRect(slider, &rc);
        Paint(draw.hdc, rc, SurfaceColor(m_sliders[slider]), [&](ControlKit& kit, const D2D1_RECT_F& r) {
            const float cy = std::round((r.top + r.bottom) / 2);
            kit.DrawSlider(D2D1::RectF(r.left + 10, cy - 2, r.right - 10, cy + 2), high > low ? (pos - low) / (high - low) : 0.f, 0.f, s);
        });
        return CDRF_SKIPDEFAULT;
    }

    // ---- Lists --------------------------------------------------------------------------------------------

    HWND DialogWindow::KitList(int id, DWORD style, std::initializer_list<const wchar_t*> columns, int rowDip, Surface on)
    {
        const HWND h = Control(WC_LISTVIEWW, L"", WS_TABSTOP | WS_GROUP | LVS_REPORT | LVS_SHOWSELALWAYS | style, id);
        if (!h)
            return nullptr;
        ListView_SetExtendedListViewStyle(h, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        int i = 0;
        for (const wchar_t* title : columns)
        {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_SUBITEM;
            col.pszText = const_cast<wchar_t*>(title);
            col.iSubItem = i;
            ListView_InsertColumn(h, i++, &col);
        }
        m_lists[h] = { rowDip, on };
        return h;
    }

    LRESULT DialogWindow::DrawListRow(const NMLVCUSTOMDRAW& draw)
    {
        // Group headers (the inspector's, History's days) as the list draws them; rows by the kit.
        if (draw.dwItemType == LVCDI_GROUP)
            return CDRF_DODEFAULT;
        if (draw.nmcd.dwDrawStage == CDDS_PREPAINT)
            return CDRF_NOTIFYITEMDRAW;
        if (draw.nmcd.dwDrawStage != CDDS_ITEMPREPAINT)
            return CDRF_DODEFAULT;
        const HWND list = draw.nmcd.hdr.hwndFrom;
        const int row = static_cast<int>(draw.nmcd.dwItemSpec);
        RECT rc{};
        if (!ListView_GetItemRect(list, row, &rc, LVIR_BOUNDS))
            return CDRF_DODEFAULT;
        RECT client{};
        GetClientRect(list, &client);
        rc.left = std::max(rc.left, client.left);
        rc.right = std::min(rc.right, client.right);
        const UINT state = ListView_GetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED);
        const bool selected = (state & LVIS_SELECTED) != 0, active = GetFocus() == list;
        const bool focused = (state & LVIS_FOCUSED) && active && FocusCues(list);
        const bool hot = (draw.nmcd.uItemState & CDIS_HOT) != 0;
        const auto it = m_lists.find(list);
        Paint(draw.nmcd.hdc, rc, SurfaceColor(it == m_lists.end() ? Surface::Card : it->second.on), [&](ControlKit& kit, const D2D1_RECT_F& r) {
            kit.DrawListItem(r, selected, hot, focused, active);
            DrawRow(list, row, kit, r, selected);
        });
        return CDRF_SKIPDEFAULT;
    }

    // ---- Screen readers -----------------------------------------------------------------------------------

    void DialogWindow::Describe(HWND control, const std::wstring& name, const std::wstring& description)
    {
        if (!control)
            return;
        if (!m_accessibility &&
            FAILED(CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(m_accessibility.GetAddressOf()))))
            return;
        m_accessibility->SetHwndPropStr(control, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, PROPID_ACC_NAME, name.c_str());
        if (!description.empty())
            m_accessibility->SetHwndPropStr(control, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, PROPID_ACC_DESCRIPTION, description.c_str());
        if (std::find(m_described.begin(), m_described.end(), control) == m_described.end())
            m_described.push_back(control);
    }

    // ---- Scrolling ----------------------------------------------------------------------------------------

    void DialogWindow::SetScroll(const RECT& area, int contentHeight)
    {
        m_scrollArea = area;
        m_scrollHeight = std::max(0, contentHeight);
        const int view = std::max(0, static_cast<int>(area.bottom - area.top));
        m_scrollY = std::clamp(m_scrollY, 0, std::max(0, m_scrollHeight - view));
        SCROLLINFO info{ sizeof(info), SIF_ALL | SIF_DISABLENOSCROLL };
        info.nMin = 0;
        info.nMax = std::max(0, m_scrollHeight - 1);
        info.nPage = static_cast<UINT>(view);
        info.nPos = m_scrollY;
        SetScrollInfo(m_hwnd, SB_VERT, &info, TRUE);
    }

    void DialogWindow::ScrollTo(int y)
    {
        const int view = std::max(0, static_cast<int>(m_scrollArea.bottom - m_scrollArea.top));
        y = std::clamp(y, 0, std::max(0, m_scrollHeight - view));
        if (y == m_scrollY)
            return;
        m_scrollY = y;
        SetScrollPos(m_hwnd, SB_VERT, y, TRUE);
        // Move everything at once, then paint once.
        SendMessageW(m_hwnd, WM_SETREDRAW, FALSE, 0);
        Layout();
        SendMessageW(m_hwnd, WM_SETREDRAW, TRUE, 0);
        RedrawWindow(m_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    void DialogWindow::ScrollIntoView(const RECT& r)
    {
        if (!m_frame.scrolls || r.left < m_scrollArea.left || r.left >= m_scrollArea.right)
            return;
        const int margin = Px(16);
        if (r.top < m_scrollArea.top + margin)
            ScrollTo(m_scrollY - (m_scrollArea.top + margin - r.top));
        else if (r.bottom > m_scrollArea.bottom - margin)
            ScrollTo(m_scrollY + std::min(r.bottom - (m_scrollArea.bottom - margin), r.top - (m_scrollArea.top + margin)));
    }

    // ---- The window ---------------------------------------------------------------------------------------

    void DialogWindow::PaintWindow()
    {
        PAINTSTRUCT ps;
        const HDC dc = BeginPaint(m_hwnd, &ps);
        RECT rc{};
        GetClientRect(m_hwnd, &rc);
        const HWND focus = GetFocus();
        const bool painted = Paint(dc, rc, FromColorRef(m_colors.background), [&](ControlKit& kit, const D2D1_RECT_F& client) {
            PaintSurface(kit, client);
            for (const auto& [edit, field] : m_fields)
            {
                if (!IsWindowVisible(edit) || field.box.right <= field.box.left)
                    continue;
                const auto box = DipRect(field.box);
                kit.DrawTextBox(box, focus == edit, field.invalid, IsWindowEnabled(edit) != FALSE);
                if (field.search)
                    kit.DrawGlyph(kGlyphSearch, 13.f, D2D1::RectF(box.left + 8, box.top, box.left + 28, box.bottom), kit.Theme().textDim);
            }
        });
        if (!painted)
            FillRect(dc, &rc, m_background.get());
        PaintGdi(dc);
        EndPaint(m_hwnd, &ps);
    }

    bool DialogWindow::KitMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result)
    {
        switch (msg)
        {
        case WM_NOTIFY:
        {
            const auto* hdr = reinterpret_cast<const NMHDR*>(lParam);
            if (hdr->code != NM_CUSTOMDRAW)
                return false;
            if (const auto it = m_styles.find(hdr->hwndFrom); it != m_styles.end())
            {
                result = DrawStyled(*reinterpret_cast<const NMCUSTOMDRAW*>(lParam), it->second);
                return true;
            }
            if (m_sliders.contains(hdr->hwndFrom))
            {
                result = DrawSlider(*reinterpret_cast<const NMCUSTOMDRAW*>(lParam));
                return true;
            }
            if (m_lists.contains(hdr->hwndFrom))
            {
                result = DrawListRow(*reinterpret_cast<const NMLVCUSTOMDRAW*>(lParam));
                return true;
            }
            return false;
        }
        case WM_DRAWITEM:
        {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (item->CtlType == ODT_STATIC && m_labels.contains(item->hwndItem))
                DrawLabel(*item);
            else if (item->CtlType == ODT_COMBOBOX && m_dropDowns.contains(item->hwndItem))
                DrawDropDownItem(*item);
            else
                return false;
            result = TRUE;
            return true;
        }
        case WM_MEASUREITEM:
        {
            // Sent while a drop-down is being created, before DropDown() knows its handle; the kit's
            // drop-downs are the only owner-drawn combo boxes.
            auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (item->CtlType != ODT_COMBOBOX)
                return false;
            item->itemHeight = static_cast<UINT>(Px(item->itemID == static_cast<UINT>(-1) ? kDropDownField : kDropDownRow));
            result = TRUE;
            return true;
        }
        case WM_COMMAND:
        {
            // Observed only: the window still gets its controls' commands.
            const auto control = reinterpret_cast<HWND>(lParam);
            const UINT code = HIWORD(wParam);
            if ((code == EN_SETFOCUS || code == EN_KILLFOCUS) && m_fields.contains(control))
                InvalidateRect(m_hwnd, &m_fields[control].box, FALSE);
            if ((code == CBN_DROPDOWN || code == CBN_CLOSEUP || code == CBN_SELCHANGE) && m_dropDowns.contains(control))
                InvalidateRect(control, nullptr, FALSE);
            return false;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORSTATIC:
        {
            // Text fields, a read-only one too, and the open list of a drop-down: the field colour.
            const auto control = reinterpret_cast<HWND>(lParam);
            const bool field = m_fields.contains(control);
            if (!field && !(msg == WM_CTLCOLORLISTBOX && !m_dropDowns.empty()))
                return false;
            const HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, field && !IsWindowEnabled(control) ? m_colors.muted : m_colors.text);
            SetBkColor(dc, m_colors.field);
            result = reinterpret_cast<LRESULT>(m_field.get());
            return true;
        }
        case WM_LBUTTONDOWN:
        {
            // A click in a field's box, beside its edit, is a click in the field.
            const POINT p{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            for (const auto& [edit, field] : m_fields)
                if (IsWindowVisible(edit) && IsWindowEnabled(edit) && PtInRect(&field.box, p))
                {
                    SetFocus(edit);
                    SendMessageW(edit, EM_SETSEL, 0, -1);
                    result = 0;
                    return true;
                }
            return false;
        }
        case WM_VSCROLL:
        {
            if (!m_frame.scrolls || lParam)
                return false;
            const int view = static_cast<int>(m_scrollArea.bottom - m_scrollArea.top), line = Px(40);
            SCROLLINFO info{ sizeof(info), SIF_TRACKPOS };
            GetScrollInfo(m_hwnd, SB_VERT, &info);
            switch (LOWORD(wParam))
            {
            case SB_LINEUP: ScrollTo(m_scrollY - line); break;
            case SB_LINEDOWN: ScrollTo(m_scrollY + line); break;
            case SB_PAGEUP: ScrollTo(m_scrollY - std::max(line, view - line)); break;
            case SB_PAGEDOWN: ScrollTo(m_scrollY + std::max(line, view - line)); break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: ScrollTo(info.nTrackPos); break;
            case SB_TOP: ScrollTo(0); break;
            case SB_BOTTOM: ScrollTo(m_scrollHeight); break;
            default: break;
            }
            result = 0;
            return true;
        }
        case WM_MOUSEWHEEL:
            if (!m_frame.scrolls)
                return false;
            ScrollTo(m_scrollY - GET_WHEEL_DELTA_WPARAM(wParam) * Px(48) / WHEEL_DELTA);
            result = 0;
            return true;
        case WM_PAINT:
            PaintWindow();
            result = 0;
            return true;
        case WM_ERASEBKGND:
            result = 1;   // WM_PAINT paints all of it
            return true;
        case WM_DESTROY:
            // Annotations live with the controls; take them back while the controls still exist.
            if (m_accessibility)
            {
                const MSAAPROPID props[] = { PROPID_ACC_NAME, PROPID_ACC_DESCRIPTION };
                for (HWND control : m_described)
                    m_accessibility->ClearHwndProps(control, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, props, 2);
            }
            m_described.clear();
            return false;
        default:
            return false;
        }
    }

    bool PickColour(HWND owner, uint32_t& rgb)
    {
        static COLORREF custom[16]{};
        CHOOSECOLORW cc{ sizeof(cc) };
        cc.hwndOwner = owner;
        cc.rgbResult = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
        cc.lpCustColors = custom;
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (!ChooseColorW(&cc))
            return false;
        rgb = (static_cast<uint32_t>(GetRValue(cc.rgbResult)) << 16) | (static_cast<uint32_t>(GetGValue(cc.rgbResult)) << 8) | GetBValue(cc.rgbResult);
        return true;
    }

    bool DialogWindow::PreTranslate(MSG& msg)
    {
        if (!m_hwnd || (msg.hwnd != m_hwnd && !IsChild(m_hwnd, msg.hwnd)))
            return false;
        const HWND before = GetFocus();
        const bool consumed = IsDialogMessageW(m_hwnd, &msg) != FALSE;
        // Tab or an arrow moved the focus: bring the control into view in a scrolling column.
        if (const HWND after = GetFocus(); m_hwnd && m_frame.scrolls && after != before && after && IsChild(m_hwnd, after))
        {
            RECT r{};
            GetWindowRect(after, &r);
            MapWindowPoints(nullptr, m_hwnd, reinterpret_cast<POINT*>(&r), 2);
            ScrollIntoView(r);
        }
        return consumed;
    }

    LRESULT CALLBACK DialogWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_NCCREATE)
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        auto* self = reinterpret_cast<DialogWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self)
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        if (!self->m_hwnd)
        {
            // Messages before CreateWindowExW returns: the kit's drop-downs are measured now.
            LRESULT result = 0;
            if (msg == WM_MEASUREITEM && self->KitMessage(msg, wParam, lParam, result))
                return result;
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        if (self->m_hwnd != hwnd)
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        if (msg == WM_NCDESTROY)
        {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            self->m_hwnd = nullptr;
            self->OnNcDestroy();
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        LRESULT result = 0;
        if (self->KitMessage(msg, wParam, lParam, result))
            return result;
        return self->Handle(msg, wParam, lParam);
    }

    LRESULT DialogWindow::Handle(UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_SIZE:
            Layout();
            return 0;
        case WM_GETMINMAXINFO:
        {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize = { Px(m_frame.minWidth), Px(m_frame.minHeight) };
            return 0;
        }
        case WM_DPICHANGED:
        {
            // The window moved to a monitor with another scale. lParam is the size Windows suggests at the new DPI.
            const auto* r = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(m_hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            ApplyDpi(HIWORD(wParam));
            return 0;
        }
        case WM_SETTINGCHANGE:
            // "ImmersiveColorSet": the user switched between light and dark.
            if (lParam && EqualsNoCase(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet"))
                ApplyTheme();
            break;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, StaticTextColor(reinterpret_cast<HWND>(lParam)));
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(m_background.get());
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            if (m_colors.dark)
            {
                const HDC dc = reinterpret_cast<HDC>(wParam);
                SetTextColor(dc, m_colors.text);
                SetBkColor(dc, m_colors.field);
                return reinterpret_cast<LRESULT>(m_field.get());
            }
            break;
        default:
            break;
        }
        return DefWindowProcW(m_hwnd, msg, wParam, lParam);
    }
}
