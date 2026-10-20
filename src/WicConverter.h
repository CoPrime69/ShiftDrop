#pragma once
#include "IConverter.h"

#include <set>
#include <string>

namespace sd
{
    // Image conversion through the Windows Imaging Component.
    //
    // Decoding: whatever WIC decoders are installed. PNG, JPEG, BMP, GIF, TIFF, ICO are built in;
    // WebP, HEIC/HEIF and AVIF depend on Store extensions ("WebP Image Extensions", "HEIF Image
    // Extensions" + "HEVC Video Extensions", "AV1 Video Extension"). The decoder list is read at
    // runtime, so a slice lights up only for files this PC can actually decode.
    // Encoding: PNG, JPG, BMP, TIFF (always present in WIC).
    class WicConverter final : public IConverter
    {
    public:
        WicConverter();   // enumerates codecs; call after COM is initialised on this thread

        bool supports(std::wstring_view extension) const override;
        std::vector<Action> actions() const override;
        JobResult run(const Job& job, const ProgressFn& progress, std::stop_token stop) const override;

        const std::wstring& CodecSummary() const { return m_summary; }   // for the startup log

    private:
        std::set<std::wstring> m_decodable;   // ".jpg", ".heic", ...
        std::wstring m_summary;
    };

    // True when a file with this extension is already in the action's output format (e.g. ".jpeg" for JPG).
    bool IsAlreadyTargetFormat(std::wstring_view actionId, const std::wstring& extension);
}
