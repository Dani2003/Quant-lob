#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>
#include <bit>

namespace lob {

#pragma pack(push, 1)

/// NASDAQ OUCH 5.0 Enter Order Message (Outbound from Trader -> Exchange)
struct OuchEnterOrder {
    uint8_t  type{'O'};             // Message Type: 'O'
    uint64_t order_token{0};        // Client Order Token (Unique per day)
    uint8_t  buy_sell{'B'};         // 'B' = Buy, 'S' = Sell
    uint32_t shares{0};             // Total order quantity (Big-Endian)
    char     stock[8]{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '}; // Ticker Symbol
    uint32_t price{0};              // Limit price in 4 decimal fixed point (Big-Endian)
    uint32_t time_in_force{0};      // 0 = Day, 99999 = IOC (Immediate or Cancel)
    char     firm[4]{'L', 'O', 'B', '1'}; // MPID / Firm identifier
    uint8_t  display{'Y'};          // 'Y' = Displayed, 'N' = Non-display
    uint8_t  capacity{'P'};         // 'P' = Principal, 'A' = Agency
    uint8_t  intermarket_sweep{'N'}; // 'Y' = ISO, 'N' = Non-ISO
    uint32_t min_qty{0};            // Minimum executable quantity
};

/// NASDAQ OUCH 5.0 Cancel Order Message (Outbound from Trader -> Exchange)
struct OuchCancelOrder {
    uint8_t  type{'X'};             // Message Type: 'X'
    uint64_t order_token{0};        // Order token to cancel
    uint32_t shares{0};             // Shares to cancel (0 = entire remaining balance)
};

/// NASDAQ OUCH 5.0 Order Accepted (Inbound from Exchange -> Trader)
struct OuchOrderAccepted {
    uint8_t  type{'A'};             // Message Type: 'A'
    uint64_t timestamp{0};          // Nanoseconds since midnight
    uint64_t order_token{0};        // Original client order token
    uint8_t  buy_sell{'B'};         // 'B' or 'S'
    uint32_t shares{0};             // Accepted quantity
    char     stock[8]{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    uint32_t price{0};              // Accepted price
    uint32_t time_in_force{0};
    char     firm[4];
    uint8_t  display{'Y'};
    uint64_t order_reference_number{0}; // Exchange-assigned global order ID
    uint8_t  order_state{'L'};      // 'L' = Live
};

/// NASDAQ OUCH 5.0 Order Executed (Inbound from Exchange -> Trader)
struct OuchOrderExecuted {
    uint8_t  type{'E'};             // Message Type: 'E'
    uint64_t timestamp{0};          // Execution timestamp
    uint64_t order_token{0};        // Order token executed
    uint32_t executed_shares{0};    // Executed quantity
    uint32_t execution_price{0};     // Executed price
    uint8_t  liquidity_flag{'A'};   // 'A' = Added Liquidity (Passive), 'R' = Removed (Aggressive)
    uint64_t match_number{0};       // Exchange execution trade ID
};

/// NASDAQ OUCH 5.0 Order Canceled (Inbound from Exchange -> Trader)
struct OuchOrderCanceled {
    uint8_t  type{'C'};             // Message Type: 'C'
    uint64_t timestamp{0};
    uint64_t order_token{0};
    uint32_t decrement_shares{0};   // Cancelled quantity
    uint8_t  reason{'U'};           // 'U' = User requested, 'I' = IOC expired, 'T' = Timeout
};

/// NASDAQ OUCH 5.0 Order Rejected (Inbound from Exchange -> Trader)
struct OuchOrderRejected {
    uint8_t  type{'J'};             // Message Type: 'J'
    uint64_t timestamp{0};
    uint64_t order_token{0};
    uint8_t  reason{'R'};           // Rejection reason code
};

#pragma pack(pop)

/// Endian conversion utilities (OUCH wire format is Big-Endian network order)
inline uint32_t to_big_endian32(uint32_t val) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return __builtin_bswap32(val);
    }
    return val;
}

inline uint64_t to_big_endian64(uint64_t val) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return __builtin_bswap64(val);
    }
    return val;
}

inline uint32_t from_big_endian32(uint32_t val) noexcept {
    return to_big_endian32(val);
}

inline uint64_t from_big_endian64(uint64_t val) noexcept {
    return to_big_endian64(val);
}

/// Zero-copy Serializer for Outbound OUCH Packets
class OuchSerializer {
public:
    static inline size_t serialize_enter_order(
        uint8_t* destination_buffer,
        uint64_t order_token,
        uint8_t buy_sell,
        uint32_t shares,
        std::string_view symbol,
        uint32_t price,
        uint32_t time_in_force = 0) noexcept
    {
        auto* msg = reinterpret_cast<OuchEnterOrder*>(destination_buffer);
        msg->type = 'O';
        msg->order_token = to_big_endian64(order_token);
        msg->buy_sell = buy_sell;
        msg->shares = to_big_endian32(shares);
        
        std::memset(msg->stock, ' ', 8);
        std::memcpy(msg->stock, symbol.data(), std::min(symbol.size(), size_t{8}));
        
        msg->price = to_big_endian32(price);
        msg->time_in_force = to_big_endian32(time_in_force);
        std::memcpy(msg->firm, "LOB1", 4);
        msg->display = 'Y';
        msg->capacity = 'P';
        msg->intermarket_sweep = 'N';
        msg->min_qty = 0;
        
        return sizeof(OuchEnterOrder);
    }

    static inline size_t serialize_cancel_order(
        uint8_t* destination_buffer,
        uint64_t order_token,
        uint32_t shares = 0) noexcept
    {
        auto* msg = reinterpret_cast<OuchCancelOrder*>(destination_buffer);
        msg->type = 'X';
        msg->order_token = to_big_endian64(order_token);
        msg->shares = to_big_endian32(shares);
        return sizeof(OuchCancelOrder);
    }
};

} // namespace lob
