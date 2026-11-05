#include <iostream>
#include <cassert>
#include <vector>
#include <cstring>
#include <memory>
#include "../include/risk_engine.hpp"
#include "../include/ouch_messages.hpp"
#include "../include/order_state_machine.hpp"
#include "../include/tsc_clock.hpp"
#include "../include/paper_depth_ledger.hpp"
#include "../include/trading_engine.hpp"

using namespace lob;

void test_risk_engine_limits() {
    std::cout << "[TEST] Running Pre-Trade Risk Engine Guardrail Invariants...\n";

    RiskConfig config;
    config.max_order_qty = 1000;
    config.max_order_notional = 100'000'00; // $100,000 in cents
    config.max_net_position = 2000;
    config.price_collar_ticks = 20; // 20 ticks from mid
    config.max_orders_per_second = 10;
    config.max_drawdown_limit = -10'000'00;

    RiskEngine risk(config);
    uint64_t t = 1'000'000'000ULL; // 1s

    // Test 1: Normal Approved Order
    auto res = risk.check_order(0, 100'00, 500, 100'00, t);
    assert(res == RiskCheckResult::Approved);

    // Test 2: Fat-finger Quantity Check
    res = risk.check_order(0, 100'00, 1500, 100'00, t);
    assert(res == RiskCheckResult::ExceedsMaxOrderSize);

    // Test 3: Max Notional Check (100 * 1500 = $150,000 > $100,000)
    res = risk.check_order(0, 200'00, 800, 100'00, t); // 200 * 800 = 160,000
    assert(res == RiskCheckResult::ExceedsMaxNotional);

    // Test 4: Price Collar (Mid is 100'00, order at 100'25 -> 25 ticks > 20 collar)
    res = risk.check_order(0, 100'25, 100, 100'00, t);
    assert(res == RiskCheckResult::PriceCollarBreached);

    // Test 5: Position Limits
    // First, fill 1800 shares
    risk.on_fill(0, 100'00, 1800);
    assert(risk.current_position() == 1800);
    // Attempting to buy another 500 exceeds max_net_position (2300 > 2000)
    res = risk.check_order(0, 100'00, 500, 100'00, t);
    assert(res == RiskCheckResult::PositionLimitBreached);
    // But selling 500 reduces position, should be approved!
    res = risk.check_order(1, 100'00, 500, 100'00, t);
    assert(res == RiskCheckResult::Approved);

    // Test 6: Kill Switch
    risk.trigger_kill_switch();
    assert(risk.is_kill_switch_active());
    res = risk.check_order(1, 100'00, 100, 100'00, t);
    assert(res == RiskCheckResult::KillSwitchActive);
    risk.reset_kill_switch();
    assert(!risk.is_kill_switch_active());
    (void)res;

    std::cout << "  -> Risk Engine Guardrail Checks PASSED.\n";

}

void test_pnl_and_inventory_accounting() {
    std::cout << "[TEST] Running Position, Realized PnL & Mark-to-Market Invariants...\n";

    RiskEngine risk;
    // Buy 100 shares @ $100 (10,000 cents)
    risk.on_fill(0, 100'00, 100);
    assert(risk.current_position() == 100);
    assert(risk.realized_pnl() == 0);

    // Mark to market at $105 (10,500 cents) -> Unrealized = +$500
    risk.mark_to_market(105'00);
    assert(risk.unrealized_pnl() == 500'00);
    assert(risk.total_pnl() == 500'00);

    // Sell 50 shares @ $110 (11,000 cents) -> Realized = 50 * $10 = +$500
    risk.on_fill(1, 110'00, 50);
    assert(risk.current_position() == 50);
    assert(risk.realized_pnl() == 500'00);

    // Mark to market at $110 -> 50 * $10 = $500 unrealized, total = $1000
    risk.mark_to_market(110'00);
    assert(risk.unrealized_pnl() == 500'00);
    assert(risk.total_pnl() == 1000'00);

    // Close remaining 50 shares @ $108 -> Realized += 50 * $8 = +$400, total realized = $900
    risk.on_fill(1, 108'00, 50);
    assert(risk.current_position() == 0);
    assert(risk.realized_pnl() == 900'00);
    risk.mark_to_market(108'00);
    assert(risk.unrealized_pnl() == 0);
    assert(risk.total_pnl() == 900'00);

    std::cout << "  -> PnL & Inventory Accounting Invariants PASSED.\n";
}

void test_ouch_wire_serialization() {
    std::cout << "[TEST] Running NASDAQ OUCH 5.0 Serialization Invariants...\n";

    uint8_t buffer[128];
    uint64_t order_token = 0x123456789ABCDEF0ULL;
    uint32_t shares = 250;
    uint32_t price = 1500000; // $150.0000

    size_t bytes = OuchSerializer::serialize_enter_order(buffer, order_token, 'B', shares, "AAPL", price);
    assert(bytes == sizeof(OuchEnterOrder));

    auto* msg = reinterpret_cast<const OuchEnterOrder*>(buffer);
    assert(msg->type == 'O');
    assert(from_big_endian64(msg->order_token) == order_token);
    assert(msg->buy_sell == 'B');
    assert(from_big_endian32(msg->shares) == shares);
    assert(from_big_endian32(msg->price) == price);
    assert(std::string_view(msg->stock, 4) == "AAPL");
    assert(msg->display == 'Y');

    // Cancel message serialization
    bytes = OuchSerializer::serialize_cancel_order(buffer, order_token, shares);
    assert(bytes == sizeof(OuchCancelOrder));
    auto* cancel_msg = reinterpret_cast<const OuchCancelOrder*>(buffer);
    assert(cancel_msg->type == 'X');
    assert(from_big_endian64(cancel_msg->order_token) == order_token);
    assert(from_big_endian32(cancel_msg->shares) == shares);
    (void)bytes;
    (void)msg;
    (void)cancel_msg;

    std::cout << "  -> NASDAQ OUCH 5.0 Serialization Invariants PASSED.\n";

}

void test_order_state_machine() {
    std::cout << "[TEST] Running Order State Machine (OMS) Lifecycle Invariants...\n";

    RiskEngine risk;
    OrderStateMachine<1024> oms;

    uint64_t token1 = 101;
    uint64_t t = 1'000'000'000ULL;

    // 1. Submit order -> PendingNew
    auto* ord = oms.submit_order(token1, 0, 100'00, 200, 100'00, t, risk);
    assert(ord != nullptr);
    assert(ord->status == OrderStatus::PendingNew);
    assert(ord->leaves_qty == 200);

    // 2. Exchange Accepts -> Active
    bool ok = oms.on_order_accepted(token1, 999001);
    assert(ok);
    assert(ord->status == OrderStatus::Active);
    assert(ord->exchange_ref == 999001);

    // 3. Partial execution: 100 shares @ $100 -> PartiallyFilled
    ok = oms.on_order_executed(token1, 100, 100'00, risk);
    assert(ok);
    assert(ord->status == OrderStatus::PartiallyFilled);
    assert(ord->executed_qty == 100);
    assert(ord->leaves_qty == 100);
    assert(risk.current_position() == 100);

    // 4. Fill remainder: 100 shares @ $100 -> Filled
    ok = oms.on_order_executed(token1, 100, 100'00, risk);
    assert(ok);
    assert(ord->status == OrderStatus::Filled);
    assert(ord->leaves_qty == 0);
    assert(risk.current_position() == 200);

    // 5. Test Cancellation Lifecycle
    uint64_t token2 = 102;
    ord = oms.submit_order(token2, 1, 101'00, 100, 100'00, t, risk);
    assert(ord->status == OrderStatus::PendingNew);
    oms.on_order_accepted(token2, 999002);
    assert(ord->status == OrderStatus::Active);

    oms.request_cancel(token2);
    assert(ord->status == OrderStatus::PendingCancel);

    oms.on_order_canceled(token2, 100, risk);
    assert(ord->status == OrderStatus::Canceled);
    assert(ord->leaves_qty == 0);

    // 6. Test Risk Rejection Lifecycle
    uint64_t token3 = 103;
    // Price Collar Breach: price 150'00 vs mid 100'00 (collar is 50 ticks)
    ord = oms.submit_order(token3, 0, 160'00, 100, 100'00, t, risk);
    assert(ord->status == OrderStatus::RiskRejected);
    assert(ord->risk_result == RiskCheckResult::PriceCollarBreached);

    // 7. Test Fill Racing with Pending Cancel
    uint64_t token4 = 104;
    ord = oms.submit_order(token4, 0, 100'00, 200, 100'00, t, risk);
    assert(ord->status == OrderStatus::PendingNew);
    oms.on_order_accepted(token4, 999004);
    assert(ord->status == OrderStatus::Active);

    // Client requests cancel -> transitions to PendingCancel
    oms.request_cancel(token4);
    assert(ord->status == OrderStatus::PendingCancel);

    // A fill races in before the cancel was processed at venue: 100 shares filled
    ok = oms.on_order_executed(token4, 100, 100'00, risk);
    assert(ok && "Fill racing with PendingCancel must be accepted.");
    assert(ord->status == OrderStatus::PendingCancel);
    assert(ord->leaves_qty == 100);

    // Cancel confirmation arrives for remaining 100 shares
    ok = oms.on_order_canceled(token4, 100, risk);
    assert(ok);
    assert(ord->status == OrderStatus::Canceled);
    assert(ord->leaves_qty == 0);

    // 8. Invalid transitions: cannot fill or cancel an already canceled order
    ok = oms.on_order_executed(token4, 50, 100'00, risk);
    assert(!ok && "Cannot fill an already canceled order.");
    ok = oms.on_order_canceled(token4, 50, risk);
    assert(!ok && "Cannot cancel an already canceled order.");

    (void)ord;
    (void)ok;

    std::cout << "  -> Order State Machine Lifecycle Invariants PASSED.\n";
}

void test_risk_reservations_and_limits() {
    std::cout << "[TEST] Running In-Flight Risk Reservations & Pending Limit Checks...\n";

    RiskConfig config;
    config.max_order_qty = 2000;
    config.max_order_notional = 500'000'00;
    config.max_net_position = 1500; // Hard max net position 1500 shares
    config.price_collar_ticks = 50;

    RiskEngine risk(config);
    uint64_t t = 1'000'000'000ULL;

    // 1. Initial state: Position 0, Pending Buys 0, Pending Sells 0
    assert(risk.current_position() == 0);

    // 2. First order: Buy 1000 shares @ $100.00
    auto res = risk.check_order(0, 100'00, 1000, 100'00, t);
    assert(res == RiskCheckResult::Approved);
    // Reserve the in-flight buy order
    risk.reserve_order(0, 1000);

    // 3. Second order arrives before Order 1 fills: Buy 800 shares @ $100.00
    // Current filled = 0, but pending buys = 1000. Total potential = 1800 > 1500!
    // Must be rejected by risk engine even though filled position is 0!
    res = risk.check_order(0, 100'00, 800, 100'00, t);
    assert(res == RiskCheckResult::PositionLimitBreached);

    // 4. However, a Sell order of 500 shares should still be approved (hedges risk)
    res = risk.check_order(1, 100'00, 500, 100'00, t);
    assert(res == RiskCheckResult::Approved);

    // 5. Order 1 is canceled by exchange: release reservation
    risk.release_order(0, 1000);

    // 6. Now the 800 share buy order should be approved
    res = risk.check_order(0, 100'00, 800, 100'00, t);
    assert(res == RiskCheckResult::Approved);
    risk.reserve_order(0, 800);

    // 7. Order fills for 800 shares: release reservation and record fill
    risk.release_order(0, 800);
    risk.on_fill(0, 100'00, 800);
    assert(risk.current_position() == 800);
    (void)res;

    std::cout << "  -> Risk Reservations & In-Flight Checks PASSED.\n";
}

void test_oms_hash_collision_probing() {
    std::cout << "[TEST] Running OMS Open-Addressing Collision Probing & Tombstones...\n";

    OrderStateMachine<64> oms; // Small capacity 64 to force wrap-around & probing
    RiskEngine risk;
    uint64_t t = 1'000'000ULL;

    // Pick two tokens that hash to the exact same index:
    // hash_token(k) = (k * 11400714819323198485ULL) & 63
    uint64_t token_a = 1;
    size_t hash_a = (token_a * 11400714819323198485ULL) & 63;
    uint64_t token_b = 0;
    for (uint64_t k = 2; k < 10000; ++k) {
        if (((k * 11400714819323198485ULL) & 63) == hash_a) {
            token_b = k;
            break;
        }
    }
    assert(token_b != 0);

    // Submit token A
    auto* ord_a = oms.submit_order(token_a, 0, 100'00, 100, 100'00, t, risk);
    assert(ord_a != nullptr && ord_a->order_token == token_a);

    // Submit colliding token B -> must probe to next slot without overwriting token A
    auto* ord_b = oms.submit_order(token_b, 1, 100'10, 100, 100'00, t, risk);
    assert(ord_b != nullptr && ord_b->order_token == token_b);
    assert(ord_a != ord_b);
    (void)ord_a;
    (void)ord_b;

    // Verify both orders can be independently found
    auto* found_a = oms.find_order(token_a);
    auto* found_b = oms.find_order(token_b);
    assert(found_a == ord_a && found_a->order_token == token_a);
    assert(found_b == ord_b && found_b->order_token == token_b);
    (void)found_a;
    (void)found_b;

    // Cancel token A -> becomes tombstone
    oms.on_order_accepted(token_a, 10001);
    oms.on_order_canceled(token_a, 100, risk);
    assert(ord_a->status == OrderStatus::Canceled);

    // Finding token B must STILL work across the tombstone!
    found_b = oms.find_order(token_b);
    assert(found_b == ord_b && found_b->order_token == token_b);

    std::cout << "  -> OMS Hash Collision Probing & Tombstones PASSED.\n";
}

void test_paper_depth_ledger_price_alternation_and_replenishment() {
    std::cout << "[TEST] Running Paper Depth Ledger Alternation, Replenishment & Partial Fills...\n";

    PaperDepthLedger ledger;

    // 1. Initial Fill at Price A ($150.00 = 15000 ticks): 100 shares displayed, request 100
    uint32_t fill = ledger.consume(15000, 100, 100);
    assert(fill == 100);
    assert(ledger.get_consumed(15000) == 100);
    assert(ledger.get_available(15000, 100) == 0);

    // 2. Next intent shifts to Price B ($150.10 = 15010 ticks): 200 shares displayed, request 50
    fill = ledger.consume(15010, 200, 50);
    assert(fill == 50);
    assert(ledger.get_consumed(15010) == 50);
    assert(ledger.get_available(15010, 200) == 150);

    // 3. Price Alternation (A -> B -> A): Return to Price A with same displayed size (100)
    // Must NOT fill again! Available depth is 0!
    fill = ledger.consume(15000, 100, 100);
    assert(fill == 0 && "Price alternation must prevent duplicate execution against the same displayed shares");
    assert(ledger.get_available(15000, 100) == 0);

    // 4. Depth Replenishment at Price A: New ITCH Add orders arrive, displayed grows to 350 (+250 shares)
    // Available depth must immediately become 250 shares!
    assert(ledger.get_available(15000, 350) == 250);
    fill = ledger.consume(15000, 350, 150);
    assert(fill == 150);
    assert(ledger.get_consumed(15000) == 250);
    assert(ledger.get_available(15000, 350) == 100);

    // Consume remaining 100 shares of replenishment
    fill = ledger.consume(15000, 350, 200);
    assert(fill == 100 && "Partial fill must be capped by the remaining 100 shares of replenishment");
    assert(ledger.get_consumed(15000) == 350);
    assert(ledger.get_available(15000, 350) == 0);

    // 5. Partial fill progression at Price C (15020 ticks)
    // Displayed = 100. Sequential requests: 30, 40, 50.
    fill = ledger.consume(15020, 100, 30);
    assert(fill == 30);
    assert(ledger.get_available(15020, 100) == 70);

    fill = ledger.consume(15020, 100, 40);
    assert(fill == 40);
    assert(ledger.get_available(15020, 100) == 30);

    fill = ledger.consume(15020, 100, 50);
    assert(fill == 30 && "Third step should be partial fill of remaining 30 shares");
    assert(ledger.get_available(15020, 100) == 0);

    fill = ledger.consume(15020, 100, 10);
    assert(fill == 0);

    // 6. External Cancellation at Price D (15030 ticks)
    // Displayed = 100. We consume 40. Then market cancels 40 shares (displayed shrinks to 60).
    fill = ledger.consume(15030, 100, 40);
    assert(fill == 40);
    // Displayed drops to 60 in market: available is 60 - 40 = 20
    assert(ledger.get_available(15030, 60) == 20);
    fill = ledger.consume(15030, 60, 50);
    assert(fill == 20 && "Fill must respect market cancellations");
    assert(ledger.get_available(15030, 60) == 0);
    (void)fill;

    std::cout << "  -> Paper Depth Ledger Alternation, Replenishment & Partial Fills PASSED.\n";
}

void test_engine_shutdown_and_quiescent_reporting() {
    std::cout << "[TEST] Running Engine Shutdown & Quiescent Dashboard Reporting...\n";

    EngineConfig config{};
    config.symbol = "AAPL";
    config.enable_core_pinning = false;
    config.enable_udp_streaming = false;

    auto engine = std::make_unique<PipelinedTradingEngine>(config);
    engine->start();
    assert(engine->is_running() == true);

    // Inject a synthetic packet
    itch::AddOrderMsg msg{};
    std::memset(&msg, 0, sizeof(msg));
    msg.length = (std::endian::native == std::endian::little) ? __builtin_bswap16(36) : 36;
    msg.message_type = 'A';
    msg.stock_locate = (std::endian::native == std::endian::little) ? __builtin_bswap16(1) : 1;
    msg.order_ref_num = (std::endian::native == std::endian::little) ? __builtin_bswap64(1) : 1;
    msg.buy_sell_indicator = 'B';
    msg.shares = (std::endian::native == std::endian::little) ? __builtin_bswap32(100) : 100;
    msg.price = (std::endian::native == std::endian::little) ? __builtin_bswap32(1500000) : 1500000;
    std::memset(msg.stock, ' ', sizeof(msg.stock));
    std::memcpy(msg.stock, "AAPL", 4);

    bool injected = engine->inject_network_packet(reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
    assert(injected);
    (void)injected;

    // Gracefully stop engine: workers join, engine becomes quiescent
    engine->stop();
    assert(engine->is_running() == false);

    // Verify dashboard reporting can be executed on quiescent engine without data races
    engine->print_status_dashboard();

    std::cout << "  -> Engine Shutdown & Quiescent Dashboard Reporting PASSED.\n";
}

void test_paper_depth_ledger_collisions_and_capacity_exhaustion() {
    std::cout << "[TEST] Running Paper Depth Ledger Collisions & Capacity Exhaustion...\n";

    PaperDepthLedger ledger;

    // 1. Generate prices that hash to the exact same index
    // hash_price(p) = (p * 2654435761u) & PaperDepthLedger::MASK (255)
    std::vector<uint32_t> colliding_prices;
    const size_t target_hash = 42;
    for (uint32_t p = 1; p < 100000 && colliding_prices.size() < PaperDepthLedger::MAX_PROBES; ++p) {
        if (((static_cast<size_t>(p) * 2654435761u) & PaperDepthLedger::MASK) == target_hash) {
            colliding_prices.push_back(p);
        }
    }
    assert(colliding_prices.size() == PaperDepthLedger::MAX_PROBES);

    // 2. Consume liquidity at each colliding price within the probe budget
    for (size_t i = 0; i < colliding_prices.size(); ++i) {
        uint32_t p = colliding_prices[i];
        uint32_t qty = static_cast<uint32_t>(10 * (i + 1));
        uint32_t fill = ledger.consume(p, 1000, qty);
        assert(fill == qty);
        assert(ledger.get_consumed(p) == qty);
        (void)fill;
    }

    // Verify all colliding prices retained their individual consumed states without eviction
    for (size_t i = 0; i < colliding_prices.size(); ++i) {
        uint32_t p = colliding_prices[i];
        uint32_t expected_qty = static_cast<uint32_t>(10 * (i + 1));
        assert(ledger.get_consumed(p) == expected_qty);
        (void)p;
        (void)expected_qty;
    }

    // 3. Find one more price that hashes to the exact same saturated probe sequence
    uint32_t overflow_price = 0;
    for (uint32_t p = colliding_prices.back() + 1; p < 200000; ++p) {
        if (((static_cast<size_t>(p) * 2654435761u) & PaperDepthLedger::MASK) == target_hash) {
            overflow_price = p;
            break;
        }
    }
    assert(overflow_price != 0);

    // 4. Probe exhaustion: must fail closed! Fill must be 0, and MUST NOT evict any existing entry!
    uint32_t overflow_fill = ledger.consume(overflow_price, 500, 100);
    assert(overflow_fill == 0 && "Exhausted ledger must fail closed and refuse untracked executions");
    (void)overflow_fill;

    // 5. Verify that none of the original entries were evicted or corrupted by the overflow attempt
    for (size_t i = 0; i < colliding_prices.size(); ++i) {
        uint32_t p = colliding_prices[i];
        uint32_t expected_qty = static_cast<uint32_t>(10 * (i + 1));
        assert(ledger.get_consumed(p) == expected_qty && "Overflow must never evict existing consumed state");
        (void)p;
        (void)expected_qty;
    }

    std::cout << "  -> Paper Depth Ledger Collisions & Capacity Exhaustion PASSED.\n";
}

void test_wait_free_triple_buffer_concurrency() {
    std::cout << "[TEST] Running Wait-Free Triple Buffer Multithreaded Concurrency...\n";

    struct TestPayload {
        uint64_t seq{0};
        uint64_t a{0};
        uint64_t b{0};
        uint64_t c{0};
    };

    WaitFreeTripleBuffer<TestPayload> buffer;
    std::atomic<bool> stop_flag{false};
    constexpr size_t TOTAL_WRITES = 50'000;

    std::thread writer([&]() {
        for (uint64_t i = 1; i <= TOTAL_WRITES; ++i) {
            TestPayload p{i, i * 2, i * 3, i * 4};
            buffer.publish(p);
        }
        stop_flag.store(true, std::memory_order_release);
    });

    uint64_t snapshots_read = 0;
    while (!stop_flag.load(std::memory_order_acquire)) {
        TestPayload out{};
        if (buffer.read_snapshot(out)) {
            // Verify payload consistency (no torn reads across non-atomic fields)
            assert(out.b == out.a * 2);
            assert(out.c == out.a * 3);
            ++snapshots_read;
        }
    }
    writer.join();

    TestPayload final_out{};
    bool ok = buffer.read_snapshot(final_out);
    assert(ok);
    assert(final_out.seq == TOTAL_WRITES);
    (void)ok;
    (void)snapshots_read;

    std::cout << "  -> Wait-Free Triple Buffer Multithreaded Concurrency PASSED.\n";
}

void test_concurrent_dashboard_snapshot_and_workers_joined_state() {
    std::cout << "[TEST] Running Concurrent Dashboard Snapshot & Workers Joined State...\n";

    EngineConfig config{};
    config.symbol = "AAPL";
    config.enable_core_pinning = false;
    config.enable_udp_streaming = false;

    auto engine = std::make_unique<PipelinedTradingEngine>(config);
    assert(engine->are_workers_joined() == true);

    engine->start();
    assert(engine->is_running() == true);
    assert(engine->are_workers_joined() == false);

    std::atomic<bool> feeder_running{true};
    constexpr size_t PACKET_COUNT = 5'000;

    // Background thread actively flooding the engine with ITCH packets to drive live telemetry updates
    std::thread feeder([&]() {
        for (size_t i = 0; i < PACKET_COUNT; ++i) {
            itch::AddOrderMsg msg{};
            std::memset(&msg, 0, sizeof(msg));
            msg.length = (std::endian::native == std::endian::little) ? __builtin_bswap16(36) : 36;
            msg.message_type = 'A';
            msg.stock_locate = (std::endian::native == std::endian::little) ? __builtin_bswap16(1) : 1;
            msg.order_ref_num = (std::endian::native == std::endian::little) ? __builtin_bswap64(i + 1) : (i + 1);
            msg.buy_sell_indicator = (i % 2 == 0) ? 'B' : 'S';
            msg.shares = (std::endian::native == std::endian::little) ? __builtin_bswap32(100) : 100;
            uint32_t price = static_cast<uint32_t>((15000 + (i % 10)) * 100);
            msg.price = (std::endian::native == std::endian::little) ? __builtin_bswap32(price) : price;
            std::memset(msg.stock, ' ', sizeof(msg.stock));
            std::memcpy(msg.stock, "AAPL", 4);

            (void)engine->inject_network_packet(reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
        }
        feeder_running.store(false, std::memory_order_release);
    });

    // Main thread continuously reads live snapshots while writer is actively publishing
    size_t live_reads = 0;
    while (feeder_running.load(std::memory_order_acquire)) {
        ShmTelemetryBlock snapshot{};
        if (engine->read_telemetry_snapshot(snapshot)) {
            // Verify invariant consistency
            assert(snapshot.messages_ingested <= PACKET_COUNT);
            ++live_reads;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    feeder.join();
    assert(live_reads > 0 && "Must capture live telemetry snapshots concurrently with active writer");

    // Invoke dashboard once during live transition
    engine->print_status_dashboard();

    // Stop the engine: workers join cleanly
    engine->stop();
    assert(engine->is_running() == false);
    assert(engine->are_workers_joined() == true);

    // Verify post-stop quiescent dashboard call
    engine->print_status_dashboard();

    std::cout << "  -> Concurrent Dashboard Snapshot & Workers Joined State PASSED.\n";
}

int main() {
    std::cout << "==========================================================\n";
    std::cout << "   RUNNING PHASE 4 & 5: RISK & EXECUTION TEST SUITE       \n";
    std::cout << "==========================================================\n";

    test_risk_engine_limits();
    test_pnl_and_inventory_accounting();
    test_ouch_wire_serialization();
    test_order_state_machine();
    test_risk_reservations_and_limits();
    test_oms_hash_collision_probing();
    test_paper_depth_ledger_price_alternation_and_replenishment();
    test_paper_depth_ledger_collisions_and_capacity_exhaustion();
    test_wait_free_triple_buffer_concurrency();
    test_engine_shutdown_and_quiescent_reporting();
    test_concurrent_dashboard_snapshot_and_workers_joined_state();

    std::cout << "==========================================================\n";
    std::cout << "   ALL RISK & EXECUTION INVARIANTS PASSED SUCCESSFULLY!  \n";
    std::cout << "==========================================================\n";
    return 0;
}

