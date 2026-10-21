#pragma once
#include "DialogWindow.h"
#include "HistoryStore.h"
#include "HistoryView.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace sd
{
    // "History…" from the tray: the most recent runs, with Undo and Show files. A search box and the
    // Action and When filters over a report-view ListView whose rows show Undone and Failed runs in
    // their own colours; Clear history empties the list (after asking). Standard controls in a
    // DialogWindow.
    class HistoryWindow final : public DialogWindow
    {
    public:
        using Reveal = std::function<void(const std::vector<std::wstring>& paths)>;

        HistoryWindow(HINSTANCE instance, HistoryStore* history, Reveal reveal);

        void Show();      // creates the window or brings it to the front, then refreshes
        void Refresh();   // no-op while the window is closed

    private:
        LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam) override;
        void CreateChildren() override;
        void OnDpiChanged() override;
        void OnThemeChanged() override;
        void Layout() override;
        void FillActions();   // the Action filter's choices, keeping the one picked
        void Fill(int64_t keep);   // the rows the search and filters keep, `keep` (a run id) selected if shown
        history::Filter CurrentFilter() const;
        void UpdateButtons();
        const RunRecord* Selected() const;
        int64_t SelectedId() const;   // 0: none
        void OnUndo();
        void OnShowFiles();
        void OnClear();
        LRESULT OnListNotify(NMHDR* hdr);

        HistoryStore* m_history;
        Reveal m_reveal;

        HWND m_search = nullptr;   // children are destroyed with m_hwnd
        HWND m_actionLabel = nullptr, m_action = nullptr, m_whenLabel = nullptr, m_when = nullptr;
        HWND m_list = nullptr;
        HWND m_undo = nullptr;
        HWND m_show = nullptr;
        HWND m_clear = nullptr;
        HWND m_close = nullptr;
        HWND m_status = nullptr;
        std::vector<RunRecord> m_runs;
        std::vector<size_t> m_shown;          // the list's rows: indices into m_runs
        std::vector<std::wstring> m_actions;  // the Action filter's choices after "All actions"
        bool m_filling = false;               // our own changes to the controls: not the user's
    };
}
