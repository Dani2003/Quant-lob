#define QUANT_LOB_EXPORTS
#include "quant_lob_c_api.h"
#include "order_book.hpp"
#include <immintrin.h>
#include <memory>
#include <cstring>

using namespace lob;

extern "C" {

OrderBookHandle lob_create(void) {
    auto* book = new (std::nothrow) OrderBook();
    return static_cast<OrderBookHandle>(book);
}

void lob_destroy(OrderBookHandle handle) {
    if (handle) {
        delete static_cast<OrderBook*>(handle);
    }
}

int lob_add_limit(
    OrderBookHandle handle,
    uint64_t order_id,
    uint64_t client_id,
    uint8_t side,
    uint32_t price,
    uint32_t quantity,
    uint64_t timestamp_ns) 
{
    if (!handle) return 0;
    auto* book = static_cast<OrderBook*>(handle);
    Side s = (side == 0) ? Side::Buy : Side::Sell;
    return book->add_limit_order(order_id, client_id, s, price, quantity, timestamp_ns) ? 1 : 0;
}

int lob_add_market(
    OrderBookHandle handle,
    uint64_t order_id,
    uint64_t client_id,
    uint8_t side,
    uint32_t quantity,
    uint64_t timestamp_ns) 
{
    if (!handle) return 0;
    auto* book = static_cast<OrderBook*>(handle);
    Side s = (side == 0) ? Side::Buy : Side::Sell;
    return book->add_market_order(order_id, client_id, s, quantity, timestamp_ns) ? 1 : 0;
}

int lob_cancel(OrderBookHandle handle, uint64_t order_id) {
    if (!handle) return 0;
    auto* book = static_cast<OrderBook*>(handle);
    return book->cancel_order(order_id) ? 1 : 0;
}

int lob_get_bbo(
    OrderBookHandle handle,
    uint32_t* best_bid_price,
    uint32_t* best_bid_vol,
    uint32_t* best_ask_price,
    uint32_t* best_ask_vol) 
{
    if (!handle) return 0;
    auto* book = static_cast<OrderBook*>(handle);
    const auto bid = book->get_best_bid();
    const auto ask = book->get_best_ask();

    if (best_bid_price) *best_bid_price = bid ? bid->price : 0;
    if (best_bid_vol)   *best_bid_vol   = bid ? bid->total_volume : 0;
    if (best_ask_price) *best_ask_price = ask ? ask->price : 0;
    if (best_ask_vol)   *best_ask_vol   = ask ? ask->total_volume : 0;

    return (bid.has_value() || ask.has_value()) ? 1 : 0;
}

int lob_get_mid_and_spread(
    OrderBookHandle handle,
    double* mid_price,
    uint32_t* spread_ticks) 
{
    if (!handle) return 0;
    auto* book = static_cast<OrderBook*>(handle);
    const auto bid = book->get_best_bid();
    const auto ask = book->get_best_ask();

    if (bid && ask && ask->price > bid->price) {
        if (mid_price) *mid_price = (static_cast<double>(bid->price) + static_cast<double>(ask->price)) / 2.0;
        if (spread_ticks) *spread_ticks = ask->price - bid->price;
        return 1;
    }
    if (mid_price) *mid_price = 0.0;
    if (spread_ticks) *spread_ticks = 0;
    return 0;
}

int lob_get_top5(
    OrderBookHandle handle,
    uint32_t* bid_prices,
    uint32_t* bid_vols,
    uint32_t* ask_prices,
    uint32_t* ask_vols,
    uint32_t* bid_depth,
    uint32_t* ask_depth) 
{
    if (!handle) return 0;
    auto* book = static_cast<OrderBook*>(handle);
    Top5BookView view{};
    book->get_top5_view(view);

    if (bid_depth) *bid_depth = view.bid_depth;
    if (ask_depth) *ask_depth = view.ask_depth;

    for (size_t i = 0; i < 5; ++i) {
        if (bid_prices) bid_prices[i] = view.bid_prices[i];
        if (bid_vols)   bid_vols[i]   = view.bid_volumes[i];
        if (ask_prices) ask_prices[i] = view.ask_prices[i];
        if (ask_vols)   ask_vols[i]   = view.ask_volumes[i];
    }
    return 1;
}


void lob_calculate_obi(
    const uint32_t* bid_vols,
    const uint32_t* ask_vols,
    float* out_levels_4,
    float* out_weighted_obi) 
{
    if (!bid_vols || !ask_vols) return;

#if defined(__SSE4_1__) || defined(__AVX2__)
    __m128 b = _mm_set_ps(static_cast<float>(bid_vols[3]), static_cast<float>(bid_vols[2]),
                           static_cast<float>(bid_vols[1]), static_cast<float>(bid_vols[0]));
    __m128 a = _mm_set_ps(static_cast<float>(ask_vols[3]), static_cast<float>(ask_vols[2]),
                           static_cast<float>(ask_vols[1]), static_cast<float>(ask_vols[0]));

    __m128 diff = _mm_sub_ps(b, a);
    __m128 sum  = _mm_add_ps(b, a);
    __m128 eps  = _mm_set1_ps(1e-6f);
    sum = _mm_max_ps(sum, eps);

    __m128 obi = _mm_div_ps(diff, sum);

    alignas(16) float res[4];
    _mm_store_ps(res, obi);

    if (out_levels_4) {
        for (int i = 0; i < 4; ++i) out_levels_4[i] = res[i];
    }
    if (out_weighted_obi) {
        *out_weighted_obi = res[0] * 0.40f + res[1] * 0.30f + res[2] * 0.20f + res[3] * 0.10f;
    }
#else
    float weighted = 0.0f;
    const float weights[4] = {0.40f, 0.30f, 0.20f, 0.10f};
    for (int i = 0; i < 4; ++i) {
        float b = static_cast<float>(bid_vols[i]);
        float a = static_cast<float>(ask_vols[i]);
        float sum = (b + a > 0.0f) ? (b + a) : 1e-6f;
        float o = (b - a) / sum;
        if (out_levels_4) out_levels_4[i] = o;
        weighted += o * weights[i];
    }
    if (out_weighted_obi) *out_weighted_obi = weighted;
#endif
}

double lob_calculate_micro_price(
    uint32_t best_bid,
    uint32_t bid_vol,
    uint32_t best_ask,
    uint32_t ask_vol) 
{
    uint64_t total = static_cast<uint64_t>(bid_vol) + static_cast<uint64_t>(ask_vol);
    if (total == 0) return 0.0;
    double weighted = static_cast<double>(bid_vol) * static_cast<double>(best_ask)
                    + static_cast<double>(ask_vol) * static_cast<double>(best_bid);
    return weighted / static_cast<double>(total);
}

} // extern "C"

