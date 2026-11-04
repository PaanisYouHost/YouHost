#pragma once

namespace youhost
{

// Silent processBlock calls made on the loader thread, before the plugin is
// published into the live graph. The samples are discarded. They are not a
// delay line and they do not add latency. The first live callback is then
// not the plugin's first process.
inline constexpr int kPluginPrimeBlocks = 32;

// One cold process is enough to miss a 32-sample period (666 us at 48 kHz).
// De-Feedback's first block was about 825 us. Later blocks were under the period.
inline bool primeCoversColdProcess(int primeBlocks) noexcept
{
    return primeBlocks >= 1;
}

// After `primeBlocks` discarded calls, the next call index is the first live one.
inline int firstLiveProcessIndex(int primeBlocks) noexcept
{
    if (primeBlocks < 0)
        return 0;
    return primeBlocks;
}

} // namespace youhost
