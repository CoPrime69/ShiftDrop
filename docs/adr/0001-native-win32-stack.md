# ADR 0001: Native C++20, raw Win32 and COM

Status: accepted (2026-10-20)

## Context
ShiftDrop's core is a global mouse hook, an OLE drop target inside a layered topmost window, and
shell integration (tray, Explorer reveal, IFileOperation). All of these are Win32/COM APIs. The hook
callback runs on our UI thread for every mouse event on the desktop, so latency matters.

## Decision
C++20, raw Win32 and COM, no framework (no MFC, ATL, Qt, .NET). MSVC with CMake and vcpkg in
manifest mode; static CRT so the output is one exe. RAII through wil and WRL `ComPtr`; no bare
`new`/`delete`.

## Consequences
- No managed runtime to install, no interop layer between the hook, OLE and our code.
- UI is hand-built (Direct2D wheel, plain Win32 history window). More code than XAML for windows with
  many controls; acceptable while the UI is a wheel, a list and a settings page.
- Everything outside the gesture UI lives in `shiftdrop_core`, a static library the unit tests link.
