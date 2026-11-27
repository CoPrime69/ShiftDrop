// The Win32 side of platform/Process.h.
#include "platform/Process.h"

#include <windows.h>

namespace sd::platform
{
    uint32_t CurrentProcessId()
    {
        return GetCurrentProcessId();
    }
}
