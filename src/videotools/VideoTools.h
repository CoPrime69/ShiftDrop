#pragma once
#include "Media.h"
#include "mediaeditor/MediaProbe.h"
#include "mediaeditor/MediaTools.h"
#include "mediaeditor/Timeline.h"

#include <string>
#include <vector>

// The video tools (Trim, Crop, Speed, Join, Frames, Split, Redact) as ffmpeg command lines. Everything here is pure: given what the input holds, the
// user's choices and the H.264 encoder, the arguments after "ffmpeg". VideoEngine.cpp runs them.
//
// Stream copy is used wherever it gives exactly what was asked: a Fast trim or split cuts at
// keyframes without re-encoding, and clips that already match are joined as they are. Everything
// that changes pixels or needs frame accuracy is encoded with the fastest H.264 encoder on the PC.
namespace sd::videotools
{
    using mediatools::Container;
    using mediatools::PixelRect;
    using mediatools::ProbeInfo;
    using mediatools::TimeRange;

    // The output options of a re-encoded edit: the first video and every audio stream, `videoFilter`
    // (may be empty) plus even dimensions, H.264 at edit quality for an outW x outH x fps picture, the
    // soundtrack copied when the container takes it (or AAC through `audioFilter`), and the muxer.
    std::vector<std::wstring> EncodeOutput(const Container& container, const ProbeInfo& info, H264Encoder encoder,
                                           const std::wstring& videoFilter, int outW, int outH, double fps,
                                           const std::wstring& audioFilter = {});

    // 9. Trim Video: keep `range`. Fast copies the streams, starting at the keyframe at or before the
    // start (instant, lossless, may begin a little early); otherwise frame-exact, re-encoded.
    std::vector<std::wstring> TrimArgs(const std::wstring& input, const std::wstring& lowerExtension, const ProbeInfo& info,
                                       TimeRange range, bool fast, H264Encoder encoder, const std::wstring& output);
    Container TrimContainer(const std::wstring& lowerExtension, bool fast);

    // 10. Crop Video: the box (in shown pixels), scaled to outW x outH when that differs; audio kept.
    std::wstring CropFilter(const PixelRect& box, int outW, int outH);
    std::vector<std::wstring> CropArgs(const std::wstring& input, const std::wstring& lowerExtension, const ProbeInfo& info,
                                       const PixelRect& box, int outW, int outH, H264Encoder encoder, const std::wstring& output);

    // 11. Change Video Speed (0.25x to 4x). The picture is re-timed and kept at the source's frame
    // rate; the sound is sped up or slowed down without changing its pitch (atempo, chained because
    // each atempo stays in 0.5..2 for the best quality). Silent videos simply have no audio.
    inline constexpr double kMinSpeed = 0.25, kMaxSpeed = 4.0;
    std::wstring AtempoChain(double factor);
    std::vector<std::wstring> SpeedArgs(const std::wstring& input, const std::wstring& lowerExtension, const ProbeInfo& info, double factor,
                                        H264Encoder encoder, const std::wstring& output);
    // "2x", "0.5x", "1.25x"
    std::wstring SpeedLabel(double factor);

    // 12. Join Videos. Clips with the same codecs, size and frame rate (a phone's clips) are joined as
    // they are through the concat demuxer; anything else is encoded on the first clip's canvas, each
    // clip scaled to fit and padded, silent clips given silence.
    bool CanJoinByCopy(const std::vector<ProbeInfo>& clips);
    // The concat demuxer's list: one "file '<path>'" line per clip, quotes escaped, UTF-8.
    std::string ConcatList(const std::vector<std::wstring>& paths);
    std::string EscapeConcatPath(const std::string& utf8Path);
    std::vector<std::wstring> JoinCopyArgs(const std::wstring& listFile, const ProbeInfo& first, const std::wstring& output);
    std::vector<std::wstring> JoinEncodeArgs(const std::vector<std::wstring>& inputs, const std::vector<ProbeInfo>& clips,
                                             H264Encoder encoder, const std::wstring& output, bool fill = false);
    // The first clip's shown size (even) and frame rate (30 when unknown, at most 60).
    void JoinCanvas(const ProbeInfo& first, int& w, int& h, double& fps);

    // 13. Save Video Frames: the exact frame showing at `us`, full size, as a PNG.
    std::vector<std::wstring> FrameToPngArgs(const std::wstring& input, int64_t us, double fps, const std::wstring& output);
    // "clip 00-01-02.345.png"
    std::wstring FramePngName(const std::wstring& stem, int64_t us);
    // Frame times snapped to the frame grid, sorted, without duplicates.
    std::vector<int64_t> NormalizeFrameTimes(std::vector<int64_t> times, double fps, int64_t durationUs);

    // 14. Split Video into parts at `points`. Fast: one pass of the segment muxer, streams copied, each
    // part starting at a keyframe. Exact: each part encoded on its own from the exact frame.
    std::wstring PartName(const std::wstring& stem, int number, int count, const std::wstring& extension);   // "clip part 2.mp4"
    std::vector<std::wstring> SplitCopyArgs(const std::wstring& input, const std::wstring& lowerExtension, const std::vector<int64_t>& points,
                                            const std::wstring& folder, const std::wstring& stem);
    std::vector<std::wstring> SplitPartArgs(const std::wstring& input, const std::wstring& lowerExtension, const ProbeInfo& info,
                                            TimeRange part, H264Encoder encoder, const std::wstring& output);

    // 15. Redact Video: solid or blurred boxes, each over its own time range.
    struct RedactBox
    {
        PixelRect box;      // shown pixels
        TimeRange range;
        bool blur = true;   // false = solid black
    };
    // "between(t,1.500000,3.000000)"
    std::wstring BetweenExpr(TimeRange range);
    std::wstring RedactGraph(const std::vector<RedactBox>& boxes, int frameW, int frameH);   // [0:v:0] in, [vout] out
    std::vector<std::wstring> RedactArgs(const std::wstring& input, const std::wstring& lowerExtension, const ProbeInfo& info,
                                         const std::vector<RedactBox>& boxes, H264Encoder encoder, const std::wstring& output);

    using mediatools::FpsExpr;
    using mediatools::Num;
}
