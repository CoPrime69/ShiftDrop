// The Win32 side of platform/Clock.h.
#include "platform/Clock.h"

#include <windows.h>

namespace sd::platform
{
    uint64_t MonotonicMilliseconds()
    {
        return GetTickCount64();
    }

    LocalDateTime LocalTimeNow()
    {
        SYSTEMTIME t{};
        GetLocalTime(&t);
        return { t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond };
    }
}
