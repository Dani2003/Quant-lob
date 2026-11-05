#include <cassert>
#include <iostream>
#include <memory>
#include <vector>

#include "order_book.hpp"
#include "feed_handler.hpp"
#include "ouch_messages.hpp"

using namespace lob;

void test_price_time_priority() {
    std::cout << "[Test 1] Verifying Strict Price-Time (FIFO) Priority... ";
    auto book = std::make_unique<OrderBook>();

    // Add two limit sell orders at the same price: 100.50 (10050 ticks)
    // Order 1 arrives first (Maker 1)
    (void)book->add_limit_order(1, 10, Side::Sell, 10050, 100);
    // Order 2 arrives second (Maker 2)
    (void)book->add_limit_order(2, 20, Side::Sell, 10050, 200);

    uint64_t first_maker_filled = 0;
    book->set_trade_callback([](const Trade& trade, void* user_data) {
        auto* out = static_cast<uint64_t*>(user_data);
        if (*out == 0) {
            *out = trade.maker_order_id;
        }
    }, &first_maker_filled);

    // Aggressive Buy Order arrives for 50 shares
    // Must fill against Order 1 first (FIFO)
    (void)book->add_limit_order(3, 30, Side::Buy, 10050, 50);

    assert(first_maker_filled == 1 && "FIFO violated! Order 1 must be filled first.");
    assert(book->get_resting_order_count() == 2 && "Both orders should remain (Order 1 partially filled).");

    std::cout << "PASSED [OK]\n";
}

void test_partial_fill_and_queue_position() {
    std::cout << "[Test 2] Verifying Partial Fill & In-Place Queue Maintenance... ";
    auto book = std::make_unique<OrderBook>();

    // Order 1: 100 shares @ $100.00
    (void)book->add_limit_order(1, 10, Side::Sell, 10000, 100);
    // Order 2: 100 shares @ $100.00
    (void)book->add_limit_order(2, 20, Side::Sell, 10000, 100);

    // Cross with 40 shares Buy
    (void)book->add_market_order(3, 30, Side::Buy, 40);

    auto best_ask = book->get_best_ask();
    assert(best_ask.has_value());
    assert(best_ask->total_volume == 160 && "Remaining volume should be 160 shares.");
    assert(best_ask->order_count == 2 && "Both orders should still exist.");

    // Next cross with 60 shares Buy should completely exhaust Order 1, without touching Order 2
    (void)book->add_market_order(4, 30, Side::Buy, 60);

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
    (void)book->add_limit_order(1, 10, Side::Sell, 10001, 50);  // Level 1
    (void)book->add_limit_order(2, 20, Side::Sell, 10002, 100); // Level 2
    (void)book->add_limit_order(3, 30, Side::Sell, 10003, 150); // Level 3

    assert(book->get_best_ask()->price == 10001);

    // Aggressive market order sweeps 200 shares
    // Should consume Level 1 (50 shares), Level 2 (100 shares), and 50 shares of Level 3
    (void)book->add_market_order(4, 40, Side::Buy, 200);

    assert(book->get_best_ask()->price == 10003 && "Top of book should advance to Level 3.");
    assert(book->get_best_ask()->total_volume == 100 && "Level 3 should have 100 shares remaining.");
    assert(book->get_total_matched_volume() == 200 && "Total volume matched must equal 200.");

    std::cout << "PASSED [OK]\n";
}

void test_cancellation_invariance() {
    std::cout << "[Test 4] Verifying O(1) Cancellation Invariants... ";
    auto book = std::make_unique<OrderBook>();

    (void)book->add_limit_order(100, 1, Side::Buy, 9990, 50);
    (void)book->add_limit_order(101, 1, Side::Buy, 9995, 100); // Best bid

    assert(book->get_best_bid()->price == 9995);

    // Cancel best bid
    bool ok = book->cancel_order(101);
    assert(ok && "Cancel must succeed for active order.");
    (void)ok;
    assert(book->get_best_bid()->price == 9990 && "Best bid must instantly fall back to 9990.");

    // Attempt double cancel
    bool double_cancel = book->cancel_order(101);
    assert(!double_cancel && "Double cancel must return false safely.");
    (void)double_cancel;


    std::cout << "PASSED [OK]\n";
}

void test_order_book_reduce_execute_replace() {
    std::cout << "[Test 5] Verifying Reduce, Execute & Replace Invariants... ";
    auto book = std::make_unique<OrderBook>();

    // 1. Add Buy Order 1: 100 shares @ $100.00 (10000 ticks)
    bool ok = book->add_limit_order(1, 10, Side::Buy, 10000, 100);
    assert(ok);
    assert(book->get_best_bid().has_value() && book->get_best_bid()->price == 10000);
    assert(book->get_best_bid()->total_volume == 100);

    // 2. Reduce Order 1 by 40 shares -> 60 shares remaining
    ok = book->reduce_order(1, 40);
    assert(ok);
    assert(book->get_best_bid()->total_volume == 60);

    // 3. Execute Order 1 for 35 shares -> 25 shares remaining
    uint64_t trade_vol = 0;
    book->set_trade_callback([](const Trade& trade, void* user_data) {
        *static_cast<uint64_t*>(user_data) += trade.quantity;
    }, &trade_vol);

    ok = book->execute_order(1, 35, 9001, 12345);
    assert(ok);
    assert(trade_vol == 35);
    assert(book->get_best_bid()->total_volume == 25);

    // 4. Replace Order 1 with Order 2 @ $100.05 (10005 ticks) for 80 shares
    ok = book->replace_order(1, 2, 10005, 80, 12346);
    assert(ok);
    assert(book->get_best_bid()->price == 10005);
    assert(book->get_best_bid()->total_volume == 80);
    assert(book->get_resting_order_count() == 1);

    // 5. Verify reducing more than remaining cancels the order
    ok = book->reduce_order(2, 80);
    assert(ok);
    assert(!book->get_best_bid().has_value());
    assert(book->get_resting_order_count() == 0);

    std::cout << "PASSED [OK]\n";
}

void test_itch_feed_handler_lifecycle() {
    std::cout << "[Test 6] Verifying ITCH Wire Parsing (Add, Exec, Cancel, Replace, Delete)... ";
    OrderBook book;
    FeedHandler handler(book);

    // 1. ITCH Add Order 'A': Buy 100 shares of AAPL @ $150.00 (1500000)
    itch::AddOrderMsg add_msg{};
    add_msg.length = to_big_endian16(36);
    add_msg.message_type = 'A';
    add_msg.stock_locate = to_big_endian16(1);
    add_msg.tracking_number = 0;
    add_msg.order_ref_num = to_big_endian64(5001);
    add_msg.buy_sell_indicator = 'B';
    add_msg.shares = to_big_endian32(100);
    std::memcpy(add_msg.stock, "AAPL    ", 8);
    add_msg.price = to_big_endian32(1500000); // 15000 ticks

    size_t consumed = handler.process_packet(reinterpret_cast<const uint8_t*>(&add_msg), sizeof(add_msg));
    assert(consumed == sizeof(add_msg));
    assert(book.get_best_bid().has_value() && book.get_best_bid()->price == 15000);
    assert(book.get_best_bid()->total_volume == 100);

    // 2. ITCH Order Cancel 'X': Reduce by 30 shares
    itch::OrderCancelMsg cancel_msg{};
    cancel_msg.length = to_big_endian16(sizeof(itch::OrderCancelMsg) - 2);
    cancel_msg.message_type = 'X';
    cancel_msg.stock_locate = to_big_endian16(1);
    cancel_msg.order_ref_num = to_big_endian64(5001);
    cancel_msg.canceled_shares = to_big_endian32(30);

    consumed = handler.process_packet(reinterpret_cast<const uint8_t*>(&cancel_msg), sizeof(cancel_msg));
    assert(consumed == sizeof(cancel_msg));
    assert(book.get_best_bid()->total_volume == 70);

    // 3. ITCH Order Executed 'E': Fill 40 shares
    itch::OrderExecutedMsg exec_msg{};
    exec_msg.length = to_big_endian16(sizeof(itch::OrderExecutedMsg) - 2);
    exec_msg.message_type = 'E';
    exec_msg.stock_locate = to_big_endian16(1);
    exec_msg.order_ref_num = to_big_endian64(5001);
    exec_msg.executed_shares = to_big_endian32(40);
    exec_msg.match_number = to_big_endian64(777);

    consumed = handler.process_packet(reinterpret_cast<const uint8_t*>(&exec_msg), sizeof(exec_msg));
    assert(consumed == sizeof(exec_msg));
    assert(book.get_best_bid()->total_volume == 30);

    // 4. ITCH Order Replace 'U': Replace remaining 30 with order 5002 for 150 shares @ $150.10
    itch::OrderReplaceMsg replace_msg{};
    replace_msg.length = to_big_endian16(sizeof(itch::OrderReplaceMsg) - 2);
    replace_msg.message_type = 'U';
    replace_msg.stock_locate = to_big_endian16(1);
    replace_msg.original_order_ref_num = to_big_endian64(5001);
    replace_msg.new_order_ref_num = to_big_endian64(5002);
    replace_msg.shares = to_big_endian32(150);
    replace_msg.price = to_big_endian32(1501000); // 15010 ticks

    consumed = handler.process_packet(reinterpret_cast<const uint8_t*>(&replace_msg), sizeof(replace_msg));
    assert(consumed == sizeof(replace_msg));
    assert(book.get_best_bid()->price == 15010);
    assert(book.get_best_bid()->total_volume == 150);

    // 5. ITCH Order Delete 'D': Delete order 5002
    itch::OrderDeleteMsg del_msg{};
    del_msg.length = to_big_endian16(sizeof(itch::OrderDeleteMsg) - 2);
    del_msg.message_type = 'D';
    del_msg.stock_locate = to_big_endian16(1);
    del_msg.order_ref_num = to_big_endian64(5002);

    consumed = handler.process_packet(reinterpret_cast<const uint8_t*>(&del_msg), sizeof(del_msg));
    assert(consumed == sizeof(del_msg));
    assert(!book.get_best_bid().has_value());

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
    test_order_book_reduce_execute_replace();
    test_itch_feed_handler_lifecycle();

    std::cout << "==========================================================\n";
    std::cout << "  ALL 6 CORE INVARIANT TESTS PASSED SUCCESSFULLY!         \n";
    std::cout << "==========================================================\n";
    return 0;
}

