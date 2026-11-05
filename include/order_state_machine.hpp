#pragma once

#include <cstdint>
#include <algorithm>
#include <cstring>
#include <string_view>
#include <array>
#include "ouch_messages.hpp"
#include "risk_engine.hpp"

namespace lob {

enum class OrderStatus : uint8_t {
    Empty = 0,
    PendingRisk,
    RiskRejected,
    PendingNew,
    Active,
    PartiallyFilled,
    Filled,
    PendingCancel,
    Canceled,
    Rejected
};

inline const char* order_status_to_string(OrderStatus status) {
    switch (status) {
        case OrderStatus::Empty: return "Empty";
        case OrderStatus::PendingRisk: return "PendingRisk";
        case OrderStatus::RiskRejected: return "RiskRejected";
        case OrderStatus::PendingNew: return "PendingNew";
        case OrderStatus::Active: return "Active";
        case OrderStatus::PartiallyFilled: return "PartiallyFilled";
        case OrderStatus::Filled: return "Filled";
        case OrderStatus::PendingCancel: return "PendingCancel";
        case OrderStatus::Canceled: return "Canceled";
        case OrderStatus::Rejected: return "Rejected";
        default: return "Unknown";
    }
}

/// Tracked order state entry
struct alignas(64) TrackedOrder {
    uint64_t order_token{0};
    uint64_t exchange_ref{0};
    uint32_t price{0};
    uint32_t original_qty{0};
    uint32_t executed_qty{0};
    uint32_t leaves_qty{0};
    uint64_t total_executed_value{0};
    uint8_t  side{0}; // 0 = Buy, 1 = Sell
    OrderStatus status{OrderStatus::Empty};
    RiskCheckResult risk_result{RiskCheckResult::Approved};

    [[nodiscard]] inline uint32_t avg_execution_price() const noexcept {
        return (executed_qty > 0) ? static_cast<uint32_t>(total_executed_value / executed_qty) : 0;
    }
};

/// High-Performance, Zero-Allocation Order State Machine (OMS)
/// Manages up to MAX_ORDERS concurrent order tokens using flat direct indexing
template <size_t MaxOrders = 65536>
class OrderStateMachine {
public:
    OrderStateMachine() {
        reset();
    }

    void reset() {
        for (auto& order : orders_) {
            order.status = OrderStatus::Empty;
            order.order_token = 0;
        }
    }

    /// Submit a new order into the OMS with pre-trade risk evaluation
    TrackedOrder* submit_order(
        uint64_t order_token,
        uint8_t side,
        uint32_t price,
        uint32_t qty,
        uint32_t mid_price,
        uint64_t timestamp_ns,
        RiskEngine& risk_engine) 
    {
        if (order_token == 0 || side > 1 || price == 0 || qty == 0) return nullptr;

        size_t available_idx = MaxOrders;
        const size_t start = hash_token(order_token);
        static constexpr size_t MAX_PROBES = (MaxOrders < 64) ? MaxOrders : 64;

        for (size_t probe = 0; probe < MAX_PROBES; ++probe) {
            const size_t candidate = (start + probe) & IndexMask;
            TrackedOrder& slot = orders_[candidate];

            if (slot.status == OrderStatus::Empty) {
                if (available_idx == MaxOrders) available_idx = candidate;
                break;
            }

            if (slot.order_token == order_token) {
                return nullptr; // Duplicate token
            }

            if (available_idx == MaxOrders &&
                (slot.status == OrderStatus::Filled ||
                 slot.status == OrderStatus::Canceled ||
                 slot.status == OrderStatus::Rejected ||
                 slot.status == OrderStatus::RiskRejected)) {
                available_idx = candidate; // Reusable tombstone
            }
        }

        if (available_idx == MaxOrders) return nullptr;
        TrackedOrder& order = orders_[available_idx];
        order.order_token = order_token;
        order.exchange_ref = 0;
        order.side = side;
        order.price = price;
        order.original_qty = qty;
        order.leaves_qty = qty;
        order.executed_qty = 0;
        order.total_executed_value = 0;
        order.status = OrderStatus::PendingRisk;

        // Perform Sub-30ns Pre-Trade Risk Check
        order.risk_result = risk_engine.check_order(side, price, qty, mid_price, timestamp_ns);
        if (order.risk_result != RiskCheckResult::Approved) {
            order.status = OrderStatus::RiskRejected;
            return &order;
        }

        risk_engine.reserve_order(side, qty);
        order.status = OrderStatus::PendingNew;
        return &order;
    }

    /// Transition to Active upon OUCH OrderAccepted message
    bool on_order_accepted(uint64_t order_token, uint64_t exchange_ref) noexcept {
        TrackedOrder* order = find_order(order_token);
        if (!order || order->status != OrderStatus::PendingNew) {
            return false;
        }
        order->exchange_ref = exchange_ref;
        order->status = OrderStatus::Active;
        return true;
    }

    /// Process OUCH OrderExecuted fill (supports fills racing with pending cancellation)
    bool on_order_executed(
        uint64_t order_token,
        uint32_t executed_shares,
        uint32_t execution_price,
        RiskEngine& risk_engine) noexcept 
    {
        TrackedOrder* order = find_order(order_token);
        if (!order || (order->status != OrderStatus::Active &&
                       order->status != OrderStatus::PartiallyFilled &&
                       order->status != OrderStatus::PendingCancel) ||
            executed_shares == 0 || executed_shares > order->leaves_qty) {
            return false;
        }

        order->executed_qty += executed_shares;
        order->leaves_qty = (order->leaves_qty > executed_shares) ? (order->leaves_qty - executed_shares) : 0;
        order->total_executed_value += static_cast<uint64_t>(executed_shares) * static_cast<uint64_t>(execution_price);

        // Update Risk Engine position & PnL accounting
        risk_engine.release_order(order->side, executed_shares);
        risk_engine.on_fill(order->side, execution_price, executed_shares);

        if (order->leaves_qty == 0) {
            order->status = OrderStatus::Filled;
        } else if (order->status != OrderStatus::PendingCancel) {
            order->status = OrderStatus::PartiallyFilled;
        }

        return true;
    }

    /// Request Order Cancellation
    bool request_cancel(uint64_t order_token) noexcept {
        TrackedOrder* order = find_order(order_token);
        if (!order || (order->status != OrderStatus::Active && order->status != OrderStatus::PartiallyFilled)) {
            return false;
        }
        order->status = OrderStatus::PendingCancel;
        return true;
    }

    /// Transition to Canceled upon OUCH OrderCanceled message
    bool on_order_canceled(uint64_t order_token, uint32_t decrement_shares, RiskEngine& risk_engine) noexcept {
        TrackedOrder* order = find_order(order_token);
        if (!order || (order->status != OrderStatus::Active &&
                       order->status != OrderStatus::PartiallyFilled &&
                       order->status != OrderStatus::PendingCancel)) return false;

        const uint32_t released = std::min(order->leaves_qty, decrement_shares);
        risk_engine.release_order(order->side, released);
        order->leaves_qty -= released;
        if (order->leaves_qty == 0) {
            order->status = OrderStatus::Canceled;
        }
        return true;
    }

    /// Transition to Rejected upon OUCH OrderRejected message
    bool on_order_rejected(uint64_t order_token, RiskEngine& risk_engine) noexcept {
        TrackedOrder* order = find_order(order_token);
        if (!order || order->status != OrderStatus::PendingNew) return false;
        risk_engine.release_order(order->side, order->leaves_qty);
        order->leaves_qty = 0;
        order->status = OrderStatus::Rejected;
        return true;
    }

    [[nodiscard]] inline TrackedOrder* find_order(uint64_t order_token) noexcept {
        if (order_token == 0) return nullptr;
        const size_t start = hash_token(order_token);
        static constexpr size_t MAX_PROBES = (MaxOrders < 64) ? MaxOrders : 64;
        for (size_t probe = 0; probe < MAX_PROBES; ++probe) {
            TrackedOrder& order = orders_[(start + probe) & IndexMask];
            if (order.status == OrderStatus::Empty) return nullptr;
            if (order.order_token == order_token) return &order;
        }
        return nullptr;
    }

private:
    static constexpr size_t IndexMask = MaxOrders - 1;
    static_assert((MaxOrders & (MaxOrders - 1)) == 0, "MaxOrders must be a power of two");

    inline size_t hash_token(uint64_t token) const noexcept {
        return static_cast<size_t>(token) & IndexMask;
    }

    std::array<TrackedOrder, MaxOrders> orders_{};
};

} // namespace lob

