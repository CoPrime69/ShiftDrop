#pragma once
// The Win32 types and constants that shared headers name: HRESULT, HWND, UINT, the error codes and
// the like. A shared header that needs nothing else from Windows includes this rather than
// <windows.h>, so that another platform can supply those types alone, and never stand-ins for Win32
// functions. On Windows this is <windows.h> itself, so nothing changes there.
#include <windows.h>
