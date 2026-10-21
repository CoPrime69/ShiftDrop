#include "Brand.h"
#include "Overlay.h"
#include "DropDescription.h"
#include "ErrorText.h"
#include "Log.h"
#include "PanelLayout.h"
#include "Theme.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <numbers>

using Microsoft::WRL::ComPtr;
using namespace sd::theme;

namespace sd
{
    namespace
    {
        constexpr wchar_t kClassName[] = L"" SD_NAME L".Overlay";

        // Sizes in DIPs; Layout() turns them into pixels for the monitor and the Size setting. The ring
        // radii are in keyboard/WheelLayout.h (one ring up to 8 slices, two up to 16).
        constexpr float kGapDip = 4.f;         // straight gap between tiles
        constexpr float kCornerDip = 6.f;      // tile corner rounding
        constexpr float kLiftDip = 7.f;        // a hovered tile grows outward by this much...
        constexpr float kPushDip = 3.f;        // ...and moves out along its centre line by this much
        constexpr float kInnerLiftDip = 2.f;   // on two rings the inner ring lifts less, so it stays off the outer one
        constexpr float kInnerPushDip = 1.f;
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
        constexpr UINT_PTR kTimerMore = 2;     // dwelling on "More" during a drag turns the page
        constexpr UINT kMoreDwellMs = 650;
        constexpr LONG kKeyHoldPx = 2;         // a slice picked by key stays lit until the mouse moves more than this
        constexpr float kEdgeRoomDip = 8.f;    // gap kept between the tiles and the monitor's work area edge
        constexpr float kEdgeGuardDip = 6.f;   // a moved wheel ignores the cursor until it moves this far

        constexpr wchar_t kMoreIcon = 0xE712;   // "More" (three dots)

        // The centre's count line ("3 files"): this size, smaller when it is long, then two lines.
        constexpr float kCountDip = 19.f, kCountMinDip = 13.f;
        // The badge the count moves into when the drag picture is over the centre.
        constexpr float kBadgeFontDip = 13.f;
        constexpr float kBadgeLineDip = 18.f;
        constexpr float kBadgePadDip = 9.f, kBadgeVPadDip = 3.f;
        constexpr float kBadgeMaxDip = 210.f;

        // How big `text` is laid out in `format`, wrapping at `maxWidth` px.
        D2D1_SIZE_F LayoutSize(IDWriteFactory* dwrite, const std::wstring& text, IDWriteTextFormat* format, float maxWidth)
        {
            ComPtr<IDWriteTextLayout> layout;
            DWRITE_TEXT_METRICS m{};
            if (text.empty() || !format || FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, maxWidth, 10000.f, &layout)) ||
                FAILED(layout->GetMetrics(&m)))
                return D2D1::SizeF(0.f, 0.f);
            return D2D1::SizeF(m.widthIncludingTrailingWhitespace, m.height);
        }
    }

    Overlay::Overlay(HINSTANCE instance, std::vector<WheelRing> rings, ContentProvider provider, DropHandler onDrop, Events events)
        : m_instance(instance), m_rings(std::move(rings)), m_provider(std::move(provider)), m_onDrop(std::move(onDrop)),
          m_events(std::move(events))
    {
    }

    Overlay::~Overlay() { Destroy(); }

    bool Overlay::Create()
    {
        // WS_EX_NOACTIVATE + WM_MOUSEACTIVATE=MA_NOACTIVATE: showing or touching the wheel never takes
        // focus from the app you are dragging out of. WS_EX_TOOLWINDOW keeps it off the taskbar/Alt+Tab.
        m_hwnd.reset(CreatePanel(m_instance, kClassName, WndProc, WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, SD_NAME, this));
        if (!m_hwnd)
        {
            log::Error(L"overlay: CreateWindowExW failed ({})", GetLastError());
            return false;
        }
        if (!InitGraphics())
        {
            log::Error(L"overlay: Direct2D/DirectWrite unavailable");
            return false;
        }
        m_a11y = std::make_unique<a11y::WheelElement>(m_hwnd.get(), static_cast<a11y::WheelView&>(*this));

        // With the shell's drag-image helper, the dragged item's picture (Explorer's icon or thumbnail
        // and its name, a browser's image) stays visible and follows the cursor over the wheel.
        m_dropTarget = Microsoft::WRL::Make<DropTarget>(m_hwnd.get(), this, MakeDragImageHelper());
        const HRESULT hr = RegisterDragDrop(m_hwnd.get(), m_dropTarget.Get());
        if (FAILED(hr))
        {
            log::Error(L"overlay: RegisterDragDrop failed ({})", HexCode(hr));
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
        m_captureScroll = false;
        if (m_registered && m_hwnd)
            RevokeDragDrop(m_hwnd.get());   // releases OLE's reference to m_dropTarget
        m_registered = false;
        m_a11y.reset();   // before the window: it tells UIA the window's elements are gone
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
            if (self && wParam == kTimerMore)
            {
                KillTimer(hwnd, kTimerMore);
                if (self->m_mode == Mode::Wheel && self->m_layout.IsMore(self->m_hot))
                    self->NextPage();
            }
            return 0;
        // Click mode: the wheel is not part of a drag, so it gets ordinary mouse messages (it is
        // still never activated, see WM_MOUSEACTIVATE).
        case WM_MOUSEMOVE:
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            if (self && self->InClickMode())
            {
                POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ClientToScreen(hwnd, &pt);
                if (msg == WM_MOUSEMOVE)
                {
                    if (!self->m_tracking)
                    {
                        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                        self->m_tracking = TrackMouseEvent(&tme) != FALSE;
                    }
                    self->OnClickMove(pt);
                }
                else if (msg == WM_LBUTTONUP)
                    self->OnClickUp(pt);
                else
                    self->Cancel();
            }
            return 0;
        case WM_MOUSELEAVE:
            if (self)
            {
                self->m_tracking = false;
                if (self->InClickMode() && !self->m_keyHot)
                    self->SetHot(kOutside);
            }
            return 0;
        case WM_GETOBJECT:
            if (LRESULT result = 0; self && self->m_a11y && self->m_a11y->OnGetObject(wParam, lParam, result))
                return result;
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    void Overlay::SetMode(Mode mode)
    {
        const bool closed = m_mode == Mode::Wheel && mode != Mode::Wheel;
        m_mode = mode;
        m_captureScroll = mode == Mode::Probe || mode == Mode::Wheel;
        if (closed && m_a11y)
            m_a11y->Closed();
    }

    // ---- Layout ----------------------------------------------------------------------------------

    void Overlay::Layout(POINT screenPt, int rings)
    {
        m_monitor = MonitorFromPoint(screenPt, MONITOR_DEFAULTTONEAREST);
        m_scale = ScaleFor(m_monitor, m_appearance.size);
        EnsureTextFormats();
        m_tileCache.clear();

        const float s = m_scale;
        m_windowRings = std::clamp(rings, 1, 2);
        m_tileIn = std::round(wheel::kInnerInDip * s);
        m_inner = std::round(wheel::kCentreDip * s);
        const float extent = std::round(wheel::RingBand(m_windowRings - 1).outer * s);
        const int half = static_cast<int>(std::ceil(extent + kMarginDip * s));
        m_size = SIZE{ 2 * half, 2 * half };
        m_c = D2D1::Point2F(static_cast<float>(half), static_cast<float>(half));
        // Near a screen edge the wheel moves in so every tile stays on screen (the window's shadow
        // margin may still hang over the edge). The cursor is then no longer on the centre, so
        // PointerSlot treats the opening spot as the centre until the cursor moves: dropping without
        // moving still runs nothing.
        const POINT centre = ClampCentre(screenPt, extent + kEdgeRoomDip * s);
        m_shifted = centre.x != m_openPt.x || centre.y != m_openPt.y;
        m_edgeGuard = m_edgeGuard || m_shifted;
        m_origin = POINT{ centre.x - half, centre.y - half };
    }

    POINT Overlay::ClampCentre(POINT pt, float extentPx) const
    {
        MONITORINFO mi{ sizeof(mi) };
        if (!GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi))
            return pt;
        // The work area leaves out the taskbar, so no tile hides under it.
        const RECT& work = mi.rcWork;
        const LONG r = static_cast<LONG>(std::ceil(extentPx));
        const auto clampAxis = [r](LONG v, LONG lo, LONG hi) {
            return hi - lo < 2 * r ? (lo + hi) / 2 : std::clamp(v, lo + r, hi - r);   // a tiny monitor: centred
        };
        return POINT{ clampAxis(pt.x, work.left, work.right), clampAxis(pt.y, work.top, work.bottom) };
    }

    int Overlay::PointerSlot(POINT pt)
    {
        if (m_edgeGuard)
        {
            const LONG guard = static_cast<LONG>(std::lround(kEdgeGuardDip * m_scale));
            if (std::abs(pt.x - m_openPt.x) <= guard && std::abs(pt.y - m_openPt.y) <= guard)
                return HitTest(pt) == kOutside ? kOutside : kCenter;
            m_edgeGuard = false;
        }
        return HitTest(pt);
    }

    void Overlay::EnsureRings(int rings)
    {
        if (rings <= m_windowRings)
            return;   // never shrinks while open: a one-ring wheel sits fine in a bigger window
        // Grows around the same centre, so nothing under the cursor moves.
        const POINT centre{ m_origin.x + static_cast<LONG>(std::lround(m_c.x)), m_origin.y + static_cast<LONG>(std::lround(m_c.y)) };
        Layout(centre, rings);
    }

    void Overlay::Arrange(int page)
    {
        m_layout = wheel::Arrange(m_content.slices.size(), page);
        m_shown.clear();
        for (int i = 0; i < static_cast<int>(m_layout.slots.size()); ++i)
        {
            if (m_layout.IsMore(i))
                m_shown.push_back(WheelSlice{ L"More", L"", true, kMoreIcon, false,
                                              std::format(L"Page {} of {}", m_layout.page + 1, m_layout.pages) });
            else
                m_shown.push_back(m_content.slices[static_cast<size_t>(m_layout.ContentIndex(i))]);
        }
        m_hotT.assign(m_shown.size(), 0.f);
        EnsureRings(m_layout.rings);
        m_tileOut = std::round(m_layout.OuterEdge() * m_scale);
        m_radius = m_tileOut;
        m_fits = FitShown(m_shown, m_layout);
    }

    std::vector<wheel::FittedLabel> Overlay::FitShown(const std::vector<WheelSlice>& slices, const wheel::Layout& layout)
    {
        std::vector<wheel::LabelSpec> labels;
        for (const auto& slice : slices)
            labels.push_back(wheel::LabelSpec{ slice.label, slice.icon ? wheel::LabelStyle::Icon : wheel::LabelStyle::Name });
        const auto measure = [this](const std::wstring& text, wheel::LabelStyle style) {
            const float full = wheel::FullSize(style);
            return theme::TextWidth(m_dwrite.Get(), text, SizedFormat(style == wheel::LabelStyle::Name ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD, full)) /
                   m_scale;
        };
        auto fits = wheel::FitLabels(layout, labels, measure);
        for (size_t i = 0; i < fits.size(); ++i)
            if (fits[i].shortened || !fits[i].fits || fits[i].size < wheel::MinSize(labels[i].style))
                log::Info(L"overlay: label '{}' drawn as '{}' at {:.1f} DIP{}", labels[i].text, fits[i].Text(), fits[i].size,
                          fits[i].fits ? L"" : L" (does not fit)");
        return fits;
    }

    bool Overlay::Selectable(int slot) const
    {
        return slot >= 0 && slot < static_cast<int>(m_shown.size()) && m_shown[static_cast<size_t>(slot)].enabled;
    }

    bool Overlay::Explains(int slot) const
    {
        return slot >= 0 && slot < static_cast<int>(m_shown.size()) && !m_shown[static_cast<size_t>(slot)].enabled &&
               !m_shown[static_cast<size_t>(slot)].detail.empty();
    }

    // ---- Showing, hiding, switching -------------------------------------------------------------

    void Overlay::ShowProbe(POINT screenPt, int ring)
    {
        if (!m_hwnd)
            return;
        m_frames.Stop(m_hwnd.get(), kTimerFrame);
        // A previous wheel may still be fading out (click-through); this one must take drops again.
        SetWindowLongPtrW(m_hwnd.get(), GWL_EXSTYLE, GetWindowLongPtrW(m_hwnd.get(), GWL_EXSTYLE) & ~WS_EX_TRANSPARENT);
        if (m_mode == Mode::Closing)
            NotifyHidden();

        m_ring = std::clamp(ring, 0, std::max(0, static_cast<int>(m_rings.size()) - 1));
        m_openPt = screenPt;
        m_edgeGuard = false;
        Layout(screenPt, 1);
        m_tileOut = std::round(wheel::kInnerOutDip * m_scale);
        m_radius = m_tileOut;
        m_palette = Current(m_appearance.style);
        m_motion = AnimationsOn();
        m_backdrop.Reset();
        m_backdropPx = {};
        if (m_appearance.style == Style::Glass)
        {
            // Before the window is shown, so the snapshot is of what is under it. It covers the
            // largest (two-ring) wheel, so a wheel that grows later still has glass under it. About 6
            // window pixels per snapshot pixel at 100%; blurring a small image is cheap and the
            // upscale smooths.
            const int step = std::max(4, static_cast<int>(std::lround(6.f * m_scale)));
            // Centred where the largest wheel would sit: a smaller wheel moved in from the same edge
            // never moves further than that, so it stays inside the snapshot.
            const int half = static_cast<int>(std::ceil((wheel::kOuterOutDip + kMarginDip) * m_scale));
            const POINT at = ClampCentre(screenPt, (wheel::kOuterOutDip + kEdgeRoomDip) * m_scale);
            m_backdropOrigin = POINT{ at.x - half, at.y - half };
            m_backdropPx = CaptureBlurred(RECT{ at.x - half, at.y - half, at.x + half, at.y + half }, step);
            m_backdropStep = static_cast<float>(step);
        }
        SetMode(Mode::Probe);
        KillTimer(m_hwnd.get(), kTimerMore);
        m_announced = false;
        m_hot = kCenter;
        m_detail = m_why = -1;
        m_whyT = 0.f;
        m_scrollAccum = 0;
        m_files.clear();
        m_content = {};
        m_layout = m_prevLayout = {};
        m_shown.clear();
        m_prevShown.clear();
        m_fits.clear();
        m_prevFits.clear();
        m_picture.reset();
        m_badge = false;
        m_badgeT = 0.f;
        m_keyHot = false;
        m_click = false;
        m_sink = false;
        m_lastHighlight.clear();
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
        m_frames.Stop(m_hwnd.get(), kTimerFrame);
        KillTimer(m_hwnd.get(), kTimerMore);
        if (m_mode != Mode::Hidden)
            ShowWindow(m_hwnd.get(), SW_HIDE);
        SetWindowLongPtrW(m_hwnd.get(), GWL_EXSTYLE, GetWindowLongPtrW(m_hwnd.get(), GWL_EXSTYLE) & ~WS_EX_TRANSPARENT);
        SetMode(Mode::Hidden);
        m_click = false;
        m_sink = false;
        NotifyHighlight();
        NotifyHidden();
    }

    void Overlay::BeginClose()
    {
        KillTimer(m_hwnd.get(), kTimerMore);
        if (m_mode != Mode::Wheel || (!m_motion && !m_sink))
        {
            Hide();
            return;
        }
        // Fade out over a few frames. WS_EX_TRANSPARENT makes the fading window click-through, so a
        // click right after the drop reaches whatever is underneath. Not while it sinks a drag whose
        // slice ran by key: until the drag source has seen the Esc, a release must land here (and do
        // nothing), not in the folder under the wheel.
        if (!m_sink)
            SetWindowLongPtrW(m_hwnd.get(), GWL_EXSTYLE, GetWindowLongPtrW(m_hwnd.get(), GWL_EXSTYLE) | WS_EX_TRANSPARENT);
        SetMode(Mode::Closing);
        m_closeT = m_motion ? 0.f : 1.f;
        NotifyHighlight();
        NotifyHidden();
        Animate();
    }

    void Overlay::Cancel()
    {
        if (m_mode != Mode::Wheel && m_mode != Mode::Probe)
            return;
        m_dropped = false;
        BeginClose();
    }

    void Overlay::SetRings(std::vector<WheelRing> rings)
    {
        if (IsVisible() || rings.empty())
            return;
        m_rings = std::move(rings);
        m_ring = 0;
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
        while (m_scrollAccum <= -WHEEL_DELTA)
        {
            m_scrollAccum += WHEEL_DELTA;
            if (const int r = NextRing(1); r != m_ring)
                SwitchTo(r, 1);
        }
        while (m_scrollAccum >= WHEEL_DELTA)
        {
            m_scrollAccum -= WHEEL_DELTA;
            if (const int r = NextRing(-1); r != m_ring)
                SwitchTo(r, -1);
        }
    }

    // Scrolling (and Up/Down past the last ring) visits every wheel in order and wraps around:
    // Convert, Tools, File actions, Convert... A wheel with nothing for these files still shows, with
    // its slices dimmed and the reason in the centre, so the order never changes under the user.
    int Overlay::NextRing(int step) const
    {
        const int n = static_cast<int>(m_rings.size());
        if (n < 2)
            return m_ring;
        return ((m_ring + step) % n + n) % n;
    }

    bool Overlay::HasActions(int ring) const
    {
        if (m_files.empty() || !m_provider)
            return true;   // probe: the files are not known yet
        const auto content = m_provider(m_files, ring);
        return content.explains || std::any_of(content.slices.begin(), content.slices.end(), [](const WheelSlice& s) { return s.enabled; });
    }

    void Overlay::SwitchTo(int ring, int direction)
    {
        m_ring = ring;
        log::Info(L"overlay: switched to wheel {} ({})", ring, m_rings[static_cast<size_t>(ring)].title);
        if (m_events.ringChanged)
            m_events.ringChanged(ring);
        if (m_mode != Mode::Wheel)
            return;   // probe: DragEnter will ask for this ring's content
        m_prevLayout = m_layout;
        m_prevShown = m_shown;
        m_prevFits = m_fits;
        m_content = m_provider ? m_provider(m_files, m_ring) : WheelContent{};
        Arrange(0);
        m_detail = m_why = -1;
        m_whyT = 0.f;
        m_switchDir = direction;
        m_switchT = m_motion ? 0.f : 1.f;
        // A keyboard user lands on the new wheel's first slice; a mouse user on whatever is under the cursor.
        if (m_keyHot)
        {
            std::vector<bool> selectable(m_shown.size());
            for (size_t i = 0; i < m_shown.size(); ++i)
                selectable[i] = m_shown[i].enabled;
            const int first = keynav::FirstSelectable(m_layout, selectable);
            m_hot = first >= 0 ? first : kCenter;
        }
        else
        {
            m_hot = PointerSlot(m_lastPt);
        }
        if (Selectable(m_hot))
            m_detail = m_hot;
        else if (Explains(m_hot))
            m_why = m_hot;
        KillTimer(m_hwnd.get(), kTimerMore);
        UpdateCentrePlan();
        if (m_a11y)
            m_a11y->SlicesChanged(true);
        Animate();
        NotifyHighlight();
    }

    void Overlay::NextPage()
    {
        if (m_mode != Mode::Wheel || m_layout.pages < 2)
            return;
        m_prevLayout = m_layout;
        m_prevShown = m_shown;
        m_prevFits = m_fits;
        Arrange(m_layout.page + 1);
        m_detail = m_why = -1;
        m_whyT = 0.f;
        m_switchDir = 1;
        m_switchT = m_motion ? 0.f : 1.f;
        // Enter on "More" keeps the highlight on "More", so Enter again turns the next page.
        int more = -1;
        for (int i = 0; i < static_cast<int>(m_shown.size()); ++i)
            if (m_layout.IsMore(i))
                more = i;
        m_hot = m_keyHot ? more : PointerSlot(m_lastPt);
        if (Selectable(m_hot))
            m_detail = m_hot;
        else if (Explains(m_hot))
            m_why = m_hot;
        log::Info(L"overlay: page {} of {}", m_layout.page + 1, m_layout.pages);
        // Still dwelling on "More" during a drag: keep turning.
        if (!m_click && !m_keyHot && m_layout.IsMore(m_hot))
            SetTimer(m_hwnd.get(), kTimerMore, kMoreDwellMs, nullptr);
        if (m_a11y)
            m_a11y->SlicesChanged(true);
        Animate();
        NotifyHighlight();
    }

    // ---- Animation -------------------------------------------------------------------------------

    // Starts the frame timer if it is not running; it stops once everything settles.
    void Overlay::Animate()
    {
        if (!m_hwnd)
            return;
        if (!m_motion)
        {
            m_openT = m_switchT = 1.f;
            for (size_t i = 0; i < m_hotT.size(); ++i)
                m_hotT[i] = static_cast<int>(i) == m_hot && Selectable(m_hot) ? 1.f : 0.f;
            m_whyT = m_hot == m_why && Explains(m_hot) ? 1.f : 0.f;
            m_badgeT = m_badge ? 1.f : 0.f;
            Render();
            return;
        }
        m_frames.Start(m_hwnd.get(), kTimerFrame);
        Render();
    }

    void Overlay::OnFrame()
    {
        const float dt = m_frames.Tick(GetTickCount64());

        if (m_mode == Mode::Wheel || m_mode == Mode::Closing)
            m_openT = std::min(1.f, m_openT + dt / kOpenMs);
        m_switchT = std::min(1.f, m_switchT + dt / kSwitchMs);
        for (size_t i = 0; i < m_hotT.size(); ++i)
            panel::Approach(m_hotT[i], static_cast<int>(i) == m_hot && Selectable(m_hot) ? 1.f : 0.f, dt, kHotTauMs, 0.004f);
        panel::Approach(m_whyT, m_hot == m_why && Explains(m_hot) ? 1.f : 0.f, dt, kHotTauMs, 0.004f);
        panel::Approach(m_away, m_awayTarget, dt, kAwayTauMs, 0.004f);
        panel::Approach(m_badgeT, m_badge ? 1.f : 0.f, dt, kHotTauMs, 0.004f);
        if (m_mode == Mode::Closing)
        {
            m_closeT = std::min(1.f, m_closeT + dt / kCloseMs);
            if (m_closeT >= 1.f)
            {
                if (!m_sink)
                {
                    Hide();
                    return;
                }
                // Faded out, but the drag is still on: an invisible disc keeps catching the drop
                // until OLE says the drag is over (DragLeave or Drop).
                Render();
                m_frames.Stop(m_hwnd.get(), kTimerFrame);
                return;
            }
        }
        Render();
        if (Settled())
            m_frames.Stop(m_hwnd.get(), kTimerFrame);
    }

    bool Overlay::Settled() const
    {
        if (m_mode == Mode::Closing || m_openT < 1.f || m_switchT < 1.f || m_away != m_awayTarget || m_badgeT != (m_badge ? 1.f : 0.f))
            return false;
        for (size_t i = 0; i < m_hotT.size(); ++i)
            if (m_hotT[i] != (static_cast<int>(i) == m_hot && Selectable(m_hot) ? 1.f : 0.f))
                return false;
        return m_whyT == (m_hot == m_why && Explains(m_hot) ? 1.f : 0.f);
    }

    // ---- Hit-testing and drag events -------------------------------------------------------------

    int Overlay::HitTest(POINT pt) const
    {
        const float dx = static_cast<float>(pt.x - m_origin.x) - m_c.x, dy = static_cast<float>(pt.y - m_origin.y) - m_c.y;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d > m_radius + kLiftDip * m_scale)
            return kOutside;
        if (d < m_inner || m_shown.empty())
            return kCenter;
        const float deg = std::atan2(dx, -dy) * 180.f / std::numbers::pi_v<float>;   // 0 at top, clockwise
        return wheel::HitSlot(m_layout, d / m_scale, deg, kLiftDip);
    }

    bool Overlay::Contains(POINT pt) const
    {
        return m_mode == Mode::Wheel && HitTest(pt) != kOutside;
    }

    void Overlay::SetHot(int hot, bool byKey)
    {
        if (byKey)
        {
            m_keyHot = true;
            GetCursorPos(&m_keyAnchor);
        }
        if (hot == m_hot)
            return;
        m_hot = hot;
        if (Selectable(hot))
            m_detail = hot;
        else if (Explains(hot))
            m_why = hot;
        // Holding a drag over "More" turns the page; clicks and keys turn it at once instead.
        if (!byKey && !m_click && m_layout.IsMore(hot))
            SetTimer(m_hwnd.get(), kTimerMore, kMoreDwellMs, nullptr);
        else
            KillTimer(m_hwnd.get(), kTimerMore);
        UpdateCentrePlan();   // the badge says what the centre would (the highlighted slice's detail)
        Animate();
        NotifyHighlight();
    }

    void Overlay::NotifyHighlight()
    {
        if (m_a11y)
            m_a11y->HighlightChanged();
        if (!m_events.highlighted)
            return;
        WheelHighlight h;
        if (m_mode == Mode::Wheel && Selectable(m_hot) && !m_layout.IsMore(m_hot))
            h.actionId = m_shown[static_cast<size_t>(m_hot)].actionId;
        h.files = &m_files;
        h.centre = POINT{ m_origin.x + static_cast<LONG>(std::lround(m_c.x)), m_origin.y + static_cast<LONG>(std::lround(m_c.y)) };
        h.radius = m_tileOut + kLiftDip * m_scale;
        h.monitor = m_monitor;
        h.scale = m_scale;
        // Only changes are passed on (the same slice under a moving cursor is not news).
        const std::wstring key = h.actionId.empty() ? std::wstring() : std::format(L"{}|{}|{}", h.actionId, m_ring, std::lround(h.radius));
        if (key == m_lastHighlight)
            return;
        m_lastHighlight = key;
        m_events.highlighted(h);
    }

    // ---- Screen readers (a11y::WheelView) --------------------------------------------------------

    std::wstring Overlay::ViewTitle() const
    {
        std::wstring title = static_cast<size_t>(m_ring) < m_rings.size() ? m_rings[static_cast<size_t>(m_ring)].title : std::wstring();
        if (m_layout.pages > 1)
            title += std::format(L"{}page {} of {}", title.empty() ? L"" : L", ", m_layout.page + 1, m_layout.pages);
        return title;
    }

    a11y::WheelView::Slice Overlay::ViewSlice(size_t index) const
    {
        Slice out;
        if (index >= m_shown.size())
            return out;
        const auto& s = m_shown[index];
        // The label, then what the centre would show for it; a detail that already starts with the
        // label ("PNG, lossless") stands alone.
        out.name = s.label;
        const auto add = [&out](const std::wstring& part) {
            if (part.empty())
                return;
            if (part.starts_with(out.name))
                out.name = part;
            else
                out.name += L", " + part;
        };
        if (!s.enabled)
            out.name += s.detail.empty() ? std::wstring(L", not available") : L", not available: " + s.detail;
        else
        {
            add(s.headline);
            add(s.detail);
        }
        out.enabled = s.enabled;

        // The tile's extent: its corners and a few points along its arcs, in screen pixels.
        const int slot = static_cast<int>(index);
        const float mid = m_layout.MidDeg(slot), half = m_layout.SpanDeg(slot) / 2.f;
        const auto band = m_layout.BandOf(slot);
        float l = 1e9f, t = 1e9f, r = -1e9f, b = -1e9f;
        for (int k = 0; k <= 4; ++k)
            for (const float radius : { band.inner, band.outer })
            {
                const float rad = (mid - half + half * static_cast<float>(k) / 2.f) * std::numbers::pi_v<float> / 180.f;
                const float x = m_c.x + radius * m_scale * std::sin(rad), y = m_c.y - radius * m_scale * std::cos(rad);
                l = std::min(l, x), r = std::max(r, x), t = std::min(t, y), b = std::max(b, y);
            }
        out.bounds = RECT{ m_origin.x + static_cast<LONG>(std::floor(l)), m_origin.y + static_cast<LONG>(std::floor(t)),
                           m_origin.x + static_cast<LONG>(std::ceil(r)), m_origin.y + static_cast<LONG>(std::ceil(b)) };
        return out;
    }

    int Overlay::ViewHighlighted() const
    {
        return m_mode == Mode::Wheel && m_hot >= 0 && m_hot < static_cast<int>(m_shown.size()) ? m_hot : -1;
    }

    int Overlay::ViewSliceAt(POINT screen) const
    {
        const int slot = m_mode == Mode::Wheel ? HitTest(screen) : kOutside;
        return slot >= 0 && slot < static_cast<int>(m_shown.size()) ? slot : -1;
    }

    // ---- Keyboard and click mode -----------------------------------------------------------------

    void Overlay::Navigate(keynav::Key key)
    {
        if (m_mode != Mode::Wheel)
            return;
        std::vector<bool> selectable(m_shown.size());
        for (size_t i = 0; i < m_shown.size(); ++i)
            selectable[i] = m_shown[i].enabled;
        const auto move = keynav::Navigate(m_layout, selectable, m_hot >= 0 ? m_hot : -1, key);
        if (move.wheel != 0)
        {
            const int ring = NextRing(move.wheel);
            m_keyHot = true;
            GetCursorPos(&m_keyAnchor);
            if (ring != m_ring)
                SwitchTo(ring, move.wheel);
            return;
        }
        if (move.slot >= 0)
            SetHot(move.slot, true);
    }

    void Overlay::Run(int slot)
    {
        if (m_onDrop && Selectable(slot) && !m_layout.IsMore(slot) && !m_files.empty())
        {
            m_dropped = true;
            // Handed over before the wheel starts closing, so whoever listens to `hidden` already
            // knows something ran.
            m_onDrop(m_files, m_shown[static_cast<size_t>(slot)].actionId);
        }
    }

    Overlay::Activation Overlay::Activate()
    {
        if (m_mode != Mode::Wheel || !Selectable(m_hot))
            return Activation::None;
        if (m_layout.IsMore(m_hot))
        {
            m_keyHot = true;
            GetCursorPos(&m_keyAnchor);
            NextPage();
            return Activation::Paged;
        }
        Run(m_hot);
        m_sink = m_inDrag;
        BeginClose();   // fades out with the chosen slice still lit
        return Activation::Ran;
    }

    void Overlay::ShowForFiles(POINT screenPt, int ring, std::vector<std::wstring> files)
    {
        if (!m_hwnd || files.empty() || m_inDrag)
            return;   // a drag owns the wheel
        ShowProbe(screenPt, ring);
        m_click = true;
        Open(files, screenPt);
        // Ready for the mouse at once: the cursor is over the centre, and WM_MOUSEMOVE comes only
        // once it moves.
        OnClickMove(screenPt);
    }

    void Overlay::OnClickMove(POINT pt)
    {
        m_lastPt = pt;
        if (m_keyHot && std::abs(pt.x - m_keyAnchor.x) <= kKeyHoldPx && std::abs(pt.y - m_keyAnchor.y) <= kKeyHoldPx)
            return;
        m_keyHot = false;
        SetHot(PointerSlot(pt));
    }

    void Overlay::OnClickUp(POINT pt)
    {
        const bool kept = m_keyHot && std::abs(pt.x - m_keyAnchor.x) <= kKeyHoldPx && std::abs(pt.y - m_keyAnchor.y) <= kKeyHoldPx;
        const int hot = kept ? m_hot : PointerSlot(pt);
        if (hot >= 0 && m_layout.IsMore(hot))
        {
            m_lastPt = pt;
            NextPage();
            return;
        }
        if (hot >= 0 && !Selectable(hot))
            return;   // a dimmed slice: nothing happens, the wheel stays
        if (hot >= 0)
        {
            m_hot = hot;
            Run(hot);
            BeginClose();
            return;
        }
        Cancel();   // the centre (or outside): close without doing anything
    }

    void Overlay::OnDragPicture(const RECT* aroundCursor)
    {
        m_picture = aroundCursor ? std::optional<RECT>(*aroundCursor) : std::nullopt;
    }

    bool Overlay::OnDragEnter(const std::vector<std::wstring>& files, POINT pt)
    {
        if (m_click)
            return false;   // a click-mode wheel is not a drop target
        m_inDrag = true;
        if (m_mode != Mode::Probe && m_mode != Mode::Wheel)
            return false;
        return Open(files, pt);
    }

    bool Overlay::Open(const std::vector<std::wstring>& files, POINT pt)
    {
        if (files.empty())
        {
            log::Info(L"overlay: drag has no files (text or a URL); hiding");
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
        Arrange(0);
        if (opening)
            m_openT = m_motion ? 0.f : 1.f;
        SetMode(Mode::Wheel);
        m_lastPt = pt;
        m_hot = PointerSlot(pt);
        UpdateCentrePlan();
        if (opening)
            m_badgeT = m_badge ? 1.f : 0.f;   // it opens with the count where it can be seen
        if (!m_announced && m_events.shown)
            m_events.shown(m_ring, m_monitor);
        m_announced = true;
        if (m_a11y)
            m_a11y->SlicesChanged(opening);
        Animate();
        NotifyHighlight();
        log::Info(L"overlay: {} with {} file(s), first: {}", m_click ? L"opened on a selection" : L"DragEnter", files.size(), files.front());
        return true;
    }

    DWORD Overlay::OnDragOver(POINT pt, DWORD allowed)
    {
        if (m_mode != Mode::Wheel)
            return DROPEFFECT_NONE;
        m_lastPt = pt;
        // OLE calls DragOver repeatedly even when the mouse is still: a slice picked by key keeps the
        // highlight until the cursor really moves.
        int hot = m_hot;
        if (!m_keyHot || std::abs(pt.x - m_keyAnchor.x) > kKeyHoldPx || std::abs(pt.y - m_keyAnchor.y) > kKeyHoldPx)
        {
            m_keyHot = false;
            hot = PointerSlot(pt);
            SetHot(hot);
        }
        UpdateCentrePlan();   // the badge follows the picture
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
        if (hot == kCenter || Selectable(hot))
            return SafeEffect(allowed);
        return DROPEFFECT_NONE;
#endif
    }

    // A drag whose slice already ran by key has ended (the Esc reached the source, or it was dropped
    // anyway): the rest of the fade can be click-through.
    bool Overlay::EndSink()
    {
        if (!m_sink)
            return false;
        m_sink = false;
        if (m_mode != Mode::Closing || m_closeT >= 1.f)
        {
            Hide();
            return true;
        }
        SetWindowLongPtrW(m_hwnd.get(), GWL_EXSTYLE, GetWindowLongPtrW(m_hwnd.get(), GWL_EXSTYLE) | WS_EX_TRANSPARENT);
        Animate();
        return true;
    }

    void Overlay::OnDragLeave()
    {
        m_inDrag = false;
        if (EndSink())
            return;
        if (m_mode == Mode::Closing)
            return;   // already fading out
        if (IsVisible())
            log::Info(L"overlay: DragLeave (cursor left the wheel, Esc, or released on the centre); hiding");
        BeginClose();
    }

    void Overlay::OnDrop(const std::vector<std::wstring>& files, POINT pt)
    {
        m_inDrag = false;
        if (EndSink())
            return;   // the slice already ran by key: this drop only ends the drag
        m_dropped = true;
        // A slice picked by key (cursor not moved since) is the one dropped on.
        const int hot = m_keyHot ? m_hot : PointerSlot(pt);
        // The drop is handed over before the wheel starts closing, so whoever listens to `hidden`
        // already knows something was dropped.
        if (m_onDrop && !files.empty())
        {
#if SHIFTDROP_M0
            (void)hot;
            m_onDrop(files, L"");
#else
            if (Selectable(hot) && !m_layout.IsMore(hot))
            {
                m_hot = hot;
                m_onDrop(files, m_shown[static_cast<size_t>(hot)].actionId);
            }
#endif
        }
        BeginClose();   // fades out with the chosen slice still lit
    }

    // ---- Drawing resources -----------------------------------------------------------------------

    bool Overlay::EnsureTextFormats()
    {
        if (m_labelFont && m_fontScale == m_scale)
            return true;
        m_fontScale = m_scale;
        m_sizedFonts.clear();
        // Centred: every label sits in the middle of its tile or the wheel's centre.
        constexpr auto C = DWRITE_TEXT_ALIGNMENT_CENTER;
        m_labelFont = MakeFormat(m_textFamily, wheel::kIconLabelDip, DWRITE_FONT_WEIGHT_SEMI_BOLD, C);
        m_formatFont = MakeFormat(m_textFamily, wheel::kNameLabelDip, DWRITE_FONT_WEIGHT_BOLD, C);
        m_iconFont = MakeFormat(m_iconFamily, wheel::kIconFontDip, DWRITE_FONT_WEIGHT_NORMAL, C);
        m_countFont = MakeFormat(m_textFamily, kCountDip, DWRITE_FONT_WEIGHT_BOLD, C);
        m_ringFont = MakeFormat(m_textFamily, 10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, C);
        m_detailFont = MakeFormat(m_textFamily, 11.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, C);
        if (m_detailFont)
            m_detailFont->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        return m_labelFont && m_formatFont && m_iconFont && m_countFont && m_ringFont && m_detailFont;
    }

    IDWriteTextFormat* Overlay::SizedFormat(DWRITE_FONT_WEIGHT weight, float dip, bool wrap, bool icon)
    {
        const int key = (static_cast<int>(weight) << 16) | (wrap ? 1 << 15 : 0) | (icon ? 1 << 14 : 0) | static_cast<int>(std::lround(dip * 4.f));
        auto& format = m_sizedFonts[key];
        if (!format && (format = MakeFormat(icon ? m_iconFamily : m_textFamily, dip, weight, DWRITE_TEXT_ALIGNMENT_CENTER)))
        {
            // Labels are fitted before they are drawn (wheel::FitLabels): nothing is trimmed to "...".
            DWRITE_TRIMMING none{ DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0 };
            format->SetTrimming(&none, nullptr);
            if (wrap)
                format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        }
        return format.Get();
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

    void Overlay::DrawSlices(const std::vector<WheelSlice>& slices, const std::vector<wheel::FittedLabel>& fits, const wheel::Layout& layout,
                             const std::vector<float>& hotT, float alpha, float rotation)
    {
        const size_t n = std::min({ slices.size(), fits.size(), layout.slots.size() });
        if (n == 0 || alpha <= 0.f)
            return;
        D2D1_MATRIX_3X2_F base{};
        m_rt->GetTransform(&base);
        const auto turned = D2D1::Matrix3x2F::Rotation(rotation, m_c) * base;

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
            const auto& slice = slices[i];
            const auto& slot = layout.slots[i];
            const int si = static_cast<int>(i);
            const float t = i < hotT.size() && slice.enabled ? hotT[i] : 0.f;
            const float e = EaseOutCubic(t);
            const float mid = layout.MidDeg(si);
            const auto band = layout.BandOf(si);
            const float tileIn = std::round(band.inner * s), tileOut = std::round(band.outer * s);
            // The hovered tile lifts: it grows outward and moves out along its centre line (less on
            // the inner of two rings, which would otherwise touch the outer one).
            const bool innerOfTwo = layout.rings == 2 && slot.ring == 0;
            const float push = (innerOfTwo ? kInnerPushDip : kPushDip) * s * e;
            const float rad = mid * std::numbers::pi_v<float> / 180.f;
            m_rt->SetTransform(D2D1::Matrix3x2F::Translation(push * std::sin(rad), -push * std::cos(rad)) * turned);

            const float outer = tileOut + (innerOfTwo ? kInnerLiftDip : kLiftDip) * s * e;
            auto tile = TileGeometry(static_cast<size_t>(slot.count), static_cast<size_t>(slot.index), tileIn, outer);
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
                    SUCCEEDED(m_rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(Polar(outer, mid), Polar(tileIn, mid)),
                                                              collection.Get(), &fill)))
                {
                    fill->SetOpacity(t * alpha);
                    m_rt->FillGeometry(tile.Get(), fill.Get());
                }
            }
            m_rt->DrawGeometry(tile.Get(), Brush(m_palette.tileLine, alpha * (1.f - t)).Get(), 1.f);

            // Icon above the label, or a format name alone in bold, where wheel::FitLabels found room
            // for it in this tile: on one line or two, as large as fits, clear of the tile's edges.
            const auto& fit = fits[i];
            const auto at = Polar(fit.radius * s + 2.f * s * e, mid);
            const auto colour = slice.enabled ? Mix(m_palette.text, m_palette.textOnAccent, t) : m_palette.textDim;
            if (slice.icon)
            {
                const float iy = at.y + fit.iconDy * s, ih = 0.75f * fit.iconSize * s;
                const auto box = D2D1::RectF(at.x - ih, iy - ih, at.x + ih, iy + ih);
                D2D1_MATRIX_3X2_F current{};
                m_rt->GetTransform(&current);
                if (slice.mirrorIcon)
                    m_rt->SetTransform(D2D1::Matrix3x2F::Scale(-1.f, 1.f, D2D1::Point2F(at.x, iy)) * current);
                const wchar_t glyph[2]{ slice.icon, 0 };
                Text(glyph, fit.iconSize < wheel::kIconFontDip ? SizedFormat(DWRITE_FONT_WEIGHT_NORMAL, fit.iconSize, false, true) : m_iconFont.Get(), box,
                     colour, alpha);
                m_rt->SetTransform(current);
            }
            auto* font = SizedFormat(slice.icon ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_BOLD, fit.size);
            // The lines were measured to fit; the wide box only keeps DirectWrite from wrapping them.
            const float half = tileOut, lineHalf = 0.75f * fit.size * s;
            for (size_t k = 0; k < fit.lines.size() && k < fit.lineDy.size(); ++k)
            {
                const float ly = at.y + fit.lineDy[k] * s;
                Text(fit.lines[k], font, D2D1::RectF(at.x - half, ly - lineHalf, at.x + half, ly + lineHalf), colour, alpha);
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
        float detailT = 0.f;
        if (m_detail >= 0 && static_cast<size_t>(m_detail) < m_hotT.size())
            detailT = m_hotT[static_cast<size_t>(m_detail)];

        // With the drag picture over the middle, the text is in the badge beside it instead (DrawBadge).
        const float shown = 1.f - m_badgeT;

        // The count (or a size), crossfading to the hovered slice's headline when it has one.
        const auto upper = D2D1::RectF(m_c.x - w / 2, m_c.y - 22.f * s, m_c.x + w / 2, m_c.y + 2.f * s);
        const std::wstring* headline = nullptr;
        if (m_detail >= 0 && static_cast<size_t>(m_detail) < m_shown.size() && !m_shown[static_cast<size_t>(m_detail)].headline.empty())
            headline = &m_shown[static_cast<size_t>(m_detail)].headline;
        DrawCentreLine(m_content.centerText, upper, (headline ? 1.f - detailT : 1.f) * shown);
        if (headline && detailT > 0.f)
            DrawCentreLine(*headline, upper, detailT * shown);

        // Below the count: the wheel's name, crossfading to the hovered slice's detail.
        const auto lower = D2D1::RectF(m_c.x - w / 2, m_c.y + 2.f * s, m_c.x + w / 2, m_c.y + 20.f * s);
        if (!m_rings.empty())
        {
            std::wstring title = m_rings[static_cast<size_t>(m_ring)].title;
            for (auto& ch : title)
                ch = static_cast<wchar_t>(towupper(ch));
            Text(title, m_ringFont.Get(), lower, m_palette.text, 0.62f * (1.f - std::max(detailT, m_whyT)) * shown);
        }
        // A dimmed slice says why it can't be used ("Needs HEIF Image Extensions").
        if (m_whyT > 0.f && m_why >= 0 && static_cast<size_t>(m_why) < m_shown.size())
            Text(m_shown[static_cast<size_t>(m_why)].detail, m_detailFont.Get(),
                 D2D1::RectF(lower.left, lower.top - 1.f * s, lower.right, lower.bottom + 12.f * s), m_palette.text, 0.85f * m_whyT * shown);
        if (detailT > 0.f && m_detail >= 0 && static_cast<size_t>(m_detail) < m_shown.size())
        {
            const auto& slice = m_shown[static_cast<size_t>(m_detail)];
            Text(slice.detail.empty() ? slice.label : slice.detail, m_detailFont.Get(),
                 D2D1::RectF(lower.left, lower.top - 1.f * s, lower.right, lower.bottom + 12.f * s), m_palette.text, 0.85f * detailT * shown);
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
                                  Brush(on ? m_palette.accentMark : m_palette.dot).Get());
            }
        }
        DrawBadge();
    }

    // The count line: at full size if it fits the disc, smaller if it is long, and on two lines (growing
    // upward) at the smallest size if even that is too wide. Never trimmed.
    void Overlay::DrawCentreLine(const std::wstring& text, const D2D1_RECT_F& line, float alpha)
    {
        if (text.empty() || alpha <= 0.f)
            return;
        const float s = m_scale;
        const float room = (line.right - line.left) / s;
        const float full = TextWidth(text, m_countFont.Get()) / s;
        const float size = wheel::ShrinkToFit(full, kCountDip, kCountMinDip, room);
        if (full * size / kCountDip <= room)
        {
            Text(text, SizedFormat(DWRITE_FONT_WEIGHT_BOLD, size), line, m_palette.text, alpha);
            return;
        }
        const float extra = kCountMinDip * 1.3f * s;
        Text(text, SizedFormat(DWRITE_FONT_WEIGHT_BOLD, kCountMinDip, true), D2D1::RectF(line.left, line.top - extra, line.right, line.bottom), m_palette.text,
             alpha);
    }

    // ---- The badge: the count beside the drag picture ----------------------------------------------

    std::optional<wheel::Rect> Overlay::PictureRect() const
    {
        if (!m_picture || !m_inDrag || m_click)
            return std::nullopt;
        const float s = m_scale;
        const float cx = static_cast<float>(m_lastPt.x - m_origin.x) - m_c.x, cy = static_cast<float>(m_lastPt.y - m_origin.y) - m_c.y;
        // Only while the cursor is on the centre disc. Over a slice the picture is on that slice, and a
        // badge following it there would cover the labels around it.
        if (std::hypot(cx, cy) > m_inner)
            return std::nullopt;
        return wheel::Rect{ (cx + static_cast<float>(m_picture->left)) / s, (cy + static_cast<float>(m_picture->top)) / s,
                            (cx + static_cast<float>(m_picture->right)) / s, (cy + static_cast<float>(m_picture->bottom)) / s };
    }

    std::wstring Overlay::BadgeDetail() const
    {
        if (Selectable(m_hot) && static_cast<size_t>(m_hot) < m_shown.size())
        {
            const auto& slice = m_shown[static_cast<size_t>(m_hot)];
            return slice.detail.empty() ? slice.label : slice.detail;
        }
        if (Explains(m_hot))
            return m_shown[static_cast<size_t>(m_hot)].detail;
        return {};
    }

    D2D1_SIZE_F Overlay::BadgeSize()
    {
        const float s = m_scale;
        auto* font = SizedFormat(DWRITE_FONT_WEIGHT_BOLD, kBadgeFontDip);
        float w = TextWidth(m_content.centerText, font) / s;
        if (Selectable(m_hot) && !m_shown[static_cast<size_t>(m_hot)].headline.empty())
            w = std::max(w, TextWidth(m_shown[static_cast<size_t>(m_hot)].headline, font) / s);
        const float inner = kBadgeMaxDip - 2.f * kBadgePadDip;
        w = std::min(w, inner);
        float h = kBadgeLineDip;
        if (const auto detail = BadgeDetail(); !detail.empty())
        {
            const auto size = LayoutSize(m_dwrite.Get(), detail, m_detailFont.Get(), inner * s);
            w = std::max(w, std::min(inner, size.width / s));
            h += size.height / s;
        }
        return D2D1::SizeF(w + 2.f * kBadgePadDip, h + 2.f * kBadgeVPadDip);
    }

    void Overlay::UpdateCentrePlan()
    {
        if (m_mode != Mode::Wheel)
            return;
        const float s = m_scale;
        // The count line and the name or detail under it, as DrawCentre lays them out, DIPs from the centre.
        const float half = m_inner / s - 7.f;
        const wheel::Rect text{ -half, -22.f, half, 20.f };
        const auto picture = PictureRect();
        const auto size = picture ? BadgeSize() : D2D1::SizeF(0.f, 0.f);   // no measuring on every move without one
        const auto plan = wheel::PlanCentre(text, picture, size.width, size.height, m_badge);
        const auto at = D2D1::Point2F(m_c.x + plan.x * s, m_c.y + plan.y * s);
        const bool moved = plan.badge && (std::abs(at.x - m_badgeAt.x) >= 0.5f || std::abs(at.y - m_badgeAt.y) >= 0.5f);
        if (plan.badge)
            m_badgeAt = at;
        if (plan.badge != m_badge || moved)
        {
            m_badge = plan.badge;
            Animate();
        }
    }

    void Overlay::DrawBadge()
    {
        if (m_badgeT <= 0.f)
            return;
        const float s = m_scale, a = m_badgeT;
        const auto size = BadgeSize();
        const float w = size.width * s, h = size.height * s;
        const auto box = D2D1::RectF(m_badgeAt.x - w / 2.f, m_badgeAt.y - h / 2.f, m_badgeAt.x + w / 2.f, m_badgeAt.y + h / 2.f);
        const float corner = std::min(h / 2.f, 10.f * s);
        const auto rounded = D2D1::RoundedRect(box, corner, corner);
        m_rt->FillRoundedRectangle(rounded, Brush(m_palette.centre, a).Get());
        m_rt->DrawRoundedRectangle(rounded, Brush(m_palette.centreLine, a).Get(), 1.f);

        float detailT = 0.f;
        if (m_detail >= 0 && static_cast<size_t>(m_detail) < m_hotT.size())
            detailT = m_hotT[static_cast<size_t>(m_detail)];
        const std::wstring* headline = nullptr;
        if (m_detail >= 0 && static_cast<size_t>(m_detail) < m_shown.size() && !m_shown[static_cast<size_t>(m_detail)].headline.empty())
            headline = &m_shown[static_cast<size_t>(m_detail)].headline;
        const float top = box.top + kBadgeVPadDip * s;
        const auto line = D2D1::RectF(box.left, top, box.right, top + kBadgeLineDip * s);
        auto* font = SizedFormat(DWRITE_FONT_WEIGHT_BOLD, kBadgeFontDip);
        Text(m_content.centerText, font, line, m_palette.text, a * (headline ? 1.f - detailT : 1.f));
        if (headline && detailT > 0.f)
            Text(*headline, font, line, m_palette.text, a * detailT);
        if (const auto detail = BadgeDetail(); !detail.empty())
            Text(detail, m_detailFont.Get(), D2D1::RectF(box.left + kBadgePadDip * s, line.bottom, box.right - kBadgePadDip * s, box.bottom - kBadgeVPadDip * s),
                 m_palette.text, 0.85f * a * std::max(detailT, m_whyT));
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
                    // The snapshot covers the largest wheel around where it opened; the window may be smaller.
                    glass->SetTransform(D2D1::Matrix3x2F::Scale(m_backdropStep, m_backdropStep) *
                                        D2D1::Matrix3x2F::Translation(static_cast<float>(m_backdropOrigin.x - m_origin.x),
                                                                      static_cast<float>(m_backdropOrigin.y - m_origin.y)));
                    m_rt->FillEllipse(disc, glass.Get());
                }
            }
            m_rt->FillEllipse(disc, Brush(m_palette.glassTint, m_backdrop ? 1.f : 1.3f).Get());
            m_rt->DrawEllipse(D2D1::Ellipse(m_c, r - 0.5f, r - 0.5f), Brush(m_palette.discLine).Get(), 1.f);
        }

#if SHIFTDROP_M0
        // Placeholder build: one plain ring and the product name; any drop on it is logged.
        if (auto ring = TileGeometry(1, 0, m_tileIn, m_tileOut))
            m_rt->FillGeometry(ring.Get(), Brush(m_palette.tile).Get());
        DrawCentre();
        Text(SD_NAME, m_labelFont.Get(), D2D1::RectF(m_c.x - m_tileOut, m_c.y - m_tileOut, m_c.x + m_tileOut, m_c.y - m_tileIn), m_palette.text);
#else
        if (m_shown.empty())
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
            DrawSlices(m_prevShown, m_prevFits, m_prevLayout, {}, 1.f - e, -turn * e);
            DrawSlices(m_shown, m_fits, m_layout, m_hotT, e, turn * (1.f - e));
        }
        else
        {
            DrawSlices(m_shown, m_fits, m_layout, m_hotT, 1.f, 0.f);
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
        else if (m_mode == Mode::Closing && m_sink && m_closeT >= 1.f)
        {
            // Faded out while still sinking a drag (see OnFrame): the probe's invisible disc.
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
        else if (m_mode == Mode::Closing && m_sink && m_closeT >= 1.f)
            opacity = 1.f;
        else if (m_mode == Mode::Closing)
            // While sinking a drag never quite 0, which would let the drop through the window.
            opacity = std::max(m_sink ? 0.05f : 0.f, (1.f - kAwayFade * m_away) * (1.f - EaseOutCubic(m_closeT)));
        m_canvas->End(m_hwnd.get(), m_origin, opacity);
        m_rt = nullptr;
    }
}
