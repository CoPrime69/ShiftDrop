#pragma once
#include "Media.h"

#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace sd
{
    // The bundled ffmpeg.exe (LGPL build, installed in "engines" next to ShiftDrop.exe), run as a
    // child process: no console window, below-normal priority, local files only (see FfmpegArgs).
    //
    // Each run is in a job object that kills the process when the job handle closes, so cancelling,
    // or ShiftDrop exiting or crashing, never leaves an ffmpeg running in the background.
    class FfmpegEngine
    {
    public:
        explicit FfmpegEngine(std::wstring exe);   // empty: no engine (media slices are disabled)

        // %SHIFTDROP_FFMPEG% if set (tests, development), else engines\ffmpeg.exe next to our exe.
        // Empty if the file is not there.
        static std::wstring Locate();

        bool Available() const { return !m_exe.empty(); }
        const std::wstring& Path() const { return m_exe; }

        // The best H.264 encoder that works on this PC: each candidate encodes a few frames once, the
        // first time a video needs H.264. Thread-safe; later calls return the cached answer.
        H264Encoder H264() const;

        struct Result
        {
            bool ok = false;
            bool cancelled = false;
            std::wstring error;   // ffmpeg's own last error lines, for the log and the result panel
        };
        // Runs ffmpeg with `args` (after the program name). `progress` gets 0..1 from a reader thread
        // as the output advances, when the input's duration is known. Blocks until ffmpeg exits or
        // `stop` is requested (then the process is killed).
        // `log`, if given, receives ffmpeg's log lines (the first few hundred).
        Result Run(const std::vector<std::wstring>& args, const std::function<void(double)>& progress, std::stop_token stop,
                   std::vector<std::string>* log = nullptr) const;

        // Streams, size and duration of an input (one quick ffmpeg run that writes nothing).
        MediaInfo Probe(const std::wstring& input) const;

    private:
        std::wstring m_exe;
        mutable std::once_flag m_probeOnce;
        mutable H264Encoder m_h264 = H264Encoder::OpenH264;
    };
}
