#include "inspect/InspectorWindow.h"
#include "Brand.h"
#include "DialogWindow.h"
#include "Ffmpeg.h"
#include "Log.h"
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
        constexpr int kIdTabs = 100, kIdSearch = 101, kIdList = 102, kIdStatus = 103;
        constexpr int kCmdCopyValue = 1, kCmdCopyField = 2, kCmdCopyAll = 3;
        constexpr UINT kMsgRead = WM_APP + 1;   // wParam: file index, lParam: FileMetadata*, owned by the receiver

        // DIPs, scaled by the window's DPI.
        constexpr int kMargin = 10, kGap = 6, kSearchH = 24, kStatusH = 18, kTabsH = 28;

        std::wstring FileName(const std::wstring& path)
        {
            const auto slash = path.find_last_of(L"\\/");
            return slash == std::wstring::npos ? path : path.substr(slash + 1);
        }

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
                    out += L" ↵ ";
                else
                    out += value[i];
            }
            return out;
        }

        bool CopyText(HWND owner, const std::wstring& text)
        {
            if (!OpenClipboard(owner))
                return false;
            EmptyClipboard();
            bool ok = false;
            if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t)))
            {
                if (auto* p = static_cast<wchar_t*>(GlobalLock(mem)))
                {
                    std::copy(text.c_str(), text.c_str() + text.size() + 1, p);
                    GlobalUnlock(mem);
                    ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
                }
                if (!ok)
                    GlobalFree(mem);
            }
            CloseClipboard();
            return ok;
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
                               { .className = kClass, .width = 620, .height = 560, .minWidth = 360, .minHeight = 260, .lightBackground = COLOR_BTNFACE }),
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
                        SetFocus(hwnd == self->m_search ? self->m_list : self->m_search);
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
                m_tabs = Control(WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS | TCS_SINGLELINE | TCS_FOCUSNEVER, kIdTabs);
                for (size_t i = 0; i < m_paths.size(); ++i)
                {
                    auto name = FileName(m_paths[i]);
                    TCITEMW item{};
                    item.mask = TCIF_TEXT;
                    item.pszText = name.data();
                    TabCtrl_InsertItem(m_tabs, static_cast<int>(i), &item);
                }
                if (m_paths.size() < 2)
                    ShowWindow(m_tabs, SW_HIDE);   // tabs only for several files
                m_search = Control(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, kIdSearch, WS_EX_CLIENTEDGE);
                // TRUE: the hint stays while the box has the focus and is empty.
                SendMessageW(m_search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search fields and values (Ctrl+F)"));
                m_list = Control(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER, kIdList);
                ListView_SetExtendedListViewStyle(m_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_INFOTIP);
                const wchar_t* titles[] = { L"Field", L"Value" };
                for (int i = 0; i < 2; ++i)
                {
                    LVCOLUMNW col{};
                    col.mask = LVCF_TEXT | LVCF_SUBITEM;
                    col.pszText = const_cast<wchar_t*>(titles[i]);
                    col.iSubItem = i;
                    ListView_InsertColumn(m_list, i, &col);
                }
                ListView_EnableGroupView(m_list, TRUE);
                m_status = Control(WC_STATICW, L"", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, kIdStatus);
                SetWindowSubclass(m_search, KeyProc, 1, reinterpret_cast<DWORD_PTR>(this));
                SetWindowSubclass(m_list, KeyProc, 1, reinterpret_cast<DWORD_PTR>(this));
            }

            void OnDpiChanged() override { ListView_SetColumnWidth(m_list, 0, Px(190)); }

            void OnThemeChanged() override
            {
                ThemeList(m_list);
                SetTheme({ m_search }, L"DarkMode_CFD");
            }

            void Layout() override
            {
                RECT rc{};
                GetClientRect(m_hwnd, &rc);
                const int margin = Px(kMargin), gap = Px(kGap);
                int y = margin;
                HDWP dwp = BeginDeferWindowPos(4);
                const auto place = [&](HWND h, int x, int top, int w, int hgt) {
                    if (dwp)
                        dwp = DeferWindowPos(dwp, h, nullptr, x, top, std::max(w, 0), std::max(hgt, 0), SWP_NOZORDER | SWP_NOACTIVATE);
                };
                const int width = rc.right - 2 * margin;
                if (m_paths.size() > 1)
                {
                    place(m_tabs, margin, y, width, Px(kTabsH));
                    y += Px(kTabsH) + gap;
                }
                place(m_search, margin, y, width, Px(kSearchH));
                y += Px(kSearchH) + gap;
                const int statusY = rc.bottom - margin - Px(kStatusH);
                place(m_list, margin, y, width, statusY - gap - y);
                place(m_status, margin, statusY, width, Px(kStatusH));
                if (dwp)
                    EndDeferWindowPos(dwp);
                // The value column takes the rest of the width.
                RECT list{};
                GetClientRect(m_list, &list);
                ListView_SetColumnWidth(m_list, 1, std::max(Px(80), static_cast<int>(list.right) - ListView_GetColumnWidth(m_list, 0) - 2));
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

            std::wstring Query() const
            {
                const int n = GetWindowTextLengthW(m_search);
                std::wstring text(static_cast<size_t>(n) + 1, L'\0');
                GetWindowTextW(m_search, text.data(), n + 1);
                text.resize(static_cast<size_t>(n));
                return text;
            }

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
                    status = L"Reading…";
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
                    status += L" · read-only · Ctrl+C copies the value";
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
                if (CopyText(m_hwnd, text))
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
                    return 0;
                case WM_CONTEXTMENU:
                    if (reinterpret_cast<HWND>(wParam) == m_list)
                        ContextMenu(POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
                    return 0;
                case WM_NOTIFY:
                {
                    const auto* hdr = reinterpret_cast<const NMHDR*>(lParam);
                    if (hdr->idFrom == kIdTabs && hdr->code == TCN_SELCHANGE)
                    {
                        m_tab = static_cast<size_t>(std::max(0, TabCtrl_GetCurSel(m_tabs)));
                        Fill();
                    }
                    else if (hdr->idFrom == kIdList && hdr->code == NM_DBLCLK)
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

            HWND m_tabs = nullptr, m_search = nullptr, m_list = nullptr, m_status = nullptr;
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
