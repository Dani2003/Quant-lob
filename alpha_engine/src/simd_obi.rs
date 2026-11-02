//! SIMD-Accelerated Order Book Imbalance (OBI) Engine
//!
//! Formula:
//!   OBI = (BidVolume - AskVolume) / (BidVolume + AskVolume)
//!
//! Uses hardware vector instructions (SSE4.1 / AVX) to process 4 price levels
//! in parallel within a single CPU clock cycle.

/// Result of vectorized 4-level Order Book Imbalance calculation.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Level4Obi {
    /// Per-level imbalance values [-1.0, 1.0] for Top 4 depth levels
    pub levels: [f32; 4],
    /// Volume-decay weighted aggregate imbalance across levels
    pub weighted_obi: f32,
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
        // Load first 4 uint32 elements directly into 128-bit XMM registers (Zero-Copy)
        let bids_i = _mm_loadu_si128(bids.as_ptr() as *const __m128i);
        let asks_i = _mm_loadu_si128(asks.as_ptr() as *const __m128i);

        // Convert packed 32-bit integers to single-precision IEEE-754 floats
        let bids_f = _mm_cvtepi32_ps(bids_i);
        let asks_f = _mm_cvtepi32_ps(asks_i);

        // Vector subtraction: (Bid - Ask) across all 4 levels in parallel
        let diff = _mm_sub_ps(bids_f, asks_f);

        // Vector addition: (Bid + Ask) across all 4 levels in parallel
        let sum = _mm_add_ps(bids_f, asks_f);

        // Compare sum == 0.0 to prevent division by zero NaN
        let zero = _mm_setzero_ps();
        let is_zero = _mm_cmpeq_ps(sum, zero);

        // Vector division: (Bid - Ask) / (Bid + Ask)
        let div = _mm_div_ps(diff, sum);

        // Blend: if sum == 0 return 0.0, else return div
        let safe_obi = _mm_blendv_ps(div, zero, is_zero);

        let mut levels = [0.0f32; 4];
        _mm_storeu_ps(levels.as_mut_ptr(), safe_obi);

        // Vectorized Exponential Decay Weights: [1.0, 0.5, 0.25, 0.125]
        let weights = _mm_setr_ps(1.0, 0.5, 0.25, 0.125);
        let w_diff = _mm_mul_ps(diff, weights);
        let w_sum = _mm_mul_ps(sum, weights);

        // Horizontal sum using SSE3/SSE4 horizontal adds
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
            // SAFETY: Verified feature availability via CPUID
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
    fn test_perfect_ask_skew() {
        let bids = [0, 0, 0, 0, 0];
        let asks = [1000, 500, 200, 100, 0];

        let res = calculate_obi(&bids, &asks);
        assert_eq!(res.levels, [-1.0, -1.0, -1.0, -1.0]);
        assert_eq!(res.weighted_obi, -1.0);
    }

    #[test]
    fn test_balanced_book() {
        let bids = [500, 300, 200, 100, 50];
        let asks = [500, 300, 200, 100, 50];

        let res = calculate_obi(&bids, &asks);
        assert_eq!(res.levels, [0.0, 0.0, 0.0, 0.0]);
        assert_eq!(res.weighted_obi, 0.0);
    }

    #[test]
    fn test_zero_volume_graceful() {
        let bids = [0, 0, 0, 0, 0];
        let asks = [0, 0, 0, 0, 0];

        let res = calculate_obi(&bids, &asks);
        assert_eq!(res.levels, [0.0, 0.0, 0.0, 0.0]);
        assert_eq!(res.weighted_obi, 0.0);
    }
}

