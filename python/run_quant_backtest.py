#!/usr/bin/env python3
"""
Quant-LOB: End-to-End High-Frequency Backtest Demo.
Simulates 100,000 continuous LOB ticks, executes Avellaneda-Stoikov market-making,
and outputs complete PnL and TCA execution quality metrics.
"""

import random
import time
from quant_lob import PyOrderBook, Side, MarketMakingStrategy, LobBacktester

def generate_synthetic_tick_stream(num_ticks: int = 100_000):
    print(f"Generating {num_ticks:,} realistic NASDAQ ITCH market ticks...")
    random.seed(42)
    ticks = []

    mid_price = 15000 # $150.00
    order_id = 1

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
        qty = random.choice([100, 200, 500])

        action = "ADD"
        if len(ticks) > 100 and random.random() < 0.20:
            action = "CANCEL"
            order_id = random.randint(max(1, len(ticks) - 100), len(ticks))
        else:
            order_id += 1

        ticks.append({
            "id": order_id,
            "side": side,
            "price": price,
            "qty": qty,
            "action": action
        })

    return ticks

def main():
    print("==========================================================")
    print("  QUANT-LOB: PYTHON QUANTITATIVE BACKTESTING SUITE        ")
    print("  Engine: High-Throughput C++20 LOB + TCA Markout Models  ")
    print("==========================================================\n")

    num_ticks = 50_000
    ticks = generate_synthetic_tick_stream(num_ticks)

    strategy = MarketMakingStrategy(
        obi_threshold=0.25,
        min_spread_ticks=2,
        max_inventory=1000,
        inventory_gamma=0.002,
        quote_lot_size=100
    )

    backtester = LobBacktester(strategy)

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
    print(f"Final Realized PnL          : ${result.final_pnl_dollars:,.2f}")
    print(f"Maximum Drawdown            : ${result.max_drawdown_dollars:,.2f}")
    print(f"Annualized Sharpe Ratio     : {result.sharpe_ratio:.2f}")
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
