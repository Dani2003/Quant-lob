#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define CPU_PAUSE() _mm_pause()
#else
#define CPU_PAUSE() std::this_thread::yield()
#endif

#include "cpu_affinity.hpp"
#include "feed_handler.hpp"
#include "memory_pool.hpp"
#include "order_book.hpp"
#include "ring_buffer.hpp"
#include "tsc_clock.hpp"
#include "risk_engine.hpp"
#include "ouch_messages.hpp"
#include "order_state_machine.hpp"

using namespace lob;

constexpr size_t TOTAL_ORDERS = 1'000'000;
constexpr size_t RING_BUFFER_CAPACITY = 65536; // 64K slots (power of 2)
constexpr size_t MEMORY_POOL_CAPACITY = 65536;

/**
 * @brief Benchmark 1: Pipelined Single-Threaded Push & Pop Loop through RingBuffer.
 */
void run_sequential_ring_buffer_benchmark() {
    std::cout << "==========================================================\n";
    std::cout << "[Benchmark 1] SPSC Ring Buffer Sequential (" << TOTAL_ORDERS << " Orders)\n";
    std::cout << "==========================================================\n";

    RingBuffer<Order, RING_BUFFER_CAPACITY> ring_buffer;
    Order dummy{};

    const auto start_time = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < TOTAL_ORDERS; ++i) {
        dummy.order_id = i + 1;
        dummy.client_id = 42;
        dummy.price = 10050; // $100.50
        dummy.quantity = 100;
        dummy.side = (i % 2 == 0) ? Side::Buy : Side::Sell;
        dummy.type = OrderType::Limit;

        while (!ring_buffer.try_push(dummy)) {
            CPU_PAUSE();
        }

        Order popped_order{};
        while (!ring_buffer.try_pop(popped_order)) {
            CPU_PAUSE();
        }
    }

    const auto end_time = std::chrono::high_resolution_clock::now();
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
    const double elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0;
    const double orders_per_sec = (static_cast<double>(TOTAL_ORDERS) / static_cast<double>(elapsed_ns)) * 1e9;
    const double avg_latency_ns = static_cast<double>(elapsed_ns) / static_cast<double>(TOTAL_ORDERS);

    std::cout << "Total Elapsed Time : " << std::fixed << std::setprecision(3) << elapsed_ms << " ms (" 
              << (elapsed_ns / 1'000) << " us)\n";
    std::cout << "Throughput         : " << std::fixed << std::setprecision(2) << (orders_per_sec / 1e6) << " Million ops/sec\n";
    std::cout << "Average Latency    : " << std::fixed << std::setprecision(2) << avg_latency_ns << " ns / round-trip\n\n";
}

/**
 * @brief Benchmark 2: Concurrent Multi-Threaded SPSC Benchmark with CPU Affinity.
 */
void run_concurrent_spsc_benchmark() {
    std::cout << "==========================================================\n";
    std::cout << "[Benchmark 2] Concurrent SPSC Producer-Consumer (" << TOTAL_ORDERS << " Orders)\n";
    std::cout << "==========================================================\n";

    static RingBuffer<Order, RING_BUFFER_CAPACITY> ring_buffer;

    std::atomic<bool> start_signal{false};
    uint64_t popped_checksum = 0;
    size_t popped_count = 0;

    // Consumer Thread pinned to Core 1
    std::thread consumer([&]() {
        CpuAffinity::pin_current_thread(1);

        while (!start_signal.load(std::memory_order_acquire)) {
            CPU_PAUSE();
        }

        Order order{};
        while (popped_count < TOTAL_ORDERS) {
            if (ring_buffer.try_pop(order)) {
                popped_checksum += order.order_id;
                ++popped_count;
            } else {
                CPU_PAUSE();
            }
        }
    });

    // Producer Thread pinned to Core 0
    CpuAffinity::pin_current_thread(0);

    const auto start_time = std::chrono::high_resolution_clock::now();
    start_signal.store(true, std::memory_order_release);

    for (size_t i = 1; i <= TOTAL_ORDERS; ++i) {
        Order order{};
        order.order_id = i;
        order.client_id = 1001;
        order.price = 25000;
        order.quantity = 50;
        order.side = Side::Buy;
        order.type = OrderType::Limit;

        while (!ring_buffer.try_push(order)) {
            CPU_PAUSE();
        }
    }

    consumer.join();
    const auto end_time = std::chrono::high_resolution_clock::now();

    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
    const double elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0;
    const double throughput_mops = (static_cast<double>(TOTAL_ORDERS) / static_cast<double>(elapsed_ns)) * 1e3;
    const double avg_latency_ns = static_cast<double>(elapsed_ns) / static_cast<double>(TOTAL_ORDERS);

    const uint64_t expected_checksum = (TOTAL_ORDERS * (TOTAL_ORDERS + 1)) / 2;

    std::cout << "Total Elapsed Time : " << std::fixed << std::setprecision(3) << elapsed_ms << " ms (" 
              << (elapsed_ns / 1'000) << " us)\n";
    std::cout << "Throughput         : " << std::fixed << std::setprecision(2) << throughput_mops << " Million orders/sec\n";
    std::cout << "Average Latency    : " << std::fixed << std::setprecision(2) << avg_latency_ns << " ns / order\n";
    std::cout << "Checksum Validation: " << (popped_checksum == expected_checksum ? "PASSED [OK]" : "FAILED") << "\n\n";
}

/**
 * @brief Benchmark 3: Zero-Allocation Memory Pool Hot-Path Benchmark.
 */
void run_memory_pool_benchmark() {
    std::cout << "==========================================================\n";
    std::cout << "[Benchmark 3] Pre-Allocated Memory Pool (" << TOTAL_ORDERS << " Alloc/Deallocs)\n";
    std::cout << "==========================================================\n";

    static MemoryPool<Order, MEMORY_POOL_CAPACITY> pool;
    constexpr size_t BATCH_SIZE = 4096;
    std::vector<Order*> active_orders(BATCH_SIZE, nullptr);

    const auto start_time = std::chrono::high_resolution_clock::now();

    const size_t iterations = TOTAL_ORDERS / BATCH_SIZE;
    for (size_t iter = 0; iter < iterations; ++iter) {
        for (size_t i = 0; i < BATCH_SIZE; ++i) {
            active_orders[i] = pool.allocate();
            active_orders[i]->order_id = iter * BATCH_SIZE + i;
        }

        for (size_t i = 0; i < BATCH_SIZE; ++i) {
            pool.deallocate(active_orders[i]);
        }
    }

    const auto end_time = std::chrono::high_resolution_clock::now();
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
    const double elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0;
    const double avg_cost_ns = static_cast<double>(elapsed_ns) / static_cast<double>(iterations * BATCH_SIZE * 2);

    std::cout << "Total Elapsed Time : " << std::fixed << std::setprecision(3) << elapsed_ms << " ms\n";
    std::cout << "Avg Cost / Alloc   : " << std::fixed << std::setprecision(2) << avg_cost_ns << " ns (Zero kernel overhead)\n";
    std::cout << "Pool Availability  : " << pool.available() << " / " << pool.capacity() << " slots available\n\n";
}

struct OrderCommand {
    enum class Action : uint8_t {
        AddLimit,
        AddMarket,
        Cancel
    };

    Action action;
    Side side;
    uint32_t price;
    uint32_t quantity;
    uint64_t order_id;
};

/**
 * @brief Benchmark 4: Core Limit Order Book (LOB) Matching Stream with Hardware TSC Profiler.
 */
void run_lob_matching_benchmark() {
    std::cout << "==========================================================\n";
    std::cout << "[Benchmark 4] Core LOB Matching Stream + Hardware TSC Profiler\n";
    std::cout << "==========================================================\n";

    constexpr size_t STREAM_SIZE = 1'000'000;
    constexpr uint32_t BASE_MID_PRICE = 10'000; // $100.00 in tick units

    std::cout << "Generating " << STREAM_SIZE << " simulated continuous order events...\n";
    std::vector<OrderCommand> stream;
    stream.reserve(STREAM_SIZE);

    uint64_t rng_state = 0x853c49e6748fea9bULL;
    auto xorshift64 = [&rng_state]() -> uint64_t {
        uint64_t x = rng_state;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return rng_state = x;
    };

    std::vector<uint64_t> active_order_ids;
    active_order_ids.reserve(STREAM_SIZE / 2);

    uint64_t next_order_id = 1;

    for (size_t i = 0; i < STREAM_SIZE; ++i) {
        uint64_t rand_val = xorshift64();
        uint32_t prob = static_cast<uint32_t>(rand_val % 100);

        if (prob < 60) {
            Side side = ((rand_val >> 8) & 1) ? Side::Buy : Side::Sell;
            uint32_t spread_offset = 1 + static_cast<uint32_t>((rand_val >> 16) % 20);
            uint32_t price = (side == Side::Buy) ? (BASE_MID_PRICE - spread_offset) : (BASE_MID_PRICE + spread_offset);
            uint32_t qty = 10 + static_cast<uint32_t>((rand_val >> 24) % 90);

            uint64_t oid = next_order_id++;
            stream.push_back(OrderCommand{OrderCommand::Action::AddLimit, side, price, qty, oid});
            active_order_ids.push_back(oid);
        } else if (prob < 85) {
            Side side = ((rand_val >> 8) & 1) ? Side::Buy : Side::Sell;
            bool is_market = ((rand_val >> 9) & 1);
            uint32_t qty = 5 + static_cast<uint32_t>((rand_val >> 16) % 50);

            if (is_market) {
                stream.push_back(OrderCommand{OrderCommand::Action::AddMarket, side, 0, qty, next_order_id++});
            } else {
                uint32_t cross_offset = static_cast<uint32_t>((rand_val >> 24) % 5);
                uint32_t price = (side == Side::Buy) ? (BASE_MID_PRICE + cross_offset) : (BASE_MID_PRICE - cross_offset);
                stream.push_back(OrderCommand{OrderCommand::Action::AddLimit, side, price, qty, next_order_id++});
            }
        } else {
            if (!active_order_ids.empty()) {
                size_t cancel_idx = (rand_val >> 16) % active_order_ids.size();
                uint64_t cancel_id = active_order_ids[cancel_idx];

                active_order_ids[cancel_idx] = active_order_ids.back();
                active_order_ids.pop_back();

                stream.push_back(OrderCommand{OrderCommand::Action::Cancel, Side::Buy, 0, 0, cancel_id});
            } else {
                uint64_t oid = next_order_id++;
                stream.push_back(OrderCommand{OrderCommand::Action::AddLimit, Side::Buy, BASE_MID_PRICE - 5, 25, oid});
                active_order_ids.push_back(oid);
            }
        }
    }

    std::cout << "Simulated stream ready. Executing matching engine loop...\n\n";

    auto book = std::make_unique<OrderBook>();
    LatencyProfiler<1'000'000> profiler;

    // Warm-up initial book
    for (uint32_t p = 1; p <= 10; ++p) {
        (void)book->add_limit_order(next_order_id++, 100, Side::Buy, BASE_MID_PRICE - p, 100);
        (void)book->add_limit_order(next_order_id++, 100, Side::Sell, BASE_MID_PRICE + p, 100);
    }

    const auto start_time = std::chrono::high_resolution_clock::now();

    for (const auto& cmd : stream) {
        const uint64_t t0 = TscClock::rdtsc();

        switch (cmd.action) {
            case OrderCommand::Action::AddLimit:
                (void)book->add_limit_order(cmd.order_id, 1, cmd.side, cmd.price, cmd.quantity);
                break;
            case OrderCommand::Action::AddMarket:
                (void)book->add_market_order(cmd.order_id, 1, cmd.side, cmd.quantity);
                break;
            case OrderCommand::Action::Cancel:
                (void)book->cancel_order(cmd.order_id);
                break;
        }

        const uint64_t t1 = TscClock::rdtsc();
        profiler.record_cycles(t0, t1);
    }


    const auto end_time = std::chrono::high_resolution_clock::now();

    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
    const double elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0;
    const double ops_per_sec = (static_cast<double>(STREAM_SIZE) / static_cast<double>(elapsed_ns)) * 1e9;

    const uint64_t total_trades = book->get_total_trades_count();
    const uint64_t matched_volume = book->get_total_matched_volume();
    const double trades_per_sec = (static_cast<double>(total_trades) / static_cast<double>(elapsed_ns)) * 1e9;

    std::cout << "Stream Events Processed: " << STREAM_SIZE << " order operations\n";
    std::cout << "Total Elapsed Time     : " << std::fixed << std::setprecision(3) << elapsed_ms << " ms (" 
              << (elapsed_ns / 1'000) << " us)\n";
    std::cout << "Order Ops Throughput   : " << std::fixed << std::setprecision(2) << (ops_per_sec / 1e6) 
              << " Million ops/sec\n";
    std::cout << "Trades Executed        : " << total_trades << " trades\n";
    std::cout << "Matched Share Volume   : " << matched_volume << " units\n";
    std::cout << "Execution Throughput   : " << std::fixed << std::setprecision(2) << (trades_per_sec / 1e6) 
              << " Million trades/sec\n";
    std::cout << "Resting Orders in Book : " << book->get_resting_order_count() << "\n\n";

    // Print Hardware RDTSC Tail Latency Distribution
    profiler.print_report("Matching Engine Hot Path");

    // Top of Book & Level 2 Depth Inspection
    std::cout << "--- Level 2 Market Depth Snapshot (Post-Benchmark) ---\n";
    auto depth = book->get_depth(5);

    auto best_bid = book->get_best_bid();
    auto best_ask = book->get_best_ask();

    if (best_bid && best_ask) {
        std::cout << "Best Bid: $" << std::fixed << std::setprecision(2) << (best_bid->price / 100.0) 
                  << " (Vol: " << best_bid->total_volume << ", Orders: " << best_bid->order_count << ")\n";
        std::cout << "Best Ask: $" << std::fixed << std::setprecision(2) << (best_ask->price / 100.0) 
                  << " (Vol: " << best_ask->total_volume << ", Orders: " << best_ask->order_count << ")\n";
        std::cout << "Spread  : " << book->get_spread() << " ticks ($" << (book->get_spread() / 100.0) << ")\n";
        std::cout << "Mid Prc : $" << std::fixed << std::setprecision(2) << (book->get_mid_price() / 100.0) << "\n\n";
    }

    std::cout << std::setw(12) << "BID VOLUME" << std::setw(12) << "BID PRICE" 
              << " | " 
              << std::setw(12) << "ASK PRICE" << std::setw(12) << "ASK VOLUME" << "\n";
    std::cout << "---------------------------------------------------------\n";

    size_t rows = std::max(depth.bid_depth, depth.ask_depth);
    for (size_t r = 0; r < rows; ++r) {
        if (r < depth.bid_depth) {
            std::cout << std::setw(12) << depth.bids[r].total_volume 
                      << std::setw(12) << depth.bids[r].price;
        } else {
            std::cout << std::setw(24) << " ";
        }

        std::cout << " | ";

        if (r < depth.ask_depth) {
            std::cout << std::setw(12) << depth.asks[r].price 
                      << std::setw(12) << depth.asks[r].total_volume;
        }
        std::cout << "\n";
    }
    std::cout << "==========================================================\n\n";
}

/**
 * @brief Benchmark 5: Binary ITCH 5.0 Network Wire Feed Ingestion.
 */
void run_itch_feed_benchmark() {
    std::cout << "==========================================================\n";
    std::cout << "[Benchmark 5] Binary NASDAQ ITCH 5.0 Feed Ingestion\n";
    std::cout << "==========================================================\n";

    auto book = std::make_unique<OrderBook>();
    FeedHandler handler(*book);

    constexpr size_t MSG_COUNT = 500'000;
    std::vector<itch::AddOrderMsg> binary_stream(MSG_COUNT);
    uint16_t add_order_len = 36;

    for (size_t i = 0; i < MSG_COUNT; ++i) {
        binary_stream[i].length = itch::read_be16(&add_order_len); // 36 bytes payload
        binary_stream[i].message_type = 'A';
        binary_stream[i].stock_locate = 1;
        binary_stream[i].order_ref_num = itch::read_be64(&i);
        binary_stream[i].buy_sell_indicator = (i % 2 == 0) ? 'B' : 'S';
        uint32_t shares = 100;
        binary_stream[i].shares = itch::read_be32(&shares);
        uint32_t price = static_cast<uint32_t>((10000 + (i % 50)) * 100);
        binary_stream[i].price = itch::read_be32(&price);
        std::memcpy(binary_stream[i].stock, "AAPL    ", 8);
    }


    const auto* raw_ptr = reinterpret_cast<const uint8_t*>(binary_stream.data());
    const size_t total_bytes = MSG_COUNT * sizeof(itch::AddOrderMsg);

    const auto start_time = std::chrono::high_resolution_clock::now();

    size_t offset = 0;
    while (offset < total_bytes) {
        size_t consumed = handler.process_packet(raw_ptr + offset, total_bytes - offset);
        if (consumed == 0) break;
        offset += consumed;
    }

    const auto end_time = std::chrono::high_resolution_clock::now();
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
    const double elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0;
    const double msgs_per_sec = (static_cast<double>(handler.messages_processed()) / static_cast<double>(elapsed_ns)) * 1e9;
    const double throughput_mb_s = (static_cast<double>(handler.bytes_processed()) / (1024.0 * 1024.0)) / (static_cast<double>(elapsed_ns) / 1e9);

    std::cout << "Binary Messages Ingested: " << handler.messages_processed() << "\n";
    std::cout << "Total Bytes Parsed      : " << (handler.bytes_processed() / (1024 * 1024)) << " MB\n";
    std::cout << "Ingestion Time          : " << std::fixed << std::setprecision(3) << elapsed_ms << " ms\n";
    std::cout << "Feed Ingestion Rate     : " << std::fixed << std::setprecision(2) << (msgs_per_sec / 1e6) << " Million msgs/sec\n";
    std::cout << "Network Bandwidth Rate  : " << std::fixed << std::setprecision(2) << throughput_mb_s << " MB/s\n";
    std::cout << "==========================================================\n";
}

/**
 * @brief Benchmark 6: Pre-Trade Risk Engine & OUCH Order State Machine Dispatch.
 */
void run_risk_and_execution_benchmark() {
    std::cout << "==========================================================\n";
    std::cout << "[Benchmark 6] Pre-Trade Risk & OUCH Execution Dispatch\n";
    std::cout << "==========================================================\n";

    RiskConfig config;
    config.max_order_qty = 10'000;
    config.max_order_notional = 1'000'000'00; // $1,000,000
    config.max_net_position = 50'000;
    config.price_collar_ticks = 100;
    config.max_orders_per_second = 10'000'000; // High limit for micro-benchmark

    RiskEngine risk(config);
    OrderStateMachine<65536> oms;
    LatencyProfiler<500'000> profiler;

    constexpr size_t OPS = 500'000;
    uint8_t ouch_wire_buffer[128];
    uint32_t reference_mid = 150'00; // $150.00

    const auto start_wall = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < OPS; ++i) {
        uint64_t token = i + 1;
        uint32_t price = reference_mid + (i % 10);
        uint32_t qty = 100;
        uint8_t side = (i % 2 == 0) ? 0 : 1;

        const uint64_t t0 = TscClock::rdtsc();

        // 1. Evaluate Pre-Trade Risk & register in OMS
        auto* ord = oms.submit_order(token, side, price, qty, reference_mid, t0, risk);

        // 2. Serialize wire-level NASDAQ OUCH 5.0 EnterOrder packet
        if (__builtin_expect(ord && ord->status == OrderStatus::PendingNew, 1)) {
            size_t wire_bytes = OuchSerializer::serialize_enter_order(
                ouch_wire_buffer, token, (side == 0 ? 'B' : 'S'), qty, "AAPL", price
            );
            (void)wire_bytes;
        }

        const uint64_t t1 = TscClock::rdtsc();
        profiler.record_cycles(t0, t1);
    }

    const auto end_wall = std::chrono::high_resolution_clock::now();
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_wall - start_wall).count();
    const double elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0;
    const double ops_per_sec = (static_cast<double>(OPS) / static_cast<double>(elapsed_ns)) * 1e9;

    std::cout << "Orders Evaluated & Serialized: " << OPS << "\n";
    std::cout << "Elapsed Time                 : " << std::fixed << std::setprecision(3) << elapsed_ms << " ms\n";
    std::cout << "Throughput                   : " << std::fixed << std::setprecision(2) << (ops_per_sec / 1e6) << " Million ops/sec\n";
    std::cout << "----------------------------------------------------------\n";
    profiler.print_report("Risk+OUCH Pipeline");
    std::cout << "==========================================================\n\n";

}

int main() {
    std::cout << "==========================================================\n";
    std::cout << "  QUANT-LOB: PRODUCTION-GRADE HIGH-FREQUENCY TRADING LOB \n";
    std::cout << "  Architecture: C++20 | Zero-Allocation | RDTSC Profiler \n";
    std::cout << "==========================================================\n\n";

    run_sequential_ring_buffer_benchmark();
    run_concurrent_spsc_benchmark();
    run_memory_pool_benchmark();
    run_lob_matching_benchmark();
    run_itch_feed_benchmark();
    run_risk_and_execution_benchmark();

    return 0;
}

