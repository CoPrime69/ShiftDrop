#include "Action.h"
#include "Media.h"

namespace sd
{
    void RegisterModuleActions(ActionRegistry& registry);   // Modules.cpp

    const std::vector<ImageFormatInfo>& ImageFormats()
    {
        static const std::vector<ImageFormatInfo> formats = {
            { "png", L"PNG", L".png", L".png", true, false },
            { "jpg", L"JPG", L".jpg", L".jpg.jpeg.jpe.jfif", false, true },
            { "webp", L"WebP", L".webp", L".webp", true, true },
            { "avif", L"AVIF", L".avif", L".avif", true, true },                      // libavif (src/formats)
            { "heic", L"HEIC", L".heic", L".heic.heif.hif", false, true, true },      // Windows' HEIF codec, when installed
            { "tiff", L"TIFF", L".tiff", L".tif.tiff", true, false },
            { "bmp", L"BMP", L".bmp", L".bmp.dib", false, false },
        };
        return formats;
    }

    const ImageFormatInfo* FindImageFormat(std::string_view id)
    {
        for (const auto& f : ImageFormats())
            if (id == f.id)
                return &f;
        return nullptr;
    }

    bool IsAlreadyFormat(const ImageFormatInfo& format, const std::wstring& ext)
    {
        if (ext.empty())
            return false;
        // aliases is a run of ".ext" tokens; match whole tokens only.
        std::wstring_view all(format.aliases);
        size_t pos = 0;
        while ((pos = all.find(ext, pos)) != std::wstring_view::npos)
        {
            const size_t end = pos + ext.size();
            if (end == all.size() || all[end] == L'.')
                return true;
            pos = end;
        }
        return false;
    }

    const ImageFormatInfo* FormatOfExtension(const std::wstring& ext)
    {
        for (const auto& f : ImageFormats())
            if (IsAlreadyFormat(f, ext))
                return &f;
        return nullptr;
    }

    ActionRegistry::ActionRegistry()
    {
        std::vector<std::string> formatIds;
        for (const auto& f : ImageFormats())
            formatIds.emplace_back(f.id);
        formatIds.emplace_back("same");   // keep the source's format (PNG if it can't be written back)

        m_actions.push_back(ActionDescriptor{
            "image.resize", L"Resize", StepKind::ImageTransform,
            { ParamSpec{ "maxEdge", ParamType::Int, 16, 65535, {}, 2048 } } });
        m_actions.push_back(ActionDescriptor{
            "image.scale", L"Scale", StepKind::ImageTransform,
            { ParamSpec{ "percent", ParamType::Int, 1, 100, {}, 50 } } });
        m_actions.push_back(ActionDescriptor{
            "image.rotate", L"Rotate", StepKind::ImageTransform,
            { ParamSpec{ "degrees", ParamType::Enum, 0, 0, { "90", "180", "270" }, "90" } } });
        m_actions.push_back(ActionDescriptor{
            "image.flip", L"Flip", StepKind::ImageTransform,
            { ParamSpec{ "axis", ParamType::Enum, 0, 0, { "horizontal", "vertical" }, "horizontal" } } });
        m_actions.push_back(ActionDescriptor{ "image.strip_metadata", L"Strip metadata", StepKind::ImageMetadata, {} });

        // File actions (Alt wheel). They take any file or folder and produce no new image.
        m_actions.push_back(ActionDescriptor{ "file.copy", L"Copy", StepKind::FileAction, {} });
        m_actions.push_back(ActionDescriptor{ "file.move", L"Move to", StepKind::FileAction, {} });
        m_actions.push_back(ActionDescriptor{ "file.copy_path", L"Copy path", StepKind::FileAction, {} });
        m_actions.push_back(ActionDescriptor{ "file.terminal", L"Terminal", StepKind::FileAction, {} });
        m_actions.push_back(ActionDescriptor{ "file.duplicate", L"Duplicate", StepKind::FileAction, {} });
        std::vector<std::string> mediaIds;
        for (const auto& f : MediaFormats())
            mediaIds.emplace_back(f.id);
        m_actions.push_back(ActionDescriptor{
            "media.convert", L"Convert media", StepKind::MediaEncode,
            { ParamSpec{ "format", ParamType::Enum, 0, 0, mediaIds, "mp4" } } });
        // Compress and Fit to… for video (H.264/AAC MP4) and audio (AAC M4A). With targetMB or percent
        // the result is guaranteed to be at most that size; with neither, a quality preset.
        m_actions.push_back(ActionDescriptor{
            "media.compress", L"Compress media", StepKind::MediaEncode,
            { ParamSpec{ "targetMB", ParamType::Number, 0, 100000, {}, 0 },
              ParamSpec{ "percent", ParamType::Int, 0, 99, {}, 0 },        // of the source's size
              ParamSpec{ "maxHeight", ParamType::Int, 0, 4320, {}, 1080 } } });   // 0 = keep

        // The image counterpart: JPEG (WebP and AVIF stay themselves), at `quality`, or at the best quality that fits
        // maxKB / percent of the source, shrinking the picture only if even low quality is too big.
        m_actions.push_back(ActionDescriptor{
            "image.compress", L"Compress", StepKind::ImageEncode,
            { ParamSpec{ "maxKB", ParamType::Int, 0, 10'000'000, {}, 0 },
              ParamSpec{ "percent", ParamType::Int, 0, 99, {}, 0 },
              ParamSpec{ "quality", ParamType::Int, 1, 100, {}, 75 } } });
        m_actions.push_back(ActionDescriptor{
            "image.encode", L"Encode", StepKind::ImageEncode,
            { ParamSpec{ "format", ParamType::Enum, 0, 0, formatIds, "png" },
              ParamSpec{ "quality", ParamType::Int, 1, 100, {}, 90 } } });
        RegisterModuleActions(*this);
    }

    const ActionDescriptor* ActionRegistry::Find(std::string_view id) const
    {
        for (const auto& a : m_actions)
            if (a.id == id)
                return &a;
        return nullptr;
    }
}
