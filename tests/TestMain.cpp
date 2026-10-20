#include <windows.h>
#include <objbase.h>

#include <gtest/gtest.h>

int main(int argc, char** argv)
{
    // STA, like the app's UI thread: Undo and Move use IFileOperation, which needs one.
    // WIC (the image tests) works in either apartment.
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    if (SUCCEEDED(hr))
        CoUninitialize();
    return result;
}
