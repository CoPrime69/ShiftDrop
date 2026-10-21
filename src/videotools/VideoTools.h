#pragma once
#include "Media.h"
#include "mediaeditor/MediaProbe.h"
#include "mediaeditor/MediaTools.h"
#include "mediaeditor/Timeline.h"

#include <string>
#include <string_view>
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

    // 9. Trim Video: keep `range`, or remove it and join what is on either side. Fast copies the
    // streams, starting at the keyframe at or before the start (instant, lossless, may begin a little
    // early); otherwise frame-exact, re-encoded. Removing a part and fading at the cuts re-encode.
    struct TrimChoice
    {
        bool remove = false;   // cut the selection out and join the rest
        bool fade = false;     // short fades of the picture and the sound at each cut
    };
    inline constexpr int64_t kFadeUs = 500'000;   // a fade's length (shorter in a short part)
    // Whether the streams can be copied: Fast, keeping the selection, no fades.
    bool TrimCopies(bool fast, const TrimChoice& choice);
    // What ends up in the result, in order: the selection, or the parts before and after it (a
    // part shorter than kMinRangeUs is left out).
    std::vector<TimeRange> TrimParts(TimeRange selection, int64_t durationUs, bool remove);
    int64_t TrimmedLength(TimeRange selection, int64_t durationUs, bool remove);
    // fade (video) or afade (audio) filters for a part `lengthUs` long that fades in at its start
    // and / or out at its end; empty when neither.
    std::wstring FadeFilter(bool audio, int64_t lengthUs, bool fadeIn, bool fadeOut);
    std::vector<std::wstring> TrimArgs(const std::wstring& input, const std::wstring& lowerExtension, const ProbeInfo& info,
                                       TimeRange range, bool fast, H264Encoder encoder, const std::wstring& output,
                                       const TrimChoice& choice = {});
    Container TrimContainer(const std::wstring& lowerExtension, bool fast);

    // 10. Crop Video: the box (in shown pixels), scaled to outW x outH when that differs; audio kept.
    std::wstring CropFilter(const PixelRect& box, int outW, int outH);
    std::vector<std::wstring> CropArgs(const std::wstring& input, const std::wstring& lowerExtension, const ProbeInfo& info,
                                       const PixelRect& box, int outW, int outH, H264Encoder encoder, const std::wstring& output);

    // 11. Change Video Speed (0.25x to 4x). The picture is re-timed and kept at the source's frame
    // rate. The sound is sped up or slowed down without changing its pitch (atempo, chained because
    // each atempo stays in 0.5..2 for the best quality), or, with `keepPitch` off, played faster or
    // slower like a tape, its pitch following (asetrate, then back to the source's rate). Silent
    // videos simply have no audio.
    inline constexpr double kMinSpeed = 0.25, kMaxSpeed = 4.0;
    std::wstring AtempoChain(double factor);
    // The sound's filter at `factor`; atempo when the sample rate is unknown.
    std::wstring SpeedAudioFilter(double factor, bool keepPitch, int sampleRate);
    std::vector<std::wstring> SpeedArgs(const std::wstring& input, const std::wstring& lowerExtension, const ProbeInfo& info, double factor,
                                        H264Encoder encoder, const std::wstring& output, bool keepPitch = true);
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

    // 13. Save Video Frames: the frames marked on the timeline, every Nth frame, or N a second, full
    // size as PNG, JPG or WebP. ffmpeg writes PNG; JPG and WebP come from the app's own image
    // encoders (ImagePipeline.h), at the Convert wheel's quality.
    std::vector<std::wstring> FrameToPngArgs(const std::wstring& input, int64_t us, double fps, const std::wstring& output);
    // "clip 00-01-02.345.png"
    std::wstring FrameFileName(const std::wstring& stem, int64_t us, const std::wstring& extension);
    // Frame times snapped to the frame grid, sorted, without duplicates.
    std::vector<int64_t> NormalizeFrameTimes(std::vector<int64_t> times, double fps, int64_t durationUs);

    enum class FramePick { Marks, Every, PerSecond };
    FramePick FramePickByName(std::string_view name);   // "marks", "every", "rate"
    struct FrameSequence
    {
        FramePick pick = FramePick::Every;
        int every = 30;          // every Nth frame
        double perSecond = 1;    // or this many a second
    };
    inline constexpr int kMaxEvery = 100'000;
    inline constexpr double kMinPerSecond = 0.01, kMaxPerSecond = 60;
    // The select (every Nth) or fps (N a second) filter of one pass through the video.
    std::wstring FrameSequenceFilter(const FrameSequence& seq);
    // Every picked frame as a full-size PNG into `folder`, numbered from 000000.png.
    std::vector<std::wstring> FrameSequenceArgs(const std::wstring& input, const FrameSequence& seq, const std::wstring& folder);
    // How many frames that pass writes from a video `durationUs` long at `fps` (0 when the frame rate
    // is unknown and every Nth is asked for), and when the i-th of them shows.
    int64_t FrameSequenceCount(const FrameSequence& seq, double fps, int64_t durationUs);
    int64_t FrameSequenceTime(const FrameSequence& seq, double fps, int64_t index);
    // About how big one w x h video frame is as "png", "jpg" or "webp" (for the estimate).
    uint64_t FrameFileBytes(int width, int height, std::string_view format);
    // The quality JPG and WebP frames are written at.
    int FrameQuality(std::string_view format);

    // 14. Split Video into parts at `points` (marked, equal parts, or every so long: EveryPoints).
    // Fast: one pass of the segment muxer, streams copied, each part starting at a keyframe. Exact:
    // each part encoded on its own from the exact frame.
    inline constexpr int kMaxSplitParts = 1000;
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
