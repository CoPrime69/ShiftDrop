#include "archive/ArchiveSource.h"
#include "archive/EntryPipe.h"
#include "Log.h"
#include "Media.h"
#include "sandbox/Sandbox.h"

#include <wil/resource.h>

#include <cwchar>
#include <filesystem>
#include <format>
#include <mutex>

namespace sd::archive
{
    namespace
    {
        // The worker's job limits. Archive formats need little memory (7z/LZMA2 at most a few hundred
        // MB for its dictionary); anything far beyond that is a decompression bomb or a bug.
        constexpr uint64_t kWorkerMemory = 2ull << 30;
        constexpr uint64_t kWorkerCpuSeconds = 2 * 60 * 60;

        std::wstring HandleArg(HANDLE h)
        {
            return std::to_wstring(reinterpret_cast<uintptr_t>(h));
        }

        Result InProcess(HANDLE file, const ReadOptions& options, IEntrySink& sink, std::stop_token stop, const std::function<void(double)>& progress)
        {
            return ReadArchive(file, options, sink, [&](uint64_t consumed) {
                if (progress && options.inputSize > 0)
                    progress(std::min(1.0, static_cast<double>(consumed) / static_cast<double>(options.inputSize)));
                return !stop.stop_requested();
            });
        }
    }

    Result ReadEntries(const std::wstring& path, Kind kind, const Limits& limits, IEntrySink& sink, std::stop_token stop,
                       const std::function<void(double)>& progress, ReadMode* mode)
    {
        // Inheritable from the start (the worker gets exactly these handles through the handle list,
        // so no other thread's child can pick them up). Read-only: the worker cannot change the archive.
        SECURITY_ATTRIBUTES inherit{ sizeof(inherit), nullptr, TRUE };
        wil::unique_hfile file(CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ, &inherit, OPEN_EXISTING,
                                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!file)
            return Fail(Status::Io, std::format(L"cannot open the archive ({})", GetLastError()));
        LARGE_INTEGER size{};
        GetFileSizeEx(file.get(), &size);
        ReadOptions options{ kind, limits, static_cast<uint64_t>(size.QuadPart) };

        const auto worker = sandbox::WorkerPath();
        if (worker.empty())
        {
            static std::once_flag once;
            std::call_once(once, [] { log::Warn(L"archive: no worker exe; reading archives inside the app process (not sandboxed)"); });
            if (mode)
                *mode = ReadMode::InProcess;
            return InProcess(file.get(), options, sink, stop, progress);
        }

        wil::unique_handle pipeRead, pipeWrite;
        if (!CreatePipe(&pipeRead, &pipeWrite, &inherit, 1 << 20))
            return Fail(Status::Io, L"cannot create a pipe for the archive worker");
        SetHandleInformation(pipeRead.get(), HANDLE_FLAG_INHERIT, 0);

        sandbox::LaunchSpec spec;
        spec.exe = worker;
        spec.commandLine = std::format(L"{} --sd-worker archive-read --in {} --out {} --kind {} --size {} --max-bytes {} --max-entries {}", QuoteArg(worker),
                                       HandleArg(file.get()), HandleArg(pipeWrite.get()), static_cast<int>(kind), options.inputSize,
                                       limits.maxTotalBytes, limits.maxEntries);
        spec.inherit = { file.get(), pipeWrite.get() };
        spec.limits = sandbox::Limits{ kWorkerMemory, kWorkerCpuSeconds, true, true };
        spec.appContainer = sandbox::ArchiveSandboxEnabled();
        spec.mitigations = true;
        auto child = sandbox::Launch(spec);
        if (!child)
            return Fail(Status::Worker, child.error);
        if (mode)
            *mode = child.sandboxed ? ReadMode::Sandboxed : ReadMode::Worker;
        pipeWrite.reset();   // ours: the pipe ends when the worker exits

        // Cancel ends the worker through its job; the pipe then breaks and PumpPipe returns.
        std::stop_callback onStop(stop, [&] { TerminateJobObject(child.job.get(), 1); });
        Result result = PumpPipe(pipeRead.get(), sink, [&](uint64_t consumed) {
            if (progress && options.inputSize > 0)
                progress(std::min(1.0, static_cast<double>(consumed) / static_cast<double>(options.inputSize)));
        });
        if (!result.ok())
            TerminateJobObject(child.job.get(), 1);   // whatever it is doing, it is no longer wanted
        WaitForSingleObject(child.process.get(), 5000);
        if (stop.stop_requested())
            return Fail(Status::Cancelled, L"cancelled");
        return result;
    }

    int RunReadJob(const std::vector<std::wstring>& args)
    {
        HANDLE in = nullptr, out = nullptr;
        ReadOptions options;
        for (size_t i = 0; i + 1 < args.size(); i += 2)
        {
            const auto& key = args[i];
            const uint64_t value = std::wcstoull(args[i + 1].c_str(), nullptr, 10);
            if (key == L"--in") in = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
            else if (key == L"--out") out = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
            else if (key == L"--kind") options.kind = static_cast<Kind>(value);
            else if (key == L"--size") options.inputSize = value;
            else if (key == L"--max-bytes") options.limits.maxTotalBytes = value;
            else if (key == L"--max-entries") options.limits.maxEntries = value;
        }
        if (!in || !out)
            return 2;
        PipeWriter writer(out);
        uint64_t lastSent = 0;
        const Result r = ReadArchive(in, options, writer, [&](uint64_t consumed) {
            // A progress frame per 256 KiB of archive is plenty; the parent kills us to cancel.
            if (consumed - lastSent >= (256u << 10))
            {
                lastSent = consumed;
                return writer.Progress(consumed);
            }
            return true;
        });
        return writer.Finish(r) ? 0 : 3;
    }
}
