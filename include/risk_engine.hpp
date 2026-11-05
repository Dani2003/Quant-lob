#pragma once

#include <cstdint>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cmath>

namespace lob {

/// Cache-line aligned size to prevent false sharing
inline constexpr size_t CacheLineSize = 64;

/// Result codes returned by the Pre-Trade Risk Engine
enum class RiskCheckResult : uint8_t {
    Approved = 0,
    KillSwitchActive = 1,
    ExceedsMaxOrderSize = 2,
    ExceedsMaxNotional = 3,
    PriceCollarBreached = 4,
    OrderRateLimitExceeded = 5,
    PositionLimitBreached = 6,
    DrawdownLimitBreached = 7,
    InvalidPriceOrQuantity = 8
};

inline const char* risk_result_to_string(RiskCheckResult res) {
    switch (res) {
        case RiskCheckResult::Approved: return "Approved";
        case RiskCheckResult::KillSwitchActive: return "KillSwitchActive";
        case RiskCheckResult::ExceedsMaxOrderSize: return "ExceedsMaxOrderSize";
        case RiskCheckResult::ExceedsMaxNotional: return "ExceedsMaxNotional";
        case RiskCheckResult::PriceCollarBreached: return "PriceCollarBreached";
        case RiskCheckResult::OrderRateLimitExceeded: return "OrderRateLimitExceeded";
        case RiskCheckResult::PositionLimitBreached: return "PositionLimitBreached";
        case RiskCheckResult::DrawdownLimitBreached: return "DrawdownLimitBreached";
        case RiskCheckResult::InvalidPriceOrQuantity: return "InvalidPriceOrQuantity";
        default: return "Unknown";
    }
}

/// Production Configuration for Pre-Trade Risk Guardrails
struct RiskConfig {
    uint32_t max_order_qty{5'000};           // Max contracts/shares per single order
    uint64_t max_order_notional{500'000'00}; // Max notional per order ($500,000 in cents)
    int64_t  max_net_position{10'000};       // Max net open position (+/- shares)
    uint32_t price_collar_ticks{50};         // Max deviation in ticks from BBO mid-price
    uint32_t max_orders_per_second{100'000}; // Leaky bucket rate limit
    int64_t  max_drawdown_limit{-50'000'00}; // Global circuit breaker loss ($50,000 in cents)
};

/// High-Performance, Sub-Microsecond Pre-Trade Risk Engine
/// Enforces regulatory (SEC Rule 15c3-5 / MiFID II RTS 6) and firm-level risk checks
/// with sub-30ns critical path latency and zero heap allocations.
class alignas(CacheLineSize) RiskEngine {
public:
    explicit RiskEngine(const RiskConfig& config = RiskConfig{})
        : config_(config),
          kill_switch_(false),
          current_position_(0),
          realized_pnl_(0),
          unrealized_pnl_(0),
          order_rate_count_(0),
          last_rate_window_sec_(0) {}

    /// Hot-Path Pre-Trade Risk Evaluation (Target: < 30 nanoseconds)
    /// @param side: 0 = Buy, 1 = Sell
    /// @param price: Order price in ticks/cents
    /// @param qty: Order quantity
    /// @param mid_price: Current reference BBO mid-price (0 if book is empty)
    /// @param timestamp_ns: Current TSC timestamp in nanoseconds
    [[nodiscard]] inline RiskCheckResult check_order(
        uint8_t side,
        uint32_t price,
        uint32_t qty,
        uint32_t mid_price,
        uint64_t timestamp_ns) noexcept 
    {
        // 1. Hardware Kill-Switch (Lowest overhead check first)
        if (__builtin_expect(kill_switch_.load(std::memory_order_relaxed), 0)) {
            return RiskCheckResult::KillSwitchActive;
        }

        // 2. Basic Sanity Check
        if (__builtin_expect(price == 0 || qty == 0, 0)) {
            return RiskCheckResult::InvalidPriceOrQuantity;
        }

        // 3. Max Order Quantity (Fat-finger size protection)
        if (__builtin_expect(qty > config_.max_order_qty, 0)) {
            return RiskCheckResult::ExceedsMaxOrderSize;
        }

        // 4. Max Order Notional (Price * Qty)
        uint64_t notional = static_cast<uint64_t>(price) * static_cast<uint64_t>(qty);
        if (__builtin_expect(notional > config_.max_order_notional, 0)) {
            return RiskCheckResult::ExceedsMaxNotional;
        }

        // 5. Price Collar (Prevents crossing book through stale or aberrant limit prices)
        if (__builtin_expect(mid_price > 0, 1)) {
            uint32_t diff = (price > mid_price) ? (price - mid_price) : (mid_price - price);
            if (__builtin_expect(diff > config_.price_collar_ticks, 0)) {
                return RiskCheckResult::PriceCollarBreached;
            }
        }

        // 6. Net Position Limit (Potential inventory after this order executes)
        const int64_t current_position = current_position_.load(std::memory_order_relaxed);
        const int64_t max_after_buys = current_position + pending_buy_qty_.load(std::memory_order_relaxed) +
                                       (side == 0 ? static_cast<int64_t>(qty) : 0);
        const int64_t min_after_sells = current_position - pending_sell_qty_.load(std::memory_order_relaxed) -
                                        (side == 1 ? static_cast<int64_t>(qty) : 0);
        if (__builtin_expect(max_after_buys > config_.max_net_position ||
                             min_after_sells < -config_.max_net_position, 0)) {
            return RiskCheckResult::PositionLimitBreached;
        }

        // 7. High-Resolution Order Rate Limiting (1-second sliding window)
        uint64_t current_sec = timestamp_ns / 1'000'000'000ULL;
        if (__builtin_expect(current_sec != last_rate_window_sec_, 0)) {
            last_rate_window_sec_ = current_sec;
            order_rate_count_ = 0;
        }
        if (__builtin_expect(++order_rate_count_ > config_.max_orders_per_second, 0)) {
            return RiskCheckResult::OrderRateLimitExceeded;
        }

        // 8. Cumulative PnL Drawdown Breaker
        int64_t total_pnl = realized_pnl_ + unrealized_pnl_;
        if (__builtin_expect(total_pnl < config_.max_drawdown_limit, 0)) {
            kill_switch_.store(true, std::memory_order_release);
            return RiskCheckResult::DrawdownLimitBreached;
        }

        return RiskCheckResult::Approved;
    }

    inline void reserve_order(uint8_t side, uint32_t qty) noexcept {
        if (side == 0) pending_buy_qty_.fetch_add(qty, std::memory_order_relaxed);
        else if (side == 1) pending_sell_qty_.fetch_add(qty, std::memory_order_relaxed);
    }

    inline void release_order(uint8_t side, uint32_t qty) noexcept {
        auto& pending = (side == 0) ? pending_buy_qty_ : pending_sell_qty_;
        int64_t current = pending.load(std::memory_order_relaxed);
        while (current > 0 && !pending.compare_exchange_weak(
            current, std::max<int64_t>(0, current - static_cast<int64_t>(qty)),
            std::memory_order_relaxed)) {}
    }

    /// On-Fill accounting update: Updates inventory, cash, and realized PnL
    inline void on_fill(uint8_t side, uint32_t fill_price, uint32_t fill_qty) noexcept {
        int64_t signed_qty = (side == 0) ? static_cast<int64_t>(fill_qty) : -static_cast<int64_t>(fill_qty);
        int64_t prev_position = current_position_.load(std::memory_order_relaxed);
        current_position_.store(prev_position + signed_qty, std::memory_order_relaxed);

        const bool reducing = (prev_position > 0 && side == 1) || (prev_position < 0 && side == 0);
        const int64_t closing_qty = reducing
            ? std::min(std::abs(prev_position), static_cast<int64_t>(fill_qty)) : 0;
        if (reducing) {
            int64_t price_delta = (prev_position > 0) ? 
                (static_cast<int64_t>(fill_price) - avg_entry_price_) : 
                (avg_entry_price_ - static_cast<int64_t>(fill_price));
            realized_pnl_ += price_delta * closing_qty;
        }

        const int64_t new_position = prev_position + signed_qty;
        if (new_position == 0) {
            avg_entry_price_ = 0;
        } else if (reducing) {
            if (static_cast<uint64_t>(fill_qty) > static_cast<uint64_t>(closing_qty))
                avg_entry_price_ = fill_price;
        } else {
            const uint64_t old_abs = static_cast<uint64_t>(std::abs(prev_position));
            const uint64_t total_abs = static_cast<uint64_t>(std::abs(new_position));
            const uint64_t total_value = old_abs * static_cast<uint64_t>(avg_entry_price_) +
                                         static_cast<uint64_t>(fill_qty) * static_cast<uint64_t>(fill_price);
            avg_entry_price_ = static_cast<int64_t>(total_value / total_abs);
        }
    }

    /// Mark-to-market valuation against current BBO mid-price
    inline void mark_to_market(uint32_t mid_price) noexcept {
        const int64_t position = current_position_.load(std::memory_order_relaxed);
        if (position != 0 && mid_price > 0 && avg_entry_price_ > 0) {
            unrealized_pnl_ = (position > 0) ?
                (static_cast<int64_t>(mid_price) - avg_entry_price_) * position :
                (avg_entry_price_ - static_cast<int64_t>(mid_price)) * (-position);
        } else {
            unrealized_pnl_ = 0;
        }
    }

    /// Emergency Kill Switch - instantly blocks all outgoing orders
    inline void trigger_kill_switch() noexcept {
        kill_switch_.store(true, std::memory_order_release);
    }

    /// Reset Kill Switch after manual review
    inline void reset_kill_switch() noexcept {
        kill_switch_.store(false, std::memory_order_release);
    }

    [[nodiscard]] inline bool is_kill_switch_active() const noexcept {
        return kill_switch_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] inline int64_t current_position() const noexcept { return current_position_.load(std::memory_order_relaxed); }
    [[nodiscard]] inline int64_t realized_pnl() const noexcept { return realized_pnl_; }
    [[nodiscard]] inline int64_t unrealized_pnl() const noexcept { return unrealized_pnl_; }
    [[nodiscard]] inline int64_t total_pnl() const noexcept { return realized_pnl_ + unrealized_pnl_; }
    [[nodiscard]] inline const RiskConfig& config() const noexcept { return config_; }

private:
    RiskConfig config_;
    alignas(CacheLineSize) std::atomic<bool> kill_switch_{false};

    alignas(CacheLineSize) std::atomic<int64_t> current_position_{0};
    alignas(CacheLineSize) std::atomic<int64_t> pending_buy_qty_{0};
    alignas(CacheLineSize) std::atomic<int64_t> pending_sell_qty_{0};
    int64_t avg_entry_price_{0};
    int64_t realized_pnl_{0};
    int64_t unrealized_pnl_{0};

    alignas(CacheLineSize) uint32_t order_rate_count_{0};
    uint64_t last_rate_window_sec_{0};
};

} // namespace lob

