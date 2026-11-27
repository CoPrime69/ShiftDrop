#pragma once

namespace sd::platform
{
    // Sets the calling thread up for COM for as long as this lives: single-threaded (an STA, which the
    // shell's and Office's objects need) or multithreaded (an MTA). Without COM there is nothing to do.
    class ComApartmentForThread
    {
    public:
        explicit ComApartmentForThread(bool singleThreaded);
        ~ComApartmentForThread();
        ComApartmentForThread(const ComApartmentForThread&) = delete;
        ComApartmentForThread& operator=(const ComApartmentForThread&) = delete;

    private:
        bool m_joined = false;   // so only a successful join is undone
    };
}
