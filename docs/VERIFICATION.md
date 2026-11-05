# Quant-LOB v1.0: Verification Record & Benchmark Baseline

This document provides a verified, reproducible record of build commands, unit test results, PCAP replay execution, and micro-benchmark figures for **Quant-LOB v1.0 (Research & Paper/Replay Engine)**.

---

## 1. Testbed Environment

| Parameter | Value |
| :--- | :--- |
| **Commit Hash** | Checked at commit `9ad732b` (branch `main`) |
| **Operating System** | Linux 6.6.x (Ubuntu / WSL2 x86-64) |
| **CPU Architecture** | Intel x86-64 (8 logical cores @ 1.90 GHz) |
| **Compiler** | GNU Compiler Collection (GCC) 15.2.0 |
| **Compilation Flags** | `-O3 -march=native -flto -DNDEBUG -std=c++20` |
| **Build Generator** | CMake 3.28+ with Unix Makefiles |

---

## 2. Deterministic Build Commands

```bash
# Clone and enter directory
cd /mnt/d/NetSol\ Internship/Quant-lob

# Create clean release build directory
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --config Release -j$(nproc)
```

Target binaries produced:
- `lob_tests`: Limit Order Book core invariants and ITCH feed handler unit tests
- `lob_risk_tests`: Pre-trade risk checks, OMS lifecycle, and risk reservation tests
- `lob_network_tca_tests`: MoldUDP64 multicast parsing, gap tracking, and TCA markout tests
- `lob_pcap_tests`: Deterministic PCAP reader, NUMA allocator, and AF_XDP ring tests
- `lob_benchmark`: 6-stage micro-benchmark suite with cycle-accurate RDTSC profiler
- `lob_engine_service`: Standalone 4-core pipelined paper/replay daemon
- `lob_pcap_replay`: Line-rate PCAP packet capture replayer CLI

---

## 3. Unit Test Suite Results (100% Pass)

Run command:
```bash
./lob_tests && ./lob_risk_tests && ./lob_network_tca_tests && ./lob_pcap_tests
```

### A. Core LOB Tests (`lob_tests`)
```
==========================================================
  QUANT-LOB: CORE INVARIANT UNIT TEST SUITE               
==========================================================
[Test 1] Verifying Strict Price-Time (FIFO) Priority... PASSED [OK]
[Test 2] Verifying Partial Fill & In-Place Queue Maintenance... PASSED [OK]
[Test 3] Verifying Multi-Level Book Sweep... PASSED [OK]
[Test 4] Verifying O(1) Cancellation Invariants... PASSED [OK]
[Test 5] Verifying Reduce, Execute & Replace Invariants... PASSED [OK]
[Test 6] Verifying ITCH Wire Parsing (Add, Exec, Cancel, Replace, Delete)... PASSED [OK]
==========================================================
  ALL 6 CORE INVARIANT TESTS PASSED SUCCESSFULLY!         
==========================================================
```

### B. Risk & Execution OMS Tests (`lob_risk_tests`)
```
==========================================================
   RUNNING PHASE 4 & 5: RISK & EXECUTION TEST SUITE       
==========================================================
[TEST] Running Pre-Trade Risk Engine Guardrail Invariants...
  -> Risk Engine Guardrail Checks PASSED.
[TEST] Running Position, Realized PnL & Mark-to-Market Invariants...
  -> PnL & Inventory Accounting Invariants PASSED.
[TEST] Running NASDAQ OUCH 5.0 Serialization Invariants...
  -> NASDAQ OUCH 5.0 Serialization Invariants PASSED.
[TEST] Running Order State Machine (OMS) Lifecycle Invariants...
  -> Order State Machine Lifecycle Invariants PASSED.
[TEST] Running In-Flight Risk Reservations & Pending Limit Checks...
  -> Risk Reservations & In-Flight Checks PASSED.
[TEST] Running OMS Open-Addressing Collision Probing & Tombstones...
  -> OMS Hash Collision Probing & Tombstones PASSED.
[TEST] Running Paper Depth Ledger Alternation, Replenishment & Partial Fills...
  -> Paper Depth Ledger Alternation, Replenishment & Partial Fills PASSED.
[TEST] Running Engine Shutdown & Quiescent Dashboard Reporting...
  -> Engine Shutdown & Quiescent Dashboard Reporting PASSED.
==========================================================
   ALL RISK & EXECUTION INVARIANTS PASSED SUCCESSFULLY!  
==========================================================
```

### C. MoldUDP64 & TCA Tests (`lob_network_tca_tests`)
```
==========================================================
   RUNNING PHASE 7 & 8: MOLDUDP64 & TCA TEST SUITE        
==========================================================
[TEST] Running MoldUDP64 Multicast Protocol & Gap Detection...
  -> MoldUDP64 Multicast & Gap Detection Invariants PASSED.
[TEST] Running Real-Time TCA & Adverse Selection Invariants...
  -> TCA & Adverse Selection Invariants PASSED.
[TEST] Running MoldUDP64 Framing Rejection & Corrupted Wire Tests...
  -> MoldUDP64 Framing Rejection Tests PASSED.
==========================================================
   ALL MOLDUDP64 & TCA INVARIANTS PASSED SUCCESSFULLY!    
==========================================================
```

### D. PCAP & Infrastructure Tests (`lob_pcap_tests`)
```
==========================================================
   RUNNING ADVANCED PHASES: PCAP, AF_XDP & NUMA TESTS     
==========================================================
[TEST] Running NUMA-Aware Memory & Page Pre-faulting...
  -> NUMA Page-Locked Allocation PASSED.
[TEST] Running Deterministic Binary PCAP Replay Engine...
  -> Deterministic PCAP Replay PASSED (500 packets parsed).
[TEST] Running AF_XDP Kernel Bypass Zero-Copy Receiver...
  -> AF_XDP Kernel Bypass Zero-Copy Ingress PASSED.
==========================================================
   ALL ADVANCED HFT INVARIANTS PASSED SUCCESSFULLY!       
==========================================================
```

---

## 4. Controlled Benchmark Results

Run command:
```bash
./lob_benchmark
```

Raw measured performance under Release flags:

| Benchmark Stage | Sample Size | Throughput | Latency (P50 / P99) | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **1. SPSC Ring Sequential** | 1,000,000 orders | **110.60 Million ops/sec** | 9.04 ns / round-trip | Single-threaded push/pop |
| **2. Concurrent SPSC Cross-Core** | 1,000,000 orders | **76.72 Million orders/sec** | 13.03 ns / order | Core 0 $\to$ Core 1 pinned |
| **3. Memory Pool Alloc/Free** | 1,000,000 allocs | **190.80 Million ops/sec** | 5.24 ns / allocation | Zero kernel heap allocations |
| **4. Core LOB Matching Engine** | 1,000,000 events | **4.38 Million ops/sec** | **P50: 149.8 ns \| P99: 622.9 ns** | 320,891 trades executed |
| **5. ITCH 5.0 Feed Ingestion** | 500,000 wire msgs | **8.12 Million msgs/sec** | 294.15 MB/s bandwidth | Zero-copy binary parser |
| **6. Risk Gateway + OMS Dispatch**| 500,000 orders | **5.99 Million ops/sec** | **P50: 133.4 ns \| P99: 319.6 ns** | Pre-trade risk + OUCH wire |

---

## 5. Deterministic PCAP Replay Verification

Run commands:
```bash
# 1. Generate clean synthetic test capture with 10,000 MoldUDP64 packets
./lob_pcap_replay --generate 10000

# 2. Ingest capture through the pipelined engine
./lob_engine_service --symbol AAPL --pcap market_data_capture.pcap
```

Expected verifiable output:
```
Total Packets Replayed     : 10,000
Total ITCH Messages Parsed : 20,000
Replay Wall Time           : 57.201 ms
Packet Processing Rate     : 0.17 Million packets/sec
Message Ingestion Rate     : 0.35 Million msgs/sec
----------------------------------------------------------
Final Market Top of Book   :
  Best Bid                 : $150.00 (1000000 shares)
  Best Ask                 : $150.10 (1000000 shares)
  Resting Orders in Book   : 20000
```

---

## 6. Known Scope & Architectural Limitations

To ensure intellectual honesty and clear technical scope during evaluation:

1. **No Live Exchange Gateway**:
   - The engine formats NASDAQ OUCH 5.0 binary order messages in a local memory buffer for serialization verification.
   - No TCP/IP socket connects to an actual exchange gateway, and no live order entry session is maintained.
2. **Pre-Trade Risk Scope**:
   - The pre-trade risk engine implements algorithmic guardrails (fat-finger size, price collars, net position limits, rate limiters).
   - This models technical risk checks in user-space, but does not constitute legal or regulatory certification under SEC Rule 15c3-5, which applies to broker-dealers with market access.
3. **Market Feed Coverage**:
   - The ITCH parser implements core trading messages: Add Order (`'A'`), Order Executed (`'E'`), Order Cancel (`'X'`), Order Delete (`'D'`), and Order Replace (`'U'`).
   - Administrative and regulatory ITCH messages (System Event, Reg SHO, Trading Action) are intentionally excluded.
4. **MoldUDP64 Sequence Mismatch Handling**:
   - The engine logs sequence gaps and formats MoldUDP64 TCP gap request packets.
   - However, in replay mode, missing sequence ranges are treated as stream discontinuities; automatic out-of-band gap recovery over TCP is not wired to a live retransmission server.
5. **Paper Execution Assumptions**:
   - Aggressive orders execute against displayed BBO volume in paper mode, tracking consumed liquidity across price levels via a dedicated cache (`PaperDepthLedger`) to prevent duplicate execution across alternating prices and accurately reflect market replenishment and cancellations.
   - Deeper multi-level queue depletion, passive queue positioning, and venue market-data transit latencies are not simulated in this simplified paper execution loop.
   - Exchange transaction fees and rebates are excluded from the paper PnL accounting (modeled at 0.0 bps).
   - The Python backtest is a synthetic demonstration tool for parameter sensitivity, not proof of financial profitability or live alpha edge.

