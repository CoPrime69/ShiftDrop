#pragma once
#include "Engine.h"
#include "Ffmpeg.h"
#include "HistoryStore.h"
#include "ImagePipeline.h"
#include "Recipe.h"

#include <functional>
#include <memory>
#include <stop_token>
#include <string>

namespace sd
{
    // Runs one recipe on one file: staging file -> fused pipeline -> atomic commit -> hash -> history.
    // Safe to call from several worker threads at once.
    class PipelineRunner
    {
    public:
        // `media` may be null (tests without ffmpeg); media recipes then refuse every file.
        PipelineRunner(std::shared_ptr<const ImageCodecs> codecs, HistoryStore* history, std::shared_ptr<const FfmpegEngine> media = nullptr);

        // Is this recipe meant for this kind of file at all (an image recipe for an image, MP4 for a
        // video)? The wheel shows only such recipes, so a video gets video formats and a photo image
        // formats.
        bool Applies(const Recipe& recipe, const std::wstring& path) const;
        // Can this recipe take this file? `why` explains a "no" (shown in the wheel's centre / logs).
        bool Accepts(const Recipe& recipe, const std::wstring& path, std::wstring* why = nullptr) const;
        // True when running the recipe would be pointless: a one-step "to PNG" on a PNG.
        bool IsNoOp(const Recipe& recipe, const std::wstring& path) const;

        // The feature engine that runs this recipe (Engine.h), or null for the built-in image, media
        // and file paths. Engines come from Modules.cpp and are made by the constructor.
        const IEngine* EngineFor(const Recipe& recipe) const;
        JobLane Lane(const Recipe& recipe) const;

        // `progress` (optional) gets 0..1 for this file while it runs, from a worker thread; only media
        // recipes report it (an image takes a moment, a video can take minutes).
        ItemResult Run(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                       const std::function<void(double)>& progress = {}) const;

        // One output from every file (an engine's IsBatch recipe: join, merge, collage, zip).
        ItemResult RunBatch(const Recipe& recipe, int64_t runId, const std::vector<std::wstring>& sources, std::stop_token stop,
                            const std::function<void(double)>& progress = {}) const;

        // Deletes staging files left behind by a crash or kill in an earlier session.
        void RecoverInterrupted() const;

    private:
        ItemResult RunDuplicate(const Recipe& recipe, int64_t runId, const std::wstring& source,
                                const std::function<int64_t()>& elapsedMs) const;
        ItemResult RunMedia(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                            const std::function<void(double)>& progress, const std::function<int64_t()>& elapsedMs) const;
        FfmpegEngine::Result RunCompress(const Recipe& recipe, const std::wstring& source, const MediaInfo& info, H264Encoder h264,
                                         const std::wstring& staging, const std::function<void(double)>& progress, std::stop_token stop) const;
        std::shared_ptr<const ImageCodecs> m_codecs;
        HistoryStore* m_history;   // may be null (tests, or the database could not be opened)
        std::shared_ptr<const FfmpegEngine> m_media;
        std::vector<std::unique_ptr<IEngine>> m_engines;
    };
}
