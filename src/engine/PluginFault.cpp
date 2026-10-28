#include "PluginFault.h"

#include <atomic>
#include <cstdint>

#if defined(__APPLE__) || defined(__linux__)
 #include <pthread.h>
 #include <signal.h>
 #include <unistd.h>
#endif

namespace youhost
{
namespace
{

#if defined(__APPLE__) || defined(__linux__)
thread_local bool hostThread = false;
std::atomic<int> faultPending { 0 };
bool installed = false;

bool isHostThread() noexcept
{
    return hostThread;
}

void faultHandler(int signalNumber, siginfo_t*, void*)
{
    if (isHostThread())
    {
        struct sigaction restore {};
        restore.sa_handler = SIG_DFL;
        sigaction(signalNumber, &restore, nullptr);
        raise(signalNumber);
        return;
    }

    faultPending.store(1, std::memory_order_relaxed);
    for (;;)
        pause();
}

void installOne(int signalNumber)
{
    struct sigaction action {};
    action.sa_sigaction = faultHandler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(signalNumber, &action, nullptr);
}
#endif

} // namespace

void installPluginFaultGuard()
{
#if defined(__APPLE__) || defined(__linux__)
    if (installed)
        return;
    installed = true;
    hostThread = true;
    installOne(SIGSEGV);
    installOne(SIGBUS);
#else
#endif
}

void noteAudioThread() noexcept
{
#if defined(__APPLE__) || defined(__linux__)
    hostThread = true;
#else
#endif
}

bool takePluginFault() noexcept
{
#if defined(__APPLE__) || defined(__linux__)
    return faultPending.exchange(0, std::memory_order_relaxed) != 0;
#else
    return false;
#endif
}

} // namespace youhost
