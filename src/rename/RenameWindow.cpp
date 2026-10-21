#include "rename/RenameWindow.h"
#include "Brand.h"
#include "Log.h"
#include "PathUtil.h"

#include <commctrl.h>
#include <windowsx.h>
#include <shlwapi.h>

#include <algorithm>
#include <format>

namespace sd
{
    namespace
    {
        constexpr wchar_t kClass[] = L"" SD_NAME L".Rename";
        constexpr int kIdPattern = 100, kIdFind = 101, kIdReplace = 102, kIdStart = 103, kIdMatchCase = 104, kIdList = 105;
        constexpr int kIdChangeName = 106, kIdChangeWhole = 107;

        // Sizes in DIPs, scaled by the window's DPI in Layout().
        constexpr int kMargin = 14, kGap = 8, kLabelW = 104, kRowH = 26, kRowGap = 6, kHintH = 20;
        constexpr int kButtonW = 110, kButtonH = 30, kStartW = 80, kNameChoiceW = 80;

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

    RenameWindow::RenameWindow(HINSTANCE instance)
        : DialogWindow(instance, { .className = kClass, .width = 640, .height = 572, .minWidth = 420, .minHeight = 412 })
    {
    }

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
            if (!Create(L"" SD_NAME))
                return;
            m_filling = true;
            Button_SetCheck(m_changeName, BST_CHECKED);
            SetWindowTextW(m_pattern, L"{name}");
            SetWindowTextW(m_start, L"1");
            m_filling = false;
        }
        // The pattern and the rest stay as they were last time: renaming batch after batch the same way
        // needs no retyping, and the list shows what it does before anything happens.
        const auto title = m_items.size() == 1 ? std::format(L"Rename {} – " SD_NAME, FileName(m_items[0].path))
                                               : std::format(L"Rename {} items – " SD_NAME, m_items.size());
        SetWindowTextW(m_hwnd, title.c_str());
        Update();
        Present(m_pattern);   // allowed in front: the user just dropped on the slice
        SendMessageW(m_pattern, EM_SETSEL, 0, -1);
        log::Info(L"rename: window for {} item(s)", m_items.size());
    }

    void RenameWindow::CreateChildren()
    {
        // Each label is created just before its control: screen readers name a control after the
        // static text in front of it, and Tab visits the controls in this order.
        const auto label = [&](const wchar_t* text) { return Control(WC_STATICW, text, SS_LEFT | SS_CENTERIMAGE); };
        const auto edit = [&](int controlId, DWORD style = 0) {
            return Control(WC_EDITW, L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | style, controlId);
        };
        m_changeLabel = label(L"Change");
        // WS_GROUP on the first: the arrow keys move between the two, Tab moves on.
        m_changeName = Control(WC_BUTTONW, L"N&ame", WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, kIdChangeName);
        m_changeWhole = Control(WC_BUTTONW, L"Name and e&xtension", BS_AUTORADIOBUTTON, kIdChangeWhole);
        m_patternLabel = Control(WC_STATICW, L"&New name", WS_GROUP | SS_LEFT | SS_CENTERIMAGE);   // ends the radio group
        m_pattern = edit(kIdPattern);
        SendMessageW(m_pattern, EM_LIMITTEXT, 255, 0);
        m_fields = Control(WC_STATICW, kFieldsHint, SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS);
        m_findLabel = label(L"&Find");
        m_find = edit(kIdFind);
        m_replaceLabel = label(L"Re&place with");
        m_replace = edit(kIdReplace);
        m_startLabel = label(L"&Start {n} at");
        m_start = edit(kIdStart, ES_NUMBER);
        SendMessageW(m_start, EM_LIMITTEXT, 6, 0);
        m_matchCase = Control(WC_BUTTONW, L"Find &matches case", WS_TABSTOP | BS_AUTOCHECKBOX, kIdMatchCase);
        m_list = Control(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                         kIdList);
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
        m_status = Control(WC_STATICW, L"", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS | SS_CENTERIMAGE);
        // IDOK and IDCANCEL: IsDialogMessage turns Enter and Esc into them.
        m_rename = Control(WC_BUTTONW, L"&Rename", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
        m_cancel = Control(WC_BUTTONW, L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL);
    }

    void RenameWindow::OnThemeChanged()
    {
        ThemeList(m_list, L"Explorer");
        SetTheme({ m_changeName, m_changeWhole, m_matchCase, m_rename, m_cancel }, L"DarkMode_Explorer", L"Explorer");
        SetTheme({ m_pattern, m_find, m_replace, m_start }, L"DarkMode_CFD");
    }

    COLORREF RenameWindow::StaticTextColor(HWND control) const
    {
        if (control == m_status && (!m_plan.error.empty() || m_plan.problems > 0 || m_plan.warnings > 0))
            return ProblemColor();
        return control == m_fields ? GetSysColor(COLOR_GRAYTEXT) : m_colors.text;
    }

    void RenameWindow::Layout()
    {
        RECT rc{};
        GetClientRect(m_hwnd, &rc);
        const int margin = Px(kMargin), gap = Px(kGap), lw = Px(kLabelW), rh = Px(kRowH), rowGap = Px(kRowGap);
        const int fieldX = margin + lw, fieldW = std::max(0, static_cast<int>(rc.right) - margin - fieldX);
        int y = margin;
        MoveWindow(m_changeLabel, margin, y, lw - gap, rh, TRUE);
        MoveWindow(m_changeName, fieldX, y, Px(kNameChoiceW), rh, TRUE);
        MoveWindow(m_changeWhole, fieldX + Px(kNameChoiceW) + gap, y, std::max(0, fieldW - Px(kNameChoiceW) - gap), rh, TRUE);
        y += rh + rowGap;
        const auto row = [&](HWND labelHwnd, HWND control) {
            MoveWindow(labelHwnd, margin, y, lw - gap, rh, TRUE);
            MoveWindow(control, fieldX, y, fieldW, rh, TRUE);
            y += rh + rowGap;
        };
        row(m_patternLabel, m_pattern);
        MoveWindow(m_fields, fieldX, y - rowGap / 2, fieldW, Px(kHintH), TRUE);
        y += Px(kHintH) + rowGap;
        row(m_findLabel, m_find);
        row(m_replaceLabel, m_replace);
        MoveWindow(m_startLabel, margin, y, lw - gap, rh, TRUE);
        MoveWindow(m_start, fieldX, y, Px(kStartW), rh, TRUE);
        MoveWindow(m_matchCase, fieldX + Px(kStartW) + 2 * gap, y, std::max(0, fieldW - Px(kStartW) - 2 * gap), rh, TRUE);
        y += rh + gap;

        const int bw = Px(kButtonW), bh = Px(kButtonH), buttonsY = rc.bottom - margin - bh;
        const int listW = rc.right - 2 * margin;
        MoveWindow(m_list, margin, y, listW, std::max(0, buttonsY - gap - y), TRUE);
        const int inner = std::max(0, listW - GetSystemMetricsForDpi(SM_CXVSCROLL, m_dpi) - Px(4));
        ListView_SetColumnWidth(m_list, 0, inner * 36 / 100);
        ListView_SetColumnWidth(m_list, 1, inner * 36 / 100);
        ListView_SetColumnWidth(m_list, 2, inner - 2 * (inner * 36 / 100));
        MoveWindow(m_cancel, rc.right - margin - bw, buttonsY, bw, bh, TRUE);
        MoveWindow(m_rename, rc.right - margin - 2 * bw - gap, buttonsY, bw, bh, TRUE);
        MoveWindow(m_status, margin, buttonsY, std::max(0, static_cast<int>(rc.right) - 3 * margin - 2 * bw - gap), bh, TRUE);
    }

    rename::Change RenameWindow::ChangeChosen() const
    {
        return Button_GetCheck(m_changeWhole) == BST_CHECKED ? rename::Change::NameAndExtension : rename::Change::Name;
    }

    void RenameWindow::SetChange(rename::Change change)
    {
        // The pattern follows, so the names stay as they were until the user edits them.
        const auto text = rename::PatternFor(change, Text(m_pattern));
        m_filling = true;
        SetWindowTextW(m_pattern, text.c_str());
        m_filling = false;
        SetWindowTextW(m_patternLabel, change == rename::Change::NameAndExtension ? L"&New file name" : L"&New name");
        Update();
        InvalidateRect(m_status, nullptr, TRUE);
    }

    void RenameWindow::Update()
    {
        rename::Pattern pattern;
        pattern.text = Text(m_pattern);
        pattern.find = Text(m_find);
        pattern.replace = Text(m_replace);
        pattern.matchCase = SendMessageW(m_matchCase, BM_GETCHECK, 0, 0) == BST_CHECKED;
        pattern.change = ChangeChosen();
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
        else if (m_plan.warnings > 0)
        {
            const auto first = std::find_if(m_plan.items.begin(), m_plan.items.end(), [](const rename::Planned& p) { return !p.warning.empty(); });
            status = m_plan.warnings == 1 ? std::format(L"Careful: {}, so it may not open the same way", first->warning)
                                          : std::format(L"Careful: {} extensions change, so those files may not open the same way", m_plan.warnings);
        }
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
        ShowWindow(m_hwnd, SW_HIDE);   // kept for next time; destroyed with the app
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
            default: text = !p ? L"" : !p->problem.empty() ? p->problem : !p->warning.empty() ? p->warning : p->to == p->from ? L"unchanged" : L""; break;
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
                draw->clrText = row < m_plan.items.size() && !m_plan.items[row].problem.empty() ? ProblemColor() : m_colors.text;
                return CDRF_NEWFONT;
            }
        }
        return 0;
    }

    LRESULT RenameWindow::Handle(UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
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
            case kIdChangeName:
            case kIdChangeWhole:
                if (HIWORD(wParam) == BN_CLICKED && !m_filling)
                    SetChange(ChangeChosen());
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
        return DialogWindow::Handle(msg, wParam, lParam);
    }
}
