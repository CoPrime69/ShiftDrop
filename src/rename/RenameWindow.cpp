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

        constexpr int kIdField = 120;   // + the field's place in kFields

        // Sizes in DIPs (App Windows page), scaled by the window's DPI in Layout().
        constexpr int kMargin = 24, kGap = 8, kControlH = 32, kLabelH = 20, kRowH = 36, kStartW = 110;

        // The fields a pattern can hold, as buttons that type them in.
        constexpr const wchar_t* kFields[] = { L"{name}", L"{n}", L"{date}", L"{ext}", L"{folder}" };
        constexpr const wchar_t* kFieldNames[] = { L"the name", L"a number, {n:000} for three digits", L"the modified date", L"the extension",
                                                   L"the folder's name" };

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
        : DialogWindow(instance, { .className = kClass, .width = 680, .height = 640, .minWidth = 480, .minHeight = 500, .kitFont = true })
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
        const auto title = m_items.size() == 1 ? std::format(L"Rename {} \u2013 " SD_NAME, FileName(m_items[0].path))
                                               : std::format(L"Rename {} items \u2013 " SD_NAME, m_items.size());
        SetWindowTextW(m_hwnd, title.c_str());
        SetWindowTextW(m_heading, m_items.size() == 1 ? std::format(L"Rename {}", FileName(m_items[0].path)).c_str()
                                                       : std::format(L"Rename {} files", m_items.size()).c_str());
        Update();
        Present(m_pattern);   // allowed in front: the user just dropped on the slice
        SendMessageW(m_pattern, EM_SETSEL, 0, -1);
        log::Info(L"rename: window for {} item(s)", m_items.size());
    }

    void RenameWindow::CreateChildren()
    {
        // Each label just before its control (screen readers name a control after the text in
        // front of it), in reading order, which is the Tab order.
        m_heading = Label(L"", TextRole::Question, Surface::Window);
        m_patternLabel = Label(L"New name", TextRole::Strong, Surface::Window);
        m_pattern = TextField(kIdPattern, 0, L"{name}", false, Surface::Window);
        SendMessageW(m_pattern, EM_LIMITTEXT, 255, 0);
        Describe(m_pattern, L"New name", L"The pattern every new name is made from");
        for (size_t i = 0; i < std::size(kFields); ++i)
        {
            const HWND b = KitButton(Look::Button, kFields[i], kIdField + static_cast<int>(i), Surface::Window, 0, i == 0);
            Describe(b, std::format(L"Add {}", kFields[i]), std::format(L"Types {} into the new name: {}", kFields[i], kFieldNames[i]));
            m_fields.push_back(b);
        }
        m_findLabel = Label(L"Find", TextRole::Strong, Surface::Window);
        m_find = TextField(kIdFind, 0, L"Text to find", false, Surface::Window);
        m_replaceLabel = Label(L"Replace with", TextRole::Strong, Surface::Window);
        m_replace = TextField(kIdReplace, 0, L"Nothing", false, Surface::Window);
        m_startLabel = Label(L"Start {n} at", TextRole::Strong, Surface::Window);
        m_start = TextField(kIdStart, ES_NUMBER, L"1", false, Surface::Window);
        SendMessageW(m_start, EM_LIMITTEXT, 6, 0);
        m_changeLabel = Label(L"Change", TextRole::Strong, Surface::Window);
        m_changeName = KitButton(Look::Segment, L"Name", kIdChangeName, Surface::Window, 0, true);
        m_changeWhole = KitButton(Look::Segment, L"Name and extension", kIdChangeWhole, Surface::Window, 0, false);
        StyleOf(m_changeName).part = ControlKit::Part::First;
        StyleOf(m_changeWhole).part = ControlKit::Part::Last;
        Describe(m_changeName, L"Change: Name");
        Describe(m_changeWhole, L"Change: Name and extension");
        m_matchCase = KitButton(Look::Chip, L"Match case", kIdMatchCase, Surface::Window);
        Describe(m_matchCase, L"Find matches case");
        m_previewLabel = Label(L"Preview", TextRole::Group, Surface::Window);
        m_count = Label(L"", TextRole::Caption, Surface::Window);
        m_list = KitList(kIdList, LVS_OWNERDATA | LVS_SINGLESEL | LVS_NOCOLUMNHEADER | LVS_NOSORTHEADER, { L"Name now", L"New name", L"Note" }, kRowH);
        Describe(m_list, L"Preview", L"Every file's name now and its new name");
        m_status = Label(L"", TextRole::Caption, Surface::Window);
        // IDOK and IDCANCEL: IsDialogMessage turns Enter and Esc into them.
        m_cancel = KitButton(Look::Button, L"Cancel", IDCANCEL, Surface::Window);
        m_rename = KitButton(Look::Primary, L"Rename", IDOK, Surface::Window);
    }

    void RenameWindow::InsertField(const wchar_t* field)
    {
        SetFocus(m_pattern);
        SendMessageW(m_pattern, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(field));   // an EN_CHANGE plans again
    }

    void RenameWindow::Layout()
    {
        RECT rc{};
        GetClientRect(m_hwnd, &rc);
        const int margin = Px(kMargin), gap = Px(kGap), ch = Px(kControlH), lh = Px(kLabelH);
        const int width = std::max(0, static_cast<int>(rc.right) - 2 * margin);
        const auto place = [](HWND h, int x, int y, int w, int hgt) { MoveWindow(h, x, y, std::max(w, 0), std::max(hgt, 0), TRUE); };
        const auto field = [&](HWND edit, int x, int y, int w) { PlaceField(edit, { x, y, x + std::max(w, 0), y + ch }); };
        int y = Px(16);
        place(m_heading, margin, y, width, Px(30));
        y += Px(42);
        // The pattern, and the fields it can hold under it.
        place(m_patternLabel, margin, y, width, lh);
        y += lh + Px(4);
        field(m_pattern, margin, y, width);
        y += ch + gap;
        int x = margin;
        for (HWND b : m_fields)
        {
            const int w = Px(static_cast<int>(Kit().TextWidth(Text(b), ControlKit::kControlText)) + 24);
            place(b, x, y, w, Px(28));
            x += w + Px(6);
        }
        y += Px(28) + Px(16);
        // Find and Replace with side by side.
        const int half = (width - Px(16)) / 2;
        place(m_findLabel, margin, y, half, lh);
        place(m_replaceLabel, margin + half + Px(16), y, half, lh);
        y += lh + Px(4);
        field(m_find, margin, y, half);
        field(m_replace, margin + half + Px(16), y, half);
        y += ch + Px(16);
        // Where {n} starts, what changes, and whether Find matches case.
        place(m_startLabel, margin, y, Px(kStartW), lh);
        place(m_changeLabel, margin + Px(kStartW) + Px(16), y, Px(200), lh);
        y += lh + Px(4);
        field(m_start, margin, y, Px(kStartW));
        x = margin + Px(kStartW) + Px(16);
        for (HWND seg : { m_changeName, m_changeWhole })
        {
            const int w = Px(static_cast<int>(Kit().SegmentWidth(Text(seg))));
            place(seg, x, y, w, ch + Px(4));
            x += w;
        }
        const int chipW = Px(static_cast<int>(Kit().ChipWidth(Text(m_matchCase))));
        place(m_matchCase, std::max(x + Px(16), static_cast<int>(rc.right) - margin - chipW), y + Px(2), chipW, ch);
        y += ch + Px(4) + Px(20);
        // The preview: its count at the end of its heading, the list on a card.
        place(m_previewLabel, margin, y, half, Px(22));
        place(m_count, margin + half, y + Px(2), width - half, Px(18));
        y += Px(22) + Px(8);
        const int barY = rc.bottom - Px(16) - ch;
        m_listCard = { margin, y, rc.right - margin, std::max(y + Px(60), barY - Px(14)) };
        place(m_list, m_listCard.left + Px(1), m_listCard.top + Px(4), m_listCard.right - m_listCard.left - Px(2), m_listCard.bottom - m_listCard.top - Px(8));
        RECT list{};
        GetClientRect(m_list, &list);
        ListView_SetColumnWidth(m_list, 0, list.right);   // one column across the row: the kit draws the rest
        const int renameW = std::max(Px(120), Px(static_cast<int>(Kit().TextWidth(Text(m_rename), ControlKit::kControlText)) + 36));
        place(m_rename, rc.right - margin - renameW, barY, renameW, ch);
        place(m_cancel, rc.right - margin - renameW - gap - Px(96), barY, Px(96), ch);
        place(m_status, margin, barY + (ch - Px(18)) / 2, rc.right - 2 * margin - renameW - gap - Px(96) - gap, Px(18));
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    void RenameWindow::PaintSurface(ControlKit& kit, const D2D1_RECT_F&)
    {
        if (m_listCard.right > m_listCard.left)
            kit.DrawCard(DipRect(m_listCard));
    }

    void RenameWindow::DrawRow(HWND, int row, ControlKit& kit, const D2D1_RECT_F& r, bool)
    {
        if (row < 0 || static_cast<size_t>(row) >= m_items.size())
            return;
        // A wrong pattern plans nothing: the list still shows the names as they are.
        const auto& c = kit.Theme();
        const rename::Planned* p = static_cast<size_t>(row) < m_plan.items.size() ? &m_plan.items[static_cast<size_t>(row)] : nullptr;
        const std::wstring note = !p ? L"" : !p->problem.empty() ? p->problem : !p->warning.empty() ? p->warning : p->to == p->from ? L"unchanged" : L"";
        const bool problem = p && !p->problem.empty(), warning = p && p->problem.empty() && !p->warning.empty();
        const float pillW = note.empty() ? 0.f : std::min(kit.PillWidth(note), (r.right - r.left) / 3);
        const float right = r.right - 16 - (pillW ? pillW + 12 : 0.f);
        const float mid = r.left + 16 + (right - r.left - 16) * 0.48f;
        const auto format = kit.TextFormat(ControlKit::kControlText);
        kit.DrawText(FileName(m_items[static_cast<size_t>(row)].path), format, D2D1::RectF(r.left + 16, r.top, mid - 12, r.bottom), c.textDim);
        kit.DrawGlyph(0xE72A, 11.f, D2D1::RectF(mid - 12, r.top, mid + 8, r.bottom), c.textFaint);   // Forward
        if (p)
            kit.DrawText(FileName(p->to), kit.TextFormat(ControlKit::kControlText, p->Changes() ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL),
                         D2D1::RectF(mid + 14, r.top, right, r.bottom), problem ? c.danger : c.text);
        if (!note.empty())
        {
            const float cy = (r.top + r.bottom) / 2;
            const auto pill = D2D1::RectF(r.right - 16 - pillW, cy - 10, r.right - 16, cy + 10);
            if (problem || warning)
                kit.DrawPill(pill, note, problem ? c.danger : c.warn);
            else
                kit.DrawText(note, kit.TextFormat(ControlKit::kSmallText, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING), pill, c.textFaint);
        }
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
        SetWindowTextW(m_patternLabel, change == rename::Change::NameAndExtension ? L"New file name" : L"New name");
        Update();
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
        SetRole(m_status, !m_plan.error.empty() || m_plan.problems > 0 || m_plan.warnings > 0 ? TextRole::Error : TextRole::Caption);
        // "6 files · 1 conflict" over the list, and the button says what it does.
        const auto files = m_items.size() == 1 ? std::wstring(L"1 file") : std::format(L"{} files", m_items.size());
        SetWindowTextW(m_count, m_plan.problems > 0 ? std::format(L"{} \u00B7 {} can\u2019t be used", files, m_plan.problems).c_str()
                                                    : std::format(L"{} \u00B7 {} changing", files, m_plan.changes).c_str());
        SetWindowTextW(m_rename, m_items.size() == 1 ? L"Rename" : std::format(L"Rename {} files", m_items.size()).c_str());
        Redraw(m_rename);
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
                    Update();
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
            default:
                if (const int i = LOWORD(wParam) - kIdField; i >= 0 && i < static_cast<int>(std::size(kFields)) && HIWORD(wParam) == BN_CLICKED)
                    InsertField(kFields[i]);
                break;
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
