#include "Brand.h"
#include "Island.h"
#include "Log.h"

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

        // DIPs.
        constexpr float kTopGapDip = 10.f;      // work-area top edge to the panel
        constexpr float kPadDip = 24.f;         // shadow room
        constexpr float kMaxWidthDip = 720.f;   // the window is this wide; the panel inside it is narrower
        constexpr float kMaxHeightDip = 72.f;
        constexpr float kInsetDip = 8.f;        // panel edge to content

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
        constexpr wchar_t kGlyphError = 0xE711;     // Cancel
        constexpr wchar_t kGlyphOpen = 0xE8A7;      // OpenInNewWindow
        constexpr wchar_t kGlyphClose = 0xE8BB;     // ChromeClose (the cancel ×)
        constexpr float kCancelDip = 26.f;          // the × button's diameter

        // Carry (a result with output files).
        constexpr ULONGLONG kCarryMs = 5000;        // a success that can be carried stays a little longer
        constexpr float kActionDip = 28.f;          // Float and Copy buttons
        constexpr float kActionGapDip = 6.f;
        constexpr wchar_t kGlyphPin = 0xE718;       // Pin (Float shelf)
        constexpr wchar_t kGlyphCopy = 0xE8C8;      // Copy
        constexpr const wchar_t* kHintDrag = L"Drag into any app, or click to show in Explorer";
        constexpr const wchar_t* kHintActions[2] = { L"Keep on the Float shelf", L"Copy to the clipboard" };
    }

    Island::Island(HINSTANCE instance, std::vector<WheelRing> rings, std::function<void()> onClick)
        : m_instance(instance), m_rings(std::move(rings)), m_onClick(std::move(onClick))
    {
    }

    Island::~Island() = default;

    bool Island::Create()
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = m_instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
        m_hwnd.reset(CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
                                     kClassName, SD_NAME, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, m_instance, this));
        if (!m_hwnd ||
            FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2d.ReleaseAndGetAddressOf())) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(m_dwrite.ReleaseAndGetAddressOf()))))
        {
            log::Error(L"island: could not create the top panel");
            return false;
        }
        m_canvas = std::make_unique<Canvas>(m_d2d.Get());
        m_textFamily = TextFamily(m_dwrite.Get());
        m_iconFamily = IconFamily(m_dwrite.Get());
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
        m_scale = ScaleFor(monitor, m_appearance.size);
        EnsureTextFormats();
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(monitor, &mi);
        m_pad = std::round(kPadDip * m_scale);
        const float w = std::round(kMaxWidthDip * m_scale) + 2.f * m_pad;
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
                const int step = std::max(4, static_cast<int>(std::lround(6.f * m_scale)));
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

    std::wstring Island::ResultLine() const
    {
        if (m_actionHover >= 0 && !m_actionDone[m_actionHover])
            return kHintActions[m_actionHover];
        if (m_resultHover && m_actions.drag && !m_actionDone[0] && !m_actionDone[1])
            return kHintDrag;
        return m_subtitle;
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
            if (m_onClick)
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
        if (!m_animating)
        {
            m_animating = true;
            m_lastFrame = GetTickCount64();
            // ~64 fps (the timer rounds up to the system tick); the result timeout also runs here.
            SetTimer(m_hwnd.get(), kTimerFrame, 10, nullptr);
        }
        Render();
    }

    void Island::OnFrame()
    {
        const ULONGLONG now = GetTickCount64();
        const float dt = static_cast<float>(std::min<ULONGLONG>(now - m_lastFrame, 50));
        m_lastFrame = now;
        const auto approach = [dt](float& value, float target, float tau) {
            value += (target - value) * (1.f - std::exp(-dt / tau));
            if (std::abs(target - value) < 0.002f * std::max(1.f, std::abs(target)))
                value = target;
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
                KillTimer(m_hwnd.get(), kTimerFrame);
                m_animating = false;
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
        {
            KillTimer(m_hwnd.get(), kTimerFrame);
            m_animating = false;
        }
    }

    // ---- Content size ----------------------------------------------------------------------------

    float Island::ContentHeight() const
    {
        const float s = m_scale;
        switch (m_state)
        {
        case State::Switcher: return std::round(44.f * s);
        case State::Progress: return std::round(56.f * s);
        default: return std::round(m_subtitle.empty() ? 50.f * s : 60.f * s);
        }
    }

    float Island::ContentWidth() const
    {
        const float s = m_scale, inset = kInsetDip * s;
        float w = 0.f;
        switch (m_state)
        {
        case State::Switcher:
            for (const auto& ring : m_rings)
            {
                w += 2.f * 12.f * s + 18.f * s + 7.f * s + TextWidth(ring.title, m_tabFont.Get());
            }
            w += 14.f * s + TextWidth(SwitchHint(m_rings.size()), m_hintFont.Get()) + 10.f * s;
            break;
        case State::Progress:
            w = 14.f * s + 26.f * s + 12.f * s + std::max(TextWidth(m_title, m_titleFont.Get()), 200.f * s) + 28.f * s +
                TextWidth(std::format(L"{} of {}", m_total, m_total), m_subFont.Get()) + 14.f * s + 2.f * s;   // matches DrawProgress, +2 so it never trims
            if (m_onCancel || m_cancelled)
                w += kCancelDip * s + 10.f * s;
            break;
        default:
        {
            float text = std::max(TextWidth(m_title, m_titleFont.Get()), TextWidth(m_subtitle, m_subFont.Get()));
            if (m_actions.Any())
            {
                // Room for the hints too, so the panel does not jump when the mouse arrives.
                text = std::max({ text, TextWidth(kHintDrag, m_subFont.Get()), TextWidth(kHintActions[0], m_subFont.Get()),
                                  TextWidth(kHintActions[1], m_subFont.Get()) });
                text += static_cast<float>(ActionCount()) * (kActionDip + kActionGapDip) * s;
            }
            w = 14.f * s + 32.f * s + 12.f * s + text + 20.f * s + 20.f * s + 12.f * s;
            break;
        }
        }
        return std::min(std::round(w + 2.f * inset), std::round(kMaxWidthDip * s));
    }

    // ---- Drawing ---------------------------------------------------------------------------------

    ComPtr<IDWriteTextFormat> Island::MakeFormat(const std::wstring& family, float dip, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align) const
    {
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(m_dwrite->CreateTextFormat(family.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                              dip * m_scale, L"", &format)))
            return nullptr;
        format->SetTextAlignment(align);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(m_dwrite->CreateEllipsisTrimmingSign(format.Get(), &ellipsis)))
            format->SetTrimming(&trimming, ellipsis.Get());
        return format;
    }

    bool Island::EnsureTextFormats()
    {
        if (m_tabFont && m_fontScale == m_scale)
            return true;
        m_fontScale = m_scale;
        constexpr auto L = DWRITE_TEXT_ALIGNMENT_LEADING, C = DWRITE_TEXT_ALIGNMENT_CENTER, R = DWRITE_TEXT_ALIGNMENT_TRAILING;
        m_tabFont = MakeFormat(m_textFamily, 13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, L);
        m_tabIconFont = MakeFormat(m_iconFamily, 15.f, DWRITE_FONT_WEIGHT_NORMAL, C);
        m_hintFont = MakeFormat(m_textFamily, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, L);
        m_titleFont = MakeFormat(m_textFamily, 14.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, L);
        m_subFont = MakeFormat(m_textFamily, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, L);
        m_statusIconFont = MakeFormat(m_iconFamily, 15.f, DWRITE_FONT_WEIGHT_BOLD, C);
        m_cancelFont = MakeFormat(m_iconFamily, 9.5f, DWRITE_FONT_WEIGHT_NORMAL, C);
        m_actionFont = MakeFormat(m_iconFamily, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, C);
        (void)R;
        return m_tabFont && m_tabIconFont && m_hintFont && m_titleFont && m_subFont && m_statusIconFont && m_cancelFont && m_actionFont;
    }

    float Island::TextWidth(const std::wstring& text, IDWriteTextFormat* format) const
    {
        ComPtr<IDWriteTextLayout> layout;
        DWRITE_TEXT_METRICS metrics{};
        if (text.empty() || !format ||
            FAILED(m_dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, 10000.f, 100.f, &layout)) ||
            FAILED(layout->GetMetrics(&metrics)))
            return 0.f;
        return std::ceil(metrics.widthIncludingTrailingWhitespace);
    }

    ComPtr<ID2D1SolidColorBrush> Island::Brush(const D2D1_COLOR_F& color, float alpha)
    {
        ComPtr<ID2D1SolidColorBrush> brush;
        m_rt->CreateSolidColorBrush(WithAlpha(color, alpha), &brush);
        return brush;
    }

    void Island::Text(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float alpha)
    {
        if (text.empty() || !format || alpha <= 0.f)
            return;
        m_rt->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, rect, Brush(color, alpha).Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void Island::DrawSwitcher(const D2D1_RECT_F& box, float alpha)
    {
        const float s = m_scale;
        // Tabs left to right, measured the same way ContentWidth() does.
        std::vector<D2D1_RECT_F> tabs;
        float x = box.left;
        for (const auto& ring : m_rings)
        {
            // Names only, no Shift / Ctrl / Alt keycaps: scrolling reaches every wheel, and a key per
            // wheel stops scaling as wheels are added.
            const float w = 2.f * 12.f * s + 18.f * s + 7.f * s + TextWidth(ring.title, m_tabFont.Get());
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
        m_rt->FillRoundedRectangle(D2D1::RoundedRect(sel, r, r), Brush(m_palette.accent, alpha).Get());

        for (size_t i = 0; i < tabs.size(); ++i)
        {
            const auto& t = tabs[i];
            const auto& ring = m_rings[i];
            const float on = std::max(0.f, 1.f - std::abs(m_tabPos - static_cast<float>(i)));
            const auto colour = Mix(m_palette.panelTextDim, m_palette.textOnAccent, on);
            const float cy = (t.top + t.bottom) / 2.f;
            float tx = t.left + 12.f * s;
            const wchar_t glyph[2]{ ring.icon, 0 };
            Text(glyph, m_tabIconFont.Get(), D2D1::RectF(tx, cy - 11.f * s, tx + 18.f * s, cy + 11.f * s), colour, alpha);
            tx += 18.f * s + 7.f * s;
            const float tw = TextWidth(ring.title, m_tabFont.Get());
            Text(ring.title, m_tabFont.Get(), D2D1::RectF(tx, t.top, tx + tw + 1.f, t.bottom), colour, alpha);
        }
        const float hx = x + 14.f * s;
        Text(SwitchHint(tabs.size()), m_hintFont.Get(), D2D1::RectF(hx, box.top, box.right, box.bottom), m_palette.panelTextDim, alpha);
    }

    void Island::DrawProgress(const D2D1_RECT_F& box, float alpha)
    {
        const float s = m_scale;
        const float cy = (box.top + box.bottom) / 2.f;
        // Spinner: an accent arc turning on a faint track.
        const auto c = D2D1::Point2F(box.left + 14.f * s + 13.f * s, cy);
        const float r = 10.f * s;
        m_rt->DrawEllipse(D2D1::Ellipse(c, r, r), Brush(m_palette.keycap, alpha).Get(), 2.5f * s);
        ComPtr<ID2D1PathGeometry> arc;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(m_d2d->CreatePathGeometry(&arc)) && SUCCEEDED(arc->Open(&sink)))
        {
            const auto at = [&](float deg) {
                const float rad = deg * 3.14159265f / 180.f;
                return D2D1::Point2F(c.x + r * std::sin(rad), c.y - r * std::cos(rad));
            };
            sink->BeginFigure(at(m_spin), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(at(m_spin + 100.f), D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            ComPtr<ID2D1StrokeStyle> round;
            m_d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &round);
            m_rt->DrawGeometry(arc.Get(), Brush(m_palette.accent, alpha).Get(), 2.5f * s, round.Get());
        }

        // The × sits at the right end; the text and the bar end before it.
        auto right = box.right;
        if (m_onCancel || m_cancelled)
        {
            const float d = kCancelDip * s;
            const auto c2 = D2D1::Point2F(box.right - 6.f * s - d / 2.f, cy);
            m_cancelRect = D2D1::RectF(c2.x - d / 2.f, c2.y - d / 2.f, c2.x + d / 2.f, c2.y + d / 2.f);
            const float on = m_cancelHover ? 1.f : 0.f;
            m_rt->FillEllipse(D2D1::Ellipse(c2, d / 2.f, d / 2.f),
                              Brush(m_cancelHover ? m_palette.error : m_palette.keycap, (m_cancelHover ? 0.9f : 1.f) * alpha).Get());
            const wchar_t x[2]{ kGlyphClose, 0 };
            Text(x, m_cancelFont.Get(), m_cancelRect, Mix(m_palette.panelTextDim, m_palette.textOnAccent, on), (m_cancelled ? 0.4f : 1.f) * alpha);
            right = m_cancelRect.left - 4.f * s;
        }

        const float tx = box.left + 14.f * s + 26.f * s + 12.f * s;
        // "2 of 5" files; a single video or audio file shows its percentage instead.
        const bool percent = m_fractional && m_total == 1;
        const auto count = m_cancelled ? std::wstring(L"Cancelling\u2026")
                           : percent  ? std::format(L"{}%", static_cast<int>(m_done * 100.0))
                                      : std::format(L"{} of {}", static_cast<size_t>(m_done), m_total);
        const float cw = std::max(TextWidth(std::format(L"{} of {}", m_total, m_total), m_subFont.Get()),
                                  m_cancelled ? TextWidth(count, m_subFont.Get()) : 0.f);
        Text(m_title, m_titleFont.Get(), D2D1::RectF(tx, box.top + 4.f * s, right - cw - 28.f * s, cy + 3.f * s), m_palette.panelText, alpha);
        Text(count, m_subFont.Get(), D2D1::RectF(right - cw - 14.f * s, box.top + 4.f * s, right, cy + 3.f * s), m_palette.panelTextDim, alpha);
        // Progress bar under the title.
        const auto track = D2D1::RectF(tx, cy + 9.f * s, right - 14.f * s, cy + 13.f * s);
        m_rt->FillRoundedRectangle(D2D1::RoundedRect(track, 2.f * s, 2.f * s), Brush(m_palette.keycap, alpha).Get());
        if (m_bar > 0.f)
        {
            const auto fill = D2D1::RectF(track.left, track.top, track.left + (track.right - track.left) * m_bar, track.bottom);
            m_rt->FillRoundedRectangle(D2D1::RoundedRect(fill, 2.f * s, 2.f * s), Brush(m_palette.accent, alpha).Get());
        }
    }

    void Island::DrawResult(const D2D1_RECT_F& box, float alpha)
    {
        const float s = m_scale;
        const float cy = (box.top + box.bottom) / 2.f;
        const auto& tone = m_outcome == Outcome::Success ? m_palette.ok : m_outcome == Outcome::Partial ? m_palette.warn : m_palette.error;
        const wchar_t glyph = m_outcome == Outcome::Success ? kGlyphCheck : m_outcome == Outcome::Partial ? kGlyphWarning : kGlyphError;
        // Status badge, popping in with the content.
        const float pop = 0.7f + 0.3f * Spring(m_contentT);
        const auto c = D2D1::Point2F(box.left + 14.f * s + 16.f * s, cy);
        const float r = 15.f * s * pop;
        m_rt->FillEllipse(D2D1::Ellipse(c, r, r), Brush(tone, 0.22f * alpha).Get());
        const wchar_t g[2]{ glyph, 0 };
        Text(g, m_statusIconFont.Get(), D2D1::RectF(c.x - r, c.y - r, c.x + r, c.y + r), tone, alpha);

        const float tx = box.left + 14.f * s + 32.f * s + 12.f * s;
        const float right = box.right - 20.f * s - 12.f * s;
        // Float and Copy, just left of the "open" glyph.
        float textRight = right;
        if (const size_t n = ActionCount())
        {
            const float d = kActionDip * s, gap = kActionGapDip * s;
            const std::function<std::wstring()>* fns[2] = { &m_actions.toShelf, &m_actions.copy };
            const wchar_t glyphs[2] = { kGlyphPin, kGlyphCopy };
            float x = right - static_cast<float>(n) * (d + gap);
            textRight = x - 4.f * s;
            for (size_t i = 0; i < n; ++i, x += d + gap)
            {
                m_actionRects[i] = D2D1::RectF(x, cy - d / 2.f, x + d, cy + d / 2.f);
                if (!*fns[i])
                    continue;
                const bool hover = m_actionHover == static_cast<int>(i), done = m_actionDone[i];
                const auto c2 = D2D1::Point2F(x + d / 2.f, cy);
                m_rt->FillEllipse(D2D1::Ellipse(c2, d / 2.f, d / 2.f),
                                  Brush(done ? WithAlpha(m_palette.ok, 0.22f) : hover ? m_palette.accent : m_palette.keycap, alpha).Get());
                const wchar_t g2[2]{ done ? kGlyphCheck : glyphs[i], 0 };
                Text(g2, m_actionFont.Get(), m_actionRects[i], done ? m_palette.ok : hover ? m_palette.textOnAccent : m_palette.panelText, alpha);
            }
        }
        const auto line = ResultLine();
        if (line.empty())
        {
            Text(m_title, m_titleFont.Get(), D2D1::RectF(tx, box.top, textRight, box.bottom), m_palette.panelText, alpha);
        }
        else
        {
            Text(m_title, m_titleFont.Get(), D2D1::RectF(tx, box.top + 6.f * s, textRight, cy + 2.f * s), m_palette.panelText, alpha);
            Text(line, m_subFont.Get(), D2D1::RectF(tx, cy + 1.f * s, textRight, box.bottom - 6.f * s), m_palette.panelTextDim, alpha);
        }
        const wchar_t open[2]{ kGlyphOpen, 0 };
        Text(open, m_tabIconFont.Get(), D2D1::RectF(right, box.top, box.right - 8.f * s, box.bottom), m_palette.panelTextDim, alpha);
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
