#include "formats/Svg.h"
#include "FileIo.h"
#include "Utf.h"

#include <lunasvg/lunasvg.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace sd::formats
{
    namespace
    {
        // lunasvg draws text only with fonts it has been given, and ours (vcpkg's default) does not
        // scan the fonts folder. The usual families and the CSS generic ones' fallbacks are added
        // the first time an SVG with text is drawn: a few files read once, not hundreds parsed.
        void AddSystemFonts()
        {
            static std::once_flag once;
            std::call_once(once, [] {
                wchar_t windows[MAX_PATH] = {};
                if (!GetWindowsDirectoryW(windows, MAX_PATH))
                    return;
                const std::string fonts = Narrow(windows) + "\\Fonts\\";
                struct Face
                {
                    const char* family;
                    const char* regular;
                    const char* bold;
                    const char* italic;
                    const char* boldItalic;
                };
                static const Face faces[] = {
                    { "Arial", "arial.ttf", "arialbd.ttf", "ariali.ttf", "arialbi.ttf" },          // sans-serif
                    { "Times New Roman", "times.ttf", "timesbd.ttf", "timesi.ttf", "timesbi.ttf" }, // serif
                    { "Courier New", "cour.ttf", "courbd.ttf", "couri.ttf", "courbi.ttf" },         // monospace
                    { "Segoe UI", "segoeui.ttf", "segoeuib.ttf", "segoeuii.ttf", "segoeuiz.ttf" },
                    { "Verdana", "verdana.ttf", "verdanab.ttf", "verdanai.ttf", "verdanaz.ttf" },
                    { "Georgia", "georgia.ttf", "georgiab.ttf", "georgiai.ttf", "georgiaz.ttf" },
                    { "Calibri", "calibri.ttf", "calibrib.ttf", "calibrii.ttf", "calibriz.ttf" },
                    { "Comic Sans MS", "comic.ttf", "comicbd.ttf", "comici.ttf", "comicz.ttf" },      // cursive
                    { "Impact", "impact.ttf", nullptr, nullptr, nullptr },                            // fantasy
                };
                for (const auto& f : faces)
                {
                    const char* files[] = { f.regular, f.bold, f.italic, f.boldItalic };
                    for (int i = 0; i < 4; ++i)
                        if (files[i])
                            lunasvg_add_font_face_from_file(f.family, i == 1 || i == 3, i >= 2, (fonts + files[i]).c_str());
                }
            });
        }

        bool HasText(const char* data, size_t size) { return std::string_view(data, size).find("<text") != std::string_view::npos; }

        // .svgz is a gzipped SVG.
        bool Gunzip(const std::vector<uint8_t>& in, std::string& out)
        {
            z_stream z{};
            if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK)
                return false;
            z.next_in = const_cast<Bytef*>(in.data());
            z.avail_in = static_cast<uInt>(in.size());
            int r = Z_OK;
            char chunk[64 * 1024];
            while (r == Z_OK)
            {
                z.next_out = reinterpret_cast<Bytef*>(chunk);
                z.avail_out = sizeof(chunk);
                r = inflate(&z, Z_NO_FLUSH);
                out.append(chunk, sizeof(chunk) - z.avail_out);
                if (out.size() > (256u << 20))   // a 256 MB SVG is a zip bomb, not a drawing
                    r = Z_MEM_ERROR;
            }
            inflateEnd(&z);
            return r == Z_STREAM_END;
        }

        HRESULT ReadSvgText(const std::wstring& path, std::string& text)
        {
            std::vector<uint8_t> bytes;
            if (!ReadFileBytes(path, bytes, nullptr, kMaxImageFileBytes))
                return HRESULT_FROM_WIN32(GetLastError());
            if (bytes.size() >= 2 && bytes[0] == 0x1f && bytes[1] == 0x8b)
                return Gunzip(bytes, text) ? S_OK : WINCODEC_ERR_BADIMAGE;
            text.assign(bytes.begin(), bytes.end());
            return S_OK;
        }

        struct Planned
        {
            std::unique_ptr<lunasvg::Document> doc;
            SvgSize size;
            lunasvg::Matrix matrix;   // document units to output pixels
        };

        bool Plan(const char* data, size_t size, Planned& plan)
        {
            plan.doc = lunasvg::Document::loadFromData(data, size);
            if (!plan.doc)
                return false;
            const auto root = plan.doc->documentElement();
            const bool own = (root.hasAttribute("width") && IsAbsoluteLength(root.getAttribute("width"))) ||
                             (root.hasAttribute("height") && IsAbsoluteLength(root.getAttribute("height")));
            const float iw = plan.doc->width(), ih = plan.doc->height();
            const auto box = plan.doc->boundingBox();
            plan.size = SvgRasterSize(own, iw, ih, box.w, box.h);
            if (plan.size.width == 0)
                return false;
            if (iw > 0 && ih > 0)
                plan.matrix = lunasvg::Matrix(plan.size.width / iw, 0, 0, plan.size.height / ih, 0, 0);
            else if (box.w > 0 && box.h > 0)   // no size and no viewBox: fit the drawing itself
            {
                const float k = std::min(plan.size.width / box.w, plan.size.height / box.h);
                plan.matrix = lunasvg::Matrix(k, 0, 0, k, -box.x * k, -box.y * k);
            }
            return true;
        }

        class Hook final : public IImageFormatHook
        {
        public:
            std::vector<std::wstring> DecodedExtensions() const override { return { L".svg", L".svgz" }; }

            HRESULT Decode(IWICImagingFactory* factory, const std::wstring& path, std::stop_token stop,
                           Microsoft::WRL::ComPtr<IWICBitmapSource>& out, std::wstring& error) const override
            {
                std::string text;
                if (const HRESULT hr = ReadSvgText(path, text); FAILED(hr))
                    return hr;
                if (stop.stop_requested())
                    return HRESULT_FROM_WIN32(ERROR_CANCELLED);
                Image image;
                if (const HRESULT hr = RenderSvg(text.data(), text.size(), image, error); FAILED(hr))
                    return hr;
                return ToWicBitmap(factory, image, out);
            }

            bool ReadSize(const std::wstring& path, uint32_t& width, uint32_t& height) const override
            {
                std::string text;
                return SUCCEEDED(ReadSvgText(path, text)) && ReadSvgSize(text.data(), text.size(), width, height);
            }
        };
    }

    bool IsAbsoluteLength(std::string_view value)
    {
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
            value.remove_suffix(1);
        if (value.empty() || value.back() == '%' || value == "auto")
            return false;
        // A number first ("48", "10.5cm", ".5in"); a unit, if any, is lunasvg's to resolve.
        return (value.front() >= '0' && value.front() <= '9') || (value.front() == '.' && value.size() > 1);
    }

    SvgSize SvgRasterSize(bool ownSize, float iw, float ih, float bw, float bh)
    {
        const auto fit = [](double w, double h, double longSide) {
            const double k = longSide / std::max(w, h);
            return SvgSize{ static_cast<uint32_t>(std::max(1.0, std::round(w * k))), static_cast<uint32_t>(std::max(1.0, std::round(h * k))) };
        };
        if (ownSize && iw > 0 && ih > 0)
        {
            if (std::max(iw, ih) > static_cast<float>(kSvgMaxSide))
                return fit(iw, ih, kSvgMaxSide);
            return SvgSize{ static_cast<uint32_t>(std::max(1.0f, std::ceil(iw - 0.01f))), static_cast<uint32_t>(std::max(1.0f, std::ceil(ih - 0.01f))) };
        }
        if (iw > 0 && ih > 0)
            return fit(iw, ih, kSvgDefaultLongSide);
        if (bw > 0 && bh > 0)
            return fit(bw, bh, kSvgDefaultLongSide);
        return SvgSize{ kSvgDefaultLongSide, kSvgDefaultLongSide };
    }

    HRESULT RenderSvg(const char* data, size_t size, Image& out, std::wstring& error)
    {
        if (HasText(data, size))
            AddSystemFonts();
        Planned plan;
        if (!Plan(data, size, plan))
        {
            error = L"not a readable SVG";
            return WINCODEC_ERR_BADIMAGE;
        }
        lunasvg::Bitmap bitmap(static_cast<int>(plan.size.width), static_cast<int>(plan.size.height));
        if (bitmap.isNull())
            return E_OUTOFMEMORY;
        bitmap.clear(0x00000000);   // transparent where nothing is drawn
        plan.doc->render(bitmap, plan.matrix);
        // lunasvg draws premultiplied ARGB32, which in memory is premultiplied BGRA.
        out.width = plan.size.width;
        out.height = plan.size.height;
        out.bgra.resize(static_cast<size_t>(out.width) * out.height * 4);
        const auto* src = bitmap.data();
        const size_t row = static_cast<size_t>(out.width) * 4;
        for (uint32_t y = 0; y < out.height; ++y)
            std::memcpy(out.Row(y), src + static_cast<size_t>(y) * bitmap.stride(), row);
        Unpremultiply(out.bgra);
        return S_OK;
    }

    bool ReadSvgSize(const char* data, size_t size, uint32_t& width, uint32_t& height)
    {
        Planned plan;
        if (!Plan(data, size, plan))
            return false;
        width = plan.size.width;
        height = plan.size.height;
        return true;
    }

    const IImageFormatHook& SvgHook()
    {
        static const Hook hook;
        return hook;
    }
}
