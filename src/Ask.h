#pragma once
#include "Secret.h"

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

// Questions a running job asks the user: the password of an archive, whether to print all of a
// very long spreadsheet, whether to save every frame of a long GIF. The job runs on a worker; the
// question is shown on the UI thread the way the Normalize chooser asks (a TaskDialog with command
// links and Cancel; a small masked box for a password) while the worker waits for the answer.
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
    };

    // From a worker: shows `question` and waits. Returns a choice's id, or kCancel when the user
    // cancels or `stop` is requested.
    int Choose(const Question& question, std::stop_token stop);

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

    // The app calls this once on its UI thread: questions are then shown on that thread. Uninstall
    // (at exit) closes an open question as Cancel; after it there is no one to ask again.
    void InstallUi();
    void UninstallUi();

    // Tests: answer questions on the asking thread instead (empty functions remove them).
    using ChooseFn = std::function<int(const Question&)>;
    using PasswordFn = std::function<bool(const PasswordRequest&, Secret&)>;
    void SetAnswers(ChooseFn choose, PasswordFn password);

    // 1 -> "1st", 2 -> "2nd", 11 -> "11th", 23 -> "23rd".
    std::wstring Ordinal(uint64_t n);
}
