#pragma once
#include "Engine.h"
#include "ImagePipeline.h"

#include <memory>

namespace sd::sharesafe
{
    // Share-safe (S5, sharesafe.clean) and Remove metadata (metadata.remove): a copy to send that
    // carries nothing about the person or the device. Pictures lose EXIF, GPS, XMP, IPTC, maker notes,
    // thumbnails, comments and trailing data (losslessly where the container allows); video and
    // audio are stream-copied by ffmpeg without global, stream or chapter metadata. Every output is
    // read back and checked before it gets its final name, and the result line says what went.
    //
    // Share-safe also looks for personal details written in pictures (emails, phone numbers, card
    // numbers, IBANs) with Windows' own text recognition, and offers covers for them in the Redact
    // editor before anything is written.
    class ShareSafeEngine final : public IEngine
    {
    public:
        ShareSafeEngine(const EngineServices& services, std::shared_ptr<const ImageCodecs> codecs);

        bool Handles(const Recipe& recipe) const override;
        bool Applies(const Recipe& recipe, const std::wstring& path) const override;
        bool Accepts(const Recipe& recipe, const std::wstring& path, std::wstring* why) const override;
        JobLane Lane(const Recipe&) const override { return JobLane::Media; }   // video and audio go through ffmpeg
        ItemResult Run(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop,
                       const ProgressFn& progress) const override;
        bool NeedsEditor(const Recipe& recipe) const override;
        void OpenEditor(HWND owner, const Recipe& recipe, const std::vector<std::wstring>& files,
                        std::function<void(Recipe adjusted, std::vector<std::wstring> files)> done) const override;

    private:
        bool IsImage(const std::wstring& lowerExtension) const;
        ItemResult RunImage(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop, const ProgressFn& progress) const;
        ItemResult RunMedia(const Recipe& recipe, int64_t runId, const std::wstring& source, std::stop_token stop, const ProgressFn& progress) const;

        EngineServices m_services;
        std::shared_ptr<const ImageCodecs> m_codecs;
    };
}
