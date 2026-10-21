#pragma once
#include "Ffmpeg.h"
#include "Overlay.h"
#include "PanelWindow.h"
#include "Recipe.h"
#include "Settings.h"
#include "Theme.h"
#include "preview/Predictor.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wil/resource.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

namespace sd
{
    // The "know before you drop" card: while a slice is highlighted, a small panel under the wheel
    // (above it when there is no room below) says what that slice would make of the files: format,
    // predicted size against the original's, the output name, and what changes (pixel size, metadata
    // removed). The thumbnail is the source's, turned and flipped the way the recipe will.
    //
    // A click-through (WS_EX_TRANSPARENT), non-activating layered window: it never takes the drop,
    // a click or the focus. The numbers come from preview::Predictor on its own thread.
    class PreviewCard : private PanelWindow
    {
    public:
        PreviewCard(HINSTANCE instance, std::shared_ptr<const FfmpegEngine> media);
        ~PreviewCard();
        PreviewCard(const PreviewCard&) = delete;
        PreviewCard& operator=(const PreviewCard&) = delete;

        bool Create();
        void Destroy();
        void SetAppearance(const Appearance& appearance) { m_appearance = appearance; }

        // The wheel's highlight changed. `recipe` is the highlighted slice's (null: none, or a slice
        // with nothing to predict); the card hides then.
        void Update(const WheelHighlight& highlight, const Recipe* recipe);

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        void OnPrediction(std::unique_ptr<preview::Prediction> prediction);
        void Place(const WheelHighlight& highlight);
        void HideNow();
        void Render();
        bool EnsureFormats();

        HINSTANCE m_instance;
        std::shared_ptr<const FfmpegEngine> m_media;
        Appearance m_appearance;
        wil::unique_hwnd m_hwnd;
        std::unique_ptr<preview::Predictor> m_predictor;

        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_titleFont, m_sizeFont, m_lineFont, m_smallFont;

        theme::Palette m_palette{};
        POINT m_origin{};
        SIZE m_size{};
        bool m_shown = false;

        uint64_t m_token = 0;              // the request the card shows
        preview::Prediction m_current;
        std::wstring m_label;              // the slice's label, until the prediction says the format
        Microsoft::WRL::ComPtr<ID2D1Bitmap> m_thumbBitmap;
        const preview::Thumbnail* m_thumbFor = nullptr;
        unsigned m_thumbGeneration = 0;
    };
}
