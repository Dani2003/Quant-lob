#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
    #ifdef QUANT_LOB_EXPORTS
        #define QUANT_LOB_API __declspec(dllexport)
    #else
        #define QUANT_LOB_API __declspec(dllimport)
    #endif
#else
    #define QUANT_LOB_API __attribute__((visibility("default")))
#endif

typedef void* OrderBookHandle;

/// Create a new pre-allocated C++20 OrderBook instance
QUANT_LOB_API OrderBookHandle lob_create(void);

/// Destroy an OrderBook instance and free its resources
QUANT_LOB_API void lob_destroy(OrderBookHandle handle);

/// Add a Limit Order (side: 0 = Buy, 1 = Sell)
QUANT_LOB_API int lob_add_limit(
    OrderBookHandle handle,
    uint64_t order_id,
    uint64_t client_id,
    uint8_t side,
    uint32_t price,
    uint32_t quantity,
    uint64_t timestamp_ns
);

/// Add a Market Order (side: 0 = Buy, 1 = Sell)
QUANT_LOB_API int lob_add_market(
    OrderBookHandle handle,
    uint64_t order_id,
    uint64_t client_id,
    uint8_t side,
    uint32_t quantity,
    uint64_t timestamp_ns
);

/// Cancel an order by ID in O(1)
QUANT_LOB_API int lob_cancel(OrderBookHandle handle, uint64_t order_id);

/// Get Best Bid and Offer (BBO)
QUANT_LOB_API int lob_get_bbo(
    OrderBookHandle handle,
    uint32_t* best_bid_price,
    uint32_t* best_bid_vol,
    uint32_t* best_ask_price,
    uint32_t* best_ask_vol
);

/// Get current Mid-Price and Spread
QUANT_LOB_API int lob_get_mid_and_spread(
    OrderBookHandle handle,
    double* mid_price,
    uint32_t* spread_ticks
);

/// Read Top-5 Level Depth (Prices and Volumes)
QUANT_LOB_API int lob_get_top5(
    OrderBookHandle handle,
    uint32_t* bid_prices,
    uint32_t* bid_vols,
    uint32_t* ask_prices,
    uint32_t* ask_vols,
    uint32_t* bid_depth,
    uint32_t* ask_depth
);

/// Calculate SIMD Order Book Imbalance across top 4 levels
QUANT_LOB_API void lob_calculate_obi(
    const uint32_t* bid_vols,
    const uint32_t* ask_vols,
    float* out_levels_4,
    float* out_weighted_obi
);

/// Calculate Stoikov Micro-Price
QUANT_LOB_API double lob_calculate_micro_price(
    uint32_t best_bid,
    uint32_t bid_vol,
    uint32_t best_ask,
    uint32_t ask_vol
);

#ifdef __cplusplus
}
#endif

