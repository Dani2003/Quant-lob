#pragma once

#include <memory>
#include <cstdint>
#include "order_book.hpp"

namespace lob {

struct Top5View;

// Factory function for cxx UniquePtr
std::unique_ptr<OrderBook> new_order_book();

// C++ wrappers adapting C++ enum class types to FFI-friendly primitives
bool order_book_add_limit(OrderBook& book,
                          uint64_t order_id,
                          uint64_t client_id,
                          uint8_t side,
                          uint32_t price,
                          uint32_t quantity,
                          uint64_t timestamp_ns) noexcept;

bool order_book_add_market(OrderBook& book,
                           uint64_t order_id,
                           uint64_t client_id,
                           uint8_t side,
                           uint32_t quantity,
                           uint64_t timestamp_ns) noexcept;

bool order_book_cancel(OrderBook& book, uint64_t order_id) noexcept;

// Zero-copy population of Top-5 depth view
void get_top5_snapshot(const OrderBook& book, Top5View& out) noexcept;

} // namespace lob
