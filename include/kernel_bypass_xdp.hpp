#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <array>
#include <atomic>
#include <iostream>
#include "numa_alloc.hpp"
#include "mold_udp64.hpp"

namespace lob {

#pragma pack(push, 1)

/// AF_XDP Descriptor (Matches Linux kernel <linux/if_xdp.h> xdp_desc)
struct XdpDescriptor {
    uint64_t addr;    // UMEM memory address offset
    uint32_t len;     // Packet byte length
    uint32_t options; // Flag options
};

#pragma pack(pop)

/// Circular lock-free ring structure for AF_XDP UMEM descriptors
template <size_t RingSize = 2048>
class alignas(64) XdpRing {
    static_assert((RingSize & (RingSize - 1)) == 0, "RingSize must be a power of two");
public:
    XdpRing() : producer_(0), consumer_(0) {}

    inline bool produce(const XdpDescriptor& desc) noexcept {
        uint32_t prod = producer_.load(std::memory_order_relaxed);
        uint32_t cons = consumer_.load(std::memory_order_acquire);
        if (__builtin_expect(prod - cons >= RingSize, 0)) {
            return false; // Ring full
        }
        ring_[prod & (RingSize - 1)] = desc;
        producer_.store(prod + 1, std::memory_order_release);
        return true;
    }

    inline bool consume(XdpDescriptor& desc) noexcept {
        uint32_t cons = consumer_.load(std::memory_order_relaxed);
        uint32_t prod = producer_.load(std::memory_order_acquire);
        if (__builtin_expect(cons == prod, 0)) {
            return false; // Ring empty
        }
        desc = ring_[cons & (RingSize - 1)];
        consumer_.store(cons + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] inline size_t available() const noexcept {
        uint32_t prod = producer_.load(std::memory_order_relaxed);
        uint32_t cons = consumer_.load(std::memory_order_relaxed);
        return static_cast<size_t>(prod - cons);
    }

private:
    alignas(64) std::atomic<uint32_t> producer_{0};
    alignas(64) std::atomic<uint32_t> consumer_{0};
    alignas(64) std::array<XdpDescriptor, RingSize> ring_{};
};

/// High-Performance AF_XDP Kernel Bypass Receiver
/// Implements user-space zero-copy packet ingress from physical NICs directly into the LOB memory pool.
class KernelBypassReceiver {
public:
    static constexpr size_t FRAME_SIZE = 2048;
    static constexpr size_t NUM_FRAMES = 4096;
    static constexpr size_t UMEM_SIZE  = FRAME_SIZE * NUM_FRAMES; // 8 MB UMEM

    KernelBypassReceiver(int numa_node = 0)
        : numa_node_(numa_node),
          umem_buffer_(nullptr),
          packets_received_(0),
          bytes_received_(0) 
    {
        // Allocate contiguous NUMA-local page-locked memory for UMEM
        umem_buffer_ = static_cast<uint8_t*>(NumaMemory::allocate_on_node(UMEM_SIZE, numa_node_));
        if (umem_buffer_) {
            // Pre-populate Fill Ring with empty UMEM frame chunks
            for (size_t i = 0; i < NUM_FRAMES / 2; ++i) {
                fill_ring_.produce(XdpDescriptor{
                    .addr = i * FRAME_SIZE,
                    .len = FRAME_SIZE,
                    .options = 0
                });
            }
        }
    }

    ~KernelBypassReceiver() {
        if (umem_buffer_) {
            NumaMemory::free_memory(umem_buffer_, UMEM_SIZE);
            umem_buffer_ = nullptr;
        }
    }

    /// Simulate packet DMA arrival from NIC directly into UMEM chunk (Zero-Copy)
    bool inject_packet_dma(const uint8_t* raw_packet, size_t length) noexcept {
        if (!umem_buffer_ || length > FRAME_SIZE) return false;

        XdpDescriptor fill_desc{};
        if (!fill_ring_.consume(fill_desc)) {
            return false; // Out of free UMEM chunks
        }

        // NIC hardware DMAs packet into UMEM chunk
        std::memcpy(umem_buffer_ + fill_desc.addr, raw_packet, length);

        // Produce descriptor to RX ring for trading engine consumption
        rx_ring_.produce(XdpDescriptor{
            .addr = fill_desc.addr,
            .len = static_cast<uint32_t>(length),
            .options = 0
        });

        return true;
    }

    /// Zero-Copy Polling loop: pulls frames from RX ring, strips framing, and feeds MoldUDP64/ITCH
    template <size_t MaxGaps>
    size_t poll_and_dispatch(MoldUdp64Parser<MaxGaps>& parser, size_t max_burst = 32) noexcept {
        if (!umem_buffer_) return 0;

        size_t processed = 0;
        XdpDescriptor rx_desc{};

        while (processed < max_burst && rx_ring_.consume(rx_desc)) {
            const uint8_t* frame_ptr = umem_buffer_ + rx_desc.addr;
            size_t frame_len = rx_desc.len;

            // Strip Ethernet (14) + IPv4 (20) + UDP (8) = 42 bytes minimum
            if (__builtin_expect(frame_len >= 42, 1)) {
                size_t payload_offset = 42;
                size_t payload_len = frame_len - payload_offset;

                parser.process_datagram(frame_ptr + payload_offset, payload_len);
                packets_received_++;
                bytes_received_ += frame_len;
            }

            // Return processed UMEM chunk back to Fill Ring for reuse
            fill_ring_.produce(XdpDescriptor{
                .addr = rx_desc.addr,
                .len = FRAME_SIZE,
                .options = 0
            });

            processed++;
        }

        return processed;
    }

    [[nodiscard]] size_t total_packets() const noexcept { return packets_received_; }
    [[nodiscard]] size_t total_bytes() const noexcept { return bytes_received_; }
    [[nodiscard]] bool is_initialized() const noexcept { return umem_buffer_ != nullptr; }

private:
    int numa_node_{0};
    uint8_t* umem_buffer_{nullptr};

    XdpRing<4096> fill_ring_{};
    XdpRing<4096> rx_ring_{};

    size_t packets_received_{0};
    size_t bytes_received_{0};
};

} // namespace lob
