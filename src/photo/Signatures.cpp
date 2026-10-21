#include "photo/Signatures.h"
#include "Log.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>

namespace sd::photo
{
    namespace fs = std::filesystem;

    namespace
    {
        bool IsSignatureFile(const fs::path& p)
        {
            return p.extension() == L".png" && p.filename().wstring().rfind(L"signature-", 0) == 0;
        }

        bool IsDraftFile(const fs::path& p) { return p.extension() == L".png" && p.filename().wstring().rfind(L"draft-", 0) == 0; }
    }

    SignatureStore SignatureStore::Default() { return SignatureStore((fs::path(log::AppDataFolder()) / L"signatures").wstring()); }

    std::vector<std::wstring> SignatureStore::List() const
    {
        std::vector<std::wstring> out;
        std::error_code ec;
        if (m_folder.empty() || !fs::is_directory(m_folder, ec))
            return out;
        const auto now = fs::file_time_type::clock::now();
        for (const auto& entry : fs::directory_iterator(m_folder, ec))
        {
            std::error_code e;
            if (!entry.is_regular_file(e))
                continue;
            if (IsSignatureFile(entry.path()))
                out.push_back(entry.path().wstring());
            else if (IsDraftFile(entry.path()) && now - entry.last_write_time(e) > std::chrono::hours(24) && !e)
                fs::remove(entry.path(), e);
        }
        // The names carry the time they were made: newest first.
        std::sort(out.begin(), out.end(), std::greater<>());
        return out;
    }

    std::wstring SignatureStore::Add(const canvas::Raster& ink, std::wstring& error, bool draft) const
    {
        if (ink.Empty())
        {
            error = L"the signature is empty";
            return {};
        }
        std::error_code ec;
        fs::create_directories(m_folder, ec);
        SYSTEMTIME t{};
        GetLocalTime(&t);
        const std::wstring stem = std::format(L"{}-{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}", draft ? L"draft" : L"signature", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                                              t.wMilliseconds);
        fs::path path = fs::path(m_folder) / (stem + L".png");
        for (int n = 2; fs::exists(path, ec) && n < 100; ++n)
            path = fs::path(m_folder) / std::format(L"{}-{}.png", stem, n);
        const auto& png = canvas::EditedFormatFor(L".png");
        if (FAILED(canvas::SaveRasterToFile(ink, png, canvas::EditedQuality(png), path.wstring(), error)))
        {
            std::filesystem::remove(path, ec);
            return {};
        }
        if (draft)
            return path.wstring();
        // Only the newest few are kept.
        const auto all = List();
        for (size_t i = kMax; i < all.size(); ++i)
            Remove(all[i]);
        log::Info(L"signatures: saved a new signature ({}x{})", ink.width, ink.height);
        return path.wstring();
    }

    bool SignatureStore::Holds(const std::wstring& path) const
    {
        if (m_folder.empty() || path.empty())
            return false;
        std::error_code ec;
        const fs::path p = fs::weakly_canonical(fs::path(path), ec);
        const fs::path folder = fs::weakly_canonical(fs::path(m_folder), ec);
        return !ec && p.parent_path() == folder && IsSignatureFile(p);
    }

    bool SignatureStore::Remove(const std::wstring& path) const
    {
        if (!Holds(path))
            return false;
        std::error_code ec;
        const bool removed = fs::remove(path, ec);
        if (removed)
            log::Info(L"signatures: removed a saved signature");
        return removed;
    }

    void PaperToTransparent(canvas::Raster& r)
    {
        if (r.Empty())
            return;
        // The paper's lightness: the 90th percentile of each pixel's darkest channel, so ink in any
        // colour (blue, black) counts as dark and a page in shadow still counts as paper.
        std::array<size_t, 256> histogram{};
        const size_t count = static_cast<size_t>(r.width) * r.height;
        for (size_t i = 0; i < count; ++i)
        {
            const uint8_t* p = &r.px[i * 4];
            ++histogram[std::min({ p[0], p[1], p[2] })];
        }
        size_t seen = 0;
        int paper = 255;
        for (int v = 0; v < 256; ++v)
        {
            seen += histogram[static_cast<size_t>(v)];
            if (seen * 10 >= count * 9)
            {
                paper = v;
                break;
            }
        }
        paper = std::max(paper, 96);
        const double clear = paper - 24.0, solid = paper - 96.0;   // lighter than `clear`: paper; darker than `solid`: ink
        for (size_t i = 0; i < count; ++i)
        {
            uint8_t* p = &r.px[i * 4];
            const double m = std::min({ p[0], p[1], p[2] });
            const double a = std::clamp((clear - m) / (clear - solid), 0.0, 1.0) * (p[3] / 255.0);
            if (a <= 0)
            {
                p[0] = p[1] = p[2] = p[3] = 0;
                continue;
            }
            // The ink's own colour: the pixel with the paper's share taken out.
            for (int c = 0; c < 3; ++c)
                p[c] = static_cast<uint8_t>(std::clamp((p[c] - (1 - a) * paper) / a, 0.0, 255.0));
            p[3] = static_cast<uint8_t>(std::lround(a * 255));
        }
    }

    bool TrimToInk(canvas::Raster& r, unsigned margin)
    {
        if (r.Empty())
            return false;
        unsigned left = r.width, top = r.height, right = 0, bottom = 0;
        for (unsigned y = 0; y < r.height; ++y)
            for (unsigned x = 0; x < r.width; ++x)
                if (r.At(x, y)[3] > 16)
                {
                    left = std::min(left, x), right = std::max(right, x);
                    top = std::min(top, y), bottom = std::max(bottom, y);
                }
        if (left > right || top > bottom)
            return false;
        const unsigned x0 = left > margin ? left - margin : 0, y0 = top > margin ? top - margin : 0;
        const unsigned x1 = std::min(r.width, right + 1 + margin), y1 = std::min(r.height, bottom + 1 + margin);
        canvas::Raster out;
        if (!out.Allocate(x1 - x0, y1 - y0))
            return false;
        out.dpiX = r.dpiX;
        out.dpiY = r.dpiY;
        for (unsigned y = y0; y < y1; ++y)
            std::copy_n(r.At(x0, y), static_cast<size_t>(x1 - x0) * 4, out.At(0, y - y0));
        r = std::move(out);
        return true;
    }
}
