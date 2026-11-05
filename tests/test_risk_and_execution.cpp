#include <iostream>
#include <cassert>
#include <vector>
#include "../include/risk_engine.hpp"
#include "../include/ouch_messages.hpp"
#include "../include/order_state_machine.hpp"
#include "../include/tsc_clock.hpp"

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

    // Verify both orders can be independently found
    auto* found_a = oms.find_order(token_a);
    auto* found_b = oms.find_order(token_b);
    assert(found_a == ord_a && found_a->order_token == token_a);
    assert(found_b == ord_b && found_b->order_token == token_b);

    // Cancel token A -> becomes tombstone
    oms.on_order_accepted(token_a, 10001);
    oms.on_order_canceled(token_a, 100, risk);
    assert(ord_a->status == OrderStatus::Canceled);

    // Finding token B must STILL work across the tombstone!
    found_b = oms.find_order(token_b);
    assert(found_b == ord_b && found_b->order_token == token_b);

    std::cout << "  -> OMS Hash Collision Probing & Tombstones PASSED.\n";
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

    std::cout << "==========================================================\n";
    std::cout << "   ALL RISK & EXECUTION INVARIANTS PASSED SUCCESSFULLY!  \n";
    std::cout << "==========================================================\n";
    return 0;
}

