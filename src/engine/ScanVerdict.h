#pragma once

#include <string>
#include <string_view>

namespace youhost
{

// What the scanner observed. A timeout is not a crash: license and iLok
// checks are slow, and a worker that is still alive has not crashed.
enum class ScanKind
{
    ok,
    timeout,
    crash,
    pluginExit,
    workerStart,
    cancelled,
    failed
};

struct ScanObservation
{
    bool cancelled = false;
    bool workerStarted = false;
    bool resultReady = false;
    bool workerAlive = false;
    bool quitRequested = false;
    bool resultOk = false;
};

inline ScanKind classifyScan(const ScanObservation& seen) noexcept
{
    if (seen.cancelled)
        return ScanKind::cancelled;
    if (! seen.workerStarted)
        return ScanKind::workerStart;
    if (seen.resultReady)
        return seen.resultOk ? ScanKind::ok : ScanKind::failed;
    if (seen.workerAlive)
        return ScanKind::timeout;
    if (seen.quitRequested)
        return ScanKind::pluginExit;
    return ScanKind::crash;
}

struct ScanPlan
{
    bool accept = false;
    bool retry = false;
    bool blacklist = false;
    const char* reason = "";
};

// attempt is 0 for the first try. A crash, a plugin exit, a timeout, and a
// worker that never started are tried once more in a fresh process before
// anything is blacklisted. A timeout is never stored as a crash.
inline ScanPlan planScanAttempt(ScanKind kind, int attempt) noexcept
{
    ScanPlan plan;
    if (kind == ScanKind::ok)
    {
        plan.accept = true;
        plan.reason = "ok";
        return plan;
    }
    if (kind == ScanKind::cancelled)
    {
        plan.reason = "cancelled";
        return plan;
    }

    const bool again = attempt < 1;
    plan.retry = again;
    if (kind == ScanKind::timeout)
    {
        plan.reason = again ? "Scan timed out. Retrying once."
                            : "Scan timed out. Not a crash. It can be scanned again.";
        plan.blacklist = false;
        return plan;
    }
    if (kind == ScanKind::workerStart)
    {
        plan.reason = again ? "Scanner could not start. Retrying once."
                            : "Scanner could not start. Not a plugin crash.";
        plan.blacklist = false;
        return plan;
    }
    if (kind == ScanKind::pluginExit)
    {
        plan.reason = again ? "Plugin asked the scanner to quit. Retrying once."
                            : "Plugin asked the scanner to quit. Not marked as a crash.";
        plan.blacklist = ! again;
        return plan;
    }
    if (kind == ScanKind::failed)
    {
        plan.reason = again ? "The plugin failed to scan. Retrying once."
                            : "The plugin failed to scan.";
        plan.blacklist = ! again;
        return plan;
    }
    plan.reason = again ? "Plugin crashed the scanner. Retrying once."
                        : "Plugin crashed the scanner";
    plan.blacklist = ! again;
    return plan;
}

// One module per process. Reusing a worker lets a late license thread from
// the previous plugin kill the process while the next plugin is scanned.
inline constexpr int kScanWorkersPerPlugin = 1;

inline int scanTimeoutMs(bool shell, int attempt) noexcept
{
    if (shell)
        return attempt <= 0 ? 60000 : 120000;
    return attempt <= 0 ? 45000 : 90000;
}

inline bool storedFailureBlacklists(std::string_view reason) noexcept
{
    if (reason.find("Not a crash") != std::string_view::npos)
        return false;
    if (reason.find("Not marked as a crash") != std::string_view::npos)
        return false;
    if (reason.find("Timed out") != std::string_view::npos || reason.find("timed out") != std::string_view::npos)
        return false;
    if (reason.find("could not start") != std::string_view::npos)
        return false;
    return true;
}

} // namespace youhost
