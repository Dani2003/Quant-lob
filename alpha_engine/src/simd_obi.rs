//! SIMD-Accelerated Order Book Imbalance (OBI) & Quantitative Signals Engine
//!
//! Mathematical Formulations:
//!   1. OBI = (BidVolume - AskVolume) / (BidVolume + AskVolume)
//!   2. Stoikov Micro-Price = (BidVol * AskPrice + AskVol * BidPrice) / (BidVol + AskVol)
//!   3. Multi-Level Volume-Decay Weighted Pressure

/// Result of vectorized 4-level Order Book Imbalance calculation.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Level4Obi {
    /// Per-level imbalance values [-1.0, 1.0] for Top 4 depth levels
    pub levels: [f32; 4],
    /// Volume-decay weighted aggregate imbalance across levels
    pub weighted_obi: f32,
}

/// Comprehensive Quantitative Alpha Signal containing OBI, Micro-Price, and Spread.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct QuantAlphaSignal {
    pub obi: Level4Obi,
    pub micro_price: f64,
    pub mid_price: f64,
    pub spread_ticks: u32,
}

/// Calculates the Stoikov Micro-Price adjusting mid-price by order queue pressure.
#[inline(always)]
pub fn calculate_micro_price(
    best_bid_price: u32,
    best_bid_vol: u32,
    best_ask_price: u32,
    best_ask_vol: u32,
) -> f64 {
    let total_vol = (best_bid_vol + best_ask_vol) as f64;
    if total_vol == 0.0 {
        return (best_bid_price as f64 + best_ask_price as f64) / 2.0;
    }
    ((best_bid_vol as f64 * best_ask_price as f64) + (best_ask_vol as f64 * best_bid_price as f64))
        / total_vol
}

/// Fallback portable scalar calculation for non-x86 or when SIMD is unavailable.
#[inline]
pub fn calculate_obi_scalar(bids: &[u32; 5], asks: &[u32; 5]) -> Level4Obi {
    let mut levels = [0.0f32; 4];
    let weights = [1.0f32, 0.5f32, 0.25f32, 0.125f32];

    let mut weighted_diff = 0.0f32;
    let mut weighted_sum = 0.0f32;

    for i in 0..4 {
        let b = bids[i] as f32;
        let a = asks[i] as f32;
        let sum = b + a;
        let diff = b - a;

        levels[i] = if sum > 0.0 { diff / sum } else { 0.0 };

        weighted_diff += weights[i] * diff;
        weighted_sum += weights[i] * sum;
    }

    let weighted_obi = if weighted_sum > 0.0 {
        weighted_diff / weighted_sum
    } else {
        0.0
    };

    Level4Obi {
        levels,
        weighted_obi,
    }
}

#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
pub mod x86 {
    use super::Level4Obi;
    use core::arch::x86_64::*;

    /// Vectorized 4-level OBI calculation executing in parallel in SIMD registers.
    ///
    /// # Safety
    /// Requires SSE4.1 CPU instruction support.
    #[target_feature(enable = "sse4.1")]
    pub unsafe fn calculate_obi_simd_sse41(bids: &[u32; 5], asks: &[u32; 5]) -> Level4Obi {
        let bids_i = _mm_loadu_si128(bids.as_ptr() as *const __m128i);
        let asks_i = _mm_loadu_si128(asks.as_ptr() as *const __m128i);

        let bids_f = _mm_cvtepi32_ps(bids_i);
        let asks_f = _mm_cvtepi32_ps(asks_i);

        let diff = _mm_sub_ps(bids_f, asks_f);
        let sum = _mm_add_ps(bids_f, asks_f);

        let zero = _mm_setzero_ps();
        let is_zero = _mm_cmpeq_ps(sum, zero);

        let div = _mm_div_ps(diff, sum);
        let safe_obi = _mm_blendv_ps(div, zero, is_zero);

        let mut levels = [0.0f32; 4];
        _mm_storeu_ps(levels.as_mut_ptr(), safe_obi);

        let weights = _mm_setr_ps(1.0, 0.5, 0.25, 0.125);
        let w_diff = _mm_mul_ps(diff, weights);
        let w_sum = _mm_mul_ps(sum, weights);

        let h_diff = _mm_hadd_ps(w_diff, w_diff);
        let total_w_diff = _mm_cvtss_f32(_mm_hadd_ps(h_diff, h_diff));

        let h_sum = _mm_hadd_ps(w_sum, w_sum);
        let total_w_sum = _mm_cvtss_f32(_mm_hadd_ps(h_sum, h_sum));

        let weighted_obi = if total_w_sum > 0.0 {
            total_w_diff / total_w_sum
        } else {
            0.0
        };

        Level4Obi {
            levels,
            weighted_obi,
        }
    }
}

/// Dispatches to hardware SIMD if supported by CPU, otherwise falls back gracefully.
#[inline]
pub fn calculate_obi(bids: &[u32; 5], asks: &[u32; 5]) -> Level4Obi {
    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    {
        if is_x86_feature_detected!("sse4.1") {
            return unsafe { x86::calculate_obi_simd_sse41(bids, asks) };
        }
    }

    calculate_obi_scalar(bids, asks)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_perfect_bid_skew() {
        let bids = [1000, 500, 200, 100, 0];
        let asks = [0, 0, 0, 0, 0];
        let res = calculate_obi(&bids, &asks);
        assert_eq!(res.levels, [1.0, 1.0, 1.0, 1.0]);
        assert_eq!(res.weighted_obi, 1.0);
    }

    #[test]
    fn test_micro_price_calculation() {
        // Bid: $100.00 (vol: 800), Ask: $100.02 (vol: 200)
        // More bids than asks -> micro-price leans toward ask price
        let mp = calculate_micro_price(10000, 800, 10002, 200);
        assert!(mp > 10001.0, "Micro-price must adjust higher due to strong bid volume.");
        assert_eq!(mp, 10001.6);
    }
}
