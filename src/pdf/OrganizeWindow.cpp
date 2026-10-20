// Organize PDF Pages: every page of the dropped PDFs as a thumbnail grid (rendered by Windows.Data.Pdf
// on a worker). Drag to reorder; R / Shift+R rotate, Ctrl+D duplicates, Delete removes, Ctrl+Z
// undoes; Save writes a new PDF next to the first one. Installs itself into pdf::Editors().
#include "Log.h"
#include "pdf/EditorWindow.h"
#include "pdf/PageRenderer.h"
#include "pdf/PdfDoc.h"
#include "pdf/PdfEditors.h"
#include "pdf/PdfModule.h"

#include <objbase.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <map>
#include <memory>
#include <thread>

namespace sd::pdf
{
    namespace
    {
        constexpr UINT kCounted = WM_APP + 1;   // lParam: Counted*, ours to delete
        constexpr UINT kThumb = WM_APP + 2;     // lParam: Thumb*, ours to delete

        // DIPs
        constexpr float kMargin = 16.f, kToolbar = 56.f, kFooter = 64.f;
        constexpr float kCellW = 164.f, kCellH = 196.f, kThumbBox = 136.f;
        constexpr float kButtonW = 104.f, kButtonH = 32.f;

        struct Counted
        {
            bool ok = false;
            std::vector<uint32_t> counts;
            std::wstring error;
        };

        struct Thumb
        {
            uint32_t file = 0, page = 0;
            Bitmap pixels;
        };

        struct CachedThumb
        {
            Bitmap pixels;
            Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
            unsigned generation = 0;
        };

        class OrganizeWindow final : public EditorWindow
        {
        public:
            OrganizeWindow(const std::vector<std::wstring>& files, std::function<void(PagePlan)> done)
                : m_files(files), m_done(std::move(done))
            {
                auto button = [&](const wchar_t* text, bool primary, std::function<void()> action) {
                    m_buttons.push_back(Button{ text, 0, primary, true, {}, std::move(action) });
                };
                button(L"Rotate left", false, [this] { Rotate(-90); });
                button(L"Rotate right", false, [this] { Rotate(90); });
                button(L"Duplicate", false, [this] { Duplicate(); });
                button(L"Delete", false, [this] { Remove(); });
                button(L"Undo", false, [this] { Undo(); });
                button(L"Cancel", false, [this] { Cancel(); });
                button(L"Save", true, [this] { Save(); });
            }

            bool Open()
            {
                const std::wstring title = m_files.size() == 1 ? std::format(L"Organize pages · {}", FileNameOf(m_files.front()))
                                                               : std::format(L"Organize pages · {} PDFs", m_files.size());
                if (!Create(title, 900, 640, 560, 400))
                    return false;
                StartLoading();
                return true;
            }

        private:
            enum ButtonId { kRotateLeft, kRotateRight, kDuplicate, kDelete, kUndo, kCancel, kSave };

            // ---- Loading --------------------------------------------------------------------------

            void StartLoading()
            {
                const auto thumbPx = static_cast<uint32_t>(std::lround(kThumbBox * Scale()));
                std::thread([hwnd = Hwnd(), files = m_files, stop = m_stop, thumbPx] {
                    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // WinRT rendering blocks: MTA
                    auto counted = std::make_unique<Counted>();
                    try
                    {
                        counted->ok = CountPages(files, counted->counts, counted->error);
                    }
                    catch (...)
                    {
                        counted->error = L"the PDF could not be read";
                    }
                    const bool ok = counted->ok;
                    const auto counts = counted->counts;
                    if (PostMessageW(hwnd, kCounted, 0, reinterpret_cast<LPARAM>(counted.get())))
                        counted.release();
                    if (ok && RenderingAvailable())
                    {
                        for (uint32_t f = 0; f < files.size() && !stop->load(); ++f)
                        {
                            std::wstring error;
                            const auto doc = OpenForRendering(files[f], error);
                            if (!doc)
                            {
                                log::Warn(L"organize: no thumbnails for {}: {}", files[f], error);
                                continue;
                            }
                            const uint32_t pages = std::min(counts[f], doc->PageCount());
                            for (uint32_t p = 0; p < pages && !stop->load(); ++p)
                            {
                                double w = 0, h = 0;
                                if (!doc->PageSize(p, w, h))
                                    continue;
                                const double s = thumbPx / std::max(w, h);
                                auto thumb = std::make_unique<Thumb>();
                                thumb->file = f;
                                thumb->page = p;
                                if (!doc->Render(p, std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(w * s))),
                                                 std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(h * s))), thumb->pixels, error))
                                    continue;
                                if (PostMessageW(hwnd, kThumb, 0, reinterpret_cast<LPARAM>(thumb.get())))
                                    thumb.release();
                                else
                                    break;   // the window is gone
                            }
                        }
                    }
                    if (SUCCEEDED(com))
                        CoUninitialize();
                }).detach();
            }

            void OnClosing() override { m_stop->store(true); }

            // ---- Edits ----------------------------------------------------------------------------

            std::vector<size_t> Selection() const
            {
                std::vector<size_t> s;
                for (size_t i = 0; i < m_selected.size(); ++i)
                    if (m_selected[i])
                        s.push_back(i);
                if (s.empty() && m_focus < m_plan.size())
                    s.push_back(m_focus);
                return s;
            }

            void Select(const std::vector<size_t>& indices)
            {
                m_selected.assign(m_plan.size(), false);
                for (const size_t i : indices)
                    if (i < m_selected.size())
                        m_selected[i] = true;
                if (!indices.empty())
                    m_focus = m_anchor = indices.front();
                m_focus = m_plan.empty() ? 0 : std::min(m_focus, m_plan.size() - 1);
            }

            void Remember()
            {
                m_undo.push_back({ m_plan, m_selected });
                if (m_undo.size() > 200)
                    m_undo.erase(m_undo.begin());
            }

            void Changed()
            {
                ClampScroll();
                EnsureVisible(m_focus);
                Invalidate();
            }

            void Rotate(int degrees)
            {
                if (m_plan.empty())
                    return;
                Remember();
                const auto s = Selection();
                RotatePages(m_plan, s, degrees);
                Select(s);
                Changed();
            }

            void Duplicate()
            {
                if (m_plan.empty())
                    return;
                Remember();
                Select(DuplicatePages(m_plan, Selection()));
                Changed();
            }

            void Remove()
            {
                if (m_plan.empty())
                    return;
                Remember();
                const auto focus = RemoveItems(m_plan, Selection());
                Select(focus < 0 ? std::vector<size_t>{} : std::vector<size_t>{ static_cast<size_t>(focus) });
                Changed();
            }

            void MoveSelection(size_t insertBefore)
            {
                Remember();
                Select(MoveItems(m_plan, Selection(), insertBefore));
                Changed();
            }

            void Undo()
            {
                if (m_undo.empty())
                {
                    MessageBeep(MB_OK);
                    return;
                }
                m_plan = std::move(m_undo.back().first);
                m_selected = std::move(m_undo.back().second);
                m_undo.pop_back();
                m_focus = m_plan.empty() ? 0 : std::min(m_focus, m_plan.size() - 1);
                Changed();
            }

            void Save()
            {
                if (m_plan.empty() || !m_loaded)
                {
                    MessageBeep(MB_OK);
                    return;
                }
                auto plan = std::move(m_plan);
                auto done = std::move(m_done);
                Close();   // deletes this
                done(std::move(plan));
            }

            void Cancel()
            {
                if (!m_undo.empty() &&
                    MessageBoxW(Hwnd(), L"Close without saving? Your changes to the pages will be lost.", L"Organize pages",
                                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
                    return;
                Close();   // deletes this
            }

            // ---- Geometry -------------------------------------------------------------------------

            D2D1_RECT_F GridRect(D2D1_SIZE_F size) const { return D2D1::RectF(kMargin, kToolbar, size.width - kMargin, size.height - kFooter); }

            size_t Columns() const
            {
                const auto grid = GridRect(ClientDip());
                return std::max<size_t>(1, static_cast<size_t>((grid.right - grid.left) / kCellW));
            }

            float GridLeft() const
            {
                const auto grid = GridRect(ClientDip());
                return grid.left + ((grid.right - grid.left) - static_cast<float>(Columns()) * kCellW) / 2.f;
            }

            D2D1_RECT_F CellRect(size_t i) const
            {
                const size_t cols = Columns();
                const float x = GridLeft() + static_cast<float>(i % cols) * kCellW;
                const float y = GridRect(ClientDip()).top + static_cast<float>(i / cols) * kCellH - m_scroll;
                return D2D1::RectF(x, y, x + kCellW, y + kCellH);
            }

            long long ItemAt(D2D1_POINT_2F p) const
            {
                const auto grid = GridRect(ClientDip());
                if (p.y < grid.top || p.y > grid.bottom)
                    return -1;
                for (size_t i = 0; i < m_plan.size(); ++i)
                {
                    const auto r = CellRect(i);
                    if (p.x >= r.left + 4.f && p.x < r.right - 4.f && p.y >= r.top + 4.f && p.y < r.bottom - 4.f)
                        return static_cast<long long>(i);
                }
                return -1;
            }

            size_t InsertAt(D2D1_POINT_2F p) const
            {
                const size_t cols = Columns();
                const auto grid = GridRect(ClientDip());
                const float rowF = (p.y - grid.top + m_scroll) / kCellH;
                const size_t rows = (m_plan.size() + cols - 1) / cols;
                const auto row = static_cast<size_t>(std::clamp(rowF, 0.f, static_cast<float>(std::max<size_t>(rows, 1) - 1)));
                const auto col = static_cast<size_t>(std::clamp((p.x - GridLeft()) / kCellW + 0.5f, 0.f, static_cast<float>(cols)));
                return std::min(m_plan.size(), row * cols + col);
            }

            void ClampScroll()
            {
                const auto grid = GridRect(ClientDip());
                const size_t cols = Columns();
                const float content = static_cast<float>((m_plan.size() + cols - 1) / cols) * kCellH;
                m_scroll = std::clamp(m_scroll, 0.f, std::max(0.f, content - (grid.bottom - grid.top)));
            }

            void EnsureVisible(size_t i)
            {
                if (i >= m_plan.size())
                    return;
                const auto grid = GridRect(ClientDip());
                const float top = static_cast<float>(i / Columns()) * kCellH, height = grid.bottom - grid.top;
                if (top < m_scroll)
                    m_scroll = top;
                else if (top + kCellH > m_scroll + height)
                    m_scroll = top + kCellH - height;
                ClampScroll();
            }

            void Layout(D2D1_SIZE_F size) override
            {
                // Toolbar on the left of the top row; Cancel and Save at the bottom right.
                const float widths[] = { 104.f, 112.f, 96.f, 84.f, 76.f };
                float x = kMargin;
                const float y = (kToolbar - kButtonH) / 2.f;
                for (int i = kRotateLeft; i <= kUndo; ++i)
                {
                    m_buttons[i].rect = D2D1::RectF(x, y, x + widths[i], y + kButtonH);
                    x += widths[i] + 8.f;
                }
                const float by = size.height - kFooter + (kFooter - kButtonH) / 2.f;
                m_buttons[kSave].rect = D2D1::RectF(size.width - kMargin - kButtonW, by, size.width - kMargin, by + kButtonH);
                m_buttons[kCancel].rect = D2D1::RectF(m_buttons[kSave].rect.left - 8.f - kButtonW, by, m_buttons[kSave].rect.left - 8.f, by + kButtonH);
                ClampScroll();
            }

            // ---- Drawing --------------------------------------------------------------------------

            ID2D1Bitmap* BitmapFor(const PageRef& ref)
            {
                const auto it = m_thumbs.find({ ref.file, ref.page });
                if (it == m_thumbs.end())
                    return nullptr;
                auto& t = it->second;
                if (!t.bitmap || t.generation != DeviceGeneration())
                {
                    t.bitmap.Reset();
                    const auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                    // Opaque pixels: straight and premultiplied alpha are the same.
                    if (FAILED(Target()->CreateBitmap(D2D1::SizeU(t.pixels.width, t.pixels.height), t.pixels.bgra.data(), t.pixels.width * 4,
                                                      props, &t.bitmap)))
                        return nullptr;
                    t.generation = DeviceGeneration();
                }
                return t.bitmap.Get();
            }

            void DrawPage(ID2D1RenderTarget* rt, size_t i, const D2D1_RECT_F& cell)
            {
                const auto& c = Theme();
                const auto& ref = m_plan[i];
                const bool selected = i < m_selected.size() && m_selected[i];
                if (selected)
                    if (auto* b = Brush(c.selection))
                        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cell.left + 4.f, cell.top + 4.f, cell.right - 4.f, cell.bottom - 4.f), 6.f, 6.f), b);
                if (i == m_focus && m_showFocus)
                    if (auto* b = Brush(c.textDim))
                        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cell.left + 4.5f, cell.top + 4.5f, cell.right - 4.5f, cell.bottom - 4.5f), 6.f, 6.f), b, 1.f);

                // The page, turned by its extra rotation, fitted in the box.
                const D2D1_POINT_2F centre = D2D1::Point2F((cell.left + cell.right) / 2.f, cell.top + 14.f + kThumbBox / 2.f);
                auto* bitmap = BitmapFor(ref);
                float bw = 0.773f, bh = 1.f;   // A4/Letter-ish until the thumbnail arrives
                if (bitmap)
                {
                    const auto s = bitmap->GetSize();
                    bw = s.width;
                    bh = s.height;
                }
                const bool sideways = ref.rotation == 90 || ref.rotation == 270;
                const float fit = kThumbBox / std::max(sideways ? bh : bw, sideways ? bw : bh);
                const float hw = bw * fit / 2.f, hh = bh * fit / 2.f;
                const auto page = D2D1::RectF(centre.x - hw, centre.y - hh, centre.x + hw, centre.y + hh);
                D2D1_MATRIX_3X2_F old;
                rt->GetTransform(&old);
                rt->SetTransform(D2D1::Matrix3x2F::Rotation(static_cast<float>(ref.rotation), centre) * old);
                if (auto* b = Brush(D2D1::ColorF(1, 1, 1, 1)))
                    rt->FillRectangle(page, b);
                if (bitmap)
                    rt->DrawBitmap(bitmap, page, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                else
                    DrawText(std::format(L"{}", ref.page + 1), TextFormat(20.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), page,
                             D2D1::ColorF(0, 0, 0, 0.35f));
                if (auto* b = Brush(selected ? c.accent : c.line))
                    rt->DrawRectangle(page, b, selected ? 2.f : 1.f);
                rt->SetTransform(old);

                std::wstring label = m_files.size() > 1 ? std::format(L"{} · page {}", static_cast<wchar_t>(L'A' + std::min<uint32_t>(ref.file, 25)), ref.page + 1)
                                                        : std::format(L"Page {}", ref.page + 1);
                if (ref.rotation)
                    label += std::format(L" · {}°", ref.rotation);
                DrawText(label, TextFormat(12.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER),
                         D2D1::RectF(cell.left + 6.f, cell.top + 14.f + kThumbBox + 6.f, cell.right - 6.f, cell.bottom - 8.f), selected ? c.text : c.textDim);
            }

            void Paint(ID2D1RenderTarget* rt, D2D1_SIZE_F size) override
            {
                const auto& c = Theme();
                const auto grid = GridRect(size);
                if (auto* b = Brush(c.surface))
                    rt->FillRoundedRectangle(D2D1::RoundedRect(grid, 6.f, 6.f), b);
                if (!m_loaded)
                    DrawText(m_error.empty() ? std::wstring(L"Reading pages…") : L"This PDF cannot be opened: " + m_error,
                             TextFormat(14.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), grid, c.textDim);
                else if (m_plan.empty())
                    DrawText(L"No pages left. Undo (Ctrl+Z) brings them back.", TextFormat(14.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER),
                             grid, c.textDim);
                rt->PushAxisAlignedClip(grid, D2D1_ANTIALIAS_MODE_ALIASED);
                for (size_t i = 0; i < m_plan.size(); ++i)
                {
                    const auto cell = CellRect(i);
                    if (cell.bottom < grid.top || cell.top > grid.bottom)
                        continue;
                    DrawPage(rt, i, cell);
                }
                if (m_dragging)
                {
                    const size_t cols = Columns();
                    const size_t at = m_insert;
                    // The bar sits before item `at`, or after the last item of its row.
                    const bool rowEnd = at > 0 && (at % cols == 0 || at == m_plan.size());
                    const auto cell = CellRect(rowEnd ? at - 1 : at);
                    const float x = rowEnd ? cell.right : cell.left;
                    if (auto* b = Brush(c.accent))
                        rt->FillRectangle(D2D1::RectF(x - 2.f, cell.top + 8.f, x + 2.f, cell.bottom - 8.f), b);
                }
                rt->PopAxisAlignedClip();

                size_t selected = 0;
                for (const bool s : m_selected)
                    selected += s ? 1 : 0;
                std::wstring status = m_plan.size() == 1 ? L"1 page" : std::format(L"{} pages", m_plan.size());
                if (selected)
                    status += std::format(L" · {} selected", selected);
                status += L" · drag to move · R rotates · Ctrl+D duplicates · Del deletes";
                DrawText(m_loaded ? status : std::wstring(), TextFormat(12.f),
                         D2D1::RectF(kMargin, size.height - kFooter, m_buttons[kCancel].rect.left - 8.f, size.height), c.textDim);

                const bool any = m_loaded && !m_plan.empty();
                for (int i = kRotateLeft; i <= kDelete; ++i)
                    m_buttons[i].enabled = any;
                m_buttons[kUndo].enabled = !m_undo.empty();
                m_buttons[kSave].enabled = any;
                DrawButtons(rt);
            }

            // ---- Input ----------------------------------------------------------------------------

            void SelectRange(size_t from, size_t to)
            {
                m_selected.assign(m_plan.size(), false);
                for (size_t k = std::min(from, to); k <= std::max(from, to) && k < m_plan.size(); ++k)
                    m_selected[k] = true;
            }

            void FocusTo(long long target)
            {
                if (m_plan.empty())
                    return;
                const auto to = static_cast<size_t>(std::clamp<long long>(target, 0, static_cast<long long>(m_plan.size()) - 1));
                m_focus = to;
                if (Shift())
                    SelectRange(m_anchor, to);
                else if (!Ctrl())
                {
                    Select({ to });
                }
                m_showFocus = true;
                EnsureVisible(m_focus);
                Invalidate();
            }

            bool OnKey(UINT key, bool alt)
            {
                const auto cols = static_cast<long long>(Columns());
                const auto focus = static_cast<long long>(m_focus);
                const bool move = (alt || Ctrl()) && !m_plan.empty();
                const auto s = Selection();
                switch (key)
                {
                case VK_LEFT:
                    if (move)
                        MoveSelection(s.front() == 0 ? 0 : s.front() - 1);
                    else
                        FocusTo(focus - 1);
                    return true;
                case VK_RIGHT:
                    if (move)
                        MoveSelection(std::min(m_plan.size(), s.back() + 2));
                    else
                        FocusTo(focus + 1);
                    return true;
                case VK_UP:
                    if (move)
                        MoveSelection(s.front() < static_cast<size_t>(cols) ? 0 : s.front() - static_cast<size_t>(cols));
                    else
                        FocusTo(focus - cols);
                    return true;
                case VK_DOWN:
                    if (move)
                        MoveSelection(std::min(m_plan.size(), s.back() + 1 + static_cast<size_t>(cols)));
                    else
                        FocusTo(focus + cols);
                    return true;
                case VK_HOME:
                    FocusTo(0);
                    return true;
                case VK_END:
                    FocusTo(static_cast<long long>(m_plan.size()) - 1);
                    return true;
                case VK_SPACE:
                    if (m_focus < m_selected.size())
                    {
                        if (Ctrl())
                            m_selected[m_focus] = !m_selected[m_focus];
                        else
                            Select({ m_focus });
                        m_anchor = m_focus;
                        Invalidate();
                    }
                    return true;
                case 'A':
                    if (Ctrl())
                    {
                        m_selected.assign(m_plan.size(), true);
                        Invalidate();
                    }
                    return true;
                case 'R':
                    if (!alt)
                        Rotate(Shift() ? -90 : 90);
                    return true;
                case 'D':
                    if (Ctrl())
                        Duplicate();
                    return true;
                case 'Z':
                    if (Ctrl())
                        Undo();
                    return true;
                case VK_DELETE:
                case VK_BACK:
                    Remove();
                    return true;
                case VK_RETURN:
                    Save();   // may delete this
                    return true;
                case VK_ESCAPE:
                    if (m_dragging)
                    {
                        m_dragging = m_pressed = false;
                        ReleaseCapture();
                        Invalidate();
                    }
                    else
                        Cancel();   // may delete this
                    return true;
                default:
                    return false;
                }
            }

            bool OnMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result) override
            {
                result = 0;
                switch (msg)
                {
                case kCounted:
                {
                    std::unique_ptr<Counted> counted(reinterpret_cast<Counted*>(lParam));
                    if (counted->ok)
                    {
                        m_loaded = true;
                        m_plan = AllPages(counted->counts);
                        Select(m_plan.empty() ? std::vector<size_t>{} : std::vector<size_t>{ 0 });
                    }
                    else
                        m_error = counted->error.empty() ? std::wstring(L"unknown error") : counted->error;
                    ClampScroll();
                    Invalidate();
                    return true;
                }
                case kThumb:
                {
                    std::unique_ptr<Thumb> thumb(reinterpret_cast<Thumb*>(lParam));
                    m_thumbs[{ thumb->file, thumb->page }] = CachedThumb{ std::move(thumb->pixels), nullptr, 0 };
                    Invalidate();
                    return true;
                }
                case WM_KEYDOWN:
                case WM_SYSKEYDOWN:
                    if (!m_loaded && wParam != VK_ESCAPE)
                        return false;
                    return OnKey(static_cast<UINT>(wParam), msg == WM_SYSKEYDOWN);
                case WM_LBUTTONDOWN:
                {
                    SetFocus(Hwnd());
                    const auto p = DipFromLParam(lParam);
                    const auto at = ItemAt(p);
                    m_clickedSelected = false;
                    if (at < 0)
                    {
                        if (!Ctrl() && GridContains(p))
                        {
                            m_selected.assign(m_plan.size(), false);
                            Invalidate();
                        }
                        return true;
                    }
                    const auto i = static_cast<size_t>(at);
                    if (Ctrl())
                    {
                        m_selected[i] = !m_selected[i];
                        m_anchor = i;
                    }
                    else if (Shift())
                        SelectRange(m_anchor, i);
                    else if (m_selected[i])
                        m_clickedSelected = true;   // keep the group for a drag; a plain click narrows it on release
                    else
                        Select({ i });
                    m_focus = i;
                    m_showFocus = false;
                    m_pressed = true;
                    m_pressAt = p;
                    SetCapture(Hwnd());
                    Invalidate();
                    return true;
                }
                case WM_MOUSEMOVE:
                    if (m_pressed)
                    {
                        const auto p = DipFromLParam(lParam);
                        const float threshold = static_cast<float>(GetSystemMetrics(SM_CXDRAG)) / Scale();
                        if (!m_dragging && (std::abs(p.x - m_pressAt.x) > threshold || std::abs(p.y - m_pressAt.y) > threshold))
                        {
                            m_dragging = true;
                            if (!m_selected[m_focus])
                                m_selected[m_focus] = true;
                        }
                        if (m_dragging)
                        {
                            const auto grid = GridRect(ClientDip());
                            if (p.y < grid.top + 24.f)
                                m_scroll -= 12.f;
                            else if (p.y > grid.bottom - 24.f)
                                m_scroll += 12.f;
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
                            MoveSelection(m_insert);
                        }
                        else if (m_clickedSelected)
                            Select({ m_focus });
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
                    m_scroll -= static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA * kCellH * 0.5f;
                    ClampScroll();
                    Invalidate();
                    return true;
                default:
                    return false;
                }
            }

            bool GridContains(D2D1_POINT_2F p) const
            {
                const auto g = GridRect(ClientDip());
                return p.x >= g.left && p.x < g.right && p.y >= g.top && p.y < g.bottom;
            }

            std::vector<std::wstring> m_files;
            std::function<void(PagePlan)> m_done;
            std::shared_ptr<std::atomic<bool>> m_stop = std::make_shared<std::atomic<bool>>(false);
            bool m_loaded = false;
            std::wstring m_error;
            PagePlan m_plan;
            std::vector<bool> m_selected;
            std::vector<std::pair<PagePlan, std::vector<bool>>> m_undo;
            std::map<std::pair<uint32_t, uint32_t>, CachedThumb> m_thumbs;
            size_t m_focus = 0, m_anchor = 0, m_insert = 0;
            float m_scroll = 0;
            bool m_pressed = false, m_dragging = false, m_clickedSelected = false, m_showFocus = false;
            D2D1_POINT_2F m_pressAt{};
        };

        void OpenOrganize(HWND, const std::vector<std::wstring>& files, std::function<void(PagePlan)> done)
        {
            auto window = std::make_unique<OrganizeWindow>(files, std::move(done));
            if (window->Open())
                window.release();   // owns itself now
            else
                log::Warn(L"organize: the window could not open");
        }

        const bool g_installed = [] {
            Editors().organize = &OpenOrganize;
            return true;
        }();
    }
}
