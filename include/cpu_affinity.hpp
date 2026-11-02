#pragma once

#include <iostream>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace lob {

/**
 * @brief Thread affinity utility to pin critical matching & parsing threads
 * to isolated CPU cores, preventing L1/L2 cache evictions and OS context switching.
 */
class CpuAffinity {
public:
    /**
     * @brief Pins the calling thread to a specific hardware CPU core.
     * @param core_id Hardware core ID (0-based index).
     * @return true if affinity successfully established.
     */
    static bool pin_current_thread(size_t core_id) noexcept {
#if defined(__linux__)
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);

        const int result = pthread_setaffinity_np(
            pthread_self(), sizeof(cpu_set_t), &cpuset);
        return (result == 0);
#elif defined(_WIN32)
        const DWORD_PTR mask = static_cast<DWORD_PTR>(1ULL << core_id);
        const DWORD_PTR result = SetThreadAffinityMask(GetCurrentThread(), mask);
        return (result != 0);
#else
        (void)core_id;
        return false;
#endif
    }
};

} // namespace lob

