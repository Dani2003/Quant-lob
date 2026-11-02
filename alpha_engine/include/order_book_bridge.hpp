#pragma once

#include <memory>
#include <cstdint>
#include "order_book.hpp"

namespace lob {
namespace ffi {

struct Top5View {
    std::array<uint32_t, 5> bid_prices;
    std::array<uint32_t, 5> bid_volumes;
    std::array<uint32_t, 5> ask_prices;
    std::array<uint32_t, 5> ask_volumes;
    uint32_t bid_depth;
    uint32_t ask_depth;
};

// Factory function for cxx UniquePtr
inline std::unique_ptr<OrderBook> new_order_book() {
    return std::make_unique<OrderBook>();
}

// Zero-copy populator for Rust FFI bridge
inline Top5View get_top5_snapshot(const OrderBook& book) noexcept {
    Top5BookView view;
    book.get_top5_view(view);

    Top5View result;
    for (size_t i = 0; i < 5; ++i) {
        result.bid_prices[i] = view.bid_prices[i];
        result.bid_volumes[i] = view.bid_volumes[i];
        result.ask_prices[i] = view.ask_prices[i];
        result.ask_volumes[i] = view.ask_volumes[i];
    }
    result.bid_depth = view.bid_depth;
    result.ask_depth = view.ask_depth;
    return result;
}

// Direct memory pointer access for absolute zero-copy SIMD processing
inline const uint32_t* get_bid_volumes_ptr(const Top5BookView& view) noexcept {
    return view.bid_volumes;
}

inline const uint32_t* get_ask_volumes_ptr(const Top5BookView& view) noexcept {
    return view.ask_volumes;
}

} // namespace ffi
} // namespace lob

