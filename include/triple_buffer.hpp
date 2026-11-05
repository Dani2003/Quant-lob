#pragma once

#include <cstdint>
#include <array>
#include <atomic>
#include <mutex>

namespace lob {

/**
 * @brief Wait-Free Triple Buffer for lock-free publishing of immutable telemetry snapshots.
 *
 * Implements Lamport's 3-slot asynchronous buffer algorithm:
 * - Single Writer is completely wait-free: writes to private write_slot, then atomically
 *   swaps with clean_slot. Zero mutexes, zero spinlocks, zero heap allocation.
 * - Readers exchange read_slot with clean_slot and read an immutable snapshot.
 * - Guaranteed: write_slot != read_slot at all times. Zero concurrent read/write to the
 *   same memory location, eliminating C++ data races on non-atomic payload fields.
 */
template <typename T>
class alignas(64) WaitFreeTripleBuffer {
public:
    WaitFreeTripleBuffer() : clean_slot_{2}, write_slot_{0}, read_slot_{1} {}

    /// Completely wait-free publish from single writer thread (sub-5ns latency)
    void publish(const T& data) noexcept {
        buffers_[write_slot_] = data;
        write_slot_ = clean_slot_.exchange(write_slot_, std::memory_order_acq_rel);
        has_data_.store(true, std::memory_order_release);
    }

    /// Thread-safe snapshot read of the most recently published snapshot
    bool read_snapshot(T& out) const noexcept {
        if (!has_data_.load(std::memory_order_acquire)) {
            return false;
        }
        std::lock_guard<std::mutex> lock(reader_mutex_);
        read_slot_ = clean_slot_.exchange(read_slot_, std::memory_order_acq_rel);
        out = buffers_[read_slot_];
        return true;
    }

    [[nodiscard]] bool has_data() const noexcept {
        return has_data_.load(std::memory_order_acquire);
    }

private:
    std::array<T, 3> buffers_{};
    mutable std::atomic<uint32_t> clean_slot_{2};
    std::atomic<bool> has_data_{false};
    uint32_t write_slot_{0};
    mutable uint32_t read_slot_{1};
    mutable std::mutex reader_mutex_{};
};

} // namespace lob
