#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <fstream>
#include <bit>
#include <algorithm>
#include "mold_udp64.hpp"

namespace lob {

#pragma pack(push, 1)

/// Standard PCAP Global Header (24 bytes)
struct PcapGlobalHeader {
    uint32_t magic_number{0xa1b2c3d4}; // 0xa1b2c3d4 (microsecond) or 0xa1b23c4d (nanosecond)
    uint16_t version_major{2};
    uint16_t version_minor{4};
    int32_t  thiszone{0};
    uint32_t sigfigs{0};
    uint32_t snaplen{65535};
    uint32_t network{1}; // 1 = LINKTYPE_ETHERNET
};

/// PCAP Packet Record Header (16 bytes)
struct PcapPacketHeader {
    uint32_t ts_sec{0};   // Timestamp seconds
    uint32_t ts_usec{0};  // Timestamp microseconds or nanoseconds
    uint32_t incl_len{0}; // Number of octets of packet saved in file
    uint32_t orig_len{0}; // Actual length of packet
};

/// Ethernet II Header (14 bytes)
struct EthernetHeader {
    uint8_t  dest_mac[6];
    uint8_t  src_mac[6];
    uint16_t ether_type; // 0x0800 for IPv4 (Big-Endian)
};

/// IPv4 Header (20 bytes minimum)
struct Ipv4Header {
    uint8_t  ihl_version; // Version (4 bits) + IHL (4 bits)
    uint8_t  tos;
    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_fragment;
    uint8_t  ttl;
    uint8_t  protocol; // 17 (0x11) for UDP
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dest_ip;
};

/// UDP Header (8 bytes)
struct UdpHeader {
    uint16_t src_port;
    uint16_t dest_port;
    uint16_t length;
    uint16_t checksum;
};

#pragma pack(pop)

/// High-Performance Zero-Copy PCAP File Reader & Replay Engine
/// Parses raw network captures and strips Ethernet/IPv4/UDP headers to feed payloads directly into the LOB.
class PcapReader {
public:
    PcapReader() : is_nanosecond_pcap_(false), byte_swapped_(false) {}

    /// Open and parse global header of a .pcap file
    bool open(const std::string& filepath) {
        file_.open(filepath, std::ios::binary);
        if (!file_.is_open()) {
            return false;
        }

        PcapGlobalHeader header{};
        file_.read(reinterpret_cast<char*>(&header), sizeof(PcapGlobalHeader));
        if (file_.gcount() != sizeof(PcapGlobalHeader)) {
            return false;
        }

        // Validate magic number
        if (header.magic_number == 0xa1b2c3d4) {
            is_nanosecond_pcap_ = false;
            byte_swapped_ = false;
        } else if (header.magic_number == 0xd4c3b2a1) {
            is_nanosecond_pcap_ = false;
            byte_swapped_ = true;
        } else if (header.magic_number == 0xa1b23c4d) {
            is_nanosecond_pcap_ = true;
            byte_swapped_ = false;
        } else if (header.magic_number == 0x4d3cb2a1) {
            is_nanosecond_pcap_ = true;
            byte_swapped_ = true;
        } else {
            return false; // Unknown PCAP format
        }

        return true;
    }

    void close() {
        if (file_.is_open()) {
            file_.close();
        }
    }

    /// Replay the entire PCAP stream into a parser or engine zero-copy
    template <typename ParserType>
    size_t replay(ParserType& parser) {
        if (!file_.is_open()) return 0;

        size_t packets_replayed = 0;
        std::vector<uint8_t> packet_buffer(65536);

        while (file_) {
            PcapPacketHeader pkt_hdr{};
            file_.read(reinterpret_cast<char*>(&pkt_hdr), sizeof(PcapPacketHeader));
            if (file_.gcount() != sizeof(PcapPacketHeader)) {
                break;
            }

            uint32_t cap_len = byte_swapped_ ? __builtin_bswap32(pkt_hdr.incl_len) : pkt_hdr.incl_len;
            if (cap_len > packet_buffer.size() || cap_len == 0) {
                break;
            }

            file_.read(reinterpret_cast<char*>(packet_buffer.data()), cap_len);
            if (static_cast<size_t>(file_.gcount()) != cap_len) {
                break;
            }

            // Strip Network Framing: Ethernet (14) + IPv4 (20) + UDP (8) = 42 bytes minimum
            if (cap_len < 42) continue;

            const auto* eth = reinterpret_cast<const EthernetHeader*>(packet_buffer.data());
            uint16_t eth_type = (std::endian::native == std::endian::little) ? 
                                 __builtin_bswap16(eth->ether_type) : eth->ether_type;
            if (eth_type != 0x0800) continue; // Skip non-IPv4

            const auto* ip = reinterpret_cast<const Ipv4Header*>(packet_buffer.data() + sizeof(EthernetHeader));
            if (ip->protocol != 17) continue; // Skip non-UDP

            size_t ip_hdr_len = (ip->ihl_version & 0x0F) * 4;
            size_t udp_offset = sizeof(EthernetHeader) + ip_hdr_len;
            if (udp_offset + sizeof(UdpHeader) > cap_len) continue;

            const auto* udp = reinterpret_cast<const UdpHeader*>(packet_buffer.data() + udp_offset);
            uint16_t udp_len = (std::endian::native == std::endian::little) ? 
                                __builtin_bswap16(udp->length) : udp->length;

            if (udp_len < sizeof(UdpHeader)) continue;
            size_t payload_len = udp_len - sizeof(UdpHeader);
            size_t payload_offset = udp_offset + sizeof(UdpHeader);

            if (payload_offset + payload_len > cap_len) continue;

            // Compute packet timestamp in nanoseconds
            uint64_t ts_sec = byte_swapped_ ? __builtin_bswap32(pkt_hdr.ts_sec) : pkt_hdr.ts_sec;
            uint64_t ts_sub = byte_swapped_ ? __builtin_bswap32(pkt_hdr.ts_usec) : pkt_hdr.ts_usec;
            uint64_t timestamp_ns = is_nanosecond_pcap_ ? (ts_sec * 1'000'000'000ULL + ts_sub) 
                                                        : (ts_sec * 1'000'000'000ULL + ts_sub * 1'000ULL);

            // Dispatch payload directly into MoldUDP64 parser
            parser.process_datagram(packet_buffer.data() + payload_offset, payload_len, timestamp_ns);
            packets_replayed++;
        }

        return packets_replayed;
    }

    /// Utility: Generate a synthetic PCAP capture file containing MoldUDP64/ITCH packets for deterministic benchmarking
    static bool generate_synthetic_itch_pcap(const std::string& filepath, size_t packet_count = 1000) {
        std::ofstream out(filepath, std::ios::binary);
        if (!out.is_open()) return false;

        // Write PCAP Global Header
        PcapGlobalHeader global_hdr{};
        out.write(reinterpret_cast<const char*>(&global_hdr), sizeof(PcapGlobalHeader));

        // Create standard network framing
        EthernetHeader eth{};
        eth.ether_type = (std::endian::native == std::endian::little) ? __builtin_bswap16(0x0800) : 0x0800;

        Ipv4Header ip{};
        ip.ihl_version = 0x45; // IPv4, IHL = 5 (20 bytes)
        ip.ttl = 64;
        ip.protocol = 17;      // UDP
        ip.src_ip = 0x0100007F; // 127.0.0.1
        ip.dest_ip = 0x0100A8C0; // 192.168.0.1

        UdpHeader udp{};
        udp.src_port = (std::endian::native == std::endian::little) ? __builtin_bswap16(12345) : 12345;
        udp.dest_port = (std::endian::native == std::endian::little) ? __builtin_bswap16(23456) : 23456;

        uint64_t seq = 1;

        for (size_t i = 0; i < packet_count; ++i) {
            uint8_t payload[256];
            auto* mold = reinterpret_cast<MoldUdp64Header*>(payload);
            std::memcpy(mold->session, "NASDAQ0001", 10);
            mold->sequence_number = (std::endian::native == std::endian::little) ? __builtin_bswap64(seq) : seq;
            mold->message_count = (std::endian::native == std::endian::little) ? __builtin_bswap16(2) : 2;

            size_t offset = sizeof(MoldUdp64Header);

            // Message 1: Add Order
            uint16_t msg1_len = 36;
            *reinterpret_cast<uint16_t*>(payload + offset) = (std::endian::native == std::endian::little) ? 
                                                             __builtin_bswap16(msg1_len) : msg1_len;
            offset += 2;
            itch::AddOrderMsg m1{};
            m1.length = (std::endian::native == std::endian::little) ? __builtin_bswap16(msg1_len) : msg1_len;
            m1.message_type = 'A';
            m1.order_ref_num = (std::endian::native == std::endian::little) ? __builtin_bswap64(i * 2 + 1) : (i * 2 + 1);
            m1.buy_sell_indicator = 'B';
            m1.shares = (std::endian::native == std::endian::little) ? __builtin_bswap32(100) : 100;
            m1.price = (std::endian::native == std::endian::little) ? __builtin_bswap32(1500000) : 1500000;
            std::memcpy(m1.stock, "AAPL    ", 8);
            std::memcpy(payload + offset, reinterpret_cast<const uint8_t*>(&m1) + sizeof(m1.length), msg1_len);
            offset += msg1_len;

            // Message 2: Add Order
            uint16_t msg2_len = 36;
            *reinterpret_cast<uint16_t*>(payload + offset) = (std::endian::native == std::endian::little) ? 
                                                             __builtin_bswap16(msg2_len) : msg2_len;
            offset += 2;
            itch::AddOrderMsg m2{};
            m2.length = (std::endian::native == std::endian::little) ? __builtin_bswap16(msg2_len) : msg2_len;
            m2.message_type = 'A';
            m2.order_ref_num = (std::endian::native == std::endian::little) ? __builtin_bswap64(i * 2 + 2) : (i * 2 + 2);
            m2.buy_sell_indicator = 'S';
            m2.shares = (std::endian::native == std::endian::little) ? __builtin_bswap32(100) : 100;
            m2.price = (std::endian::native == std::endian::little) ? __builtin_bswap32(1501000) : 1501000;
            std::memcpy(m2.stock, "AAPL    ", 8);
            std::memcpy(payload + offset, reinterpret_cast<const uint8_t*>(&m2) + sizeof(m2.length), msg2_len);
            offset += msg2_len;

            seq += 2;

            size_t total_udp_len = sizeof(UdpHeader) + offset;
            udp.length = (std::endian::native == std::endian::little) ? __builtin_bswap16(total_udp_len) : total_udp_len;

            size_t total_ip_len = sizeof(Ipv4Header) + total_udp_len;
            ip.total_length = (std::endian::native == std::endian::little) ? __builtin_bswap16(total_ip_len) : total_ip_len;

            size_t total_frame_len = sizeof(EthernetHeader) + total_ip_len;

            PcapPacketHeader pkt_hdr{};
            pkt_hdr.ts_sec = 1700000000 + static_cast<uint32_t>(i / 1000);
            pkt_hdr.ts_usec = static_cast<uint32_t>((i % 1000) * 1000);
            pkt_hdr.incl_len = total_frame_len;
            pkt_hdr.orig_len = total_frame_len;

            out.write(reinterpret_cast<const char*>(&pkt_hdr), sizeof(PcapPacketHeader));
            out.write(reinterpret_cast<const char*>(&eth), sizeof(EthernetHeader));
            out.write(reinterpret_cast<const char*>(&ip), sizeof(Ipv4Header));
            out.write(reinterpret_cast<const char*>(&udp), sizeof(UdpHeader));
            out.write(reinterpret_cast<const char*>(payload), offset);
        }

        return true;
    }

private:
    std::ifstream file_;
    bool is_nanosecond_pcap_{false};
    bool byte_swapped_{false};
};

} // namespace lob

