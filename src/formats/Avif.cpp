#include "formats/Avif.h"
#include "FileIo.h"
#include "Utf.h"
#include "Wic.h"

#include <avif/avif.h>
#include <avif/avif_cxx.h>

#include <algorithm>
#include <format>

namespace sd::formats
{
    namespace
    {
        std::wstring ResultText(avifResult result, const avifDiagnostics* diag = nullptr)
        {
            std::wstring text = Widen(avifResultToString(result));
            if (diag && diag->error[0])
                text += L": " + Widen(diag->error);
            return text;
        }

        // Applies the container's turn and mirror, in the order HEIF/MIAF defines (rotate, then mirror).
        void MakeUpright(const avifImage& image, Image& pixels)
        {
            if (image.transformFlags & AVIF_TRANSFORM_IROT)
                RotateAntiClockwise(pixels, image.irot.angle);
            if (image.transformFlags & AVIF_TRANSFORM_IMIR)
                Mirror(pixels, image.imir.axis);
        }

        bool Turned(const avifImage& image)
        {
            return (image.transformFlags & AVIF_TRANSFORM_IROT) && (image.irot.angle % 2) == 1;
        }

        avif::DecoderPtr ParsedDecoder(const uint8_t* data, size_t size, int threads, avifResult& result)
        {
            avif::DecoderPtr decoder(avifDecoderCreate());
            if (!decoder)
            {
                result = AVIF_RESULT_OUT_OF_MEMORY;
                return nullptr;
            }
            decoder->maxThreads = threads > 0 ? threads : CodecThreads();
            decoder->ignoreExif = AVIF_TRUE;   // outputs don't carry metadata; skip reading it
            decoder->ignoreXMP = AVIF_TRUE;
            result = avifDecoderSetIOMemory(decoder.get(), data, size);
            if (result == AVIF_RESULT_OK)
                result = avifDecoderParse(decoder.get());
            return result == AVIF_RESULT_OK ? std::move(decoder) : nullptr;
        }

        class Hook final : public IImageFormatHook
        {
        public:
            std::vector<std::wstring> DecodedExtensions() const override { return { L".avif" }; }

            HRESULT Decode(IWICImagingFactory* factory, const std::wstring& path, std::stop_token stop,
                           Microsoft::WRL::ComPtr<IWICBitmapSource>& out, std::wstring& error) const override
            {
                std::vector<uint8_t> bytes;
                if (!ReadFileBytes(path, bytes, nullptr, kMaxImageFileBytes))
                    return HRESULT_FROM_WIN32(GetLastError());
                if (stop.stop_requested())
                    return HRESULT_FROM_WIN32(ERROR_CANCELLED);
                Image image;
                if (const HRESULT hr = DecodeAvif(bytes.data(), bytes.size(), image, error); FAILED(hr))
                    return hr;
                return ToWicBitmap(factory, image, out);
            }

            bool ReadSize(const std::wstring& path, uint32_t& width, uint32_t& height) const override
            {
                std::vector<uint8_t> bytes;
                return ReadFileBytes(path, bytes, nullptr, kMaxImageFileBytes) && ReadAvifSize(bytes.data(), bytes.size(), width, height);
            }

            const char* EncodedFormat() const override { return "avif"; }

            HRESULT Encode(IWICImagingFactory* factory, IWICBitmapSource* pixels, int quality, IStream* out, std::wstring& error) const override
            {
                Image image;
                if (const HRESULT hr = wic::CopyBgra(factory, pixels, image.width, image.height, image.bgra); FAILED(hr))
                    return hr;
                AvifOptions options;
                options.quality = std::clamp(quality, 1, 100);
                std::vector<uint8_t> encoded;
                if (const HRESULT hr = EncodeAvif(image, options, encoded, error); FAILED(hr))
                    return hr;
                ULONG written = 0;
                if (const HRESULT hr = out->Write(encoded.data(), static_cast<ULONG>(encoded.size()), &written); FAILED(hr))
                    return hr;
                return written == encoded.size() ? S_OK : HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
            }
        };
    }

    bool AvifFullChroma(int quality) { return quality >= 90; }

    namespace
    {
        // The picture as 8-bit sRGB YUV (full range), with an alpha plane when `alpha`.
        HRESULT ToYuv(const Image& source, int quality, bool alpha, avif::ImagePtr& image, std::wstring& error)
        {
            image.reset(avifImageCreate(source.width, source.height, 8,
                                        AvifFullChroma(quality) ? AVIF_PIXEL_FORMAT_YUV444 : AVIF_PIXEL_FORMAT_YUV420));
            if (!image)
                return E_OUTOFMEMORY;
            // sRGB, full range: what every browser and viewer assumes for an untagged 8-bit picture.
            image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
            image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
            image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT601;
            image->yuvRange = AVIF_RANGE_FULL;

            avifRGBImage rgb;
            avifRGBImageSetDefaults(&rgb, image.get());
            rgb.format = AVIF_RGB_FORMAT_BGRA;
            rgb.depth = 8;
            rgb.ignoreAlpha = alpha ? AVIF_FALSE : AVIF_TRUE;   // an opaque picture gets no alpha plane at all
            rgb.pixels = const_cast<uint8_t*>(source.bgra.data());
            rgb.rowBytes = source.width * 4;
            if (const avifResult r = avifImageRGBToYUV(image.get(), &rgb); r != AVIF_RESULT_OK)
            {
                error = L"AVIF colour conversion: " + ResultText(r);
                return E_FAIL;
            }
            return S_OK;
        }

        avif::EncoderPtr NewEncoder(const AvifOptions& options)
        {
            avif::EncoderPtr encoder(avifEncoderCreate());
            if (!encoder)
                return nullptr;
            encoder->codecChoice = AVIF_CODEC_CHOICE_AOM;
            encoder->maxThreads = options.threads > 0 ? options.threads : CodecThreads();
            encoder->speed = options.speed;
            encoder->quality = std::clamp(options.quality, 0, 100);
            // Edges of logos and cut-outs show alpha artefacts first, so alpha gets a higher floor.
            encoder->qualityAlpha = std::max(encoder->quality, 80);
            // Tiles let aom spread one picture over every core (and decoders too).
            encoder->autoTiling = AVIF_TRUE;
            return encoder;
        }

        void TakeOutput(avifRWData& output, std::vector<uint8_t>& out)
        {
            out.assign(output.data, output.data + output.size);
            avifRWDataFree(&output);
        }
    }

    HRESULT EncodeAvif(const Image& source, const AvifOptions& options, std::vector<uint8_t>& out, std::wstring& error)
    {
        if (source.Empty())
            return E_INVALIDARG;
        avif::ImagePtr image;
        if (const HRESULT hr = ToYuv(source, options.quality, !source.Opaque(), image, error); FAILED(hr))
            return hr;
        auto encoder = NewEncoder(options);
        if (!encoder)
            return E_OUTOFMEMORY;
        avifRWData output = AVIF_DATA_EMPTY;
        const avifResult r = avifEncoderWrite(encoder.get(), image.get(), &output);
        if (r != AVIF_RESULT_OK)
        {
            avifRWDataFree(&output);
            error = L"AVIF encode: " + ResultText(r, &encoder->diag);
            return E_FAIL;
        }
        TakeOutput(output, out);
        return S_OK;
    }

    struct AvifSequence::State
    {
        avif::EncoderPtr encoder;
        AvifOptions options;
        uint32_t width = 0, height = 0;
        uint32_t frames = 0;
    };

    AvifSequence::AvifSequence(const AvifOptions& options) : m_state(std::make_unique<State>())
    {
        m_state->options = options;
        m_state->encoder = NewEncoder(options);
        if (m_state->encoder)
            m_state->encoder->timescale = 1000;   // durations in milliseconds
    }

    AvifSequence::~AvifSequence() = default;

    HRESULT AvifSequence::Add(const Image& frame, int durationMs, std::wstring& error)
    {
        if (!m_state->encoder)
            return E_OUTOFMEMORY;
        if (frame.Empty() || (m_state->frames && (frame.width != m_state->width || frame.height != m_state->height)))
            return E_INVALIDARG;
        // Every frame of a sequence needs the same planes, so an animation always carries alpha
        // (an opaque one costs a few bytes per frame).
        avif::ImagePtr image;
        if (const HRESULT hr = ToYuv(frame, m_state->options.quality, true, image, error); FAILED(hr))
            return hr;
        const avifResult r = avifEncoderAddImage(m_state->encoder.get(), image.get(), static_cast<uint64_t>(std::max(1, durationMs)),
                                                 AVIF_ADD_IMAGE_FLAG_NONE);
        if (r != AVIF_RESULT_OK)
        {
            error = L"AVIF encode: " + ResultText(r, &m_state->encoder->diag);
            return E_FAIL;
        }
        m_state->width = frame.width;
        m_state->height = frame.height;
        ++m_state->frames;
        return S_OK;
    }

    HRESULT AvifSequence::Finish(std::vector<uint8_t>& out, std::wstring& error)
    {
        if (!m_state->encoder || m_state->frames == 0)
            return E_UNEXPECTED;
        avifRWData output = AVIF_DATA_EMPTY;
        const avifResult r = avifEncoderFinish(m_state->encoder.get(), &output);
        if (r != AVIF_RESULT_OK)
        {
            avifRWDataFree(&output);
            error = L"AVIF encode: " + ResultText(r, &m_state->encoder->diag);
            return E_FAIL;
        }
        TakeOutput(output, out);
        return S_OK;
    }

    HRESULT DecodeAvif(const uint8_t* data, size_t size, Image& out, std::wstring& error, int threads)
    {
        avifResult r = AVIF_RESULT_OK;
        auto decoder = ParsedDecoder(data, size, threads, r);
        if (!decoder)
        {
            error = L"not a readable AVIF: " + ResultText(r);
            return WINCODEC_ERR_BADIMAGE;
        }
        if (r = avifDecoderNextImage(decoder.get()); r != AVIF_RESULT_OK)
        {
            error = L"AVIF decode: " + ResultText(r, &decoder->diag);
            return WINCODEC_ERR_BADIMAGE;
        }
        const avifImage& image = *decoder->image;
        avifRGBImage rgb;
        avifRGBImageSetDefaults(&rgb, &image);
        rgb.format = AVIF_RGB_FORMAT_BGRA;
        rgb.depth = 8;   // 10- and 12-bit pictures are brought down to 8 bits
        rgb.maxThreads = decoder->maxThreads;
        out.width = image.width;
        out.height = image.height;
        out.bgra.resize(static_cast<size_t>(image.width) * image.height * 4);
        rgb.pixels = out.bgra.data();
        rgb.rowBytes = image.width * 4;
        if (r = avifImageYUVToRGB(&image, &rgb); r != AVIF_RESULT_OK)
        {
            error = L"AVIF colour conversion: " + ResultText(r);
            return E_FAIL;
        }
        MakeUpright(image, out);
        return S_OK;
    }

    bool ReadAvifSize(const uint8_t* data, size_t size, uint32_t& width, uint32_t& height)
    {
        avifResult r = AVIF_RESULT_OK;
        auto decoder = ParsedDecoder(data, size, 1, r);
        if (!decoder || decoder->image->width == 0)
            return false;
        const bool turned = Turned(*decoder->image);
        width = turned ? decoder->image->height : decoder->image->width;
        height = turned ? decoder->image->width : decoder->image->height;
        return true;
    }

    const IImageFormatHook& AvifHook()
    {
        static const Hook hook;
        return hook;
    }
}
