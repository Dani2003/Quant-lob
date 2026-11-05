#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <csignal>
#include <vector>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <cstdio>

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
    std::cout << "   QUANT-LOB: PAPER / PCAP REPLAY ENGINE                 \n";
    std::cout << "   No live exchange connection is configured             \n";
    std::cout << "==========================================================\n\n";

    PipelinedTradingEngine::EngineConfig config{};
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
        } else if (arg == "--synthetic-price-cents" && i + 1 < argc) {
            config.synthetic_price_cents = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--synthetic-order-qty" && i + 1 < argc) {
            config.synthetic_order_qty = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--quote-lot" && i + 1 < argc) {
            config.quote_lot_size = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--obi-threshold" && i + 1 < argc) {
            config.obi_threshold = std::stof(argv[++i]);
        } else if (arg == "--aggressive-obi-threshold" && i + 1 < argc) {
            config.aggressive_obi_threshold = std::stof(argv[++i]);
        } else if (arg == "--min-spread-ticks" && i + 1 < argc) {
            config.min_spread_ticks = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--obi-weights" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%f,%f,%f,%f", &config.obi_level_weights[0],
                            &config.obi_level_weights[1], &config.obi_level_weights[2],
                            &config.obi_level_weights[3]) != 4) {
                std::cerr << "--obi-weights requires four comma-separated numbers.\n";
                return 2;
            }
        } else if (arg == "--inventory-gamma" && i + 1 < argc) {
            config.inventory_gamma = std::stod(argv[++i]);
        } else if (arg == "--core-ingress" && i + 1 < argc) {
            config.core_ingress = std::stoi(argv[++i]);
        } else if (arg == "--core-matching" && i + 1 < argc) {
            config.core_matching = std::stoi(argv[++i]);
        } else if (arg == "--core-strategy" && i + 1 < argc) {
            config.core_strategy = std::stoi(argv[++i]);
        } else if (arg == "--core-execution" && i + 1 < argc) {
            config.core_execution = std::stoi(argv[++i]);
        } else if (arg == "--max-order-qty" && i + 1 < argc) {
            config.risk_config.max_order_qty = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--max-position" && i + 1 < argc) {
            config.risk_config.max_net_position = std::stoll(argv[++i]);
        } else if (arg == "--max-notional-cents" && i + 1 < argc) {
            config.risk_config.max_order_notional = std::stoull(argv[++i]);
        } else if (arg == "--price-collar-ticks" && i + 1 < argc) {
            config.risk_config.price_collar_ticks = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--max-orders-per-second" && i + 1 < argc) {
            config.risk_config.max_orders_per_second = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--max-drawdown-cents" && i + 1 < argc) {
            config.risk_config.max_drawdown_limit = -std::llabs(std::stoll(argv[++i]));
        } else if (arg == "--udp-host" && i + 1 < argc) {
            config.udp_host = argv[++i];
        } else if (arg == "--no-pinning") {
            config.enable_core_pinning = false;
        } else if (arg == "--udp-port" && i + 1 < argc) {
            config.udp_port = static_cast<uint16_t>(std::stoul(argv[++i]));
            config.enable_udp_streaming = true;
        } else if (arg == "--no-udp") {
            config.enable_udp_streaming = false;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: lob_engine_service [--symbol SYM] [--pcap FILE] [--packets N] [options]\n"
                      << "  --quote-lot N --obi-threshold X --aggressive-obi-threshold X\n"
                      << "  --min-spread-ticks N --obi-weights W1,W2,W3,W4 --inventory-gamma X\n"
                      << "  --core-ingress N --core-matching N --core-strategy N --core-execution N\n"
                      << "  --synthetic-price-cents N --synthetic-order-qty N\n"
                      << "  --max-order-qty N --max-position N --max-notional-cents N\n"
                      << "  --price-collar-ticks N --max-orders-per-second N --max-drawdown-cents N\n"
                      << "  --udp-host HOST --udp-port PORT --no-udp --no-pinning\n";
            return 0;
        } else {
            std::cerr << "Unknown or incomplete argument: " << arg << "\n";
            return 2;
        }
    }

    const float obi_weight_sum = config.obi_level_weights[0] + config.obi_level_weights[1] +
                                 config.obi_level_weights[2] + config.obi_level_weights[3];
    bool invalid_obi_weights = !std::isfinite(obi_weight_sum) || obi_weight_sum <= 0.0f;
    for (float weight : config.obi_level_weights)
        invalid_obi_weights = invalid_obi_weights || !std::isfinite(weight) || weight < 0.0f;

    if (config.symbol.empty() || config.quote_lot_size == 0 || invalid_obi_weights ||
        config.quote_lot_size > config.risk_config.max_order_qty || config.risk_config.max_order_qty == 0 ||
            config.risk_config.max_net_position <= 0 || config.obi_threshold < 0.0f || config.obi_threshold > 1.0f ||
        config.aggressive_obi_threshold < config.obi_threshold || config.aggressive_obi_threshold > 1.0f ||
        !std::isfinite(config.obi_threshold) || !std::isfinite(config.aggressive_obi_threshold) ||
        !std::isfinite(config.inventory_gamma) || config.inventory_gamma < 0.0 ||
        config.risk_config.max_orders_per_second == 0 || config.risk_config.max_order_notional == 0 ||
        config.risk_config.max_drawdown_limit >= 0 || config.udp_port == 0 ||
        config.synthetic_order_qty == 0 || config.synthetic_price_cents == 0 || config.symbol.size() > 8 ||
        config.core_ingress < 0 || config.core_matching < 0 || config.core_strategy < 0 || config.core_execution < 0) {
        std::cerr << "Invalid configuration: review symbol, strategy, risk limits, and UDP port.\n";
        return 2;
    }

    auto engine = std::make_unique<PipelinedTradingEngine>(config);
    std::cout << "[1/3] Initializing local paper/replay pipeline...\n";
    std::cout << "  Core Mapping: Ingress [Core " << config.core_ingress 
              << "] | Matcher [Core " << config.core_matching
              << "] | Alpha [Core " << config.core_strategy
              << "] | Execution [Core " << config.core_execution << "]\n";
    if (config.enable_udp_streaming) {
        std::cout << "  UDP Broadcast: Streaming binary market feed to " << config.udp_host << ":" << config.udp_port << "\n";
    }

    engine->start();
    std::cout << "[2/3] Worker threads running" << (config.enable_core_pinning ? " (pinning requested).\n" : ".\n");

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
        std::cout << "[3/3] Ingesting " << simulated_packets << " synthetic ITCH add messages...\n";
        
        const uint32_t base_price = config.synthetic_price_cents;
        for (size_t i = 0; i < simulated_packets && !g_stop_requested.load(std::memory_order_relaxed); ++i) {
            itch::AddOrderMsg msg{};
            std::memset(&msg, 0, sizeof(msg));
            msg.length = (std::endian::native == std::endian::little) ? __builtin_bswap16(36) : 36;
            msg.message_type = 'A';
            msg.stock_locate = (std::endian::native == std::endian::little) ? __builtin_bswap16(1) : 1;
            msg.order_ref_num = (std::endian::native == std::endian::little) ? __builtin_bswap64(i + 1) : (i + 1);
            msg.buy_sell_indicator = (i % 2 == 0) ? 'B' : 'S';
            uint32_t shares = config.synthetic_order_qty;
            msg.shares = (std::endian::native == std::endian::little) ? __builtin_bswap32(shares) : shares;
            uint32_t price = static_cast<uint32_t>((base_price + (i % 20)) * 100);
            msg.price = (std::endian::native == std::endian::little) ? __builtin_bswap32(price) : price;
            std::memset(msg.stock, ' ', sizeof(msg.stock));
            std::memcpy(msg.stock, config.symbol.data(), std::min(sizeof(msg.stock), config.symbol.size()));

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

    std::cout << "\nReplay completed in " << elapsed_ms << " ms.\n";
    // Stop and join all worker threads first to eliminate any data race with the order book
    engine->stop();
    engine->print_status_dashboard();

    std::cout << "Engine cleanly halted. Telemetry unlinked.\n";
    return 0;
}

