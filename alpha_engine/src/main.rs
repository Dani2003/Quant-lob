use alpha_engine::{calculate_obi, simd_obi::calculate_obi_scalar, AlphaEngine};
use std::time::Instant;

#[link(name = "quant_lob_bridge")]
extern "C" {}

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

    // 4. Compute Quantitative Alpha Signals (SIMD OBI + Stoikov Micro-Price)
    let signal = engine.compute_alpha_signals();
    println!("--- Quantitative Alpha Signals ---");
    println!("  Standard Mid-Price : ${:.2}", signal.mid_price / 100.0);
    println!("  Stoikov Micro-Price: ${:.4} (Adjusted for queue size)", signal.micro_price / 100.0);
    println!("  Spread             : {} ticks (${:.2})\n", signal.spread_ticks, signal.spread_ticks as f64 / 100.0);

    println!("--- SIMD-Vectorized Order Book Imbalance (OBI) ---");
    println!("Formula: OBI = (BidVolume - AskVolume) / (BidVolume + AskVolume)");
    for i in 0..4 {
        println!(
            "  Level {}: OBI = {:+0.4} (Bid: {}, Ask: {})",
            i + 1,
            signal.obi.levels[i],
            snapshot.bid_volumes[i],
            snapshot.ask_volumes[i]
        );
    }
    println!("  Weighted Multi-Level OBI: {:+0.4}\n", signal.obi.weighted_obi);

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

    // 6. Benchmark High-Frequency Strategy Decision Engine
    println!("\n[4/4] Evaluating HFT Market-Making & Alpha Strategy Decisions...");
    let mut strategy = alpha_engine::HftAlphaStrategy::new(alpha_engine::StrategyConfig::default());
    let action = strategy.evaluate(&signal);
    println!("  Generated Action: {:?}", action);

    let start_strat = Instant::now();
    let mut actions_taken = 0usize;
    for _ in 0..BENCH_ITERS {
        let act = strategy.evaluate(&signal);
        if act != alpha_engine::StrategyAction::Hold {
            actions_taken += 1;
        }
    }
    let dur_strat = start_strat.elapsed();
    let strat_ns = dur_strat.as_nanos() as f64 / BENCH_ITERS as f64;
    println!("  Strategy Decision Latency: {:0.2} ns / evaluation ({:0.2} M decisions/sec)",
             strat_ns, (BENCH_ITERS as f64 / dur_strat.as_secs_f64()) / 1e6);

    println!("\n==========================================================");
    println!("  ALPHA & EXECUTION ENGINE READY FOR PRODUCTION           ");
    println!("==========================================================");
}


