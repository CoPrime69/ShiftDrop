// Page rendering with Windows.Data.Pdf, the PDF renderer that ships with Windows. Installs itself
// into pdf::Renderer() before main().
#include "pdf/PageRenderer.h"
#include "Log.h"
#include "WinRtError.h"

#include <unknwn.h>
#include <winrt/base.h>
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

namespace sd::pdf
{
    namespace
    {
        namespace wdp = winrt::Windows::Data::Pdf;
        namespace imaging = winrt::Windows::Graphics::Imaging;
        namespace storage = winrt::Windows::Storage;

        std::wstring Message(const winrt::hresult_error& e)
        {
            const HRESULT code = e.code();
            if (code == HRESULT_FROM_WIN32(ERROR_WRONG_PASSWORD) || code == HRESULT_FROM_WIN32(ERROR_INVALID_PASSWORD))
                return L"the PDF is locked with a password";
            return WinRtMessage(e, L"Windows' PDF renderer failed");
        }

        class RenderedPdf final : public IRenderedPdf
        {
        public:
            explicit RenderedPdf(wdp::PdfDocument doc) : m_doc(std::move(doc)) {}

            uint32_t PageCount() const override { return m_doc.PageCount(); }

            bool PageSize(uint32_t page, double& widthPt, double& heightPt) const override
            {
                try
                {
                    // Size is the crop box with /Rotate applied, in DIPs (1/96 inch).
                    const auto size = m_doc.GetPage(page).Size();
                    widthPt = size.Width * 72.0 / 96.0;
                    heightPt = size.Height * 72.0 / 96.0;
                    return widthPt > 0 && heightPt > 0;
                }
                catch (const winrt::hresult_error& e)
                {
                    log::Warn(L"pdf render: page {} size: {}", page + 1, Message(e));
                    return false;
                }
            }

            bool Render(uint32_t page, uint32_t widthPx, uint32_t heightPx, Bitmap& out, std::wstring& error) const override
            {
                try
                {
                    auto pdfPage = m_doc.GetPage(page);
                    storage::Streams::InMemoryRandomAccessStream stream;
                    wdp::PdfPageRenderOptions options;
                    options.DestinationWidth(widthPx);
                    options.DestinationHeight(heightPx);
                    options.BitmapEncoderId(imaging::BitmapEncoder::BmpEncoderId());   // no PNG compression to undo
                    pdfPage.RenderToStreamAsync(stream, options).get();
                    stream.Seek(0);
                    const auto decoder = imaging::BitmapDecoder::CreateAsync(imaging::BitmapDecoder::BmpDecoderId(), stream).get();
                    const auto pixels = decoder.GetPixelDataAsync(imaging::BitmapPixelFormat::Bgra8, imaging::BitmapAlphaMode::Straight,
                                                                  imaging::BitmapTransform(), imaging::ExifOrientationMode::IgnoreExifOrientation,
                                                                  imaging::ColorManagementMode::DoNotColorManage)
                                            .get();
                    const auto data = pixels.DetachPixelData();
                    out.width = decoder.PixelWidth();
                    out.height = decoder.PixelHeight();
                    if (data.size() != static_cast<size_t>(out.width) * out.height * 4 || out.width == 0 || out.height == 0)
                    {
                        error = L"Windows' PDF renderer returned no picture";
                        return false;
                    }
                    out.bgra.assign(data.begin(), data.end());
                    // The page is painted on white; the BMP's alpha byte carries nothing.
                    for (size_t i = 3; i < out.bgra.size(); i += 4)
                        out.bgra[i] = 255;
                    return true;
                }
                catch (const winrt::hresult_error& e)
                {
                    error = Message(e);
                    return false;
                }
            }

        private:
            wdp::PdfDocument m_doc{ nullptr };
        };

        std::unique_ptr<IRenderedPdf> Open(const std::wstring& path, std::wstring& error)
        {
            try
            {
                const auto file = storage::StorageFile::GetFileFromPathAsync(path).get();
                auto doc = wdp::PdfDocument::LoadFromFileAsync(file).get();
                if (doc.IsPasswordProtected())
                {
                    error = L"the PDF is locked with a password";
                    return nullptr;
                }
                return std::make_unique<RenderedPdf>(std::move(doc));
            }
            catch (const winrt::hresult_error& e)
            {
                error = Message(e);
                return nullptr;
            }
        }

        const bool g_installed = [] {
            Renderer().open = &Open;
            return true;
        }();
    }
}
