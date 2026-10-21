#pragma once
#include "Engine.h"
#include "ImagePipeline.h"
#include "canvas/Raster.h"

#include <memory>
#include <string>
#include <string_view>

// What the engines that edit one decoded picture share (Crop, Redact and the photo tools): which
// files they take, and how an edited copy is written.
namespace sd::canvas
{
    // Takes any picture this PC can decode, not a video or audio file; says which decoder is missing
    // otherwise.
    class PictureEngine : public IEngine
    {
    public:
        bool Applies(const Recipe& recipe, const std::wstring& path) const override;
        bool Accepts(const Recipe& recipe, const std::wstring& path, std::wstring* why) const override;

    protected:
        PictureEngine(const EngineServices& services, std::shared_ptr<const ImageCodecs> codecs);

        EngineServices m_services;
        std::shared_ptr<const ImageCodecs> m_codecs;
    };

    // Writes `image` into `out` as an edited copy of `source` (Raster.h: EncodeEdited) and commits it
    // as "name (suffix).ext". With `carryExif`, the source's dates, camera and location stay with the
    // copy; its old thumbnail and orientation don't.
    ItemResult CommitEdited(StagedOutput& out, const Raster& image, const std::wstring& source, std::wstring_view suffix, bool carryExif);
}
