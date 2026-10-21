#pragma once
#include "DialogWindow.h"
#include "HistoryStore.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace sd
{
    // "History…" from the tray: the most recent runs, with Undo and Show files. A report-view
    // ListView and three buttons in a DialogWindow.
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
        void UpdateButtons();
        const RunRecord* Selected() const;
        void OnUndo();
        void OnShowFiles();

        HistoryStore* m_history;
        Reveal m_reveal;

        HWND m_list = nullptr;     // children are destroyed with m_hwnd
        HWND m_undo = nullptr;
        HWND m_show = nullptr;
        HWND m_close = nullptr;
        HWND m_status = nullptr;
        std::vector<RunRecord> m_runs;
    };
}
