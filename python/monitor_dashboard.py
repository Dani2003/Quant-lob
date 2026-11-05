#!/usr/bin/env python3
"""
Quant-LOB: Real-Time Terminal Telemetry Dashboard.
Reads live shared memory / C++ engine metrics and displays an institutional trading terminal.
"""

import sys
import time
import os
import mmap
import ctypes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from quant_lob import PyOrderBook, Side


def render_ascii_dashboard(
    bid_p, bid_v, ask_p, ask_v, mid, micro, obi,
    msgs, matched, trades, volume, pos, realized_pnl, unrealized_pnl, p99_ns
):
    os.system('cls' if os.name == 'nt' else 'clear')
    print("================================================================================")
    print("         QUANT-LOB: PAPER / REPLAY MONITOR                                      ")
    print("================================================================================")
    print(f" Status: PAPER / REPLAY | Engine: C++ | Telemetry: Shared Memory")
    print("--------------------------------------------------------------------------------")
    print(" MARKET DEPTH (BBO)                    | QUANTITATIVE ALPHA SIGNALS             ")
    print(f" Best Bid : ${bid_p/100:>7.2f}  ({bid_v:>6} sh)     | Mid-Price     : ${mid/100:>7.2f}               ")
    print(f" Best Ask : ${ask_p/100:>7.2f}  ({ask_v:>6} sh)     | Micro-Price   : ${micro/100:>7.4f}             ")
    spread_cents = (ask_p - bid_p) / 100.0 if ask_p > bid_p else 0.0
    print(f" Spread   : ${spread_cents:>7.2f}  ({ask_p - bid_p:>4} ticks)    | 4-Level OBI   : {obi:>+7.4f}                 ")
    print("--------------------------------------------------------------------------------")
    print(" PAPER RISK / OMS                         | PIPELINE COUNTERS                    ")
    print(f" Net Position : {pos:>+7} shares         | Wire Packets  : {msgs:>10,}            ")
    print(f" Realized PnL : ${realized_pnl/100:>+10.2f}            | Add Events    : {matched:>10,}            ")
    print(f" Unrealized   : ${unrealized_pnl/100:>+10.2f}            | Market Execs  : {trades:>10,}            ")
    total_pnl = realized_pnl + unrealized_pnl
    print(f" Total PnL    : ${total_pnl/100:>+10.2f}            | P99 Latency   : {p99_ns:>8.1f} ns           ")
    print("================================================================================")
    print(" Press Ctrl+C to detach monitor.")

def main():
    if "--demo" in sys.argv:
        run_demo()
        return
    if os.name != "posix":
        raise SystemExit("Live shared-memory telemetry is available on POSIX systems. Use --demo for sample data.")
    shm_path = "/dev/shm/quant_lob_telemetry"
    try:
        fd = os.open(shm_path, os.O_RDONLY)
    except OSError as exc:
        raise SystemExit(f"Engine telemetry not found at {shm_path}. Start the engine first, or pass --demo.") from exc

    class Telemetry(ctypes.Structure):
        _fields_ = [
            ("sequence", ctypes.c_uint64), ("heartbeat_timestamp_ns", ctypes.c_uint64),
            ("best_bid_price", ctypes.c_uint32), ("best_bid_volume", ctypes.c_uint32),
            ("best_ask_price", ctypes.c_uint32), ("best_ask_volume", ctypes.c_uint32),
            ("spread_ticks", ctypes.c_uint32), ("mid_price", ctypes.c_double),
            ("micro_price", ctypes.c_double), ("weighted_obi", ctypes.c_float),
            ("messages_ingested", ctypes.c_uint64), ("orders_matched", ctypes.c_uint64),
            ("trades_executed", ctypes.c_uint64), ("volume_traded", ctypes.c_uint64),
            ("net_inventory", ctypes.c_int64), ("realized_pnl_cents", ctypes.c_int64),
            ("unrealized_pnl_cents", ctypes.c_int64), ("total_pnl_cents", ctypes.c_int64),
            ("active_orders_resting", ctypes.c_uint64), ("risk_rejections", ctypes.c_uint64),
            ("kill_switch_active", ctypes.c_bool), ("p50_latency_ns", ctypes.c_double),
            ("p99_latency_ns", ctypes.c_double), ("p99_9_latency_ns", ctypes.c_double),
            ("avg_effective_half_spread", ctypes.c_double), ("avg_realized_spread_1ms", ctypes.c_double),
            ("toxic_flow_ratio", ctypes.c_double),
        ]

    telemetry_map = None
    try:
        telemetry_map = mmap.mmap(fd, ctypes.sizeof(Telemetry), access=mmap.ACCESS_READ)
        while True:
            snapshot = Telemetry.from_buffer_copy(telemetry_map[:ctypes.sizeof(Telemetry)])
            if snapshot.sequence & 1:
                time.sleep(0.01)
                continue
            confirm = Telemetry.from_buffer_copy(telemetry_map[:ctypes.sizeof(Telemetry)])
            if snapshot.sequence != confirm.sequence:
                continue
            render_ascii_dashboard(
                snapshot.best_bid_price, snapshot.best_bid_volume,
                snapshot.best_ask_price, snapshot.best_ask_volume,
                snapshot.mid_price, snapshot.micro_price, snapshot.weighted_obi,
                snapshot.messages_ingested, snapshot.orders_matched,
                snapshot.trades_executed, snapshot.volume_traded,
                snapshot.net_inventory, snapshot.realized_pnl_cents,
                snapshot.unrealized_pnl_cents, snapshot.p99_latency_ns,
            )
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    finally:
        if telemetry_map is not None:
            telemetry_map.close()
        os.close(fd)

def run_demo():
    print("Demo mode: values below are generated locally, not read from an engine.")
    t = 0
    pos = 0
    realized_pnl = 0
    unrealized_pnl = 0

    base_bid = 15000
    base_ask = 15002

    try:
        while True:
            t += 1
            bid_v = 1200 + (t % 10) * 50
            ask_v = 800 - (t % 10) * 30
            mid = (base_bid + base_ask) / 2.0
            micro = (bid_v * base_ask + ask_v * base_bid) / (bid_v + ask_v)
            obi = (bid_v - ask_v) / (bid_v + ask_v)

            if t % 5 == 0:
                pos += 100 if obi > 0 else -100
                realized_pnl += 250

            unrealized_pnl = pos * 5

            render_ascii_dashboard(
                base_bid, bid_v, base_ask, ask_v, mid, micro, obi,
                msgs=t * 1500, matched=t * 800, trades=t * 120, volume=t * 12000,
                pos=pos, realized_pnl=realized_pnl, unrealized_pnl=unrealized_pnl, p99_ns=37.4
            )
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\nDetached from Quant-LOB Monitor.")

if __name__ == "__main__":
    main()
