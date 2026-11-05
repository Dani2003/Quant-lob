#!/usr/bin/env python3
"""
Quant-LOB: End-to-End High-Frequency Backtest Demo.
Simulates 100,000 continuous LOB ticks, executes Avellaneda-Stoikov market-making,
and outputs complete PnL and TCA execution quality metrics.
"""

import random
import time
import argparse
from quant_lob import PyOrderBook, Side, MarketMakingStrategy, LobBacktester

def generate_synthetic_tick_stream(num_ticks: int = 100_000, seed: int = 42,
                                   start_price_cents: int = 15_000, base_order_qty: int = 100):
    print(f"Generating {num_ticks:,} synthetic ITCH-shaped market events...")
    random.seed(seed)
    ticks = []

    mid_price = start_price_cents
    next_order_id = 1
    active_order_ids = []

    for i in range(num_ticks):
        # Brownian motion mid-price drift
        drift = random.choice([-1, 0, 0, 1])
        mid_price = max(1000, mid_price + drift)

        side = Side.BUY if random.random() < 0.50 else Side.SELL
        
        # 75% resting limit orders, 25% aggressive taker orders crossing the spread
        is_aggressive = random.random() < 0.25
        if is_aggressive:
            offset = 0  # Market/crossing order matching the inside market
        else:
            offset = random.randint(1, 4)

        price = (mid_price - offset) if side == Side.BUY else (mid_price + offset)
        qty = random.choice([base_order_qty, 2 * base_order_qty, 5 * base_order_qty])

        action = "ADD"
        if active_order_ids and random.random() < 0.20:
            action = "CANCEL"
            order_id = active_order_ids.pop(random.randrange(len(active_order_ids)))
        else:
            order_id = next_order_id
            next_order_id += 1
            active_order_ids.append(order_id)

        ticks.append({
            "id": order_id,
            "side": side,
            "price": price,
            "qty": qty,
            "action": action
        })

    return ticks

def main():
    parser = argparse.ArgumentParser(description="Run the synthetic Quant-LOB paper backtest.")
    parser.add_argument("--ticks", type=int, default=50_000)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--start-price-cents", type=int, default=15_000)
    parser.add_argument("--order-qty", type=int, default=100)
    parser.add_argument("--obi-threshold", type=float, default=0.25)
    parser.add_argument("--aggressive-obi-threshold", type=float, default=0.60)
    parser.add_argument("--min-spread-ticks", type=int, default=2)
    parser.add_argument("--max-inventory", type=int, default=1_000)
    parser.add_argument("--inventory-gamma", type=float, default=0.002)
    parser.add_argument("--markout-horizon-events", type=int, default=50)
    args = parser.parse_args()

    if args.ticks < 1 or args.start_price_cents < 1 or args.order_qty < 1:
        parser.error("ticks, start price, and order quantity must be positive")
    if not 0.0 <= args.obi_threshold <= args.aggressive_obi_threshold <= 1.0:
        parser.error("OBI thresholds must satisfy 0 <= obi <= aggressive <= 1")

    print("==========================================================")
    print("  QUANT-LOB: PYTHON QUANTITATIVE BACKTESTING SUITE        ")
    print("  Engine: High-Throughput C++20 LOB + TCA Markout Models  ")
    print("==========================================================\n")

    num_ticks = args.ticks
    ticks = generate_synthetic_tick_stream(num_ticks, args.seed, args.start_price_cents, args.order_qty)

    strategy = MarketMakingStrategy(
        obi_threshold=args.obi_threshold,
        aggressive_obi_threshold=args.aggressive_obi_threshold,
        min_spread_ticks=args.min_spread_ticks,
        max_inventory=args.max_inventory,
        inventory_gamma=args.inventory_gamma,
        quote_lot_size=args.order_qty
    )

    backtester = LobBacktester(strategy, markout_horizon_events=args.markout_horizon_events)

    print(f"Executing event-driven simulation across {num_ticks:,} market ticks...")
    t0 = time.perf_counter()
    result = backtester.run_simulation(ticks)
    t1 = time.perf_counter()

    elapsed = t1 - t0
    ticks_per_sec = num_ticks / elapsed

    print(f"\nSimulation completed in {elapsed:.3f} seconds ({ticks_per_sec:,.0f} ticks/second)\n")
    print("==========================================================")
    print("              BACKTEST PERFORMANCE SUMMARY                ")
    print("==========================================================")
    print(f"Total Market Ticks Ingested : {result.total_ticks:,}")
    print(f"Total Strategy Trades       : {result.total_trades:,}")
    print(f"Total Volume Executed       : {result.total_volume:,} shares")
    print(f"Final Mark-to-Market PnL    : ${result.final_pnl_dollars:,.2f}")
    print(f"Maximum Drawdown            : ${result.max_drawdown_dollars:,.2f}")
    print(f"Per-Event Sharpe (not ann.) : {result.sharpe_ratio:.2f}")
    print(f"Trade Win Rate              : {result.win_rate:.2f}%")
    print(f"Ending Net Inventory        : {result.final_inventory:+,} shares")
    print("----------------------------------------------------------")
    print("            TRANSACTION COST ANALYSIS (TCA)               ")
    print("----------------------------------------------------------")
    print(f"Avg Effective Half-Spread   : {result.avg_effective_spread_ticks:.3f} ticks")
    print(f"Avg Realized Spread (1ms)   : {result.avg_realized_spread_ticks:.3f} ticks")
    print(f"Adverse Selection Ratio     : {result.adverse_selection_ratio:.2f}% toxic flow")
    print("==========================================================")

if __name__ == "__main__":
    main()
