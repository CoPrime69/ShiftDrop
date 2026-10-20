#include "Brand.h"
#include "Overlay.h"
#include "Log.h"

#include <algorithm>
#include <cmath>
#include <numbers>

using Microsoft::WRL::ComPtr;
using namespace sd::theme;

namespace sd
{
    namespace
    {
        constexpr wchar_t kClassName[] = L"" SD_NAME L".Overlay";

        // Sizes in DIPs; Layout() turns them into pixels for the monitor and the Size setting.
        constexpr float kTileOutDip = 128.f;   // outer edge of the tiles
        constexpr float kTileInDip = 58.f;     // inner edge of the tiles
        constexpr float kCentreDip = 52.f;     // centre disc = dead zone: dropping here cancels
        constexpr float kGapDip = 4.f;         // straight gap between tiles
        constexpr float kCornerDip = 6.f;      // tile corner rounding
        constexpr float kLiftDip = 7.f;        // a hovered tile grows outward by this much...
        constexpr float kPushDip = 3.f;        // ...and moves out along its centre line by this much
        constexpr float kMarginDip = 34.f;     // room around the tiles for the lift and the shadow

        // Animation lengths, ms.
        constexpr float kOpenMs = 420.f;       // spring; it looks settled after about half of this
        constexpr float kFadeInMs = 120.f;
        constexpr float kSwitchMs = 260.f;
        constexpr float kCloseMs = 200.f;
        constexpr float kAwayTauMs = 50.f;     // approach for the fade as the cursor heads out of the wheel
        constexpr float kAwayFade = 0.45f;     // opacity lost at the window edge (never 0: drops still land)
        constexpr float kHotTauMs = 42.f;      // exponential approach for hover highlights
        constexpr UINT_PTR kTimerFrame = 1;

        // Private messages to the overlay window.
        constexpr UINT kMsgHideCursor = WM_APP + 1;   // after DragEnter has returned to the drag source
    }

    Overlay::Overlay(HINSTANCE instance, std::vector<WheelRing> rings, ContentProvider provider, DropHandler onDrop, Events events)
        : m_instance(instance), m_rings(std::move(rings)), m_provider(std::move(provider)), m_onDrop(std::move(onDrop)),
          m_events(std::move(events))
    {
    }

    Overlay::~Overlay() { Destroy(); }

    bool Overlay::Create()
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = m_instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);

        // WS_EX_NOACTIVATE + WM_MOUSEACTIVATE=MA_NOACTIVATE: showing or touching the wheel never takes
        // focus from the app you are dragging out of. WS_EX_TOOLWINDOW keeps it off the taskbar/Alt+Tab.
        m_hwnd.reset(CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                     kClassName, SD_NAME, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, m_instance, this));
        if (!m_hwnd)
        {
            log::Error(L"overlay: CreateWindowExW failed ({})", GetLastError());
            return false;
        }
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2d.ReleaseAndGetAddressOf())) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(m_dwrite.ReleaseAndGetAddressOf()))))
        {
            log::Error(L"overlay: Direct2D/DirectWrite unavailable");
            return false;
        }
        m_canvas = std::make_unique<Canvas>(m_d2d.Get());
        m_textFamily = TextFamily(m_dwrite.Get());
        m_iconFamily = IconFamily(m_dwrite.Get());

        m_dropTarget = Microsoft::WRL::Make<DropTarget>(m_hwnd.get(), this);
        const HRESULT hr = RegisterDragDrop(m_hwnd.get(), m_dropTarget.Get());
        if (FAILED(hr))
        {
            log::Error(L"overlay: RegisterDragDrop failed (0x{:08X})", static_cast<unsigned>(hr));
            return false;
        }
        m_registered = true;

        // About the Windows 11 acrylic backdrop (DWMWA_SYSTEMBACKDROP_TYPE): DWM only applies system
        // backdrops to windows it composes itself. A per-pixel-alpha layered window supplies its own
        // finished bitmap, so the attribute has no effect here. The Glass style therefore blurs a
        // snapshot of the screen under the wheel itself (theme::CaptureBlurred).
        log::Info(L"overlay: ready ({}, {})", m_textFamily, m_iconFamily);
        return true;
    }

    void Overlay::Destroy()
    {
        m_cursor.Restore();
        m_captureScroll = false;
        if (m_registered && m_hwnd)
            RevokeDragDrop(m_hwnd.get());   // releases OLE's reference to m_dropTarget
        m_registered = false;
        m_hwnd.reset();
        m_dropTarget.Reset();
    }

    LRESULT CALLBACK Overlay::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_NCCREATE)
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        auto* self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg)
        {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return HTCLIENT;   // the alpha-0 pixels are already excluded by the layered-window hit test
        case WM_TIMER:
            if (self && wParam == kTimerFrame)
                self->OnFrame();
            return 0;
        case kMsgHideCursor:
            if (self && self->m_inDrag && self->m_mode == Mode::Wheel && self->m_appearance.hideCursor)
                self->m_cursor.Hide();
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    void Overlay::SetMode(Mode mode)
    {
        m_mode = mode;
        m_captureScroll = mode == Mode::Probe || mode == Mode::Wheel;
    }

    // ---- Layout ----------------------------------------------------------------------------------

    void Overlay::Layout(POINT screenPt)
    {
        m_monitor = MonitorFromPoint(screenPt, MONITOR_DEFAULTTONEAREST);
        m_scale = ScaleFor(m_monitor, m_appearance.size);
        EnsureTextFormats();
        m_tileCache.clear();

        const float s = m_scale;
        m_tileOut = std::round(kTileOutDip * s);
        m_tileIn = std::round(kTileInDip * s);
        m_inner = std::round(kCentreDip * s);
        m_radius = m_tileOut;
        const int half = static_cast<int>(std::ceil(m_tileOut + kMarginDip * s));
        m_size = SIZE{ 2 * half, 2 * half };
        m_c = D2D1::Point2F(static_cast<float>(half), static_cast<float>(half));
        // Centred on the cursor even near a screen edge: shifting the wheel would put the cursor on a
        // slice instead of the dead zone, and a drop without moving would then trigger an action.
        m_origin = POINT{ screenPt.x - half, screenPt.y - half };
    }

    // ---- Showing, hiding, switching -------------------------------------------------------------

    void Overlay::ShowProbe(POINT screenPt, int ring)
    {
        if (!m_hwnd)
            return;
        KillTimer(m_hwnd.get(), kTimerFrame);
        m_animating = false;
        // A previous wheel may still be fading out (click-through); this one must take drops again.
        SetWindowLongPtrW(m_hwnd.get(), GWL_EXSTYLE, GetWindowLongPtrW(m_hwnd.get(), GWL_EXSTYLE) & ~WS_EX_TRANSPARENT);
        if (m_mode == Mode::Closing)
            NotifyHidden();

        m_ring = std::clamp(ring, 0, std::max(0, static_cast<int>(m_rings.size()) - 1));
        Layout(screenPt);
        m_palette = Current(m_appearance.style);
        m_motion = AnimationsOn();
        m_backdrop.Reset();
        m_backdropPx = {};
        if (m_appearance.style == Style::Glass)
        {
            // Before the window is shown, so the snapshot is of what is under it. About 6 window
            // pixels per snapshot pixel at 100%; blurring a small image is cheap and the upscale smooths.
            const int step = std::max(4, static_cast<int>(std::lround(6.f * m_scale)));
            m_backdropPx = CaptureBlurred(RECT{ m_origin.x, m_origin.y, m_origin.x + m_size.cx, m_origin.y + m_size.cy }, step);
            m_backdropStep = static_cast<float>(step);
        }
        SetMode(Mode::Probe);
        m_announced = false;
        m_hot = kCenter;
        m_detail = -1;
        m_scrollAccum = 0;
        m_files.clear();
        m_content = {};
        m_previous = {};
        m_hotT.clear();
        m_lastPt = screenPt;
        m_openT = 0.f;
        m_switchT = 1.f;
        m_away = m_awayTarget = 0.f;
        m_dropped = false;
        Render();
        // Topmost above other topmost windows, without activation.
        SetWindowPos(m_hwnd.get(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        log::Info(L"overlay: probe at ({}, {}), scale {:.2f}, wheel {}", screenPt.x, screenPt.y, m_scale, m_ring);
    }

    void Overlay::NotifyHidden()
    {
        if (m_announced && m_events.hidden)
            m_events.hidden();
        m_announced = false;
    }

    void Overlay::Hide()
    {
        if (!m_hwnd)
            return;
        m_cursor.Restore();
        KillTimer(m_hwnd.get(), kTimerFrame);
        m_animating = false;
        if (m_mode != Mode::Hidden)
            ShowWindow(m_hwnd.get(), SW_HIDE);
        SetWindowLongPtrW(m_hwnd.get(), GWL_EXSTYLE, GetWindowLongPtrW(m_hwnd.get(), GWL_EXSTYLE) & ~WS_EX_TRANSPARENT);
        SetMode(Mode::Hidden);
        NotifyHidden();
    }

    void Overlay::BeginClose()
    {
        m_cursor.Restore();
        if (m_mode != Mode::Wheel || !m_motion)
        {
            Hide();
            return;
        }
        // Fade out over a few frames. WS_EX_TRANSPARENT makes the fading window click-through, so a
        // click right after the drop reaches whatever is underneath.
        SetWindowLongPtrW(m_hwnd.get(), GWL_EXSTYLE, GetWindowLongPtrW(m_hwnd.get(), GWL_EXSTYLE) | WS_EX_TRANSPARENT);
        SetMode(Mode::Closing);
        m_closeT = 0.f;
        NotifyHidden();
        Animate();
    }

    void Overlay::SetRing(int ring)
    {
        if (!IsVisible() || ring < 0 || ring >= static_cast<int>(m_rings.size()) || ring == m_ring)
            return;
        SwitchTo(ring, ring > m_ring ? 1 : -1);
    }

    void Overlay::OnScroll(int wheelDelta)
    {
        if (!IsVisible() || m_rings.size() < 2)
            return;
        // Notches are 120; precision touchpads send many small deltas, so accumulate. Scrolling down
        // (negative delta) moves to the next wheel, like moving down a list.
        m_scrollAccum += wheelDelta;
        const int n = static_cast<int>(m_rings.size());
        // Wheels with nothing for these files are skipped, so scrolling only visits useful ones.
        const auto next = [&](int step) {
            for (int i = 1, r = m_ring; i < n; ++i)
            {
                r = (r + step + n) % n;
                if (HasActions(r))
                    return r;
            }
            return (m_ring + step + n) % n;   // nothing applies anywhere: plain order
        };
        while (m_scrollAccum <= -WHEEL_DELTA)
        {
            m_scrollAccum += WHEEL_DELTA;
            if (const int r = next(1); r != m_ring)
                SwitchTo(r, 1);
        }
        while (m_scrollAccum >= WHEEL_DELTA)
        {
            m_scrollAccum -= WHEEL_DELTA;
            if (const int r = next(-1); r != m_ring)
                SwitchTo(r, -1);
        }
    }

    bool Overlay::HasActions(int ring) const
    {
        if (m_files.empty() || !m_provider)
            return true;   // probe: the files are not known yet
        const auto content = m_provider(m_files, ring);
        return std::any_of(content.slices.begin(), content.slices.end(), [](const WheelSlice& s) { return s.enabled; });
    }

    void Overlay::SwitchTo(int ring, int direction)
    {
        m_ring = ring;
        log::Info(L"overlay: switched to wheel {} ({})", ring, m_rings[static_cast<size_t>(ring)].title);
        if (m_events.ringChanged)
            m_events.ringChanged(ring);
        if (m_mode != Mode::Wheel)
            return;   // probe: DragEnter will ask for this ring's content
        m_previous = m_content;
        m_content = m_provider ? m_provider(m_files, m_ring) : WheelContent{};
        m_hotT.assign(m_content.slices.size(), 0.f);
        m_detail = -1;
        m_switchDir = direction;
        m_switchT = m_motion ? 0.f : 1.f;
        m_tileCache.clear();
        m_hot = HitTest(m_lastPt);
        if (m_hot >= 0 && m_content.slices[static_cast<size_t>(m_hot)].enabled)
            m_detail = m_hot;
        Animate();
    }

    // ---- Animation -------------------------------------------------------------------------------

    // Starts the frame timer if it is not running. 10 ms rounds up to the system timer tick
    // (~15.6 ms), so animations run at about 64 frames a second and stop once everything settles.
    void Overlay::Animate()
    {
        if (!m_hwnd)
            return;
        if (!m_motion)
        {
            m_openT = m_switchT = 1.f;
            for (size_t i = 0; i < m_hotT.size(); ++i)
                m_hotT[i] = static_cast<int>(i) == m_hot && m_content.slices[i].enabled ? 1.f : 0.f;
            Render();
            return;
        }
        if (!m_animating)
        {
            m_animating = true;
            m_lastFrame = GetTickCount64();
            SetTimer(m_hwnd.get(), kTimerFrame, 10, nullptr);
        }
        Render();
    }

    void Overlay::OnFrame()
    {
        const ULONGLONG now = GetTickCount64();
        const float dt = static_cast<float>(std::min<ULONGLONG>(now - m_lastFrame, 50));
        m_lastFrame = now;

        if (m_mode == Mode::Wheel || m_mode == Mode::Closing)
            m_openT = std::min(1.f, m_openT + dt / kOpenMs);
        m_switchT = std::min(1.f, m_switchT + dt / kSwitchMs);
        const float k = 1.f - std::exp(-dt / kHotTauMs);
        for (size_t i = 0; i < m_hotT.size(); ++i)
        {
            const float target = static_cast<int>(i) == m_hot && m_content.slices[i].enabled ? 1.f : 0.f;
            m_hotT[i] += (target - m_hotT[i]) * k;
            if (std::abs(target - m_hotT[i]) < 0.004f)
                m_hotT[i] = target;
        }
        m_away += (m_awayTarget - m_away) * (1.f - std::exp(-dt / kAwayTauMs));
        if (std::abs(m_awayTarget - m_away) < 0.004f)
            m_away = m_awayTarget;
        if (m_mode == Mode::Closing)
        {
            m_closeT = std::min(1.f, m_closeT + dt / kCloseMs);
            if (m_closeT >= 1.f)
            {
                Hide();
                return;
            }
        }
        Render();
        if (Settled())
        {
            KillTimer(m_hwnd.get(), kTimerFrame);
            m_animating = false;
        }
    }

    bool Overlay::Settled() const
    {
        if (m_mode == Mode::Closing || m_openT < 1.f || m_switchT < 1.f || m_away != m_awayTarget)
            return false;
        for (size_t i = 0; i < m_hotT.size(); ++i)
            if (m_hotT[i] != (static_cast<int>(i) == m_hot && m_content.slices[i].enabled ? 1.f : 0.f))
                return false;
        return true;
    }

    // ---- Hit-testing and drag events -------------------------------------------------------------

    int Overlay::HitTest(POINT pt) const
    {
        const float dx = static_cast<float>(pt.x - m_origin.x) - m_c.x, dy = static_cast<float>(pt.y - m_origin.y) - m_c.y;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d > m_radius + kLiftDip * m_scale)
            return kOutside;
        if (d < m_inner || m_content.slices.empty())
            return kCenter;
        float deg = std::atan2(dx, -dy) * 180.f / std::numbers::pi_v<float>;   // 0 at top, clockwise
        if (deg < 0)
            deg += 360.f;
        const auto n = static_cast<int>(m_content.slices.size());
        const float span = 360.f / static_cast<float>(n);
        return static_cast<int>(std::floor((deg + span / 2.f) / span)) % n;
    }

    void Overlay::SetHot(int hot)
    {
        if (hot == m_hot)
            return;
        m_hot = hot;
        if (hot >= 0 && m_content.slices[static_cast<size_t>(hot)].enabled)
            m_detail = hot;
        Animate();
    }

    bool Overlay::OnDragEnter(const std::vector<std::wstring>& files, POINT pt)
    {
        m_inDrag = true;
        if (m_mode != Mode::Probe && m_mode != Mode::Wheel)
            return false;
        if (files.empty())
        {
            log::Info(L"overlay: drag has no CF_HDROP (text, URL or virtual file); hiding");
            Hide();
            return false;
        }
        const bool opening = m_mode == Mode::Probe;
        m_files = files;
        if (opening && m_rings[static_cast<size_t>(m_ring)].fallThrough && !HasActions(m_ring))
        {
            for (int r = 0; r < static_cast<int>(m_rings.size()); ++r)
                if (r != m_ring && HasActions(r))
                {
                    log::Info(L"overlay: nothing on {} applies; opening {}", m_rings[static_cast<size_t>(m_ring)].title,
                              m_rings[static_cast<size_t>(r)].title);
                    m_ring = r;
                    break;
                }
        }
        m_content = m_provider ? m_provider(files, m_ring) : WheelContent{};
        m_hotT.assign(m_content.slices.size(), 0.f);
        m_tileCache.clear();
        if (opening)
        {
            m_openT = m_motion ? 0.f : 1.f;
            if (m_appearance.hideCursor)
                PostMessageW(m_hwnd.get(), kMsgHideCursor, 0, 0);   // not inside DragEnter: the source is waiting on us
        }
        SetMode(Mode::Wheel);
        m_lastPt = pt;
        m_hot = HitTest(pt);
        if (!m_announced && m_events.shown)
            m_events.shown(m_ring, m_monitor);
        m_announced = true;
        Animate();
        log::Info(L"overlay: DragEnter with {} file(s), first: {}", files.size(), files.front());
        return true;
    }

    DWORD Overlay::OnDragOver(POINT pt, DWORD allowed)
    {
        if (m_mode != Mode::Wheel)
            return DROPEFFECT_NONE;
        m_lastPt = pt;
        const int hot = HitTest(pt);
        SetHot(hot);
        // Heading out past the tiles, the wheel fades a little with distance, so leaving it is a
        // gradual hand-off rather than a sudden disappearance at the window edge.
        const float d = std::hypot(static_cast<float>(pt.x - m_origin.x) - m_c.x, static_cast<float>(pt.y - m_origin.y) - m_c.y);
        const float away = std::clamp((d - m_tileOut) / (kMarginDip * m_scale), 0.f, 1.f);
        if (std::abs(away - m_awayTarget) > 0.01f)
        {
            m_awayTarget = away;
            Animate();
        }
#if SHIFTDROP_M0
        return hot != kOutside ? SafeEffect(allowed) : DROPEFFECT_NONE;
#else
        // Disabled slices answer NONE, so the cursor says "not allowed" there. The centre is where the
        // cursor starts, so it answers COPY like a slice: a "not allowed" cursor the moment the wheel
        // opens reads as broken. Releasing on the centre still does nothing (OnDrop finds no slice).
        if (hot == kCenter || (hot >= 0 && m_content.slices[static_cast<size_t>(hot)].enabled))
            return SafeEffect(allowed);
        return DROPEFFECT_NONE;
#endif
    }

    void Overlay::OnDragLeave()
    {
        m_inDrag = false;
        m_cursor.Restore();
        if (IsVisible())
            log::Info(L"overlay: DragLeave (cursor left the wheel, Esc, or released on the centre); hiding");
        BeginClose();
    }

    void Overlay::OnDrop(const std::vector<std::wstring>& files, POINT pt)
    {
        m_inDrag = false;
        m_dropped = true;
        m_cursor.Restore();
        const int hot = HitTest(pt);
        // The drop is handed over before the wheel starts closing, so whoever listens to `hidden`
        // already knows something was dropped.
        if (m_onDrop && !files.empty())
        {
#if SHIFTDROP_M0
            (void)hot;
            m_onDrop(files, L"");
#else
            if (hot >= 0 && m_content.slices[static_cast<size_t>(hot)].enabled)
                m_onDrop(files, m_content.slices[static_cast<size_t>(hot)].actionId);
#endif
        }
        BeginClose();   // fades out with the chosen slice still lit
    }

    // ---- Drawing resources -----------------------------------------------------------------------

    ComPtr<IDWriteTextFormat> Overlay::MakeFormat(const std::wstring& family, float dip, DWRITE_FONT_WEIGHT weight) const
    {
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(m_dwrite->CreateTextFormat(family.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                              dip * m_scale, L"", &format)))
            return nullptr;
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        // Labels that do not fit end in "…" rather than being cut mid-letter.
        DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(m_dwrite->CreateEllipsisTrimmingSign(format.Get(), &ellipsis)))
            format->SetTrimming(&trimming, ellipsis.Get());
        return format;
    }

    bool Overlay::EnsureTextFormats()
    {
        if (m_labelFont && m_fontScale == m_scale)
            return true;
        m_fontScale = m_scale;
        m_labelFont = MakeFormat(m_textFamily, 13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        m_formatFont = MakeFormat(m_textFamily, 18.f, DWRITE_FONT_WEIGHT_BOLD);
        m_iconFont = MakeFormat(m_iconFamily, 22.f, DWRITE_FONT_WEIGHT_NORMAL);
        m_countFont = MakeFormat(m_textFamily, 19.f, DWRITE_FONT_WEIGHT_BOLD);
        m_ringFont = MakeFormat(m_textFamily, 10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        m_detailFont = MakeFormat(m_textFamily, 11.f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        if (m_detailFont)
            m_detailFont->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        return m_labelFont && m_formatFont && m_iconFont && m_countFont && m_ringFont && m_detailFont;
    }

    ComPtr<ID2D1SolidColorBrush> Overlay::Brush(const D2D1_COLOR_F& color, float alpha)
    {
        ComPtr<ID2D1SolidColorBrush> brush;
        m_rt->CreateSolidColorBrush(WithAlpha(color, alpha), &brush);
        return brush;
    }

    void Overlay::Text(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float alpha)
    {
        if (text.empty() || !format || alpha <= 0.f)
            return;
        m_rt->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, rect, Brush(color, alpha).Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    D2D1_POINT_2F Overlay::Polar(float radius, float degrees) const
    {
        const float rad = degrees * std::numbers::pi_v<float> / 180.f;
        return D2D1::Point2F(m_c.x + radius * std::sin(rad), m_c.y - radius * std::cos(rad));
    }

    // One tile: an annular sector with a constant-width gap to its neighbours and rounded corners.
    // Cached, because only the hovered tile changes size from frame to frame.
    ComPtr<ID2D1Geometry> Overlay::TileGeometry(size_t count, size_t index, float inner, float outer)
    {
        const uint64_t key = (static_cast<uint64_t>(count) << 56) | (static_cast<uint64_t>(index) << 48) |
                             (static_cast<uint64_t>(std::lround(inner * 4.f)) << 24) | static_cast<uint64_t>(std::lround(outer * 4.f));
        if (auto it = m_tileCache.find(key); it != m_tileCache.end())
            return it->second;

        const float corner = kCornerDip * m_scale;
        ComPtr<ID2D1PathGeometry> core;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(m_d2d->CreatePathGeometry(&core)) || FAILED(core->Open(&sink)))
            return nullptr;
        if (count == 1)
        {
            // A single slice is the whole ring: two circles, filled with the alternate rule.
            sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
            for (const float r : { outer, inner })
            {
                sink->BeginFigure(Polar(r, 0.f), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddArc(D2D1::ArcSegment(Polar(r, 180.f), D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sink->AddArc(D2D1::ArcSegment(Polar(r, 360.f), D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            }
            sink->Close();
            return m_tileCache[key] = core;
        }
        // Each side is a line kGap/2 from the radial line (not a radius), so the gap has the same width
        // all along. Every corner is a circle of radius `corner` tangent to both edges it joins.
        const float span = 360.f / static_cast<float>(count);
        const float a0 = static_cast<float>(index) * span - span / 2.f, a1 = a0 + span;
        const float g = kGapDip * m_scale / 2.f;
        const float c = std::min(corner, (outer - inner) / 2.f - 0.5f);
        constexpr float kDeg = 180.f / std::numbers::pi_v<float>;
        if (c <= 0.f || inner + c <= g + c)
            return nullptr;
        // A point `along` the radial line at angle a, `side` across it (positive = toward larger angles).
        const auto at = [&](float a, float along, float side) {
            const float rad = a / kDeg;
            return D2D1::Point2F(m_c.x + along * std::sin(rad) + side * std::cos(rad),
                                 m_c.y - along * std::cos(rad) + side * std::sin(rad));
        };
        // Corner circles: tangent to the outer arc from inside (centre at outer - c) or to the inner arc
        // from outside (centre at inner + c), and to the side line (centre g + c away from the radial).
        const float outerC = outer - c, innerC = inner + c;
        const float dOut = std::asin((g + c) / outerC) * kDeg, dIn = std::asin((g + c) / innerC) * kDeg;
        const float lOut = std::sqrt(outerC * outerC - (g + c) * (g + c));   // tangent point along the side
        const float lIn = std::sqrt(innerC * innerC - (g + c) * (g + c));
        const auto cw = D2D1_SWEEP_DIRECTION_CLOCKWISE;
        const D2D1_SIZE_F cr = D2D1::SizeF(c, c);
        const auto outerArc = (span - 2.f * dOut) > 180.f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;
        const auto innerArc = (span - 2.f * dIn) > 180.f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;

        sink->BeginFigure(Polar(outer, a0 + dOut), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddArc(D2D1::ArcSegment(Polar(outer, a1 - dOut), D2D1::SizeF(outer, outer), 0.f, cw, outerArc));
        sink->AddArc(D2D1::ArcSegment(at(a1, lOut, -g), cr, 0.f, cw, D2D1_ARC_SIZE_SMALL));
        sink->AddLine(at(a1, lIn, -g));
        sink->AddArc(D2D1::ArcSegment(Polar(inner, a1 - dIn), cr, 0.f, cw, D2D1_ARC_SIZE_SMALL));
        sink->AddArc(D2D1::ArcSegment(Polar(inner, a0 + dIn), D2D1::SizeF(inner, inner), 0.f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, innerArc));
        sink->AddArc(D2D1::ArcSegment(at(a0, lIn, g), cr, 0.f, cw, D2D1_ARC_SIZE_SMALL));
        sink->AddLine(at(a0, lOut, g));
        sink->AddArc(D2D1::ArcSegment(Polar(outer, a0 + dOut), cr, 0.f, cw, D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        return m_tileCache[key] = core;
    }

    // ---- Drawing ---------------------------------------------------------------------------------

    void Overlay::DrawSlices(const WheelContent& content, const std::vector<float>& hotT, float alpha, float rotation)
    {
        const size_t n = content.slices.size();
        if (n == 0 || alpha <= 0.f)
            return;
        D2D1_MATRIX_3X2_F base{};
        m_rt->GetTransform(&base);
        const auto turned = D2D1::Matrix3x2F::Rotation(rotation, m_c) * base;

        const float span = 360.f / static_cast<float>(n);
        const float s = m_scale;
        // Draw the hovered tile last so its lift overlaps its neighbours cleanly.
        std::vector<size_t> order(n);
        for (size_t i = 0; i < n; ++i)
            order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return (a < hotT.size() ? hotT[a] : 0.f) < (b < hotT.size() ? hotT[b] : 0.f);
        });
        for (const size_t i : order)
        {
            const auto& slice = content.slices[i];
            const float t = i < hotT.size() && slice.enabled ? hotT[i] : 0.f;
            const float e = EaseOutCubic(t);
            const float mid = static_cast<float>(i) * span;
            // The hovered tile lifts: it grows outward and moves out along its centre line.
            const float push = kPushDip * s * e;
            const float rad = mid * std::numbers::pi_v<float> / 180.f;
            m_rt->SetTransform(D2D1::Matrix3x2F::Translation(push * std::sin(rad), -push * std::cos(rad)) * turned);

            const float outer = m_tileOut + kLiftDip * s * e;
            auto tile = TileGeometry(n, i, m_tileIn, outer);
            if (!tile)
                continue;
            m_rt->FillGeometry(tile.Get(), Brush(m_palette.tile, alpha).Get());
            if (t > 0.001f)
            {
                // Accent fill, a little lighter toward the rim.
                const D2D1_GRADIENT_STOP stops[] = { { 0.f, m_palette.accentHi }, { 1.f, m_palette.accent } };
                ComPtr<ID2D1GradientStopCollection> collection;
                ComPtr<ID2D1LinearGradientBrush> fill;
                if (SUCCEEDED(m_rt->CreateGradientStopCollection(stops, 2, &collection)) &&
                    SUCCEEDED(m_rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(Polar(outer, mid), Polar(m_tileIn, mid)),
                                                              collection.Get(), &fill)))
                {
                    fill->SetOpacity(t * alpha);
                    m_rt->FillGeometry(tile.Get(), fill.Get());
                }
            }
            m_rt->DrawGeometry(tile.Get(), Brush(m_palette.tileLine, alpha * (1.f - t)).Get(), 1.f);

            // Icon above the label on the tile's centre line; format names (no icon) are larger.
            const float r = (m_tileIn + m_tileOut) / 2.f + 2.f * s * e;
            const auto at = Polar(r, mid);
            const float width = std::min(2.f * std::numbers::pi_v<float> * r / static_cast<float>(n), 2.f * r) - 12.f * s;
            const auto colour = slice.enabled ? Mix(m_palette.text, m_palette.textOnAccent, t) : m_palette.textDim;
            if (slice.icon)
            {
                const float iy = at.y - 11.f * s;
                const auto box = D2D1::RectF(at.x - 18.f * s, iy - 15.f * s, at.x + 18.f * s, iy + 15.f * s);
                D2D1_MATRIX_3X2_F current{};
                m_rt->GetTransform(&current);
                if (slice.mirrorIcon)
                    m_rt->SetTransform(D2D1::Matrix3x2F::Scale(-1.f, 1.f, D2D1::Point2F(at.x, iy)) * current);
                const wchar_t glyph[2]{ slice.icon, 0 };
                Text(glyph, m_iconFont.Get(), box, colour, alpha);
                m_rt->SetTransform(current);
                Text(slice.label, m_labelFont.Get(), D2D1::RectF(at.x - width / 2.f, at.y + 4.f * s, at.x + width / 2.f, at.y + 23.f * s), colour, alpha);
            }
            else
            {
                Text(slice.label, m_formatFont.Get(), D2D1::RectF(at.x - width / 2.f, at.y - 14.f * s, at.x + width / 2.f, at.y + 14.f * s), colour, alpha);
            }
        }
        m_rt->SetTransform(base);
    }

    void Overlay::DrawCentre()
    {
        const float s = m_scale;
        m_rt->FillEllipse(D2D1::Ellipse(m_c, m_inner, m_inner), Brush(m_palette.centre).Get());
        m_rt->DrawEllipse(D2D1::Ellipse(m_c, m_inner - 0.5f, m_inner - 0.5f), Brush(m_palette.centreLine).Get(), 1.f);

        const float w = 2.f * m_inner - 14.f * s;
        Text(m_content.centerText, m_countFont.Get(), D2D1::RectF(m_c.x - w / 2, m_c.y - 22.f * s, m_c.x + w / 2, m_c.y + 2.f * s), m_palette.text);

        // Below the count: the wheel's name, crossfading to the hovered slice's detail.
        float detailT = 0.f;
        if (m_detail >= 0 && static_cast<size_t>(m_detail) < m_hotT.size())
            detailT = m_hotT[static_cast<size_t>(m_detail)];
        const auto lower = D2D1::RectF(m_c.x - w / 2, m_c.y + 2.f * s, m_c.x + w / 2, m_c.y + 20.f * s);
        if (!m_rings.empty())
        {
            std::wstring title = m_rings[static_cast<size_t>(m_ring)].title;
            for (auto& ch : title)
                ch = static_cast<wchar_t>(towupper(ch));
            Text(title, m_ringFont.Get(), lower, m_palette.text, 0.62f * (1.f - detailT));
        }
        if (detailT > 0.f && m_detail >= 0 && static_cast<size_t>(m_detail) < m_content.slices.size())
        {
            const auto& slice = m_content.slices[static_cast<size_t>(m_detail)];
            Text(slice.detail.empty() ? slice.label : slice.detail, m_detailFont.Get(),
                 D2D1::RectF(lower.left, lower.top - 1.f * s, lower.right, lower.bottom + 12.f * s), m_palette.text, 0.85f * detailT);
        }

        // Dots: one per wheel, the current one in the accent colour (scroll to move between them).
        const size_t n = m_rings.size();
        if (n > 1)
        {
            const float gap = 9.f * s, dotY = m_c.y + m_inner - 12.f * s;
            const float x0 = m_c.x - gap * static_cast<float>(n - 1) / 2.f;
            for (size_t i = 0; i < n; ++i)
            {
                const bool on = static_cast<int>(i) == m_ring;
                const float r = (on ? 2.6f : 2.1f) * s;
                m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x0 + gap * static_cast<float>(i), dotY), r, r),
                                  Brush(on ? m_palette.accent : m_palette.textDim, on ? 1.f : 0.9f).Get());
            }
        }
    }

    void Overlay::DrawScene()
    {
        const float s = m_scale;
        // Opening springs from 86% with a slight overshoot; closing shrinks a little while it fades.
        float zoom = m_motion ? 0.86f + 0.14f * Spring(m_openT) : 1.f;
        zoom *= 1.f - 0.03f * m_away;
        if (m_mode == Mode::Closing)
            // After a drop it settles in place; when the cursor left, it shrinks away a bit more.
            zoom *= 1.f - (m_dropped ? 0.04f : 0.1f) * EaseOutCubic(m_closeT);
        m_rt->SetTransform(D2D1::Matrix3x2F::Scale(zoom, zoom, m_c));

        // An almost invisible disc under everything keeps the gaps between tiles hit-testable, so the
        // cursor never "leaves" the wheel while crossing a gap.
        const float reach = m_tileOut + kLiftDip * s;
        m_rt->FillEllipse(D2D1::Ellipse(m_c, reach, reach), Brush(D2D1::ColorF(0, 0, 0, 1), 0.02f).Get());

        // Soft shadow under the ring, slightly lower than the wheel as if lit from above.
        const float shadowR = m_tileOut + 26.f * s;
        const auto sc = D2D1::Point2F(m_c.x, m_c.y + 5.f * s);
        const D2D1_GRADIENT_STOP stops[] = {
            { 0.f, D2D1::ColorF(0, 0, 0, 0) },
            { (m_tileIn - 4.f * s) / shadowR, D2D1::ColorF(0, 0, 0, 0) },
            { (m_tileIn + 10.f * s) / shadowR, WithAlpha(m_palette.shadow, 0.55f) },
            { m_tileOut / shadowR, m_palette.shadow },
            { (m_tileOut + 9.f * s) / shadowR, WithAlpha(m_palette.shadow, 0.35f) },
            { 1.f, D2D1::ColorF(0, 0, 0, 0) },
        };
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1RadialGradientBrush> shadow;
        if (SUCCEEDED(m_rt->CreateGradientStopCollection(stops, ARRAYSIZE(stops), &collection)) &&
            SUCCEEDED(m_rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(sc, D2D1::Point2F(), shadowR, shadowR), collection.Get(), &shadow)))
            m_rt->FillEllipse(D2D1::Ellipse(sc, shadowR, shadowR), shadow.Get());

        if (m_appearance.style == Style::Glass)
        {
            // Frosted disc: the blurred snapshot, then the tint, then two hairlines.
            if (!m_backdrop || m_backdropGeneration != m_canvas->DeviceGeneration())
            {
                m_backdrop = ToBitmap(m_rt, m_backdropPx);
                m_backdropGeneration = m_canvas->DeviceGeneration();
            }
            const float r = m_tileOut + 4.f * s;
            const auto disc = D2D1::Ellipse(m_c, r, r);
            if (m_backdrop)
            {
                ComPtr<ID2D1BitmapBrush> glass;
                m_rt->CreateBitmapBrush(m_backdrop.Get(),
                                        D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR), &glass);
                if (glass)
                {
                    glass->SetTransform(D2D1::Matrix3x2F::Scale(m_backdropStep, m_backdropStep));
                    m_rt->FillEllipse(disc, glass.Get());
                }
            }
            m_rt->FillEllipse(disc, Brush(m_palette.glassTint, m_backdrop ? 1.f : 1.3f).Get());
            m_rt->DrawEllipse(D2D1::Ellipse(m_c, r + 0.5f, r + 0.5f), Brush(D2D1::ColorF(0, 0, 0, 1), m_palette.dark ? 0.5f : 0.15f).Get(), 1.f);
            m_rt->DrawEllipse(D2D1::Ellipse(m_c, r - 0.5f, r - 0.5f), Brush(D2D1::ColorF(1, 1, 1, 1), m_palette.dark ? 0.14f : 0.8f).Get(), 1.f);
        }

#if SHIFTDROP_M0
        // M0 placeholder: one plain ring and the product name; any drop on it is logged.
        if (auto ring = TileGeometry(1, 0, m_tileIn, m_tileOut))
            m_rt->FillGeometry(ring.Get(), Brush(m_palette.tile).Get());
        DrawCentre();
        Text(SD_NAME, m_labelFont.Get(), D2D1::RectF(m_c.x - m_tileOut, m_c.y - m_tileOut, m_c.x + m_tileOut, m_c.y - m_tileIn), m_palette.text);
#else
        if (m_content.slices.empty())
        {
            // Nothing applies to these files on this wheel: an empty ring keeps the shape.
            if (auto ring = TileGeometry(1, 0, m_tileIn, m_tileOut))
                m_rt->FillGeometry(ring.Get(), Brush(m_palette.tile, 0.6f).Get());
        }
        // Switching wheels: the old tiles turn away and fade while the new ones turn in.
        if (m_switchT < 1.f)
        {
            const float e = EaseOutCubic(m_switchT);
            const float turn = 24.f * static_cast<float>(m_switchDir);
            DrawSlices(m_previous, {}, 1.f - e, -turn * e);
            DrawSlices(m_content, m_hotT, e, turn * (1.f - e));
        }
        else
        {
            DrawSlices(m_content, m_hotT, 1.f, 0.f);
        }
        DrawCentre();
#endif
    }

    void Overlay::Render()
    {
        if (!m_hwnd || !m_canvas || !EnsureTextFormats())
            return;
        m_rt = m_canvas->Begin(m_size);
        if (!m_rt)
            return;
        if (m_mode == Mode::Probe)
        {
            // Alpha ~2%: invisible to the eye, but non-zero so the disc is hit-testable for OLE.
            m_rt->FillEllipse(D2D1::Ellipse(m_c, m_radius, m_radius), Brush(D2D1::ColorF(0, 0, 0, 1), 0.02f).Get());
        }
        else if (m_mode == Mode::Wheel || m_mode == Mode::Closing)
        {
            DrawScene();
        }
        float opacity = 1.f;
        if (m_mode == Mode::Wheel && m_motion)
            // Never fully transparent while it takes drops: the layered hit test ignores pixels that
            // end up with alpha 0.
            opacity = std::max(0.1f, EaseOutCubic(m_openT * kOpenMs / kFadeInMs)) * (1.f - kAwayFade * m_away);
        else if (m_mode == Mode::Closing)
            opacity = (1.f - kAwayFade * m_away) * (1.f - EaseOutCubic(m_closeT));
        m_canvas->End(m_hwnd.get(), m_origin, opacity);
        m_rt = nullptr;
    }
}
