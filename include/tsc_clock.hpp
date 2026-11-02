#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

namespace lob {

/**
 * @brief Sub-nanosecond Hardware Timestamp Counter (TSC) Clock.
 *
 * Provides CPU-cycle precision with serialized execution (LFENCE/RDTSC)
 * to measure sub-microsecond tick-to-trade latency without OS syscall overhead.
 */
class TscClock {
public:
    static inline uint64_t rdtsc() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
        _mm_lfence(); // Serialize CPU pipeline
        return __rdtsc();
#else
        return static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    }

    /**
     * @brief Calibrates CPU TSC frequency against steady_clock over a sleep interval.
     */
    static double calibrate_ghz() noexcept {
        const auto start_time = std::chrono::steady_clock::now();
        const uint64_t start_cycles = rdtsc();

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        const uint64_t end_cycles = rdtsc();
        const auto end_time = std::chrono::steady_clock::now();

        const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end_time - start_time).count();

        if (elapsed_ns == 0) return 3.0; // Fallback default 3.0 GHz

        const double cycles = static_cast<double>(end_cycles - start_cycles);
        const double ghz = cycles / static_cast<double>(elapsed_ns);
        return (ghz > 0.5 && ghz < 6.0) ? ghz : 3.0;
    }

    static inline double cycles_to_ns(uint64_t cycles, double cpu_ghz) noexcept {
        return static_cast<double>(cycles) / cpu_ghz;
    }
};

/**
 * @brief Fixed-size zero-allocation Latency Profiler for HFT Tail Latency Analysis.
 *
 * Computes exact percentiles: Min, P50 (Median), P90, P99, P99.9, and Max latency.
 */
template <size_t MaxSamples = 1'000'000>
class LatencyProfiler {
public:
    LatencyProfiler() : cpu_ghz_(TscClock::calibrate_ghz()) {
        samples_.reserve(MaxSamples);
    }

    inline void record_cycles(uint64_t start_cycles, uint64_t end_cycles) noexcept {
        if (samples_.size() < MaxSamples && end_cycles >= start_cycles) {
            samples_.push_back(static_cast<uint32_t>(end_cycles - start_cycles));
        }
    }

    struct LatencyReport {
        double min_ns{0.0};
        double p50_ns{0.0};
        double p90_ns{0.0};
        double p99_ns{0.0};
        double p99_9_ns{0.0};
        double max_ns{0.0};
        double mean_ns{0.0};
        size_t sample_count{0};
    };

    LatencyReport compute_report() noexcept {
        LatencyReport report{};
        if (samples_.empty()) return report;

        report.sample_count = samples_.size();

        // Sort samples in-place for exact percentile ranking
        std::sort(samples_.begin(), samples_.end());

        uint64_t total_cycles = 0;
        for (uint32_t c : samples_) {
            total_cycles += c;
        }

        report.mean_ns = TscClock::cycles_to_ns(total_cycles / report.sample_count, cpu_ghz_);
        report.min_ns = TscClock::cycles_to_ns(samples_.front(), cpu_ghz_);
        report.max_ns = TscClock::cycles_to_ns(samples_.back(), cpu_ghz_);

        const auto get_percentile = [this](double p) -> double {
            size_t idx = static_cast<size_t>(std::ceil(p * static_cast<double>(samples_.size()))) - 1;
            idx = std::min(idx, samples_.size() - 1);
            return TscClock::cycles_to_ns(samples_[idx], cpu_ghz_);
        };

        report.p50_ns = get_percentile(0.50);
        report.p90_ns = get_percentile(0.90);
        report.p99_ns = get_percentile(0.99);
        report.p99_9_ns = get_percentile(0.999);

        return report;
    }

    void print_report(const std::string& title) noexcept {
        const auto r = compute_report();
        std::cout << "--- " << title << " Latency Profile (" << r.sample_count << " samples) ---\n";
        std::cout << "  CPU Clock Rate : " << std::fixed << std::setprecision(2) << cpu_ghz_ << " GHz\n";
        std::cout << "  Min Latency    : " << std::setw(8) << std::setprecision(1) << r.min_ns << " ns\n";
        std::cout << "  P50 (Median)   : " << std::setw(8) << std::setprecision(1) << r.p50_ns << " ns\n";
        std::cout << "  P90            : " << std::setw(8) << std::setprecision(1) << r.p90_ns << " ns\n";
        std::cout << "  P99 (Tail)     : " << std::setw(8) << std::setprecision(1) << r.p99_ns << " ns\n";
        std::cout << "  P99.9 (Extreme): " << std::setw(8) << std::setprecision(1) << r.p99_9_ns << " ns\n";
        std::cout << "  Max Latency    : " << std::setw(8) << std::setprecision(1) << r.max_ns << " ns\n";
        std::cout << "  Mean Latency   : " << std::setw(8) << std::setprecision(1) << r.mean_ns << " ns\n\n";
    }

    void reset() noexcept {
        samples_.clear();
    }

private:
    double cpu_ghz_;
    std::vector<uint32_t> samples_;
};

} // namespace lob

