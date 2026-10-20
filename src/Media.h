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

    // ffmpeg's output options for a format: codecs, quality, filters and the muxer (-f), everything
    // between the input and the output path. `transcode` forces re-encoding for a remux format (the
    // fallback when copying the streams failed).
    std::vector<std::wstring> OutputArgs(const MediaFormatInfo& format, H264Encoder h264, bool transcode = false);

    // The whole command line after "ffmpeg": quiet banner, no stdin, local files only, progress as
    // key=value lines on stdout, and -y because the output is our own empty staging file.
    std::vector<std::wstring> FfmpegArgs(const std::wstring& input, const std::vector<std::wstring>& outputArgs, const std::wstring& output);

    // Quotes one argument for CreateProcess the way CommandLineToArgvW reads it back.
    std::wstring QuoteArg(const std::wstring& arg);

    // "  Duration: 00:01:23.45, start: ..." from ffmpeg's log -> microseconds; -1 if the line has no
    // duration or it is "N/A" (live streams, some raw formats).
    int64_t ParseDurationLine(std::string_view line);
    // "out_time_us=12345678" from -progress -> microseconds; -1 for other lines or "N/A".
    int64_t ParseProgressLine(std::string_view line);
}
