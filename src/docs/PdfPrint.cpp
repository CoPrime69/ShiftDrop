#include "docs/PdfPrint.h"
#include "ErrorText.h"
#include "FileIo.h"
#include "Strings.h"
#include "TempPath.h"
#include "docs/Bytes.h"
#include "docs/Paper.h"
#include "Log.h"
#include "Utf.h"

#include <windows.h>
#include <dwrite.h>
#include <wincodec.h>
#include <winspool.h>
#include <wil/resource.h>
#include <wrl/client.h>

#include <chrono>
#include <format>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace sd::docs
{
    namespace
    {
        const wchar_t* kBodyFont = L"Segoe UI";
        const wchar_t* kMonoFont = L"Consolas";

        int FontKey(const FontStyle& f)
        {
            return static_cast<int>(f.size * 100) << 3 | (f.bold ? 1 : 0) | (f.italic ? 2 : 0) | (f.mono ? 4 : 0);
        }

        class DirectWriteMeasure final : public ITextMeasure
        {
        public:
            bool Init(std::wstring& error)
            {
                HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(m_factory.GetAddressOf()));
                if (SUCCEEDED(hr))
                    hr = m_factory->GetSystemFontCollection(&m_fonts);
                if (FAILED(hr))
                {
                    error = std::format(L"DirectWrite is unavailable ({})", HexCode(hr));
                    return false;
                }
                return true;
            }

            float Width(std::wstring_view text, const FontStyle& font) override
            {
                if (text.empty())
                    return 0;
                std::wstring key(text);
                key.push_back(static_cast<wchar_t>(0xE000 + (FontKey(font) & 0xFFF)));
                key += std::to_wstring(FontKey(font));
                if (const auto it = m_widths.find(key); it != m_widths.end())
                    return it->second;
                float width = 0;
                ComPtr<IDWriteTextLayout> layout;
                if (auto* format = Format(font);
                    format && SUCCEEDED(m_factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format, 1e6f, 1e6f, &layout)))
                {
                    DWRITE_TEXT_METRICS tm{};
                    if (SUCCEEDED(layout->GetMetrics(&tm)))
                        width = tm.widthIncludingTrailingWhitespace;
                }
                if (m_widths.size() > 200000)
                    m_widths.clear();
                m_widths.emplace(std::move(key), width);
                return width;
            }

            FontMetrics Metrics(const FontStyle& font) override
            {
                const int key = FontKey(font);
                if (const auto it = m_metrics.find(key); it != m_metrics.end())
                    return it->second;
                FontMetrics m{ font.size * 1.07f, font.size * 0.27f, 0 };   // Segoe UI's own, if all else fails
                UINT32 index = 0;
                BOOL exists = FALSE;
                ComPtr<IDWriteFontFamily> family;
                ComPtr<IDWriteFont> face;
                if (SUCCEEDED(m_fonts->FindFamilyName(font.mono ? kMonoFont : kBodyFont, &index, &exists)) && exists &&
                    SUCCEEDED(m_fonts->GetFontFamily(index, &family)) &&
                    SUCCEEDED(family->GetFirstMatchingFont(font.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                           font.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, &face)))
                {
                    DWRITE_FONT_METRICS fm{};
                    face->GetMetrics(&fm);
                    const float scale = font.size / fm.designUnitsPerEm;
                    m = FontMetrics{ fm.ascent * scale, fm.descent * scale, fm.lineGap * scale };
                }
                m_metrics.emplace(key, m);
                return m;
            }

        private:
            IDWriteTextFormat* Format(const FontStyle& font)
            {
                const int key = FontKey(font);
                if (const auto it = m_formats.find(key); it != m_formats.end())
                    return it->second.Get();
                ComPtr<IDWriteTextFormat> format;
                m_factory->CreateTextFormat(font.mono ? kMonoFont : kBodyFont, nullptr, font.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                            font.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, font.size,
                                            L"", &format);
                if (format)
                    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                return m_formats.emplace(key, format).first->second.Get();
            }

            ComPtr<IDWriteFactory> m_factory;
            ComPtr<IDWriteFontCollection> m_fonts;
            std::map<int, ComPtr<IDWriteTextFormat>> m_formats;
            std::map<int, FontMetrics> m_metrics;
            std::unordered_map<std::wstring, float> m_widths;
        };

        // 0xRRGGBB -> GDI's COLORREF (0x00BBGGRR).
        COLORREF Gdi(uint32_t c) { return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF); }

        // Decodes an image (any WIC format) to top-down 32-bit pixels flattened onto white, at most
        // `maxW` x `maxH` (the printed size at 200 DPI: sharp on paper, small in the PDF).
        bool DecodeForPrint(const std::string& data, UINT maxW, UINT maxH, UINT& w, UINT& h, std::vector<BYTE>& pixels)
        {
            ComPtr<IWICImagingFactory> wic;
            ComPtr<IWICStream> stream;
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            ComPtr<IWICFormatConverter> converter;
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
                FAILED(wic->CreateStream(&stream)) ||
                FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(data.data())), static_cast<DWORD>(data.size()))) ||
                FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
                FAILED(decoder->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&converter)) ||
                FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
                return false;
            UINT sw = 0, sh = 0;
            converter->GetSize(&sw, &sh);
            if (sw == 0 || sh == 0)
                return false;
            ComPtr<IWICBitmapSource> source = converter;
            w = sw;
            h = sh;
            if (sw > maxW || sh > maxH)
            {
                const double s = std::min(static_cast<double>(maxW) / sw, static_cast<double>(maxH) / sh);
                w = std::max(1u, static_cast<UINT>(sw * s));
                h = std::max(1u, static_cast<UINT>(sh * s));
                ComPtr<IWICBitmapScaler> scaler;
                if (FAILED(wic->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(converter.Get(), w, h, WICBitmapInterpolationModeFant)))
                    return false;
                source = scaler;
            }
            pixels.resize(static_cast<size_t>(w) * h * 4);
            if (FAILED(source->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data())))
                return false;
            // Premultiplied alpha over white: colour + (255 - alpha).
            for (size_t i = 0; i < pixels.size(); i += 4)
            {
                const BYTE inv = static_cast<BYTE>(255 - pixels[i + 3]);
                pixels[i] = static_cast<BYTE>(std::min(255, pixels[i] + inv));
                pixels[i + 1] = static_cast<BYTE>(std::min(255, pixels[i + 1] + inv));
                pixels[i + 2] = static_cast<BYTE>(std::min(255, pixels[i + 2] + inv));
                pixels[i + 3] = 255;
            }
            return true;
        }

        struct PrinterHandle
        {
            HANDLE h = nullptr;
            ~PrinterHandle()
            {
                if (h)
                    ClosePrinter(h);
            }
        };

        bool PdfComplete(const std::wstring& path)
        {
            std::string bytes;
            if (!ReadFileBytes(path, bytes) || bytes.size() < 64 || !bytes.starts_with("%PDF"))
                return false;
            return bytes.find("%%EOF", bytes.size() > 1024 ? bytes.size() - 1024 : 0) != std::string::npos;
        }
    }

    bool FindPdfPrinter(std::wstring* name, std::wstring* why)
    {
        static std::mutex lock;
        static ULONGLONG checked = 0;
        static std::wstring found;
        std::lock_guard guard(lock);
        const ULONGLONG now = GetTickCount64();
        if (checked == 0 || now - checked > 30000)
        {
            found.clear();
            DWORD needed = 0, count = 0;
            // Level 2 for the driver name; local printers only (no network round trips).
            EnumPrintersW(PRINTER_ENUM_LOCAL, nullptr, 2, nullptr, 0, &needed, &count);
            std::vector<BYTE> buffer(needed);
            if (needed && EnumPrintersW(PRINTER_ENUM_LOCAL, nullptr, 2, buffer.data(), needed, &needed, &count))
            {
                const auto* printers = reinterpret_cast<const PRINTER_INFO_2W*>(buffer.data());
                for (DWORD i = 0; i < count; ++i)
                {
                    const std::wstring driver = printers[i].pDriverName ? printers[i].pDriverName : L"";
                    const std::wstring pname = printers[i].pPrinterName ? printers[i].pPrinterName : L"";
                    if (EqualsNoCase(driver, L"Microsoft Print To PDF") || EqualsNoCase(pname, L"Microsoft Print to PDF"))
                    {
                        found = pname;
                        break;
                    }
                }
            }
            checked = now;
        }
        if (found.empty())
        {
            if (why)
                *why = L"PDF needs Windows' \"Microsoft Print to PDF\" printer: turn it on in Windows Features";
            return false;
        }
        if (name)
            *name = found;
        return true;
    }

    std::unique_ptr<ITextMeasure> MakeDirectWriteMeasure(std::wstring& error)
    {
        auto m = std::make_unique<DirectWriteMeasure>();
        if (!m->Init(error))
            return nullptr;
        return m;
    }

    bool PrintPagesToPdf(const std::vector<Page>& pages, const Document& doc, const PageSetup& page, bool letter, bool landscape,
                         const std::wstring& title, std::stop_token stop, const ProgressFn& progress, std::string& pdf, std::wstring& error)
    {
        std::wstring printer;
        if (!FindPdfPrinter(&printer, &error))
            return false;
        PrinterHandle handle;
        if (!OpenPrinterW(printer.data(), &handle.h, nullptr))
        {
            error = std::format(L"cannot open the PDF printer (error {})", GetLastError());
            return false;
        }
        // The printer's own settings with our paper size and orientation, so no dialog is needed.
        const LONG dmSize = DocumentPropertiesW(nullptr, handle.h, printer.data(), nullptr, nullptr, 0);
        std::vector<BYTE> dmBuffer(static_cast<size_t>(std::max<LONG>(dmSize, sizeof(DEVMODEW))));
        auto* dm = reinterpret_cast<DEVMODEW*>(dmBuffer.data());
        if (dmSize <= 0 || DocumentPropertiesW(nullptr, handle.h, printer.data(), dm, nullptr, DM_OUT_BUFFER) != IDOK)
        {
            error = L"cannot read the PDF printer's settings";
            return false;
        }
        dm->dmFields |= DM_PAPERSIZE | DM_ORIENTATION;
        dm->dmPaperSize = letter ? DMPAPER_LETTER : DMPAPER_A4;
        dm->dmOrientation = landscape ? DMORIENT_LANDSCAPE : DMORIENT_PORTRAIT;
        DocumentPropertiesW(nullptr, handle.h, printer.data(), dm, dm, DM_IN_BUFFER | DM_OUT_BUFFER);

        wil::unique_hdc dc(CreateDCW(L"WINSPOOL", printer.c_str(), nullptr, dm));
        if (!dc)
        {
            error = L"cannot start the PDF printer";
            return false;
        }

        // The printer writes a new file in %TEMP%; its bytes then go into our staging file.
        const auto temp = TempFilePath(L"print", L".pdf");
        auto removeTemp = wil::scope_exit([&] { DeleteFileW(temp.c_str()); });

        DOCINFOW info{};
        info.cbSize = sizeof(info);
        info.lpszDocName = title.c_str();
        info.lpszOutput = temp.c_str();
        const int job = StartDocW(dc.get(), &info);
        if (job <= 0)
        {
            error = std::format(L"the PDF printer refused the job (error {})", GetLastError());
            return false;
        }

        const double sx = GetDeviceCaps(dc.get(), LOGPIXELSX) / 72.0, sy = GetDeviceCaps(dc.get(), LOGPIXELSY) / 72.0;
        const int offX = GetDeviceCaps(dc.get(), PHYSICALOFFSETX), offY = GetDeviceCaps(dc.get(), PHYSICALOFFSETY);
        const auto X = [&](float x) { return static_cast<int>(std::lround(x * sx)) - offX; };
        const auto Y = [&](float y) { return static_cast<int>(std::lround(y * sy)) - offY; };
        (void)page;

        std::map<std::pair<int, bool>, wil::unique_hfont> fonts;
        const auto font = [&](const FontStyle& f, bool underline) -> HFONT {
            auto& slot = fonts[{ FontKey(f), underline }];
            if (!slot)
                slot.reset(CreateFontW(-static_cast<int>(std::lround(f.size * sy)), 0, 0, 0, f.bold ? FW_BOLD : FW_NORMAL, f.italic, underline, FALSE,
                                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                       f.mono ? kMonoFont : kBodyFont));
            return slot.get();
        };

        bool ok = true;
        for (size_t p = 0; p < pages.size() && ok; ++p)
        {
            if (stop.stop_requested())
            {
                AbortDoc(dc.get());
                error = L"cancelled";
                return false;
            }
            if (StartPage(dc.get()) <= 0)
            {
                ok = false;
                break;
            }
            SetBkMode(dc.get(), TRANSPARENT);
            SetTextAlign(dc.get(), TA_BASELINE | TA_LEFT | TA_NOUPDATECP);
            for (const auto& item : pages[p].items)
            {
                switch (item.kind)
                {
                case ItemKind::Text:
                    if (!item.text.empty())
                    {
                        SelectObject(dc.get(), font(item.font, item.underline));
                        SetTextColor(dc.get(), Gdi(item.color));
                        ExtTextOutW(dc.get(), X(item.x), Y(item.y), 0, nullptr, item.text.c_str(), static_cast<UINT>(item.text.size()), nullptr);
                    }
                    break;
                case ItemKind::Rect:
                {
                    RECT r{ X(item.x), Y(item.y), X(item.x + item.w), Y(item.y + item.h) };
                    if (item.fill != kNoColor)
                    {
                        wil::unique_hbrush brush(CreateSolidBrush(Gdi(item.fill)));
                        FillRect(dc.get(), &r, brush.get());
                    }
                    if (item.color != kNoColor)
                    {
                        wil::unique_hpen pen(CreatePen(PS_SOLID, std::max(1, static_cast<int>(item.lineWidth * sx)), Gdi(item.color)));
                        const auto oldPen = SelectObject(dc.get(), pen.get());
                        const auto oldBrush = SelectObject(dc.get(), GetStockObject(NULL_BRUSH));
                        Rectangle(dc.get(), r.left, r.top, r.right, r.bottom);
                        SelectObject(dc.get(), oldBrush);
                        SelectObject(dc.get(), oldPen);
                    }
                    break;
                }
                case ItemKind::Line:
                {
                    wil::unique_hpen pen(CreatePen(PS_SOLID, std::max(1, static_cast<int>(item.lineWidth * sx)), Gdi(item.color)));
                    const auto old = SelectObject(dc.get(), pen.get());
                    MoveToEx(dc.get(), X(item.x), Y(item.y), nullptr);
                    LineTo(dc.get(), X(item.x + item.w), Y(item.y + item.h));
                    SelectObject(dc.get(), old);
                    break;
                }
                case ItemKind::Image:
                {
                    if (item.image < 0 || static_cast<size_t>(item.image) >= doc.images.size())
                        break;
                    UINT w = 0, h = 0;
                    std::vector<BYTE> pixels;
                    const auto maxW = static_cast<UINT>(std::max(1.f, item.w / 72.f * 200)), maxH = static_cast<UINT>(std::max(1.f, item.h / 72.f * 200));
                    if (!DecodeForPrint(doc.images[static_cast<size_t>(item.image)].data, maxW, maxH, w, h, pixels))
                        break;
                    BITMAPINFO bmi{};
                    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
                    bmi.bmiHeader.biWidth = static_cast<LONG>(w);
                    bmi.bmiHeader.biHeight = -static_cast<LONG>(h);   // top-down
                    bmi.bmiHeader.biPlanes = 1;
                    bmi.bmiHeader.biBitCount = 32;
                    bmi.bmiHeader.biCompression = BI_RGB;
                    SetStretchBltMode(dc.get(), HALFTONE);
                    StretchDIBits(dc.get(), X(item.x), Y(item.y), X(item.x + item.w) - X(item.x), Y(item.y + item.h) - Y(item.y), 0, 0,
                                  static_cast<int>(w), static_cast<int>(h), pixels.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
                    break;
                }
                }
            }
            ok = EndPage(dc.get()) > 0;
            if (progress)
                progress(0.1 + 0.8 * static_cast<double>(p + 1) / static_cast<double>(pages.size()));
        }
        if (!ok)
        {
            AbortDoc(dc.get());
            error = L"the PDF printer stopped while printing";
            return false;
        }
        if (EndDoc(dc.get()) <= 0)
        {
            error = L"the PDF printer could not finish the job";
            return false;
        }
        dc.reset();

        // The spooler hands the job to the printer and writes the file afterwards: wait for it.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(3);
        while (true)
        {
            DWORD needed = 0;
            GetJobW(handle.h, static_cast<DWORD>(job), 1, nullptr, 0, &needed);
            const bool queued = needed > 0;
            if (queued)
            {
                std::vector<BYTE> jobInfo(needed);
                if (GetJobW(handle.h, static_cast<DWORD>(job), 1, jobInfo.data(), needed, &needed))
                {
                    const auto* ji = reinterpret_cast<const JOB_INFO_1W*>(jobInfo.data());
                    if (ji->Status & (JOB_STATUS_ERROR | JOB_STATUS_OFFLINE | JOB_STATUS_PAPEROUT | JOB_STATUS_USER_INTERVENTION))
                    {
                        SetJobW(handle.h, static_cast<DWORD>(job), 0, nullptr, JOB_CONTROL_DELETE);
                        error = L"the PDF printer reported an error";
                        return false;
                    }
                }
            }
            if (!queued && PdfComplete(temp))
                break;
            if (stop.stop_requested())
            {
                SetJobW(handle.h, static_cast<DWORD>(job), 0, nullptr, JOB_CONTROL_DELETE);
                error = L"cancelled";
                return false;
            }
            if (std::chrono::steady_clock::now() > deadline)
            {
                SetJobW(handle.h, static_cast<DWORD>(job), 0, nullptr, JOB_CONTROL_DELETE);
                error = L"the PDF printer did not finish in 3 minutes";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!ReadFileBytes(temp, pdf, &error))
            return false;
        if (progress)
            progress(1.0);
        return true;
    }

    bool DocumentToPdf(const Document& doc, const std::wstring& title, bool landscape, float bodySize, std::stop_token stop,
                       const ProgressFn& progress, std::string& pdf, std::wstring& error)
    {
        if (!FindPdfPrinter(nullptr, &error))
            return false;
        auto measure = MakeDirectWriteMeasure(error);
        if (!measure)
            return false;
        const auto paper = DefaultPaper();
        LayoutOptions o;
        o.bodySize = bodySize;
        o.page.width = landscape ? paper.heightPt : paper.widthPt;
        o.page.height = landscape ? paper.widthPt : paper.heightPt;
        const auto pages = LayoutDocument(doc, o, *measure);
        if (progress)
            progress(0.1);
        if (stop.stop_requested())
        {
            error = L"cancelled";
            return false;
        }
        log::Info(L"docs: printing {} page(s) to PDF", pages.size());
        return PrintPagesToPdf(pages, doc, o.page, paper.letter, landscape, title, stop, progress, pdf, error);
    }
}
