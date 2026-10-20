#include "mediaeditor/Playback.h"
#include "Log.h"

#include <xaudio2.h>

#include <algorithm>
#include <chrono>

namespace sd::mediatools
{
    namespace
    {
        // The first chunk is short so sound starts quickly; the rest are longer.
        constexpr int64_t kFirstChunkUs = 1'500'000;
        constexpr int64_t kChunkUs = 6'000'000;
        // The worker keeps this much decoded ahead of what is heard.
        constexpr uint64_t kAheadFrames = Playback::kRate * 4;

        using CreateFn = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
    }

    Playback::Playback(std::shared_ptr<const FfmpegEngine> ffmpeg, const TempFolder* temp) : m_ffmpeg(std::move(ffmpeg)), m_temp(temp)
    {
        QueryPerformanceFrequency(&m_clockFreq);
        // XAudio2 2.9 ships with Windows 10 and later; loaded here so a PC without it still runs
        // (the editor then previews without sound).
        m_dll = LoadLibraryExW(L"xaudio2_9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto create = m_dll ? reinterpret_cast<CreateFn>(GetProcAddress(m_dll, "XAudio2Create")) : nullptr;
        if (create && SUCCEEDED(create(&m_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)) &&
            SUCCEEDED(m_xaudio->CreateMasteringVoice(&m_master, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, nullptr, nullptr,
                                                     AudioCategory_Media)))
        {
            WAVEFORMATEX format{};
            format.wFormatTag = WAVE_FORMAT_PCM;
            format.nChannels = 2;
            format.nSamplesPerSec = kRate;
            format.wBitsPerSample = 16;
            format.nBlockAlign = 4;
            format.nAvgBytesPerSec = kRate * 4;
            if (FAILED(m_xaudio->CreateSourceVoice(&m_source, &format)))
                m_source = nullptr;
        }
        if (!m_source)
            log::Warn(L"media editor: no sound device (XAudio2); playback moves the playhead without sound");
        m_thread = std::jthread([this](std::stop_token stop) { Worker(stop); });
    }

    Playback::~Playback()
    {
        {
            std::lock_guard lock(m_lock);
            m_cancel.request_stop();
        }
        m_thread.request_stop();
        m_wake.notify_all();
        if (m_thread.joinable())
            m_thread.join();
        // Destroying a voice waits for XAudio2 to stop reading its buffers.
        if (m_source)
            m_source->DestroyVoice();
        if (m_master)
            m_master->DestroyVoice();
        if (m_xaudio)
            m_xaudio->Release();
        if (m_dll)
            FreeLibrary(m_dll);
    }

    void Playback::Play(const std::wstring& path, TimeRange range, FilterFn filter, double rate, bool withSound)
    {
        Stop();
        std::lock_guard lock(m_lock);
        m_range = range;
        m_rate = rate > 0 ? rate : 1;
        m_withSound = withSound && m_source && m_ffmpeg;
        m_playing = true;
        m_decodedAll = false;
        m_submittedFrames = 0;
        m_started = false;
        QueryPerformanceCounter(&m_clockStart);
        if (m_withSound)
        {
            m_cancel = std::stop_source();
            m_request = { path, range, std::move(filter), m_rate, ++m_generation };
            m_wake.notify_all();
        }
    }

    void Playback::Stop()
    {
        const int64_t at = PositionUs();
        std::lock_guard lock(m_lock);
        if (!m_playing)
            return;
        m_stoppedAt = at;
        m_playing = false;
        m_cancel.request_stop();
        ++m_generation;   // chunks still being decoded for the old request are dropped
        m_request = {};
        if (m_source)
        {
            m_source->Stop();
            m_source->FlushSourceBuffers();
            // XAudio2 lets go of flushed buffers on its next pass; until then they are kept.
            const ULONGLONG now = GetTickCount64();
            for (auto& b : m_queued)
            {
                b.retiredAt = now;
                m_retired.push_back(std::move(b));
            }
            m_queued.clear();
            ReleasePlayed();
        }
    }

    uint64_t Playback::PlayedFrames() const
    {
        if (!m_source || !m_started)
            return 0;
        XAUDIO2_VOICE_STATE state{};
        m_source->GetState(&state);
        return state.SamplesPlayed > m_baseSamples ? state.SamplesPlayed - m_baseSamples : 0;
    }

    int64_t Playback::PositionUs() const
    {
        std::lock_guard lock(m_lock);
        if (!m_playing)
            return m_stoppedAt;
        double heardUs = 0;
        if (m_withSound)
            heardUs = static_cast<double>(std::min(PlayedFrames(), m_submittedFrames)) * 1e6 / kRate;
        else
        {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            heardUs = static_cast<double>(now.QuadPart - m_clockStart.QuadPart) * 1e6 / static_cast<double>(m_clockFreq.QuadPart);
        }
        return std::min(m_range.end, m_range.start + static_cast<int64_t>(heardUs * m_rate));
    }

    bool Playback::AtEnd() const
    {
        if (!m_playing)
            return false;
        if (PositionUs() >= m_range.end)
            return true;
        std::lock_guard lock(m_lock);
        // With sound: everything decoded has been heard (the file may end before the range does).
        return m_withSound && m_decodedAll && m_started && PlayedFrames() >= m_submittedFrames;
    }

    void Playback::ReleasePlayed()
    {
        const ULONGLONG now = GetTickCount64();
        while (!m_retired.empty() && now - m_retired.front().retiredAt > 250)
            m_retired.pop_front();
        if (!m_source || m_queued.empty())
            return;
        // Buffers before the one XAudio2 is playing are finished; with none queued, all are.
        XAUDIO2_VOICE_STATE state{};
        m_source->GetState(&state);
        const auto current = reinterpret_cast<uint64_t>(state.pCurrentBufferContext);
        if (state.BuffersQueued == 0)
        {
            m_queued.clear();
            return;
        }
        const auto it = std::find_if(m_queued.begin(), m_queued.end(), [current](const Buffer& b) { return b.id == current; });
        if (it != m_queued.end())
            m_queued.erase(m_queued.begin(), it);
    }

    void Playback::Submit(std::vector<int16_t> pcm, uint64_t generation)
    {
        std::lock_guard lock(m_lock);
        if (generation != m_generation || pcm.empty() || !m_source)
            return;
        ReleasePlayed();
        m_queued.push_back({ m_nextBuffer++, 0, std::move(pcm) });
        const auto& buffer = m_queued.back();
        XAUDIO2_BUFFER xb{};
        xb.AudioBytes = static_cast<UINT32>(buffer.pcm.size() * sizeof(int16_t));
        xb.pAudioData = reinterpret_cast<const BYTE*>(buffer.pcm.data());
        xb.pContext = reinterpret_cast<void*>(buffer.id);
        if (FAILED(m_source->SubmitSourceBuffer(&xb)))
        {
            m_queued.pop_back();
            return;
        }
        m_submittedFrames += buffer.pcm.size() / 2;
        if (!m_started)
        {
            XAUDIO2_VOICE_STATE state{};
            m_source->GetState(&state);
            m_baseSamples = state.SamplesPlayed;
            m_source->Start();
            m_started = true;
        }
    }

    void Playback::Worker(std::stop_token stop)
    {
        while (!stop.stop_requested())
        {
            Request request;
            std::stop_token cancel;
            {
                std::unique_lock lock(m_lock);
                m_wake.wait(lock, stop, [&] { return !m_request.path.empty(); });
                if (stop.stop_requested())
                    return;
                request = std::move(m_request);
                m_request = {};
                cancel = m_cancel.get_token();
            }
            int64_t at = request.range.start;
            bool first = true;
            while (!stop.stop_requested() && !cancel.stop_requested() && at < request.range.end)
            {
                const TimeRange chunk{ at, std::min(request.range.end, at + (first ? kFirstChunkUs : kChunkUs)) };
                const Filter filter = request.filter ? request.filter(chunk) : Filter{};
                std::vector<int16_t> pcm;
                if (!m_temp || !DecodePlaybackPcm(*m_ffmpeg, request.path, chunk, filter.graph, filter.complex, kRate, *m_temp, cancel, pcm))
                    break;
                Submit(std::move(pcm), request.generation);
                first = false;
                at = chunk.end;
                // Stay a few seconds ahead of what is heard, no more.
                std::unique_lock lock(m_lock);
                while (!stop.stop_requested() && !cancel.stop_requested() && request.generation == m_generation &&
                       m_submittedFrames > PlayedFrames() + kAheadFrames)
                    m_wake.wait_for(lock, stop, std::chrono::milliseconds(60), [] { return false; });
            }
            std::lock_guard lock(m_lock);
            if (request.generation == m_generation)
                m_decodedAll = true;
        }
    }
}
