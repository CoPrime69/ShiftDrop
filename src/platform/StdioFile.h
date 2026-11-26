#pragma once
#include <cstdio>
#include <string>

// C stdio files opened by a wide path, for the libraries that take a FILE* (QPDF).
namespace sd::platform
{
    // Opens `path` to read as binary; nullptr if it can't.
    FILE* OpenFileToRead(const std::wstring& path);
    // Opens `path` to write as binary from the start, creating it or emptying it; nullptr if it can't.
    FILE* OpenFileToWrite(const std::wstring& path);
}
