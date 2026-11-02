#include "order_book.hpp"

#include <cstring>

namespace lob {

OrderBook::OrderBook()
    : order_pool_(std::make_unique<MemoryPool<OrderNode, MAX_ORDERS>>()),
      price_levels_(std::make_unique<PriceLevel[]>(MAX_PRICE_TICKS)),
      order_map_(std::make_unique<OrderMapEntry[]>(ORDER_MAP_CAPACITY)) {
    // Direct initialization of tick prices for O(1) lookup
    for (uint32_t p = 0; p < MAX_PRICE_TICKS; ++p) {
        price_levels_[p].price = p;
        price_levels_[p].total_volume = 0;
        price_levels_[p].order_count = 0;
        price_levels_[p].head = nullptr;
        price_levels_[p].tail = nullptr;
        price_levels_[p].prev_level = nullptr;
        price_levels_[p].next_level = nullptr;
    }

    std::memset(order_map_.get(), 0, sizeof(OrderMapEntry) * ORDER_MAP_CAPACITY);
}

bool OrderBook::add_limit_order(uint64_t order_id,
                                uint64_t client_id,
                                Side side,
                                uint32_t price,
                                uint32_t quantity,
                                uint64_t timestamp_ns) noexcept {
    if (order_id == 0 || order_id == TOMBSTONE || quantity == 0 || price == 0 || price >= MAX_PRICE_TICKS) [[unlikely]] {
        return false;
    }

    uint32_t remaining_qty = quantity;

    if (side == Side::Buy) {
        // Attempt to cross against the ask book first
        match_buy_limit(order_id, client_id, price, remaining_qty, timestamp_ns);

        // Any remaining unfilled quantity rests on the bid book
        if (remaining_qty > 0) {
            OrderNode* node = order_pool_->allocate();
            if (!node) [[unlikely]] {
                return false; // Pool exhausted
            }

            node->order.order_id = order_id;
            node->order.client_id = client_id;
            node->order.timestamp_ns = timestamp_ns;
            node->order.price = price;
            node->order.quantity = remaining_qty;
            node->order.side = Side::Buy;
            node->order.type = OrderType::Limit;

            PriceLevel& level = price_levels_[price];
            if (level.empty()) {
                link_bid_level(&level);
            }
            level.append(node);

            insert_order_map(order_id, node);
            ++total_resting_orders_;
        }
    } else {
        // Attempt to cross against the bid book first
        match_sell_limit(order_id, client_id, price, remaining_qty, timestamp_ns);

        // Any remaining unfilled quantity rests on the ask book
        if (remaining_qty > 0) {
            OrderNode* node = order_pool_->allocate();
            if (!node) [[unlikely]] {
                return false;
            }

            node->order.order_id = order_id;
            node->order.client_id = client_id;
            node->order.timestamp_ns = timestamp_ns;
            node->order.price = price;
            node->order.quantity = remaining_qty;
            node->order.side = Side::Sell;
            node->order.type = OrderType::Limit;

            PriceLevel& level = price_levels_[price];
            if (level.empty()) {
                link_ask_level(&level);
            }
            level.append(node);

            insert_order_map(order_id, node);
            ++total_resting_orders_;
        }
    }

    return true;
}

bool OrderBook::add_market_order(uint64_t order_id,
                                 uint64_t client_id,
                                 Side side,
                                 uint32_t quantity,
                                 uint64_t timestamp_ns) noexcept {
    if (order_id == 0 || quantity == 0) [[unlikely]] {
        return false;
    }

    uint32_t remaining_qty = quantity;

    if (side == Side::Buy) {
        // Buy Market order crosses all available ask levels
        match_buy_limit(order_id, client_id, MAX_PRICE_TICKS - 1, remaining_qty, timestamp_ns);
    } else {
        // Sell Market order crosses all available bid levels
        match_sell_limit(order_id, client_id, 0, remaining_qty, timestamp_ns);
    }

    return (remaining_qty < quantity);
}

bool OrderBook::cancel_order(uint64_t order_id) noexcept {
    OrderNode* node = find_order_map(order_id);
    if (!node) [[unlikely]] {
        return false;
    }

    PriceLevel* level = node->level;
    level->total_volume -= node->order.quantity;
    level->remove(node);

    if (level->empty()) {
        if (node->order.side == Side::Buy) {
            unlink_bid_level(level);
        } else {
            unlink_ask_level(level);
        }
    }

    remove_order_map(order_id);
    order_pool_->deallocate(node);
    --total_resting_orders_;
    return true;
}

void OrderBook::match_buy_limit(uint64_t order_id,
                                uint64_t client_id,
                                uint32_t price,
                                uint32_t& remaining_qty,
                                uint64_t timestamp_ns) noexcept {
    (void)client_id;
    while (best_ask_level_ != nullptr && price >= best_ask_level_->price && remaining_qty > 0) {
        OrderNode* maker = best_ask_level_->head;
        while (maker != nullptr && remaining_qty > 0) {
            const uint32_t fill_qty = std::min(remaining_qty, maker->order.quantity);

            record_trade(maker->order.order_id, order_id, maker->order.price, fill_qty, Side::Buy, timestamp_ns);

            maker->order.quantity -= fill_qty;
            best_ask_level_->total_volume -= fill_qty;
            remaining_qty -= fill_qty;

            if (maker->order.quantity == 0) {
                OrderNode* next_maker = maker->next;
                best_ask_level_->remove(maker);
                remove_order_map(maker->order.order_id);
                order_pool_->deallocate(maker);
                --total_resting_orders_;
                maker = next_maker;
            } else {
                break; // Taker order satisfied
            }
        }

        if (best_ask_level_->empty()) {
            unlink_ask_level(best_ask_level_);
        }
    }
}

void OrderBook::match_sell_limit(uint64_t order_id,
                                 uint64_t client_id,
                                 uint32_t price,
                                 uint32_t& remaining_qty,
                                 uint64_t timestamp_ns) noexcept {
    (void)client_id;
    while (best_bid_level_ != nullptr && price <= best_bid_level_->price && remaining_qty > 0) {
        OrderNode* maker = best_bid_level_->head;
        while (maker != nullptr && remaining_qty > 0) {
            const uint32_t fill_qty = std::min(remaining_qty, maker->order.quantity);

            record_trade(maker->order.order_id, order_id, maker->order.price, fill_qty, Side::Sell, timestamp_ns);

            maker->order.quantity -= fill_qty;
            best_bid_level_->total_volume -= fill_qty;
            remaining_qty -= fill_qty;

            if (maker->order.quantity == 0) {
                OrderNode* next_maker = maker->next;
                best_bid_level_->remove(maker);
                remove_order_map(maker->order.order_id);
                order_pool_->deallocate(maker);
                --total_resting_orders_;
                maker = next_maker;
            } else {
                break; // Taker order satisfied
            }
        }

        if (best_bid_level_->empty()) {
            unlink_bid_level(best_bid_level_);
        }
    }
}

void OrderBook::link_bid_level(PriceLevel* level) noexcept {
    if (!best_bid_level_) {
        best_bid_level_ = level;
        level->prev_level = nullptr;
        level->next_level = nullptr;
        return;
    }

    if (level->price > best_bid_level_->price) {
        level->next_level = best_bid_level_;
        level->prev_level = nullptr;
        best_bid_level_->prev_level = level;
        best_bid_level_ = level;
        return;
    }

    PriceLevel* curr = best_bid_level_;
    while (curr->next_level != nullptr && curr->next_level->price > level->price) {
        curr = curr->next_level;
    }

    level->next_level = curr->next_level;
    level->prev_level = curr;
    if (curr->next_level) {
        curr->next_level->prev_level = level;
    }
    curr->next_level = level;
}

void OrderBook::unlink_bid_level(PriceLevel* level) noexcept {
    if (level->prev_level) {
        level->prev_level->next_level = level->next_level;
    } else {
        best_bid_level_ = level->next_level;
    }
    if (level->next_level) {
        level->next_level->prev_level = level->prev_level;
    }
    level->prev_level = nullptr;
    level->next_level = nullptr;
}

void OrderBook::link_ask_level(PriceLevel* level) noexcept {
    if (!best_ask_level_) {
        best_ask_level_ = level;
        level->prev_level = nullptr;
        level->next_level = nullptr;
        return;
    }

    if (level->price < best_ask_level_->price) {
        level->next_level = best_ask_level_;
        level->prev_level = nullptr;
        best_ask_level_->prev_level = level;
        best_ask_level_ = level;
        return;
    }

    PriceLevel* curr = best_ask_level_;
    while (curr->next_level != nullptr && curr->next_level->price < level->price) {
        curr = curr->next_level;
    }

    level->next_level = curr->next_level;
    level->prev_level = curr;
    if (curr->next_level) {
        curr->next_level->prev_level = level;
    }
    curr->next_level = level;
}

void OrderBook::unlink_ask_level(PriceLevel* level) noexcept {
    if (level->prev_level) {
        level->prev_level->next_level = level->next_level;
    } else {
        best_ask_level_ = level->next_level;
    }
    if (level->next_level) {
        level->next_level->prev_level = level->prev_level;
    }
    level->prev_level = nullptr;
    level->next_level = nullptr;
}

void OrderBook::insert_order_map(uint64_t order_id, OrderNode* node) noexcept {
    size_t idx = hash_order_id(order_id) & ORDER_MAP_MASK;
    size_t first_tombstone = SIZE_MAX;

    while (order_map_[idx].order_id != 0) {
        if (order_map_[idx].order_id == order_id) {
            order_map_[idx].node = node;
            return;
        }
        if (order_map_[idx].order_id == TOMBSTONE && first_tombstone == SIZE_MAX) {
            first_tombstone = idx;
        }
        idx = (idx + 1) & ORDER_MAP_MASK;
    }

    if (first_tombstone != SIZE_MAX) {
        idx = first_tombstone;
    }
    order_map_[idx].order_id = order_id;
    order_map_[idx].node = node;
}

OrderNode* OrderBook::find_order_map(uint64_t order_id) const noexcept {
    size_t idx = hash_order_id(order_id) & ORDER_MAP_MASK;
    while (order_map_[idx].order_id != 0) {
        if (order_map_[idx].order_id == order_id) {
            return order_map_[idx].node;
        }
        idx = (idx + 1) & ORDER_MAP_MASK;
    }
    return nullptr;
}

void OrderBook::remove_order_map(uint64_t order_id) noexcept {
    size_t idx = hash_order_id(order_id) & ORDER_MAP_MASK;
    while (order_map_[idx].order_id != 0) {
        if (order_map_[idx].order_id == order_id) {
            order_map_[idx].order_id = TOMBSTONE;
            order_map_[idx].node = nullptr;
            return;
        }
        idx = (idx + 1) & ORDER_MAP_MASK;
    }
}

std::optional<PriceLevelInfo> OrderBook::get_best_bid() const noexcept {
    if (best_bid_level_) {
        return PriceLevelInfo{best_bid_level_->price, best_bid_level_->total_volume, best_bid_level_->order_count};
    }
    return std::nullopt;
}

std::optional<PriceLevelInfo> OrderBook::get_best_ask() const noexcept {
    if (best_ask_level_) {
        return PriceLevelInfo{best_ask_level_->price, best_ask_level_->total_volume, best_ask_level_->order_count};
    }
    return std::nullopt;
}

uint32_t OrderBook::get_spread() const noexcept {
    if (best_bid_level_ && best_ask_level_) {
        return (best_ask_level_->price > best_bid_level_->price) 
            ? (best_ask_level_->price - best_bid_level_->price) : 0;
    }
    return 0;
}

double OrderBook::get_mid_price() const noexcept {
    if (best_bid_level_ && best_ask_level_) {
        return static_cast<double>(best_bid_level_->price + best_ask_level_->price) / 2.0;
    }
    if (best_bid_level_) return static_cast<double>(best_bid_level_->price);
    if (best_ask_level_) return static_cast<double>(best_ask_level_->price);
    return 0.0;
}

MarketDepth OrderBook::get_depth(size_t max_levels) const noexcept {
    MarketDepth depth;
    depth.bid_depth = 0;
    depth.ask_depth = 0;

    const size_t target_levels = std::min(max_levels, MarketDepth::MAX_DEPTH);

    const PriceLevel* bid = best_bid_level_;
    while (bid && depth.bid_depth < target_levels) {
        depth.bids[depth.bid_depth++] = PriceLevelInfo{bid->price, bid->total_volume, bid->order_count};
        bid = bid->next_level;
    }

    const PriceLevel* ask = best_ask_level_;
    while (ask && depth.ask_depth < target_levels) {
        depth.asks[depth.ask_depth++] = PriceLevelInfo{ask->price, ask->total_volume, ask->order_count};
        ask = ask->next_level;
    }

    return depth;
}

void OrderBook::get_top5_view(Top5BookView& out) const noexcept {
    out.bid_depth = 0;
    out.ask_depth = 0;

    const PriceLevel* bid = best_bid_level_;
    while (bid && out.bid_depth < 5) {
        out.bid_prices[out.bid_depth] = bid->price;
        out.bid_volumes[out.bid_depth] = bid->total_volume;
        ++out.bid_depth;
        bid = bid->next_level;
    }
    for (size_t i = out.bid_depth; i < 5; ++i) {
        out.bid_prices[i] = 0;
        out.bid_volumes[i] = 0;
    }

    const PriceLevel* ask = best_ask_level_;
    while (ask && out.ask_depth < 5) {
        out.ask_prices[out.ask_depth] = ask->price;
        out.ask_volumes[out.ask_depth] = ask->total_volume;
        ++out.ask_depth;
        ask = ask->next_level;
    }
    for (size_t i = out.ask_depth; i < 5; ++i) {
        out.ask_prices[i] = 0;
        out.ask_volumes[i] = 0;
    }
}

void OrderBook::clear() noexcept {
    order_pool_->reset();
    for (uint32_t p = 0; p < MAX_PRICE_TICKS; ++p) {
        price_levels_[p].total_volume = 0;
        price_levels_[p].order_count = 0;
        price_levels_[p].head = nullptr;
        price_levels_[p].tail = nullptr;
        price_levels_[p].prev_level = nullptr;
        price_levels_[p].next_level = nullptr;
    }
    std::memset(order_map_.get(), 0, sizeof(OrderMapEntry) * ORDER_MAP_CAPACITY);
    best_bid_level_ = nullptr;
    best_ask_level_ = nullptr;
    total_resting_orders_ = 0;
    total_trades_ = 0;
    total_matched_volume_ = 0;
}

} // namespace lob
