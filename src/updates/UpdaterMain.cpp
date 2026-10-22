// ShiftDrop-Updater.exe: checks for, downloads and installs a new release. The one program of an
// install that goes online (ShiftDrop.exe and the archive worker import no networking at all, which
// scripts/check-imports.ps1 checks). ShiftDrop starts it; it has no window, does one job per run,
// writes what happened to update.json (updates::State) and exits with an updates::Result.
#include "Brand.h"
#include "FileIo.h"
#include "Hash.h"
#include "Log.h"
#include "PathUtil.h"
#include "Settings.h"
#include "Strings.h"
#include "TempPath.h"
#include "updates/Http.h"
#include "updates/Updates.h"
#include "Utf.h"

#include <windows.h>
#include <shellapi.h>
#include <wil/resource.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

namespace sd::updates
{
    namespace
    {
        constexpr uint64_t kMaxJson = 2u << 20, kMaxSums = 64u << 10, kMaxSetup = 512ull << 20;

        int64_t Now()
        {
            return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        std::wstring NowLocal()
        {
            SYSTEMTIME t{};
            GetLocalTime(&t);
            return std::format(L"{:04}-{:02}-{:02} {:02}:{:02}:{:02}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        }

        int Code(Result r)
        {
            return static_cast<int>(r);
        }

        // [Updates] TestFeed: a release JSON on this PC stands in for GitHub's, and its assets may be
        // files on this PC. Only for testing an update before a real one is published.
        struct Source
        {
            std::wstring feed;
            bool Testing() const { return !feed.empty(); }
        };

        bool IsLocalFile(std::wstring_view path)
        {
            return path.size() > 3 && path[1] == L':' && path[2] == L'\\';
        }

        // A file the release names: one of its downloads, or with a test feed a file on this PC.
        bool Fetch(const Source& source, const std::wstring& url, const std::wstring& path, uint64_t maxBytes, std::wstring& error)
        {
            if (source.Testing() && IsLocalFile(url))
            {
                if (!CopyFileW(url.c_str(), path.c_str(), FALSE))
                {
                    error = std::format(L"Couldn’t copy {} ({})", url, GetLastError());
                    return false;
                }
                return true;
            }
            if (!IsReleaseDownload(url))
            {
                error = L"The release points somewhere other than its GitHub downloads";
                return false;
            }
            return http::Download(url, path, maxBytes, error);
        }

        bool FetchText(const Source& source, const std::wstring& url, uint64_t maxBytes, std::string& text, std::wstring& error)
        {
            if (source.Testing() && IsLocalFile(url))
                return ReadFileBytes(url, text, &error, maxBytes);
            if (!IsReleaseDownload(url))
            {
                error = L"The release points somewhere other than its GitHub downloads";
                return false;
            }
            return http::Get(url, L"text/plain", maxBytes, text, error);
        }

        // ---- --check ----------------------------------------------------------------------------------

        // %TEMP%\ShiftDrop-update: the downloads, and the copy of this exe that ran Setup.
        std::wstring DownloadRoot()
        {
            return TempDirectory() + L"" SD_NAME L"-update";
        }

        Result Check(const Source& source, State& state, bool manual)
        {
            // What an earlier update left (that run has long finished: ShiftDrop started this one).
            if (state.outcome != Outcome::Downloaded)
            {
                std::error_code ec;
                std::filesystem::remove_all(DownloadRoot(), ec);
            }
            state = {};
            state.checked = Now();
            state.checkedLocal = NowLocal();
            std::string json;
            std::wstring error;
            bool got = false;
            if (source.Testing())
            {
                log::Info(L"updater: reading the test feed {} instead of GitHub", source.feed);
                got = ReadFileBytes(source.feed, json, &error, kMaxJson);
            }
            else
            {
                log::Info(L"updater: GET {}", kLatestUrl);
                got = http::Get(kLatestUrl, L"application/vnd.github+json", kMaxJson, json, error);
            }
            const auto release = got ? ParseRelease(json, &error) : std::nullopt;
            if (!release)
            {
                log::Warn(L"updater: check failed: {}", error);
                state.outcome = Outcome::CheckFailed;
                state.error = error;
                SaveState(state, StatePath());
                return Result::Failed;
            }
            state.hasRelease = true;
            state.release = *release;
            const auto verdict = Judge(RunningBase(), *release, state.checked, manual);
            state.outcome = verdict == Verdict::Available ? Outcome::Available : verdict == Verdict::Settling ? Outcome::Settling : Outcome::UpToDate;
            log::Info(L"updater: latest is {} (published {} days ago); this is {}: {}", ToText(release->version), (state.checked - release->published) / 86400,
                      RunningVersion(), ResultLine(state, RunningBase()));
            SaveState(state, StatePath());
            return verdict == Verdict::Available ? Result::Available : Result::Ok;
        }

        // ---- --download -------------------------------------------------------------------------------

        Result Download(const Source& source, State& state)
        {
            const auto fail = [&](std::wstring why) {
                log::Warn(L"updater: download failed: {}", why);
                state.outcome = Outcome::DownloadFailed;
                state.error = std::move(why);
                state.setupPath.clear();
                state.setupSha256.clear();
                SaveState(state, StatePath());
                return Result::Failed;
            };
            if (!HasUpdate(state, RunningBase()))
                return fail(L"There is no newer version to download");
            const auto& r = state.release;
            const std::wstring setupName = SetupName(r.version);
            if (r.setup.url.empty())
                return fail(std::format(L"The release has no {}", setupName));
            if (r.sums.url.empty())
                return fail(std::format(L"The release has no {}", kSumsName));

            // %TEMP%\ShiftDrop-update\<version>, emptied of anything an earlier try left.
            const std::wstring root = DownloadRoot(), folder = root + L"\\" + ToText(r.version);
            CreateDirectoryW(root.c_str(), nullptr);
            CreateDirectoryW(folder.c_str(), nullptr);
            const std::wstring setup = folder + L"\\" + setupName, part = setup + L".part";
            DeleteFileW(setup.c_str());
            DeleteFileW(part.c_str());

            std::wstring error;
            std::string sums;
            log::Info(L"updater: GET {}", r.sums.url);
            if (!FetchText(source, r.sums.url, kMaxSums, sums, error))
                return fail(error);
            const auto expected = FindSum(sums, setupName);
            if (!expected)
                return fail(std::format(L"{} doesn’t list {}", kSumsName, setupName));
            log::Info(L"updater: GET {} ({} bytes)", r.setup.url, r.setup.size);
            if (!Fetch(source, r.setup.url, part, kMaxSetup, error))
                return fail(error);
            uint64_t size = 0;
            const std::string actual = Sha256File(part, &size);
            // Refused unless it is exactly the file SHA256SUMS.txt (and GitHub's own digest, when it
            // gives one) says.
            if (!SameSum(actual, *expected) || (!r.setup.sha256.empty() && !SameSum(actual, r.setup.sha256)) || (r.setup.size && size != r.setup.size))
            {
                DeleteFileW(part.c_str());
                return fail(std::format(L"The download doesn’t match its SHA-256 in {}", kSumsName));
            }
            if (!MoveFileExW(part.c_str(), setup.c_str(), MOVEFILE_REPLACE_EXISTING))
                return fail(std::format(L"Couldn’t keep the download ({})", GetLastError()));
            // Setup replaces this exe in the install folder, so --install runs from a copy next to it.
            const std::wstring self = folder + L"\\" + kUpdaterExe;
            if (!CopyFileW(ThisExe().c_str(), self.c_str(), FALSE))
                return fail(std::format(L"Couldn’t prepare the update ({})", GetLastError()));
            state.outcome = Outcome::Downloaded;
            state.error.clear();
            state.setupPath = setup;
            state.setupSha256 = actual;
            log::Info(L"updater: {} downloaded and verified (SHA-256 {})", setupName, Widen(actual));
            SaveState(state, StatePath());
            return Result::Ok;
        }

        // ---- --install --------------------------------------------------------------------------------

        bool Start(const std::wstring& exe, const std::wstring& args, PROCESS_INFORMATION& pi)
        {
            std::wstring line = Quote(exe) + (args.empty() ? L"" : L" " + args);
            STARTUPINFOW si{ sizeof(si) };
            return CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi) != FALSE;
        }

        void StartApp(const std::wstring& app)
        {
            PROCESS_INFORMATION pi{};
            if (Start(app, L"", pi))
            {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
            else
                log::Error(L"updater: couldn't start {} ({})", app, GetLastError());
        }

        Result Install(State& state, const std::wstring& setup, DWORD pid, const std::wstring& app)
        {
            const std::wstring from = ToText(RunningBase());
            const auto finish = [&](Outcome outcome, std::wstring why) {
                state.outcome = outcome;
                state.error = std::move(why);
                state.fromVersion = from;
                SaveState(state, StatePath());
            };
            // ShiftDrop quits itself once no job is running; Setup can only replace its files after that.
            if (wil::unique_handle process{ OpenProcess(SYNCHRONIZE, FALSE, pid) })
                if (WaitForSingleObject(process.get(), 120000) != WAIT_OBJECT_0)
                {
                    log::Warn(L"updater: ShiftDrop (pid {}) is still running; not installing", pid);
                    finish(Outcome::InstallFailed, L"ShiftDrop didn’t close");
                    return Result::Failed;   // it is still running: nothing to start again
                }
            // Only the installer this updater downloaded and checked, unchanged since.
            if (!EqualsNoCase(setup, state.setupPath) || !SameSum(Sha256File(setup), state.setupSha256))
            {
                log::Warn(L"updater: {} is not the verified download; not installing", setup);
                finish(Outcome::InstallFailed, L"The download changed after it was checked");
                StartApp(app);
                return Result::Failed;
            }
            const std::wstring logPath = setup + L".log";
            log::Info(L"updater: running {} {}", setup, SetupArgs(logPath));
            PROCESS_INFORMATION pi{};
            DWORD code = 1;
            if (!Start(setup, SetupArgs(logPath), pi))
                log::Error(L"updater: couldn't start Setup ({})", GetLastError());
            else
            {
                wil::unique_handle process(pi.hProcess), thread(pi.hThread);
                if (WaitForSingleObject(process.get(), 15 * 60 * 1000) != WAIT_OBJECT_0)
                    log::Error(L"updater: Setup is still running after 15 minutes");
                else
                    GetExitCodeProcess(process.get(), &code);
            }
            const std::wstring version = state.hasRelease ? ToText(state.release.version) : L"";
            if (code == 0)
            {
                log::Info(L"updater: {} installed over {}", version, from);
                finish(Outcome::Installed, L"");
            }
            else
            {
                // Setup puts every file back when it fails, so the old ShiftDrop is still there.
                log::Error(L"updater: Setup failed with code {} (its log: {})", code, logPath);
                finish(Outcome::InstallFailed, std::format(L"Setup ended with code {}", code));
            }
            StartApp(app);   // the new one, or the old one again
            return code == 0 ? Result::Ok : Result::Failed;
        }

        // The value after `name` on the command line, or empty.
        std::wstring After(const std::vector<std::wstring>& args, const wchar_t* name)
        {
            for (size_t i = 0; i + 1 < args.size(); ++i)
                if (args[i] == name)
                    return args[i + 1];
            return {};
        }

        bool Has(const std::vector<std::wstring>& args, const wchar_t* name)
        {
            for (const auto& a : args)
                if (a == name)
                    return true;
            return false;
        }

        int Main()
        {
            int argc = 0;
            wil::unique_hlocal_ptr<PWSTR> argv(CommandLineToArgvW(GetCommandLineW(), &argc));
            std::vector<std::wstring> args;
            for (int i = 1; argv && i < argc; ++i)
                args.emplace_back(argv.get()[i]);
            log::Init();
            const Source source{ LoadTestFeed(SettingsPath()) };
            State state = LoadState(StatePath());
            if (Has(args, kInstallArg))
            {
                const std::wstring setup = After(args, kInstallArg), app = After(args, kAppArg), pid = After(args, kWaitArg);
                if (setup.empty() || app.empty() || pid.empty())
                    return Code(Result::BadArgs);
                return Code(Install(state, setup, static_cast<DWORD>(wcstoul(pid.c_str(), nullptr, 10)), app));
            }
            if (Has(args, kCheckArg))
            {
                const Result found = Check(source, state, Has(args, kManualArg));
                if (found != Result::Available || !Has(args, kDownloadArg))
                    return Code(found);
                return Code(Download(source, state) == Result::Ok ? Result::Available : Result::Failed);
            }
            if (Has(args, kDownloadArg))
                return Code(Download(source, state));
            return Code(Result::BadArgs);
        }
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    return sd::updates::Main();
}
