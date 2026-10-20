#pragma once
#include "Ffmpeg.h"
#include "mediaeditor/MediaProbe.h"
#include "mediaeditor/MediaTools.h"
#include "mediaeditor/Timeline.h"

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// The editor's background work, so the window never waits on ffmpeg: one thread for what the user is
// looking at right now (probes, then the newest preview frame) and one for the rest (waveforms, then
// filmstrip thumbnails). Results come back through a posted message; the window takes them on its own
// thread. Destroying the loader stops both threads (and the ffmpeg they run) and waits for them.
namespace sd::mediatools
{
    struct LoadResult
    {
        enum class Kind { Probe, Thumb, Preview, Waveform, WaveformProgress } kind = Kind::Probe;
        int file = 0;
        int64_t us = 0;                                // Thumb, Preview: the frame's time
        uint64_t key = 0;                              // Thumb: the caller's key; Preview: the request's serial
        ProbeInfo info;                                // Probe
        Frame frame;                                   // Thumb, Preview (empty if it could not be read)
        std::shared_ptr<const PeakPyramid> peaks;      // Waveform (null if it could not be read)
        double progress = 0;                           // WaveformProgress, 0..1
    };

    class MediaLoader
    {
    public:
        struct ThumbJob
        {
            int64_t us = 0;
            uint64_t key = 0;
            bool fast = true;   // nearest keyframe
        };

        MediaLoader(std::shared_ptr<const FfmpegEngine> ffmpeg, const TempFolder* temp, HWND notify, UINT message);
        ~MediaLoader();
        MediaLoader(const MediaLoader&) = delete;
        MediaLoader& operator=(const MediaLoader&) = delete;

        void Probe(int file, const std::wstring& path);
        // Replaces any preview not yet started: only the newest position matters.
        void Preview(int file, const std::wstring& path, const ProbeInfo& info, int64_t us, int maxW, int maxH, uint64_t serial);
        // Replaces the thumbnails not yet started (the view moved), or adds to them.
        void Thumbs(int file, const std::wstring& path, const ProbeInfo& info, int maxW, int maxH, std::vector<ThumbJob> jobs, bool replace = true);
        void Waveform(int file, const std::wstring& path, int64_t durationUs);

        std::vector<LoadResult> Take();

    private:
        struct FileJob
        {
            int file = 0;
            std::wstring path;
            ProbeInfo info;
            int64_t us = 0;
            int maxW = 0, maxH = 0;
            uint64_t key = 0;
            bool fast = false;
        };
        void Interactive(std::stop_token stop);
        void Background(std::stop_token stop);
        void Post(LoadResult result);

        std::shared_ptr<const FfmpegEngine> m_ffmpeg;
        const TempFolder* m_temp;
        HWND m_notify;
        UINT m_message;

        std::mutex m_lock;
        std::condition_variable_any m_wake;
        std::deque<FileJob> m_probes;
        std::optional<FileJob> m_preview;
        std::deque<FileJob> m_waveforms;
        std::deque<FileJob> m_thumbs;
        std::vector<LoadResult> m_results;

        std::jthread m_interactive;   // last: stopped and joined first
        std::jthread m_background;
    };
}
