#pragma once
#include "JobQueue.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <thread>
#include <vector>

// Several pages or pictures at once: WIC decodes and encodes on one core each, so a PDF of many
// pages goes about as many times faster as the PC has cores to spare.
namespace sd::pdf
{
    // Calls work(i) for every i in [0, count) on up to JobQueue::PoolThreads() threads (the calling
    // one included) and returns when all have returned. The extra threads join the MTA, as the app's
    // workers do (WIC is free-threaded; WinRT calls that block are fine there).
    inline void ForEachParallel(size_t count, const std::function<void(size_t)>& work)
    {
        std::atomic<size_t> next{ 0 };
        const auto run = [&] {
            for (size_t i; (i = next++) < count;)
                work(i);
        };
        std::vector<std::jthread> helpers;
        const size_t threads = std::min<size_t>(JobQueue::PoolThreads(), count);
        for (size_t t = 1; t < threads; ++t)
            helpers.emplace_back([&] {
                const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                run();
                if (SUCCEEDED(hr))
                    CoUninitialize();
            });
        run();
    }   // the helpers join here
}
