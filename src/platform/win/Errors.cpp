// The Win32 side of ErrorText.h and platform/Errors.h: the system's text for an error code, and the
// codes that mean "cancelled".
#include "ErrorText.h"
#include "platform/Errors.h"

#include <shobjidl.h>
#include <wil/resource.h>

namespace sd
{
    std::wstring SystemMessage(HRESULT hr)
    {
        wil::unique_hlocal_string msg;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                       static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
        return TrimMessage(msg ? msg.get() : L"");
    }

    namespace platform
    {
        bool IsCancelledCode(HRESULT hr)
        {
            return hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) || hr == COPYENGINE_E_USER_CANCELLED;
        }

        HRESULT LastErrorCode()
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }
    }
}
