#include "rename/RenameWindow.h"
#include "Brand.h"
#include "Log.h"
#include "PathUtil.h"
#include "Strings.h"
#include "Theme.h"
#include "resource.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shlwapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <format>

namespace sd
{
    namespace
    {
        constexpr wchar_t kClass[] = L"" SD_NAME L".Rename";
        constexpr int kIdPattern = 100, kIdFind = 101, kIdReplace = 102, kIdStart = 103, kIdMatchCase = 104, kIdList = 105;

        // Sizes in DIPs, scaled by the window's DPI in Layout().
        constexpr int kWidth = 640, kHeight = 540, kMinWidth = 420, kMinHeight = 380;
        constexpr int kMargin = 14, kGap = 8, kLabelW = 104, kRowH = 26, kRowGap = 6, kHintH = 20;
        constexpr int kButtonW = 110, kButtonH = 30, kStartW = 80;

        constexpr wchar_t kFieldsHint[] = L"{name}  name    {n} or {n:000}  number    {date}  modified    {ext}  extension    {folder}  folder";

        // {date}: the modified date, local time, as 2026-10-21 (sorts the same as it reads).
        rename::Item MakeItem(const std::wstring& path)
        {
            rename::Item item{ path, L"", false };
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
                return item;
            item.folder = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            SYSTEMTIME utc{}, local{};
            if (FileTimeToSystemTime(&data.ftLastWriteTime, &utc) && SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
                item.date = std::format(L"{:04}-{:02}-{:02}", local.wYear, local.wMonth, local.wDay);
            return item;
        }
    }

    RenameWindow::RenameWindow(HINSTANCE instance) : m_instance(instance) {}

    RenameWindow::~RenameWindow() = default;

    void RenameWindow::Show(std::vector<std::wstring> files, Confirm onConfirm)
    {
        if (files.empty())
            return;
        // Explorer's order by name ("photo 2" before "photo 10"), which is also the order {n} counts in.
        std::stable_sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) {
            return StrCmpLogicalW(FileName(a).c_str(), FileName(b).c_str()) < 0;
        });
        m_items.clear();
        for (const auto& f : files)
            m_items.push_back(MakeItem(f));
        m_onConfirm = std::move(onConfirm);

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
            m_hwnd.reset(CreateWindowExW(WS_EX_CONTROLPARENT, kClass, L"" SD_NAME, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                         kWidth, kHeight, nullptr, nullptr, m_instance, this));
            if (!m_hwnd)
                return;
            CreateChildren();
            const UINT dpi = GetDpiForWindow(m_hwnd.get());
            RECT r{ 0, 0, MulDiv(kWidth, static_cast<int>(dpi), 96), MulDiv(kHeight, static_cast<int>(dpi), 96) };
            AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_CONTROLPARENT, dpi);
            SetWindowPos(m_hwnd.get(), nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            ApplyDpi(dpi);
            m_filling = true;
            SetWindowTextW(m_pattern, L"{name}");
            SetWindowTextW(m_start, L"1");
            m_filling = false;
        }
        // The pattern and the rest stay as they were last time: renaming batch after batch the same way
        // needs no retyping, and the list shows what it does before anything happens.
        const auto title = m_items.size() == 1 ? std::format(L"Rename {} – " SD_NAME, FileName(m_items[0].path))
                                               : std::format(L"Rename {} items – " SD_NAME, m_items.size());
        SetWindowTextW(m_hwnd.get(), title.c_str());
        ApplyTheme();
        Update();
        ShowWindow(m_hwnd.get(), IsIconic(m_hwnd.get()) ? SW_RESTORE : SW_SHOWNORMAL);
        SetForegroundWindow(m_hwnd.get());   // allowed: the user just dropped on the slice
        SetFocus(m_pattern);
        SendMessageW(m_pattern, EM_SETSEL, 0, -1);
        log::Info(L"rename: window for {} item(s)", m_items.size());
    }

    void RenameWindow::CreateChildren()
    {
        const HWND parent = m_hwnd.get();
        const auto id = [](int value) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(value)); };
        // Each label is created just before its control: screen readers name a control after the
        // static text in front of it, and Tab visits the controls in this order.
        const auto label = [&](const wchar_t* text) {
            return CreateWindowExW(0, WC_STATICW, text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE, 0, 0, 0, 0, parent, nullptr,
                                   m_instance, nullptr);
        };
        const auto edit = [&](int controlId, DWORD style = 0) {
            return CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | style, 0, 0, 0, 0,
                                   parent, id(controlId), m_instance, nullptr);
        };
        m_patternLabel = label(L"&New name");
        m_pattern = edit(kIdPattern);
        SendMessageW(m_pattern, EM_LIMITTEXT, 255, 0);
        m_fields = CreateWindowExW(0, WC_STATICW, kFieldsHint, WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 0, 0, 0, 0,
                                   parent, nullptr, m_instance, nullptr);
        m_findLabel = label(L"&Find");
        m_find = edit(kIdFind);
        m_replaceLabel = label(L"Re&place with");
        m_replace = edit(kIdReplace);
        m_startLabel = label(L"&Start {n} at");
        m_start = edit(kIdStart, ES_NUMBER);
        SendMessageW(m_start, EM_LIMITTEXT, 6, 0);
        m_matchCase = CreateWindowExW(0, WC_BUTTONW, L"Find &matches case", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0,
                                      parent, id(kIdMatchCase), m_instance, nullptr);
        m_list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL |
                                     LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                                 0, 0, 0, 0, parent, id(kIdList), m_instance, nullptr);
        ListView_SetExtendedListViewStyle(m_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const wchar_t* titles[] = { L"Name now", L"New name", L"Note" };
        for (int i = 0; i < 3; ++i)
        {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_SUBITEM;
            col.pszText = const_cast<wchar_t*>(titles[i]);
            col.iSubItem = i;
            ListView_InsertColumn(m_list, i, &col);
        }
        m_status = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS | SS_CENTERIMAGE, 0, 0, 0, 0,
                                   parent, nullptr, m_instance, nullptr);
        const auto button = [&](const wchar_t* text, int controlId, DWORD style) {
            return CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 0, 0, parent, id(controlId),
                                   m_instance, nullptr);
        };
        // IDOK and IDCANCEL: IsDialogMessage turns Enter and Esc into them.
        m_rename = button(L"&Rename", IDOK, BS_DEFPUSHBUTTON);
        m_cancel = button(L"Cancel", IDCANCEL, BS_PUSHBUTTON);
    }

    void RenameWindow::ApplyDpi(UINT dpi)
    {
        m_dpi = dpi;
        NONCLIENTMETRICSW metrics{ sizeof(metrics) };
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi))
            m_font.reset(CreateFontIndirectW(&metrics.lfMessageFont));
        for (HWND child : { m_patternLabel, m_pattern, m_fields, m_findLabel, m_find, m_replaceLabel, m_replace, m_startLabel, m_start,
                            m_matchCase, m_list, m_status, m_rename, m_cancel })
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(m_font.get()), TRUE);
        Layout();
    }

    void RenameWindow::ApplyTheme()
    {
        // The same light or dark as Settings (theme::Current follows Windows or the override).
        m_dark = theme::Current(Style::Solid).dark;
        const BOOL dark = m_dark;
        DwmSetWindowAttribute(m_hwnd.get(), 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        const COLORREF back = m_dark ? RGB(32, 32, 32) : GetSysColor(COLOR_WINDOW);
        m_textColour = m_dark ? RGB(240, 240, 240) : GetSysColor(COLOR_WINDOWTEXT);
        m_fieldColour = m_dark ? RGB(45, 45, 45) : GetSysColor(COLOR_WINDOW);
        m_problemColour = m_dark ? RGB(255, 153, 164) : RGB(196, 43, 28);
        m_background.reset(CreateSolidBrush(back));
        m_field.reset(CreateSolidBrush(m_fieldColour));
        for (HWND child : { m_list, m_matchCase, m_rename, m_cancel })
            SetWindowTheme(child, m_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        for (HWND child : { m_pattern, m_find, m_replace, m_start })
            SetWindowTheme(child, m_dark ? L"DarkMode_CFD" : nullptr, nullptr);
        SetWindowTheme(ListView_GetHeader(m_list), m_dark ? L"DarkMode_ItemsView" : nullptr, nullptr);
        ListView_SetBkColor(m_list, m_fieldColour);
        ListView_SetTextBkColor(m_list, m_fieldColour);
        ListView_SetTextColor(m_list, m_textColour);
        InvalidateRect(m_hwnd.get(), nullptr, TRUE);
    }

    void RenameWindow::Layout()
    {
        RECT rc{};
        GetClientRect(m_hwnd.get(), &rc);
        const auto px = [this](int dip) { return MulDiv(dip, static_cast<int>(m_dpi), 96); };
        const int margin = px(kMargin), gap = px(kGap), lw = px(kLabelW), rh = px(kRowH), rowGap = px(kRowGap);
        const int fieldX = margin + lw, fieldW = std::max(0, static_cast<int>(rc.right) - margin - fieldX);
        int y = margin;
        const auto row = [&](HWND labelHwnd, HWND control) {
            MoveWindow(labelHwnd, margin, y, lw - gap, rh, TRUE);
            MoveWindow(control, fieldX, y, fieldW, rh, TRUE);
            y += rh + rowGap;
        };
        row(m_patternLabel, m_pattern);
        MoveWindow(m_fields, fieldX, y - rowGap / 2, fieldW, px(kHintH), TRUE);
        y += px(kHintH) + rowGap;
        row(m_findLabel, m_find);
        row(m_replaceLabel, m_replace);
        MoveWindow(m_startLabel, margin, y, lw - gap, rh, TRUE);
        MoveWindow(m_start, fieldX, y, px(kStartW), rh, TRUE);
        MoveWindow(m_matchCase, fieldX + px(kStartW) + 2 * gap, y, std::max(0, fieldW - px(kStartW) - 2 * gap), rh, TRUE);
        y += rh + gap;

        const int bw = px(kButtonW), bh = px(kButtonH), buttonsY = rc.bottom - margin - bh;
        const int listW = rc.right - 2 * margin;
        MoveWindow(m_list, margin, y, listW, std::max(0, buttonsY - gap - y), TRUE);
        const int inner = std::max(0, listW - GetSystemMetricsForDpi(SM_CXVSCROLL, m_dpi) - px(4));
        ListView_SetColumnWidth(m_list, 0, inner * 36 / 100);
        ListView_SetColumnWidth(m_list, 1, inner * 36 / 100);
        ListView_SetColumnWidth(m_list, 2, inner - 2 * (inner * 36 / 100));
        MoveWindow(m_cancel, rc.right - margin - bw, buttonsY, bw, bh, TRUE);
        MoveWindow(m_rename, rc.right - margin - 2 * bw - gap, buttonsY, bw, bh, TRUE);
        MoveWindow(m_status, margin, buttonsY, std::max(0, static_cast<int>(rc.right) - 3 * margin - 2 * bw - gap), bh, TRUE);
    }

    std::wstring RenameWindow::Text(HWND edit) const
    {
        std::wstring text(static_cast<size_t>(GetWindowTextLengthW(edit)) + 1, L'\0');
        text.resize(static_cast<size_t>(GetWindowTextW(edit, text.data(), static_cast<int>(text.size()))));
        return text;
    }

    void RenameWindow::Update()
    {
        rename::Pattern pattern;
        pattern.text = Text(m_pattern);
        pattern.find = Text(m_find);
        pattern.replace = Text(m_replace);
        pattern.matchCase = SendMessageW(m_matchCase, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const auto start = Text(m_start);
        pattern.start = start.empty() ? 1 : std::clamp(_wtoi(start.c_str()), 0, 999999);
        m_plan = rename::MakePlan(pattern, m_items);

        std::wstring status;
        if (!m_plan.error.empty())
            status = m_plan.error;
        else if (m_plan.problems > 0)
        {
            const auto first = std::find_if(m_plan.items.begin(), m_plan.items.end(), [](const rename::Planned& p) { return !p.problem.empty(); });
            status = m_plan.problems == 1 ? std::format(L"1 name can't be used: {}", first->problem)
                                          : std::format(L"{} names can't be used, such as: {}", m_plan.problems, first->problem);
        }
        else if (m_plan.changes == 0)
            status = L"No name changes yet";
        else if (m_items.size() == 1)
            status = L"Ready to rename";
        else if (m_plan.changes == m_items.size())
            status = std::format(L"All {} items get a new name", m_items.size());
        else
            status = std::format(L"{} of {} items get a new name", m_plan.changes, m_items.size());
        SetWindowTextW(m_status, status.c_str());
        EnableWindow(m_rename, m_plan.CanRun());
        ListView_SetItemCountEx(m_list, static_cast<int>(m_items.size()), LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
        InvalidateRect(m_list, nullptr, FALSE);
    }

    void RenameWindow::OnRename()
    {
        if (!m_plan.CanRun())
        {
            MessageBeep(MB_ICONWARNING);
            return;
        }
        std::vector<Rename> renames;
        for (const auto& p : m_plan.items)
            if (p.Changes())
                renames.push_back(Rename{ p.from, p.to });
        auto confirm = std::move(m_onConfirm);
        Close();
        log::Info(L"rename: {} of {} item(s) to rename", renames.size(), m_plan.items.size());
        if (confirm)
            confirm(std::move(renames));
    }

    void RenameWindow::Close()
    {
        ShowWindow(m_hwnd.get(), SW_HIDE);   // kept for next time; destroyed with the app
        m_items.clear();
        m_plan = {};
        m_onConfirm = nullptr;
        ListView_SetItemCountEx(m_list, 0, 0);
    }

    LRESULT RenameWindow::OnListNotify(NMHDR* hdr)
    {
        if (hdr->code == LVN_GETDISPINFOW)
        {
            auto* info = reinterpret_cast<NMLVDISPINFOW*>(hdr);
            const auto row = static_cast<size_t>(info->item.iItem);
            if (!(info->item.mask & LVIF_TEXT) || row >= m_items.size() || !info->item.pszText || info->item.cchTextMax <= 0)
                return 0;
            // A wrong pattern plans nothing: the list still shows the names as they are.
            const rename::Planned* p = row < m_plan.items.size() ? &m_plan.items[row] : nullptr;
            std::wstring text;
            switch (info->item.iSubItem)
            {
            case 0: text = FileName(m_items[row].path); break;
            case 1: text = p ? FileName(p->to) : L""; break;
            default: text = !p ? L"" : !p->problem.empty() ? p->problem : p->to == p->from ? L"unchanged" : L""; break;
            }
            wcsncpy_s(info->item.pszText, static_cast<size_t>(info->item.cchTextMax), text.c_str(), _TRUNCATE);
            return 0;
        }
        if (hdr->code == NM_CUSTOMDRAW)
        {
            // Rows whose new name can't be used are drawn in the error colour.
            auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(hdr);
            if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
            if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
            {
                const auto row = static_cast<size_t>(draw->nmcd.dwItemSpec);
                draw->clrText = row < m_plan.items.size() && !m_plan.items[row].problem.empty() ? m_problemColour : m_textColour;
                return CDRF_NEWFONT;
            }
        }
        return 0;
    }

    bool RenameWindow::PreTranslate(MSG& msg)
    {
        if (!m_hwnd || (msg.hwnd != m_hwnd.get() && !IsChild(m_hwnd.get(), msg.hwnd)))
            return false;
        return IsDialogMessageW(m_hwnd.get(), &msg) != FALSE;
    }

    LRESULT CALLBACK RenameWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_NCCREATE)
        {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self = reinterpret_cast<RenameWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self || !self->m_hwnd || self->m_hwnd.get() != hwnd)
            return DefWindowProcW(hwnd, msg, wParam, lParam);   // messages before CreateWindowExW returns
        return self->Handle(msg, wParam, lParam);
    }

    LRESULT RenameWindow::Handle(UINT msg, WPARAM wParam, LPARAM lParam)
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
            const HWND control = reinterpret_cast<HWND>(lParam);
            const bool problem = control == m_status && (!m_plan.error.empty() || m_plan.problems > 0);
            SetTextColor(dc, problem ? m_problemColour : control == m_fields ? GetSysColor(COLOR_GRAYTEXT) : m_textColour);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(m_background.get());
        }
        case WM_CTLCOLOREDIT:
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
            case IDOK: OnRename(); break;
            case IDCANCEL: Close(); break;
            case kIdPattern:
            case kIdFind:
            case kIdReplace:
            case kIdStart:
                if (HIWORD(wParam) == EN_CHANGE && !m_filling)
                {
                    Update();
                    InvalidateRect(m_status, nullptr, TRUE);   // its colour follows the result
                }
                break;
            case kIdMatchCase:
                if (HIWORD(wParam) == BN_CLICKED)
                    Update();
                break;
            default: break;
            }
            return 0;
        case WM_NOTIFY:
        {
            auto* hdr = reinterpret_cast<NMHDR*>(lParam);
            if (hdr->idFrom == kIdList)
                return OnListNotify(hdr);
            return 0;
        }
        case WM_CLOSE:
            Close();
            return 0;
        default:
            break;
        }
        return DefWindowProcW(m_hwnd.get(), msg, wParam, lParam);
    }
}
