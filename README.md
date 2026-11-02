# Quant-LOB: Ultra-Low Latency C++20 / Rust Limit Order Book & SIMD Alpha Engine

[![C++20](https://img.shields.io/badge/Language-C%2B%2B20-00599C?logo=cplusplus)](https://en.cppreference.com/w/cpp/20)
[![Rust](https://img.shields.io/badge/Language-Rust_2021-DEA584?logo=rust)](https://www.rust-lang.org/)
[![Python](https://img.shields.io/badge/Language-Python_3.10+-3776AB?logo=python)](https://python.org/)
[![Build & Test CI](https://github.com/Dani2003/Quant-lob/actions/workflows/ci.yml/badge.svg)](https://github.com/Dani2003/Quant-lob/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)


A production-grade, sub-microsecond **Price-Time Priority (FIFO) Limit Order Book (LOB)**, **Sub-30ns Pre-Trade Risk Gateway**, **NASDAQ MoldUDP64 Multicast Ingestion Engine**, and **Quantitative Alpha / Backtesting SDK** engineered for institutional High-Frequency Trading (HFT) and automated market making.

---

## 🏛 Institutional System Architecture

```
Quant-LOB
├── include/
│   ├── ring_buffer.hpp          # Lock-free SPSC Ring Buffer (alignas(64), shadow head/tail)
│   ├── memory_pool.hpp          # Pre-allocated O(1) Memory Pool (Zero dynamic heap allocations)
│   ├── order_book.hpp           # Direct flat-array tick indexing & intrusive doubly-linked ladders
│   ├── feed_handler.hpp         # Binary NASDAQ ITCH 5.0 wire protocol packet parser
│   ├── mold_udp64.hpp           # NASDAQ MoldUDP64 Multicast session packet parser & gap detection
│   ├── pcap_reader.hpp          # Deterministic binary PCAP packet capture reader & replay engine
│   ├── kernel_bypass_xdp.hpp    # Linux AF_XDP (XSK) zero-copy kernel bypass packet ingress
│   ├── numa_alloc.hpp           # Institutional NUMA-aware page-locked memory allocator
│   ├── risk_engine.hpp          # Sub-30ns Pre-Trade Risk Engine (Fat-finger, collars, rate limit, kill-switch)
│   ├── ouch_messages.hpp        # Binary NASDAQ OUCH 5.0 order entry wire serialization ('O', 'X', 'A', 'E')
│   ├── order_state_machine.hpp  # Zero-allocation Order State Machine (OMS) lifecycle manager
│   ├── tca_engine.hpp           # Real-time Transaction Cost Analysis (TCA) & Adverse Selection markout engine
│   ├── quant_lob_c_api.h        # High-performance C-ABI bridge for Python bindings
│   ├── tsc_clock.hpp            # Sub-nanosecond RDTSC CPU cycle profiler (P50/P99/P99.9 metrics)
│   └── cpu_affinity.hpp         # OS core pinning (pthread_setaffinity_np / SetThreadAffinityMask)
├── src/
│   ├── order_book.cpp           # Matching engine implementation
│   ├── feed_handler.cpp         # Zero-copy binary ITCH parser
│   ├── quant_lob_c_api.cpp      # C-ABI export implementations
│   └── main.cpp                 # 6-Stage production benchmark suite (LOB, ITCH, Risk, OMS)
├── tests/
│   ├── test_order_book.cpp      # Matching engine invariant unit tests (FIFO, sweeps, cancels)
│   ├── test_risk_and_execution.cpp # Pre-trade risk, PnL accounting, and OUCH lifecycle tests
│   ├── test_network_and_tca.cpp # MoldUDP64 gap recovery & TCA markout unit tests
│   └── test_pcap_and_numa.cpp   # Deterministic PCAP replay, AF_XDP & NUMA invariant tests
├── alpha_engine/                # Rust Quantitative Intelligence Layer (via cxx crate)
│   ├── build.rs                 # CXX FFI native build script
│   ├── include/
│   │   └── order_book_bridge.hpp
│   └── src/
│       ├── lib.rs               # Zero-copy bridge declarations & AlphaEngine API
│       ├── simd_obi.rs          # SSE4.1/AVX SIMD-vectorized Order Book Imbalance & Stoikov Micro-Price
│       ├── strategy.rs          # HFT Market-Making strategy with Avellaneda-Stoikov inventory skew
│       └── main.rs              # 10M calculation throughput & strategy decision benchmark
├── python/                      # High-Throughput Python Quant Research & Backtesting Framework
│   ├── quant_lob/
│   │   ├── __init__.py
│   │   ├── engine.py            # ctypes bridge to C++ matching engine
│   │   └── backtester.py        # Event-driven backtester with markout TCA curves
│   └── run_quant_backtest.py    # 50,000 tick market making simulation demo
└── CMakeLists.txt               # Standard C++20 Release configuration (-O3 -march=native -flto)
```

---

## 🚀 Key Production Engineering Features

### 1. Zero-Allocation Hot Path
- Standard node-based structures (`std::map`, `std::list`) invoke OS kernel memory managers (`malloc`/`new`), causing page faults and unbounded latency spikes.
- Quant-LOB pre-allocates all storage during startup:
  - **MemoryPool**: $O(1)$ intrusive free-list for `OrderNode` instances.
  - **Flat Price Indexing**: `PriceLevel price_levels_[MAX_PRICE_TICKS]` provides true $O(1)$ direct array lookup without tree traversal.
  - **Order Map**: Pre-allocated flat open-addressing hash table with linear probing and tombstones for $O(1)$ cancellations by ID.

### 2. Cache-Line Isolation (`alignas(64)`)
- Eliminates **False Sharing**: Producer state (`tail_`), consumer state (`head_`), and storage slots are padded and aligned to 64-byte boundaries.
- **Shadow Head/Tail Pointers**: Producer and consumer check local copies before reading cross-core atomics with `acquire` semantics, reducing cross-core interconnect (UPI/Infinity Fabric) bandwidth saturation.

### 3. Binary NASDAQ ITCH 5.0 Feed Ingestion
- Wire-level packed structs parse binary messages directly from network buffers without string parsing or allocation:
  - Add Order (`'A'`), Order Executed (`'E'`), Order Cancel (`'X'`), Order Delete (`'D'`), Order Replace (`'U'`).

### 4. NASDAQ MoldUDP64 Multicast Protocol & Gap Recovery
- Ingests multi-message UDP multicast datagrams containing ITCH payloads.
- **Sequence Gap Detection**: Automatically detects dropped packets (`received_seq > expected_seq`), logs missing packet intervals into an intrusive gap table, and generates MoldUDP64 TCP gap re-request packets without blocking the hot path.

### 5. Sub-30ns Pre-Trade Risk Engine & Kill Switch
- Evaluates orders against firm and regulatory limits (SEC Rule 15c3-5 / MiFID II RTS 6) before dispatch:
  - **Atomic Hardware Kill-Switch**: Instantly halts all outgoing order flow.
  - **Fat-Finger Guard**: Hard limits on maximum order quantity and total notional.
  - **Dynamic Price Collars**: Rejects limit prices exceeding $N$ ticks from reference BBO mid-price.
  - **Sliding-Window Rate Limiter**: Throttles bursts to protect against runaway algorithmic loops.
  - **Inventory & Drawdown Circuit Breaker**: Real-time position tracking and automatic liquidation trigger.

### 6. NASDAQ OUCH 5.0 Execution Gateway & OMS
- Binary serialization for NASDAQ OUCH 5.0 order entry (`'O'`) and cancel (`'X'`).
- Zero-allocation Order State Machine (OMS) managing complete lifecycle transitions:
  $$\text{PendingRisk} \longrightarrow \text{PendingNew} \longrightarrow \text{Active} \longrightarrow \text{PartiallyFilled} \longrightarrow \text{Filled} \,/\, \text{Canceled}$$

### 7. Real-Time Transaction Cost Analysis (TCA) & Markouts
- Evaluates execution edge and toxic flow adverse selection at multiple post-trade horizons ($100\,\mu\text{s}, 1\,\text{ms}, 10\,\text{ms}, 100\,\text{ms}$):
  $$\text{EffectiveSpread} = 2 \cdot D \cdot (P_{\text{fill}} - \text{Mid}_{\tau})$$
  $$\text{RealizedSpread}_{\Delta t} = 2 \cdot D \cdot (P_{\text{fill}} - \text{Mid}_{\tau + \Delta t})$$
  $$\text{AdverseSelection}_{\Delta t} = 2 \cdot D \cdot (\text{Mid}_{\tau + \Delta t} - \text{Mid}_{\tau})$$

### 8. Rust SIMD Quantitative Signals & Strategy Engine
- **Order Book Imbalance (OBI)**:
  $$\text{OBI}_k = \frac{\text{BidVolume}_k - \text{AskVolume}_k}{\text{BidVolume}_k + \text{AskVolume}_k} \in [-1.0, 1.0]$$
- **Stoikov Micro-Price**:
  $$\text{MicroPrice} = \frac{\text{BidVolume} \cdot \text{AskPrice} + \text{AskVolume} \cdot \text{BidPrice}}{\text{BidVolume} + \text{AskVolume}}$$
- **Avellaneda-Stoikov Reservation Price**:
  $$R(s, q) = \text{MicroPrice} - q \cdot \gamma \cdot \sigma^2$$
  Actively shades quotes to attract offsetting order flow when holding net inventory ($q$).

### 9. Python Quant Research & Backtesting Framework
- Provides zero-copy bindings to the C++ matching engine via a high-performance C-ABI.
- Event-driven backtester simulating historical or synthetic ITCH tick streams, tracking inventory, PnL, Sharpe ratio, and TCA markouts.

---

## 🛠 Build & Run Instructions

### 1. C++20 Benchmark & Test Suite (CMake)

```bash
cd "/mnt/d/NetSol Internship/Quant-lob"
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..

# Compile in Release mode (-O3 -march=native -flto)
cmake --build . --config Release

# Run invariant unit tests
./lob_tests                 # Core LOB engine tests
./lob_risk_tests            # Pre-trade risk & OMS tests
./lob_network_tca_tests     # MoldUDP64 & TCA markout tests
./lob_pcap_tests            # PCAP replay, AF_XDP & NUMA tests

# Run 6-stage production benchmark suite
./lob_benchmark
```

### 2. Rust FFI Alpha Engine & Strategy (Cargo)

```bash
cd "/mnt/d/NetSol Internship/Quant-lob/alpha_engine"

# Run SIMD OBI, Micro-Price & HFT Strategy benchmark
cargo run --release
```

### 3. Python Quantitative Research & Backtesting

```bash
cd "/mnt/d/NetSol Internship/Quant-lob"

# Run 50,000 tick market making backtest with full TCA reporting
python python/run_quant_backtest.py
```

---

## 🎯 Interview Defense Quick Reference

| Concept | The "Why" & Hardware Mechanics |
| :--- | :--- |
| **False Sharing** | Multiple CPU cores modifying variables residing on the same 64-byte cache line cause the MESI cache protocol to continuously invalidate L1/L2 caches across cores. `alignas(64)` gives each core ownership of its private cache line. |
| **Zero Heap Allocation** | `malloc` and `new` trigger OS kernel syscalls and memory allocator heap locks. Our intrusive memory pool pre-allocates nodes at startup to eliminate all runtime allocations on the critical trading path. |
| **Direct Indexing vs std::map** | `std::map` is a red-black tree requiring $O(\log N)$ pointer traversals and heap-allocated nodes, leading to severe L1/L2 cache misses. Direct flat array indexing delivers guaranteed $O(1)$ memory access in a single CPU instruction. |
| **MoldUDP64 Gap Detection** | Market data over UDP can drop packets during microbursts. Tracking 64-bit sequence numbers enables instant detection of gaps and parallel out-of-band TCP replay requests without stalling live matching. |
| **Pre-Trade Risk in <30ns** | Regulatory compliance (SEC Rule 15c3-5) requires every order to be vetted. Branch predictor optimization (`__builtin_expect`) and cache-line co-location ensure all 8 risk checks complete in ~20-30 nanoseconds. |
| **SIMD Vectorization** | Rather than running a loop with sequential subtractions, additions, and divisions, SIMD instructions (`_mm_sub_ps`, `_mm_div_ps`) process 4 depth levels simultaneously in a single clock cycle. |
| **Adverse Selection & Markouts** | In HFT, executing passive quotes is only profitable if the post-trade price doesn't immediately move through your price. Evaluating 1ms markouts separates informed toxic flow from uninformed retail flow. |
