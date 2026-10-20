#include "Action.h"
#include "Media.h"

namespace sd
{
    const std::vector<ImageFormatInfo>& ImageFormats()
    {
        static const std::vector<ImageFormatInfo> formats = {
            { "png", L"PNG", L".png", L".png", true, false },
            { "jpg", L"JPG", L".jpg", L".jpg.jpeg.jpe.jfif", false, true },
            { "webp", L"WebP", L".webp", L".webp", true, true },
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

        m_actions.push_back(ActionDescriptor{
            "image.encode", L"Encode", StepKind::ImageEncode,
            { ParamSpec{ "format", ParamType::Enum, 0, 0, formatIds, "png" },
              ParamSpec{ "quality", ParamType::Int, 1, 100, {}, 90 } } });
    }

    const ActionDescriptor* ActionRegistry::Find(std::string_view id) const
    {
        for (const auto& a : m_actions)
            if (a.id == id)
                return &a;
        return nullptr;
    }
}
