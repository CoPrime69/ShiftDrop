#include "DragImage.h"
#include "Log.h"

#include <shlguid.h>   // CLSID_DragDropHelper
#include <shobjidl.h>
#include <wil/resource.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace sd
{
    namespace
    {
        constexpr UINT kDdwmUpdateWindow = WM_USER + 3;   // DDWM_UPDATEWINDOW: the drag window re-reads its picture
        constexpr float kThumbDip = 96.f;
        constexpr float kNameMaxDip = 150.f;
    }

    void SetDropDescription(IDataObject* data, DROPIMAGETYPE type, const wchar_t* message)
    {
        if (!data)
            return;
        static const auto format = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_DROPDESCRIPTION));
        wil::unique_hglobal memory(GlobalAlloc(GHND, sizeof(DROPDESCRIPTION)));
        if (!memory)
            return;
        auto* description = static_cast<DROPDESCRIPTION*>(GlobalLock(memory.get()));
        if (!description)
            return;
        description->type = type;
        wcsncpy_s(description->szMessage, message, _TRUNCATE);
        GlobalUnlock(memory.get());
        FORMATETC fmt{ format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM medium{};
        medium.tymed = TYMED_HGLOBAL;
        medium.hGlobal = memory.get();
        if (SUCCEEDED(data->SetData(&fmt, &medium, TRUE)))   // fRelease: the data object owns it now
            memory.release();
    }

    bool ReplaceDragImage(IDataObject* data, const theme::Pixels& thumb, const std::wstring& name, float scale,
                          ID2D1Factory* d2d, IDWriteFactory* dwrite, const std::wstring& fontFamily)
    {
        if (!data || thumb.px.empty() || !d2d || !dwrite)
            return false;

        // Layout: the thumbnail fitted in a square, the name centred under it on a dark pill.
        const float box = kThumbDip * scale;
        const float k = std::min(box / static_cast<float>(thumb.w), box / static_cast<float>(thumb.h));
        const float tw = std::round(static_cast<float>(thumb.w) * k), th = std::round(static_cast<float>(thumb.h) * k);
        ComPtr<IDWriteTextFormat> format;
        ComPtr<IDWriteTextLayout> layout;
        DWRITE_TEXT_METRICS metrics{};
        const float padX = 6.f * scale, padY = 2.f * scale, gap = 4.f * scale, maxW = kNameMaxDip * scale;
        if (!name.empty())
        {
            if (FAILED(dwrite->CreateTextFormat(fontFamily.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                DWRITE_FONT_STRETCH_NORMAL, 12.f * scale, L"", &format)))
                return false;
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            ComPtr<IDWriteInlineObject> ellipsis;
            if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(format.Get(), &ellipsis)))
                format->SetTrimming(&trimming, ellipsis.Get());
            if (FAILED(dwrite->CreateTextLayout(name.c_str(), static_cast<UINT32>(name.size()), format.Get(), maxW, 40.f * scale, &layout)))
                return false;
            layout->GetMetrics(&metrics);
        }
        const float labelW = layout ? std::min(metrics.width, maxW) + 2.f * padX : 0.f;
        const float labelH = layout ? metrics.height + 2.f * padY : 0.f;
        const int w = static_cast<int>(std::ceil(std::max(tw, labelW))) + 2;
        const int h = static_cast<int>(std::ceil(th + (layout ? gap + labelH : 0.f))) + 2;

        // A top-down 32-bit DIB that Direct2D draws into through a memory DC.
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        wil::unique_hbitmap dib(CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0));
        wil::unique_hdc dc(CreateCompatibleDC(nullptr));
        if (!dib || !dc)
            return false;
        std::memset(bits, 0, static_cast<size_t>(w) * h * 4);
        {
            auto select = wil::SelectObject(dc.get(), dib.get());
            ComPtr<ID2D1DCRenderTarget> rt;
            const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            const RECT rc{ 0, 0, w, h };
            if (FAILED(d2d->CreateDCRenderTarget(&props, &rt)) || FAILED(rt->BindDC(dc.get(), &rc)))
                return false;
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);   // ClearType needs an opaque background
            rt->BeginDraw();
            rt->Clear(D2D1::ColorF(0, 0, 0, 0));
            const float cx = static_cast<float>(w) / 2.f;
            if (auto bitmap = theme::ToBitmap(rt.Get(), thumb))
                rt->DrawBitmap(bitmap.Get(), D2D1::RectF(cx - tw / 2.f, 1.f, cx + tw / 2.f, 1.f + th), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            if (layout)
            {
                ComPtr<ID2D1SolidColorBrush> pill, text;
                rt->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.08f, 0.09f, 0.82f), &pill);
                rt->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.96f), &text);
                const float top = 1.f + th + gap;
                if (pill && text)
                {
                    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - labelW / 2.f, top, cx + labelW / 2.f, top + labelH), 4.f * scale, 4.f * scale),
                                             pill.Get());
                    // The layout is maxW wide and centres its line.
                    rt->DrawTextLayout(D2D1::Point2F(cx - maxW / 2.f, top + padY - metrics.top), layout.Get(), text.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
            if (FAILED(rt->EndDraw()))
                return false;
        }
        GdiFlush();

        ComPtr<IDragSourceHelper> helper;
        if (FAILED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper))))
            return false;
        // The pointer sits on the middle of the thumbnail.
        SHDRAGIMAGE image{ SIZE{ w, h }, POINT{ w / 2, 1 + static_cast<LONG>(th / 2.f) }, dib.get(), CLR_NONE };
        const HRESULT hr = helper->InitializeFromBitmap(&image, data);
        if (FAILED(hr))
        {
            log::Info(L"drag image: InitializeFromBitmap failed (0x{:08X}); keeping the original", static_cast<unsigned>(hr));
            return false;
        }
        dib.release();   // the drag-image manager owns the bitmap now

        // Ask the drag window (its handle is in the data object) to show the new picture.
        static const auto windowFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"DragWindow"));
        FORMATETC fmt{ windowFormat, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM medium{};
        if (SUCCEEDED(data->GetData(&fmt, &medium)))
        {
            if (const auto* handle = static_cast<const DWORD*>(GlobalLock(medium.hGlobal)))
            {
                // Stored as 32 bits so 32- and 64-bit processes agree; window handles fit in 32 bits.
                const HWND window = static_cast<HWND>(ULongToHandle(*handle));
                GlobalUnlock(medium.hGlobal);
                PostMessageW(window, kDdwmUpdateWindow, 0, 0);
            }
            ReleaseStgMedium(&medium);
        }
        return true;
    }
}
