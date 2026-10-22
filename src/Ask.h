#pragma once
#include "Secret.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

// Questions a running job asks the user: the password of an archive, whether to print all of a
// very long spreadsheet, whether to save every frame of a long GIF. The job runs on a worker; the
// question is shown on the UI thread while the worker waits for the answer: in the app's own question
// window (AskWindow.h, given to InstallUi), or else a TaskDialog with command links and Cancel and a
// small masked box for a password. A question asked on the UI thread itself
// (Normalize's "how loud?") goes through ChooseNow and is shown at once.
//
// It cannot deadlock: the UI thread never waits for a worker, questions are shown one at a time, and
// the job's stop token (the × in the top panel, or quitting) closes an open question as Cancel and
// lets the worker go at once.
namespace sd::ask
{
    // A command link: a bold first line and an optional second line saying what it does.
    struct Choice
    {
        int id = 0;   // > 0
        std::wstring label;
        std::wstring note;
    };

    constexpr int kCancel = 0;

    struct Question
    {
        std::wstring title;            // the window title: the tool ("Spreadsheet to PDF")
        std::wstring heading;          // the question, in bold
        std::wstring text;             // a line under it, optional
        std::vector<Choice> choices;   // in order; Cancel is always offered as well
        // The answer when there is no one to ask (the tests, or a build without the UI): what the
        // job did before it asked. kCancel is allowed.
        int unattended = kCancel;
        // The choices are the options of one answer (Normalize's Speech, Music, Broadcast): the
        // question window lists them to pick one, the first picked, under one `confirm` button.
        bool pickOne = false;
        std::wstring confirm;   // that button's text; "OK" when empty
        std::wstring cancel;    // the Cancel button's text ("Later"); "Cancel" when empty
    };

    // The buttons the question window shows for `question`, in order; the first is the primary one
    // (Enter presses it) and Cancel is last. With pickOne, one kConfirm button answers with the
    // option picked in the list.
    constexpr int kConfirm = -1;
    struct Button
    {
        int id = kCancel;
        std::wstring label;
        bool primary = false;
    };
    std::vector<Button> ButtonsOf(const Question& question);

    // From a worker: shows `question` and waits. Returns a choice's id, or kCancel when the user
    // cancels or `stop` is requested.
    int Choose(const Question& question, std::stop_token stop);

    // From the UI thread: shows `question` over `owner` (may be null) at once and returns a choice's
    // id, or kCancel. The tests' answers and the unattended answer apply as they do to Choose.
    int ChooseNow(HWND owner, const Question& question);

    struct PasswordRequest
    {
        std::wstring title;    // the window title
        std::wstring file;     // the file's name, shown in the box
        bool wrong = false;    // the last password didn't work: say so
    };

    // From a worker: asks for a password in a small box with a masked field, OK and Cancel. True with
    // the password in `password`; false on Cancel, on a stop, or with no one to ask. The password is
    // never logged or stored: it goes from the box into `password` and nowhere else.
    bool Password(const PasswordRequest& request, Secret& password, std::stop_token stop);

    // How the UI thread shows a question, when the app has a window of its own for it. Each shows
    // its question over `owner` (null: as a window of its own, with a taskbar button), calls `shown`
    // (when set) with its window once it exists, and returns when it is answered. The window closes
    // as Cancel on WM_COMMAND IDCANCEL: that is how a job that stops closes it.
    struct Presenter
    {
        // A choice's id (for pickOne, the option picked), or kCancel.
        std::function<int(HWND owner, const Question&, const std::function<void(HWND)>& shown)> choose;
        // IDOK with what was typed put straight into `password` (no other copy), or kCancel.
        std::function<int(const PasswordRequest&, Secret& password, const std::function<void(HWND)>& shown)> password;
    };

    // The app calls this once on its UI thread: questions are then shown on that thread, through
    // `presenter` where it has a function. Uninstall (at exit) closes an open question as Cancel;
    // after it there is no one to ask again.
    void InstallUi(Presenter presenter = {});
    void UninstallUi();

    // Tests: answer questions on the asking thread instead (empty functions remove them).
    using ChooseFn = std::function<int(const Question&)>;
    using PasswordFn = std::function<bool(const PasswordRequest&, Secret&)>;
    void SetAnswers(ChooseFn choose, PasswordFn password);

    // 1 -> "1st", 2 -> "2nd", 11 -> "11th", 23 -> "23rd".
    std::wstring Ordinal(uint64_t n);
}
