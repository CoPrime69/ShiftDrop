#include "keyboard/WheelRequest.h"
#include "Strings.h"

#include <shellapi.h>
#include <wil/resource.h>

namespace sd::wheelrequest
{
    bool HasArg(const std::vector<std::wstring>& args, std::wstring_view arg)
    {
        for (const auto& a : args)
            if (EqualsNoCase(a, arg))
                return true;
        return false;
    }

    std::vector<std::wstring> WheelPaths(const std::vector<std::wstring>& args)
    {
        std::vector<std::wstring> paths;
        bool after = false;
        for (const auto& a : args)
        {
            if (!after)
                after = EqualsNoCase(a, kWheelArg);
            else if (!a.empty())
                paths.push_back(a);
        }
        return paths;
    }

    std::vector<std::wstring> CommandLineArgs()
    {
        int argc = 0;
        wil::unique_hlocal_ptr<PWSTR> argv(CommandLineToArgvW(GetCommandLineW(), &argc));
        std::vector<std::wstring> args;
        for (int i = 1; argv && i < argc; ++i)
            args.emplace_back(argv.get()[i]);
        return args;
    }

    std::wstring Encode(const std::vector<std::wstring>& paths)
    {
        std::wstring out;
        for (const auto& p : paths)
        {
            if (p.empty())
                continue;
            out += p;
            out += L'\0';
        }
        return out;
    }

    std::vector<std::wstring> Decode(const wchar_t* data, size_t chars)
    {
        std::vector<std::wstring> paths;
        if (!data)
            return paths;
        size_t start = 0;
        for (size_t i = 0; i < chars; ++i)
        {
            if (data[i] == L'\0')
            {
                if (i > start)
                    paths.emplace_back(data + start, i - start);
                start = i + 1;
            }
        }
        if (start < chars)   // a sender that left out the last NUL
            paths.emplace_back(data + start, chars - start);
        return paths;
    }

    std::vector<std::wstring> CleanSelection(const std::vector<std::wstring>& paths)
    {
        std::vector<std::wstring> out;
        for (const auto& p : paths)
        {
            if (p.empty())
                continue;
            bool seen = false;
            for (const auto& q : out)
                seen = seen || EqualsNoCase(p, q);
            if (!seen)
                out.push_back(p);
        }
        return out;
    }
}
