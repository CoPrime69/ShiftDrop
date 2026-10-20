#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The editor's maths, kept free of any window so it can be unit-tested: timecodes, the frame grid,
// time <-> pixel mapping with zoom, ranges and split points, the waveform envelope and crop boxes.
// Times are microseconds everywhere; recipe JSON stores seconds.
namespace sd::mediatools
{
    // ---- Timecodes ------------------------------------------------------------------------------

    // "00:01:23.456" (hours always shown, milliseconds rounded).
    std::wstring FormatTimecode(int64_t us);
    // Short ruler labels: "0:05", "1:23.5", "1:02:03".
    std::wstring FormatRulerLabel(int64_t us, int64_t stepUs);
    // Accepts "1:23.456", "83.5", "01:02:03", "1:2:3.25" and "12" (seconds). False for anything else.
    bool ParseTimecode(std::wstring_view text, int64_t& us);

    // ---- Frame grid -----------------------------------------------------------------------------

    // One frame's length; 0 when the frame rate is unknown (audio, or an odd stream).
    int64_t FrameDurationUs(double fps);
    // The start of the frame showing at `us` (frames are on a grid from 0).
    int64_t SnapToFrame(int64_t us, double fps);
    // The start of the last frame of a clip.
    int64_t LastFrameTime(double fps, int64_t durationUs);
    // `frames` frames forward or back from `us` (snapped first), kept inside the clip. Without a frame
    // rate one step is 10 ms (audio).
    int64_t StepFrames(int64_t us, double fps, int frames, int64_t durationUs);

    // ---- Ranges and points ----------------------------------------------------------------------

    struct TimeRange
    {
        int64_t start = 0;
        int64_t end = 0;
        int64_t Length() const { return end - start; }
        bool Contains(int64_t t) const { return t >= start && t < end; }
        bool operator==(const TimeRange&) const = default;
    };
    inline constexpr int64_t kMinRangeUs = 50'000;   // shorter ranges are not worth a cut or a bleep

    // Clamps to [0, duration], drops ranges shorter than kMinRangeUs, sorts and merges overlaps.
    std::vector<TimeRange> NormalizeRanges(std::vector<TimeRange> ranges, int64_t durationUs);
    // Sorted cut points strictly inside the clip, at least kMinRangeUs from each other and the ends.
    std::vector<int64_t> NormalizePoints(std::vector<int64_t> points, int64_t durationUs);
    // The pieces between cut points: n points make n + 1 parts.
    std::vector<TimeRange> PartsFromPoints(const std::vector<int64_t>& points, int64_t durationUs);
    // The cut points that make `parts` equal parts.
    std::vector<int64_t> EqualPartPoints(int64_t durationUs, int parts);

    // Moving one edge of range `index` in a sorted, non-overlapping list: the edge stops at its
    // neighbours and the clip ends, and the range never gets shorter than kMinRangeUs.
    int64_t ClampEdge(const std::vector<TimeRange>& ranges, size_t index, bool startEdge, int64_t t, int64_t durationUs);
    // A new range of up to `length` at `t`, fitted into the gap around it; false when there is no room.
    bool FitNewRange(const std::vector<TimeRange>& ranges, int64_t t, int64_t length, int64_t durationUs, TimeRange& out);

    // ---- Time <-> pixels --------------------------------------------------------------------------

    struct TimelineView
    {
        int64_t durationUs = 0;
        double widthPx = 0;      // the drawable width of the timeline
        int64_t startUs = 0;     // time at the left edge
        double usPerPx = 0;      // zoom

        void Fit();                                   // the whole clip
        double FitUsPerPx() const;
        double MinUsPerPx() const { return 500.0; }   // zoomed in to half a millisecond per pixel at most
        bool Zoomed() const { return usPerPx < FitUsPerPx() * 0.999; }
        double XOf(int64_t us) const;
        int64_t TimeAt(double x) const;               // clamped to the clip
        int64_t EndUs() const;                        // time at the right edge (clamped)
        // Zooms by `factor` (>1 = closer) keeping the time under `x` where it is.
        void ZoomAt(double factor, double x);
        void ScrollBy(double px);
        void EnsureVisible(int64_t us);               // scrolls so `us` is on screen
        void Clamp();
    };

    // The ruler's tick step: the smallest "round" interval (10 ms ... 1 h) at least `minPx` apart.
    int64_t TickStepUs(double usPerPx, double minPx);

    // Centre times of `count` equal slices: where the filmstrip's thumbnails are taken.
    std::vector<int64_t> FilmstripTimes(int64_t durationUs, int count);

    // ---- Waveform --------------------------------------------------------------------------------

    // Min/max envelope of mono PCM at power-of-two bucket sizes, so a column of any zoom is a handful of
    // lookups instead of a scan.
    class PeakPyramid
    {
    public:
        void Build(std::vector<int16_t> samples, int sampleRate);
        bool Empty() const { return m_samples.empty(); }
        int SampleRate() const { return m_rate; }
        int64_t SampleCount() const { return static_cast<int64_t>(m_samples.size()); }
        // Lowest and highest sample (-1..1) in [first, last).
        std::pair<float, float> Range(int64_t first, int64_t last) const;
        // The same for a span of time.
        std::pair<float, float> Span(int64_t startUs, int64_t endUs) const;
        // Loudest sample of the whole clip (0..1), to scale quiet recordings up for display.
        float Peak() const { return m_peak; }

    private:
        static constexpr int kBase = 16;   // samples per bucket at level 0
        std::vector<int16_t> m_samples;
        std::vector<std::vector<std::pair<int16_t, int16_t>>> m_levels;   // level k: buckets of kBase << k samples
        int m_rate = 0;
        float m_peak = 0;
    };

    // ---- Crop boxes ------------------------------------------------------------------------------

    struct PixelRect
    {
        int x = 0, y = 0, w = 0, h = 0;
        bool operator==(const PixelRect&) const = default;
    };
    struct Aspect
    {
        int num = 0, den = 0;   // 0:0 = free
        bool Free() const { return num <= 0 || den <= 0; }
    };
    // "free", "original" (the frame's own shape), "16:9", "9:16", "1:1", "4:5". Free for anything else.
    Aspect AspectByName(std::string_view name, int frameW, int frameH);
    // The largest box of that shape inside the frame, centred, with even sides.
    PixelRect CenteredCrop(int frameW, int frameH, Aspect aspect);
    // Inside the frame, even position and size, at least 16 px.
    PixelRect ClampCrop(PixelRect r, int frameW, int frameH);
    // Handles: 0..3 corners (top-left, top-right, bottom-right, bottom-left), 4..7 edges (top, right,
    // bottom, left), 8 = the whole box. With an aspect, corners keep it and edges grow the other side
    // around the centre.
    PixelRect DragCrop(const PixelRect& start, int handle, int dx, int dy, int frameW, int frameH, Aspect aspect);
    // A box from two corners (drawing a new one), kept inside the frame.
    PixelRect RectFromCorners(int x0, int y0, int x1, int y1, int frameW, int frameH);
    // The output size for a crop: `wantW` x `wantH` when given (0 = from the other side and the crop's
    // shape, both 0 = the crop's own size), even, at most 8192.
    void CropOutputSize(const PixelRect& crop, int wantW, int wantH, int& outW, int& outH);
}
