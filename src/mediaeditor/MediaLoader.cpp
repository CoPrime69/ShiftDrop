#include "mediaeditor/MediaLoader.h"

namespace sd::mediatools
{
    MediaLoader::MediaLoader(std::shared_ptr<const FfmpegEngine> ffmpeg, const TempFolder* temp, HWND notify, UINT message)
        : m_ffmpeg(std::move(ffmpeg)), m_temp(temp), m_notify(notify), m_message(message)
    {
        m_interactive = std::jthread([this](std::stop_token stop) { Interactive(stop); });
        m_background = std::jthread([this](std::stop_token stop) { Background(stop); });
    }

    MediaLoader::~MediaLoader()
    {
        m_interactive.request_stop();
        m_background.request_stop();
        m_wake.notify_all();
        if (m_interactive.joinable())
            m_interactive.join();
        if (m_background.joinable())
            m_background.join();
    }

    void MediaLoader::Probe(int file, const std::wstring& path)
    {
        std::lock_guard lock(m_lock);
        m_probes.push_back({ file, path });
        m_wake.notify_all();
    }

    void MediaLoader::Preview(int file, const std::wstring& path, const ProbeInfo& info, int64_t us, int maxW, int maxH, uint64_t serial)
    {
        std::lock_guard lock(m_lock);
        m_preview = FileJob{ file, path, info, us, maxW, maxH, serial, false };
        m_wake.notify_all();
    }

    void MediaLoader::Thumbs(int file, const std::wstring& path, const ProbeInfo& info, int maxW, int maxH, std::vector<ThumbJob> jobs, bool replace)
    {
        std::lock_guard lock(m_lock);
        if (replace)
            m_thumbs.clear();
        for (const auto& j : jobs)
            m_thumbs.push_back({ file, path, info, j.us, maxW, maxH, j.key, j.fast });
        m_wake.notify_all();
    }

    void MediaLoader::Waveform(int file, const std::wstring& path, int64_t durationUs)
    {
        std::lock_guard lock(m_lock);
        m_waveforms.push_back({ file, path, {}, durationUs });
        m_wake.notify_all();
    }

    std::vector<LoadResult> MediaLoader::Take()
    {
        std::lock_guard lock(m_lock);
        return std::exchange(m_results, {});
    }

    void MediaLoader::Post(LoadResult result)
    {
        bool first = false;
        {
            std::lock_guard lock(m_lock);
            first = m_results.empty();
            // A newer progress report replaces an older one still waiting.
            if (result.kind == LoadResult::Kind::WaveformProgress && !m_results.empty() && m_results.back().kind == result.kind)
                m_results.back() = std::move(result);
            else
                m_results.push_back(std::move(result));
        }
        if (first)
            PostMessageW(m_notify, m_message, 0, 0);
    }

    void MediaLoader::Interactive(std::stop_token stop)
    {
        while (!stop.stop_requested())
        {
            FileJob job;
            bool probe = false;
            {
                std::unique_lock lock(m_lock);
                m_wake.wait(lock, stop, [&] { return !m_probes.empty() || m_preview; });
                if (stop.stop_requested())
                    return;
                if (!m_probes.empty())
                {
                    job = std::move(m_probes.front());
                    m_probes.pop_front();
                    probe = true;
                }
                else
                {
                    job = std::move(*m_preview);
                    m_preview.reset();
                }
            }
            LoadResult r;
            r.file = job.file;
            if (probe)
            {
                r.kind = LoadResult::Kind::Probe;
                r.info = mediatools::Probe(*m_ffmpeg, job.path, stop);
            }
            else
            {
                r.kind = LoadResult::Kind::Preview;
                r.us = job.us;
                r.key = job.key;
                if (m_temp)
                    ExtractFrame(*m_ffmpeg, job.path, job.info, job.us, job.maxW, job.maxH, false, *m_temp, stop, r.frame);
            }
            if (!stop.stop_requested())
                Post(std::move(r));
        }
    }

    void MediaLoader::Background(std::stop_token stop)
    {
        while (!stop.stop_requested())
        {
            FileJob job;
            bool waveform = false;
            {
                std::unique_lock lock(m_lock);
                m_wake.wait(lock, stop, [&] { return !m_waveforms.empty() || !m_thumbs.empty(); });
                if (stop.stop_requested())
                    return;
                waveform = !m_waveforms.empty();
                auto& queue = waveform ? m_waveforms : m_thumbs;
                job = std::move(queue.front());
                queue.pop_front();
            }
            LoadResult r;
            r.file = job.file;
            if (waveform)
            {
                r.kind = LoadResult::Kind::Waveform;
                const int rate = WaveformRate(job.us);
                std::vector<int16_t> pcm;
                const auto progress = [&](double p) {
                    LoadResult step;
                    step.kind = LoadResult::Kind::WaveformProgress;
                    step.file = job.file;
                    step.progress = p;
                    Post(std::move(step));
                };
                if (m_temp && ExtractMonoPcm(*m_ffmpeg, job.path, rate, *m_temp, stop, pcm, progress, job.us))
                {
                    auto peaks = std::make_shared<PeakPyramid>();
                    peaks->Build(std::move(pcm), rate);
                    r.peaks = std::move(peaks);
                }
            }
            else
            {
                r.kind = LoadResult::Kind::Thumb;
                r.us = job.us;
                r.key = job.key;
                if (m_temp)
                    ExtractFrame(*m_ffmpeg, job.path, job.info, job.us, job.maxW, job.maxH, job.fast, *m_temp, stop, r.frame);
            }
            if (!stop.stop_requested())
                Post(std::move(r));
        }
    }
}
