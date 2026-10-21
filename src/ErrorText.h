#pragma once
#include <windows.h>

#include <string>
#include <string_view>

// How an HRESULT reads in the log and in the messages the app shows. One copy, so every error code
// looks the same ("0x80070005") wherever it appears. The C++/WinRT form is in WinRtError.h.
namespace sd
{
    // "0x80070005".
    std::wstring HexCode(HRESULT hr);
    // The system's text for `hr` ("Access is denied."), without the line break; empty when it has none.
    std::wstring SystemMessage(HRESULT hr);
    // "0x80070005 Access is denied.": for the log and for details.
    std::wstring HResultText(HRESULT hr);
    // "Access is denied", "cancelled", or "error 0x80070005" when the system has no text: for a
    // message the user reads.
    std::wstring HResultMessage(HRESULT hr);
    // Text without trailing line breaks and spaces.
    std::wstring TrimMessage(std::wstring text);
}
