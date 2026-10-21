#include "canvas/PictureEngine.h"
#include "FileIo.h"
#include "Media.h"
#include "OutputPath.h"
#include "metadata/Metadata.h"

#include <format>

namespace sd::canvas
{
    PictureEngine::PictureEngine(const EngineServices& services, std::shared_ptr<const ImageCodecs> codecs)
        : m_services(services), m_codecs(std::move(codecs))
    {
    }

    bool PictureEngine::Applies(const Recipe&, const std::wstring& path) const
    {
        const auto ext = LowerExtension(path);
        return MediaKindOfExtension(ext) == MediaKind::None && m_codecs && m_codecs->CanDecode(ext);
    }

    bool PictureEngine::Accepts(const Recipe& recipe, const std::wstring& path, std::wstring* why) const
    {
        if (Applies(recipe, path))
            return true;
        if (why)
            *why = std::format(L"no image decoder for '{}' on this PC", LowerExtension(path));
        return false;
    }

    ItemResult CommitEdited(StagedOutput& out, const Raster& image, const std::wstring& source, std::wstring_view suffix, bool carryExif)
    {
        std::vector<uint8_t> bytes;
        std::wstring error;
        const ImageFormatInfo* format = nullptr;
        if (FAILED(EncodeEdited(image, source, bytes, error, &format)))
            return out.Fail(error);
        std::vector<uint8_t> original;
        if (carryExif && ReadFileBytes(source, original))
            metadata::CarryEditedExif(original, bytes, static_cast<int>(image.width), static_cast<int>(image.height));
        if (!WriteFileBytes(out.Path(), bytes, WriteMode::Any))
            return out.Fail(L"cannot write next to the source");
        return out.Commit(suffix, format->extension);
    }
}
