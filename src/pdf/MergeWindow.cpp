// Merge PDFs (and Edit PDF with several files): the dropped PDFs in Explorer's order, top to
// bottom; drag (or Alt+Up/Down) to change the order, Delete to leave one out, Enter to merge.
// More... shows every page in the page view (OrganizeWindow.h), whose Back comes back here with the
// page edits kept. Installs itself into pdf::Editors().
#include "Log.h"
#include "Theme.h"
#include "UiThread.h"
#include "pdf/EditorWindow.h"
#include "pdf/OrganizeWindow.h"
#include "pdf/PageEdits.h"
#include "pdf/PdfDoc.h"
#include "pdf/PdfEditors.h"
#include "pdf/PdfModule.h"
#include "pdf/ScrollList.h"

#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <memory>
#include <thread>

namespace sd::pdf
{
    namespace
    {
        constexpr UINT kPagesCounted = WM_APP + 1;   // lParam: Counted*, ours to delete

        // DIPs
        constexpr float kMargin = 16.f, kHeader = 56.f, kRowH = 44.f, kFooter = 64.f, kButtonW = 104.f, kButtonH = 32.f;

        using Done = std::function<void(std::vector<std::wstring>, PagePlan)>;

        struct Item
        {
            uint32_t file = 0;      // index in the drop: the page plan and the file's tag go by it
            std::wstring path;
            uint64_t bytes = 0;
            long long pages = -1;   // -1 while counting, -2 unreadable
        };

        struct Counted
        {
            uint32_t file = 0;
            long long pages = -2;
        };

        class MergeWindow final : public EditorWindow
        {
        public:
            MergeWindow(const std::vector<std::wstring>& files, bool edit, Done done) : m_files(files), m_edit(edit), m_done(std::move(done))
            {
                for (uint32_t i = 0; i < files.size(); ++i)
                {
                    std::error_code ec;
                    const auto size = std::filesystem::file_size(files[i], ec);
                    m_items.push_back(Item{ i, files[i], ec ? 0 : size });
                }
                m_planOrder = Order();
                // In ButtonId order.
                m_buttons.push_back(Button{ L"More…", 0, false, true, {}, [this] { More(); } });
                m_buttons.push_back(Button{ L"Cancel", 0, false, true, {}, [this] { Cancel(); } });
                m_buttons.push_back(Button{ L"Merge", 0, true, true, {}, [this] { Finish(); } });
            }

            // `pages`: start with every page in the page view (Edit PDF); the list shows after Back.
            bool Open(bool pages)
            {
                if (!Create(Title(), 560, 460, 380, 300, !pages))
                    return false;
                StartCounting();
                if (pages && !More())
                {
                    ShowWindow(Hwnd(), SW_SHOWNORMAL);
                    SetForegroundWindow(Hwnd());
                }
                return true;
            }

        private:
            enum ButtonId { kMore, kCancel, kMerge };

            std::wstring Title() const { return m_edit ? L"Edit PDFs" : L"Merge PDFs"; }
            size_t MinFiles() const { return m_edit ? 1 : 2; }   // a merge needs two files

            std::vector<uint32_t> Order() const
            {
                std::vector<uint32_t> order;
                for (const auto& i : m_items)
                    order.push_back(i.file);
                return order;
            }

            void StartCounting()
            {
                // Page counts read on a worker (QPDF opens each file); posted back one by one.
                std::vector<std::wstring> paths;
                for (const auto& i : m_items)
                    paths.push_back(i.path);
                std::thread([hwnd = Hwnd(), paths, stop = m_stop] {
                    for (uint32_t f = 0; f < paths.size(); ++f)
                    {
                        if (stop->load())
                            return;
                        std::vector<uint32_t> counts;
                        std::wstring error;
                        auto counted = std::make_unique<Counted>();
                        counted->file = f;
                        try
                        {
                            if (CountPages({ paths[f] }, counts, error) && !counts.empty())
                                counted->pages = counts.front();
                        }
                        catch (...)
                        {
                        }
                        PostOwned(hwnd, kPagesCounted, std::move(counted));
                    }
                }).detach();
            }

            void OnClosing() override
            {
                m_stop->store(true);
                // The edited pages go with the export when it uses them; otherwise nothing reads them now.
                if (!m_editFolder.empty() && !m_exported)
                    RemoveEditFolder(m_editFolder);
            }

            // ---- The page view ------------------------------------------------------------------

            // The page plan holds changes the user would lose by closing.
            bool PagesChanged() const
            {
                if (!m_hasPlan)
                    return false;
                std::vector<uint32_t> counts(m_files.size(), 0);
                for (const auto& i : m_items)
                {
                    if (i.pages < 0)
                        return true;
                    counts[i.file] = static_cast<uint32_t>(i.pages);
                }
                return EditedPages(m_plan) > 0 || m_plan != AllPages(counts, Order());
            }

            bool More()
            {
                PageViewRequest request;
                request.kind = m_edit ? PageViewKind::Edit : PageViewKind::Merge;
                request.files = m_files;
                request.order = Order();
                if (m_hasPlan)
                    request.plan = m_plan;
                request.changed = PagesChanged();
                request.editFolder = m_editFolder;
                // The list stays (hidden) while the pages are up; it goes only with them.
                const HWND hwnd = Hwnd();
                request.save = [this, hwnd](PagePlan plan, std::wstring folder) {
                    if (!IsWindow(hwnd))
                        return;
                    m_plan = std::move(plan);
                    m_hasPlan = true;
                    m_editFolder = std::move(folder);
                    if (!Finish())   // deletes this when it goes ahead
                    {
                        ShowWindow(hwnd, SW_SHOWNORMAL);
                        Invalidate();
                    }
                };
                request.back = [this, hwnd](PagePlan plan, std::wstring folder) {
                    if (!IsWindow(hwnd))
                        return;
                    m_plan = std::move(plan);
                    m_hasPlan = true;
                    m_planOrder = Order();
                    m_editFolder = std::move(folder);
                    m_notice.clear();
                    ShowWindow(hwnd, SW_SHOWNORMAL);
                    SetForegroundWindow(hwnd);
                    Invalidate();
                };
                request.cancelled = [this, hwnd] {
                    if (!IsWindow(hwnd))
                        return;
                    m_editFolder.clear();   // the page view removed it
                    Close();                // deletes this
                };
                if (!OpenPageView(std::move(request)))
                {
                    m_notice = L"The pages cannot be shown.";
                    Invalidate();
                    return false;
                }
                ShowWindow(hwnd, SW_HIDE);
                return true;
            }

            // ---- The list -----------------------------------------------------------------------

            bool Finish()
            {
                const auto order = Order();
                PagePlan plan;
                if (m_hasPlan)
                    plan = InListOrder(m_plan, order);
                if (order.size() < MinFiles() || (m_hasPlan && plan.empty()))
                {
                    MessageBeep(MB_OK);   // nothing left to write
                    return false;
                }
                std::vector<std::wstring> ordered;
                for (const auto f : order)
                    ordered.push_back(m_files[f]);
                m_exported = EditedPages(plan) > 0;   // the export removes the edited pages once written
                auto done = std::move(m_done);
                Close();   // deletes this
                done(std::move(ordered), std::move(plan));
                return true;
            }

            void Cancel()
            {
                if (PagesChanged() && !ConfirmDiscard(Hwnd(), Title()))
                    return;
                Close();   // deletes this
            }

            // The list changed: the page plan follows the files' new order.
            void ListChanged()
            {
                const auto order = Order();
                if (m_hasPlan)
                {
                    bool regrouped = false;
                    m_plan = PlanForFileOrder(m_plan, m_planOrder, order, &regrouped);
                    if (regrouped)
                        m_notice = L"Pages you had moved in among another file's are back with their own file.";
                }
                m_planOrder = order;
                EnsureVisible(m_focus);
                Invalidate();
            }

            D2D1_RECT_F ListRect(D2D1_SIZE_F size) const { return D2D1::RectF(kMargin, kHeader, size.width - kMargin, size.height - kFooter); }
            float ListHeight() const
            {
                const auto list = ListRect(ClientDip());
                return list.bottom - list.top;
            }

            void Layout(D2D1_SIZE_F size) override
            {
                const float y = size.height - kFooter + (kFooter - kButtonH) / 2.f;
                m_buttons[kMerge].rect = D2D1::RectF(size.width - kMargin - kButtonW, y, size.width - kMargin, y + kButtonH);
                m_buttons[kCancel].rect = D2D1::RectF(m_buttons[kMerge].rect.left - 8.f - kButtonW, y, m_buttons[kMerge].rect.left - 8.f, y + kButtonH);
                m_buttons[kMore].rect = D2D1::RectF(m_buttons[kCancel].rect.left - 8.f - kButtonW, y, m_buttons[kCancel].rect.left - 8.f, y + kButtonH);
                const auto list = ListRect(size);
                m_scroll.cellWidth = std::max(1.f, list.right - list.left);
                m_scroll.rowHeight = kRowH;
                m_scroll.columns = 1;
                m_scroll.Clamp(m_items.size(), list.bottom - list.top);
            }

            void EnsureVisible(size_t index) { m_scroll.Reveal(index, m_items.size(), ListHeight()); }

            void Move(int delta)
            {
                if (m_items.empty())
                    return;
                const size_t to = delta < 0 ? (m_focus == 0 ? 0 : m_focus - 1) : std::min(m_items.size(), m_focus + 2);
                m_focus = MoveItems(m_items, { m_focus }, to).front();
                ListChanged();
            }

            void Remove()
            {
                if (m_items.size() <= MinFiles())
                {
                    MessageBeep(MB_OK);
                    return;
                }
                const auto next = RemoveItems(m_items, { m_focus });
                m_focus = next < 0 ? 0 : static_cast<size_t>(next);
                m_scroll.Clamp(m_items.size(), ListHeight());
                ListChanged();
            }

            std::wstring Details(const Item& item) const
            {
                std::wstring pages = item.pages == -1 ? L"counting pages…" : item.pages == -2 ? L"cannot be read" : item.pages == 1 ? L"1 page" : std::format(L"{} pages", item.pages);
                if (m_hasPlan && item.pages >= 0)
                    pages = std::format(L"{} of {}", PagesKept(m_plan, item.file), pages);
                return std::format(L"{} · {}", pages, SizeText(item.bytes));
            }

            void Paint(ID2D1RenderTarget* rt, D2D1_SIZE_F size) override
            {
                const auto& c = Theme();
                long long total = 0;
                bool counting = false;
                for (const auto& i : m_items)
                {
                    if (i.pages >= 0)
                        total += i.pages;
                    counting |= i.pages == -1;
                }
                if (m_hasPlan)
                    total = static_cast<long long>(InListOrder(m_plan, Order()).size());
                const std::wstring heading = m_edit ? std::format(L"Edit {} PDFs, top to bottom", m_items.size()) : std::format(L"Merge {} PDFs, top to bottom", m_items.size());
                DrawText(heading, TextFormat(18.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(kMargin, 8.f, size.width - kMargin, 34.f), c.text);
                std::wstring hint = L"Drag to reorder · Alt+↑/↓ moves · Delete leaves one out · More… (Alt+M) shows every page";
                bool notice = true;
                if (!m_notice.empty())
                    hint = m_notice;
                else if (m_hasPlan && !FilesStayTogether(m_plan))
                    hint = L"Some pages sit among another file's. Moving a file here puts its pages back together.";
                else
                    notice = false;
                DrawText(hint, TextFormat(12.f), D2D1::RectF(kMargin, 32.f, size.width - kMargin, 50.f), notice ? c.text : c.textDim);

                const auto list = ListRect(size);
                if (auto* b = Brush(c.surface))
                    rt->FillRoundedRectangle(D2D1::RoundedRect(list, 6.f, 6.f), b);
                rt->PushAxisAlignedClip(list, D2D1_ANTIALIAS_MODE_ALIASED);
                const auto [first, end] = m_scroll.Visible(m_items.size(), list.bottom - list.top);
                for (size_t i = first; i < end; ++i)
                {
                    const float top = list.top + static_cast<float>(i) * kRowH - m_scroll.offset;
                    const auto row = D2D1::RectF(list.left + 4.f, top + 2.f, list.right - 4.f, top + kRowH - 2.f);
                    const bool dragged = m_drag.dragging && i == m_focus;
                    if (i == m_focus)
                        if (auto* b = Brush(dragged ? theme::WithAlpha(c.selection, 0.5f) : c.selection))
                            rt->FillRoundedRectangle(D2D1::RoundedRect(row, 4.f, 4.f), b);
                    const auto& item = m_items[i];
                    DrawText(std::format(L"{}", i + 1), TextFormat(13.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING),
                             D2D1::RectF(row.left, row.top, row.left + 26.f, row.bottom), c.textDim);
                    // The file's tag, as its pages carry it in the page view.
                    const auto letter = FileTagLetter(item.file);
                    const float cy = (row.top + row.bottom) / 2.f, tagW = 10.f + 7.f * static_cast<float>(letter.size());
                    const auto tag = D2D1::RectF(row.left + 36.f, cy - 8.f, row.left + 36.f + tagW, cy + 8.f);
                    if (auto* b = Brush(FileTagColor(item.file)))
                        rt->FillRoundedRectangle(D2D1::RoundedRect(tag, 3.f, 3.f), b);
                    DrawText(letter, TextFormat(10.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), tag, D2D1::ColorF(1, 1, 1, 1));
                    const float detailsW = 200.f;
                    DrawText(FileNameOf(item.path), TextFormat(14.f), D2D1::RectF(tag.right + 8.f, row.top, row.right - detailsW - 8.f, row.bottom), c.text);
                    DrawText(Details(item), TextFormat(12.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING),
                             D2D1::RectF(row.right - detailsW, row.top, row.right - 10.f, row.bottom), c.textDim);
                }
                if (m_drag.dragging)
                {
                    const float y = list.top + static_cast<float>(m_insert) * kRowH - m_scroll.offset;
                    if (auto* b = Brush(c.accent))
                        rt->FillRectangle(D2D1::RectF(list.left + 4.f, y - 1.5f, list.right - 4.f, y + 1.5f), b);
                }
                rt->PopAxisAlignedClip();

                const std::wstring summary = counting && !m_hasPlan ? std::wstring(L"Counting pages…") : total == 1 ? std::wstring(L"1 page in all") : std::format(L"{} pages in all", total);
                const float y = size.height - kFooter;
                DrawText(summary, TextFormat(12.f), D2D1::RectF(kMargin, y, m_buttons[kMore].rect.left - 8.f, size.height), c.textDim);
                m_buttons[kMerge].text = m_edit && m_items.size() == 1 ? L"Save" : L"Merge";
                m_buttons[kMerge].enabled = m_items.size() >= MinFiles() && (!m_hasPlan || total > 0);
                DrawButtons(rt);
            }

            bool OnMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result) override
            {
                result = 0;
                switch (msg)
                {
                case kPagesCounted:
                {
                    std::unique_ptr<Counted> counted(reinterpret_cast<Counted*>(lParam));
                    for (auto& i : m_items)
                        if (i.file == counted->file)
                            i.pages = counted->pages;
                    Invalidate();
                    return true;
                }
                case WM_KEYDOWN:
                case WM_SYSKEYDOWN:
                    return OnKey(static_cast<UINT>(wParam), msg == WM_SYSKEYDOWN);
                case WM_LBUTTONDOWN:
                {
                    const auto p = DipFromLParam(lParam);
                    const auto list = ListRect(ClientDip());
                    const auto row = m_scroll.ItemAt(p.x - list.left, p.y - list.top, m_items.size());
                    if (row >= 0 && p.y >= list.top && p.y < list.bottom)
                    {
                        m_focus = static_cast<size_t>(row);
                        m_drag.Press(p.x, p.y);
                        SetCapture(Hwnd());
                        Invalidate();
                    }
                    return true;
                }
                case WM_MOUSEMOVE:
                    if (m_drag.pressed)
                    {
                        const auto p = DipFromLParam(lParam);
                        m_drag.Move(p.x, p.y, static_cast<float>(GetSystemMetrics(SM_CYDRAG)) / Scale());
                        if (m_drag.dragging)
                        {
                            const auto list = ListRect(ClientDip());
                            m_scroll.EdgeScroll(p.y - list.top, list.bottom - list.top, m_items.size());
                            m_insert = m_scroll.InsertAt(p.x - list.left, p.y - list.top, m_items.size());
                            Invalidate();
                        }
                    }
                    return true;
                case WM_LBUTTONUP:
                    if (m_drag.pressed)
                    {
                        const bool dragged = m_drag.dragging;
                        m_drag.Reset();
                        ReleaseCapture();
                        if (dragged)
                        {
                            m_focus = MoveItems(m_items, { m_focus }, m_insert).front();
                            ListChanged();
                        }
                        Invalidate();
                    }
                    return true;
                case WM_CAPTURECHANGED:
                    if (m_drag.pressed && reinterpret_cast<HWND>(lParam) != Hwnd())
                    {
                        m_drag.Reset();
                        Invalidate();
                    }
                    return false;
                case WM_LBUTTONDBLCLK:
                    return true;
                case WM_MOUSEWHEEL:
                    m_scroll.Wheel(GET_WHEEL_DELTA_WPARAM(wParam), 2.f, m_items.size(), ListHeight());
                    Invalidate();
                    return true;
                default:
                    return false;
                }
            }

            bool OnKey(UINT key, bool alt)
            {
                const bool move = alt || Ctrl();
                switch (key)
                {
                case VK_UP:
                    if (move)
                        Move(-1);
                    else if (m_focus > 0)
                        --m_focus;
                    break;
                case VK_DOWN:
                    if (move)
                        Move(+1);
                    else if (m_focus + 1 < m_items.size())
                        ++m_focus;
                    break;
                case VK_HOME:
                    m_focus = 0;
                    break;
                case VK_END:
                    m_focus = m_items.empty() ? 0 : m_items.size() - 1;
                    break;
                case VK_DELETE:
                    Remove();
                    break;
                case 'M':
                    if (!alt)
                        return false;
                    More();
                    return true;
                case VK_RETURN:
                    Finish();   // may delete this
                    return true;
                case VK_ESCAPE:
                    Cancel();   // may delete this
                    return true;
                default:
                    return false;   // Alt+F4 and the rest go to Windows
                }
                EnsureVisible(m_focus);
                Invalidate();
                return true;
            }

            std::vector<std::wstring> m_files;   // as dropped; Item::file and the plan point here
            bool m_edit = false;
            Done m_done;
            std::vector<Item> m_items;
            std::shared_ptr<std::atomic<bool>> m_stop = std::make_shared<std::atomic<bool>>(false);

            // The page view's plan, once it was used (file indices into m_files), and the list order
            // it was made for.
            PagePlan m_plan;
            bool m_hasPlan = false, m_exported = false;
            std::vector<uint32_t> m_planOrder;
            std::wstring m_editFolder, m_notice;

            size_t m_focus = 0, m_insert = 0;
            ScrollList m_scroll;
            DragGesture m_drag;
        };

        void OpenMerge(HWND, const std::vector<std::wstring>& files, bool pageView, Done done)
        {
            if (pageView && files.size() == 1)
            {
                // Edit PDF with one file: straight to its pages; Save writes "name (edited).pdf".
                PageViewRequest request;
                request.kind = PageViewKind::Edit;
                request.files = files;
                request.save = [files, done = std::move(done)](PagePlan plan, std::wstring) { done(files, std::move(plan)); };
                if (!OpenPageView(std::move(request)))
                    log::Warn(L"edit: the window could not open");
                return;
            }
            auto window = std::make_unique<MergeWindow>(files, pageView, std::move(done));
            if (window->Open(pageView))
                window.release();   // owns itself now
            else
                log::Warn(L"merge: the window could not open");
        }

        const bool g_installed = [] {
            Editors().merge = &OpenMerge;
            return true;
        }();
    }
}
