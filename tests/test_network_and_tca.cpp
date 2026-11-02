#include <iostream>
#include <cassert>
#include <vector>
#include "../include/mold_udp64.hpp"
#include "../include/tca_engine.hpp"
#include "../include/order_book.hpp"
#include "../include/ouch_messages.hpp"

using namespace lob;

void test_mold_udp64_parser() {
    std::cout << "[TEST] Running MoldUDP64 Multicast Protocol & Gap Detection...\n";

    OrderBook book;
    FeedHandler feed(book);
    MoldUdp64Parser<256> parser(feed);

    // Construct a MoldUDP64 datagram containing two 36-byte ITCH AddOrder messages
    uint8_t buffer[512];
    auto* header = reinterpret_cast<MoldUdp64Header*>(buffer);
    std::memcpy(header->session, "NASDAQ0001", 10);
    header->sequence_number = to_big_endian64(1);
    header->message_count = to_big_endian16(2);

    size_t offset = sizeof(MoldUdp64Header);

    // Message 1: Add Buy Order 100 shares @ $100.00
    uint16_t msg1_len = 36;
    *reinterpret_cast<uint16_t*>(buffer + offset) = to_big_endian16(msg1_len);
    offset += 2;
    auto* m1 = reinterpret_cast<itch::AddOrderMsg*>(buffer + offset);
    m1->length = to_big_endian16(msg1_len);
    m1->message_type = 'A';
    m1->order_ref_num = to_big_endian64(1001);
    m1->buy_sell_indicator = 'B';
    m1->shares = to_big_endian32(100);
    m1->price = to_big_endian32(1000000);
    std::memcpy(m1->stock, "AAPL    ", 8);
    offset += msg1_len;

    // Message 2: Add Sell Order 150 shares @ $100.10
    uint16_t msg2_len = 36;
    *reinterpret_cast<uint16_t*>(buffer + offset) = to_big_endian16(msg2_len);
    offset += 2;
    auto* m2 = reinterpret_cast<itch::AddOrderMsg*>(buffer + offset);
    m2->length = to_big_endian16(msg2_len);
    m2->message_type = 'A';
    m2->order_ref_num = to_big_endian64(1002);
    m2->buy_sell_indicator = 'S';
    m2->shares = to_big_endian32(150);
    m2->price = to_big_endian32(1001000);
    std::memcpy(m2->stock, "AAPL    ", 8);
    offset += msg2_len;

    // 1. Process initial sequential datagram
    size_t msgs = parser.process_datagram(buffer, offset, 1000);
    assert(msgs == 2);
    assert(parser.total_packets() == 1);
    assert(parser.total_messages() == 2);
    assert(parser.expected_sequence_number() == 3);
    assert(parser.total_gaps() == 0);

    // Check book state
    assert(book.get_best_bid().has_value() && book.get_best_bid()->price == 10000);
    assert(book.get_best_ask().has_value() && book.get_best_ask()->price == 10010);

    // 2. Test Gap Detection: Inject packet starting at sequence 6 (gap: expected 3, got 6 -> missing 3, 4, 5)
    header->sequence_number = to_big_endian64(6);
    header->message_count = to_big_endian16(1);
    msgs = parser.process_datagram(buffer, sizeof(MoldUdp64Header) + 2 + msg1_len, 2000);
    assert(msgs == 1);
    assert(parser.total_gaps() == 1);
    assert(parser.expected_sequence_number() == 7);

    const auto* gap = parser.get_gap(0);
    assert(gap != nullptr);
    assert(gap->expected_seq == 3);
    assert(gap->received_seq == 6);
    assert(gap->missing_count == 3);
    (void)gap;

    // 3. Test Gap Request Serialization
    uint8_t req_buf[64];
    size_t req_bytes = MoldUdp64Parser<256>::serialize_gap_request(req_buf, "NASDAQ0001", 3, 3);
    assert(req_bytes == sizeof(MoldUdp64Request));
    auto* req = reinterpret_cast<const MoldUdp64Request*>(req_buf);
    assert(from_big_endian64(req->sequence_number) == 3);
    assert(from_big_endian16(req->message_count) == 3);
    (void)req_bytes;
    (void)req;

    // 4. Test Duplicate Packet Rejection
    header->sequence_number = to_big_endian64(2); // Already passed sequence 7
    header->message_count = to_big_endian16(1);
    msgs = parser.process_datagram(buffer, sizeof(MoldUdp64Header) + 2 + msg1_len, 3000);
    assert(msgs == 0);
    assert(parser.total_duplicates() == 1);
    (void)msgs;


    std::cout << "  -> MoldUDP64 Multicast & Gap Detection Invariants PASSED.\n";
}

void test_tca_engine() {
    std::cout << "[TEST] Running Real-Time TCA & Adverse Selection Invariants...\n";

    TcaEngine<1024, 2048> tca;

    // Time t = 0ms: Market mid-price is $100.00 (10,000 cents)
    tca.record_market_tick(0, 10000);

    // Trade 1: Passive Sell filled at Ask: $100.05 (10,005 cents) for 100 shares
    // Effective spread = -1 * (10005 - 10000) = +5 cents captured
    auto* t1 = tca.record_fill(1, 0, 1, 10005, 100, 10000);
    assert(t1 != nullptr);
    assert(t1->effective_half_spread == 5.0);

    // Time t = 0.5ms: Market mid remains $100.00
    tca.record_market_tick(500'000ULL, 10000);

    // Time t = 1.0ms: Market moves UP to $100.08 (10,008 cents)
    // As a seller, price moving up means adverse selection / toxic flow!
    tca.record_market_tick(1'000'000ULL, 10008);

    // Evaluate markouts
    tca.evaluate_markouts();

    assert(t1->mid_1ms == 10008);
    // Realized spread = -1 * (10005 - 10008) = -3 cents (loss)
    assert(t1->realized_spread_1ms == -3.0);
    // Adverse selection = -1 * (10008 - 10000) = +8 cents (adverse drift)
    assert(t1->adverse_selection_1ms == 8.0);
    assert(t1->is_toxic == true);
    (void)t1;

    assert(tca.total_trades() == 1);

    assert(tca.toxic_flow_ratio() == 1.0);

    std::cout << "  -> TCA & Adverse Selection Invariants PASSED.\n";
}

int main() {
    std::cout << "==========================================================\n";
    std::cout << "   RUNNING PHASE 7 & 8: MOLDUDP64 & TCA TEST SUITE        \n";
    std::cout << "==========================================================\n";

    test_mold_udp64_parser();
    test_tca_engine();

    std::cout << "==========================================================\n";
    std::cout << "   ALL MOLDUDP64 & TCA INVARIANTS PASSED SUCCESSFULLY!    \n";
    std::cout << "==========================================================\n";
    return 0;
}

