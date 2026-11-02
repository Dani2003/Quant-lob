#include "order_book_bridge.hpp"
#include "alpha_engine/src/lib.rs.h"

namespace lob {

std::unique_ptr<OrderBook> new_order_book() {
    return std::make_unique<OrderBook>();
}

bool order_book_add_limit(OrderBook& book,
                          uint64_t order_id,
                          uint64_t client_id,
                          uint8_t side,
                          uint32_t price,
                          uint32_t quantity,
                          uint64_t timestamp_ns) noexcept {
    return book.add_limit_order(order_id, client_id, static_cast<Side>(side), price, quantity, timestamp_ns);
}

bool order_book_add_market(OrderBook& book,
                           uint64_t order_id,
                           uint64_t client_id,
                           uint8_t side,
                           uint32_t quantity,
                           uint64_t timestamp_ns) noexcept {
    return book.add_market_order(order_id, client_id, static_cast<Side>(side), quantity, timestamp_ns);
}

bool order_book_cancel(OrderBook& book, uint64_t order_id) noexcept {
    return book.cancel_order(order_id);
}

void get_top5_snapshot(const OrderBook& book, Top5View& out) noexcept {
    Top5BookView view;
    book.get_top5_view(view);

    for (size_t i = 0; i < 5; ++i) {
        out.bid_prices[i] = view.bid_prices[i];
        out.bid_volumes[i] = view.bid_volumes[i];
        out.ask_prices[i] = view.ask_prices[i];
        out.ask_volumes[i] = view.ask_volumes[i];
    }
    out.bid_depth = view.bid_depth;
    out.ask_depth = view.ask_depth;
}

} // namespace lob

