#include "preview/PreviewCard.h"
#include "Brand.h"
#include "Log.h"

#include <algorithm>
#include <cmath>

using namespace sd::theme;

namespace sd
{
    namespace
    {
        constexpr wchar_t kClassName[] = L"" SD_NAME L".Preview";
        constexpr UINT kMsgPrediction = WM_APP + 1;   // lParam: preview::Prediction*, owned by the receiver
        constexpr UINT_PTR kTimerHide = 1;            // a short gap between two slices does not blink the card
        constexpr UINT kHideDelayMs = 120;

        // Card layout, DIPs, as the brand kit's App UI page draws it under the wheel: the file, then
        // "WebP   1.2 MB · was 3.4 MB" over the output's name (and what changes, when something does).
        constexpr float kWidthDip = 308.f;
        constexpr float kHeightDip = 52.f;       // two lines
        constexpr float kDetailDip = 14.f;       // the third line, when there is one
        constexpr float kGapDip = 10.f;          // from the wheel's edge
        constexpr float kPadDip = 13.f;
        constexpr float kThumbWDip = 26.f, kThumbHDip = 32.f;
        constexpr float kThumbGapDip = 9.7f;
        constexpr float kCornerDip = 6.5f;
        constexpr float kShadowDip = 14.f;       // room around the card for its shadow
        constexpr uint32_t kThumbPx = 128;   // asked of the shell; drawn smaller

        // The source's thumbnail for the card. Runs on the predictor's worker thread.
        std::shared_ptr<const preview::Thumbnail> CardThumbnail(const std::wstring& path)
        {
            auto thumb = ShellThumbnail(path, kThumbPx);
            return thumb.px.empty() ? nullptr : std::make_shared<const preview::Thumbnail>(std::move(thumb));
        }
    }

    PreviewCard::PreviewCard(HINSTANCE instance, std::shared_ptr<const FfmpegEngine> media)
        : m_instance(instance), m_media(std::move(media))
    {
    }

    PreviewCard::~PreviewCard() { Destroy(); }

    bool PreviewCard::Create()
    {
        // WS_EX_TRANSPARENT: clicks, hovers and drops go straight through to what is underneath.
        m_hwnd.reset(CreatePanel(m_instance, kClassName, WndProc, WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                 SD_NAME, this));
        if (!m_hwnd || !InitGraphics())
        {
            log::Error(L"preview: window or Direct2D unavailable; no predictions");
            m_hwnd.reset();
            return false;
        }
        HWND hwnd = m_hwnd.get();
        m_predictor = std::make_unique<preview::Predictor>(
            m_media,
            [hwnd](preview::Prediction p) {
                auto owned = std::make_unique<preview::Prediction>(std::move(p));
                if (PostMessageW(hwnd, kMsgPrediction, 0, reinterpret_cast<LPARAM>(owned.get())))
                    owned.release();   // the window owns it now
            },
            CardThumbnail);
        return true;
    }

    void PreviewCard::Destroy()
    {
        m_predictor.reset();   // joins the worker: nothing posts to the window after this
        m_hwnd.reset();
    }

    LRESULT CALLBACK PreviewCard::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_NCCREATE)
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        auto* self = reinterpret_cast<PreviewCard*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg)
        {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case kMsgPrediction:
        {
            std::unique_ptr<preview::Prediction> p(reinterpret_cast<preview::Prediction*>(lParam));
            if (self)
                self->OnPrediction(std::move(p));
            return 0;
        }
        case WM_TIMER:
            if (self && wParam == kTimerHide)
            {
                KillTimer(hwnd, kTimerHide);
                self->HideNow();
            }
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    void PreviewCard::Update(const WheelHighlight& h, const Recipe* recipe)
    {
        if (!m_hwnd || !m_predictor)
            return;
        if (!recipe || h.actionId.empty() || !h.files || h.files->empty() || !preview::Predictor::Predicts(*recipe))
        {
            m_predictor->Cancel();
            ++m_token;   // anything still on its way is stale
            if (m_shown)
                SetTimer(m_hwnd.get(), kTimerHide, kHideDelayMs, nullptr);
            return;
        }
        KillTimer(m_hwnd.get(), kTimerHide);
        m_token++;
        m_current = {};
        m_current.token = m_token;
        m_label = recipe->label;
        m_predictor->Request(m_token, *h.files, *recipe);
        Place(h);
        Render();
        if (!m_shown)
            SetWindowPos(m_hwnd.get(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        m_shown = true;
    }

    void PreviewCard::HideNow()
    {
        if (m_shown)
            ShowWindow(m_hwnd.get(), SW_HIDE);
        m_shown = false;
        m_current = {};
        m_thumbBitmap.Reset();
        m_thumbFor = nullptr;
    }

    void PreviewCard::OnPrediction(std::unique_ptr<preview::Prediction> p)
    {
        if (!p || p->token != m_token || !m_shown)
            return;   // for a slice no longer highlighted
        m_current = std::move(*p);
        Fit();   // a line on what changes makes it taller
        Render();
    }

    // Under the wheel, centred; above it when the monitor's work area ends too soon.
    void PreviewCard::Place(const WheelHighlight& h)
    {
        m_scale = h.scale > 0.f ? h.scale : 1.f;
        m_palette = Current(m_appearance.style);
        const LONG w = static_cast<LONG>(std::lround(kWidthDip * m_scale));
        const LONG ht = static_cast<LONG>(std::lround((kHeightDip + kDetailDip) * m_scale));   // the tallest it gets
        MONITORINFO mi{ sizeof(mi) };
        RECT work{ h.centre.x - 2000, h.centre.y - 2000, h.centre.x + 2000, h.centre.y + 2000 };
        if (h.monitor && GetMonitorInfoW(h.monitor, &mi))
            work = mi.rcWork;
        const LONG gap = static_cast<LONG>(std::lround(kGapDip * m_scale));
        const LONG reach = static_cast<LONG>(std::lround(h.radius));
        m_above = h.centre.y + reach + gap + ht > work.bottom;
        m_edgeY = m_above ? std::max(work.top + ht, h.centre.y - reach - gap) : std::min(h.centre.y + reach + gap, work.bottom - ht);
        m_left = std::clamp(h.centre.x - w / 2, work.left, std::max(work.left, work.right - w));
        Fit();
    }

    void PreviewCard::Fit()
    {
        const LONG pad = static_cast<LONG>(std::lround(kShadowDip * m_scale));
        const LONG w = static_cast<LONG>(std::lround(kWidthDip * m_scale));
        const LONG ht = static_cast<LONG>(std::lround((kHeightDip + (m_current.detail.empty() ? 0.f : kDetailDip)) * m_scale));
        m_size = SIZE{ w + 2 * pad, ht + 2 * pad };
        m_origin = POINT{ m_left - pad, (m_above ? m_edgeY - ht : m_edgeY) - pad };
    }

    bool PreviewCard::EnsureFormats()
    {
        if (m_titleFont && m_fontScale == m_scale)
            return true;
        m_fontScale = m_scale;
        constexpr auto L = DWRITE_TEXT_ALIGNMENT_LEADING, R = DWRITE_TEXT_ALIGNMENT_TRAILING;
        m_titleFont = MakeFormat(m_textFamily, 12.25f, DWRITE_FONT_WEIGHT_BOLD, L);
        m_sizeFont = MakeFormat(m_textFamily, 12.25f, DWRITE_FONT_WEIGHT_NORMAL, R);
        m_lineFont = MakeFormat(m_textFamily, 11.f, DWRITE_FONT_WEIGHT_NORMAL, L);
        m_smallFont = MakeFormat(m_textFamily, 11.f, DWRITE_FONT_WEIGHT_NORMAL, L);
        return m_titleFont && m_sizeFont && m_lineFont && m_smallFont;
    }

    void PreviewCard::Render()
    {
        if (!m_hwnd || !m_canvas || !EnsureFormats())
            return;
        m_rt = m_canvas->Begin(m_size);
        if (!m_rt)
            return;
        const float s = m_scale;
        const float pad0 = std::round(kShadowDip * s);
        const float w = static_cast<float>(m_size.cx) - 2.f * pad0, h = static_cast<float>(m_size.cy) - 2.f * pad0;
        const auto& pal = m_palette;
        m_rt->SetTransform(D2D1::Matrix3x2F::Translation(pad0, pad0));
        // A soft shadow (a few growing, fading copies, as the top panel), then the card.
        const float radius = kCornerDip * s;
        for (int k = 5; k >= 1; --k)
        {
            const float g = 2.2f * s * static_cast<float>(k);
            m_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(-g, -g + 5.f * s, w + g, h + g + 5.f * s), radius + g, radius + g),
                                       Brush(pal.shadow, 0.04f).Get());
        }
        const auto card = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), radius, radius);
        m_rt->FillRoundedRectangle(card, Brush(pal.panel).Get());
        m_rt->DrawRoundedRectangle(card, Brush(pal.panelLine).Get(), 1.f);

        // The source's thumbnail, through the recipe's rotate / flip so it looks like the result; the
        // brand's file mark until there is one (or for a file without a picture).
        const float pad = kPadDip * s, bw = kThumbWDip * s, bh = kThumbHDip * s;
        const auto boxRect = D2D1::RectF(pad, (h - bh) / 2.f, pad + bw, (h + bh) / 2.f);
        const auto& thumb = m_current.thumb;
        if (thumb && (thumb.get() != m_thumbFor || m_thumbGeneration != m_canvas->DeviceGeneration() || !m_thumbBitmap))
        {
            m_thumbBitmap = ToBitmap(m_rt, *thumb);
            m_thumbFor = thumb.get();
            m_thumbGeneration = m_canvas->DeviceGeneration();
        }
        if (thumb && m_thumbBitmap)
        {
            const auto& o = m_current.orientation;
            const float tw = static_cast<float>(thumb->w), th = static_cast<float>(thumb->h);
            // Fit what will be seen (sides swapped by a quarter turn) into the box.
            const float k = std::min(bw / (o.SwapsSides() ? th : tw), bh / (o.SwapsSides() ? tw : th));
            const D2D1_POINT_2F c{ (boxRect.left + boxRect.right) / 2.f, (boxRect.top + boxRect.bottom) / 2.f };
            const auto dest = D2D1::RectF(c.x - tw * k / 2.f, c.y - th * k / 2.f, c.x + tw * k / 2.f, c.y + th * k / 2.f);
            // x' = a x + b y, y' = c x + d y about the box centre.
            const D2D1::Matrix3x2F turn(static_cast<float>(o.a), static_cast<float>(o.c), static_cast<float>(o.b), static_cast<float>(o.d), 0.f, 0.f);
            m_rt->SetTransform(D2D1::Matrix3x2F::Translation(-c.x, -c.y) * turn * D2D1::Matrix3x2F::Translation(c.x + pad0, c.y + pad0));
            m_rt->DrawBitmap(m_thumbBitmap.Get(), dest, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            m_rt->SetTransform(D2D1::Matrix3x2F::Translation(pad0, pad0));
        }
        else
        {
            DrawFileMark(boxRect, pal, false);
        }

        // "WebP   1.2 MB · was 3.4 MB" / the output name / what changes.
        const float x0 = pad + bw + kThumbGapDip * s, x1 = w - pad;
        const float top = (h - (m_current.detail.empty() ? 34.f : 48.f) * s) / 2.f;
        const std::wstring format = m_current.format.empty() ? m_label : m_current.format;
        std::wstring size = m_current.size.empty() ? std::wstring(m_current.final ? L"" : L"estimating\u2026") : m_current.size;
        if (!m_current.was.empty() && !m_current.size.empty())
            size += L" \u00b7 " + m_current.was;
        const float sizeWidth = (x1 - x0) * 0.62f;
        Text(format, m_titleFont.Get(), D2D1::RectF(x0, top, x1 - sizeWidth - 6.f * s, top + 18.f * s), pal.panelText);
        Text(size, m_sizeFont.Get(), D2D1::RectF(x1 - sizeWidth, top, x1, top + 18.f * s), pal.panelTextDim);
        Text(m_current.name, m_lineFont.Get(), D2D1::RectF(x0, top + 18.f * s, x1, top + 34.f * s), pal.panelText);
        Text(m_current.detail, m_smallFont.Get(), D2D1::RectF(x0, top + 34.f * s, x1, top + 48.f * s), pal.panelTextDim);
        m_rt->SetTransform(D2D1::Matrix3x2F::Identity());

        m_canvas->End(m_hwnd.get(), m_origin, 1.f);
        m_rt = nullptr;
    }
}
