#include "feed_handler.hpp"

namespace lob {

size_t FeedHandler::process_packet(const uint8_t* buffer, size_t bytes_available) noexcept {
    if (bytes_available < sizeof(itch::ItchHeader)) [[unlikely]] {
        return 0;
    }

    const auto* header = reinterpret_cast<const itch::ItchHeader*>(buffer);
    const uint16_t msg_len = itch::read_be16(&header->length);

    if (bytes_available < sizeof(uint16_t) + msg_len) [[unlikely]] {
        return 0; // Partial message
    }

    switch (header->message_type) {
        case 'A': {
            if (sizeof(uint16_t) + msg_len >= sizeof(itch::AddOrderMsg)) {
                const auto* msg = reinterpret_cast<const itch::AddOrderMsg*>(buffer);
                const uint64_t oid = itch::read_be64(&msg->order_ref_num);
                const uint32_t shares = itch::read_be32(&msg->shares);
                const uint32_t price = itch::read_be32(&msg->price) / 100; // normalize to cent ticks
                const Side side = (msg->buy_sell_indicator == 'B') ? Side::Buy : Side::Sell;

                (void)book_.add_limit_order(oid, 1, side, price, shares);
            }
            break;
        }

        case 'D': {
            if (sizeof(uint16_t) + msg_len >= sizeof(itch::OrderDeleteMsg)) {
                const auto* msg = reinterpret_cast<const itch::OrderDeleteMsg*>(buffer);
                const uint64_t oid = itch::read_be64(&msg->order_ref_num);
                (void)book_.cancel_order(oid);
            }
            break;
        }

        case 'X': {
            if (sizeof(uint16_t) + msg_len >= sizeof(itch::OrderCancelMsg)) {
                const auto* msg = reinterpret_cast<const itch::OrderCancelMsg*>(buffer);
                const uint64_t oid = itch::read_be64(&msg->order_ref_num);
                // In ITCH, partial cancel reduces shares, full cancel deletes
                (void)book_.cancel_order(oid);
            }
            break;
        }

        case 'U': {
            if (sizeof(uint16_t) + msg_len >= sizeof(itch::OrderReplaceMsg)) {
                const auto* msg = reinterpret_cast<const itch::OrderReplaceMsg*>(buffer);
                const uint64_t orig_oid = itch::read_be64(&msg->original_order_ref_num);
                const uint64_t new_oid = itch::read_be64(&msg->new_order_ref_num);
                const uint32_t shares = itch::read_be32(&msg->shares);
                const uint32_t price = itch::read_be32(&msg->price) / 100;

                // Atomic replace: Cancel original, insert new with refreshed priority
                (void)book_.cancel_order(orig_oid);
                (void)book_.add_limit_order(new_oid, 1, Side::Buy, price, shares);
            }

            break;
        }

        default:
            // Non-book control message (Trade, Halt, System Event, etc.)
            break;
    }

    const size_t total_consumed = sizeof(uint16_t) + msg_len;
    ++messages_processed_;
    bytes_processed_ += total_consumed;
    return total_consumed;
}

} // namespace lob

