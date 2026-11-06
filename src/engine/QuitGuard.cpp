#include "QuitGuard.h"
#include "QuitPolicy.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#if defined(__APPLE__)
 #include <dlfcn.h>
 #include <execinfo.h>
 #include <fcntl.h>
 #include <unistd.h>
#endif

namespace youhost
{
namespace
{

std::atomic<int> userQuitArmed { 0 };
std::atomic<int> processExitAllowed { 0 };
char quitLogPath[768] {};
char quitMarkDirectory[768] {};

void writeQuitMarker()
{
#if defined(__APPLE__)
    if (quitMarkDirectory[0] == '\0')
        return;
    char path[900];
    std::snprintf(path, sizeof(path), "%s/quit-request", quitMarkDirectory);
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
    {
        const char mark[] = "exit\n";
        (void) ::write(fd, mark, sizeof(mark) - 1);
        ::close(fd);
    }
#else
    (void) 0;
#endif
}

void logQuitRequest(const char* how, int code)
{
#if defined(__APPLE__)
    const int fd = quitLogPath[0] != '\0' ? ::open(quitLogPath, O_WRONLY | O_APPEND | O_CREAT, 0644) : 2;
    if (fd < 0)
        return;
    char line[160];
    const int length = std::snprintf(line, sizeof(line), "\nquit request %s code %d\n", how != nullptr ? how : "quit", code);
    if (length > 0)
        (void) ::write(fd, line, static_cast<std::size_t>(length));
    void* frames[32];
    const int count = ::backtrace(frames, 32);
    if (count > 0)
        ::backtrace_symbols_fd(frames, count, fd);
    if (fd != 2)
        ::close(fd);
#else
    (void) how;
    (void) code;
#endif
    writeQuitMarker();
}

#if defined(__APPLE__)
using ExitFn = void (*)(int);

void youhostInterposedExit(int status)
{
    logQuitRequest("exit", status);
    if (refuseUnsolicitedQuit(userQuitArmed.load(std::memory_order_acquire) != 0,
                              false,
                              processExitAllowed.load(std::memory_order_acquire) != 0))
        return;
    auto* next = reinterpret_cast<ExitFn>(dlsym(RTLD_NEXT, "exit"));
    if (next != nullptr)
        next(status);
}

void youhostInterposedUnderscoreExit(int status)
{
    logQuitRequest("_exit", status);
    if (refuseUnsolicitedQuit(userQuitArmed.load(std::memory_order_acquire) != 0,
                              false,
                              processExitAllowed.load(std::memory_order_acquire) != 0))
        return;
    auto* next = reinterpret_cast<ExitFn>(dlsym(RTLD_NEXT, "_exit"));
    if (next != nullptr)
        next(status);
}

#define YOUHOST_INTERPOSE(_replacement, _replacee)                                                                     \
    __attribute__((used)) static const struct                                                                          \
    {                                                                                                                  \
        const void* replacement;                                                                                      \
        const void* replacee;                                                                                         \
    } youhostInterpose##_replacee __attribute__((section("__DATA,__interpose")))                                       \
        = { reinterpret_cast<const void*>(_replacement), reinterpret_cast<const void*>(_replacee) }

YOUHOST_INTERPOSE(youhostInterposedExit, exit);
YOUHOST_INTERPOSE(youhostInterposedUnderscoreExit, _exit);
#endif

} // namespace

void noteQuitLogPath(const char* path) noexcept
{
    if (path == nullptr)
        path = "";
    std::strncpy(quitLogPath, path, sizeof(quitLogPath) - 1);
    quitLogPath[sizeof(quitLogPath) - 1] = '\0';
}

void noteQuitMarkDirectory(const char* path) noexcept
{
    if (path == nullptr)
        path = "";
    std::strncpy(quitMarkDirectory, path, sizeof(quitMarkDirectory) - 1);
    quitMarkDirectory[sizeof(quitMarkDirectory) - 1] = '\0';
}

void armUserQuit() noexcept
{
    userQuitArmed.store(1, std::memory_order_release);
}

void allowProcessExit() noexcept
{
    processExitAllowed.store(1, std::memory_order_release);
}

bool consumeUserQuit() noexcept
{
    return userQuitArmed.exchange(0, std::memory_order_acq_rel) != 0;
}

#if ! defined(__APPLE__)
bool macCurrentEventIsUserQuit() noexcept
{
    return false;
}

void installCommandQuitMonitor() noexcept
{
}
#endif

bool shouldRefuseQuitRequest() noexcept
{
    const bool armed = consumeUserQuit();
    const bool event = macCurrentEventIsUserQuit();
    const bool allowed = processExitAllowed.load(std::memory_order_acquire) != 0;
    if (refuseUnsolicitedQuit(armed, event, allowed))
        return true;
    allowProcessExit();
    return false;
}

} // namespace youhost
