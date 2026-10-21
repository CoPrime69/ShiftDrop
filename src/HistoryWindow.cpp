#include "HistoryWindow.h"
#include "Ask.h"
#include "Brand.h"
#include "Log.h"
#include "PathUtil.h"
#include "Undo.h"
#include "Utf.h"

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
        constexpr int kIdSearch = 103;
        constexpr int kIdAction = 104;
        constexpr int kIdWhen = 105;
        constexpr int kIdClear = 106;
        constexpr int kRunLimit = 200;

        // Control sizes in DIPs (1/96 inch); scaled by the window's DPI in Layout().
        constexpr int kMargin = 12, kGap = 8, kButtonW = 96, kButtonH = 28, kClearW = 112;
        constexpr int kFieldH = 24, kLabelW = 46, kActionW = 170, kWhenW = 112, kDropH = 300;

        // The When filter, in history::Period order.
        constexpr const wchar_t* kPeriods[] = { L"Any time", L"Today", L"Last 7 days" };

        std::chrono::year_month_day Today()
        {
            SYSTEMTIME t{};
            GetLocalTime(&t);   // RunRecord::started is local time too
            return std::chrono::year_month_day{ std::chrono::year{ t.wYear }, std::chrono::month{ t.wMonth }, std::chrono::day{ t.wDay } };
        }

        int Selection(HWND combo)
        {
            return static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
        }
    }

    HistoryWindow::HistoryWindow(HINSTANCE instance, HistoryStore* history, Reveal reveal)
        // In light mode the classic "dialog" face, which also follows high-contrast themes.
        : DialogWindow(instance, { .className = kClass, .width = 720, .height = 460, .minWidth = 520, .minHeight = 280, .lightBackground = COLOR_BTNFACE }),
          m_history(history), m_reveal(std::move(reveal))
    {
    }

    void HistoryWindow::Show()
    {
        if (!m_hwnd && !Create(L"" SD_NAME L" history"))
            return;
        Refresh();
        Present(m_list);   // allowed in front: we are handling the user's tray-menu click
    }

    void HistoryWindow::CreateChildren()
    {
        // Each label is created just before its control: screen readers name a control after the
        // static text in front of it, and Tab visits the controls in this order.
        m_search = Control(WC_EDITW, L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, kIdSearch);
        SendMessageW(m_search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search files and actions"));
        m_actionLabel = Control(WC_STATICW, L"&Action", SS_RIGHT | SS_CENTERIMAGE);
        m_action = Control(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, kIdAction);
        m_whenLabel = Control(WC_STATICW, L"&When", SS_RIGHT | SS_CENTERIMAGE);
        m_when = Control(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST, kIdWhen);
        for (const wchar_t* period : kPeriods)
            SendMessageW(m_when, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(period));
        SendMessageW(m_when, CB_SETCURSEL, 0, 0);

        m_list = Control(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, kIdList);
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

        const auto button = [&](const wchar_t* text, int id) { return Control(WC_BUTTONW, text, WS_TABSTOP | BS_PUSHBUTTON, id); };
        m_undo = button(L"&Undo", kIdUndo);
        m_show = button(L"&Show files", kIdShow);
        m_status = Control(WC_STATICW, L"", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS);
        m_clear = button(L"C&lear history", kIdClear);
        m_close = button(L"Close", IDCANCEL);   // IDCANCEL: IsDialogMessage maps Esc to it
    }

    void HistoryWindow::OnDpiChanged()
    {
        const int widths[] = { 150, 280, 70, 120 };
        for (int i = 0; i < 4; ++i)
            ListView_SetColumnWidth(m_list, i, Px(widths[i]));
    }

    void HistoryWindow::OnThemeChanged()
    {
        ThemeList(m_list);
        SetTheme({ m_undo, m_show, m_clear, m_close }, L"DarkMode_Explorer");
        SetTheme({ m_search, m_action, m_when }, L"DarkMode_CFD");
    }

    void HistoryWindow::Layout()
    {
        RECT rc{};
        GetClientRect(m_hwnd, &rc);
        const int margin = Px(kMargin), gap = Px(kGap), bw = Px(kButtonW), bh = Px(kButtonH), fh = Px(kFieldH);
        const int buttonsY = rc.bottom - margin - bh;

        HDWP dwp = BeginDeferWindowPos(11);
        auto place = [&](HWND h, int x, int y, int w, int hgt) {
            if (dwp)
                dwp = DeferWindowPos(dwp, h, nullptr, x, y, std::max(w, 0), std::max(hgt, 0), SWP_NOZORDER | SWP_NOACTIVATE);
        };
        // Top row: the search box takes what the two filters leave. A drop-down list's height is
        // its open list's; the closed box keeps the font's height.
        const int whenX = rc.right - margin - Px(kWhenW), whenLabelX = whenX - gap - Px(kLabelW);
        const int actionX = whenLabelX - gap - Px(kActionW), actionLabelX = actionX - gap - Px(kLabelW);
        place(m_search, margin, margin, actionLabelX - gap - margin, fh);
        place(m_actionLabel, actionLabelX, margin, Px(kLabelW), fh);
        place(m_action, actionX, margin, Px(kActionW), Px(kDropH));
        place(m_whenLabel, whenLabelX, margin, Px(kLabelW), fh);
        place(m_when, whenX, margin, Px(kWhenW), Px(kDropH));

        const int listY = margin + fh + gap;
        place(m_list, margin, listY, rc.right - 2 * margin, buttonsY - gap - listY);
        place(m_undo, margin, buttonsY, bw, bh);
        place(m_show, margin + bw + gap, buttonsY, bw, bh);
        place(m_close, rc.right - margin - bw, buttonsY, bw, bh);
        const int clearX = rc.right - margin - bw - gap - Px(kClearW);
        place(m_clear, clearX, buttonsY, Px(kClearW), bh);
        const int statusX = margin + 2 * (bw + gap);
        place(m_status, statusX, buttonsY + (bh - Px(16)) / 2, clearX - gap - statusX, Px(18));
        if (dwp)
            EndDeferWindowPos(dwp);
    }

    void HistoryWindow::Refresh()
    {
        if (!m_hwnd || !m_history)
            return;
        const int64_t keep = SelectedId();   // before m_runs is replaced
        m_runs = m_history->RecentRuns(kRunLimit);
        FillActions();
        Fill(keep);
    }

    void HistoryWindow::FillActions()
    {
        const int picked = Selection(m_action);
        const std::wstring keep = picked > 0 && static_cast<size_t>(picked) <= m_actions.size() ? m_actions[static_cast<size_t>(picked) - 1] : L"";
        m_actions = history::Actions(m_runs);
        // An action picked before keeps its place in the list while the filter is on it, even once
        // no run of it is left (after Clear history): the filter then shows nothing, as it says.
        if (!keep.empty() && std::find(m_actions.begin(), m_actions.end(), keep) == m_actions.end())
            m_actions.push_back(keep);
        m_filling = true;
        SendMessageW(m_action, CB_RESETCONTENT, 0, 0);
        SendMessageW(m_action, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All actions"));
        int select = 0;
        for (size_t i = 0; i < m_actions.size(); ++i)
        {
            SendMessageW(m_action, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(m_actions[i].c_str()));
            if (m_actions[i] == keep)
                select = static_cast<int>(i) + 1;
        }
        SendMessageW(m_action, CB_SETCURSEL, select, 0);
        m_filling = false;
    }

    history::Filter HistoryWindow::CurrentFilter() const
    {
        history::Filter filter;
        filter.search = Text(m_search);
        const int action = Selection(m_action);
        if (action > 0 && static_cast<size_t>(action) <= m_actions.size())
            filter.action = m_actions[static_cast<size_t>(action) - 1];
        const int when = Selection(m_when);
        filter.period = when == 1 ? history::Period::Today : when == 2 ? history::Period::Week : history::Period::All;
        return filter;
    }

    void HistoryWindow::Fill(int64_t keep)
    {
        const auto filter = CurrentFilter();
        m_shown = history::Apply(m_runs, filter, Today());
        SendMessageW(m_list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(m_list);
        int select = m_shown.empty() ? -1 : 0;
        for (size_t i = 0; i < m_shown.size(); ++i)
        {
            const auto& run = m_runs[m_shown[i]];
            const int row = static_cast<int>(i);
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = row;
            item.pszText = const_cast<wchar_t*>(run.started.c_str());
            ListView_InsertItem(m_list, &item);
            ListView_SetItemText(m_list, row, 1, const_cast<wchar_t*>(run.recipeLabel.c_str()));
            auto files = std::format(L"{} / {}", run.done, run.items);
            ListView_SetItemText(m_list, row, 2, files.data());
            auto result = history::StateOf(run).text;
            ListView_SetItemText(m_list, row, 3, result.data());
            if (run.id == keep)
                select = row;
        }
        if (select >= 0)
            ListView_SetItemState(m_list, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        SendMessageW(m_list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(m_list, nullptr, TRUE);

        std::wstring status;
        if (m_runs.empty())
            status = L"Nothing here yet: every file job shows up here.";
        else if (m_shown.empty())
            status = L"No run matches the search and filters.";
        else if (m_shown.size() < m_runs.size())
            status = std::format(L"{} of {} runs", m_shown.size(), m_runs.size());
        SetWindowTextW(m_status, status.c_str());
        UpdateButtons();
    }

    int64_t HistoryWindow::SelectedId() const
    {
        const auto* run = Selected();
        return run ? run->id : 0;
    }

    const RunRecord* HistoryWindow::Selected() const
    {
        if (!m_list)
            return nullptr;
        const int row = ListView_GetNextItem(m_list, -1, LVNI_SELECTED);
        return row >= 0 && static_cast<size_t>(row) < m_shown.size() ? &m_runs[m_shown[static_cast<size_t>(row)]] : nullptr;
    }

    void HistoryWindow::UpdateButtons()
    {
        const auto* run = Selected();
        EnableWindow(m_undo, run && history::CanUndo(*run));
        EnableWindow(m_show, run != nullptr);
        EnableWindow(m_clear, !m_runs.empty());
    }

    void HistoryWindow::OnUndo()
    {
        const auto* run = Selected();
        if (!run || !history::CanUndo(*run))
            return;
        // Copy: Refresh() below replaces m_runs.
        const RunRecord target = *run;
        // Undo re-hashes every output here, on the UI thread.
        const auto report = UndoRun(*m_history, target, m_hwnd);
        Refresh();
        SetWindowTextW(m_status, report.message.c_str());
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
            if (item.status == status::Done && !item.output.empty() && PathExists(item.output))
                outputs.push_back(item.output);
            else if (PathExists(item.source))
                sources.push_back(item.source);
        }
        if (!outputs.empty())
            m_reveal(outputs);
        else if (!sources.empty())
            m_reveal(sources);
        else
            SetWindowTextW(m_status, L"Those files are no longer there.");
    }

    void HistoryWindow::OnClear()
    {
        if (!m_history || m_runs.empty())
            return;
        ask::Question q;
        q.title = L"" SD_NAME L" history";
        q.heading = L"Clear history?";
        q.text = L"Only the list is cleared: files " SD_NAME L" made stay where they are and nothing is deleted. "
                 L"Cleared runs can no longer be undone from History.";
        q.choices = { { 1, L"Clear history", L"Remove every finished run from this list" } };
        if (ask::ChooseNow(m_hwnd, q) != 1)   // modal over the window
            return;
        const int cleared = m_history->Clear();
        Refresh();
        const auto message = cleared == 1 ? std::wstring(L"Cleared 1 run. No file was changed.")
                                          : std::format(L"Cleared {} runs. No file was changed.", cleared);
        SetWindowTextW(m_status, message.c_str());
        SetFocus(m_search);   // the buttons went grey; keep the focus somewhere that works
    }

    LRESULT HistoryWindow::OnListNotify(NMHDR* hdr)
    {
        switch (hdr->code)
        {
        case LVN_ITEMCHANGED:
            UpdateButtons();
            return 0;
        case NM_DBLCLK:
        case NM_RETURN:
            OnShowFiles();
            return 0;
        case NM_CUSTOMDRAW:
        {
            // Undone runs in a muted colour and failed ones in the error colour, the whole row.
            auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(hdr);
            if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
            if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
            {
                const auto row = static_cast<size_t>(draw->nmcd.dwItemSpec);
                const auto tone = row < m_shown.size() ? history::StateOf(m_runs[m_shown[row]]).tone : history::Tone::Normal;
                draw->clrText = tone == history::Tone::Problem ? ProblemColor() : tone == history::Tone::Muted ? MutedColor() : m_colors.text;
                return CDRF_NEWFONT;
            }
            return CDRF_DODEFAULT;
        }
        default:
            return 0;
        }
    }

    LRESULT HistoryWindow::Handle(UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case kIdUndo: OnUndo(); break;
            case kIdShow: OnShowFiles(); break;
            case kIdClear: OnClear(); break;
            case kIdSearch:
                if (HIWORD(wParam) == EN_CHANGE)
                    Fill(SelectedId());   // as the user types
                break;
            case kIdAction:
            case kIdWhen:
                if (HIWORD(wParam) == CBN_SELCHANGE && !m_filling)
                    Fill(SelectedId());
                break;
            case IDCANCEL: ShowWindow(m_hwnd, SW_HIDE); break;
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
            // Hide instead of destroying: the next "History…" opens instantly with the same size and place.
            ShowWindow(m_hwnd, SW_HIDE);
            return 0;
        default:
            return DialogWindow::Handle(msg, wParam, lParam);
        }
    }
}
