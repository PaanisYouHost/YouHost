#pragma once

namespace youhost
{

// The audio thread is one core. Each worker thread is another. A single
// channel chain stays on the audio thread. More chains than cores share cores.
inline int chainCores(int jobs, int workerThreads) noexcept
{
    if (jobs <= 1 || workerThreads <= 0)
        return 1;
    const int available = workerThreads + 1;
    return jobs < available ? jobs : available;
}

// Contiguous slice of [0, jobs) for this core. The slices cover every job once.
inline void chainSlice(int jobs, int cores, int core, int& begin, int& end) noexcept
{
    begin = 0;
    end = 0;
    if (jobs <= 0 || cores <= 0 || core < 0 || core >= cores)
        return;
    const int base = jobs / cores;
    const int extra = jobs % cores;
    begin = core * base + (core < extra ? core : extra);
    end = begin + base + (core < extra ? 1 : 0);
}

} // namespace youhost
