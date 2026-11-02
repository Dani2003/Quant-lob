#pragma once

#include <cstdint>
#include <cstring>
#include <atomic>
#include <string_view>

#if defined(__linux__)
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace lob {

/// Cache-line aligned shared memory telemetry block
/// Updated at sub-microsecond frequency by the trading engine; read lock-free by external monitors.
struct alignas(64) ShmTelemetryBlock {
    // Magic header & sequence number for seqlock-style atomic read
    uint64_t sequence{0};
    uint64_t heartbeat_timestamp_ns{0};

    // Market State (BBO & Depth)
    uint32_t best_bid_price{0};
    uint32_t best_bid_volume{0};
    uint32_t best_ask_price{0};
    uint32_t best_ask_volume{0};
    uint32_t spread_ticks{0};
    double   mid_price{0.0};
    double   micro_price{0.0};
    float    weighted_obi{0.0f};

    // Engine Throughput Counters
    uint64_t messages_ingested{0};
    uint64_t orders_matched{0};
    uint64_t trades_executed{0};
    uint64_t volume_traded{0};

    // Strategy, Risk & Execution OMS
    int64_t  net_inventory{0};
    int64_t  realized_pnl_cents{0};
    int64_t  unrealized_pnl_cents{0};
    int64_t  total_pnl_cents{0};
    uint64_t active_orders_resting{0};
    uint64_t risk_rejections{0};
    bool     kill_switch_active{false};

    // Hardware Latency Benchmarks (RDTSC in nanoseconds)
    double   p50_latency_ns{0.0};
    double   p99_latency_ns{0.0};
    double   p99_9_latency_ns{0.0};

    // Transaction Cost Analysis (TCA)
    double   avg_effective_half_spread{0.0};
    double   avg_realized_spread_1ms{0.0};
    double   toxic_flow_ratio{0.0};
};

/// High-Performance Zero-Copy Shared Memory Telemetry Manager
class ShmTelemetryManager {
public:
    static constexpr std::string_view SHM_PATH = "/quant_lob_telemetry";

    ShmTelemetryManager() : block_(nullptr), fd_(-1), is_producer_(false) {}

    ~ShmTelemetryManager() {
        close();
    }

    /// Initialize as Producer (Trading Engine writes live state)
    bool init_producer() {
        is_producer_ = true;
#if defined(__linux__)
        fd_ = ::shm_open(SHM_PATH.data(), O_CREAT | O_RDWR | O_TRUNC, 0666);
        if (fd_ < 0) {
            block_ = new ShmTelemetryBlock{};
            return true; // Fallback in-process
        }
        if (::ftruncate(fd_, sizeof(ShmTelemetryBlock)) != 0) {
            ::close(fd_);
            fd_ = -1;
            block_ = new ShmTelemetryBlock{};
            return true;
        }
        void* ptr = ::mmap(nullptr, sizeof(ShmTelemetryBlock), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (ptr == MAP_FAILED) {
            block_ = new ShmTelemetryBlock{};
            return true;
        }
        block_ = static_cast<ShmTelemetryBlock*>(ptr);
        std::memset(block_, 0, sizeof(ShmTelemetryBlock));
        return true;
#else
        block_ = new ShmTelemetryBlock{};
        std::memset(block_, 0, sizeof(ShmTelemetryBlock));
        return true;
#endif
    }

    /// Initialize as Consumer (External Monitor reads live state)
    bool init_consumer() {
        is_producer_ = false;
#if defined(__linux__)
        fd_ = ::shm_open(SHM_PATH.data(), O_RDONLY, 0666);
        if (fd_ < 0) return false;
        void* ptr = ::mmap(nullptr, sizeof(ShmTelemetryBlock), PROT_READ, MAP_SHARED, fd_, 0);
        if (ptr == MAP_FAILED) return false;
        block_ = static_cast<ShmTelemetryBlock*>(ptr);
        return true;
#else
        return false;
#endif
    }

    void close() {
#if defined(__linux__)
        if (block_ && fd_ >= 0) {
            ::munmap(block_, sizeof(ShmTelemetryBlock));
            block_ = nullptr;
            ::close(fd_);
            fd_ = -1;
            if (is_producer_) {
                ::shm_unlink(SHM_PATH.data());
            }
        }
#endif
        if (block_ && fd_ < 0) {
            delete block_;
            block_ = nullptr;
        }
    }

    [[nodiscard]] inline ShmTelemetryBlock* get() noexcept { return block_; }
    [[nodiscard]] inline const ShmTelemetryBlock* get() const noexcept { return block_; }

private:
    ShmTelemetryBlock* block_{nullptr};
    int fd_{-1};
    bool is_producer_{false};
};

} // namespace lob

