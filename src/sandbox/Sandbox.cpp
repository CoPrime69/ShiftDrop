#include "sandbox/Sandbox.h"
#include "Brand.h"
#include "Log.h"
#include "Settings.h"

#include <aclapi.h>
#include <sddl.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>

namespace sd::sandbox
{
    namespace
    {
        constexpr const wchar_t* kProfile = SD_NAME L".Sandbox";
        // What a granted image or folder may do: read and run, nothing else.
        constexpr DWORD kReadRun = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;

        // userenv's AppContainer functions are looked up at run time: they are missing on Wine and on
        // some stripped-down Windows images, and a static import would stop the app from starting there.
        using CreateProfileFn = HRESULT(WINAPI*)(PCWSTR, PCWSTR, PCWSTR, PSID_AND_ATTRIBUTES, DWORD, PSID*);
        using DeriveSidFn = HRESULT(WINAPI*)(PCWSTR, PSID*);
        using DeleteProfileFn = HRESULT(WINAPI*)(PCWSTR);

        HMODULE UserEnv()
        {
            static HMODULE module = LoadLibraryExW(L"userenv.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            return module;
        }

        template <class Fn>
        Fn UserEnvFunction(const char* name)
        {
            const HMODULE m = UserEnv();
            return m ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
        }

        std::mutex g_mutex;
        std::wstring g_workerPath;          // SetWorkerPath override
        std::atomic<int> g_archives{ -1 };  // -1 = not read from settings.ini yet
        std::atomic<int> g_media{ -1 };

        bool ReadSetting(std::atomic<int>& cache, const wchar_t* key, int fallback)
        {
            int v = cache.load();
            if (v < 0)
            {
                v = GetPrivateProfileIntW(L"Sandbox", key, fallback, SettingsPath().c_str()) != 0 ? 1 : 0;
                cache = v;
            }
            return v != 0;
        }

        // A handle list attribute must name only inheritable handles, each once.
        std::vector<HANDLE> UniqueHandles(const std::vector<HANDLE>& handles)
        {
            std::vector<HANDLE> out;
            for (HANDLE h : handles)
                if (h && h != INVALID_HANDLE_VALUE && std::find(out.begin(), out.end(), h) == out.end())
                    out.push_back(h);
            return out;
        }

        wil::unique_handle MakeJob(const Limits& limits, std::wstring& error)
        {
            wil::unique_handle job(CreateJobObjectW(nullptr, nullptr));
            if (!job)
            {
                error = std::format(L"cannot create a job object ({})", GetLastError());
                return {};
            }
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            auto& basic = info.BasicLimitInformation;
            basic.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
            if (limits.memoryBytes)
            {
                basic.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
                info.ProcessMemoryLimit = static_cast<SIZE_T>(limits.memoryBytes);
            }
            if (limits.cpuSeconds)
            {
                basic.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_TIME;
                basic.PerProcessUserTimeLimit.QuadPart = static_cast<LONGLONG>(limits.cpuSeconds) * 10'000'000;   // 100 ns units
            }
            if (limits.noChildren)
            {
                basic.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
                basic.ActiveProcessLimit = 1;
            }
            if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &info, sizeof(info)))
            {
                // Kill-on-close is the one limit we cannot run without.
                static std::once_flag once;
                std::call_once(once, [] { log::Warn(L"sandbox: job limits unsupported here ({}); using kill-on-close only", GetLastError()); });
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION minimal{};
                minimal.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &minimal, sizeof(minimal)))
                {
                    error = std::format(L"cannot set up the job object ({})", GetLastError());
                    return {};
                }
            }
            if (limits.noUi)
            {
                JOBOBJECT_BASIC_UI_RESTRICTIONS ui{ JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | JOB_OBJECT_UILIMIT_EXITWINDOWS |
                                                    JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                                                    JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD };
                SetInformationJobObject(job.get(), JobObjectBasicUIRestrictions, &ui, sizeof(ui));   // best effort
            }
            return job;
        }

        // True when the child's token really is an AppContainer token (Wine accepts the attribute
        // but does not enforce it, so the attribute alone proves nothing).
        bool IsAppContainerProcess(HANDLE process)
        {
            wil::unique_handle token;
            if (!OpenProcessToken(process, TOKEN_QUERY, &token))
                return false;
            DWORD isContainer = 0, size = 0;
            return GetTokenInformation(token.get(), TokenIsAppContainer, &isContainer, sizeof(isContainer), &size) && isContainer != 0;
        }

        bool TryCreate(const LaunchSpec& spec, bool container, PROCESS_INFORMATION& pi, DWORD& error)
        {
            const auto inherit = UniqueHandles(spec.inherit);
            DWORD count = 0;
            count += inherit.empty() ? 0 : 1;
            count += container ? 1 : 0;
            count += spec.mitigations ? 1 : 0;
            SIZE_T size = 0;
            InitializeProcThreadAttributeList(nullptr, count == 0 ? 1 : count, 0, &size);
            std::vector<char> buffer(size);
            auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
            if (!InitializeProcThreadAttributeList(attrs, count == 0 ? 1 : count, 0, &size))
            {
                error = GetLastError();
                return false;
            }
            auto cleanup = wil::scope_exit([&] { DeleteProcThreadAttributeList(attrs); });

            if (!inherit.empty() &&
                !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, const_cast<HANDLE*>(inherit.data()),
                                           inherit.size() * sizeof(HANDLE), nullptr, nullptr))
            {
                error = GetLastError();
                return false;
            }
            // No capabilities at all: no internetClient, no privateNetwork, no library folders.
            SECURITY_CAPABILITIES caps{};
            caps.AppContainerSid = AppContainer().sid;
            if (container && !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &caps, sizeof(caps), nullptr, nullptr))
            {
                error = GetLastError();
                return false;
            }
            // Defence in depth for our own worker: no injected extension DLLs (AppInit, hooks), and no
            // images from network shares or low-integrity locations.
            DWORD64 policy = PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON |
                             PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_REMOTE_ALWAYS_ON |
                             PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_LOW_LABEL_ALWAYS_ON;
            if (spec.mitigations)
                UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &policy, sizeof(policy), nullptr, nullptr);

            STARTUPINFOEXW si{};
            si.StartupInfo.cb = sizeof(si);
            if (spec.stdIn || spec.stdOut || spec.stdErr)
            {
                si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
                si.StartupInfo.hStdInput = spec.stdIn;
                si.StartupInfo.hStdOutput = spec.stdOut;
                si.StartupInfo.hStdError = spec.stdErr;
            }
            std::vector<unsigned char> fdBlock;
            if (!spec.crtFds.empty())
            {
                fdBlock = CrtFdBlock(spec.crtFds);
                si.StartupInfo.cbReserved2 = static_cast<WORD>(fdBlock.size());
                si.StartupInfo.lpReserved2 = fdBlock.data();
            }
            if (spec.hideWindow)
            {
                si.StartupInfo.dwFlags |= STARTF_USESHOWWINDOW;
                si.StartupInfo.wShowWindow = SW_HIDE;
            }
            si.lpAttributeList = attrs;

            // In the AppContainer the child's current folder is System32, which every AppContainer may
            // read (ours may be a folder it cannot open). A plain child keeps ours unless told otherwise.
            wchar_t system[MAX_PATH]{};
            GetSystemDirectoryW(system, MAX_PATH);
            const wchar_t* folder = container ? system : spec.workingDir.empty() ? nullptr : spec.workingDir.c_str();
            std::wstring commandLine = spec.commandLine;   // CreateProcessW may write to it
            if (!CreateProcessW(spec.exe.c_str(), commandLine.data(), nullptr, nullptr, inherit.empty() ? FALSE : TRUE,
                                CREATE_SUSPENDED | CREATE_NO_WINDOW | spec.priority | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                                nullptr, folder, &si.StartupInfo, &pi))
            {
                error = GetLastError();
                return false;
            }
            return true;
        }
    }

    const Container& AppContainer()
    {
        static const Container container = [] {
            Container c;
            const auto create = UserEnvFunction<CreateProfileFn>("CreateAppContainerProfile");
            const auto derive = UserEnvFunction<DeriveSidFn>("DeriveAppContainerSidFromAppContainerName");
            if (!create || !derive)
            {
                c.why = L"this Windows has no AppContainer support";
                return c;
            }
            PSID sid = nullptr;
            HRESULT hr = create(kProfile, SD_NAME L" sandbox", SD_NAME L" reads archives and media here, with no access to files or the network",
                                nullptr, 0, &sid);
            if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
                hr = derive(kProfile, &sid);
            if (FAILED(hr) || !sid)
            {
                c.why = std::format(L"cannot create the AppContainer profile (0x{:08X})", static_cast<unsigned>(hr));
                return c;
            }
            c.sid = sid;   // kept for the life of the process (FreeSid at exit would gain nothing)
            wchar_t* text = nullptr;
            if (ConvertSidToStringSidW(sid, &text))
            {
                c.sidText = text;
                LocalFree(text);
            }
            return c;
        }();
        return container;
    }

    void RemoveProfile()
    {
        if (const auto remove = UserEnvFunction<DeleteProfileFn>("DeleteAppContainerProfile"))
            remove(kProfile);
    }

    bool GrantRead(const std::wstring& path, bool inherit)
    {
        const auto& c = AppContainer();
        if (!c.Available())
            return false;
        PACL dacl = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS)
            return false;
        auto freeDescriptor = wil::scope_exit([&] { LocalFree(descriptor); });
        // Already there (an earlier run, or inherited from the folder): leave the file alone.
        if (dacl)
            for (DWORD i = 0; i < dacl->AceCount; ++i)
            {
                void* raw = nullptr;
                if (!GetAce(dacl, i, &raw))
                    continue;
                const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
                if (ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && (ace->Mask & kReadRun) == kReadRun &&
                    EqualSid(const_cast<DWORD*>(&ace->SidStart), c.sid) && (!inherit || (ace->Header.AceFlags & OBJECT_INHERIT_ACE)))
                    return true;
            }
        EXPLICIT_ACCESS_W access{};
        access.grfAccessPermissions = kReadRun;
        access.grfAccessMode = GRANT_ACCESS;
        access.grfInheritance = inherit ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        access.Trustee.ptstrName = static_cast<LPWSTR>(c.sid);
        PACL updated = nullptr;
        if (SetEntriesInAclW(1, &access, dacl, &updated) != ERROR_SUCCESS)
            return false;
        auto freeAcl = wil::scope_exit([&] { LocalFree(updated); });
        const DWORD err = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, updated, nullptr);
        if (err != ERROR_SUCCESS)
            log::Warn(L"sandbox: cannot let the sandbox read {} ({})", path, err);
        return err == ERROR_SUCCESS;
    }

    std::vector<unsigned char> CrtFdBlock(const std::vector<HANDLE>& fds)
    {
        // The layout the Microsoft C runtime reads at startup (and writes in _spawn): an int count,
        // then one flag byte per descriptor, then one HANDLE per descriptor, unaligned.
        constexpr unsigned char kOpen = 0x01;   // FOPEN; no FTEXT, so binary mode
        const int count = static_cast<int>(fds.size());
        std::vector<unsigned char> block(sizeof(int) + fds.size() * (1 + sizeof(HANDLE)));
        std::memcpy(block.data(), &count, sizeof(int));
        unsigned char* flags = block.data() + sizeof(int);
        unsigned char* handles = flags + fds.size();
        for (size_t i = 0; i < fds.size(); ++i)
        {
            const bool open = fds[i] && fds[i] != INVALID_HANDLE_VALUE;
            flags[i] = open ? kOpen : 0;
            const HANDLE h = open ? fds[i] : INVALID_HANDLE_VALUE;
            std::memcpy(handles + i * sizeof(HANDLE), &h, sizeof(HANDLE));
        }
        return block;
    }

    Process Launch(const LaunchSpec& spec)
    {
        Process p;
        const auto& c = AppContainer();
        bool container = spec.appContainer && c.Available();
        if (spec.appContainer && !c.Available())
        {
            static std::once_flag once;
            std::call_once(once, [&] { log::Warn(L"sandbox: AppContainer unavailable ({}); workers run as plain child processes in a job", c.why); });
        }
        // The image, and its folder (the loader looks there first for every DLL it loads).
        if (container && !(GrantRead(spec.exe) && GrantRead(std::filesystem::path(spec.exe).parent_path().wstring())))
        {
            log::Warn(L"sandbox: {} cannot be granted to the AppContainer; running it without the sandbox",
                      std::filesystem::path(spec.exe).filename().wstring());
            container = false;
        }

        p.job = MakeJob(spec.limits, p.error);
        if (!p.job)
            return p;

        PROCESS_INFORMATION pi{};
        DWORD error = 0;
        bool started = TryCreate(spec, container, pi, error);
        if (!started && container)
        {
            static std::once_flag once;
            std::call_once(once, [&] { log::Warn(L"sandbox: starting a process in the AppContainer failed ({}); falling back to a plain child", error); });
            container = false;
            started = TryCreate(spec, false, pi, error);
        }
        if (!started)
        {
            p.error = std::format(L"cannot start {} ({})", std::filesystem::path(spec.exe).filename().wstring(), error);
            return p;
        }
        wil::unique_handle process(pi.hProcess), thread(pi.hThread);
        if (!AssignProcessToJobObject(p.job.get(), process.get()))
        {
            TerminateProcess(process.get(), 1);
            p.error = std::format(L"cannot put the process in a job object ({})", GetLastError());
            return p;
        }
        p.sandboxed = container && IsAppContainerProcess(process.get());
        if (container && !p.sandboxed)
        {
            static std::once_flag once;
            std::call_once(once, [] { log::Warn(L"sandbox: the AppContainer is not enforced on this system; workers run unconfined"); });
        }
        ResumeThread(thread.get());
        p.process = std::move(process);
        return p;
    }

    std::wstring WorkerPath()
    {
        {
            std::scoped_lock lock(g_mutex);
            if (!g_workerPath.empty())
                return g_workerPath;
        }
        wchar_t env[MAX_PATH]{};
        if (const DWORD n = GetEnvironmentVariableW(L"SHIFTDROP_WORKER", env, MAX_PATH); n > 0 && n < MAX_PATH)
            return GetFileAttributesW(env) != INVALID_FILE_ATTRIBUTES ? std::wstring(env) : std::wstring();
        wchar_t self[MAX_PATH]{};
        const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
        if (n == 0 || n == MAX_PATH)
            return {};
        const auto exe = std::filesystem::path(self).parent_path() / L"engines" / (SD_NAME L"Worker.exe");
        return GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES ? exe.wstring() : std::wstring();
    }

    void SetWorkerPath(std::wstring path)
    {
        std::scoped_lock lock(g_mutex);
        g_workerPath = std::move(path);
    }

    bool ArchiveSandboxEnabled() { return ReadSetting(g_archives, L"Archives", 1); }
    bool MediaSandboxEnabled() { return ReadSetting(g_media, L"Media", 0); }
    void SetMediaSandbox(bool on) { g_media = on ? 1 : 0; }
    void SetArchiveSandbox(bool on) { g_archives = on ? 1 : 0; }
}
