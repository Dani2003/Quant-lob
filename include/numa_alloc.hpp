#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

#if defined(__linux__)
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MPOL_BIND
#define MPOL_BIND 2
#endif

#ifndef MPOL_MF_MOVE
#define MPOL_MF_MOVE (1 << 1)
#endif
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

namespace lob {

/**
 * @brief Institutional NUMA-Aware Memory Allocator.
 *
 * Ensures memory pool pages are physically allocated on the local memory controller
 * of the specific CPU socket where the trading thread is pinned, eliminating
 * cross-socket interconnect latency (UPI / Infinity Fabric hops).
 */
class NumaMemory {
public:
    /**
     * @brief Allocates NUMA-local, cache-aligned, page-locked memory.
     * @param size Bytes to allocate
     * @param numa_node Target NUMA node ID (e.g., 0 for Socket 0)
     */
    static void* allocate_on_node(size_t size, int numa_node = 0) noexcept {
        // Align up to standard 4KB page boundary
        constexpr size_t PageSize = 4096;
        size_t aligned_size = (size + PageSize - 1) & ~(PageSize - 1);

#if defined(__linux__)
        // Allocate anonymous memory via mmap
        void* ptr = ::mmap(nullptr, aligned_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr == MAP_FAILED) {
            return nullptr;
        }

        // Bind memory to target NUMA node using direct Linux kernel syscall
#if defined(SYS_mbind)
        unsigned long nodemask = (1UL << numa_node);
        long ret = ::syscall(SYS_mbind, ptr, aligned_size, MPOL_BIND, &nodemask, sizeof(nodemask) * 8, MPOL_MF_MOVE);
        (void)ret;
#else
        (void)numa_node;
#endif

        // Lock memory into RAM to prevent OS swapping
        ::mlock(ptr, aligned_size);

        // Pre-fault all pages: touch every 4KB page to eliminate runtime page faults
        auto* byte_ptr = static_cast<volatile uint8_t*>(ptr);
        for (size_t offset = 0; offset < aligned_size; offset += PageSize) {
            byte_ptr[offset] = 0;
        }

        return ptr;

#elif defined(_WIN32)
        // Windows NUMA allocation
        HANDLE process = GetCurrentProcess();
        void* ptr = VirtualAllocExNuma(process, nullptr, aligned_size,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE, numa_node);
        if (!ptr) {
            ptr = VirtualAlloc(nullptr, aligned_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        }
        if (ptr) {
            // Pre-fault pages
            auto* byte_ptr = static_cast<volatile uint8_t*>(ptr);
            for (size_t offset = 0; offset < aligned_size; offset += PageSize) {
                byte_ptr[offset] = 0;
            }
        }
        return ptr;

#else
        (void)numa_node;
        void* ptr = nullptr;
        if (posix_memalign(&ptr, 64, aligned_size) != 0) {
            return nullptr;
        }
        std::memset(ptr, 0, aligned_size);
        return ptr;
#endif
    }

    /**
     * @brief Frees memory allocated via allocate_on_node.
     */
    static void free_memory(void* ptr, size_t size) noexcept {
        if (!ptr) return;
        constexpr size_t PageSize = 4096;
        size_t aligned_size = (size + PageSize - 1) & ~(PageSize - 1);

#if defined(__linux__)
        ::munlock(ptr, aligned_size);
        ::munmap(ptr, aligned_size);
#elif defined(_WIN32)
        (void)aligned_size;
        VirtualFree(ptr, 0, MEM_RELEASE);
#else
        (void)aligned_size;
        ::free(ptr);
#endif
    }
};

} // namespace lob
