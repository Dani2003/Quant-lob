#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <algorithm>

namespace lob {

/**
 * @brief Zero-allocation depth consumption ledger for paper replay execution.
 *
 * Tracks displayed liquidity and consumed volume across price levels:
 * - Prevents duplicate fills when prices alternate (A -> B -> A).
 * - Tracks legitimate depth replenishment (market orders adding depth expand available volume).
 * - Handles partial fills accurately across successive execution decisions.
 * - Reflects depth reductions from market cancellations.
 */
class PaperDepthLedger {
public:
    static constexpr size_t CAPACITY = 256;
    static constexpr size_t MASK = CAPACITY - 1;
    static constexpr size_t MAX_PROBES = 16;

    struct LevelEntry {
        uint32_t price{0};
        uint32_t consumed_vol{0};
        uint32_t last_observed_vol{0};
    };

    constexpr PaperDepthLedger() noexcept = default;

    void reset() noexcept {
        for (auto& entry : entries_) {
            entry = LevelEntry{};
        }
    }

    /**
     * @brief Attempt to consume available liquidity at a given price level.
     * @param price Price level in cents/ticks
     * @param displayed_vol Current displayed depth in the market snapshot
     * @param requested_qty Desired order quantity
     * @return Filled quantity (0 <= fill <= requested_qty)
     */
    uint32_t consume(uint32_t price, uint32_t displayed_vol, uint32_t requested_qty) noexcept {
        if (__builtin_expect(price == 0 || displayed_vol == 0 || requested_qty == 0, 0)) {
            return 0;
        }

        LevelEntry* entry = find_or_create_entry(price, displayed_vol);
        if (!entry) return 0;

        // Synchronize with external changes to displayed depth
        if (displayed_vol > entry->last_observed_vol) {
            // Replenishment: additional shares added to this price level
            entry->last_observed_vol = displayed_vol;
        } else if (displayed_vol < entry->last_observed_vol) {
            // Depletion from outside fills or cancellations: shrink observation
            entry->last_observed_vol = displayed_vol;
            if (entry->consumed_vol > displayed_vol) {
                entry->consumed_vol = displayed_vol;
            }
        }

        const uint32_t available = (displayed_vol > entry->consumed_vol) ? (displayed_vol - entry->consumed_vol) : 0;
        const uint32_t fill_qty = std::min(requested_qty, available);
        entry->consumed_vol += fill_qty;
        return fill_qty;
    }

    [[nodiscard]] uint32_t get_consumed(uint32_t price) const noexcept {
        const LevelEntry* entry = find_entry(price);
        return entry ? entry->consumed_vol : 0;
    }

    [[nodiscard]] uint32_t get_available(uint32_t price, uint32_t displayed_vol) const noexcept {
        const LevelEntry* entry = find_entry(price);
        if (!entry) return displayed_vol;
        if (displayed_vol <= entry->consumed_vol) return 0;
        return displayed_vol - entry->consumed_vol;
    }

private:
    static constexpr size_t hash_price(uint32_t price) noexcept {
        return (static_cast<size_t>(price) * 2654435761u) & MASK;
    }

    [[nodiscard]] const LevelEntry* find_entry(uint32_t price) const noexcept {
        size_t idx = hash_price(price);
        for (size_t probe = 0; probe < MAX_PROBES; ++probe) {
            size_t slot = (idx + probe) & MASK;
            if (entries_[slot].price == price) {
                return &entries_[slot];
            }
            if (entries_[slot].price == 0) {
                break;
            }
        }
        return nullptr;
    }

    [[nodiscard]] LevelEntry* find_or_create_entry(uint32_t price, uint32_t initial_vol) noexcept {
        size_t idx = hash_price(price);
        size_t empty_slot = CAPACITY;

        for (size_t probe = 0; probe < MAX_PROBES; ++probe) {
            size_t slot = (idx + probe) & MASK;
            if (entries_[slot].price == price) {
                return &entries_[slot];
            }
            if (entries_[slot].price == 0 && empty_slot == CAPACITY) {
                empty_slot = slot;
            }
        }

        // If not found, use first empty slot or replace at base slot
        size_t target_slot = (empty_slot != CAPACITY) ? empty_slot : (idx & MASK);
        auto& entry = entries_[target_slot];
        entry.price = price;
        entry.consumed_vol = 0;
        entry.last_observed_vol = initial_vol;
        return &entry;
    }

    std::array<LevelEntry, CAPACITY> entries_{};
};

} // namespace lob

