#include <iostream>
#include <cassert>
#include <vector>
#include <filesystem>
#include "../include/pcap_reader.hpp"
#include "../include/numa_alloc.hpp"
#include "../include/kernel_bypass_xdp.hpp"
#include "../include/order_book.hpp"

using namespace lob;

void test_numa_memory_allocator() {
    std::cout << "[TEST] Running NUMA-Aware Memory & Page Pre-faulting...\n";

    constexpr size_t AllocSize = 1024 * 1024 * 4; // 4 MB
    void* ptr = NumaMemory::allocate_on_node(AllocSize, 0);
    assert(ptr != nullptr && "NUMA allocation failed");

    // Verify memory is readable and writable
    auto* byte_ptr = static_cast<uint8_t*>(ptr);
    for (size_t i = 0; i < AllocSize; i += 4096) {
        byte_ptr[i] = static_cast<uint8_t>(i & 0xFF);
        assert(byte_ptr[i] == static_cast<uint8_t>(i & 0xFF));
    }

    NumaMemory::free_memory(ptr, AllocSize);
    std::cout << "  -> NUMA Page-Locked Allocation PASSED.\n";
}

void test_pcap_replay_engine() {
    std::cout << "[TEST] Running Deterministic Binary PCAP Replay Engine...\n";

    const std::string pcap_file = "test_itch_replay.pcap";
    constexpr size_t PACKET_COUNT = 500;

    // 1. Generate synthetic PCAP capture file
    bool gen_ok = PcapReader::generate_synthetic_itch_pcap(pcap_file, PACKET_COUNT);
    assert(gen_ok && "Failed to generate synthetic PCAP file");
    (void)gen_ok;

    // 2. Setup OrderBook & MoldUDP64 Parser
    OrderBook book;
    FeedHandler handler(book);
    MoldUdp64Parser<512> parser(handler);

    // 3. Open and replay PCAP file
    PcapReader reader;
    bool open_ok = reader.open(pcap_file);
    assert(open_ok && "Failed to open PCAP file");
    (void)open_ok;


    size_t replayed = reader.replay(parser);
    reader.close();

    assert(replayed == PACKET_COUNT && "All packets in PCAP must be replayed");
    assert(parser.total_packets() == PACKET_COUNT);
    assert(parser.total_messages() == PACKET_COUNT * 2);
    assert(parser.total_gaps() == 0 && "Deterministic PCAP should have zero sequence gaps");

    // Verify order book state updated from PCAP ITCH packets
    assert(book.get_best_bid().has_value());
    assert(book.get_best_ask().has_value());
    assert(book.get_best_bid()->price == 15000); // 15000 cents ($150.00)
    assert(book.get_best_ask()->price == 15010); // 15010 cents ($150.10)

    // Cleanup temporary file
    std::filesystem::remove(pcap_file);

    std::cout << "  -> Deterministic PCAP Replay PASSED (" << replayed << " packets parsed).\n";
}

void test_kernel_bypass_xdp() {
    std::cout << "[TEST] Running AF_XDP Kernel Bypass Zero-Copy Receiver...\n";

    OrderBook book;
    FeedHandler handler(book);
    MoldUdp64Parser<512> parser(handler);

    KernelBypassReceiver xdp(0);
    assert(xdp.is_initialized() && "AF_XDP UMEM failed to initialize");

    // Construct raw Ethernet + IPv4 + UDP + MoldUDP64 packet
    uint8_t raw_frame[128];
    auto* eth = reinterpret_cast<EthernetHeader*>(raw_frame);
    eth->ether_type = (std::endian::native == std::endian::little) ? __builtin_bswap16(0x0800) : 0x0800;

    auto* ip = reinterpret_cast<Ipv4Header*>(raw_frame + sizeof(EthernetHeader));
    ip->ihl_version = 0x45;
    ip->protocol = 17;

    auto* udp = reinterpret_cast<UdpHeader*>(raw_frame + sizeof(EthernetHeader) + 20);
    udp->length = (std::endian::native == std::endian::little) ? __builtin_bswap16(sizeof(UdpHeader) + sizeof(MoldUdp64Header)) : 
                                                                 (sizeof(UdpHeader) + sizeof(MoldUdp64Header));

    auto* mold = reinterpret_cast<MoldUdp64Header*>(raw_frame + 42);
    std::memcpy(mold->session, "NASDAQ0001", 10);
    mold->sequence_number = (std::endian::native == std::endian::little) ? __builtin_bswap64(1001) : 1001;
    mold->message_count = 0; // Heartbeat / Empty message block

    // 1. Simulate NIC DMA directly into UMEM chunk
    bool dma_ok = xdp.inject_packet_dma(raw_frame, 42 + sizeof(MoldUdp64Header));
    assert(dma_ok && "NIC DMA packet injection failed");
    (void)dma_ok;

    // 2. Poll RX ring zero-copy
    size_t processed = xdp.poll_and_dispatch(parser, 32);
    assert(processed == 1 && "AF_XDP polling failed to consume packet");
    assert(xdp.total_packets() == 1);
    (void)processed;


    std::cout << "  -> AF_XDP Kernel Bypass Zero-Copy Ingress PASSED.\n";
}

int main() {
    std::cout << "==========================================================\n";
    std::cout << "   RUNNING ADVANCED PHASES: PCAP, AF_XDP & NUMA TESTS     \n";
    std::cout << "==========================================================\n";

    test_numa_memory_allocator();
    test_pcap_replay_engine();
    test_kernel_bypass_xdp();

    std::cout << "==========================================================\n";
    std::cout << "   ALL ADVANCED HFT INVARIANTS PASSED SUCCESSFULLY!       \n";
    std::cout << "==========================================================\n";
    return 0;
}

