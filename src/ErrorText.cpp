#include "ErrorText.h"
#include "platform/Errors.h"

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

    std::wstring HResultText(HRESULT hr)
    {
        return HexCode(hr) + L" " + SystemMessage(hr);
    }

    std::wstring HResultMessage(HRESULT hr)
    {
        if (platform::IsCancelledCode(hr))
            return L"cancelled";
        auto text = SystemMessage(hr);
        while (!text.empty() && text.back() == L'.')
            text.pop_back();
        return text.empty() ? L"error " + HexCode(hr) : text;
    }
}
