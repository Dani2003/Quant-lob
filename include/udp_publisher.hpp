#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace lob {

#pragma pack(push, 1)
/// Binary Market Data Wire Packet streamed over UDP to client applications
struct alignas(8) BinaryMarketDataPacket {
    uint64_t sequence{0};
    uint64_t timestamp_ns{0};
    char     symbol[8]{"AAPL    "};
    uint32_t bid_price{0};
    uint32_t bid_volume{0};
    uint32_t ask_price{0};
    uint32_t ask_volume{0};
    uint32_t last_trade_price{0};
    uint32_t last_trade_volume{0};
    double   mid_price{0.0};
    double   micro_price{0.0};
    float    obi{0.0f};
};
#pragma pack(pop)

/**
 * @brief High-Throughput Asynchronous UDP Binary Market Data Streamer.
 * Broadcasts top-of-book depth, alpha indicators and trade updates to client applications.
 */
class UdpMarketDataPublisher {
public:
    UdpMarketDataPublisher() : socket_fd_(-1), is_active_(false) {
        std::memset(&dest_addr_, 0, sizeof(dest_addr_));
    }

    ~UdpMarketDataPublisher() {
        close();
    }

    bool init(std::string_view target_ip = "127.0.0.1", uint16_t port = 9876) {
#if defined(_WIN32)
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        socket_fd_ = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        if (socket_fd_ < 0) return false;
        u_long mode = 1;
        ioctlsocket(socket_fd_, FIONBIO, &mode);
#else
        socket_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd_ < 0) return false;
        int flags = ::fcntl(socket_fd_, F_GETFL, 0);
        if (flags >= 0) {
            ::fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);
        }
#endif
        dest_addr_.sin_family = AF_INET;
        dest_addr_.sin_port = htons(port);
        inet_pton(AF_INET, target_ip.data(), &dest_addr_.sin_addr);

        is_active_ = true;
        return true;
    }

    void close() {
        if (socket_fd_ >= 0) {
#if defined(_WIN32)
            closesocket(socket_fd_);
            WSACleanup();
#else
            ::close(socket_fd_);
#endif
            socket_fd_ = -1;
        }
        is_active_ = false;
    }

    /// Broadcast market update packet zero-copy over UDP
    bool publish_tick(const BinaryMarketDataPacket& packet) noexcept {
        if (!is_active_ || socket_fd_ < 0) return false;

#if defined(_WIN32)
        int sent = ::sendto(
            socket_fd_,
            reinterpret_cast<const char*>(&packet),
            sizeof(BinaryMarketDataPacket),
            0,
            reinterpret_cast<const sockaddr*>(&dest_addr_),
            sizeof(dest_addr_)
        );
#else
        ssize_t sent = ::sendto(
            socket_fd_,
            &packet,
            sizeof(BinaryMarketDataPacket),
            MSG_DONTWAIT,
            reinterpret_cast<const sockaddr*>(&dest_addr_),
            sizeof(dest_addr_)
        );
#endif
        return (sent == static_cast<ssize_t>(sizeof(BinaryMarketDataPacket)));
    }

    [[nodiscard]] bool is_active() const noexcept { return is_active_; }

private:
    int socket_fd_;
    sockaddr_in dest_addr_;
    bool is_active_;
};

} // namespace lob

