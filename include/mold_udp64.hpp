#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>
#include <array>
#include <bit>
#include <algorithm>
#include "feed_handler.hpp"

namespace lob {

#pragma pack(push, 1)

/// NASDAQ MoldUDP64 Downstream Packet Header
/// MoldUDP64 encapsulates multiple length-prefixed ITCH messages inside a single UDP datagram.
struct MoldUdp64Header {
    char     session[10];       // Session identifier (ASCII)
    uint64_t sequence_number;   // Sequence number of the first message in this packet (Big-Endian)
    uint16_t message_count;     // Number of messages contained in this packet (0xFFFF = End of Session)
};

/// NASDAQ MoldUDP64 Request Packet (Sent by client over TCP/UDP to request missing message range)
struct MoldUdp64Request {
    char     session[10];       // Session identifier to replay
    uint64_t sequence_number;   // Starting sequence number requested
    uint16_t message_count;     // Number of messages requested
};

#pragma pack(pop)

/// Recorded gap in the packet stream
struct PacketGap {
    uint64_t expected_seq{0};
    uint64_t received_seq{0};
    uint64_t missing_count{0};
    uint64_t timestamp_ns{0};
};

/// High-Performance, Zero-Copy MoldUDP64 Protocol Parser & Gap Detector
/// Unpacks multi-message UDP packets and routes individual ITCH messages directly into the FeedHandler.
template <size_t MaxTrackedGaps = 1024>
class MoldUdp64Parser {
public:
    explicit MoldUdp64Parser(FeedHandler& feed_handler)
        : feed_handler_(feed_handler),
          expected_sequence_number_(1),
          total_packets_processed_(0),
          total_messages_unpacked_(0),
          total_gaps_detected_(0),
          total_duplicate_packets_(0),
          session_ended_(false) 
    {
        std::memset(current_session_, 0, sizeof(current_session_));
    }

    /// Process a raw UDP datagram payload
    /// @param buffer Pointer to raw UDP packet payload
    /// @param length Byte length of the datagram
    /// @param timestamp_ns Hardware receipt timestamp (from NIC or RDTSC)
    /// @return Number of ITCH messages parsed and dispatched to the book
    size_t process_datagram(const uint8_t* buffer, size_t length, uint64_t timestamp_ns = 0) {
        if (__builtin_expect(length < sizeof(MoldUdp64Header), 0)) {
            return 0; // Truncated or malformed packet
        }

        const auto* header = reinterpret_cast<const MoldUdp64Header*>(buffer);
        uint64_t seq = (std::endian::native == std::endian::little) ? 
                        __builtin_bswap64(header->sequence_number) : header->sequence_number;
        uint16_t count = (std::endian::native == std::endian::little) ? 
                         __builtin_bswap16(header->message_count) : header->message_count;

        // Session initialization
        if (__builtin_expect(total_packets_processed_ == 0, 0)) {
            std::memcpy(current_session_, header->session, 10);
            expected_sequence_number_ = seq;
        }

        // Check for End of Session
        if (__builtin_expect(count == 0xFFFF, 0)) {
            session_ended_ = true;
            return 0;
        }

        // Sequence Gap Detection
        if (__builtin_expect(seq > expected_sequence_number_, 0)) {
            record_gap(expected_sequence_number_, seq, timestamp_ns);
            // In HFT, we process out-of-order forward packets while requesting missing block
            expected_sequence_number_ = seq + count;
        } else if (__builtin_expect(seq < expected_sequence_number_, 0)) {
            // Duplicate or retransmitted packet
            total_duplicate_packets_++;
            return 0;
        } else {
            // Normal sequential packet
            expected_sequence_number_ += count;
        }

        total_packets_processed_++;

        // Unpack variable-length length-prefixed ITCH messages
        size_t offset = sizeof(MoldUdp64Header);
        size_t parsed_in_packet = 0;

        for (uint16_t i = 0; i < count && offset + 2 <= length; ++i) {
            // 2-byte big-endian message length
            uint16_t raw_msg_len = *reinterpret_cast<const uint16_t*>(buffer + offset);
            uint16_t msg_len = (std::endian::native == std::endian::little) ? 
                               __builtin_bswap16(raw_msg_len) : raw_msg_len;
            offset += 2;

            if (__builtin_expect(offset + msg_len > length, 0)) {
                break; // Corrupted payload length
            }

            // Feed directly into ITCH FeedHandler zero-copy
            feed_handler_.process_packet(buffer + offset - 2, msg_len + 2);

            offset += msg_len;
            parsed_in_packet++;
        }

        total_messages_unpacked_ += parsed_in_packet;
        return parsed_in_packet;
    }

    /// Serializes a MoldUDP64 Gap Request to recover lost packets
    static size_t serialize_gap_request(
        uint8_t* out_buffer,
        std::string_view session,
        uint64_t start_seq,
        uint16_t count) noexcept 
    {
        auto* req = reinterpret_cast<MoldUdp64Request*>(out_buffer);
        std::memset(req->session, ' ', 10);
        std::memcpy(req->session, session.data(), std::min(session.size(), size_t{10}));
        
        req->sequence_number = (std::endian::native == std::endian::little) ? 
                                __builtin_bswap64(start_seq) : start_seq;
        req->message_count = (std::endian::native == std::endian::little) ? 
                              __builtin_bswap16(count) : count;
        return sizeof(MoldUdp64Request);
    }

    [[nodiscard]] uint64_t expected_sequence_number() const noexcept { return expected_sequence_number_; }
    [[nodiscard]] uint64_t total_packets() const noexcept { return total_packets_processed_; }
    [[nodiscard]] uint64_t total_messages() const noexcept { return total_messages_unpacked_; }
    [[nodiscard]] uint64_t total_gaps() const noexcept { return total_gaps_detected_; }
    [[nodiscard]] uint64_t total_duplicates() const noexcept { return total_duplicate_packets_; }
    [[nodiscard]] bool is_session_ended() const noexcept { return session_ended_; }
    [[nodiscard]] std::string_view session_id() const noexcept { return std::string_view(current_session_, 10); }

    [[nodiscard]] const PacketGap* get_gap(size_t index) const noexcept {
        return (index < gap_count_) ? &gap_history_[index] : nullptr;
    }

private:
    void record_gap(uint64_t expected, uint64_t received, uint64_t timestamp_ns) noexcept {
        total_gaps_detected_++;
        if (gap_count_ < MaxTrackedGaps) {
            gap_history_[gap_count_] = PacketGap{
                .expected_seq = expected,
                .received_seq = received,
                .missing_count = received - expected,
                .timestamp_ns = timestamp_ns
            };
            gap_count_++;
        }
    }

    FeedHandler& feed_handler_;
    char current_session_[10];
    uint64_t expected_sequence_number_{1};
    uint64_t total_packets_processed_{0};
    uint64_t total_messages_unpacked_{0};
    uint64_t total_gaps_detected_{0};
    uint64_t total_duplicate_packets_{0};
    bool session_ended_{false};

    size_t gap_count_{0};
    std::array<PacketGap, MaxTrackedGaps> gap_history_{};
};

} // namespace lob
