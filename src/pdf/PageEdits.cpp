#include "pdf/PageEdits.h"
#include "Brand.h"
#include "FileIo.h"
#include "Log.h"
#include "Recipe.h"
#include "TempPath.h"
#include "canvas/Raster.h"
#include "crop/CropMath.h"
#include "crop/CropTool.h"
#include "photo/Adjust.h"
#include "photo/PhotoEngine.h"
#include "photo/PhotoTools.h"
#include "pdf/ImagePdf.h"
#include "pdf/PageMarks.h"
#include "redact/RedactEngine.h"
#include "redact/RedactTool.h"
#include "Utf.h"

#include <windows.h>

#include <algorithm>
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
            const auto temp = TempDirectory();
            return temp.empty() ? std::filesystem::path() : std::filesystem::path(temp) / (SD_NAME L" pages");
        }

        // Beside a page edited with marks: the picture without them, and the marks.
        std::wstring StemOf(const std::wstring& path)
        {
            std::wstring stem = std::filesystem::path(path).replace_extension().wstring();
            if (stem.ends_with(L".base"))
                stem.resize(stem.size() - 5);
            return stem;
        }
        std::wstring MarksFileOf(const std::wstring& path) { return StemOf(path) + L".marks.json"; }

        // The marks an earlier edit left beside its picture: where the unmarked page came from, and
        // the marks. False when the picture has none.
        bool ReadMarksFile(const std::wstring& picture, std::wstring& source, uint32_t& page, photo::Annotations& marks)
        {
            std::string text;
            std::error_code ec;
            const auto path = MarksFileOf(picture);
            if (!std::filesystem::path(picture).stem().wstring().ends_with(L".base") || !std::filesystem::exists(path, ec) || !ReadFileBytes(path, text))
                return false;
            const auto j = nlohmann::json::parse(text, nullptr, false);
            if (!j.is_object() || !j.contains("source") || !j["source"].is_string())
                return false;
            source = Widen(j["source"].get<std::string>());
            page = j.value("page", 0u);
            marks = photo::Annotations::FromJson(j.value("marks", nlohmann::json::object()));
            return true;
        }

        bool UsesMarks(PageTool tool) { return tool == PageTool::Annotate || tool == PageTool::Sign; }
    }

    bool KeepsPage(PageTool tool) { return UsesMarks(tool); }

    const std::vector<PageToolInfo>& PageTools()
    {
        static const std::vector<PageToolInfo> tools = {
            { PageTool::Redact, L"Redact…" },
            { PageTool::Annotate, L"Annotate…" },
            { PageTool::Sign, L"Fill and sign…" },
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

    std::wstring EditFileStem(const std::wstring& folder, bool result, unsigned serial, size_t n)
    {
        return (std::filesystem::path(folder) / std::format(L"{} {}-{}", result ? L"edit" : L"page", serial, n)).wstring();
    }

    unsigned FreeEditSerial(const std::wstring& folder, unsigned after)
    {
        unsigned used = after;
        std::error_code ec;
        if (!folder.empty())
            for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
            {
                const std::wstring name = entry.path().filename().wstring();
                if (!name.starts_with(L"page ") && !name.starts_with(L"edit "))
                    continue;
                unsigned serial = 0;
                size_t i = 5;
                for (; i < name.size() && i < 14 && name[i] >= L'0' && name[i] <= L'9'; ++i)
                    serial = serial * 10 + static_cast<unsigned>(name[i] - L'0');
                if (i > 5 && i < name.size() && name[i] == L'-')
                    used = std::max(used, serial);
            }
        return used + 1;
    }

    std::unique_ptr<canvas::ICanvasTool> MakePageTool(PageTool tool, const std::vector<std::wstring>& pictures)
    {
        switch (tool)
        {
        case PageTool::Redact:
            return std::make_unique<redact::RedactTool>(pictures, redact::RedactTool::Mode::Redact);
        case PageTool::Annotate:
        case PageTool::Sign:
        {
            auto annotate = std::make_unique<photo::AnnotateTool>(pictures, photo::AnnotateOptions{ true, tool == PageTool::Sign });
            // A page edited with marks before opens with them, to change or add to.
            for (size_t i = 0; i < pictures.size(); ++i)
            {
                std::wstring source;
                uint32_t page = 0;
                photo::Annotations marks;
                if (ReadMarksFile(pictures[i], source, page, marks))
                    annotate->Seed(i, std::move(marks));
            }
            return annotate;
        }
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

        const nlohmann::json own = ParamsForFile(params, picture);
        photo::Annotations marks;
        photo::Adjustments adjust;
        crop::CropParams cropping;
        if (UsesMarks(tool))
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
        if (UsesMarks(tool))
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

    std::wstring PictureOf(const std::wstring& editedPage, PageTool tool)
    {
        const std::wstring stem = std::filesystem::path(editedPage).replace_extension().wstring();
        std::error_code ec;
        if (UsesMarks(tool) && std::filesystem::exists(stem + L".marks.json", ec) && std::filesystem::exists(stem + L".base.png", ec))
            return stem + L".base.png";
        return PictureOf(editedPage);
    }

    bool SavePageEdit(PageTool tool, const nlohmann::json& params, const std::wstring& picture, const std::wstring& source, uint32_t page,
                      double pointsPerPixel, const std::wstring& stem, bool& changed, std::wstring& error)
    {
        changed = false;
        const std::wstring png = stem + L".png", pdf = stem + L".pdf";
        const auto asPicture = [&] {
            // The whole page as a picture: the tools that change pixels, and the fallback.
            return RenderPageEdit(tool, params, picture, png, changed, error) && (!changed || WritePicturePage(png, pointsPerPixel, pdf, error));
        };
        if (!UsesMarks(tool))
            return asPicture();

        // Marks over the page as it was: an earlier edit's unmarked page when this picture is one, or
        // the earlier edit's own page (a redacted page stays redacted).
        std::wstring base = source;
        uint32_t basePage = page;
        photo::Annotations before;
        const bool seeded = ReadMarksFile(picture, base, basePage, before);
        std::error_code exists;
        const std::wstring earlier = std::filesystem::path(picture).replace_extension(L".pdf").wstring();
        if (!seeded && std::filesystem::exists(earlier, exists))
        {
            base = earlier;
            basePage = 0;
        }
        photo::Annotations marks = photo::Annotations::FromJson(ParamsForFile(params, picture));
        if (seeded ? marks.ToJson() == before.ToJson() : marks.Items().empty())
            return true;   // left as it was

        auto image = std::make_shared<canvas::Raster>();
        if (FAILED(canvas::LoadRaster(picture, *image, error)))
            return false;
        const auto images = photo::LoadInsertedPictures(marks);
        PageOverlay overlay;
        size_t pictures = 0;
        std::wstring why;
        MarkedPageInfo info;
        if (FAILED(photo::MarksToPdf(photo::Picture::Of(image), marks, images, overlay, &pictures, why)) || !WriteMarkedPage(base, basePage, overlay, pdf, why, &info))
        {
            log::Warn(L"pages: the marks could not be written over the page ({}); saving a picture of it", why);
            std::error_code ec;
            std::filesystem::remove(pdf, ec);
            return asPicture();
        }
        log::Info(L"pages: marks written over the page: {} text runs in {} fonts, {} images ({} drawn as pictures), {} shapes", info.textRuns, info.fonts,
                  info.images, pictures, info.paths);

        // For editing it again: the page without the marks, the marks, and the pictures they place.
        std::error_code ec;
        std::filesystem::copy_file(picture, stem + L".base.png", std::filesystem::copy_options::overwrite_existing, ec);
        auto json = marks.ToJson();
        size_t copied = 0;
        std::map<std::wstring, std::wstring> copies;
        for (auto& item : json["items"])
        {
            if (!item.contains("image") || !item["image"].is_string())
                continue;
            const std::wstring from = Widen(item["image"].get<std::string>());
            auto& copy = copies[from];
            if (copy.empty())
            {
                copy = std::format(L"{}.picture {}{}", stem, ++copied, std::filesystem::path(from).extension().wstring());
                if (!std::filesystem::copy_file(from, copy, std::filesystem::copy_options::overwrite_existing, ec))
                    copy = from;
            }
            item["image"] = Narrow(copy);
        }
        const nlohmann::json sidecar = { { "source", Narrow(base) }, { "page", basePage }, { "marks", std::move(json) } };
        if (!WriteFileBytes(MarksFileOf(png), sidecar.dump(), WriteMode::Any))
            log::Warn(L"pages: the marks could not be kept for editing again");

        // The picture with the marks, for the tools that change pixels.
        bool drawn = false;
        std::wstring drawError;
        if (marks.Items().empty() || !RenderPageEdit(tool, params, picture, png, drawn, drawError) || !drawn)
            std::filesystem::copy_file(picture, png, std::filesystem::copy_options::overwrite_existing, ec);
        changed = true;
        return true;
    }
}
