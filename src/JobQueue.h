#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace sd
{
    enum class JobLane;   // Engine.h

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

    // The app's queues, one per JobLane (main.cpp routes every job through this). The pool and the
    // audio lane get PoolThreads() workers; media, shell and office one each. Shell and office are
    // STA. Office exports have a lane of their own, so a slow Word or PowerPoint export never holds
    // up Move to... or Rename... on the shell lane.
    class JobLanes
    {
    public:
        explicit JobLanes(unsigned poolThreads = JobQueue::PoolThreads());
        // Stops and joins the lanes one by one: the pool, media (a running ffmpeg is killed through its
        // job object), audio, office (a running export is cancelled), then shell, which waits for a move
        // in progress because IFileOperation can't be cancelled halfway.
        ~JobLanes();
        JobLanes(const JobLanes&) = delete;
        JobLanes& operator=(const JobLanes&) = delete;

        JobQueue& For(JobLane lane);
        // How many workers `lane` gets.
        static unsigned Threads(JobLane lane, unsigned poolThreads = JobQueue::PoolThreads());

    private:
        std::unique_ptr<JobQueue> m_pool, m_media, m_audio, m_office, m_shell;
    };
}
