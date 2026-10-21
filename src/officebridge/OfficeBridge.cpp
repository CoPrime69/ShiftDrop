#include "officebridge/OfficeBridge.h"
#include "Media.h"
#include "OutputPath.h"
#include "Utf.h"
#include "sandbox/Sandbox.h"

#include <objbase.h>
#include <shlobj.h>
#include <wil/resource.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <format>

namespace sd::office
{
    namespace
    {
        bool FileExists(const std::wstring& path)
        {
            const DWORD a = GetFileAttributesW(path.c_str());
            return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
        }

        std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* value, DWORD view)
        {
            wchar_t buffer[MAX_PATH * 2]{};
            DWORD size = sizeof(buffer);
            if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | view, nullptr, buffer, &size) != ERROR_SUCCESS)
                return {};
            return buffer;
        }

        // Is `progId` registered with an out-of-process server (an installed Office app)?
        bool HasLocalServer(const wchar_t* progId)
        {
            CLSID clsid{};
            if (FAILED(CLSIDFromProgID(progId, &clsid)))
                return false;
            wchar_t guid[64]{};
            StringFromGUID2(clsid, guid, 64);
            const auto key = std::format(L"CLSID\\{}\\LocalServer32", guid);
            HKEY h = nullptr;
            for (const DWORD view : { KEY_WOW64_64KEY, KEY_WOW64_32KEY })
                if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key.c_str(), 0, KEY_READ | view, &h) == ERROR_SUCCESS)
                {
                    RegCloseKey(h);
                    return true;
                }
            return false;
        }

        std::wstring FindSoffice()
        {
            // The installer records the program folder (".../LibreOffice/program") here.
            for (const HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER })
                for (const DWORD view : { RRF_SUBKEY_WOW6464KEY, RRF_SUBKEY_WOW6432KEY })
                {
                    auto dir = RegString(root, L"SOFTWARE\\LibreOffice\\UNO\\InstallPath", nullptr, view);
                    if (!dir.empty())
                    {
                        auto exe = (std::filesystem::path(dir) / L"soffice.exe").wstring();
                        if (FileExists(exe))
                            return exe;
                    }
                }
            for (const auto id : { FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86 })
            {
                wil::unique_cotaskmem_string dir;
                if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &dir)))
                {
                    auto exe = (std::filesystem::path(dir.get()) / L"LibreOffice" / L"program" / L"soffice.exe").wstring();
                    if (FileExists(exe))
                        return exe;
                }
            }
            return {};
        }

    }

    Family FamilyOf(std::wstring_view path)
    {
        const auto ext = LowerExtension(path);
        for (const wchar_t* e : { L".doc", L".docx", L".docm", L".dotx", L".dotm", L".odt", L".rtf" })
            if (ext == e)
                return Family::Word;
        for (const wchar_t* e : { L".xls", L".xlsx", L".xlsm", L".xlsb", L".ods" })
            if (ext == e)
                return Family::Excel;
        for (const wchar_t* e : { L".ppt", L".pptx", L".pptm", L".pps", L".ppsx", L".odp" })
            if (ext == e)
                return Family::PowerPoint;
        return Family::None;
    }

    Apps DetectApps()
    {
        Apps apps;
        apps.word = HasLocalServer(L"Word.Application");
        apps.excel = HasLocalServer(L"Excel.Application");
        apps.powerpoint = HasLocalServer(L"PowerPoint.Application");
        apps.soffice = FindSoffice();
        return apps;
    }

    Backend Choose(const Apps& apps, Family family, std::string_view target)
    {
        if (target == "png" || target == "jpg")
            return family == Family::PowerPoint && apps.powerpoint ? Backend::PowerPoint : Backend::None;
        if (target != "pdf" || family == Family::None)
            return Backend::None;
        if (family == Family::Word && apps.word)
            return Backend::Word;
        if (family == Family::Excel && apps.excel)
            return Backend::Excel;
        if (family == Family::PowerPoint && apps.powerpoint)
            return Backend::PowerPoint;
        return apps.soffice.empty() ? Backend::None : Backend::LibreOffice;
    }

    const wchar_t* BackendName(Backend backend)
    {
        switch (backend)
        {
        case Backend::Word: return L"Word";
        case Backend::Excel: return L"Excel";
        case Backend::PowerPoint: return L"PowerPoint";
        case Backend::LibreOffice: return L"LibreOffice";
        default: return L"no office app";
        }
    }

    std::wstring FileUrl(std::wstring_view path)
    {
        std::wstring p(path);
        std::replace(p.begin(), p.end(), L'\\', L'/');
        std::wstring prefix = L"file:///";
        if (p.starts_with(L"//"))
        {
            prefix = L"file:";
        }
        std::wstring out = prefix;
        for (const unsigned char c : Narrow(p))
        {
            if (std::isalnum(c) || c == '/' || c == ':' || c == '-' || c == '.' || c == '_' || c == '~')
                out += static_cast<wchar_t>(c);
            else
                out += std::format(L"%{:02X}", static_cast<unsigned>(c));
        }
        return out;
    }

    std::wstring LibreOfficeCommandLine(const std::wstring& soffice, const std::wstring& profileUrl, const std::wstring& outDir,
                                        const std::wstring& source)
    {
        return QuoteArg(soffice) + L" --headless --invisible --norestore --nologo --nodefault --nolockcheck --nofirststartwizard " +
               QuoteArg(L"-env:UserInstallation=" + profileUrl) + L" --convert-to pdf --outdir " + QuoteArg(outDir) + L" " + QuoteArg(source);
    }

    std::wstring SlideFileName(int index, int count, std::wstring_view extension)
    {
        const int width = std::max(2, static_cast<int>(std::to_wstring(std::max(count, 1)).size()));
        return std::format(L"Slide {:0{}}{}", index, width, extension);
    }

    SIZE SlidePixels(float slideWidth, float slideHeight)
    {
        if (!(slideWidth > 0) || !(slideHeight > 0))
            return { 1920, 1080 };
        constexpr float kLong = 1920.f;
        if (slideWidth >= slideHeight)
            return { static_cast<LONG>(kLong), std::max<LONG>(1, std::lround(kLong * slideHeight / slideWidth)) };
        return { std::max<LONG>(1, std::lround(kLong * slideWidth / slideHeight)), static_cast<LONG>(kLong) };
    }

    Outcome ConvertWithLibreOffice(const std::wstring& soffice, const Job& job)
    {
        Outcome result;
        // Our own profile: LibreOffice hands a conversion to an instance already running on the
        // same profile, which then ignores --headless and may never write the file.
        wil::unique_cotaskmem_string local;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local)))
        {
            result.error = L"cannot find the local app data folder";
            return result;
        }
        const auto profile = std::filesystem::path(local.get()) / L"ShiftDrop" / L"libreoffice";
        std::error_code ec;
        std::filesystem::create_directories(profile, ec);

        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        GUID g{};
        CoCreateGuid(&g);
        const auto outDir = std::filesystem::path(temp) / std::format(L"ShiftDrop-office-{:08x}{:04x}", g.Data1, g.Data2);
        if (!CreateDirectoryW(outDir.c_str(), nullptr))
        {
            result.error = L"cannot create a temporary folder";
            return result;
        }
        auto removeOut = wil::scope_exit([&] { std::filesystem::remove_all(outDir, ec); });

        // A plain child (LibreOffice needs its profile and the user's file) in a kill-on-close job, so
        // it never outlives us. soffice.exe starts soffice.bin, and it needs its program folder as the
        // current folder and its windows hidden.
        sandbox::LaunchSpec spec;
        spec.exe = soffice;
        spec.commandLine = LibreOfficeCommandLine(soffice, FileUrl(profile.wstring()), outDir.wstring(), job.source);
        spec.appContainer = false;
        spec.limits.noChildren = false;
        spec.limits.noUi = false;
        spec.workingDir = std::filesystem::path(soffice).parent_path().wstring();
        spec.hideWindow = true;
        auto launched = sandbox::Launch(spec);
        if (!launched)
        {
            result.error = launched.error;
            return result;
        }
        const HANDLE process = launched.process.get();
        const HANDLE processJob = launched.job.get();
        if (job.progress)
            job.progress(0.1);

        // soffice.exe starts soffice.bin; wait until every process in the job has ended.
        const ULONGLONG started = GetTickCount64();
        bool timedOut = false;
        for (;;)
        {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
            const bool any = QueryInformationJobObject(processJob, JobObjectBasicAccountingInformation, &info, sizeof(info), nullptr)
                                 ? info.ActiveProcesses > 0
                                 : WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
            if (!any)
                break;
            if (job.stop.stop_requested())
            {
                result.cancelled = true;
                break;
            }
            if (GetTickCount64() - started > job.timeoutMs)
            {
                timedOut = true;
                break;
            }
            Sleep(100);
        }
        if (result.cancelled || timedOut)
        {
            TerminateJobObject(processJob, 1);
            WaitForSingleObject(process, 5000);
            result.error = result.cancelled ? L"cancelled" : std::format(L"LibreOffice took longer than {} minutes", job.timeoutMs / 60000);
            return result;
        }

        const auto produced = outDir / (std::filesystem::path(job.source).stem().wstring() + L".pdf");
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(produced.c_str(), GetFileExInfoStandard, &data) || (data.nFileSizeLow == 0 && data.nFileSizeHigh == 0))
        {
            DWORD code = 0;
            GetExitCodeProcess(process, &code);
            result.error = std::format(L"LibreOffice could not convert the file (exit code {})", code);
            return result;
        }
        // Into our staging file (which we created and own).
        if (!CopyFileW(produced.c_str(), job.output.c_str(), FALSE))
        {
            result.error = std::format(L"cannot write the PDF (error {})", GetLastError());
            return result;
        }
        if (job.progress)
            job.progress(1.0);
        result.ok = true;
        return result;
    }
}
