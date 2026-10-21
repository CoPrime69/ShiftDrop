#include "Ffmpeg.h"
#include "Brand.h"
#include "Log.h"
#include "Utf.h"
#include "sandbox/MediaSandbox.h"

#include <windows.h>
#include <wil/resource.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <filesystem>
#include <format>
#include <thread>

namespace sd
{
    namespace
    {
        // Reads a pipe to EOF, calling `line` for each complete line (without the line break).
        void ReadLines(HANDLE pipe, const std::function<void(std::string_view)>& line)
        {
            std::string pending;
            char buffer[4096];
            DWORD read = 0;
            while (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0)
            {
                pending.append(buffer, read);
                size_t start = 0, end;
                // ffmpeg ends log lines with \n, and rewrites status lines with \r.
                while ((end = pending.find_first_of("\r\n", start)) != std::string::npos)
                {
                    if (end > start)
                        line(std::string_view(pending).substr(start, end - start));
                    start = end + 1;
                }
                pending.erase(0, start);
            }
            if (!pending.empty())
                line(pending);
        }
    }

    FfmpegEngine::FfmpegEngine(std::wstring exe) : m_exe(std::move(exe)) {}

    std::wstring FfmpegEngine::Locate()
    {
        wchar_t env[MAX_PATH]{};
        if (const DWORD n = GetEnvironmentVariableW(L"SHIFTDROP_FFMPEG", env, MAX_PATH); n > 0 && n < MAX_PATH)
            return GetFileAttributesW(env) != INVALID_FILE_ATTRIBUTES ? std::wstring(env) : std::wstring();
        wchar_t self[MAX_PATH]{};
        const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
        if (n == 0 || n == MAX_PATH)
            return {};
        const auto exe = std::filesystem::path(self).parent_path() / L"engines" / L"ffmpeg.exe";
        return GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES ? exe.wstring() : std::wstring();
    }

    H264Encoder FfmpegEngine::H264() const
    {
        std::call_once(m_probeOnce, [this] {
            // A GPU encoder is listed whenever ffmpeg was built with it, but only works with that
            // vendor's GPU and driver, so each one is tried on a few black frames. The tries run at
            // the same time (on a PC without that GPU most fail at once) and the best that worked wins.
            constexpr H264Encoder candidates[] = { H264Encoder::Nvenc, H264Encoder::Qsv, H264Encoder::Amf, H264Encoder::MediaFoundation };
            std::atomic<bool> works[std::size(candidates)]{};
            {
                std::vector<std::jthread> tries;
                for (size_t i = 0; i < std::size(candidates); ++i)
                    tries.emplace_back([&, i] {
                        const auto candidate = candidates[i];
                        const std::vector<std::wstring> args{ L"-hide_banner", L"-nostdin", L"-f", L"lavfi", L"-i", L"color=c=black:s=320x240:r=30:d=0.3",
                                                              L"-c:v", EncoderName(candidate), L"-pix_fmt", candidate == H264Encoder::Nvenc ? L"yuv420p" : L"nv12",
                                                              L"-f", L"null", L"-" };
                        std::stop_source never;
                        works[i] = Run(args, nullptr, never.get_token()).ok;
                    });
            }   // joins every try
            for (size_t i = 0; i < std::size(candidates); ++i)
                if (works[i])
                {
                    m_h264 = candidates[i];
                    break;
                }
            log::Info(L"ffmpeg: H.264 encoder {}", EncoderName(m_h264));
        });
        return m_h264;
    }

    MediaInfo FfmpegEngine::Probe(const std::wstring& input) const
    {
        std::vector<std::string> log;
        std::stop_source never;
        Run(ProbeArgs(input), nullptr, never.get_token(), &log);   // exits with 1 by design: no output was named
        return ParseMediaInfo(log);
    }

    FfmpegEngine::Result FfmpegEngine::Run(const std::vector<std::wstring>& args, const std::function<void(double)>& progress,
                                           std::stop_token stop, std::vector<std::string>* log, int64_t outputUs) const
    {
        Result result;
        if (m_exe.empty())
        {
            result.error = L"the media engine (ffmpeg.exe) is missing; reinstall " SD_NAME;
            return result;
        }
        // S6: with [Sandbox] Media=1, a run that reads one file into our staging output works on
        // handles inside the AppContainer (sandbox/MediaSandbox.h). Any other run is unchanged.
        sandbox::MediaJail jail;
        bool jailed = false;
        if (sandbox::MediaSandboxEnabled())
        {
            std::wstring why;
            jailed = jail.Prepare(args, why);
            if (!jailed)
                log::Info(L"ffmpeg: not sandboxed: {}", why);
        }
        std::wstring commandLine = QuoteArg(m_exe);
        for (const auto& a : jailed ? jail.Args() : args)
            commandLine += L' ' + QuoteArg(a);

        // Pipes for the child's stdout (progress) and stderr (log). Only the child's ends are
        // inheritable, and the attribute list below passes exactly those three handles, so handles
        // from other threads' work never leak into ffmpeg.
        SECURITY_ATTRIBUTES inherit{ sizeof(inherit), nullptr, TRUE };
        wil::unique_handle outRead, outWrite, errRead, errWrite;
        if (!CreatePipe(&outRead, &outWrite, &inherit, 0) || !CreatePipe(&errRead, &errWrite, &inherit, 0))
        {
            result.error = L"cannot create pipes for ffmpeg";
            return result;
        }
        SetHandleInformation(outRead.get(), HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(errRead.get(), HANDLE_FLAG_INHERIT, 0);
        wil::unique_hfile nul(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr));

        // Exactly these handles are inherited (a handle list), so handles from other threads' work
        // never leak into ffmpeg. sandbox::Launch starts it suspended, puts it in a kill-on-close job
        // (closing `job` on return, or Windows if we die, ends ffmpeg) and only then lets it run.
        sandbox::LaunchSpec spec;
        spec.exe = m_exe;
        spec.commandLine = commandLine;
        spec.inherit = { nul.get(), outWrite.get(), errWrite.get() };
        spec.stdIn = nul.get();
        spec.stdOut = outWrite.get();
        spec.stdErr = errWrite.get();
        spec.appContainer = jailed;
        // A jailed run also gets a memory cap, no child processes and no UI access; a normal run keeps
        // the plain kill-on-close job it always had.
        spec.limits = jailed ? sandbox::Limits{ 8ull << 30, 0, true, true } : sandbox::Limits{ 0, 0, false, false };
        if (jailed && !jail.Open(spec, result.error))
            return result;
        auto child = sandbox::Launch(spec);
        if (!child)
        {
            result.error = std::format(L"cannot start ffmpeg: {}", child.error);
            return result;
        }
        wil::unique_handle process = std::move(child.process), job = std::move(child.job);
        // Our copies of the child's ends: closed now, so the reads below end when ffmpeg exits.
        outWrite.reset();
        errWrite.reset();
        nul.reset();

        std::atomic<int64_t> durationUs{ outputUs > 0 ? outputUs : -1 };   // else the input's, from the log
        std::deque<std::string> tail;   // ffmpeg's last log lines, for the error message
        std::jthread errReader([&] {
            ReadLines(errRead.get(), [&](std::string_view line) {
                if (durationUs.load() < 0)
                    if (const int64_t d = ParseDurationLine(line); d > 0)
                        durationUs = d;
                tail.emplace_back(line);
                if (tail.size() > 12)
                    tail.pop_front();
                if (log && log->size() < 400)
                    log->emplace_back(line);
            });
        });
        std::jthread outReader([&] {
            ReadLines(outRead.get(), [&](std::string_view line) {
                const int64_t at = ParseProgressLine(line);
                const int64_t total = durationUs.load();
                if (progress && at >= 0 && total > 0)
                    progress(std::min(1.0, static_cast<double>(at) / static_cast<double>(total)));
            });
        });

        while (WaitForSingleObject(process.get(), 100) == WAIT_TIMEOUT)
            if (stop.stop_requested())
            {
                TerminateJobObject(job.get(), 1);
                result.cancelled = true;
                WaitForSingleObject(process.get(), 5000);
                break;
            }
        errReader.join();
        outReader.join();

        DWORD exitCode = 1;
        GetExitCodeProcess(process.get(), &exitCode);
        result.ok = !result.cancelled && exitCode == 0;
        if (result.cancelled)
            result.error = L"cancelled";
        else if (!result.ok)
        {
            // The useful part is usually just above "Conversion failed!": keep the last few lines.
            std::string text;
            for (auto it = tail.size() > 3 ? tail.end() - 3 : tail.begin(); it != tail.end(); ++it)
                text += (text.empty() ? "" : " | ") + *it;
            result.error = Widen(text.empty() ? std::format("ffmpeg exited with code {}", exitCode) : text);
        }
        return result;
    }
}
