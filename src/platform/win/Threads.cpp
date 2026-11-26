// The Win32 side of platform/Threads.h.
#include "platform/Threads.h"

#include <windows.h>
#include <objbase.h>

namespace sd::platform
{
    ComApartmentForThread::ComApartmentForThread(bool singleThreaded)
    {
        const HRESULT hr = CoInitializeEx(nullptr, singleThreaded ? COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE : COINIT_MULTITHREADED);
        m_joined = SUCCEEDED(hr);
    }

    ComApartmentForThread::~ComApartmentForThread()
    {
        if (m_joined)
            CoUninitialize();
    }
}
