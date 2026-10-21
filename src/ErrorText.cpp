#include "ErrorText.h"

#include <shobjidl.h>
#include <wil/resource.h>

#include <format>

namespace sd
{
    std::wstring HexCode(HRESULT hr)
    {
        return std::format(L"0x{:08X}", static_cast<unsigned>(hr));
    }

    std::wstring TrimMessage(std::wstring text)
    {
        while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' '))
            text.pop_back();
        return text;
    }

    std::wstring SystemMessage(HRESULT hr)
    {
        wil::unique_hlocal_string msg;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                       static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
        return TrimMessage(msg ? msg.get() : L"");
    }

    std::wstring HResultText(HRESULT hr)
    {
        return HexCode(hr) + L" " + SystemMessage(hr);
    }

    std::wstring HResultMessage(HRESULT hr)
    {
        if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) || hr == COPYENGINE_E_USER_CANCELLED)
            return L"cancelled";
        auto text = SystemMessage(hr);
        while (!text.empty() && text.back() == L'.')
            text.pop_back();
        return text.empty() ? L"error " + HexCode(hr) : text;
    }
}
