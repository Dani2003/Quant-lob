#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#ifdef __cpp_lib_hardware_interference_size
using std::hardware_destructive_interference_size;
#else
constexpr size_t hardware_destructive_interference_size = 64;
#endif

namespace lob {

enum class Side : uint8_t {
    Buy  = 0,
    Sell = 1
};

enum class OrderType : uint8_t {
    Limit  = 0,
    Market = 1,
    Cancel = 2
};

/**
 * @brief Cache-line aligned Order structure (64 bytes).
 *
 * Sized and aligned precisely to match a standard CPU L1/L2 cache line (64 bytes).
 * Ensures that fetching an Order into L1 cache triggers exactly one cache line fill
 * without crossing cache line boundaries or inducing false sharing.
 */
struct alignas(hardware_destructive_interference_size) Order {
    uint64_t order_id{0};       // 8 bytes: Unique order identifier
    uint64_t client_id{0};      // 8 bytes: Participant/session ID
    uint64_t timestamp_ns{0};   // 8 bytes: Hardware ingress timestamp (nanoseconds)
    uint32_t price{0};          // 4 bytes: Fixed-point tick price
    uint32_t quantity{0};       // 4 bytes: Order share quantity
    Side side{Side::Buy};       // 1 byte:  Buy/Sell side
    OrderType type{OrderType::Limit}; // 1 byte: Order execution type
    uint8_t padding[30]{0};     // 30 bytes: Explicit padding to round up to exactly 64 bytes

    void reset() noexcept {
        order_id = 0;
        client_id = 0;
        timestamp_ns = 0;
        price = 0;
        quantity = 0;
        side = Side::Buy;
        type = OrderType::Limit;
    }
};

static_assert(sizeof(Order) == 64, "Order struct must be exactly 64 bytes (1 cache line).");
static_assert(alignof(Order) == 64, "Order struct must be 64-byte aligned.");

/**
 * @brief High-frequency pre-allocated Memory Object Pool.
 *
 * Eliminates OS kernel heap allocations (malloc/free, new/delete) on the critical hot path.
 * Pre-allocates a contiguous chunk of memory and maintains an intrusive O(1) singly linked free list.
 *
 * @tparam T Object type to allocate.
 * @tparam Capacity Maximum number of pre-allocated objects.
 */
template <typename T, size_t Capacity>
class alignas(hardware_destructive_interference_size) MemoryPool {
public:
    static constexpr size_t PoolCapacity = Capacity;

    constexpr MemoryPool() noexcept {
        initialize_free_list();
    }

    ~MemoryPool() noexcept = default;

    // Hot-path memory pools are fixed in physical RAM
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&) = delete;
    MemoryPool& operator=(MemoryPool&&) = delete;

    /**
     * @brief Hot-path O(1) allocation. In-place placement new construction.
     * @return Pointer to initialized object, or nullptr if pool exhausted.
     */
    template <typename... Args>
    [[nodiscard]] T* allocate(Args&&... args) noexcept {
        if (!free_head_) [[unlikely]] {
            return nullptr;
        }

        Slot* slot = free_head_;
        free_head_ = slot->next;
        --available_;

        T* obj = reinterpret_cast<T*>(&slot->storage);
        ::new (static_cast<void*>(obj)) T(std::forward<Args>(args)...);
        return obj;
    }

    /**
     * @brief Hot-path O(1) deallocation. Destructs object and returns slot to free list.
     */
    void deallocate(T* ptr) noexcept {
        if (!ptr) [[unlikely]] {
            return;
        }

        if constexpr (!std::is_trivially_destructible_v<T>) {
            ptr->~T();
        }

        auto* slot = reinterpret_cast<Slot*>(ptr);
        slot->next = free_head_;
        free_head_ = slot;
        ++available_;
    }

    [[nodiscard]] size_t available() const noexcept {
        return available_;
    }

    [[nodiscard]] size_t capacity() const noexcept {
        return Capacity;
    }

    [[nodiscard]] bool empty() const noexcept {
        return available_ == 0;
    }

    void reset() noexcept {
        initialize_free_list();
    }

private:
    union Slot {
        alignas(alignof(T)) std::byte storage[sizeof(T)];
        Slot* next;

        Slot() noexcept {}
        ~Slot() noexcept {}
    };

    void initialize_free_list() noexcept {
        for (size_t i = 0; i < Capacity - 1; ++i) {
            storage_[i].next = &storage_[i + 1];
        }
        storage_[Capacity - 1].next = nullptr;
        free_head_ = &storage_[0];
        available_ = Capacity;
    }

    // Contiguous pre-allocated buffer of slots
    alignas(hardware_destructive_interference_size) Slot storage_[Capacity];
    alignas(hardware_destructive_interference_size) Slot* free_head_{nullptr};
    size_t available_{Capacity};
};

} // namespace lob

