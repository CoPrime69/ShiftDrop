// The signature pad: draw a signature, or take it from a picture of one. On the editors' frame
// (EditorFrame.h), so it looks like the picture editor it opens over, light or dark, at every DPI.
// The writing area is white whatever the theme: it stands for the paper.
#include "canvas/SignaturePad.h"
#include "EditorFrame.h"
#include "Log.h"
#include "Utf.h"
#include "photo/Annotations.h"
#include "photo/PhotoEngine.h"
#include "photo/Signatures.h"

#include <commdlg.h>
#include <d2d1_1.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

namespace sd::canvas
{
    bool PickPictureFile(HWND owner, std::wstring& path)
    {
        wchar_t name[MAX_PATH * 4] = L"";
        OPENFILENAMEW ofn{ sizeof(ofn) };
        ofn.hwndOwner = owner;
        ofn.lpstrFilter = L"Pictures\0*.png;*.jpg;*.jpeg;*.jpe;*.jfif;*.webp;*.bmp;*.gif;*.tif;*.tiff;*.heic;*.heif;*.avif;*.ico\0All files\0*.*\0";
        ofn.lpstrFile = name;
        ofn.nMaxFile = static_cast<DWORD>(std::size(name));
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
        if (!GetOpenFileNameW(&ofn))
            return false;
        path = name;
        return true;
    }

    namespace
    {
        using Microsoft::WRL::ComPtr;
        using Done = std::function<void(std::shared_ptr<const Raster>, bool)>;

        constexpr UINT kPictureReady = WM_APP + 60;   // lParam: Opened*, ours to delete
        constexpr UINT kInkReady = WM_APP + 61;       // lParam: Inked*, ours to delete

        constexpr float kPad = 16.f;
        constexpr float kRow = 32.f;
        constexpr float kInkWidth = 2.6f;   // DIPs
        constexpr float kRenderScale = 4.f; // pixels per DIP in the saved signature
        const uint32_t kInks[] = { 0x111111, 0x1F3A93 };

        struct Opened
        {
            uint64_t generation = 0;
            std::shared_ptr<Raster> original, clear;
            std::wstring error;
        };

        struct Inked
        {
            std::shared_ptr<Raster> ink;
            std::wstring error;
        };

        // A straight-alpha raster as a premultiplied D2D bitmap.
        ComPtr<ID2D1Bitmap> MakeBitmap(ID2D1RenderTarget* rt, const Raster& r)
        {
            ComPtr<ID2D1Bitmap> bmp;
            if (r.Empty())
                return bmp;
            std::vector<uint8_t> px(r.px.size());
            for (size_t i = 0; i < px.size(); i += 4)
            {
                const unsigned a = r.px[i + 3];
                for (int c = 0; c < 3; ++c)
                    px[i + c] = static_cast<uint8_t>((r.px[i + c] * a + 127) / 255);
                px[i + 3] = static_cast<uint8_t>(a);
            }
            const auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
            rt->CreateBitmap(D2D1::SizeU(r.width, r.height), px.data(), r.width * 4, props, &bmp);
            return bmp;
        }

        class SignaturePad final : public EditorFrame
        {
        public:
            SignaturePad(HWND owner, bool fromPicture, Done done)
                : m_owner(owner), m_picture(fromPicture), m_done(std::move(done)), m_alive(std::make_shared<std::atomic<bool>>(true))
            {
            }

            bool Open()
            {
                if (!Create(L"Add a signature", 600, 420, 460, 340, false, m_owner))
                    return false;
                m_buttons.push_back(Button{ L"Cancel", 0, false, true, {}, [this] { Close(); } });
                m_buttons.push_back(Button{ L"Add", 0, true, false, {}, [this] { Add(); } });
                Layout(ClientDip());
                EnableWindow(m_owner, FALSE);
                Present();
                if (m_picture)
                    PostMessageW(Hwnd(), WM_COMMAND, 1, 0);   // straight to the Open dialog, once the pad shows
                return true;
            }

        private:
            // ---- Layout -----------------------------------------------------------------------------
            D2D1_RECT_F Card(D2D1_SIZE_F size) const { return D2D1::RectF(kPad, 96, size.width - kPad, size.height - 112); }
            D2D1_RECT_F Tab(int i) const { return D2D1::RectF(kPad + i * 124.f, 52, kPad + i * 124.f + 118, 80); }
            D2D1_RECT_F ToolRow(D2D1_SIZE_F size) const { return D2D1::RectF(kPad, size.height - 100, size.width - kPad, size.height - 100 + kRow); }
            D2D1_RECT_F ClearButton(D2D1_SIZE_F size) const
            {
                const auto r = ToolRow(size);
                return D2D1::RectF(r.left, r.top, r.left + 96, r.bottom);
            }
            D2D1_RECT_F Swatch(D2D1_SIZE_F size, int i) const
            {
                const auto r = ToolRow(size);
                const float x = r.right - 2 * 36.f + i * 36.f;
                return D2D1::RectF(x, r.top + 2, x + 28, r.top + 30);
            }
            D2D1_RECT_F ClearToggle(D2D1_SIZE_F size) const
            {
                const auto r = ToolRow(size);
                return D2D1::RectF(r.left, r.top, r.left + 250, r.bottom);
            }
            D2D1_RECT_F AnotherButton(D2D1_SIZE_F size) const
            {
                const auto r = ToolRow(size);
                return D2D1::RectF(r.right - 150, r.top, r.right, r.bottom);
            }
            D2D1_RECT_F RememberToggle(D2D1_SIZE_F size) const
            {
                return D2D1::RectF(kPad, size.height - kPad - kRow, std::min(size.width - kPad - 220, kPad + 230), size.height - kPad);
            }

            void Layout(D2D1_SIZE_F size) override
            {
                if (m_buttons.size() < 2)
                    return;
                const float y = size.height - kPad - kRow;
                m_buttons[0].rect = D2D1::RectF(size.width - kPad - 208, y, size.width - kPad - 108, y + kRow);
                m_buttons[1].rect = D2D1::RectF(size.width - kPad - 100, y, size.width - kPad, y + kRow);
            }

            static bool In(const D2D1_RECT_F& r, D2D1_POINT_2F p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }

            bool HasInk() const { return m_picture ? (m_seeThrough ? m_clear : m_original) != nullptr : !m_strokes.empty(); }

            // ---- Painting ---------------------------------------------------------------------------
            void Paint(ID2D1RenderTarget* rt, D2D1_SIZE_F size) override
            {
                const auto& c = Theme();
                if (m_buttons.size() >= 2)
                    m_buttons[1].enabled = HasInk() && !m_saving;
                DrawText(L"Add a signature", TextFormat(20.f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(kPad, 12, size.width - kPad, 44), c.text);
                const wchar_t* tabs[] = { L"Draw", L"From a photo" };
                for (int i = 0; i < 2; ++i)
                    DrawChoice(Tab(i), tabs[i], m_picture == (i == 1), { true, m_hot == 10 + i, false, false });
                DrawText(L"Stays on this PC", TextFormat(12.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING),
                         D2D1::RectF(size.width - 220, 52, size.width - kPad, 80), c.textDim);

                const auto card = Card(size);
                FillRound(card, m_picture ? D2D1::ColorF(0xEDEDED) : D2D1::ColorF(0xFFFFFF), 8.f);
                StrokeRound(card, c.controlLine, 1.f, 8.f);
                if (m_picture)
                    PaintPicture(rt, card);
                else
                    PaintDrawing(rt, card);

                if (m_picture)
                {
                    if (m_original)
                    {
                        const auto t = ClearToggle(size);
                        DrawText(L"Make the white see-through", TextFormat(kControlText), D2D1::RectF(t.left, t.top, t.right - 52, t.bottom), c.text);
                        const float cy = (t.top + t.bottom) / 2;
                        DrawSwitch(D2D1::RectF(t.right - 44, cy - 10, t.right - 4, cy + 10), m_seeThrough, { true, m_hot == 3, false, false });
                        DrawButton(AnotherButton(size), L"Another picture…", 0, false, { true, m_hot == 4, false, false });
                    }
                }
                else
                {
                    DrawButton(ClearButton(size), L"Clear", 0xE894, false, { !m_strokes.empty(), m_hot == 1, false, false });   // Clear
                    for (int i = 0; i < 2; ++i)
                    {
                        const auto r = Swatch(size, i);
                        const auto centre = D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2);
                        if (static_cast<int>(m_ink) == i)
                            if (auto* b = Brush(c.accent))
                                rt->DrawEllipse(D2D1::Ellipse(centre, 13.f, 13.f), b, 2.f);
                        if (auto* b = Brush(D2D1::ColorF(kInks[i])))
                            rt->FillEllipse(D2D1::Ellipse(centre, 10.f, 10.f), b);
                    }
                }
                const auto remember = RememberToggle(size);
                DrawText(L"Remember for next time", TextFormat(kControlText), D2D1::RectF(remember.left, remember.top, remember.right - 52, remember.bottom),
                         c.text);
                const float cy = (remember.top + remember.bottom) / 2;
                DrawSwitch(D2D1::RectF(remember.right - 44, cy - 10, remember.right - 4, cy + 10), m_remember, { true, m_hot == 2, false, false });
                if (!m_status.empty())
                    DrawText(m_status, TextFormat(12.f), D2D1::RectF(kPad, size.height - 64, size.width - kPad, size.height - 50), c.textDim);
                DrawButtons();
            }

            void PaintDrawing(ID2D1RenderTarget* rt, const D2D1_RECT_F& card)
            {
                // A line to sign on, as on a form.
                const float base = card.top + (card.bottom - card.top) * 0.7f;
                if (auto* b = Brush(D2D1::ColorF(0xC8C8C8)))
                    rt->DrawLine(D2D1::Point2F(card.left + 24, base), D2D1::Point2F(card.right - 24, base), b, 1.f);
                if (m_strokes.empty())
                    DrawText(L"Sign here with the mouse or a pen", TextFormat(13.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER),
                             D2D1::RectF(card.left, base + 8, card.right, base + 30), D2D1::ColorF(0x8A8A8A));
                auto* ink = Brush(D2D1::ColorF(kInks[m_ink]));
                if (!ink)
                    return;
                ComPtr<ID2D1StrokeStyle> round;
                Factory()->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                                             nullptr, 0, &round);
                rt->PushAxisAlignedClip(card, D2D1_ANTIALIAS_MODE_ALIASED);
                for (const auto& stroke : m_strokes)
                {
                    // Smoothed as the saved signature is (Annotations' pen strokes).
                    ComPtr<ID2D1PathGeometry> path;
                    ComPtr<ID2D1GeometrySink> sink;
                    if (stroke.empty() || FAILED(Factory()->CreatePathGeometry(&path)) || FAILED(path->Open(&sink)))
                        continue;
                    const auto at = [&](PointD p) { return D2D1::Point2F(card.left + float(p.x), card.top + float(p.y)); };
                    sink->BeginFigure(at(stroke[0]), D2D1_FIGURE_BEGIN_HOLLOW);
                    for (const auto& q : photo::SmoothStroke(stroke))
                        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(at(q.control), at(q.end)));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    sink->Close();
                    if (stroke.size() == 1)
                        rt->FillEllipse(D2D1::Ellipse(at(stroke[0]), kInkWidth / 2, kInkWidth / 2), ink);
                    else
                        rt->DrawGeometry(path.Get(), ink, kInkWidth, round.Get());
                }
                rt->PopAxisAlignedClip();
            }

            void PaintPicture(ID2D1RenderTarget* rt, const D2D1_RECT_F& card)
            {
                const auto& c = Theme();
                const auto& shown = m_seeThrough ? m_clear : m_original;
                if (!shown)
                {
                    const std::wstring text = m_loading ? std::wstring(L"Opening…") : m_error.empty() ? std::wstring(L"Choose a photo or a scan of your signature on white paper.")
                                                                                                      : L"Can't use that picture: " + m_error;
                    DrawText(text, TextFormat(13.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER),
                             D2D1::RectF(card.left + 16, card.top + 16, card.right - 16, (card.top + card.bottom) / 2 - 4), D2D1::ColorF(0x444444));
                    if (!m_loading)
                        DrawButton(ChooseButton(card), L"Choose a picture…", 0xEB9F, true, { true, m_hot == 5, false, false });   // Photo
                    return;
                }
                if (!m_bitmap || m_bitmapGeneration != DeviceGeneration() || m_bitmapOf != shown.get())
                {
                    m_bitmap = MakeBitmap(rt, *shown);
                    m_bitmapGeneration = DeviceGeneration();
                    m_bitmapOf = shown.get();
                }
                if (!m_bitmap)
                    return;
                const float room = 16;
                const float s = std::min({ (card.right - card.left - 2 * room) / shown->width, (card.bottom - card.top - 2 * room) / shown->height, 2.f });
                const float w = shown->width * s, h = shown->height * s;
                const float x = (card.left + card.right - w) / 2, y = (card.top + card.bottom - h) / 2;
                rt->DrawBitmap(m_bitmap.Get(), D2D1::RectF(x, y, x + w, y + h), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                (void)c;
            }

            D2D1_RECT_F ChooseButton(const D2D1_RECT_F& card) const
            {
                const float cx = (card.left + card.right) / 2, cy = (card.top + card.bottom) / 2;
                return D2D1::RectF(cx - 90, cy + 4, cx + 90, cy + 4 + kRow);
            }

            // ---- Actions ----------------------------------------------------------------------------
            void ChoosePicture()
            {
                std::wstring path;
                if (!PickPictureFile(Hwnd(), path))
                    return;
                m_loading = true;
                m_error.clear();
                ++m_generation;
                Invalidate();
                std::thread([hwnd = Hwnd(), path, generation = m_generation, alive = m_alive] {
                    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    auto out = std::make_unique<Opened>();
                    out->generation = generation;
                    auto original = std::make_shared<Raster>();
                    if (SUCCEEDED(LoadRaster(path, *original, out->error, 2400)))
                    {
                        auto clear = std::make_shared<Raster>(*original);
                        photo::PaperToTransparent(*clear);
                        if (photo::TrimToInk(*clear, 12))
                            out->clear = std::move(clear);
                        out->original = std::move(original);
                        if (!out->clear)
                            out->error = L"no ink was found on it";
                    }
                    else if (out->error.empty())
                        out->error = L"it could not be opened";
                    if (SUCCEEDED(co))
                        CoUninitialize();
                    if (alive->load() && PostMessageW(hwnd, kPictureReady, 0, reinterpret_cast<LPARAM>(out.get())))
                        out.release();
                }).detach();
            }

            void Add()
            {
                if (!HasInk() || m_saving)
                    return;
                if (m_picture)
                {
                    Finish(m_seeThrough ? m_clear : m_original);
                    return;
                }
                // The strokes drawn again, larger, on a transparent picture of the writing area.
                const auto card = Card(ClientDip());
                photo::Annotations marks;
                for (const auto& stroke : m_strokes)
                {
                    photo::Annotation a;
                    a.kind = photo::Mark::Pen;
                    a.color = kInks[m_ink];
                    a.width = kInkWidth * kRenderScale;
                    for (const auto& p : stroke)
                        a.points.push_back({ p.x * kRenderScale, p.y * kRenderScale });
                    marks.Add(std::move(a));
                }
                const UINT w = static_cast<UINT>(std::ceil((card.right - card.left) * kRenderScale));
                const UINT h = static_cast<UINT>(std::ceil((card.bottom - card.top) * kRenderScale));
                m_saving = true;
                m_status = L"Saving the signature…";
                Invalidate();
                std::thread([hwnd = Hwnd(), marks = std::move(marks), w, h, alive = m_alive] {
                    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    auto out = std::make_unique<Inked>();
                    Raster paper;
                    auto ink = std::make_shared<Raster>();
                    if (!paper.Allocate(w, h))
                        out->error = L"there isn't enough memory";
                    else if (SUCCEEDED(photo::RenderAnnotated(paper, marks, {}, *ink, out->error)) && photo::TrimToInk(*ink, 8))
                        out->ink = std::move(ink);
                    else if (out->error.empty())
                        out->error = L"it could not be drawn";
                    if (SUCCEEDED(co))
                        CoUninitialize();
                    if (alive->load() && PostMessageW(hwnd, kInkReady, 0, reinterpret_cast<LPARAM>(out.get())))
                        out.release();
                }).detach();
            }

            void Finish(std::shared_ptr<const Raster> ink)
            {
                auto done = std::move(m_done);
                const bool remember = m_remember;
                Close();   // deletes this; the owner is enabled again first
                if (done && ink)
                    done(std::move(ink), remember);
            }

            void OnClosing() override
            {
                m_alive->store(false);
                if (m_owner)
                {
                    // Enabled before this window goes, so Windows gives the focus back to it.
                    EnableWindow(m_owner, TRUE);
                    SetForegroundWindow(m_owner);
                }
            }

            int HotAt(D2D1_POINT_2F p) const
            {
                const auto size = ClientDip();
                for (int i = 0; i < 2; ++i)
                    if (In(Tab(i), p))
                        return 10 + i;
                if (In(RememberToggle(size), p))
                    return 2;
                if (m_picture)
                {
                    if (m_original && In(ClearToggle(size), p))
                        return 3;
                    if (m_original && In(AnotherButton(size), p))
                        return 4;
                    if (!m_original && !m_loading && In(ChooseButton(Card(size)), p))
                        return 5;
                    return -1;
                }
                if (In(ClearButton(size), p))
                    return 1;
                for (int i = 0; i < 2; ++i)
                    if (In(Swatch(size, i), p))
                        return 20 + i;
                return -1;
            }

            void Click(int hot)
            {
                switch (hot)
                {
                case 1: m_strokes.clear(); break;
                case 2: m_remember = !m_remember; break;
                case 3: m_seeThrough = !m_seeThrough; break;
                case 4:
                case 5: ChoosePicture(); break;
                case 10:
                case 11: m_picture = hot == 11; m_status.clear(); break;
                case 20:
                case 21: m_ink = static_cast<size_t>(hot - 20); break;
                default: return;
                }
                Invalidate();
            }

            bool OnMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result) override
            {
                (void)result;
                switch (msg)
                {
                case WM_COMMAND:
                    if (wParam == 1)
                        ChoosePicture();
                    return true;
                case kPictureReady:
                {
                    std::unique_ptr<Opened> opened(reinterpret_cast<Opened*>(lParam));
                    if (opened->generation != m_generation)
                        return true;
                    m_loading = false;
                    m_error = opened->error;
                    if (opened->original)
                    {
                        m_original = std::move(opened->original);
                        m_clear = std::move(opened->clear);
                        m_seeThrough = m_clear != nullptr;
                    }
                    Invalidate();
                    return true;
                }
                case kInkReady:
                {
                    std::unique_ptr<Inked> inked(reinterpret_cast<Inked*>(lParam));
                    m_saving = false;
                    if (!inked->ink)
                    {
                        m_status = L"The signature could not be made: " + inked->error;
                        log::Warn(L"signature: {}", m_status);
                        Invalidate();
                        return true;
                    }
                    Finish(std::move(inked->ink));
                    return true;
                }
                case WM_SETCURSOR:
                    if (LOWORD(lParam) == HTCLIENT)
                    {
                        POINT pt{};
                        GetCursorPos(&pt);
                        ScreenToClient(Hwnd(), &pt);
                        const auto p = D2D1::Point2F(pt.x / Scale(), pt.y / Scale());
                        const bool drawing = !m_picture && In(Card(ClientDip()), p);
                        SetCursor(LoadCursorW(nullptr, drawing ? IDC_CROSS : HotAt(p) >= 0 || ButtonAt(p) >= 0 ? IDC_HAND : IDC_ARROW));
                        result = TRUE;
                        return true;
                    }
                    return false;
                case WM_LBUTTONDOWN:
                case WM_LBUTTONDBLCLK:
                {
                    const auto p = DipFromLParam(lParam);
                    if (ButtonAt(p) >= 0)
                        return false;
                    const auto card = Card(ClientDip());
                    if (!m_picture && In(card, p) && !m_saving)
                    {
                        m_strokes.push_back({ PointD{ p.x - card.left, p.y - card.top } });
                        m_writing = true;
                        SetCapture(Hwnd());
                        Invalidate();
                        return true;
                    }
                    Click(HotAt(p));
                    return true;
                }
                case WM_MOUSEMOVE:
                {
                    const auto p = DipFromLParam(lParam);
                    if (m_writing && !m_strokes.empty())
                    {
                        const auto card = Card(ClientDip());
                        const PointD at{ p.x - card.left, p.y - card.top };
                        auto& stroke = m_strokes.back();
                        if (std::hypot(at.x - stroke.back().x, at.y - stroke.back().y) >= 0.75)
                        {
                            stroke.push_back(at);
                            Invalidate();
                        }
                        return true;
                    }
                    const int hot = HotAt(p);
                    if (hot != m_hot)
                    {
                        m_hot = hot;
                        Invalidate();
                    }
                    return false;   // the frame tracks its buttons
                }
                case WM_LBUTTONUP:
                    if (m_writing)
                    {
                        m_writing = false;
                        ReleaseCapture();
                        Invalidate();
                        return true;
                    }
                    return false;
                case WM_CAPTURECHANGED:
                    m_writing = false;
                    return false;
                case WM_KEYDOWN:
                    switch (wParam)
                    {
                    case VK_ESCAPE: Close(); return true;
                    case VK_RETURN: Add(); return true;
                    case VK_DELETE: m_strokes.clear(); Invalidate(); return true;
                    case 'Z':
                        if (Ctrl() && !m_strokes.empty() && !m_writing)
                        {
                            m_strokes.pop_back();   // the last stroke
                            Invalidate();
                        }
                        return true;
                    case 'O':
                        if (Ctrl())
                        {
                            m_picture = true;
                            ChoosePicture();
                            Invalidate();
                        }
                        return true;
                    default: return false;
                    }
                default: return false;
                }
            }

            HWND m_owner = nullptr;
            bool m_picture = false;
            Done m_done;
            std::shared_ptr<std::atomic<bool>> m_alive;
            std::vector<std::vector<PointD>> m_strokes;   // DIPs in the writing area
            size_t m_ink = 0;
            bool m_writing = false, m_remember = true, m_seeThrough = true, m_loading = false, m_saving = false;
            std::shared_ptr<Raster> m_original, m_clear;
            std::wstring m_error, m_status;
            uint64_t m_generation = 0;
            ComPtr<ID2D1Bitmap> m_bitmap;
            unsigned m_bitmapGeneration = 0;
            const Raster* m_bitmapOf = nullptr;
            int m_hot = -1;
        };
    }

    bool OpenSignaturePad(HWND owner, bool fromPicture, std::function<void(std::shared_ptr<const Raster> ink, bool remember)> done)
    {
        auto* pad = new SignaturePad(owner, fromPicture, std::move(done));
        if (!pad->Open())
        {
            log::Error(L"signature: the pad could not be opened");
            delete pad;
            return false;
        }
        return true;   // the window deletes itself when it closes
    }
}
