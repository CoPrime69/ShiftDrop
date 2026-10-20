#pragma once
#include "Ffmpeg.h"
#include "Media.h"

#include <stop_token>
#include <string>
#include <vector>

// What the video and audio tools need to know about an input, beyond Media.h's MediaInfo: the size
// as it is shown (phones store portrait video as landscape plus a rotation), and the audio layout.
namespace sd::mediatools
{
    struct ProbeInfo
    {
        MediaInfo media;          // codecs, coded size, frame rate, duration
        int rotation = 0;         // 0, 90, 180 or 270: how the player turns the picture
        int width = 0;            // as shown, rotation applied (ffmpeg's filters see this size)
        int height = 0;
        double sar = 1.0;         // pixel aspect ratio (1 = square pixels)
        int channels = 0;         // audio channels (0 = no audio)
        int sampleRate = 0;       // Hz
        bool coverArt = false;    // a song with an attached picture

        bool HasVideo() const { return !media.videoCodec.empty(); }
        bool HasAudio() const { return !media.audioCodec.empty(); }
        int64_t DurationUs() const { return media.durationUs; }
    };

    // From the log of a probe run (ProbeArgs).
    ProbeInfo ParseProbe(const std::vector<std::string>& log);
    // One quick ffmpeg run that writes nothing.
    ProbeInfo Probe(const FfmpegEngine& engine, const std::wstring& path, std::stop_token stop = {});

    // "stereo" -> 2, "mono" -> 1, "5.1(side)" -> 6, "3 channels" -> 3; 0 if not a layout.
    int ChannelsOfLayout(std::string_view layout);
}
