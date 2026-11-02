#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <csignal>
#include <vector>
#include <atomic>

#include "trading_engine.hpp"
#include "pcap_reader.hpp"

using namespace lob;

static std::atomic<bool> g_stop_requested{false};

void signal_handler(int) {
    g_stop_requested.store(true, std::memory_order_release);
}

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "==========================================================\n";
    std::cout << "   QUANT-LOB: PRODUCTION HIGH-FREQUENCY TRADING ENGINE   \n";
    std::cout << "   Architecture: 4-Core Pipelined | AF_XDP | SPSC Rings   \n";
    std::cout << "==========================================================\n\n";

    PipelinedTradingEngine::EngineConfig config{};
    config.symbol = "AAPL";
    config.quote_lot_size = 100;
    config.obi_threshold = 0.25f;
    config.inventory_gamma = 0.005;
    config.enable_core_pinning = true;

    std::string pcap_file{};
    size_t simulated_packets = 100'000;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--symbol" && i + 1 < argc) {
            config.symbol = argv[++i];
        } else if (arg == "--pcap" && i + 1 < argc) {
            pcap_file = argv[++i];
        } else if (arg == "--packets" && i + 1 < argc) {
            simulated_packets = std::stoull(argv[++i]);
        } else if (arg == "--no-pinning") {
            config.enable_core_pinning = false;
        }
    }

    auto engine = std::make_unique<PipelinedTradingEngine>(config);
    std::cout << "[1/3] Initializing 4-Core Autonomous Trading Engine...\n";
    std::cout << "  Core Mapping: Ingress [Core " << config.core_ingress 
              << "] | Matcher [Core " << config.core_matching
              << "] | Alpha [Core " << config.core_strategy
              << "] | Execution [Core " << config.core_execution << "]\n";

    engine->start();
    std::cout << "[2/3] Trading Engine threads pinned and running.\n";

    const auto start_wall = std::chrono::high_resolution_clock::now();

    if (!pcap_file.empty()) {
        std::cout << "[3/3] Streaming Historical PCAP Market Capture: " << pcap_file << "\n";
        PcapReader reader;
        if (reader.open(pcap_file)) {
            size_t count = reader.replay(*engine);
            std::cout << "  PCAP Replay Finished: " << count << " packets ingested.\n";
            reader.close();
        } else {
            std::cerr << "  Failed to open PCAP file: " << pcap_file << "\n";
        }
    } else {
        std::cout << "[3/3] Ingesting " << simulated_packets << " Live NASDAQ ITCH 5.0 Packets...\n";
        
        uint32_t base_price = 15000; // $150.00
        for (size_t i = 0; i < simulated_packets && !g_stop_requested.load(std::memory_order_relaxed); ++i) {
            itch::AddOrderMsg msg{};
            msg.length = (std::endian::native == std::endian::little) ? __builtin_bswap16(36) : 36;
            msg.message_type = 'A';
            msg.stock_locate = 1;
            msg.order_ref_num = (std::endian::native == std::endian::little) ? __builtin_bswap64(i + 1) : (i + 1);
            msg.buy_sell_indicator = (i % 2 == 0) ? 'B' : 'S';
            uint32_t shares = 100;
            msg.shares = (std::endian::native == std::endian::little) ? __builtin_bswap32(shares) : shares;
            uint32_t price = static_cast<uint32_t>((base_price + (i % 20)) * 100);
            msg.price = (std::endian::native == std::endian::little) ? __builtin_bswap32(price) : price;
            std::memcpy(msg.stock, "AAPL    ", 8);

            engine->inject_network_packet(reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));

            if ((i & 0x7FFF) == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
    }

    // Allow pipeline to flush
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto end_wall = std::chrono::high_resolution_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_wall - start_wall).count();

    std::cout << "\nProduction Execution Completed in " << elapsed_ms << " ms.\n";
    engine->print_status_dashboard();
    engine->stop();

    std::cout << "Engine cleanly halted. Telemetry unlinked.\n";
    return 0;
}

