// The Windows side of platform/StdioFile.h: the C runtime's wide-path fopen.
#include "platform/StdioFile.h"

namespace sd::platform
{
    FILE* OpenFileToRead(const std::wstring& path)
    {
        return _wfopen(path.c_str(), L"rb");
    }

    FILE* OpenFileToWrite(const std::wstring& path)
    {
        return _wfopen(path.c_str(), L"wb");
    }
}
