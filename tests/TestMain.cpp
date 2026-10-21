#include <windows.h>
#include <objbase.h>

#include <gtest/gtest.h>

namespace sd::test
{
    int RunWorkerIfAsked();   // SandboxTests.cpp: the sandbox tests start this exe as their worker
}

// CTest starts this exe once per test, several at a time (CI runs four at once on each of its three
// test runners), so a test must not share a file or folder with another: TempDir (TestUtil.h) names
// its folder after the process.
int main(int argc, char** argv)
{
    if (const int code = sd::test::RunWorkerIfAsked(); code >= 0)
        return code;
    // STA, like the app's UI thread: Undo and Move use IFileOperation, which needs one.
    // WIC (the image tests) works in either apartment.
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    if (SUCCEEDED(hr))
        CoUninitialize();
    return result;
}
