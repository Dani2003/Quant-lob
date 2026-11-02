#include <cassert>
#include <iostream>
#include <memory>

#include "order_book.hpp"

using namespace lob;

void test_price_time_priority() {
    std::cout << "[Test 1] Verifying Strict Price-Time (FIFO) Priority... ";
    auto book = std::make_unique<OrderBook>();

    // Add two limit sell orders at the same price: 100.50 (10050 ticks)
    // Order 1 arrives first (Maker 1)
    book->add_limit_order(1, 10, Side::Sell, 10050, 100);
    // Order 2 arrives second (Maker 2)
    book->add_limit_order(2, 20, Side::Sell, 10050, 200);

    uint64_t first_maker_filled = 0;
    book->set_trade_callback([](const Trade& trade, void* user_data) {
        auto* out = static_cast<uint64_t*>(user_data);
        if (*out == 0) {
            *out = trade.maker_order_id;
        }
    }, &first_maker_filled);

    // Aggressive Buy Order arrives for 50 shares
    // Must fill against Order 1 first (FIFO)
    book->add_limit_order(3, 30, Side::Buy, 10050, 50);

    assert(first_maker_filled == 1 && "FIFO violated! Order 1 must be filled first.");
    assert(book->get_resting_order_count() == 2 && "Both orders should remain (Order 1 partially filled).");

    std::cout << "PASSED [OK]\n";
}

void test_partial_fill_and_queue_position() {
    std::cout << "[Test 2] Verifying Partial Fill & In-Place Queue Maintenance... ";
    auto book = std::make_unique<OrderBook>();

    // Order 1: 100 shares @ $100.00
    book->add_limit_order(1, 10, Side::Sell, 10000, 100);
    // Order 2: 100 shares @ $100.00
    book->add_limit_order(2, 20, Side::Sell, 10000, 100);

    // Cross with 40 shares Buy
    book->add_market_order(3, 30, Side::Buy, 40);

    auto best_ask = book->get_best_ask();
    assert(best_ask.has_value());
    assert(best_ask->total_volume == 160 && "Remaining volume should be 160 shares.");
    assert(best_ask->order_count == 2 && "Both orders should still exist.");

    // Next cross with 60 shares Buy should completely exhaust Order 1, without touching Order 2
    book->add_market_order(4, 30, Side::Buy, 60);

    best_ask = book->get_best_ask();
    assert(best_ask.has_value());
    assert(best_ask->total_volume == 100 && "Order 1 fully filled, 100 shares left.");
    assert(best_ask->order_count == 1 && "Only Order 2 should remain.");

    std::cout << "PASSED [OK]\n";
}

void test_multi_level_market_sweep() {
    std::cout << "[Test 3] Verifying Multi-Level Book Sweep... ";
    auto book = std::make_unique<OrderBook>();

    // Build Ask ladder
    book->add_limit_order(1, 10, Side::Sell, 10001, 50);  // Level 1
    book->add_limit_order(2, 20, Side::Sell, 10002, 100); // Level 2
    book->add_limit_order(3, 30, Side::Sell, 10003, 150); // Level 3

    assert(book->get_best_ask()->price == 10001);

    // Aggressive market order sweeps 200 shares
    // Should consume Level 1 (50 shares), Level 2 (100 shares), and 50 shares of Level 3
    book->add_market_order(4, 40, Side::Buy, 200);

    assert(book->get_best_ask()->price == 10003 && "Top of book should advance to Level 3.");
    assert(book->get_best_ask()->total_volume == 100 && "Level 3 should have 100 shares remaining.");
    assert(book->get_total_matched_volume() == 200 && "Total volume matched must equal 200.");

    std::cout << "PASSED [OK]\n";
}

void test_cancellation_invariance() {
    std::cout << "[Test 4] Verifying O(1) Cancellation Invariants... ";
    auto book = std::make_unique<OrderBook>();

    book->add_limit_order(100, 1, Side::Buy, 9990, 50);
    book->add_limit_order(101, 1, Side::Buy, 9995, 100); // Best bid

    assert(book->get_best_bid()->price == 9995);

    // Cancel best bid
    bool ok = book->cancel_order(101);
    assert(ok && "Cancel must succeed for active order.");
    assert(book->get_best_bid()->price == 9990 && "Best bid must instantly fall back to 9990.");

    // Attempt double cancel
    bool double_cancel = book->cancel_order(101);
    assert(!double_cancel && "Double cancel must return false safely.");

    std::cout << "PASSED [OK]\n";
}

int main() {
    std::cout << "==========================================================\n";
    std::cout << "  QUANT-LOB: CORE INVARIANT UNIT TEST SUITE               \n";
    std::cout << "==========================================================\n";

    test_price_time_priority();
    test_partial_fill_and_queue_position();
    test_multi_level_market_sweep();
    test_cancellation_invariance();

    std::cout << "==========================================================\n";
    std::cout << "  ALL 4 CORE INVARIANT TESTS PASSED SUCCESSFULLY!         \n";
    std::cout << "==========================================================\n";
    return 0;
}
