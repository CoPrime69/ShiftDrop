// The page view (OrganizeWindow.h): Organize PDF Pages, and the pages of Merge PDFs and Edit PDF.
// Every page of the files as a thumbnail grid rendered by Windows.Data.Pdf on a worker; drag to
// reorder, R / Shift+R rotate, Ctrl+D duplicates, Delete leaves out, Ctrl+Z undoes. Merge and Edit
// add a large preview of the selected page and per-page Edit through the existing picture editors
// (PageEdits.h). Organize installs itself into pdf::Editors().
#include "Ask.h"
#include "Log.h"
#include "PathUtil.h"
#include "Theme.h"
#include "UiThread.h"
#include "Utf.h"
#include "canvas/Editor.h"
#include "EditorFrame.h"
#include "pdf/OrganizeWindow.h"
#include "pdf/PageEdits.h"
#include "pdf/PageRenderer.h"
#include "pdf/PdfDoc.h"
#include "pdf/PdfEditors.h"
#include "pdf/PdfModule.h"
#include "pdf/ScrollList.h"
#include "pdf/Wic.h"

#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace sd::pdf
{
    namespace
    {
        constexpr UINT kCounted = WM_APP + 1;      // lParam: Counted*, ours to delete
        constexpr UINT kRendered = WM_APP + 2;     // lParam: RenderedPage*, ours to delete
        constexpr UINT kEditReady = WM_APP + 3;    // lParam: EditBatch*, ours to delete
        constexpr UINT kEditApplied = WM_APP + 4;  // wParam: serial; lParam: nlohmann::json*, ours to delete
        constexpr UINT kEditClosed = WM_APP + 5;   // wParam: serial
        constexpr UINT kEditDone = WM_APP + 6;     // lParam: EditBatch*, ours to delete

        // DIPs
        constexpr float kMargin = 16.f, kToolbar = 56.f, kHint = 22.f, kFooter = 64.f;
        constexpr float kCellW = 164.f, kCellH = 204.f, kThumbBox = 136.f;
        constexpr float kButtonW = 104.f, kButtonH = 32.f, kNavH = 44.f, kCaptionH = 30.f;
        constexpr size_t kMaxBitmaps = 240;   // device bitmaps kept beyond the ones on screen

        // Tag colours for the files, told apart in light and dark themes.
        constexpr uint32_t kTagColors[] = { 0x0078D4, 0xC239B3, 0x10893E, 0xCA5010, 0x8764B8, 0x038387, 0xD13438, 0x986F0B };

        enum MenuId : UINT
        {
            kMenuToolFirst = 100,   // + index into PageTools()
            kMenuRevert = 200,
            kMenuRotateRight,
            kMenuRotateLeft,
            kMenuDuplicate,
            kMenuDelete,
            kMenuViewLarge,
        };

        struct Counted
        {
            bool ok = false;
            std::vector<uint32_t> counts;   // per file of the request; 0 for files not shown
            std::wstring error;
        };

        // ---- Rendering ----------------------------------------------------------------------------

        // A page to render: page `page` of `path` (a source file, or an edited page's one-page PDF),
        // fitted in `maxSide` pixels.
        struct RenderJob
        {
            std::wstring path;
            uint32_t page = 0;
            uint32_t maxSide = 0;
            bool preview = false;
        };

        struct RenderedPage
        {
            RenderJob job;
            Bitmap pixels;   // empty when it failed
            double widthPt = 0, heightPt = 0;
        };

        // One worker renders what the window asks for, the preview before any thumbnail, and posts
        // each page back as kRendered. The window never waits for it.
        class RenderQueue
        {
        public:
            ~RenderQueue()
            {
                std::vector<std::wstring> remove;
                {
                    std::scoped_lock lock(m_state->mutex);
                    m_state->stop = true;
                    if (!m_state->started)
                        remove = std::move(m_state->removeOnExit);
                }
                m_state->wake.notify_all();
                for (const auto& folder : remove)
                    RemoveEditFolder(folder);
            }

            // The thumbnails still needed, most wanted first; replaces the previous list.
            void Thumbnails(HWND hwnd, std::vector<RenderJob> wanted)
            {
                {
                    std::scoped_lock lock(m_state->mutex);
                    m_state->thumbs.assign(wanted.begin(), wanted.end());
                }
                Start(hwnd);
            }

            // The preview to render next; replaces one not started yet.
            void Preview(HWND hwnd, RenderJob job)
            {
                job.preview = true;
                {
                    std::scoped_lock lock(m_state->mutex);
                    m_state->preview = std::move(job);
                }
                Start(hwnd);
            }

            // A folder (PageEdits.h) to remove once the worker has let go of the pages in it.
            void RemoveWhenDone(std::wstring folder)
            {
                std::scoped_lock lock(m_state->mutex);
                m_state->removeOnExit.push_back(std::move(folder));
            }

        private:
            struct State
            {
                std::mutex mutex;
                std::condition_variable wake;
                bool stop = false, started = false;
                std::deque<RenderJob> thumbs;
                std::optional<RenderJob> preview;
                HWND hwnd = nullptr;
                std::vector<std::wstring> removeOnExit;
            };

            static void Work(const std::shared_ptr<State>& state)
            {
                std::map<std::wstring, std::unique_ptr<IRenderedPdf>> docs;
                std::set<std::wstring> broken;
                for (;;)
                {
                    RenderJob job;
                    HWND target = nullptr;
                    {
                        std::unique_lock lock(state->mutex);
                        state->wake.wait(lock, [&] { return state->stop || state->preview || !state->thumbs.empty(); });
                        if (state->stop)
                            break;
                        if (state->preview)
                        {
                            job = std::move(*state->preview);
                            state->preview.reset();
                        }
                        else
                        {
                            job = std::move(state->thumbs.front());
                            state->thumbs.pop_front();
                        }
                        target = state->hwnd;
                    }
                    auto out = std::make_unique<RenderedPage>();
                    out->job = job;
                    auto& doc = docs[job.path];
                    if (!doc && !broken.contains(job.path))
                    {
                        std::wstring error;
                        doc = OpenForRendering(job.path, error);
                        if (!doc)
                        {
                            broken.insert(job.path);
                            log::Warn(L"pages: no pictures of {}: {}", job.path, error);
                        }
                    }
                    double w = 0, h = 0;
                    if (doc && job.page < doc->PageCount() && doc->PageSize(job.page, w, h) && w > 0 && h > 0)
                    {
                        out->widthPt = w;
                        out->heightPt = h;
                        uint32_t pw = 0, ph = 0;
                        PixelSize(w, h, 1e6, std::max<uint32_t>(job.maxSide, 1), pw, ph);   // fitted in maxSide
                        std::wstring error;
                        if (!doc->Render(job.page, pw, ph, out->pixels, error))
                            out->pixels = {};
                    }
                    if (!PostOwned(target, kRendered, std::move(out)) && !IsWindow(target))
                        break;   // the window is gone
                }
                docs.clear();   // lets go of the files before their folder is removed
                std::vector<std::wstring> remove;
                {
                    std::scoped_lock lock(state->mutex);
                    remove = std::move(state->removeOnExit);
                }
                for (const auto& folder : remove)
                    RemoveEditFolder(folder);
            }

            void Start(HWND hwnd)
            {
                {
                    std::scoped_lock lock(m_state->mutex);
                    m_state->hwnd = hwnd;
                    if (m_state->started || !RenderingAvailable())
                        return;
                    m_state->started = true;
                }
                m_state->wake.notify_all();
                RunMtaDetached([state = m_state] { Work(state); });   // WinRT rendering blocks: MTA
            }

            std::shared_ptr<State> m_state = std::make_shared<State>();
        };

        // Rendered pixels and their device bitmap, rebuilt when the window's device changes.
        struct PagePicture
        {
            Bitmap pixels;
            double widthPt = 0, heightPt = 0;
            Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
            unsigned generation = 0;
            uint64_t used = 0;   // the paint that last drew it
        };

        // ---- Editing pages ------------------------------------------------------------------------

        struct EditItem
        {
            size_t index = 0;          // in the plan
            std::wstring source;       // the PDF its pixels come from (a source file or an edited page)
            uint32_t page = 0;
            std::wstring picture;      // what the editor opens
            bool rendered = false;     // `picture` is a fresh render, removed once the edit is done
            double pointsPerPixel = 0;
            std::wstring edited;       // the result: a one-page PDF, empty when left as it was
        };

        struct EditBatch
        {
            unsigned serial = 0;
            PageTool tool = PageTool::Redact;
            std::wstring folder;
            std::vector<EditItem> items;
            std::wstring error;
        };

        // Renders each page to a picture for the editor (or reuses an earlier edit's picture).
        void PreparePictures(EditBatch& batch)
        {
            for (size_t n = 0; n < batch.items.size() && batch.error.empty(); ++n)
            {
                auto& item = batch.items[n];
                std::wstring error;
                auto doc = OpenForRendering(item.source, error);
                double w = 0, h = 0;
                if (!doc || !doc->PageSize(item.page, w, h) || w <= 0 || h <= 0)
                {
                    batch.error = error.empty() ? std::wstring(L"the page could not be read") : error;
                    return;
                }
                std::error_code ec;
                if (!item.picture.empty() && std::filesystem::exists(item.picture, ec))
                {
                    Bitmap earlier;
                    if (!DecodeImageFile(item.picture, earlier, error) || earlier.width == 0)
                    {
                        batch.error = error;
                        return;
                    }
                    item.pointsPerPixel = w / earlier.width;
                    continue;
                }
                uint32_t pw = 0, ph = 0;
                PixelSize(w, h, kEditDpi, kEditMaxSide, pw, ph);
                Bitmap pixels;
                if (!doc->Render(item.page, pw, ph, pixels, error))
                {
                    batch.error = error;
                    return;
                }
                item.pointsPerPixel = w / pw;
                item.picture = (std::filesystem::path(batch.folder) / std::format(L"page {}-{}.png", batch.serial, n + 1)).wstring();
                item.rendered = true;
                if (FAILED(EncodeImageFile(pixels, item.picture, false, 0, 72.0 / item.pointsPerPixel)))
                    batch.error = L"the page could not be saved for editing";
            }
        }

        // The editor's result for each page, as an edited page the plan can point at.
        void SaveEdits(EditBatch& batch, const nlohmann::json& params)
        {
            for (size_t n = 0; n < batch.items.size(); ++n)
            {
                auto& item = batch.items[n];
                const auto stem = (std::filesystem::path(batch.folder) / std::format(L"edit {}-{}", batch.serial, n + 1)).wstring();
                const std::wstring picture = stem + L".png", page = stem + L".pdf";
                bool changed = false;
                std::wstring error;
                if (!RenderPageEdit(batch.tool, params, item.picture, picture, changed, error) ||
                    (changed && !WritePicturePage(picture, item.pointsPerPixel, page, error)))
                {
                    batch.error = error.empty() ? std::wstring(L"the edit could not be saved") : error;
                    return;
                }
                if (changed)
                    item.edited = page;
            }
        }

        void RemoveRenders(const EditBatch& batch)
        {
            // The plain renders held the page as it was; only the edited pictures are kept.
            for (const auto& item : batch.items)
                if (item.rendered)
                {
                    std::error_code ec;
                    std::filesystem::remove(item.picture, ec);
                }
        }

        bool In(const D2D1_RECT_F& r, D2D1_POINT_2F p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }

        // ---- The window ---------------------------------------------------------------------------

        class PageView final : public EditorFrame
        {
        public:
            explicit PageView(PageViewRequest request) : m_request(std::move(request))
            {
                if (m_request.order.empty())
                    for (uint32_t i = 0; i < m_request.files.size(); ++i)
                        m_request.order.push_back(i);
                m_editFolder = m_request.editFolder;
                m_notice = m_request.notice;
                const bool organize = Organizing();
                const auto button = [&](const wchar_t* text, wchar_t icon, bool primary, std::function<void()> action) {
                    m_buttons.push_back(Button{ text, icon, primary, true, {}, std::move(action) });
                };
                // In ButtonId order.
                button(L"Rotate left", 0, false, [this] { Rotate(-90); });
                button(L"Rotate right", 0, false, [this] { Rotate(90); });
                button(L"Duplicate", 0, false, [this] { Duplicate(); });
                button(L"Delete", 0, false, [this] { Remove(); });
                button(L"Undo", 0, false, [this] { Undo(); });
                button(L"Edit page", 0xE70F, false, [this] { ShowEditMenu(); });
                button(L"Revert", 0, false, [this] { Revert(); });
                button(L"Previous", 0, false, [this] { Step(-1); });
                button(L"Next", 0, false, [this] { Step(+1); });
                button(L"Back", 0, false, [this] { Back(); });
                button(L"Cancel", 0, false, [this] { Cancel(); });
                button(organize || !Several() ? L"Save" : L"Merge and export", 0, true, [this] { Save(); });
            }

            bool Open()
            {
                std::wstring title;
                const auto& files = m_request.files;
                switch (m_request.kind)
                {
                case PageViewKind::Organize:
                    title = files.size() == 1 ? std::format(L"Organize pages · {}", FileName(files.front())) : std::format(L"Organize pages · {} PDFs", files.size());
                    break;
                case PageViewKind::Merge:
                    title = std::format(L"Merge {} PDFs · every page", m_request.order.size());
                    break;
                case PageViewKind::Edit:
                    title = Several() ? std::format(L"Edit {} PDFs", m_request.order.size()) : std::format(L"Edit PDF · {}", FileName(files[m_request.order.front()]));
                    break;
                }
                if (!(Organizing() ? Create(title, 900, 640, 560, 400) : Create(title, 1240, 820, 760, 500)))
                    return false;
                StartCounting();
                return true;
            }

        private:
            enum ButtonId { kRotateLeft, kRotateRight, kDuplicate, kDelete, kUndo, kEdit, kRevert, kPrev, kNext, kBack, kCancel, kSave };

            bool Organizing() const { return m_request.kind == PageViewKind::Organize; }
            bool Several() const { return m_request.order.size() > 1; }
            bool Editing() const { return m_editSerial != 0; }

            // ---- Loading ------------------------------------------------------------------------

            void StartCounting()
            {
                // QPDF counts the pages on a worker; the thumbnails follow, the ones on screen first.
                RunMtaDetached([hwnd = Hwnd(), files = m_request.files, order = m_request.order, sweep = !Organizing()] {
                    if (sweep)
                        RemoveOldEditFolders();   // left by an earlier run that stopped mid-edit
                    auto counted = std::make_unique<Counted>();
                    counted->counts.assign(files.size(), 0);
                    std::vector<std::wstring> shown;
                    for (const auto f : order)
                        shown.push_back(files[f]);
                    std::vector<uint32_t> counts;
                    try
                    {
                        counted->ok = CountPages(shown, counts, counted->error);
                    }
                    catch (...)
                    {
                        counted->error = L"the PDF could not be read";
                    }
                    for (size_t i = 0; counted->ok && i < order.size() && i < counts.size(); ++i)
                        counted->counts[order[i]] = counts[i];
                    PostOwned(hwnd, kCounted, std::move(counted));
                });
            }

            void OnCounted(std::unique_ptr<Counted> counted)
            {
                if (!counted->ok)
                {
                    m_error = counted->error.empty() ? std::wstring(L"unknown error") : counted->error;
                    Invalidate();
                    return;
                }
                std::string problem;
                PagePlan plan = m_request.plan.empty() ? AllPages(counted->counts, m_request.order) : m_request.plan;
                if (!m_request.plan.empty() && !CheckPlan(InListOrder(plan, m_request.order), [&] {
                        std::vector<uint32_t> c;
                        for (const auto f : m_request.order)
                            c.push_back(counted->counts[f]);
                        return c;
                    }(), problem))
                {
                    plan = AllPages(counted->counts, m_request.order);   // the files changed meanwhile: start over
                    m_notice = L"The files changed since the pages were last edited, so every page is back as it is in the files.";
                }
                m_loaded = true;
                m_plan = std::move(plan);
                Select(m_plan.empty() ? std::vector<size_t>{} : std::vector<size_t>{ 0 });
                ++m_revision;
                Relayout();
            }

            void OnClosing() override
            {
                if (m_leaving)
                    return;
                // Closed without saving or going back: nothing will read the edited pages now.
                if (!m_editFolder.empty())
                    m_queue.RemoveWhenDone(m_editFolder);
                if (auto cancelled = std::move(m_request.cancelled))
                    cancelled();
            }

            // ---- Leaving ------------------------------------------------------------------------

            bool HasChanges() const
            {
                return !m_undo.empty() || m_request.changed || EditedPages(m_plan) > 0;
            }

            void Save()
            {
                if (m_plan.empty() || !m_loaded || Editing())
                {
                    MessageBeep(MB_OK);
                    return;
                }
                if (!m_editFolder.empty() && EditedPages(m_plan) == 0)
                {
                    m_queue.RemoveWhenDone(m_editFolder);   // every edit was reverted: nothing will read them
                    m_editFolder.clear();
                }
                Leave(std::move(m_request.save));
            }

            void Back()
            {
                if (!m_request.back || Editing() || !m_loaded)
                    return;
                Leave(std::move(m_request.back));
            }

            void Leave(std::function<void(PagePlan, std::wstring)> then)
            {
                m_leaving = true;
                auto plan = std::move(m_plan);
                auto folder = m_editFolder;
                Close();   // deletes this
                if (then)
                    then(std::move(plan), std::move(folder));
            }

            void Cancel()
            {
                if (Editing())
                    return;
                if (HasChanges() && !ConfirmDiscard(Hwnd(), Organizing() ? L"Organize pages" : m_request.kind == PageViewKind::Merge ? L"Merge PDFs" : L"Edit PDF"))
                    return;
                Close();   // deletes this
            }

            // ---- Edits of the plan --------------------------------------------------------------

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

            size_t SelectedCount() const { return static_cast<size_t>(std::count(m_selected.begin(), m_selected.end(), true)); }

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
                ++m_revision;
                m_notice.clear();
                if (!Editing())
                    m_status.clear();
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
                if (m_plan.empty())
                    return;
                Remember();
                Select(MoveItems(m_plan, Selection(), insertBefore));
                Changed();
            }

            bool SelectionEdited() const
            {
                const auto s = Selection();
                return std::any_of(s.begin(), s.end(), [&](size_t i) { return i < m_plan.size() && !m_plan[i].edited.empty(); });
            }

            void Revert()
            {
                if (!SelectionEdited())
                {
                    MessageBeep(MB_OK);
                    return;
                }
                Remember();
                RevertPages(m_plan, Selection());
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
                m_selected.resize(m_plan.size(), false);
                m_focus = m_plan.empty() ? 0 : std::min(m_focus, m_plan.size() - 1);
                Changed();
            }

            // ---- Geometry -----------------------------------------------------------------------

            D2D1_RECT_F GridRect() const { return m_grid; }

            void ClampScroll() { m_scroll.Clamp(m_plan.size(), m_grid.bottom - m_grid.top); }
            void EnsureVisible(size_t i)
            {
                if (i < m_plan.size())
                    m_scroll.Reveal(i, m_plan.size(), m_grid.bottom - m_grid.top);
            }

            float GridLeft() const { return m_grid.left + ((m_grid.right - m_grid.left) - static_cast<float>(m_scroll.columns) * kCellW) / 2.f; }

            D2D1_RECT_F CellRect(size_t i) const
            {
                const size_t cols = m_scroll.columns;
                const float x = GridLeft() + static_cast<float>(i % cols) * kCellW;
                const float y = m_grid.top + static_cast<float>(i / cols) * kCellH - m_scroll.offset;
                return D2D1::RectF(x, y, x + kCellW, y + kCellH);
            }

            bool GridShown() const { return !m_big; }
            bool GridContains(D2D1_POINT_2F p) const { return GridShown() && In(m_grid, p); }

            long long ItemAt(D2D1_POINT_2F p) const
            {
                if (!GridContains(p))
                    return -1;
                const auto at = m_scroll.ItemAt(p.x - GridLeft(), p.y - m_grid.top, m_plan.size());
                if (at < 0)
                    return -1;
                const auto r = CellRect(static_cast<size_t>(at));
                return p.x >= r.left + 4.f && p.x < r.right - 4.f && p.y >= r.top + 4.f && p.y < r.bottom - 4.f ? at : -1;
            }

            void Relayout()
            {
                Layout(ClientDip());
                Invalidate();
            }

            void Layout(D2D1_SIZE_F size) override
            {
                const bool organize = Organizing();
                for (auto& b : m_buttons)
                    b.visible = true;
                // The toolbar: page edits on the left; in Merge and Edit the editors above the preview.
                const float widths[] = { 104.f, 112.f, 96.f, 84.f, 76.f };
                float x = kMargin;
                const float ty = (kToolbar - kButtonH) / 2.f;
                for (int i = kRotateLeft; i <= kUndo; ++i)
                {
                    m_buttons[i].rect = D2D1::RectF(x, ty, x + widths[i], ty + kButtonH);
                    x += widths[i] + 8.f;
                }
                // The footer: Back, Cancel and Save / Merge and export at the bottom right.
                const float by = size.height - kFooter + (kFooter - kButtonH) / 2.f;
                const float saveW = organize || !Several() ? kButtonW : 152.f;
                m_buttons[kSave].rect = D2D1::RectF(size.width - kMargin - saveW, by, size.width - kMargin, by + kButtonH);
                m_buttons[kCancel].rect = D2D1::RectF(m_buttons[kSave].rect.left - 8.f - kButtonW, by, m_buttons[kSave].rect.left - 8.f, by + kButtonH);
                m_buttons[kBack].rect = D2D1::RectF(m_buttons[kCancel].rect.left - 8.f - kButtonW, by, m_buttons[kCancel].rect.left - 8.f, by + kButtonH);
                m_buttons[kBack].visible = static_cast<bool>(m_request.back);

                const float top = organize ? kToolbar : kToolbar + kHint, bottom = size.height - kFooter;
                if (organize)
                {
                    for (int i : { kEdit, kRevert, kPrev, kNext })
                        m_buttons[i].visible = false;
                    m_grid = D2D1::RectF(kMargin, top, size.width - kMargin, bottom);
                    m_previewRect = {};
                }
                else
                {
                    const float previewW = std::clamp((size.width - 2.f * kMargin) * 0.4f, 300.f, 620.f);
                    m_previewRect = m_big ? D2D1::RectF(kMargin, top, size.width - kMargin, bottom)
                                          : D2D1::RectF(size.width - kMargin - previewW, top, size.width - kMargin, bottom);
                    // The grid keeps its place while the large view is up, so it comes back where it was.
                    m_grid = D2D1::RectF(kMargin, top, size.width - kMargin - previewW - 12.f, bottom);
                    m_buttons[kRevert].rect = D2D1::RectF(size.width - kMargin - 84.f, ty, size.width - kMargin, ty + kButtonH);
                    m_buttons[kEdit].rect = D2D1::RectF(m_buttons[kRevert].rect.left - 8.f - 124.f, ty, m_buttons[kRevert].rect.left - 8.f, ty + kButtonH);
                    const float ny = m_previewRect.bottom - kNavH + (kNavH - kButtonH) / 2.f;
                    const float cx = (m_previewRect.left + m_previewRect.right) / 2.f;
                    m_buttons[kPrev].rect = D2D1::RectF(cx - 56.f - 96.f, ny, cx - 56.f, ny + kButtonH);
                    m_buttons[kNext].rect = D2D1::RectF(cx + 56.f, ny, cx + 56.f + 96.f, ny + kButtonH);
                }
                m_scroll.cellWidth = kCellW;
                m_scroll.rowHeight = kCellH;
                m_scroll.columns = std::max<size_t>(1, static_cast<size_t>((m_grid.right - m_grid.left) / kCellW));
                ClampScroll();
                m_requestedFirst = SIZE_MAX;   // the pages on screen may have changed
            }

            // ---- Thumbnails ---------------------------------------------------------------------

            std::wstring PathOf(const PageRef& ref) const
            {
                if (!ref.edited.empty())
                    return ref.edited;
                return ref.file < m_request.files.size() ? m_request.files[ref.file] : std::wstring();
            }
            static uint32_t PageOf(const PageRef& ref) { return ref.edited.empty() ? ref.page : 0; }

            PagePicture* Thumbnail(const PageRef& ref)
            {
                const auto it = m_thumbs.find({ PathOf(ref), PageOf(ref) });
                return it == m_thumbs.end() ? nullptr : &it->second;
            }

            float Aspect(const PageRef& ref) const
            {
                const auto it = m_thumbs.find({ PathOf(ref), PageOf(ref) });
                if (it != m_thumbs.end() && it->second.heightPt > 0)
                    return static_cast<float>(it->second.widthPt / it->second.heightPt);
                return 0.773f;   // A4/Letter-ish until the page arrives
            }

            void RequestThumbnails()
            {
                // The pages on screen first, then outwards from them, so scrolling anywhere in a long
                // document fills that part in at once while the rest keeps coming.
                if (m_plan.empty() || !GridShown())
                    return;
                const auto [first, end] = m_scroll.Visible(m_plan.size(), m_grid.bottom - m_grid.top);
                if (first == m_requestedFirst && end == m_requestedEnd && m_revision == m_requestedRevision)
                    return;
                m_requestedFirst = first;
                m_requestedEnd = end;
                m_requestedRevision = m_revision;
                std::vector<RenderJob> jobs;
                std::set<std::pair<std::wstring, uint32_t>> queued;
                const auto px = static_cast<uint32_t>(std::lround(kThumbBox * Scale()));
                const auto want = [&](size_t i) {
                    auto key = std::make_pair(PathOf(m_plan[i]), PageOf(m_plan[i]));
                    if (key.first.empty() || m_thumbs.contains(key) || !queued.insert(key).second)
                        return;
                    jobs.push_back(RenderJob{ key.first, key.second, px, false });
                };
                for (size_t i = first; i < end; ++i)
                    want(i);
                for (size_t d = 1; first >= d || end + d - 1 < m_plan.size(); ++d)
                {
                    if (end + d - 1 < m_plan.size())
                        want(end + d - 1);
                    if (first >= d)
                        want(first - d);
                }
                m_queue.Thumbnails(Hwnd(), std::move(jobs));
            }

            // ---- Drawing ------------------------------------------------------------------------

            // Draws `picture` (or a white placeholder of `aspect`, width / height, showing
            // `placeholder`) turned by `rotation` and fitted in `box`; returns where the page landed.
            D2D1_RECT_F DrawPage(ID2D1RenderTarget* rt, PagePicture* picture, float aspect, int rotation, const D2D1_RECT_F& box,
                                 std::wstring_view placeholder, bool selected)
            {
                const auto& c = Theme();
                ID2D1Bitmap* bitmap = nullptr;
                if (picture && !picture->pixels.Empty())
                {
                    picture->used = m_paint;
                    if (!picture->bitmap || picture->generation != DeviceGeneration())
                    {
                        picture->bitmap.Reset();
                        const auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                        // Opaque pixels: straight and premultiplied alpha are the same.
                        if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(picture->pixels.width, picture->pixels.height), picture->pixels.bgra.data(),
                                                       picture->pixels.width * 4, props, &picture->bitmap)))
                            picture->generation = DeviceGeneration();
                    }
                    bitmap = picture->bitmap.Get();
                }
                float bw = aspect, bh = 1.f;
                if (bitmap)
                {
                    const auto s = bitmap->GetPixelSize();
                    bw = static_cast<float>(s.width);
                    bh = static_cast<float>(s.height);
                }
                const auto centre = D2D1::Point2F((box.left + box.right) / 2.f, (box.top + box.bottom) / 2.f);
                const bool sideways = rotation == 90 || rotation == 270;
                const float fit = std::min((box.right - box.left) / (sideways ? bh : bw), (box.bottom - box.top) / (sideways ? bw : bh));
                const float hw = bw * fit / 2.f, hh = bh * fit / 2.f;
                const auto page = D2D1::RectF(centre.x - hw, centre.y - hh, centre.x + hw, centre.y + hh);
                D2D1_MATRIX_3X2_F old;
                rt->GetTransform(&old);
                rt->SetTransform(D2D1::Matrix3x2F::Rotation(static_cast<float>(rotation), centre) * old);
                if (auto* b = Brush(D2D1::ColorF(1, 1, 1, 1)))
                    rt->FillRectangle(page, b);
                if (bitmap)
                    rt->DrawBitmap(bitmap, page, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                else
                    DrawText(placeholder, TextFormat(20.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), page, D2D1::ColorF(0, 0, 0, 0.35f));
                if (auto* b = Brush(selected ? c.accent : c.line))
                    rt->DrawRectangle(page, b, selected ? 2.f : 1.f);
                rt->SetTransform(old);
                return sideways ? D2D1::RectF(centre.x - hh, centre.y - hw, centre.x + hh, centre.y + hw) : page;
            }

            void DrawTag(ID2D1RenderTarget* rt, uint32_t file, const D2D1_RECT_F& tag)
            {
                if (auto* b = Brush(FileTagColor(file)))
                    rt->FillRoundedRectangle(D2D1::RoundedRect(tag, 3.f, 3.f), b);
                DrawText(FileTagLetter(file), TextFormat(10.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), tag, D2D1::ColorF(1, 1, 1, 1));
            }

            void DrawCell(ID2D1RenderTarget* rt, size_t i, const D2D1_RECT_F& cell)
            {
                const auto& c = Theme();
                const auto& ref = m_plan[i];
                const bool selected = i < m_selected.size() && m_selected[i];
                if (selected)
                    if (auto* b = Brush(m_drag.dragging ? theme::WithAlpha(c.accentSoft, 0.5f) : c.accentSoft))
                        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cell.left + 4.f, cell.top + 4.f, cell.right - 4.f, cell.bottom - 4.f), 6.f, 6.f), b);
                if (i == m_focus && m_showFocus)
                    if (auto* b = Brush(c.textDim))
                        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cell.left + 4.5f, cell.top + 4.5f, cell.right - 4.5f, cell.bottom - 4.5f), 6.f, 6.f), b, 1.f);

                const auto box = D2D1::RectF(cell.left + (kCellW - kThumbBox) / 2.f, cell.top + 12.f, cell.left + (kCellW + kThumbBox) / 2.f, cell.top + 12.f + kThumbBox);
                const bool organize = Organizing();
                const auto page = DrawPage(rt, Thumbnail(ref), Aspect(ref), ref.rotation, box, std::format(L"{}", organize ? ref.page + 1 : i + 1), selected);
                if (!ref.edited.empty())
                {
                    // An edited page carries a small pencil badge on its top-right corner.
                    if (auto* b = Brush(c.accent))
                        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(page.right - 2.f, page.top + 2.f), 10.f, 10.f), b);
                    const wchar_t pencil[2] = { 0xE70F, 0 };
                    DrawText(pencil, IconFormat(11.f), D2D1::RectF(page.right - 12.f, page.top - 8.f, page.right + 8.f, page.top + 12.f), c.onAccent);
                }

                std::wstring from = std::format(L"page {}", ref.page + 1);
                if (ref.rotation)
                    from += std::format(L" · {}°", ref.rotation);
                if (!ref.edited.empty())
                    from += L" · edited";
                const float textTop = cell.top + 12.f + kThumbBox + 4.f;
                if (organize)
                {
                    // Organize: where the page comes from ("Page 3", or "B · page 3" with several files).
                    const std::wstring label = m_request.files.size() > 1 ? std::format(L"{} · {}", FileTagLetter(ref.file), from)
                                                                          : L"P" + from.substr(1);
                    DrawText(label, TextFormat(12.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER),
                             D2D1::RectF(cell.left + 6.f, textTop + 2.f, cell.right - 6.f, cell.bottom - 8.f), selected ? c.text : c.textDim);
                    return;
                }
                // Merge and Edit: its page number in the result, then the file's tag and its page there.
                DrawText(std::format(L"{}", i + 1), TextFormat(13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER),
                         D2D1::RectF(cell.left + 6.f, textTop, cell.right - 6.f, textTop + 18.f), c.text);
                const float lineTop = textTop + 19.f, lineBottom = lineTop + 18.f;
                if (!Several())
                {
                    DrawText(from, TextFormat(12.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER),
                             D2D1::RectF(cell.left + 6.f, lineTop, cell.right - 6.f, lineBottom), c.textDim);
                    return;
                }
                const float tagW = 10.f + 7.f * static_cast<float>(FileTagLetter(ref.file).size());
                const float total = std::min(kCellW - 16.f, tagW + 6.f + TextWidth(from, 12.f));
                const float left = (cell.left + cell.right - total) / 2.f;
                const auto tag = D2D1::RectF(left, lineTop + 2.f, left + tagW, lineBottom - 2.f);
                DrawTag(rt, ref.file, tag);
                DrawText(from, TextFormat(12.f), D2D1::RectF(tag.right + 6.f, lineTop, cell.right - 6.f, lineBottom), c.textDim);
            }

            void PaintGrid(ID2D1RenderTarget* rt)
            {
                const auto& c = Theme();
                if (auto* b = Brush(c.panel))
                    rt->FillRoundedRectangle(D2D1::RoundedRect(m_grid, 6.f, 6.f), b);
                rt->PushAxisAlignedClip(m_grid, D2D1_ANTIALIAS_MODE_ALIASED);
                // Only the rows on screen are drawn: a 1,000-page plan paints as fast as a short one.
                const auto [first, end] = m_scroll.Visible(m_plan.size(), m_grid.bottom - m_grid.top);
                for (size_t i = first; i < end; ++i)
                    DrawCell(rt, i, CellRect(i));
                if (m_drag.dragging && !m_plan.empty())
                {
                    const size_t cols = m_scroll.columns, at = m_insert;
                    // The bar sits before item `at`, or after the last item of its row.
                    const bool rowEnd = at > 0 && (at % cols == 0 || at == m_plan.size());
                    const auto cell = CellRect(rowEnd ? at - 1 : at);
                    const float x = rowEnd ? cell.right : cell.left;
                    if (auto* b = Brush(c.accent))
                        rt->FillRectangle(D2D1::RectF(x - 2.f, cell.top + 8.f, x + 2.f, cell.bottom - 8.f), b);
                }
                rt->PopAxisAlignedClip();
                const wchar_t* empty = !m_loaded ? (m_error.empty() ? L"Reading pages…" : L"")
                                       : m_plan.empty() ? L"No pages left. Undo (Ctrl+Z) brings them back."
                                                        : L"";
                DrawText(empty, TextFormat(14.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), m_grid, c.textDim);
                if (!m_loaded && !m_error.empty())
                    DrawText(L"This PDF cannot be opened: " + m_error, TextFormat(14.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), m_grid, c.textDim);

                // Device bitmaps of pages far off screen go (their pixels stay, so they come back at once).
                size_t bitmaps = 0;
                for (const auto& [key, t] : m_thumbs)
                    bitmaps += t.bitmap ? 1 : 0;
                if (bitmaps > kMaxBitmaps)
                    for (auto& [key, t] : m_thumbs)
                        if (t.used + 2 < m_paint)
                            t.bitmap.Reset();
            }

            // ---- The preview (Merge and Edit) ---------------------------------------------------

            D2D1_RECT_F PreviewPageBox() const
            {
                const auto& r = m_previewRect;
                return D2D1::RectF(r.left + 16.f, r.top + kCaptionH + 8.f, r.right - 16.f, r.bottom - kNavH - 8.f);
            }

            void RequestPreview()
            {
                if (Organizing() || m_plan.empty() || m_focus >= m_plan.size())
                    return;
                const auto& ref = m_plan[m_focus];
                const auto box = PreviewPageBox();
                const bool sideways = ref.rotation == 90 || ref.rotation == 270;
                const float aspect = Aspect(ref);   // width / height of the page as it is
                const float w = sideways ? 1.f : aspect, h = sideways ? aspect : 1.f;
                const float fit = std::min(std::max(1.f, box.right - box.left) / w, std::max(1.f, box.bottom - box.top) / h);
                const auto side = static_cast<uint32_t>(std::clamp<long>(std::lround(std::max(w, h) * fit * Scale()), 64, 4096));
                RenderJob job{ PathOf(ref), PageOf(ref), side, true };
                if (job.path == m_previewWanted.path && job.page == m_previewWanted.page && job.maxSide == m_previewWanted.maxSide)
                    return;
                m_previewWanted = job;
                m_queue.Preview(Hwnd(), std::move(job));
            }

            void PaintPreview(ID2D1RenderTarget* rt)
            {
                const auto& c = Theme();
                const auto& r = m_previewRect;
                if (auto* b = Brush(c.panel))
                    rt->FillRoundedRectangle(D2D1::RoundedRect(r, 6.f, 6.f), b);
                if (m_plan.empty())
                    return;
                const size_t k = std::min(m_focus, m_plan.size() - 1);
                const auto& ref = m_plan[k];
                // Caption: where the page comes from.
                std::wstring from = Several() ? std::format(L"{} · page {}", FileName(m_request.files[ref.file]), ref.page + 1)
                                              : std::format(L"Page {} of the original", ref.page + 1);
                if (!ref.edited.empty())
                    from += L" · edited";
                if (ref.rotation)
                    from += std::format(L" · turned {}°", ref.rotation);
                float textLeft = r.left + 16.f;
                if (Several())
                {
                    const float tagW = 10.f + 7.f * static_cast<float>(FileTagLetter(ref.file).size());
                    DrawTag(rt, ref.file, D2D1::RectF(textLeft, r.top + 9.f, textLeft + tagW, r.top + 25.f));
                    textLeft += tagW + 6.f;
                }
                DrawText(from, TextFormat(12.f), D2D1::RectF(textLeft, r.top + 2.f, r.right - 16.f, r.top + kCaptionH), c.textDim);

                // The large render once it is in; the thumbnail, scaled up, until then.
                const std::pair<std::wstring, uint32_t> key{ PathOf(ref), PageOf(ref) };
                DrawPage(rt, key == m_previewKey ? &m_preview : Thumbnail(ref), Aspect(ref), ref.rotation, PreviewPageBox(), std::format(L"{}", k + 1), false);
                // Previous · k of N · Next
                DrawText(std::format(L"{} of {}", k + 1, m_plan.size()), TextFormat(13.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER),
                         D2D1::RectF(m_buttons[kPrev].rect.right, m_buttons[kPrev].rect.top, m_buttons[kNext].rect.left, m_buttons[kNext].rect.bottom), c.text);
            }

            void Step(int delta)
            {
                const long long to = static_cast<long long>(m_focus) + delta;
                if (m_plan.empty() || to < 0 || to >= static_cast<long long>(m_plan.size()))
                {
                    MessageBeep(MB_OK);
                    return;
                }
                FocusTo(to, false);
            }

            void SetBig(bool big)
            {
                if (Organizing())
                    return;
                m_big = big;
                Layout(ClientDip());
                EnsureVisible(m_focus);
                Invalidate();
            }

            // ---- Paint --------------------------------------------------------------------------

            void Paint(ID2D1RenderTarget* rt, D2D1_SIZE_F size) override
            {
                ++m_paint;
                const auto& c = Theme();
                if (GridShown())
                    PaintGrid(rt);
                if (!Organizing())
                {
                    PaintPreview(rt);
                    std::wstring hint = m_big ? L"← → turn the pages · R turns · Delete leaves out · Enter or Esc shows all pages"
                                              : std::format(L"Drag pages to reorder{} · Alt+arrows move · R turns · Delete leaves out · right-click to edit · "
                                                            L"double-click to view large",
                                                            Several() ? L", across files too" : L"");
                    if (!m_notice.empty())
                        hint = m_notice;
                    DrawText(hint, TextFormat(12.f), D2D1::RectF(kMargin, kToolbar - 4.f, size.width - kMargin, kToolbar + kHint - 4.f), m_notice.empty() ? c.textDim : c.text);
                }

                std::wstring status;
                if (m_loaded)
                {
                    const size_t selected = SelectedCount();
                    if (Organizing())
                    {
                        status = m_plan.size() == 1 ? L"1 page" : std::format(L"{} pages", m_plan.size());
                        if (selected)
                            status += std::format(L" · {} selected", selected);
                        status += L" · drag to move · R rotates · Ctrl+D duplicates · Del deletes";
                    }
                    else
                    {
                        const auto edited = EditedPages(m_plan);
                        status = m_plan.size() == 1 ? std::wstring(L"1 page in all") : std::format(L"{} pages in all", m_plan.size());
                        if (selected > 1)
                            status += std::format(L" · {} selected", selected);
                        if (edited)
                            status += std::format(L" · {} edited", edited);
                    }
                }
                if (!m_status.empty())
                    status = m_status;
                const float left = m_buttons[kBack].visible ? m_buttons[kBack].rect.left : m_buttons[kCancel].rect.left;
                DrawText(status, TextFormat(12.f), D2D1::RectF(kMargin, size.height - kFooter, left - 8.f, size.height), c.textDim);

                const bool any = m_loaded && !m_plan.empty() && !Editing();
                for (int i = kRotateLeft; i <= kDelete; ++i)
                    m_buttons[i].enabled = any;
                m_buttons[kUndo].enabled = !m_undo.empty() && !Editing();
                m_buttons[kEdit].enabled = any && RenderingAvailable() && canvas::HasEditorWindow();
                m_buttons[kRevert].enabled = any && SelectionEdited();
                m_buttons[kPrev].enabled = any && m_focus > 0;
                m_buttons[kNext].enabled = any && m_focus + 1 < m_plan.size();
                m_buttons[kBack].enabled = m_buttons[kSave].enabled = any;
                DrawButtons();
                RequestThumbnails();
                RequestPreview();
            }

            // ---- Editing pages ------------------------------------------------------------------

            void ShowEditMenu()
            {
                const auto& r = m_buttons[kEdit].rect;
                POINT pt{ static_cast<LONG>(r.left * Scale()), static_cast<LONG>(r.bottom * Scale()) };
                ClientToScreen(Hwnd(), &pt);
                ShowMenu(pt, true);
            }

            // The right-click menu (and, `editOnly`, the Edit page button's): for the selected pages.
            void ShowMenu(POINT pt, bool editOnly)
            {
                if (m_plan.empty() || Editing() || !m_loaded)
                    return;
                const auto selection = Selection();
                const bool canEdit = !Organizing() && RenderingAvailable() && canvas::HasEditorWindow();
                HMENU tools = CreatePopupMenu();
                for (size_t i = 0; i < PageTools().size(); ++i)
                    AppendMenuW(tools, MF_STRING | (canEdit ? 0 : MF_GRAYED), kMenuToolFirst + i, PageTools()[i].label);
                HMENU menu = tools;
                if (!editOnly)
                {
                    menu = CreatePopupMenu();
                    if (!Organizing())
                    {
                        const std::wstring edit = selection.size() > 1 ? std::format(L"Edit {} pages", selection.size()) : std::wstring(L"Edit page");
                        AppendMenuW(menu, MF_POPUP | (canEdit ? 0 : MF_GRAYED), reinterpret_cast<UINT_PTR>(tools), edit.c_str());
                    }
                    else
                        DestroyMenu(tools);
                }
                else
                    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                if (!Organizing())
                    AppendMenuW(menu, MF_STRING | (SelectionEdited() ? 0 : MF_GRAYED), kMenuRevert, L"Revert to the original");
                if (!editOnly)
                {
                    if (!Organizing())
                        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(menu, MF_STRING, kMenuRotateRight, L"Rotate right\tR");
                    AppendMenuW(menu, MF_STRING, kMenuRotateLeft, L"Rotate left\tShift+R");
                    AppendMenuW(menu, MF_STRING, kMenuDuplicate, L"Duplicate\tCtrl+D");
                    AppendMenuW(menu, MF_STRING, kMenuDelete, Organizing() ? L"Delete\tDel" : L"Leave out\tDel");
                    if (!Organizing())
                    {
                        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                        AppendMenuW(menu, MF_STRING, kMenuViewLarge, L"View large\tEnter");
                    }
                }
                const auto picked = static_cast<UINT>(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, Hwnd(), nullptr));
                DestroyMenu(menu);   // and the submenu with it
                if (picked >= kMenuToolFirst && picked < kMenuToolFirst + PageTools().size())
                    StartEdit(PageTools()[picked - kMenuToolFirst].tool);
                else if (picked == kMenuRevert)
                    Revert();
                else if (picked == kMenuRotateRight)
                    Rotate(90);
                else if (picked == kMenuRotateLeft)
                    Rotate(-90);
                else if (picked == kMenuDuplicate)
                    Duplicate();
                else if (picked == kMenuDelete)
                    Remove();
                else if (picked == kMenuViewLarge)
                    SetBig(true);
            }

            // While pages are being edited the window takes no input, so the edit lands on the pages
            // it started from.
            void SetBusy(bool busy)
            {
                EnableWindow(Hwnd(), busy ? FALSE : TRUE);
                if (!busy)
                    SetForegroundWindow(Hwnd());
                Invalidate();
            }

            void StartEdit(PageTool tool)
            {
                if (Editing() || Organizing())
                    return;
                if (m_editFolder.empty())
                    m_editFolder = NewEditFolder();
                if (m_editFolder.empty())
                {
                    m_status = L"Pages cannot be edited: the temporary folder cannot be written.";
                    Invalidate();
                    return;
                }
                auto batch = std::make_unique<EditBatch>();
                batch->serial = ++m_serialCounter;
                batch->tool = tool;
                batch->folder = m_editFolder;
                for (const auto i : Selection())
                {
                    const auto& ref = m_plan[i];
                    EditItem item;
                    item.index = i;
                    item.source = PathOf(ref);
                    item.page = PageOf(ref);
                    if (!ref.edited.empty())
                        item.picture = PictureOf(ref.edited);   // builds on the earlier edit
                    batch->items.push_back(std::move(item));
                }
                if (batch->items.empty())
                    return;
                m_editSerial = batch->serial;
                m_status = batch->items.size() == 1 ? std::wstring(L"Opening the page…") : std::format(L"Opening {} pages…", batch->items.size());
                SetBusy(true);
                RunMtaDetached([hwnd = Hwnd(), batch = std::shared_ptr<EditBatch>(std::move(batch))] {
                    PreparePictures(*batch);   // Windows' PDF renderer blocks: on a worker
                    PostOwned(hwnd, kEditReady, std::make_unique<EditBatch>(std::move(*batch)));
                });
            }

            void OnEditReady(std::unique_ptr<EditBatch> batch)
            {
                if (batch->serial != m_editSerial)
                    return;
                if (!batch->error.empty())
                {
                    RemoveRenders(*batch);
                    EndEdit(L"The page could not be opened for editing: " + batch->error);
                    return;
                }
                // The page opens in the same editor as the picture tool, in its own window.
                canvas::EditorRequest request;
                for (const auto& item : batch->items)
                    request.files.push_back(item.picture);
                request.tool = MakePageTool(batch->tool, request.files);
                const HWND hwnd = Hwnd();
                const unsigned serial = batch->serial;
                request.apply = [hwnd, serial](nlohmann::json params) {
                    PostOwned(hwnd, kEditApplied, std::make_unique<nlohmann::json>(std::move(params)), serial);
                };
                request.closed = [hwnd, serial] { PostMessageW(hwnd, kEditClosed, serial, 0); };
                m_pending = std::move(batch);
                m_status.clear();
                Invalidate();
                if (!request.tool || !canvas::OpenEditorWindow(std::move(request)))
                {
                    // A window that failed to open has posted kEditClosed; by then no edit is in
                    // progress, so it does nothing.
                    RemoveRenders(*m_pending);
                    m_pending.reset();
                    EndEdit(L"The editor could not open.");
                }
            }

            void OnEditApplied(unsigned serial, std::unique_ptr<nlohmann::json> params)
            {
                if (serial != m_editSerial || !m_pending)
                    return;
                m_applying = true;
                m_status = L"Saving the edit…";
                Invalidate();
                RunMtaDetached([hwnd = Hwnd(), batch = std::shared_ptr<EditBatch>(std::move(m_pending)), params = std::move(*params)] {
                    SaveEdits(*batch, params);
                    PostOwned(hwnd, kEditDone, std::make_unique<EditBatch>(std::move(*batch)));
                });
            }

            void OnEditClosed(unsigned serial)
            {
                if (serial != m_editSerial || m_applying || !m_pending)
                    return;   // an applied edit finishes in OnEditDone
                RemoveRenders(*m_pending);
                m_pending.reset();
                EndEdit({});
            }

            void OnEditDone(std::unique_ptr<EditBatch> batch)
            {
                if (batch->serial != m_editSerial)
                    return;
                RemoveRenders(*batch);
                std::vector<size_t> edited;
                if (batch->error.empty())
                    for (const auto& item : batch->items)
                        if (!item.edited.empty())
                            edited.push_back(item.index);
                if (!edited.empty())
                {
                    // The edited copy replaces the page in the plan: undoable like any other change.
                    Remember();
                    for (const auto& item : batch->items)
                        if (!item.edited.empty() && item.index < m_plan.size())
                            m_plan[item.index].edited = item.edited;
                    Changed();
                }
                std::wstring status;
                if (!batch->error.empty())
                    status = L"The edit could not be saved: " + batch->error;
                else if (edited.empty())
                    status = L"Nothing was changed on the page.";
                else
                    status = edited.size() == 1 ? std::wstring(L"1 page edited. Revert brings back the original.")
                                                : std::format(L"{} pages edited. Revert brings back the originals.", edited.size());
                EndEdit(status);
            }

            void EndEdit(std::wstring status)
            {
                m_editSerial = 0;
                m_applying = false;
                m_status = std::move(status);
                m_previewWanted = {};
                SetBusy(false);
            }

            // ---- Input --------------------------------------------------------------------------

            void SelectRange(size_t from, size_t to)
            {
                m_selected.assign(m_plan.size(), false);
                for (size_t k = std::min(from, to); k <= std::max(from, to) && k < m_plan.size(); ++k)
                    m_selected[k] = true;
            }

            // Moves the focus; with Shift it extends the selection, with Ctrl it leaves it.
            void FocusTo(long long target, bool showFocus = true)
            {
                if (m_plan.empty())
                    return;
                const auto to = static_cast<size_t>(std::clamp<long long>(target, 0, static_cast<long long>(m_plan.size()) - 1));
                m_focus = to;
                if (Shift())
                    SelectRange(m_anchor, to);
                else if (!Ctrl())
                    Select({ to });
                m_showFocus = showFocus;
                EnsureVisible(m_focus);
                Invalidate();
            }

            bool OnKey(UINT key, bool alt)
            {
                const bool ctrl = Ctrl(), shift = Shift();
                if (key == VK_ESCAPE)
                {
                    if (m_drag.dragging)
                    {
                        m_drag.Reset();
                        ReleaseCapture();
                        Invalidate();
                    }
                    else if (m_big)
                        SetBig(false);
                    else
                        Cancel();   // may delete this
                    return true;
                }
                if (key == VK_RETURN)
                {
                    if (Organizing() || ctrl)
                        Save();   // may delete this
                    else
                        SetBig(!m_big);
                    return true;
                }
                if (!m_loaded)
                    return false;
                if (m_big && !alt && !ctrl)
                    switch (key)
                    {
                    case VK_LEFT:
                    case VK_UP:
                    case VK_PRIOR:
                        Step(-1);
                        return true;
                    case VK_RIGHT:
                    case VK_DOWN:
                    case VK_NEXT:
                    case VK_SPACE:
                        Step(+1);
                        return true;
                    default:
                        break;
                    }
                const auto cols = static_cast<long long>(m_scroll.columns);
                const auto focus = static_cast<long long>(m_focus);
                const bool move = (alt || ctrl) && !m_plan.empty();
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
                case VK_PRIOR:
                case VK_NEXT:
                {
                    const auto rows = std::max<long long>(1, static_cast<long long>((m_grid.bottom - m_grid.top) / kCellH));
                    FocusTo(focus + (key == VK_PRIOR ? -1 : 1) * rows * cols);
                    return true;
                }
                case VK_HOME:
                    FocusTo(0);
                    return true;
                case VK_END:
                    FocusTo(static_cast<long long>(m_plan.size()) - 1);
                    return true;
                case VK_SPACE:
                    if (m_focus < m_selected.size())
                    {
                        if (ctrl)
                            m_selected[m_focus] = !m_selected[m_focus];
                        else
                            Select({ m_focus });
                        m_anchor = m_focus;
                        Invalidate();
                    }
                    return true;
                case 'A':
                    if (!ctrl)
                        return false;
                    m_selected.assign(m_plan.size(), true);
                    Invalidate();
                    return true;
                case 'R':
                    if (alt || ctrl)
                        return false;
                    Rotate(shift ? -90 : 90);
                    return true;
                case 'D':
                    if (!ctrl)
                        return false;
                    Duplicate();
                    return true;
                case 'Z':
                    if (!ctrl)
                        return false;
                    Undo();
                    return true;
                case 'E':
                    if (!ctrl || Organizing())
                        return false;
                    ShowEditMenu();
                    return true;
                case VK_DELETE:
                    Remove();
                    return true;
                case VK_BACK:
                    if (alt)
                        Back();   // may delete this
                    else
                        Remove();
                    return true;
                case VK_APPS:
                case VK_F10:
                    if (key == VK_F10 && !shift)
                        return false;
                    MenuAtFocus();
                    return true;
                default:
                    return false;   // Alt+F4 and the rest go to Windows
                }
            }

            void MenuAtFocus()
            {
                if (m_focus >= m_plan.size())
                    return;
                const auto r = m_big ? m_previewRect : CellRect(m_focus);
                POINT pt{ static_cast<LONG>((r.left + 40.f) * Scale()), static_cast<LONG>((r.top + 40.f) * Scale()) };
                ClientToScreen(Hwnd(), &pt);
                ShowMenu(pt, false);
            }

            D2D1_POINT_2F FromScreen(LPARAM lParam) const
            {
                POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(Hwnd(), &pt);
                return D2D1::Point2F(static_cast<float>(pt.x) / Scale(), static_cast<float>(pt.y) / Scale());
            }

            bool OnMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result) override
            {
                result = 0;
                switch (msg)
                {
                case kCounted:
                    OnCounted(std::unique_ptr<Counted>(reinterpret_cast<Counted*>(lParam)));
                    return true;
                case kRendered:
                {
                    std::unique_ptr<RenderedPage> page(reinterpret_cast<RenderedPage*>(lParam));
                    if (page->job.preview)
                    {
                        if (page->job.path != m_previewWanted.path || page->job.page != m_previewWanted.page || page->pixels.Empty())
                            return true;   // the selection moved on meanwhile
                        m_previewKey = { page->job.path, page->job.page };
                        m_preview = PagePicture{ std::move(page->pixels), page->widthPt, page->heightPt, nullptr, 0, 0 };
                    }
                    else
                        m_thumbs[{ page->job.path, page->job.page }] = PagePicture{ std::move(page->pixels), page->widthPt, page->heightPt, nullptr, 0, 0 };
                    Invalidate();
                    return true;
                }
                case kEditReady:
                    OnEditReady(std::unique_ptr<EditBatch>(reinterpret_cast<EditBatch*>(lParam)));
                    return true;
                case kEditApplied:
                    OnEditApplied(static_cast<unsigned>(wParam), std::unique_ptr<nlohmann::json>(reinterpret_cast<nlohmann::json*>(lParam)));
                    return true;
                case kEditClosed:
                    OnEditClosed(static_cast<unsigned>(wParam));
                    return true;
                case kEditDone:
                    OnEditDone(std::unique_ptr<EditBatch>(reinterpret_cast<EditBatch*>(lParam)));
                    return true;
                case WM_KEYDOWN:
                case WM_SYSKEYDOWN:
                    if (Editing())
                        return true;
                    return OnKey(static_cast<UINT>(wParam), msg == WM_SYSKEYDOWN);
                case WM_LBUTTONDOWN:
                case WM_RBUTTONDOWN:
                    return OnPress(msg, DipFromLParam(lParam));
                case WM_CONTEXTMENU:
                {
                    if (lParam == -1)
                    {
                        MenuAtFocus();   // Shift+F10 or the menu key
                        return true;
                    }
                    const auto p = FromScreen(lParam);
                    if (ItemAt(p) < 0 && !(m_big && In(m_previewRect, p)))
                        return false;
                    ShowMenu(POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }, false);
                    return true;
                }
                case WM_MOUSEMOVE:
                    if (!m_drag.pressed)
                        return false;
                    {
                        const auto p = DipFromLParam(lParam);
                        if (m_drag.Move(p.x, p.y, static_cast<float>(GetSystemMetrics(SM_CXDRAG)) / Scale()) && m_focus < m_selected.size())
                            m_selected[m_focus] = true;
                        if (m_drag.dragging)
                        {
                            m_scroll.EdgeScroll(p.y - m_grid.top, m_grid.bottom - m_grid.top, m_plan.size());
                            m_insert = m_scroll.InsertAt(p.x - GridLeft(), p.y - m_grid.top, m_plan.size());
                            Invalidate();
                        }
                    }
                    return true;
                case WM_LBUTTONUP:
                    if (!m_drag.pressed)
                        return false;
                    {
                        const bool dragged = m_drag.dragging;
                        m_drag.Reset();
                        ReleaseCapture();
                        if (dragged)
                            MoveSelection(m_insert);
                        else if (m_clickedSelected)
                            Select({ m_focus });
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
                {
                    const auto p = DipFromLParam(lParam);
                    if (!Organizing() && (ItemAt(p) >= 0 || In(m_previewRect, p)))
                    {
                        if (const auto at = ItemAt(p); at >= 0)
                            FocusTo(at, false);
                        SetBig(!m_big);   // double-click a page: view it large; again: back to all pages
                    }
                    return true;
                }
                case WM_MOUSEWHEEL:
                {
                    const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                    if (m_big)
                        Step(delta > 0 ? -1 : +1);
                    else if (GridContains(FromScreen(lParam)))
                    {
                        m_scroll.Wheel(delta, 0.5f, m_plan.size(), m_grid.bottom - m_grid.top);
                        Invalidate();
                    }
                    return true;
                }
                default:
                    return false;
                }
            }

            bool OnPress(UINT msg, D2D1_POINT_2F p)
            {
                if (!GridContains(p) || !m_loaded)
                    return false;
                SetFocus(Hwnd());
                const auto at = ItemAt(p);
                m_clickedSelected = false;
                if (at < 0)
                {
                    if (msg == WM_LBUTTONDOWN && !Ctrl())
                    {
                        m_selected.assign(m_plan.size(), false);
                        Invalidate();
                    }
                    return true;
                }
                const auto i = static_cast<size_t>(at);
                if (msg == WM_RBUTTONDOWN)
                {
                    // A right-click outside the selection selects just that page; inside, the menu
                    // works on the whole selection.
                    if (!m_selected[i])
                        Select({ i });
                    m_focus = i;
                    Invalidate();
                    return false;   // DefWindowProc turns the button-up into WM_CONTEXTMENU
                }
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
                m_drag.Press(p.x, p.y);
                SetCapture(Hwnd());
                Invalidate();
                return true;
            }

            // ---- State --------------------------------------------------------------------------

            PageViewRequest m_request;
            RenderQueue m_queue;
            bool m_loaded = false, m_leaving = false;
            std::wstring m_error, m_status, m_notice;

            PagePlan m_plan;
            std::vector<bool> m_selected;
            std::vector<std::pair<PagePlan, std::vector<bool>>> m_undo;
            uint64_t m_revision = 0;

            std::map<std::pair<std::wstring, uint32_t>, PagePicture> m_thumbs;
            uint64_t m_paint = 0;
            size_t m_requestedFirst = SIZE_MAX, m_requestedEnd = 0;
            uint64_t m_requestedRevision = ~0ull;

            D2D1_RECT_F m_grid{};
            ScrollList m_scroll;
            DragGesture m_drag;
            size_t m_focus = 0, m_anchor = 0, m_insert = 0;
            bool m_clickedSelected = false, m_showFocus = false;

            bool m_big = false;   // the preview fills the window
            D2D1_RECT_F m_previewRect{};
            RenderJob m_previewWanted;
            PagePicture m_preview;
            std::pair<std::wstring, uint32_t> m_previewKey;

            std::wstring m_editFolder;
            unsigned m_serialCounter = 0, m_editSerial = 0;
            bool m_applying = false;
            std::unique_ptr<EditBatch> m_pending;   // the pages in the editor
        };

        void OpenOrganize(HWND, const std::vector<std::wstring>& files, std::function<void(PagePlan)> done)
        {
            PageViewRequest request;
            request.kind = PageViewKind::Organize;
            request.files = files;
            request.save = [done = std::move(done)](PagePlan plan, std::wstring) { done(std::move(plan)); };
            if (!OpenPageView(std::move(request)))
                log::Warn(L"organize: the window could not open");
        }

        const bool g_installed = [] {
            Editors().organize = &OpenOrganize;
            return true;
        }();
    }

    bool OpenPageView(PageViewRequest request)
    {
        auto window = std::make_unique<PageView>(std::move(request));
        if (!window->Open())
            return false;
        window.release();   // owns itself now
        return true;
    }

    bool ConfirmDiscard(HWND window, const std::wstring& title)
    {
        ask::Question q;
        q.title = title;
        q.heading = L"Close without saving?";
        q.text = L"Your changes to the pages will be lost.";
        q.choices = { { 1, L"Close without saving", L"" } };
        return ask::ChooseNow(window, q) == 1;   // modal over the window
    }

    D2D1_COLOR_F FileTagColor(uint32_t file)
    {
        const uint32_t rgb = kTagColors[file % std::size(kTagColors)];
        return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, 1.f);
    }

    std::wstring FileTagLetter(uint32_t file)
    {
        std::wstring s;
        for (uint32_t n = file + 1; n > 0; n = (n - 1) / 26)
            s.insert(s.begin(), static_cast<wchar_t>(L'A' + (n - 1) % 26));
        return s;   // A..Z, then AA, AB...
    }
}
