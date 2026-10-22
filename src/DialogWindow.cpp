// The accessibility GUIDs (CLSID_AccPropServices, PROPID_ACC_*) are defined here: no import library has them.
#include <windows.h>
#include <initguid.h>
#include <oleacc.h>

#include "DialogWindow.h"
#include "EditorChrome.h"
#include "Strings.h"
#include "resource.h"

#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <wil/result.h>
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
        Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> target;   // bound to `memory` for each drawing
        UINT dpi = 0;                                          // the target's
        ControlKit kit;
        // What each control and the window draw into first, at its top left, then copy to the DC they
        // were given with BitBlt. Never the given DC itself: a control may hand over a buffered-paint DC
        // whose origin is moved to the part being repainted (a trackbar moving its thumb, a button
        // fading between states), which GDI honours and Direct2D's DC target does not, so drawing on it
        // straight put the control's left end where its thumb was: the streaks.
        wil::unique_hbitmap bitmap;   // before `memory`: the DC goes first and lets go of it
        wil::unique_hdc memory;
        SIZE bufferSize{};
        // The window's surface (its cards, panels, field boxes) as WM_PAINT last drew it, whole: every
        // control draws its part of it behind itself, so a label or a control shows exactly what it
        // sits on (a card, a panel inside it, a card's border and rounded corners) instead of a
        // rectangle of a colour it was told.
        Microsoft::WRL::ComPtr<ID2D1BitmapRenderTarget> backdropTarget;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> backdrop;
        SIZE backdropSize{};
        bool backdropStale = true;
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
        // Focus rings as a dialog has them: hidden when it was opened with the mouse, shown once the
        // keyboard moves the focus (PreTranslate).
        SendMessageW(m_hwnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_INITIALIZE, 0), 0);
        SetFocus(focus);
    }

    HWND DialogWindow::Control(const wchar_t* windowClass, const wchar_t* text, DWORD style, int id, DWORD exStyle) const
    {
        // WS_CLIPSIBLINGS: a control never paints over another, so what a control shows is always its own
        // drawing, even where two meet for a moment.
        return CreateWindowExW(exStyle, windowClass, text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, 0, 0, 0, 0, m_hwnd,
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
        // No erase: WM_PAINT paints all of the window, and each control all of itself.
        RedrawWindow(m_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
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

    void DialogWindow::ResetTarget()
    {
        auto& k = *m_kit;
        k.kit.SetTarget(nullptr);
        k.backdrop.Reset();
        k.backdropTarget.Reset();
        k.backdropSize = {};
        k.target.Reset();
    }

    bool DialogWindow::EnsureTarget()
    {
        Kit();
        auto& k = *m_kit;
        if (k.target && k.dpi != m_dpi)
            ResetTarget();
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
        return true;
    }

    HDC DialogWindow::Buffer(int width, int height)
    {
        auto& k = *m_kit;
        if (!k.memory)
            k.memory.reset(CreateCompatibleDC(nullptr));
        if (!k.memory)
            return nullptr;
        if (!k.bitmap || width > k.bufferSize.cx || height > k.bufferSize.cy)
        {
            // Grown to the largest drawing so far; a top-down 32-bit DIB.
            const SIZE size{ std::max<LONG>(width, k.bufferSize.cx), std::max<LONG>(height, k.bufferSize.cy) };
            BITMAPINFO info{};
            info.bmiHeader = { sizeof(BITMAPINFOHEADER), size.cx, -size.cy, 1, 32, BI_RGB };
            void* bits = nullptr;
            wil::unique_hbitmap bitmap(CreateDIBSection(k.memory.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
            if (!bitmap)
                return nullptr;
            SelectObject(k.memory.get(), bitmap.get());
            k.bitmap = std::move(bitmap);   // the old one is no longer selected: freed here
            k.bufferSize = size;
        }
        return k.memory.get();
    }

    void DialogWindow::PaintBackdrop(ControlKit& kit, const D2D1_RECT_F& client)
    {
        PaintSurface(kit, client);
        const HWND focus = GetFocus();
        for (const auto& [edit, field] : m_fields)
        {
            if (!IsWindowVisible(edit) || field.box.right <= field.box.left)
                continue;
            const auto box = DipRect(field.box);
            kit.DrawTextBox(box, focus == edit, field.invalid, IsWindowEnabled(edit) != FALSE);
            if (field.search)
                kit.DrawGlyph(kGlyphSearch, 13.f, D2D1::RectF(box.left + 8, box.top, box.left + 28, box.bottom), kit.Theme().textDim);
        }
    }

    bool DialogWindow::EnsureBackdrop()
    {
        auto& k = *m_kit;
        RECT client{};
        GetClientRect(m_hwnd, &client);
        const SIZE size{ client.right, client.bottom };
        if (size.cx <= 0 || size.cy <= 0)
            return false;
        if (k.backdrop && !k.backdropStale && size.cx == k.backdropSize.cx && size.cy == k.backdropSize.cy)
            return true;
        const float scale = static_cast<float>(m_dpi) / 96.f;
        if (!k.backdropTarget || size.cx != k.backdropSize.cx || size.cy != k.backdropSize.cy)
        {
            k.backdrop.Reset();
            k.backdropTarget.Reset();
            k.backdropSize = {};
            // A bitmap of the client area's pixels at the window's DPI, opaque (ClearType) when it can be.
            const D2D1_SIZE_F dips{ static_cast<float>(size.cx) / scale, static_cast<float>(size.cy) / scale };
            const D2D1_SIZE_U pixels{ static_cast<UINT32>(size.cx), static_cast<UINT32>(size.cy) };
            auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
            if (FAILED(k.target->CreateCompatibleRenderTarget(&dips, &pixels, &format, D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &k.backdropTarget)))
            {
                format.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
                if (FAILED(k.target->CreateCompatibleRenderTarget(&dips, &pixels, &format, D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &k.backdropTarget)))
                    return false;
            }
            if (FAILED(k.backdropTarget->GetBitmap(&k.backdrop)))
            {
                k.backdropTarget.Reset();
                return false;
            }
            k.backdropSize = size;
        }
        k.kit.SetTarget(k.backdropTarget.Get());
        k.backdropTarget->BeginDraw();
        k.backdropTarget->SetTransform(D2D1::Matrix3x2F::Identity());
        const auto behind = FromColorRef(m_colors.background);
        k.backdropTarget->Clear(D2D1::ColorF(behind.r, behind.g, behind.b));
        PaintBackdrop(k.kit, D2D1::RectF(0, 0, static_cast<float>(size.cx) / scale, static_cast<float>(size.cy) / scale));
        const HRESULT hr = k.backdropTarget->EndDraw();
        k.kit.SetTarget(k.target.Get());
        if (FAILED(hr))
        {
            k.backdrop.Reset();
            k.backdropTarget.Reset();
            k.backdropSize = {};
            return false;
        }
        k.backdropStale = false;
        return true;
    }

    bool DialogWindow::Paint(HDC dc, const RECT& rc, HWND over, const D2D1_COLOR_F& behind, const std::function<void(ControlKit&, const D2D1_RECT_F&)>& paint)
    {
        const int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0 || !EnsureTarget())
            return false;
        auto& k = *m_kit;
        const HDC memory = Buffer(w, h);
        const RECT bound{ 0, 0, w, h };
        if (!memory || FAILED(k.target->BindDC(memory, &bound)))
            return false;
        // Where `rc` is in the window's client area, for its part of the surface.
        POINT origin{ rc.left, rc.top };
        bool backdrop = false;
        if (over)
        {
            if (over != m_hwnd)
                MapWindowPoints(over, m_hwnd, &origin, 1);
            backdrop = EnsureBackdrop();
        }
        const float scale = static_cast<float>(m_dpi) / 96.f;
        k.target->BeginDraw();
        k.target->SetTransform(D2D1::Matrix3x2F::Identity());
        k.target->Clear(D2D1::ColorF(behind.r, behind.g, behind.b));
        if (backdrop)
        {
            // Pixel for pixel; only the part inside the client area (a control scrolled half out of it).
            const LONG x0 = std::max<LONG>(origin.x, 0), y0 = std::max<LONG>(origin.y, 0);
            const LONG x1 = std::min<LONG>(origin.x + w, k.backdropSize.cx), y1 = std::min<LONG>(origin.y + h, k.backdropSize.cy);
            if (x1 > x0 && y1 > y0)
            {
                const auto dip = [scale](LONG v) { return static_cast<float>(v) / scale; };
                k.target->DrawBitmap(k.backdrop.Get(), D2D1::RectF(dip(x0 - origin.x), dip(y0 - origin.y), dip(x1 - origin.x), dip(y1 - origin.y)), 1.f,
                                     D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1::RectF(dip(x0), dip(y0), dip(x1), dip(y1)));
            }
        }
        paint(k.kit, D2D1::RectF(0, 0, static_cast<float>(w) / scale, static_cast<float>(h) / scale));
        const HRESULT hr = k.target->EndDraw();
        if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET))
        {
            ResetTarget();
            return false;
        }
        if (FAILED(hr))
            return false;
        // GDI copies it where `rc` is in the DC's own coordinates, inside its clipping.
        return BitBlt(dc, rc.left, rc.top, w, h, memory, 0, 0, SRCCOPY) != FALSE;
    }

    LRESULT DialogWindow::CustomDraw(const NMCUSTOMDRAW& draw, const KitPaint& paint)
    {
        if (draw.dwDrawStage != CDDS_PREPAINT)
            return CDRF_DODEFAULT;
        const UINT state = draw.uItemState;
        const ControlKit::ControlState s{ !(state & CDIS_DISABLED), (state & CDIS_HOT) != 0, (state & CDIS_SELECTED) != 0,
                                          (state & CDIS_FOCUS) && FocusCues(draw.hdr.hwndFrom) };
        if (!Paint(draw.hdc, draw.rc, draw.hdr.hwndFrom, FromColorRef(m_colors.background), [&](ControlKit& kit, const D2D1_RECT_F& r) { paint(kit, r, s); }))
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
        const bool painted = Paint(draw.hdc, draw.rc, control, SurfaceColor(style.on), [&](ControlKit& kit, const D2D1_RECT_F& r) {
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
                { 11.5f, kNormal, 2, false, DWRITE_TEXT_ALIGNMENT_TRAILING },   // SmallEnd
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
        Paint(item.hDC, item.rcItem, item.hwndItem, SurfaceColor(info.on), [&](ControlKit& kit, const D2D1_RECT_F& r) {
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
        Paint(dc, rc, combo, SurfaceColor(it == m_dropDowns.end() ? Surface::Card : it->second),
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
        Paint(item.hDC, item.rcItem, nullptr, FromColorRef(m_colors.field),
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
        SetWindowSubclass(h, SliderProc, 1, reinterpret_cast<DWORD_PTR>(this));
        return h;
    }

    LRESULT CALLBACK DialogWindow::SliderProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR)
    {
        switch (msg)
        {
        case WM_PAINT:
            // A trackbar repaints only its thumb's old and new rectangles when the thumb moves, but the
            // kit's thumb is larger than the trackbar's and the bar's filled part runs up to it: each
            // repaint covers the whole control, so no piece of an earlier drawing stays behind (the
            // streaks and dashes along the bar).
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case WM_ERASEBKGND:
            return 1;   // painted whole by the custom draw
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, SliderProc, id);
            break;
        default:
            break;
        }
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    LRESULT DialogWindow::DrawSlider(const NMCUSTOMDRAW& draw)
    {
        if (draw.dwDrawStage != CDDS_PREPAINT)
            return CDRF_DODEFAULT;
        const HWND slider = draw.hdr.hwndFrom;
        const auto s = StateOf(slider, GetCapture() == slider);
        // The bar on the trackbar's own channel and the thumb on its own thumb: where it takes clicks,
        // and what it repaints when the thumb moves.
        RECT channel{}, thumb{}, rc{};
        SendMessageW(slider, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
        SendMessageW(slider, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
        GetClientRect(slider, &rc);
        const auto parts = chrome::SliderOnChannel(DipRect(channel), DipRect(thumb), 4.f);
        Paint(draw.hdc, rc, slider, SurfaceColor(m_sliders[slider]),
              [&](ControlKit& kit, const D2D1_RECT_F&) { kit.DrawSlider(parts.track, parts.value, 0.f, s); });
        return CDRF_SKIPDEFAULT;
    }

    int DialogWindow::PlaceSlider(HWND slider, const RECT& box, HWND low, HWND high)
    {
        SetWindowPos(slider, nullptr, box.left, box.top, std::max(0, static_cast<int>(box.right - box.left)), std::max(0, static_cast<int>(box.bottom - box.top)),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        // Where the bar is drawn (SliderOnChannel), in client pixels.
        RECT channel{};
        SendMessageW(slider, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
        const auto track = chrome::SliderOnChannel(DipRect(channel), {}, 4.f).track;
        const float left = Dip(box.left) + track.left, right = Dip(box.left) + track.right;
        const int top = box.bottom, height = LabelHeight(low ? low : high, Px(100));
        const auto width = [&](HWND label) { return label ? std::ceil(Kit().TextWidth(Text(label), LookOf(static_cast<int>(TextRole::Small)).size)) + 2.f : 0.f; };
        const auto ends = chrome::LayoutSliderEnds(left, right, Dip(top), Dip(height), width(low), width(high));
        const auto place = [&](HWND label, const D2D1_RECT_F& r, TextRole role) {
            if (!label)
                return;
            SetRole(label, role);
            const int l = static_cast<int>(std::floor(r.left * static_cast<float>(m_dpi) / 96.f)), rr = static_cast<int>(std::ceil(r.right * static_cast<float>(m_dpi) / 96.f));
            SetWindowPos(label, nullptr, l, top, rr - l, height, SWP_NOZORDER | SWP_NOACTIVATE);
        };
        place(low, ends.low, TextRole::Small);
        place(high, ends.high, TextRole::SmallEnd);
        return top + height;
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
        // On the list's own background colour (a list paints the rest of itself).
        Paint(draw.nmcd.hdc, rc, nullptr, SurfaceColor(it == m_lists.end() ? Surface::Card : it->second.on), [&](ControlKit& kit, const D2D1_RECT_F& r) {
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
        const int dy = m_scrollY - y;
        m_scrollY = y;
        SetScrollPos(m_hwnd, SB_VERT, y, TRUE);
        if (m_holds > 0 || !IsWindowVisible(m_hwnd))
        {
            Layout();   // a Rebuild() paints everything once it is done; a hidden window paints when shown
            return;
        }
        // Plain scrolling, as any window does it: the column's pixels and its controls move together in
        // one step, and only the strip that comes into view is painted. No control draws itself again
        // unless it comes into view.
        ScrollWindowEx(m_hwnd, 0, dy, &m_scrollArea, &m_scrollArea, nullptr, nullptr, SW_SCROLLCHILDREN | SW_INVALIDATE);
        m_scrolling = true;
        Layout();   // where the window's cards are now; its controls are there already
        m_scrolling = false;
        UpdateWindow(m_hwnd);
    }

    void DialogWindow::Rebuild(const std::function<void()>& change)
    {
        if (m_holds++ == 0)
        {
            // WM_SETREDRAW TRUE would show a hidden window: hold the drawing only while it is shown.
            m_held = m_hwnd && IsWindowVisible(m_hwnd);
            if (m_held)
                SendMessageW(m_hwnd, WM_SETREDRAW, FALSE, 0);
        }
        const auto done = wil::scope_exit([&] {
            if (--m_holds > 0 || !m_held)
                return;
            m_held = false;
            SendMessageW(m_hwnd, WM_SETREDRAW, TRUE, 0);
            // Once, from the new content: the window's surface, then every control over it. No erase:
            // WM_PAINT paints all of the window, and each control all of itself.
            RedrawWindow(m_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        });
        change();
    }

    void DialogWindow::ShowOnly(std::span<const HWND> all, std::span<const HWND> placed)
    {
        std::vector<HWND> shown(placed.begin(), placed.end());
        std::sort(shown.begin(), shown.end());
        for (HWND h : all)
        {
            const bool show = std::binary_search(shown.begin(), shown.end(), h);
            const bool visible = (GetWindowLongPtrW(h, GWL_STYLE) & WS_VISIBLE) != 0;
            if (show != visible)
                ShowWindow(h, show ? SW_SHOWNA : SW_HIDE);
        }
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
        // The surface is drawn again whole (Layout, a scroll, the theme or a field's focus changed it),
        // then shown: the window's own pixels are the backdrop's.
        Kit();
        m_kit->backdropStale = true;
        if (!Paint(dc, rc, m_hwnd, FromColorRef(m_colors.background), [](ControlKit&, const D2D1_RECT_F&) {}))
            FillRect(dc, &rc, m_background.get());
        PaintGdi(dc);
        EndPaint(m_hwnd, &ps);
        // The kit's controls over what was painted draw their part of the new surface behind them.
        for (HWND child = GetWindow(m_hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        {
            if (!IsWindowVisible(child) || !(m_styles.contains(child) || m_labels.contains(child) || m_dropDowns.contains(child) || m_sliders.contains(child)))
                continue;
            RECT r{}, overlap{};
            GetWindowRect(child, &r);
            MapWindowPoints(nullptr, m_hwnd, reinterpret_cast<POINT*>(&r), 2);
            if (IntersectRect(&overlap, &r, &ps.rcPaint))
                InvalidateRect(child, nullptr, FALSE);
        }
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
        case WM_PARENTNOTIFY:
            // A click on a control: focus rings are for the keyboard, so they go (as in WinUI), and
            // the control clicked shows none.
            if (const UINT event = LOWORD(wParam); event == WM_LBUTTONDOWN || event == WM_RBUTTONDOWN || event == WM_MBUTTONDOWN)
                ShowFocusCues(false);
            return false;
        case WM_LBUTTONDOWN:
        {
            ShowFocusCues(false);
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
            const int view = static_cast<int>(m_scrollArea.bottom - m_scrollArea.top), line = Px(static_cast<int>(chrome::kScrollLine));
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
        {
            // As far as the user set a notch to scroll (Mouse settings: lines at a time, or a page).
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            const int line = Px(static_cast<int>(chrome::kScrollLine)), view = static_cast<int>(m_scrollArea.bottom - m_scrollArea.top);
            ScrollTo(m_scrollY + chrome::WheelScroll(GET_WHEEL_DELTA_WPARAM(wParam), lines, line, std::max(line, view - line), m_wheelRest));
            result = 0;
            return true;
        }
        case WM_PAINT:
            PaintWindow();
            result = 0;
            return true;
        case WM_UPDATEUISTATE:
            // Focus rings shown or hidden: every control draws itself again (and the field boxes).
            result = DefWindowProcW(m_hwnd, msg, wParam, lParam);
            RedrawWindow(m_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
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

    void DialogWindow::ShowFocusCues(bool show)
    {
        if (!m_hwnd)
            return;
        const bool hidden = (SendMessageW(m_hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) != 0;
        if (hidden == show)
            SendMessageW(m_hwnd, WM_CHANGEUISTATE, MAKEWPARAM(show ? UIS_CLEAR : UIS_SET, UISF_HIDEFOCUS), 0);
    }

    bool DialogWindow::PreTranslate(MSG& msg)
    {
        if (!m_hwnd || (msg.hwnd != m_hwnd && !IsChild(m_hwnd, msg.hwnd)))
            return false;
        // The keys that move the focus show the focus rings (a click hides them: WM_PARENTNOTIFY).
        if (msg.message == WM_KEYDOWN && (msg.wParam == VK_TAB || (msg.wParam >= VK_PRIOR && msg.wParam <= VK_DOWN)))
            ShowFocusCues(true);
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
