#include "photo/Compose.h"
#include "ErrorText.h"
#include "Log.h"
#include "photo/Gpu.h"

#include <d2d1effects.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>

namespace sd::photo
{
    using canvas::Raster;

    namespace
    {
        D2D1_COLOR_F Rgb(uint32_t rgb, float alpha = 1.f)
        {
            return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, alpha);
        }

        D2D1_RECT_F Rect(const RectD& r) { return D2D1::RectF(float(r.x), float(r.y), float(r.Right()), float(r.Bottom())); }
        D2D1_RECT_F Rect(const RectI& r) { return D2D1::RectF(float(r.x), float(r.y), float(r.x + r.w), float(r.y + r.h)); }
        D2D1_POINT_2F Pt(PointD p) { return D2D1::Point2F(float(p.x), float(p.y)); }

        ComPtr<ID2D1Factory> FactoryOf(ID2D1DeviceContext* dc)
        {
            ComPtr<ID2D1Factory> f;
            dc->GetFactory(&f);
            return f;
        }

        ComPtr<ID2D1SolidColorBrush> Brush(ID2D1DeviceContext* dc, const D2D1_COLOR_F& c)
        {
            ComPtr<ID2D1SolidColorBrush> b;
            dc->CreateSolidColorBrush(c, &b);
            return b;
        }

        ComPtr<ID2D1StrokeStyle> RoundStroke(ID2D1DeviceContext* dc)
        {
            ComPtr<ID2D1StrokeStyle> s;
            FactoryOf(dc)->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                         D2D1_LINE_JOIN_ROUND),
                                             nullptr, 0, &s);
            return s;
        }

        // Draws `src` (a part of the bitmap, in its pixels) into `dest`, inside rounded corners of
        // `radius` when there are any.
        void DrawInto(ID2D1DeviceContext* dc, ID2D1Bitmap1* bitmap, const D2D1_RECT_F& dest, const D2D1_RECT_F* src, double radius, float opacity = 1.f)
        {
            ComPtr<ID2D1RoundedRectangleGeometry> clip;
            if (radius > 0.5)
            {
                FactoryOf(dc)->CreateRoundedRectangleGeometry(D2D1::RoundedRect(dest, float(radius), float(radius)), &clip);
                if (clip)
                    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
            }
            dc->DrawBitmap(bitmap, dest, opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, src);
            if (clip)
                dc->PopLayer();
        }

        // Average-down resize of a straight-alpha raster to exactly w x h (w, h no larger than it).
        Raster Resized(const Raster& src, unsigned w, unsigned h)
        {
            Raster out;
            if (src.Empty() || !out.Allocate(std::max(1u, w), std::max(1u, h)))
                return out;
            out.dpiX = src.dpiX;
            out.dpiY = src.dpiY;
            const double fx = double(src.width) / out.width, fy = double(src.height) / out.height;
            for (unsigned y = 0; y < out.height; ++y)
            {
                const unsigned y0 = std::min(src.height - 1, static_cast<unsigned>(y * fy));
                const unsigned y1 = std::max(y0 + 1, std::min(src.height, static_cast<unsigned>(std::ceil((y + 1) * fy))));
                for (unsigned x = 0; x < out.width; ++x)
                {
                    const unsigned x0 = std::min(src.width - 1, static_cast<unsigned>(x * fx));
                    const unsigned x1 = std::max(x0 + 1, std::min(src.width, static_cast<unsigned>(std::ceil((x + 1) * fx))));
                    uint64_t sum[4]{}, n = 0;
                    for (unsigned sy = y0; sy < y1; ++sy)
                        for (unsigned sx = x0; sx < x1; ++sx, ++n)
                        {
                            const uint8_t* p = src.At(sx, sy);
                            for (int c = 0; c < 4; ++c)
                                sum[c] += p[c];
                        }
                    uint8_t* d = out.At(x, y);
                    for (int c = 0; c < 4; ++c)
                        d[c] = static_cast<uint8_t>((sum[c] + n / 2) / n);
                }
            }
            return out;
        }
    }

    std::shared_ptr<const Raster> Reduced(const std::shared_ptr<const Raster>& source, unsigned maxEdge)
    {
        if (!source || source->Empty() || std::max(source->width, source->height) <= maxEdge)
            return source;
        const double s = double(maxEdge) / std::max(source->width, source->height);
        return std::make_shared<Raster>(Resized(*source, std::max(1u, unsigned(std::lround(source->width * s))),
                                                std::max(1u, unsigned(std::lround(source->height * s)))));
    }

    ID2D1Bitmap1* Bitmaps::Get(ID2D1DeviceContext* dc, unsigned device, const std::shared_ptr<const Raster>& raster)
    {
        if (!raster || raster->Empty())
            return nullptr;
        ComPtr<ID2D1Device> current;
        dc->GetDevice(&current);
        if (current != m_device || device != m_generation)
        {
            m_map.clear();
            m_device = current;
            m_generation = device;
        }
        auto& e = m_map[raster.get()];
        if (!e.bitmap || e.raster.lock() != raster)
        {
            HRESULT hr = S_OK;
            e.bitmap = gpu::Upload(dc, *raster, &hr);
            e.raster = raster;
            if (!e.bitmap)
                log::Warn(L"photo: cannot make a {}x{} bitmap ({})", raster->width, raster->height, HexCode(hr));
        }
        return e.bitmap.Get();
    }

    IDWriteFactory* WriteFactory()
    {
        static ComPtr<IDWriteFactory> factory;
        static std::once_flag once;
        std::call_once(once, [] {
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
        });
        return factory.Get();
    }

    // ---- Edit Photos --------------------------------------------------------------------------------

    HRESULT DrawAdjusted(ID2D1DeviceContext* dc, unsigned device, double scale, const Picture& photo, const Adjustments& a, const HazeModel* haze,
                         fx::AdjustGraph& graph, Bitmaps& bitmaps, CpuCopy& cpu)
    {
        if (photo.Empty())
            return E_INVALIDARG;
        const unsigned outW = std::max(1u, unsigned(std::lround(photo.width * scale)));
        const unsigned outH = std::max(1u, unsigned(std::lround(photo.height * scale)));
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        if (graph.dc != dc)
            graph = fx::AdjustGraph{ dc };
        if (!graph.failed)
        {
            auto* bitmap = bitmaps.Get(dc, device, photo.raster);
            if (!bitmap)
                return E_OUTOFMEMORY;
            fx::PrepareContext(dc);
            ComPtr<ID2D1Image> source = bitmap;
            if (photo.raster->width != outW || photo.raster->height != outH)
            {
                // The graph runs at the output's size, so its radii are in output pixels.
                if (!graph.resample && FAILED(dc->CreateEffect(CLSID_D2D12DAffineTransform, &graph.resample)))
                    graph.resample.Reset();
                if (graph.resample)
                {
                    const auto m = D2D1::Matrix3x2F::Scale(float(outW) / photo.raster->width, float(outH) / photo.raster->height);
                    const UINT32 cubic = 5, hard = D2D1_BORDER_MODE_HARD;   // D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC
                    graph.resample->SetValue(0, D2D1_PROPERTY_TYPE_ENUM, reinterpret_cast<const BYTE*>(&cubic), 4);
                    graph.resample->SetValue(1, D2D1_PROPERTY_TYPE_ENUM, reinterpret_cast<const BYTE*>(&hard), 4);
                    graph.resample->SetValue(2, D2D1_PROPERTY_TYPE_MATRIX_3X2, reinterpret_cast<const BYTE*>(&m), sizeof(m));
                    graph.resample->SetInput(0, bitmap);
                    graph.resample->GetOutput(&source);
                }
                else
                    graph.failed = true;
            }
            if (!graph.failed)
                if (auto image = fx::Adjust(dc, graph, source.Get(), outW, outH, photo.width, photo.height, scale, a, haze))
                {
                    dc->DrawImage(image.Get());
                    return S_OK;
                }
        }

        // The CPU path, on a copy at the output's size.
        nlohmann::json key = ToJson(a);
        key["scale"] = scale;
        key["w"] = outW;
        key["haze"] = haze ? reinterpret_cast<uintptr_t>(haze) : 0;
        key["src"] = reinterpret_cast<uintptr_t>(photo.raster.get());
        if (!cpu.pixels || cpu.key != key)
        {
            auto copy = std::make_shared<Raster>(photo.raster->width == outW && photo.raster->height == outH ? *photo.raster
                                                                                                           : Resized(*photo.raster, outW, outH));
            ApplyAdjustments(*copy, a, haze, scale);
            cpu.pixels = std::move(copy);
            cpu.key = std::move(key);
        }
        auto* bitmap = bitmaps.Get(dc, device, cpu.pixels);
        if (!bitmap)
            return E_OUTOFMEMORY;
        dc->DrawBitmap(bitmap, D2D1::RectF(0, 0, float(outW), float(outH)), 1.f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
        return S_OK;
    }

    // ---- Annotate Photos ----------------------------------------------------------------------------

    namespace
    {
        ComPtr<IDWriteTextLayout> TextLayout(const std::wstring& text, double fontSize, float maxWidth, bool wrap, bool centred)
        {
            ComPtr<IDWriteTextFormat> format;
            ComPtr<IDWriteTextLayout> layout;
            auto* dw = WriteFactory();
            if (!dw || FAILED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                   float(fontSize), L"", &format)))
                return layout;
            format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
            if (centred)
            {
                format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
            dw->CreateTextLayout(text.c_str(), UINT32(text.size()), format.Get(), maxWidth, centred ? maxWidth : 100000.f, &layout);
            return layout;
        }

        SizeD Measure(IDWriteTextLayout* layout)
        {
            DWRITE_TEXT_METRICS m{};
            if (!layout || FAILED(layout->GetMetrics(&m)))
                return {};
            return { m.widthIncludingTrailingWhitespace, m.height };
        }

        void Caret(ID2D1DeviceContext* dc, IDWriteTextLayout* layout, size_t length, D2D1_POINT_2F origin, ID2D1Brush* brush, float width)
        {
            FLOAT x = 0, y = 0;
            DWRITE_HIT_TEST_METRICS hit{};
            if (!layout || FAILED(layout->HitTestTextPosition(UINT32(length), length > 0, &x, &y, &hit)))
                return;
            dc->DrawLine(D2D1::Point2F(origin.x + x, origin.y + hit.top), D2D1::Point2F(origin.x + x, origin.y + hit.top + hit.height), brush, width);
        }

        ComPtr<ID2D1PathGeometry> Polyline(ID2D1DeviceContext* dc, const std::vector<PointD>& pts)
        {
            ComPtr<ID2D1PathGeometry> path;
            ComPtr<ID2D1GeometrySink> sink;
            if (pts.empty() || FAILED(FactoryOf(dc)->CreatePathGeometry(&path)) || FAILED(path->Open(&sink)))
                return nullptr;
            sink->BeginFigure(Pt(pts[0]), D2D1_FIGURE_BEGIN_HOLLOW);
            if (pts.size() == 1)
                sink->AddLine(D2D1::Point2F(float(pts[0].x) + 0.01f, float(pts[0].y)));
            // Smoothed through the midpoints, so a quick stroke doesn't look like a polygon.
            for (size_t i = 1; i + 1 < pts.size(); ++i)
            {
                const PointD mid{ (pts[i].x + pts[i + 1].x) / 2, (pts[i].y + pts[i + 1].y) / 2 };
                sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Pt(pts[i]), Pt(mid)));
            }
            if (pts.size() > 1)
                sink->AddLine(Pt(pts.back()));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            return path;
        }

        void DrawArrow(ID2D1DeviceContext* dc, const Annotation& m, ID2D1Brush* brush, ID2D1StrokeStyle* round)
        {
            const double dx = m.b.x - m.a.x, dy = m.b.y - m.a.y, len = std::hypot(dx, dy);
            if (len < 0.5)
                return;
            const double ux = dx / len, uy = dy / len;
            const double head = std::min(len * 0.6, std::max(m.width * 3.6, 10.0)), half = head * 0.55;
            const PointD base{ m.b.x - ux * head * 0.85, m.b.y - uy * head * 0.85 };
            dc->DrawLine(Pt(m.a), Pt(base), brush, float(m.width), round);
            ComPtr<ID2D1PathGeometry> path;
            ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(FactoryOf(dc)->CreatePathGeometry(&path)) || FAILED(path->Open(&sink)))
                return;
            const PointD back{ m.b.x - ux * head, m.b.y - uy * head };
            sink->BeginFigure(Pt(m.b), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(Pt({ back.x - uy * half, back.y + ux * half }));
            sink->AddLine(Pt({ back.x + uy * half, back.y - ux * half }));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            dc->FillGeometry(path.Get(), brush);
            dc->DrawGeometry(path.Get(), brush, float(std::max(1.0, m.width * 0.25)), round);
        }

        void DrawCallout(ID2D1DeviceContext* dc, const Annotation& m, Annotations& marks, uint32_t editing)
        {
            const RectD box = canvas::FromCorners(m.a, m.b);
            if (box.w < 1 || box.h < 1)
                return;
            auto fill = Brush(dc, Rgb(m.color));
            auto ink = Brush(dc, Rgb(ContrastingText(m.color)));
            const double r = std::min({ box.w, box.h, m.fontSize * 1.2 }) * 0.35;
            // The tail: a triangle from inside the box to the tip, drawn first so the box covers its base.
            const PointD c = box.Centre();
            const double dx = m.tip.x - c.x, dy = m.tip.y - c.y, len = std::max(1.0, std::hypot(dx, dy));
            const double half = std::min(box.w, box.h) * 0.16;
            ComPtr<ID2D1PathGeometry> tail;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(FactoryOf(dc)->CreatePathGeometry(&tail)) && SUCCEEDED(tail->Open(&sink)))
            {
                sink->BeginFigure(Pt({ c.x - dy / len * half, c.y + dx / len * half }), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(Pt(m.tip));
                sink->AddLine(Pt({ c.x + dy / len * half, c.y - dx / len * half }));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                dc->FillGeometry(tail.Get(), fill.Get());
            }
            dc->FillRoundedRectangle(D2D1::RoundedRect(Rect(box), float(r), float(r)), fill.Get());
            const double pad = std::min(box.w, box.h) * 0.12;
            const float tw = float(std::max(1.0, box.w - 2 * pad));
            auto layout = TextLayout(m.text, m.fontSize, tw, true, false);
            if (!layout)
                return;
            layout->SetMaxHeight(float(std::max(1.0, box.h - 2 * pad)));
            layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const auto origin = D2D1::Point2F(float(box.x + pad), float(box.y + pad));
            dc->DrawTextLayout(origin, layout.Get(), ink.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            marks.SetMeasured(m.id, { box.w, box.h });
            if (editing == m.id)
                Caret(dc, layout.Get(), m.text.size(), origin, ink.Get(), float(std::max(1.0, m.fontSize / 14)));
        }

        void DrawMagnifier(ID2D1DeviceContext* dc, unsigned device, const Annotation& m, const Picture& photo, Bitmaps& bitmaps, ID2D1Brush* ring)
        {
            auto* bitmap = bitmaps.Get(dc, device, photo.raster);
            const double R = m.radius * m.zoom;
            const float line = float(std::max(1.5, m.width * 0.6));
            dc->DrawEllipse(D2D1::Ellipse(Pt(m.a), float(m.radius), float(m.radius)), ring, line);
            // The line joining the spot to the loupe, edge to edge.
            const double dx = m.b.x - m.a.x, dy = m.b.y - m.a.y, len = std::hypot(dx, dy);
            if (len > m.radius + R)
            {
                const double ux = dx / len, uy = dy / len;
                dc->DrawLine(Pt({ m.a.x + ux * m.radius, m.a.y + uy * m.radius }), Pt({ m.b.x - ux * R, m.b.y - uy * R }), ring, line);
            }
            ComPtr<ID2D1EllipseGeometry> clip;
            const auto loupe = D2D1::Ellipse(Pt(m.b), float(R), float(R));
            if (bitmap && SUCCEEDED(FactoryOf(dc)->CreateEllipseGeometry(loupe, &clip)))
            {
                D2D1_MATRIX_3X2_F outer{};
                dc->GetTransform(&outer);
                dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
                // Picture point p shows at b + (p - a) * zoom.
                const auto zoom = D2D1::Matrix3x2F::Translation(float(-m.a.x), float(-m.a.y)) * D2D1::Matrix3x2F::Scale(float(m.zoom), float(m.zoom)) *
                                  D2D1::Matrix3x2F::Translation(float(m.b.x), float(m.b.y));
                dc->SetTransform(zoom * outer);
                dc->DrawBitmap(bitmap, D2D1::RectF(0, 0, float(photo.width), float(photo.height)), 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
                dc->SetTransform(outer);
                dc->PopLayer();
            }
            dc->DrawEllipse(loupe, ring, float(std::max(2.0, m.width)));
        }
    }

    HRESULT DrawAnnotated(ID2D1DeviceContext* dc, unsigned device, double scale, const Picture& photo, Annotations& marks,
                          const std::map<std::wstring, Picture>& images, Bitmaps& bitmaps, uint32_t editing)
    {
        if (photo.Empty())
            return E_INVALIDARG;
        dc->SetTransform(D2D1::Matrix3x2F::Scale(float(scale), float(scale)));
        auto* base = bitmaps.Get(dc, device, photo.raster);
        if (!base)
            return E_OUTOFMEMORY;
        dc->DrawBitmap(base, D2D1::RectF(0, 0, float(photo.width), float(photo.height)), 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
        auto round = RoundStroke(dc);
        for (const auto& m : marks.Items())
        {
            auto brush = Brush(dc, Rgb(m.color));
            if (!brush)
                return E_OUTOFMEMORY;
            switch (m.kind)
            {
            case Mark::Arrow: DrawArrow(dc, m, brush.Get(), round.Get()); break;
            case Mark::Rect:
            {
                const auto r = Rect(canvas::FromCorners(m.a, m.b));
                if (m.fill)
                    dc->FillRectangle(r, brush.Get());
                else
                    dc->DrawRectangle(r, brush.Get(), float(m.width), round.Get());
                break;
            }
            case Mark::Ellipse:
            {
                const RectD r = canvas::FromCorners(m.a, m.b);
                const auto e = D2D1::Ellipse(Pt(r.Centre()), float(r.w / 2), float(r.h / 2));
                if (m.fill)
                    dc->FillEllipse(e, brush.Get());
                else
                    dc->DrawEllipse(e, brush.Get(), float(m.width));
                break;
            }
            case Mark::Text:
            {
                const std::wstring shown = m.text.empty() && editing == m.id ? std::wstring() : m.text;
                auto layout = TextLayout(shown.empty() ? std::wstring(L" ") : shown, m.fontSize, 100000.f, false, false);
                SizeD size = Measure(layout.Get());
                size.w = std::max(size.w, m.fontSize * 0.3);
                marks.SetMeasured(m.id, size);
                const auto origin = Pt(m.a);
                auto ink = brush;
                if (m.fill)
                {
                    const double pad = m.fontSize * 0.3;
                    const RectD back{ m.a.x - pad, m.a.y - pad, size.w + 2 * pad, size.h + 2 * pad };
                    dc->FillRoundedRectangle(D2D1::RoundedRect(Rect(back), float(pad), float(pad)), brush.Get());
                    ink = Brush(dc, Rgb(ContrastingText(m.color)));
                }
                if (layout && !shown.empty())
                    dc->DrawTextLayout(origin, layout.Get(), ink.Get());
                if (editing == m.id)
                    Caret(dc, layout.Get(), shown.size(), origin, ink.Get(), float(std::max(1.0, m.fontSize / 14)));
                break;
            }
            case Mark::Callout: DrawCallout(dc, m, marks, editing); break;
            case Mark::Number:
            {
                const double r = m.fontSize * 0.75;
                dc->FillEllipse(D2D1::Ellipse(Pt(m.a), float(r), float(r)), brush.Get());
                auto ring = Brush(dc, D2D1::ColorF(1, 1, 1, 0.9f));
                dc->DrawEllipse(D2D1::Ellipse(Pt(m.a), float(r), float(r)), ring.Get(), float(std::max(1.0, r * 0.08)));
                auto ink = Brush(dc, Rgb(ContrastingText(m.color)));
                const float side = float(2 * r);
                auto layout = TextLayout(std::to_wstring(marks.NumberOf(m.id)), m.fontSize * 0.9, side, false, true);
                if (layout)
                    dc->DrawTextLayout(D2D1::Point2F(float(m.a.x - r), float(m.a.y - r)), layout.Get(), ink.Get());
                break;
            }
            case Mark::Magnifier: DrawMagnifier(dc, device, m, photo, bitmaps, brush.Get()); break;
            case Mark::Image:
            {
                const auto it = images.find(m.image);
                if (it == images.end() || it->second.Empty())
                    break;
                if (auto* bmp = bitmaps.Get(dc, device, it->second.raster))
                    dc->DrawBitmap(bmp, Rect(canvas::FromCorners(m.a, m.b)), 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
                break;
            }
            case Mark::Pen:
            case Mark::Highlighter:
            {
                auto path = Polyline(dc, m.points);
                if (!path)
                    break;
                // A highlighter is see-through as a whole, so where it crosses itself it isn't darker.
                const bool marker = m.kind == Mark::Highlighter;
                if (marker)
                    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 0.42f),
                                  nullptr);
                dc->DrawGeometry(path.Get(), brush.Get(), float(m.width), round.Get());
                if (marker)
                    dc->PopLayer();
                break;
            }
            }
        }
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        return S_OK;
    }

    // ---- Add a Background ---------------------------------------------------------------------------

    canvas::Raster ShadowMask(double w, double h, double radius, double sigma, double opacity, double& scale)
    {
        Raster out;
        if (w <= 0 || h <= 0 || sigma <= 0)
            return out;
        // About four mask pixels per sigma: plenty for something this soft.
        scale = std::clamp(4.0 / sigma, 0.005, 1.0);
        const double margin = 3 * sigma;
        const unsigned mw = std::max(1u, unsigned(std::ceil((w + 2 * margin) * scale)));
        const unsigned mh = std::max(1u, unsigned(std::ceil((h + 2 * margin) * scale)));
        Plane alpha(mw, mh);
        const double hx = w / 2, hy = h / 2, r = std::min({ radius, hx, hy });
        for (unsigned y = 0; y < mh; ++y)
            for (unsigned x = 0; x < mw; ++x)
            {
                // Signed distance from the pixel centre to the rounded rectangle, in output pixels.
                const double px = (x + 0.5) / scale - margin - hx, py = (y + 0.5) / scale - margin - hy;
                const double qx = std::abs(px) - (hx - r), qy = std::abs(py) - (hy - r);
                const double d = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - r;
                alpha.At(x, y) = float(std::clamp(0.5 - d * scale, 0.0, 1.0));
            }
        alpha = Blur(alpha, sigma * scale);
        if (!out.Allocate(mw, mh))
            return out;
        for (size_t i = 0; i < alpha.v.size(); ++i)
        {
            uint8_t* p = out.px.data() + i * 4;
            p[0] = p[1] = p[2] = 0;
            p[3] = static_cast<uint8_t>(std::clamp(std::lround(alpha.v[i] * opacity * 255), 0l, 255l));
        }
        return out;
    }

    canvas::Raster BlurredBackdrop(const canvas::Raster& source, double sigma, double outLong)
    {
        auto reduced = Reduced(std::shared_ptr<const Raster>(&source, [](const Raster*) {}), 512);
        Raster out = *reduced;
        const double s = sigma * std::max(out.width, out.height) / std::max(1.0, outLong);
        if (s < 0.3)
            return out;
        const size_t n = static_cast<size_t>(out.width) * out.height;
        for (int c = 0; c < 3; ++c)
        {
            Plane p(out.width, out.height);
            for (size_t i = 0; i < n; ++i)
                p.v[i] = out.px[i * 4 + c];
            p = Blur(p, s);
            for (size_t i = 0; i < n; ++i)
                out.px[i * 4 + c] = static_cast<uint8_t>(std::clamp(std::lround(p.v[i]), 0l, 255l));
        }
        for (size_t i = 0; i < n; ++i)
            out.px[i * 4 + 3] = 255;
        return out;
    }

    HRESULT DrawBackground(ID2D1DeviceContext* dc, unsigned device, double scale, const Picture& photo, const BackgroundParams& p,
                           const BackgroundLayout& layout, const Picture& backgroundImage, Bitmaps& bitmaps, BackgroundCache& cache)
    {
        if (photo.Empty())
            return E_INVALIDARG;
        dc->SetTransform(D2D1::Matrix3x2F::Scale(float(scale), float(scale)));
        const auto canvasRect = D2D1::RectF(0, 0, float(layout.width), float(layout.height));

        // The backdrop.
        switch (p.fill)
        {
        case Fill::Solid:
            dc->FillRectangle(canvasRect, Brush(dc, Rgb(p.color1)).Get());
            break;
        case Fill::Gradient:
        {
            const D2D1_GRADIENT_STOP stops[] = { { 0.f, Rgb(p.color1) }, { 1.f, Rgb(p.color2) } };
            ComPtr<ID2D1GradientStopCollection> collection;
            ComPtr<ID2D1LinearGradientBrush> brush;
            const double t = p.angle * 3.14159265358979 / 180, ux = std::cos(t), uy = std::sin(t);
            const double half = (std::abs(layout.width * ux) + std::abs(layout.height * uy)) / 2;
            const PointD c{ layout.width / 2.0, layout.height / 2.0 };
            if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &collection)) &&
                SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(Pt({ c.x - ux * half, c.y - uy * half }),
                                                                                            Pt({ c.x + ux * half, c.y + uy * half })),
                                                        collection.Get(), &brush)))
                dc->FillRectangle(canvasRect, brush.Get());
            break;
        }
        case Fill::Blur:
        case Fill::Image:
        {
            const Picture& from = p.fill == Fill::Image && !backgroundImage.Empty() ? backgroundImage : photo;
            const double sigma = BackgroundBlurSigma(layout, p.blur);
            std::shared_ptr<const Raster> pixels = from.raster;
            if (sigma > 0.5)
            {
                const std::string key = std::format("{}:{}:{:.1f}:{}x{}", reinterpret_cast<uintptr_t>(from.raster.get()), from.raster->width, sigma,
                                                    layout.width, layout.height);
                if (cache.backdropKey != key || !cache.backdrop)
                {
                    cache.backdrop = std::make_shared<Raster>(BlurredBackdrop(*from.raster, sigma, std::max(layout.width, layout.height)));
                    cache.backdropKey = key;
                }
                pixels = cache.backdrop;
            }
            if (auto* bmp = bitmaps.Get(dc, device, pixels))
            {
                const RectD src = CoverSource({ double(pixels->width), double(pixels->height) }, { double(layout.width), double(layout.height) });
                const auto s = Rect(src);
                dc->DrawBitmap(bmp, canvasRect, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &s);
            }
            if (p.fill == Fill::Blur)   // a little darker, so the photo stands out from its own backdrop
                dc->FillRectangle(canvasRect, Brush(dc, D2D1::ColorF(0, 0, 0, 0.12f)).Get());
            break;
        }
        }

        // The shadow.
        const RectI& pic = layout.picture;
        if (layout.shadowSigma > 0.5)
        {
            const std::string key = std::format("{}x{}:{:.1f}:{:.1f}:{:.2f}", pic.w, pic.h, layout.radius, layout.shadowSigma, layout.shadowOpacity);
            if (cache.shadowKey != key || !cache.shadow)
            {
                double s = 1;
                cache.shadow = std::make_shared<Raster>(ShadowMask(pic.w, pic.h, layout.radius, layout.shadowSigma, layout.shadowOpacity, s));
                cache.shadowKey = key;
            }
            if (auto* bmp = bitmaps.Get(dc, device, cache.shadow))
            {
                const double m = 3 * layout.shadowSigma;
                const auto dest = D2D1::RectF(float(pic.x - m), float(pic.y - m + layout.shadowOffset), float(pic.x + pic.w + m),
                                              float(pic.y + pic.h + m + layout.shadowOffset));
                dc->DrawBitmap(bmp, dest, 1.f, D2D1_INTERPOLATION_MODE_LINEAR, nullptr);
            }
        }

        // The photo, with its corners rounded.
        auto* bmp = bitmaps.Get(dc, device, photo.raster);
        if (!bmp)
            return E_OUTOFMEMORY;
        DrawInto(dc, bmp, Rect(pic), nullptr, layout.radius);
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        return S_OK;
    }

    // ---- Create a Collage ---------------------------------------------------------------------------

    HRESULT DrawCollage(ID2D1DeviceContext* dc, unsigned device, double scale, const std::vector<Picture>& pictures, const CollageParams& p,
                        const CollageLayout& layout, Bitmaps& bitmaps)
    {
        dc->SetTransform(D2D1::Matrix3x2F::Scale(float(scale), float(scale)));
        dc->FillRectangle(D2D1::RectF(0, 0, float(layout.width), float(layout.height)), Brush(dc, Rgb(p.background)).Get());
        for (const auto& cell : layout.cells)
        {
            if (cell.image < 0 || static_cast<size_t>(cell.image) >= pictures.size() || pictures[cell.image].Empty())
                continue;
            const auto& pic = pictures[cell.image];
            auto* bmp = bitmaps.Get(dc, device, pic.raster);
            if (!bmp)
                return E_OUTOFMEMORY;
            const SizeD px{ double(pic.raster->width), double(pic.raster->height) };
            const RectD cellRect{ double(cell.rect.x), double(cell.rect.y), double(cell.rect.w), double(cell.rect.h) };
            if (p.fit)
                DrawInto(dc, bmp, Rect(FitInside(px, cellRect)), nullptr, layout.radius);
            else
            {
                const auto src = Rect(CoverSource(px, { cellRect.w, cellRect.h }));
                DrawInto(dc, bmp, Rect(cellRect), &src, layout.radius);
            }
        }
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        return S_OK;
    }
}
