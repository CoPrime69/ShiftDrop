#include "JobQueue.h"
#include "Engine.h"
#include "Log.h"
#include "platform/Threads.h"

#include <algorithm>
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

    unsigned JobQueue::PoolThreads()
    {
        const unsigned cores = std::thread::hardware_concurrency();
        return std::clamp(cores > 1 ? cores - 1 : 1u, 1u, 8u);
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

    size_t JobQueue::Busy() const
    {
        std::scoped_lock lock(m_mutex);
        return m_tasks.size() + m_running;
    }

    void JobQueue::Run(std::stop_token stop)
    {
        // An STA thread here never hosts objects other apartments call into, so it needs no message pump.
        const platform::ComApartmentForThread com(m_apartment == Apartment::Sta);

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
                ++m_running;
            }
            // The task no longer counts as running once it ends, however it ends.
            struct Finished
            {
                JobQueue* queue;
                ~Finished()
                {
                    std::scoped_lock lock(queue->m_mutex);
                    --queue->m_running;
                }
            } done{ this };
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

    JobLanes::JobLanes(unsigned poolThreads)
        : m_pool(std::make_unique<JobQueue>(Threads(JobLane::Pool, poolThreads))),
          m_media(std::make_unique<JobQueue>(Threads(JobLane::Media))),
          m_audio(std::make_unique<JobQueue>(Threads(JobLane::Audio, poolThreads))),
          m_office(std::make_unique<JobQueue>(Threads(JobLane::Office), JobQueue::Apartment::Sta)),
          m_shell(std::make_unique<JobQueue>(Threads(JobLane::Shell), JobQueue::Apartment::Sta))
    {
    }

    JobLanes::~JobLanes()
    {
        m_pool.reset();
        m_media.reset();
        m_audio.reset();
        m_office.reset();
        m_shell.reset();
    }

    JobQueue& JobLanes::For(JobLane lane)
    {
        switch (lane)
        {
        case JobLane::Media: return *m_media;
        case JobLane::Audio: return *m_audio;
        case JobLane::Shell: return *m_shell;
        case JobLane::Office: return *m_office;
        case JobLane::Pool: break;
        }
        return *m_pool;
    }

    bool JobLanes::Busy() const
    {
        for (const auto* queue : { m_pool.get(), m_media.get(), m_audio.get(), m_office.get(), m_shell.get() })
            if (queue && queue->Busy() > 0)
                return true;
        return false;
    }

    unsigned JobLanes::Threads(JobLane lane, unsigned poolThreads)
    {
        return lane == JobLane::Pool || lane == JobLane::Audio ? std::max(poolThreads, 1u) : 1u;
    }
}
