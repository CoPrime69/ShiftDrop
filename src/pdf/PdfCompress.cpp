#include "pdf/PdfCompress.h"
#include "Log.h"
#include "pdf/Parallel.h"
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

        // One picture on its way to a smaller JPEG. Reading it from and writing it back to the PDF is
        // QPDF's work and stays on one thread; the decode, scale and encode in between (the slow part)
        // run on several pictures at once.
        struct Job
        {
            QPDFObjectHandle image;
            std::string dct;        // the JPEG as stored, decoded by Encode
            Samples samples;        // or the pixels, already unpacked from a lossless filter
            int channels = 0;
            uint32_t w = 0, h = 0, tw = 0, th = 0;
            uint64_t before = 0;    // bytes stored now
            std::string jpeg;       // the new JPEG; empty when it should stay as it is
        };

        // Checks one image and takes what Encode needs out of the PDF. False when it is left alone.
        bool Prepare(QPDFObjectHandle image, const Placement* placement, const CompressOptions& options, Job& job)
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
            job.image = image;
            job.channels = channels;
            job.w = w;
            job.h = h;
            job.before = length.isInteger() ? static_cast<uint64_t>(length.getIntValue()) : image.getRawStreamData()->getSize();
            if (filters.size() == 1 && (filters[0] == "/DCTDecode" || filters[0] == "/DCT"))
            {
                const auto raw = image.getRawStreamData();
                job.dct.assign(reinterpret_cast<const char*>(raw->getBuffer()), raw->getSize());
            }
            else
            {
                if (!std::all_of(filters.begin(), filters.end(), Lossless))
                    return false;   // JPX, JBIG2, CCITT: leave them be
                const auto data = image.getStreamData(qpdf_dl_generalized);
                if (data->getSize() != static_cast<size_t>(w) * h * channels)
                    return false;
                job.samples.width = w;
                job.samples.height = h;
                job.samples.channels = static_cast<uint32_t>(channels);
                job.samples.data.assign(data->getBuffer(), data->getBuffer() + data->getSize());
            }
            TargetSize(w, h, placement, options.dpi, job.tw, job.th);
            return true;
        }

        // Decode, scale and encode: WIC only, so any number of these run at once. Leaves job.jpeg
        // empty when the picture should stay as it is.
        void Encode(Job& job, const CompressOptions& options)
        {
            if (!job.dct.empty() && (FAILED(DecodeJpeg(job.dct, job.samples)) || static_cast<int>(job.samples.channels) != job.channels ||
                                     job.samples.width != job.w || job.samples.height != job.h))
                return;
            job.dct = {};
            if ((job.tw != job.w || job.th != job.h) && FAILED(ResizeSamples(job.samples, job.tw, job.th)))
                return;
            std::string jpeg;
            if (FAILED(EncodeJpeg(job.samples, options.quality, jpeg)) || jpeg.empty())
                return;
            if (static_cast<double>(jpeg.size()) > 0.9 * static_cast<double>(job.before))
                return;   // not worth a generation of JPEG loss
            job.jpeg = std::move(jpeg);
            job.samples.data = {};
        }

        // Puts the new JPEG in the PDF. True when the image was replaced.
        bool Apply(Job& job)
        {
            if (job.jpeg.empty())
                return false;
            auto dict = job.image.getDict();
            job.image.replaceStreamData(job.jpeg, QPDFObjectHandle::newName("/DCTDecode"), QPDFObjectHandle::newNull());
            dict.replaceKey("/Width", QPDFObjectHandle::newInteger(job.samples.width));
            dict.replaceKey("/Height", QPDFObjectHandle::newInteger(job.samples.height));
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
            // A few pictures at a time (as many as there are threads to encode them, so only that
            // many are unpacked in memory at once): taken out, encoded side by side, put back.
            const size_t chunk = std::max<size_t>(1, JobQueue::PoolThreads());
            for (size_t first = 0; first < images.size(); first += chunk)
            {
                if (stop.stop_requested())
                {
                    error = kCancelled;
                    return false;
                }
                std::vector<Job> jobs;
                for (size_t i = first; i < std::min(images.size(), first + chunk); ++i)
                {
                    const auto it = placements.find(images[i].getObjGen());
                    try
                    {
                        if (Job job; Prepare(images[i], it == placements.end() ? nullptr : &it->second, options, job))
                            jobs.push_back(std::move(job));
                    }
                    catch (const std::exception& e)
                    {
                        log::Warn(L"pdf compress: image {} left as it was: {}", images[i].getObjGen().getObj(), QpdfError(e));
                    }
                }
                ForEachParallel(jobs.size(), [&](size_t i) { Encode(jobs[i], options); });
                for (auto& job : jobs)
                {
                    try
                    {
                        if (Apply(job))
                            ++stats.recompressed;
                    }
                    catch (const std::exception& e)
                    {
                        log::Warn(L"pdf compress: image {} left as it was: {}", job.image.getObjGen().getObj(), QpdfError(e));
                    }
                }
                if (progress)
                    progress(0.05 + 0.65 * static_cast<double>(std::min(images.size(), first + chunk)) / static_cast<double>(images.size()));
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
