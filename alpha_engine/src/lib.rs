pub mod simd_obi;

pub use simd_obi::{calculate_obi, Level4Obi};

// Explicitly bind the static C++ bridge library compiled by build.rs
#[link(name = "quant_lob_bridge", kind = "static")]
extern "C" {}

#[cxx::bridge(namespace = "lob")]
pub mod ffi {
    /// Shared memory snapshot of Top-5 price/volume levels.
    /// Aligned for zero-copy SIMD vector processing.
    #[derive(Debug, Clone, Copy, Default)]
    pub struct Top5View {
        pub bid_prices: [u32; 5],
        pub bid_volumes: [u32; 5],
        pub ask_prices: [u32; 5],
        pub ask_volumes: [u32; 5],
        pub bid_depth: u32,
        pub ask_depth: u32,
    }

    unsafe extern "C++" {
        include!("order_book_bridge.hpp");

        type OrderBook;

        /// Factory creating an unallocated, zero-heap pre-initialized C++ OrderBook.
        fn new_order_book() -> UniquePtr<OrderBook>;

        /// Hot-path Limit Order submission.
        fn order_book_add_limit(
            book: Pin<&mut OrderBook>,
            order_id: u64,
            client_id: u64,
            side: u8,
            price: u32,
            quantity: u32,
            timestamp_ns: u64,
        ) -> bool;

        /// Hot-path Market Order submission.
        fn order_book_add_market(
            book: Pin<&mut OrderBook>,
            order_id: u64,
            client_id: u64,
            side: u8,
            quantity: u32,
            timestamp_ns: u64,
        ) -> bool;

        /// Hot-path Order Cancellation by ID in O(1).
        fn order_book_cancel(book: Pin<&mut OrderBook>, order_id: u64) -> bool;

        /// Zero-copy read of Top-5 levels directly into caller's memory.
        fn get_top5_snapshot(book: &OrderBook, out: &mut Top5View);
    }
}

/// High-level Rust Quantitative Intelligence Engine.
/// Bridges low-latency C++20 Order Matching with SIMD Alpha generation.
pub struct AlphaEngine {
    book: cxx::UniquePtr<ffi::OrderBook>,
}

impl AlphaEngine {
    /// Creates a new Alpha Engine with an underlying pre-allocated C++ OrderBook.
    pub fn new() -> Self {
        Self {
            book: ffi::new_order_book(),
        }
    }

    /// Add a Limit Order to the C++ matching engine (0 = Buy, 1 = Sell).
    #[inline(always)]
    pub fn add_limit_order(
        &mut self,
        order_id: u64,
        client_id: u64,
        side: u8,
        price: u32,
        quantity: u32,
        timestamp_ns: u64,
    ) -> bool {
        ffi::order_book_add_limit(
            self.book.pin_mut(),
            order_id,
            client_id,
            side,
            price,
            quantity,
            timestamp_ns,
        )
    }

    /// Add a Market Order to immediately cross resting liquidity.
    #[inline(always)]
    pub fn add_market_order(
        &mut self,
        order_id: u64,
        client_id: u64,
        side: u8,
        quantity: u32,
        timestamp_ns: u64,
    ) -> bool {
        ffi::order_book_add_market(
            self.book.pin_mut(),
            order_id,
            client_id,
            side,
            quantity,
            timestamp_ns,
        )
    }

    /// Cancel an active resting order in O(1).
    #[inline(always)]
    pub fn cancel_order(&mut self, order_id: u64) -> bool {
        ffi::order_book_cancel(self.book.pin_mut(), order_id)
    }

    /// Reads Top-5 Market Depth from C++ without heap allocation.
    #[inline(always)]
    pub fn get_top5_snapshot(&self) -> ffi::Top5View {
        let mut snapshot = ffi::Top5View::default();
        ffi::get_top5_snapshot(&self.book, &mut snapshot);
        snapshot
    }

    /// Calculates SIMD-accelerated Order Book Imbalance (OBI) across the top levels.
    ///
    /// Imbalance Formula: (BidVolume - AskVolume) / (BidVolume + AskVolume)
    #[inline(always)]
    pub fn compute_simd_obi(&self) -> Level4Obi {
        let snapshot = self.get_top5_snapshot();
        calculate_obi(&snapshot.bid_volumes, &snapshot.ask_volumes)
    }
}

impl Default for AlphaEngine {
    fn default() -> Self {
        Self::new()
    }
}
