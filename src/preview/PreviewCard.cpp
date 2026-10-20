#include "preview/PreviewCard.h"
#include "Brand.h"
#include "Log.h"

#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;
using namespace sd::theme;

namespace sd
{
    namespace
    {
        constexpr wchar_t kClassName[] = L"" SD_NAME L".Preview";
        constexpr UINT kMsgPrediction = WM_APP + 1;   // lParam: preview::Prediction*, owned by the receiver
        constexpr UINT_PTR kTimerHide = 1;            // a short gap between two slices does not blink the card
        constexpr UINT kHideDelayMs = 120;

        // Card layout, DIPs.
        constexpr float kWidthDip = 324.f;
        constexpr float kHeightDip = 78.f;
        constexpr float kGapDip = 10.f;      // from the wheel's edge
        constexpr float kPadDip = 12.f;
        constexpr float kThumbDip = 54.f;
        constexpr float kCornerDip = 10.f;
        constexpr uint32_t kThumbPx = 128;   // asked of the shell; drawn smaller

        // The shell's thumbnail (Explorer's cache, so usually instant), as premultiplied BGRA. Runs on
        // the predictor's worker thread.
        std::shared_ptr<const preview::Thumbnail> ShellThumbnail(const std::wstring& path)
        {
            ComPtr<IShellItemImageFactory> factory;
            wil::unique_hbitmap bitmap;
            if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory))) ||
                FAILED(factory->GetImage(SIZE{ static_cast<LONG>(kThumbPx), static_cast<LONG>(kThumbPx) }, SIIGBF_RESIZETOFIT, &bitmap)))
                return nullptr;
            BITMAP info{};
            if (!GetObjectW(bitmap.get(), sizeof(info), &info) || info.bmWidth <= 0 || info.bmHeight == 0)
                return nullptr;
            auto thumb = std::make_shared<preview::Thumbnail>();
            thumb->w = static_cast<uint32_t>(info.bmWidth);
            thumb->h = static_cast<uint32_t>(std::abs(info.bmHeight));
            thumb->px.resize(static_cast<size_t>(thumb->w) * thumb->h);
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = info.bmWidth;
            bi.bmiHeader.biHeight = -static_cast<LONG>(thumb->h);   // top-down rows
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            wil::unique_hdc dc(CreateCompatibleDC(nullptr));
            if (!dc || !GetDIBits(dc.get(), bitmap.get(), 0, thumb->h, thumb->px.data(), &bi, DIB_RGB_COLORS))
                return nullptr;
            // Opaque pictures often come with alpha 0 everywhere; ones with alpha may or may not be
            // premultiplied. Fix both so Direct2D gets premultiplied pixels.
            bool anyAlpha = false, straight = false;
            for (const uint32_t p : thumb->px)
            {
                const uint32_t a = p >> 24;
                anyAlpha |= a != 0;
                straight |= ((p >> 16) & 0xFF) > a || ((p >> 8) & 0xFF) > a || (p & 0xFF) > a;
            }
            for (auto& p : thumb->px)
            {
                if (!anyAlpha)
                    p |= 0xFF000000u;
                else if (straight)
                {
                    const uint32_t a = p >> 24;
                    const auto mul = [a](uint32_t c) { return (c * a + 127) / 255; };
                    p = (a << 24) | (mul((p >> 16) & 0xFF) << 16) | (mul((p >> 8) & 0xFF) << 8) | mul(p & 0xFF);
                }
            }
            return thumb;
        }
    }

    PreviewCard::PreviewCard(HINSTANCE instance, std::shared_ptr<const FfmpegEngine> media)
        : m_instance(instance), m_media(std::move(media))
    {
    }

    PreviewCard::~PreviewCard() { Destroy(); }

    bool PreviewCard::Create()
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = m_instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
        // WS_EX_TRANSPARENT: clicks, hovers and drops go straight through to what is underneath.
        m_hwnd.reset(CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                     kClassName, SD_NAME, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, m_instance, this));
        if (!m_hwnd ||
            FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2d.ReleaseAndGetAddressOf())) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(m_dwrite.ReleaseAndGetAddressOf()))))
        {
            log::Error(L"preview: window or Direct2D unavailable; no predictions");
            m_hwnd.reset();
            return false;
        }
        m_canvas = std::make_unique<Canvas>(m_d2d.Get());
        m_family = TextFamily(m_dwrite.Get());
        HWND hwnd = m_hwnd.get();
        m_predictor = std::make_unique<preview::Predictor>(
            m_media,
            [hwnd](preview::Prediction p) {
                auto owned = std::make_unique<preview::Prediction>(std::move(p));
                if (PostMessageW(hwnd, kMsgPrediction, 0, reinterpret_cast<LPARAM>(owned.get())))
                    owned.release();   // the window owns it now
            },
            ShellThumbnail);
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
        Render();
    }

    // Under the wheel, centred; above it when the monitor's work area ends too soon.
    void PreviewCard::Place(const WheelHighlight& h)
    {
        m_scale = h.scale > 0.f ? h.scale : 1.f;
        m_palette = Current(m_appearance.style);
        const LONG w = static_cast<LONG>(std::lround(kWidthDip * m_scale)), ht = static_cast<LONG>(std::lround(kHeightDip * m_scale));
        m_size = SIZE{ w, ht };
        MONITORINFO mi{ sizeof(mi) };
        RECT work{ h.centre.x - 2000, h.centre.y - 2000, h.centre.x + 2000, h.centre.y + 2000 };
        if (h.monitor && GetMonitorInfoW(h.monitor, &mi))
            work = mi.rcWork;
        const LONG gap = static_cast<LONG>(std::lround(kGapDip * m_scale));
        const LONG reach = static_cast<LONG>(std::lround(h.radius));
        LONG y = h.centre.y + reach + gap;
        if (y + ht > work.bottom)
            y = h.centre.y - reach - gap - ht;
        y = std::clamp(y, work.top, std::max(work.top, work.bottom - ht));
        const LONG x = std::clamp(h.centre.x - w / 2, work.left, std::max(work.left, work.right - w));
        m_origin = POINT{ x, y };
    }

    ComPtr<IDWriteTextFormat> PreviewCard::MakeFormat(float dip, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align) const
    {
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(m_dwrite->CreateTextFormat(m_family.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
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

    bool PreviewCard::EnsureFormats()
    {
        if (m_titleFont && m_fontScale == m_scale)
            return true;
        m_fontScale = m_scale;
        m_titleFont = MakeFormat(14.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
        m_sizeFont = MakeFormat(11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
        m_lineFont = MakeFormat(12.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
        m_smallFont = MakeFormat(11.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
        return m_titleFont && m_sizeFont && m_lineFont && m_smallFont;
    }

    void PreviewCard::Text(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color)
    {
        if (text.empty() || !format)
            return;
        ComPtr<ID2D1SolidColorBrush> brush;
        m_rt->CreateSolidColorBrush(color, &brush);
        m_rt->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void PreviewCard::Render()
    {
        if (!m_hwnd || !m_canvas || !EnsureFormats())
            return;
        m_rt = m_canvas->Begin(m_size);
        if (!m_rt)
            return;
        const float s = m_scale;
        const float w = static_cast<float>(m_size.cx), h = static_cast<float>(m_size.cy);
        const auto& pal = m_palette;
        ComPtr<ID2D1SolidColorBrush> panel, line, dim;
        m_rt->CreateSolidColorBrush(pal.panel, &panel);
        m_rt->CreateSolidColorBrush(pal.panelLine, &line);
        m_rt->CreateSolidColorBrush(WithAlpha(pal.panelTextDim, 0.25f), &dim);
        const auto card = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), kCornerDip * s, kCornerDip * s);
        m_rt->FillRoundedRectangle(card, panel.Get());
        m_rt->DrawRoundedRectangle(card, line.Get(), 1.f);

        // Thumbnail, through the recipe's rotate / flip so it looks like the result.
        const float pad = kPadDip * s, box = kThumbDip * s;
        const auto boxRect = D2D1::RectF(pad, (h - box) / 2.f, pad + box, (h + box) / 2.f);
        const auto& thumb = m_current.thumb;
        if (thumb && (thumb.get() != m_thumbFor || m_thumbGeneration != m_canvas->DeviceGeneration() || !m_thumbBitmap))
        {
            m_thumbBitmap.Reset();
            m_rt->CreateBitmap(D2D1::SizeU(thumb->w, thumb->h), thumb->px.data(), thumb->w * 4,
                               D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &m_thumbBitmap);
            m_thumbFor = thumb.get();
            m_thumbGeneration = m_canvas->DeviceGeneration();
        }
        if (thumb && m_thumbBitmap)
        {
            const auto& o = m_current.orientation;
            const float tw = static_cast<float>(thumb->w), th = static_cast<float>(thumb->h);
            // Fit what will be seen (sides swapped by a quarter turn) into the box.
            const float k = std::min(box / (o.SwapsSides() ? th : tw), box / (o.SwapsSides() ? tw : th));
            const D2D1_POINT_2F c{ (boxRect.left + boxRect.right) / 2.f, (boxRect.top + boxRect.bottom) / 2.f };
            const auto dest = D2D1::RectF(c.x - tw * k / 2.f, c.y - th * k / 2.f, c.x + tw * k / 2.f, c.y + th * k / 2.f);
            // x' = a x + b y, y' = c x + d y about the box centre.
            const D2D1::Matrix3x2F turn(static_cast<float>(o.a), static_cast<float>(o.c), static_cast<float>(o.b), static_cast<float>(o.d), 0.f, 0.f);
            m_rt->SetTransform(D2D1::Matrix3x2F::Translation(-c.x, -c.y) * turn * D2D1::Matrix3x2F::Translation(c.x, c.y));
            m_rt->DrawBitmap(m_thumbBitmap.Get(), dest, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            m_rt->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        else
        {
            m_rt->FillRoundedRectangle(D2D1::RoundedRect(boxRect, 6.f * s, 6.f * s), dim.Get());
        }

        // Text: "WebP   ~1.2 MB · was 3.4 MB" / the output name / what changes.
        const float x0 = pad + box + 12.f * s, x1 = w - pad;
        const float top = (h - box) / 2.f - 1.f * s;
        const std::wstring format = m_current.format.empty() ? m_label : m_current.format;
        std::wstring size = m_current.size.empty() ? std::wstring(m_current.final ? L"" : L"estimating…") : m_current.size;
        if (!m_current.was.empty() && !m_current.size.empty())
            size += L"  ·  " + m_current.was;
        const float sizeWidth = (x1 - x0) * 0.62f;
        Text(format, m_titleFont.Get(), D2D1::RectF(x0, top, x1 - sizeWidth - 6.f * s, top + 20.f * s), pal.panelText);
        Text(size, m_sizeFont.Get(), D2D1::RectF(x1 - sizeWidth, top, x1, top + 20.f * s), m_current.size.empty() ? pal.panelTextDim : pal.panelText);
        Text(m_current.name, m_lineFont.Get(), D2D1::RectF(x0, top + 20.f * s, x1, top + 38.f * s), pal.panelText);
        Text(m_current.detail, m_smallFont.Get(), D2D1::RectF(x0, top + 38.f * s, x1, top + 55.f * s), pal.panelTextDim);

        m_canvas->End(m_hwnd.get(), m_origin, 1.f);
        m_rt = nullptr;
    }
}
