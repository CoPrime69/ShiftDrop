#pragma once
#include "Ffmpeg.h"
#include "mediaeditor/MediaTools.h"
#include "mediaeditor/Timeline.h"

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

// Listening in the editor: ffmpeg decodes the part to play (through the tool's filter, so Bleep and
// Channels can be heard before they are applied) in short chunks on a worker thread, and XAudio2
// plays them back to back. Without a sound device (or for a silent video) a clock moves the playhead
// instead, so playback still works as a preview.
namespace sd::mediatools
{
    class Playback
    {
    public:
        static constexpr int kRate = 48000;   // stereo s16 at this rate, whatever the source

        // The filter for one chunk [start, end) of the source: an -af chain, or a -filter_complex graph
        // ([0:a] in, [out] out) when `complex` is set. Called on the worker thread.
        struct Filter
        {
            std::wstring graph;
            bool complex = false;
        };
        using FilterFn = std::function<Filter(TimeRange chunk)>;

        Playback(std::shared_ptr<const FfmpegEngine> ffmpeg, const TempFolder* temp);
        ~Playback();
        Playback(const Playback&) = delete;
        Playback& operator=(const Playback&) = delete;

        // Plays `range` of `path` (stops what was playing). `rate` is how fast the source moves per
        // second heard (2 for a 2x speed preview). `withSound` false = the clock only.
        void Play(const std::wstring& path, TimeRange range, FilterFn filter, double rate, bool withSound);
        void Stop();
        bool Playing() const { return m_playing; }
        // The source time being heard now (moves while playing; stays put after Stop).
        int64_t PositionUs() const;
        // True once the range has been played through (the caller then stops).
        bool AtEnd() const;
        bool HasDevice() const { return m_source != nullptr; }

    private:
        struct Request
        {
            std::wstring path;
            TimeRange range;
            FilterFn filter;
            double rate = 1;
            uint64_t generation = 0;
        };
        void Worker(std::stop_token stop);
        void Submit(std::vector<int16_t> pcm, uint64_t generation);
        uint64_t PlayedFrames() const;   // since the current Play
        void ReleasePlayed();            // frees buffers XAudio2 has finished with (m_lock held)

        std::shared_ptr<const FfmpegEngine> m_ffmpeg;
        const TempFolder* m_temp;

        HMODULE m_dll = nullptr;
        IXAudio2* m_xaudio = nullptr;
        IXAudio2MasteringVoice* m_master = nullptr;
        IXAudio2SourceVoice* m_source = nullptr;

        mutable std::mutex m_lock;
        std::condition_variable_any m_wake;
        Request m_request;                    // the newest Play
        std::stop_source m_cancel;            // stops the chunk being decoded for an old request
        uint64_t m_generation = 0;
        struct Buffer
        {
            uint64_t id = 0;              // XAudio2's buffer context, to tell which one it is playing
            ULONGLONG retiredAt = 0;      // flushed by Stop at this tick
            std::vector<int16_t> pcm;
        };
        std::deque<Buffer> m_queued;      // submitted, oldest first
        std::deque<Buffer> m_retired;     // flushed; freed after a grace period (XAudio2 lets go on its next pass)
        uint64_t m_nextBuffer = 1;
        uint64_t m_submittedFrames = 0;
        uint64_t m_baseSamples = 0;           // XAudio2's SamplesPlayed when this Play started
        bool m_started = false;               // the voice is running
        bool m_decodedAll = false;            // the worker has submitted the whole range

        bool m_playing = false;
        bool m_withSound = false;
        TimeRange m_range;
        double m_rate = 1;
        int64_t m_stoppedAt = 0;
        LARGE_INTEGER m_clockStart{}, m_clockFreq{};

        std::jthread m_thread;   // last: joined first
    };
}
