#pragma once
#include "FileMove.h"
#include "rename/RenamePattern.h"

#include <windows.h>
#include <wil/resource.h>

#include <functional>
#include <string>
#include <vector>

namespace sd
{
    // File actions › Rename…: the pattern, Find and replace, where {n} starts, and a live list of every
    // file's new name. A name that can't be used (taken, the same for two files, invalid) is marked in
    // the list, and Rename stays off until every name works, so nothing is renamed halfway or
    // overwritten. Rename hands the names to `onConfirm` (main.cpp renames them on the shell thread);
    // Cancel or Esc closes it. Files are listed (and numbered) by name, the way Explorer sorts them.
    // Standard controls only, so screen readers read it as it is; light or dark like Settings.
    class RenameWindow
    {
    public:
        using Confirm = std::function<void(std::vector<Rename> renames)>;

        explicit RenameWindow(HINSTANCE instance);
        ~RenameWindow();
        RenameWindow(const RenameWindow&) = delete;
        RenameWindow& operator=(const RenameWindow&) = delete;

        // Opens the window for `files` (or brings it back with them instead of what it showed).
        void Show(std::vector<std::wstring> files, Confirm onConfirm);
        bool PreTranslate(MSG& msg);   // Tab, Enter and Esc for the message loop; true when consumed

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam);
        void CreateChildren();
        void ApplyDpi(UINT dpi);
        void ApplyTheme();
        void Layout();
        void Update();                 // a field changed: plan again and refresh the list
        void OnRename();
        void Close();
        LRESULT OnListNotify(NMHDR* hdr);
        std::wstring Text(HWND edit) const;

        HINSTANCE m_instance;
        Confirm m_onConfirm;
        std::vector<rename::Item> m_items;
        rename::Plan m_plan;

        wil::unique_hwnd m_hwnd;
        HWND m_patternLabel = nullptr, m_pattern = nullptr, m_fields = nullptr;   // children are destroyed with m_hwnd
        HWND m_findLabel = nullptr, m_find = nullptr, m_replaceLabel = nullptr, m_replace = nullptr;
        HWND m_startLabel = nullptr, m_start = nullptr, m_matchCase = nullptr;
        HWND m_list = nullptr, m_status = nullptr, m_rename = nullptr, m_cancel = nullptr;
        wil::unique_hfont m_font;
        wil::unique_hbrush m_background, m_field;
        COLORREF m_textColour = 0, m_fieldColour = 0, m_problemColour = 0;
        bool m_dark = false;
        UINT m_dpi = 96;
        bool m_filling = false;   // our own SetWindowText calls: not the user's
    };
}
