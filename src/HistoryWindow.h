#pragma once
#include "HistoryStore.h"

#include <windows.h>
#include <wil/resource.h>

#include <functional>
#include <string>
#include <vector>

namespace sd
{
    // "History…" from the tray: the most recent runs, with Undo and Show files.
    //
    // A plain top-level window with a report-view ListView and three buttons, laid out by hand in
    // device pixels from the window's DPI. Under per-monitor-v2 Windows scales the frame for us, but
    // child-control fonts and positions are ours to scale, so WM_DPICHANGED rebuilds the font and
    // re-lays out everything.
    class HistoryWindow
    {
    public:
        using Reveal = std::function<void(const std::vector<std::wstring>& paths)>;

        HistoryWindow(HINSTANCE instance, HistoryStore* history, Reveal reveal);
        ~HistoryWindow();
        HistoryWindow(const HistoryWindow&) = delete;
        HistoryWindow& operator=(const HistoryWindow&) = delete;

        void Show();      // creates the window or brings it to the front, then refreshes
        void Refresh();   // no-op while the window is closed
        // Tab / Esc handling for the message loop; true when the message was consumed.
        bool PreTranslate(MSG& msg);

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam);
        void CreateChildren();
        void ApplyDpi(UINT dpi);
        void Layout();
        void UpdateButtons();
        const RunRecord* Selected() const;
        void OnUndo();
        void OnShowFiles();

        HINSTANCE m_instance;
        HistoryStore* m_history;
        Reveal m_reveal;

        wil::unique_hwnd m_hwnd;
        HWND m_list = nullptr;     // children are destroyed with m_hwnd
        HWND m_undo = nullptr;
        HWND m_show = nullptr;
        HWND m_close = nullptr;
        HWND m_status = nullptr;
        wil::unique_hfont m_font;
        UINT m_dpi = 96;
        std::vector<RunRecord> m_runs;
    };
}
