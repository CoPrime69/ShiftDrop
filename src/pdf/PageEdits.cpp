#include "pdf/PageEdits.h"
#include "Brand.h"
#include "Log.h"
#include "canvas/Raster.h"
#include "crop/CropMath.h"
#include "crop/CropTool.h"
#include "photo/Adjust.h"
#include "photo/PhotoEngine.h"
#include "photo/PhotoTools.h"
#include "pdf/ImagePdf.h"
#include "redact/RedactEngine.h"
#include "redact/RedactTool.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>

namespace sd::pdf
{
    namespace
    {
        std::filesystem::path Root()
        {
            wchar_t temp[MAX_PATH + 1] = {};
            const DWORD n = GetTempPathW(MAX_PATH + 1, temp);
            if (n == 0 || n > MAX_PATH)
                return {};
            return std::filesystem::path(temp) / (SD_NAME L" pages");
        }
    }

    const std::vector<PageToolInfo>& PageTools()
    {
        static const std::vector<PageToolInfo> tools = {
            { PageTool::Redact, L"Redact…" },
            { PageTool::Annotate, L"Annotate…" },
            { PageTool::Adjust, L"Light and colour…" },
        };
        return tools;
    }

    std::wstring NewEditFolder()
    {
        const auto root = Root();
        if (root.empty())
            return {};
        static std::atomic<unsigned> serial{ 0 };
        std::error_code ec;
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            const auto folder = root / std::format(L"{}-{}-{}", GetCurrentProcessId(), GetTickCount64(), ++serial);
            std::filesystem::create_directories(root, ec);
            if (std::filesystem::create_directory(folder, ec))
                return folder.wstring();
        }
        log::Warn(L"pages: no folder for edited pages under {}", root.wstring());
        return {};
    }

    void RemoveEditFolder(const std::wstring& folder)
    {
        if (folder.empty())
            return;
        // Only ever a folder this file made.
        const auto root = Root();
        if (root.empty() || std::filesystem::path(folder).parent_path() != root)
            return;
        std::error_code ec;
        std::filesystem::remove_all(folder, ec);
    }

    void RemoveOldEditFolders()
    {
        const auto root = Root();
        std::error_code ec;
        if (root.empty() || !std::filesystem::is_directory(root, ec))
            return;
        const auto now = std::filesystem::file_time_type::clock::now();
        for (const auto& entry : std::filesystem::directory_iterator(root, ec))
        {
            std::error_code e;
            const auto when = entry.last_write_time(e);
            if (!e && entry.is_directory(e) && now - when > std::chrono::hours(24))
                std::filesystem::remove_all(entry.path(), e);
        }
    }

    std::unique_ptr<canvas::ICanvasTool> MakePageTool(PageTool tool, const std::vector<std::wstring>& pictures)
    {
        switch (tool)
        {
        case PageTool::Redact:
            return std::make_unique<redact::RedactTool>(pictures, redact::RedactTool::Mode::Redact);
        case PageTool::Annotate:
            return photo::MakeAnnotateTool(pictures);
        case PageTool::Crop:
            return std::make_unique<crop::CropTool>(pictures.size());
        case PageTool::Adjust:
            return photo::MakeEditTool(pictures, nlohmann::json::object());
        }
        return nullptr;
    }

    bool RenderPageEdit(PageTool tool, const nlohmann::json& params, const std::wstring& picture, const std::wstring& out, bool& changed,
                        std::wstring& error)
    {
        changed = false;
        if (tool == PageTool::Redact)
        {
            if (redact::CoversFor(params, picture).empty())
                return true;
            std::wstring extension;
            size_t covered = 0;
            if (FAILED(redact::WriteRedacted(picture, params, out, extension, covered, {}, error)))
                return false;
            changed = covered > 0;
            return true;
        }

        const nlohmann::json own = photo::ParamsForFile(params, picture);
        photo::Annotations marks;
        photo::Adjustments adjust;
        crop::CropParams cropping;
        if (tool == PageTool::Annotate)
        {
            marks = photo::Annotations::FromJson(own);
            if (marks.Items().empty())
                return true;
        }
        else if (tool == PageTool::Crop)
        {
            cropping = crop::ParseParams(params);
            if (cropping.box.w <= 0 || cropping.box.h <= 0)
                return true;
        }
        else
        {
            adjust = photo::ParseAdjustments(own);
            if (adjust.IsIdentity())
                return true;
        }
        // Each tool's own render, the one its engine exports with.
        canvas::Raster image, result;
        if (FAILED(canvas::LoadRaster(picture, image, error)))
            return false;
        HRESULT hr = S_OK;
        if (tool == PageTool::Annotate)
            hr = photo::RenderAnnotated(image, marks, photo::LoadInsertedPictures(marks), result, error);
        else if (tool == PageTool::Crop)
        {
            const auto plan = crop::PlanCrop(cropping, static_cast<int>(image.width), static_cast<int>(image.height));
            if (plan.box.w <= 0 || plan.box.h <= 0 || !crop::RenderCrop(image, plan, result))
                hr = E_OUTOFMEMORY;
        }
        else
            hr = photo::RenderAdjusted(image, adjust, result, error);
        if (FAILED(hr))
        {
            if (error.empty())
                error = L"the page could not be drawn";
            return false;
        }
        result.dpiX = image.dpiX;
        result.dpiY = image.dpiY;
        const auto& png = canvas::EditedFormatFor(L".png");
        if (FAILED(canvas::SaveRasterToFile(result, png, canvas::EditedQuality(png), out, error)))
            return false;
        changed = true;
        return true;
    }

    bool WritePicturePage(const std::wstring& picture, double pointsPerPixel, const std::wstring& out, std::wstring& error)
    {
        ImageSource image;
        if (!LoadImageSource(picture, image, error))
            return false;
        if (pointsPerPixel <= 0 || image.DisplayWidth() == 0 || image.DisplayHeight() == 0)
        {
            error = L"the edited page has no size";
            return false;
        }
        image.dpiX = image.dpiY = 72.0 / pointsPerPixel;   // the resolution that gives the page its size
        ImagePdfBuilder pdf(out);
        if (!pdf.Ok())
        {
            error = L"cannot write the edited page";
            return false;
        }
        return pdf.AddPage(image, nullptr, error) && pdf.Finish(error);
    }

    std::wstring PictureOf(const std::wstring& editedPage)
    {
        return std::filesystem::path(editedPage).replace_extension(L".png").wstring();
    }
}
