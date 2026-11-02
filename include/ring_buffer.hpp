#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

// Standard 64-byte cache line size (prevents false sharing on L1/L2 caches without compiler warnings)
inline constexpr size_t HardwareDestructiveInterferenceSize = 64;
inline constexpr size_t hardware_destructive_interference_size = HardwareDestructiveInterferenceSize;

namespace lob {

/**
 * @brief High-frequency lock-free Single-Producer-Single-Consumer (SPSC) Ring Buffer.
 *
 * Engineered for sub-microsecond trading architectures:
 * - Zero dynamic heap allocation on hot path.
 * - Power-of-two capacity for O(1) bitwise index masking (avoids integer division).
 * - Cache-line aligned (alignas(64)) to eliminate cross-core false sharing between
 *   producer state, consumer state, and data storage.
 * - Shadow/cached head and tail pointers to minimize cross-core atomic synchronization
 *   traffic across the CPU cache coherency interconnect (UPI/Infinity Fabric).
 *
 * @tparam T Element type.
 * @tparam Capacity Power-of-two ring buffer capacity.
 */
template <typename T, size_t Capacity>
class alignas(hardware_destructive_interference_size) RingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0 && Capacity > 0,
                  "RingBuffer Capacity must be a power of 2 for fast bitwise masking.");

public:
    static constexpr size_t BufferCapacity = Capacity;
    static constexpr size_t BufferMask = Capacity - 1;

    constexpr RingBuffer() noexcept
        : head_(0),
          cached_tail_(0),
          tail_(0),
          cached_head_(0) {}

    ~RingBuffer() noexcept {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            T item;
            while (try_pop(item)) {
                // Drain and destruct all active elements
            }
        }
    }

    // Hardware-pinned infrastructure: non-copyable and non-movable
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;

    /**
     * @brief Producer Hot Path: Push item by copy.
     */
    [[nodiscard]] bool try_push(const T& item) noexcept {
        return emplace(item);
    }

    /**
     * @brief Producer Hot Path: Push item by move.
     */
    [[nodiscard]] bool try_push(T&& item) noexcept {
        return emplace(std::move(item));
    }

    /**
     * @brief Producer Hot Path: In-place construction with zero allocation.
     */
    template <typename... Args>
    [[nodiscard]] bool emplace(Args&&... args) noexcept {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);

        // Fast-path: Check cached head first to avoid cross-core L3/coherency bus reads
        if (current_tail - cached_head_ >= Capacity) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (current_tail - cached_head_ >= Capacity) {
                return false; // Queue is full
            }
        }

        Slot& slot = storage_[current_tail & BufferMask];
        ::new (static_cast<void*>(&slot.storage)) T(std::forward<Args>(args)...);

        // Release order to ensure element is completely written before tail becomes visible
        tail_.store(current_tail + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Consumer Hot Path: Pop item into out-parameter.
     */
    [[nodiscard]] bool try_pop(T& value) noexcept {
        const size_t current_head = head_.load(std::memory_order_relaxed);

        // Fast-path: Check cached tail first to avoid bus synchronization
        if (current_head == cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (current_head == cached_tail_) {
                return false; // Queue is empty
            }
        }

        Slot& slot = storage_[current_head & BufferMask];
        T* item_ptr = reinterpret_cast<T*>(&slot.storage);
        value = std::move(*item_ptr);

        if constexpr (!std::is_trivially_destructible_v<T>) {
            item_ptr->~T();
        }

        // Release head to inform producer that the slot is available
        head_.store(current_head + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Consumer Hot Path: Pop item returning std::optional<T>.
     */
    [[nodiscard]] std::optional<T> try_pop() noexcept {
        const size_t current_head = head_.load(std::memory_order_relaxed);

        if (current_head == cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (current_head == cached_tail_) {
                return std::nullopt;
            }
        }

        Slot& slot = storage_[current_head & BufferMask];
        T* item_ptr = reinterpret_cast<T*>(&slot.storage);
        std::optional<T> result(std::move(*item_ptr));

        if constexpr (!std::is_trivially_destructible_v<T>) {
            item_ptr->~T();
        }

        head_.store(current_head + 1, std::memory_order_release);
        return result;
    }

    /**
     * @brief Number of elements currently in the buffer (approximate on concurrent threads).
     */
    [[nodiscard]] size_t size() const noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_relaxed);
        return (tail >= head) ? (tail - head) : 0;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return Capacity;
    }

private:
    struct Slot {
        alignas(alignof(T)) std::byte storage[sizeof(T)];
    };

    // Consumer cache line (64 bytes)
    alignas(hardware_destructive_interference_size) std::atomic<size_t> head_{0};
    size_t cached_tail_{0};

    // Producer cache line (64 bytes)
    alignas(hardware_destructive_interference_size) std::atomic<size_t> tail_{0};
    size_t cached_head_{0};

    // Contiguous storage buffer cache-line aligned
    alignas(hardware_destructive_interference_size) Slot storage_[Capacity];
};

} // namespace lob

