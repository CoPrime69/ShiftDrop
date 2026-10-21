#include "photo/Compose.h"
#include "ErrorText.h"
#include "Log.h"
#include "pdf/FontSubset.h"
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
        constexpr float kNoLimit = 100000.f;   // a layout width or height that never binds

        ComPtr<IDWriteTextLayout> MakeLayout(const std::wstring& text, const wchar_t* family, int weight, bool italic, double fontSize, float maxWidth,
                                             float maxHeight, bool wrap, DWRITE_TEXT_ALIGNMENT align, bool middle)
        {
            ComPtr<IDWriteTextFormat> format;
            ComPtr<IDWriteTextLayout> layout;
            auto* dw = WriteFactory();
            if (!dw || FAILED(dw->CreateTextFormat(family, nullptr, static_cast<DWRITE_FONT_WEIGHT>(std::clamp(weight, 1, 999)),
                                                   italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                   float(std::max(1.0, fontSize)), L"", &format)))
                return layout;
            // Words longer than the box break where they must (DWRITE_WORD_WRAPPING_EMERGENCY_BREAK, Windows 8.1).
            if (!wrap || FAILED(format->SetWordWrapping(static_cast<DWRITE_WORD_WRAPPING>(2))))
                format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
            format->SetTextAlignment(align);
            if (middle)
                format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            dw->CreateTextLayout(text.c_str(), UINT32(text.size()), format.Get(), maxWidth, maxHeight, &layout);
            return layout;
        }

        DWRITE_TEXT_ALIGNMENT AlignOf(Align a)
        {
            return a == Align::Centre ? DWRITE_TEXT_ALIGNMENT_CENTER : a == Align::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING;
        }

        // A text, callout or number mark's text as it is drawn: the layout, its top-left corner, the
        // ink and, for a callout, the box it is clipped to.
        struct MarkText
        {
            ComPtr<IDWriteTextLayout> layout;
            PointD origin;
            uint32_t ink = 0;
            RectD clip;   // empty: not clipped
            SizeD measured;
        };

        bool MakeMarkText(const Annotation& m, const Annotations& marks, MarkText& out)
        {
            switch (m.kind)
            {
            case Mark::Text:
            {
                const bool wrap = m.wrap;
                const float width = wrap ? float(std::max(MinTextWidth(m), m.b.x - m.a.x)) : kNoLimit;
                out.layout = MakeLayout(m.text, FontOf(m), m.weight, m.italic, m.fontSize, width, kNoLimit, wrap, AlignOf(m.align), false);
                if (!out.layout)
                    return false;
                if (m.underline)
                    out.layout->SetUnderline(TRUE, { 0, UINT32(m.text.size()) });
                DWRITE_TEXT_METRICS tm{};
                out.layout->GetMetrics(&tm);
                const double textWidth = std::max<double>(tm.widthIncludingTrailingWhitespace, m.fontSize * 0.3);
                if (!wrap && m.align != Align::Left)
                    out.layout->SetMaxWidth(float(textWidth));   // lines line up within the text's own width
                out.measured = { wrap ? double(width) : textWidth, tm.height };
                out.origin = m.a;
                out.ink = m.fill ? ContrastingText(m.color) : m.color;
                return true;
            }
            case Mark::Callout:
            {
                const RectD box = canvas::FromCorners(m.a, m.b);
                if (box.w < 1 || box.h < 1)
                    return false;
                const double pad = std::min(box.w, box.h) * 0.12;
                const float tw = float(std::max(1.0, box.w - 2 * pad)), th = float(std::max(1.0, box.h - 2 * pad));
                out.layout = MakeLayout(m.text, L"Segoe UI", 600, false, m.fontSize, tw, th, true, DWRITE_TEXT_ALIGNMENT_CENTER, true);
                if (!out.layout)
                    return false;
                out.origin = { box.x + pad, box.y + pad };
                out.clip = { box.x + pad, box.y + pad, tw, th };
                out.ink = ContrastingText(m.color);
                out.measured = { box.w, box.h };
                return true;
            }
            case Mark::Number:
            {
                const double r = m.fontSize * 0.75;
                const float side = float(2 * r);
                out.layout = MakeLayout(std::to_wstring(marks.NumberOf(m.id)), L"Segoe UI", 600, false, m.fontSize * 0.9, side, side, false,
                                        DWRITE_TEXT_ALIGNMENT_CENTER, true);
                if (!out.layout)
                    return false;
                out.origin = { m.a.x - r, m.a.y - r };
                out.ink = ContrastingText(m.color);
                out.measured = { side, side };
                return true;
            }
            default: return false;
            }
        }

        void DrawMarkText(ID2D1DeviceContext* dc, const MarkText& t)
        {
            auto ink = Brush(dc, Rgb(t.ink));
            if (t.layout && ink)
                dc->DrawTextLayout(Pt(t.origin), t.layout.Get(), ink.Get(), t.clip.Empty() ? D2D1_DRAW_TEXT_OPTIONS_NONE : D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        ComPtr<ID2D1PathGeometry> Polyline(ID2D1DeviceContext* dc, const std::vector<PointD>& pts)
        {
            ComPtr<ID2D1PathGeometry> path;
            ComPtr<ID2D1GeometrySink> sink;
            if (pts.empty() || FAILED(FactoryOf(dc)->CreatePathGeometry(&path)) || FAILED(path->Open(&sink)))
                return nullptr;
            sink->BeginFigure(Pt(pts[0]), D2D1_FIGURE_BEGIN_HOLLOW);
            // Smoothed through the midpoints, so a quick stroke doesn't look like a polygon.
            for (const auto& seg : SmoothStroke(pts))
                sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Pt(seg.control), Pt(seg.end)));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            return path;
        }

        ComPtr<ID2D1PathGeometry> Polygon(ID2D1DeviceContext* dc, const PointD* pts, size_t n)
        {
            ComPtr<ID2D1PathGeometry> path;
            ComPtr<ID2D1GeometrySink> sink;
            if (n < 2 || FAILED(FactoryOf(dc)->CreatePathGeometry(&path)) || FAILED(path->Open(&sink)))
                return nullptr;
            sink->BeginFigure(Pt(pts[0]), D2D1_FIGURE_BEGIN_FILLED);
            for (size_t i = 1; i < n; ++i)
                sink->AddLine(Pt(pts[i]));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            return path;
        }

        void DrawArrow(ID2D1DeviceContext* dc, const Annotation& m, ID2D1Brush* brush, ID2D1StrokeStyle* round)
        {
            if (std::hypot(m.b.x - m.a.x, m.b.y - m.a.y) < 0.5)
                return;
            const ArrowShape shape = ArrowGeometry(m);
            dc->DrawLine(Pt(m.a), Pt(shape.lineEnd), brush, float(m.width), round);
            if (auto head = Polygon(dc, shape.head, 3))
            {
                dc->FillGeometry(head.Get(), brush);
                dc->DrawGeometry(head.Get(), brush, float(shape.headStroke), round);
            }
        }

        void DrawCallout(ID2D1DeviceContext* dc, const Annotation& m, Annotations& marks)
        {
            const RectD box = canvas::FromCorners(m.a, m.b);
            if (box.w < 1 || box.h < 1)
                return;
            auto fill = Brush(dc, Rgb(m.color));
            const float r = float(CalloutRadius(m));
            // The tail first, so the box covers its base.
            PointD tail[3];
            CalloutTail(m, tail);
            if (auto path = Polygon(dc, tail, 3))
                dc->FillGeometry(path.Get(), fill.Get());
            dc->FillRoundedRectangle(D2D1::RoundedRect(Rect(box), r, r), fill.Get());
            MarkText text;
            if (!MakeMarkText(m, marks, text))
                return;
            DrawMarkText(dc, text);
            marks.SetMeasured(m.id, text.measured);
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

        void DrawMark(ID2D1DeviceContext* dc, unsigned device, const Annotation& m, const Picture& photo, Annotations& marks,
                      const std::map<std::wstring, Picture>& images, Bitmaps& bitmaps, ID2D1StrokeStyle* round)
        {
            auto brush = Brush(dc, Rgb(m.color));
            if (!brush)
                return;
            switch (m.kind)
            {
            case Mark::Arrow: DrawArrow(dc, m, brush.Get(), round); break;
            case Mark::Rect:
            {
                const auto r = Rect(canvas::FromCorners(m.a, m.b));
                if (m.fill)
                    dc->FillRectangle(r, brush.Get());
                else
                    dc->DrawRectangle(r, brush.Get(), float(m.width), round);
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
                MarkText text;
                if (!MakeMarkText(m, marks, text))
                    break;
                marks.SetMeasured(m.id, text.measured);
                if (m.fill)
                {
                    const double pad = m.fontSize * 0.3;
                    const RectD box = marks.TextBox(m);
                    const RectD back{ box.x - pad, box.y - pad, box.w + 2 * pad, box.h + 2 * pad };
                    dc->FillRoundedRectangle(D2D1::RoundedRect(Rect(back), float(pad), float(pad)), brush.Get());
                }
                if (!m.text.empty())
                    DrawMarkText(dc, text);
                break;
            }
            case Mark::Callout: DrawCallout(dc, m, marks); break;
            case Mark::Number:
            {
                const double r = m.fontSize * 0.75;
                dc->FillEllipse(D2D1::Ellipse(Pt(m.a), float(r), float(r)), brush.Get());
                auto ring = Brush(dc, D2D1::ColorF(1, 1, 1, 0.9f));
                dc->DrawEllipse(D2D1::Ellipse(Pt(m.a), float(r), float(r)), ring.Get(), float(std::max(1.0, r * 0.08)));
                MarkText text;
                if (MakeMarkText(m, marks, text))
                    DrawMarkText(dc, text);
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
                    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), kHighlighterOpacity),
                                  nullptr);
                dc->DrawGeometry(path.Get(), brush.Get(), float(m.width), round);
                if (marker)
                    dc->PopLayer();
                break;
            }
            case Mark::Check:
            case Mark::Cross:
            {
                double width = 1;
                for (const auto& stroke : CheckStrokes(m, width))
                {
                    // Straight segments with round caps and joins.
                    ComPtr<ID2D1PathGeometry> path;
                    ComPtr<ID2D1GeometrySink> sink;
                    if (FAILED(FactoryOf(dc)->CreatePathGeometry(&path)) || FAILED(path->Open(&sink)))
                        break;
                    sink->BeginFigure(Pt(stroke[0]), D2D1_FIGURE_BEGIN_HOLLOW);
                    for (size_t i = 1; i < stroke.size(); ++i)
                        sink->AddLine(Pt(stroke[i]));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    sink->Close();
                    dc->DrawGeometry(path.Get(), brush.Get(), float(width), round);
                }
                break;
            }
            }
        }
    }

    bool LayOutMarkText(const Annotation& m, const Annotations& marks, TextLayoutInfo& info, PointD& origin, SizeD* measured)
    {
        MarkText text;
        if (!MakeMarkText(m, marks, text))
            return false;
        info = DescribeTextLayout(text.layout.Get(), m.kind == Mark::Number ? std::to_wstring(marks.NumberOf(m.id)) : m.text);
        origin = text.origin;
        if (measured)
            *measured = text.measured;
        return true;
    }

    TextLayoutInfo DescribeTextLayout(IDWriteTextLayout* layout, const std::wstring& text)
    {
        TextLayoutInfo info;
        if (!layout)
            return info;
        DWRITE_TEXT_METRICS tm{};
        layout->GetMetrics(&tm);
        info.width = tm.widthIncludingTrailingWhitespace;
        info.height = tm.height;
        UINT32 count = 0;
        layout->GetLineMetrics(nullptr, 0, &count);
        std::vector<DWRITE_LINE_METRICS> lines(count);
        if (count && FAILED(layout->GetLineMetrics(lines.data(), count, &count)))
            lines.clear();
        const size_t n = text.size();
        info.caretX.assign(n + 1, 0.0);
        for (size_t pos = 0; pos <= n; ++pos)
        {
            FLOAT x = 0, y = 0;
            DWRITE_HIT_TEST_METRICS hit{};
            // The end of a text that doesn't end in a line break is the last character's trailing edge.
            const bool trailing = pos == n && n > 0 && text[n - 1] != L'\n';
            if (SUCCEEDED(layout->HitTestTextPosition(UINT32(trailing ? n - 1 : pos), trailing, &x, &y, &hit)))
                info.caretX[pos] = x;
        }
        size_t start = 0;
        double top = 0;
        for (const auto& lm : lines)
        {
            TextLine line;
            line.start = std::min(start, n);
            line.end = std::min(start + lm.length - lm.newlineLength, n);
            line.top = top;
            line.height = lm.height;
            line.left = line.right = info.caretX[line.start];
            if (line.end > line.start)
            {
                UINT32 rects = 0;
                layout->HitTestTextRange(UINT32(line.start), UINT32(line.end - line.start), 0, 0, nullptr, 0, &rects);
                std::vector<DWRITE_HIT_TEST_METRICS> hits(rects);
                if (rects && SUCCEEDED(layout->HitTestTextRange(UINT32(line.start), UINT32(line.end - line.start), 0, 0, hits.data(), rects, &rects)))
                {
                    line.left = hits[0].left;
                    line.right = hits[0].left + hits[0].width;
                    for (const auto& h : hits)
                    {
                        line.left = std::min<double>(line.left, h.left);
                        line.right = std::max<double>(line.right, h.left + h.width);
                    }
                }
            }
            info.lines.push_back(line);
            start += lm.length;
            top += lm.height;
        }
        if (info.lines.empty())
            info.lines.push_back({ 0, 0, 0, tm.height, 0, 0 });
        return info;
    }

    const std::vector<std::wstring>& SystemFontFamilies()
    {
        static std::vector<std::wstring> families;
        static std::once_flag once;
        std::call_once(once, [] {
            ComPtr<IDWriteFontCollection> collection;
            auto* dw = WriteFactory();
            if (!dw || FAILED(dw->GetSystemFontCollection(&collection, FALSE)))
                return;
            wchar_t locale[LOCALE_NAME_MAX_LENGTH] = L"";
            GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
            for (UINT32 i = 0; i < collection->GetFontFamilyCount(); ++i)
            {
                ComPtr<IDWriteFontFamily> family;
                ComPtr<IDWriteLocalizedStrings> names;
                if (FAILED(collection->GetFontFamily(i, &family)) || FAILED(family->GetFamilyNames(&names)))
                    continue;
                // The family's name as DirectWrite finds it again: English where it has one.
                UINT32 index = 0;
                BOOL exists = FALSE;
                names->FindLocaleName(L"en-us", &index, &exists);
                if (!exists)
                    names->FindLocaleName(locale, &index, &exists);
                if (!exists)
                    index = 0;
                UINT32 length = 0;
                if (FAILED(names->GetStringLength(index, &length)))
                    continue;
                std::wstring name(length + 1, L'\0');
                if (FAILED(names->GetString(index, name.data(), length + 1)))
                    continue;
                name.resize(length);
                if (!name.empty() && name[0] != L'@')   // vertical CJK variants
                    families.push_back(std::move(name));
            }
            std::sort(families.begin(), families.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
            families.erase(std::unique(families.begin(), families.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }),
                           families.end());
        });
        return families;
    }

    HRESULT DrawMarks(ID2D1DeviceContext* dc, unsigned device, const D2D1_MATRIX_3X2_F& transform, const Picture& photo, Annotations& marks,
                      const std::map<std::wstring, Picture>& images, Bitmaps& bitmaps, const std::function<bool(const Annotation&)>& only)
    {
        dc->SetTransform(transform);
        auto round = RoundStroke(dc);
        for (const auto& m : marks.Items())
            if (!only || only(m))
                DrawMark(dc, device, m, photo, marks, images, bitmaps, round.Get());
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        return S_OK;
    }

    HRESULT DrawAnnotated(ID2D1DeviceContext* dc, unsigned device, double scale, const Picture& photo, Annotations& marks,
                          const std::map<std::wstring, Picture>& images, Bitmaps& bitmaps)
    {
        if (photo.Empty())
            return E_INVALIDARG;
        const auto transform = D2D1::Matrix3x2F::Scale(float(scale), float(scale));
        dc->SetTransform(transform);
        auto* base = bitmaps.Get(dc, device, photo.raster);
        if (!base)
            return E_OUTOFMEMORY;
        dc->DrawBitmap(base, D2D1::RectF(0, 0, float(photo.width), float(photo.height)), 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
        return DrawMarks(dc, device, transform, photo, marks, images, bitmaps, {});
    }

    // ---- Marks as PDF content -----------------------------------------------------------------------

    namespace
    {
        // Collects what a DirectWrite layout draws (its glyph runs and underlines) instead of drawing
        // it, so a PDF can show the same glyphs at the same places as real text.
        class GlyphCapture final : public IDWriteTextRenderer
        {
        public:
            struct Run
            {
                ComPtr<IDWriteFontFace> face;
                pdf::MarkGlyphs glyphs;
            };
            std::vector<Run> runs;
            std::vector<RectD> lines;   // underlines and strike-throughs

            IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override
            {
                if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) || riid == __uuidof(IDWriteTextRenderer))
                {
                    *out = static_cast<IDWriteTextRenderer*>(this);
                    return S_OK;
                }
                *out = nullptr;
                return E_NOINTERFACE;
            }
            IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }    // on the stack: never freed through COM
            IFACEMETHODIMP_(ULONG) Release() override { return 1; }
            IFACEMETHODIMP IsPixelSnappingDisabled(void*, BOOL* disabled) override
            {
                *disabled = TRUE;
                return S_OK;
            }
            IFACEMETHODIMP GetCurrentTransform(void*, DWRITE_MATRIX* m) override
            {
                *m = { 1, 0, 0, 1, 0, 0 };
                return S_OK;
            }
            IFACEMETHODIMP GetPixelsPerDip(void*, FLOAT* ppd) override
            {
                *ppd = 1;
                return S_OK;
            }
            IFACEMETHODIMP DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION* desc,
                                        IUnknown*) override
            {
                if (!run || !run->fontFace || run->glyphCount == 0)
                    return S_OK;
                Run r;
                r.face = run->fontFace;
                auto& g = r.glyphs;
                g.size = run->fontEmSize;
                g.origin = { x, y };
                g.rtl = (run->bidiLevel & 1) != 0;
                const auto sim = run->fontFace->GetSimulations();
                g.bold = (sim & DWRITE_FONT_SIMULATIONS_BOLD) != 0;
                g.oblique = (sim & DWRITE_FONT_SIMULATIONS_OBLIQUE) != 0;
                g.glyphs.assign(run->glyphIndices, run->glyphIndices + run->glyphCount);
                for (UINT32 i = 0; i < run->glyphCount; ++i)
                {
                    g.advances.push_back(run->glyphAdvances ? run->glyphAdvances[i] : 0.0);
                    if (run->glyphOffsets)
                        g.offsets.push_back({ run->glyphOffsets[i].advanceOffset, run->glyphOffsets[i].ascenderOffset });
                }
                // Each cluster's characters go with its first glyph (for copying and searching the text).
                g.text.assign(run->glyphCount, {});
                if (desc && desc->string && desc->clusterMap)
                    for (UINT32 c = 0; c < desc->stringLength; ++c)
                        if (desc->clusterMap[c] < run->glyphCount)
                            g.text[desc->clusterMap[c]].push_back(desc->string[c]);
                runs.push_back(std::move(r));
                return S_OK;
            }
            IFACEMETHODIMP DrawUnderline(void*, FLOAT x, FLOAT y, const DWRITE_UNDERLINE* u, IUnknown*) override
            {
                if (u)
                    Line(x, y, u->width, u->thickness, u->offset, u->readingDirection == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT);
                return S_OK;
            }
            IFACEMETHODIMP DrawStrikethrough(void*, FLOAT x, FLOAT y, const DWRITE_STRIKETHROUGH* s, IUnknown*) override
            {
                if (s)
                    Line(x, y, s->width, s->thickness, s->offset, s->readingDirection == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT);
                return S_OK;
            }
            IFACEMETHODIMP DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }

        private:
            void Line(double x, double y, double width, double thickness, double offset, bool rtl)
            {
                lines.push_back({ rtl ? x - width : x, y + offset, width, std::max(0.1, thickness) });
            }
        };

        // A font face's file, read once per face.
        struct FaceFile
        {
            std::string key;
            std::string bytes;
            uint32_t index = 0;
        };

        bool ReadFace(IDWriteFontFace* face, FaceFile& out)
        {
            UINT32 count = 0;
            if (FAILED(face->GetFiles(&count, nullptr)) || count != 1)
                return false;
            ComPtr<IDWriteFontFile> file;
            ComPtr<IDWriteFontFileLoader> loader;
            ComPtr<IDWriteFontFileStream> stream;
            const void* key = nullptr;
            UINT32 keySize = 0;
            UINT64 size = 0;
            if (FAILED(face->GetFiles(&count, &file)) || FAILED(file->GetReferenceKey(&key, &keySize)) || FAILED(file->GetLoader(&loader)) ||
                FAILED(loader->CreateStreamFromKey(key, keySize, &stream)) || FAILED(stream->GetFileSize(&size)) || size == 0 || size > (256u << 20))
                return false;
            const void* data = nullptr;
            void* context = nullptr;
            if (FAILED(stream->ReadFileFragment(&data, 0, size, &context)))
                return false;
            out.bytes.assign(static_cast<const char*>(data), static_cast<size_t>(size));
            stream->ReleaseFileFragment(context);
            out.index = face->GetIndex();
            out.key = std::string(static_cast<const char*>(key), keySize) + "#" + std::to_string(out.index);
            return true;
        }

        uint32_t ColorOf(const Annotation& m) { return m.color & 0xFFFFFF; }

        pdf::MarkItem PathItem(pdf::MarkPath path)
        {
            pdf::MarkItem item;
            item.kind = pdf::MarkItem::Kind::Path;
            item.path = std::move(path);
            return item;
        }

        pdf::MarkPath Filled(uint32_t rgb, float alpha = 1)
        {
            pdf::MarkPath p;
            p.fill = true;
            p.rgb = rgb;
            p.alpha = alpha;
            return p;
        }

        pdf::MarkPath Stroked(uint32_t rgb, double width, float alpha = 1)
        {
            pdf::MarkPath p;
            p.stroke = true;
            p.rgb = rgb;
            p.width = width;
            p.alpha = alpha;
            return p;
        }

        // Builds the PDF items of the marks, one by one, in order.
        class OverlayBuilder
        {
        public:
            OverlayBuilder(const Picture& page, Annotations& marks, const std::map<std::wstring, Picture>& images, pdf::PageOverlay& out)
                : m_page(page), m_marks(marks), m_images(images), m_out(out)
            {
            }

            HRESULT Add(const Annotation& m, std::wstring& error)
            {
                switch (m.kind)
                {
                case Mark::Arrow:
                {
                    if (std::hypot(m.b.x - m.a.x, m.b.y - m.a.y) < 0.5)
                        return S_OK;
                    const ArrowShape shape = ArrowGeometry(m);
                    auto line = Stroked(ColorOf(m), m.width);
                    line.MoveTo(m.a);
                    line.LineTo(shape.lineEnd);
                    m_out.items.push_back(PathItem(std::move(line)));
                    auto head = Stroked(ColorOf(m), shape.headStroke);
                    head.fill = true;
                    head.MoveTo(shape.head[0]);
                    head.LineTo(shape.head[1]);
                    head.LineTo(shape.head[2]);
                    head.Close();
                    m_out.items.push_back(PathItem(std::move(head)));
                    return S_OK;
                }
                case Mark::Rect:
                case Mark::Ellipse:
                {
                    const RectD r = canvas::FromCorners(m.a, m.b);
                    auto p = m.fill ? Filled(ColorOf(m)) : Stroked(ColorOf(m), m.width);
                    if (m.kind == Mark::Rect)
                        p.Rect(r);
                    else
                        p.Ellipse(r.Centre(), r.w / 2, r.h / 2);
                    m_out.items.push_back(PathItem(std::move(p)));
                    return S_OK;
                }
                case Mark::Text:
                case Mark::Callout:
                case Mark::Number:
                    return AddTextMark(m, error);
                case Mark::Image:
                {
                    const auto it = m_images.find(m.image);
                    if (it == m_images.end() || it->second.Empty())
                        return S_OK;
                    pdf::MarkItem item;
                    item.kind = pdf::MarkItem::Kind::Image;
                    item.image.raster = it->second.raster;
                    item.image.at = canvas::FromCorners(m.a, m.b);
                    m_out.items.push_back(std::move(item));
                    return S_OK;
                }
                case Mark::Pen:
                case Mark::Highlighter:
                {
                    if (m.points.empty())
                        return S_OK;
                    auto p = Stroked(ColorOf(m), m.width, m.kind == Mark::Highlighter ? kHighlighterOpacity : 1.f);
                    p.MoveTo(m.points[0]);
                    const auto segments = SmoothStroke(m.points);
                    for (const auto& s : segments)
                        p.QuadTo(s.control, s.end);
                    if (segments.empty())
                        p.LineTo(m.points[0]);   // a dot: a round cap on a line of no length
                    m_out.items.push_back(PathItem(std::move(p)));
                    return S_OK;
                }
                case Mark::Check:
                case Mark::Cross:
                {
                    double width = 1;
                    for (const auto& stroke : CheckStrokes(m, width))
                    {
                        auto p = Stroked(ColorOf(m), width);
                        p.MoveTo(stroke[0]);
                        for (size_t i = 1; i < stroke.size(); ++i)
                            p.LineTo(stroke[i]);
                        m_out.items.push_back(PathItem(std::move(p)));
                    }
                    return S_OK;
                }
                case Mark::Magnifier:
                {
                    // Pixels of the page itself, enlarged: a picture is all it can be.
                    const double R = m.radius * m.zoom;
                    RectD box = canvas::FromCorners({ m.a.x - m.radius, m.a.y - m.radius }, { m.a.x + m.radius, m.a.y + m.radius });
                    box = Union(box, { m.b.x - R, m.b.y - R, 2 * R, 2 * R });
                    return AddPicture(m, Inflate(box, std::max(4.0, m.width * 2)), 1.0, error);
                }
                }
                return S_OK;
            }

            size_t Pictures() const { return m_pictures; }

        private:
            static RectD Union(const RectD& a, const RectD& b)
            {
                const double l = std::min(a.x, b.x), t = std::min(a.y, b.y);
                return { l, t, std::max(a.Right(), b.Right()) - l, std::max(a.Bottom(), b.Bottom()) - t };
            }
            static RectD Inflate(const RectD& r, double d) { return { r.x - d, r.y - d, r.w + 2 * d, r.h + 2 * d }; }

            HRESULT AddTextMark(const Annotation& m, std::wstring& error)
            {
                MarkText text;
                if (!MakeMarkText(m, m_marks, text))
                    return S_OK;
                if (m.kind == Mark::Text || m.kind == Mark::Callout)
                    m_marks.SetMeasured(m.id, text.measured);
                GlyphCapture capture;
                if (text.layout && FAILED(text.layout->Draw(nullptr, &capture, float(text.origin.x), float(text.origin.y))))
                    capture.runs.clear();
                // Every face must go into the PDF as it is drawn; otherwise the whole mark is a picture.
                std::vector<size_t> fonts;
                for (const auto& run : capture.runs)
                {
                    const size_t f = FontFor(run.face.Get(), m);
                    if (f == SIZE_MAX)
                    {
                        RectD box = m_marks.Bounds(m);
                        if (m.kind == Mark::Callout)
                            box = Union(box, { m.tip.x, m.tip.y, 0, 0 });
                        return AddPicture(m, Inflate(box, std::max(2.0, m.fontSize * 0.2)), 3.0, error);
                    }
                    fonts.push_back(f);
                }
                // The shapes behind the text.
                switch (m.kind)
                {
                case Mark::Text:
                    if (m.fill)
                    {
                        const double pad = m.fontSize * 0.3;
                        const RectD box = m_marks.TextBox(m);
                        auto back = Filled(ColorOf(m));
                        back.RoundedRect({ box.x - pad, box.y - pad, box.w + 2 * pad, box.h + 2 * pad }, pad);
                        m_out.items.push_back(PathItem(std::move(back)));
                    }
                    break;
                case Mark::Callout:
                {
                    PointD tail[3];
                    CalloutTail(m, tail);
                    auto t = Filled(ColorOf(m));
                    t.MoveTo(tail[0]);
                    t.LineTo(tail[1]);
                    t.LineTo(tail[2]);
                    t.Close();
                    m_out.items.push_back(PathItem(std::move(t)));
                    auto box = Filled(ColorOf(m));
                    box.RoundedRect(canvas::FromCorners(m.a, m.b), CalloutRadius(m));
                    m_out.items.push_back(PathItem(std::move(box)));
                    break;
                }
                case Mark::Number:
                {
                    const double r = m.fontSize * 0.75;
                    auto disc = Filled(ColorOf(m));
                    disc.Ellipse(m.a, r, r);
                    m_out.items.push_back(PathItem(std::move(disc)));
                    auto ring = Stroked(0xFFFFFF, std::max(1.0, r * 0.08), 0.9f);
                    ring.Ellipse(m.a, r, r);
                    m_out.items.push_back(PathItem(std::move(ring)));
                    break;
                }
                default: break;
                }
                if (m.kind == Mark::Text && m.text.empty())
                    return S_OK;
                for (size_t i = 0; i < capture.runs.size(); ++i)
                {
                    pdf::MarkItem item;
                    item.kind = pdf::MarkItem::Kind::Glyphs;
                    item.glyphs = std::move(capture.runs[i].glyphs);
                    item.glyphs.font = fonts[i];
                    item.glyphs.rgb = text.ink & 0xFFFFFF;
                    item.clip = text.clip;
                    m_out.items.push_back(std::move(item));
                }
                for (const auto& line : capture.lines)
                {
                    auto p = Filled(text.ink & 0xFFFFFF);
                    p.Rect(line);
                    pdf::MarkItem item = PathItem(std::move(p));
                    item.clip = text.clip;
                    m_out.items.push_back(std::move(item));
                }
                return S_OK;
            }

            // The overlay's font for a face, or SIZE_MAX when it may not or cannot be embedded.
            size_t FontFor(IDWriteFontFace* face, const Annotation& m)
            {
                if (const auto it = m_faces.find(face); it != m_faces.end())
                    return it->second;
                size_t index = SIZE_MAX;
                FaceFile file;
                if (ReadFace(face, file))
                {
                    const auto use = pdf::CheckFont(file.bytes, file.index, m.kind == Mark::Text ? m.weight : 600, m.kind == Mark::Text && m.italic);
                    if (use == pdf::FontUse::Embed)
                    {
                        for (size_t f = 0; f < m_out.fonts.size() && index == SIZE_MAX; ++f)
                            if (m_out.fonts[f].key == file.key)
                                index = f;
                        if (index == SIZE_MAX)
                        {
                            pdf::MarkFont font;
                            font.key = file.key;
                            font.index = file.index;
                            font.file = std::move(file.bytes);
                            std::wstring family = m.kind == Mark::Text ? FontOf(m) : L"Segoe UI";
                            font.name.clear();
                            for (const wchar_t c : family)
                                if (c < 128)
                                    font.name.push_back(char(c));
                            index = m_out.fonts.size();
                            m_out.fonts.push_back(std::move(font));
                        }
                    }
                    else
                        log::Info(L"pages: a text mark's font can't be embedded ({}); it goes in as a picture", static_cast<int>(use));
                }
                m_faces.emplace(face, index);
                return index;
            }

            // One mark drawn as pixels at `scale` times the page picture's resolution, cropped to `box`.
            HRESULT AddPicture(const Annotation& m, RectD box, double scale, std::wstring& error)
            {
                box = Intersect(box, { 0, 0, double(m_page.width), double(m_page.height) });
                if (box.w < 1 || box.h < 1)
                    return S_OK;
                if (!m_dc)
                {
                    const auto device = gpu::Device::Get();
                    m_dc = device ? device->NewContext() : nullptr;
                    if (!m_dc)
                    {
                        error = L"no Direct2D device to draw with";
                        return E_FAIL;
                    }
                }
                const double limit = std::max(256u, gpu::MaxEdge(m_dc.Get()));
                scale = std::min(scale, limit / std::max(box.w, box.h));
                const unsigned w = static_cast<unsigned>(std::ceil(box.w * scale)), h = static_cast<unsigned>(std::ceil(box.h * scale));
                auto pixels = std::make_shared<Raster>();
                const auto transform = D2D1::Matrix3x2F::Translation(float(-box.x), float(-box.y)) * D2D1::Matrix3x2F::Scale(float(scale), float(scale));
                const uint32_t id = m.id;
                const HRESULT hr = gpu::RenderToRaster(m_dc.Get(), w, h, [&](ID2D1DeviceContext* dc) {
                    return DrawMarks(dc, 0, transform, m_page, m_marks, m_images, m_bitmaps, [id](const Annotation& a) { return a.id == id; });
                }, *pixels);
                if (FAILED(hr))
                {
                    error = L"a mark could not be drawn (" + HexCode(hr) + L")";
                    return hr;
                }
                pdf::MarkItem item;
                item.kind = pdf::MarkItem::Kind::Image;
                item.image.raster = std::move(pixels);
                item.image.at = { box.x, box.y, w / scale, h / scale };
                m_out.items.push_back(std::move(item));
                ++m_pictures;
                return S_OK;
            }

            static RectD Intersect(const RectD& a, const RectD& b)
            {
                const double l = std::max(a.x, b.x), t = std::max(a.y, b.y), r = std::min(a.Right(), b.Right()), btm = std::min(a.Bottom(), b.Bottom());
                return r > l && btm > t ? RectD{ l, t, r - l, btm - t } : RectD{};
            }

            const Picture& m_page;
            Annotations& m_marks;
            const std::map<std::wstring, Picture>& m_images;
            pdf::PageOverlay& m_out;
            std::map<IDWriteFontFace*, size_t> m_faces;
            ComPtr<ID2D1DeviceContext> m_dc;
            Bitmaps m_bitmaps;
            size_t m_pictures = 0;
        };
    }

    HRESULT MarksToPdf(const Picture& page, Annotations& marks, const std::map<std::wstring, Picture>& images, pdf::PageOverlay& out, size_t* pictures,
                       std::wstring& error)
    {
        out = {};
        out.width = page.width;
        out.height = page.height;
        if (page.width <= 0 || page.height <= 0)
        {
            error = L"the page has no size";
            return E_INVALIDARG;
        }
        OverlayBuilder builder(page, marks, images, out);
        for (const auto& m : marks.Items())
            if (const HRESULT hr = builder.Add(m, error); FAILED(hr))
                return hr;
        if (pictures)
            *pictures = builder.Pictures();
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
