#include "AskWindow.h"
#include "Brand.h"
#include "DialogWindow.h"
#include "Log.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace sd
{
    namespace
    {
        constexpr wchar_t kClass[] = L"" SD_NAME L".Question";
        constexpr int kIdPassword = 100, kIdOption = 200, kIdButton = 300;

        // DIPs (App Windows page: the questions are 440 wide, 24 around the text, a bar of buttons).
        constexpr int kWidth = 440, kPad = 24, kButtonH = 32, kButtonGap = 8, kBar = 72, kOptionH = 52;

        // One question, shown modally on the UI thread: Run() returns once it is answered.
        class QuestionWindow final : public DialogWindow
        {
        public:
            QuestionWindow(HINSTANCE instance, const ask::Question* question, const ask::PasswordRequest* password, Secret* secret)
                : DialogWindow(instance, { .className = kClass, .width = kWidth, .height = 200, .minWidth = 320, .minHeight = 160, .fixed = true }),
                  m_question(question), m_password(password), m_secret(secret)
            {
            }

            int Run(HWND owner, const std::function<void(HWND)>& shown)
            {
                const std::wstring title = m_password ? m_password->title : m_question->title;
                if (!Create(title.empty() ? L"" SD_NAME : title.c_str(), owner))
                {
                    log::Warn(L"ask: cannot show the question window ({})", GetLastError());
                    return ask::kCancel;
                }
                if (owner)
                {
                    // Over the window that asks, like a dialog of its own.
                    RECT o{}, w{};
                    GetWindowRect(owner, &o);
                    GetWindowRect(m_hwnd, &w);
                    SetWindowPos(m_hwnd, nullptr, o.left + (o.right - o.left - (w.right - w.left)) / 2, o.top + (o.bottom - o.top - (w.bottom - w.top)) / 3,
                                 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    EnableWindow(owner, FALSE);
                }
                Present(First());
                if (shown)
                    shown(m_hwnd);   // places it (a job's question) and closes it at once if the job has gone
                MSG msg{};
                while (!m_done && m_hwnd)
                {
                    const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
                    if (got <= 0)
                    {
                        if (got == 0)
                            PostQuitMessage(static_cast<int>(msg.wParam));   // for the app's own loop
                        m_answer = ask::kCancel;
                        break;
                    }
                    if (!PreTranslate(msg))
                    {
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                }
                if (owner)
                {
                    EnableWindow(owner, TRUE);
                    SetActiveWindow(owner);   // before this one goes, so the owner comes back in front
                }
                if (m_hwnd)
                    WipeField();
                return m_answer;
            }

        private:
            void CreateChildren() override
            {
                const std::wstring heading = m_password ? std::wstring(L"Password needed") : m_question->heading;
                m_heading = Label(heading.c_str(), TextRole::Question);
                std::wstring text = m_password ? std::format(L"{} is protected. Enter its password to open it.", m_password->file) : m_question->text;
                m_text = Label(text.c_str(), TextRole::Body);
                if (m_password)
                {
                    m_field = TextField(kIdPassword, ES_PASSWORD, L"Password");
                    SendMessageW(m_field, EM_LIMITTEXT, Secret::kCapacity, 0);
                    Describe(m_field, L"Password", text);
                    m_error = Label(m_password->wrong ? L"That password didn\u2019t work. Try again." : L"", TextRole::Error);
                    m_note = Label(L"It is used for this file only and never saved.", TextRole::Hint);
                }
                else if (m_question->pickOne)
                    for (size_t i = 0; i < m_question->choices.size(); ++i)
                    {
                        const auto& c = m_question->choices[i];
                        const HWND option = KitButton(Look::Radio, c.label.c_str(), kIdOption + static_cast<int>(i), Surface::Card, 0, i == 0);
                        StyleOf(option).detail = c.note;
                        Describe(option, c.label, c.note);
                        m_options.push_back(option);
                    }
                // The buttons: the primary one is IDOK (Enter presses it) and Cancel is IDCANCEL (Esc).
                const auto buttons = ask::ButtonsOf(*Question());
                for (size_t i = 0; i < buttons.size(); ++i)
                {
                    const auto& b = buttons[i];
                    const int id = b.id == ask::kCancel ? IDCANCEL : b.primary ? IDOK : kIdButton + static_cast<int>(i);
                    const HWND h = KitButton(b.primary ? Look::Primary : Look::Button, b.label.c_str(), id, Surface::Window);
                    m_buttons.push_back({ h, b.id, NoteOf(b.id) });
                    if (!m_buttons.back().note.empty())
                    {
                        Describe(h, b.label, m_buttons.back().note);
                        // BN_SETFOCUS: the line under the question says what the focused button does.
                        SetWindowLongPtrW(h, GWL_STYLE, GetWindowLongPtrW(h, GWL_STYLE) | BS_NOTIFY);
                    }
                }
                if (std::any_of(m_buttons.begin(), m_buttons.end(), [](const ButtonView& b) { return !b.note.empty(); }))
                    m_buttonNote = Label(L"", TextRole::Caption);
                if (!m_options.empty())
                    CheckOne(m_options, 0);
                if (m_password)
                    EnableWindow(Primary(), FALSE);   // until something is typed
            }

            const ask::Question* Question() const
            {
                static const ask::Question kPassword = [] {
                    ask::Question q;
                    q.pickOne = true;
                    q.confirm = L"Open";
                    return q;
                }();
                return m_password ? &kPassword : m_question;
            }

            std::wstring NoteOf(int id) const
            {
                if (m_password || m_question->pickOne)
                    return {};
                for (const auto& c : m_question->choices)
                    if (c.id == id)
                        return c.note;
                return {};
            }

            HWND Primary() const
            {
                for (const auto& b : m_buttons)
                    if (GetDlgCtrlID(b.button) == IDOK)
                        return b.button;
                return m_buttons.empty() ? nullptr : m_buttons.front().button;
            }

            HWND First() const
            {
                if (m_field)
                    return m_field;
                if (!m_options.empty())
                    return m_options.front();
                return Primary();
            }

            void Layout() override
            {
                RECT rc{};
                GetClientRect(m_hwnd, &rc);
                const int pad = Px(kPad), width = static_cast<int>(rc.right) - 2 * pad;
                int y = pad;
                const auto label = [&](HWND h, int gapAfter) {
                    if (!h || GetWindowTextLengthW(h) == 0)
                    {
                        if (h)
                            ShowWindow(h, SW_HIDE);
                        return;
                    }
                    const int lh = LabelHeight(h, width);
                    MoveWindow(h, pad, y, width, lh, TRUE);
                    ShowWindow(h, SW_SHOWNA);
                    y += lh + gapAfter;
                };
                label(m_heading, Px(8));
                label(m_text, Px(16));
                if (m_field)
                {
                    PlaceField(m_field, { pad, y, pad + width, y + Px(kButtonH) });
                    y += Px(kButtonH) + Px(8);
                    label(m_error, Px(4));
                    label(m_note, 0);
                    y += Px(8);
                }
                m_optionBox = {};
                if (!m_options.empty())
                {
                    m_optionBox = { pad, y, pad + width, y + Px(6) * 2 + static_cast<int>(m_options.size()) * Px(kOptionH) };
                    int oy = y + Px(6);
                    for (HWND option : m_options)
                    {
                        MoveWindow(option, pad + Px(6), oy, width - Px(12), Px(kOptionH), TRUE);
                        oy += Px(kOptionH);
                    }
                    y = m_optionBox.bottom + Px(16);
                }
                if (m_buttonNote)
                    label(m_buttonNote, Px(8));
                m_barTop = std::max(y, pad + Px(40)) + Px(8);
                const int needed = m_barTop + Px(kBar);
                if (rc.bottom != needed && !m_fitting)
                {
                    // As tall as what it says: sized again (and laid out again) at this width.
                    m_fitting = true;
                    SetClientSize(kWidth, static_cast<int>(std::lround(Dip(needed))));
                    m_fitting = false;
                    return;
                }
                // The buttons at the end of the bar, each as wide as its text (88 DIPs at least).
                int x = static_cast<int>(rc.right) - pad;
                const int by = m_barTop + (Px(kBar) - Px(kButtonH)) / 2;
                for (auto it = m_buttons.rbegin(); it != m_buttons.rend(); ++it)
                {
                    const int w = std::max(Px(88), Px(static_cast<int>(Kit().TextWidth(Text(it->button), ControlKit::kControlText))) + Px(32));
                    x -= w;
                    MoveWindow(it->button, x, by, w, Px(kButtonH), TRUE);
                    x -= Px(kButtonGap);
                }
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }

            void PaintSurface(ControlKit& kit, const D2D1_RECT_F& client) override
            {
                // The question on the card colour; the buttons on a bar of the window colour under a line.
                const auto& c = kit.Theme();
                const float bar = Dip(m_barTop);
                kit.FillRound(D2D1::RectF(client.left, client.top, client.right, bar), c.panel, 0.f);
                kit.DrawFooterBar(D2D1::RectF(client.left, bar, client.right, client.bottom), L"", client.left);
                if (m_optionBox.right > m_optionBox.left)
                    kit.StrokeRound(DipRect(m_optionBox), c.line, 1.f, ControlKit::kCardRadius);
            }

            void Answer(int answer)
            {
                m_answer = answer;
                m_done = true;
            }

            // What the password field held, overwritten before the control frees it.
            void WipeField()
            {
                if (!m_field)
                    return;
                if (const int length = GetWindowTextLengthW(m_field); length > 0)
                    SetWindowTextW(m_field, std::wstring(static_cast<size_t>(length), L' ').c_str());
                SetWindowTextW(m_field, L"");
            }

            void Confirm(int id)
            {
                if (m_password)
                {
                    // Straight from the control into the secret: no other copy is made.
                    const int n = GetWindowTextW(m_field, m_secret->Buffer(), static_cast<int>(Secret::kCapacity + 1));
                    m_secret->SetSize(n > 0 ? static_cast<size_t>(n) : 0);
                    WipeField();
                    Answer(m_secret->empty() ? ask::kCancel : IDOK);
                    return;
                }
                if (id == ask::kConfirm)
                {
                    for (size_t i = 0; i < m_options.size(); ++i)
                        if (Checked(m_options[i]))
                            return Answer(m_question->choices[i].id);
                    return Answer(ask::kCancel);
                }
                Answer(id);
            }

            LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam) override
            {
                switch (msg)
                {
                case WM_COMMAND:
                {
                    const int id = LOWORD(wParam), code = HIWORD(wParam);
                    if (id == IDCANCEL)
                    {
                        WipeField();
                        Answer(ask::kCancel);
                    }
                    else if (id == kIdPassword && code == EN_CHANGE)
                        EnableWindow(Primary(), GetWindowTextLengthW(m_field) > 0);
                    else if (id == IDOK || (id >= kIdButton && code == BN_CLICKED))
                    {
                        if (id == IDOK && !IsWindowEnabled(Primary()))
                            return 0;   // Enter in an empty password field
                        const HWND button = id == IDOK ? Primary() : reinterpret_cast<HWND>(lParam);
                        for (const auto& b : m_buttons)
                            if (b.button == button)
                                Confirm(b.answer);
                    }
                    else if (code == BN_SETFOCUS && m_buttonNote)
                    {
                        for (const auto& b : m_buttons)
                            if (b.button == reinterpret_cast<HWND>(lParam) && Text(m_buttonNote) != b.note)
                            {
                                SetWindowTextW(m_buttonNote, b.note.c_str());
                                Layout();
                            }
                    }
                    return 0;
                }
                case WM_CLOSE:
                    WipeField();
                    Answer(ask::kCancel);
                    return 0;
                default:
                    return DialogWindow::Handle(msg, wParam, lParam);
                }
            }

            const ask::Question* m_question;
            const ask::PasswordRequest* m_password;
            Secret* m_secret;
            HWND m_heading = nullptr, m_text = nullptr, m_field = nullptr, m_error = nullptr, m_note = nullptr, m_buttonNote = nullptr;
            std::vector<HWND> m_options;
            struct ButtonView
            {
                HWND button = nullptr;
                int answer = ask::kCancel;   // what pressing it answers (ask::kConfirm: the option picked)
                std::wstring note;
            };
            std::vector<ButtonView> m_buttons;
            RECT m_optionBox{};
            int m_barTop = 0;
            bool m_fitting = false;
            bool m_done = false;
            int m_answer = ask::kCancel;
        };
    }

    ask::Presenter QuestionWindows(HINSTANCE instance)
    {
        ask::Presenter p;
        p.choose = [instance](HWND owner, const ask::Question& q, const std::function<void(HWND)>& shown) {
            QuestionWindow window(instance, &q, nullptr, nullptr);
            return window.Run(owner, shown);
        };
        p.password = [instance](const ask::PasswordRequest& r, Secret& password, const std::function<void(HWND)>& shown) {
            QuestionWindow window(instance, nullptr, &r, &password);
            return window.Run(nullptr, shown);
        };
        return p;
    }
}
