#pragma once

#include <cstdint>
#include <array>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <iomanip>

namespace lob {

/// Standard post-trade markout horizons in nanoseconds
inline constexpr uint64_t HORIZON_100US = 100'000ULL;        // 100 microseconds
inline constexpr uint64_t HORIZON_1MS   = 1'000'000ULL;      // 1 millisecond
inline constexpr uint64_t HORIZON_10MS  = 10'000'000ULL;     // 10 milliseconds
inline constexpr uint64_t HORIZON_100MS = 100'000'000ULL;    // 100 milliseconds

/// Historical mid-price tick entry for time-series lookups
struct MidPriceTick {
    uint64_t timestamp_ns{0};
    uint32_t mid_price{0};
};

/// Individual Trade Markout and Execution Quality Record
struct TradeMarkout {
    uint64_t fill_id{0};
    uint64_t fill_timestamp_ns{0};
    uint8_t  side{0};          // 0 = Buy, 1 = Sell
    uint32_t fill_price{0};
    uint32_t fill_qty{0};
    uint32_t arrival_mid_price{0};

    // Subsequent market mid-prices at markout horizons
    uint32_t mid_100us{0};
    uint32_t mid_1ms{0};
    uint32_t mid_10ms{0};
    uint32_t mid_100ms{0};

    // Computed TCA metrics in cents
    double effective_half_spread{0.0};
    double realized_spread_1ms{0.0};
    double adverse_selection_1ms{0.0};
    bool   is_toxic{false}; // True if market moved adversely against our fill within 1ms
};

/// High-Performance Zero-Allocation Real-Time TCA & Execution Quality Engine
template <size_t MaxTrades = 32768, size_t MaxTicks = 65536>
class TcaEngine {
public:
    TcaEngine() {
        reset();
    }

    void reset() {
        trade_count_ = 0;
        tick_head_ = 0;
        tick_count_ = 0;
        total_volume_ = 0;
        sum_effective_spread_ = 0.0;
        sum_realized_spread_1ms_ = 0.0;
        sum_adverse_selection_1ms_ = 0.0;
        toxic_trade_count_ = 0;
    }

    /// Record a market BBO mid-price tick into the high-speed circular history buffer
    inline void record_market_tick(uint64_t timestamp_ns, uint32_t mid_price) noexcept {
        if (__builtin_expect(mid_price == 0, 0)) return;

        size_t idx = tick_head_ & (MaxTicks - 1);
        tick_history_[idx] = MidPriceTick{
            .timestamp_ns = timestamp_ns,
            .mid_price = mid_price
        };
        tick_head_++;
        if (tick_count_ < MaxTicks) {
            tick_count_++;
        }
    }

    /// Record an execution fill and snapshot the arrival mid-price
    inline TradeMarkout* record_fill(
        uint64_t fill_id,
        uint64_t timestamp_ns,
        uint8_t side,
        uint32_t fill_price,
        uint32_t fill_qty,
        uint32_t current_mid) noexcept 
    {
        if (__builtin_expect(trade_count_ >= MaxTrades, 0)) {
            return nullptr;
        }

        TradeMarkout& trade = trades_[trade_count_++];
        trade.fill_id = fill_id;
        trade.fill_timestamp_ns = timestamp_ns;
        trade.side = side;
        trade.fill_price = fill_price;
        trade.fill_qty = fill_qty;
        trade.arrival_mid_price = current_mid;

        // Effective half-spread: direction * (fill_price - mid)
        // side == 0 (Buy): paying above mid is positive spread paid
        // side == 1 (Sell): selling below mid is positive spread paid
        double direction = (side == 0) ? 1.0 : -1.0;
        trade.effective_half_spread = direction * (static_cast<double>(fill_price) - static_cast<double>(current_mid));

        total_volume_ += fill_qty;
        sum_effective_spread_ += trade.effective_half_spread * fill_qty;

        return &trade;
    }

    /// Evaluate post-trade markouts for all open trades once market ticks elapse
    void evaluate_markouts() noexcept {
        if (trade_count_ == 0 || tick_count_ == 0) return;

        for (size_t i = 0; i < trade_count_; ++i) {
            TradeMarkout& t = trades_[i];
            double direction = (t.side == 0) ? 1.0 : -1.0;

            if (t.mid_100us == 0) {
                t.mid_100us = lookup_mid_at(t.fill_timestamp_ns + HORIZON_100US);
            }
            if (t.mid_1ms == 0) {
                uint32_t mid1 = lookup_mid_at(t.fill_timestamp_ns + HORIZON_1MS);
                if (mid1 > 0) {
                    t.mid_1ms = mid1;
                    // Realized spread: direction * (fill_price - mid_1ms)
                    t.realized_spread_1ms = direction * (static_cast<double>(t.fill_price) - static_cast<double>(mid1));
                    // Adverse selection / price impact: direction * (mid_1ms - arrival_mid)
                    t.adverse_selection_1ms = direction * (static_cast<double>(mid1) - static_cast<double>(t.arrival_mid_price));

                    if (t.adverse_selection_1ms > 0.0) {
                        t.is_toxic = true;
                        toxic_trade_count_++;
                    }

                    sum_realized_spread_1ms_ += t.realized_spread_1ms * t.fill_qty;
                    sum_adverse_selection_1ms_ += t.adverse_selection_1ms * t.fill_qty;
                }
            }
            if (t.mid_10ms == 0) {
                t.mid_10ms = lookup_mid_at(t.fill_timestamp_ns + HORIZON_10MS);
            }
            if (t.mid_100ms == 0) {
                t.mid_100ms = lookup_mid_at(t.fill_timestamp_ns + HORIZON_100MS);
            }
        }
    }

    /// Binary search lookup for mid-price closest to target timestamp
    [[nodiscard]] uint32_t lookup_mid_at(uint64_t target_ns) const noexcept {
        if (tick_count_ == 0) return 0;

        size_t start_idx = (tick_count_ < MaxTicks) ? 0 : (tick_head_ & (MaxTicks - 1));
        
        // Linear scan or fast reverse search since target_ns is usually near recent ticks
        for (size_t offset = 0; offset < tick_count_; ++offset) {
            size_t idx = (tick_head_ - 1 - offset) & (MaxTicks - 1);
            if (tick_history_[idx].timestamp_ns <= target_ns) {
                return tick_history_[idx].mid_price;
            }
        }
        return tick_history_[start_idx].mid_price;
    }

    [[nodiscard]] size_t total_trades() const noexcept { return trade_count_; }
    [[nodiscard]] uint64_t total_volume() const noexcept { return total_volume_; }

    [[nodiscard]] double average_effective_spread() const noexcept {
        return (total_volume_ > 0) ? (sum_effective_spread_ / static_cast<double>(total_volume_)) : 0.0;
    }

    [[nodiscard]] double average_realized_spread_1ms() const noexcept {
        return (total_volume_ > 0) ? (sum_realized_spread_1ms_ / static_cast<double>(total_volume_)) : 0.0;
    }

    [[nodiscard]] double average_adverse_selection_1ms() const noexcept {
        return (total_volume_ > 0) ? (sum_adverse_selection_1ms_ / static_cast<double>(total_volume_)) : 0.0;
    }

    [[nodiscard]] double toxic_flow_ratio() const noexcept {
        return (trade_count_ > 0) ? (static_cast<double>(toxic_trade_count_) / static_cast<double>(trade_count_)) : 0.0;
    }

    void print_summary() const {
        std::cout << "==========================================================\n";
        std::cout << "        TRANSACTION COST ANALYSIS (TCA) REPORT           \n";
        std::cout << "==========================================================\n";
        std::cout << "Total Executions Evaluated : " << trade_count_ << "\n";
        std::cout << "Total Volume Filled        : " << total_volume_ << " shares\n";
        std::cout << "Avg Effective Half-Spread  : " << std::fixed << std::setprecision(4) 
                  << average_effective_spread() << " ticks/cents\n";
        std::cout << "Avg Realized Spread (1ms)  : " << std::fixed << std::setprecision(4) 
                  << average_realized_spread_1ms() << " ticks/cents\n";
        std::cout << "Avg Adverse Selection (1ms): " << std::fixed << std::setprecision(4) 
                  << average_adverse_selection_1ms() << " ticks/cents\n";
        std::cout << "Adverse Flow Ratio         : " << std::fixed << std::setprecision(2) 
                  << (toxic_flow_ratio() * 100.0) << " % of trades\n";
        std::cout << "==========================================================\n";
    }

private:
    size_t trade_count_{0};
    std::array<TradeMarkout, MaxTrades> trades_{};

    size_t tick_head_{0};
    size_t tick_count_{0};
    std::array<MidPriceTick, MaxTicks> tick_history_{};

    uint64_t total_volume_{0};
    double sum_effective_spread_{0.0};
    double sum_realized_spread_1ms_{0.0};
    double sum_adverse_selection_1ms_{0.0};
    size_t toxic_trade_count_{0};
};

} // namespace lob
