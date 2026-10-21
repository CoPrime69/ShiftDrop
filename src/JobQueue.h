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
    // A small worker pool. Tasks run on background threads and never on the UI thread. Each thread
    // initialises COM in the apartment the queue was made for: MTA for converters (WIC is free-threaded),
    // STA for shell work (IFileOperation is only supported on STA threads). Destroying the queue asks running tasks to stop through
    // their stop_token, drops tasks still waiting, and joins the threads.
    class JobQueue
    {
    public:
        using Task = std::function<void(std::stop_token)>;

        enum class Apartment { Mta, Sta };
        explicit JobQueue(unsigned threads, Apartment apartment = Apartment::Mta);

        // Workers for jobs that each keep one core busy (images, sound through ffmpeg): one per
        // logical processor but one, so the rest of the PC stays responsive, and at most 8 (a 12 MP
        // photo needs about 150 MB while it converts).
        static unsigned PoolThreads();
        ~JobQueue();
        JobQueue(const JobQueue&) = delete;
        JobQueue& operator=(const JobQueue&) = delete;

        void Submit(Task task);
        size_t Pending() const;

    private:
        void Run(std::stop_token stop);

        Apartment m_apartment;

        mutable std::mutex m_mutex;
        std::condition_variable_any m_cv;
        std::deque<Task> m_tasks;
        std::vector<std::jthread> m_threads;   // last member: destroyed (stopped + joined) first
    };
}
