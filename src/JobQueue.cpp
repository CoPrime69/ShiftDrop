#include "JobQueue.h"
#include "Log.h"

#include <windows.h>
#include <objbase.h>
#include <wil/resource.h>

#include <cstring>
#include <exception>

namespace sd
{
    JobQueue::JobQueue(unsigned threads, Apartment apartment) : m_apartment(apartment)
    {
        if (threads == 0)
            threads = 1;
        m_threads.reserve(threads);
        for (unsigned i = 0; i < threads; ++i)
            m_threads.emplace_back([this](std::stop_token st) { Run(st); });
    }

    JobQueue::~JobQueue()
    {
        for (auto& t : m_threads)
            t.request_stop();
        m_cv.notify_all();
        m_threads.clear();   // joins
    }

    void JobQueue::Submit(Task task)
    {
        {
            std::scoped_lock lock(m_mutex);
            m_tasks.push_back(std::move(task));
        }
        m_cv.notify_one();
    }

    size_t JobQueue::Pending() const
    {
        std::scoped_lock lock(m_mutex);
        return m_tasks.size();
    }

    void JobQueue::Run(std::stop_token stop)
    {
        // An STA thread here never hosts objects other apartments call into, so it needs no message pump.
        const HRESULT hr = CoInitializeEx(nullptr, m_apartment == Apartment::Sta ? COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE
                                                                                 : COINIT_MULTITHREADED);
        auto uninit = wil::scope_exit([hr] { if (SUCCEEDED(hr)) CoUninitialize(); });

        while (!stop.stop_requested())
        {
            Task task;
            {
                std::unique_lock lock(m_mutex);
                // The stop_token overload wakes up when stop is requested, so shutdown never hangs here.
                if (!m_cv.wait(lock, stop, [this] { return !m_tasks.empty(); }))
                    return;
                task = std::move(m_tasks.front());
                m_tasks.pop_front();
            }
            try
            {
                task(stop);
            }
            catch (const std::exception& e)
            {
                log::Error(L"job: unhandled exception: {}", std::wstring(e.what(), e.what() + std::strlen(e.what())));
            }
            catch (...)
            {
                log::Error(L"job: unhandled non-standard exception");
            }
        }
    }
}
