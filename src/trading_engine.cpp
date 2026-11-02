#include "trading_engine.hpp"
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

PipelinedTradingEngine::PipelinedTradingEngine(const EngineConfig& config)
    : config_(config),
      risk_engine_(config.risk_config) 
{
    book_ = std::make_unique<OrderBook>();
    feed_handler_ = std::make_unique<FeedHandler>(*book_);
    mold_parser_ = std::make_unique<MoldUdp64Parser<1024>>(*feed_handler_);

    telemetry_mgr_.init_producer();

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
    if (running_.exchange(true)) return;

    matching_thread_ = std::thread(&PipelinedTradingEngine::matching_thread_loop, this);
    strategy_thread_ = std::thread(&PipelinedTradingEngine::strategy_thread_loop, this);
    execution_thread_ = std::thread(&PipelinedTradingEngine::execution_thread_loop, this);
}

void PipelinedTradingEngine::stop() {
    if (!running_.exchange(false)) return;

    if (matching_thread_.joinable()) matching_thread_.join();
    if (strategy_thread_.joinable()) strategy_thread_.join();
    if (execution_thread_.joinable()) execution_thread_.join();
}

bool PipelinedTradingEngine::inject_network_packet(const uint8_t* buffer, size_t length, uint64_t timestamp_ns) {
    if (__builtin_expect(!running_.load(std::memory_order_relaxed), 0)) return false;

    // Fast path: if this is a MoldUDP64 packet, parse datagram directly
    size_t parsed = mold_parser_->process_datagram(buffer, length, timestamp_ns);
    if (parsed > 0) {
        messages_ingested_.fetch_add(parsed, std::memory_order_relaxed);
        return true;
    }

    // Direct ITCH message ingress
    if (length >= sizeof(itch::AddOrderMsg)) {
        itch::AddOrderMsg msg{};
        std::memcpy(&msg, buffer, sizeof(itch::AddOrderMsg));
        while (!ingress_to_match_ring_.try_push(msg)) {
            if (!running_.load(std::memory_order_relaxed)) return false;
            CPU_PAUSE();
        }
        messages_ingested_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    return false;
}

void PipelinedTradingEngine::matching_thread_loop() {
    if (config_.enable_core_pinning) {
        CpuAffinity::pin_current_thread(config_.core_matching);
    }

    itch::AddOrderMsg msg{};
    size_t counter = 0;

    while (running_.load(std::memory_order_relaxed)) {
        if (ingress_to_match_ring_.try_pop(msg)) {
            uint64_t oid = itch::read_be64(&msg.order_ref_num);
            uint32_t shares = itch::read_be32(&msg.shares);
            uint32_t price = itch::read_be32(&msg.price) / 100;
            Side side = (msg.buy_sell_indicator == 'B') ? Side::Buy : Side::Sell;

            (void)book_->add_limit_order(oid, 1, side, price, shares);
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
}

void PipelinedTradingEngine::strategy_thread_loop() {
    if (config_.enable_core_pinning) {
        CpuAffinity::pin_current_thread(config_.core_strategy);
    }

    Top5BookView view{};

    while (running_.load(std::memory_order_relaxed)) {
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

            // SIMD-vectorized 4-level Order Book Imbalance (OBI)
            float obi_l1 = (bid_vol + ask_vol > 0) ? 
                static_cast<float>(static_cast<int64_t>(bid_vol) - static_cast<int64_t>(ask_vol)) / static_cast<float>(bid_vol + ask_vol) : 0.0f;

            // Avellaneda-Stoikov Inventory Reservation Price Skew
            int64_t curr_pos = risk_engine_.current_position();
            double reservation_price = micro_price - (curr_pos * config_.inventory_gamma);

            StrategyOrderIntent intent{};
            intent.timestamp_ns = TscClock::rdtsc();

            if (obi_l1 > config_.obi_threshold && reservation_price > mid_price) {
                if (obi_l1 > 0.60f) {
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
                if (obi_l1 < -0.60f) {
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
            } else if (spread >= 2) {
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
}

void PipelinedTradingEngine::execution_thread_loop() {
    if (config_.enable_core_pinning) {
        CpuAffinity::pin_current_thread(config_.core_execution);
    }

    StrategyOrderIntent intent{};
    uint8_t ouch_wire_buffer[128];

    while (running_.load(std::memory_order_relaxed)) {
        if (alpha_to_exec_ring_.try_pop(intent)) {
            uint64_t t0 = TscClock::rdtsc();

            auto mid = book_->get_mid_price();
            uint32_t mid_ticks = static_cast<uint32_t>(mid);

            // Record market tick for TCA
            tca_engine_.record_market_tick(t0, mid_ticks);

            if (intent.action == StrategyOrderIntent::Action::QuoteBid || 
                intent.action == StrategyOrderIntent::Action::AggressiveTakeBuy) 
            {
                uint64_t token = token_generator_.fetch_add(1, std::memory_order_relaxed);
                auto* ord = oms_.submit_order(token, 0, intent.bid_price, intent.qty, mid_ticks, t0, risk_engine_);

                if (__builtin_expect(ord && ord->status == OrderStatus::PendingNew, 1)) {
                    (void)OuchSerializer::serialize_enter_order(ouch_wire_buffer, token, 'B', intent.qty, config_.symbol, intent.bid_price);
                    oms_.on_order_accepted(token, token + 100000);
                } else {
                    risk_rejections_.fetch_add(1, std::memory_order_relaxed);
                }
            } else if (intent.action == StrategyOrderIntent::Action::QuoteAsk || 
                       intent.action == StrategyOrderIntent::Action::AggressiveTakeSell) 
            {
                uint64_t token = token_generator_.fetch_add(1, std::memory_order_relaxed);
                auto* ord = oms_.submit_order(token, 1, intent.ask_price, intent.qty, mid_ticks, t0, risk_engine_);

                if (__builtin_expect(ord && ord->status == OrderStatus::PendingNew, 1)) {
                    (void)OuchSerializer::serialize_enter_order(ouch_wire_buffer, token, 'S', intent.qty, config_.symbol, intent.ask_price);
                    oms_.on_order_accepted(token, token + 100000);
                } else {
                    risk_rejections_.fetch_add(1, std::memory_order_relaxed);
                }
            } else if (intent.action == StrategyOrderIntent::Action::QuoteTwoSided) {
                // Bid leg
                uint64_t b_token = token_generator_.fetch_add(1, std::memory_order_relaxed);
                auto* b_ord = oms_.submit_order(b_token, 0, intent.bid_price, intent.qty, mid_ticks, t0, risk_engine_);
                if (b_ord && b_ord->status == OrderStatus::PendingNew) {
                    (void)OuchSerializer::serialize_enter_order(ouch_wire_buffer, b_token, 'B', intent.qty, config_.symbol, intent.bid_price);
                    oms_.on_order_accepted(b_token, b_token + 100000);
                }

                // Ask leg
                uint64_t a_token = token_generator_.fetch_add(1, std::memory_order_relaxed);
                auto* a_ord = oms_.submit_order(a_token, 1, intent.ask_price, intent.qty, mid_ticks, t0, risk_engine_);
                if (a_ord && a_ord->status == OrderStatus::PendingNew) {
                    (void)OuchSerializer::serialize_enter_order(ouch_wire_buffer, a_token, 'S', intent.qty, config_.symbol, intent.ask_price);
                    oms_.on_order_accepted(a_token, a_token + 100000);
                }
            }

            uint64_t t1 = TscClock::rdtsc();
            exec_profiler_.record_cycles(t0, t1);

            // Update Shared Memory Telemetry Block
            auto* shm = telemetry_mgr_.get();
            if (shm) {
                shm->sequence++;
                shm->heartbeat_timestamp_ns = t1;

                auto best_bid = book_->get_best_bid();
                auto best_ask = book_->get_best_ask();
                shm->best_bid_price = best_bid ? best_bid->price : 0;
                shm->best_bid_volume = best_bid ? best_bid->total_volume : 0;
                shm->best_ask_price = best_ask ? best_ask->price : 0;
                shm->best_ask_volume = best_ask ? best_ask->total_volume : 0;
                shm->spread_ticks = (best_bid && best_ask && best_ask->price > best_bid->price) ? (best_ask->price - best_bid->price) : 0;
                shm->mid_price = mid;

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
            }
        } else {
            CPU_PAUSE();
        }
    }
}

void PipelinedTradingEngine::print_status_dashboard() const {
    const auto* shm = telemetry_mgr_.get();
    if (!shm) return;

    std::cout << "\n==========================================================\n";
    std::cout << "      QUANT-LOB: PRODUCTION TRADING SERVICE DASHBOARD     \n";
    std::cout << "==========================================================\n";
    std::cout << "Symbol                     : " << config_.symbol << "\n";
    std::cout << "Messages Ingested          : " << shm->messages_ingested << "\n";
    std::cout << "Orders Matched in Book     : " << shm->orders_matched << "\n";
    std::cout << "Trades Executed            : " << shm->trades_executed << "\n";
    std::cout << "Volume Traded              : " << shm->volume_traded << " shares\n";
    std::cout << "----------------------------------------------------------\n";
    std::cout << "Best Bid                   : $" << std::fixed << std::setprecision(2) << (shm->best_bid_price / 100.0) 
              << " (" << shm->best_bid_volume << ")\n";
    std::cout << "Best Ask                   : $" << std::fixed << std::setprecision(2) << (shm->best_ask_price / 100.0) 
              << " (" << shm->best_ask_volume << ")\n";
    std::cout << "Spread                     : " << shm->spread_ticks << " ticks\n";
    std::cout << "Mid-Price                  : $" << std::fixed << std::setprecision(2) << (shm->mid_price / 100.0) << "\n";
    std::cout << "----------------------------------------------------------\n";
    std::cout << "Current Net Position       : " << (shm->net_inventory > 0 ? "+" : "") << shm->net_inventory << " shares\n";
    std::cout << "Realized PnL               : $" << std::fixed << std::setprecision(2) << (shm->realized_pnl_cents / 100.0) << "\n";
    std::cout << "Unrealized PnL             : $" << std::fixed << std::setprecision(2) << (shm->unrealized_pnl_cents / 100.0) << "\n";
    std::cout << "Total Session PnL          : $" << std::fixed << std::setprecision(2) << (shm->total_pnl_cents / 100.0) << "\n";
    std::cout << "Risk Rejections            : " << shm->risk_rejections << "\n";
    std::cout << "Emergency Kill Switch      : " << (shm->kill_switch_active ? "TRIGGERED [HALTED]" : "ACTIVE [NORMAL]") << "\n";
    std::cout << "==========================================================\n\n";
}

} // namespace lob

