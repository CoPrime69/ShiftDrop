#pragma once
#include "canvas/Raster.h"
#include "photo/Annotations.h"
#include "photo/TextBox.h"
#include "photo/Effects.h"
#include "photo/Layout.h"

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Drawing the photo tools' results with Direct2D. The editors' live previews and the engines'
// exports call the same functions, the preview at a smaller `scale` (output pixels per full-size
// pixel) and from smaller copies of the pictures, so what the editor shows is what gets saved.
// Every function draws into a context whose target is set and cleared, in full-size coordinates
// (it sets the transform to the scale itself).
namespace sd::photo
{
    using Microsoft::WRL::ComPtr;

    // A picture to draw: the pixels (possibly a reduced copy) and its full size.
    struct Picture
    {
        std::shared_ptr<const canvas::Raster> raster;
        int width = 0, height = 0;   // full size; equal to the raster's for exports
        bool Empty() const { return !raster || raster->Empty(); }
        static Picture Of(std::shared_ptr<const canvas::Raster> r)
        {
            Picture p;
            p.width = r ? static_cast<int>(r->width) : 0;
            p.height = r ? static_cast<int>(r->height) : 0;
            p.raster = std::move(r);
            return p;
        }
    };

    // Device bitmaps of the pictures a result uses, made once per device and kept between renders.
    class Bitmaps
    {
    public:
        ID2D1Bitmap1* Get(ID2D1DeviceContext* dc, unsigned device, const std::shared_ptr<const canvas::Raster>& raster);
        void Clear() { m_map.clear(); }

    private:
        struct Entry
        {
            std::weak_ptr<const canvas::Raster> raster;   // a freed raster's address can come back for a new one
            ComPtr<ID2D1Bitmap1> bitmap;
        };
        ComPtr<ID2D1Device> m_device;
        unsigned m_generation = ~0u;
        std::map<const canvas::Raster*, Entry> m_map;
    };

    IDWriteFactory* WriteFactory();

    // ---- Edit Photos ----------------------------------------------------------------------------
    // The adjusted photo. The GPU effects when the device runs them, else the CPU pipeline on a copy
    // (`cpu` caches it between renders of the same settings).
    struct CpuCopy
    {
        nlohmann::json key;
        std::shared_ptr<canvas::Raster> pixels;
    };
    HRESULT DrawAdjusted(ID2D1DeviceContext* dc, unsigned device, double scale, const Picture& photo, const Adjustments& a, const HazeModel* haze,
                         fx::AdjustGraph& graph, Bitmaps& bitmaps, CpuCopy& cpu);

    // ---- Annotate Photos --------------------------------------------------------------------------
    // The photo with its marks. `images` holds the inserted pictures by path. Text sizes are reported
    // back into `marks`.
    HRESULT DrawAnnotated(ID2D1DeviceContext* dc, unsigned device, double scale, const Picture& photo, Annotations& marks,
                          const std::map<std::wstring, Picture>& images, Bitmaps& bitmaps);
    // Only the marks (those `only` accepts, or all), with `transform` from picture pixels to the
    // target, over whatever the target holds (a PDF page's added layer starts transparent).
    HRESULT DrawMarks(ID2D1DeviceContext* dc, unsigned device, const D2D1_MATRIX_3X2_F& transform, const Picture& photo, Annotations& marks,
                      const std::map<std::wstring, Picture>& images, Bitmaps& bitmaps, const std::function<bool(const Annotation&)>& only);

    // Where a text mark's (a callout's, a number's) lines and caret positions are, laid out exactly
    // as it is drawn; `origin` gets the layout's top-left corner in the picture and `measured` the
    // size Annotations::SetMeasured takes. False for other marks.
    bool LayOutMarkText(const Annotation& m, const Annotations& marks, TextLayoutInfo& info, canvas::PointD& origin, canvas::SizeD* measured);
    TextLayoutInfo DescribeTextLayout(IDWriteTextLayout* layout, const std::wstring& text);
    // The font families installed on this PC (DirectWrite's system collection), sorted, by the names
    // DirectWrite finds them by. Made once.
    const std::vector<std::wstring>& SystemFontFamilies();

    // ---- Add a Background -------------------------------------------------------------------------
    // CPU-made layers of a background result (a blurred backdrop, the shadow), kept between renders.
    struct BackgroundCache
    {
        std::string backdropKey, shadowKey;
        std::shared_ptr<canvas::Raster> backdrop, shadow;
    };
    HRESULT DrawBackground(ID2D1DeviceContext* dc, unsigned device, double scale, const Picture& photo, const BackgroundParams& p,
                           const BackgroundLayout& layout, const Picture& backgroundImage, Bitmaps& bitmaps, BackgroundCache& cache);
    // The shadow's alpha: a rounded rectangle blurred by `sigma`, drawn at reduced size (`scale`
    // pixels per output pixel, set here) and stretched; covers the rectangle grown by 3 sigma.
    canvas::Raster ShadowMask(double width, double height, double radius, double sigma, double opacity, double& scale);
    // A picture made small and blurred (the backdrop): at most 512 pixels, `sigma` in output pixels
    // of an output whose longer side is `outLong`.
    canvas::Raster BlurredBackdrop(const canvas::Raster& source, double sigma, double outLong);

    // ---- Create a Collage -------------------------------------------------------------------------
    HRESULT DrawCollage(ID2D1DeviceContext* dc, unsigned device, double scale, const std::vector<Picture>& pictures, const CollageParams& p,
                        const CollageLayout& layout, Bitmaps& bitmaps);

    // A straight-alpha raster no larger than `maxEdge` (averaged down; shared when already small).
    std::shared_ptr<const canvas::Raster> Reduced(const std::shared_ptr<const canvas::Raster>& source, unsigned maxEdge);
}
