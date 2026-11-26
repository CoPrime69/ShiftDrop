#pragma once
#include "platform/WinTypes.h"

#include <string>

// The file-system calls output staging makes (OutputPath.cpp, StagedOutput.cpp). Each makes or
// removes one thing and never replaces what is there. When one returns false, platform::LastErrorCode
// (platform/Errors.h) says why.
namespace sd::platform
{
    // Creates an empty file at `path` and closes it; fails if anything is already at that name.
    HRESULT CreateNewFile(const std::wstring& path);
    // Creates the folder `path`; false if anything is already at that name, or it can't be made.
    bool CreateNewFolder(const std::wstring& path);
    // Renames `from` to `to`; false if anything is at `to` (it is never replaced) or the rename fails.
    bool RenameWithoutReplacing(const std::wstring& from, const std::wstring& to);
    // Deletes the file at `path`; false if it can't.
    bool RemoveFile(const std::wstring& path);
    // Removes the folder `path` if it is empty, and leaves it otherwise.
    void RemoveFolderIfEmpty(const std::wstring& path);
}
