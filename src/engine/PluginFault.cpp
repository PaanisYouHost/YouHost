#include "PluginFault.h"

#include <atomic>

#if defined(__APPLE__) || defined(__linux__)
 #include <pthread.h>
 #include <setjmp.h>
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
#endif

} // namespace

#if defined(__APPLE__) || defined(__linux__)
// Per thread. An audio-thread fault must not jump to the message loop.
thread_local sigjmp_buf messageJump;
thread_local int messageJumpArmed = 0;
MessageFaultRecovery messageRecovery = nullptr;
int messageRecoveryDepth = 0;
#endif

void setMessageFaultRecovery(MessageFaultRecovery recovery) noexcept
{
#if defined(__APPLE__) || defined(__linux__)
    messageRecovery = recovery;
#else
    (void) recovery;
#endif
}

namespace
{

#if defined(__APPLE__) || defined(__linux__)

bool isHostThread() noexcept
{
    return hostThread;
}

void faultHandler(int signalNumber, siginfo_t*, void*)
{
    if (isHostThread() && messageJumpArmed != 0)
    {
        youhost::messageJumpArmed = 0;
        faultPending.store(kPluginFaultMessageThread, std::memory_order_relaxed);
        siglongjmp(youhost::messageJump, 1);
    }

    if (isHostThread())
    {
        struct sigaction restore {};
        restore.sa_handler = SIG_DFL;
        sigaction(signalNumber, &restore, nullptr);
        raise(signalNumber);
        return;
    }

    faultPending.store(kPluginFaultOtherThread, std::memory_order_relaxed);
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

int takePluginFault() noexcept
{
#if defined(__APPLE__) || defined(__linux__)
    return faultPending.exchange(0, std::memory_order_relaxed);
#else
    return 0;
#endif
}

} // namespace youhost

#if defined(__APPLE__) || defined(__linux__)
extern "C" int youhostMessageFaultSetjmp()
{
    if (sigsetjmp(youhost::messageJump, 1) != 0)
    {
        youhost::messageJumpArmed = 0;
        return 1;
    }
    youhost::messageJumpArmed = 1;
    return 0;
}

extern "C" void youhostMessageFaultRecovered()
{
    // Arm again so a fault while the plugin is being released jumps here
    // and returns. One fault during recovery does not call the hook again.
    if (youhost::messageRecoveryDepth != 0)
        return;
    if (sigsetjmp(youhost::messageJump, 1) != 0)
    {
        youhost::messageJumpArmed = 0;
        youhost::messageRecoveryDepth = 0;
        return;
    }
    youhost::messageJumpArmed = 1;
    youhost::messageRecoveryDepth = 1;
    if (youhost::messageRecovery != nullptr)
        youhost::messageRecovery();
    youhost::messageJumpArmed = 0;
    youhost::messageRecoveryDepth = 0;
}
#else
extern "C" int youhostMessageFaultSetjmp()
{
    return 0;
}

extern "C" void youhostMessageFaultRecovered()
{
}
#endif
