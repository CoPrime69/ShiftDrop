#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace sd
{
    // A small worker pool. Tasks run on background threads (each with COM initialised as MTA, which
    // WIC needs) and never on the UI thread. Destroying the queue asks running tasks to stop through
    // their stop_token, drops tasks still waiting, and joins the threads.
    class JobQueue
    {
    public:
        using Task = std::function<void(std::stop_token)>;

        explicit JobQueue(unsigned threads);
        ~JobQueue();
        JobQueue(const JobQueue&) = delete;
        JobQueue& operator=(const JobQueue&) = delete;

        void Submit(Task task);
        size_t Pending() const;

    private:
        void Run(std::stop_token stop);

        mutable std::mutex m_mutex;
        std::condition_variable_any m_cv;
        std::deque<Task> m_tasks;
        std::vector<std::jthread> m_threads;   // last member: destroyed (stopped + joined) first
    };
}
