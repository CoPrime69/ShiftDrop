#pragma once
#include <cstddef>
#include <string>
#include <vector>

// Video and audio metadata through ffmpeg: what a probe says a file carries, and the arguments that
// copy its streams into a new file without any of it. Pure functions; running ffmpeg is Ffmpeg.h.
namespace sd::metadata
{
    struct MediaTag
    {
        enum class Scope { Global, Stream, Chapter } scope = Scope::Global;
        std::string key;     // as ffmpeg prints it ("creation_time", "com.apple.quicktime.location.ISO6709")
        std::string value;
    };

    struct MediaTags
    {
        std::vector<MediaTag> tags;
        size_t chapters = 0;
        size_t dataStreams = 0;     // timed metadata tracks (a phone's GPS or camera data), not picture or sound
        size_t coverArt = 0;        // attached pictures (album art, thumbnails)
        size_t attachments = 0;     // Matroska attachments

        // Tags a share-safe copy must not carry: everything but container plumbing (brands, the
        // muxer's name, handler names, language codes...).
        std::vector<MediaTag> Personal() const;
        size_t PersonalCount() const;   // personal tags + chapters + data streams + cover art + attachments
        // "GPS", "dates", "camera"... most telling first.
        std::vector<std::wstring> Kinds() const;
    };

    // Reads the "Metadata:" sections, chapters and streams from the log of an ffmpeg probe run.
    MediaTags ParseMediaTags(const std::vector<std::string>& logLines);
    bool IsPlumbingTag(const std::string& key);

    // ffmpeg's muxer for an extension (the staging file has none): ".mp4" -> "mp4". Empty if we
    // don't rewrite that format.
    std::wstring MuxerFor(const std::wstring& lowerExtension);

    // Output options that copy picture, sound and subtitles as they are (no re-encode), and drop
    // global, stream and chapter metadata, chapters, data tracks, cover art and attachments.
    std::vector<std::wstring> StripMediaArgs(const std::wstring& lowerExtension);
}
