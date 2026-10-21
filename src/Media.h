#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Audio and video formats (M3). Everything here is pure: which files are media, what each output
// format is, and the ffmpeg arguments that produce it. Running ffmpeg is in Ffmpeg.h.
namespace sd
{
    enum class MediaKind { None, Video, Audio };

    // What a file is, by extension. Animated GIFs stay images (WIC decodes them).
    MediaKind MediaKindOfExtension(const std::wstring& lowerExtension);

    // The H.264 encoders ffmpeg can use, best first. GPU encoders are fast and keep the CPU free; Media
    // Foundation is Windows' own (hardware when the driver offers it, Microsoft's software encoder
    // otherwise); OpenH264 always works. x264 is GPL and not in our LGPL ffmpeg build.
    enum class H264Encoder { Nvenc, Qsv, Amf, MediaFoundation, OpenH264 };
    const wchar_t* EncoderName(H264Encoder encoder);   // ffmpeg's name, e.g. L"h264_nvenc"
    // "-c:v <encoder>" and its constant-quality settings at `cq` (the usual H.264 scale: 23 = visually
    // lossless for most video); encoders without a reliable constant-quality mode get `fallbackBps`.
    std::vector<std::wstring> H264QualityArgs(H264Encoder encoder, int cq, int64_t fallbackBps);

    struct MediaFormatInfo
    {
        const char* id;                // "mp4"; also the media.convert "format" parameter
        const wchar_t* label;          // "MP4"
        const wchar_t* extension;      // ".mp4"
        const wchar_t* aliases;        // source extensions that already are this format: ".mp4.m4v"
        const wchar_t* description;    // shown in the wheel's centre: "H.264 video, AAC audio"
        bool fromVideo;                // offered for video files
        bool fromAudio;                // offered for audio files
        bool remux;                    // try copying the streams first (MKV takes almost any codec)
    };
    const std::vector<MediaFormatInfo>& MediaFormats();
    const MediaFormatInfo* FindMediaFormat(std::string_view id);
    bool IsAlreadyMediaFormat(const MediaFormatInfo& format, const std::wstring& lowerExtension);
    bool MediaFormatAccepts(const MediaFormatInfo& format, MediaKind kind);

    // What ffmpeg reports about an input (ParseMediaInfo reads it from the log of a probe run).
    struct MediaInfo
    {
        int64_t durationUs = -1;
        std::string videoCodec;   // ffmpeg's codec names ("h264", "aac"); empty when there is no such stream
        std::string audioCodec;   // (cover art in a song is not counted as video)
        int width = 0, height = 0;
        double fps = 0;
    };
    MediaInfo ParseMediaInfo(const std::vector<std::string>& logLines);

    // How one conversion runs: which streams are copied as they are (instant and lossless, when the
    // target container plays them), and how the rest is encoded.
    struct MediaPlan
    {
        H264Encoder h264 = H264Encoder::OpenH264;
        bool copyAll = false;     // MKV: every stream copied
        bool copyVideo = false;
        bool copyAudio = false;
        bool hevc = false;        // copied video is HEVC (MP4/MOV tag it hvc1)
        bool hwDecode = false;    // decode on the GPU as well (-hwaccel auto, falls back by itself)
        int64_t videoBps = 0;     // > 0: encode the video at this average bitrate instead of a quality
    };
    MediaPlan PlanConversion(const MediaFormatInfo& format, const MediaInfo& info, H264Encoder h264);
    // The fallback when copying failed (a stream the container refused): re-encode everything.
    MediaPlan TranscodePlan(const MediaPlan& plan);
    // A video Convert never comes out bigger than its source: when the quality setting overshoots,
    // it runs again at `share` of the source's own bitrate, less room for the sound when it has any.
    // 0 when the length is unknown.
    int64_t CapVideoBps(int64_t sourceBytes, int64_t durationUs, double share, bool hasAudio);

    // ffmpeg's input options (before -i) and output options (codecs, quality, filters and the muxer,
    // named with -f because the staging file's extension is ".sdpart") for a plan.
    std::vector<std::wstring> InputArgs(const MediaPlan& plan);
    std::vector<std::wstring> OutputArgs(const MediaFormatInfo& format, const MediaPlan& plan);

    // The whole command line after "ffmpeg": quiet banner, no stdin, local files only, progress as
    // key=value lines on stdout, and -y because the output is our own empty staging file.
    std::vector<std::wstring> FfmpegArgs(const std::wstring& input, const std::vector<std::wstring>& inputArgs,
                                         const std::vector<std::wstring>& outputArgs, const std::wstring& output);
    // Reads an input without writing anything: its streams and duration end up in the log.
    std::vector<std::wstring> ProbeArgs(const std::wstring& input);

    // Compress and Fit to… (media.compress). A size target becomes bitrates (and, when the bitrate
    // is too low for the source's size, a smaller standard height).
    struct SizeFit
    {
        int64_t videoBps = 0;
        int64_t audioBps = 0;
        int height = 0;   // 0 = keep the source's height
    };
    // False when the target is too small for the input's length to give a watchable result.
    bool PlanSizeFit(int64_t targetBytes, const MediaInfo& info, SizeFit& fit);
    // H.264 + AAC MP4 for video, AAC M4A for audio. `fit` null = the quality preset; `maxHeight` 0 = any.
    std::vector<std::wstring> CompressArgs(const MediaInfo& info, H264Encoder h264, const SizeFit* fit, int maxHeight);

    // Quotes one argument for CreateProcess the way CommandLineToArgvW reads it back.
    std::wstring QuoteArg(const std::wstring& arg);

    // "  Duration: 00:01:23.45, start: ..." from ffmpeg's log -> microseconds; -1 if the line has no
    // duration or it is "N/A" (live streams, some raw formats).
    int64_t ParseDurationLine(std::string_view line);
    // "out_time_us=12345678" from -progress -> microseconds; -1 for other lines or "N/A".
    int64_t ParseProgressLine(std::string_view line);
}
