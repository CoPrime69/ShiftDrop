#include "Ask.h"
#include "Log.h"
#include "resource.h"

#include <windows.h>
#include <commctrl.h>

#include <condition_variable>
#include <deque>
#include <format>
#include <memory>
#include <mutex>

namespace sd::ask
{
    namespace
    {
        constexpr UINT kAskMessage = WM_APP + 1;
        constexpr wchar_t kWindowClass[] = L"ShiftDropAsk";

        // One question on its way to the UI thread and back. Both sides hold it (shared_ptr); `done`
        // is the answer arriving, `abandoned` the worker giving up (stop requested).
        struct Request
        {
            bool password = false;
            Question question;
            PasswordRequest passwordRequest;

            std::mutex mutex;
            std::condition_variable_any answered;
            bool done = false;
            bool abandoned = false;
            HWND dialog = nullptr;   // while it is on screen
            int answer = kCancel;
            Secret secret;           // what was typed, until the worker takes it
        };
        using RequestPtr = std::shared_ptr<Request>;

        std::mutex g_mutex;
        HWND g_window = nullptr;   // the message-only window on the UI thread
        ChooseFn g_choose;
        PasswordFn g_password;

        // UI thread only.
        std::deque<RequestPtr> g_queue;
        RequestPtr g_current;   // on screen
        bool g_showing = false;

        // Closes the dialog showing `r` as Cancel, from any thread (posted: never waits for the UI).
        void CloseDialog(Request& r)
        {
            if (!r.dialog)
                return;
            if (r.password)
                PostMessageW(r.dialog, WM_COMMAND, IDCANCEL, 0);
            else
                PostMessageW(r.dialog, TDM_CLICK_BUTTON, IDCANCEL, 0);
        }

        // Called by the dialog once it exists: remember it, or close it at once if the worker has
        // already gone. Puts it in the middle of the monitor the mouse is on (the user just dropped
        // there) and in front, with the app's icon on its title bar.
        void Shown(Request& r, HWND dialog)
        {
            {
                std::scoped_lock lock(r.mutex);
                r.dialog = dialog;
                if (r.abandoned)
                    CloseDialog(r);
            }
            POINT cursor{};
            GetCursorPos(&cursor);
            MONITORINFO mi{ sizeof(mi) };
            RECT box{};
            if (GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi) && GetWindowRect(dialog, &box))
            {
                const RECT& work = mi.rcWork;
                const int w = box.right - box.left, h = box.bottom - box.top;
                SetWindowPos(dialog, nullptr, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 3, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            const HINSTANCE instance = GetModuleHandleW(nullptr);
            if (const auto icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                                GetSystemMetrics(SM_CYSMICON), LR_SHARED)))
                SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
            // A job's question comes from a background app; when Windows won't bring it forward, its
            // taskbar button flashes instead.
            SetForegroundWindow(dialog);
        }

        void Finish(Request& r, int answer)
        {
            std::scoped_lock lock(r.mutex);
            r.dialog = nullptr;
            r.answer = answer;
            if (r.abandoned)
                r.secret.Clear();
            r.done = true;
            r.answered.notify_all();
        }

        // ---- The choice: a TaskDialog with command links ----------------------------------------

        HRESULT CALLBACK ChoiceCallback(HWND dialog, UINT notification, WPARAM, LPARAM, LONG_PTR data)
        {
            if (notification == TDN_CREATED)
                Shown(*reinterpret_cast<Request*>(data), dialog);
            return S_OK;
        }

        // TaskDialogIndirect is only in Common Controls 6, which the app's manifest selects: looked up
        // once at run time, as a static import would stop the test runner (no manifest) from loading.
        using TaskDialogFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
        TaskDialogFn TaskDialog()
        {
            static const auto fn = [] {
                const HMODULE comctl = LoadLibraryW(L"comctl32.dll");
                return comctl ? reinterpret_cast<TaskDialogFn>(reinterpret_cast<void*>(GetProcAddress(comctl, "TaskDialogIndirect"))) : nullptr;
            }();
            return fn;
        }

        // Shows `q` as command links and Cancel; returns the id picked, or kCancel. With an `owner` it
        // opens over that window; without one it has its own taskbar button (a job's question may come
        // while the user is elsewhere) and `callback` places it.
        int ShowChoice(const Question& q, HWND owner, PFTASKDIALOGCALLBACK callback, LONG_PTR data)
        {
            std::vector<std::wstring> texts;
            for (const auto& c : q.choices)
                texts.push_back(c.note.empty() ? c.label : c.label + L"\n" + c.note);
            std::vector<TASKDIALOG_BUTTON> buttons;
            for (size_t i = 0; i < q.choices.size(); ++i)
                buttons.push_back({ q.choices[i].id, texts[i].c_str() });

            TASKDIALOGCONFIG config{ sizeof(config) };
            config.hwndParent = owner;
            config.hInstance = GetModuleHandleW(nullptr);
            config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | (owner ? TDF_POSITION_RELATIVE_TO_WINDOW : 0);
            config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
            config.pszWindowTitle = q.title.c_str();
            config.pszMainInstruction = q.heading.c_str();
            config.pszContent = q.text.empty() ? nullptr : q.text.c_str();
            config.cButtons = static_cast<UINT>(buttons.size());
            config.pButtons = buttons.data();
            config.nDefaultButton = q.choices.empty() ? IDCANCEL : q.choices.front().id;
            config.pfCallback = callback;
            config.lpCallbackData = data;
            const auto taskDialog = TaskDialog();
            int picked = kCancel;
            if (!taskDialog || FAILED(taskDialog(&config, &picked, nullptr, nullptr)))
            {
                log::Warn(L"ask: cannot show \"{}\"", q.heading);
                return kCancel;
            }
            for (const auto& c : q.choices)
                if (c.id == picked)
                    return picked;
            return kCancel;
        }

        // ---- The password box: a tiny dialog from an in-memory template ---------------------------

        constexpr int kTextId = 100, kLabelId = 101, kEditId = 102, kNoteId = 103;

        // DLGTEMPLATE and DLGITEMTEMPLATE records, laid out as Windows reads them (WORD units,
        // each item DWORD aligned).
        class DialogTemplate
        {
        public:
            void Begin(DWORD style, DWORD exStyle, WORD items, short cx, short cy, const wchar_t* font, WORD points)
            {
                Dword(style);
                Dword(exStyle);
                m_words.push_back(items);
                Shorts({ 0, 0, cx, cy });
                m_words.push_back(0);   // no menu
                m_words.push_back(0);   // the standard dialog class
                Text(L"");              // the title is set in WM_INITDIALOG
                m_words.push_back(points);
                Text(font);
            }
            void Item(DWORD style, short x, short y, short cx, short cy, WORD id, WORD atom, const wchar_t* text)
            {
                if (m_words.size() % 2)
                    m_words.push_back(0);
                Dword(style | WS_CHILD | WS_VISIBLE);
                Dword(0);
                Shorts({ x, y, cx, cy });
                m_words.push_back(id);
                m_words.push_back(0xFFFF);
                m_words.push_back(atom);
                Text(text);
                m_words.push_back(0);   // no creation data
            }
            const DLGTEMPLATE* Get() const { return reinterpret_cast<const DLGTEMPLATE*>(m_words.data()); }

        private:
            void Dword(DWORD v)
            {
                m_words.push_back(LOWORD(v));
                m_words.push_back(HIWORD(v));
            }
            void Shorts(std::initializer_list<short> values)
            {
                for (short v : values)
                    m_words.push_back(static_cast<WORD>(v));
            }
            void Text(const wchar_t* s)
            {
                for (; *s; ++s)
                    m_words.push_back(static_cast<WORD>(*s));
                m_words.push_back(0);
            }
            std::vector<WORD> m_words;
        };

        // Overwrites what the edit control holds before it goes, as far as its API allows: the
        // control's own buffer is freed with the dialog.
        void WipeEdit(HWND dialog)
        {
            const HWND edit = GetDlgItem(dialog, kEditId);
            const int length = GetWindowTextLengthW(edit);
            if (length > 0)
                SetWindowTextW(edit, std::wstring(static_cast<size_t>(length), L' ').c_str());
            SetWindowTextW(edit, L"");
        }

        INT_PTR CALLBACK PasswordProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
        {
            auto* r = reinterpret_cast<Request*>(GetWindowLongPtrW(dialog, DWLP_USER));
            switch (message)
            {
            case WM_INITDIALOG:
            {
                r = reinterpret_cast<Request*>(lParam);
                SetWindowLongPtrW(dialog, DWLP_USER, lParam);
                const auto& p = r->passwordRequest;
                SetWindowTextW(dialog, p.title.c_str());
                const auto text = p.wrong ? std::format(L"That password didn't work for {}. Try again.", p.file)
                                          : std::format(L"{} is protected with a password.", p.file);
                SetDlgItemTextW(dialog, kTextId, text.c_str());
                SendDlgItemMessageW(dialog, kEditId, EM_LIMITTEXT, Secret::kCapacity, 0);
                EnableWindow(GetDlgItem(dialog, IDOK), FALSE);
                Shown(*r, dialog);
                SetFocus(GetDlgItem(dialog, kEditId));
                return FALSE;   // the focus is set
            }
            case WM_COMMAND:
                switch (LOWORD(wParam))
                {
                case kEditId:
                    if (HIWORD(wParam) == EN_CHANGE)
                        EnableWindow(GetDlgItem(dialog, IDOK), GetWindowTextLengthW(GetDlgItem(dialog, kEditId)) > 0);
                    return TRUE;
                case IDOK:
                {
                    // Straight from the control into the secret: no other copy is made.
                    const int n = GetDlgItemTextW(dialog, kEditId, r->secret.Buffer(), static_cast<int>(Secret::kCapacity + 1));
                    r->secret.SetSize(n > 0 ? static_cast<size_t>(n) : 0);
                    WipeEdit(dialog);
                    EndDialog(dialog, r->secret.empty() ? IDCANCEL : IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    WipeEdit(dialog);
                    EndDialog(dialog, IDCANCEL);
                    return TRUE;
                }
                break;
            }
            return FALSE;
        }

        int ShowPassword(Request& r)
        {
            // Dialog units; Windows scales the dialog for the monitor's DPI (per-monitor v2).
            DialogTemplate t;
            // No owner and WS_EX_APPWINDOW: a taskbar button of its own, like the choices.
            t.Begin(DS_SETFONT | DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU, WS_EX_APPWINDOW, 6, 232, 92, L"Segoe UI", 9);
            t.Item(SS_LEFT | SS_NOPREFIX, 10, 9, 212, 18, kTextId, 0x0082, L"");
            t.Item(SS_LEFT, 10, 32, 50, 9, kLabelId, 0x0082, L"&Password:");
            t.Item(ES_PASSWORD | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 10, 43, 212, 13, kEditId, 0x0081, L"");
            t.Item(SS_LEFT, 10, 59, 212, 9, kNoteId, 0x0082, L"It is used for this file only and never saved.");
            t.Item(BS_DEFPUSHBUTTON | WS_TABSTOP, 118, 72, 50, 14, IDOK, 0x0080, L"OK");
            t.Item(BS_PUSHBUTTON | WS_TABSTOP, 172, 72, 50, 14, IDCANCEL, 0x0080, L"Cancel");
            const INT_PTR result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), t.Get(), nullptr, &PasswordProc, reinterpret_cast<LPARAM>(&r));
            if (result == -1)
                log::Warn(L"ask: cannot show the password box ({})", GetLastError());
            return result == IDOK ? IDOK : kCancel;
        }

        // ---- The UI thread's side ------------------------------------------------------------------

        void ShowQueued()
        {
            // A question that arrives while another is open waits for it (the open dialog's own
            // message loop delivers it here): never two at once.
            if (g_showing)
                return;
            g_showing = true;
            while (!g_queue.empty())
            {
                RequestPtr r = std::move(g_queue.front());
                g_queue.pop_front();
                {
                    std::scoped_lock lock(r->mutex);
                    if (r->abandoned)
                        continue;
                }
                g_current = r;
                const int answer = r->password ? ShowPassword(*r) : ShowChoice(r->question, nullptr, &ChoiceCallback, reinterpret_cast<LONG_PTR>(r.get()));
                g_current.reset();
                Finish(*r, answer);
            }
            g_showing = false;
        }

        LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
        {
            if (message == kAskMessage)
            {
                std::unique_ptr<RequestPtr> r(reinterpret_cast<RequestPtr*>(lParam));
                g_queue.push_back(std::move(*r));
                ShowQueued();
                return 0;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        // ---- The worker's side -------------------------------------------------------------------

        // Hands `r` to the UI thread and waits for the answer or a stop. False: no answer (cancelled).
        bool Ask(const RequestPtr& r, std::stop_token stop)
        {
            HWND window = nullptr;
            {
                std::scoped_lock lock(g_mutex);
                window = g_window;
            }
            auto message = std::make_unique<RequestPtr>(r);
            if (!window || !PostMessageW(window, kAskMessage, 0, reinterpret_cast<LPARAM>(message.get())))
                return false;
            message.release();   // the window owns it now
            std::unique_lock lock(r->mutex);
            if (r->answered.wait(lock, stop, [&] { return r->done; }))
                return true;
            r->abandoned = true;   // the UI thread skips it, or closes it if it is on screen
            CloseDialog(*r);
            return false;
        }

        enum class Route
        {
            Ui,
            Answers,
            Unattended,
        };

        Route RouteNow(ChooseFn* choose, PasswordFn* password)
        {
            std::scoped_lock lock(g_mutex);
            if (choose)
                *choose = g_choose;
            if (password)
                *password = g_password;
            if ((choose && *choose) || (password && *password))
                return Route::Answers;
            return g_window ? Route::Ui : Route::Unattended;
        }
    }

    int Choose(const Question& question, std::stop_token stop)
    {
        if (stop.stop_requested())
            return kCancel;
        ChooseFn answer;
        switch (RouteNow(&answer, nullptr))
        {
        case Route::Unattended: return question.unattended;
        case Route::Answers: return answer(question);
        case Route::Ui: break;
        }
        auto r = std::make_shared<Request>();
        r->question = question;
        log::Info(L"ask: {}", question.heading);
        if (!Ask(r, stop))
            return kCancel;
        std::scoped_lock lock(r->mutex);
        log::Info(L"ask: answer {}", r->answer);
        return r->answer;
    }

    int ChooseNow(HWND owner, const Question& question)
    {
        ChooseFn answer;
        switch (RouteNow(&answer, nullptr))
        {
        case Route::Unattended: return question.unattended;
        case Route::Answers: return answer(question);
        case Route::Ui: break;
        }
        return ShowChoice(question, owner, nullptr, 0);
    }

    bool Password(const PasswordRequest& request, Secret& password, std::stop_token stop)
    {
        password.Clear();
        if (stop.stop_requested())
            return false;
        PasswordFn answer;
        switch (RouteNow(nullptr, &answer))
        {
        case Route::Unattended: return false;
        case Route::Answers: return answer(request, password) && !password.empty();
        case Route::Ui: break;
        }
        auto r = std::make_shared<Request>();
        r->password = true;
        r->passwordRequest = request;
        log::Info(L"ask: password for {}{}", request.file, request.wrong ? L" (the last one was wrong)" : L"");
        if (!Ask(r, stop))
            return false;
        std::scoped_lock lock(r->mutex);
        if (r->answer != IDOK || r->secret.empty())
            return false;
        password = std::move(r->secret);   // moves the buffer: the text itself is not copied
        return true;
    }

    void InstallUi()
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = &WindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kWindowClass;
        RegisterClassExW(&wc);
        const HWND window = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (!window)
            log::Warn(L"ask: cannot create the question window ({}); jobs will not ask", GetLastError());
        std::scoped_lock lock(g_mutex);
        g_window = window;
    }

    void UninstallUi()
    {
        HWND window = nullptr;
        {
            std::scoped_lock lock(g_mutex);
            window = g_window;
            g_window = nullptr;
        }
        // Questions not shown yet are answered Cancel, and one on screen is closed as Cancel.
        for (auto& r : g_queue)
            Finish(*r, kCancel);
        g_queue.clear();
        if (g_current)
        {
            std::scoped_lock lock(g_current->mutex);
            CloseDialog(*g_current);
        }
        if (window)
            DestroyWindow(window);
    }

    void SetAnswers(ChooseFn choose, PasswordFn password)
    {
        std::scoped_lock lock(g_mutex);
        g_choose = std::move(choose);
        g_password = std::move(password);
    }

    std::wstring Thousands(uint64_t n)
    {
        std::wstring digits = std::to_wstring(n), out;
        for (size_t i = 0; i < digits.size(); ++i)
        {
            if (i > 0 && (digits.size() - i) % 3 == 0)
                out += L',';
            out += digits[i];
        }
        return out;
    }

    std::wstring Ordinal(uint64_t n)
    {
        const uint64_t tens = n % 100, ones = n % 10;
        const wchar_t* suffix = tens >= 11 && tens <= 13 ? L"th" : ones == 1 ? L"st" : ones == 2 ? L"nd" : ones == 3 ? L"rd" : L"th";
        return std::to_wstring(n) + suffix;
    }
}
