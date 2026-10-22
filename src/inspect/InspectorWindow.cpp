#include "inspect/InspectorWindow.h"
#include "Brand.h"
#include "Clipboard.h"
#include "DialogWindow.h"
#include "Ffmpeg.h"
#include "Log.h"
#include "PathUtil.h"
#include "inspect/InspectModule.h"
#include "inspect/Metadata.h"
#include "inspect/MetadataReader.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <windowsx.h>
#include <wil/resource.h>

#include <algorithm>
#include <format>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace sd::inspect
{
    namespace
    {
        constexpr wchar_t kClass[] = L"" SD_NAME L".Inspector";
        constexpr int kIdTab = 100, kIdSearch = 90, kIdList = 91;   // a file's tab is kIdTab + its place
        constexpr int kCmdCopyValue = 1, kCmdCopyField = 2, kCmdCopyAll = 3;
        constexpr UINT kMsgRead = WM_APP + 1;   // wParam: file index, lParam: FileMetadata*, owned by the receiver

        // DIPs (App Windows page), scaled by the window's DPI.
        constexpr int kMargin = 20, kGap = 8, kSearchH = 32, kStatusH = 18, kTabsH = 38, kRowH = 32, kFieldW = 190;

        // A ListView row is one line: line breaks in a value (comments, XMP) show as a return mark.
        std::wstring OneLine(const std::wstring& value)
        {
            std::wstring out;
            out.reserve(value.size());
            for (size_t i = 0; i < value.size(); ++i)
            {
                if (value[i] == L'\r')
                    continue;
                if (value[i] == L'\n')
                    out += L" \u21B5 ";
                else
                    out += value[i];
            }
            return out;
        }

        // What the worker and the window share. The window may close while a file is still being
        // read: the worker then stops, and anything it posts to the dead window is freed.
        struct Shared
        {
            std::stop_source stop;
        };

        // One per Inspect drop; it deletes itself when its window closes (OnNcDestroy).
        class InspectorWindow final : public DialogWindow
        {
        public:
            InspectorWindow(std::vector<std::wstring> files, std::shared_ptr<const FfmpegEngine> ffmpeg)
                // In light mode the classic "dialog" face, like the history window.
                : DialogWindow(GetModuleHandleW(nullptr),
                               { .className = kClass, .width = 640, .height = 600, .minWidth = 380, .minHeight = 300, .lightBackground = COLOR_BTNFACE,
                                 .kitFont = true }),
                  m_paths(std::move(files)), m_ffmpeg(std::move(ffmpeg))
            {
                m_files.resize(m_paths.size());
                m_loaded.assign(m_paths.size(), false);
            }

            bool Show(HWND owner)
            {
                (void)owner;   // a top-level window of its own: it outlives the wheel and shows on the taskbar
                const std::wstring title = m_paths.size() == 1 ? std::format(L"Inspect: {}", FileName(m_paths.front()))
                                                               : std::format(L"Inspect: {} files", m_paths.size());
                if (!Create(title.c_str()))
                    return false;
                Fill();
                Present(m_search);   // the user just asked for it (the drop)
                StartReading();
                return true;
            }

        private:
            void OnNcDestroy() override
            {
                m_shared->stop.request_stop();
                delete this;   // one window, one object: it goes with its window
            }

            // Keys for the search box and the list (no dialog manager in the app's message loop):
            // Esc closes, Ctrl+F goes to the search, Down / Enter from the search to the list, Tab
            // between them, Ctrl+C / Ctrl+A in the list.
            static LRESULT CALLBACK KeyProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
            {
                auto* self = reinterpret_cast<InspectorWindow*>(data);
                const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                if (msg == WM_GETDLGCODE && hwnd == self->m_search)
                    return DefSubclassProc(hwnd, msg, wParam, lParam) | DLGC_WANTALLKEYS;
                // (Ctrl+C and Ctrl+A arrive as characters 3 and 1; the search box keeps its own.)
                if (msg == WM_GETDLGCODE)
                    return DefSubclassProc(hwnd, msg, wParam, lParam) | DLGC_WANTARROWS | DLGC_WANTTAB;
                if (msg == WM_CHAR && (wParam == VK_ESCAPE || wParam == VK_RETURN || wParam == VK_TAB ||
                                       (ctrl && (wParam == 6 || (hwnd == self->m_list && (wParam == 3 || wParam == 1))))))
                    return 0;   // handled on WM_KEYDOWN (no beep, no stray characters)
                if (msg == WM_KEYDOWN)
                {
                    if (wParam == VK_ESCAPE)
                    {
                        PostMessageW(self->m_hwnd, WM_CLOSE, 0, 0);
                        return 0;
                    }
                    if (ctrl && wParam == 'F')
                    {
                        SetFocus(self->m_search);
                        Edit_SetSel(self->m_search, 0, -1);
                        return 0;
                    }
                    if (wParam == VK_TAB)
                    {
                        // Search, the list, the file's tab (with several files), and round again.
                        const HWND tab = self->m_tabs.size() > 1 ? self->m_tabs[self->m_tab] : nullptr;
                        HWND next = hwnd == self->m_search ? self->m_list : hwnd == self->m_list && tab ? tab : self->m_search;
                        if (GetKeyState(VK_SHIFT) & 0x8000)
                            next = hwnd == self->m_search ? (tab ? tab : self->m_list) : hwnd == self->m_list ? self->m_search : self->m_list;
                        SetFocus(next);
                        return 0;
                    }
                    if ((wParam == VK_LEFT || wParam == VK_RIGHT) && std::find(self->m_tabs.begin(), self->m_tabs.end(), hwnd) != self->m_tabs.end())
                    {
                        // The arrow keys move between the files' tabs, as in a tab control.
                        const size_t count = self->m_tabs.size();
                        self->SelectTab((self->m_tab + (wParam == VK_LEFT ? count - 1 : 1)) % count);
                        SetFocus(self->m_tabs[self->m_tab]);
                        return 0;
                    }
                    if (ctrl && (wParam == VK_NEXT || wParam == VK_PRIOR) && self->m_tabs.size() > 1)
                    {
                        const size_t count = self->m_tabs.size();
                        self->SelectTab((self->m_tab + (wParam == VK_PRIOR ? count - 1 : 1)) % count);
                        return 0;
                    }
                    if (hwnd == self->m_search && (wParam == VK_DOWN || wParam == VK_RETURN))
                    {
                        SetFocus(self->m_list);
                        if (ListView_GetNextItem(self->m_list, -1, LVNI_SELECTED) < 0 && ListView_GetItemCount(self->m_list) > 0)
                            ListView_SetItemState(self->m_list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                        return 0;
                    }
                    if (hwnd == self->m_list && ctrl && wParam == 'C')
                    {
                        self->Copy(kCmdCopyValue);
                        return 0;
                    }
                    if (hwnd == self->m_list && ctrl && wParam == 'A')
                    {
                        ListView_SetItemState(self->m_list, -1, LVIS_SELECTED, LVIS_SELECTED);
                        return 0;
                    }
                }
                return DefSubclassProc(hwnd, msg, wParam, lParam);
            }

            void CreateChildren() override
            {
                // A tab per file, only for several files.
                if (m_paths.size() > 1)
                    for (size_t i = 0; i < m_paths.size(); ++i)
                    {
                        const HWND tab = KitButton(Look::Tab, FileName(m_paths[i]).c_str(), kIdTab + static_cast<int>(i), Surface::Window, 0, i == 0);
                        SetWindowSubclass(tab, KeyProc, 1, reinterpret_cast<DWORD_PTR>(this));
                        m_tabs.push_back(tab);
                    }
                m_search = TextField(kIdSearch, 0, L"Search fields and values (Ctrl+F)", true, Surface::Window);
                Describe(m_search, L"Search fields and values");
                m_note = Label(L"Read-only. To remove fields, use File actions \u203A Remove metadata.", TextRole::Hint, Surface::Window);
                m_list = KitList(kIdList, LVS_NOCOLUMNHEADER | LVS_NOSORTHEADER, { L"Field", L"Value" }, kRowH);
                ListView_SetExtendedListViewStyleEx(m_list, LVS_EX_INFOTIP, LVS_EX_INFOTIP);
                ListView_EnableGroupView(m_list, TRUE);
                Describe(m_list, L"Fields", L"Ctrl+C copies the value");
                m_status = Label(L"", TextRole::Caption, Surface::Window);
                SetWindowSubclass(m_search, KeyProc, 1, reinterpret_cast<DWORD_PTR>(this));
                SetWindowSubclass(m_list, KeyProc, 1, reinterpret_cast<DWORD_PTR>(this));
                CheckOne(m_tabs, 0);
            }

            void OnDpiChanged() override { ListView_SetColumnWidth(m_list, 0, Px(kFieldW)); }

            void Layout() override
            {
                RECT rc{};
                GetClientRect(m_hwnd, &rc);
                const int margin = Px(kMargin), gap = Px(kGap), width = std::max(0, static_cast<int>(rc.right) - 2 * margin);
                const auto place = [](HWND h, int x, int y, int w, int hgt) { MoveWindow(h, x, y, std::max(w, 0), std::max(hgt, 0), TRUE); };
                int y = Px(12);
                m_tabLine = {};
                if (!m_tabs.empty())
                {
                    int x = margin;
                    for (HWND tab : m_tabs)
                    {
                        const int w = Px(static_cast<int>(Kit().TabWidth(Text(tab))));
                        place(tab, x, y, std::min(w, std::max(0, static_cast<int>(rc.right) - margin - x)), Px(kTabsH));
                        x += w + Px(20);
                    }
                    y += Px(kTabsH);
                    m_tabLine = { margin, y, rc.right - margin, y + std::max(1, Px(1)) };
                    y += Px(14);
                }
                PlaceField(m_search, { margin, y, margin + width, y + Px(kSearchH) });
                y += Px(kSearchH) + gap;
                const int noteH = LabelHeight(m_note, width);
                place(m_note, margin, y, width, noteH);
                y += noteH + Px(10);
                const int statusY = rc.bottom - Px(12) - Px(kStatusH);
                m_listCard = { margin, y, rc.right - margin, std::max(y + Px(60), statusY - gap) };
                place(m_list, m_listCard.left + Px(1), m_listCard.top + Px(4), m_listCard.right - m_listCard.left - Px(2), m_listCard.bottom - m_listCard.top - Px(8));
                place(m_status, margin, statusY, width, Px(kStatusH));
                // The value column takes the rest of the width.
                RECT list{};
                GetClientRect(m_list, &list);
                ListView_SetColumnWidth(m_list, 1, std::max(Px(80), static_cast<int>(list.right) - ListView_GetColumnWidth(m_list, 0) - 2));
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }

            void PaintSurface(ControlKit& kit, const D2D1_RECT_F&) override
            {
                if (m_tabLine.right > m_tabLine.left)
                    kit.FillRound(DipRect(m_tabLine), kit.Theme().line, 0.f);
                if (m_listCard.right > m_listCard.left)
                    kit.DrawCard(DipRect(m_listCard));
            }

            void DrawRow(HWND, int row, ControlKit& kit, const D2D1_RECT_F& r, bool) override
            {
                LVITEMW item{};
                item.mask = LVIF_PARAM;
                item.iItem = row;
                if (!ListView_GetItem(m_list, &item) || static_cast<size_t>(item.lParam) >= m_rows.size())
                    return;
                const auto& field = m_rows[static_cast<size_t>(item.lParam)];
                const auto& c = kit.Theme();
                const float split = r.left + 16 + kFieldW - 8;
                kit.DrawText(field.name, kit.TextFormat(ControlKit::kSmallText + 0.5f), D2D1::RectF(r.left + 16, r.top, split - 12, r.bottom), c.textDim);
                kit.DrawText(OneLine(field.value), kit.TextFormat(ControlKit::kControlText), D2D1::RectF(split, r.top, r.right - 12, r.bottom), c.text);
            }

            void SelectTab(size_t tab)
            {
                if (tab >= m_paths.size() || tab == m_tab)
                    return;
                m_tab = tab;
                CheckOne(m_tabs, static_cast<int>(tab));
                Fill();
            }

            void StartReading()
            {
                // One worker for all the files of this window, in tab order.
                std::thread([hwnd = m_hwnd, paths = m_paths, ffmpeg = m_ffmpeg, stop = m_shared->stop.get_token()] {
                    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    for (size_t i = 0; i < paths.size() && !stop.stop_requested(); ++i)
                    {
                        std::unique_ptr<FileMetadata> read;
                        try
                        {
                            read = std::make_unique<FileMetadata>(ReadMetadata(paths[i], ffmpeg.get(), stop));
                        }
                        catch (const std::exception&)
                        {
                            read = std::make_unique<FileMetadata>();
                            read->path = paths[i];
                            read->error = L"could not be read";
                        }
                        if (stop.stop_requested())
                            break;
                        if (PostMessageW(hwnd, kMsgRead, i, reinterpret_cast<LPARAM>(read.get())))
                            read.release();   // the window owns it now
                    }
                    if (SUCCEEDED(init))
                        CoUninitialize();
                }).detach();
            }

            void OnRead(size_t index, std::unique_ptr<FileMetadata> read)
            {
                if (!read || index >= m_files.size())
                    return;
                m_files[index] = std::move(*read);
                m_loaded[index] = true;
                log::Info(L"inspect: {} field(s) in {}", m_files[index].fields.size(), m_paths[index]);
                if (index == m_tab)
                    Fill();
            }

            std::wstring Query() const { return Text(m_search); }

            // Rebuilds the list for the current tab and search text.
            void Fill()
            {
                SendMessageW(m_list, WM_SETREDRAW, FALSE, 0);
                ListView_DeleteAllItems(m_list);
                ListView_RemoveAllGroups(m_list);
                m_rows.clear();
                const auto& file = m_files[m_tab];
                std::wstring status;
                if (!m_loaded[m_tab])
                    status = L"Reading\u2026";
                else if (!file.error.empty())
                    status = std::format(L"{}: {}", FileName(m_paths[m_tab]), file.error);
                else
                {
                    const auto query = Query();
                    const auto shown = Filter(file.fields, query);
                    int groupId = 1, row = 0;
                    for (const auto& group : GroupFields(shown))
                    {
                        LVGROUP g{};
                        g.cbSize = sizeof(g);
                        g.mask = LVGF_HEADER | LVGF_GROUPID | LVGF_STATE;
                        g.pszHeader = const_cast<wchar_t*>(group.name.c_str());
                        g.iGroupId = groupId;
                        g.stateMask = LVGS_COLLAPSIBLE;
                        g.state = LVGS_COLLAPSIBLE;
                        ListView_InsertGroup(m_list, -1, &g);
                        for (const auto& field : group.fields)
                        {
                            LVITEMW item{};
                            item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_GROUPID;
                            item.iItem = row;
                            item.iGroupId = groupId;
                            item.lParam = static_cast<LPARAM>(m_rows.size());
                            item.pszText = const_cast<wchar_t*>(field.name.c_str());
                            const int at = ListView_InsertItem(m_list, &item);
                            auto value = OneLine(field.value);
                            ListView_SetItemText(m_list, at, 1, value.data());
                            m_rows.push_back(field);
                            ++row;
                        }
                        ++groupId;
                    }
                    status = query.empty() ? std::format(L"{} field{}", m_rows.size(), m_rows.size() == 1 ? L"" : L"s")
                                           : std::format(L"{} of {} fields match", m_rows.size(), file.fields.size());
                    status += L" \u00B7 Ctrl+C copies the value";
                }
                SendMessageW(m_list, WM_SETREDRAW, TRUE, 0);
                InvalidateRect(m_list, nullptr, TRUE);
                SetWindowTextW(m_status, status.c_str());
            }

            std::vector<const Field*> SelectedRows() const
            {
                std::vector<const Field*> rows;
                for (int i = ListView_GetNextItem(m_list, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(m_list, i, LVNI_SELECTED))
                {
                    LVITEMW item{};
                    item.mask = LVIF_PARAM;
                    item.iItem = i;
                    if (ListView_GetItem(m_list, &item) && static_cast<size_t>(item.lParam) < m_rows.size())
                        rows.push_back(&m_rows[static_cast<size_t>(item.lParam)]);
                }
                return rows;
            }

            void Copy(int command)
            {
                std::vector<const Field*> rows;
                if (command == kCmdCopyAll)
                    for (const auto& r : m_rows)
                        rows.push_back(&r);
                else
                    rows = SelectedRows();
                if (rows.empty())
                    return;
                std::wstring text;
                // One value alone is copied as it is; several rows (or "field and value") as lines.
                if (command == kCmdCopyValue && rows.size() == 1)
                    text = rows.front()->value;
                else
                {
                    std::wstring group;
                    for (const auto* r : rows)
                    {
                        if (command == kCmdCopyAll && r->group != group)
                        {
                            text += (text.empty() ? L"" : L"\r\n") + r->group + L"\r\n";
                            group = r->group;
                        }
                        text += command == kCmdCopyValue ? r->value : r->name + L": " + r->value;
                        text += L"\r\n";
                    }
                }
                if (clipboard::SetText(m_hwnd, text))
                    SetWindowTextW(m_status, rows.size() == 1 ? std::format(L"Copied {}", rows.front()->name).c_str()
                                                              : std::format(L"Copied {} fields", rows.size()).c_str());
            }

            void ContextMenu(POINT pt)
            {
                if (pt.x == -1 && pt.y == -1)   // from the keyboard (Shift+F10, the menu key)
                {
                    RECT r{};
                    const int row = ListView_GetNextItem(m_list, -1, LVNI_FOCUSED);
                    if (row >= 0 && ListView_GetItemRect(m_list, row, &r, LVIR_LABEL))
                        pt = POINT{ r.left, r.bottom };
                    ClientToScreen(m_list, &pt);
                }
                wil::unique_hmenu menu(CreatePopupMenu());
                const UINT one = SelectedRows().empty() ? MF_GRAYED : 0;
                AppendMenuW(menu.get(), MF_STRING | one, kCmdCopyValue, L"Copy value\tCtrl+C");
                AppendMenuW(menu.get(), MF_STRING | one, kCmdCopyField, L"Copy field and value");
                AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu.get(), MF_STRING | (m_rows.empty() ? MF_GRAYED : 0), kCmdCopyAll, L"Copy all shown fields");
                const int command = TrackPopupMenuEx(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, m_hwnd, nullptr);
                if (command)
                    Copy(command);
            }

            LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam) override
            {
                switch (msg)
                {
                case kMsgRead:
                    OnRead(static_cast<size_t>(wParam), std::unique_ptr<FileMetadata>(reinterpret_cast<FileMetadata*>(lParam)));
                    return 0;
                case WM_ACTIVATE:
                    if (LOWORD(wParam) != WA_INACTIVE && m_focus)
                        SetFocus(m_focus);   // back where it was
                    else if (LOWORD(wParam) == WA_INACTIVE)
                        m_focus = GetFocus();
                    return 0;
                case WM_COMMAND:
                    if (LOWORD(wParam) == kIdSearch && HIWORD(wParam) == EN_CHANGE)
                        Fill();
                    else if (const int tab = LOWORD(wParam) - kIdTab; tab >= 0 && static_cast<size_t>(tab) < m_tabs.size() && HIWORD(wParam) == BN_CLICKED)
                        SelectTab(static_cast<size_t>(tab));
                    return 0;
                case WM_CONTEXTMENU:
                    if (reinterpret_cast<HWND>(wParam) == m_list)
                        ContextMenu(POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
                    return 0;
                case WM_NOTIFY:
                {
                    const auto* hdr = reinterpret_cast<const NMHDR*>(lParam);
                    if (hdr->idFrom == kIdList && hdr->code == NM_DBLCLK)
                        Copy(kCmdCopyValue);
                    return 0;
                }
                case WM_CLOSE:
                    DestroyWindow(m_hwnd);
                    return 0;
                default:
                    return DialogWindow::Handle(msg, wParam, lParam);
                }
            }

            std::vector<std::wstring> m_paths;
            std::shared_ptr<const FfmpegEngine> m_ffmpeg;
            std::shared_ptr<Shared> m_shared = std::make_shared<Shared>();
            std::vector<FileMetadata> m_files;
            std::vector<bool> m_loaded;
            std::vector<Field> m_rows;   // the list's rows, by item lParam
            size_t m_tab = 0;

            std::vector<HWND> m_tabs;   // one per file, with several
            HWND m_search = nullptr, m_note = nullptr, m_list = nullptr, m_status = nullptr;
            RECT m_tabLine{}, m_listCard{};
            HWND m_focus = nullptr;
        };

        void Open(HWND owner, const std::vector<std::wstring>& files, std::shared_ptr<const FfmpegEngine> ffmpeg)
        {
            if (files.empty())
                return;
            auto window = std::make_unique<InspectorWindow>(files, std::move(ffmpeg));
            if (window->Show(owner))
                window.release();   // owned by its window from here on (see WM_NCDESTROY)
            else
                log::Error(L"inspect: could not open the window ({})", GetLastError());
        }
    }

    void InstallWindow() { SetWindowOpener(&Open); }
}
