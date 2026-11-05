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
    (void)ord;
    (void)ok;

    std::cout << "  -> Order State Machine Lifecycle Invariants PASSED.\n";

}

int main() {
    std::cout << "==========================================================\n";
    std::cout << "   RUNNING PHASE 4 & 5: RISK & EXECUTION TEST SUITE       \n";
    std::cout << "==========================================================\n";

    test_risk_engine_limits();
    test_pnl_and_inventory_accounting();
    test_ouch_wire_serialization();
    test_order_state_machine();

    std::cout << "==========================================================\n";
    std::cout << "   ALL RISK & EXECUTION INVARIANTS PASSED SUCCESSFULLY!  \n";
    std::cout << "==========================================================\n";
    return 0;
}

