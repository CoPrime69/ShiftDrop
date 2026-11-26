#include "platform/win/StagingStream.h"
#include "OutputPath.h"

#include <shlwapi.h>

namespace sd
{
    HRESULT CreateStaging(const std::wstring& source, std::wstring& stagingPath, Microsoft::WRL::ComPtr<IStream>& stream)
    {
        return CreateStagingFile(source, stagingPath, [&stream](const std::wstring& candidate) {
            // fCreate=TRUE + STGM_FAILIFTHERE: only ever creates a new file.
            return SHCreateStreamOnFileEx(candidate.c_str(), STGM_WRITE | STGM_SHARE_DENY_WRITE | STGM_FAILIFTHERE, FILE_ATTRIBUTE_NORMAL, TRUE,
                                          nullptr, stream.ReleaseAndGetAddressOf());
        });
    }

    HRESULT OpenStagingStream(const StagedOutput& out, Microsoft::WRL::ComPtr<IStream>& stream)
    {
        if (out.Path().empty() || out.IsFolderOutput())
            return E_UNEXPECTED;
        // fCreate=FALSE: the file was made new by ReserveStaging, so this only ever opens our own file.
        return SHCreateStreamOnFileEx(out.Path().c_str(), STGM_WRITE | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr,
                                      stream.ReleaseAndGetAddressOf());
    }
}
