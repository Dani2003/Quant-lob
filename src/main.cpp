#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define CPU_PAUSE() _mm_pause()
#else
#define CPU_PAUSE() std::this_thread::yield()
#endif

#include "memory_pool.hpp"
#include "order_book.hpp"
#include "ring_buffer.hpp"

using namespace lob;

constexpr size_t TOTAL_ORDERS = 1'000'000;
constexpr size_t RING_BUFFER_CAPACITY = 65536; // 64K slots
constexpr size_t MEMORY_POOL_CAPACITY = 65536;

/**
 * @brief Benchmark 1: Sequential Push & Pop Loop through RingBuffer.
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
 * @brief Benchmark 2: Concurrent Multi-Threaded SPSC Benchmark.
 */
void run_concurrent_spsc_benchmark() {
    std::cout << "==========================================================\n";
    std::cout << "[Benchmark 2] Concurrent SPSC Producer-Consumer (" << TOTAL_ORDERS << " Orders)\n";
    std::cout << "==========================================================\n";

    static RingBuffer<Order, RING_BUFFER_CAPACITY> ring_buffer;

    std::atomic<bool> start_signal{false};
    uint64_t popped_checksum = 0;
    size_t popped_count = 0;

    std::thread consumer([&]() {
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
 * @brief Command representation for simulated continuous order book stream.
 */
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
 * @brief Benchmark 3: Phase 2 Core LOB Matching Engine Throughput.
 * Simulates a continuous realistic order book feed:
 * - 60% Passive Limit orders (liquidity providing / resting)
 * - 25% Aggressive crossing Limit & Market orders (immediate matches/executions)
 * - 15% Cancellations (order lifecycle churn)
 */
void run_lob_matching_benchmark() {
    std::cout << "==========================================================\n";
    std::cout << "[Benchmark 3] Core Limit Order Book (LOB) Matching Stream\n";
    std::cout << "==========================================================\n";

    constexpr size_t STREAM_SIZE = 1'000'000;
    constexpr uint32_t BASE_MID_PRICE = 10'000; // $100.00 in tick units

    std::cout << "Generating " << STREAM_SIZE << " simulated continuous order events...\n";
    std::vector<OrderCommand> stream;
    stream.reserve(STREAM_SIZE);

    // Fast XorShift64 PRNG to generate deterministically reproducible realistic flow
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
            // 60%: Passive Limit Order (adding liquidity near top of book)
            Side side = ((rand_val >> 8) & 1) ? Side::Buy : Side::Sell;
            uint32_t spread_offset = 1 + static_cast<uint32_t>((rand_val >> 16) % 20); // 1 to 20 ticks
            uint32_t price = (side == Side::Buy) ? (BASE_MID_PRICE - spread_offset) : (BASE_MID_PRICE + spread_offset);
            uint32_t qty = 10 + static_cast<uint32_t>((rand_val >> 24) % 90);          // 10 to 100 shares

            uint64_t oid = next_order_id++;
            stream.push_back(OrderCommand{OrderCommand::Action::AddLimit, side, price, qty, oid});
            active_order_ids.push_back(oid);
        } else if (prob < 85) {
            // 25%: Aggressive Crossing Order (Limit or Market order triggering execution)
            Side side = ((rand_val >> 8) & 1) ? Side::Buy : Side::Sell;
            bool is_market = ((rand_val >> 9) & 1);
            uint32_t qty = 5 + static_cast<uint32_t>((rand_val >> 16) % 50);

            if (is_market) {
                stream.push_back(OrderCommand{OrderCommand::Action::AddMarket, side, 0, qty, next_order_id++});
            } else {
                // Aggressive crossing limit price: Buy crosses ask, Sell crosses bid
                uint32_t cross_offset = static_cast<uint32_t>((rand_val >> 24) % 5);
                uint32_t price = (side == Side::Buy) ? (BASE_MID_PRICE + cross_offset) : (BASE_MID_PRICE - cross_offset);
                stream.push_back(OrderCommand{OrderCommand::Action::AddLimit, side, price, qty, next_order_id++});
            }
        } else {
            // 15%: Order Cancellation
            if (!active_order_ids.empty()) {
                size_t cancel_idx = (rand_val >> 16) % active_order_ids.size();
                uint64_t cancel_id = active_order_ids[cancel_idx];

                // Fast swap-and-pop removal from active pool
                active_order_ids[cancel_idx] = active_order_ids.back();
                active_order_ids.pop_back();

                stream.push_back(OrderCommand{OrderCommand::Action::Cancel, Side::Buy, 0, 0, cancel_id});
            } else {
                // Fallback to passive limit order if no active orders to cancel
                uint64_t oid = next_order_id++;
                stream.push_back(OrderCommand{OrderCommand::Action::AddLimit, Side::Buy, BASE_MID_PRICE - 5, 25, oid});
                active_order_ids.push_back(oid);
            }
        }
    }

    std::cout << "Simulated stream ready. Executing matching engine loop...\n\n";

    // Allocate OrderBook (Zero allocations occur after initialization)
    auto book = std::make_unique<OrderBook>();

    // Warm-up cache lines with a small initial book
    for (uint32_t p = 1; p <= 10; ++p) {
        book->add_limit_order(next_order_id++, 100, Side::Buy, BASE_MID_PRICE - p, 100);
        book->add_limit_order(next_order_id++, 100, Side::Sell, BASE_MID_PRICE + p, 100);
    }

    // Benchmark Hot Loop
    const auto start_time = std::chrono::high_resolution_clock::now();

    for (const auto& cmd : stream) {
        switch (cmd.action) {
            case OrderCommand::Action::AddLimit:
                book->add_limit_order(cmd.order_id, 1, cmd.side, cmd.price, cmd.quantity);
                break;
            case OrderCommand::Action::AddMarket:
                book->add_market_order(cmd.order_id, 1, cmd.side, cmd.quantity);
                break;
            case OrderCommand::Action::Cancel:
                book->cancel_order(cmd.order_id);
                break;
        }
    }

    const auto end_time = std::chrono::high_resolution_clock::now();

    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
    const double elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0;
    const double ops_per_sec = (static_cast<double>(STREAM_SIZE) / static_cast<double>(elapsed_ns)) * 1e9;
    const double avg_latency_ns = static_cast<double>(elapsed_ns) / static_cast<double>(STREAM_SIZE);

    const uint64_t total_trades = book->get_total_trades_count();
    const uint64_t matched_volume = book->get_total_matched_volume();
    const double trades_per_sec = (static_cast<double>(total_trades) / static_cast<double>(elapsed_ns)) * 1e9;

    std::cout << "Stream Events Processed: " << STREAM_SIZE << " order operations\n";
    std::cout << "Total Elapsed Time     : " << std::fixed << std::setprecision(3) << elapsed_ms << " ms (" 
              << (elapsed_ns / 1'000) << " us)\n";
    std::cout << "Order Ops Throughput   : " << std::fixed << std::setprecision(2) << (ops_per_sec / 1e6) 
              << " Million ops/sec\n";
    std::cout << "Average Latency / Op   : " << std::fixed << std::setprecision(2) << avg_latency_ns << " ns / operation\n";
    std::cout << "Trades Executed        : " << total_trades << " trades\n";
    std::cout << "Matched Share Volume   : " << matched_volume << " units\n";
    std::cout << "Execution Throughput   : " << std::fixed << std::setprecision(2) << (trades_per_sec / 1e6) 
              << " Million trades/sec\n";
    std::cout << "Resting Orders in Book : " << book->get_resting_order_count() << "\n\n";

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
    std::cout << "==========================================================\n";
}

int main() {
    std::cout << "==========================================================\n";
    std::cout << "  QUANT-LOB: HIGH-FREQUENCY TRADING (HFT) BENCHMARK SUITE \n";
    std::cout << "  Architecture: C++20 | Zero-Allocation | alignas(64) L1  \n";
    std::cout << "==========================================================\n\n";

    run_sequential_ring_buffer_benchmark();
    run_concurrent_spsc_benchmark();
    run_lob_matching_benchmark();

    return 0;
}
