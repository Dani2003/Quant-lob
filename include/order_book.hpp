#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "memory_pool.hpp"

namespace lob {

/**
 * @brief Trade execution event generated on order matching.
 */
struct Trade {
    uint64_t maker_order_id{0};
    uint64_t taker_order_id{0};
    uint32_t price{0};          // Executed price (maker price)
    uint32_t quantity{0};       // Executed fill quantity
    Side taker_side{Side::Buy}; // Side of the incoming aggressive order
    uint64_t timestamp_ns{0};
};

/**
 * @brief Trade listener callback function pointer for zero-overhead event reporting.
 */
using TradeCallback = void (*)(const Trade&, void* user_data);

struct PriceLevel;

/**
 * @brief Intrusive doubly-linked list node wrapping an Order.
 * Cache-line aligned (alignas(64)) to optimize CPU L1 cache line utilization.
 */
struct alignas(hardware_destructive_interference_size) OrderNode {
    Order order;
    OrderNode* prev{nullptr};
    OrderNode* next{nullptr};
    PriceLevel* level{nullptr};
};

/**
 * @brief Doubly-linked Price Level containing a FIFO queue of resting orders.
 */
struct PriceLevel {
    uint32_t price{0};
    uint32_t total_volume{0};
    uint32_t order_count{0};
    OrderNode* head{nullptr};
    OrderNode* tail{nullptr};

    // Intrusive pointers for sorted price ladder traversal
    PriceLevel* prev_level{nullptr};
    PriceLevel* next_level{nullptr};

    [[nodiscard]] bool empty() const noexcept {
        return head == nullptr;
    }

    void append(OrderNode* node) noexcept {
        node->level = this;
        node->next = nullptr;
        node->prev = tail;
        if (tail) {
            tail->next = node;
        } else {
            head = node;
        }
        tail = node;
        total_volume += node->order.quantity;
        ++order_count;
    }

    void remove(OrderNode* node) noexcept {
        if (node->prev) {
            node->prev->next = node->next;
        } else {
            head = node->next;
        }
        if (node->next) {
            node->next->prev = node->prev;
        } else {
            tail = node->prev;
        }
        node->prev = nullptr;
        node->next = nullptr;
        node->level = nullptr;
        --order_count;
    }
};

/**
 * @brief Summary statistics for a single price level.
 */
struct PriceLevelInfo {
    uint32_t price{0};
    uint32_t total_volume{0};
    uint32_t order_count{0};
};

/**
 * @brief Market depth snapshot (Level 2).
 */
struct MarketDepth {
    static constexpr size_t MAX_DEPTH = 10;
    PriceLevelInfo bids[MAX_DEPTH];
    PriceLevelInfo asks[MAX_DEPTH];
    size_t bid_depth{0};
    size_t ask_depth{0};
};

/**
 * @brief Contiguous Top-5 bid/ask snapshot designed specifically for zero-copy FFI and SIMD.
 * Fixed 32-bit integer arrays aligned to 16 bytes for direct vector load instructions.
 */
struct alignas(16) Top5BookView {
    uint32_t bid_prices[5]{0};
    uint32_t bid_volumes[5]{0};
    uint32_t ask_prices[5]{0};
    uint32_t ask_volumes[5]{0};
    uint32_t bid_depth{0};
    uint32_t ask_depth{0};
};

/**
 * @brief Sub-microsecond Price-Time Priority Limit Order Book (LOB).
 *
 * Engineered for HFT production requirements:
 * - Strict Price-Time (FIFO) priority matching.
 * - Flat price array with direct O(1) indexing (zero std::map heap allocations).
 * - Intrusive doubly-linked active price ladders for instantaneous best bid/ask access.
 * - Pre-allocated MemoryPool for order nodes (zero malloc/new on hot path).
 * - Pre-allocated open-addressing hash table for O(1) order cancellations by ID.
 */
class alignas(hardware_destructive_interference_size) OrderBook {
public:
    static constexpr size_t MAX_ORDERS = 262'144;           // Pre-allocated order pool capacity
    static constexpr uint32_t MAX_PRICE_TICKS = 200'000;    // Tick range ($0.01 to $2,000.00)
    static constexpr size_t ORDER_MAP_CAPACITY = 524'288;   // Power of 2 for fast bitwise masking
    static constexpr size_t ORDER_MAP_MASK = ORDER_MAP_CAPACITY - 1;
    static constexpr uint64_t TOMBSTONE = 0xFFFFFFFFFFFFFFFFULL;

    OrderBook();
    ~OrderBook() = default;

    // Fixed infrastructure: non-copyable and non-movable
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;

    /**
     * @brief Hot-path Limit Order insertion & matching.
     * @return true if order was successfully processed (matched and/or rested).
     */
    [[nodiscard]] bool add_limit_order(uint64_t order_id,
                                       uint64_t client_id,
                                       Side side,
                                       uint32_t price,
                                       uint32_t quantity,
                                       uint64_t timestamp_ns = 0) noexcept;

    /**
     * @brief Hot-path Market Order execution.
     * Immediately matches against resting opposite book until filled or book exhausted.
     * @return true if order matched at least 1 unit.
     */
    [[nodiscard]] bool add_market_order(uint64_t order_id,
                                        uint64_t client_id,
                                        Side side,
                                        uint32_t quantity,
                                        uint64_t timestamp_ns = 0) noexcept;

    /**
     * @brief Hot-path Order Cancellation by ID in O(1).
     * @return true if order existed and was cancelled; false if not found.
     */
    [[nodiscard]] bool cancel_order(uint64_t order_id) noexcept;

    // Top-of-Book and Market Data Queries
    [[nodiscard]] std::optional<PriceLevelInfo> get_best_bid() const noexcept;
    [[nodiscard]] std::optional<PriceLevelInfo> get_best_ask() const noexcept;
    [[nodiscard]] uint32_t get_spread() const noexcept;
    [[nodiscard]] double get_mid_price() const noexcept;

    [[nodiscard]] size_t get_resting_order_count() const noexcept { return total_resting_orders_; }
    [[nodiscard]] uint64_t get_total_trades_count() const noexcept { return total_trades_; }
    [[nodiscard]] uint64_t get_total_matched_volume() const noexcept { return total_matched_volume_; }

    [[nodiscard]] MarketDepth get_depth(size_t max_levels = 5) const noexcept;
    void get_top5_view(Top5BookView& out) const noexcept;

    void set_trade_callback(TradeCallback callback, void* user_data = nullptr) noexcept {
        trade_callback_ = callback;
        trade_callback_user_data_ = user_data;
    }

    void clear() noexcept;

private:
    struct OrderMapEntry {
        uint64_t order_id{0};
        OrderNode* node{nullptr};
    };

    // Internal matching engines
    void match_buy_limit(uint64_t order_id, uint64_t client_id, uint32_t price, uint32_t& remaining_qty, uint64_t timestamp_ns) noexcept;
    void match_sell_limit(uint64_t order_id, uint64_t client_id, uint32_t price, uint32_t& remaining_qty, uint64_t timestamp_ns) noexcept;

    // Price ladder maintenance
    void link_bid_level(PriceLevel* level) noexcept;
    void unlink_bid_level(PriceLevel* level) noexcept;
    void link_ask_level(PriceLevel* level) noexcept;
    void unlink_ask_level(PriceLevel* level) noexcept;

    // Order hash map operations
    void insert_order_map(uint64_t order_id, OrderNode* node) noexcept;
    [[nodiscard]] OrderNode* find_order_map(uint64_t order_id) const noexcept;
    void remove_order_map(uint64_t order_id) noexcept;

    static inline size_t hash_order_id(uint64_t x) noexcept {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return static_cast<size_t>(x);
    }

    inline void record_trade(uint64_t maker_id, uint64_t taker_id, uint32_t price, uint32_t qty, Side taker_side, uint64_t ts) noexcept {
        ++total_trades_;
        total_matched_volume_ += qty;
        if (trade_callback_) [[unlikely]] {
            trade_callback_(Trade{maker_id, taker_id, price, qty, taker_side, ts}, trade_callback_user_data_);
        }
    }

    // Pre-allocated MemoryPool for order nodes (zero hot-path dynamic memory)
    std::unique_ptr<MemoryPool<OrderNode, MAX_ORDERS>> order_pool_;

    // Flat pre-allocated array of price levels for O(1) tick access
    std::unique_ptr<PriceLevel[]> price_levels_;

    // Pre-allocated flat open-addressing order hash table for O(1) cancellations
    std::unique_ptr<OrderMapEntry[]> order_map_;

    // Active sorted price ladders
    PriceLevel* best_bid_level_{nullptr};
    PriceLevel* best_ask_level_{nullptr};

    // Metrics
    size_t total_resting_orders_{0};
    uint64_t total_trades_{0};
    uint64_t total_matched_volume_{0};

    // Callback
    TradeCallback trade_callback_{nullptr};
    void* trade_callback_user_data_{nullptr};
};

} // namespace lob