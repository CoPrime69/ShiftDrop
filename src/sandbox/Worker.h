#pragma once
#include <string>
#include <vector>

// The sandboxed worker (ShiftDropWorker.exe): a tiny program with no window, no COM and no file or
// network access of its own (it runs in the AppContainer). It does one job per run on the handles
// its parent passed and exits:
//   ShiftDropWorker.exe --sd-worker archive-read --in <handle> --out <handle> ...
namespace sd::sandbox
{
    // The arguments after "--sd-worker" when this process was started as a worker; empty otherwise.
    std::vector<std::wstring> WorkerArgs();

    // Runs the job named by args[0]; returns the exit code.
    int WorkerMain(const std::vector<std::wstring>& args);
}
