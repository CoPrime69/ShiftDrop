// The image canvas window: the picture, fit to the window, with zoom and pan; the tool's overlay;
// a side panel with the tool's options; Apply and Cancel; undo and redo. Drawn with Direct2D in DIPs
// on the editors' frame (EditorFrame.h), so every editor has the same look, follows the
// light/dark setting and the accent colour, and is sharp at every DPI.
//
// The window only previews. The output is rendered by the tool's engine on a worker, from the
// params the tool hands back on Apply.
#include "canvas/Editor.h"
#include "Clipboard.h"
#include "DialogWindow.h"
#include "ErrorText.h"
#include "FormatText.h"
#include "Log.h"
#include "PathUtil.h"
#include "Theme.h"
#include "UiThread.h"
#include "Utf.h"
#include "canvas/Covers.h"
#include "canvas/SignaturePad.h"
#include "EditorChrome.h"
#include "EditorFrame.h"

#include <d2d1_1.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <map>
#include <thread>

namespace sd::canvas
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        constexpr UINT kLoaded = WM_APP + 40;           // lParam: Loaded*, ours to delete
        constexpr UINT kAsyncDone = WM_APP + 41;        // lParam: std::function<void()>*, ours to delete
        constexpr UINT kPrepassProgress = WM_APP + 42;  // wParam: 0..1000
        constexpr UINT kPrepassDone = WM_APP + 43;      // wParam: 1 = open the editor, 0 = apply as is
        constexpr UINT kLayerReady = WM_APP + 44;       // lParam: Layer*, ours to delete
        constexpr UINT kThumbReady = WM_APP + 45;       // lParam: Thumb*, ours to delete

        constexpr float kPad = 16.f;         // DIPs
        constexpr float kGap = chrome::kPanelGap;   // between the panel's rows
        constexpr float kRow = chrome::kControlHeight;
        constexpr double kFitMargin = 24.0;  // around the picture when it is fit to the window
        constexpr UINT kPreviewEdge = 4096;  // the on-screen copy of the picture (D2D's safe bitmap size)
        constexpr UINT kLayerEdge = 1600;    // blur and pixelation previews

        D2D1_COLOR_F D2(Color c) { return D2D1::ColorF(c.r, c.g, c.b, c.a); }
        Color FromD2(const D2D1_COLOR_F& c) { return { c.r, c.g, c.b, c.a }; }
        D2D1_RECT_F Rect(const RectD& r) { return D2D1::RectF(float(r.x), float(r.y), float(r.Right()), float(r.Bottom())); }
        bool In(const D2D1_RECT_F& r, D2D1_POINT_2F p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }

        // Premultiplied BGRA pixels for a D2D bitmap, kept on the CPU so the bitmap can be rebuilt
        // after a device loss.
        struct Pixels
        {
            UINT width = 0, height = 0;
            std::vector<uint8_t> bgra;   // premultiplied
            bool Empty() const { return width == 0 || height == 0; }
        };

        // A copy at most `maxEdge` on its longer side, shrunk by averaging, premultiplied.
        Pixels Shrink(const Raster& src, UINT maxEdge)
        {
            Pixels out;
            if (src.Empty())
                return out;
            const double s = std::min(1.0, static_cast<double>(maxEdge) / std::max(src.width, src.height));
            out.width = std::max(1u, static_cast<UINT>(std::lround(src.width * s)));
            out.height = std::max(1u, static_cast<UINT>(std::lround(src.height * s)));
            out.bgra.resize(static_cast<size_t>(out.width) * out.height * 4);
            const double fx = static_cast<double>(src.width) / out.width, fy = static_cast<double>(src.height) / out.height;
            for (UINT y = 0; y < out.height; ++y)
            {
                const UINT y0 = static_cast<UINT>(y * fy), y1 = std::max(y0 + 1, std::min(src.height, static_cast<UINT>(std::ceil((y + 1) * fy))));
                for (UINT x = 0; x < out.width; ++x)
                {
                    const UINT x0 = static_cast<UINT>(x * fx), x1 = std::max(x0 + 1, std::min(src.width, static_cast<UINT>(std::ceil((x + 1) * fx))));
                    uint64_t sum[4]{};
                    for (UINT sy = y0; sy < y1; ++sy)
                        for (UINT sx = x0; sx < x1; ++sx)
                        {
                            const uint8_t* p = src.At(sx, sy);
                            for (int c = 0; c < 3; ++c)
                                sum[c] += uint64_t{ p[c] } * p[3];
                            sum[3] += p[3];
                        }
                    const uint64_t n = uint64_t{ x1 - x0 } * (y1 - y0);
                    uint8_t* d = out.bgra.data() + (static_cast<size_t>(y) * out.width + x) * 4;
                    for (int c = 0; c < 3; ++c)
                        d[c] = static_cast<uint8_t>(sum[c] / (255 * n));
                    d[3] = static_cast<uint8_t>(sum[3] / n);
                }
            }
            return out;
        }

        Raster ShrinkStraight(const Raster& src, UINT maxEdge)
        {
            const Pixels p = Shrink(src, maxEdge);
            Raster r;
            if (!r.Allocate(p.width, p.height))
                return r;
            for (size_t i = 0; i < p.bgra.size(); i += 4)
            {
                const unsigned a = p.bgra[i + 3];
                for (int c = 0; c < 3; ++c)
                    r.px[i + c] = static_cast<uint8_t>(a ? std::min(255u, (p.bgra[i + c] * 255u + a / 2) / a) : 0);
                r.px[i + 3] = static_cast<uint8_t>(a);
            }
            return r;
        }

        struct Loaded
        {
            uint64_t generation = 0;
            size_t index = 0;
            std::shared_ptr<const Raster> full;
            Pixels preview;
            bool alpha = false;
            uint64_t bytes = 0;   // the file's size, for the footer
            std::wstring error;
        };

        struct Layer
        {
            uint64_t generation = 0;
            Effect effect = Effect::Blur;
            Pixels pixels;
        };

        // A small copy of a picture a Pictures control offers (a saved signature).
        struct Thumb
        {
            std::wstring path;
            Pixels pixels;
        };

        constexpr float kDropRow = 28.f;     // a row of a dropdown's list
        constexpr int kDropRows = 12;        // rows the list shows at once
        constexpr float kTileH = 64.f;       // a picture tile

        LPCWSTR CursorId(CursorShape shape)
        {
            switch (shape)
            {
            case CursorShape::Move: return IDC_SIZEALL;
            case CursorShape::SizeNS: return IDC_SIZENS;
            case CursorShape::SizeWE: return IDC_SIZEWE;
            case CursorShape::SizeNWSE: return IDC_SIZENWSE;
            case CursorShape::SizeNESW: return IDC_SIZENESW;
            case CursorShape::Cross: return IDC_CROSS;
            case CursorShape::Hand: return IDC_HAND;
            case CursorShape::Busy: return IDC_APPSTARTING;
            case CursorShape::Text: return IDC_IBEAM;
            case CursorShape::Arrow: break;
            }
            return IDC_ARROW;
        }

        std::wstring Number(double value, int decimals)
        {
            return std::format(L"{:.{}f}", value, std::max(0, decimals));
        }

        class CanvasWindow final : public EditorFrame, public ICanvasHost
        {
        public:
            explicit CanvasWindow(EditorRequest request)
                : m_files(std::move(request.files)), m_tool(std::move(request.tool)), m_apply(std::move(request.apply)),
                  m_closed(std::move(request.closed)), m_alive(std::make_shared<std::atomic<bool>>(true))
            {
                m_tool->Attach(this);
            }

            ~CanvasWindow() override
            {
                if (auto closed = std::move(m_closed))
                    closed();   // closed without applying
            }

            bool Open()
            {
                if (auto prepass = m_tool->MakePrepass())
                {
                    m_mode = Mode::Prepass;
                    m_prepassMessage = prepass->message;
                    m_buttons.push_back(Button{ L"Cancel", 0, false, true, {}, [this] { Close(); } });
                    if (!Create(m_tool->Title(), 440, 150, 360, 140))
                        return false;
                    RunPrepass(std::move(prepass));
                    return true;
                }
                if (!Create(m_tool->Title(), 1180, 760, 760, 480))
                    return false;
                StartEditor();
                return true;
            }

            // ---- ICanvasHost ------------------------------------------------------------------------
            void Redraw() override { Invalidate(); }
            void Changed() override { RecordUndo(); }
            void SetStatus(const std::wstring& text) override
            {
                m_status = text;
                Invalidate();
            }
            void RunAsync(std::function<std::function<void()>(std::stop_token)> work) override
            {
                ++m_jobs;
                // The worker keeps the tool alive (it may touch it) even if the window closes first.
                std::thread([work = std::move(work), keep = m_tool, hwnd = Hwnd(), token = m_stop.get_token()] {
                    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    std::function<void()> then;
                    try
                    {
                        then = work(token);
                    }
                    catch (const std::exception& e)
                    {
                        log::Error(L"editor: background work failed: {}", Widen(e.what()));
                    }
                    if (SUCCEEDED(co))
                        CoUninitialize();
                    auto* posted = new std::function<void()>(std::move(then));
                    if (token.stop_requested() || !PostMessageW(hwnd, kAsyncDone, 0, reinterpret_cast<LPARAM>(posted)))
                        delete posted;
                }).detach();
                Invalidate();
            }
            bool Busy() const override { return m_jobs > 0; }
            std::shared_ptr<const Raster> Picture() const override { return m_full; }
            size_t FileIndex() const override { return m_index; }
            void ShowFile(size_t index) override
            {
                if (index < m_files.size() && index != m_index && m_tool->PerFile())
                    Load(index);
            }
            bool PickImage(std::wstring& path) override { return PickPictureFile(Hwnd(), path); }
            bool CopyText(const std::wstring& text) override { return clipboard::SetText(Hwnd(), text); }
            bool PasteText(std::wstring& text) override { return clipboard::GetText(Hwnd(), text); }
            bool NewSignature(bool fromPicture, std::function<void(std::shared_ptr<const Raster>, bool)> done) override
            {
                return OpenSignaturePad(Hwnd(), fromPicture, std::move(done));
            }

        private:
            enum class Mode { Prepass, Editor };

            // ---- Prepass ----------------------------------------------------------------------------
            void RunPrepass(std::unique_ptr<Prepass> prepass)
            {
                std::thread([run = std::move(prepass->run), keep = m_tool, hwnd = Hwnd(), token = m_stop.get_token()] {
                    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    bool open = false;
                    try
                    {
                        open = run(token, [hwnd](double p) { PostMessageW(hwnd, kPrepassProgress, static_cast<WPARAM>(std::clamp(p, 0.0, 1.0) * 1000), 0); });
                    }
                    catch (const std::exception& e)
                    {
                        log::Error(L"editor: the check failed: {}", Widen(e.what()));
                    }
                    if (SUCCEEDED(co))
                        CoUninitialize();
                    if (!token.stop_requested())
                        PostMessageW(hwnd, kPrepassDone, open ? 1 : 0, 0);
                }).detach();
            }

            void StartEditor()
            {
                m_mode = Mode::Editor;
                m_buttons.clear();
                m_buttons.push_back(Button{ L"", 0xE7A7, false, true, {}, [this] { Undo(); }, true, true });   // Undo
                m_buttons.push_back(Button{ L"", 0xE7A6, false, true, {}, [this] { Redo(); }, true, true });   // Redo
                m_buttons.push_back(Button{ L"", 0xE76B, false, true, {}, [this] { ShowFile(m_index ? m_index - 1 : m_files.size() - 1); }, true, true });
                m_buttons.push_back(Button{ L"", 0xE76C, false, true, {}, [this] { ShowFile((m_index + 1) % m_files.size()); }, true, true });
                m_buttons.push_back(Button{ L"Cancel", 0, false, true, {}, [this] { Close(); } });
                m_buttons.push_back(Button{ m_tool->ApplyLabel(), 0, true, true, {}, [this] { Apply(); } });
                Layout(ClientDip());
                UpdateTitle();
                Load(0);
            }

            void GrowToEditor()
            {
                // From the small "Checking..." window to the full editor, on the same monitor.
                MONITORINFO monitor{ sizeof(monitor) };
                GetMonitorInfoW(MonitorFromWindow(Hwnd(), MONITOR_DEFAULTTONEAREST), &monitor);
                const RECT work = monitor.rcWork;
                const UINT dpi = GetDpiForWindow(Hwnd());
                RECT r{ 0, 0, MulDiv(1180, static_cast<int>(dpi), 96), MulDiv(760, static_cast<int>(dpi), 96) };
                AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
                const int w = std::min<int>(r.right - r.left, work.right - work.left);
                const int h = std::min<int>(r.bottom - r.top, work.bottom - work.top);
                StartEditor();
                SetWindowPos(Hwnd(), nullptr, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w, h,
                             SWP_NOZORDER);
                BringToFront(Hwnd());
            }

            // ---- Files ------------------------------------------------------------------------------
            void Load(size_t index)
            {
                m_index = index;
                m_full.reset();
                m_picture.Reset();
                m_preview = {};
                m_layers[0] = m_layers[1] = {};
                m_layerBitmaps[0].Reset();
                m_layerBitmaps[1].Reset();
                m_layerPending[0] = m_layerPending[1] = false;
                m_loadError.clear();
                m_fileBytes = 0;
                ++m_generation;
                UpdateTitle();
                std::thread([hwnd = Hwnd(), path = m_files[index], generation = m_generation, index, alive = m_alive] {
                    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    auto out = std::make_unique<Loaded>();
                    out->generation = generation;
                    out->index = index;
                    WIN32_FILE_ATTRIBUTE_DATA attributes{};
                    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
                        out->bytes = (uint64_t{ attributes.nFileSizeHigh } << 32) | attributes.nFileSizeLow;
                    auto full = std::make_shared<Raster>();
                    if (SUCCEEDED(LoadRaster(path, *full, out->error)))
                    {
                        out->preview = Shrink(*full, kPreviewEdge);
                        for (size_t i = 3; i < full->px.size() && !out->alpha; i += 4)
                            out->alpha = full->px[i] != 255;
                        out->full = std::move(full);
                    }
                    else if (out->error.empty())
                        out->error = L"this picture could not be opened";
                    if (SUCCEEDED(co))
                        CoUninitialize();
                    if (alive->load() && PostMessageW(hwnd, kLoaded, 0, reinterpret_cast<LPARAM>(out.get())))
                        out.release();
                }).detach();
                Invalidate();
            }

            void OnLoaded(std::unique_ptr<Loaded> loaded)
            {
                if (loaded->generation != m_generation)
                    return;
                if (!loaded->full)
                {
                    m_loadError = loaded->error;
                    log::Warn(L"editor: cannot open {}: {}", m_files[m_index], m_loadError);
                    Invalidate();
                    return;
                }
                m_full = loaded->full;
                m_fileBytes = loaded->bytes;
                m_preview = std::move(loaded->preview);
                m_alpha = loaded->alpha;
                m_tool->ImageReady(loaded->index, static_cast<int>(m_full->width), static_cast<int>(m_full->height));
                if (m_undo.empty())
                    RecordUndo();
                m_viewImage = {};
                Invalidate();
            }

            void UpdateTitle()
            {
                if (!Hwnd() || m_files.empty())
                    return;
                const auto name = FileName(m_files[m_index]);
                SetWindowTextW(Hwnd(), (name + L" · " + m_tool->Title()).c_str());
            }

            // The picture's size in tool coordinates (after the tool's quarter turns).
            SizeD ToolImageSize() const
            {
                if (!m_full)
                    return {};
                if (const auto* preview = m_tool->Preview())
                    return preview->Size();
                const auto rot = m_tool->Rotation();
                return (rot.quarterTurns & 1) ? SizeD{ double(m_full->height), double(m_full->width) } : SizeD{ double(m_full->width), double(m_full->height) };
            }

            // Pixels of the full picture -> DIPs on screen, through the tool's rotation and the view.
            D2D1::Matrix3x2F PictureTransform(double sourcePerBitmapPixel) const
            {
                const auto rot = m_tool->Rotation();
                const float W = float(m_full->width), H = float(m_full->height);
                auto m = D2D1::Matrix3x2F::Scale(float(sourcePerBitmapPixel), float(sourcePerBitmapPixel));
                switch (rot.quarterTurns & 3)
                {
                case 1: m = m * D2D1::Matrix3x2F::Rotation(90.f) * D2D1::Matrix3x2F::Translation(H, 0); break;
                case 2: m = m * D2D1::Matrix3x2F::Rotation(180.f) * D2D1::Matrix3x2F::Translation(W, H); break;
                case 3: m = m * D2D1::Matrix3x2F::Rotation(270.f) * D2D1::Matrix3x2F::Translation(0, W); break;
                default: break;
                }
                const SizeD r = ToolImageSize();
                if (rot.degrees != 0)
                    m = m * D2D1::Matrix3x2F::Rotation(float(rot.degrees), D2D1::Point2F(float(r.w / 2), float(r.h / 2)));
                const float z = float(m_view.Zoom());
                const PointD o = m_view.Offset();
                return m * D2D1::Matrix3x2F::Scale(z, z) * D2D1::Matrix3x2F::Translation(float(o.x), float(o.y));
            }

            // ---- Layout -----------------------------------------------------------------------------
            // The tool bar, the picture, the options panel and the footer (EditorChrome.h).
            static chrome::FrameRects Frame(D2D1_SIZE_F size) { return chrome::LayoutFrame(size.width, size.height); }
            D2D1_RECT_F CanvasRect(D2D1_SIZE_F size) const { return Frame(size).stage; }
            D2D1_RECT_F PanelRect(D2D1_SIZE_F size) const { return Frame(size).panel; }
            bool Switcher() const { return m_tool->PerFile() && m_files.size() > 1; }

            void Layout(D2D1_SIZE_F size) override
            {
                if (m_mode == Mode::Prepass)
                {
                    if (!m_buttons.empty())
                        m_buttons[0].rect = D2D1::RectF(size.width - kPad - 96, size.height - kPad - kRow, size.width - kPad, size.height - kPad);
                    return;
                }
                const auto frame = Frame(size);
                const auto& canvas = frame.stage;
                m_view.SetViewport({ canvas.left, canvas.top, canvas.right - canvas.left, canvas.bottom - canvas.top });
                // The tool bar: the tool's tab, the file switcher; undo, redo and the zoom on the right.
                const auto& bar = frame.toolBar;
                const auto right = chrome::LayoutToolBarRight(bar, true, true);
                const float cy = (bar.top + bar.bottom) / 2;
                const float tabW = chrome::TabWidth(TextWidth(m_tool->ToolName(), 12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD));
                m_tab = D2D1::RectF(bar.left + chrome::kBarPadX, cy - chrome::kTabHeight / 2, bar.left + chrome::kBarPadX + tabW, cy + chrome::kTabHeight / 2);
                m_zoomField = right.zoom;
                m_barDivider = right.divider;
                m_barText = D2D1::RectF(m_tab.right + 16, bar.top, std::max(m_tab.right + 16, right.left - 12), bar.bottom);
                if (m_buttons.size() >= 6)
                {
                    m_buttons[0].rect = right.undo;
                    m_buttons[1].rect = right.redo;
                    const float sx = m_tab.right + 16;
                    m_buttons[2].rect = Switcher() ? D2D1::RectF(sx, cy - 16, sx + 32, cy + 16) : D2D1::RectF();
                    m_buttons[3].rect = Switcher() ? D2D1::RectF(sx + 36, cy - 16, sx + 68, cy + 16) : D2D1::RectF();
                    if (Switcher())
                        m_barText.left = sx + 80;
                    LayoutFooter(frame.footer);
                }
            }

            // Cancel and Apply on the footer's right; Apply's label can change (Redact, Redact all).
            void LayoutFooter(const D2D1_RECT_F& footer)
            {
                if (m_buttons.size() < 6)
                    return;
                const float by = (footer.top + footer.bottom - kRow) / 2;
                const float applyW = std::max(80.f, TextWidth(m_buttons[5].text, kControlText, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 28);
                const float cancelW = std::max(72.f, TextWidth(m_buttons[4].text, kControlText) + 28);
                const float ax = footer.right - chrome::kFooterPadX;
                m_buttons[5].rect = D2D1::RectF(ax - applyW, by, ax, by + kRow);
                m_buttons[4].rect = D2D1::RectF(ax - applyW - 8 - cancelW, by, ax - applyW - 8, by + kRow);
            }

            // Where the panel's controls start: under the tool's name.
            float ContentTop() const
            {
                return PanelRect(ClientDip()).top + chrome::kPanelPadY + chrome::kPanelTitleHeight + chrome::kPanelGap;
            }

            // ---- Painting ---------------------------------------------------------------------------
            void Paint(ID2D1RenderTarget* rt, D2D1_SIZE_F size) override
            {
                if (m_mode == Mode::Prepass)
                {
                    PaintPrepass(rt, size);
                    return;
                }
                // The rotation (Crop) decides the picture's shape; refit when it changes.
                const SizeD image = ToolImageSize();
                if (m_full && (image.w != m_viewImage.w || image.h != m_viewImage.h))
                {
                    m_view.SetImage(image);
                    m_view.Fit(kFitMargin);
                    m_viewImage = image;
                }
                const auto canvas = CanvasRect(size);
                if (auto* b = Brush(Theme().stage))
                    rt->FillRectangle(canvas, b);
                rt->PushAxisAlignedClip(canvas, D2D1_ANTIALIAS_MODE_ALIASED);
                if (m_full)
                {
                    PaintPicture(rt);
                    Painter painter(*this, rt, canvas);
                    m_tool->Draw(painter);
                }
                else
                {
                    const std::wstring text = m_loadError.empty() ? std::wstring(L"Opening…") : L"Can't open this picture: " + m_loadError;
                    DrawText(text, TextFormat(14.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), canvas, Theme().textDim);
                }
                rt->PopAxisAlignedClip();
                PaintToolBar(rt, size);
                PaintPanel(rt, size);
                PaintDrop(rt, size);
            }

            // The tool's tab, the file switcher or the number of pictures, and the zoom.
            void PaintToolBar(ID2D1RenderTarget* rt, D2D1_SIZE_F size)
            {
                const auto& c = Theme();
                DrawToolBar(Frame(size).toolBar);
                DrawToolTab(m_tab, m_tool->Icon(), m_tool->ToolName(), true, {});
                std::wstring text;
                if (Switcher())
                    text = std::format(L"{} of {} · {}", m_index + 1, m_files.size(), FileName(m_files[m_index]));
                else if (m_files.size() > 1)
                    text = std::format(L"{} pictures", m_files.size());
                else if (!m_files.empty())
                    text = FileName(m_files[0]);
                DrawText(text, TextFormat(12.5f), m_barText, c.textDim);
                DrawToolBarDivider(m_barDivider);
                DrawDropField(m_zoomField, chrome::ZoomLabel(m_view.Zoom(), m_view.IsFit()), m_drop.id == kZoomDrop,
                              { m_full != nullptr, In(m_zoomField, m_mouse), false, false });
                (void)rt;
            }

            void PaintPrepass(ID2D1RenderTarget* rt, D2D1_SIZE_F size)
            {
                const auto& c = Theme();
                DrawText(m_prepassMessage + L"…", TextFormat(14.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(kPad, kPad, size.width - kPad, kPad + 24),
                         c.text);
                DrawText(L"Nothing leaves this PC. Windows reads the text.", TextFormat(12.f), D2D1::RectF(kPad, kPad + 26, size.width - kPad, kPad + 44),
                         c.textDim);
                const auto track = D2D1::RectF(kPad, kPad + 58, size.width - kPad, kPad + 62);
                if (auto* b = Brush(c.line))
                    rt->FillRoundedRectangle(D2D1::RoundedRect(track, 2, 2), b);
                auto done = track;
                done.right = track.left + (track.right - track.left) * float(m_prepassProgress);
                if (auto* b = Brush(c.accent))
                    rt->FillRoundedRectangle(D2D1::RoundedRect(done, 2, 2), b);
                DrawButtons();
            }

            // A tool's own result (IPreview): rendered into a bitmap on a second device context of
            // the window's Direct2D device (resources are shared between a device's contexts), at
            // about the screen's resolution for the current zoom, and redrawn only when the tool's
            // revision or that resolution changes.
            bool PaintPreview(ID2D1RenderTarget* rt, IPreview& preview)
            {
                ComPtr<ID2D1DeviceContext> windowDc;
                if (FAILED(rt->QueryInterface(IID_PPV_ARGS(&windowDc))))
                    return false;
                if (!m_previewDc || m_previewGeneration != DeviceGeneration())
                {
                    m_previewDc.Reset();
                    m_previewTarget.Reset();
                    ComPtr<ID2D1Device> device;
                    windowDc->GetDevice(&device);
                    if (!device || FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_previewDc)))
                        return false;
                    m_previewDc->SetDpi(96.f, 96.f);
                    m_previewGeneration = DeviceGeneration();
                    m_previewFailed = false;
                }
                if (m_previewFailed)
                    return false;
                const SizeD size = preview.Size();
                if (size.w < 1 || size.h < 1)
                    return false;
                // Screen pixels per result pixel, rounded up to a power of two so zooming doesn't
                // re-render on every notch; never more than the result itself or the bitmap limit.
                const double needed = m_view.Zoom() * Scale();
                double scale = std::min(1.0, std::exp2(std::ceil(std::log2(std::max(needed, 1.0 / 64)))));
                const double limit = std::min<double>(kPreviewEdge, m_previewDc->GetMaximumBitmapSize());
                scale = std::min(scale, limit / std::max(size.w, size.h));
                const UINT w = std::max(1u, static_cast<UINT>(std::ceil(size.w * scale)));
                const UINT h = std::max(1u, static_cast<UINT>(std::ceil(size.h * scale)));
                if (!m_previewTarget || m_previewRevision != preview.Revision() || m_previewScale != scale)
                {
                    const auto px = m_previewTarget ? m_previewTarget->GetPixelSize() : D2D1_SIZE_U{};
                    if (px.width != w || px.height != h)
                    {
                        m_previewTarget.Reset();
                        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
                        if (FAILED(m_previewDc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, &m_previewTarget)))
                            return false;
                    }
                    m_previewDc->SetTarget(m_previewTarget.Get());
                    m_previewDc->BeginDraw();
                    m_previewDc->SetTransform(D2D1::Matrix3x2F::Identity());
                    m_previewDc->Clear(D2D1::ColorF(0, 0, 0, 0));
                    const bool drawn = preview.Render(m_previewDc.Get(), scale, DeviceGeneration());
                    const HRESULT hr = m_previewDc->EndDraw();
                    m_previewDc->SetTarget(nullptr);   // a bitmap can't be drawn while it is a target
                    if (FAILED(hr) || !drawn)
                    {
                        if (FAILED(hr))
                            log::Warn(L"editor: the preview could not be drawn ({})", HexCode(hr));
                        m_previewFailed = FAILED(hr);
                        m_previewTarget.Reset();
                        return false;
                    }
                    m_previewRevision = preview.Revision();
                    m_previewScale = scale;
                }
                Pixels shape;
                shape.width = w;
                shape.height = h;
                DrawPictureBitmap(rt, m_previewTarget.Get(), PictureTransform(1.0 / scale), shape, 1.0 / scale);
                return true;
            }

            void PaintPicture(ID2D1RenderTarget* rt)
            {
                if (auto* preview = m_tool->Preview(); preview && PaintPreview(rt, *preview))
                    return;
                if (!m_picture || m_pictureGeneration != DeviceGeneration())
                {
                    m_picture = MakeBitmap(rt, m_preview);
                    m_layerBitmaps[0].Reset();
                    m_layerBitmaps[1].Reset();
                    m_pictureGeneration = DeviceGeneration();
                }
                if (!m_picture)
                    return;
                const auto transform = PictureTransform(double(m_full->width) / m_preview.width);
                if (m_alpha)
                    PaintChecker(rt, PictureTransform(1.0));
                DrawPictureBitmap(rt, m_picture.Get(), transform, m_preview, double(m_full->width) / m_preview.width);
            }

            void DrawPictureBitmap(ID2D1RenderTarget* rt, ID2D1Bitmap* bitmap, const D2D1::Matrix3x2F& transform, const Pixels& pixels, double sourcePerPixel)
            {
                // Sharp pixels once each is bigger than two screen pixels; smooth (cubic) below that.
                const double screenPerPixel = m_view.Zoom() * sourcePerPixel * Scale();
                const auto dest = D2D1::RectF(0, 0, float(pixels.width), float(pixels.height));
                rt->SetTransform(transform);
                ComPtr<ID2D1DeviceContext> dc;
                if (SUCCEEDED(rt->QueryInterface(IID_PPV_ARGS(&dc))))
                    dc->DrawBitmap(bitmap, dest, 1.f, screenPerPixel > 2 ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                else
                    rt->DrawBitmap(bitmap, dest, 1.f, screenPerPixel > 2 ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                rt->SetTransform(D2D1::Matrix3x2F::Identity());
            }

            // A checkerboard of fixed screen size behind a transparent picture.
            void PaintChecker(ID2D1RenderTarget* rt, const D2D1::Matrix3x2F& transform)
            {
                if (!m_checker || m_checkerGeneration != DeviceGeneration())
                {
                    m_checker.Reset();
                    m_checkerGeneration = DeviceGeneration();
                    const bool dark = Theme().dark;
                    const uint32_t a = dark ? 0xFF3A3A3A : 0xFFFFFFFF, b = dark ? 0xFF2C2C2C : 0xFFE0E0E0;
                    uint32_t px[16 * 16];
                    for (int y = 0; y < 16; ++y)
                        for (int x = 0; x < 16; ++x)
                            px[y * 16 + x] = ((x < 8) == (y < 8)) ? a : b;
                    ComPtr<ID2D1Bitmap> bmp;
                    const auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
                    if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(16, 16), px, 64, props, &bmp)))
                        rt->CreateBitmapBrush(bmp.Get(), D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP), &m_checker);
                }
                ComPtr<ID2D1Factory> factory;
                rt->GetFactory(&factory);
                ComPtr<ID2D1RectangleGeometry> rect;
                ComPtr<ID2D1TransformedGeometry> shape;
                if (!m_checker || FAILED(factory->CreateRectangleGeometry(D2D1::RectF(0, 0, float(m_full->width), float(m_full->height)), &rect)) ||
                    FAILED(factory->CreateTransformedGeometry(rect.Get(), transform, &shape)))
                    return;
                rt->FillGeometry(shape.Get(), m_checker.Get());
            }

            ComPtr<ID2D1Bitmap> MakeBitmap(ID2D1RenderTarget* rt, const Pixels& p)
            {
                ComPtr<ID2D1Bitmap> bmp;
                if (p.Empty())
                    return bmp;
                const auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
                if (FAILED(rt->CreateBitmap(D2D1::SizeU(p.width, p.height), p.bgra.data(), p.width * 4, props, &bmp)))
                    log::Warn(L"editor: cannot make a {}x{} bitmap", p.width, p.height);
                return bmp;
            }

            // The blur or pixelation of the whole picture, at preview size, for DrawEffect.
            ID2D1Bitmap* EffectBitmap(ID2D1RenderTarget* rt, Effect effect)
            {
                const int i = effect == Effect::Blur ? 0 : 1;
                if (!m_layers[i].Empty())
                {
                    if (!m_layerBitmaps[i])
                        m_layerBitmaps[i] = MakeBitmap(rt, m_layers[i]);
                    return m_layerBitmaps[i].Get();
                }
                if (!m_layerPending[i] && m_full)
                {
                    m_layerPending[i] = true;
                    std::thread([hwnd = Hwnd(), full = m_full, effect, generation = m_generation, alive = m_alive] {
                        Raster reduced = ShrinkStraight(*full, kLayerEdge);
                        const double s = static_cast<double>(reduced.width) / full->width;
                        if (effect == Effect::Blur)
                            BlurAll(reduced, BlurSigma(full->width, full->height) * s);
                        else
                            PixelateAll(reduced, std::max(1, static_cast<int>(std::lround(PixelBlock(full->width, full->height) * s))));
                        auto layer = std::make_unique<Layer>();
                        layer->generation = generation;
                        layer->effect = effect;
                        layer->pixels = Shrink(reduced, kLayerEdge);   // premultiplied, same size
                        if (alive->load() && PostMessageW(hwnd, kLayerReady, 0, reinterpret_cast<LPARAM>(layer.get())))
                            layer.release();
                    }).detach();
                }
                return nullptr;
            }

            // ---- The side panel ---------------------------------------------------------------------
            struct Item
            {
                size_t control = 0;
                D2D1_RECT_F rect{};
                std::vector<D2D1_RECT_F> parts;   // chips, swatches, a slider's track
                D2D1_RECT_F track{};              // a segmented choice's track
            };

            float TextHeight(const std::wstring& text, float width, float sizeDip)
            {
                if (!DWrite())
                    return sizeDip * 1.4f;
                if (!m_wrapped || m_wrappedSize != sizeDip)
                {
                    m_wrapped.Reset();
                    DWrite()->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                               sizeDip, L"", &m_wrapped);
                    m_wrappedSize = sizeDip;
                }
                ComPtr<IDWriteTextLayout> layout;
                if (!m_wrapped || FAILED(DWrite()->CreateTextLayout(text.c_str(), UINT32(text.size()), m_wrapped.Get(), width, 1000.f, &layout)))
                    return sizeDip * 1.4f;
                DWRITE_TEXT_METRICS m{};
                layout->GetMetrics(&m);
                return m.height;
            }

            void LayoutPanel(const D2D1_RECT_F& panel)
            {
                m_controls.clear();
                m_tool->BuildPanel(m_controls);
                m_items.clear();
                const float left = panel.left + chrome::kPanelPadX, right = panel.right - chrome::kPanelPadX, width = right - left;
                float y = ContentTop() - m_scroll;
                for (size_t i = 0; i < m_controls.size(); ++i)
                {
                    const auto& c = m_controls[i];
                    Item item;
                    item.control = i;
                    // Two half-width controls in a row.
                    const bool pairWithPrevious = c.half && i > 0 && m_controls[i - 1].half && !m_items.empty() &&
                                                  m_items.back().rect.left == left && m_items.back().rect.right < right - 1 &&
                                                  m_controls[i - 1].kind == c.kind;
                    if (pairWithPrevious)
                    {
                        const auto& prev = m_items.back().rect;
                        item.rect = D2D1::RectF(prev.right + kGap, prev.top, right, prev.bottom);
                        m_items.push_back(item);
                        continue;
                    }
                    const float halfRight = c.half ? left + (width - kGap) / 2 : right;
                    switch (c.kind)
                    {
                    case ControlKind::Section:
                        y += 6;
                        item.rect = D2D1::RectF(left, y, right, y + 16);
                        y += 16 + kGap;
                        break;
                    case ControlKind::Note:
                    {
                        const float h = TextHeight(c.text, width, 12.f);
                        item.rect = D2D1::RectF(left, y, right, y + h);
                        y += h + kGap;
                        break;
                    }
                    case ControlKind::Button:
                    case ControlKind::Toggle:
                    case ControlKind::Number:
                    case ControlKind::Dropdown:
                        item.rect = D2D1::RectF(left, y, halfRight, y + kRow);
                        y += kRow + kGap;
                        break;
                    case ControlKind::Choice:
                    {
                        std::vector<float> widths;
                        if (c.icons.size() == c.options.size() && !c.icons.empty())
                        {
                            // Tiles of an icon over a name, five to a row.
                            const float w = (width - 4 * 4) / 5;
                            widths.assign(c.options.size(), w);
                            item.parts = chrome::Flow(widths, left, right, y, 50, 4, 4);
                        }
                        else
                        {
                            for (const auto& option : c.options)
                                widths.push_back(TextWidth(option, kControlText, DWRITE_FONT_WEIGHT_SEMI_BOLD));
                            // One segmented control when it fits (its track is the item's rectangle), else chips.
                            if (auto segments = c.chips ? std::vector<D2D1_RECT_F>{} : chrome::Segments(widths, left, y, width); !segments.empty())
                            {
                                item.track = segments.front();
                                item.parts.assign(segments.begin() + 1, segments.end());
                            }
                            else
                            {
                                for (size_t k = 0; k < widths.size(); ++k)
                                    widths[k] = std::min(width, chrome::ChipWidth(widths[k], static_cast<int>(k) == c.selected));
                                item.parts = chrome::Flow(widths, left, right, y, kRow, 6, 6);
                            }
                        }
                        float bottom = y;
                        for (const auto& part : item.parts)
                            bottom = std::max(bottom, part.bottom);
                        if (item.track.bottom > item.track.top)
                            bottom = std::max(bottom, item.track.bottom);
                        item.rect = D2D1::RectF(left, y, right, bottom);
                        y = bottom + kGap;
                        break;
                    }
                    case ControlKind::Slider:
                        // The label and value over the bar; the knob is 16 DIPs.
                        item.rect = D2D1::RectF(left, y, right, y + 32);
                        item.parts.push_back(D2D1::RectF(left + 8, y + 24, right - 8, y + 28));
                        y += 32 + kGap;
                        break;
                    case ControlKind::Swatches:
                    {
                        // 26 DIP colours 8 apart; the kit's swatch keeps room for its ring around them.
                        float x = left - 4;
                        if (!c.label.empty())
                            x += 60;
                        for (size_t s = 0; s < c.swatches.size(); ++s)
                        {
                            item.parts.push_back(D2D1::RectF(x, y, x + 34, y + 34));
                            x += 34;
                        }
                        item.rect = D2D1::RectF(left, y, right, y + 34);
                        y += 34 + kGap;
                        break;
                    }
                    case ControlKind::Icons:
                    {
                        // One row of square buttons, as many as fit.
                        const float n = float(std::max<size_t>(1, c.icons.size()));
                        const float side = std::min(kRow, (width - (n - 1) * 3) / n);
                        for (size_t k = 0; k < c.icons.size(); ++k)
                        {
                            const float x = left + k * (side + 3);
                            item.parts.push_back(D2D1::RectF(x, y, x + side, y + side));
                        }
                        item.rect = D2D1::RectF(left, y, right, y + side);
                        y += side + kGap;
                        break;
                    }
                    case ControlKind::Pictures:
                    {
                        // Two tiles a row.
                        const float w = (width - kGap) / 2;
                        for (size_t k = 0; k < c.pictures.size(); ++k)
                        {
                            const float x = left + (k % 2) * (w + kGap), top = y + (k / 2) * (kTileH + kGap);
                            item.parts.push_back(D2D1::RectF(x, top, x + w, top + kTileH));
                        }
                        const float rows = float((c.pictures.size() + 1) / 2);
                        item.rect = D2D1::RectF(left, y, right, y + std::max(0.f, rows * (kTileH + kGap) - kGap));
                        y += rows * (kTileH + kGap);
                        break;
                    }
                    }
                    m_items.push_back(std::move(item));
                }
                m_contentHeight = y + m_scroll - ContentTop();
            }

            void PaintPanel(ID2D1RenderTarget* rt, D2D1_SIZE_F size)
            {
                const auto frame = Frame(size);
                const auto& panel = frame.panel;
                DrawSidePanel(panel, m_tool->ToolName());

                // The tool's controls, scrollable under its name.
                LayoutPanel(panel);
                const float top = ContentTop() - 4, bottom = panel.bottom;
                const float overflow = std::max(0.f, m_contentHeight - (bottom - top) + chrome::kPanelPadY);
                if (m_scroll > overflow)
                {
                    m_scroll = overflow;
                    LayoutPanel(panel);
                }
                rt->PushAxisAlignedClip(D2D1::RectF(panel.left + 1, top, panel.right, bottom), D2D1_ANTIALIAS_MODE_ALIASED);
                for (const auto& item : m_items)
                    PaintControl(rt, m_controls[item.control], item);
                rt->PopAxisAlignedClip();
                if (m_drop.id.empty())
                    PaintTip(rt);

                // The footer: what the window is doing or will do, Cancel and Apply.
                std::wstring why;
                const bool canApply = m_full && m_tool->CanApply(&why);
                std::wstring status = m_status;
                if (status.empty())
                    status = !canApply && !why.empty() ? why : PictureSummary() + L"Enter applies · Esc cancels · Ctrl+Z undoes";
                if (Busy())
                    status = (m_status.empty() ? std::wstring(L"Working") : m_status);
                if (m_buttons.size() >= 6)
                {
                    m_buttons[0].enabled = m_undoAt > 0 || m_tool->TextInput();
                    m_buttons[1].enabled = m_undoAt + 1 < m_undo.size();
                    m_buttons[2].enabled = m_buttons[3].enabled = Switcher() && !Busy();
                    m_buttons[5].enabled = canApply && !Busy();
                    m_buttons[5].text = m_tool->ApplyLabel();
                    LayoutFooter(frame.footer);
                }
                DrawFooterBar(frame.footer, status, m_buttons.size() >= 6 ? m_buttons[4].rect.left : frame.footer.right);
                DrawButtons();
            }

            // "4032 × 3024 · 3.4 MB · " for the picture on show, or nothing while it loads.
            std::wstring PictureSummary() const
            {
                if (!m_full)
                    return {};
                std::wstring text = std::format(L"{} × {} · ", m_full->width, m_full->height);
                if (m_fileBytes > 0)
                    text += SizeText(m_fileBytes) + L" · ";
                return text;
            }

            void PaintControl(ID2D1RenderTarget* rt, const Control& c, const Item& item)
            {
                const auto& t = Theme();
                const float alpha = c.enabled ? 1.f : kDisabledAlpha;
                const auto ink = theme::WithAlpha(t.text, alpha);
                const auto dim = theme::WithAlpha(t.textDim, alpha);
                const bool hot = m_hotControl == c.id && !c.id.empty();
                const ControlState state{ c.enabled, hot, false, false };
                switch (c.kind)
                {
                case ControlKind::Section:
                    DrawSectionLabel(item.rect, c.label);
                    break;
                case ControlKind::Note:
                    if (m_wrapped)
                        if (auto* b = Brush(dim))
                            rt->DrawText(c.text.c_str(), UINT32(c.text.size()), m_wrapped.Get(), item.rect, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    break;
                case ControlKind::Button:
                    DrawButton(item.rect, c.label, c.icon, c.primary, state);
                    break;
                case ControlKind::Toggle:
                {
                    DrawText(c.label, TextFormat(kControlText), D2D1::RectF(item.rect.left, item.rect.top, item.rect.right - 84, item.rect.bottom), ink);
                    const float cy = (item.rect.top + item.rect.bottom) / 2;
                    DrawText(c.on ? L"On" : L"Off", TextFormat(kControlText, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING),
                             D2D1::RectF(item.rect.right - 84, item.rect.top, item.rect.right - 52, item.rect.bottom), ink);
                    DrawSwitch(D2D1::RectF(item.rect.right - 40, cy - 10, item.rect.right, cy + 10), c.on, state);
                    break;
                }
                case ControlKind::Choice:
                {
                    const bool tiles = c.icons.size() == c.options.size() && !c.icons.empty();
                    const bool segmented = item.track.bottom > item.track.top;
                    if (segmented)
                        DrawSegmentTrack(item.track);
                    for (size_t i = 0; i < item.parts.size() && i < c.options.size(); ++i)
                    {
                        const bool chosen = static_cast<int>(i) == c.selected;
                        const ControlState part{ c.enabled, hot && m_hotPart == static_cast<int>(i), false, false };
                        if (tiles)
                            DrawToolTile(item.parts[i], c.icons[i], c.options[i], chosen, part);
                        else if (segmented)
                            DrawSegment(item.parts[i], c.options[i], chosen, part);
                        else
                            DrawChip(item.parts[i], c.options[i], chosen, part);
                    }
                    break;
                }
                case ControlKind::Number:
                {
                    const bool editing = m_editId == c.id;
                    DrawField(item.rect, editing, false, state);
                    const auto inner = D2D1::RectF(item.rect.left + 10, item.rect.top, item.rect.right - 10, item.rect.bottom);
                    DrawText(c.label, TextFormat(12.f), inner, dim);
                    std::wstring value = editing ? m_editText : Number(c.value, c.decimals);
                    if (!c.text.empty())
                        value += L" " + c.text;
                    if (editing && (GetTickCount64() / 530) % 2 == 0)
                        value.insert(m_editText.size(), L"|");
                    DrawText(value, TextFormat(kControlText, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING), inner, ink);
                    break;
                }
                case ControlKind::Slider:
                {
                    DrawText(c.label, TextFormat(kControlText), D2D1::RectF(item.rect.left, item.rect.top, item.rect.right - 70, item.rect.top + 18), ink);
                    DrawText(Number(c.value, c.decimals) + c.text, TextFormat(kControlText, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING),
                             D2D1::RectF(item.rect.right - 70, item.rect.top, item.rect.right, item.rect.top + 18), dim);
                    const auto fraction = [&](double v) { return c.max > c.min ? float((v - c.min) / (c.max - c.min)) : 0.f; };
                    // Filled from the middle when the range is centred on zero (straighten), else from the left.
                    DrawSlider(item.parts[0], fraction(c.value), c.min < 0 && c.max > 0 ? fraction(0) : 0.f, { c.enabled, hot, m_sliderId == c.id, false });
                    break;
                }
                case ControlKind::Swatches:
                {
                    if (!c.label.empty())
                        DrawText(c.label, TextFormat(13.f), D2D1::RectF(item.rect.left, item.rect.top, item.rect.left + 58, item.rect.bottom), ink);
                    for (size_t i = 0; i < item.parts.size(); ++i)
                    {
                        const auto& r = item.parts[i];
                        const bool selected = static_cast<int>(i) == c.selected;
                        DrawSwatch(r, D2(c.swatches[i]), 0, t.text, selected, { c.enabled, hot && m_hotPart == static_cast<int>(i), false, false });
                        if (c.custom && i + 1 == item.parts.size())
                        {
                            // The colour picker chip: a small "+" badge.
                            const auto badge = D2D1::Point2F(r.right - 7, r.bottom - 7);
                            if (auto* b = Brush(t.panel))
                                rt->FillEllipse(D2D1::Ellipse(badge, 6.f, 6.f), b);
                            if (auto* b = Brush(t.chipLine))
                                rt->DrawEllipse(D2D1::Ellipse(badge, 5.5f, 5.5f), b);
                            DrawText(std::wstring(1, wchar_t(0xE710)), IconFormat(7.f), D2D1::RectF(badge.x - 6, badge.y - 6, badge.x + 6, badge.y + 6), t.text);   // Add
                        }
                    }
                    break;
                }
                case ControlKind::Dropdown:
                {
                    const bool open = m_drop.id == c.id;
                    DrawField(item.rect, open, false, state);
                    float x = item.rect.left + 10;
                    if (!c.label.empty())
                    {
                        const float w = TextWidth(c.label, 12.f) + 10;
                        DrawText(c.label, TextFormat(12.f), D2D1::RectF(x, item.rect.top, x + w, item.rect.bottom), dim);
                        x += w;
                    }
                    const std::wstring value = c.selected >= 0 && c.selected < static_cast<int>(c.options.size()) ? c.options[c.selected] : c.text;
                    DrawText(value, c.fonts ? FontFormat(value) : TextFormat(kControlText), D2D1::RectF(x, item.rect.top, item.rect.right - 28, item.rect.bottom),
                             ink);
                    DrawText(std::wstring(1, open ? wchar_t(0xE70E) : wchar_t(0xE70D)), IconFormat(10.f),
                             D2D1::RectF(item.rect.right - 26, item.rect.top, item.rect.right - 8, item.rect.bottom), dim);   // ChevronUp / Down
                    break;
                }
                case ControlKind::Icons:
                    for (size_t i = 0; i < item.parts.size() && i < c.icons.size(); ++i)
                        DrawIconButton(item.parts[i], c.icons[i], i < c.flags.size() && c.flags[i],
                                       { c.enabled, hot && m_hotPart == static_cast<int>(i), false, false });
                    break;
                case ControlKind::Pictures:
                    for (size_t i = 0; i < item.parts.size() && i < c.pictures.size(); ++i)
                    {
                        // On a light card whatever the theme: signatures are dark ink.
                        const auto& r = item.parts[i];
                        const bool tileHot = hot && m_hotPart == static_cast<int>(i);
                        FillRound(r, t.paper, 6.f);
                        StrokeRound(r, tileHot ? t.accent : t.controlLine, tileHot ? 1.5f : 1.f, 6.f);
                        if (const auto* thumb = ThumbFor(c.pictures[i]))
                        {
                            const float s = std::min((r.right - r.left - 16) / thumb->width, (r.bottom - r.top - 12) / thumb->height);
                            const float w = thumb->width * s, h = thumb->height * s;
                            const float x = (r.left + r.right - w) / 2, y = (r.top + r.bottom - h) / 2;
                            if (auto* bitmap = ThumbBitmap(rt, c.pictures[i]))
                                rt->DrawBitmap(bitmap, D2D1::RectF(x, y, x + w, y + h), alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                        }
                        if (tileHot)
                        {
                            // The remove mark.
                            const auto x = RemoveRect(r);
                            const bool on = In(x, m_mouse);
                            if (on)
                                FillRound(x, t.danger, 9.f);
                            DrawText(std::wstring(1, wchar_t(0xE711)), IconFormat(10.f), x, on ? t.onDanger : t.paperText);   // Cancel
                        }
                    }
                    break;
                }
            }

            // A Pictures tile's remove mark, in its top-right corner.
            static D2D1_RECT_F RemoveRect(const D2D1_RECT_F& tile) { return D2D1::RectF(tile.right - 22, tile.top + 4, tile.right - 4, tile.top + 22); }

            // The text format a font family's name is shown in: the family itself, at the controls'
            // size (the interface font for a symbol font, whose name would be unreadable).
            IDWriteTextFormat* FontFormat(const std::wstring& family)
            {
                if (const auto it = m_fontFormats.find(family); it != m_fontFormats.end())
                    return it->second ? it->second.Get() : TextFormat(kControlText);
                if (m_fontFormats.size() > 600)
                    m_fontFormats.clear();
                ComPtr<IDWriteTextFormat> format;
                ComPtr<IDWriteFontCollection> fonts;
                UINT32 index = 0;
                BOOL exists = FALSE;
                ComPtr<IDWriteFontFamily> found;
                ComPtr<IDWriteFont> font;
                if (DWrite() && SUCCEEDED(DWrite()->GetSystemFontCollection(&fonts)) && SUCCEEDED(fonts->FindFamilyName(family.c_str(), &index, &exists)) && exists &&
                    SUCCEEDED(fonts->GetFontFamily(index, &found)) &&
                    SUCCEEDED(found->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)) &&
                    !font->IsSymbolFont() &&
                    SUCCEEDED(DWrite()->CreateTextFormat(family.c_str(), fonts.Get(), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                         DWRITE_FONT_STRETCH_NORMAL, 14.f, L"", &format)))
                {
                    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
                    ComPtr<IDWriteInlineObject> ellipsis;
                    DWrite()->CreateEllipsisTrimmingSign(format.Get(), &ellipsis);
                    format->SetTrimming(&trimming, ellipsis.Get());
                }
                auto* out = format.Get();
                m_fontFormats.emplace(family, std::move(format));
                return out ? out : TextFormat(kControlText);
            }

            // A Pictures tile's picture, loaded on a worker the first time it is asked for.
            const Pixels* ThumbFor(const std::wstring& path)
            {
                const auto it = m_thumbs.find(path);
                if (it != m_thumbs.end())
                    return it->second.pixels.Empty() ? nullptr : &it->second.pixels;
                m_thumbs[path];   // pending
                std::thread([hwnd = Hwnd(), path, alive = m_alive] {
                    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    auto out = std::make_unique<Thumb>();
                    out->path = path;
                    Raster r;
                    std::wstring error;
                    if (SUCCEEDED(LoadRaster(path, r, error, 512)))
                        out->pixels = Shrink(r, 256);
                    if (SUCCEEDED(co))
                        CoUninitialize();
                    if (alive->load() && PostMessageW(hwnd, kThumbReady, 0, reinterpret_cast<LPARAM>(out.get())))
                        out.release();
                }).detach();
                return nullptr;
            }

            ID2D1Bitmap* ThumbBitmap(ID2D1RenderTarget* rt, const std::wstring& path)
            {
                auto it = m_thumbs.find(path);
                if (it == m_thumbs.end() || it->second.pixels.Empty())
                    return nullptr;
                auto& t = it->second;
                if (!t.bitmap || t.generation != DeviceGeneration())
                {
                    t.bitmap = MakeBitmap(rt, t.pixels);
                    t.generation = DeviceGeneration();
                }
                return t.bitmap.Get();
            }

            // The name of the icon button under the mouse, below it.
            void PaintTip(ID2D1RenderTarget* rt)
            {
                for (const auto& item : m_items)
                {
                    const auto& c = m_controls[item.control];
                    if (c.kind != ControlKind::Icons || c.id != m_hotControl || m_hotPart < 0 || m_hotPart >= static_cast<int>(c.options.size()) ||
                        m_hotPart >= static_cast<int>(item.parts.size()))
                        continue;
                    const auto& text = c.options[m_hotPart];
                    const auto& r = item.parts[m_hotPart];
                    const auto size = ClientDip();
                    const float w = TextWidth(text, 12.f) + 16, h = 24;
                    const float x = std::clamp((r.left + r.right - w) / 2, PanelRect(size).left + 4, size.width - w - 4);
                    const auto tip = D2D1::RectF(x, r.bottom + 4, x + w, r.bottom + 4 + h);
                    FillRound(D2D1::RectF(tip.left, tip.top + 1, tip.right, tip.bottom + 2), Theme().shadow, 5.f);
                    FillRound(tip, Theme().window, 5.f);
                    StrokeRound(tip, Theme().controlLine, 1.f, 5.f);
                    DrawText(text, TextFormat(12.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), tip, Theme().text);
                    (void)rt;
                }
            }

            // ---- A dropdown's list -------------------------------------------------------------------
            // Drawn over the window below (or above) its field; typing filters it, the arrows, Page
            // Up/Down, Home and End move through it, Enter picks and Esc closes.
            struct DropList
            {
                std::string id;
                std::vector<std::wstring> options;
                bool fonts = false;
                bool search = true;       // a search line on top (not for the zoom's short list)
                int selected = -1;
                std::wstring filter;
                std::vector<int> shown;   // indexes into options
                int hot = 0;              // into shown
                int first = 0;            // the first row shown
                D2D1_RECT_F field{}, rect{};
            };

            void OpenDrop(const Control& c, const D2D1_RECT_F& field)
            {
                m_drop = {};
                m_drop.id = c.id;
                m_drop.options = c.options;
                m_drop.fonts = c.fonts;
                m_drop.selected = c.selected;
                m_drop.field = field;
                FilterDrop();
                for (size_t i = 0; i < m_drop.shown.size(); ++i)
                    if (m_drop.shown[i] == c.selected)
                        m_drop.hot = static_cast<int>(i);
                m_drop.first = std::max(0, m_drop.hot - kDropRows / 2);
                ShowDropHot();
                SetTimer(Hwnd(), 2, 530, nullptr);   // the search line's caret
                Invalidate();
            }

            void CloseDrop()
            {
                if (m_drop.id.empty())
                    return;
                m_drop = {};
                KillTimer(Hwnd(), 2);
                Invalidate();
            }

            void FilterDrop()
            {
                m_drop.shown.clear();
                std::wstring needle = m_drop.filter;
                CharLowerBuffW(needle.data(), static_cast<DWORD>(needle.size()));
                // Names that start with the typed text first, then names that hold it.
                std::vector<int> later;
                for (size_t i = 0; i < m_drop.options.size(); ++i)
                {
                    std::wstring name = m_drop.options[i];
                    CharLowerBuffW(name.data(), static_cast<DWORD>(name.size()));
                    const size_t at = needle.empty() ? 0 : name.find(needle);
                    if (at == 0)
                        m_drop.shown.push_back(static_cast<int>(i));
                    else if (at != std::wstring::npos)
                        later.push_back(static_cast<int>(i));
                }
                m_drop.shown.insert(m_drop.shown.end(), later.begin(), later.end());
                m_drop.hot = 0;
                m_drop.first = 0;
            }

            void ShowDropHot()
            {
                const int n = static_cast<int>(m_drop.shown.size());
                m_drop.hot = std::clamp(m_drop.hot, 0, std::max(0, n - 1));
                if (m_drop.hot < m_drop.first)
                    m_drop.first = m_drop.hot;
                if (m_drop.hot >= m_drop.first + kDropRows)
                    m_drop.first = m_drop.hot - kDropRows + 1;
                m_drop.first = std::clamp(m_drop.first, 0, std::max(0, n - kDropRows));
            }

            void PickDrop(int row)
            {
                if (row < 0 || row >= static_cast<int>(m_drop.shown.size()))
                    return;
                ControlEvent e;
                e.id = m_drop.id;
                e.index = m_drop.shown[row];
                CloseDrop();
                if (e.id == kZoomDrop)
                    PickZoom(e.index);
                else
                    Handle(m_tool->OnControl(e));
            }

            float DropRowsTop() const { return m_drop.rect.top + (m_drop.search ? 36.f : 6.f); }

            // The zoom field's list: Fit and a few sizes.
            static constexpr char kZoomDrop[] = "\x01zoom";   // not a tool control's id

            void OpenZoomDrop()
            {
                if (m_drop.id == kZoomDrop)
                {
                    CloseDrop();
                    return;
                }
                Control zoom;
                zoom.id = kZoomDrop;
                const auto choices = chrome::ZoomChoices();
                for (size_t i = 0; i < choices.size(); ++i)
                {
                    zoom.options.push_back(choices[i].label);
                    if ((choices[i].zoom == 0 && m_view.IsFit()) || (!m_view.IsFit() && std::abs(choices[i].zoom - m_view.Zoom()) < 1e-3))
                        zoom.selected = static_cast<int>(i);
                }
                OpenDrop(zoom, m_zoomField);
                m_drop.search = false;
                KillTimer(Hwnd(), 2);
            }

            void PickZoom(int option)
            {
                const auto choices = chrome::ZoomChoices();
                if (option < 0 || option >= static_cast<int>(choices.size()))
                    return;
                const auto canvas = CanvasRect(ClientDip());
                const PointD centre{ (canvas.left + canvas.right) / 2, (canvas.top + canvas.bottom) / 2 };
                if (choices[option].zoom == 0)
                    m_view.Fit(kFitMargin);
                else
                    m_view.ZoomAt(choices[option].zoom, centre);
                Invalidate();
            }

            int DropRowAt(D2D1_POINT_2F p) const
            {
                if (!In(m_drop.rect, p) || p.y < DropRowsTop())
                    return -1;
                const int row = m_drop.first + static_cast<int>((p.y - DropRowsTop()) / kDropRow);
                return row < static_cast<int>(m_drop.shown.size()) && row < m_drop.first + kDropRows ? row : -1;
            }

            void PaintDrop(ID2D1RenderTarget* rt, D2D1_SIZE_F size)
            {
                if (m_drop.id.empty())
                    return;
                const auto& t = Theme();
                const int rows = std::max(1, std::min(kDropRows, static_cast<int>(m_drop.shown.size())));
                const float h = (m_drop.search ? 36.f : 6.f) + rows * kDropRow + 6;
                const float w = std::max(m_drop.search ? 280.f : 120.f, m_drop.field.right - m_drop.field.left);
                const float x = std::max(8.f, std::min(m_drop.field.right - w, size.width - w - 8));
                float top = m_drop.field.bottom + 4;
                if (top + h > size.height - 8)
                    top = std::max(8.f, m_drop.field.top - 4 - h);
                m_drop.rect = D2D1::RectF(x, top, x + w, top + h);
                FillRound(D2D1::RectF(x - 1, top + 1, x + w + 1, top + h + 4), t.shadow, 8.f);
                FillRound(m_drop.rect, t.window, 8.f);
                StrokeRound(m_drop.rect, t.controlLine, 1.f, 8.f);
                if (m_drop.search)
                    PaintDropSearch(x, top, w);
                if (m_drop.shown.empty())
                {
                    DrawText(L"No match", TextFormat(12.f), D2D1::RectF(x + 14, DropRowsTop(), x + w - 14, DropRowsTop() + kDropRow), t.textDim);
                    return;
                }
                PaintDropRows(rt, x, w, top, h, rows);
            }

            void PaintDropSearch(float x, float top, float w)
            {
                const auto& t = Theme();
                const auto search = D2D1::RectF(x + 6, top + 6, x + w - 6, top + 32);
                FillRound(search, t.control, kControlRadius);
                DrawText(std::wstring(1, wchar_t(0xE721)), IconFormat(12.f), D2D1::RectF(search.left + 8, search.top, search.left + 26, search.bottom), t.textDim);
                const auto searchText = D2D1::RectF(search.left + 30, search.top, search.right - 8, search.bottom);
                if (m_drop.filter.empty())
                    DrawText(m_drop.fonts ? L"Type to find a font" : L"Type to search", TextFormat(12.f), searchText, t.textFaint);
                else
                    DrawText(m_drop.filter + ((GetTickCount64() / 530) % 2 == 0 ? L"|" : L""), TextFormat(kControlText), searchText, t.text);
            }

            void PaintDropRows(ID2D1RenderTarget* rt, float x, float w, float top, float h, int rows)
            {
                const auto& t = Theme();
                rt->PushAxisAlignedClip(D2D1::RectF(x, DropRowsTop(), x + w, top + h - 4), D2D1_ANTIALIAS_MODE_ALIASED);
                for (int i = 0; i < rows && m_drop.first + i < static_cast<int>(m_drop.shown.size()); ++i)
                {
                    const int row = m_drop.first + i;
                    const int option = m_drop.shown[row];
                    const auto r = D2D1::RectF(x + 6, DropRowsTop() + i * kDropRow, x + w - 12, DropRowsTop() + (i + 1) * kDropRow);
                    if (row == m_drop.hot)
                        FillRound(r, t.controlHover, kControlRadius);
                    if (option == m_drop.selected)
                        FillRound(D2D1::RectF(r.left, r.top + 7, r.left + 3, r.bottom - 7), t.accent, 1.5f);
                    const auto& name = m_drop.options[option];
                    DrawText(name, m_drop.fonts ? FontFormat(name) : TextFormat(kControlText), D2D1::RectF(r.left + 10, r.top, r.right - 6, r.bottom), t.text);
                }
                rt->PopAxisAlignedClip();
                const int n = static_cast<int>(m_drop.shown.size());
                if (n > kDropRows)
                {
                    // Where the rows shown are in the whole list.
                    const float track = rows * kDropRow;
                    const float thumb = std::max(20.f, track * kDropRows / n);
                    const float y = DropRowsTop() + (track - thumb) * m_drop.first / std::max(1, n - kDropRows);
                    FillRound(D2D1::RectF(x + w - 8, y, x + w - 5, y + thumb), t.textFaint, 1.5f);
                }
            }

            // Keys while a dropdown's list is open; true when used.
            bool DropKey(UINT vk)
            {
                if (m_drop.id.empty())
                    return false;
                switch (vk)
                {
                case VK_ESCAPE:
                case VK_TAB: CloseDrop(); return true;
                case VK_RETURN: PickDrop(m_drop.hot); return true;
                case VK_UP: --m_drop.hot; break;
                case VK_DOWN: ++m_drop.hot; break;
                case VK_PRIOR: m_drop.hot -= kDropRows - 1; break;
                case VK_NEXT: m_drop.hot += kDropRows - 1; break;
                case VK_HOME: m_drop.hot = 0; break;
                case VK_END: m_drop.hot = static_cast<int>(m_drop.shown.size()) - 1; break;
                case VK_BACK:
                    if (!m_drop.filter.empty())
                    {
                        m_drop.filter.pop_back();
                        FilterDrop();
                    }
                    break;
                default: return true;   // typed text arrives as WM_CHAR
                }
                ShowDropHot();
                Invalidate();
                return true;
            }

            // The control (and part) under a point in the panel.
            const Item* ItemAt(D2D1_POINT_2F p, int* part) const
            {
                const auto size = ClientDip();
                const auto panel = PanelRect(size);
                if (p.y < ContentTop() - 4 || p.y > panel.bottom || p.x < panel.left)
                    return nullptr;
                for (const auto& item : m_items)
                {
                    for (size_t i = 0; i < item.parts.size(); ++i)
                        if (In(item.parts[i], p) || (m_controls[item.control].kind == ControlKind::Slider && In(item.rect, p)))
                        {
                            if (part)
                                *part = static_cast<int>(i);
                            return &item;
                        }
                    const auto kind = m_controls[item.control].kind;
                    if (In(item.rect, p) && kind != ControlKind::Choice && kind != ControlKind::Swatches && kind != ControlKind::Icons &&
                        kind != ControlKind::Pictures)
                    {
                        if (part)
                            *part = -1;
                        return &item;
                    }
                }
                return nullptr;
            }

            double SliderValue(const Control& c, const Item& item, float x) const
            {
                const auto& track = item.parts[0];
                const double f = std::clamp((x - track.left) / std::max(1.f, track.right - track.left), 0.f, 1.f);
                const double step = std::pow(10.0, -c.decimals);
                return std::clamp(std::round((c.min + f * (c.max - c.min)) / step) * step, c.min, c.max);
            }

            void PanelDown(D2D1_POINT_2F p)
            {
                CommitEdit();
                int part = -1;
                const Item* item = ItemAt(p, &part);
                if (!item)
                    return;
                const Control c = m_controls[item->control];
                if (!c.enabled || c.id.empty())
                    return;
                ControlEvent e;
                e.id = c.id;
                switch (c.kind)
                {
                case ControlKind::Button:
                    Handle(m_tool->OnControl(e));
                    break;
                case ControlKind::Toggle:
                    e.on = !c.on;
                    Handle(m_tool->OnControl(e));
                    break;
                case ControlKind::Choice:
                    if (part >= 0 && part != c.selected)
                    {
                        e.index = part;
                        Handle(m_tool->OnControl(e));
                    }
                    break;
                case ControlKind::Number:
                    m_editId = c.id;
                    m_editText = Number(c.value, c.decimals);
                    SetTimer(Hwnd(), 1, 530, nullptr);   // caret blink
                    Invalidate();
                    break;
                case ControlKind::Slider:
                    m_sliderId = c.id;
                    SetCapture(Hwnd());
                    e.value = SliderValue(c, *item, p.x);
                    e.final = false;
                    Handle(m_tool->OnControl(e));
                    break;
                case ControlKind::Swatches:
                    if (part < 0)
                        break;
                    e.index = part;
                    if (c.custom && part + 1 == static_cast<int>(c.swatches.size()) && (part == c.selected || c.swatches[part].a == 0))
                    {
                        if (!PickColour(c.swatches[part], e.rgb))
                            break;
                    }
                    Handle(m_tool->OnControl(e));
                    break;
                case ControlKind::Dropdown:
                    if (m_drop.id == c.id)
                        CloseDrop();
                    else
                        OpenDrop(c, item->rect);
                    break;
                case ControlKind::Icons:
                    if (part < 0)
                        break;
                    e.index = part;
                    Handle(m_tool->OnControl(e));
                    break;
                case ControlKind::Pictures:
                    if (part < 0)
                        break;
                    e.index = part;
                    e.on = In(RemoveRect(item->parts[part]), p);
                    Handle(m_tool->OnControl(e));
                    break;
                default:
                    break;
                }
            }

            bool PickColour(const Color& current, uint32_t& rgb)
            {
                rgb = (uint32_t(BYTE(current.r * 255)) << 16) | (uint32_t(BYTE(current.g * 255)) << 8) | BYTE(current.b * 255);
                if (!sd::PickColour(Hwnd(), rgb))
                    return false;
                if (rgb == 0)
                    rgb = 0x010101;   // 0 means "no colour picked" to tools
                return true;
            }

            void CommitEdit()
            {
                if (m_editId.empty())
                    return;
                const std::string id = std::exchange(m_editId, {});
                KillTimer(Hwnd(), 1);
                wchar_t* end = nullptr;
                const double v = std::wcstod(m_editText.c_str(), &end);
                if (end != m_editText.c_str())
                {
                    ControlEvent e;
                    e.id = id;
                    e.value = v;
                    for (const auto& c : m_controls)
                        if (c.id == id)
                            e.value = std::clamp(v, c.min, c.max > c.min ? c.max : v);
                    Handle(m_tool->OnControl(e));
                }
                Invalidate();
            }

            bool EditKey(UINT vk)
            {
                if (m_editId.empty())
                    return false;
                if (vk == VK_RETURN || vk == VK_TAB)
                {
                    const std::string was = m_editId;
                    CommitEdit();
                    if (vk == VK_TAB)
                    {
                        // On to the next (or previous) number field.
                        std::vector<std::string> fields;
                        for (const auto& c : m_controls)
                            if (c.kind == ControlKind::Number && c.enabled)
                                fields.push_back(c.id);
                        const auto at = std::find(fields.begin(), fields.end(), was);
                        if (at != fields.end() && fields.size() > 1)
                        {
                            const size_t i = size_t(at - fields.begin());
                            const size_t next = Shift() ? (i + fields.size() - 1) % fields.size() : (i + 1) % fields.size();
                            for (const auto& c : m_controls)
                                if (c.id == fields[next])
                                {
                                    m_editId = c.id;
                                    m_editText = Number(c.value, c.decimals);
                                    SetTimer(Hwnd(), 1, 530, nullptr);
                                }
                        }
                    }
                    return true;
                }
                if (vk == VK_ESCAPE)
                {
                    m_editId.clear();
                    KillTimer(Hwnd(), 1);
                    Invalidate();
                    return true;
                }
                if (vk == VK_BACK)
                {
                    if (!m_editText.empty())
                        m_editText.pop_back();
                    Invalidate();
                    return true;
                }
                return true;   // everything else goes to WM_CHAR
            }

            // ---- Tool responses and undo --------------------------------------------------------------
            void Handle(const Response& r)
            {
                if (r.changed)
                    RecordUndo();
                if (r.redraw || r.panel || r.changed)
                    Invalidate();
            }

            void RecordUndo()
            {
                auto state = m_tool->State();
                if (!m_undo.empty() && m_undo[m_undoAt] == state)
                    return;
                if (!m_undo.empty())
                    m_undo.resize(m_undoAt + 1);
                m_undo.push_back(std::move(state));
                if (m_undo.size() > 200)
                    m_undo.erase(m_undo.begin());
                m_undoAt = m_undo.size() - 1;
                Invalidate();
            }

            void Undo()
            {
                if (m_dragging)
                    return;
                // Typing records a step a word at a time: what was typed since comes back first.
                if (!m_undo.empty())
                    RecordUndo();
                if (m_undoAt == 0)
                    return;
                m_tool->Restore(m_undo[--m_undoAt]);
                m_status.clear();
                Invalidate();
            }

            void Redo()
            {
                if (m_undoAt + 1 >= m_undo.size() || m_dragging)
                    return;
                m_tool->Restore(m_undo[++m_undoAt]);
                m_status.clear();
                Invalidate();
            }

            void Apply()
            {
                std::wstring why;
                if (!m_full || Busy())
                    return;
                if (!m_tool->CanApply(&why))
                {
                    SetStatus(why);
                    return;
                }
                auto apply = std::move(m_apply);
                auto closed = std::move(m_closed);
                auto params = m_tool->Params();
                Close();   // deletes this
                if (apply)
                    apply(std::move(params));
                if (closed)
                    closed();
            }

            void OnClosing() override
            {
                m_alive->store(false);
                m_stop.request_stop();
            }

            // ---- Input ------------------------------------------------------------------------------
            Pointer PointerAt(D2D1_POINT_2F p) const
            {
                Pointer ptr;
                ptr.at = m_view.ToImage(PointD{ p.x, p.y });
                ptr.tolerance = 7.0 / std::max(1e-6, m_view.Zoom());
                ptr.shift = Shift();
                ptr.ctrl = Ctrl();
                ptr.alt = Alt();
                return ptr;
            }

            bool OverCanvas(D2D1_POINT_2F p) const { return In(CanvasRect(ClientDip()), p); }

            bool OnMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result) override
            {
                switch (msg)
                {
                case kLoaded:
                    OnLoaded(std::unique_ptr<Loaded>(reinterpret_cast<Loaded*>(lParam)));
                    return true;
                case kLayerReady:
                {
                    std::unique_ptr<Layer> layer(reinterpret_cast<Layer*>(lParam));
                    if (layer->generation == m_generation)
                    {
                        const int i = layer->effect == Effect::Blur ? 0 : 1;
                        m_layers[i] = std::move(layer->pixels);
                        m_layerBitmaps[i].Reset();
                        Invalidate();
                    }
                    return true;
                }
                case kAsyncDone:
                {
                    std::unique_ptr<std::function<void()>> then(reinterpret_cast<std::function<void()>*>(lParam));
                    m_jobs = std::max(0, m_jobs - 1);
                    if (*then)
                        (*then)();
                    Invalidate();
                    return true;
                }
                case kPrepassProgress:
                    m_prepassProgress = static_cast<double>(wParam) / 1000.0;
                    Invalidate();
                    return true;
                case kPrepassDone:
                    if (wParam)
                        GrowToEditor();
                    else
                    {
                        // Nothing to review: apply straight away (the scrub still runs).
                        auto apply = std::move(m_apply);
                        auto closed = std::move(m_closed);
                        auto params = m_tool->Params();
                        Close();   // deletes this
                        if (apply)
                            apply(std::move(params));
                        if (closed)
                            closed();
                    }
                    return true;
                case WM_TIMER:
                    if (wParam == 1 || wParam == 2)
                        Invalidate();
                    return true;
                case kThumbReady:
                {
                    std::unique_ptr<Thumb> thumb(reinterpret_cast<Thumb*>(lParam));
                    auto& t = m_thumbs[thumb->path];
                    t.pixels = std::move(thumb->pixels);
                    t.bitmap.Reset();
                    Invalidate();
                    return true;
                }
                default:
                    break;
                }
                if (m_mode != Mode::Editor)
                {
                    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE)
                    {
                        Close();
                        return true;
                    }
                    return false;
                }

                switch (msg)
                {
                case WM_SETCURSOR:
                    if (LOWORD(lParam) == HTCLIENT)
                    {
                        POINT pt{};
                        GetCursorPos(&pt);
                        ScreenToClient(Hwnd(), &pt);
                        const auto p = D2D1::Point2F(pt.x / Scale(), pt.y / Scale());
                        LPCWSTR id = IDC_ARROW;
                        if (m_panning)
                            id = IDC_SIZEALL;
                        else if (OverCanvas(p) && m_full)
                            id = m_space ? IDC_HAND : CursorId(Busy() ? CursorShape::Busy : m_tool->CursorAt(PointerAt(p)));
                        else if (ItemAt(p, nullptr))
                            id = IDC_HAND;
                        SetCursor(LoadCursorW(nullptr, id));
                        result = TRUE;
                        return true;
                    }
                    return false;
                case WM_LBUTTONDOWN:
                case WM_LBUTTONDBLCLK:
                case WM_MBUTTONDOWN:
                {
                    SetFocus(Hwnd());
                    const auto p = DipFromLParam(lParam);
                    if (msg == WM_LBUTTONDBLCLK && ButtonAt(p) >= 0)
                        return false;
                    // A double click, and a third click soon after it on the same spot.
                    int clicks = 1;
                    if (msg == WM_LBUTTONDBLCLK)
                    {
                        clicks = 2;
                        m_doubleAt = GetMessageTime();
                        m_doublePoint = p;
                    }
                    else if (msg == WM_LBUTTONDOWN && m_doubleAt && static_cast<UINT>(GetMessageTime() - m_doubleAt) <= GetDoubleClickTime() &&
                             std::abs(p.x - m_doublePoint.x) <= 4 && std::abs(p.y - m_doublePoint.y) <= 4)
                        clicks = 3;
                    if (clicks != 2)
                        m_doubleAt = 0;
                    if (!m_drop.id.empty())
                    {
                        // A click picks from the open list, or closes it.
                        const bool inList = In(m_drop.rect, p), onField = In(m_drop.field, p);
                        if (inList)
                            PickDrop(DropRowAt(p));   // the search line keeps it open
                        else
                            CloseDrop();
                        if (inList || onField || msg != WM_LBUTTONDOWN || OverCanvas(p))
                            return true;
                    }
                    if (msg == WM_LBUTTONDOWN && In(m_zoomField, p))
                    {
                        CommitEdit();
                        if (m_full)
                            OpenZoomDrop();
                        return true;
                    }
                    if (!OverCanvas(p))
                    {
                        if (msg != WM_MBUTTONDOWN)
                            PanelDown(p);
                        return true;
                    }
                    CommitEdit();
                    if (!m_full)
                        return true;
                    if (msg == WM_MBUTTONDOWN || m_space)
                    {
                        m_panning = true;
                        m_panFrom = p;
                        SetCapture(Hwnd());
                        return true;
                    }
                    if (Busy())
                        return true;
                    m_dragging = true;
                    SetCapture(Hwnd());
                    Pointer pointer = PointerAt(p);
                    pointer.clicks = clicks;
                    Handle(m_tool->PointerDown(pointer));
                    return true;
                }
                case WM_MOUSEMOVE:
                {
                    const auto p = DipFromLParam(lParam);
                    if (In(m_zoomField, p) != In(m_zoomField, m_mouse))
                        Invalidate();   // the zoom field's hover
                    m_mouse = p;
                    if (!m_drop.id.empty())
                    {
                        const int row = DropRowAt(p);
                        if (row >= 0 && row != m_drop.hot)
                        {
                            m_drop.hot = row;
                            Invalidate();
                        }
                        return false;
                    }
                    if (m_panning)
                    {
                        m_view.PanBy(p.x - m_panFrom.x, p.y - m_panFrom.y);
                        m_panFrom = p;
                        Invalidate();
                        return true;
                    }
                    if (!m_sliderId.empty())
                    {
                        for (const auto& item : m_items)
                            if (m_controls[item.control].id == m_sliderId)
                            {
                                ControlEvent e;
                                e.id = m_sliderId;
                                e.value = SliderValue(m_controls[item.control], item, p.x);
                                e.final = false;
                                Handle(m_tool->OnControl(e));
                            }
                        return true;
                    }
                    if (m_dragging)
                    {
                        Handle(m_tool->PointerMove(PointerAt(p)));
                        return true;
                    }
                    // Hover: grips light up on the canvas, controls in the panel.
                    if (OverCanvas(p) && m_full)
                        Handle(m_tool->Hover(PointerAt(p)));
                    int part = -1;
                    const Item* item = OverCanvas(p) ? nullptr : ItemAt(p, &part);
                    const std::string hot = item ? m_controls[item->control].id : std::string();
                    if (hot != m_hotControl || part != m_hotPart)
                    {
                        m_hotControl = hot;
                        m_hotPart = part;
                        Invalidate();
                    }
                    TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, Hwnd(), 0 };
                    TrackMouseEvent(&track);
                    return false;   // the base still tracks its buttons
                }
                case WM_MOUSELEAVE:
                    if (!m_hotControl.empty())
                    {
                        m_hotControl.clear();
                        Invalidate();
                    }
                    return false;
                case WM_LBUTTONUP:
                case WM_MBUTTONUP:
                {
                    const auto p = DipFromLParam(lParam);
                    if (m_panning)
                    {
                        m_panning = false;
                        ReleaseCapture();
                        return true;
                    }
                    if (!m_sliderId.empty())
                    {
                        const std::string id = std::exchange(m_sliderId, {});
                        ReleaseCapture();
                        for (const auto& item : m_items)
                            if (m_controls[item.control].id == id)
                            {
                                ControlEvent e;
                                e.id = id;
                                e.value = SliderValue(m_controls[item.control], item, p.x);
                                e.final = true;
                                Handle(m_tool->OnControl(e));
                            }
                        return true;
                    }
                    if (m_dragging)
                    {
                        m_dragging = false;
                        ReleaseCapture();
                        Handle(m_tool->PointerUp(PointerAt(p)));
                        return true;
                    }
                    return false;
                }
                case WM_CAPTURECHANGED:
                    if (m_dragging && reinterpret_cast<HWND>(lParam) != Hwnd())
                    {
                        m_dragging = false;
                        Handle(m_tool->PointerCancel());
                    }
                    m_panning = false;
                    return false;
                case WM_MOUSEWHEEL:
                {
                    POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                    ScreenToClient(Hwnd(), &pt);
                    const auto p = D2D1::Point2F(pt.x / Scale(), pt.y / Scale());
                    const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                    if (!m_drop.id.empty())
                    {
                        const int n = static_cast<int>(m_drop.shown.size());
                        m_drop.first = std::clamp(m_drop.first - delta / WHEEL_DELTA * 3, 0, std::max(0, n - kDropRows));
                        Invalidate();
                        return true;
                    }
                    if (OverCanvas(p))
                    {
                        if (m_full)
                        {
                            m_wheel += delta;
                            const int notches = m_wheel / WHEEL_DELTA;
                            m_wheel -= notches * WHEEL_DELTA;
                            if (notches)
                                m_view.Step(notches, { p.x, p.y });
                            Invalidate();
                        }
                    }
                    else
                    {
                        m_scroll = std::max(0.f, m_scroll - delta / 120.f * 48.f);
                        Invalidate();
                    }
                    return true;
                }
                case WM_CHAR:
                    if (!m_drop.id.empty())
                    {
                        const wchar_t ch = static_cast<wchar_t>(wParam);
                        if (ch >= 0x20 && ch != 0x7F && m_drop.filter.size() < 40)
                        {
                            m_drop.filter.push_back(ch);
                            FilterDrop();
                            Invalidate();
                        }
                        return true;
                    }
                    if (!m_editId.empty())
                    {
                        const wchar_t ch = static_cast<wchar_t>(wParam);
                        if ((ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L',' || (ch == L'-' && m_editText.empty()))
                        {
                            if (m_editText.size() < 12)
                                m_editText.push_back(ch == L',' ? L'.' : ch);
                            Invalidate();
                        }
                        return true;
                    }
                    if (m_tool->TextInput())
                    {
                        // Typed text (Enter, Esc, Tab and Backspace arrive as keys instead).
                        const wchar_t ch = static_cast<wchar_t>(wParam);
                        // Ctrl with Alt is AltGr, which types characters on many keyboards.
                        if (ch >= 0x20 && ch != 0x7F && (!Ctrl() || Alt()))
                            Handle(m_tool->Char(ch));
                        return true;
                    }
                    return false;
                case WM_KEYDOWN:
                    OnKey(static_cast<UINT>(wParam));
                    return true;
                case WM_KEYUP:
                    if (wParam == VK_SPACE)
                    {
                        m_space = false;
                        return true;
                    }
                    return false;
                default:
                    return false;
                }
            }


            void OnKey(UINT vk)
            {
                if (DropKey(vk) || EditKey(vk))
                    return;
                const bool ctrl = Ctrl(), shift = Shift();
                if (ctrl && shift && vk == 'F')
                {
                    // The font list, as in word processors.
                    for (const auto& item : m_items)
                        if (m_controls[item.control].kind == ControlKind::Dropdown && m_controls[item.control].fonts && m_controls[item.control].enabled)
                        {
                            OpenDrop(m_controls[item.control], item.rect);
                            return;
                        }
                }
                const auto canvas = CanvasRect(ClientDip());
                const PointD centre{ (canvas.left + canvas.right) / 2, (canvas.top + canvas.bottom) / 2 };
                // A tool taking typed text gets the keys first (Enter ends the text, not the editor).
                if (m_tool->TextInput() && !(ctrl && (vk == 'Z' || vk == 'Y' || vk == '0' || vk == '1' || vk == VK_OEM_PLUS || vk == VK_OEM_MINUS)))
                {
                    Handle(m_tool->Key(vk, shift, ctrl));
                    return;
                }
                switch (vk)
                {
                case VK_RETURN:
                    Apply();
                    return;
                case VK_ESCAPE:
                    if (m_dragging)
                    {
                        m_dragging = false;
                        ReleaseCapture();
                        Handle(m_tool->PointerCancel());
                        return;
                    }
                    if (m_tool->Escape())
                    {
                        // The tool dropped a half-done step (a picture to place, a selection).
                        Invalidate();
                        return;
                    }
                    Close();
                    return;
                case VK_SPACE:
                    m_space = true;
                    return;
                case VK_NEXT:
                    ShowFile((m_index + 1) % m_files.size());
                    return;
                case VK_PRIOR:
                    ShowFile(m_index ? m_index - 1 : m_files.size() - 1);
                    return;
                default:
                    break;
                }
                if (ctrl)
                {
                    switch (vk)
                    {
                    case 'Z':
                        shift ? Redo() : Undo();
                        return;
                    case 'Y':
                        Redo();
                        return;
                    case '0':
                    case VK_NUMPAD0:
                        m_view.Fit(kFitMargin);
                        Invalidate();
                        return;
                    case '1':
                    case VK_NUMPAD1:
                        m_view.ZoomAt(1.0, centre);
                        Invalidate();
                        return;
                    case VK_OEM_PLUS:
                    case VK_ADD:
                        m_view.Step(1, centre);
                        Invalidate();
                        return;
                    case VK_OEM_MINUS:
                    case VK_SUBTRACT:
                        m_view.Step(-1, centre);
                        Invalidate();
                        return;
                    default:
                        break;
                    }
                }
                if (m_full && !Busy())
                    Handle(m_tool->Key(vk, shift, ctrl));
            }

            // ---- The painter tools draw with ----------------------------------------------------------
            class Painter final : public IPainter
            {
            public:
                Painter(CanvasWindow& w, ID2D1RenderTarget* rt, const D2D1_RECT_F& canvas) : m_w(w), m_rt(rt), m_canvas(canvas)
                {
                    const auto& t = w.Theme();
                    m_colors.dark = t.dark;
                    m_colors.accent = FromD2(t.accent);
                    m_colors.onAccent = FromD2(t.onAccent);
                    m_colors.text = FromD2(t.text);
                    m_colors.textDim = FromD2(t.textDim);
                    m_colors.shade = Color{ 0, 0, 0, t.dark ? 0.6f : 0.5f };
                    m_colors.line = Color{ 1, 1, 1, 0.95f };
                    m_colors.handle = Color{ 1, 1, 1, 1 };
                }
                const canvas::Colors& Theme() const override { return m_colors; }
                double Zoom() const override { return m_w.m_view.Zoom(); }
                void FillRect(const RectD& r, Color c) override
                {
                    if (auto* b = m_w.Brush(D2(c)))
                        m_rt->FillRectangle(Rect(m_w.m_view.ToView(r)), b);
                }
                void StrokeRect(const RectD& r, Color c, float widthDip, bool dashed) override
                {
                    auto* b = m_w.Brush(D2(c));
                    if (!b)
                        return;
                    const auto v = Rect(m_w.m_view.ToView(r));
                    if (dashed)
                    {
                        if (!m_dash)
                        {
                            ComPtr<ID2D1Factory> f;
                            m_rt->GetFactory(&f);
                            const float dashes[] = { 4.f, 3.f };
                            f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                                             D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_CUSTOM, 0.f),
                                                 dashes, 2, &m_dash);
                        }
                        m_rt->DrawRectangle(v, b, widthDip, m_dash.Get());
                    }
                    else
                        m_rt->DrawRectangle(v, b, widthDip);
                }
                void Line(PointD a, PointD b, Color c, float widthDip) override
                {
                    const auto va = m_w.m_view.ToView(a), vb = m_w.m_view.ToView(b);
                    if (auto* brush = m_w.Brush(D2(c)))
                        m_rt->DrawLine(D2D1::Point2F(float(va.x), float(va.y)), D2D1::Point2F(float(vb.x), float(vb.y)), brush, widthDip);
                }
                void ShadeOutside(const RectD& keep, Color c) override
                {
                    auto* b = m_w.Brush(D2(c));
                    if (!b)
                        return;
                    const auto k = Rect(m_w.m_view.ToView(keep));
                    const auto& v = m_canvas;
                    m_rt->FillRectangle(D2D1::RectF(v.left, v.top, v.right, k.top), b);
                    m_rt->FillRectangle(D2D1::RectF(v.left, k.bottom, v.right, v.bottom), b);
                    m_rt->FillRectangle(D2D1::RectF(v.left, k.top, k.left, k.bottom), b);
                    m_rt->FillRectangle(D2D1::RectF(k.right, k.top, v.right, k.bottom), b);
                }
                void DrawEffect(const RectD& r, Effect effect) override
                {
                    auto* layer = m_w.EffectBitmap(m_rt, effect);
                    const auto v = Rect(m_w.m_view.ToView(r));
                    if (!layer)
                    {
                        // Still being made: a veil, so the cover reads as a cover meanwhile.
                        if (auto* b = m_w.Brush(D2D1::ColorF(0.5f, 0.5f, 0.5f, 0.85f)))
                            m_rt->FillRectangle(v, b);
                        return;
                    }
                    const auto& pixels = m_w.m_layers[effect == Effect::Blur ? 0 : 1];
                    m_rt->PushAxisAlignedClip(v, D2D1_ANTIALIAS_MODE_ALIASED);
                    const double per = double(m_w.m_full->width) / pixels.width;
                    m_w.DrawPictureBitmap(m_rt, layer, m_w.PictureTransform(per), pixels, per);
                    m_rt->PopAxisAlignedClip();
                }
                void Grip(PointD at, bool hot) override
                {
                    const auto p = m_w.m_view.ToView(at);
                    const float s = hot ? 6.f : 4.5f;
                    const auto r = D2D1::RectF(float(p.x) - s, float(p.y) - s, float(p.x) + s, float(p.y) + s);
                    if (auto* b = m_w.Brush(hot ? m_w.Theme().accent : D2D1::ColorF(1, 1, 1, 1)))
                        m_rt->FillRectangle(r, b);
                    if (auto* b = m_w.Brush(hot ? m_w.Theme().onAccent : m_w.Theme().accent))
                        m_rt->DrawRectangle(r, b, 1.5f);
                }
                void Tag(PointD at, const std::wstring& text, Color back, Color fore) override
                {
                    const auto p = m_w.m_view.ToView(at);
                    const float w = m_w.TextWidth(text, 12.f) + 14.f, h = 22.f;
                    float x = std::clamp(float(p.x), m_canvas.left + 4, std::max(m_canvas.left + 4, m_canvas.right - w - 4));
                    float y = std::clamp(float(p.y), m_canvas.top + 4, std::max(m_canvas.top + 4, m_canvas.bottom - h - 4));
                    const auto r = D2D1::RectF(x, y, x + w, y + h);
                    if (auto* b = m_w.Brush(D2(back)))
                        m_rt->FillRoundedRectangle(D2D1::RoundedRect(r, 4, 4), b);
                    m_w.DrawText(text, m_w.TextFormat(12.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), r, D2(fore));
                }

            private:
                CanvasWindow& m_w;
                ID2D1RenderTarget* m_rt;
                D2D1_RECT_F m_canvas;
                canvas::Colors m_colors;
                ComPtr<ID2D1StrokeStyle> m_dash;
            };

            // ---- State ------------------------------------------------------------------------------
            std::vector<std::wstring> m_files;
            std::shared_ptr<ICanvasTool> m_tool;
            std::function<void(nlohmann::json)> m_apply;
            std::function<void()> m_closed;   // still set when the window closes without applying
            std::shared_ptr<std::atomic<bool>> m_alive;
            std::stop_source m_stop;
            ComPtr<IDWriteTextFormat> m_wrapped;
            float m_wrappedSize = 0;

            Mode m_mode = Mode::Editor;
            std::wstring m_prepassMessage;
            double m_prepassProgress = 0;

            size_t m_index = 0;
            uint64_t m_generation = 0;
            std::shared_ptr<const Raster> m_full;
            Pixels m_preview;
            bool m_alpha = false;
            std::wstring m_loadError;
            ComPtr<ID2D1Bitmap> m_picture;
            unsigned m_pictureGeneration = 0;
            ComPtr<ID2D1DeviceContext> m_previewDc;   // IPreview rendering
            ComPtr<ID2D1Bitmap1> m_previewTarget;
            unsigned m_previewGeneration = 0;
            uint64_t m_previewRevision = 0;
            double m_previewScale = 0;
            bool m_previewFailed = false;
            ComPtr<ID2D1BitmapBrush> m_checker;
            unsigned m_checkerGeneration = 0;
            Pixels m_layers[2];
            ComPtr<ID2D1Bitmap> m_layerBitmaps[2];
            bool m_layerPending[2]{};

            ViewTransform m_view;
            SizeD m_viewImage;
            D2D1_RECT_F m_tab{}, m_zoomField{}, m_barDivider{}, m_barText{};   // on the tool bar
            uint64_t m_fileBytes = 0;   // the picture's file, for the footer
            bool m_dragging = false, m_panning = false, m_space = false;
            D2D1_POINT_2F m_panFrom{};
            int m_wheel = 0;

            std::vector<Control> m_controls;
            std::vector<Item> m_items;
            float m_scroll = 0, m_contentHeight = 0;
            std::string m_hotControl;
            int m_hotPart = -1;
            std::string m_editId, m_sliderId;
            std::wstring m_editText;
            std::wstring m_status;
            int m_jobs = 0;

            std::vector<nlohmann::json> m_undo;
            size_t m_undoAt = 0;

            D2D1_POINT_2F m_mouse{};
            LONG m_doubleAt = 0;   // the message time of the last double click
            D2D1_POINT_2F m_doublePoint{};
            DropList m_drop;
            std::map<std::wstring, ComPtr<IDWriteTextFormat>> m_fontFormats;
            struct CachedThumb
            {
                Pixels pixels;
                ComPtr<ID2D1Bitmap> bitmap;
                unsigned generation = 0;
            };
            std::map<std::wstring, CachedThumb> m_thumbs;
        };

        bool OpenWindow(EditorRequest request)
        {
            if (!request.tool || request.files.empty())
                return false;
            auto* window = new CanvasWindow(std::move(request));
            if (!window->Open())
            {
                log::Error(L"editor: the window could not be created");
                delete window;
                return false;
            }
            return true;   // the window deletes itself when it closes
        }

        // In the app only: engines open the editor through canvas::OpenEditorWindow (Editor.h).
        [[maybe_unused]] const bool g_registered = (RegisterEditorWindow(&OpenWindow), true);
    }
}
