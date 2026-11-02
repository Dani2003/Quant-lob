#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "order_book.hpp"

#if defined(_MSC_VER)
#define PACKED_STRUCT struct
#pragma pack(push, 1)
#else
#define PACKED_STRUCT struct __attribute__((packed))
#endif

namespace lob {

namespace itch {

// Endianness conversion helpers for binary market feed (Big-Endian network to Little-Endian host)
inline uint16_t read_be16(const void* ptr) noexcept {
    uint16_t val;
    std::memcpy(&val, ptr, sizeof(val));
#if defined(_MSC_VER)
    return _byteswap_ushort(val);
#else
    return __builtin_bswap16(val);
#endif
}

inline uint32_t read_be32(const void* ptr) noexcept {
    uint32_t val;
    std::memcpy(&val, ptr, sizeof(val));
#if defined(_MSC_VER)
    return _byteswap_ulong(val);
#else
    return __builtin_bswap32(val);
#endif
}

inline uint64_t read_be64(const void* ptr) noexcept {
    uint64_t val;
    std::memcpy(&val, ptr, sizeof(val));
#if defined(_MSC_VER)
    return _byteswap_uint64(val);
#else
    return __builtin_bswap64(val);
#endif
}

/**
 * @brief Binary NASDAQ ITCH 5.0 Packet Definitions (Exact wire format).
 */
#pragma pack(push, 1)

struct ItchHeader {
    uint16_t length;
    char message_type;
};

struct AddOrderMsg {
    uint16_t length;          // 2 bytes
    char message_type;        // 'A'
    uint16_t stock_locate;    // 2 bytes
    uint16_t tracking_number; // 2 bytes
    uint8_t timestamp[6];     // 6 bytes (nanoseconds since midnight)
    uint64_t order_ref_num;   // 8 bytes
    char buy_sell_indicator;  // 'B' or 'S'
    uint32_t shares;          // 4 bytes
    char stock[8];            // 8 bytes
    uint32_t price;           // 4 bytes (fixed point 4 decimals)
};

struct OrderExecutedMsg {
    uint16_t length;
    char message_type;        // 'E'
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t timestamp[6];
    uint64_t order_ref_num;
    uint32_t executed_shares;
    uint64_t match_number;
};

struct OrderCancelMsg {
    uint16_t length;
    char message_type;        // 'X'
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t timestamp[6];
    uint64_t order_ref_num;
    uint32_t canceled_shares;
};

struct OrderDeleteMsg {
    uint16_t length;
    char message_type;        // 'D'
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t timestamp[6];
    uint64_t order_ref_num;
};

struct OrderReplaceMsg {
    uint16_t length;
    char message_type;        // 'U'
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t timestamp[6];
    uint64_t original_order_ref_num;
    uint64_t new_order_ref_num;
    uint32_t shares;
    uint32_t price;
};

#pragma pack(pop)

} // namespace itch

/**
 * @brief Zero-Allocation Binary Market Feed Handler.
 * Decodes raw binary ITCH 5.0 wire packets directly into the OrderBook engine.
 */
class FeedHandler {
public:
    explicit FeedHandler(OrderBook& book) noexcept : book_(book) {}

    /**
     * @brief Parses and dispatches a single raw binary ITCH message in-place.
     * @param buffer Pointer to raw bytes.
     * @param bytes_available Buffer size.
     * @return Number of bytes consumed, or 0 if incomplete message.
     */
    size_t process_packet(const uint8_t* buffer, size_t bytes_available) noexcept;

    uint64_t messages_processed() const noexcept { return messages_processed_; }
    uint64_t bytes_processed() const noexcept { return bytes_processed_; }

private:
    OrderBook& book_;
    uint64_t messages_processed_{0};
    uint64_t bytes_processed_{0};
};

} // namespace lob

