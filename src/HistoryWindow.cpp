#include "HistoryWindow.h"
#include "Brand.h"
#include "Log.h"
#include "Undo.h"
#include "Utf.h"
#include "resource.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <format>

namespace sd
{
    namespace
    {
        constexpr wchar_t kClass[] = L"" SD_NAME L".History";
        constexpr int kIdList = 100;
        constexpr int kIdUndo = 101;
        constexpr int kIdShow = 102;
        constexpr int kRunLimit = 200;

        // Window and control sizes in DIPs (1/96 inch); scaled by the window's DPI in Layout().
        constexpr int kWidth = 680, kHeight = 440, kMinWidth = 420, kMinHeight = 260;
        constexpr int kMargin = 12, kGap = 8, kButtonW = 96, kButtonH = 28;

        const wchar_t* StatusText(const std::string& s)
        {
            if (s == status::Done) return L"Done";
            if (s == status::Partial) return L"Partly done";
            if (s == status::Failed) return L"Failed";
            if (s == status::Undone) return L"Undone";
            if (s == status::Interrupted) return L"Interrupted";
            if (s == status::Running) return L"Running";
            return L"";
        }

        bool CanUndo(const RunRecord& run)
        {
            return (run.status == status::Done || run.status == status::Partial) && run.done > 0;
        }
    }

    HistoryWindow::HistoryWindow(HINSTANCE instance, HistoryStore* history, Reveal reveal)
        : m_instance(instance), m_history(history), m_reveal(std::move(reveal))
    {
    }

    HistoryWindow::~HistoryWindow() = default;

    void HistoryWindow::Show()
    {
        if (!m_hwnd)
        {
            static const bool registered = [this] {
                WNDCLASSEXW wc{ sizeof(wc) };
                wc.lpfnWndProc = WndProc;
                wc.hInstance = m_instance;
                wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                // COLOR_BTNFACE + 1: the classic "dialog" background, which also follows high-contrast themes.
                wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
                wc.hIcon = LoadIconW(m_instance, MAKEINTRESOURCEW(IDI_APPICON));
                wc.lpszClassName = kClass;
                return RegisterClassExW(&wc) != 0;
            }();
            if (!registered)
                return;
            // WS_EX_CONTROLPARENT lets IsDialogMessage move focus with Tab between our child controls.
            m_hwnd.reset(CreateWindowExW(WS_EX_CONTROLPARENT, kClass, L"" SD_NAME L" history", WS_OVERLAPPEDWINDOW,
                                         CW_USEDEFAULT, CW_USEDEFAULT, kWidth, kHeight, nullptr, nullptr, m_instance, this));
            if (!m_hwnd)
                return;
            CreateChildren();
            // Size the client area for this monitor's DPI; CreateWindow took the size in physical pixels.
            const UINT dpi = GetDpiForWindow(m_hwnd.get());
            RECT r{ 0, 0, MulDiv(kWidth, static_cast<int>(dpi), 96), MulDiv(kHeight, static_cast<int>(dpi), 96) };
            AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_CONTROLPARENT, dpi);
            SetWindowPos(m_hwnd.get(), nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            ApplyDpi(dpi);
        }
        Refresh();
        ShowWindow(m_hwnd.get(), IsIconic(m_hwnd.get()) ? SW_RESTORE : SW_SHOWNORMAL);
        SetForegroundWindow(m_hwnd.get());   // allowed: we are handling the user's tray-menu click
        SetFocus(m_list);
    }

    void HistoryWindow::CreateChildren()
    {
        const HWND parent = m_hwnd.get();
        m_list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                 0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdList)), m_instance, nullptr);
        ListView_SetExtendedListViewStyle(m_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        const wchar_t* titles[] = { L"When", L"Action", L"Files", L"Result" };
        for (int i = 0; i < 4; ++i)
        {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_SUBITEM;
            col.pszText = const_cast<wchar_t*>(titles[i]);
            col.iSubItem = i;
            ListView_InsertColumn(m_list, i, &col);
        }

        auto button = [&](const wchar_t* text, int id, DWORD style) {
            return CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 0, 0, parent,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), m_instance, nullptr);
        };
        m_undo = button(L"&Undo", kIdUndo, BS_PUSHBUTTON);
        m_show = button(L"&Show files", kIdShow, BS_PUSHBUTTON);
        m_close = button(L"Close", IDCANCEL, BS_PUSHBUTTON);   // IDCANCEL: IsDialogMessage maps Esc to it
        m_status = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS,
                                   0, 0, 0, 0, parent, nullptr, m_instance, nullptr);
    }

    void HistoryWindow::ApplyDpi(UINT dpi)
    {
        m_dpi = dpi;
        // The message font ("Segoe UI 9pt") as the user configured it, at this window's DPI.
        NONCLIENTMETRICSW metrics{ sizeof(metrics) };
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi))
            m_font.reset(CreateFontIndirectW(&metrics.lfMessageFont));
        for (HWND child : { m_list, m_undo, m_show, m_close, m_status })
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(m_font.get()), TRUE);

        const int widths[] = { 150, 260, 70, 110 };
        for (int i = 0; i < 4; ++i)
            ListView_SetColumnWidth(m_list, i, MulDiv(widths[i], static_cast<int>(dpi), 96));
        Layout();
    }

    void HistoryWindow::Layout()
    {
        RECT rc{};
        GetClientRect(m_hwnd.get(), &rc);
        auto px = [this](int dip) { return MulDiv(dip, static_cast<int>(m_dpi), 96); };
        const int margin = px(kMargin), gap = px(kGap), bw = px(kButtonW), bh = px(kButtonH);
        const int buttonsY = rc.bottom - margin - bh;

        HDWP dwp = BeginDeferWindowPos(5);
        auto place = [&](HWND h, int x, int y, int w, int hgt) {
            if (dwp)
                dwp = DeferWindowPos(dwp, h, nullptr, x, y, std::max(w, 0), std::max(hgt, 0), SWP_NOZORDER | SWP_NOACTIVATE);
        };
        place(m_list, margin, margin, rc.right - 2 * margin, buttonsY - gap - margin);
        place(m_undo, margin, buttonsY, bw, bh);
        place(m_show, margin + bw + gap, buttonsY, bw, bh);
        place(m_close, rc.right - margin - bw, buttonsY, bw, bh);
        const int statusX = margin + 2 * (bw + gap);
        place(m_status, statusX, buttonsY + (bh - px(16)) / 2, rc.right - margin - bw - gap - statusX, px(18));
        if (dwp)
            EndDeferWindowPos(dwp);
    }

    void HistoryWindow::Refresh()
    {
        if (!m_hwnd || !m_history)
            return;
        const auto* before = Selected();
        const int64_t keep = before ? before->id : 0;

        m_runs = m_history->RecentRuns(kRunLimit);
        SendMessageW(m_list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(m_list);
        int select = m_runs.empty() ? -1 : 0;
        for (size_t i = 0; i < m_runs.size(); ++i)
        {
            const auto& run = m_runs[i];
            const int row = static_cast<int>(i);
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = row;
            item.pszText = const_cast<wchar_t*>(run.started.c_str());
            item.lParam = static_cast<LPARAM>(i);
            ListView_InsertItem(m_list, &item);
            ListView_SetItemText(m_list, row, 1, const_cast<wchar_t*>(run.recipeLabel.c_str()));
            auto files = std::format(L"{} / {}", run.done, run.items);
            ListView_SetItemText(m_list, row, 2, files.data());
            ListView_SetItemText(m_list, row, 3, const_cast<wchar_t*>(StatusText(run.status)));
            if (run.id == keep)
                select = row;
        }
        if (select >= 0)
            ListView_SetItemState(m_list, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        SendMessageW(m_list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(m_list, nullptr, TRUE);
        UpdateButtons();
    }

    const RunRecord* HistoryWindow::Selected() const
    {
        if (!m_list)
            return nullptr;
        const int row = ListView_GetNextItem(m_list, -1, LVNI_SELECTED);
        return row >= 0 && static_cast<size_t>(row) < m_runs.size() ? &m_runs[static_cast<size_t>(row)] : nullptr;
    }

    void HistoryWindow::UpdateButtons()
    {
        const auto* run = Selected();
        EnableWindow(m_undo, run && CanUndo(*run));
        EnableWindow(m_show, run != nullptr);
    }

    void HistoryWindow::OnUndo()
    {
        const auto* run = Selected();
        if (!run || !CanUndo(*run))
            return;
        // Copy: Refresh() below replaces m_runs.
        const RunRecord target = *run;
        // Undo re-hashes every output here, on the UI thread.
        const auto report = UndoRun(*m_history, target, m_hwnd.get());
        SetWindowTextW(m_status, report.message.c_str());
        Refresh();
    }

    void HistoryWindow::OnShowFiles()
    {
        const auto* run = Selected();
        if (!run || !m_reveal)
            return;
        // Outputs that still exist; for undone or failed runs, the sources instead.
        std::vector<std::wstring> outputs, sources;
        for (const auto& item : m_history->Items(run->id))
        {
            if (item.status == status::Done && !item.output.empty() && GetFileAttributesW(item.output.c_str()) != INVALID_FILE_ATTRIBUTES)
                outputs.push_back(item.output);
            else if (GetFileAttributesW(item.source.c_str()) != INVALID_FILE_ATTRIBUTES)
                sources.push_back(item.source);
        }
        if (!outputs.empty())
            m_reveal(outputs);
        else if (!sources.empty())
            m_reveal(sources);
        else
            SetWindowTextW(m_status, L"Those files are no longer there.");
    }

    bool HistoryWindow::PreTranslate(MSG& msg)
    {
        if (!m_hwnd || (msg.hwnd != m_hwnd.get() && !IsChild(m_hwnd.get(), msg.hwnd)))
            return false;
        return IsDialogMessageW(m_hwnd.get(), &msg) != FALSE;
    }

    LRESULT CALLBACK HistoryWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_NCCREATE)
        {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self = reinterpret_cast<HistoryWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self || !self->m_hwnd || self->m_hwnd.get() != hwnd)
            return DefWindowProcW(hwnd, msg, wParam, lParam);   // messages before CreateWindowExW returns
        return self->Handle(msg, wParam, lParam);
    }

    LRESULT HistoryWindow::Handle(UINT msg, WPARAM wParam, LPARAM lParam)
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
            // The window moved to a monitor with another scale. lParam is the size Windows suggests at the new DPI.
            const auto* r = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(m_hwnd.get(), nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            ApplyDpi(HIWORD(wParam));
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case kIdUndo: OnUndo(); break;
            case kIdShow: OnShowFiles(); break;
            case IDCANCEL: ShowWindow(m_hwnd.get(), SW_HIDE); break;
            default: break;
            }
            return 0;
        case WM_NOTIFY:
        {
            const auto* hdr = reinterpret_cast<const NMHDR*>(lParam);
            if (hdr->idFrom == kIdList && hdr->code == LVN_ITEMCHANGED)
                UpdateButtons();
            else if (hdr->idFrom == kIdList && (hdr->code == NM_DBLCLK || hdr->code == NM_RETURN))
                OnShowFiles();
            return 0;
        }
        case WM_CLOSE:
            // Hide instead of destroying: the next "History…" opens instantly with the same size and place.
            ShowWindow(m_hwnd.get(), SW_HIDE);
            return 0;
        default:
            return DefWindowProcW(m_hwnd.get(), msg, wParam, lParam);
        }
    }
}
