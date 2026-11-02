# Quant-LOB: Ultra-Low Latency C++20 / Rust Limit Order Book & SIMD Alpha Engine

[![C++20](https://img.shields.io/badge/Language-C%2B%2B20-00599C?logo=cplusplus)](https://en.cppreference.com/w/cpp/20)
[![Rust](https://img.shields.io/badge/Language-Rust_2021-DEA584?logo=rust)](https://www.rust-lang.org/)
[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

A production-grade, sub-microsecond **Price-Time Priority (FIFO) Limit Order Book (LOB)** and **Quantitative Alpha Engine** engineered for High-Frequency Trading (HFT) and algorithmic market making.

---

## 🏛 System Architecture

```
Quant-LOB
├── include/
│   ├── ring_buffer.hpp         # Lock-free SPSC Ring Buffer (alignas(64), shadow head/tail)
│   ├── memory_pool.hpp         # Pre-allocated O(1) Memory Pool (Zero dynamic heap allocations)
│   ├── order_book.hpp          # Direct flat-array tick indexing & intrusive doubly-linked ladders
│   ├── feed_handler.hpp        # Binary NASDAQ ITCH 5.0 wire protocol packet parser
│   ├── tsc_clock.hpp           # Sub-nanosecond RDTSC CPU cycle profiler (P50/P99/P99.9 metrics)
│   └── cpu_affinity.hpp        # OS core pinning (pthread_setaffinity_np / SetThreadAffinityMask)
├── src/
│   ├── order_book.cpp          # Matching engine implementation
│   ├── feed_handler.cpp        # Zero-copy binary ITCH parser
│   └── main.cpp                # 5-Stage production benchmark suite
├── tests/
│   └── test_order_book.cpp     # Matching engine invariant unit tests (FIFO, sweeps, cancels)
├── alpha_engine/               # Rust Quantitative Intelligence Layer (via cxx crate)
│   ├── build.rs                # CXX FFI native build script
│   ├── include/
│   │   └── order_book_bridge.hpp
│   └── src/
│       ├── lib.rs              # Zero-copy bridge declarations & AlphaEngine API
│       ├── simd_obi.rs         # SSE4.1/AVX SIMD-vectorized Order Book Imbalance & Stoikov Micro-Price
│       └── main.rs             # 10,000,000 calculation throughput benchmark
└── CMakeLists.txt              # Standard C++20 Release configuration (-O3 -march=native -flto)
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

### 4. Tail Latency Hardware Profiler (RDTSC)
- Uses `_mm_lfence()` + `__rdtsc()` CPU cycle instructions to record exact tail latency percentiles (**P50, P90, P99, P99.9, Max**) with sub-nanosecond resolution.

### 5. Rust SIMD Quantitative Signals
- **Order Book Imbalance (OBI)**:
  $$\text{OBI}_k = \frac{\text{BidVolume}_k - \text{AskVolume}_k}{\text{BidVolume}_k + \text{AskVolume}_k} \in [-1.0, 1.0]$$
- **Stoikov Micro-Price**:
  $$\text{MicroPrice} = \frac{\text{BidVolume} \cdot \text{AskPrice} + \text{AskVolume} \cdot \text{BidPrice}}{\text{BidVolume} + \text{AskVolume}}$$
- Vectorized using 128-bit `XMM` registers (`_mm_sub_ps`, `_mm_add_ps`, `_mm_div_ps`) computing all 4 levels in parallel in single CPU clock cycles.

---

## 🛠 Build & Run Instructions

### 1. C++20 Benchmark & Test Suite (CMake)

```bash
# Generate build files
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..

# Compile in Release mode (-O3 -march=native -flto)
cmake --build . --config Release

# Run core invariant tests
./lob_tests

# Run the 5-stage benchmark suite
./lob_benchmark
```

### 2. Rust FFI Alpha Engine (Cargo)

```bash
cd alpha_engine

# Run the SIMD OBI & Micro-Price benchmark
cargo run --release
```

---

## 🎯 Interview Defense Quick Reference

| Concept | The "Why" & Hardware Mechanics |
| :--- | :--- |
| **False Sharing** | Multiple CPU cores modifying variables residing on the same 64-byte cache line cause the MESI cache protocol to continuously invalidate L1/L2 caches across cores. `alignas(64)` gives each core ownership of its private cache line. |
| **Zero Heap Allocation** | `malloc` and `new` trigger OS kernel syscalls and memory allocator heap locks. Our intrusive memory pool pre-allocates nodes at startup to eliminate all runtime allocations on the critical trading path. |
| **Direct Indexing vs std::map** | `std::map` is a red-black tree requiring $O(\log N)$ pointer traversals and heap-allocated nodes, leading to severe L1/L2 cache misses. Direct flat array indexing delivers guaranteed $O(1)$ memory access in a single CPU instruction. |
| **SIMD Vectorization** | Rather than running a loop with sequential subtractions, additions, and divisions, SIMD instructions (`_mm_sub_ps`, `_mm_div_ps`) process 4 depth levels simultaneously in a single clock cycle. |
| **Micro-Price** | Standard mid-price $(P_B + P_A)/2$ ignores queue volume imbalance. Stoikov's Micro-Price weights price by volume, correctly predicting short-term tick drift toward the thinner side of the book. |

