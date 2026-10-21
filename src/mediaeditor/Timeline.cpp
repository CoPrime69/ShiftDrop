#include "mediaeditor/Timeline.h"
#include "FormatText.h"
#include "Strings.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace sd::mediatools
{
    // ---- Timecodes ------------------------------------------------------------------------------

    std::wstring FormatTimecode(int64_t us)
    {
        return Timecode(us, true);
    }

    std::wstring FormatRulerLabel(int64_t us, int64_t stepUs)
    {
        if (us < 0)
            us = 0;
        const int64_t s = us / 1'000'000;
        std::wstring whole = s >= 3600 ? std::format(L"{}:{:02}:{:02}", s / 3600, s / 60 % 60, s % 60) : std::format(L"{}:{:02}", s / 60, s % 60);
        // Sub-second steps show as many decimals as the step needs.
        const int64_t frac = us % 1'000'000;
        if (stepUs < 100'000)
            return whole + std::format(L".{:02}", frac / 10'000);
        if (stepUs < 1'000'000)
            return whole + std::format(L".{}", frac / 100'000);
        return whole;
    }

    bool ParseTimecode(std::wstring_view text, int64_t& us)
    {
        text = Trim(text);
        if (text.empty())
            return false;
        // Up to three ':'-separated fields; only the last may have a fraction.
        int64_t fields[3]{};
        int count = 0;
        int64_t fracUs = 0;
        size_t i = 0;
        while (true)
        {
            if (count == 3)
                return false;
            int64_t value = 0;
            size_t digits = 0;
            while (i < text.size() && text[i] >= L'0' && text[i] <= L'9')
            {
                value = value * 10 + (text[i] - L'0');
                if (value > 100'000'000)
                    return false;
                ++i, ++digits;
            }
            if (i < text.size() && (text[i] == L'.' || text[i] == L','))
            {
                // Fraction of a second: only in the last field.
                ++i;
                int64_t scale = 100'000;
                size_t fracDigits = 0;
                while (i < text.size() && text[i] >= L'0' && text[i] <= L'9')
                {
                    fracUs += (text[i] - L'0') * scale;
                    scale /= 10;
                    ++i, ++fracDigits;
                }
                if (digits == 0 && fracDigits == 0)
                    return false;
                fields[count++] = value;
                if (i != text.size())
                    return false;
                break;
            }
            if (digits == 0)
                return false;
            fields[count++] = value;
            if (i == text.size())
                break;
            if (text[i] != L':')
                return false;
            ++i;
        }
        int64_t seconds = 0;
        for (int k = 0; k < count; ++k)
        {
            // Minutes and seconds after the first field must be below 60.
            if (k > 0 && fields[k] >= 60)
                return false;
            seconds = seconds * 60 + fields[k];
        }
        us = seconds * 1'000'000 + fracUs;
        return true;
    }

    // ---- Frame grid -----------------------------------------------------------------------------

    int64_t FrameDurationUs(double fps)
    {
        if (!(fps > 1.0 && fps < 1000.0))
            return 0;
        return static_cast<int64_t>(std::llround(1e6 / fps));
    }

    int64_t SnapToFrame(int64_t us, double fps)
    {
        if (FrameDurationUs(fps) == 0)
            return std::max<int64_t>(0, us);
        // The frame index from the exact rate, so 29.97 fps does not drift over an hour.
        const double index = std::floor(static_cast<double>(std::max<int64_t>(0, us)) * fps / 1e6 + 1e-6);
        return static_cast<int64_t>(std::llround(index * 1e6 / fps));
    }

    int64_t LastFrameTime(double fps, int64_t durationUs)
    {
        const int64_t frame = FrameDurationUs(fps);
        if (frame == 0 || durationUs <= 0)
            return std::max<int64_t>(0, durationUs);
        return SnapToFrame(std::max<int64_t>(0, durationUs - frame / 2), fps);
    }

    int64_t StepFrames(int64_t us, double fps, int frames, int64_t durationUs)
    {
        if (FrameDurationUs(fps) == 0)
            return std::clamp<int64_t>(us + frames * 10'000LL, 0, std::max<int64_t>(0, durationUs));
        const double index = std::floor(static_cast<double>(std::max<int64_t>(0, us)) * fps / 1e6 + 1e-6) + frames;
        const int64_t t = static_cast<int64_t>(std::llround(std::max(0.0, index) * 1e6 / fps));
        return std::min(t, LastFrameTime(fps, durationUs));
    }

    // ---- Ranges and points ----------------------------------------------------------------------

    std::vector<TimeRange> NormalizeRanges(std::vector<TimeRange> ranges, int64_t durationUs)
    {
        for (auto& r : ranges)
        {
            if (r.start > r.end)
                std::swap(r.start, r.end);
            r.start = std::clamp<int64_t>(r.start, 0, durationUs);
            r.end = std::clamp<int64_t>(r.end, 0, durationUs);
        }
        std::sort(ranges.begin(), ranges.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
        std::vector<TimeRange> out;
        for (const auto& r : ranges)
        {
            if (!out.empty() && r.start <= out.back().end)
                out.back().end = std::max(out.back().end, r.end);
            else
                out.push_back(r);
        }
        std::erase_if(out, [](const TimeRange& r) { return r.Length() < kMinRangeUs; });
        return out;
    }

    std::vector<int64_t> NormalizePoints(std::vector<int64_t> points, int64_t durationUs)
    {
        std::sort(points.begin(), points.end());
        std::vector<int64_t> out;
        for (const int64_t p : points)
        {
            if (p < kMinRangeUs || p > durationUs - kMinRangeUs)
                continue;
            if (!out.empty() && p - out.back() < kMinRangeUs)
                continue;
            out.push_back(p);
        }
        return out;
    }

    std::vector<TimeRange> PartsFromPoints(const std::vector<int64_t>& points, int64_t durationUs)
    {
        std::vector<TimeRange> parts;
        int64_t at = 0;
        for (const int64_t p : NormalizePoints(points, durationUs))
        {
            parts.push_back({ at, p });
            at = p;
        }
        parts.push_back({ at, durationUs });
        return parts;
    }

    std::vector<int64_t> EqualPartPoints(int64_t durationUs, int parts)
    {
        std::vector<int64_t> points;
        if (parts < 2 || durationUs <= 0)
            return points;
        for (int i = 1; i < parts; ++i)
            points.push_back(durationUs * i / parts);
        return NormalizePoints(points, durationUs);
    }

    int64_t ClampEdge(const std::vector<TimeRange>& ranges, size_t index, bool startEdge, int64_t t, int64_t durationUs)
    {
        if (index >= ranges.size())
            return t;
        const auto& r = ranges[index];
        if (startEdge)
        {
            const int64_t lo = index > 0 ? ranges[index - 1].end : 0;
            return std::clamp<int64_t>(t, lo, std::max(lo, r.end - kMinRangeUs));
        }
        const int64_t hi = index + 1 < ranges.size() ? ranges[index + 1].start : durationUs;
        return std::clamp<int64_t>(t, std::min(hi, r.start + kMinRangeUs), hi);
    }

    bool FitNewRange(const std::vector<TimeRange>& ranges, int64_t t, int64_t length, int64_t durationUs, TimeRange& out)
    {
        // The gap that holds t.
        int64_t lo = 0, hi = durationUs;
        for (const auto& r : ranges)
        {
            if (r.Contains(t))
                return false;
            if (r.end <= t)
                lo = std::max(lo, r.end);
            if (r.start > t)
                hi = std::min(hi, r.start);
        }
        if (hi - lo < kMinRangeUs)
            return false;
        length = std::clamp<int64_t>(length, kMinRangeUs, hi - lo);
        out.start = std::clamp<int64_t>(t, lo, hi - length);
        out.end = out.start + length;
        return true;
    }

    // ---- Time <-> pixels --------------------------------------------------------------------------

    double TimelineView::FitUsPerPx() const
    {
        return widthPx > 0 && durationUs > 0 ? static_cast<double>(durationUs) / widthPx : 1.0;
    }

    void TimelineView::Fit()
    {
        usPerPx = FitUsPerPx();
        startUs = 0;
    }

    double TimelineView::XOf(int64_t us) const
    {
        return usPerPx > 0 ? static_cast<double>(us - startUs) / usPerPx : 0.0;
    }

    int64_t TimelineView::TimeAt(double x) const
    {
        const double t = static_cast<double>(startUs) + x * usPerPx;
        return std::clamp<int64_t>(static_cast<int64_t>(std::llround(t)), 0, durationUs);
    }

    int64_t TimelineView::EndUs() const
    {
        return std::min<int64_t>(durationUs, startUs + static_cast<int64_t>(widthPx * usPerPx));
    }

    void TimelineView::Clamp()
    {
        const double fit = FitUsPerPx();
        usPerPx = std::clamp(usPerPx > 0 ? usPerPx : fit, std::min(MinUsPerPx(), fit), fit);
        const int64_t span = static_cast<int64_t>(widthPx * usPerPx);
        startUs = std::clamp<int64_t>(startUs, 0, std::max<int64_t>(0, durationUs - span));
    }

    void TimelineView::ZoomAt(double factor, double x)
    {
        if (factor <= 0)
            return;
        const double anchor = static_cast<double>(startUs) + x * usPerPx;
        usPerPx /= factor;
        Clamp();
        startUs = static_cast<int64_t>(std::llround(anchor - x * usPerPx));
        Clamp();
    }

    void TimelineView::ScrollBy(double px)
    {
        startUs += static_cast<int64_t>(std::llround(px * usPerPx));
        Clamp();
    }

    void TimelineView::EnsureVisible(int64_t us)
    {
        const int64_t span = static_cast<int64_t>(widthPx * usPerPx);
        const int64_t margin = span / 10;
        if (us < startUs + margin / 2)
            startUs = us - margin;
        else if (us > startUs + span - margin / 2)
            startUs = us - span + margin;
        Clamp();
    }

    int64_t TickStepUs(double usPerPx, double minPx)
    {
        static constexpr int64_t kSteps[] = { 10'000, 20'000, 50'000, 100'000, 200'000, 500'000, 1'000'000, 2'000'000, 5'000'000,
                                              10'000'000, 15'000'000, 30'000'000, 60'000'000, 120'000'000, 300'000'000, 600'000'000,
                                              900'000'000, 1'800'000'000, 3'600'000'000 };
        for (const int64_t step : kSteps)
            if (static_cast<double>(step) / usPerPx >= minPx)
                return step;
        return kSteps[std::size(kSteps) - 1];
    }

    std::vector<int64_t> FilmstripTimes(int64_t durationUs, int count)
    {
        std::vector<int64_t> times;
        if (durationUs <= 0 || count <= 0)
            return times;
        for (int i = 0; i < count; ++i)
            times.push_back(durationUs * (2 * i + 1) / (2 * count));
        return times;
    }

    // ---- Waveform --------------------------------------------------------------------------------

    void PeakPyramid::Build(std::vector<int16_t> samples, int sampleRate)
    {
        m_samples = std::move(samples);
        m_rate = sampleRate;
        m_levels.clear();
        int16_t top = 0;
        for (const int16_t s : m_samples)
            top = std::max<int16_t>(top, static_cast<int16_t>(s == std::numeric_limits<int16_t>::min() ? 32767 : std::abs(s)));
        m_peak = static_cast<float>(top) / 32767.f;
        if (m_samples.empty())
            return;
        std::vector<std::pair<int16_t, int16_t>> level((m_samples.size() + kBase - 1) / kBase);
        for (size_t b = 0; b < level.size(); ++b)
        {
            const size_t first = b * kBase, last = std::min(m_samples.size(), first + kBase);
            const auto [lo, hi] = std::minmax_element(m_samples.begin() + static_cast<ptrdiff_t>(first), m_samples.begin() + static_cast<ptrdiff_t>(last));
            level[b] = { *lo, *hi };
        }
        m_levels.push_back(std::move(level));
        while (m_levels.back().size() > 1)
        {
            const auto& below = m_levels.back();
            std::vector<std::pair<int16_t, int16_t>> up((below.size() + 1) / 2);
            for (size_t b = 0; b < up.size(); ++b)
            {
                auto v = below[2 * b];
                if (2 * b + 1 < below.size())
                    v = { std::min(v.first, below[2 * b + 1].first), std::max(v.second, below[2 * b + 1].second) };
                up[b] = v;
            }
            m_levels.push_back(std::move(up));
        }
    }

    std::pair<float, float> PeakPyramid::Range(int64_t first, int64_t last) const
    {
        const int64_t n = SampleCount();
        first = std::clamp<int64_t>(first, 0, n);
        last = std::clamp<int64_t>(last, 0, n);
        if (last <= first)
        {
            if (first < n)
                last = first + 1;
            else
                return { 0.f, 0.f };
        }
        int lo = 32767, hi = -32768;
        const auto take = [&](int16_t a, int16_t b) {
            lo = std::min<int>(lo, a);
            hi = std::max<int>(hi, b);
        };
        const int64_t span = last - first;
        if (span < kBase * 2 || m_levels.empty())
        {
            for (int64_t i = first; i < last; ++i)
                take(m_samples[static_cast<size_t>(i)], m_samples[static_cast<size_t>(i)]);
        }
        else
        {
            // The coarsest level whose buckets still fit the span twice; its buckets touching the span
            // (a slight over-cover at the edges, invisible at one column per pixel).
            size_t level = 0;
            while (level + 1 < m_levels.size() && (static_cast<int64_t>(kBase) << (level + 1)) * 2 <= span)
                ++level;
            const int64_t size = static_cast<int64_t>(kBase) << level;
            const auto& buckets = m_levels[level];
            for (int64_t b = first / size; b <= (last - 1) / size && b < static_cast<int64_t>(buckets.size()); ++b)
                take(buckets[static_cast<size_t>(b)].first, buckets[static_cast<size_t>(b)].second);
        }
        return { static_cast<float>(lo) / 32768.f, static_cast<float>(hi) / 32767.f };
    }

    std::pair<float, float> PeakPyramid::Span(int64_t startUs, int64_t endUs) const
    {
        if (m_rate <= 0)
            return { 0.f, 0.f };
        const auto at = [this](int64_t us) { return static_cast<int64_t>(static_cast<double>(us) * m_rate / 1e6); };
        return Range(at(startUs), at(endUs));
    }

    // ---- Crop boxes ------------------------------------------------------------------------------

    namespace
    {
        int Even(int v) { return v & ~1; }
    }

    Aspect AspectByName(std::string_view name, int frameW, int frameH)
    {
        if (name == "original")
            return { frameW, frameH };
        if (name == "16:9") return { 16, 9 };
        if (name == "9:16") return { 9, 16 };
        if (name == "1:1") return { 1, 1 };
        if (name == "4:5") return { 4, 5 };
        if (name == "4:3") return { 4, 3 };
        return {};
    }

    PixelRect CenteredCrop(int frameW, int frameH, Aspect aspect)
    {
        if (frameW <= 0 || frameH <= 0)
            return {};
        int w = frameW, h = frameH;
        if (!aspect.Free())
        {
            // As wide as possible; if that is too tall, as tall as possible.
            h = static_cast<int>(static_cast<int64_t>(w) * aspect.den / aspect.num);
            if (h > frameH)
            {
                h = frameH;
                w = static_cast<int>(static_cast<int64_t>(h) * aspect.num / aspect.den);
            }
        }
        w = std::max(2, Even(w));
        h = std::max(2, Even(h));
        return { Even((frameW - w) / 2), Even((frameH - h) / 2), w, h };
    }

    PixelRect ClampCrop(PixelRect r, int frameW, int frameH)
    {
        const int minSide = std::min({ 16, Even(frameW), Even(frameH) });
        r.w = std::clamp(Even(r.w), minSide, Even(frameW));
        r.h = std::clamp(Even(r.h), minSide, Even(frameH));
        r.x = std::clamp(Even(r.x), 0, Even(frameW - r.w));
        r.y = std::clamp(Even(r.y), 0, Even(frameH - r.h));
        return r;
    }

    PixelRect DragCrop(const PixelRect& s, int handle, int dx, int dy, int frameW, int frameH, Aspect aspect)
    {
        if (handle == 8)
        {
            PixelRect r = s;
            r.x = std::clamp(s.x + dx, 0, frameW - s.w);
            r.y = std::clamp(s.y + dy, 0, frameH - s.h);
            return ClampCrop(r, frameW, frameH);
        }
        // Edges of the box being dragged, in doubles so the aspect maths stays smooth.
        double left = s.x, top = s.y, right = s.x + s.w, bottom = s.y + s.h;
        const bool moveLeft = handle == 0 || handle == 3 || handle == 7;
        const bool moveRight = handle == 1 || handle == 2 || handle == 5;
        const bool moveTop = handle == 0 || handle == 1 || handle == 4;
        const bool moveBottom = handle == 2 || handle == 3 || handle == 6;
        if (moveLeft) left = std::clamp(left + dx, 0.0, right - 16);
        if (moveRight) right = std::clamp(right + dx, left + 16, static_cast<double>(frameW));
        if (moveTop) top = std::clamp(top + dy, 0.0, bottom - 16);
        if (moveBottom) bottom = std::clamp(bottom + dy, top + 16, static_cast<double>(frameH));
        if (!aspect.Free())
        {
            const double ratio = static_cast<double>(aspect.num) / aspect.den;
            double w = right - left, h = bottom - top;
            const double cx = (left + right) / 2, cy = (top + bottom) / 2;
            // How far the box may reach from the side that stays put (corners: the opposite corner;
            // edges: the opposite edge, with the other two sides growing around the centre).
            double maxW = moveLeft ? right : frameW - left;
            double maxH = moveTop ? bottom : frameH - top;
            if (handle <= 3)
            {
                // A corner: the side that moved more leads.
                if (std::abs(dx) * 1.0 >= std::abs(dy) * ratio)
                    h = w / ratio;
                else
                    w = h * ratio;
            }
            else if (handle == 4 || handle == 6)
            {
                w = h * ratio;   // top or bottom edge: the height leads
                maxW = 2 * std::min(cx, frameW - cx);
            }
            else
            {
                h = w / ratio;   // left or right edge: the width leads
                maxH = 2 * std::min(cy, frameH - cy);
            }
            const double scale = std::min({ 1.0, maxW / w, maxH / h });
            w *= scale;
            h *= scale;
            if (handle == 4 || handle == 6)
                left = cx - w / 2, right = cx + w / 2;
            else if (moveLeft)
                left = right - w;
            else
                right = left + w;
            if (handle == 5 || handle == 7)
                top = cy - h / 2, bottom = cy + h / 2;
            else if (moveTop)
                top = bottom - h;
            else
                bottom = top + h;
        }
        PixelRect r{ static_cast<int>(std::lround(left)), static_cast<int>(std::lround(top)), static_cast<int>(std::lround(right - left)),
                     static_cast<int>(std::lround(bottom - top)) };
        return ClampCrop(r, frameW, frameH);
    }

    PixelRect RectFromCorners(int x0, int y0, int x1, int y1, int frameW, int frameH)
    {
        x0 = std::clamp(x0, 0, frameW), x1 = std::clamp(x1, 0, frameW);
        y0 = std::clamp(y0, 0, frameH), y1 = std::clamp(y1, 0, frameH);
        PixelRect r{ std::min(x0, x1), std::min(y0, y1), std::abs(x1 - x0), std::abs(y1 - y0) };
        return ClampCrop(r, frameW, frameH);
    }

    void CropOutputSize(const PixelRect& crop, int wantW, int wantH, int& outW, int& outH)
    {
        outW = crop.w, outH = crop.h;
        if (crop.w <= 0 || crop.h <= 0)
            return;
        if (wantW > 0 && wantH > 0)
            outW = wantW, outH = wantH;
        else if (wantW > 0)
            outW = wantW, outH = static_cast<int>(std::lround(static_cast<double>(wantW) * crop.h / crop.w));
        else if (wantH > 0)
            outH = wantH, outW = static_cast<int>(std::lround(static_cast<double>(wantH) * crop.w / crop.h));
        outW = std::clamp(Even(outW), 16, 8192);
        outH = std::clamp(Even(outH), 16, 8192);
    }
}
