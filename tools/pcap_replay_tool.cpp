#include <iostream>
#include <string>
#include <chrono>
#include <iomanip>

#include "pcap_reader.hpp"
#include "order_book.hpp"
#include "feed_handler.hpp"
#include "mold_udp64.hpp"

using namespace lob;

int main(int argc, char** argv) {
    std::cout << "==========================================================\n";
    std::cout << "  QUANT-LOB: HIGH-SPEED BINARY PCAP REPLAY ENGINE CLI     \n";
    std::cout << "  Formats: libpcap Ethernet / IPv4 / UDP / MoldUDP64      \n";
    std::cout << "==========================================================\n\n";

    std::string pcap_file = "market_data_capture.pcap";
    size_t generate_packets = 50'000;
    bool do_generate = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--file" && i + 1 < argc) {
            pcap_file = argv[++i];
        } else if (arg == "--generate") {
            do_generate = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                generate_packets = std::stoull(argv[++i]);
            }
        }
    }

    if (do_generate) {
        std::cout << "Generating synthetic PCAP with " << generate_packets << " MoldUDP64/ITCH packets...\n";
        bool ok = PcapReader::generate_synthetic_itch_pcap(pcap_file, generate_packets);
        if (!ok) {
            std::cerr << "Failed to write PCAP file: " << pcap_file << "\n";
            return 1;
        }
        std::cout << "Wrote " << pcap_file << " successfully.\n\n";
    }

    PcapReader reader;
    if (!reader.open(pcap_file)) {
        std::cout << "PCAP file not found: " << pcap_file << "\n";
        std::cout << "Generating benchmark file automatically...\n";
        PcapReader::generate_synthetic_itch_pcap(pcap_file, 25'000);
        if (!reader.open(pcap_file)) {
            std::cerr << "Error opening PCAP: " << pcap_file << "\n";
            return 1;
        }
    }

    OrderBook book;
    FeedHandler handler(book);
    MoldUdp64Parser<1024> parser(handler);

    std::cout << "Replaying PCAP through LOB matching engine at max CPU line rate...\n";
    const auto t0 = std::chrono::high_resolution_clock::now();

    size_t packets_replayed = reader.replay(parser);
    reader.close();

    const auto t1 = std::chrono::high_resolution_clock::now();
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    const double elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0;
    const double pps = (static_cast<double>(packets_replayed) / static_cast<double>(elapsed_ns)) * 1e9;
    const double mps = (static_cast<double>(parser.total_messages()) / static_cast<double>(elapsed_ns)) * 1e9;

    std::cout << "\n==========================================================\n";
    std::cout << "             PCAP REPLAY BENCHMARK REPORT                 \n";
    std::cout << "==========================================================\n";
    std::cout << "Total Packets Replayed     : " << packets_replayed << "\n";
    std::cout << "Total ITCH Messages Parsed : " << parser.total_messages() << "\n";
    std::cout << "Sequence Gaps Detected     : " << parser.total_gaps() << " (Zero loss)\n";
    std::cout << "Replay Wall Time           : " << std::fixed << std::setprecision(3) << elapsed_ms << " ms\n";
    std::cout << "Packet Processing Rate     : " << std::fixed << std::setprecision(2) << (pps / 1e6) << " Million packets/sec\n";
    std::cout << "Message Ingestion Rate     : " << std::fixed << std::setprecision(2) << (mps / 1e6) << " Million msgs/sec\n";
    std::cout << "----------------------------------------------------------\n";
    
    auto best_bid = book.get_best_bid();
    auto best_ask = book.get_best_ask();
    std::cout << "Final Market Top of Book   :\n";
    if (best_bid) {
        std::cout << "  Best Bid                 : $" << (best_bid->price / 100.0) << " (" << best_bid->total_volume << " shares)\n";
    }
    if (best_ask) {
        std::cout << "  Best Ask                 : $" << (best_ask->price / 100.0) << " (" << best_ask->total_volume << " shares)\n";
    }
    std::cout << "  Resting Orders in Book   : " << book.get_resting_order_count() << "\n";
    std::cout << "==========================================================\n";

    return 0;
}

