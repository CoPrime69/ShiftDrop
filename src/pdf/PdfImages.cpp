#include "pdf/PdfImages.h"
#include "pdf/Wic.h"

#include <qpdf/Buffer.hh>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <vector>

namespace sd::pdf
{
    namespace
    {
        constexpr uint64_t kMaxPixels = 50'000'000;   // a 50 MP picture is about 200 MB unpacked

        enum class Model { Gray, Rgb, Cmyk };

        int Channels(Model m) { return m == Model::Gray ? 1 : m == Model::Rgb ? 3 : 4; }

        struct Space
        {
            Model model = Model::Gray;
            bool indexed = false;
            int hival = 0;
            std::vector<uint8_t> palette;   // indexed: hival + 1 entries, gray or RGB (CMYK converted)

            int Components() const { return indexed ? 1 : Channels(model); }
            int OutChannels() const { return model == Model::Gray ? 1 : 3; }
        };

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

        bool IsDct(const std::string& f) { return f == "/DCTDecode" || f == "/DCT"; }

        bool Lossless(const std::string& f)
        {
            return f == "/FlateDecode" || f == "/Fl" || f == "/LZWDecode" || f == "/LZW" || f == "/RunLengthDecode" || f == "/RL" ||
                   f == "/ASCII85Decode" || f == "/A85" || f == "/ASCIIHexDecode" || f == "/AHx";
        }

        // Device, calibrated and ICC-based spaces (an ICC profile is read as the device space with
        // its number of components).
        bool PlainSpace(QPDFObjectHandle cs, Model& model)
        {
            std::string name;
            if (cs.isName())
                name = cs.getName();
            else if (cs.isArray() && cs.getArrayNItems() >= 1 && cs.getArrayItem(0).isName())
            {
                name = cs.getArrayItem(0).getName();
                if (name == "/ICCBased")
                {
                    auto n = cs.getArrayNItems() >= 2 && cs.getArrayItem(1).isStream() ? cs.getArrayItem(1).getDict().getKey("/N")
                                                                                         : QPDFObjectHandle::newNull();
                    const long long count = n.isInteger() ? n.getIntValue() : 0;
                    if (count != 1 && count != 3 && count != 4)
                        return false;
                    model = count == 1 ? Model::Gray : count == 3 ? Model::Rgb : Model::Cmyk;
                    return true;
                }
            }
            if (name == "/DeviceGray" || name == "/G" || name == "/CalGray")
                model = Model::Gray;
            else if (name == "/DeviceRGB" || name == "/RGB" || name == "/CalRGB")
                model = Model::Rgb;
            else if (name == "/DeviceCMYK" || name == "/CMYK")
                model = Model::Cmyk;
            else
                return false;
            return true;
        }

        // CMYK to RGB the simple way (no profile): good enough to recognise a picture.
        void CmykToRgb(const uint8_t* cmyk, uint8_t* rgb)
        {
            const unsigned k = 255u - cmyk[3];
            for (int i = 0; i < 3; ++i)
                rgb[i] = static_cast<uint8_t>((255u - cmyk[i]) * k / 255u);
        }

        bool ReadSpace(QPDFObjectHandle cs, Space& out, std::string& why)
        {
            if (PlainSpace(cs, out.model))
                return true;
            if (cs.isArray() && cs.getArrayNItems() == 4 && cs.getArrayItem(0).isName() &&
                (cs.getArrayItem(0).getName() == "/Indexed" || cs.getArrayItem(0).getName() == "/I"))
            {
                Model base = Model::Gray;
                const auto hival = cs.getArrayItem(2);
                if (!PlainSpace(cs.getArrayItem(1), base) || !hival.isInteger() || hival.getIntValue() < 0 || hival.getIntValue() > 255)
                {
                    why = "indexed colour over a colour space this can't read";
                    return false;
                }
                std::string lookup;
                auto table = cs.getArrayItem(3);
                if (table.isString())
                    lookup = table.getStringValue();
                else if (table.isStream())
                {
                    const auto data = table.getStreamData(qpdf_dl_specialized);
                    lookup.assign(reinterpret_cast<const char*>(data->getBuffer()), data->getSize());
                }
                const int entries = static_cast<int>(hival.getIntValue()) + 1, n = Channels(base);
                if (lookup.size() < static_cast<size_t>(entries) * n)
                {
                    why = "the colour table is short";
                    return false;
                }
                out.indexed = true;
                out.hival = entries - 1;
                out.model = base == Model::Gray ? Model::Gray : Model::Rgb;
                for (int e = 0; e < entries; ++e)
                {
                    const auto* p = reinterpret_cast<const uint8_t*>(lookup.data()) + static_cast<size_t>(e) * n;
                    if (base == Model::Cmyk)
                    {
                        uint8_t rgb[3];
                        CmykToRgb(p, rgb);
                        out.palette.insert(out.palette.end(), rgb, rgb + 3);
                    }
                    else
                        out.palette.insert(out.palette.end(), p, p + n);
                }
                return true;
            }
            why = "a colour space this can't read (Lab, separation or DeviceN)";
            return false;
        }

        // The /Decode ranges for each component; the PDF defaults when there is none.
        void DecodeRanges(QPDFObjectHandle decode, const Space& space, int bpc, std::array<double, 8>& range)
        {
            const int n = space.Components();
            for (int c = 0; c < n; ++c)
            {
                range[2 * c] = 0;
                range[2 * c + 1] = space.indexed ? static_cast<double>((1u << std::min(bpc, 8)) - 1) : 1.0;
            }
            if (decode.isArray() && decode.getArrayNItems() >= 2 * n)
                for (int i = 0; i < 2 * n; ++i)
                    if (decode.getArrayItem(i).isNumber())
                        range[i] = decode.getArrayItem(i).getNumericValue();
        }

        // Unpacked stream data (1, 2, 4, 8 or 16 bits per component) to 8-bit gray or RGB.
        bool Unpack(const uint8_t* data, size_t size, uint32_t w, uint32_t h, int bpc, const Space& space, QPDFObjectHandle decode, Samples& out,
                    std::string& why)
        {
            const int n = space.Components();
            const size_t rowBytes = (static_cast<size_t>(w) * n * bpc + 7) / 8;
            if (size < rowBytes * h)
            {
                why = "the picture's data is short";
                return false;
            }
            // 16-bit samples are read by their high byte, so every sample is 0..max with max <= 255.
            const int bits = std::min(bpc, 8);
            const unsigned max = (1u << bits) - 1;
            std::array<double, 8> range{};
            DecodeRanges(decode, space, bits, range);
            // One table per component from the stored value to the output byte (or palette index).
            std::vector<std::array<uint8_t, 256>> lut(static_cast<size_t>(n));
            for (int c = 0; c < n; ++c)
                for (unsigned v = 0; v <= max; ++v)
                {
                    const double t = range[2 * c] + v * (range[2 * c + 1] - range[2 * c]) / max;
                    lut[c][v] = space.indexed ? static_cast<uint8_t>(std::clamp<long>(std::lround(t), 0, space.hival))
                                              : static_cast<uint8_t>(std::lround(std::clamp(t, 0.0, 1.0) * 255.0));
                }
            out.width = w;
            out.height = h;
            out.channels = static_cast<uint32_t>(space.OutChannels());
            out.data.resize(static_cast<size_t>(w) * h * out.channels);
            uint8_t* dst = out.data.data();
            uint8_t px[4] = {};
            for (uint32_t y = 0; y < h; ++y)
            {
                const uint8_t* row = data + rowBytes * y;
                for (uint32_t x = 0; x < w; ++x)
                {
                    for (int c = 0; c < n; ++c)
                    {
                        const size_t i = static_cast<size_t>(x) * n + c;
                        unsigned v = 0;
                        if (bpc == 8)
                            v = row[i];
                        else if (bpc == 16)
                            v = row[2 * i];
                        else
                        {
                            const size_t bit = i * bpc;
                            v = (row[bit / 8] >> (8 - bpc - bit % 8)) & max;
                        }
                        px[c] = lut[c][v];
                    }
                    if (space.indexed)
                    {
                        const int k = space.OutChannels();
                        std::copy_n(space.palette.data() + static_cast<size_t>(px[0]) * k, k, dst);
                        dst += k;
                    }
                    else if (space.model == Model::Cmyk)
                    {
                        CmykToRgb(px, dst);
                        dst += 3;
                    }
                    else
                    {
                        std::copy_n(px, n, dst);
                        dst += n;
                    }
                }
            }
            return true;
        }

        // Any image this file can read, as 8-bit gray or RGB samples. `jpeg` gets the stored JPEG when
        // the picture is one that can be passed on untouched.
        bool Decode(QPDFObjectHandle image, Samples& out, std::string* jpeg, std::string& why)
        {
            auto dict = image.getDict();
            if (dict.getKey("/ImageMask").isBool() && dict.getKey("/ImageMask").getBoolValue())
            {
                why = "a stencil mask";
                return false;
            }
            const auto width = dict.getKey("/Width"), height = dict.getKey("/Height");
            if (!width.isInteger() || !height.isInteger() || width.getIntValue() <= 0 || height.getIntValue() <= 0 ||
                width.getIntValue() > 30000 || height.getIntValue() > 30000)
            {
                why = "no usable size";
                return false;
            }
            const auto w = static_cast<uint32_t>(width.getIntValue()), h = static_cast<uint32_t>(height.getIntValue());
            if (static_cast<uint64_t>(w) * h > kMaxPixels)
            {
                why = "over 50 megapixels";
                return false;
            }
            Space space;
            if (!ReadSpace(dict.getKey("/ColorSpace"), space, why))
                return false;
            const auto bpcKey = dict.getKey("/BitsPerComponent");
            const int bpc = bpcKey.isInteger() ? static_cast<int>(bpcKey.getIntValue()) : 8;
            if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16)
            {
                why = "an unusual sample size";
                return false;
            }
            const auto filters = Filters(dict.getKey("/Filter"));
            const auto decode = dict.getKey("/Decode");
            if (filters.size() == 1 && IsDct(filters[0]))
            {
                if (space.indexed || space.model == Model::Cmyk)
                {
                    why = "a CMYK JPEG";
                    return false;
                }
                const auto raw = image.getRawStreamData();
                std::string stored(reinterpret_cast<const char*>(raw->getBuffer()), raw->getSize());
                if (jpeg && !decode.isArray())
                {
                    *jpeg = std::move(stored);
                    out.width = w;
                    out.height = h;
                    return true;
                }
                Samples s;
                if (FAILED(DecodeJpeg(stored, s)) || s.width != w || s.height != h || static_cast<int>(s.channels) != space.OutChannels())
                {
                    why = "the JPEG does not decode";
                    return false;
                }
                if (decode.isArray())
                {
                    std::array<double, 8> range{};
                    DecodeRanges(decode, space, 8, range);
                    for (size_t i = 0; i < s.data.size(); ++i)
                    {
                        const size_t c = i % s.channels;
                        const double t = range[2 * c] + s.data[i] / 255.0 * (range[2 * c + 1] - range[2 * c]);
                        s.data[i] = static_cast<uint8_t>(std::lround(std::clamp(t, 0.0, 1.0) * 255.0));
                    }
                }
                out = std::move(s);
                return true;
            }
            if (!std::all_of(filters.begin(), filters.end(), Lossless))
            {
                why = "JPEG 2000, JBIG2 or fax compression";
                return false;
            }
            const auto data = image.getStreamData(qpdf_dl_specialized);
            return Unpack(data->getBuffer(), data->getSize(), w, h, bpc, space, decode, out, why);
        }
    }

    bool ImageToFile(QPDFObjectHandle image, ImageFile& out, std::string& why)
    {
        out = ImageFile{};
        try
        {
            if (!image.isStream())
            {
                why = "not an image";
                return false;
            }
            auto smask = image.getDict().getKey("/SMask");
            Samples samples;
            std::string jpeg;
            if (!Decode(image, samples, smask.isStream() ? nullptr : &jpeg, why))
                return false;
            out.width = samples.width;
            out.height = samples.height;
            if (!jpeg.empty())
            {
                out.bytes = std::move(jpeg);
                return true;
            }
            // A soft mask becomes the PNG's alpha. One that can't be read leaves the picture opaque.
            Samples alpha;
            std::string ignored;
            if (smask.isStream() && Decode(smask, alpha, nullptr, ignored) && alpha.channels == 1 &&
                ((alpha.width == samples.width && alpha.height == samples.height) || SUCCEEDED(ResizeSamples(alpha, samples.width, samples.height))))
            {
                Samples rgba;
                rgba.width = samples.width;
                rgba.height = samples.height;
                rgba.channels = 4;
                rgba.data.resize(static_cast<size_t>(rgba.width) * rgba.height * 4);
                const size_t pixels = static_cast<size_t>(rgba.width) * rgba.height;
                for (size_t i = 0; i < pixels; ++i)
                {
                    for (int c = 0; c < 3; ++c)
                        rgba.data[i * 4 + c] = samples.channels == 1 ? samples.data[i] : samples.data[i * 3 + c];
                    rgba.data[i * 4 + 3] = alpha.data[i];
                }
                samples = std::move(rgba);
            }
            if (FAILED(EncodePng(samples, out.bytes)) || out.bytes.empty())
            {
                why = "the PNG encoder failed";
                return false;
            }
            return true;
        }
        catch (const std::exception& e)
        {
            why = e.what();
            return false;
        }
    }
}
