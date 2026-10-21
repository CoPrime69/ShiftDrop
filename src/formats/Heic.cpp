#include "formats/Heic.h"
#include "ErrorText.h"
#include "ImagePipeline.h"
#include "Wic.h"

#include <shlwapi.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace sd::formats
{
    namespace
    {
        // GUID_ContainerFormatHeif (Windows SDK 10.0.17763+), spelled out so older headers build too.
        constexpr GUID kContainerHeif = { 0xe1e62521, 0x6787, 0x405b, { 0xa3, 0x39, 0x50, 0x07, 0x15, 0xb5, 0x76, 0x3f } };

        std::atomic<int> g_support{ -1 };    // HeicSupport once known, -1 before the first question
        std::atomic<int> g_override{ -1 };   // tests

        HRESULT EncodeHeif(IWICImagingFactory* factory, IWICBitmapSource* pixels, int quality, IStream* out)
        {
            ComPtr<IWICBitmapEncoder> encoder;
            RETURN_IF_FAILED(factory->CreateEncoder(kContainerHeif, nullptr, &encoder));
            RETURN_IF_FAILED(encoder->Initialize(out, WICBitmapEncoderNoCache));
            ComPtr<IWICBitmapFrameEncode> frame;
            ComPtr<IPropertyBag2> options;
            RETURN_IF_FAILED(encoder->CreateNewFrame(&frame, &options));
            if (options)
            {
                PROPBAG2 name{};
                name.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
                VARIANT value;
                VariantInit(&value);
                value.vt = VT_R4;
                value.fltVal = static_cast<float>(std::clamp(quality, 1, 100)) / 100.f;
                options->Write(1, &name, &value);   // optional: a codec without the option keeps its default
            }
            RETURN_IF_FAILED(frame->Initialize(options.Get()));
            UINT w = 0, h = 0;
            RETURN_IF_FAILED(pixels->GetSize(&w, &h));
            RETURN_IF_FAILED(frame->SetSize(w, h));
            // HEIC pictures are opaque; the codec picks its own layout and we convert to it.
            WICPixelFormatGUID chosen = GUID_WICPixelFormat24bppBGR;
            RETURN_IF_FAILED(frame->SetPixelFormat(&chosen));
            ComPtr<IWICBitmapSource> source = pixels;
            WICPixelFormatGUID have{};
            RETURN_IF_FAILED(pixels->GetPixelFormat(&have));
            if (have != chosen)
            {
                ComPtr<IWICFormatConverter> converter;
                RETURN_IF_FAILED(factory->CreateFormatConverter(&converter));
                RETURN_IF_FAILED(converter->Initialize(pixels, chosen, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));
                source = converter;
            }
            RETURN_IF_FAILED(frame->WriteSource(source.Get(), nullptr));
            RETURN_IF_FAILED(frame->Commit());
            return encoder->Commit();
        }

        // A real 64 x 64 encode: the HEIF codec registers its encoder even when the HEVC encoder it
        // needs is missing, so only an encode that finishes proves HEIC can be written.
        HeicSupport Probe()
        {
            auto com = wil::CoInitializeEx(COINIT_MULTITHREADED);
            auto factory = wic::Factory();
            ComPtr<IWICBitmapEncoder> encoder;
            if (!factory || FAILED(factory->CreateEncoder(kContainerHeif, nullptr, &encoder)))
                return HeicSupport::NoHeif;
            std::vector<BYTE> grey(64 * 64 * 3, 128);
            ComPtr<IWICBitmap> bitmap;
            ComPtr<IStream> memory;
            memory.Attach(SHCreateMemStream(nullptr, 0));
            if (!memory || FAILED(factory->CreateBitmapFromMemory(64, 64, GUID_WICPixelFormat24bppBGR, 64 * 3, static_cast<UINT>(grey.size()),
                                                                  grey.data(), &bitmap)))
                return HeicSupport::NoHevc;
            if (FAILED(EncodeHeif(factory.Get(), bitmap.Get(), 80, memory.Get())))
                return HeicSupport::NoHevc;
            STATSTG stat{};
            return SUCCEEDED(memory->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart > 0 ? HeicSupport::Ready : HeicSupport::NoHevc;
        }

        void StartProbe()
        {
            static std::once_flag once;
            static std::jthread worker;   // joined at exit; the probe takes well under a second
            std::call_once(once, [] {
                // Quick answer first: with no HEIF encoder registered at all there is nothing to wait for.
                auto factory = wic::Factory();
                ComPtr<IWICBitmapEncoder> encoder;
                if (!factory || FAILED(factory->CreateEncoder(kContainerHeif, nullptr, &encoder)))
                {
                    g_support = static_cast<int>(HeicSupport::NoHeif);
                    return;
                }
                g_support = static_cast<int>(HeicSupport::Checking);
                worker = std::jthread([] { g_support = static_cast<int>(Probe()); });
            });
        }

        bool IsHeicExtension(const std::wstring& ext) { return ext == L".heic" || ext == L".heif" || ext == L".hif"; }

        class Hook final : public IImageFormatHook
        {
        public:
            const char* EncodedFormat() const override { return "heic"; }

            bool CanEncode(std::wstring* why) const override
            {
                const auto support = HeicEncoderSupport();
                if (support == HeicSupport::Ready)
                    return true;
                if (why)
                    *why = HeicMissingReason(support);
                return false;
            }

            HRESULT Encode(IWICImagingFactory* factory, IWICBitmapSource* pixels, int quality, IStream* out, std::wstring& error) const override
            {
                const HRESULT hr = EncodeHeif(factory, pixels, quality, out);
                if (FAILED(hr))
                    error = HeicMissingReason(HeicSupport::NoHevc) + L" (" + HResultText(hr) + L")";
                return hr;
            }

            bool CodecHint(const std::wstring& ext, std::wstring* label, std::wstring* why) const override
            {
                if (!IsHeicExtension(ext))
                    return false;
                if (label)
                    *label = L"Needs HEIF Image Extensions";
                if (why)
                    *why = L"install “HEIF Image Extensions” and “HEVC Video Extensions” from the Microsoft Store to open HEIC photos";
                return true;
            }
        };
    }

    HeicSupport HeicEncoderSupport()
    {
        if (const int forced = g_override.load(); forced >= 0)
            return static_cast<HeicSupport>(forced);
        StartProbe();
        return static_cast<HeicSupport>(g_support.load());
    }

    std::wstring HeicMissingReason(HeicSupport support)
    {
        switch (support)
        {
        case HeicSupport::Ready: return {};
        case HeicSupport::Checking: return L"still checking Windows' HEIF codec; try again in a moment";
        case HeicSupport::NoHeif:
            return L"install “HEIF Image Extensions” and “HEVC Video Extensions” from the Microsoft Store to save HEIC";
        case HeicSupport::NoHevc: return L"install “HEVC Video Extensions” from the Microsoft Store to save HEIC";
        }
        return {};
    }

    void OverrideHeicSupportForTests(std::optional<HeicSupport> support) { g_override = support ? static_cast<int>(*support) : -1; }

    const IImageFormatHook& HeicHook()
    {
        static const Hook hook;
        return hook;
    }
}
