#include "pdf/PdfCompress.h"
#include "Log.h"
#include "pdf/PdfDoc.h"
#include "pdf/Wic.h"

#include <qpdf/QPDFPageDocumentHelper.hh>

#include <algorithm>
#include <cmath>
#include <set>

namespace sd::pdf
{
    namespace
    {
        // Channels of a colour space JPEG can carry as is: 1 (gray) or 3 (RGB); 0 for anything else.
        int Channels(QPDFObjectHandle space)
        {
            if (space.isName())
                return space.getName() == "/DeviceGray" ? 1 : space.getName() == "/DeviceRGB" ? 3 : 0;
            if (space.isArray() && space.getArrayNItems() == 2 && space.getArrayItem(0).isName() &&
                space.getArrayItem(0).getName() == "/ICCBased" && space.getArrayItem(1).isStream())
            {
                auto n = space.getArrayItem(1).getDict().getKey("/N");
                const int channels = n.isInteger() ? static_cast<int>(n.getIntValue()) : 0;
                return channels == 1 || channels == 3 ? channels : 0;
            }
            return 0;
        }

        std::vector<std::string> Filters(QPDFObjectHandle filter)
        {
            std::vector<std::string> out;
            if (filter.isName())
                out.push_back(filter.getName());
            else if (filter.isArray())
                for (int i = 0; i < filter.getArrayNItems(); ++i)
                    if (filter.getArrayItem(i).isName())
                        out.push_back(filter.getArrayItem(i).getName());
            return out;
        }

        bool Lossless(const std::string& f)
        {
            return f == "/FlateDecode" || f == "/Fl" || f == "/LZWDecode" || f == "/LZW" || f == "/RunLengthDecode" || f == "/RL" ||
                   f == "/ASCII85Decode" || f == "/A85" || f == "/ASCIIHexDecode" || f == "/AHx";
        }

        // Re-encodes one image if that makes it meaningfully smaller. Returns true when replaced.
        bool Recompress(QPDFObjectHandle image, const Placement* placement, const CompressOptions& options)
        {
            auto dict = image.getDict();
            if (dict.getKey("/ImageMask").isBool() && dict.getKey("/ImageMask").getBoolValue())
                return false;   // a 1-bit stencil
            if (dict.getKey("/Mask").isArray() || dict.hasKey("/Decode"))
                return false;   // colour-key masking and decode arrays don't survive lossy re-encoding
            if (!dict.getKey("/BitsPerComponent").isInteger() || dict.getKey("/BitsPerComponent").getIntValue() != 8)
                return false;
            const auto width = dict.getKey("/Width"), height = dict.getKey("/Height");
            if (!width.isInteger() || !height.isInteger() || width.getIntValue() <= 0 || height.getIntValue() <= 0 ||
                width.getIntValue() > 30000 || height.getIntValue() > 30000)
                return false;
            const int channels = Channels(dict.getKey("/ColorSpace"));
            if (!channels)
                return false;
            const auto w = static_cast<uint32_t>(width.getIntValue()), h = static_cast<uint32_t>(height.getIntValue());
            if (static_cast<uint64_t>(w) * h < 64 * 64)
                return false;   // icons and bullets: nothing to win

            const auto filters = Filters(dict.getKey("/Filter"));
            const auto length = dict.getKey("/Length");
            const uint64_t before = length.isInteger() ? static_cast<uint64_t>(length.getIntValue()) : image.getRawStreamData()->getSize();

            Samples samples;
            if (filters.size() == 1 && (filters[0] == "/DCTDecode" || filters[0] == "/DCT"))
            {
                const auto raw = image.getRawStreamData();
                if (FAILED(DecodeJpeg(std::string(reinterpret_cast<const char*>(raw->getBuffer()), raw->getSize()), samples)) ||
                    static_cast<int>(samples.channels) != channels || samples.width != w || samples.height != h)
                    return false;
            }
            else
            {
                if (!std::all_of(filters.begin(), filters.end(), Lossless))
                    return false;   // JPX, JBIG2, CCITT: leave them be
                const auto data = image.getStreamData(qpdf_dl_generalized);
                if (data->getSize() != static_cast<size_t>(w) * h * channels)
                    return false;
                samples.width = w;
                samples.height = h;
                samples.channels = static_cast<uint32_t>(channels);
                samples.data.assign(data->getBuffer(), data->getBuffer() + data->getSize());
            }

            uint32_t tw = w, th = h;
            TargetSize(w, h, placement, options.dpi, tw, th);
            if ((tw != w || th != h) && FAILED(ResizeSamples(samples, tw, th)))
                return false;
            std::string jpeg;
            if (FAILED(EncodeJpeg(samples, options.quality, jpeg)) || jpeg.empty())
                return false;
            if (static_cast<double>(jpeg.size()) > 0.9 * static_cast<double>(before))
                return false;   // not worth a generation of JPEG loss

            image.replaceStreamData(jpeg, QPDFObjectHandle::newName("/DCTDecode"), QPDFObjectHandle::newNull());
            dict.replaceKey("/Width", QPDFObjectHandle::newInteger(samples.width));
            dict.replaceKey("/Height", QPDFObjectHandle::newInteger(samples.height));
            return true;
        }
    }

    void TargetSize(uint32_t w, uint32_t h, const Placement* placement, int dpi, uint32_t& outW, uint32_t& outH)
    {
        outW = w;
        outH = h;
        if (!placement || placement->width <= 0 || placement->height <= 0 || dpi <= 0 || w == 0 || h == 0)
            return;
        // The unit square is drawn placement->width x height points; 72 points per inch.
        const double wantW = placement->width / 72.0 * dpi, wantH = placement->height / 72.0 * dpi;
        const double scale = std::max(wantW / w, wantH / h);
        if (scale >= 0.8)
            return;
        outW = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(w * scale)));
        outH = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(h * scale)));
    }

    bool CompressPdf(const std::wstring& source, const std::wstring& out, const CompressOptions& options, std::stop_token stop,
                     const ProgressFn& progress, CompressStats& stats, std::wstring& error)
    {
        auto pdf = OpenPdf(source, error);
        if (!pdf)
            return false;
        try
        {
            std::map<QPDFObjGen, Placement> placements;
            for (auto& page : QPDFPageDocumentHelper(*pdf).getAllPages())
            {
                CollectImagePlacements(page, placements);
                try
                {
                    page.removeUnreferencedResources();   // fonts and images a page lists but never uses
                }
                catch (const std::exception&)
                {
                }
            }
            if (progress)
                progress(0.05);

            std::vector<QPDFObjectHandle> images;
            for (auto& object : pdf->getAllObjects())
                if (object.isStream() && object.getDict().getKey("/Subtype").isName() && object.getDict().getKey("/Subtype").getName() == "/Image")
                    images.push_back(object);
            // Soft masks are images too, but JPEG noise in an alpha channel shows as halos: keep them.
            std::set<QPDFObjGen> masks;
            for (auto& image : images)
                if (auto mask = image.getDict().getKey("/SMask"); mask.isStream() && mask.isIndirect())
                    masks.insert(mask.getObjGen());
            std::erase_if(images, [&](QPDFObjectHandle& image) { return masks.count(image.getObjGen()) > 0; });
            stats.images = static_cast<int>(images.size());
            for (size_t i = 0; i < images.size(); ++i)
            {
                if (stop.stop_requested())
                {
                    error = kCancelled;
                    return false;
                }
                const auto it = placements.find(images[i].getObjGen());
                try
                {
                    if (Recompress(images[i], it == placements.end() ? nullptr : &it->second, options))
                        ++stats.recompressed;
                }
                catch (const std::exception& e)
                {
                    log::Warn(L"pdf compress: image {} left as it was: {}", images[i].getObjGen().getObj(), QpdfError(e));
                }
                if (progress)
                    progress(0.05 + 0.65 * static_cast<double>(i + 1) / static_cast<double>(images.size()));
            }
            return WritePdf(*pdf, out, WriteOptions{ true }, error, stop, [&](double f) {
                if (progress)
                    progress(0.7 + 0.3 * f);
            });
        }
        catch (const std::exception& e)
        {
            error = QpdfError(e);
            return false;
        }
    }
}
