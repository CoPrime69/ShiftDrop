#pragma once
#include "Engine.h"
#include "Ffmpeg.h"
#include "Media.h"
#include "mediaeditor/MediaProbe.h"
#include "mediaeditor/Timeline.h"

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

// What the video tools, the audio tools and the editor share: ffmpeg command lines with several
// inputs, which container and codecs an edited file goes into, frame and PCM extraction for the
// editor, and running ffmpeg into a staged output. The argument builders are pure, so they are
// unit-tested; the runners are thin.
namespace sd::mediatools
{
    // ---- Command lines ---------------------------------------------------------------------------

    struct Input
    {
        std::wstring path;
        std::vector<std::wstring> options;   // input options, before its -i (-ss, -hwaccel, -f concat...)
    };
    // FfmpegArgs with several inputs. -protocol_whitelist is a per-input option, so every input gets
    // its own "file" whitelist: none of them can be a URL.
    std::vector<std::wstring> MultiInputArgs(const std::vector<Input>& inputs, const std::vector<std::wstring>& outputArgs,
                                             const std::wstring& output);

    // "12.345" (seconds, millisecond precision): how ffmpeg takes a time.
    std::wstring Seconds(int64_t us);
    // Shortest decimal for a number in a filter ("0.5", "1.25", "3"), rounded to 6 places.
    std::wstring Num(double value);
    // A frame rate for the fps filter: "30000/1001" for 29.97, "25" for 25.
    std::wstring FpsExpr(double fps);
    // Seconds as a JSON number, and back.
    double ToSeconds(int64_t us);
    int64_t FromSeconds(double seconds);

    // ---- Where an edited file goes -----------------------------------------------------------------

    struct Container
    {
        std::wstring muxer;       // for -f (the staging file's ".sdpart" says nothing)
        std::wstring extension;   // ".mp4"
    };
    // Streams copied as they are (Fast trim/split, join of matching clips): the source's own
    // container, or Matroska, which takes almost anything, for containers ffmpeg can't write well.
    Container CopyContainer(const std::wstring& lowerExtension);
    // Re-encoded to H.264 + AAC: MOV and MKV stay what they are, everything else becomes MP4.
    Container EncodeContainer(const std::wstring& lowerExtension);
    // Can this container hold that audio codec as it is (so the soundtrack is copied, not re-encoded)?
    bool ContainerTakesAudio(const Container& container, const std::string& codec);

    // H.264 at "edit" quality (constant quality 20 on GPUs, a bitrate from the frame size elsewhere),
    // even dimensions guaranteed by the caller's filters.
    std::vector<std::wstring> EditVideoArgs(H264Encoder encoder, int width, int height, double fps);
    // -hwaccel auto when a GPU encoder is in use: decoding on the same GPU saves the CPU a lot, and
    // ffmpeg falls back to software by itself.
    std::vector<std::wstring> DecodeArgs(H264Encoder encoder);
    // The soundtrack for a re-encoded video: copied when the container takes it, AAC otherwise.
    std::vector<std::wstring> SoundtrackArgs(const Container& container, const ProbeInfo& info);

    // Audio tools write the format the source already is (MP3 stays MP3); anything that isn't one of
    // ours becomes M4A.
    const MediaFormatInfo& AudioOutputFormat(const std::wstring& lowerExtension);
    // That format's codec and muxer arguments, without the stream mapping (the tool maps its filter's
    // output).
    std::vector<std::wstring> AudioCodecArgs(const MediaFormatInfo& format, int sampleRate);

    // ---- Extraction for the editor -------------------------------------------------------------------

    // A folder of our own under %TEMP%, deleted with everything in it when this goes away.
    class TempFolder
    {
    public:
        TempFolder();
        ~TempFolder();
        TempFolder(const TempFolder&) = delete;
        TempFolder& operator=(const TempFolder&) = delete;
        bool Ok() const { return !m_path.empty(); }
        const std::wstring& Path() const { return m_path; }
        std::wstring File(const std::wstring& name) const { return m_path + L"\\" + name; }

    private:
        std::wstring m_path;
    };

    // The size a frame is scaled to so it fits `maxW` x `maxH` (shown shape, pixel aspect applied), even.
    void FitFrame(const ProbeInfo& info, int maxW, int maxH, int& w, int& h);
    // One frame at `us` as raw BGRA of exactly w x h. `fast` = the nearest keyframe (filmstrip),
    // otherwise the exact frame showing at `us` (preview).
    std::vector<std::wstring> FrameArgs(const std::wstring& input, int64_t us, double fps, int w, int h, bool fast, const std::wstring& output);
    // Mono 16-bit PCM at `sampleRate` for the waveform.
    std::vector<std::wstring> MonoPcmArgs(const std::wstring& input, int sampleRate, const std::wstring& output);
    // Stereo 16-bit PCM of [start, end) at `sampleRate` for playback, through `filter` (an -af chain, or
    // a -filter_complex graph with [0:a] in and [out] out when `complex`; empty = as it is).
    std::vector<std::wstring> PlaybackPcmArgs(const std::wstring& input, TimeRange range, const std::wstring& filter, bool complex,
                                              int sampleRate, const std::wstring& output);
    // A sample rate for the waveform that keeps a long recording to a few million samples.
    int WaveformRate(int64_t durationUs);
    // Where to seek for the frame showing at `us`: a quarter frame early, so rounding never skips it.
    int64_t FrameSeekUs(int64_t us, double fps);

    struct Frame
    {
        std::vector<uint32_t> bgra;   // opaque, top-down
        int w = 0, h = 0;
    };
    bool ExtractFrame(const FfmpegEngine& engine, const std::wstring& input, const ProbeInfo& info, int64_t us, int maxW, int maxH,
                      bool fast, const TempFolder& temp, std::stop_token stop, Frame& out);
    bool ExtractMonoPcm(const FfmpegEngine& engine, const std::wstring& input, int sampleRate, const TempFolder& temp, std::stop_token stop,
                        std::vector<int16_t>& out, const ProgressFn& progress = {}, int64_t durationUs = 0);
    bool DecodePlaybackPcm(const FfmpegEngine& engine, const std::wstring& input, TimeRange range, const std::wstring& filter, bool complex,
                           int sampleRate, const TempFolder& temp, std::stop_token stop, std::vector<int16_t>& out);

    // ---- Running a tool ------------------------------------------------------------------------------

    // One ffmpeg run into a staged file next to `source`, committed as "name (suffix).ext".
    // `args` gets the staging path; `outputUs` is the result's length (for progress); `log`, if given,
    // receives ffmpeg's log.
    ItemResult RunToStaged(const FfmpegEngine& engine, HistoryStore* history, int64_t runId, const std::wstring& source,
                           const std::function<std::vector<std::wstring>(const std::wstring& staging)>& args, int64_t outputUs,
                           std::wstring_view suffix, std::wstring_view extension, std::stop_token stop, const ProgressFn& progress,
                           std::vector<std::string>* log = nullptr);

    // Several ffmpeg runs into one staged folder ("name (suffix)"), each told its share of the
    // progress. `run(i, folder, progress)` does step i of `steps` and returns its result.
    ItemResult RunToStagedFolder(HistoryStore* history, int64_t runId, const std::wstring& source, int steps,
                                 const std::function<FfmpegEngine::Result(int i, const std::wstring& folder, const ProgressFn& progress)>& run,
                                 std::wstring_view suffix, std::stop_token stop, const ProgressFn& progress);

    // A failure before any output exists (no video stream, nothing to do), recorded in history like
    // every other result. `cancelled` records it as skipped.
    ItemResult FailItem(HistoryStore* history, int64_t runId, const std::wstring& source, std::wstring error, bool cancelled = false);

    // A file name part safe in an ffmpeg output pattern (the segment muxer reads % as a field).
    std::wstring EscapePercent(const std::wstring& text);
    // Stem (no folder, no extension) of a path.
    std::wstring Stem(const std::wstring& path);
}
