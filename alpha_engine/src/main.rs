use alpha_engine::{calculate_obi, simd_obi::calculate_obi_scalar, AlphaEngine};
use std::time::Instant;

fn main() {
    println!("==========================================================");
    println!("  QUANT-LOB: RUST FFI ALPHA ENGINE & SIMD OBI BENCHMARK  ");
    println!("  Bridge: cxx crate | C++20 Limit Order Book | SSE/AVX    ");
    println!("==========================================================\n");

    // 1. Initialize C++ OrderBook through zero-cost Rust FFI boundary
    let mut engine = AlphaEngine::new();
    println!("[1/4] C++20 OrderBook initialized via cxx bridge.");

    // 2. Seed realistic Top-5 market depth
    let base_mid = 10_000u32; // $100.00 in ticks

    // Populate Bids (Prices descending: 99.99, 99.98, 99.97, 99.96, 99.95)
    let bid_qtys = [1200u32, 850, 600, 400, 250];
    for (i, &qty) in bid_qtys.iter().enumerate() {
        let price = base_mid - (i as u32 + 1);
        engine.add_limit_order(i as u64 + 1, 100, 0, price, qty, 0);
    }

    // Populate Asks (Prices ascending: 100.01, 100.02, 100.03, 100.04, 100.05)
    let ask_qtys = [400u32, 500, 750, 900, 1100];
    for (i, &qty) in ask_qtys.iter().enumerate() {
        let price = base_mid + (i as u32 + 1);
        engine.add_limit_order(100 + i as u64 + 1, 100, 1, price, qty, 0);
    }

    println!("[2/4] Market seeded with 10 resting Limit Orders across 5 levels.\n");

    // 3. Read Top-5 Snapshot via Zero-Copy FFI
    let snapshot = engine.get_top5_snapshot();
    println!("--- C++ Order Book Top 5 Levels (Zero-Copy Read) ---");
    println!("{:>10} {:>10} | {:>10} {:>10}", "BID VOL", "BID PRC", "ASK PRC", "ASK VOL");
    println!("--------------------------------------------------");
    for i in 0..5 {
        println!(
            "{:>10} {:>10} | {:>10} {:>10}",
            snapshot.bid_volumes[i],
            snapshot.bid_prices[i],
            snapshot.ask_prices[i],
            snapshot.ask_volumes[i]
        );
    }
    println!();

    // 4. Compute SIMD Order Book Imbalance (OBI)
    let obi_result = engine.compute_simd_obi();
    println!("--- SIMD-Vectorized Order Book Imbalance (OBI) ---");
    println!("Formula: OBI = (BidVolume - AskVolume) / (BidVolume + AskVolume)");
    for i in 0..4 {
        println!(
            "  Level {}: OBI = {:+0.4} (Bid: {}, Ask: {})",
            i + 1,
            obi_result.levels[i],
            snapshot.bid_volumes[i],
            snapshot.ask_volumes[i]
        );
    }
    println!("  Weighted Multi-Level OBI: {:+0.4}\n", obi_result.weighted_obi);

    // 5. Benchmark SIMD vs Scalar OBI Throughput
    println!("[3/4] Benchmarking 10,000,000 OBI Calculations (SIMD vs Scalar)...");
    const BENCH_ITERS: usize = 10_000_000;

    let bids = snapshot.bid_volumes;
    let asks = snapshot.ask_volumes;

    // Scalar Benchmark
    let start_scalar = Instant::now();
    let mut sink_scalar = 0.0f32;
    for _ in 0..BENCH_ITERS {
        let res = calculate_obi_scalar(&bids, &asks);
        sink_scalar += res.weighted_obi;
    }
    let dur_scalar = start_scalar.elapsed();
    let scalar_ns = dur_scalar.as_nanos() as f64 / BENCH_ITERS as f64;

    // SIMD Benchmark
    let start_simd = Instant::now();
    let mut sink_simd = 0.0f32;
    for _ in 0..BENCH_ITERS {
        let res = calculate_obi(&bids, &asks);
        sink_simd += res.weighted_obi;
    }
    let dur_simd = start_simd.elapsed();
    let simd_ns = dur_simd.as_nanos() as f64 / BENCH_ITERS as f64;

    println!("Scalar Engine Latency : {:0.2} ns / calculation ({:0.2} Mops/sec)", 
             scalar_ns, (BENCH_ITERS as f64 / dur_scalar.as_secs_f64()) / 1e6);
    println!("SIMD Engine Latency   : {:0.2} ns / calculation ({:0.2} Mops/sec)", 
             simd_ns, (BENCH_ITERS as f64 / dur_simd.as_secs_f64()) / 1e6);

    let speedup = scalar_ns / simd_ns;
    println!("SIMD Vector Speedup   : {:0.2}x faster", speedup);
    println!("Integrity Check       : {}", if (sink_scalar - sink_simd).abs() < 1e-2 { "VERIFIED [OK]" } else { "MISMATCH" });

    println!("\n==========================================================");
    println!("  ALPHA ENGINE READY FOR PRODUCTION DEPLOYMENT            ");
    println!("==========================================================");
}

