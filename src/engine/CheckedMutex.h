#pragma once

#include <cassert>
#include <mutex>

namespace youhost
{

// Non-recursive mutex. A second lock on the same thread asserts in debug
// builds before std::mutex would wait forever. The hold list is thread-local
// and keyed by this mutex, so nesting two different mutexes is not a false hit.
class CheckedMutex
{
public:
    void lock()
    {
#ifndef NDEBUG
        assert(! heldByThisThread() && "re-entered a non-recursive YouHost mutex");
        noteEnter();
#endif
        mutex_.lock();
    }

    void unlock()
    {
        mutex_.unlock();
#ifndef NDEBUG
        noteLeave();
#endif
    }

    bool try_lock()
    {
#ifndef NDEBUG
        if (heldByThisThread())
            return false;
#endif
        if (! mutex_.try_lock())
            return false;
#ifndef NDEBUG
        noteEnter();
#endif
        return true;
    }

    // True when this thread already holds the mutex. lock() asserts on that.
    bool debugReentryWouldAssert() const noexcept
    {
#ifndef NDEBUG
        return heldByThisThread();
#else
        return false;
#endif
    }

private:
    struct Hold
    {
        const CheckedMutex* mutex = nullptr;
        int depth = 0;
    };

    static constexpr int kMaxHolds = 8;

    static Hold* holds() noexcept
    {
        thread_local Hold storage[kMaxHolds] {};
        return storage;
    }

    static int& holdCount() noexcept
    {
        thread_local int count = 0;
        return count;
    }

    bool heldByThisThread() const noexcept
    {
        const int count = holdCount();
        const Hold* list = holds();
        for (int index = 0; index < count; ++index)
            if (list[index].mutex == this && list[index].depth > 0)
                return true;
        return false;
    }

    void noteEnter() noexcept
    {
        Hold* list = holds();
        int& count = holdCount();
        for (int index = 0; index < count; ++index)
        {
            if (list[index].mutex == this)
            {
                ++list[index].depth;
                return;
            }
        }
        if (count < kMaxHolds)
            list[count++] = Hold { this, 1 };
    }

    void noteLeave() noexcept
    {
        Hold* list = holds();
        int& count = holdCount();
        for (int index = 0; index < count; ++index)
        {
            if (list[index].mutex != this)
                continue;
            if (--list[index].depth > 0)
                return;
            list[index] = list[count - 1];
            list[count - 1] = {};
            --count;
            return;
        }
    }

    std::mutex mutex_;
};

} // namespace youhost
