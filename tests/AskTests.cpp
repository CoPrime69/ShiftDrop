#include "Ask.h"
#include "Secret.h"
#include "TestUtil.h"
#include "Utf.h"

#include <windows.h>

#include <commctrl.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

using namespace sd;

// Questions a running job asks the user (src/Ask.h): a worker asks, the UI thread answers.
namespace
{
    ask::Question Sample()
    {
        ask::Question q;
        q.title = L"Ask test";
        q.heading = L"Which one?";
        q.choices = { { 1, L"One", {} }, { 2, L"Two", L"the second" } };
        q.unattended = 2;
        return q;
    }

    // Plays the user from another thread: waits for the window titled `title` (a dialog) and hands
    // it to `act`. Gives up after 5 s.
    std::jthread User(const wchar_t* title, std::function<void(HWND)> act)
    {
        return std::jthread([title, act = std::move(act)](std::stop_token own) {
            for (int i = 0; i < 250 && !own.stop_requested(); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                if (const HWND box = FindWindowW(nullptr, title))
                {
                    act(box);
                    return;
                }
            }
        });
    }

    // Runs the UI thread's message loop until `done` (or `ms` pass).
    void PumpUntil(const std::atomic<bool>& done, int ms = 15000)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!done && std::chrono::steady_clock::now() < until)
        {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
    }
}

TEST(Ask, SecretsWipeAndMove)
{
    Secret s;
    EXPECT_TRUE(s.empty());
    EXPECT_STREQ(s.c_str(), L"");
    ASSERT_TRUE(s.Assign(L"hunter2"));
    EXPECT_EQ(s.View(), L"hunter2");
    const wchar_t* buffer = s.c_str();
    Secret moved = std::move(s);
    EXPECT_EQ(moved.c_str(), buffer) << "moving hands the buffer over: the text is not copied";
    EXPECT_TRUE(s.empty());
    moved.Clear();
    EXPECT_EQ(buffer[0], L'\0');
    EXPECT_EQ(buffer[6], L'\0') << "cleared means zeroed, not just shortened";
    EXPECT_FALSE(moved.Assign(std::wstring(Secret::kCapacity + 1, L'x'))) << "too long for the box";
    EXPECT_TRUE(moved.empty());
}

TEST(Ask, NumbersInQuestions)
{
    EXPECT_EQ(ask::Ordinal(1), L"1st");
    EXPECT_EQ(ask::Ordinal(2), L"2nd");
    EXPECT_EQ(ask::Ordinal(3), L"3rd");
    EXPECT_EQ(ask::Ordinal(4), L"4th");
    EXPECT_EQ(ask::Ordinal(11), L"11th");
    EXPECT_EQ(ask::Ordinal(12), L"12th");
    EXPECT_EQ(ask::Ordinal(13), L"13th");
    EXPECT_EQ(ask::Ordinal(22), L"22nd");
    EXPECT_EQ(ask::Ordinal(101), L"101st");
    EXPECT_EQ(ask::Ordinal(111), L"111th");
}

TEST(Ask, WithNoOneToAskTheJobDoesWhatItDidBefore)
{
    EXPECT_EQ(ask::Choose(Sample(), {}), 2);
    Secret password;
    EXPECT_FALSE(ask::Password({ L"Ask test", L"a.zip", false }, password, {}));
    std::stop_source stop;
    stop.request_stop();
    EXPECT_EQ(ask::Choose(Sample(), stop.get_token()), ask::kCancel) << "a stopped job asks nothing";
}

TEST(Ask, AnswersComeFromTheUiThread)
{
    ask::InstallUi();
    // On Windows the test runner has no Common Controls 6 (no manifest), so the TaskDialog can't
    // open and the question comes back as Cancel; Wine's has one, and the "user" picks Two. Either
    // way the question went from the worker to this thread and back.
    std::atomic<bool> done{ false };
    int answer = -1;
    DWORD askedOn = 0;
    auto user = User(L"Ask test", [](HWND box) { PostMessageW(box, TDM_CLICK_BUTTON, 2, 0); });
    std::jthread worker([&] {
        askedOn = GetCurrentThreadId();
        answer = ask::Choose(Sample(), {});
        done = true;
    });
    PumpUntil(done);
    worker.join();
    user.request_stop();
    EXPECT_TRUE(done);
    EXPECT_NE(askedOn, GetCurrentThreadId());
    EXPECT_TRUE(answer == ask::kCancel || answer == 2) << answer;

    // A stop lets the worker go at once, without waiting for the UI thread.
    std::stop_source stop;
    done = false;
    std::jthread stopped([&] {
        answer = ask::Choose(Sample(), stop.get_token());
        done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.request_stop();
    stopped.join();
    EXPECT_EQ(answer, ask::kCancel);
    const std::atomic<bool> never{ false };
    PumpUntil(never, 200);   // the question still queued is skipped, not shown

    // After UninstallUi there is no one to ask again.
    ask::UninstallUi();
    EXPECT_EQ(ask::Choose(Sample(), {}), 2);
}

TEST(Ask, ChooseNowAsksOnTheCallingThread)
{
    // No one to ask, then the tests' answers: the same routes as Choose.
    EXPECT_EQ(ask::ChooseNow(nullptr, Sample()), 2);
    DWORD answeredOn = 0;
    ask::SetAnswers([&](const ask::Question& q) {
        answeredOn = GetCurrentThreadId();
        return q.choices.front().id;
    }, {});
    EXPECT_EQ(ask::ChooseNow(nullptr, Sample()), 1);
    EXPECT_EQ(answeredOn, GetCurrentThreadId());
    ask::SetAnswers({}, {});

    // With the UI: the TaskDialog opens on this thread at once (no worker, no queue). Without Common
    // Controls 6 (the Windows test runner) it can't open and the answer is Cancel; under Wine the
    // "user" picks Two.
    ask::InstallUi();
    auto user = User(L"Ask test", [](HWND box) { PostMessageW(box, TDM_CLICK_BUTTON, 2, 0); });
    const int answer = ask::ChooseNow(nullptr, Sample());
    user.request_stop();
    ask::UninstallUi();
    EXPECT_TRUE(answer == ask::kCancel || answer == 2) << answer;
}

TEST(Ask, ThePasswordBoxHandsOverWhatWasTyped)
{
    ask::InstallUi();
    std::atomic<bool> done{ false };
    bool ok = false;
    std::wstring got, shown;
    std::stop_source watchdog;   // never hang a test run: Cancel after 10 s
    std::jthread worker([&] {
        Secret password;
        ok = ask::Password({ L"Ask test password", L"locked.zip", true }, password, watchdog.get_token());
        got = password.View();
        done = true;
    });
    // The user reads what the box says, types and presses OK.
    auto user = User(L"Ask test password", [&](HWND box) {
        wchar_t text[256]{};
        GetWindowTextW(FindWindowExW(box, nullptr, L"Static", nullptr), text, 256);
        shown = text;
        SetWindowTextW(FindWindowExW(box, nullptr, L"Edit", nullptr), L"hunter2");
        PostMessageW(box, WM_COMMAND, IDOK, 0);
    });
    std::jthread timeout([&](std::stop_token own) {
        for (int i = 0; i < 1000 && !own.stop_requested() && !done; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!done)
            watchdog.request_stop();
    });
    PumpUntil(done);
    worker.join();
    user.request_stop();
    user.join();
    ask::UninstallUi();
    ASSERT_TRUE(ok) << "the box did not appear or did not answer";
    EXPECT_EQ(got, L"hunter2");
    EXPECT_NE(shown.find(L"locked.zip"), std::wstring::npos) << Narrow(shown);
    EXPECT_NE(shown.find(L"didn't work"), std::wstring::npos) << Narrow(shown);
}

// The question window's buttons: the choices, the first of them primary, and Cancel; one confirm
// button for a list to pick from.
TEST(Ask, TheQuestionWindowsButtons)
{
    auto q = Sample();
    auto buttons = ask::ButtonsOf(q);
    ASSERT_EQ(buttons.size(), 3u);
    EXPECT_EQ(buttons[0].id, 1);
    EXPECT_TRUE(buttons[0].primary);
    EXPECT_EQ(buttons[1].label, L"Two");
    EXPECT_FALSE(buttons[1].primary);
    EXPECT_EQ(buttons[2].id, ask::kCancel);
    EXPECT_EQ(buttons[2].label, L"Cancel");

    q.pickOne = true;
    q.confirm = L"Normalize";
    buttons = ask::ButtonsOf(q);
    ASSERT_EQ(buttons.size(), 2u);
    EXPECT_EQ(buttons[0].id, ask::kConfirm);
    EXPECT_EQ(buttons[0].label, L"Normalize");
    EXPECT_TRUE(buttons[0].primary);
    q.confirm.clear();
    EXPECT_EQ(ask::ButtonsOf(q)[0].label, L"OK");

    q.choices.clear();
    q.pickOne = false;
    buttons = ask::ButtonsOf(q);
    ASSERT_EQ(buttons.size(), 1u);
    EXPECT_TRUE(buttons[0].primary) << "Cancel alone is the one Enter presses";

    // The update window's "Later" is its Cancel.
    q.cancel = L"Later";
    EXPECT_EQ(ask::ButtonsOf(q).back().label, L"Later");
    EXPECT_EQ(ask::ButtonsOf(q).back().id, ask::kCancel);
}

// With the app's question window: a worker's question and password, and a question asked on the UI
// thread, go to it; what it answers comes back, and an answer that isn't a choice is Cancel.
TEST(Ask, TheAppsQuestionWindowAnswers)
{
    std::atomic<int> asked{ 0 };
    ask::Presenter presenter;
    presenter.choose = [&](HWND, const ask::Question& q, const std::function<void(HWND)>&) {
        ++asked;
        return q.heading == L"Wrong" ? 99 : 2;
    };
    presenter.password = [&](const ask::PasswordRequest& r, Secret& password, const std::function<void(HWND)>&) {
        ++asked;
        return r.wrong && password.Assign(L"hunter2") ? IDOK : ask::kCancel;
    };
    ask::InstallUi(presenter);
    std::atomic<bool> done{ false };
    int choice = -5;
    bool ok = false;
    std::wstring got;
    std::jthread worker([&] {
        choice = ask::Choose(Sample(), {});
        Secret password;
        ok = ask::Password({ L"Ask test password", L"locked.zip", true }, password, {});
        got = password.View();
        done = true;
    });
    PumpUntil(done);
    worker.join();
    auto wrong = Sample();
    wrong.heading = L"Wrong";
    EXPECT_EQ(ask::ChooseNow(nullptr, Sample()), 2);
    EXPECT_EQ(ask::ChooseNow(nullptr, wrong), ask::kCancel);
    ask::UninstallUi();
    EXPECT_EQ(choice, 2);
    EXPECT_TRUE(ok);
    EXPECT_EQ(got, L"hunter2");
    EXPECT_EQ(asked, 4);
}
