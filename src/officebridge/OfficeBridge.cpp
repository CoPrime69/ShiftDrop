#include "officebridge/OfficeBridge.h"
#include "Log.h"
#include "OutputPath.h"
#include "PathUtil.h"
#include "TempPath.h"
#include "Utf.h"

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
                        if (IsFile(exe))
                            return exe;
                    }
                }
            for (const auto id : { FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86 })
            {
                wil::unique_cotaskmem_string dir;
                if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &dir)))
                {
                    auto exe = (std::filesystem::path(dir.get()) / L"LibreOffice" / L"program" / L"soffice.exe").wstring();
                    if (IsFile(exe))
                        return exe;
                }
            }
            return {};
        }

        std::wstring Quote(const std::wstring& arg)
        {
            // Our paths never end in a backslash before the closing quote, except a bare drive or
            // folder; double any trailing backslashes so the quote is not escaped.
            std::wstring q = L"\"" + arg;
            size_t slashes = 0;
            while (slashes < arg.size() && arg[arg.size() - 1 - slashes] == L'\\')
                ++slashes;
            q.append(slashes, L'\\');
            return q + L"\"";
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
        return Quote(soffice) + L" --headless --invisible --norestore --nologo --nodefault --nolockcheck --nofirststartwizard " +
               Quote(L"-env:UserInstallation=" + profileUrl) + L" --convert-to pdf --outdir " + Quote(outDir) + L" " + Quote(source);
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
        if (log::AppDataFolder().empty())
        {
            result.error = L"cannot find the local app data folder";
            return result;
        }
        const auto profile = std::filesystem::path(log::AppDataFolder()) / L"libreoffice";
        std::error_code ec;
        std::filesystem::create_directories(profile, ec);

        const TempFolder temp;   // where soffice writes the PDF; removed with it
        if (!temp.Ok())
        {
            result.error = L"cannot create a temporary folder";
            return result;
        }
        const std::filesystem::path outDir(temp.Path());

        auto commandLine = LibreOfficeCommandLine(soffice, FileUrl(profile.wstring()), outDir.wstring(), job.source);

        wil::unique_handle processJob(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!processJob || !SetInformationJobObject(processJob.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        {
            result.error = L"cannot create a job object for LibreOffice";
            return result;
        }
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        const auto programDir = std::filesystem::path(soffice).parent_path().wstring();
        if (!CreateProcessW(soffice.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED | CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, programDir.c_str(), &si, &pi))
        {
            result.error = std::format(L"cannot start LibreOffice ({})", GetLastError());
            return result;
        }
        wil::unique_handle process(pi.hProcess), thread(pi.hThread);
        if (!AssignProcessToJobObject(processJob.get(), process.get()))
        {
            TerminateProcess(process.get(), 1);
            result.error = L"cannot put LibreOffice in a job object";
            return result;
        }
        ResumeThread(thread.get());
        if (job.progress)
            job.progress(0.1);

        // soffice.exe starts soffice.bin; wait until every process in the job has ended.
        const ULONGLONG started = GetTickCount64();
        bool timedOut = false;
        for (;;)
        {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
            const bool any = QueryInformationJobObject(processJob.get(), JobObjectBasicAccountingInformation, &info, sizeof(info), nullptr)
                                 ? info.ActiveProcesses > 0
                                 : WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT;
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
            TerminateJobObject(processJob.get(), 1);
            WaitForSingleObject(process.get(), 5000);
            result.error = result.cancelled ? L"cancelled" : std::format(L"LibreOffice took longer than {} minutes", job.timeoutMs / 60000);
            return result;
        }

        const auto produced = outDir / (std::filesystem::path(job.source).stem().wstring() + L".pdf");
        if (FileSize(produced.wstring()) == 0)   // missing or empty
        {
            DWORD code = 0;
            GetExitCodeProcess(process.get(), &code);
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
