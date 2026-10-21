#include "Brand.h"
#include "Island.h"
#include "Log.h"
#include "PanelLayout.h"

#include <algorithm>
#include <cmath>
#include <format>

#include <windowsx.h>

using Microsoft::WRL::ComPtr;
using namespace sd::theme;

namespace sd
{
    namespace
    {
        // Numbers as far as there are sections ("Shift+1-4"), for anyone without a mouse wheel.
        std::wstring SwitchHint(size_t sections)
        {
            return sections > 1 ? std::format(L"Scroll or Shift+1\u2013{}", sections) : std::wstring(L"Scroll to switch");
        }

        constexpr wchar_t kClassName[] = L"" SD_NAME L".Island";

        // DIPs, times the panel's scale (panel::Scale: 1.4x the wheel's, times the Top panel size).
        // The panel's outer sizes and the switcher's are in PanelLayout.h.
        using panel::kInsetDip;
        using panel::kMaxHeightDip;
        using panel::kPadDip;
        using panel::kTopGapDip;

        constexpr float kPresenceMs = 520.f;    // spring down; settled well before the end
        constexpr float kRetractMs = 220.f;
        constexpr float kSizeTauMs = 60.f;
        constexpr float kContentMs = 180.f;
        constexpr float kTabTauMs = 55.f;
        constexpr float kBarTauMs = 120.f;
        constexpr ULONGLONG kSuccessMs = 2000;   // a success tucks away after this long
        constexpr ULONGLONG kFailureMs = 12000;  // a failure stays longer (or until clicked)
        constexpr UINT_PTR kTimerFrame = 1;

        constexpr wchar_t kGlyphCheck = 0xE73E;     // CheckMark
        constexpr wchar_t kGlyphWarning = 0xE7BA;   // Warning
        constexpr wchar_t kGlyphError = 0xE783;     // Error (a circled !)
        constexpr wchar_t kGlyphClose = 0xE8BB;     // ChromeClose (the cancel ×)

        // The pills' insides, as the brand kit's App UI page draws them (DIPs; the content box is
        // kInsetDip inside the pill). The pill is 48 high for progress, 44 for a result.
        constexpr float kProgressHeightDip = 48.f;
        constexpr float kResultHeightDip = 44.f;
        constexpr float kLeadDip = 9.f;             // content box to the spinner or the status icon
        constexpr float kMarkDip = 17.f;            // the spinner and the status icon
        constexpr float kMarkGapDip = 9.f;          // after them
        constexpr float kTailDip = 4.f;             // the last text to the content box's end
        constexpr float kCancelDip = 26.f;          // the × button's diameter
        constexpr float kButtonEndDip = 2.7f;       // the last round button to the content box's end
        constexpr float kBarDip = 2.3f;             // the progress bar, along the pill's bottom edge
        constexpr float kBarBottomDip = 3.f;
        constexpr float kFailLineDip = 1.5f;        // a failure's red outline

        // Carry (a result with output files): a handle to drag the file out, then Float and Copy.
        constexpr ULONGLONG kCarryMs = 5000;        // a success that can be carried stays a little longer
        constexpr float kChipLeadDip = 2.7f;
        constexpr float kChipWDip = 36.f, kChipHDip = 28.f;
        constexpr float kChipGapDip = 7.6f;
        constexpr float kActionDip = 26.f;          // Float and Copy buttons
        constexpr float kActionGapDip = 7.6f;
        constexpr wchar_t kGlyphPin = 0xE718;       // Pin (Float shelf)
        constexpr wchar_t kGlyphCopy = 0xE8C8;      // Copy
        constexpr const wchar_t* kHintShow = L"Click to show in folder";
        constexpr const wchar_t* kHintDismiss = L"Click to dismiss";
        constexpr const wchar_t* kHintActions[2] = { L"Keep on the Float shelf", L"Copy to the clipboard" };
    }

    Island::Island(HINSTANCE instance, std::vector<WheelRing> rings, std::function<void()> onClick)
        : m_instance(instance), m_rings(std::move(rings)), m_onClick(std::move(onClick))
    {
    }

    Island::~Island() = default;

    bool Island::Create()
    {
        m_hwnd.reset(CreatePanel(m_instance, kClassName, WndProc, WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
                                 SD_NAME, this, IDC_HAND));
        if (!m_hwnd || !InitGraphics())
        {
            log::Error(L"island: could not create the top panel");
            return false;
        }
        m_a11y = std::make_unique<a11y::TextElement>(m_hwnd.get());
        return true;
    }

    LRESULT CALLBACK Island::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_NCCREATE)
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        auto* self = reinterpret_cast<Island*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg)
        {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return HTCLIENT;   // transparent pixels around the panel are already click-through
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_MOUSEMOVE:
        case WM_MOUSELEAVE:
            if (self && self->m_state == State::Result && !self->m_retracting)
                self->OnResultMouse(msg, POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
            else if (self && msg != WM_LBUTTONDOWN)
                self->OnMouse(msg, POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
            return 0;
        case WM_CAPTURECHANGED:
            if (self)
                self->m_pressed = false;
            return 0;
        case WM_SETCURSOR:
            // A hand over the result (it opens Explorer) and over the ×; an arrow on the rest of the progress pill.
            if (self && self->m_state == State::Progress && !self->m_cancelHover)
            {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_TIMER:
            if (self && wParam == kTimerFrame)
                self->OnFrame();
            return 0;
        case WM_GETOBJECT:
            if (LRESULT result = 0; self && self->m_a11y && self->m_a11y->OnGetObject(wParam, lParam, result))
                return result;
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    // ---- State changes ---------------------------------------------------------------------------

    void Island::Layout(HMONITOR monitor)
    {
        m_monitor = monitor;
        // The tray's Size setting and the monitor's DPI and height, as for the wheel, then larger,
        // then the Top panel size: as much of it as fits this monitor (never cut below 100%).
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(monitor, &mi);
        const float wheel = ScaleFor(monitor, m_appearance.size);
        m_scale = panel::Scale(wheel, m_panelSize);
        EnsureTextFormats();
        std::vector<float> names;
        for (const auto& ring : m_rings)
            names.push_back(TextWidth(ring.title, m_tabFont.Get()) / m_scale);
        const float fit = panel::FitScale(m_scale, panel::Scale(wheel), names, static_cast<float>(mi.rcWork.right - mi.rcWork.left),
                                          static_cast<float>(mi.rcWork.bottom - mi.rcWork.top));
        if (fit != m_scale)
        {
            if (fit != m_fittedScale)
                log::Info(L"island: top panel size {}% does not fit this screen; drawn at {}%", panel::SizePercent(m_panelSize),
                          static_cast<int>(std::lround(100.f * fit / panel::Scale(wheel))));
            m_scale = fit;
            EnsureTextFormats();
        }
        m_fittedScale = fit;
        m_pad = std::round(kPadDip * m_scale);
        // Never wider than the screen (a 1366 px laptop at Large): the switcher fits itself into this.
        m_maxContent = std::floor(panel::MaxContentWidth(m_scale, static_cast<float>(mi.rcWork.right - mi.rcWork.left)));
        const float w = m_maxContent + 2.f * m_pad;
        const float h = std::round(kMaxHeightDip * m_scale) + 2.f * m_pad;
        m_size = SIZE{ static_cast<LONG>(w), static_cast<LONG>(h) };
        const LONG cx = (mi.rcWork.left + mi.rcWork.right) / 2;
        m_origin = POINT{ cx - m_size.cx / 2, mi.rcWork.top + static_cast<LONG>(std::round(kTopGapDip * m_scale) - m_pad) };
    }

    void Island::SetClickThrough(bool through)
    {
        const LONG_PTR ex = GetWindowLongPtrW(m_hwnd.get(), GWL_EXSTYLE);
        SetWindowLongPtrW(m_hwnd.get(), GWL_EXSTYLE, through ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT));
    }

    void Island::Present(State state, HMONITOR monitor)
    {
        if (!m_hwnd)
            return;
        const bool visible = m_state != State::Hidden;
        const bool appearing = !visible || m_retracting;
        if (appearing || monitor != m_monitor)
        {
            Layout(monitor);
            m_palette = Current(m_appearance.style);
            m_motion = AnimationsOn();
            const RECT area{ m_origin.x, m_origin.y, m_origin.x + m_size.cx, m_origin.y + m_size.cy };
            if (m_appearance.style == Style::Glass && (!visible || !EqualRect(&area, &m_backdropRect)))
            {
                // Snapshot before our window covers the area.
                if (visible)
                    ShowWindow(m_hwnd.get(), SW_HIDE);
                const int step = std::max(4, static_cast<int>(std::lround(6.f * ScaleFor(monitor, m_appearance.size))));   // as blurred as the wheel's
                m_backdropPx = CaptureBlurred(area, step);
                m_backdropStep = static_cast<float>(step);
                m_backdropRect = area;
                m_backdrop.Reset();
            }
            if (m_retracting)
            {
                m_presenceT = 0.15f;   // caught while tucking away: drop back down from where it is
            }
            else if (appearing)
            {
                m_presence = 0.f;
                m_presenceT = 0.f;
                m_w = 0.f;
                m_h = 0.f;
            }
        }
        m_retracting = false;
        m_state = state;
        m_contentT = 0.f;
        if (m_w == 0.f)
        {
            // Start as a small pill and grow into the content's size while it drops down.
            m_w = ContentHeight() * 2.f;
            m_h = ContentHeight();
        }
        if (!m_motion)
        {
            m_presence = m_presenceT = m_contentT = 1.f;
            m_w = ContentWidth();
            m_h = ContentHeight();
        }
        // Clickable only when there is something to click: a result, or a job's ×.
        SetClickThrough(!(state == State::Result || (state == State::Progress && m_onCancel)));
        Render();
        SetWindowPos(m_hwnd.get(), HWND_TOPMOST, m_origin.x, m_origin.y, m_size.cx, m_size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        Animate();
    }

    void Island::Retract()
    {
        if (m_state == State::Hidden || m_retracting)
            return;
        SetClickThrough(true);
        if (!m_motion)
        {
            ShowWindow(m_hwnd.get(), SW_HIDE);
            m_state = State::Hidden;
            return;
        }
        m_retracting = true;
        m_presenceT = 0.f;
        Animate();
    }

    void Island::ShowSwitcher(int ring, HMONITOR monitor)
    {
        if (m_rings.empty())
            return;   // the placeholder build has a single unnamed wheel
        m_ring = ring;
        if (m_state != State::Switcher || m_retracting)
            m_tabPos = static_cast<float>(ring);
        // A finished job's result gives way to the new gesture; a job still running keeps its progress.
        if (m_state == State::Progress && !m_retracting)
            return;
        Present(State::Switcher, monitor);
    }

    void Island::SetRings(std::vector<WheelRing> rings)
    {
        m_rings = std::move(rings);
        m_ring = std::clamp(m_ring, 0, std::max(0, static_cast<int>(m_rings.size()) - 1));
        m_tabPos = static_cast<float>(m_ring);
        if (m_state == State::Switcher)
            Animate();
    }

    void Island::SetRing(int ring)
    {
        m_ring = ring;
        if (m_state == State::Switcher)
            Animate();
    }

    void Island::HideSwitcher()
    {
        if (m_state == State::Switcher)
            Retract();
    }

    void Island::ShowProgress(const std::wstring& title, size_t total, std::function<void()> onCancel)
    {
        m_title = title;
        m_total = total;
        m_done = 0.0;
        m_fractional = false;
        m_onCancel = std::move(onCancel);
        m_cancelHover = false;
        m_cancelled = false;
        m_bar = 0.f;
        if (m_a11y)
            m_a11y->Announce(title, L"ShiftDrop.Progress");
        POINT pt{};
        GetCursorPos(&pt);
        Present(State::Progress, m_monitor ? m_monitor : MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST));
    }

    void Island::SetProgress(double done)
    {
        m_done = std::clamp(done, 0.0, static_cast<double>(m_total));
        m_fractional |= done != std::floor(done);
        if (m_state == State::Progress)
            Animate();
    }

    void Island::Pump()
    {
        // Only this window's WM_TIMER: nothing else gets to run (and re-enter) in the meantime.
        MSG m{};
        while (m_hwnd && PeekMessageW(&m, m_hwnd.get(), WM_TIMER, WM_TIMER, PM_REMOVE))
            DispatchMessageW(&m);
    }

    void Island::OnMouse(UINT msg, POINT pt)
    {
        if (m_state != State::Progress || !m_onCancel || m_cancelled)
            return;
        const auto& r = m_cancelRect;
        const bool over = msg != WM_MOUSELEAVE && pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom;
        if (msg == WM_MOUSELEAVE)
            m_tracking = false;
        else if (!m_tracking)
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, m_hwnd.get(), 0 };
            m_tracking = TrackMouseEvent(&tme) != FALSE;
        }
        if (msg == WM_LBUTTONUP && over)
        {
            // Stop the job; its result (what was finished before) replaces this panel when it arrives.
            m_cancelled = true;
            m_cancelHover = false;
            auto cancel = std::move(m_onCancel);
            m_onCancel = nullptr;
            SetClickThrough(true);
            log::Info(L"island: cancel clicked");
            cancel();
            Animate();
            return;
        }
        if (over != m_cancelHover)
        {
            m_cancelHover = over;
            Animate();
        }
    }

    // ---- Carry: drag the result out, Float, Copy -----------------------------------------------

    size_t Island::ActionCount() const
    {
        return m_actions.toShelf || m_actions.copy ? 2 : 0;   // the pair is laid out together
    }

    int Island::ActionAt(POINT pt) const
    {
        const std::function<std::wstring()>* fns[2] = { &m_actions.toShelf, &m_actions.copy };
        for (int i = 0; i < static_cast<int>(ActionCount()); ++i)
        {
            const auto& r = m_actionRects[i];
            if (*fns[i] && pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom)
                return i;
        }
        return -1;
    }

    std::wstring Island::DragHint() const
    {
        return std::format(L"{} \u00b7 drag the {} out", kHintShow, m_actions.files > 1 ? L"files" : L"file");
    }

    std::wstring Island::ResultLine() const
    {
        if (m_actionHover >= 0 && !m_actionDone[m_actionHover])
            return kHintActions[m_actionHover];
        if (m_resultHover && m_outcome != Outcome::Failure && !m_actionDone[0] && !m_actionDone[1])
            return m_actions.drag ? DragHint() : std::wstring(kHintShow);
        return m_subtitle;
    }

    bool Island::TwoLines() const
    {
        return !m_subtitle.empty() || m_actions.Any();
    }

    void Island::OnResultMouse(UINT msg, POINT pt)
    {
        if (msg == WM_MOUSELEAVE)
        {
            m_tracking = false;
            m_resultHover = false;
            m_actionHover = -1;
            Animate();
            return;
        }
        if (!m_tracking)
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, m_hwnd.get(), 0 };
            m_tracking = TrackMouseEvent(&tme) != FALSE;
        }
        const int over = ActionAt(pt);
        if (!m_resultHover || over != m_actionHover)
        {
            m_resultHover = true;
            m_actionHover = over;
            Animate();
        }

        switch (msg)
        {
        case WM_LBUTTONDOWN:
            m_pressed = true;
            m_pressPt = pt;
            m_pressAction = over;
            SetCapture(m_hwnd.get());
            break;
        case WM_MOUSEMOVE:
            // Past the system drag distance on the chip itself: carry the outputs into another app.
            if (m_pressed && m_pressAction < 0 && m_actions.drag &&
                (std::abs(pt.x - m_pressPt.x) > GetSystemMetrics(SM_CXDRAG) || std::abs(pt.y - m_pressPt.y) > GetSystemMetrics(SM_CYDRAG)))
            {
                m_pressed = false;
                ReleaseCapture();
                m_carrying = true;
                const auto drag = m_actions.drag;   // a copy: the callback may show a new result
                drag();
                m_carrying = false;
                Retract();
            }
            break;
        case WM_LBUTTONUP:
        {
            const bool pressed = m_pressed;
            m_pressed = false;
            ReleaseCapture();
            if (!pressed)
                break;
            if (m_pressAction >= 0)
            {
                // A button: run it if the mouse is still on it, and say what happened.
                if (over == m_pressAction && !m_actionDone[over])
                {
                    const auto fn = over == 0 ? m_actions.toShelf : m_actions.copy;
                    m_subtitle = fn();
                    m_actionDone[over] = true;
                    m_resultSince = GetTickCount64();
                    Animate();
                }
                break;
            }
            // A failure only goes away ("Click to dismiss"); anything else shows its files.
            if (m_onClick && m_outcome != Outcome::Failure)
                m_onClick();
            Retract();
            break;
        }
        default:
            break;
        }
    }

    void Island::ShowResult(Outcome outcome, const std::wstring& title, const std::wstring& subtitle, ResultActions actions)
    {
        m_actions = std::move(actions);
        m_actionDone[0] = m_actionDone[1] = false;
        m_actionHover = m_pressAction = -1;
        m_pressed = false;
        m_resultHover = false;
        m_outcome = outcome;
        m_onCancel = nullptr;   // the job is over
        m_cancelled = false;
        m_cancelHover = false;
        m_title = title;
        m_subtitle = subtitle;
        m_resultSince = GetTickCount64();
        if (m_a11y)
            m_a11y->Announce(subtitle.empty() ? title : std::format(L"{}. {}", title, subtitle), L"ShiftDrop.Result", true);
        POINT pt{};
        GetCursorPos(&pt);
        Present(State::Result, m_monitor ? m_monitor : MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST));
    }

    // ---- Animation -------------------------------------------------------------------------------

    void Island::Animate()
    {
        if (!m_hwnd)
            return;
        m_frames.Start(m_hwnd.get(), kTimerFrame);   // the result timeout also runs on it
        Render();
    }

    void Island::OnFrame()
    {
        const ULONGLONG now = GetTickCount64();
        const float dt = m_frames.Tick(now);
        const auto approach = [dt](float& value, float target, float tau) {
            panel::Approach(value, target, dt, tau, 0.002f * std::max(1.f, std::abs(target)));
        };

        if (m_retracting)
        {
            m_presenceT = std::min(1.f, m_presenceT + dt / kRetractMs);
            m_presence = 1.f - EaseInOutCubic(m_presenceT);
            if (m_presenceT >= 1.f)
            {
                m_retracting = false;
                m_state = State::Hidden;
                ShowWindow(m_hwnd.get(), SW_HIDE);
                m_frames.Stop(m_hwnd.get(), kTimerFrame);
                return;
            }
        }
        else if (m_motion)
        {
            m_presenceT = std::min(1.f, m_presenceT + dt / kPresenceMs);
            m_presence = Spring(m_presenceT);
        }
        if (m_motion)
        {
            approach(m_w, ContentWidth(), kSizeTauMs);
            approach(m_h, ContentHeight(), kSizeTauMs);
            m_contentT = std::min(1.f, m_contentT + dt / kContentMs);
            approach(m_tabPos, static_cast<float>(m_ring), kTabTauMs);
            approach(m_bar, m_total ? static_cast<float>(m_done / static_cast<double>(m_total)) : 0.f, kBarTauMs);
            m_spin = std::fmod(m_spin + dt * 0.36f, 360.f);   // one turn a second
        }
        else
        {
            m_tabPos = static_cast<float>(m_ring);
            m_bar = m_total ? static_cast<float>(m_done / static_cast<double>(m_total)) : 0.f;
        }

        if (m_state == State::Result && !m_retracting)
        {
            if (m_resultHover || m_carrying)
                m_resultSince = now;   // never tucks away from under the mouse
            const ULONGLONG limit = m_outcome != Outcome::Success ? kFailureMs : m_actions.Any() ? kCarryMs : kSuccessMs;
            if (now - m_resultSince >= limit)
            {
                Retract();
                return;
            }
        }
        Render();

        // Keep ticking while something moves, a spinner spins, or a result waits to tuck away.
        const bool moving = m_presenceT < 1.f || m_contentT < 1.f || m_w != ContentWidth() || m_h != ContentHeight() ||
                            m_tabPos != static_cast<float>(m_ring);
        if (!moving && m_state != State::Progress && m_state != State::Result)
            m_frames.Stop(m_hwnd.get(), kTimerFrame);
    }

    // ---- Content size ----------------------------------------------------------------------------

    float Island::ContentHeight() const
    {
        const float s = m_scale;
        switch (m_state)
        {
        case State::Switcher: return std::round(panel::kSwitcherHeightDip * s);
        case State::Progress: return std::round(kProgressHeightDip * s);
        default: return std::round(kResultHeightDip * s);
        }
    }

    float Island::ContentWidth() const
    {
        const float s = m_scale, inset = kInsetDip * s;
        float w = 0.f;
        switch (m_state)
        {
        case State::Switcher:
            w = FitSwitcher().width;
            break;
        case State::Progress:
            // As DrawProgress lays it out; the percentage measured at its widest so the pill keeps its size.
            w = (kLeadDip + kMarkDip + kMarkGapDip) * s + TextWidth(m_title, m_titleFont.Get()) + kMarkGapDip * s +
                std::max(TextWidth(L"\u00b7 100%", m_countFont.Get()), m_cancelled ? TextWidth(L"\u00b7 Cancelling\u2026", m_countFont.Get()) : 0.f);
            w += m_onCancel || m_cancelled ? (4.6f + kCancelDip + kButtonEndDip) * s : kLeadDip * s;
            break;
        default:
        {
            // As DrawResult: the lead (the carry handle or the status icon), the text, the buttons.
            const bool two = TwoLines();
            const bool failed = m_outcome == Outcome::Failure;
            float text = TextWidth(m_title, (two ? m_doneFont : m_titleFont).Get());
            if (failed)
                text += kMarkGapDip * s + TextWidth(kHintDismiss, m_hintFont.Get());
            text = std::max(text, TextWidth(m_subtitle, m_subFont.Get()));
            if (!failed)
            {
                // Room for the hints too, so the pill does not jump when the mouse arrives.
                text = std::max(text, TextWidth(m_actions.drag ? DragHint() : std::wstring(kHintShow), m_subFont.Get()));
                if (m_actions.Any())
                    text = std::max({ text, TextWidth(kHintActions[0], m_subFont.Get()), TextWidth(kHintActions[1], m_subFont.Get()) });
            }
            w = (m_actions.drag ? (kChipLeadDip + kChipWDip + kChipGapDip) : (kLeadDip + kMarkDip + kMarkGapDip)) * s + text;
            if (const size_t n = ActionCount())
                w += 6.f * s + static_cast<float>(n) * kActionDip * s + static_cast<float>(n - 1) * kActionGapDip * s + kButtonEndDip * s;
            else
                w += kTailDip * s;
            break;
        }
        }
        return std::min(std::round(w + 2.f * inset + 2.f * m_scale), m_maxContent);   // +2 so text never trims
    }

    panel::Switcher Island::FitSwitcher() const
    {
        std::vector<float> names;
        for (const auto& ring : m_rings)
            names.push_back(TextWidth(ring.title, m_tabFont.Get()));
        const float hint = TextWidth(SwitchHint(m_rings.size()), m_hintFont.Get());
        return panel::FitSwitcher(names, hint, m_scale, m_maxContent - 2.f * kInsetDip * m_scale);
    }

    // ---- Drawing ---------------------------------------------------------------------------------

    bool Island::EnsureTextFormats()
    {
        if (m_tabFont && m_fontScale == m_scale)
            return true;
        m_fontScale = m_scale;
        constexpr auto L = DWRITE_TEXT_ALIGNMENT_LEADING, C = DWRITE_TEXT_ALIGNMENT_CENTER, R = DWRITE_TEXT_ALIGNMENT_TRAILING;
        m_tabFont = MakeFormat(m_textFamily, panel::kTabFontDip, DWRITE_FONT_WEIGHT_SEMI_BOLD, L);
        m_tabIconFont = MakeFormat(m_iconFamily, 15.f, DWRITE_FONT_WEIGHT_NORMAL, C);
        m_hintFont = MakeFormat(m_textFamily, panel::kHintFontDip, DWRITE_FONT_WEIGHT_NORMAL, L);
        // The kit's sizes: a one-line pill's text as the section names; a two-line result a step
        // smaller over a small line.
        m_titleFont = MakeFormat(m_textFamily, panel::kTabFontDip, DWRITE_FONT_WEIGHT_SEMI_BOLD, L);
        m_countFont = MakeFormat(m_textFamily, panel::kTabFontDip, DWRITE_FONT_WEIGHT_NORMAL, L);
        m_doneFont = MakeFormat(m_textFamily, 12.25f, DWRITE_FONT_WEIGHT_SEMI_BOLD, L);
        m_subFont = MakeFormat(m_textFamily, 9.5f, DWRITE_FONT_WEIGHT_NORMAL, L);
        m_statusIconFont = MakeFormat(m_iconFamily, 16.f, DWRITE_FONT_WEIGHT_NORMAL, C);
        m_cancelFont = MakeFormat(m_iconFamily, 9.5f, DWRITE_FONT_WEIGHT_NORMAL, C);
        m_actionFont = MakeFormat(m_iconFamily, 12.f, DWRITE_FONT_WEIGHT_NORMAL, C);
        (void)R;
        return m_tabFont && m_tabIconFont && m_hintFont && m_titleFont && m_countFont && m_doneFont && m_subFont && m_statusIconFont && m_cancelFont &&
               m_actionFont;
    }

    void Island::DrawSwitcher(const D2D1_RECT_F& box, float alpha)
    {
        const float s = m_scale;
        // Tabs left to right, laid out as ContentWidth() measured them. Names only, no Shift / Ctrl /
        // Alt keycaps: scrolling reaches every wheel, and a key per wheel stops scaling as wheels are added.
        const auto fit = FitSwitcher();
        std::vector<D2D1_RECT_F> tabs;
        float x = box.left;
        for (const float w : fit.tabs)
        {
            tabs.push_back(D2D1::RectF(x, box.top, x + w, box.bottom));
            x += w;
        }
        if (tabs.empty())
            return;

        // The accent highlight slides between tabs.
        const int lo = std::clamp(static_cast<int>(std::floor(m_tabPos)), 0, static_cast<int>(tabs.size()) - 1);
        const int hi = std::min(lo + 1, static_cast<int>(tabs.size()) - 1);
        const float f = m_tabPos - static_cast<float>(lo);
        const auto& a = tabs[static_cast<size_t>(lo)];
        const auto& b = tabs[static_cast<size_t>(hi)];
        const auto sel = D2D1::RectF(a.left + (b.left - a.left) * f, a.top, a.right + (b.right - a.right) * f, a.bottom);
        const float r = (sel.bottom - sel.top) / 2.f;
        m_rt->FillRoundedRectangle(D2D1::RoundedRect(sel, r, r), Brush(m_palette.panelAccent, alpha).Get());

        for (size_t i = 0; i < tabs.size(); ++i)
        {
            const auto& t = tabs[i];
            const auto& ring = m_rings[i];
            const float on = std::max(0.f, 1.f - std::abs(m_tabPos - static_cast<float>(i)));
            const auto colour = Mix(m_palette.panelTextDim, m_palette.panelTextOnAccent, on);
            const float cy = (t.top + t.bottom) / 2.f;
            float tx = t.left + fit.pad;
            const wchar_t glyph[2]{ ring.icon, 0 };
            Text(glyph, m_tabIconFont.Get(), D2D1::RectF(tx, cy - 11.f * s, tx + panel::kIconDip * s, cy + 11.f * s), colour, alpha);
            tx += panel::kIconDip * s + fit.iconGap;
            // Shortened names (a very narrow screen) end in "...": the text format trims.
            const float tw = fit.nameScale < 1.f ? t.right - fit.pad - tx : TextWidth(ring.title, m_tabFont.Get()) + 1.f;
            Text(ring.title, m_tabFont.Get(), D2D1::RectF(tx, t.top, tx + tw, t.bottom), colour, alpha);
        }
        if (fit.hint)
            Text(SwitchHint(tabs.size()), m_hintFont.Get(), D2D1::RectF(x + fit.hintGap, box.top, box.right, box.bottom), m_palette.panelTextDim, alpha);
    }

    void Island::DrawProgress(const D2D1_RECT_F& box, float alpha)
    {
        // "(spinner) Converting 3 files · 40%  (×)", the bar along the pill's bottom edge.
        const float s = m_scale;
        const float cy = (box.top + box.bottom) / 2.f;
        const auto c = D2D1::Point2F(box.left + (kLeadDip + kMarkDip / 2.f) * s, cy);
        const float r = (kMarkDip / 2.f - 1.f) * s, stroke = 1.8f * s;
        m_rt->DrawEllipse(D2D1::Ellipse(c, r, r), Brush(m_palette.track, alpha).Get(), stroke);
        ComPtr<ID2D1PathGeometry> arc;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(m_d2d->CreatePathGeometry(&arc)) && SUCCEEDED(arc->Open(&sink)))
        {
            const auto at = [&](float deg) {
                const float rad = deg * 3.14159265f / 180.f;
                return D2D1::Point2F(c.x + r * std::sin(rad), c.y - r * std::cos(rad));
            };
            sink->BeginFigure(at(m_spin), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(at(m_spin + 90.f), D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            ComPtr<ID2D1StrokeStyle> round;
            m_d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &round);
            m_rt->DrawGeometry(arc.Get(), Brush(m_palette.accentMark, alpha).Get(), stroke, round.Get());
        }

        // The × at the right end, on a faint disc; red under the mouse.
        float right = box.right - kLeadDip * s, barRight = box.right - kLeadDip * s;
        if (m_onCancel || m_cancelled)
        {
            const float d = kCancelDip * s;
            const auto c2 = D2D1::Point2F(box.right - kButtonEndDip * s - d / 2.f, cy);
            m_cancelRect = D2D1::RectF(c2.x - d / 2.f, c2.y - d / 2.f, c2.x + d / 2.f, c2.y + d / 2.f);
            m_rt->FillEllipse(D2D1::Ellipse(c2, d / 2.f, d / 2.f), Brush(m_cancelHover ? m_palette.error : m_palette.keycap, alpha).Get());
            const wchar_t x[2]{ kGlyphClose, 0 };
            const auto ink = m_cancelHover ? Rgb(accent::TextOn(RgbOf(m_palette.error))) : m_palette.panelText;
            Text(x, m_cancelFont.Get(), m_cancelRect, ink, (m_cancelled ? 0.4f : 1.f) * alpha);
            right = m_cancelRect.left - 4.6f * s;
            barRight = m_cancelRect.left - 8.f * s;
        }

        // A single video or audio file moves within itself; a batch by the files done.
        const auto count = m_cancelled ? std::wstring(L"\u00b7 Cancelling\u2026")
                                       : std::format(L"\u00b7 {}%", m_total ? static_cast<int>(100.0 * m_done / static_cast<double>(m_total)) : 0);
        const float tx = box.left + (kLeadDip + kMarkDip + kMarkGapDip) * s;
        const float cw = TextWidth(count, m_countFont.Get());
        const float titleRight = std::max(tx, std::min(tx + TextWidth(m_title, m_titleFont.Get()) + 1.f, right - cw - kMarkGapDip * s));
        Text(m_title, m_titleFont.Get(), D2D1::RectF(tx, box.top, titleRight, box.bottom), m_palette.panelText, alpha);
        Text(count, m_countFont.Get(), D2D1::RectF(titleRight + kMarkGapDip * s, box.top, right, box.bottom), m_palette.panelTextDim, alpha);

        // The bar, thin, along the bottom of the pill (the box is kInsetDip inside it).
        const float bottom = box.bottom + (kInsetDip - kBarBottomDip) * s;
        const auto track = D2D1::RectF(box.left + kLeadDip * s, bottom - kBarDip * s, barRight, bottom);
        const float br = kBarDip * s / 2.f;
        m_rt->FillRoundedRectangle(D2D1::RoundedRect(track, br, br), Brush(m_palette.track, alpha).Get());
        if (m_bar > 0.f)
        {
            const auto fill = D2D1::RectF(track.left, track.top, track.left + (track.right - track.left) * m_bar, track.bottom);
            m_rt->FillRoundedRectangle(D2D1::RoundedRect(fill, br, br), Brush(m_palette.accentMark, alpha).Get());
        }
    }

    void Island::DrawResult(const D2D1_RECT_F& box, float alpha)
    {
        const float s = m_scale;
        const float cy = (box.top + box.bottom) / 2.f;
        const bool failed = m_outcome == Outcome::Failure;
        float tx = 0.f;
        if (m_actions.drag)
        {
            // The handle: grip dots and the file, on a faint chip; drag it (or the pill) into an app.
            const auto chip = D2D1::RectF(box.left + kChipLeadDip * s, cy - kChipHDip * s / 2.f, box.left + (kChipLeadDip + kChipWDip) * s,
                                          cy + kChipHDip * s / 2.f);
            m_rt->FillRoundedRectangle(D2D1::RoundedRect(chip, 4.6f * s, 4.6f * s), Brush(m_palette.keycap, alpha).Get());
            for (int col = 0; col < 2; ++col)
                for (int row = -1; row <= 1; ++row)
                    m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(chip.left + (6.f + 2.9f * static_cast<float>(col)) * s, cy + 2.9f * s * static_cast<float>(row)),
                                                    0.95f * s, 0.95f * s),
                                      Brush(m_palette.panelTextDim, alpha).Get());
            DrawFileMark(D2D1::RectF(chip.left + 14.f * s, chip.top + 2.5f * s, chip.right - 2.5f * s, chip.bottom - 2.5f * s), m_palette, true, alpha);
            tx = chip.right + kChipGapDip * s;
        }
        else
        {
            // The status icon, popping in with the content.
            const auto& tone = m_outcome == Outcome::Success ? m_palette.ok : m_outcome == Outcome::Partial ? m_palette.warn : m_palette.error;
            const wchar_t glyph = m_outcome == Outcome::Success ? kGlyphCheck : m_outcome == Outcome::Partial ? kGlyphWarning : kGlyphError;
            const float pop = 0.7f + 0.3f * Spring(m_contentT);
            const auto c = D2D1::Point2F(box.left + (kLeadDip + kMarkDip / 2.f) * s, cy);
            const float r = kMarkDip / 2.f * s * pop + 2.f * s;
            const wchar_t g[2]{ glyph, 0 };
            Text(g, m_statusIconFont.Get(), D2D1::RectF(c.x - r, c.y - r, c.x + r, c.y + r), tone, alpha);
            tx = box.left + (kLeadDip + kMarkDip + kMarkGapDip) * s;
        }

        // Float and Copy at the right end.
        float textRight = box.right - kTailDip * s;
        if (const size_t n = ActionCount())
        {
            const float d = kActionDip * s, gap = kActionGapDip * s;
            const std::function<std::wstring()>* fns[2] = { &m_actions.toShelf, &m_actions.copy };
            const wchar_t glyphs[2] = { kGlyphPin, kGlyphCopy };
            float x = box.right - kButtonEndDip * s - static_cast<float>(n) * d - static_cast<float>(n - 1) * gap;
            textRight = x - 6.f * s;
            for (size_t i = 0; i < n; ++i, x += d + gap)
            {
                m_actionRects[i] = D2D1::RectF(x, cy - d / 2.f, x + d, cy + d / 2.f);
                if (!*fns[i])
                    continue;
                const bool hover = m_actionHover == static_cast<int>(i), done = m_actionDone[i];
                const auto c2 = D2D1::Point2F(x + d / 2.f, cy);
                m_rt->FillEllipse(D2D1::Ellipse(c2, d / 2.f, d / 2.f),
                                  Brush(done ? WithAlpha(m_palette.ok, 0.22f) : hover ? m_palette.panelAccent : m_palette.keycap, alpha).Get());
                const wchar_t g2[2]{ done ? kGlyphCheck : glyphs[i], 0 };
                Text(g2, m_actionFont.Get(), m_actionRects[i], done ? m_palette.ok : hover ? m_palette.panelTextOnAccent : m_palette.panelText, alpha);
            }
        }

        // The title, and a failure's "Click to dismiss" beside it; the line under it.
        const bool two = TwoLines();
        IDWriteTextFormat* titleFont = (two ? m_doneFont : m_titleFont).Get();
        const auto line = ResultLine();
        const float top = two ? cy - 13.f * s : box.top, bottom = two ? cy + 1.5f * s : box.bottom;
        float titleRight = textRight;
        if (failed)
        {
            const float hw = TextWidth(kHintDismiss, m_hintFont.Get());
            titleRight = std::max(tx, std::min(tx + TextWidth(m_title, titleFont) + 1.f, textRight - hw - kMarkGapDip * s));
            Text(kHintDismiss, m_hintFont.Get(), D2D1::RectF(titleRight + kMarkGapDip * s, top, textRight, bottom), m_palette.panelTextDim, alpha);
        }
        Text(m_title, titleFont, D2D1::RectF(tx, top, titleRight, bottom), m_palette.panelText, alpha);
        if (two)
            Text(line, m_subFont.Get(), D2D1::RectF(tx, cy + 1.5f * s, textRight, cy + 13.f * s), m_palette.panelTextDim, alpha);
    }

    void Island::Render()
    {
        if (!m_hwnd || !m_canvas || !EnsureTextFormats())
            return;
        m_rt = m_canvas->Begin(m_size);
        if (!m_rt)
            return;
        const float s = m_scale;
        const float w = std::max(m_w, 1.f), h = std::max(m_h, 1.f);
        const float cx = static_cast<float>(m_size.cx) / 2.f;
        // Slide down from above the window's top edge (which sits just above the work area).
        const float top = m_pad - (1.f - m_presence) * (h + m_pad + kTopGapDip * s);
        const auto rect = D2D1::RectF(std::round(cx - w / 2.f), top, std::round(cx + w / 2.f), top + h);
        const float radius = std::min(h / 2.f, 24.f * s);
        const auto pill = D2D1::RoundedRect(rect, radius, radius);

        // Soft shadow: a few growing, fading copies.
        for (int k = 6; k >= 1; --k)
        {
            const float g = 2.6f * s * static_cast<float>(k);
            m_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rect.left - g, rect.top - g + 4.f * s, rect.right + g, rect.bottom + g + 4.f * s), radius + g, radius + g),
                                       Brush(m_palette.shadow, 0.045f).Get());
        }
        if (m_appearance.style == Style::Glass)
        {
            if (!m_backdrop || m_backdropGeneration != m_canvas->DeviceGeneration())
            {
                m_backdrop = ToBitmap(m_rt, m_backdropPx);
                m_backdropGeneration = m_canvas->DeviceGeneration();
            }
            if (m_backdrop)
            {
                ComPtr<ID2D1BitmapBrush> glass;
                m_rt->CreateBitmapBrush(m_backdrop.Get(),
                                        D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR), &glass);
                if (glass)
                {
                    glass->SetTransform(D2D1::Matrix3x2F::Scale(m_backdropStep, m_backdropStep));
                    m_rt->FillRoundedRectangle(pill, glass.Get());
                }
            }
        }
        m_rt->FillRoundedRectangle(pill, Brush(m_palette.panel).Get());
        m_rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rect.left + 0.5f, rect.top + 0.5f, rect.right - 0.5f, rect.bottom - 0.5f), radius - 0.5f, radius - 0.5f),
                                   Brush(m_palette.panelLine).Get(), 1.f);
        if (m_state == State::Result && m_outcome == Outcome::Failure)
        {
            // A failure is outlined in the error colour; it stays until clicked.
            const float lw = kFailLineDip * s, e = lw / 2.f;
            m_rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rect.left + e, rect.top + e, rect.right - e, rect.bottom - e), radius - e, radius - e),
                                       Brush(m_palette.error, EaseOutCubic(m_contentT)).Get(), lw);
        }

        // Content is laid out for the target size and clipped to the morphing pill, so it never
        // reflows while the pill grows; it fades in after a state change.
        const float tw = ContentWidth(), th = ContentHeight(), inset = kInsetDip * s;
        const auto box = D2D1::RectF(std::round(cx - tw / 2.f) + inset, top + (h - th) / 2.f + inset,
                                     std::round(cx + tw / 2.f) - inset, top + (h + th) / 2.f - inset);
        ComPtr<ID2D1RoundedRectangleGeometry> clip;
        ComPtr<ID2D1Layer> layer;
        const bool clipped = SUCCEEDED(m_d2d->CreateRoundedRectangleGeometry(pill, &clip)) && SUCCEEDED(m_rt->CreateLayer(&layer));
        if (clipped)
            m_rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), layer.Get());
        const float alpha = EaseOutCubic(m_contentT);
        switch (m_state)
        {
        case State::Switcher: DrawSwitcher(box, alpha); break;
        case State::Progress: DrawProgress(box, alpha); break;
        case State::Result: DrawResult(box, alpha); break;
        default: break;
        }
        if (clipped)
            m_rt->PopLayer();

        const float opacity = m_retracting ? std::min(1.f, m_presence * 1.5f) : std::min(1.f, 0.35f + m_presenceT * 4.f);
        m_canvas->End(m_hwnd.get(), m_origin, opacity);
        m_rt = nullptr;
    }
}
