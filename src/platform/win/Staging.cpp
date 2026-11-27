// The Win32 side of platform/Staging.h.
#include "platform/Staging.h"

#include <shlwapi.h>
#include <wrl/client.h>

namespace sd::platform
{
    HRESULT CreateNewFile(const std::wstring& path)
    {
        // fCreate=TRUE + STGM_FAILIFTHERE: only ever creates a new file. The stream is released on
        // return, which closes the file so a writer can open it by name.
        Microsoft::WRL::ComPtr<IStream> stream;
        return SHCreateStreamOnFileEx(path.c_str(), STGM_WRITE | STGM_SHARE_DENY_WRITE | STGM_FAILIFTHERE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr,
                                      stream.ReleaseAndGetAddressOf());
    }

    bool CreateNewFolder(const std::wstring& path)
    {
        return CreateDirectoryW(path.c_str(), nullptr) != FALSE;
    }

    bool RenameWithoutReplacing(const std::wstring& from, const std::wstring& to)
    {
        // No MOVEFILE_REPLACE_EXISTING: the rename fails if the name exists, even in a race.
        return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    }

    bool RemoveFile(const std::wstring& path)
    {
        return DeleteFileW(path.c_str()) != FALSE;
    }

    void RemoveFolderIfEmpty(const std::wstring& path)
    {
        RemoveDirectoryW(path.c_str());   // only ever removes an empty folder
    }
}
