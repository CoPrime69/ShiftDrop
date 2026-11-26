// The Win32 side of platform/SecureWipe.h.
#include "platform/SecureWipe.h"

#include <windows.h>

namespace sd::platform
{
    void WipeSecret(void* data, size_t bytes)
    {
        SecureZeroMemory(data, bytes);
    }
}
