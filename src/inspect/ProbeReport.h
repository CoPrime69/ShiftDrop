#pragma once
#include "inspect/Metadata.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Everything ffmpeg's probe log says about a media file: container, tags, every stream with its own
// tags, and the chapters. A superset of Media.h's MediaInfo (which only keeps what conversions need),
// read from the same log (ProbeArgs), so nothing in Media.h changes.
namespace sd::inspect
{
    using Tags = std::vector<std::pair<std::string, std::string>>;   // key, value (UTF-8, as ffmpeg prints them)

    struct ProbeStream
    {
        std::string id;         // "0:1"
        std::string kind;       // "Video", "Audio", "Subtitle", "Data", "Attachment"
        std::string codec;      // "h264"
        std::string details;    // the rest of the line after the codec
        std::string language;   // "eng"; empty for "und"
        bool isDefault = false;
        bool attachedPic = false;   // cover art
        int width = 0, height = 0;
        double fps = 0;
        int sampleRate = 0;
        std::string channels;   // "stereo", "5.1(side)", "mono"
        int64_t bitrate = 0;    // bits per second, 0 when not printed
        Tags tags;
    };

    struct ProbeChapter
    {
        double start = 0, end = 0;   // seconds
        std::string title;
        Tags tags;
    };

    struct ProbeReport
    {
        std::string container;   // "mov,mp4,m4a,3gp,3g2,mj2"
        int64_t durationUs = -1;
        int64_t bitrate = 0;     // overall, bits per second
        Tags tags;
        std::vector<ProbeStream> streams;
        std::vector<ProbeChapter> chapters;
        bool ok = false;         // an "Input #0" was found
    };

    ProbeReport ParseProbeReport(const std::vector<std::string>& log);

    // Inspector fields: Media, then "Video track 1", "Audio track 1"..., Chapters and Tags.
    std::vector<Field> ProbeFields(const ProbeReport& report);

    // "channels" text to a count: mono 1, stereo 2, "5.1" 6, "7.1" 8; 0 when unknown.
    int ChannelCount(const std::string& layout);
}
