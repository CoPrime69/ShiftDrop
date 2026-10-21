#pragma once
#include "Media.h"
#include "mediaeditor/MediaProbe.h"
#include "mediaeditor/MediaTools.h"
#include "mediaeditor/Timeline.h"

#include <optional>
#include <string>
#include <vector>

// The audio tools (Normalize, Visualizer, Trim, Channels, Bleep) as ffmpeg command lines, plus the parsers for what ffmpeg reports back
// (loudnorm's measurement, silencedetect's periods). Pure; AudioEngine.cpp runs them. Every tool
// writes the format the source already is (an MP3 stays an MP3), at its sample rate.
namespace sd::audiotools
{
    using mediatools::ProbeInfo;
    using mediatools::TimeRange;

    // ---- 16. Normalize Volume (EBU R128, two passes) --------------------------------------------

    struct LoudnessTarget
    {
        double integrated = -16;   // LUFS
        double truePeak = -1;      // dBTP
        double range = 11;         // LU (loudness range allowed before loudnorm compresses)
    };
    // -14 streaming, -16 podcast, -23 broadcast (EBU R128); true peak -1 dBTP for all.
    LoudnessTarget TargetFor(double integrated);

    struct Loudness
    {
        double inputI = 0, inputTP = 0, inputLRA = 0, inputThresh = 0;
        double outputI = 0, outputTP = 0, outputLRA = 0, outputThresh = 0;
        double targetOffset = 0;
        bool silent = false;   // "-inf": nothing to measure
    };
    // The JSON block loudnorm prints at the end of a run with print_format=json (the last one in the log).
    std::optional<Loudness> ParseLoudnorm(const std::vector<std::string>& log);
    // The summary ebur128 prints at the end of a run (integrated loudness, its gate, loudness range
    // and true peak) as the input_* values; the output_* values stay 0.
    std::optional<Loudness> ParseEbur128(const std::vector<std::string>& log);

    // Pass 1 measures with ebur128 (BS.1770, true peak oversampled 4x): the numbers loudnorm's own
    // measuring pass gives, about four times faster, since loudnorm works at 192 kHz. Pass 2 applies
    // one gain for the whole file when the true peak allows it (a plain volume change, which is what
    // loudnorm's linear mode does), and loudnorm limits peaks dynamically only when it must.
    std::wstring MeasureFilter(const LoudnessTarget& target);
    bool IsLinear(const LoudnessTarget& target, const Loudness& measured);
    std::wstring ApplyFilter(const LoudnessTarget& target, const Loudness& measured);
    std::vector<std::wstring> MeasureArgs(const std::wstring& input, const LoudnessTarget& target);
    std::vector<std::wstring> NormalizeArgs(const std::wstring& input, const std::wstring& lowerExtension, const ProbeInfo& info,
                                            const LoudnessTarget& target, const Loudness& measured, const std::wstring& output);
    // "-23.4 -> -16.0 LUFS", with real minus signs
    std::wstring LoudnessNote(double before, double after);

    // ---- 17. Audio Visualizer --------------------------------------------------------------------

    enum class Shape { Landscape, Portrait, Square };
    Shape ShapeByName(std::string_view name);   // "landscape", "portrait", "square"
    void ShapeSize(Shape shape, int& w, int& h);  // 1920x1080, 1080x1920, 1080x1080
    // What moves in the video, each from ffmpeg's own audio-to-video filters:
    //   Bars      showfreqs: the spectrum as bars rising from a line
    //   Wave      showwaves: the waveform as a centred line
    //   Circle    avectorscope: the sound against its Hilbert transform (hilbert + afir), so each
    //             tone draws a circle as wide as it is loud
    //   Square    showfreqs at a few dozen bins, enlarged into a grid of square blocks
    //   Vertical  showfreqs bars mirrored up and down from the middle line
    //   Picture   no animation: the picture alone
    enum class VisualStyle { Bars, Wave, Circle, Square, Vertical, Picture };
    VisualStyle VisualStyleByName(std::string_view name);   // "bars", "waveform", "circle", "square", "vertical", "image"
    const char* VisualStyleName(VisualStyle style);
    // The moving picture of `audioIn` (a stream label such as "0:a:0") for a w x h video, on a
    // transparent background: a filtergraph ending in [vis]. Empty for Picture.
    std::wstring VisualGraph(VisualStyle style, int w, int h, const std::wstring& audioIn);
    // An H.264 + AAC MP4 as long as the audio: the style's animation on a dark background, or over
    // `image` (fitted to fill, darkened a little) when one is given. Picture is StillVideoArgs.
    std::vector<std::wstring> VisualizerArgs(const std::wstring& audio, const std::wstring& image, const ProbeInfo& info, VisualStyle style,
                                             Shape shape, H264Encoder encoder, const std::wstring& output);
    // A still picture (fitted, on black) with the audio.
    std::vector<std::wstring> StillVideoArgs(const std::wstring& audio, const std::wstring& image, const ProbeInfo& info, Shape shape,
                                             H264Encoder encoder, const std::wstring& output);
    // A song's cover art as a PNG (for the still picture when none is chosen).
    std::vector<std::wstring> CoverArtArgs(const std::wstring& audio, const std::wstring& output);

    // ---- 18. Trim Audio --------------------------------------------------------------------------

    inline constexpr double kSilenceDb = -50;            // quieter than this counts as silence
    inline constexpr int64_t kSilenceMinUs = 100'000;    // and for at least this long
    inline constexpr int64_t kSilencePadUs = 50'000;     // kept before the sound starts and after it ends
    std::vector<std::wstring> SilenceDetectArgs(const std::wstring& input, TimeRange range);
    // silencedetect's "silence_start: 1.2" / "silence_end: 3.4 | silence_duration: 2.2" lines, as
    // ranges on the file's timeline (`offset` = where the detection run started). A silence still
    // open at the end runs to `endUs`.
    std::vector<TimeRange> ParseSilence(const std::vector<std::string>& log, int64_t offsetUs, int64_t endUs);
    // `keep` without the silence at its start and end (all of it when it is all silence: nothing to trim).
    TimeRange TrimSilence(TimeRange keep, const std::vector<TimeRange>& silences);
    std::vector<std::wstring> TrimAudioArgs(const std::wstring& input, const std::wstring& lowerExtension, const ProbeInfo& info,
                                            TimeRange range, const std::wstring& output);

    // ---- 19. Convert Audio Channels --------------------------------------------------------------

    struct ChannelMix
    {
        bool mono = false;     // false = stereo
        bool swap = false;     // left and right exchanged
        double left = 1.0;     // gain of the left output (0..2)
        double right = 1.0;
    };
    // A pan filter for a source with `channels` channels (more than two are mixed down first).
    std::wstring PanFilter(const ChannelMix& mix, int channels);
    std::vector<std::wstring> ChannelsArgs(const std::wstring& input, const std::wstring& lowerExtension, const ProbeInfo& info,
                                           const ChannelMix& mix, const std::wstring& output);

    // ---- 20. Bleep Audio -------------------------------------------------------------------------

    // What is heard over the marked parts: a 1 kHz tone, nothing, or the sound turned down.
    enum class BleepSound { Tone, Silence, Duck };
    BleepSound BleepSoundByName(std::string_view name);   // "tone", "silence", "duck"
    inline constexpr double kDuckDb = 20, kMinDuckDb = 3, kMaxDuckDb = 60;   // how far Duck turns it down
    // [0:a] in, [out] out: the ranges muted, with a 1 kHz tone in them for Tone, or lowered by
    // `duckDb` for Duck. `offsetUs` shifts the ranges for a preview that starts later in the file.
    std::wstring BleepGraph(const std::vector<TimeRange>& ranges, BleepSound sound, double duckDb, int sampleRate, int64_t offsetUs = 0);
    std::vector<std::wstring> BleepArgs(const std::wstring& input, const std::wstring& lowerExtension, const ProbeInfo& info,
                                        const std::vector<TimeRange>& ranges, BleepSound sound, double duckDb, const std::wstring& output);

    // "between(t,a,b)+between(t,c,d)" for the ranges, shifted by -offset; "0" when there are none.
    std::wstring AnyBetween(const std::vector<TimeRange>& ranges, int64_t offsetUs = 0);
}
