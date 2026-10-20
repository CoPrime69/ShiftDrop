// Merge PDFs: the dropped PDFs in Explorer's order, top to bottom; drag (or Alt+Up/Down) to change
// the order, Delete to leave one out, Enter to merge. Installs itself into pdf::Editors().
#include "Log.h"
#include "Theme.h"
#include "pdf/EditorWindow.h"
#include "pdf/PdfDoc.h"
#include "pdf/PdfEditors.h"
#include "pdf/PdfModule.h"

#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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

        struct Item
        {
            std::wstring path;
            uint64_t bytes = 0;
            long long pages = -1;   // -1 while counting, -2 unreadable
        };

        struct Counted
        {
            std::wstring path;
            long long pages = -2;
        };

        class MergeWindow final : public EditorWindow
        {
        public:
            MergeWindow(const std::vector<std::wstring>& files, std::function<void(std::vector<std::wstring>)> done)
                : m_done(std::move(done))
            {
                for (const auto& f : files)
                {
                    std::error_code ec;
                    const auto size = std::filesystem::file_size(f, ec);
                    m_items.push_back(Item{ f, ec ? 0 : size });
                }
                m_buttons.push_back(Button{ L"Cancel", 0, false, true, {}, [this] { Close(); } });
                m_buttons.push_back(Button{ L"Merge", 0, true, true, {}, [this] { Finish(); } });
            }

            bool Open()
            {
                if (!Create(L"Merge PDFs", 560, 460, 380, 300))
                    return false;
                StartCounting();
                return true;
            }

        private:
            void StartCounting()
            {
                // Page counts read on a worker (QPDF opens each file); posted back one by one.
                std::vector<std::wstring> paths;
                for (const auto& i : m_items)
                    paths.push_back(i.path);
                std::thread([hwnd = Hwnd(), paths, stop = m_stop] {
                    for (const auto& p : paths)
                    {
                        if (stop->load())
                            return;
                        std::vector<uint32_t> counts;
                        std::wstring error;
                        auto counted = std::make_unique<Counted>();
                        counted->path = p;
                        try
                        {
                            if (CountPages({ p }, counts, error) && !counts.empty())
                                counted->pages = counts.front();
                        }
                        catch (...)
                        {
                        }
                        if (PostMessageW(hwnd, kPagesCounted, 0, reinterpret_cast<LPARAM>(counted.get())))
                            counted.release();
                    }
                }).detach();
            }

            void OnClosing() override { m_stop->store(true); }

            void Finish()
            {
                if (m_items.size() < 2)
                    return;
                std::vector<std::wstring> ordered;
                for (const auto& i : m_items)
                    ordered.push_back(i.path);
                auto done = std::move(m_done);
                Close();   // deletes this
                done(std::move(ordered));
            }

            D2D1_RECT_F ListRect(D2D1_SIZE_F size) const
            {
                return D2D1::RectF(kMargin, kHeader, size.width - kMargin, size.height - kFooter);
            }

            void Layout(D2D1_SIZE_F size) override
            {
                const float y = size.height - kFooter + (kFooter - kButtonH) / 2.f;
                m_buttons[1].rect = D2D1::RectF(size.width - kMargin - kButtonW, y, size.width - kMargin, y + kButtonH);
                m_buttons[0].rect = D2D1::RectF(m_buttons[1].rect.left - 8.f - kButtonW, y, m_buttons[1].rect.left - 8.f, y + kButtonH);
                ClampScroll();
            }

            void ClampScroll()
            {
                const auto list = ListRect(ClientDip());
                const float content = static_cast<float>(m_items.size()) * kRowH;
                m_scroll = std::clamp(m_scroll, 0.f, std::max(0.f, content - (list.bottom - list.top)));
            }

            void EnsureVisible(size_t index)
            {
                const auto list = ListRect(ClientDip());
                const float top = static_cast<float>(index) * kRowH, height = list.bottom - list.top;
                if (top < m_scroll)
                    m_scroll = top;
                else if (top + kRowH > m_scroll + height)
                    m_scroll = top + kRowH - height;
                ClampScroll();
            }

            // The row under a point (may be past the end), or -1 outside the list.
            long long RowAt(D2D1_POINT_2F p) const
            {
                const auto list = ListRect(ClientDip());
                if (p.x < list.left || p.x > list.right)
                    return -1;
                return static_cast<long long>(std::floor((p.y - list.top + m_scroll) / kRowH));
            }

            // Where a dragged row would land: before this index (0..size).
            size_t InsertAt(D2D1_POINT_2F p) const
            {
                const auto list = ListRect(ClientDip());
                const float at = (p.y - list.top + m_scroll) / kRowH + 0.5f;
                return static_cast<size_t>(std::clamp(at, 0.f, static_cast<float>(m_items.size())));
            }

            void Move(int delta)
            {
                if (m_items.empty())
                    return;
                const size_t to = delta < 0 ? (m_focus == 0 ? 0 : m_focus - 1) : std::min(m_items.size(), m_focus + 2);
                m_focus = MoveItems(m_items, { m_focus }, to).front();
                EnsureVisible(m_focus);
                Invalidate();
            }

            void Remove()
            {
                if (m_items.size() <= 2)
                {
                    MessageBeep(MB_OK);   // a merge needs two files
                    return;
                }
                const auto next = RemoveItems(m_items, { m_focus });
                m_focus = next < 0 ? 0 : static_cast<size_t>(next);
                ClampScroll();
                Invalidate();
            }

            static std::wstring Details(const Item& item)
            {
                const std::wstring pages = item.pages == -1   ? L"counting pages…"
                                           : item.pages == -2 ? L"cannot be read"
                                           : item.pages == 1  ? L"1 page"
                                                              : std::format(L"{} pages", item.pages);
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
                const std::wstring heading = std::format(L"Merge {} PDFs, top to bottom", m_items.size());
                DrawText(heading, TextFormat(18.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(kMargin, 8.f, size.width - kMargin, 34.f), c.text);
                DrawText(L"Drag to reorder · Alt+↑/↓ moves · Delete leaves one out", TextFormat(12.f),
                         D2D1::RectF(kMargin, 32.f, size.width - kMargin, 50.f), c.textDim);

                const auto list = ListRect(size);
                if (auto* b = Brush(c.surface))
                    rt->FillRoundedRectangle(D2D1::RoundedRect(list, 6.f, 6.f), b);
                rt->PushAxisAlignedClip(list, D2D1_ANTIALIAS_MODE_ALIASED);
                for (size_t i = 0; i < m_items.size(); ++i)
                {
                    const float top = list.top + static_cast<float>(i) * kRowH - m_scroll;
                    if (top + kRowH < list.top || top > list.bottom)
                        continue;
                    const auto row = D2D1::RectF(list.left + 4.f, top + 2.f, list.right - 4.f, top + kRowH - 2.f);
                    const bool dragged = m_dragging && i == m_focus;
                    if (i == m_focus)
                        if (auto* b = Brush(dragged ? theme::WithAlpha(c.selection, 0.5f) : c.selection))
                            rt->FillRoundedRectangle(D2D1::RoundedRect(row, 4.f, 4.f), b);
                    const auto& item = m_items[i];
                    DrawText(std::format(L"{}", i + 1), TextFormat(13.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING),
                             D2D1::RectF(row.left, row.top, row.left + 26.f, row.bottom), c.textDim);
                    const float detailsW = 180.f;
                    DrawText(FileNameOf(item.path), TextFormat(14.f), D2D1::RectF(row.left + 40.f, row.top, row.right - detailsW - 8.f, row.bottom), c.text);
                    DrawText(Details(item), TextFormat(12.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING),
                             D2D1::RectF(row.right - detailsW, row.top, row.right - 10.f, row.bottom), c.textDim);
                }
                if (m_dragging)
                {
                    const float y = list.top + static_cast<float>(m_insert) * kRowH - m_scroll;
                    if (auto* b = Brush(c.accent))
                        rt->FillRectangle(D2D1::RectF(list.left + 4.f, y - 1.5f, list.right - 4.f, y + 1.5f), b);
                }
                rt->PopAxisAlignedClip();

                const std::wstring summary = counting ? std::wstring(L"Counting pages…") : std::format(L"{} pages in all", total);
                const float y = size.height - kFooter;
                DrawText(summary, TextFormat(12.f), D2D1::RectF(kMargin, y, m_buttons[0].rect.left - 8.f, size.height), c.textDim);
                m_buttons[1].enabled = m_items.size() >= 2;
                DrawButtons(rt);
            }

            bool OnMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result) override
            {
                switch (msg)
                {
                case kPagesCounted:
                {
                    std::unique_ptr<Counted> counted(reinterpret_cast<Counted*>(lParam));
                    for (auto& i : m_items)
                        if (i.path == counted->path)
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
                    const auto row = RowAt(p);
                    if (row >= 0 && static_cast<size_t>(row) < m_items.size())
                    {
                        m_focus = static_cast<size_t>(row);
                        m_pressAt = p;
                        m_pressed = true;
                        SetCapture(Hwnd());
                        Invalidate();
                    }
                    return true;
                }
                case WM_MOUSEMOVE:
                    if (m_pressed)
                    {
                        const auto p = DipFromLParam(lParam);
                        const float threshold = static_cast<float>(GetSystemMetrics(SM_CYDRAG)) / Scale();
                        if (!m_dragging && std::abs(p.y - m_pressAt.y) > threshold)
                            m_dragging = true;
                        if (m_dragging)
                        {
                            // Scroll when dragging past the list's edge.
                            const auto list = ListRect(ClientDip());
                            if (p.y < list.top)
                                m_scroll -= 8.f;
                            else if (p.y > list.bottom)
                                m_scroll += 8.f;
                            ClampScroll();
                            m_insert = InsertAt(p);
                            Invalidate();
                        }
                    }
                    return true;
                case WM_LBUTTONUP:
                    if (m_pressed)
                    {
                        m_pressed = false;
                        ReleaseCapture();
                        if (m_dragging)
                        {
                            m_dragging = false;
                            m_focus = MoveItems(m_items, { m_focus }, m_insert).front();
                        }
                        Invalidate();
                    }
                    return true;
                case WM_CAPTURECHANGED:
                    if (m_pressed && reinterpret_cast<HWND>(lParam) != Hwnd())
                    {
                        m_pressed = m_dragging = false;
                        Invalidate();
                    }
                    return false;
                case WM_LBUTTONDBLCLK:
                    return true;
                case WM_MOUSEWHEEL:
                    m_scroll -= static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA * kRowH * 2.f;
                    ClampScroll();
                    Invalidate();
                    return true;
                default:
                    result = 0;
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
                case VK_RETURN:
                    Finish();   // may delete this
                    return true;
                case VK_ESCAPE:
                    Close();   // deletes this
                    return true;
                default:
                    return false;   // Alt+F4 and the rest go to Windows
                }
                EnsureVisible(m_focus);
                Invalidate();
                return true;
            }

            std::vector<Item> m_items;
            std::function<void(std::vector<std::wstring>)> m_done;
            std::shared_ptr<std::atomic<bool>> m_stop = std::make_shared<std::atomic<bool>>(false);
            size_t m_focus = 0;
            float m_scroll = 0;
            bool m_pressed = false, m_dragging = false;
            D2D1_POINT_2F m_pressAt{};
            size_t m_insert = 0;
        };

        void OpenMerge(HWND, const std::vector<std::wstring>& files, std::function<void(std::vector<std::wstring>)> done)
        {
            auto window = std::make_unique<MergeWindow>(files, std::move(done));
            if (window->Open())
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
