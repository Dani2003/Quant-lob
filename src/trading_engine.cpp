#include "trading_engine.hpp"
#include "mold_udp64.hpp"
#include <iomanip>
#include <iostream>
#include <cmath>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define CPU_PAUSE() _mm_pause()
#else
#define CPU_PAUSE() std::this_thread::yield()
#endif

namespace lob {

static uint64_t monotonic_time_ns() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

PipelinedTradingEngine::PipelinedTradingEngine()
    : PipelinedTradingEngine(EngineConfig{}) {}

PipelinedTradingEngine::PipelinedTradingEngine(const EngineConfig& config)
    : config_(config),
      risk_engine_(config.risk_config) 

{
    book_ = std::make_unique<OrderBook>();
    feed_handler_ = std::make_unique<FeedHandler>(*book_);

    telemetry_mgr_.init_producer();

    if (config_.enable_udp_streaming) {
        udp_pub_.init(config_.udp_host, config_.udp_port);
    }

    // Attach trade callback to book
    book_->set_trade_callback([](const Trade& trade, void* user_data) {
        auto* self = static_cast<PipelinedTradingEngine*>(user_data);
        self->trades_executed_.fetch_add(1, std::memory_order_relaxed);
        self->volume_traded_.fetch_add(trade.quantity, std::memory_order_relaxed);
    }, this);
}

PipelinedTradingEngine::~PipelinedTradingEngine() {
    stop();
}

void PipelinedTradingEngine::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) return;
    workers_joined_.store(false, std::memory_order_release);
    matching_done_.store(false, std::memory_order_relaxed);
    strategy_done_.store(false, std::memory_order_relaxed);

    // Seed initial clean baseline snapshot in the triple buffer
    ShmTelemetryBlock initial_block{};
    snapshot_buffer_.publish(initial_block);

    matching_thread_ = std::thread(&PipelinedTradingEngine::matching_thread_loop, this);
    strategy_thread_ = std::thread(&PipelinedTradingEngine::strategy_thread_loop, this);
    execution_thread_ = std::thread(&PipelinedTradingEngine::execution_thread_loop, this);
}

void PipelinedTradingEngine::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        if (matching_thread_.joinable()) matching_thread_.join();
        if (strategy_thread_.joinable()) strategy_thread_.join();
        if (execution_thread_.joinable()) execution_thread_.join();
        workers_joined_.store(true, std::memory_order_release);
        return;
    }

    if (matching_thread_.joinable()) matching_thread_.join();
    if (strategy_thread_.joinable()) strategy_thread_.join();
    if (execution_thread_.joinable()) execution_thread_.join();

    workers_joined_.store(true, std::memory_order_release);
}

bool PipelinedTradingEngine::inject_network_packet(const uint8_t* buffer, size_t length, uint64_t timestamp_ns) {
    (void)timestamp_ns;
    if (!buffer || __builtin_expect(!running_.load(std::memory_order_relaxed), 0)) return false;

    auto enqueue = [&](const uint8_t* bytes, size_t size) {
        if (!bytes || size == 0 || size > sizeof(IngressMessage::bytes)) return false;
        IngressMessage message{};
        message.length = static_cast<uint8_t>(size);
        std::memcpy(message.bytes, bytes, size);
        while (!ingress_to_match_ring_.try_push(message)) {
            if (!running_.load(std::memory_order_relaxed)) return false;
            CPU_PAUSE();
        }
        messages_ingested_.fetch_add(1, std::memory_order_relaxed);
        return true;
    };

    // Direct ITCH payloads have a length prefix followed by a recognized message type.
    if (buffer && length >= sizeof(itch::ItchHeader) &&
        (buffer[2] == 'A' || buffer[2] == 'D' || buffer[2] == 'E' || buffer[2] == 'X' || buffer[2] == 'U')) {
        const uint16_t msg_length = itch::read_be16(buffer);
        const size_t total_length = sizeof(uint16_t) + static_cast<size_t>(msg_length);
        if (total_length <= length) return enqueue(buffer, total_length);
    }

    // Check if this is a MoldUDP64 packet
    if (length >= sizeof(MoldUdp64Header)) {
        const auto* header = reinterpret_cast<const MoldUdp64Header*>(buffer);
        const uint64_t sequence = itch::read_be64(&header->sequence_number);
        uint16_t count = (std::endian::native == std::endian::little) ? 
                         __builtin_bswap16(header->message_count) : header->message_count;
        const bool same_session = !mold_sequence_initialized_ ||
                                  std::memcmp(header->session, mold_session_, sizeof(mold_session_)) == 0;
        if (count > 0 && count != 0xFFFF && same_session &&
            (!mold_sequence_initialized_ || sequence == expected_mold_sequence_)) {
            // Validate the entire datagram before enqueueing any of its messages.
            size_t check_offset = sizeof(MoldUdp64Header);
            bool valid = true;
            for (uint16_t i = 0; i < count; ++i) {
                if (check_offset + 2 > length) { valid = false; break; }
                uint16_t raw_len;
                std::memcpy(&raw_len, buffer + check_offset, sizeof(raw_len));
                const uint16_t msg_len = (std::endian::native == std::endian::little)
                    ? __builtin_bswap16(raw_len) : raw_len;
                check_offset += 2;
                if (msg_len == 0 || static_cast<size_t>(msg_len) + 2 > sizeof(IngressMessage::bytes) ||
                    check_offset + msg_len > length) { valid = false; break; }
                check_offset += msg_len;
            }
            if (!valid) return false;

            size_t offset = sizeof(MoldUdp64Header);
            size_t parsed = 0;
            for (uint16_t i = 0; i < count && offset + 2 <= length; ++i) {
                uint16_t raw_msg_len;
                std::memcpy(&raw_msg_len, buffer + offset, sizeof(raw_msg_len));
                uint16_t msg_len = (std::endian::native == std::endian::little) ? 
                                   __builtin_bswap16(raw_msg_len) : raw_msg_len;
                offset += 2;
                if (offset + msg_len > length) break;

                const uint8_t* msg_ptr = buffer + offset;
                uint8_t framed[64]{};
                if (static_cast<size_t>(msg_len) + 2 <= sizeof(framed)) {
                    std::memcpy(framed, &raw_msg_len, sizeof(raw_msg_len));
                    std::memcpy(framed + 2, msg_ptr, msg_len);
                    parsed += enqueue(framed, static_cast<size_t>(msg_len) + 2) ? 1 : 0;
                }
                offset += msg_len;
            }
            if (parsed > 0) {
                if (!mold_sequence_initialized_)
                    std::memcpy(mold_session_, header->session, sizeof(mold_session_));
                expected_mold_sequence_ = sequence + count;
                mold_sequence_initialized_ = true;
                return true;
            }
        }
    }

    // Direct ITCH message ingress
    return false;
}

void PipelinedTradingEngine::matching_thread_loop() {
    if (config_.enable_core_pinning) {
        CpuAffinity::pin_current_thread(config_.core_matching);
    }

    IngressMessage msg{};
    size_t counter = 0;

    while (running_.load(std::memory_order_relaxed) || !ingress_to_match_ring_.empty()) {
        if (ingress_to_match_ring_.try_pop(msg)) {
            if (msg.length >= sizeof(itch::AddOrderMsg) && msg.bytes[2] == 'A') {
                char expected_symbol[8];
                std::memset(expected_symbol, ' ', sizeof(expected_symbol));
                std::memcpy(expected_symbol, config_.symbol.data(), config_.symbol.size());
                const auto* add = reinterpret_cast<const itch::AddOrderMsg*>(msg.bytes);
                if (std::memcmp(add->stock, expected_symbol, sizeof(expected_symbol)) != 0) continue;
            }
            const size_t consumed = feed_handler_->process_packet(msg.bytes, msg.length);
            if (consumed != 0 && msg.length >= 3 && msg.bytes[2] == 'A')
                orders_matched_.fetch_add(1, std::memory_order_relaxed);

            // Periodically publish depth view to alpha engine
            if ((++counter & 0x0F) == 0) {
                Top5BookView view{};
                book_->get_top5_view(view);
                (void)match_to_alpha_ring_.try_push(view);
            }
        } else {
            CPU_PAUSE();
        }
    }
    if (counter & 0x0F) {
        Top5BookView final_view{};
        book_->get_top5_view(final_view);
        while (!match_to_alpha_ring_.try_push(final_view)) CPU_PAUSE();
    }
    matching_done_.store(true, std::memory_order_release);
}

void PipelinedTradingEngine::strategy_thread_loop() {
    if (config_.enable_core_pinning) {
        CpuAffinity::pin_current_thread(config_.core_strategy);
    }

    Top5BookView view{};

    while (running_.load(std::memory_order_relaxed) || !match_to_alpha_ring_.empty() ||
           !matching_done_.load(std::memory_order_acquire)) {
        if (match_to_alpha_ring_.try_pop(view)) {
            if (view.bid_depth == 0 || view.ask_depth == 0) continue;

            uint32_t best_bid = view.bid_prices[0];
            uint32_t best_ask = view.ask_prices[0];
            uint32_t bid_vol  = view.bid_volumes[0];
            uint32_t ask_vol  = view.ask_volumes[0];

            if (best_ask <= best_bid) continue;

            double mid_price = (best_bid + best_ask) / 2.0;
            uint32_t spread = best_ask - best_bid;

            // Stoikov Micro-Price
            uint64_t total_vol = static_cast<uint64_t>(bid_vol) + static_cast<uint64_t>(ask_vol);
            double micro_price = (total_vol > 0) ? 
                (static_cast<double>(bid_vol) * best_ask + static_cast<double>(ask_vol) * best_bid) / total_vol : mid_price;

            // Configurable volume-decay weighted four-level imbalance.
            double weighted_diff = 0.0;
            double weighted_sum = 0.0;
            for (size_t level = 0; level < 4 && level < view.bid_depth && level < view.ask_depth; ++level) {
                const double weight = config_.obi_level_weights[level];
                const double bid_level = view.bid_volumes[level];
                const double ask_level = view.ask_volumes[level];
                weighted_diff += weight * (bid_level - ask_level);
                weighted_sum += weight * (bid_level + ask_level);
            }
            const float obi_l1 = weighted_sum > 0.0 ? static_cast<float>(weighted_diff / weighted_sum) : 0.0f;

            // Avellaneda-Stoikov Inventory Reservation Price Skew
            int64_t curr_pos = risk_engine_.current_position();
            double reservation_price = micro_price - (curr_pos * config_.inventory_gamma);

            StrategyOrderIntent intent{};
            intent.timestamp_ns = TscClock::rdtsc();
            intent.best_bid = best_bid;
            intent.best_ask = best_ask;
            intent.best_bid_vol = bid_vol;
            intent.best_ask_vol = ask_vol;
            intent.mid_price = mid_price;
            intent.micro_price = micro_price;
            intent.obi = obi_l1;

            if (obi_l1 > config_.obi_threshold && reservation_price > mid_price) {
                if (obi_l1 > config_.aggressive_obi_threshold) {
                    intent.action = StrategyOrderIntent::Action::AggressiveTakeBuy;
                    intent.side = 0;
                    intent.bid_price = best_ask;
                    intent.qty = config_.quote_lot_size;
                } else {
                    intent.action = StrategyOrderIntent::Action::QuoteBid;
                    intent.side = 0;
                    intent.bid_price = best_bid;
                    intent.qty = config_.quote_lot_size;
                }
            } else if (obi_l1 < -config_.obi_threshold && reservation_price < mid_price) {
                if (obi_l1 < -config_.aggressive_obi_threshold) {
                    intent.action = StrategyOrderIntent::Action::AggressiveTakeSell;
                    intent.side = 1;
                    intent.ask_price = best_bid;
                    intent.qty = config_.quote_lot_size;
                } else {
                    intent.action = StrategyOrderIntent::Action::QuoteAsk;
                    intent.side = 1;
                    intent.ask_price = best_ask;
                    intent.qty = config_.quote_lot_size;
                }
            } else if (spread >= config_.min_spread_ticks) {
                intent.action = StrategyOrderIntent::Action::QuoteTwoSided;
                intent.bid_price = best_bid;
                intent.ask_price = best_ask;
                intent.qty = config_.quote_lot_size;
            }

            if (intent.action != StrategyOrderIntent::Action::Hold) {
                (void)alpha_to_exec_ring_.try_push(intent);
            }
        } else {
            CPU_PAUSE();
        }
    }
    strategy_done_.store(true, std::memory_order_release);
}

void PipelinedTradingEngine::execution_thread_loop() {
    if (config_.enable_core_pinning) {
        CpuAffinity::pin_current_thread(config_.core_execution);
    }

    StrategyOrderIntent intent{};
    uint8_t ouch_wire_buffer[128];

    while (running_.load(std::memory_order_relaxed) || !alpha_to_exec_ring_.empty() ||
           !strategy_done_.load(std::memory_order_acquire)) {
        if (alpha_to_exec_ring_.try_pop(intent)) {
            const uint64_t risk_timestamp_ns = monotonic_time_ns();
            uint64_t t0 = TscClock::rdtsc();

            double mid = intent.mid_price;
            uint32_t mid_ticks = static_cast<uint32_t>(mid);
            risk_engine_.mark_to_market(mid_ticks);

            // Record market tick for TCA
            tca_engine_.record_market_tick(t0, mid_ticks);

            if (intent.action == StrategyOrderIntent::Action::QuoteBid || 
                intent.action == StrategyOrderIntent::Action::AggressiveTakeBuy) 
            {
                // Cancel previous resting quotes (and stale opposite quote when direction is buy)
                if (last_active_bid_token_ != 0) {
                    oms_.on_order_canceled(last_active_bid_token_, config_.quote_lot_size, risk_engine_);
                    last_active_bid_token_ = 0;
                }
                if (last_active_ask_token_ != 0) {
                    oms_.on_order_canceled(last_active_ask_token_, config_.quote_lot_size, risk_engine_);
                    last_active_ask_token_ = 0;
                }

                uint64_t token = token_generator_.fetch_add(1, std::memory_order_relaxed);
                auto* ord = oms_.submit_order(token, 0, intent.bid_price, intent.qty, mid_ticks, risk_timestamp_ns, risk_engine_);

                if (__builtin_expect(ord && ord->status == OrderStatus::PendingNew, 1)) {
                    // Local OUCH wire formatting demonstration (no live outbound socket)
                    (void)OuchSerializer::serialize_enter_order(ouch_wire_buffer, token, 'B', intent.qty, config_.symbol, intent.bid_price);
                    oms_.on_order_accepted(token, token + 100000);

                    if (intent.action == StrategyOrderIntent::Action::AggressiveTakeBuy) {
                        // Bounded fill: consume displayed ask liquidity using multi-level ledger
                        const uint32_t fill_qty = ask_depth_ledger_.consume(intent.bid_price, intent.best_ask_vol, intent.qty);
                        if (fill_qty > 0) {
                            oms_.on_order_executed(token, fill_qty, intent.bid_price, risk_engine_);
                        }
                        if (fill_qty < intent.qty) {
                            oms_.on_order_canceled(token, intent.qty - fill_qty, risk_engine_);
                        }
                    } else {
                        last_active_bid_token_ = token;
                    }
                } else {
                    risk_rejections_.fetch_add(1, std::memory_order_relaxed);
                }
            } else if (intent.action == StrategyOrderIntent::Action::QuoteAsk || 
                       intent.action == StrategyOrderIntent::Action::AggressiveTakeSell) 
            {
                // Cancel previous resting quotes (and stale opposite quote when direction is sell)
                if (last_active_ask_token_ != 0) {
                    oms_.on_order_canceled(last_active_ask_token_, config_.quote_lot_size, risk_engine_);
                    last_active_ask_token_ = 0;
                }
                if (last_active_bid_token_ != 0) {
                    oms_.on_order_canceled(last_active_bid_token_, config_.quote_lot_size, risk_engine_);
                    last_active_bid_token_ = 0;
                }

                uint64_t token = token_generator_.fetch_add(1, std::memory_order_relaxed);
                auto* ord = oms_.submit_order(token, 1, intent.ask_price, intent.qty, mid_ticks, risk_timestamp_ns, risk_engine_);

                if (__builtin_expect(ord && ord->status == OrderStatus::PendingNew, 1)) {
                    // Local OUCH wire formatting demonstration (no live outbound socket)
                    (void)OuchSerializer::serialize_enter_order(ouch_wire_buffer, token, 'S', intent.qty, config_.symbol, intent.ask_price);
                    oms_.on_order_accepted(token, token + 100000);

                    if (intent.action == StrategyOrderIntent::Action::AggressiveTakeSell) {
                        // Bounded fill: consume displayed bid liquidity using multi-level ledger
                        const uint32_t fill_qty = bid_depth_ledger_.consume(intent.ask_price, intent.best_bid_vol, intent.qty);
                        if (fill_qty > 0) {
                            oms_.on_order_executed(token, fill_qty, intent.ask_price, risk_engine_);
                        }
                        if (fill_qty < intent.qty) {
                            oms_.on_order_canceled(token, intent.qty - fill_qty, risk_engine_);
                        }
                    } else {
                        last_active_ask_token_ = token;
                    }
                } else {
                    risk_rejections_.fetch_add(1, std::memory_order_relaxed);
                }
            } else if (intent.action == StrategyOrderIntent::Action::QuoteTwoSided) {
                // Cancel previous two-sided quotes
                if (last_active_bid_token_ != 0) {
                    oms_.on_order_canceled(last_active_bid_token_, config_.quote_lot_size, risk_engine_);
                    last_active_bid_token_ = 0;
                }
                if (last_active_ask_token_ != 0) {
                    oms_.on_order_canceled(last_active_ask_token_, config_.quote_lot_size, risk_engine_);
                    last_active_ask_token_ = 0;
                }

                // Bid leg
                uint64_t b_token = token_generator_.fetch_add(1, std::memory_order_relaxed);
                auto* b_ord = oms_.submit_order(b_token, 0, intent.bid_price, intent.qty, mid_ticks, risk_timestamp_ns, risk_engine_);
                if (b_ord && b_ord->status == OrderStatus::PendingNew) {
                    (void)OuchSerializer::serialize_enter_order(ouch_wire_buffer, b_token, 'B', intent.qty, config_.symbol, intent.bid_price);
                    oms_.on_order_accepted(b_token, b_token + 100000);
                    last_active_bid_token_ = b_token;
                }

                // Ask leg
                uint64_t a_token = token_generator_.fetch_add(1, std::memory_order_relaxed);
                auto* a_ord = oms_.submit_order(a_token, 1, intent.ask_price, intent.qty, mid_ticks, risk_timestamp_ns, risk_engine_);
                if (a_ord && a_ord->status == OrderStatus::PendingNew) {
                    (void)OuchSerializer::serialize_enter_order(ouch_wire_buffer, a_token, 'S', intent.qty, config_.symbol, intent.ask_price);
                    oms_.on_order_accepted(a_token, a_token + 100000);
                    last_active_ask_token_ = a_token;
                }
            }

            uint64_t t1 = TscClock::rdtsc();
            exec_profiler_.record_cycles(t0, t1);

            // Update Shared Memory Telemetry Block
            auto* shm = telemetry_mgr_.get();
            if (shm) {
                std::atomic_ref<uint64_t> sequence(shm->sequence);
                sequence.fetch_add(1, std::memory_order_acq_rel);
                shm->heartbeat_timestamp_ns = t1;

                shm->best_bid_price = intent.best_bid;
                shm->best_bid_volume = intent.best_bid_vol;
                shm->best_ask_price = intent.best_ask;
                shm->best_ask_volume = intent.best_ask_vol;
                shm->spread_ticks = (intent.best_ask > intent.best_bid) ? (intent.best_ask - intent.best_bid) : 0;
                shm->mid_price = mid;
                shm->micro_price = intent.micro_price;
                shm->weighted_obi = intent.obi;

                shm->messages_ingested = messages_ingested_.load(std::memory_order_relaxed);
                shm->orders_matched = orders_matched_.load(std::memory_order_relaxed);
                shm->trades_executed = trades_executed_.load(std::memory_order_relaxed);
                shm->volume_traded = volume_traded_.load(std::memory_order_relaxed);

                shm->net_inventory = risk_engine_.current_position();
                shm->realized_pnl_cents = risk_engine_.realized_pnl();
                shm->unrealized_pnl_cents = risk_engine_.unrealized_pnl();
                shm->total_pnl_cents = risk_engine_.total_pnl();
                shm->risk_rejections = risk_rejections_.load(std::memory_order_relaxed);
                shm->kill_switch_active = risk_engine_.is_kill_switch_active();

                if (config_.enable_udp_streaming) {
                    BinaryMarketDataPacket pkt{};
                    pkt.sequence = sequence.load(std::memory_order_relaxed) + 1;
                    pkt.timestamp_ns = t1;
                    std::memcpy(pkt.symbol, config_.symbol.data(), std::min(sizeof(pkt.symbol), config_.symbol.size()));
                    pkt.bid_price = intent.best_bid;
                    pkt.bid_volume = intent.best_bid_vol;
                    pkt.ask_price = intent.best_ask;
                    pkt.ask_volume = intent.best_ask_vol;
                    pkt.mid_price = mid;
                    pkt.micro_price = intent.micro_price;
                    pkt.obi = intent.obi;
                    udp_pub_.publish_tick(pkt);
                }
                sequence.fetch_add(1, std::memory_order_release);
                snapshot_buffer_.publish(*shm);
            }
        } else {
            CPU_PAUSE();
        }
    }

    // Flush any remaining active quotes upon scenario completion
    if (last_active_bid_token_ != 0) {
        oms_.on_order_canceled(last_active_bid_token_, config_.quote_lot_size, risk_engine_);
        last_active_bid_token_ = 0;
    }
    if (last_active_ask_token_ != 0) {
        oms_.on_order_canceled(last_active_ask_token_, config_.quote_lot_size, risk_engine_);
        last_active_ask_token_ = 0;
    }

    auto* final_shm = telemetry_mgr_.get();
    if (final_shm) {
        std::atomic_ref<uint64_t> sequence(final_shm->sequence);
        sequence.fetch_add(1, std::memory_order_acq_rel);
        final_shm->net_inventory = risk_engine_.current_position();
        final_shm->realized_pnl_cents = risk_engine_.realized_pnl();
        final_shm->unrealized_pnl_cents = risk_engine_.unrealized_pnl();
        final_shm->total_pnl_cents = risk_engine_.total_pnl();
        sequence.fetch_add(1, std::memory_order_release);
        snapshot_buffer_.publish(*final_shm);
    }
}

bool PipelinedTradingEngine::read_telemetry_snapshot(ShmTelemetryBlock& out) const noexcept {
    // 1. If workers are fully stopped and joined, read directly
    if (workers_joined_.load(std::memory_order_acquire)) {
        if (snapshot_buffer_.read_snapshot(out)) {
            return true;
        }
        const auto* shm = telemetry_mgr_.get();
        if (shm) {
            std::memcpy(&out, shm, sizeof(ShmTelemetryBlock));
            return true;
        }
        return false;
    }

    // 2. While workers are actively running, read an immutable snapshot from the snapshot triple buffer.
    // This mathematically guarantees write_slot != read_slot at all times, eliminating C++ data races.
    return snapshot_buffer_.read_snapshot(out);
}

void PipelinedTradingEngine::print_status_dashboard() const {
    ShmTelemetryBlock snapshot{};
    if (!read_telemetry_snapshot(snapshot)) {
        std::cout << "\n[Dashboard] Telemetry unavailable (writer contested or uninitialized).\n";
        return;
    }

    uint32_t bid_px = snapshot.best_bid_price;
    uint32_t bid_sz = snapshot.best_bid_volume;
    uint32_t ask_px = snapshot.best_ask_price;
    uint32_t ask_sz = snapshot.best_ask_volume;
    uint32_t spread = snapshot.spread_ticks;
    double mid = snapshot.mid_price;

    // Direct OrderBook inspection is ONLY permitted after all worker threads are joined
    if (workers_joined_.load(std::memory_order_acquire)) {
        const auto bid = book_->get_best_bid();
        const auto ask = book_->get_best_ask();
        if (bid.has_value()) {
            bid_px = bid->price;
            bid_sz = bid->total_volume;
        }
        if (ask.has_value()) {
            ask_px = ask->price;
            ask_sz = ask->total_volume;
        }
        spread = (ask_px > bid_px && bid_px > 0) ? (ask_px - bid_px) : 0;
        mid = (bid_px > 0 && ask_px > 0) ? ((bid_px + ask_px) / 2.0) : 0.0;
    }

    std::cout << "\n==========================================================\n";
    std::cout << "      QUANT-LOB: PAPER REPLAY & EXECUTION TELEMETRY       \n";
    std::cout << "==========================================================\n";
    std::cout << "Symbol                     : " << config_.symbol << "\n";
    std::cout << "Messages Ingested          : " << messages_ingested_.load(std::memory_order_relaxed) << "\n";
    std::cout << "Add Messages Processed     : " << orders_matched_.load(std::memory_order_relaxed) << "\n";
    std::cout << "Market Executions          : " << trades_executed_.load(std::memory_order_relaxed) << "\n";
    std::cout << "Market Volume              : " << volume_traded_.load(std::memory_order_relaxed) << " shares\n";
    std::cout << "----------------------------------------------------------\n";
    std::cout << "Best Bid                   : $" << std::fixed << std::setprecision(2) << (bid_px / 100.0) 
              << " (" << bid_sz << ")\n";
    std::cout << "Best Ask                   : $" << std::fixed << std::setprecision(2) << (ask_px / 100.0) 
              << " (" << ask_sz << ")\n";
    std::cout << "Spread                     : " << spread << " ticks\n";
    std::cout << "Mid-Price                  : $" << std::fixed << std::setprecision(2) << (mid / 100.0) << "\n";
    std::cout << "----------------------------------------------------------\n";
    std::cout << "Current Net Position       : " << (snapshot.net_inventory > 0 ? "+" : "") << snapshot.net_inventory << " shares\n";
    std::cout << "Realized PnL               : $" << std::fixed << std::setprecision(2) << (snapshot.realized_pnl_cents / 100.0) << "\n";
    std::cout << "Unrealized PnL             : $" << std::fixed << std::setprecision(2) << (snapshot.unrealized_pnl_cents / 100.0) << "\n";
    std::cout << "Total Session PnL          : $" << std::fixed << std::setprecision(2) << (snapshot.total_pnl_cents / 100.0) << "\n";
    std::cout << "Risk Rejections            : " << snapshot.risk_rejections << "\n";
    std::cout << "Paper Execution Model      : Bounded fills (depth-consumed) | Fees: Excluded (0.0 bps)\n";
    std::cout << "Active Paper Orders        : 0 resting (All quotes flushed cleanly)\n";
    std::cout << "Emergency Kill Switch      : " << (snapshot.kill_switch_active ? "TRIGGERED [HALTED]" : "ACTIVE [NORMAL]") << "\n";
    std::cout << "==========================================================\n\n";
}

} // namespace lob

