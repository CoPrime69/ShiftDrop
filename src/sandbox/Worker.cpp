#include "sandbox/Worker.h"
#include "archive/ArchiveSource.h"

#include <windows.h>

#include <algorithm>
#include <string_view>

namespace sd::sandbox
{
    std::vector<std::wstring> WorkerArgs()
    {
        // Our own split instead of CommandLineToArgvW, which would load shell32 (and its dozens of
        // dependencies) into the worker. The parent writes the program name quoted and every other
        // argument as a plain token.
        const std::wstring_view line = GetCommandLineW();
        size_t i = 0;
        if (!line.empty() && line[0] == L'"')
        {
            const size_t close = line.find(L'"', 1);
            i = close == std::wstring_view::npos ? line.size() : close + 1;
        }
        else
            i = std::min(line.find(L' '), line.size());
        std::vector<std::wstring> tokens;
        while (i < line.size())
        {
            while (i < line.size() && line[i] == L' ')
                ++i;
            const size_t end = std::min(line.find(L' ', i), line.size());
            if (end > i)
                tokens.emplace_back(line.substr(i, end - i));
            i = end;
        }
        if (tokens.empty() || tokens[0] != L"--sd-worker")
            return {};
        return std::vector<std::wstring>(tokens.begin() + 1, tokens.end());
    }

    int WorkerMain(const std::vector<std::wstring>& args)
    {
        // A crash in a parser must end quietly (the parent reports it), never with a dialog.
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        if (args.empty())
            return 2;
        const std::vector<std::wstring> rest(args.begin() + 1, args.end());
        if (args[0] == L"archive-read")
            return archive::RunReadJob(rest);
        return 2;
    }
}
