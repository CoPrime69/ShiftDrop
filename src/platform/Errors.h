#pragma once
#include "platform/WinTypes.h"

namespace sd::platform
{
    // Does this error code mean the user cancelled (a dialog, a copy)? HResultMessage says
    // "cancelled" for it.
    bool IsCancelledCode(HRESULT hr);
    // Why the last system call that failed on this thread failed, as an HRESULT. Read it straight
    // after that call.
    HRESULT LastErrorCode();
}
