#pragma once
#include <cstdint>
#include <string>

namespace sd
{
    // SHA-256 of a file's contents as lower-case hex, using Windows CNG (bcrypt). Empty on error.
    // Used by undo to make sure we only recycle outputs nobody has edited since we wrote them.
    std::string Sha256File(const std::wstring& path, uint64_t* size = nullptr);
}
