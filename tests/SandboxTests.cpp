// Sealed engines: the AppContainer worker can do its job but cannot open the user's files or
// connect anywhere, and ffmpeg works on handles inside the same sandbox.
//
// This exe doubles as the worker (TestMain.cpp): "--sd-worker probe ..." runs the probe below,
// any other job goes to sandbox::WorkerMain like in ShiftDropWorker.exe.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "Ffmpeg.h"
#include "Media.h"
#include "MediaTestUtil.h"
#include "PathUtil.h"
#include "PipelineRunner.h"
#include "TestUtil.h"
#include "Utf.h"
#include "archive/ArchiveSource.h"
#include "archive/ArchiveWriter.h"
#include "archive/Sinks.h"
#include "sandbox/MediaSandbox.h"
#include "sandbox/Sandbox.h"
#include "sandbox/Worker.h"

#include <cstring>
#include <format>
#include <sstream>

using namespace sd;
using namespace sd::test;

namespace
{
    std::wstring Hex(const std::wstring& text)
    {
        std::wstring out;
        for (const wchar_t c : text)
            out += std::format(L"{:04x}", static_cast<unsigned>(c));
        return out;
    }

    std::wstring Unhex(const std::wstring& hex)
    {
        std::wstring out;
        for (size_t i = 0; i + 4 <= hex.size(); i += 4)
            out += static_cast<wchar_t>(std::wcstoul(hex.substr(i, 4).c_str(), nullptr, 16));
        return out;
    }

    // In the worker: tries what a hostile parser would try, and reports each error code.
    int Probe(const std::vector<std::wstring>& args)
    {
        HANDLE out = nullptr;
        std::wstring path;
        int port = 0;
        bool wait = false;
        for (size_t i = 1; i + 1 < args.size(); i += 2)
        {
            if (args[i] == L"--out") out = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::wcstoull(args[i + 1].c_str(), nullptr, 10)));
            else if (args[i] == L"--path") path = Unhex(args[i + 1]);
            else if (args[i] == L"--port") port = std::stoi(args[i + 1]);
            else if (args[i] == L"--wait") wait = true;
        }
        if (wait)
        {
            Sleep(60'000);
            return 0;
        }
        DWORD open = 0;
        {
            HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f == INVALID_HANDLE_VALUE)
                open = GetLastError();
            else
                CloseHandle(f);
        }
        DWORD list = 0;
        {
            wchar_t profile[MAX_PATH]{};
            GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
            WIN32_FIND_DATAW data{};
            HANDLE find = FindFirstFileW((std::wstring(profile) + L"\\*").c_str(), &data);
            if (find == INVALID_HANDLE_VALUE)
                list = GetLastError();
            else
                FindClose(find);
        }
        int connected = 0;
        WSADATA wsa{};
        const int startup = WSAStartup(MAKEWORD(2, 2), &wsa);
        if (startup == 0)
        {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET)
                connected = WSAGetLastError();
            else
            {
                sockaddr_in to{};
                to.sin_family = AF_INET;
                to.sin_port = htons(static_cast<u_short>(port));
                inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
                // Non-blocking with a 5 s wait: inside an AppContainer, Windows' filtering platform
                // may drop the packets instead of refusing, so a blocking connect would hang ~21 s.
                u_long nonBlocking = 1;
                ioctlsocket(s, FIONBIO, &nonBlocking);
                if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0)
                    connected = 0;
                else if ((connected = WSAGetLastError()) == WSAEWOULDBLOCK)
                {
                    fd_set writable, failed;
                    FD_ZERO(&writable);
                    FD_ZERO(&failed);
                    FD_SET(s, &writable);
                    FD_SET(s, &failed);
                    timeval wait{ 5, 0 };
                    const int ready = select(0, nullptr, &writable, &failed, &wait);
                    if (ready == 0)
                        connected = WSAETIMEDOUT;
                    else if (ready < 0)
                        connected = WSAGetLastError();
                    else if (FD_ISSET(s, &failed))
                    {
                        int error = 0, size = sizeof(error);
                        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size);
                        connected = error != 0 ? error : WSAECONNREFUSED;
                    }
                    else
                        connected = 0;
                }
                closesocket(s);
            }
            WSACleanup();
        }
        DWORD spawn = 0;
        {
            wchar_t cmd[] = L"cmd.exe /c exit 0";
            STARTUPINFOW si{ sizeof(si) };
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            {
                WaitForSingleObject(pi.hProcess, 5000);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
            else
                spawn = GetLastError();
        }
        const auto report = std::format("open={} list={} startup={} connect={} spawn={}\n", open, list, startup, connected, spawn);
        DWORD written = 0;
        WriteFile(out, report.data(), static_cast<DWORD>(report.size()), &written, nullptr);
        return 0;
    }

    struct ProbeResult
    {
        bool started = false;
        bool sandboxed = false;
        unsigned long open = 99, list = 99, spawn = 99;
        int startup = 99, connect = 99;
        std::string raw;
    };

    ProbeResult RunProbe(bool appContainer, const std::wstring& path, int port)
    {
        ProbeResult result;
        SECURITY_ATTRIBUTES inherit{ sizeof(inherit), nullptr, TRUE };
        wil::unique_handle readEnd, writeEnd;
        if (!CreatePipe(&readEnd, &writeEnd, &inherit, 0))
            return result;
        SetHandleInformation(readEnd.get(), HANDLE_FLAG_INHERIT, 0);
        sandbox::LaunchSpec spec;
        spec.exe = ThisExe();
        spec.commandLine = std::format(L"\"{}\" --sd-worker probe --out {} --path {} --port {}", spec.exe,
                                       reinterpret_cast<uintptr_t>(writeEnd.get()), Hex(path), port);
        spec.inherit = { writeEnd.get() };
        spec.limits = sandbox::Limits{ 1ull << 30, 60, true, true };
        spec.appContainer = appContainer;
        auto child = sandbox::Launch(spec);
        writeEnd.reset();
        if (!child)
            return result;
        result.started = true;
        result.sandboxed = child.sandboxed;
        char buffer[512];
        DWORD read = 0;
        while (ReadFile(readEnd.get(), buffer, sizeof(buffer), &read, nullptr) && read > 0)
            result.raw.append(buffer, read);
        WaitForSingleObject(child.process.get(), 10'000);
        sscanf_s(result.raw.c_str(), "open=%lu list=%lu startup=%d connect=%d spawn=%lu", &result.open, &result.list, &result.startup,
                 &result.connect, &result.spawn);
        return result;
    }

    // A listening socket on 127.0.0.1, so a connect from outside the sandbox succeeds.
    class Listener
    {
    public:
        Listener()
        {
            WSADATA wsa{};
            WSAStartup(MAKEWORD(2, 2), &wsa);
            m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in at{};
            at.sin_family = AF_INET;
            inet_pton(AF_INET, "127.0.0.1", &at.sin_addr);
            bind(m_socket, reinterpret_cast<sockaddr*>(&at), sizeof(at));
            listen(m_socket, 4);
            int size = sizeof(at);
            getsockname(m_socket, reinterpret_cast<sockaddr*>(&at), &size);
            port = ntohs(at.sin_port);
        }
        ~Listener()
        {
            closesocket(m_socket);
            WSACleanup();
        }
        // True when a connection reached us (it waits in the backlog until accepted).
        bool Reached() const
        {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(m_socket, &readable);
            timeval now{ 0, 0 };
            return select(0, &readable, nullptr, nullptr, &now) > 0;
        }
        int port = 0;

    private:
        SOCKET m_socket = INVALID_SOCKET;
    };

    // Restores the media sandbox setting however the test ends.
    struct MediaSandboxOn
    {
        MediaSandboxOn() { sd::sandbox::SetMediaSandbox(true); }
        ~MediaSandboxOn() { sd::sandbox::SetMediaSandbox(false); }
    };

    std::string SkipReason()
    {
        const auto& c = sandbox::AppContainer();
        return "AppContainer is unavailable here (" + Narrow(c.why) + "); Wine has no AppContainer, Windows CI runs this";
    }
}

namespace sd::test
{
    int RunWorkerIfAsked()
    {
        const auto args = sandbox::WorkerArgs();
        if (args.empty())
            return -1;
        if (args[0] == L"probe")
            return Probe(args);
        return sandbox::WorkerMain(args);
    }
}

// ---- The launcher --------------------------------------------------------------------------------

TEST(Sandbox, CrtDescriptorBlockLayout)
{
    const HANDLE h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x1234));
    const auto block = sandbox::CrtFdBlock({ nullptr, INVALID_HANDLE_VALUE, h });
    ASSERT_EQ(block.size(), sizeof(int) + 3 * (1 + sizeof(HANDLE)));
    int count = 0;
    std::memcpy(&count, block.data(), sizeof(int));
    EXPECT_EQ(count, 3);
    EXPECT_EQ(block[4], 0);
    EXPECT_EQ(block[5], 0);
    EXPECT_EQ(block[6], 1);   // FOPEN
    HANDLE third = nullptr;
    std::memcpy(&third, block.data() + 4 + 3 + 2 * sizeof(HANDLE), sizeof(HANDLE));
    EXPECT_EQ(third, h);
}

TEST(Sandbox, PlainWorkerReachesWhatTheSandboxBlocks)
{
    // The control for the denial test below: the same probe outside the AppContainer can open the
    // file and connect, so a "denied" there really comes from the sandbox.
    TempDir dir;
    const auto secret = dir / L"secret.txt";
    WriteText(secret, "private");
    Listener listener;
    const auto r = RunProbe(false, secret, listener.port);
    ASSERT_TRUE(r.started);
    EXPECT_FALSE(r.sandboxed);
    EXPECT_EQ(r.open, 0u) << r.raw;
    EXPECT_EQ(r.connect, 0) << r.raw;
    EXPECT_TRUE(listener.Reached());
}

TEST(Sandbox, AppContainerDeniesFilesAndNetwork)
{
    if (!sandbox::AppContainer().Available())
        GTEST_SKIP() << SkipReason();
    TempDir dir;   // under %TEMP%, inside the user profile
    const auto secret = dir / L"secret.txt";
    WriteText(secret, "private");
    Listener listener;
    const auto r = RunProbe(true, secret, listener.port);
    ASSERT_TRUE(r.started);
    if (!r.sandboxed)
        GTEST_SKIP() << "the AppContainer is not enforced on this system";
    EXPECT_EQ(r.open, static_cast<unsigned long>(ERROR_ACCESS_DENIED)) << "a file in the user profile: " << r.raw;
    EXPECT_NE(r.list, 0u) << "listing the user profile: " << r.raw;
    EXPECT_EQ(r.startup, 0) << "Winsock itself loads";
    // Loopback from an AppContainer without capabilities is either refused (WSAEACCES) or dropped
    // by the filtering platform (a time-out); either way nothing may reach the listener.
    EXPECT_TRUE(r.connect == WSAEACCES || r.connect == WSAETIMEDOUT) << "connect to 127.0.0.1 is denied: " << r.raw;
    EXPECT_FALSE(listener.Reached()) << "nothing reached 127.0.0.1";
    EXPECT_NE(r.spawn, 0u) << "no child processes: " << r.raw;
}

TEST(Sandbox, JobKillsTheWorkerWhenClosed)
{
    sandbox::LaunchSpec spec;
    spec.exe = ThisExe();
    spec.commandLine = std::format(L"\"{}\" --sd-worker probe --wait 1", spec.exe);
    spec.limits = sandbox::Limits{ 512ull << 20, 30, true, true };
    auto child = sandbox::Launch(spec);
    ASSERT_TRUE(child) << Narrow(child.error);
    EXPECT_EQ(WaitForSingleObject(child.process.get(), 200), static_cast<DWORD>(WAIT_TIMEOUT)) << "it waits";
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    ASSERT_TRUE(QueryInformationJobObject(child.job.get(), JobObjectExtendedLimitInformation, &info, sizeof(info), nullptr));
    // Wine answers this query with all zeros; Windows reports the flags that were set.
    if (info.BasicLimitInformation.LimitFlags != 0)
    {
        EXPECT_TRUE(info.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE);
        EXPECT_TRUE(info.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY);
        EXPECT_EQ(info.ProcessMemoryLimit, static_cast<SIZE_T>(512ull << 20));
    }
    child.job.reset();   // what a cancel, an exit or a crash of ShiftDrop does
    EXPECT_EQ(WaitForSingleObject(child.process.get(), 5000), static_cast<DWORD>(WAIT_OBJECT_0));
}

TEST(Sandbox, WorkerExtractsInsideTheAppContainer)
{
    TempDir dir;
    UseTestWorker();
    const auto source = dir / L"in.txt";
    WriteText(source, "hello from the sandbox");
    const auto zip = dir / L"in.zip";
    ASSERT_TRUE(sd::archive::Compress({ source }, sd::archive::WriteFormat::Zip, zip, {}, {}).ok());

    const auto out = dir / L"out";
    std::filesystem::create_directories(out);
    sd::archive::ExtractSink sink(out);
    sd::archive::GuardSink guard(sink, sd::archive::Limits{}, std::filesystem::file_size(zip));
    sd::archive::ReadMode mode = sd::archive::ReadMode::InProcess;
    const auto r = sd::archive::ReadEntries(zip, sd::archive::Kind::Zip, sd::archive::Limits{}, guard, {}, {}, &mode);
    ASSERT_TRUE(r.ok()) << Narrow(r.message);
    EXPECT_EQ(ReadAll(out + L"\\in.txt"), "hello from the sandbox");
    if (!sandbox::AppContainer().Available())
    {
        EXPECT_EQ(mode, sd::archive::ReadMode::Worker) << "falls back to a plain worker";
        GTEST_SKIP() << SkipReason() << " (the plain worker did the job)";
    }
    EXPECT_EQ(mode, sd::archive::ReadMode::Sandboxed);
}

// ---- ffmpeg on handles ---------------------------------------------------------------------------

TEST(MediaSandbox, PlansOneInputIntoTheStagingFile)
{
    TempDir dir;
    const auto in = dir / L"in clip.avi";
    const auto out = dir / L"~in clip.1a2b3c4d.sdpart";
    WriteText(in, "x");
    WriteText(out, "");
    sandbox::MediaJail jail;
    std::wstring why;
    ASSERT_TRUE(jail.Prepare(FfmpegArgs(in, { L"-hwaccel", L"auto" }, { L"-c", L"copy", L"-f", L"matroska" }, out), why)) << Narrow(why);
    const auto& a = jail.Args();
    const auto has = [&](std::initializer_list<const wchar_t*> run) {
        const std::vector<std::wstring> want(run.begin(), run.end());
        return std::search(a.begin(), a.end(), want.begin(), want.end()) != a.end();
    };
    EXPECT_TRUE(has({ L"-protocol_whitelist", L"fd" }));
    EXPECT_FALSE(has({ L"-protocol_whitelist", L"file" }));
    EXPECT_TRUE(has({ L"-hwaccel", L"auto", L"-fd", L"3", L"-i", L"fd:" }));
    ASSERT_GE(a.size(), 3u);
    EXPECT_EQ(std::vector<std::wstring>(a.end() - 3, a.end()), (std::vector<std::wstring>{ L"-fd", L"4", L"fd:" }));
    for (const auto& arg : a)
        EXPECT_EQ(arg.find(dir.path().wstring()), std::wstring::npos) << "no path is left in the command: " << Narrow(arg);

    sandbox::MediaJail probe;
    ASSERT_TRUE(probe.Prepare(ProbeArgs(in), why)) << Narrow(why);
    EXPECT_TRUE(probe.Output().empty());
}

TEST(MediaSandbox, RunsThatNameOtherFilesStayOutside)
{
    TempDir dir;
    const auto in = dir / L"a.avi", in2 = dir / L"b.avi", out = dir / L"~a.00000000.sdpart", plain = dir / L"a.mp4";
    for (const auto& f : { in, in2, out, plain })
        WriteText(f, "x");
    const std::vector<std::wstring> mux{ L"-f", L"mp4" };
    std::wstring why;
    const auto refused = [&](std::vector<std::wstring> args) {
        sandbox::MediaJail jail;
        return !jail.Prepare(args, why);
    };
    EXPECT_TRUE(refused(FfmpegArgs(in, {}, mux, plain))) << "output is not a staging file";
    EXPECT_TRUE(refused(FfmpegArgs(in, { L"-i", in2 }, mux, out))) << "two inputs";
    EXPECT_TRUE(refused(FfmpegArgs(in, { L"-f", L"concat" }, mux, out))) << "concat lists name files";
    EXPECT_TRUE(refused(FfmpegArgs(in, {}, { L"-passlogfile", L"x", L"-f", L"mp4" }, out))) << "two-pass logs";
    EXPECT_TRUE(refused(FfmpegArgs(in, {}, { L"-vf", L"subtitles='C\\:/subs.srt'", L"-f", L"mp4" }, out))) << "subtitles from a file";
    EXPECT_TRUE(refused(FfmpegArgs(in, {}, { L"-c", L"copy" }, out))) << "no muxer named";
    EXPECT_TRUE(refused({ L"-i", in, L"-f", L"mp4", out })) << "no -nostdin";
    EXPECT_TRUE(refused({ L"-nostdin", L"-f", L"lavfi", L"-i", L"testsrc", L"-f", L"mp4", out })) << "generated input";
}

namespace
{
    bool MakeClip(const FfmpegEngine& engine, const std::wstring& path, bool video)
    {
        std::stop_source never;
        std::vector<std::wstring> args{ L"-hide_banner", L"-nostdin" };
        if (video)
            args.insert(args.end(), { L"-f", L"lavfi", L"-i", L"testsrc=s=320x240:r=25:d=2", L"-c:v", L"mpeg4" });
        args.insert(args.end(), { L"-f", L"lavfi", L"-i", L"sine=f=440:d=2", L"-c:a", L"pcm_s16le", L"-shortest", L"-y", L"-f", video ? L"avi" : L"wav", path });
        return engine.Run(args, nullptr, never.get_token()).ok;
    }
}

TEST(MediaSandbox, EveryMediaRecipeWorksOnHandles)
{
    const auto engine = TestFfmpeg();
    if (!engine->Available())
        GTEST_SKIP() << "SHIFTDROP_FFMPEG is not set";
    TempDir dir;
    Recipes recipes(dir);
    const auto video = dir / L"clip.avi", audio = dir / L"tone.wav";
    ASSERT_TRUE(MakeClip(*engine, video, true));
    ASSERT_TRUE(MakeClip(*engine, audio, false));

    MediaSandboxOn on;
    const int before = sandbox::MediaJail::OpenCount();
    const PipelineRunner runner(std::make_shared<const ImageCodecs>(), nullptr, engine);
    int runs = 0, toolRuns = 0;
    for (auto recipe : recipes.store.All())
    {
        // Convert and Compress recipes, plus the video and audio tools that run without an editor
        // (those keep working with the setting on, inside the jail or, if they name other files, outside).
        // Normalize's chooser only picks a level: run it as the preset the pick makes.
        if (recipe.steps.size() == 1 && recipe.steps[0].action == "audio.normalize")
            recipe.steps[0].params["ask"] = false;
        const bool tool = !recipe.IsMedia() && recipe.steps.size() == 1 &&
                          (recipe.steps[0].action.starts_with("video.") || recipe.steps[0].action.starts_with("audio."));
        if (!recipe.IsMedia() && !tool)
            continue;
        if (tool && (!runner.EngineFor(recipe) || runner.EngineFor(recipe)->NeedsEditor(recipe)))
            continue;
        for (const auto& source : { video, audio })
        {
            if (!runner.Applies(recipe, source) || runner.IsNoOp(recipe, source))
                continue;
            const auto r = runner.Run(recipe, 0, source, {});
            if (!r.ok)
            {
                // A refusal that has nothing to do with the jail (a size preset on a clip that is
                // already small, or too short for the target) fails the same way outside it.
                sd::sandbox::SetMediaSandbox(false);
                const auto plain = runner.Run(recipe, 0, source, {});
                sd::sandbox::SetMediaSandbox(true);
                if (plain.ok)
                    std::filesystem::remove(plain.output);
                ASSERT_FALSE(plain.ok) << recipe.id << " on " << Narrow(source) << " works outside the jail but not inside: " << Narrow(r.error);
                EXPECT_EQ(Narrow(plain.error), Narrow(r.error)) << recipe.id;
                continue;
            }
            ++(tool ? toolRuns : runs);
            ASSERT_TRUE(r.ok) << recipe.id << " on " << Narrow(source) << ": " << Narrow(r.error);
            EXPECT_GT(r.size, 0u) << recipe.id;
            const auto info = engine->Probe(r.output);   // the probe is sandboxed too
            EXPECT_GT(info.durationUs, 0) << recipe.id << ": the output plays";
            std::filesystem::remove(r.output);
        }
    }
    EXPECT_GT(runs, 15);
    EXPECT_GE(toolRuns, 1) << "Normalize needs no editor";
    EXPECT_GE(sandbox::MediaJail::OpenCount() - before, runs) << "every run (and probe) went through the jail";
    EXPECT_EQ(CountFiles(dir, L".sdpart"), 0u);
}

TEST(MediaSandbox, CancelKillsAJailedFfmpeg)
{
    const auto engine = TestFfmpeg();
    if (!engine->Available())
        GTEST_SKIP() << "SHIFTDROP_FFMPEG is not set";
    TempDir dir;
    ExpectCancelLeavesNothing(engine, dir, true);
}
