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

        // Sizes in DIPs (App Windows page), scaled by the window's DPI in Layout().
        constexpr int kMargin = 24, kGap = 8, kControlH = 32, kActionW = 200, kWhenW = 150, kRowH = 58;
        constexpr wchar_t kGlyphUndo = 0xE7A7, kGlyphFolder = 0xE8B7, kGlyphFile = 0xE8A5, kGlyphError = 0xE783, kGlyphDelete = 0xE74D;

        // The When filter, in history::Period order.
        constexpr const wchar_t* kPeriods[] = { L"Any time", L"Today", L"Last 7 days" };

        std::chrono::year_month_day Today()
        {
            SYSTEMTIME t{};
            GetLocalTime(&t);   // RunRecord::started is local time too
            return std::chrono::year_month_day{ std::chrono::year{ t.wYear }, std::chrono::month{ t.wMonth }, std::chrono::day{ t.wDay } };
        }

        history::LocalTime Now()
        {
            SYSTEMTIME t{};
            GetLocalTime(&t);
            return { Today(), t.wHour, t.wMinute };
        }

        // A row's second line: the action and how many files.
        std::wstring Detail(const RunRecord& run)
        {
            if (run.items <= 1)
                return run.recipeLabel;
            if (run.done < run.items)
                return std::format(L"{} \u00B7 {} of {} files", run.recipeLabel, run.done, run.items);
            return std::format(L"{} \u00B7 {} files", run.recipeLabel, run.items);
        }

        int Selection(HWND combo)
        {
            return static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
        }
    }

    HistoryWindow::HistoryWindow(HINSTANCE instance, HistoryStore* history, Reveal reveal)
        // In light mode the classic "dialog" face, which also follows high-contrast themes.
        : DialogWindow(instance, { .className = kClass, .width = 760, .height = 560, .minWidth = 520, .minHeight = 320, .lightBackground = COLOR_BTNFACE,
                                   .kitFont = true }),
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
        // In reading order, which is the Tab order: the search, the filters, the list, the buttons.
        m_heading = Label(L"History", TextRole::Heading, Surface::Window);
        m_search = TextField(kIdSearch, 0, L"Search files and actions", true, Surface::Window);
        Describe(m_search, L"Search files and actions");
        m_action = DropDown(kIdAction, std::span<const std::wstring>{}, Surface::Window);
        Describe(m_action, L"Action", L"Show the runs of one action");
        const std::wstring periods[] = { kPeriods[0], kPeriods[1], kPeriods[2] };
        m_when = DropDown(kIdWhen, periods, Surface::Window);
        Describe(m_when, L"When", L"Show the runs of today or the last 7 days");
        SendMessageW(m_when, CB_SETCURSEL, 0, 0);
        m_clear = KitButton(Look::Button, L"Clear history", kIdClear, Surface::Window, kGlyphDelete);

        // The columns stay for screen readers (each row reads as when, action, files, result).
        m_list = KitList(kIdList, LVS_SINGLESEL | LVS_NOCOLUMNHEADER, { L"When", L"Action", L"Files", L"Result" }, kRowH);
        ListView_EnableGroupView(m_list, TRUE);
        Describe(m_list, L"Runs", L"Enter shows the files");

        m_status = Label(L"", TextRole::Caption, Surface::Window);
        m_undo = KitButton(Look::Button, L"Undo", kIdUndo, Surface::Window, kGlyphUndo);
        m_show = KitButton(Look::Button, L"Show files", kIdShow, Surface::Window, kGlyphFolder);
        m_close = KitButton(Look::Button, L"Close", IDCANCEL, Surface::Window);   // IDCANCEL: IsDialogMessage maps Esc to it
    }

    void HistoryWindow::OnDpiChanged()
    {
        const int widths[] = { 150, 280, 70, 120 };
        for (int i = 0; i < 4; ++i)
            ListView_SetColumnWidth(m_list, i, Px(widths[i]));
    }

    void HistoryWindow::Layout()
    {
        RECT rc{};
        GetClientRect(m_hwnd, &rc);
        const int margin = Px(kMargin), gap = Px(kGap), ch = Px(kControlH);
        const auto place = [](HWND h, int x, int y, int w, int hgt) { MoveWindow(h, x, y, std::max(w, 0), std::max(hgt, 0), TRUE); };
        const auto widthOf = [&](HWND b) { return Px(static_cast<int>(Kit().TextWidth(Text(b), ControlKit::kControlText)) + 52); };

        place(m_heading, margin, Px(14), rc.right - 2 * margin, Px(40));
        // The search takes what the filters and Clear history leave.
        int y = Px(66);
        const int clearW = widthOf(m_clear);
        int x = rc.right - margin - clearW;
        place(m_clear, x, y, clearW, ch);
        x -= gap + Px(kWhenW);
        place(m_when, x, y, Px(kWhenW), ch);
        x -= gap + Px(kActionW);
        place(m_action, x, y, Px(kActionW), ch);
        PlaceField(m_search, { margin, y, std::max(margin + Px(120), x - gap), y + ch });

        // The list on a card, the buttons under it.
        const int barY = rc.bottom - Px(16) - ch;
        y += ch + Px(14);
        m_listCard = { margin, y, rc.right - margin, std::max(y + Px(60), barY - Px(14)) };
        place(m_list, m_listCard.left + Px(1), m_listCard.top + Px(4), m_listCard.right - m_listCard.left - Px(2), m_listCard.bottom - m_listCard.top - Px(8));
        RECT list{};
        GetClientRect(m_list, &list);
        ListView_SetColumnWidth(m_list, 0, list.right);   // one column across the row: the kit draws the rest
        const int closeW = std::max(Px(88), widthOf(m_close) - Px(22));
        x = rc.right - margin - closeW;
        place(m_close, x, barY, closeW, ch);
        const int showW = widthOf(m_show), undoW = widthOf(m_undo);
        x -= gap + showW;
        place(m_show, x, barY, showW, ch);
        x -= gap + undoW;
        place(m_undo, x, barY, undoW, ch);
        place(m_status, margin, barY + (ch - Px(18)) / 2, x - gap - margin, Px(18));
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    void HistoryWindow::PaintSurface(ControlKit& kit, const D2D1_RECT_F&)
    {
        if (m_listCard.right > m_listCard.left)
            kit.DrawCard(DipRect(m_listCard));
    }

    void HistoryWindow::DrawRow(HWND, int row, ControlKit& kit, const D2D1_RECT_F& r, bool)
    {
        if (row < 0 || static_cast<size_t>(row) >= m_shown.size())
            return;
        const auto& c = kit.Theme();
        const auto& run = m_runs[m_shown[static_cast<size_t>(row)]];
        const auto state = history::StateOf(run);
        const bool muted = state.tone == history::Tone::Muted, problem = state.tone == history::Tone::Problem;
        // A file's mark in a rounded square; a failed run's is the error mark.
        const float cy = (r.top + r.bottom) / 2;
        const auto mark = D2D1::RectF(r.left + 16, cy - 16, r.left + 48, cy + 16);
        kit.FillRound(mark, c.control, ControlKit::kControlRadius);
        kit.DrawGlyph(problem ? kGlyphError : muted ? kGlyphUndo : kGlyphFile, 16.f, mark, problem ? c.danger : muted ? c.textDim : c.text);
        // The file and the action at the start; when, and a pill for what isn't plainly done, at the end.
        const std::wstring when = history::WhenText(run.started, m_now);
        const float whenW = kit.TextWidth(when, ControlKit::kSmallText) + 4;
        const bool pill = muted || problem || run.status == status::Partial;
        const float pillW = pill ? kit.PillWidth(state.text) : 0.f;
        const float right = r.right - 16, textRight = right - std::max(whenW, pillW) - 16;
        kit.DrawText(history::FilesText(run), kit.TextFormat(ControlKit::kNavText, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 62, cy - 20, textRight, cy),
                     muted ? c.textDim : c.text);
        kit.DrawText(Detail(run), kit.TextFormat(ControlKit::kSmallText), D2D1::RectF(r.left + 62, cy + 1, textRight, cy + 19), c.textDim);
        kit.DrawText(when, kit.TextFormat(ControlKit::kSmallText, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING),
                     D2D1::RectF(right - whenW, cy - 20, right, cy), c.textDim);
        if (pill)
            kit.DrawPill(D2D1::RectF(right - pillW, cy + 2, right, cy + 20), state.text, problem ? c.danger : muted ? c.textDim : c.warn);
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
        m_now = Now();
        SendMessageW(m_list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(m_list);
        ListView_RemoveAllGroups(m_list);
        int select = m_shown.empty() ? -1 : 0, groupId = 0;
        std::wstring day;
        for (size_t i = 0; i < m_shown.size(); ++i)
        {
            const auto& run = m_runs[m_shown[i]];
            const int row = static_cast<int>(i);
            // Under its day, newest first like the rows.
            if (auto title = history::DayTitle(run.started, m_now.day); groupId == 0 || title != day)
            {
                day = std::move(title);
                LVGROUP g{};
                g.cbSize = sizeof(g);
                g.mask = LVGF_HEADER | LVGF_GROUPID;
                g.pszHeader = day.data();
                g.iGroupId = ++groupId;
                ListView_InsertGroup(m_list, -1, &g);
            }
            auto when = history::WhenText(run.started, m_now);
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_GROUPID;
            item.iItem = row;
            item.iGroupId = groupId;
            item.pszText = when.data();
            ListView_InsertItem(m_list, &item);
            ListView_SetItemText(m_list, row, 1, const_cast<wchar_t*>(run.recipeLabel.c_str()));
            auto files = history::FilesText(run);
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
