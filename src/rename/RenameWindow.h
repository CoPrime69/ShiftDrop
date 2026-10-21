#pragma once
#include "DialogWindow.h"
#include "FileMove.h"
#include "rename/RenamePattern.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace sd
{
    // File actions › Rename…: what changes (the name, or the name and extension), the pattern, Find
    // and replace, where {n} starts, and a live list of every file's new name. A name that can't be used (taken, the same for two files, invalid) is marked in
    // the list, and Rename stays off until every name works, so nothing is renamed halfway or
    // overwritten. Rename hands the names to `onConfirm` (main.cpp renames them on the shell thread);
    // Cancel or Esc closes it. Files are listed (and numbered) by name, the way Explorer sorts them.
    // Standard controls in a DialogWindow, like Settings.
    class RenameWindow final : public DialogWindow
    {
    public:
        using Confirm = std::function<void(std::vector<Rename> renames)>;

        explicit RenameWindow(HINSTANCE instance);

        // Opens the window for `files` (or brings it back with them instead of what it showed).
        void Show(std::vector<std::wstring> files, Confirm onConfirm);

    private:
        LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam) override;
        void CreateChildren() override;
        void OnThemeChanged() override;
        COLORREF StaticTextColor(HWND control) const override;
        void Layout() override;
        void Update();                 // a field changed: plan again and refresh the list
        void OnRename();
        void Close();
        LRESULT OnListNotify(NMHDR* hdr);
        rename::Change ChangeChosen() const;
        void SetChange(rename::Change change);   // the user picked Name or Name and extension

        Confirm m_onConfirm;
        std::vector<rename::Item> m_items;
        rename::Plan m_plan;

        HWND m_changeLabel = nullptr, m_changeName = nullptr, m_changeWhole = nullptr;   // children are destroyed with m_hwnd
        HWND m_patternLabel = nullptr, m_pattern = nullptr, m_fields = nullptr;
        HWND m_findLabel = nullptr, m_find = nullptr, m_replaceLabel = nullptr, m_replace = nullptr;
        HWND m_startLabel = nullptr, m_start = nullptr, m_matchCase = nullptr;
        HWND m_list = nullptr, m_status = nullptr, m_rename = nullptr, m_cancel = nullptr;
        bool m_filling = false;   // our own SetWindowText calls: not the user's
    };
}
