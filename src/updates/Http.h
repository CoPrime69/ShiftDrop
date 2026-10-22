#pragma once
#include <cstdint>
#include <string>

// HTTPS GETs for ShiftDrop-Updater.exe, through WinHTTP: the only networking in ShiftDrop, and only
// in that exe (scripts/check-imports.ps1 keeps winhttp.dll out of the others). Each request is a
// plain GET: no cookies, no credentials (not even to a proxy), no body, nothing about the user;
// the User-Agent is "ShiftDrop-Updater/<version>". https only, redirects too.
namespace sd::updates::http
{
    // The answer's body, at most `maxBytes`. False with a short reason in `error` for anything but
    // 200 OK.
    bool Get(const std::wstring& url, const wchar_t* accept, uint64_t maxBytes, std::string& body, std::wstring& error);
    // The same into a new file at `path` (replaced if it is there); false leaves no file.
    bool Download(const std::wstring& url, const std::wstring& path, uint64_t maxBytes, std::wstring& error);
}
