#include "officebridge/OfficeBridge.h"

#include <ole2.h>
#include <oleauto.h>
#include <tlhelp32.h>
#include <wil/resource.h>
#include <wrl/client.h>

#include <atomic>
#include <filesystem>
#include <format>
#include <set>
#include <thread>
#include <vector>

// Microsoft Office through late-bound IDispatch: no type libraries, no Office headers, so this
// builds and runs whether Office is there or not. Every call uses named arguments, so the
// argument order of each Office version doesn't matter.
namespace sd::office
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        // A VARIANT that clears itself.
        struct Var : VARIANT
        {
            Var() { VariantInit(this); }
            explicit Var(bool b) : Var() { vt = VT_BOOL, boolVal = b ? VARIANT_TRUE : VARIANT_FALSE; }
            explicit Var(int i) : Var() { vt = VT_I4, lVal = i; }
            explicit Var(const std::wstring& s) : Var() { vt = VT_BSTR, bstrVal = SysAllocString(s.c_str()); }
            Var(const Var&) = delete;
            Var& operator=(const Var&) = delete;
            Var(Var&& o) noexcept : Var() { *static_cast<VARIANT*>(this) = o, VariantInit(&o); }
            ~Var() { VariantClear(this); }

            ComPtr<IDispatch> Dispatch() const
            {
                ComPtr<IDispatch> d;
                if (vt == VT_DISPATCH && pdispVal)
                    d = pdispVal;
                return d;
            }
            double Number() const
            {
                Var copy;
                return SUCCEEDED(VariantChangeType(&copy, const_cast<Var*>(this), 0, VT_R8)) ? copy.dblVal : 0.0;
            }
        };

        struct Named
        {
            const wchar_t* name;
            Var value;
        };

        // Calls `member` on `object`. Positional arguments are given in their natural order.
        HRESULT Invoke(IDispatch* object, const wchar_t* member, WORD flags, std::vector<Named> named, std::vector<Var> positional,
                       Var* result, std::wstring& error)
        {
            if (!object)
            {
                error = std::format(L"{}: no object", member);
                return E_POINTER;
            }
            std::vector<LPOLESTR> names{ const_cast<LPOLESTR>(member) };
            for (const auto& n : named)
                names.push_back(const_cast<LPOLESTR>(n.name));
            std::vector<DISPID> ids(names.size());
            HRESULT hr = object->GetIDsOfNames(IID_NULL, names.data(), static_cast<UINT>(names.size()), LOCALE_USER_DEFAULT, ids.data());
            if (FAILED(hr))
            {
                error = std::format(L"{}: not available in this version ({:08X})", member, static_cast<unsigned>(hr));
                return hr;
            }

            // rgvarg holds the named arguments first (matching rgdispidNamedArgs), then the
            // positional ones last to first.
            std::vector<VARIANTARG> args;
            std::vector<DISPID> namedIds;
            for (size_t i = 0; i < named.size(); ++i)
            {
                args.push_back(named[i].value);
                namedIds.push_back(ids[i + 1]);
            }
            for (auto it = positional.rbegin(); it != positional.rend(); ++it)
                args.push_back(*it);
            DISPID put = DISPID_PROPERTYPUT;
            DISPPARAMS params{ args.empty() ? nullptr : args.data(), namedIds.empty() ? nullptr : namedIds.data(), static_cast<UINT>(args.size()),
                               static_cast<UINT>(namedIds.size()) };
            if (flags & DISPATCH_PROPERTYPUT)
            {
                params.rgdispidNamedArgs = &put;
                params.cNamedArgs = 1;
            }
            EXCEPINFO info{};
            UINT badArg = 0;
            Var ignored;
            hr = object->Invoke(ids[0], IID_NULL, LOCALE_USER_DEFAULT, flags, &params, result ? result : &ignored, &info, &badArg);
            if (FAILED(hr))
            {
                if (hr == DISP_E_EXCEPTION && info.pfnDeferredFillIn)
                    info.pfnDeferredFillIn(&info);
                std::wstring text = info.bstrDescription ? info.bstrDescription : L"";
                while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
                    text.pop_back();
                error = text.empty() ? std::format(L"{} failed ({:08X})", member, static_cast<unsigned>(hr == DISP_E_EXCEPTION && info.scode ? info.scode : hr))
                                     : text;
                if (hr == RPC_E_CALL_CANCELED || hr == RPC_E_DISCONNECTED || hr == RPC_S_SERVER_UNAVAILABLE || hr == RPC_E_SERVERFAULT)
                    error = L"the office app stopped responding";
            }
            SysFreeString(info.bstrSource);
            SysFreeString(info.bstrDescription);
            SysFreeString(info.bstrHelpFile);
            return hr;
        }

        template <class... T>
        std::vector<Named> NamedArgs(T&&... pairs)
        {
            std::vector<Named> v;
            (v.push_back(std::forward<T>(pairs)), ...);
            return v;
        }

        Named N(const wchar_t* name, bool b) { return Named{ name, Var(b) }; }
        Named N(const wchar_t* name, int i) { return Named{ name, Var(i) }; }
        Named N(const wchar_t* name, const std::wstring& s) { return Named{ name, Var(s) }; }

        ComPtr<IDispatch> Get(IDispatch* object, const wchar_t* property, std::wstring& error)
        {
            Var v;
            if (FAILED(Invoke(object, property, DISPATCH_PROPERTYGET, {}, {}, &v, error)))
                return nullptr;
            auto d = v.Dispatch();
            if (!d)
                error = std::format(L"{}: no object", property);
            return d;
        }

        double GetNumber(IDispatch* object, const wchar_t* property, std::wstring& error)
        {
            Var v;
            if (FAILED(Invoke(object, property, DISPATCH_PROPERTYGET, {}, {}, &v, error)))
                return 0;
            return v.Number();
        }

        // Settings that some versions lack: failures are ignored.
        void TryPut(IDispatch* object, const wchar_t* property, Var value)
        {
            std::wstring ignored;
            std::vector<Var> v;
            v.push_back(std::move(value));
            Invoke(object, property, DISPATCH_PROPERTYPUT, {}, std::move(v), nullptr, ignored);
        }

        ComPtr<IDispatch> Call(IDispatch* object, const wchar_t* method, std::vector<Named> named, std::wstring& error, bool* failed = nullptr)
        {
            Var v;
            const HRESULT hr = Invoke(object, method, DISPATCH_METHOD, std::move(named), {}, &v, error);
            if (failed)
                *failed = FAILED(hr);
            return SUCCEEDED(hr) ? v.Dispatch() : nullptr;
        }

        std::set<DWORD> ProcessIds(const wchar_t* exe)
        {
            std::set<DWORD> ids;
            wil::unique_handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
            if (!snap || snap.get() == INVALID_HANDLE_VALUE)
                return ids;
            PROCESSENTRY32W e{ sizeof(e) };
            for (BOOL ok = Process32FirstW(snap.get(), &e); ok; ok = Process32NextW(snap.get(), &e))
                if (_wcsicmp(e.szExeFile, exe) == 0)
                    ids.insert(e.th32ProcessID);
            return ids;
        }

        // "Application is busy" rejections are retried for a while instead of failing at once.
        class RetryFilter final : public IMessageFilter
        {
        public:
            STDMETHODIMP QueryInterface(REFIID riid, void** out) override
            {
                if (riid == IID_IUnknown || riid == IID_IMessageFilter)
                {
                    *out = static_cast<IMessageFilter*>(this);
                    return S_OK;
                }
                *out = nullptr;
                return E_NOINTERFACE;
            }
            STDMETHODIMP_(ULONG) AddRef() override { return 2; }
            STDMETHODIMP_(ULONG) Release() override { return 1; }
            STDMETHODIMP_(DWORD) HandleInComingCall(DWORD, HTASK, DWORD, LPINTERFACEINFO) override { return SERVERCALL_ISHANDLED; }
            STDMETHODIMP_(DWORD) RetryRejectedCall(HTASK, DWORD elapsed, DWORD type) override
            {
                return type == SERVERCALL_RETRYLATER && elapsed < 60000 ? 250 : static_cast<DWORD>(-1);
            }
            STDMETHODIMP_(DWORD) MessagePending(HTASK, DWORD, DWORD) override { return PENDINGMSG_WAITDEFPROCESS; }
        };

        // Watches the conversion from another thread: on cancel or when no progress was made for
        // the timeout, it cancels the blocked COM call and ends the app processes we started (an
        // invisible app stuck on a dialog would otherwise never return).
        class Watchdog
        {
        public:
            Watchdog(const Job& job, std::set<DWORD> ours)
                : m_ours(std::move(ours)), m_thread(GetCurrentThreadId()), m_timeout(job.timeoutMs), m_deadline(GetTickCount64() + job.timeoutMs)
            {
                CoEnableCallCancellation(nullptr);
                m_worker = std::jthread([this, stop = job.stop](std::stop_token self) {
                    while (!self.stop_requested())
                    {
                        if (stop.stop_requested() || GetTickCount64() > m_deadline.load())
                        {
                            m_fired = stop.stop_requested() ? 1 : 2;
                            Kill();
                            return;
                        }
                        Sleep(100);
                    }
                });
            }
            ~Watchdog()
            {
                m_worker.request_stop();
                m_worker.join();
                CoDisableCallCancellation(nullptr);
            }
            void Progress() { m_deadline = GetTickCount64() + m_timeout; }
            bool Fired() const { return m_fired != 0; }
            bool Cancelled() const { return m_fired == 1; }
            // When the app is ours and still running after we're done, end it.
            void Kill()
            {
                CoCancelCall(m_thread, 0);
                for (const DWORD pid : m_ours)
                    if (wil::unique_handle p{ OpenProcess(PROCESS_TERMINATE, FALSE, pid) })
                        TerminateProcess(p.get(), 1);
            }

        private:
            std::set<DWORD> m_ours;
            DWORD m_thread;
            DWORD m_timeout;
            std::atomic<ULONGLONG> m_deadline;
            std::atomic<int> m_fired{ 0 };
            std::jthread m_worker;
        };

        struct AppInfo
        {
            const wchar_t* progId;
            const wchar_t* exe;
        };
        AppInfo InfoOf(Backend app)
        {
            switch (app)
            {
            case Backend::Word: return { L"Word.Application", L"WINWORD.EXE" };
            case Backend::Excel: return { L"Excel.Application", L"EXCEL.EXE" };
            default: return { L"PowerPoint.Application", L"POWERPNT.EXE" };
            }
        }

        std::wstring TempPdf()
        {
            wchar_t dir[MAX_PATH]{};
            GetTempPathW(MAX_PATH, dir);
            GUID g{};
            CoCreateGuid(&g);
            return (std::filesystem::path(dir) / std::format(L"ShiftDrop-office-{:08x}{:04x}.pdf", g.Data1, g.Data2)).wstring();
        }

        // Passed as the password: a protected file then fails to open instead of asking for one
        // (an invisible app would wait for an answer forever). Unprotected files ignore it.
        const std::wstring kNoPassword = L"ShiftDrop: no password";
    }

    Outcome ConvertWithOffice(Backend backend, const Job& job)
    {
        Outcome result;
        const auto info = InfoOf(backend);
        CLSID clsid{};
        if (FAILED(CLSIDFromProgID(info.progId, &clsid)))
        {
            result.error = std::format(L"{} is not installed", BackendName(backend));
            return result;
        }

        RetryFilter filter;
        IMessageFilter* previous = nullptr;
        CoRegisterMessageFilter(&filter, &previous);
        auto restoreFilter = wil::scope_exit([&] {
            IMessageFilter* ours = nullptr;
            CoRegisterMessageFilter(previous, &ours);
        });

        const auto before = ProcessIds(info.exe);
        ComPtr<IDispatch> app;
        if (const HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&app)); FAILED(hr))
        {
            result.error = std::format(L"cannot start {} ({:08X})", BackendName(backend), static_cast<unsigned>(hr));
            return result;
        }
        std::set<DWORD> ours;
        for (const DWORD pid : ProcessIds(info.exe))
            if (!before.contains(pid))
                ours.insert(pid);
        // PowerPoint is single-instance: when it was already open we share the user's instance,
        // so we close only our presentation and leave the app running.
        const bool started = !ours.empty() || before.empty();
        Watchdog watchdog(job, ours);
        if (job.progress)
            job.progress(0.1);

        std::wstring error;
        const auto finish = [&](bool ok) {
            if (watchdog.Fired())
            {
                result.cancelled = watchdog.Cancelled();
                result.error = result.cancelled ? L"cancelled" : std::format(L"{} took longer than {} minutes", BackendName(backend), job.timeoutMs / 60000);
                watchdog.Kill();
            }
            else
            {
                result.ok = ok;
                if (!ok)
                    result.error = error.empty() ? std::format(L"{} could not convert the file", BackendName(backend)) : error;
            }
            return result;
        };
        const auto copyOut = [&](const std::wstring& temp) {
            if (!CopyFileW(temp.c_str(), job.output.c_str(), FALSE))
            {
                error = std::format(L"cannot write the PDF (error {})", GetLastError());
                return false;
            }
            return true;
        };

        if (backend == Backend::Word)
        {
            TryPut(app.Get(), L"Visible", Var(false));
            TryPut(app.Get(), L"DisplayAlerts", Var(0));        // wdAlertsNone
            TryPut(app.Get(), L"AutomationSecurity", Var(3));   // msoAutomationSecurityForceDisable
            TryPut(app.Get(), L"ScreenUpdating", Var(false));
            bool ok = false;
            const auto temp = TempPdf();
            auto removeTemp = wil::scope_exit([&] { DeleteFileW(temp.c_str()); });
            if (auto docs = Get(app.Get(), L"Documents", error))
            {
                auto doc = Call(docs.Get(), L"Open",
                                NamedArgs(N(L"FileName", job.source), N(L"ConfirmConversions", false), N(L"ReadOnly", true), N(L"AddToRecentFiles", false),
                                          N(L"PasswordDocument", kNoPassword), N(L"Visible", false), N(L"NoEncodingDialog", true)),
                                error);
                if (doc)
                {
                    watchdog.Progress();
                    if (job.progress)
                        job.progress(0.4);
                    bool failed = false;
                    Call(doc.Get(), L"ExportAsFixedFormat",
                         NamedArgs(N(L"OutputFileName", temp), N(L"ExportFormat", 17), N(L"OpenAfterExport", false), N(L"OptimizeFor", 0),
                                   N(L"CreateBookmarks", 1), N(L"DocStructureTags", true), N(L"IncludeDocProps", true)),
                         error, &failed);
                    ok = !failed && copyOut(temp);
                    if (!watchdog.Fired())
                    {
                        std::wstring ignored;
                        Call(doc.Get(), L"Close", NamedArgs(N(L"SaveChanges", 0)), ignored);
                    }
                }
            }
            if (!watchdog.Fired() && started)
            {
                std::wstring ignored;
                Call(app.Get(), L"Quit", NamedArgs(N(L"SaveChanges", 0)), ignored);
            }
            return finish(ok);
        }

        if (backend == Backend::Excel)
        {
            TryPut(app.Get(), L"Visible", Var(false));
            TryPut(app.Get(), L"DisplayAlerts", Var(false));
            TryPut(app.Get(), L"AutomationSecurity", Var(3));
            TryPut(app.Get(), L"ScreenUpdating", Var(false));
            TryPut(app.Get(), L"AskToUpdateLinks", Var(false));
            TryPut(app.Get(), L"EnableEvents", Var(false));
            bool ok = false;
            const auto temp = TempPdf();
            auto removeTemp = wil::scope_exit([&] { DeleteFileW(temp.c_str()); });
            if (auto books = Get(app.Get(), L"Workbooks", error))
            {
                auto book = Call(books.Get(), L"Open",
                                 NamedArgs(N(L"Filename", job.source), N(L"UpdateLinks", 0), N(L"ReadOnly", true), N(L"Password", kNoPassword),
                                           N(L"IgnoreReadOnlyRecommended", true), N(L"AddToMru", false), N(L"Notify", false)),
                                 error);
                if (book)
                {
                    watchdog.Progress();
                    if (job.progress)
                        job.progress(0.4);
                    bool failed = false;
                    Call(book.Get(), L"ExportAsFixedFormat",
                         NamedArgs(N(L"Type", 0), N(L"Filename", temp), N(L"Quality", 0), N(L"IncludeDocProperties", true), N(L"IgnorePrintAreas", false),
                                   N(L"OpenAfterPublish", false)),
                         error, &failed);
                    ok = !failed && copyOut(temp);
                    if (!watchdog.Fired())
                    {
                        std::wstring ignored;
                        Call(book.Get(), L"Close", NamedArgs(N(L"SaveChanges", false)), ignored);
                    }
                }
            }
            if (!watchdog.Fired() && started)
            {
                std::wstring ignored;
                Call(app.Get(), L"Quit", {}, ignored);
            }
            return finish(ok);
        }

        // PowerPoint. It can't be made invisible; WithWindow:=False keeps the presentation hidden.
        TryPut(app.Get(), L"DisplayAlerts", Var(1));        // ppAlertsNone
        TryPut(app.Get(), L"AutomationSecurity", Var(3));
        bool ok = false;
        if (auto presentations = Get(app.Get(), L"Presentations", error))
        {
            auto pres = Call(presentations.Get(), L"Open",
                             NamedArgs(N(L"FileName", job.source), N(L"ReadOnly", -1), N(L"Untitled", 0), N(L"WithWindow", 0)), error);
            if (pres)
            {
                watchdog.Progress();
                if (job.progress)
                    job.progress(0.3);
                if (job.target == "pdf")
                {
                    const auto temp = TempPdf();
                    auto removeTemp = wil::scope_exit([&] { DeleteFileW(temp.c_str()); });
                    bool failed = false;
                    Call(pres.Get(), L"SaveAs", NamedArgs(N(L"FileName", temp), N(L"FileFormat", 32)), error, &failed);   // ppSaveAsPDF
                    ok = !failed && copyOut(temp);
                }
                else if (auto slides = Get(pres.Get(), L"Slides", error))
                {
                    const int count = static_cast<int>(GetNumber(slides.Get(), L"Count", error));
                    SIZE px{ 1920, 1080 };
                    if (auto setup = Get(pres.Get(), L"PageSetup", error))
                        px = SlidePixels(static_cast<float>(GetNumber(setup.Get(), L"SlideWidth", error)),
                                         static_cast<float>(GetNumber(setup.Get(), L"SlideHeight", error)));
                    const std::wstring ext = job.target == "png" ? L".png" : L".jpg";
                    const std::wstring filter = job.target == "png" ? L"PNG" : L"JPG";
                    ok = count > 0;
                    if (!ok)
                        error = L"the presentation has no slides";
                    for (int i = 1; ok && i <= count && !watchdog.Fired(); ++i)
                    {
                        std::vector<Var> index;
                        index.emplace_back(i);
                        Var item;
                        ok = SUCCEEDED(Invoke(slides.Get(), L"Item", DISPATCH_METHOD | DISPATCH_PROPERTYGET, {}, std::move(index), &item, error));
                        auto slide = item.Dispatch();
                        ok = ok && slide;
                        if (!ok)
                            break;
                        const auto path = (std::filesystem::path(job.output) / SlideFileName(i, count, ext)).wstring();
                        bool failed = false;
                        Call(slide.Get(), L"Export",
                             NamedArgs(N(L"FileName", path), N(L"FilterName", filter), N(L"ScaleWidth", static_cast<int>(px.cx)),
                                       N(L"ScaleHeight", static_cast<int>(px.cy))),
                             error, &failed);
                        ok = !failed;
                        watchdog.Progress();
                        if (job.progress)
                            job.progress(0.3 + 0.7 * i / count);
                    }
                }
                if (!watchdog.Fired())
                {
                    std::wstring ignored;
                    Call(pres.Get(), L"Close", {}, ignored);
                }
            }
        }
        if (!watchdog.Fired() && started)
        {
            std::wstring ignored;
            Call(app.Get(), L"Quit", {}, ignored);
        }
        return finish(ok);
    }
}
