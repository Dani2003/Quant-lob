#!/usr/bin/env python3
"""
Quant-LOB: Asynchronous UDP Binary Market Data Stream Client.
Subscribes to live binary tick, BBO depth, and alpha signals streamed by the C++ engine.
"""

import socket
import struct
import sys
import os

# BinaryMarketDataPacket format (64-byte packed wire layout):
# uint64 sequence
# uint64 timestamp_ns
# char symbol[8]
# uint32 bid_price
# uint32 bid_volume
# uint32 ask_price
# uint32 ask_volume
# uint32 last_trade_price
# uint32 last_trade_volume
# double mid_price
# double micro_price
# float obi
PACKET_FORMAT = "<QQ8sIIIIIIddf"
PACKET_SIZE = struct.calcsize(PACKET_FORMAT)

def main():
    host = "127.0.0.1"
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9876

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((host, port))

    print("================================================================================")
    print("      QUANT-LOB: ASYNCHRONOUS UDP BINARY MARKET FEED CLIENT LISTENER           ")
    print(f"      Listening on {host}:{port} (Packet wire size: {PACKET_SIZE} bytes)       ")
    print("================================================================================")
    print(f"{'Seq':<8} | {'Symbol':<8} | {'Best Bid':<16} | {'Best Ask':<16} | {'Micro-Price':<12} | {'OBI':<8}")
    print("-" * 80)

    count = 0
    try:
        while True:
            data, _ = sock.recvfrom(1024)
            if len(data) < PACKET_SIZE:
                continue

            seq, ts, sym, bid_p, bid_v, ask_p, ask_v, trade_p, trade_v, mid, micro, obi = struct.unpack(
                PACKET_FORMAT, data[:PACKET_SIZE]
            )
            symbol_str = sym.decode('ascii', errors='ignore').strip()

            count += 1
            if count <= 20 or count % 1000 == 0:
                bid_str = f"${bid_p/100:.2f} ({bid_v} sh)"
                ask_str = f"${ask_p/100:.2f} ({ask_v} sh)"
                print(f"{seq:<8} | {symbol_str:<8} | {bid_str:<16} | {ask_str:<16} | ${micro/100:<11.4f} | {obi:>+7.4f}")

    except KeyboardInterrupt:
        print(f"\n[Client] Detached cleanly. Total binary packets received: {count}")
    finally:
        sock.close()

if __name__ == "__main__":
    main()

